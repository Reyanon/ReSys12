// Drawing layer for menu, notices, FPS (ported from ReST-V). See ui.h.
#include "ui.h"

#include <math.h>
#include <psp2/pgf.h>
#include <psp2/sysmodule.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "platform/log.h"
#include "video/video.h"

// ---- font: system default PGF (Latin); ASCII 32..126 glyphs unpacked once up front ----
typedef struct {
	uint8_t *px; // 8-bit coverage (0..255)
	int w, h, left, top;
	float adv;
} Glyph;
static Glyph s_glyph[96];
static SceFontLibHandle s_lib;
static SceFontHandle s_font;

static void *font_alloc(void *u, unsigned int n) { (void)u; return malloc(n); }
static void font_free(void *u, void *p) { (void)u; free(p); }

void ui_init(void)
{
	if (s_lib)
		return;
	sceSysmoduleLoadModule(SCE_SYSMODULE_PGF);
	SceFontNewLibParams params;
	memset(&params, 0, sizeof(params));
	params.numFonts = 1;
	params.allocFunc = font_alloc;
	params.freeFunc = font_free;
	unsigned int err = 0;
	s_lib = sceFontNewLib(&params, &err);
	if (!s_lib) {
		RS12_LOG("ui: sceFontNewLib failed %08x", err);
		return;
	}
	SceFontStyle style;
	memset(&style, 0, sizeof(style));
	style.fontH = 10.0f; // same conditions as vita2d_load_default_pgf (checked by disassembly)
	style.fontV = 10.0f;
	style.fontLanguage = SCE_FONT_LANGUAGE_LATIN;
	const int idx = sceFontFindOptimumFont(s_lib, &style, &err);
	s_font = sceFontOpen(s_lib, idx >= 0 ? idx : 0, 0, &err);
	if (!s_font) {
		RS12_LOG("ui: sceFontOpen failed %08x", err);
		return;
	}
	for (int ch = 32; ch < 128; ch++) {
		Glyph *g = &s_glyph[ch - 32];
		SceFontCharInfo ci;
		if (sceFontGetCharInfo(s_font, (unsigned)ch, &ci) < 0)
			continue;
		g->w = (int)ci.bitmapWidth;
		g->h = (int)ci.bitmapHeight;
		g->left = (int)ci.bitmapLeft;
		g->top = (int)ci.bitmapTop;
		g->adv = (float)ci.sfp26AdvanceH / 64.0f;
		if (g->w <= 0 || g->h <= 0)
			continue;
		g->px = (uint8_t *)calloc((size_t)g->w * g->h, 1);
		SceFontGlyphImage im;
		memset(&im, 0, sizeof(im));
		im.pixelFormat = SCE_FONT_PIXELFORMAT_8;
		// xPos64/yPos64 = top-left of the glyph image within the buffer (vita2d passes the atlas cell position << 6)
		im.xPos64 = 0;
		im.yPos64 = 0;
		im.bufWidth = (unsigned short)g->w;
		im.bufHeight = (unsigned short)g->h;
		im.bytesPerLine = (unsigned short)g->w;
		im.bufferPtr = (unsigned int)(uintptr_t)g->px;
		sceFontGetCharGlyphImage(s_font, (unsigned)ch, &im);
	}
	RS12_LOG("ui: system font ok ('A' %dx%d top %d adv %.1f)", s_glyph['A' - 32].w, s_glyph['A' - 32].h, s_glyph['A' - 32].top,
		(double)s_glyph['A' - 32].adv);
}

void ui_fini(void)
{
	if (s_font)
		sceFontClose(s_font);
	if (s_lib)
		sceFontDoneLib(s_lib);
	s_font = NULL;
	s_lib = NULL;
}

// ---- canvas ----
// colors kept as premultiplied RGBA; compositing is only mul/add (no divide). vitaGL overlays it with glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA).
// Only the changed area (dirty) is copied to the GPU texture (3 textures rotate, so union of the last 3 areas)
#define NBUF 3 // GPU may still be reading an earlier texture (CPU up to 2 frames ahead)
typedef struct { int x0, y0, x1, y1; } Rect;
struct UiCanvas {
	int slot, x, y, w, h;
	uint32_t *px; // cached CPU copy (RGBA, premultiplied)
	RS12Tex *tex[NBUF];
	int cur;
	Rect dirty;        // area changed since the last commit
	Rect hist[NBUF];   // area per commit (to know what changed since texture i last got data)
	int full[NBUF];    // whole texture not yet in sync
};

static void dirty_add(UiCanvas *c, int x0, int y0, int x1, int y1)
{
	if (x0 < 0) x0 = 0;
	if (y0 < 0) y0 = 0;
	if (x1 > c->w) x1 = c->w;
	if (y1 > c->h) y1 = c->h;
	if (x1 <= x0 || y1 <= y0)
		return;
	Rect *d = &c->dirty;
	if (d->x1 <= d->x0) {
		d->x0 = x0; d->y0 = y0; d->x1 = x1; d->y1 = y1;
		return;
	}
	if (x0 < d->x0) d->x0 = x0;
	if (y0 < d->y0) d->y0 = y0;
	if (x1 > d->x1) d->x1 = x1;
	if (y1 > d->y1) d->y1 = y1;
}

UiCanvas *ui_canvas(int slot, int x, int y, int w, int h)
{
	UiCanvas *c = (UiCanvas *)calloc(1, sizeof(UiCanvas));
	c->slot = slot;
	c->x = x;
	c->y = y;
	c->w = w;
	c->h = h;
	c->px = (uint32_t *)calloc((size_t)w * h, 4);
	for (int i = 0; i < NBUF; i++) {
		c->tex[i] = rs12_video_tex(w, h);
		c->full[i] = 1;
	}
	c->cur = -1;
	RS12_LOG("ui: canvas slot %d %dx%d tex %p %p %p", slot, w, h, (void *)c->tex[0], (void *)c->tex[1], (void *)c->tex[2]);
	return c;
}

void ui_canvas_move(UiCanvas *c, int x, int y)
{
	c->x = x;
	c->y = y;
}

void ui_clear(UiCanvas *c)
{
	memset(c->px, 0, (size_t)c->w * c->h * 4);
	dirty_add(c, 0, 0, c->w, c->h);
}

void ui_clear_rect(UiCanvas *c, int x, int y, int w, int h)
{
	int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > c->w ? c->w : x + w, y1 = y + h > c->h ? c->h : y + h;
	for (int yy = y0; yy < y1; yy++)
		memset(c->px + (size_t)yy * c->w + x0, 0, (size_t)(x1 - x0) * 4);
	dirty_add(c, x0, y0, x1, y1);
}

void ui_commit(UiCanvas *c)
{
	const int n = (c->cur + 1) % NBUF;
	RS12Tex *t = c->tex[n];
	if (!t)
		return;
	// changes since texture n last got data = current area + areas the other textures got in between
	Rect u = c->dirty;
	for (int k = 0; k < NBUF; k++) {
		if (k == n)
			continue;
		const Rect *h = &c->hist[k];
		if (h->x1 <= h->x0)
			continue;
		if (u.x1 <= u.x0) { u = *h; continue; }
		if (h->x0 < u.x0) u.x0 = h->x0;
		if (h->y0 < u.y0) u.y0 = h->y0;
		if (h->x1 > u.x1) u.x1 = h->x1;
		if (h->y1 > u.y1) u.y1 = h->y1;
	}
	if (c->full[n]) {
		u.x0 = 0; u.y0 = 0; u.x1 = c->w; u.y1 = c->h;
		c->full[n] = 0;
	}
	for (int y = u.y0; y < u.y1; y++)
		memcpy(t->data + (size_t)y * t->stride + u.x0, c->px + (size_t)y * c->w + u.x0, (size_t)(u.x1 - u.x0) * 4);
	c->hist[n] = c->dirty;
	c->dirty.x0 = c->dirty.y0 = c->dirty.x1 = c->dirty.y1 = 0;
	c->cur = n;
	rs12_video_layer(c->slot, t, c->x, c->y, c->w, c->h);
}

void ui_hide(UiCanvas *c)
{
	rs12_video_layer(c->slot, NULL, 0, 0, 0, 0);
}

// (x * y) / 255 rounded (0..255)
static inline unsigned mul255(unsigned x, unsigned y)
{
	const unsigned t = x * y + 128;
	return (t + (t >> 8)) >> 8;
}

// premultiplied color src over dst (premultiplied over)
static inline uint32_t over(uint32_t dst, uint32_t src)
{
	const unsigned ia = 255 - (src >> 24);
	if (ia == 255)
		return dst;
	if (!ia)
		return src;
	const uint32_t rb = ((dst & 0x00FF00FFu) * ia + 0x00800080u), ag = (((dst >> 8) & 0x00FF00FFu) * ia + 0x00800080u);
	const uint32_t rb2 = ((rb + ((rb >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
	const uint32_t ag2 = ((ag + ((ag >> 8) & 0x00FF00FFu)) >> 8) & 0x00FF00FFu;
	return src + (rb2 | (ag2 << 8));
}

// non-premultiplied color col to premultiplied with coverage cov (0..255)
static inline uint32_t premul(uint32_t col, unsigned cov)
{
	const unsigned a = mul255(col >> 24, cov);
	return mul255(col & 255, a) | (mul255((col >> 8) & 255, a) << 8) | (mul255((col >> 16) & 255, a) << 16) | (a << 24);
}

static inline void blend(uint32_t *d, uint32_t col, unsigned cov)
{
	*d = over(*d, premul(col, cov));
}

void ui_rect(UiCanvas *c, int x, int y, int w, int h, uint32_t col)
{
	int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y, x1 = x + w > c->w ? c->w : x + w, y1 = y + h > c->h ? c->h : y + h;
	if (x1 <= x0 || y1 <= y0)
		return;
	dirty_add(c, x0, y0, x1, y1);
	const uint32_t s = premul(col, 255);
	for (int yy = y0; yy < y1; yy++) {
		uint32_t *row = c->px + (size_t)yy * c->w;
		uint32_t memo_in = row[x0] ^ 1, memo_out = 0;
		for (int xx = x0; xx < x1; xx++) {
			const uint32_t d = row[xx];
			if (d != memo_in) {
				memo_in = d;
				memo_out = over(d, s);
			}
			row[xx] = memo_out;
		}
	}
}

// thick line, coverage from distance to the segment (round caps)
void ui_line(UiCanvas *c, float x0, float y0, float x1, float y1, float width, uint32_t col)
{
	const float hw = width * 0.5f;
	int bx0 = (int)floorf((x0 < x1 ? x0 : x1) - hw - 1), bx1 = (int)ceilf((x0 > x1 ? x0 : x1) + hw + 1);
	int by0 = (int)floorf((y0 < y1 ? y0 : y1) - hw - 1), by1 = (int)ceilf((y0 > y1 ? y0 : y1) + hw + 1);
	if (bx0 < 0) bx0 = 0;
	if (by0 < 0) by0 = 0;
	if (bx1 > c->w) bx1 = c->w;
	if (by1 > c->h) by1 = c->h;
	dirty_add(c, bx0, by0, bx1, by1);
	const float dx = x1 - x0, dy = y1 - y0, len2 = dx * dx + dy * dy;
	for (int y = by0; y < by1; y++)
		for (int x = bx0; x < bx1; x++) {
			const float px = (float)x + 0.5f - x0, py = (float)y + 0.5f - y0;
			float t = len2 > 0 ? (px * dx + py * dy) / len2 : 0;
			t = t < 0 ? 0 : t > 1 ? 1 : t;
			const float ex = px - t * dx, ey = py - t * dy;
			const float d = sqrtf(ex * ex + ey * ey);
			float cov = hw + 0.5f - d;
			if (cov <= 0)
				continue;
			if (cov > 1)
				cov = 1;
			blend(&c->px[(size_t)y * c->w + x], col, (unsigned)(cov * 255.0f));
		}
}

void ui_circle(UiCanvas *c, float cx, float cy, float r, uint32_t col)
{
	int bx0 = (int)floorf(cx - r - 1), bx1 = (int)ceilf(cx + r + 1), by0 = (int)floorf(cy - r - 1), by1 = (int)ceilf(cy + r + 1);
	if (bx0 < 0) bx0 = 0;
	if (by0 < 0) by0 = 0;
	if (bx1 > c->w) bx1 = c->w;
	if (by1 > c->h) by1 = c->h;
	dirty_add(c, bx0, by0, bx1, by1);
	for (int y = by0; y < by1; y++)
		for (int x = bx0; x < bx1; x++) {
			const float px = (float)x + 0.5f - cx, py = (float)y + 0.5f - cy;
			float cov = r + 0.5f - sqrtf(px * px + py * py);
			if (cov <= 0)
				continue;
			if (cov > 1)
				cov = 1;
			blend(&c->px[(size_t)y * c->w + x], col, (unsigned)(cov * 255.0f));
		}
}

void ui_save_ppm(UiCanvas *c, const char *path, uint32_t bg)
{
	FILE *f = fopen(path, "wb");
	if (!f)
		return;
	fprintf(f, "P6\n%d %d\n255\n", c->w, c->h);
	uint8_t *row = (uint8_t *)malloc((size_t)c->w * 3);
	for (int y = 0; y < c->h; y++) {
		for (int x = 0; x < c->w; x++) {
			const uint32_t d = over(bg | 0xFF000000u, c->px[(size_t)y * c->w + x]);
			row[x * 3] = (uint8_t)d;
			row[x * 3 + 1] = (uint8_t)(d >> 8);
			row[x * 3 + 2] = (uint8_t)(d >> 16);
		}
		fwrite(row, 1, (size_t)c->w * 3, f);
	}
	free(row);
	fclose(f);
}

int ui_text_width(float scale, const char *s)
{
	float w = 0;
	for (; *s; s++) {
		const unsigned ch = (unsigned char)*s;
		if (ch >= 32 && ch < 128)
			w += s_glyph[ch - 32].adv * scale;
	}
	return (int)(w + 0.5f);
}

// ---- per-scale glyphs (built once with bilinear scaling on first use: the menu uses only 4 scales) ----
typedef struct {
	float scale;
	uint8_t *px[96];
	int w[96], h[96];
} ScaledSet;
static ScaledSet s_scaled[8];
static int s_nscaled;

static const ScaledSet *scaled_set(float scale)
{
	for (int i = 0; i < s_nscaled; i++)
		if (s_scaled[i].scale == scale)
			return &s_scaled[i];
	if (s_nscaled >= 8)
		return NULL;
	ScaledSet *ss = &s_scaled[s_nscaled++];
	ss->scale = scale;
	const float inv = 1.0f / scale;
	for (int k = 0; k < 96; k++) {
		const Glyph *g = &s_glyph[k];
		if (!g->px)
			continue;
		const int w = (int)ceilf(g->w * scale) + 1, h = (int)ceilf(g->h * scale) + 1;
		uint8_t *o = (uint8_t *)calloc((size_t)w * h, 1);
		for (int y = 0; y < h; y++) {
			const float sy = ((float)y + 0.5f) * inv - 0.5f;
			const int iy = (int)floorf(sy);
			const float fy = sy - (float)iy;
			for (int x = 0; x < w; x++) {
				const float sx = ((float)x + 0.5f) * inv - 0.5f;
				const int ix = (int)floorf(sx);
				const float fx = sx - (float)ix;
				float a = 0;
				for (int q = 0; q < 4; q++) {
					const int xx = ix + (q & 1), yy = iy + (q >> 1);
					if (xx < 0 || yy < 0 || xx >= g->w || yy >= g->h)
						continue;
					a += ((q & 1) ? fx : 1 - fx) * ((q >> 1) ? fy : 1 - fy) * g->px[yy * g->w + xx];
				}
				o[y * w + x] = (uint8_t)(a > 255 ? 255 : a);
			}
		}
		ss->px[k] = o;
		ss->w[k] = w;
		ss->h[k] = h;
	}
	return ss;
}

int ui_text(UiCanvas *c, int x, int y, uint32_t col, float scale, const char *s)
{
	return ui_text_clip(c, x, y, col, scale, s, 0, c->w);
}

int ui_text_clip(UiCanvas *c, int x, int y, uint32_t col, float scale, const char *s, int cx0, int cx1)
{
	if (cx0 < 0) cx0 = 0;
	if (cx1 > c->w) cx1 = c->w;
	const ScaledSet *ss = scaled_set(scale);
	float pen = (float)x;
	for (; *s; s++) {
		const unsigned ch = (unsigned char)*s;
		if (ch < 32 || ch >= 128)
			continue;
		const int k = (int)ch - 32;
		const Glyph *g = &s_glyph[k];
		if (ss && ss->px[k]) {
			const int gx = (int)floorf(pen + g->left * scale + 0.5f), gy = (int)floorf((float)y - g->top * scale + 0.5f);
			const int w = ss->w[k], h = ss->h[k];
			dirty_add(c, gx < cx0 ? cx0 : gx, gy, gx + w > cx1 ? cx1 : gx + w, gy + h);
			for (int yy = 0; yy < h; yy++) {
				const int py = gy + yy;
				if (py < 0 || py >= c->h)
					continue;
				const uint8_t *src = ss->px[k] + yy * w;
				uint32_t *row = c->px + (size_t)py * c->w;
				for (int xx = 0; xx < w; xx++) {
					const int px = gx + xx;
					if (src[xx] && px >= cx0 && px < cx1)
						row[px] = over(row[px], premul(col, src[xx]));
				}
			}
		}
		pen += g->adv * scale;
	}
	return (int)(pen - x + 0.5f);
}
