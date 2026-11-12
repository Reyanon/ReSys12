// Neon corridor: the moving background beside the game picture. A corridor seen down its length: its walls' depth lines
// (rails) run from the screen's edges in to the picture's sides, rings on the walls flow out to the screen's edges, light
// pulses run out along the rails and dust drifts by the walls. Nothing shows inside the picture's columns (it covers them)
// The background and the rails do not move: made into vertices once (again when the picture's width or the theme changes)
// and kept in a vertex buffer; the rings, pulses and dust are made each frame. Lines are thin quads, added (blend add).
// From ReBOX (vita/src/frontend/neon.c), drawn with vitaGL here, its settings fixed at ReBOX's defaults. GPL-2.0-or-later.
#include "video/neon.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <psp2/kernel/processmgr.h>
#include <vitaGL.h>

#define SW 960
#define SH 544
#define ZM 40.0f       // the depth whose walls meet the picture's sides
#define DENSITY 2.0f   // rings between the screen's edge and the picture
#define SPEED 10.0f
#define GLOW 1.0f      // the halo's strength
#define BRIGHT 0.5f    // the lines'
#define NPULSE 60
#define NDUST 200
#define RAIL_SEG 16
#define STATIC_BYTES (512 * 1024)
#define DYN_MAX 9000   // vertices a frame

typedef struct { float x, y; uint32_t c; } Vtx;
#define VTX_COLOR ((const void *)8)

static float F, HW, HH, ZN, ZF, RING, cx, cy, edge;
static float s_lay_x = -1, s_lay_w = -1;
static uint32_t s_lay_acc;

// colours: the HTML's hsla(hue + dh, 85%, l%), the hue the theme accent's
static float s_hue, s_sat = 0.85f;   // (saturation 0: a grey theme's lines grey and white)
static uint32_t rgb_hsl(float h, float l)
{
	h = fmodf(h + 360.0f, 360.0f) / 60.0f;
	const float s = s_sat, c = (1.0f - fabsf(2.0f * l - 1.0f)) * s, x = c * (1.0f - fabsf(fmodf(h, 2.0f) - 1.0f)), m = l - c / 2;
	float r = 0, g = 0, b = 0;
	switch ((int)h) {
	case 0: r = c; g = x; break;
	case 1: r = x; g = c; break;
	case 2: g = c; b = x; break;
	case 3: g = x; b = c; break;
	case 4: r = x; b = c; break;
	default: r = c; b = x; break;
	}
	return (uint32_t)((r + m) * 255.0f + 0.5f) | (uint32_t)((g + m) * 255.0f + 0.5f) << 8 | (uint32_t)((b + m) * 255.0f + 0.5f) << 16;
}
static int s_add;   // (drawing added, GL_ONE GL_ONE: the colour is the colour times alpha)
static uint32_t col(uint32_t rgb, float a)
{
	if (s_add)
		a *= BRIGHT;
	if (a > 1.0f)
		a = 1.0f;
	if (a < 0.0f)
		a = 0.0f;
	if (s_add) {
		const unsigned k = (unsigned)(a * 256.0f);
		return ((rgb & 0xFF) * k >> 8) | (((rgb >> 8) & 0xFF) * k >> 8) << 8 | (((rgb >> 16) & 0xFF) * k >> 8) << 16 | 0xFF000000u;
	}
	return rgb | (uint32_t)(a * 255.0f + 0.5f) << 24;
}
static uint32_t C55, C62, C76, C82, C50, P65, P88, D80;   // (lightness, the pulses' hue +25, the dust's -20)

static float hue_of(uint32_t acc)
{
	const float r = (acc & 0xFF) / 255.0f, g = ((acc >> 8) & 0xFF) / 255.0f, b = ((acc >> 16) & 0xFF) / 255.0f;
	const float mx = fmaxf(r, fmaxf(g, b)), mn = fminf(r, fminf(g, b)), d = mx - mn;
	if (d < 0.12f)
		return -1.0f;   // (a grey theme: no hue, grey lines)
	float h = mx == r ? fmodf((g - b) / d, 6.0f) : mx == g ? (b - r) / d + 2.0f : (r - g) / d + 4.0f;
	h *= 60.0f;
	return h < 0 ? h + 360.0f : h;   // (0-360: below 0 means grey)
}

static void proj(float x, float y, float z, float *px, float *py)
{
	*px = cx + x * F / z;
	*py = cy + y * F / z;
}
static float smooth(float a, float b, float v)
{
	float t = (v - a) / (b - a);
	t = t < 0 ? 0 : t > 1 ? 1 : t;
	return t * t * (3 - 2 * t);
}
// fades out into the picture's columns
static float side(float sx) { return smooth(edge * 0.7f, edge * 1.02f, fabsf(sx - cx)); }
static float z_at(float t) { return 1.0f / (1.0f / ZN + (1.0f / ZF - 1.0f / ZN) * t); }

static Vtx *s_v;
static int s_n, s_max;
static void tri(float x0, float y0, uint32_t c0, float x1, float y1, uint32_t c1, float x2, float y2, uint32_t c2)
{
	if (s_n + 3 > s_max)
		return;
	Vtx *v = s_v + s_n;
	v[0] = (Vtx){ x0, y0, c0 };
	v[1] = (Vtx){ x1, y1, c1 };
	v[2] = (Vtx){ x2, y2, c2 };
	s_n += 3;
}
// a line a-b as a quad, its widths and colours at each end (thinner than a pixel: a pixel wide and fainter)
static void seg(float ax, float ay, float bx, float by, float wa, float wb, uint32_t rgb, float aa, float ab)
{
	if (wa < 1.0f) {
		aa *= wa;
		wa = 1.0f;
	}
	if (wb < 1.0f) {
		ab *= wb;
		wb = 1.0f;
	}
	float dx = bx - ax, dy = by - ay;
	const float len = sqrtf(dx * dx + dy * dy);
	if (len < 0.01f)
		return;
	dx /= len;
	dy /= len;
	const float nax = -dy * wa * 0.5f, nay = dx * wa * 0.5f, nbx = -dy * wb * 0.5f, nby = dx * wb * 0.5f;
	const uint32_t ca = col(rgb, aa), cb = col(rgb, ab);
	tri(ax + nax, ay + nay, ca, ax - nax, ay - nay, ca, bx + nbx, by + nby, cb);
	tri(bx + nbx, by + nby, cb, ax - nax, ay - nay, ca, bx - nbx, by - nby, cb);
}
static void rect(float x0, float y0, float x1, float y1, uint32_t c0, uint32_t c1)   // (colours left, right)
{
	tri(x0, y0, c0, x0, y1, c0, x1, y0, c1);
	tri(x1, y0, c1, x0, y1, c0, x1, y1, c1);
}

static uint32_t s_rand = 0x2545F491;
static float rnd(void)
{
	s_rand = s_rand * 1664525u + 1013904223u;
	return (s_rand >> 8) * (1.0f / 16777216.0f);
}

// rails (depth lines): x, y in corridor halves, major
#define NRAIL (33 * 2 + 8 * 4)
static float s_rail[NRAIL][3];
static void rails_make(void)
{
	int n = 0;
	for (int i = 0; i <= 32; i++) {
		const float fy = -1.0f + i / 16.0f;
		for (int s = -1; s <= 1; s += 2) {
			s_rail[n][0] = (float)s;
			s_rail[n][1] = fy;
			s_rail[n++][2] = i % 4 == 0;
		}
	}
	for (int i = 0; i < 8; i++)
		for (int s = -1; s <= 1; s += 2)
			for (int f = -1; f <= 1; f += 2) {
				s_rail[n][0] = s * (1.0f - i * 0.06f);
				s_rail[n][1] = (float)f;
				s_rail[n++][2] = i == 0;
			}
}

static struct { float z, v, len; int r; } s_pulse[NPULSE];
static struct { float sx, fx, fy, t; } s_dust[NDUST];
static float s_travel, s_time;
static uint64_t s_last;

static GLuint s_vbo;
static int s_nbg, s_nstatic;
static Vtx s_dyn[DYN_MAX];

static void layout(float gx, float gw)
{
	float c = gw / SW;
	c = c < 0.2f ? 0.2f : c > 0.96f ? 0.96f : c;
	F = (SW < SH ? SW : SH) * 0.9f;
	edge = gw * 0.5f;
	cx = gx + gw * 0.5f;
	cy = SH * 0.5f;
	HW = edge * ZM / F;
	HH = (SH / 2) * 1.05f * ZM * 1.6f / F;
	ZN = ZM * c * 0.9f;
	ZF = ZM * 1.6f;
	RING = (ZM - ZM * c) / DENSITY;
	s_travel = fmodf(s_travel, RING * 5.0f);
}

// the background and the rails into the kept vertices
static void static_make(void)
{
	s_v = (Vtx *)malloc(STATIC_BYTES);
	if (!s_v)
		return;
	s_n = 0;
	s_max = STATIC_BYTES / sizeof(Vtx);
	rails_make();
	// the ground, the bands darkest at the screen's edges and melting into it towards the picture, a faint glow in each
	const uint32_t ground = 0xFF26211C, deep = 0xFF130F0A, mid = 0xFF1F1A14, clear = 0x0026211C;
	rect(0, 0, SW, SH, ground, ground);
	const float band = SW * 0.5f - edge;
	for (int s = -1; s <= 1; s += 2) {
		const float x0 = s < 0 ? 0 : SW, x1 = cx + s * edge * 0.85f, xm = x0 + (x1 - x0) * 0.7f;
		if (s < 0) {
			rect(x0, 0, xm, SH, deep, mid);
			rect(xm, 0, x1, SH, mid, clear);
		} else {
			rect(xm, 0, x0, SH, mid, deep);
			rect(x1, 0, xm, SH, clear, mid);
		}
		const float gx = cx + s * (edge + band * 0.45f), r = fmaxf(band * 1.6f, SH * 0.45f);
		const uint32_t c0 = col(C50, 0.10f * GLOW), c1 = col(C50, 0);
		for (int k = 0; k < 32; k++) {
			const float a0 = k * (6.2831853f / 32), a1 = (k + 1) * (6.2831853f / 32);
			tri(gx, cy, c0, gx + cosf(a0) * r, cy + sinf(a0) * r, c1, gx + cosf(a1) * r, cy + sinf(a1) * r, c1);
		}
	}
	s_nbg = s_n;
	s_add = 1;
	for (int i = 0; i < NRAIL; i++) {
		const float x = s_rail[i][0] * HW, y = s_rail[i][1] * HH, k8 = s_rail[i][2] ? 1.0f : 0.5f;
		float px[RAIL_SEG + 1], py[RAIL_SEG + 1], a[RAIL_SEG + 1], w[RAIL_SEG + 1];
		for (int k = 0; k <= RAIL_SEG; k++) {
			const float z = z_at((float)k / RAIL_SEG);
			proj(x, y, z, &px[k], &py[k]);
			a[k] = side(px[k]) * k8;
			w[k] = fmaxf(0.6f, F / z * HW * 0.0009f);
		}
		for (int k = 0; k < RAIL_SEG; k++) {
			if (a[k] < 0.01f && a[k + 1] < 0.01f)
				continue;
			if ((py[k] < -8 && py[k + 1] < -8) || (py[k] > SH + 8 && py[k + 1] > SH + 8))
				continue;
			if (GLOW > 0)
				seg(px[k], py[k], px[k + 1], py[k + 1], w[k] * 5, w[k + 1] * 5, C55, a[k] * 0.07f * GLOW, a[k + 1] * 0.07f * GLOW);
			seg(px[k], py[k], px[k + 1], py[k + 1], w[k], w[k + 1], s_rail[i][2] ? C76 : C62, a[k] * 0.7f, a[k + 1] * 0.7f);
		}
	}
	s_nstatic = s_n;
	s_add = 0;
	if (!s_vbo)
		glGenBuffers(1, &s_vbo);
	glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
	glBufferData(GL_ARRAY_BUFFER, s_n * sizeof(Vtx), s_v, GL_STATIC_DRAW);
	glBindBuffer(GL_ARRAY_BUFFER, 0);
	free(s_v);
	s_v = NULL;
}

static void pulse_new(int i, float z)
{
	s_pulse[i].r = (int)(rnd() * NRAIL) % NRAIL;
	s_pulse[i].z = z;
	s_pulse[i].v = 0.6f + rnd() * 1.2f;
	s_pulse[i].len = 0.8f + rnd() * 2.0f;
}

void rs12_neon_draw(float game_x, float game_w, uint32_t accent)
{
	static int init;
	if (!init) {
		init = 1;
		for (int i = 0; i < NDUST; i++) {
			s_dust[i].sx = rnd() < 0.5f ? -1.0f : 1.0f;
			s_dust[i].fx = 0.75f + rnd() * 0.25f;
			s_dust[i].fy = rnd() * 2 - 1;
			s_dust[i].t = rnd();
		}
	}
	if (game_x != s_lay_x || game_w != s_lay_w || accent != s_lay_acc || !s_vbo) {
		const int first = s_lay_w < 0;
		s_lay_x = game_x;
		s_lay_w = game_w;
		s_lay_acc = accent;
		s_hue = hue_of(accent);
		s_sat = s_hue < 0 ? 0.0f : 0.85f;
		if (s_hue < 0)
			s_hue = 0;
		C50 = rgb_hsl(s_hue, 0.50f);
		C55 = rgb_hsl(s_hue, 0.55f);
		C62 = rgb_hsl(s_hue, 0.62f);
		C76 = rgb_hsl(s_hue, 0.76f);
		C82 = rgb_hsl(s_hue, 0.82f);
		P65 = rgb_hsl(s_hue + 25, 0.65f);
		P88 = rgb_hsl(s_hue + 25, 0.88f);
		D80 = rgb_hsl(s_hue - 20, 0.80f);
		layout(game_x, game_w);
		if (first)
			for (int i = 0; i < NPULSE; i++)
				pulse_new(i, ZN + rnd() * (ZF - ZN));
		static_make();
	}
	if (!s_vbo)
		return;
	const uint64_t now = sceKernelGetProcessTimeWide();
	float dt = s_last ? (now - s_last) / 1e6f : 0;
	s_last = now;
	if (dt > 0.05f)
		dt = 0.05f;
	s_travel = fmodf(s_travel + SPEED * dt * 0.22f, RING * 5.0f);
	s_time += dt;

	glDisable(GL_TEXTURE_2D);
	glEnable(GL_BLEND);
	glEnableClientState(GL_VERTEX_ARRAY);
	glEnableClientState(GL_COLOR_ARRAY);
	glBindBuffer(GL_ARRAY_BUFFER, s_vbo);
	glVertexPointer(2, GL_FLOAT, sizeof(Vtx), (const void *)0);
	glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(Vtx), VTX_COLOR);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
	glDrawArrays(GL_TRIANGLES, 0, s_nbg);
	glBlendFunc(GL_ONE, GL_ONE);
	glDrawArrays(GL_TRIANGLES, s_nbg, s_nstatic - s_nbg);
	glBindBuffer(GL_ARRAY_BUFFER, 0);

	s_add = 1;
	s_v = s_dyn;
	s_n = 0;
	s_max = DYN_MAX;
	// rings: upright on the walls, flowing out to the screen's edges
	int idx = (int)ceilf((ZF + s_travel) / RING);
	for (float z = idx * RING - s_travel; z > ZN * 0.98f && s_v; z -= RING, idx--) {
		if (z > ZF)
			continue;
		const int major = idx % 5 == 0;
		for (int s = -1; s <= 1; s += 2) {
			float tx, ty, bx, by, ix0, iy0, ix1, iy1;
			proj(s * HW, -HH, z, &tx, &ty);
			proj(s * HW, HH, z, &bx, &by);
			proj(s * HW * 0.8f, -HH, z, &ix0, &iy0);
			proj(s * HW * 0.8f, HH, z, &ix1, &iy1);
			const float w8 = side(tx);
			if (w8 < 0.01f)
				continue;
			const float w = fmaxf(0.6f, F / z * HW * (major ? 0.0018f : 0.0009f));
			const float ga = w8 * (major ? 0.14f : 0.05f) * GLOW, ca = w8 * (major ? 0.95f : 0.5f);
			const uint32_t cc = major ? C82 : C62;
			if (ty > 0) {
				seg(ix0, iy0, tx, ty, w * 6, w * 6, C55, ga, ga);
				seg(ix0, iy0, tx, ty, w, w, cc, ca, ca);
			}
			seg(tx, fmaxf(ty, -8), bx, fminf(by, SH + 8), w * 6, w * 6, C55, ga, ga);
			seg(tx, fmaxf(ty, -8), bx, fminf(by, SH + 8), w, w, cc, ca, ca);
			if (by < SH) {
				seg(bx, by, ix1, iy1, w * 6, w * 6, C55, ga, ga);
				seg(bx, by, ix1, iy1, w, w, cc, ca, ca);
			}
		}
	}
	// pulses: light running out along the rails
	for (int i = 0; i < NPULSE && s_v; i++) {
		s_pulse[i].z -= SPEED * s_pulse[i].v * dt * 0.5f;
		if (s_pulse[i].z < ZN * 0.9f)
			pulse_new(i, ZF);
		const float x = s_rail[s_pulse[i].r][0] * HW, y = s_rail[s_pulse[i].r][1] * HH;
		for (int k = 0; k < 8; k++) {
			const float z0 = s_pulse[i].z + s_pulse[i].len * k / 8, z1 = s_pulse[i].z + s_pulse[i].len * (k + 1) / 8;
			float ax, ay, bx, by;
			proj(x, y, z0, &ax, &ay);
			proj(x, y, z1, &bx, &by);
			const float w8 = side(ax) * (1.0f - k / 8.0f);
			if (w8 < 0.01f || ay < -8 || ay > SH + 8)
				continue;
			const float w = fmaxf(1.0f, F / z0 * HW * 0.0025f);
			seg(ax, ay, bx, by, w * 4, w * 4, P65, w8 * 0.2f * (0.3f + GLOW), w8 * 0.2f * (0.3f + GLOW));
			seg(ax, ay, bx, by, w, w, P88, w8, w8);
		}
	}
	// dust drifting out by the walls
	for (int i = 0; i < NDUST && s_v; i++) {
		s_dust[i].t -= dt * SPEED * 0.012f;
		if (s_dust[i].t < 0) {
			s_dust[i].t = 1;
			s_dust[i].fy = rnd() * 2 - 1;
			s_dust[i].fx = 0.75f + rnd() * 0.25f;
		}
		const float z = ZN + (ZM * 1.15f - ZN) * s_dust[i].t;
		float sx, sy;
		proj(s_dust[i].sx * s_dust[i].fx * HW, s_dust[i].fy * HH + sinf(s_time + s_dust[i].fx * 20) * HH * 0.02f, z, &sx, &sy);
		float a = side(sx) * 0.7f;
		if (a < 0.01f || sy < -4 || sy > SH + 4)
			continue;
		float r = fmaxf(0.5f, F / z * HW * 0.0012f);
		if (r < 0.75f) {
			a *= r / 0.75f;
			r = 0.75f;
		}
		const uint32_t c = col(D80, a);
		rect(sx - r, sy - r, sx + r, sy + r, c, c);
	}
	if (s_n) {
		glVertexPointer(2, GL_FLOAT, sizeof(Vtx), s_dyn);
		glColorPointer(4, GL_UNSIGNED_BYTE, sizeof(Vtx), (const uint8_t *)s_dyn + 8);
		glDrawArrays(GL_TRIANGLES, 0, s_n);
	}
	glDisableClientState(GL_VERTEX_ARRAY);
	glDisableClientState(GL_COLOR_ARRAY);
	glDisable(GL_BLEND);
	glEnable(GL_TEXTURE_2D);
	s_add = 0;
}
