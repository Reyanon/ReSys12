// vitaGL output: one textured quad from fixed function, plus the OSD.
// Based on the ReST-V vitaGL era output (ReST-V commit dc9a9dc^ video_gl.c).
#include "video/video.h"

#include <psp2/gxm.h>
#include <psp2/kernel/threadmgr.h>
#include <vitaGL.h>
#include <png.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/log.h"

#define NUM_TEX 6

static GLuint s_tex[NUM_TEX];
static uint32_t *s_pix[NUM_TEX];
static int s_w[NUM_TEX], s_h[NUM_TEX];
// 15-bit textures (PS1 VRAM format) used for normal frames; the 32-bit ones above for 24-bit (MDEC) frames
static GLuint s_tex16[NUM_TEX];
static uint16_t *s_pix16[NUM_TEX];
static int s_is16[NUM_TEX];
static int s_field[NUM_TEX];      // field shown alone (bob): 1 = odd lines, drawn half a line lower
// Finished frames are shown in order, one per present (FIFO): the renderer often finishes a frame just before or just
// after a vblank, and showing the latest one then repeated a frame and skipped the next (5-15 times a second in a
// tektagt fight at a steady 60 game frames). Up to 2 frames wait; beyond that the oldest are dropped (latency cap).
#define QLEN 8
static volatile int s_draw;          // texture the producer writes
static volatile int s_queue[QLEN];   // finished textures, s_qhead (consumer) .. s_qtail (producer), free-running
static volatile uint32_t s_qhead, s_qtail;
static volatile int s_shown = -1;    // texture shown now, and the one before (the GPU may still read it)
static volatile int s_prev_shown = -1;

// producer: a texture nobody reads or waits to show
static int pick_free(void)
{
	for (int k = 0; k < NUM_TEX; k++) {
		const int d = (s_draw + k) % NUM_TEX;
		if (d == s_shown || d == s_prev_shown)
			continue;
		int queued = 0;
		for (uint32_t q = s_qhead; q != s_qtail; q++)
			if (s_queue[q % QLEN] == d)
				queued = 1;
		if (!queued)
			return d;
	}
	return s_draw; // cannot happen with NUM_TEX >= shown + previous + queued + drawn
}

void rs12_video_init(void)
{
	vglWaitVblankStart(GL_TRUE);
	// keep the vitaGL garbage collector off CPU0 (emulation)
	vglSetupGarbageCollector(0x10000100, SCE_KERNEL_CPU_MASK_USER_2);
	vglInit(0x100000);

	glGenTextures(NUM_TEX, s_tex);
	for (int i = 0; i < NUM_TEX; i++) {
		glBindTexture(GL_TEXTURE_2D, s_tex[i]);
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, RS12_VIDEO_TEX_W, RS12_VIDEO_TEX_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		s_pix[i] = (uint32_t *)vglGetTexDataPointer(GL_TEXTURE_2D);
		s_w[i] = 640;
		s_h[i] = 480;
	}
	glGenTextures(NUM_TEX, s_tex16);
	for (int i = 0; i < NUM_TEX; i++) {
		glBindTexture(GL_TEXTURE_2D, s_tex16[i]);
		// internal format must be GL_RGBA: vitaGL only allocates 16-bit storage (U5U5U5U1) for GL_RGBA + 5_5_5_1;
		// GL_RGB5_A1 gave a 32-bit texture and the game picture came out small, doubled and purple
		glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, RS12_VIDEO_TEX_W, RS12_VIDEO_TEX_H, 0, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, NULL);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		// vitaGL stores RGB5_A1 as U5U5U5U1_RGBA; switch the GXM view to the PS1 layout (A1B5G5R5) so frames are a plain copy
		sceGxmTextureSetFormat(vglGetGxmTexture(GL_TEXTURE_2D), SCE_GXM_TEXTURE_FORMAT_U1U5U5U5_ABGR);
		s_pix16[i] = (uint16_t *)vglGetTexDataPointer(GL_TEXTURE_2D);
	}

	glViewport(0, 0, 960, 544);
	glMatrixMode(GL_PROJECTION);
	glLoadIdentity();
	glOrtho(0, 960, 544, 0, -1, 1);
	glMatrixMode(GL_MODELVIEW);
	glLoadIdentity();
	glEnable(GL_TEXTURE_2D);
	glClearColor(0, 0, 0, 1);
	RS12_LOG("video: vitaGL init, tex %p %p %p", (void *)s_pix[0], (void *)s_pix[1], (void *)s_pix[2]);
}

void rs12_video_shutdown(void)
{
	glDeleteTextures(NUM_TEX, s_tex);
	// vitaGL has no shutdown call; the process exit cleans up
}

uint32_t *rs12_video_frame_begin(int w, int h)
{
	// never hand out a texture the GPU is reading or that waits to be shown
	const int d = pick_free();
	s_draw = d;
	s_w[d] = w;
	s_h[d] = h;
	s_is16[d] = 0;
	s_field[d] = 0;
	return s_pix[d];
}

uint16_t *rs12_video_frame_begin16(int w, int h)
{
	const int d = pick_free();
	s_draw = d;
	s_w[d] = w;
	s_h[d] = h;
	s_is16[d] = 1;
	s_field[d] = 0;
	return s_pix16[d];
}

// the frame being written holds one field of a 480-line picture (0 even lines, 1 odd)
void rs12_video_frame_field(int odd)
{
	s_field[s_draw] = odd;
}

volatile uint32_t rs12_video_done, rs12_video_repeats, rs12_video_skips; // frames finished; presents with none new / several new

void rs12_video_frame_end(void)
{
	rs12_video_done++;
	s_queue[s_qtail % QLEN] = s_draw;
	__sync_synchronize();
	s_qtail++;
	s_draw = (s_draw + 1) % NUM_TEX;
}

// ---- OSD text (FPS) ----
// 5x7 font. Changes every second, so rotate 3 textures and never touch the one the GPU is reading
#define OSD_N 3
#define OSD_W 256
#define OSD_H 9
#define OSD_SCALE 3
static GLuint s_osd_tex[OSD_N];
static uint32_t *s_osd_px[OSD_N];
static int s_osd_cur = -1, s_osd_len;

static const uint8_t *osd_glyph(char c)
{
	static const uint8_t digits[10][7] = {
		{ 0x0E, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0E }, { 0x04, 0x0C, 0x04, 0x04, 0x04, 0x04, 0x0E },
		{ 0x0E, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1F }, { 0x1F, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0E },
		{ 0x02, 0x06, 0x0A, 0x12, 0x1F, 0x02, 0x02 }, { 0x1F, 0x10, 0x1E, 0x01, 0x01, 0x11, 0x0E },
		{ 0x06, 0x08, 0x10, 0x1E, 0x11, 0x11, 0x0E }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
		{ 0x0E, 0x11, 0x11, 0x0E, 0x11, 0x11, 0x0E }, { 0x0E, 0x11, 0x11, 0x0F, 0x01, 0x02, 0x0C },
	};
	static const uint8_t letters[26][7] = {
		{ 0x0E, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 }, { 0x1E, 0x11, 0x11, 0x1E, 0x11, 0x11, 0x1E }, // A B
		{ 0x0E, 0x11, 0x10, 0x10, 0x10, 0x11, 0x0E }, { 0x1E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x1E }, // C D
		{ 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x1F }, { 0x1F, 0x10, 0x10, 0x1E, 0x10, 0x10, 0x10 }, // E F
		{ 0x0E, 0x11, 0x10, 0x17, 0x11, 0x11, 0x0F }, { 0x11, 0x11, 0x11, 0x1F, 0x11, 0x11, 0x11 }, // G H
		{ 0x0E, 0x04, 0x04, 0x04, 0x04, 0x04, 0x0E }, { 0x07, 0x02, 0x02, 0x02, 0x02, 0x12, 0x0C }, // I J
		{ 0x11, 0x12, 0x14, 0x18, 0x14, 0x12, 0x11 }, { 0x10, 0x10, 0x10, 0x10, 0x10, 0x10, 0x1F }, // K L
		{ 0x11, 0x1B, 0x15, 0x15, 0x11, 0x11, 0x11 }, { 0x11, 0x11, 0x19, 0x15, 0x13, 0x11, 0x11 }, // M N
		{ 0x0E, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, { 0x1E, 0x11, 0x11, 0x1E, 0x10, 0x10, 0x10 }, // O P
		{ 0x0E, 0x11, 0x11, 0x11, 0x15, 0x12, 0x0D }, { 0x1E, 0x11, 0x11, 0x1E, 0x14, 0x12, 0x11 }, // Q R
		{ 0x0F, 0x10, 0x10, 0x0E, 0x01, 0x01, 0x1E }, { 0x1F, 0x04, 0x04, 0x04, 0x04, 0x04, 0x04 }, // S T
		{ 0x11, 0x11, 0x11, 0x11, 0x11, 0x11, 0x0E }, { 0x11, 0x11, 0x11, 0x11, 0x11, 0x0A, 0x04 }, // U V
		{ 0x11, 0x11, 0x11, 0x15, 0x15, 0x15, 0x0A }, { 0x11, 0x11, 0x0A, 0x04, 0x0A, 0x11, 0x11 }, // W X
		{ 0x11, 0x11, 0x0A, 0x04, 0x04, 0x04, 0x04 }, { 0x1F, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1F }, // Y Z
	};
	static const uint8_t dot[7] = { 0, 0, 0, 0, 0, 0x0C, 0x0C };
	static const uint8_t slash[7] = { 0x01, 0x01, 0x02, 0x04, 0x08, 0x10, 0x10 };
	static const uint8_t space[7] = { 0 };
	if (c >= '0' && c <= '9')
		return digits[c - '0'];
	if (c >= 'a' && c <= 'z')
		c -= 32;
	if (c >= 'A' && c <= 'Z')
		return letters[c - 'A'];
	if (c == '.')
		return dot;
	if (c == '/')
		return slash;
	return space;
}

void rs12_video_set_osd(const char *text)
{
	if (!text) {
		s_osd_len = 0;
		return;
	}
	if (!s_osd_tex[0]) {
		glGenTextures(OSD_N, s_osd_tex);
		for (int i = 0; i < OSD_N; i++) {
			glBindTexture(GL_TEXTURE_2D, s_osd_tex[i]);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, OSD_W, OSD_H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
			s_osd_px[i] = (uint32_t *)vglGetTexDataPointer(GL_TEXTURE_2D);
		}
	}
	const int next = (s_osd_cur + 1) % OSD_N;
	uint32_t *px = s_osd_px[next];
	int len = 0;
	while (text[len] && len < 40)
		len++;
	// translucent black box, white text (ABGR)
	for (int i = 0; i < OSD_W * OSD_H; i++)
		px[i] = 0x80000000u;
	for (int n = 0; n < len; n++) {
		const uint8_t *g = osd_glyph(text[n]);
		for (int y = 0; y < 7; y++)
			for (int x = 0; x < 5; x++)
				if (g[y] & (0x10 >> x))
					px[(y + 1) * OSD_W + 1 + n * 6 + x] = 0xFFFFFFFFu;
	}
	s_osd_len = len;
	s_osd_cur = next;
}

// corners top-left, top-right, bottom-left, bottom-right; uv per corner
static void draw_quad_uv(float x0, float y0, float x1, float y1, const float *uv)
{
	const float vtx[8] = { x0, y0, x1, y0, x0, y1, x1, y1 };
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glVertexPointer(2, GL_FLOAT, 0, vtx);
	glTexCoordPointer(2, GL_FLOAT, 0, uv);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
}

static void draw_quad(float x0, float y0, float x1, float y1, float u1, float v1)
{
	const float vtx[8] = { x0, y0, x1, y0, x0, y1, x1, y1 };
	const float uv[8] = { 0, 0, u1, 0, 0, v1, u1, v1 };
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_TEXTURE_COORD_ARRAY);
	glVertexPointer(2, GL_FLOAT, 0, vtx);
	glTexCoordPointer(2, GL_FLOAT, 0, uv);
	glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_TEXTURE_COORD_ARRAY);
}

// ---- gun crosshairs (over the game picture) ----
static int s_cross_on[2];
static float s_cross_x[2], s_cross_y[2];

void rs12_video_crosshair(int player, int on, float fx, float fy)
{
	if (player < 0 || player > 1)
		return;
	s_cross_on[player] = on;
	s_cross_x[player] = fx;
	s_cross_y[player] = fy;
}

static void draw_bar(float x0, float y0, float x1, float y1)
{
	draw_quad(x0, y0, x1, y1, 0, 0);
}

static int s_rot; // 90: vertical game, the picture turned clockwise
void rs12_video_set_rotation(int rot)
{
	s_rot = rot == 90 ? 90 : 0;
}

// a cross with a gap in the middle, black outline: P1 red, P2 blue (aim in game picture coordinates)
static void draw_crosshairs(float px, float py, float pw, float ph)
{
	if (!s_cross_on[0] && !s_cross_on[1])
		return;
	glDisable(GL_TEXTURE_2D);
	for (int p = 0; p < 2; p++) {
		if (!s_cross_on[p])
			continue;
		const float ax = s_rot ? 1.0f - s_cross_y[p] : s_cross_x[p], ay = s_rot ? s_cross_x[p] : s_cross_y[p];
		const float cx = px + ax * pw, cy = py + ay * ph, r = 14.0f, g = 4.0f, t = 1.5f;
		for (int pass = 0; pass < 2; pass++) {
			const float o = pass ? 0.0f : 1.5f; // outline first
			if (pass)
				glColor4f(p ? 0.3f : 1.0f, p ? 0.6f : 0.2f, p ? 1.0f : 0.2f, 1.0f);
			else
				glColor4f(0.0f, 0.0f, 0.0f, 1.0f);
			draw_bar(cx - r - o, cy - t - o, cx - g + o, cy + t + o);
			draw_bar(cx + g - o, cy - t - o, cx + r + o, cy + t + o);
			draw_bar(cx - t - o, cy - r - o, cx + t + o, cy - g + o);
			draw_bar(cx - t - o, cy + g - o, cx + t + o, cy + r + o);
		}
	}
	glColor4f(1.0f, 1.0f, 1.0f, 1.0f);
	glEnable(GL_TEXTURE_2D);
}

// ---- overlay layers (menu, notices, FPS) ----
RS12Tex *rs12_video_tex(int w, int h)
{
	RS12Tex *t = (RS12Tex *)calloc(1, sizeof(RS12Tex));
	if (!t)
		return NULL;
	GLuint name;
	glGenTextures(1, &name);
	glBindTexture(GL_TEXTURE_2D, name);
	glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	t->gl = name;
	t->data = (uint32_t *)vglGetTexDataPointer(GL_TEXTURE_2D);
	t->w = w;
	t->h = h;
	t->stride = (w + 7) & ~7; // vitaGL linear textures: rows aligned to 8 texels (callers use multiples of 8)
	if (!t->data) {
		RS12_LOG("video: texture %dx%d allocation failed", w, h);
		free(t);
		return NULL;
	}
	memset(t->data, 0, (size_t)t->stride * h * 4);
	return t;
}

static struct { RS12Tex *t; int x, y, w, h; } s_layer[4];
void rs12_video_layer(int slot, RS12Tex *t, int x, int y, int w, int h)
{
	if (slot < 0 || slot >= 4)
		return;
	s_layer[slot].t = t;
	s_layer[slot].x = x;
	s_layer[slot].y = y;
	s_layer[slot].w = w;
	s_layer[slot].h = h;
}

static int s_aspect, s_smooth = 1, s_border, s_border_color;
void rs12_video_set_mode(int aspect, int smooth)
{
	s_aspect = aspect;
	s_smooth = smooth;
}

void rs12_video_set_border(int border, int border_color)
{
	s_border = border;
	s_border_color = border_color;
}

// border artwork supplied by the user (not shipped): ux0:data/resys12/artwork/<set>.png, stretched to the screen
static RS12Tex *s_art;
static int s_art_w, s_art_h;
static uint8_t *load_png(const char *path, int *w, int *h)
{
	png_image img;
	memset(&img, 0, sizeof(img));
	img.version = PNG_IMAGE_VERSION;
	if (!png_image_begin_read_from_file(&img, path))
		return NULL;
	img.format = PNG_FORMAT_RGBA;
	uint8_t *buf = (uint8_t *)malloc(PNG_IMAGE_SIZE(img));
	if (!buf || !png_image_finish_read(&img, NULL, buf, 0, NULL)) {
		free(buf);
		png_image_free(&img);
		return NULL;
	}
	*w = (int)img.width;
	*h = (int)img.height;
	return buf;
}

void rs12_video_set_art(const char *set, const char *parent)
{
	s_art_w = s_art_h = 0; // (one texture: one game per run)
	if (!set)
		return;
	char path[160];
	int w = 0, h = 0;
	snprintf(path, sizeof(path), "ux0:data/resys12/artwork/%s.png", set);
	uint8_t *b = load_png(path, &w, &h);
	if (!b && parent && parent[0]) {
		snprintf(path, sizeof(path), "ux0:data/resys12/artwork/%s.png", parent);
		b = load_png(path, &w, &h);
	}
	if (!b) {
		RS12_LOG("video: %s artwork missing (Border Artwork falls back to the border colour)", set);
		return;
	}
	if (w > 2048 || h > 2048 || (s_art && (w > s_art->w || h > s_art->h))) {
		RS12_LOG("video: artwork %dx%d too large", w, h);
		free(b);
		return;
	}
	if (!s_art) {
		s_art = rs12_video_tex((w + 7) & ~7, h);
		if (s_art) {
			glBindTexture(GL_TEXTURE_2D, s_art->gl);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
			glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
		}
	}
	if (s_art) {
		for (int y = 0; y < h; y++)
			memcpy(s_art->data + (size_t)y * s_art->stride, b + (size_t)y * w * 4, (size_t)w * 4);
		s_art_w = w;
		s_art_h = h;
		RS12_LOG("video: artwork %s %dx%d", path, w, h);
	}
	free(b);
}

static void border_rgb(float *c)
{
	static const uint8_t col[6][3] = { { 0x14, 0x18, 0x3C }, { 0x28, 0x28, 0x2C }, { 0x48, 0x10, 0x14 },
		{ 0x10, 0x34, 0x1C }, { 0x2C, 0x14, 0x48 }, { 0x5A, 0x24, 0x18 } };
	const int k = s_border_color < 0 || s_border_color > 5 ? 0 : s_border_color;
	c[0] = col[k][0] / 255.0f;
	c[1] = col[k][1] / 255.0f;
	c[2] = col[k][2] / 255.0f;
}

static void draw_scene(void)
{
	glViewport(0, 0, 960, 544);
	const int i = s_shown;
	int w = 640, h = 480;
	if (i >= 0) {
		w = s_w[i];
		h = s_h[i];
	}
	float dw, dh;
	if (s_aspect == 1 && w > 0 && h > 0) { // integer scale
		// hi-res width with one field (640x240): each line counts twice so the picture keeps its shape
		const int lh = (w >= 512 && h <= 256) ? h * 2 : h;
		const int sw = s_rot ? lh : w, sh = s_rot ? w : lh; // on screen (a vertical game: turned)
		int k = 960 / sw < 544 / sh ? 960 / sw : 544 / sh;
		if (k < 1)
			k = 1;
		dw = (float)(sw * k);
		dh = (float)(sh * k);
	} else if (s_aspect == 2) {
		dw = 960.0f;
		dh = 544.0f;
	} else {
		dh = 544.0f;
		dw = s_rot ? dh * 3.0f / 4.0f : dh * 4.0f / 3.0f; // 4:3 monitor, on its side for a vertical game
	}
	// the odd field sits half a field line lower (to the left on a turned picture)
	float y_field = 0.0f, x_field = 0.0f;
	if (i >= 0 && s_field[i] && h > 0) {
		if (s_rot)
			x_field = -dw / (2.0f * h);
		else
			y_field = dh / (2.0f * h);
	}
	const float x0 = (960.0f - dw) * 0.5f + x_field, y0 = (544.0f - dh) * 0.5f + y_field;
	const int use_art = i >= 0 && s_border == 1 && s_aspect != 2 && s_art && s_art_w > 0;
	if (i >= 0 && s_border && s_aspect != 2 && !use_art) {
		float c[3];
		border_rgb(c);
		glClearColor(c[0], c[1], c[2], 1);
	} else
		glClearColor(0, 0, 0, 1);
	glClear(GL_COLOR_BUFFER_BIT);
	if (use_art) {
		glBindTexture(GL_TEXTURE_2D, s_art->gl);
		draw_quad(0, 0, 960, 544, (float)s_art_w / s_art->w, (float)s_art_h / s_art->h);
	}
	if (i >= 0) {
		glBindTexture(GL_TEXTURE_2D, s_is16[i] ? s_tex16[i] : s_tex[i]);
		const GLint f = s_smooth ? GL_LINEAR : GL_NEAREST;
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
		const float u1 = (float)w / RS12_VIDEO_TEX_W, v1 = (float)h / RS12_VIDEO_TEX_H;
		if (s_rot) {
			// clockwise: screen top-left shows the picture's bottom-left, top-right its top-left
			const float uv[8] = { 0, v1, 0, 0, u1, v1, u1, 0 };
			draw_quad_uv(x0, y0, x0 + dw, y0 + dh, uv);
		} else
			draw_quad(x0, y0, x0 + dw, y0 + dh, u1, v1);
		draw_crosshairs(x0 - x_field, y0 - y_field, dw, dh);
	}
	if (s_osd_len > 0 && s_osd_cur >= 0) {
		const float ow = (float)(s_osd_len * 6 + 1);
		glEnable(GL_BLEND);
		glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		glBindTexture(GL_TEXTURE_2D, s_osd_tex[s_osd_cur]);
		draw_quad(8, 8, 8 + ow * OSD_SCALE, 8 + OSD_H * OSD_SCALE, ow / OSD_W, 1);
		glDisable(GL_BLEND);
	}
	// overlay layers: premultiplied alpha
	int blend = 0;
	for (int k = 0; k < 4; k++) {
		RS12Tex *t = s_layer[k].t;
		if (!t)
			continue;
		if (!blend) {
			glEnable(GL_BLEND);
			glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
			blend = 1;
		}
		glBindTexture(GL_TEXTURE_2D, t->gl);
		const float lx = (float)s_layer[k].x, ly = (float)s_layer[k].y, lw = (float)s_layer[k].w, lh = (float)s_layer[k].h;
		draw_quad(lx, ly, lx + lw, ly + lh, lw / t->w, lh / t->h);
	}
	if (blend)
		glDisable(GL_BLEND);
}

void rs12_video_present(void)
{
	// next frame in order; keep at most one more waiting
	const uint32_t tail = s_qtail;
	uint32_t head = s_qhead;
	__sync_synchronize();
	if (head == tail)
		rs12_video_repeats++;
	else {
		if (tail - head > 2) {
			rs12_video_skips += tail - head - 2;
			head = tail - 2;
		}
		s_prev_shown = s_shown;
		s_shown = s_queue[head % QLEN];
		__sync_synchronize();
		s_qhead = head + 1;
	}
	draw_scene();
	vglSwapBuffers(GL_FALSE);
}

int rs12_video_capture_ppm(const char *path)
{
	draw_scene();
	uint8_t *px = (uint8_t *)malloc(960 * 544 * 4);
	if (!px)
		return -1;
	glReadPixels(0, 0, 960, 544, GL_RGBA, GL_UNSIGNED_BYTE, px);
	vglSwapBuffers(GL_FALSE);
	FILE *f = fopen(path, "wb");
	if (!f) {
		free(px);
		return -1;
	}
	fprintf(f, "P6\n960 544\n255\n");
	uint8_t *row = (uint8_t *)malloc(960 * 3);
	for (int y = 0; y < 544; y++) {
		const uint8_t *s = px + (size_t)(543 - y) * 960 * 4; // GL rows are bottom-up
		for (int x = 0; x < 960; x++) {
			row[x * 3] = s[x * 4];
			row[x * 3 + 1] = s[x * 4 + 1];
			row[x * 3 + 2] = s[x * 4 + 2];
		}
		fwrite(row, 1, 960 * 3, f);
	}
	free(row);
	fclose(f);
	free(px);
	return 0;
}
