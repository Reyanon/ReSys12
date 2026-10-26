// In-game menu, start screen game select, notices, FPS box, loading screen. Ported from ReST-V vita/src/frontend/menu.c
// (common UI of the Re series of Vita emulators): same layout, controls, animations and features; red theme colours and
// the System 12 game table, gun / wheel / lever pages and combo keys.
// Drawing is in ui.c (system default PGF font drawn to a CPU canvas and overlaid on the screen).
#include "frontend/menu.h"

#include <psp2/appmgr.h>
#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/rtc.h>
#include <psp2/shellutil.h>
#include <taihen.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frontend/touch.h"
#include "frontend/ui.h"
#include "frontend/update.h"
#include "platform/log.h"
#include "platform/timer.h"
#include "rs12_kctrl.h"
#include "rs12_version.h" // generated at build time (tools/git_version.cmake)
#include "s12.h" // s12_analog_kind (gun / wheel / lever rows)
#include "s12_rom.h"
#include "video/video.h"

#define DATA_DIR "ux0:data/resys12"
#define SETTINGS_PATH DATA_DIR "/settings.cfg"
#define CFG_DIR DATA_DIR "/cfg"
#define FAV_PATH DATA_DIR "/favorites.cfg"
#define MENU_OPEN_COMBO (SCE_CTRL_SELECT | SCE_CTRL_START)

#define SCREEN_W 960
#define SCREEN_H 544
#define PANEL_W  640
#define LINE_H   40 // menu rows (pages taller than the screen scroll)
#define ABOUT_LH 30 // About text lines
#define GL_LINE  44 // game list rows (the list scrolls)
#define TITLE_H  56
#define FOOT_H   48
#define VALUE_X  330 // from panel left edge to the value column

// Colour themes (Display > Theme; ReST-V's layout and shapes). Red is the ReSys12 default, Blue is ReST-V's. A theme
// is a gradient (top, bottom), the panel, the accent (selection), dim text, row text and the outline colour; the other
// colours are made from these
typedef struct { uint8_t top[3], bot[3], panel[3], sel[3], dim[3], item[3], edge[3]; } ThemeDef;
static const ThemeDef s_theme_def[RS12_THEME_COUNT] = {
	{ { 70, 6, 14 }, { 180, 22, 40 }, { 0x46, 0x0A, 0x12 }, { 0xFF, 0x7C, 0x7C }, { 0xD8, 0xA4, 0xAC }, { 0xFA, 0xDC, 0xE0 }, { 0xE0, 0x4C, 0x5C } },
	{ { 6, 22, 70 }, { 20, 86, 180 }, { 0x0A, 0x1C, 0x46 }, { 0x6C, 0xD0, 0xFF }, { 0x9C, 0xB0, 0xD4 }, { 0xD6, 0xE2, 0xFA }, { 0x4C, 0x8C, 0xE0 } },
	{ { 6, 50, 24 }, { 20, 136, 66 }, { 0x0A, 0x36, 0x1C }, { 0x7C, 0xF0, 0xA0 }, { 0xA0, 0xCC, 0xAC }, { 0xDA, 0xF6, 0xE0 }, { 0x4C, 0xC0, 0x78 } },
	{ { 40, 8, 70 }, { 110, 40, 180 }, { 0x2C, 0x0E, 0x4A }, { 0xD0, 0x9C, 0xFF }, { 0xC0, 0xA8, 0xD8 }, { 0xEC, 0xDE, 0xFA }, { 0xA0, 0x6C, 0xE0 } },
	{ { 70, 30, 4 }, { 186, 88, 20 }, { 0x48, 0x22, 0x08 }, { 0xFF, 0xC4, 0x6C }, { 0xDC, 0xBC, 0x9C }, { 0xFA, 0xEA, 0xD6 }, { 0xE8, 0x96, 0x40 } },
	{ { 24, 24, 28 }, { 70, 72, 82 }, { 0x26, 0x28, 0x2E }, { 0xE0, 0xE6, 0xF0 }, { 0xA8, 0xAC, 0xB4 }, { 0xE2, 0xE4, 0xE8 }, { 0x90, 0x94, 0xA0 } },
};
static const char *const s_theme_name[RS12_THEME_COUNT] = { "Red", "Blue", "Green", "Purple", "Orange", "Graphite" };
typedef struct {
	uint32_t bg, panel, item, dim, sel, hilite, edge, thumb, focus_still, focus_bright, focus_glow, shade, band, sep, toggle_on,
		toggle_off, grey, grey_sel, bar_track, bar_top, bar_bot, glow1, glow2, glow3, spinner;
	uint8_t panel_rgb[3];
} Theme;
static Theme s_th;
static int s_theme_cur = -1, s_theme_gen; // theme the colours were made for; counts changes (redraw what was kept)
static uint32_t rgb(const uint8_t *c, unsigned a) { return UI_RGBA(c[0], c[1], c[2], a); }
static uint32_t mix(const uint8_t *a, const uint8_t *b, int t, unsigned al) // a..b, t/256 towards b
{
	return UI_RGBA(a[0] + ((b[0] - a[0]) * t >> 8), a[1] + ((b[1] - a[1]) * t >> 8), a[2] + ((b[2] - a[2]) * t >> 8), al);
}
static void theme_make(int k)
{
	static const uint8_t white[3] = { 0xFF, 0xFF, 0xFF }, black[3] = { 0, 0, 0 };
	const ThemeDef *d = &s_theme_def[k];
	Theme *t = &s_th;
	t->bg = mix(d->top, black, 180, 0xC0);
	t->panel = rgb(d->panel, 0xFF); // opaque: filled without reading what is under it (fast)
	memcpy(t->panel_rgb, d->panel, 3);
	t->item = rgb(d->item, 0xFF);
	t->dim = rgb(d->dim, 0xFF);
	t->sel = rgb(d->sel, 0xFF);
	t->hilite = mix(d->sel, white, 40, 0x30);
	t->edge = rgb(d->edge, 0xFF);
	t->thumb = mix(d->sel, white, 50, 0xB0);
	t->focus_still = rgb(d->sel, 0xB0);
	t->focus_bright = mix(d->sel, white, 190, 0xFF);
	t->focus_glow = rgb(d->sel, 0x50);
	t->shade = mix(d->top, black, 200, 0x30);
	t->band = mix(d->top, black, 170, 0xFF);
	t->sep = mix(d->edge, white, 60, 0xFF);
	t->toggle_on = mix(d->edge, d->sel, 100, 0xFF);
	t->toggle_off = mix(d->dim, d->panel, 128, 0xFF);
	t->grey = mix(d->dim, d->panel, 70, 0xFF);
	t->grey_sel = mix(d->dim, d->item, 128, 0xFF);
	t->bar_track = mix(d->panel, black, 80, 0xE0);
	t->bar_top = mix(d->sel, white, 40, 0xFF);
	t->bar_bot = mix(d->edge, black, 40, 0xFF);
	t->glow1 = rgb(d->sel, 0x28);
	t->glow2 = mix(d->sel, white, 60, 0x60);
	t->glow3 = mix(d->sel, white, 220, 0xFF);
	t->spinner = mix(d->sel, white, 110, 0xFF);
	s_theme_cur = k;
	s_theme_gen++;
}
static const Theme *theme(void)
{
	const int k = g_set.theme >= 0 && g_set.theme < RS12_THEME_COUNT ? g_set.theme : 0;
	if (k != s_theme_cur)
		theme_make(k);
	return &s_th;
}
#define COL_BG    (theme()->bg)
#define COL_PANEL (theme()->panel)
#define COL_TEXT  UI_RGBA(0xFF, 0xFF, 0xFF, 0xFF)
#define COL_ITEM  (theme()->item) // rows that are not selected
#define COL_DIM   (theme()->dim)
#define COL_SEL   (theme()->sel)
#define COL_BRAND UI_RGBA(0xFF, 0x40, 0x40, 0xFF) // "Sys12" as in the app art (every theme)
#define COL_WARN  UI_RGBA(0xFF, 0xC0, 0x40, 0xFF)
#define COL_HILITE (theme()->hilite) // selected row bar
#define COL_EDGE  (theme()->edge)   // panel outline, separators
#define COL_THUMB (theme()->thumb)  // scroll bar

// button symbols inside text (drawn by draw_key)
#define G_TRI "\x10"
#define G_CIR "\x11"
#define G_CRO "\x12"
#define G_SQU "\x13"

#define STATE_SLOTS 10
#define TEST_NOTICE_MSG "State 1 saved"

// rounded rectangle drawn row by row (no overlapping parts, so translucent colours stay even)
static void rrect(UiCanvas *c, int x, int y, int w, int h, int r, uint32_t col)
{
	if (r * 2 > h) r = h / 2;
	if (r * 2 > w) r = w / 2;
	for (int yy = 0; yy < h; yy++) {
		int in = 0;
		const int d = yy < r ? r - yy : (yy >= h - r ? yy - (h - r - 1) : 0);
		if (d > 0) {
			const float dy = (float)d - 0.5f;
			in = r - (int)sqrtf((float)(r * r) - dy * dy);
		}
		ui_rect(c, x + in, y + yy, w - 2 * in, 1, col);
	}
}
// outline of a rounded rectangle, t px thick (rows of the outer shape minus the inner one: no overlap)
static void rrect_outline(UiCanvas *c, int x, int y, int w, int h, int r, int t, uint32_t col)
{
	if (r * 2 > h) r = h / 2;
	const int ri = r - t > 0 ? r - t : 0;
	for (int yy = 0; yy < h; yy++) {
		int in = 0;
		const int d = yy < r ? r - yy : (yy >= h - r ? yy - (h - r - 1) : 0);
		if (d > 0) {
			const float dy = (float)d - 0.5f;
			in = r - (int)sqrtf((float)(r * r) - dy * dy);
		}
		if (yy < t || yy >= h - t) {
			ui_rect(c, x + in, y + yy, w - 2 * in, 1, col);
			continue;
		}
		int ii = 0; // inner shape inset on this row (inner rect is t smaller on every side)
		const int yi = yy - t, hi = h - 2 * t, di = yi < ri ? ri - yi : (yi >= hi - ri ? yi - (hi - ri - 1) : 0);
		if (di > 0 && ri > 0) {
			const float dy = (float)di - 0.5f;
			ii = ri - (int)sqrtf((float)(ri * ri) - dy * dy);
		}
		const int l0 = x + in, l1 = x + t + ii, r0 = x + w - t - ii, r1 = x + w - in;
		if (l1 > l0) ui_rect(c, l0, y + yy, l1 - l0, 1, col);
		if (r1 > r0) ui_rect(c, r0, y + yy, r1 - r0, 1, col);
	}
}
// The focus outline blinks. It is a small layer of its own (slot 1, over the menu canvas), drawn once per position; the
// blink is that layer's opacity, set every frame (no CPU drawing). Only the topmost focus blinks; a focus under an open
// window keeps a steady outline
#define COL_FOCUS_STILL (theme()->focus_still)
static UiCanvas *s_focus_cv;
static int s_focus_x = -1, s_focus_y, s_focus_w, s_focus_h, s_focus_on, s_focus_want, s_focus_gen;
static int s_focus_cy0, s_focus_cy1 = SCREEN_H; // rows the focus layer may cover (the list / menu rows area)
static void focus_at(UiCanvas *c, int x, int y, int w, int h)
{
	enum { CW = SCREEN_W, CH = 56 };
	if (y - 2 < s_focus_cy0 || y + h + 2 > s_focus_cy1) {
		// partly past the rows area (while scrolling): a steady outline in the canvas, cut there like the rows
		rrect_outline(c, x, y, w, h, 6, 2, COL_FOCUS_STILL);
		return;
	}
	s_focus_want = 1;
	if (!s_focus_cv)
		s_focus_cv = ui_canvas(1, 0, 0, CW, CH);
	if (!s_focus_cv || w + 4 > CW || h + 4 > CH)
		return;
	if (x == s_focus_x && y == s_focus_y && w == s_focus_w && h == s_focus_h && s_focus_on && s_focus_gen == s_theme_gen)
		return;
	if (w != s_focus_w || h != s_focus_h || s_focus_x < 0 || s_focus_gen != s_theme_gen) {
		ui_clear(s_focus_cv);
		rrect_outline(s_focus_cv, 2, 2, w, h, 6, 2, theme()->focus_bright);
		rrect_outline(s_focus_cv, 0, 0, w + 4, h + 4, 8, 2, theme()->focus_glow); // soft glow outside
		s_focus_gen = s_theme_gen;
	}
	s_focus_x = x;
	s_focus_y = y;
	s_focus_w = w;
	s_focus_h = h;
	ui_canvas_move(s_focus_cv, x - 2, y - 2);
	ui_commit(s_focus_cv);
	s_focus_on = 1;
}
static void focus_off(void)
{
	if (s_focus_on)
		ui_hide(s_focus_cv);
	s_focus_on = 0;
}
static void focus_begin(void)
{
	s_focus_want = 0;
	s_focus_cy0 = 0;
	s_focus_cy1 = SCREEN_H;
}
static void focus_end(void)
{
	if (!s_focus_want)
		focus_off();
}
static void focus_tick(void)
{
	if (!s_focus_on)
		return;
	const float p = 0.5f + 0.5f * sinf((float)(rs12_time_us() % 1000000) * 6.2832f / 1e6f);
	rs12_video_layer_alpha(1, 0.12f + 0.88f * p);
}
// favourites as a star: five points, edges smoothed (4 x 4 samples a pixel)
static void star(UiCanvas *c, float cx, float cy, float r, uint32_t rgb)
{
	float vx[10], vy[10];
	for (int k = 0; k < 10; k++) {
		const float a = -1.5708f + k * 3.14159f / 5.0f, rr = (k & 1) ? r * 0.42f : r;
		vx[k] = cx + rr * cosf(a);
		vy[k] = cy + rr * sinf(a);
	}
	for (int py = (int)(cy - r) - 1; py <= (int)(cy + r) + 1; py++)
		for (int px = (int)(cx - r) - 1; px <= (int)(cx + r) + 1; px++) {
			int hits = 0;
			for (int s = 0; s < 16; s++) {
				const float sx = px + 0.125f + 0.25f * (s & 3), sy = py + 0.125f + 0.25f * (s >> 2);
				int in = 0;
				for (int a = 0, b = 9; a < 10; b = a++)
					if ((vy[a] > sy) != (vy[b] > sy) && sx < (vx[b] - vx[a]) * (sy - vy[a]) / (vy[b] - vy[a]) + vx[a])
						in = !in;
				hits += in;
			}
			if (hits)
				ui_rect(c, px, py, 1, 1, (rgb & 0x00FFFFFFu) | ((uint32_t)(hits * 255 / 16) << 24));
		}
}
#define COL_STAR UI_RGBA(0xFF, 0xD2, 0x48, 0xFF)
// windows with slightly rounded corners and a soft shadow: drop shadow of a window (offset 4, 6, soft over 9 px), only
// outside it: straight edges as one-colour runs, the corners a pixel at a time
enum { SH_OX = 4, SH_OY = 6, SH_B = 9, SH_A = 0x58 };
static uint32_t sh_col(int dx, int dy)
{
	const int d2 = dx * dx + dy * dy;
	return d2 >= SH_B * SH_B ? 0 : UI_RGBA(0x04, 0x04, 0x06, SH_A * (SH_B * SH_B - d2) / (SH_B * SH_B));
}
static void sh_block(UiCanvas *c, int x0, int y0, int x1, int y1, int sx0, int sy0, int sx1, int sy1)
{
	for (int yy = y0; yy < y1; yy++)
		for (int xx = x0; xx < x1; xx++) {
			const uint32_t col = sh_col(xx < sx0 ? sx0 - xx : (xx >= sx1 ? xx - sx1 + 1 : 0), yy < sy0 ? sy0 - yy : (yy >= sy1 ? yy - sy1 + 1 : 0));
			if (col)
				ui_rect(c, xx, yy, 1, 1, col);
		}
}
static void window_box(UiCanvas *c, int x, int y, int w, int h, uint32_t edge, uint32_t fill)
{
	const int sx0 = x + SH_OX, sy0 = y + SH_OY, sx1 = x + w + SH_OX, sy1 = y + h + SH_OY, B = SH_B;
	const int ex = x + w + 1, ey = y + h + 1; // first column / row past the window and its outline
	for (int xx = ex; xx < sx1 + B; xx++) { // right edge (rows beside the window)
		const uint32_t col = sh_col(xx >= sx1 ? xx - sx1 + 1 : 0, 0);
		if (col)
			ui_rect(c, xx, sy0, 1, ey - sy0, col);
	}
	for (int xx = sx0 - B; xx < x - 1; xx++) { // left edge
		const uint32_t col = sh_col(sx0 - xx, 0);
		if (col)
			ui_rect(c, xx, sy0, 1, ey - sy0, col);
	}
	for (int yy = ey; yy < sy1 + B; yy++) { // bottom edge
		const uint32_t col = sh_col(0, yy >= sy1 ? yy - sy1 + 1 : 0);
		if (col)
			ui_rect(c, sx0, yy, ex - sx0, 1, col);
	}
	sh_block(c, ex, sy0 - B, sx1 + B, sy0, sx0, sy0, sx1, sy1); // corners
	sh_block(c, ex, ey, sx1 + B, sy1 + B, sx0, sy0, sx1, sy1);
	sh_block(c, sx0 - B, ey, sx0, sy1 + B, sx0, sy0, sx1, sy1);
	rrect_outline(c, x - 1, y - 1, w + 2, h + 2, 7, 1, edge);
	rrect(c, x, y, w, h, 6, fill);
}
// the menu background: one colour per row (vertical gradient; the list also gets its header and footer bands). Filled
// row by row: copying a whole-screen image took 8 ms on the device
static uint32_t blend_px(uint32_t d, uint32_t c)
{
	const unsigned a = c >> 24;
	uint32_t o = 0xFF000000u;
	for (int k = 0; k < 24; k += 8)
		o |= ((((d >> k) & 0xFF) * (255 - a) + ((c >> k) & 0xFF) * a + 127) / 255) << k;
	return o;
}
#define LIST_FOOT_H 40 // the list's footer band (one line of button help)
static const uint32_t *theme_rows(int bands)
{
	static uint32_t rows[2][SCREEN_H];
	static int made_gen = -1;
	const Theme *th = theme();
	if (made_gen != s_theme_gen) {
		made_gen = s_theme_gen;
		const ThemeDef *d = &s_theme_def[s_theme_cur];
		const uint32_t band = th->band & 0xFFFFFFu, sep = th->sep & 0xFFFFFFu;
		for (int y = 0; y < SCREEN_H; y++) {
			const int t = y * 256 / (SCREEN_H - 1);
			rows[0][y] = rows[1][y] = mix(d->top, d->bot, t, 0xFF); // deep -> bright
			if (y < TITLE_H - 8)
				rows[1][y] = blend_px(rows[1][y], band | 0x50000000u);
			else if (y == TITLE_H - 8 || y == SCREEN_H - LIST_FOOT_H)
				rows[1][y] = blend_px(rows[1][y], sep | 0x60000000u);
			else if (y > SCREEN_H - LIST_FOOT_H)
				rows[1][y] = blend_px(rows[1][y], band | 0x60000000u);
		}
	}
	return rows[bands ? 1 : 0];
}

// ================================ settings ================================
rs12_settings g_set;

static const char *const s_keymap_keys[RS12_ACT_COUNT] = { "key_b1", "key_b2", "key_b3", "key_b4", "key_b5", "key_b6", "key_coin",
	"key_start" };
static int s_changed; // settings changed since the menu opened (saved on close only then)

void rs12_settings_default_keys(void)
{
	// Tekken layout: Square/Triangle = left/right punch, Cross/Circle = left/right kick, R = button 5 (tag), L = button 6
	g_set.keymap[RS12_ACT_B1] = RS12_KEY_SQUARE;
	g_set.keymap[RS12_ACT_B2] = RS12_KEY_TRIANGLE;
	g_set.keymap[RS12_ACT_B3] = RS12_KEY_CROSS;
	g_set.keymap[RS12_ACT_B4] = RS12_KEY_CIRCLE;
	g_set.keymap[RS12_ACT_B5] = RS12_KEY_R;
	g_set.keymap[RS12_ACT_B6] = RS12_KEY_L;
	g_set.keymap[RS12_ACT_COIN] = RS12_KEY_SELECT;
	g_set.keymap[RS12_ACT_START] = RS12_KEY_START;
	g_set.stick = RS12_STICK_LEFT;
	g_set.aim_stick = RS12_AIM_LEFT;
	g_set.aim_mode = RS12_AIM_POSITION;
	g_set.gun_sens = 5;
	g_set.crosshair = 1;
	g_set.wheel_stick = RS12_AIM_LEFT;
	g_set.lever_stick = RS12_AIM_RIGHT;
	g_set.gun_fire = RS12_KEY_R;
	g_set.gun_sensor = RS12_KEY_L;
	g_set.gas_key = RS12_KEY_R;
	g_set.brake_key = RS12_KEY_L;
	g_set.touch_keys = 1;
	g_set.touch_show = 1;
	g_set.touch_gun = 0;
	g_set.turbo_key = RS12_KEY_NONE;
	g_set.turbo_btn = 1;
	g_set.turbo_speed = RS12_TURBO_FAST;
	memset(g_set.combo, 0, sizeof(g_set.combo)); // no combos
}

void rs12_settings_defaults(void)
{
	g_set.fps = 0;
	g_set.aspect = RS12_ASPECT_4_3;
	g_set.filter = RS12_FILTER_SMOOTH;
	g_set.scan_type = RS12_SCAN_OFF;
	g_set.scan_strength = RS12_SCAN_MEDIUM;
	g_set.border = RS12_BORDER_ARTWORK; // the game's artwork, else the app's blank artwork
	g_set.border_color = RS12_BCOL_MAROON;
	g_set.rotate = RS12_ROT_AUTO;
	g_set.confirm = RS12_CONFIRM_CROSS;
	g_set.list_filter = RS12_LIST_PLAYABLE;
	g_set.theme = RS12_THEME_RED;
	g_set.latency = RS12_LATENCY_NORMAL;
	rs12_settings_default_keys();
}

uint32_t rs12_key_code(int key)
{
	static const uint32_t codes[RS12_KEY_COUNT] = { SCE_CTRL_CROSS, SCE_CTRL_CIRCLE, SCE_CTRL_SQUARE, SCE_CTRL_TRIANGLE,
		SCE_CTRL_LTRIGGER, SCE_CTRL_RTRIGGER, SCE_CTRL_SELECT, SCE_CTRL_START };
	return key >= 0 && key < RS12_KEY_COUNT ? codes[key] : 0;
}

const char *rs12_key_name(int key)
{
	static const char *const n[RS12_KEY_COUNT] = { "Cross", "Circle", "Square", "Triangle", "L", "R", "Select", "Start" };
	return key >= 0 && key < RS12_KEY_COUNT ? n[key] : "None";
}

int rs12_input_rot;
static char s_game[32]; // current game set name (cfg/<set>.cfg)
// the game's monitor orientation (MAME ROT90 = 90), 0 without a game
static int game_rotation(void)
{
	const int i = s_game[0] ? s12_find_game(s_game) : -1;
	return i >= 0 ? s12_game(i)->rot : 0;
}
void rs12_settings_apply_video(void)
{
	const int g = game_rotation();
	int r = 0;
	rs12_input_rot = 0;
	switch (g_set.rotate) {
	case RS12_ROT_AUTO: r = g; break;
	case RS12_ROT_VL: r = (g + 90) % 360; rs12_input_rot = 90; break;
	case RS12_ROT_VR: r = (g + 270) % 360; rs12_input_rot = 270; break;
	default: break;
	}
	rs12_video_set_rotation(r);
	// rs12_video_set_mode: 0 = 4:3, 1 = integer, 2 = full
	const int aspect = g_set.aspect == RS12_ASPECT_FULL ? 2 : g_set.aspect == RS12_ASPECT_INT ? 1 : 0;
	rs12_video_set_mode(aspect, g_set.filter == RS12_FILTER_SMOOTH);
	rs12_video_set_border(g_set.border, g_set.border_color);
	extern int rs12_video_vsync_pace, rs12_video_pace_forced;
	if (!rs12_video_pace_forced)
		rs12_video_vsync_pace = g_set.latency == RS12_LATENCY_LOW ? 2 : 0;
}

static int clampi(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

int rs12_dip_service;
static const char *const s_combo_keys[RS12_COMBO_KEYS] = { "cross", "circle", "square", "triangle", "l", "r" };

// combo keys in screen order (Square, Triangle, Cross, Circle, L, R) and the choices for one key: None, then every pair
static const int s_combo_order[RS12_COMBO_KEYS] = { RS12_KEY_SQUARE, RS12_KEY_TRIANGLE, RS12_KEY_CROSS, RS12_KEY_CIRCLE,
	RS12_KEY_L, RS12_KEY_R };
#define COMBO_CHOICES (1 + RS12_COMBO_KEYS * (RS12_COMBO_KEYS - 1) / 2)
static int combo_choice(int i) // choice index -> key mask
{
	if (i <= 0)
		return 0;
	for (int a = 0, n = 1; a < RS12_COMBO_KEYS; a++)
		for (int b = a + 1; b < RS12_COMBO_KEYS; b++, n++)
			if (n == i)
				return (1 << s_combo_order[a]) | (1 << s_combo_order[b]);
	return 0;
}
static int combo_index(int mask) // key mask -> choice index (0 if not a pair)
{
	for (int i = 1; i < COMBO_CHOICES; i++)
		if (combo_choice(i) == mask)
			return i;
	return 0;
}

// items per-game settings can override (global values in s_global)
typedef struct {
	int fps, aspect, filter, scan_type, scan_strength, border, border_color, rotate;
	int keymap[RS12_ACT_COUNT], stick, turbo_key, turbo_btn, turbo_speed;
	int combo[RS12_COMBO_KEYS];
	int aim_stick, aim_mode, crosshair, gun_sens, wheel_stick, lever_stick;
	int gun_fire, gun_sensor, gas_key, brake_key;
	int touch_keys, touch_show, touch_gun;
} game_part;
static game_part s_global;
static int s_game_on;   // per-game settings are active for the current game
static void part_get(game_part *g)
{
	g->fps = g_set.fps; g->aspect = g_set.aspect; g->filter = g_set.filter;
	g->scan_type = g_set.scan_type; g->scan_strength = g_set.scan_strength;
	g->border = g_set.border; g->border_color = g_set.border_color; g->rotate = g_set.rotate;
	memcpy(g->keymap, g_set.keymap, sizeof(g->keymap));
	g->stick = g_set.stick; g->turbo_key = g_set.turbo_key; g->turbo_btn = g_set.turbo_btn; g->turbo_speed = g_set.turbo_speed;
	memcpy(g->combo, g_set.combo, sizeof(g->combo));
	g->aim_stick = g_set.aim_stick; g->aim_mode = g_set.aim_mode; g->crosshair = g_set.crosshair;
	g->gun_sens = g_set.gun_sens; g->wheel_stick = g_set.wheel_stick; g->lever_stick = g_set.lever_stick;
	g->gun_fire = g_set.gun_fire; g->gun_sensor = g_set.gun_sensor; g->gas_key = g_set.gas_key; g->brake_key = g_set.brake_key;
	g->touch_keys = g_set.touch_keys; g->touch_show = g_set.touch_show; g->touch_gun = g_set.touch_gun;
}
static void part_set(const game_part *g)
{
	g_set.fps = g->fps; g_set.aspect = g->aspect; g_set.filter = g->filter;
	g_set.scan_type = g->scan_type; g_set.scan_strength = g->scan_strength;
	g_set.border = g->border; g_set.border_color = g->border_color; g_set.rotate = g->rotate;
	memcpy(g_set.keymap, g->keymap, sizeof(g->keymap));
	g_set.stick = g->stick; g_set.turbo_key = g->turbo_key; g_set.turbo_btn = g->turbo_btn; g_set.turbo_speed = g->turbo_speed;
	memcpy(g_set.combo, g->combo, sizeof(g->combo));
	g_set.aim_stick = g->aim_stick; g_set.aim_mode = g->aim_mode; g_set.crosshair = g->crosshair;
	g_set.gun_sens = g->gun_sens; g_set.wheel_stick = g->wheel_stick; g_set.lever_stick = g->lever_stick;
	g_set.gun_fire = g->gun_fire; g_set.gun_sensor = g->gun_sensor; g_set.gas_key = g->gas_key; g_set.brake_key = g->brake_key;
	g_set.touch_keys = g->touch_keys; g_set.touch_show = g->touch_show; g_set.touch_gun = g->touch_gun;
}
// one overridable line (shared by global and game files). 1 if recognized
static int read_part(const char *key, int val)
{
	if (!strcmp(key, "show_fps")) g_set.fps = val != 0;
	else if (!strcmp(key, "aspect")) g_set.aspect = clampi(val, 0, RS12_ASPECT_COUNT - 1);
	else if (!strcmp(key, "filter")) g_set.filter = clampi(val, 0, RS12_FILTER_COUNT - 1);
	else if (!strcmp(key, "scan_type")) g_set.scan_type = clampi(val, 0, RS12_SCAN_COUNT - 1);
	else if (!strcmp(key, "scan_strength")) g_set.scan_strength = clampi(val, 0, RS12_SCAN_STRENGTH_COUNT - 1);
	else if (!strcmp(key, "border")) g_set.border = clampi(val, 0, RS12_BORDER_COUNT - 1);
	else if (!strcmp(key, "border_color")) g_set.border_color = clampi(val, 0, RS12_BCOL_COUNT - 1);
	else if (!strcmp(key, "rotate")) g_set.rotate = clampi(val, 0, RS12_ROT_COUNT - 1);
	else if (!strcmp(key, "stick")) g_set.stick = clampi(val, 0, RS12_STICK_COUNT - 1);
	else if (!strcmp(key, "aim_stick")) g_set.aim_stick = clampi(val, 0, RS12_AIM_STICK_COUNT - 1);
	else if (!strcmp(key, "aim_mode")) g_set.aim_mode = clampi(val, 0, RS12_AIM_MODE_COUNT - 1);
	else if (!strcmp(key, "crosshair")) g_set.crosshair = val != 0;
	else if (!strcmp(key, "touch_keys")) g_set.touch_keys = val != 0;
	else if (!strcmp(key, "touch_show")) g_set.touch_show = val != 0;
	else if (!strcmp(key, "touch_gun")) g_set.touch_gun = val != 0;
	else if (!strcmp(key, "gun_sens")) g_set.gun_sens = clampi(val, 1, 10);
	else if (!strcmp(key, "wheel_stick")) g_set.wheel_stick = clampi(val, 0, RS12_AIM_STICK_COUNT - 1);
	else if (!strcmp(key, "lever_stick")) g_set.lever_stick = clampi(val, 0, RS12_AIM_STICK_COUNT - 1);
	else if (!strcmp(key, "gun_fire")) g_set.gun_fire = clampi(val, 0, RS12_KEY_NONE);
	else if (!strcmp(key, "gun_sensor")) g_set.gun_sensor = clampi(val, 0, RS12_KEY_NONE);
	else if (!strcmp(key, "gas_key")) g_set.gas_key = clampi(val, 0, RS12_KEY_NONE);
	else if (!strcmp(key, "brake_key")) g_set.brake_key = clampi(val, 0, RS12_KEY_NONE);
	else if (!strcmp(key, "turbo_key")) g_set.turbo_key = clampi(val, 0, RS12_KEY_NONE);
	else if (!strcmp(key, "turbo_btn")) g_set.turbo_btn = clampi(val, 1, RS12_GAME_BUTTONS);
	else if (!strcmp(key, "turbo_speed")) g_set.turbo_speed = clampi(val, 0, RS12_TURBO_SPEED_COUNT - 1);
	else if (!strncmp(key, "combo_", 6)) { // combo_<key>=<mask of two keys>
		for (int k = 0; k < RS12_COMBO_KEYS; k++)
			if (!strcmp(key + 6, s_combo_keys[k])) {
				g_set.combo[k] = combo_index(val) ? val : 0;
				return 1;
			}
		return 0;
	} else {
		for (int a = 0; a < RS12_ACT_COUNT; a++)
			if (!strcmp(key, s_keymap_keys[a])) {
				g_set.keymap[a] = clampi(val, 0, a == RS12_ACT_B1 ? RS12_KEY_COUNT - 1 : RS12_KEY_NONE);
				return 1;
			}
		return 0;
	}
	return 1;
}
static void write_part(FILE *f, const game_part *g)
{
	fprintf(f, "show_fps=%d\naspect=%d\nfilter=%d\n", g->fps, g->aspect, g->filter);
	fprintf(f, "scan_type=%d\nscan_strength=%d\n", g->scan_type, g->scan_strength);
	fprintf(f, "border=%d\nborder_color=%d\nrotate=%d\n", g->border, g->border_color, g->rotate);
	for (int a = 0; a < RS12_ACT_COUNT; a++)
		fprintf(f, "%s=%d\n", s_keymap_keys[a], g->keymap[a]);
	fprintf(f, "stick=%d\nturbo_key=%d\nturbo_btn=%d\nturbo_speed=%d\n", g->stick, g->turbo_key, g->turbo_btn, g->turbo_speed);
	fprintf(f, "aim_stick=%d\naim_mode=%d\ncrosshair=%d\n", g->aim_stick, g->aim_mode, g->crosshair);
	fprintf(f, "gun_sens=%d\nwheel_stick=%d\nlever_stick=%d\n", g->gun_sens, g->wheel_stick, g->lever_stick);
	fprintf(f, "gun_fire=%d\ngun_sensor=%d\ngas_key=%d\nbrake_key=%d\n", g->gun_fire, g->gun_sensor, g->gas_key, g->brake_key);
	for (int k = 0; k < RS12_COMBO_KEYS; k++)
		fprintf(f, "combo_%s=%d\n", s_combo_keys[k], g->combo[k]);
	fprintf(f, "touch_keys=%d\ntouch_show=%d\ntouch_gun=%d\n", g->touch_keys, g->touch_show, g->touch_gun);
}
// one "key=value" line. 0 at end
static int read_kv(FILE *f, char *key, int klen, char *val, int vlen)
{
	char line[128];
	if (!fgets(line, sizeof(line), f))
		return 0;
	char *eq = strchr(line, '=');
	if (!eq) {
		key[0] = val[0] = 0;
		return 1;
	}
	*eq = 0;
	size_t kn = strlen(line);
	if (kn >= (size_t)klen)
		kn = (size_t)klen - 1;
	memcpy(key, line, kn);
	key[kn] = 0;
	char *v = eq + 1;
	v[strcspn(v, "\r\n")] = 0;
	size_t vn = strlen(v);
	if (vn >= (size_t)vlen)
		vn = (size_t)vlen - 1;
	memcpy(val, v, vn);
	val[vn] = 0;
	return 1;
}

// ---- favorites (favorites.cfg, one set name per line) ----
#define FAV_MAX 256
static char s_fav[FAV_MAX][16];
static int s_fav_n;

int rs12_fav_is(const char *set)
{
	for (int i = 0; i < s_fav_n; i++)
		if (!strcmp(s_fav[i], set))
			return 1;
	return 0;
}

static void fav_load(void)
{
	FILE *f = fopen(FAV_PATH, "r");
	if (!f)
		return;
	char line[64];
	s_fav_n = 0;
	while (s_fav_n < FAV_MAX && fgets(line, sizeof(line), f)) {
		const size_t len = strcspn(line, "\r\n");
		line[len] = 0;
		if (len && len < sizeof(s_fav[0]) && !rs12_fav_is(line))
			memcpy(s_fav[s_fav_n++], line, len + 1);
	}
	fclose(f);
}

void rs12_fav_toggle(const char *set)
{
	int i = 0;
	while (i < s_fav_n && strcmp(s_fav[i], set) != 0)
		i++;
	if (i < s_fav_n) {
		memmove(s_fav[i], s_fav[i + 1], (size_t)(s_fav_n - i - 1) * sizeof(s_fav[0]));
		s_fav_n--;
	} else if (s_fav_n < FAV_MAX && strlen(set) < sizeof(s_fav[0]))
		strcpy(s_fav[s_fav_n++], set);
	FILE *f = fopen(FAV_PATH, "w");
	if (!f) {
		RS12_LOG("settings: save failed %s", FAV_PATH);
		return;
	}
	for (i = 0; i < s_fav_n; i++)
		fprintf(f, "%s\n", s_fav[i]);
	fclose(f);
}

void rs12_settings_load(void)
{
	rs12_settings_defaults();
	fav_load();
	FILE *f = fopen(SETTINGS_PATH, "r");
	if (f) {
		char key[32], sval[32];
		while (read_kv(f, key, sizeof(key), sval, sizeof(sval))) {
			const int val = atoi(sval);
			if (read_part(key, val))
				continue;
			else if (!strcmp(key, "menu_confirm")) g_set.confirm = clampi(val, 0, RS12_CONFIRM_COUNT - 1);
			else if (!strcmp(key, "list_filter")) g_set.list_filter = clampi(val, 0, RS12_LIST_FILTER_COUNT - 1);
			else if (!strcmp(key, "theme")) g_set.theme = clampi(val, 0, RS12_THEME_COUNT - 1);
			else if (!strcmp(key, "latency")) g_set.latency = clampi(val, 0, RS12_LATENCY_COUNT - 1);
		}
		fclose(f);
	}
	part_get(&s_global);
	RS12_LOG("settings: fps %d aspect %d filter %d border %d/%d rotate %d keys %d %d %d %d %d %d coin %d start %d stick %d turbo %d/%d",
		g_set.fps, g_set.aspect, g_set.filter, g_set.border, g_set.border_color, g_set.rotate, g_set.keymap[0], g_set.keymap[1],
		g_set.keymap[2], g_set.keymap[3], g_set.keymap[4], g_set.keymap[5], g_set.keymap[6], g_set.keymap[7], g_set.stick,
		g_set.turbo_key, g_set.turbo_btn);
}

static void game_cfg_path(char *p, int len)
{
	snprintf(p, len, CFG_DIR "/%s.cfg", s_game);
}
// the game's touch layout (with its per-game settings)
static void game_touch_path(char *p, int len)
{
	snprintf(p, len, CFG_DIR "/%s.touch", s_game);
}
static void touch_follow_settings(int keep)
{
	char p[96];
	game_touch_path(p, sizeof(p));
	rs12_touch_select(s_game[0] && s_game_on ? p : NULL, keep);
}

void rs12_settings_load_game(const char *set)
{
	snprintf(s_game, sizeof(s_game), "%s", set);
	if (s_game_on) { // drop the previous game's per-game settings and return to global values
		part_set(&s_global);
		s_game_on = 0;
	} else
		part_get(&s_global);
	char p[96];
	game_cfg_path(p, sizeof(p));
	FILE *f = fopen(p, "r");
	if (f) {
		char key[32], sval[32];
		while (read_kv(f, key, sizeof(key), sval, sizeof(sval)))
			read_part(key, atoi(sval));
		fclose(f);
		s_game_on = 1;
		RS12_LOG("settings: game settings %s", p);
	}
	touch_follow_settings(0); // (the game's touch layout with its settings, else the global one)
}

void rs12_settings_save(void)
{
	FILE *f = fopen(SETTINGS_PATH, "w");
	if (!f) {
		RS12_LOG("settings: save failed %s", SETTINGS_PATH);
		return;
	}
	// with per-game settings on, those items go to the game file (below); the global file keeps the global values
	if (!s_game_on)
		part_get(&s_global);
	write_part(f, &s_global);
	fprintf(f, "menu_confirm=%d\nlist_filter=%d\ntheme=%d\nlatency=%d\n", g_set.confirm, g_set.list_filter, g_set.theme,
		g_set.latency);
	fclose(f);
	if (s_game[0] && s_game_on)
		rs12_settings_save_game();
}

int rs12_settings_has_game(void) { return s_game_on; }

void rs12_settings_use_global(void)
{
	if (s_game_on) {
		part_set(&s_global);
		s_game_on = 0;
	}
	s_game[0] = 0; // (game file is not written until the next rs12_settings_load_game)
	touch_follow_settings(0);
}

int rs12_settings_save_game(void)
{
	if (!s_game[0])
		return 0;
	sceIoMkdir(CFG_DIR, 0777);
	char p[96];
	game_cfg_path(p, sizeof(p));
	FILE *f = fopen(p, "w");
	if (!f) {
		RS12_LOG("settings: game save failed %s", p);
		return 0;
	}
	game_part g;
	part_get(&g);
	write_part(f, &g);
	fclose(f);
	s_game_on = 1;
	touch_follow_settings(1); // the touch layout now is the game's too
	return 1;
}

int rs12_settings_delete_game(void)
{
	if (!s_game[0] || !s_game_on)
		return 0;
	char p[96];
	game_cfg_path(p, sizeof(p));
	if (sceIoRemove(p) < 0) {
		RS12_LOG("settings: game delete failed %s", p);
		return 0;
	}
	s_game_on = 0;
	game_touch_path(p, sizeof(p));
	sceIoRemove(p);
	part_set(&s_global); // (global values right away)
	rs12_settings_apply_video();
	touch_follow_settings(0);
	return 1;
}

// ================================ game table ================================
// Support tier tabs: A buttons, B gun, C wheel / lever. CD-XA boards (D) and sets MAME marks
// not working (E) are not listed.
enum { TIER_BUTTONS, TIER_GUN, TIER_WHEEL, TIER_COUNT };
static const char *const s_tier_name[TIER_COUNT] = { "Buttons", "Gun", "Wheel / Lever" };

static int game_tier(const S12Game *g)
{
	const char *in = g->inputs ? g->inputs : "";
	if (!strcmp(in, "ptblank2") || !strcmp(in, "ghlpanic") || !strcmp(in, "golgo13"))
		return TIER_GUN;
	if (!strcmp(in, "aplarail") || !strcmp(in, "technodr") || !strcmp(in, "kartduel") || !strcmp(in, "truckk"))
		return TIER_WHEEL;
	return TIER_BUTTONS;
}

static int game_listed(const S12Game *g)
{
	if (g->machine == S12_M_CDXA || g->machine == S12_M_QUESTFF || g->machine == S12_M_TRUCKK)
		return 0;
	return !(g->flags & S12_G_NOT_WORKING);
}

int rs12_game_buttons(const char *set)
{
	const int i = s12_find_game(set);
	const S12Game *g = i >= 0 ? s12_game(i) : NULL;
	if (!g || !g->inputs)
		return RS12_GAME_BUTTONS;
	const char *in = g->inputs;
	// MAME input ports: namcos12 (TTT, Soul Calibur...), tekken3, lbgrande use the 6-button JAMMA layout
	if (!strcmp(in, "namcos12") || !strcmp(in, "tekken3") || !strcmp(in, "lbgrande"))
		return 6;
	if (!strcmp(in, "mdhorse") || !strcmp(in, "pacapp") || !strcmp(in, "pacapp2"))
		return 4;
	if (!strcmp(in, "golgo13") || !strcmp(in, "kartduel") || !strcmp(in, "technodr") || !strcmp(in, "aplarail"))
		return 3;
	return 2;
}

// ================================ menu state ================================
static char s_set[32], s_title[96];
static UiCanvas *s_cv, *s_osd_cv, *s_fps_cv;
enum { PAGE_MAIN, PAGE_STATES, PAGE_DISPLAY, PAGE_CONTROLS, PAGE_ABOUT, PAGE_COMBO, PAGE_ANALOG, PAGE_TURBO, PAGE_TOUCH, PAGE_TEDIT };
static int s_page; // (defined with the other menu state below)
// rows in groups (a gap before M_STATES, M_DISPLAY and M_ABOUT): game, states, settings, about / leave.
// M_REFRESH (read the ROM folder again) only in the game list options
enum { M_RESUME, M_RESET, M_SERVICE, M_STATES, M_DISPLAY, M_CONTROLS, M_GAME_SAVE, M_GAME_DEL, M_ABOUT, M_SELECT_GAME, M_EXIT,
	M_BACK, M_REFRESH, M_COUNT };
#define GROUP_GAP 10
// game list options (START in the list): no game. Only Display / Controls / Refresh / Back, global settings only
static int s_list_mode;
// main menu rows -> items (Delete only if per-game settings exist, else Save only. Back only in list options)
static int main_list(int *k)
{
	int n = 0;
	if (s_list_mode) {
		k[n++] = M_DISPLAY;
		k[n++] = M_CONTROLS;
		k[n++] = M_REFRESH;
		k[n++] = M_BACK;
		return n;
	}
	for (int m = 0; m < M_BACK; m++) {
		if (m == (rs12_settings_has_game() ? M_GAME_SAVE : M_GAME_DEL))
			continue;
		k[n++] = m;
	}
	return n;
}
static int main_rows(void) { int k[M_COUNT]; return main_list(k); }
static int main_kind(int row)
{
	int k[M_COUNT];
	const int n = main_list(k);
	return row >= 0 && row < n ? k[row] : M_RESUME;
}
enum { S_SLOT, S_SAVE, S_LOAD, S_BACK, S_COUNT };
// Display rows (scanline rows hidden until the shader exists; values stay in the settings file)
enum { D_FPS, D_ASPECT, D_ROTATE, D_FILTER, D_BORDER, D_BORDER_COL, D_LATENCY, D_THEME, D_BACK, D_COUNT };
static const char *const s_latency_name[RS12_LATENCY_COUNT] = { "Normal", "Low (may stutter)" };
// Controls rows: game buttons (as many as the game uses, 6 in list options), coin, start, Turbo (sub page), analog stick,
// the gun / wheel / lever page (games that have one), confirm button, combo keys, defaults, back.
// Turbo page: turbo key, turbo button, turbo speed, back. Gun / wheel / lever page (PAGE_ANALOG): only that game's kind
// of rows, then back (CK_ABACK)
enum { CK_ACT, CK_TURBO_KEY, CK_TURBO_BTN, CK_TURBO_SPEED, CK_TURBO, CK_STICK, CK_AIM_STICK, CK_AIM_MODE, CK_GUN_SENS,
	CK_CROSSHAIR, CK_GUN_FIRE, CK_GUN_SENSOR, CK_WHEEL_STICK, CK_GAS, CK_BRAKE, CK_LEVER_STICK, CK_CONFIRM, CK_COMBO,
	CK_DEFAULTS, CK_BACK, CK_ANALOG, CK_ABACK, CK_TOUCH, CK_TOUCH_GUN, CK_T_ON, CK_T_SHOW, CK_T_EDIT, CK_T_CLEAR };
// Combo keys page: one row per Vita key (s_combo_order), then Back. Confirm opens the choice window
#define COMBO_ROWS (RS12_COMBO_KEYS + 1)
static int s_nbuttons = RS12_GAME_BUTTONS;
static int ctrl_buttons(void) { return s_list_mode ? RS12_GAME_BUTTONS : s_nbuttons; }
// the running game's analog control (S12_ANALOG_*; none in list options) and whether it is golgo13 (scope sensor)
static const S12Game *menu_game(void)
{
	const int i = s_list_mode ? -1 : s12_find_game(s_set);
	return i >= 0 ? s12_game(i) : NULL;
}
static int menu_analog(void) { return s12_analog_kind(menu_game()); }
static int menu_golgo(void)
{
	const S12Game *g = menu_game();
	return g && g->state == S12_S_GOLGO13;
}
// rows after the buttons, coin and start
static int ctrl_tail(int *k)
{
	int n = 0;
	k[n++] = CK_TURBO;
	k[n++] = CK_STICK;
	k[n++] = CK_TOUCH;
	if (menu_analog() != S12_ANALOG_NONE)
		k[n++] = CK_ANALOG;
	k[n++] = CK_CONFIRM;
	k[n++] = CK_COMBO;
	k[n++] = CK_DEFAULTS;
	k[n++] = CK_BACK;
	return n;
}
static int analog_rows(int *k)
{
	int n = 0;
	switch (menu_analog()) {
	case S12_ANALOG_GUN:
		k[n++] = CK_TOUCH_GUN;
		k[n++] = CK_GUN_FIRE;
		if (menu_golgo())
			k[n++] = CK_GUN_SENSOR;
		k[n++] = CK_AIM_STICK;
		k[n++] = CK_AIM_MODE;
		k[n++] = CK_GUN_SENS;
		k[n++] = CK_CROSSHAIR;
		break;
	case S12_ANALOG_WHEEL:
		k[n++] = CK_WHEEL_STICK;
		k[n++] = CK_GAS;
		k[n++] = CK_BRAKE;
		break;
	case S12_ANALOG_LEVER:
		k[n++] = CK_LEVER_STICK;
		break;
	}
	k[n++] = CK_ABACK;
	return n;
}
static int ctrl_rows(void)
{
	int k[16];
	return ctrl_buttons() + 2 + ctrl_tail(k);
}
static int ctrl_kind(int i, int *act)
{
	const int b = ctrl_buttons();
	*act = -1;
	if (i < b) {
		*act = RS12_ACT_B1 + i;
		return CK_ACT;
	}
	if (i < b + 2) {
		*act = i == b ? RS12_ACT_COIN : RS12_ACT_START;
		return CK_ACT;
	}
	int k[16];
	const int n = ctrl_tail(k);
	return i - b - 2 < n ? k[i - b - 2] : CK_BACK;
}
// row of a kind on the Controls page (coming back from a sub page)
static int ctrl_row_of(int kind)
{
	int k[16];
	const int n = ctrl_tail(k);
	for (int j = 0; j < n; j++)
		if (k[j] == kind)
			return ctrl_buttons() + 2 + j;
	return 0;
}
// current row's kind: Controls, Turbo or the gun / wheel / lever page
static int row_kind(int i, int *act)
{
	*act = -1;
	if (s_page == PAGE_TURBO)
		return i < 3 ? CK_TURBO_KEY + i : CK_BACK;
	if (s_page == PAGE_TOUCH) {
		static const int k[5] = { CK_T_ON, CK_T_SHOW, CK_T_EDIT, CK_T_CLEAR, CK_BACK };
		return i >= 0 && i < 5 ? k[i] : CK_BACK;
	}
	if (s_page == PAGE_ANALOG) {
		int k[16];
		const int n = analog_rows(k);
		return i < n ? k[i] : CK_ABACK;
	}
	return ctrl_kind(i, act);
}
#define CTRL_PAGE (s_page == PAGE_CONTROLS || s_page == PAGE_TURBO || s_page == PAGE_ANALOG || s_page == PAGE_TOUCH)
static const char *act_name(int act)
{
	static const char *const n[RS12_ACT_COUNT] = { "Button 1", "Button 2", "Button 3", "Button 4", "Button 5", "Button 6", "Coin",
		"Start" };
	return n[act];
}
int rs12_menu_buttons(void) { return s_nbuttons; }

// ---- duplicate keys: one Vita key that drives two different things in the running game (list options: the 6-button
// layout). func = what the key presses; the same thing twice is fine (the gun trigger is also a game button)
enum { KF_TURBO = 16, KF_GAS, KF_BRAKE, KF_SENSOR, KF_COMBO = 32 };
typedef struct { int key, func; const char *what; } KeyUse;
static int key_uses(KeyUse *u)
{
	int n = 0;
	for (int a = 0; a < ctrl_buttons(); a++)
		if (g_set.keymap[a] < RS12_KEY_COUNT)
			u[n++] = (KeyUse){ g_set.keymap[a], a, act_name(a) };
	u[n++] = (KeyUse){ g_set.keymap[RS12_ACT_COIN], RS12_ACT_COIN, "Coin" };
	u[n++] = (KeyUse){ g_set.keymap[RS12_ACT_START], RS12_ACT_START, "Start" };
	if (g_set.turbo_key < RS12_KEY_COUNT)
		u[n++] = (KeyUse){ g_set.turbo_key, KF_TURBO, "Turbo key" };
	for (int k = 0; k < RS12_COMBO_KEYS; k++)
		if (g_set.combo[k])
			u[n++] = (KeyUse){ k, KF_COMBO + k, "a combo key" };
	switch (menu_analog()) {
	case S12_ANALOG_GUN:
		if (g_set.gun_fire < RS12_KEY_COUNT)
			u[n++] = (KeyUse){ g_set.gun_fire, menu_golgo() ? RS12_ACT_B2 : RS12_ACT_B1, "Gun trigger" };
		if (menu_golgo() && g_set.gun_sensor < RS12_KEY_COUNT)
			u[n++] = (KeyUse){ g_set.gun_sensor, KF_SENSOR, "Gun scope sensor" };
		break;
	case S12_ANALOG_WHEEL:
		if (g_set.gas_key < RS12_KEY_COUNT)
			u[n++] = (KeyUse){ g_set.gas_key, KF_GAS, "Gas pedal" };
		if (g_set.brake_key < RS12_KEY_COUNT)
			u[n++] = (KeyUse){ g_set.brake_key, KF_BRAKE, "Brake pedal" };
		break;
	}
	return n;
}
// another use of key that presses something else, or NULL
static const char *key_clash(int key, int func)
{
	KeyUse u[32];
	const int n = key >= 0 && key < RS12_KEY_COUNT ? key_uses(u) : 0;
	for (int j = 0; j < n; j++)
		if (u[j].key == key && u[j].func != func)
			return u[j].what;
	return NULL;
}
// key and func of a Controls / Turbo / gun-wheel row that assigns a key (0 = not such a row)
static int row_key(int kind, int act, int *key, int *func)
{
	switch (kind) {
	case CK_ACT: *key = g_set.keymap[act]; *func = act; return 1;
	case CK_TURBO_KEY: *key = g_set.turbo_key; *func = KF_TURBO; return 1;
	case CK_GUN_FIRE: *key = g_set.gun_fire; *func = menu_golgo() ? RS12_ACT_B2 : RS12_ACT_B1; return 1;
	case CK_GUN_SENSOR: *key = g_set.gun_sensor; *func = KF_SENSOR; return 1;
	case CK_GAS: *key = g_set.gas_key; *func = KF_GAS; return 1;
	case CK_BRAKE: *key = g_set.brake_key; *func = KF_BRAKE; return 1;
	default: return 0;
	}
}
// notice after a key change that made a duplicate (not while the choice window steps to a value)
static int s_quiet;
static void warn_clash(int key, int func)
{
	const char *other = s_quiet ? NULL : key_clash(key, func);
	if (other)
		rs12_osd_show("%s is also used for %s", rs12_key_name(key), other);
}
static int s_sel, s_main_sel, s_reset_armed, s_state_slot, s_about_scroll, s_dirty = 1;
static int s_drawn_h; // panel height currently drawn on the menu canvas (0 = other content)
static uint32_t s_prev_buttons;

// About: vertically scrolling text (empty string = blank line, starting with '#' = heading line)
static const char *const s_about[] = {
	"#ReSys12 for Vita",
	"Version " RS12_VERSION " (" RS12_VERSION_DATE ")",
	"Developed by Reyanon",
	"  https://github.com/Reyanon",
	"",
	"Namco System 12 arcade board emulator",
	"Games (c) their owners. ROM images are not included.",
	"Not affiliated with or endorsed by Bandai Namco.",
	"Free software under the GNU GPL version 3.",
	"",
	"#Open source used",
	"pcsx_rearmed - R3000A, ari64 recompiler, GTE, gpulib, gpu_neon",
	"  https://github.com/notaz/pcsx_rearmed",
	"MAME - System 12 board, H8/3002, C352, ROM definitions",
	"  (BSD-3-Clause parts) https://github.com/mamedev/mame",
	"VitaSDK - toolchain, newlib, system stubs",
	"  https://github.com/vitasdk",
	"vitaGL - GPU output",
	"  https://github.com/Rinnegatamante/vitaGL",
	"vitaShaRK - runtime shader compiler",
	"  https://github.com/Rinnegatamante/vitaShaRK",
	"Mbed TLS - update check (HTTPS)",
	"  https://github.com/Mbed-TLS/mbedtls",
	"junzip - ROM zip reading",
	"  https://github.com/jokkebk/JUnzip",
	"zlib - inflate",
	"  https://github.com/madler/zlib",
	"libpng - border artwork",
	"  https://github.com/pnggroup/libpng",
	"",
	"License texts: LICENSE in the source repository.",
};
#define ABOUT_LINES ((int)(sizeof(s_about) / sizeof(s_about[0])))
#define ABOUT_VISIBLE 12

static void ensure_canvases(void)
{
	ui_init();
	if (!s_cv)
		s_cv = ui_canvas(0, 0, 0, SCREEN_W, SCREEN_H);
	if (!s_osd_cv)
		s_osd_cv = ui_canvas(2, 0, 0, SCREEN_W, 40);
	if (!s_fps_cv)
		s_fps_cv = ui_canvas(3, SCREEN_W - 4 - 240, 4, 240, 32); // top right (rs12_fps_update aligns the box right)
}

void rs12_menu_init(const char *set, const char *title, int buttons)
{
	s_nbuttons = buttons < 2 ? 2 : buttons > RS12_GAME_BUTTONS ? RS12_GAME_BUTTONS : buttons;
	if (g_set.turbo_btn > s_nbuttons)
		g_set.turbo_btn = 1;
	snprintf(s_set, sizeof(s_set), "%s", set);
	snprintf(s_title, sizeof(s_title), "%s", title ? title : set);
	ensure_canvases();
}

// save state slots of the running game: used or not, and when (read when the menu opens and after a save; the
// emulation is stopped then)
static char s_state_game[32];
static int s_slot_used[STATE_SLOTS];
static SceDateTime s_slot_time[STATE_SLOTS];

void rs12_menu_set_game(const char *set)
{
	snprintf(s_state_game, sizeof(s_state_game), "%s", set ? set : "");
}

void rs12_menu_states_changed(void)
{
	for (int k = 0; k < STATE_SLOTS; k++) {
		char path[128];
		SceIoStat st;
		snprintf(path, sizeof(path), DATA_DIR "/state/%s_%d.st", s_state_game, k);
		s_slot_used[k] = s_state_game[0] && sceIoGetstat(path, &st) >= 0 && st.st_size > 0;
		if (s_slot_used[k])
			s_slot_time[k] = st.st_mtime;
	}
	s_dirty = 1;
}

static int s_keep_place, s_pop, s_pop_sel; // menu closed with SELECT+START (keep its page); choice window open, its row
// The PS button belongs to the app everywhere (locked for the system from the start). One press opens the menu (in a
// game) or the options (in the game list) and closes them again like SELECT+START; nothing while loading. Only a
// double press goes to the home screen: the app lets go of the lock and the kernel helper presses PS for the system
#define RS12_KCTRL_LOADED 0x8002D013u // taiLoadStartKernelModule: already loaded (device)
static int s_kctrl = -1;    // kernel helper usable (module/rs12_kctrl.skprx): 1 yes, -1 no
static int s_ps_locked;     // the system's PS button handling locked by the app
static unsigned s_ps_one, s_ps_two; // helper counts already acted on (single, double presses)
static uint64_t s_ps_home_t; // when a double press sent the app home (lock taken again a while later)
static void ps_lock(int on)
{
	if (s_kctrl < 0 || on == s_ps_locked)
		return;
	const int r = on ? sceShellUtilLock(SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN_2) : sceShellUtilUnlock(SCE_SHELL_UTIL_LOCK_TYPE_PS_BTN_2);
	RS12_LOG("menu: PS button %s (%08x)", on ? "locked" : "unlocked", (unsigned)r);
	s_ps_locked = on;
	if (on) { // (presses made while unlocked are not acted on)
		const unsigned st = rs12KCtrlPsState();
		s_ps_one = st & 0x7FFF;
		s_ps_two = (st >> 15) & 0x7FFF;
	}
}
// before the app exits or relaunches itself: give the PS button back to the system (a relaunch with the shell lock
// still held sometimes left the new process unable to start, error C1-2569-2)
void rs12_ps_release(void)
{
	ps_lock(0);
}
// Load the kernel helper at start. Its syscall is linked into this app only if the module was loaded before the app
// started (a call right after a first load crashed on the device), so a first load since boot restarts the app once
// (argument "kctrl"); the module then stays loaded until the Vita reboots
void rs12_kctrl_init(int restarted)
{
	const SceUID m = taiLoadStartKernelModule("ux0:app/RSYS00012/module/rs12_kctrl.skprx", 0, NULL, 0); // (the kernel cannot open app0:)
	// (the module name has a version, rs12_kctrl_v4: a module of an older build may still be loaded until reboot)
	if (m >= 0 && !restarted) {
		static char *const av[] = { "kctrl", NULL };
		sceAppMgrLoadExec("app0:eboot.bin", av, NULL);
	}
	s_kctrl = (unsigned)m == RS12_KCTRL_LOADED ? 1 : -1;
	RS12_LOG("kctrl: %08x (%s)", (unsigned)m, s_kctrl > 0 ? "ready" : "not available");
	if (s_kctrl > 0) {
		sceShellUtilInitEvents(0);
		ps_lock(1);
	}
}
int rs12_kctrl_ready(void) { return s_kctrl > 0; } // (the update installs through it)
// call every frame anywhere: 1 once per single PS press, when PS is let go (PS + START takes a screenshot, the menu must
// not cover it: START while PS is held drops that press); a double press goes home
static int s_ps_pending; // a single press counted, PS still held
static int ps_poll(void)
{
	if (s_kctrl < 0)
		return 0;
	if (!s_ps_locked) {
		if (rs12_time_us() - s_ps_home_t > 1500000)
			ps_lock(1); // (back from the home screen)
		return 0;
	}
	const unsigned st = rs12KCtrlPsState(), one = st & 0x7FFF, two = (st >> 15) & 0x7FFF;
	if (two != s_ps_two) {
		s_ps_two = two;
		s_ps_one = one;
		s_ps_pending = 0;
		ps_lock(0);
		rs12KCtrlGoHome();
		s_ps_home_t = rs12_time_us();
		return 0;
	}
	if (one != s_ps_one) {
		s_ps_one = one;
		s_ps_pending = 1;
	}
	if (!s_ps_pending)
		return 0;
	if (st & (1u << 30)) { // still held: START with it is the system's screenshot
		SceCtrlData pad;
		if (sceCtrlPeekBufferPositive(0, &pad, 1) >= 0 && (pad.buttons & SCE_CTRL_START))
			s_ps_pending = 0;
		return 0;
	}
	s_ps_pending = 0;
	return 1;
}

int rs12_menu_should_open(uint32_t buttons)
{
	const int combo = (buttons & MENU_OPEN_COMBO) == MENU_OPEN_COMBO;
	const int prev_combo = (s_prev_buttons & MENU_OPEN_COMBO) == MENU_OPEN_COMBO;
	const int ps = ps_poll();
	const int open = (combo && !prev_combo) || ps;
	s_prev_buttons = buttons; // so the same press does not act right away inside the menu
	if (open) {
		if (!s_keep_place) { // (closed with SELECT+START or PS: opens where it was, sub pages too)
			s_page = PAGE_MAIN;
			s_sel = 0;
		}
		s_keep_place = 0;
		s_pop = 0;
		s_reset_armed = 0;
		s_dirty = 1;
		s_changed = 0;
		rs12_menu_states_changed();
	}
	return open;
}

int rs12_menu_slot(void)
{
	return s_state_slot;
}

static int page_items(void)
{
	switch (s_page) {
	case PAGE_DISPLAY: return D_COUNT;
	case PAGE_CONTROLS: return ctrl_rows();
	case PAGE_TURBO: return 4;
	case PAGE_TOUCH: return 5;
	case PAGE_TEDIT: return 1;
	case PAGE_COMBO: return COMBO_ROWS;
	case PAGE_ANALOG: {
		int k[16];
		return analog_rows(k);
	}
	case PAGE_STATES: return S_COUNT;
	case PAGE_ABOUT: return ABOUT_VISIBLE;
	default: return main_rows();
	}
}

static void go_page(int page)
{
	if (s_page == PAGE_MAIN)
		s_main_sel = s_sel;
	s_page = page;
	s_sel = (page == PAGE_MAIN) ? s_main_sel : 0;
	if (page == PAGE_MAIN && s_sel >= main_rows())
		s_sel = 0;
	s_reset_armed = 0;
}
// back one level: Turbo, Combo keys and the gun / wheel / lever page return to their row in Controls, other pages to the
// main page
static void page_back(void)
{
	const int from = s_page;
	if (from == PAGE_TEDIT) {
		go_page(PAGE_TOUCH);
		s_sel = 2; // (Edit touch layout)
		return;
	}
	if (from == PAGE_TURBO || from == PAGE_COMBO || from == PAGE_ANALOG || from == PAGE_TOUCH) {
		go_page(PAGE_CONTROLS);
		s_sel = ctrl_row_of(from == PAGE_TURBO ? CK_TURBO : from == PAGE_COMBO ? CK_COMBO : from == PAGE_TOUCH ? CK_TOUCH : CK_ANALOG);
		return;
	}
	go_page(PAGE_MAIN);
}

#define REPEAT_KEYS (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT)
// held directions repeat after 300 ms, then every 60 ms. Timed by the clock, not by loop passes: a slow redraw made a
// pass-counted repeat late and slow
static uint32_t key_repeat(uint32_t held, uint32_t pressed, uint64_t *next)
{
	const uint32_t dir = held & REPEAT_KEYS;
	const uint64_t now = rs12_time_us();
	if (!dir || (pressed & REPEAT_KEYS)) {
		*next = now + 300000;
		return pressed;
	}
	if (now >= *next) {
		*next = now + 60000;
		pressed |= dir;
	}
	return pressed;
}

static int wrap(int v, int n)
{
	return (v % n + n) % n;
}

// value names (items and the choice window)
static const char *const aspect[] = { "4:3", "Full screen", "Integer" };
static const char *const filter[] = { "Smooth", "Sharp" };
static const char *const border[] = { "Black", "Artwork", "Color" };
static const char *const rotate[] = { "Auto", "Off", "Vertical L", "Vertical R" };
static const char *const border_col[] = { "Navy", "Gray", "Maroon", "Green", "Purple", "Brick" };
static const char *const stick[] = { "Off", "Left", "Right", "Both" };
static const char *const turbo_speed[] = { "Fast (15/s)", "Medium (10/s)", "Slow (7.5/s)" };

static void change(int dir);
// an item with several values opens a window listing them all on the confirm button. The values of the selected item:
// names, count (0 = not such an item), current index
#define MAX_CHOICES 16
enum { CH_TEXT, CH_KEYS, CH_COMBO }; // how the choice window draws the values: names, button icons, key pairs
static int s_choice_kind; // the last item_choices() listed (drawn that way)
static int item_choices(const char **out, int *cur, const char **title)
{
	static char num[MAX_CHOICES][24];
	int n = 0;
	*cur = 0;
	*title = "";
	s_choice_kind = CH_TEXT;
	if (s_page == PAGE_DISPLAY) {
		const char *const *v = NULL;
		switch (s_sel) {
		case D_ASPECT: v = aspect; n = RS12_ASPECT_COUNT; *cur = g_set.aspect; *title = "Screen"; break;
		case D_ROTATE: v = rotate; n = RS12_ROT_COUNT; *cur = g_set.rotate; *title = "Rotate"; break;
		case D_FILTER: v = filter; n = RS12_FILTER_COUNT; *cur = g_set.filter; *title = "Filter"; break;
		case D_BORDER: v = border; n = RS12_BORDER_COUNT; *cur = g_set.border; *title = "Border"; break;
		case D_BORDER_COL: v = border_col; n = RS12_BCOL_COUNT; *cur = g_set.border_color; *title = "Border color"; break;
		case D_LATENCY: v = s_latency_name; n = RS12_LATENCY_COUNT; *cur = g_set.latency; *title = "Latency"; break;
		case D_THEME: v = s_theme_name; n = RS12_THEME_COUNT; *cur = g_set.theme; *title = "Theme"; break;
		default: return 0;
		}
		for (int i = 0; i < n; i++)
			out[i] = v[i];
	} else if (CTRL_PAGE) {
		int act;
		const int k = row_kind(s_sel, &act);
		int key, func;
		if (row_key(k, act, &key, &func)) {
			// buttons 2-6, the turbo key and the gun / wheel keys can also be None
			const int with_none = !(k == CK_ACT && (act == RS12_ACT_B1 || act >= RS12_ACT_COIN));
			n = RS12_KEY_COUNT + with_none;
			for (int i = 0; i < n; i++)
				out[i] = rs12_key_name(i);
			*cur = key;
			s_choice_kind = CH_KEYS;
			*title = k == CK_ACT ? act_name(act) : k == CK_TURBO_KEY ? "Turbo key" : k == CK_GUN_FIRE ? "Gun trigger" :
				k == CK_GUN_SENSOR ? "Gun scope sensor" : k == CK_GAS ? "Gas pedal" : "Brake pedal";
		} else if (k == CK_TURBO_BTN) {
			n = ctrl_buttons();
			for (int i = 0; i < n; i++) {
				snprintf(num[i], sizeof(num[i]), "Button %d", i + 1);
				out[i] = num[i];
			}
			*cur = g_set.turbo_btn - 1;
			*title = "Turbo button";
		} else if (k == CK_TURBO_SPEED) {
			n = RS12_TURBO_SPEED_COUNT;
			for (int i = 0; i < n; i++)
				out[i] = turbo_speed[i];
			*cur = g_set.turbo_speed;
			*title = "Turbo speed";
		} else if (k == CK_STICK) {
			n = RS12_STICK_COUNT;
			for (int i = 0; i < n; i++)
				out[i] = stick[i];
			*cur = g_set.stick;
			*title = "Analog stick";
		} else if (k == CK_GUN_SENS) {
			n = 10;
			for (int i = 0; i < n; i++) {
				snprintf(num[i], sizeof(num[i]), "%d%s", i + 1, i == 9 ? "  (direct)" : "");
				out[i] = num[i];
			}
			*cur = g_set.gun_sens - 1;
			*title = "Gun sensitivity";
		}
	} else if (s_page == PAGE_COMBO && s_sel < RS12_COMBO_KEYS) {
		n = COMBO_CHOICES;
		for (int i = 0; i < n; i++)
			out[i] = "";
		*cur = combo_index(g_set.combo[s_combo_order[s_sel]]);
		s_choice_kind = CH_COMBO;
		*title = "Combo for";
	}
	return n >= 3 ? n : 0; // two values: the button just flips them
}
static void set_choice(int idx)
{
	const char *names[MAX_CHOICES], *title;
	int cur, n = item_choices(names, &cur, &title);
	s_quiet = 1; // (the steps between pass other values: warn once at the end)
	for (int guard = 0; n && cur != idx && guard < MAX_CHOICES + 2; guard++) {
		change(+1); // the same steps as the right button (wraps, applies)
		n = item_choices(names, &cur, &title);
	}
	s_quiet = 0;
	int act, key, func;
	if (CTRL_PAGE && row_key(row_kind(s_sel, &act), act, &key, &func))
		warn_clash(key, func);
	else if (s_page == PAGE_COMBO && s_sel < RS12_COMBO_KEYS && g_set.combo[s_combo_order[s_sel]])
		warn_clash(s_combo_order[s_sel], KF_COMBO + s_combo_order[s_sel]);
}

static void change(int dir)
{
	switch (s_page) {
	case PAGE_DISPLAY:
		switch (s_sel) {
		case D_FPS: g_set.fps = !g_set.fps; break;
		case D_ASPECT: g_set.aspect = wrap(g_set.aspect + dir, RS12_ASPECT_COUNT); break;
		case D_ROTATE: g_set.rotate = wrap(g_set.rotate + dir, RS12_ROT_COUNT); break;
		case D_FILTER: g_set.filter = wrap(g_set.filter + dir, RS12_FILTER_COUNT); break;
		case D_BORDER: g_set.border = wrap(g_set.border + dir, RS12_BORDER_COUNT); break;
		case D_BORDER_COL: g_set.border_color = wrap(g_set.border_color + dir, RS12_BCOL_COUNT); break;
		case D_LATENCY: g_set.latency = wrap(g_set.latency + dir, RS12_LATENCY_COUNT); break;
		case D_THEME:
			g_set.theme = wrap(g_set.theme + dir, RS12_THEME_COUNT);
			s_drawn_h = 0; // (every colour changes: the whole screen again)
			break;
		default: return;
		}
		rs12_settings_apply_video();
		s_changed = 1;
		break;
	case PAGE_CONTROLS:
	case PAGE_TURBO:
	case PAGE_ANALOG:
	case PAGE_TOUCH: {
		int act;
		const int k = row_kind(s_sel, &act);
		if (k == CK_ACT && act >= 0) // buttons 2-6 can also be None
			g_set.keymap[act] = wrap(g_set.keymap[act] + dir, act == RS12_ACT_B1 || act >= RS12_ACT_COIN ? RS12_KEY_COUNT
				: RS12_KEY_COUNT + 1);
		else if (k == CK_TURBO_KEY)
			g_set.turbo_key = wrap(g_set.turbo_key + dir, RS12_KEY_COUNT + 1); // + None (off)
		else if (k == CK_TURBO_BTN) // 1..number of game buttons
			g_set.turbo_btn = wrap(g_set.turbo_btn - 1 + dir, ctrl_buttons()) + 1;
		else if (k == CK_TURBO_SPEED)
			g_set.turbo_speed = wrap(g_set.turbo_speed + dir, RS12_TURBO_SPEED_COUNT);
		else if (k == CK_STICK)
			g_set.stick = wrap(g_set.stick + dir, RS12_STICK_COUNT);
		else if (k == CK_AIM_STICK)
			g_set.aim_stick = wrap(g_set.aim_stick + dir, RS12_AIM_STICK_COUNT);
		else if (k == CK_AIM_MODE)
			g_set.aim_mode = wrap(g_set.aim_mode + dir, RS12_AIM_MODE_COUNT);
		else if (k == CK_CROSSHAIR)
			g_set.crosshair = !g_set.crosshair;
		else if (k == CK_TOUCH_GUN)
			g_set.touch_gun = !g_set.touch_gun;
		else if (k == CK_T_ON)
			g_set.touch_keys = !g_set.touch_keys;
		else if (k == CK_T_SHOW)
			g_set.touch_show = !g_set.touch_show;
		else if (k == CK_GUN_SENS)
			g_set.gun_sens = wrap(g_set.gun_sens - 1 + dir, 10) + 1;
		else if (k == CK_WHEEL_STICK)
			g_set.wheel_stick = wrap(g_set.wheel_stick + dir, RS12_AIM_STICK_COUNT);
		else if (k == CK_LEVER_STICK)
			g_set.lever_stick = wrap(g_set.lever_stick + dir, RS12_AIM_STICK_COUNT);
		else if (k == CK_GUN_FIRE)
			g_set.gun_fire = wrap(g_set.gun_fire + dir, RS12_KEY_COUNT + 1); // + None
		else if (k == CK_GUN_SENSOR)
			g_set.gun_sensor = wrap(g_set.gun_sensor + dir, RS12_KEY_COUNT + 1);
		else if (k == CK_GAS)
			g_set.gas_key = wrap(g_set.gas_key + dir, RS12_KEY_COUNT + 1);
		else if (k == CK_BRAKE)
			g_set.brake_key = wrap(g_set.brake_key + dir, RS12_KEY_COUNT + 1);
		else if (k == CK_CONFIRM)
			g_set.confirm = wrap(g_set.confirm + dir, RS12_CONFIRM_COUNT);
		else
			return;
		s_changed = 1;
		int key, func;
		if (row_key(k, act, &key, &func))
			warn_clash(key, func);
		break;
	}
	case PAGE_STATES:
		if (s_sel == S_SLOT)
			s_state_slot = wrap(s_state_slot + dir, STATE_SLOTS);
		break;
	case PAGE_COMBO: {
		if (s_sel >= RS12_COMBO_KEYS)
			return;
		int *v = &g_set.combo[s_combo_order[s_sel]];
		*v = combo_choice(wrap(combo_index(*v) + dir, COMBO_CHOICES));
		s_changed = 1;
		if (*v)
			warn_clash(s_combo_order[s_sel], KF_COMBO + s_combo_order[s_sel]);
		break;
	}
	}
}

// ---- touch layout editor (Controls > Touch controls > Edit touch layout) ----
// Drag on an empty spot: a new button (then its key). Tap a button: its key, or Delete. Drag a button: move it; drag its
// lower right corner: its size. Square adds one in the middle, Triangle opens the selected one's keys (no touch needed).
// START, the back button or Done leave (the layout is saved then)
#define TE_MIN 42    // smallest button side (sizes go in TE_STEP steps)
#define TE_STEP 6
#define TE_GRAB 22   // an edge or corner is grabbed this far from it, inside or outside the button
#define TE_POP_N (RS12_TKEY_COUNT + 1) // the keys, then Delete
#define TE_POP_RH 32
enum { TE_IDLE, TE_NEW, TE_MOVE, TE_SIZE };
enum { TE_L = 1, TE_R = 2, TE_T = 4, TE_B = 8 }; // edges being dragged (TE_SIZE)
static int te_sel = -1, te_mode, te_prev_n, te_sx, te_sy, te_ox, te_oy, te_ow, te_oh, te_moved, te_pop = -1, te_pop_sel;
static int te_edges;
static int te_snap(int v) // a size in TE_STEP steps, at least TE_MIN
{
	v = (v + TE_STEP / 2) / TE_STEP * TE_STEP;
	return v < TE_MIN ? TE_MIN : v;
}
// the edges of button a a touch grabs (0: none). Within TE_GRAB of an edge, outside too; inside a small button at
// most a quarter of its side, so its middle still moves it
static int te_edges_at(const Rs12TouchArea *a, int x, int y)
{
	const int gx = a->w / 4 < TE_GRAB ? a->w / 4 : TE_GRAB, gy = a->h / 4 < TE_GRAB ? a->h / 4 : TE_GRAB;
	if (x < a->x - TE_GRAB || x >= a->x + a->w + TE_GRAB || y < a->y - TE_GRAB || y >= a->y + a->h + TE_GRAB)
		return 0;
	int e = 0;
	if (x < a->x + gx)
		e |= TE_L;
	else if (x >= a->x + a->w - gx)
		e |= TE_R;
	if (y < a->y + gy)
		e |= TE_T;
	else if (y >= a->y + a->h - gy)
		e |= TE_B;
	return e;
}
static int te_nx, te_ny, te_nw, te_nh; // the button being drawn (TE_NEW)
static void tedit_open(void)
{
	te_sel = -1;
	te_mode = TE_IDLE;
	te_prev_n = 1; // (a finger already down does nothing until it is lifted)
	te_pop = -1;
	s_drawn_h = 0;
	s_dirty = 1;
}
static void te_pop_rect(int *x, int *y, int *w, int *h)
{
	*w = 320;
	*h = 64 + TE_POP_N * TE_POP_RH + 12;
	*x = (SCREEN_W - *w) / 2;
	*y = (SCREEN_H - *h) / 2;
}
static int te_done_hit(int x, int y) { return x >= SCREEN_W - 130 && y < 46; }
static void te_pop_open(int i)
{
	const Rs12TouchArea *a = rs12_touch_area(i);
	if (!a)
		return;
	te_pop = i;
	te_pop_sel = a->key;
}
static void te_pop_apply(int row)
{
	Rs12TouchArea *a = rs12_touch_area(te_pop);
	if (row == RS12_TKEY_COUNT) { // Delete
		rs12_touch_remove(te_pop);
		te_sel = -1;
	} else if (a && row >= 0 && row < RS12_TKEY_COUNT) {
		a->key = (int8_t)row;
		rs12_touch_changed();
	}
	te_pop = -1;
}
static void tedit_leave(void)
{
	rs12_touch_save();
	te_pop = -1;
	page_back();
	s_drawn_h = 0;
	s_dirty = 1;
}
static void tedit_update(uint32_t pressed)
{
	const int o_ok = g_set.confirm == RS12_CONFIRM_CIRCLE;
	const uint32_t ok = o_ok ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS, back = o_ok ? SCE_CTRL_CROSS : SCE_CTRL_CIRCLE;
	int x[6], y[6];
	const int n = rs12_touch_read(x, y);
	const int down = n > 0 && te_prev_n == 0, held = n > 0, up = n == 0 && te_prev_n > 0;
	te_prev_n = n;
	if (te_pop >= 0) { // key window: touch a row, or up/down and confirm; back or a touch outside closes it
		if (pressed & SCE_CTRL_UP) te_pop_sel = wrap(te_pop_sel - 1, TE_POP_N);
		if (pressed & SCE_CTRL_DOWN) te_pop_sel = wrap(te_pop_sel + 1, TE_POP_N);
		if (pressed & ok)
			te_pop_apply(te_pop_sel);
		else if (pressed & (back | SCE_CTRL_START))
			te_pop = -1;
		else if (down) {
			int wx, wy, ww, wh;
			te_pop_rect(&wx, &wy, &ww, &wh);
			const int row = (y[0] - wy - 60) / TE_POP_RH;
			if (x[0] >= wx && x[0] < wx + ww && y[0] >= wy + 60 && row >= 0 && row < TE_POP_N)
				te_pop_apply(row);
			else if (!(x[0] >= wx && x[0] < wx + ww && y[0] >= wy && y[0] < wy + wh))
				te_pop = -1;
		}
		if (pressed || down)
			s_dirty = 1;
		return;
	}
	if (pressed & (back | SCE_CTRL_START)) {
		tedit_leave();
		return;
	}
	if (pressed & SCE_CTRL_SQUARE) { // a new button in the middle, then its key
		const int i = rs12_touch_add(SCREEN_W / 2 - 50, SCREEN_H / 2 - 50, 100, 100, RS12_KEY_CROSS);
		if (i >= 0) {
			te_sel = i;
			te_pop_open(i);
		}
	}
	if ((pressed & SCE_CTRL_TRIANGLE) && te_sel >= 0)
		te_pop_open(te_sel);
	if (down) {
		if (te_done_hit(x[0], y[0])) {
			tedit_leave();
			return;
		}
		te_sx = x[0];
		te_sy = y[0];
		te_moved = 0;
		int hit = -1, edges = 0;
		// an edge or corner of the selected button first (its grab zone reaches outside it), then any button on top
		if (te_sel >= 0 && rs12_touch_area(te_sel)) {
			edges = te_edges_at(rs12_touch_area(te_sel), x[0], y[0]);
			if (edges)
				hit = te_sel;
		}
		for (int i = rs12_touch_count() - 1; i >= 0 && hit < 0; i--) { // (the last drawn is on top)
			const Rs12TouchArea *a = rs12_touch_area(i);
			if (x[0] >= a->x && x[0] < a->x + a->w && y[0] >= a->y && y[0] < a->y + a->h) {
				hit = i;
				edges = te_edges_at(a, x[0], y[0]);
			}
		}
		if (hit >= 0) {
			const Rs12TouchArea *a = rs12_touch_area(hit);
			te_sel = hit;
			te_edges = edges;
			te_mode = edges ? TE_SIZE : TE_MOVE;
			te_ox = a->x;
			te_oy = a->y;
			te_ow = a->w;
			te_oh = a->h;
		} else {
			te_mode = TE_NEW;
			te_sel = -1;
			te_nx = x[0];
			te_ny = y[0];
			te_nw = te_nh = 0;
		}
		s_dirty = 1;
	} else if (held && te_mode != TE_IDLE) {
		const int dx = x[0] - te_sx, dy = y[0] - te_sy;
		if (dx > 8 || dx < -8 || dy > 8 || dy < -8)
			te_moved = 1;
		Rs12TouchArea *a = rs12_touch_area(te_sel);
		if (te_mode == TE_NEW) { // (from the first touch, in TE_STEP steps)
			te_nw = (dx < 0 ? -dx : dx) / TE_STEP * TE_STEP;
			te_nh = (dy < 0 ? -dy : dy) / TE_STEP * TE_STEP;
			te_nx = dx < 0 ? te_sx - te_nw : te_sx;
			te_ny = dy < 0 ? te_sy - te_nh : te_sy;
		} else if (a && te_moved && te_mode == TE_MOVE) {
			const int nx = te_ox + dx, ny = te_oy + dy;
			a->x = (int16_t)(nx < 0 ? 0 : nx > SCREEN_W - a->w ? SCREEN_W - a->w : nx);
			a->y = (int16_t)(ny < 0 ? 0 : ny > SCREEN_H - a->h ? SCREEN_H - a->h : ny);
			rs12_touch_changed();
		} else if (a && te_mode == TE_SIZE) { // the grabbed edges follow the finger, the others stay; TE_STEP steps
			int x0 = te_ox, x1 = te_ox + te_ow, y0 = te_oy, y1 = te_oy + te_oh;
			if (te_edges & TE_L) {
				const int lim = x0 + dx < 0 ? -x0 : dx;
				x0 = x1 - te_snap(te_ow - lim);
				if (x0 < 0)
					x0 = x1 - (x1 / TE_STEP * TE_STEP < TE_MIN ? TE_MIN : x1 / TE_STEP * TE_STEP);
			} else if (te_edges & TE_R) {
				x1 = x0 + te_snap(te_ow + dx);
				if (x1 > SCREEN_W)
					x1 = x0 + (SCREEN_W - x0) / TE_STEP * TE_STEP;
			}
			if (te_edges & TE_T) {
				const int lim = y0 + dy < 0 ? -y0 : dy;
				y0 = y1 - te_snap(te_oh - lim);
				if (y0 < 0)
					y0 = y1 - (y1 / TE_STEP * TE_STEP < TE_MIN ? TE_MIN : y1 / TE_STEP * TE_STEP);
			} else if (te_edges & TE_B) {
				y1 = y0 + te_snap(te_oh + dy);
				if (y1 > SCREEN_H)
					y1 = y0 + (SCREEN_H - y0) / TE_STEP * TE_STEP;
			}
			a->x = (int16_t)x0;
			a->y = (int16_t)y0;
			a->w = (int16_t)(x1 - x0);
			a->h = (int16_t)(y1 - y0);
			rs12_touch_changed();
		}
		s_dirty = 1;
	} else if (up) {
		if (te_mode == TE_NEW && te_nw >= TE_MIN && te_nh >= TE_MIN) {
			const int i = rs12_touch_add(te_nx, te_ny, te_nw, te_nh, RS12_KEY_CROSS);
			if (i >= 0) {
				te_sel = i;
				te_pop_open(i);
			}
		} else if (te_mode == TE_MOVE && !te_moved)
			te_pop_open(te_sel); // a tap: its key
		te_mode = TE_IDLE;
		s_dirty = 1;
	}
}

static rs12_menu_result confirm(void)
{
	switch (s_page) {
	case PAGE_MAIN:
		switch (main_kind(s_sel)) {
		case M_RESUME: return RS12_MENU_CLOSE;
		case M_RESET:
			if (!s_reset_armed) {
				s_reset_armed = 1;
				return RS12_MENU_STAY;
			}
			return RS12_MENU_RESET;
		case M_SERVICE:
			// service DIP switch: on closes the menu (the game enters its test menu), off stays in the menu
			rs12_dip_service = !rs12_dip_service;
			if (rs12_dip_service)
				return RS12_MENU_CLOSE;
			break;
		case M_STATES: go_page(PAGE_STATES); break;
		case M_DISPLAY: go_page(PAGE_DISPLAY); break;
		case M_CONTROLS: go_page(PAGE_CONTROLS); break;
		case M_GAME_SAVE: // this game only (the global settings file is not written)
			if (rs12_settings_save_game())
				rs12_osd_show("Saved settings for %s", s_set);
			else
				rs12_osd_show("Could not save game settings");
			break;
		case M_GAME_DEL:
			if (rs12_settings_delete_game())
				rs12_osd_show("Deleted settings for %s", s_set);
			else
				rs12_osd_show("Could not delete game settings");
			break;
		case M_ABOUT:
			go_page(PAGE_ABOUT);
			s_about_scroll = 0;
			break;
		case M_SELECT_GAME: return RS12_MENU_SELECT_GAME;
		case M_REFRESH: return RS12_MENU_REFRESH;
		case M_EXIT: return RS12_MENU_EXIT;
		case M_BACK: return RS12_MENU_CLOSE;
		}
		break;
	case PAGE_DISPLAY:
		if (s_sel == D_BACK) go_page(PAGE_MAIN);
		else change(+1);
		break;
	case PAGE_CONTROLS:
	case PAGE_TURBO:
	case PAGE_ANALOG:
	case PAGE_TOUCH: {
		int act;
		const int k = row_kind(s_sel, &act);
		if (k == CK_T_EDIT) {
			go_page(PAGE_TEDIT);
			tedit_open();
			break;
		}
		if (k == CK_T_CLEAR) {
			if (!s_reset_armed) {
				s_reset_armed = 1; // (press again to clear)
				break;
			}
			s_reset_armed = 0;
			rs12_touch_clear();
			rs12_touch_save();
			rs12_osd_show("Touch layout cleared");
			break;
		}
		if (k == CK_TOUCH) go_page(PAGE_TOUCH);
		else if (k == CK_BACK || k == CK_ABACK) page_back();
		else if (k == CK_TURBO) go_page(PAGE_TURBO);
		else if (k == CK_COMBO) go_page(PAGE_COMBO);
		else if (k == CK_ANALOG) go_page(PAGE_ANALOG);
		else if (k == CK_DEFAULTS) {
			rs12_settings_default_keys();
			s_changed = 1;
		} else change(+1);
		break;
	}
	case PAGE_ABOUT:
		go_page(PAGE_MAIN);
		break;
	case PAGE_COMBO:
		if (s_sel == COMBO_ROWS - 1) page_back();
		else change(+1);
		break;
	case PAGE_STATES:
		if (s_sel == S_SAVE) return RS12_MENU_SAVE;
		if (s_sel == S_LOAD) {
			if (s_slot_used[s_state_slot])
				return RS12_MENU_LOAD;
			rs12_osd_show("Slot %d is empty", s_state_slot);
		}
		if (s_sel == S_BACK) go_page(PAGE_MAIN);
		if (s_sel == S_SLOT) change(+1);
		break;
	}
	return RS12_MENU_STAY;
}

rs12_menu_result rs12_menu_update(uint32_t buttons)
{
	const int ps_closed = ps_poll();
	if (ps_closed)
		s_pop = 0;
	static uint64_t rep_next;
	uint32_t pressed = key_repeat(buttons, buttons & ~s_prev_buttons, &rep_next);
	s_prev_buttons = buttons;
	if (pressed)
		s_dirty = 1;
	rs12_menu_result r = RS12_MENU_STAY;
	if (s_page == PAGE_TEDIT && !ps_closed) { // touch layout editor: touch and its own buttons
		tedit_update(pressed);
		return RS12_MENU_STAY;
	}
	const int n = page_items();
	{
		const int o_ok = g_set.confirm == RS12_CONFIRM_CIRCLE;
		const uint32_t ok = o_ok ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS, back = o_ok ? SCE_CTRL_CROSS : SCE_CTRL_CIRCLE;
		const char *names[MAX_CHOICES], *title;
		int cur;
		const int nc = item_choices(names, &cur, &title);
		if (s_pop && !ps_closed) { // choice window: up/down, confirm sets, back closes
			if (!nc)
				s_pop = 0;
			else if (pressed & SCE_CTRL_UP)
				s_pop_sel = wrap(s_pop_sel - 1, nc);
			else if (pressed & SCE_CTRL_DOWN)
				s_pop_sel = wrap(s_pop_sel + 1, nc);
			else if (pressed & ok) {
				set_choice(s_pop_sel);
				s_pop = 0;
				s_changed = 1;
			} else if (pressed & (back | SCE_CTRL_START))
				s_pop = 0;
			if (!s_pop)
				s_drawn_h = 0; // the whole panel again (the window covered part of it)
			return RS12_MENU_STAY;
		}
		if ((pressed & ok) && nc && !ps_closed) {
			s_pop = 1;
			s_pop_sel = cur;
			s_drawn_h = 0;
			return RS12_MENU_STAY;
		}
	}
	if (s_page == PAGE_ABOUT) { // up/down scroll
		const int max = ABOUT_LINES > ABOUT_VISIBLE ? ABOUT_LINES - ABOUT_VISIBLE : 0;
		if ((pressed & SCE_CTRL_UP) && s_about_scroll > 0)
			s_about_scroll--;
		if ((pressed & SCE_CTRL_DOWN) && s_about_scroll < max)
			s_about_scroll++;
		pressed &= ~REPEAT_KEYS;
	}
	if (pressed & SCE_CTRL_UP) {
		s_sel = wrap(s_sel - 1, n);
		s_reset_armed = 0;
	}
	if (pressed & SCE_CTRL_DOWN) {
		s_sel = wrap(s_sel + 1, n);
		s_reset_armed = 0;
	}
	if (pressed & SCE_CTRL_LEFT)
		change(-1);
	if (pressed & SCE_CTRL_RIGHT)
		change(+1);
	// confirm/back follow the setting (Cross/Circle). If the setting changes during this input, use the setting at press time
	const int o_ok = g_set.confirm == RS12_CONFIRM_CIRCLE;
	const uint32_t ok = o_ok ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS, back = o_ok ? SCE_CTRL_CROSS : SCE_CTRL_CIRCLE;
	if (pressed & ok)
		r = confirm();
	else if (pressed & back) {
		if (s_page == PAGE_MAIN)
			r = RS12_MENU_CLOSE;
		else
			page_back();
	}
	if (pressed & SCE_CTRL_START) {
		r = RS12_MENU_CLOSE;
		s_keep_place = (buttons & MENU_OPEN_COMBO) == MENU_OPEN_COMBO && !s_list_mode;
	}
	if (ps_closed) { // PS closes the menu as SELECT+START does (opens again where it was)
		if (s_page == PAGE_TEDIT) {
			rs12_touch_save();
			page_back();
		}
		r = RS12_MENU_CLOSE;
		s_keep_place = !s_list_mode;
	}
	// memory card write only when the user changed something (no card I/O otherwise while a game is loaded)
	if (r != RS12_MENU_STAY && r != RS12_MENU_SAVE && s_changed) {
		rs12_settings_save();
		s_changed = 0;
	}
	return r;
}

// ================================ drawing ================================
// "Re" white + "Sys12" in the brand colour (Re franchise naming rule). Returns the width
static int draw_brand(UiCanvas *c, int x, int y, float scale)
{
	int w = ui_text(c, x, y, COL_TEXT, scale, "Re");
	w += ui_text(c, x + w, y, COL_BRAND, scale, "Sys12");
	return w;
}

// one About line (heading lines start with '#'; the "ReSys12" heading uses the brand colours)
static void about_line(UiCanvas *c, int x, int y, const char *t)
{
	if (!t[0])
		return;
	if (!strncmp(t, "#ReSys12", 8)) {
		const int w = draw_brand(c, x, y, 1.0f);
		ui_text(c, x + w, y, COL_TEXT, 1.0f, t + 8);
		return;
	}
	const int head = t[0] == '#';
	ui_text(c, x, y, head ? COL_SEL : (t[0] == ' ' ? COL_DIM : COL_TEXT), head ? 1.0f : 0.85f, t + head);
}

#define VAL_ON "\x01on"   // item value: draw a toggle switch (two-state items only)
#define VAL_OFF "\x01off"
// the selected row: a soft bar with a shadow and a blinking outline (a steady one under the choice window)
static void sel_bar(UiCanvas *c, int x, int by, int w, int h)
{
	rrect(c, x, by + 3, w, h, 6, theme()->shade);
	rrect(c, x, by, w, h, 6, COL_HILITE);
	if (s_pop) // the choice window is over it
		rrect_outline(c, x, by, w, h, 6, 2, COL_FOCUS_STILL);
	else
		focus_at(c, x, by, w, h);
}
static void item(UiCanvas *c, int px, int ty, int i, int sel, const char *name, const char *value)
{
	const uint32_t col = (i == sel) ? COL_SEL : COL_TEXT;
	if (i == sel)
		sel_bar(c, px + 12, ty - 7 - (LINE_H - 6) / 2, PANEL_W - 40, LINE_H - 6); // bar centred on the text
	ui_text(c, px + 40, ty, i == sel ? COL_TEXT : COL_ITEM, 1.0f, name);
	if (value && value[0] == 1) {
		// two-state item as a toggle switch: a pill with the knob on the right (on, accent colour) or left (off, grey)
		const int on = value[2] == 'n';
		const float r = 10.0f, x0 = (float)(px + VALUE_X) + r, x1 = x0 + 26.0f, cy = (float)ty - 7.0f;
		const uint32_t track = on ? theme()->toggle_on : theme()->toggle_off;
		ui_circle(c, x0, cy, r, track);
		ui_circle(c, x1, cy, r, track);
		ui_rect(c, (int)x0, (int)(cy - r), (int)(x1 - x0), (int)(2 * r), track);
		ui_circle(c, on ? x1 : x0, cy, r - 3.0f, UI_RGBA(0xF4, 0xF4, 0xF4, 0xFF));
	} else if (value) {
		char buf[96];
		snprintf(buf, sizeof(buf), "< %s >", value);
		ui_text(c, px + VALUE_X, ty, col, 1.0f, buf);
	}
}

static void thick_line(UiCanvas *c, float x0, float y0, float x1, float y1, uint32_t col)
{
	ui_line(c, x0, y0, x1, y1, 2.4f, col);
}

// Draw a button symbol starting at (x, cy), vertically centered on cy, and return the width used. L, R, Select, Start, None as text
static int draw_key(UiCanvas *c, int x, int cy, int ty, int key, uint32_t text_col)
{
	const float r = 9.0f;
	const float cx = x + r + 1;
	switch (key) {
	case RS12_KEY_CROSS: {
		const uint32_t col = UI_RGBA(0x7C, 0xB4, 0xF0, 0xFF);
		thick_line(c, cx - r * 0.8f, cy - r * 0.8f, cx + r * 0.8f, cy + r * 0.8f, col);
		thick_line(c, cx - r * 0.8f, cy + r * 0.8f, cx + r * 0.8f, cy - r * 0.8f, col);
		return (int)(r * 2 + 2);
	}
	case RS12_KEY_CIRCLE:
		ui_circle(c, cx, cy, r, UI_RGBA(0xFF, 0x6E, 0x6E, 0xFF));
		ui_circle(c, cx, cy, r - 2.5f, COL_PANEL);
		return (int)(r * 2 + 2);
	case RS12_KEY_SQUARE: {
		const uint32_t col = UI_RGBA(0xF0, 0xA0, 0xD8, 0xFF);
		const float h = r * 0.85f;
		ui_rect(c, (int)(cx - h), (int)(cy - h), (int)(h * 2), 2, col);
		ui_rect(c, (int)(cx - h), (int)(cy + h - 2), (int)(h * 2), 2, col);
		ui_rect(c, (int)(cx - h), (int)(cy - h), 2, (int)(h * 2), col);
		ui_rect(c, (int)(cx + h - 2), (int)(cy - h), 2, (int)(h * 2), col);
		return (int)(r * 2 + 2);
	}
	case RS12_KEY_TRIANGLE: {
		const uint32_t col = UI_RGBA(0x50, 0xE0, 0xC0, 0xFF);
		const float top = cy - r, bot = cy + r * 0.75f, half = r;
		thick_line(c, cx, top, cx - half, bot, col);
		thick_line(c, cx, top, cx + half, bot, col);
		thick_line(c, cx - half, bot, cx + half, bot, col);
		return (int)(r * 2 + 2);
	}
	default: {
		const char *name = rs12_key_name(key);
		ui_text(c, x, ty, text_col, 1.0f, name);
		return ui_text_width(1.0f, name);
	}
	}
}

// a combo value: "None" or two key glyphs joined by "+". Returns the width
static int draw_combo(UiCanvas *c, int x, int ty, int mask, uint32_t col)
{
	if (!mask)
		return ui_text(c, x, ty, col, 1.0f, "None");
	const int x0 = x;
	int first = 1;
	for (int k = 0; k < RS12_COMBO_KEYS; k++) {
		const int key = s_combo_order[k];
		if (!(mask & (1 << key)))
			continue;
		if (!first)
			x += ui_text(c, x + 4, ty, col, 1.0f, "+") + 10;
		x += draw_key(c, x, ty - 7, ty, key, col);
		first = 0;
	}
	return x - x0;
}

// clash: the key also does something else in this game (shown in the warning colour with "!")
static void item_key_w(UiCanvas *c, int px, int ty, int i, int sel, const char *name, int key, int clash)
{
	const uint32_t col = clash ? COL_WARN : (i == sel) ? COL_SEL : COL_TEXT;
	item(c, px, ty, i, sel, name, NULL);
	int x = px + VALUE_X;
	ui_text(c, x, ty, col, 1.0f, "<");
	x += ui_text_width(1.0f, "< ");
	x += draw_key(c, x, ty - 7, ty, key, col);
	x += 6 + ui_text(c, x + 6, ty, col, 1.0f, ">");
	if (clash)
		ui_text(c, x + 8, ty, COL_WARN, 1.0f, "! duplicate");
}

static void item_key(UiCanvas *c, int px, int ty, int i, int sel, const char *name, int key)
{
	item_key_w(c, px, ty, i, sel, name, key, 0);
}

// one line of text (size 0.8): symbol chars (0x10-0x13) drawn as button icons
static void text_keys(UiCanvas *c, int x, int ty, uint32_t col, const char *s)
{
	char run[128];
	int k = 0;
	for (const char *p = s;; p++) {
		const unsigned char ch = (unsigned char)*p;
		if ((ch >= 0x10 && ch <= 0x13) || !ch || k == (int)sizeof(run) - 1) {
			run[k] = 0;
			if (k) {
				ui_text(c, x, ty, col, 0.8f, run);
				x += ui_text_width(0.8f, run);
			}
			k = 0;
			if (!ch)
				break;
			if (ch >= 0x10 && ch <= 0x13) {
				static const int keys[4] = { RS12_KEY_TRIANGLE, RS12_KEY_CIRCLE, RS12_KEY_CROSS, RS12_KEY_SQUARE };
				x += draw_key(c, x, ty - 6, ty, keys[ch - 0x10], col) + 2;
				continue;
			}
		}
		run[k++] = (char)ch;
	}
}

// Draw the menu panel to the canvas (caller uploads). overlay = over the game list (the dimmed list kept in s_under)
static int s_paint_x0, s_paint_y0, s_paint_x1, s_paint_y1;
static float s_mscr, s_mtgt; // menu rows scroll (px): drawn, target
static const uint32_t *s_under; // game list options: the dimmed list under the window (NULL: just drawn in full)
static int menu_anim(void) { return s_mscr != s_mtgt; } // (the menu is still scrolling: draw again)
static void tedit_paint(UiCanvas *c);
static void menu_paint(UiCanvas *c, int overlay)
{
	if (s_page == PAGE_TEDIT) {
		tedit_paint(c);
		return;
	}
	const int o_ok = g_set.confirm == RS12_CONFIRM_CIRCLE;
	const char *ok_name = o_ok ? G_CIR : G_CRO, *back_name = o_ok ? G_CRO : G_CIR;
	char foot_buf[128];
	const int n = page_items();
	// group gaps on the main page (rows y move down by the gaps above them)
	int gap_at[M_COUNT + 1] = { 0 };
	if (s_page == PAGE_MAIN && !s_list_mode)
		for (int i = 1; i <= n && i <= M_COUNT; i++) {
			const int k = i < n ? main_kind(i) : -1;
			gap_at[i] = gap_at[i - 1] + ((k == M_STATES || k == M_DISPLAY || k == M_ABOUT) ? GROUP_GAP : 0);
		}
	// rows need not fit. The row area is at most the screen minus a margin; taller pages scroll smoothly (the selected
	// row kept a row away from the edges), fading at a cut edge, with a scroll bar
	const int lh = s_page == PAGE_ABOUT ? ABOUT_LH : LINE_H;
	const int gmax = s_page == PAGE_MAIN && !s_list_mode && n > 0 ? gap_at[n - 1] : 0;
	const int content = n * lh + gmax, maxv = SCREEN_H - 32 - TITLE_H - FOOT_H;
	const int view = content < maxv ? content : maxv;
	const int h = TITLE_H + view + FOOT_H;
	const int px = (SCREEN_W - PANEL_W) / 2, py = (SCREEN_H - h) / 2;
	{
		static int last_page = -1, last_n = -1;
		const float maxs = (float)(content - view);
		const int sy = s_sel * lh + (s_page == PAGE_MAIN && !s_list_mode && s_sel < n ? gap_at[s_sel] : 0);
		if (s_page == PAGE_ABOUT)
			s_mtgt = 0;
		else {
			if (sy - lh < s_mtgt)
				s_mtgt = (float)(sy - lh);
			if (sy + 2 * lh > s_mtgt + view)
				s_mtgt = (float)(sy + 2 * lh - view);
		}
		if (s_mtgt > maxs) s_mtgt = maxs;
		if (s_mtgt < 0) s_mtgt = 0;
		if (s_page != last_page || n != last_n)
			s_mscr = s_mtgt; // another page: no animation
		last_page = s_page;
		last_n = n;
		s_mscr += (s_mtgt - s_mscr) * 0.3f;
		if (fabsf(s_mtgt - s_mscr) < 0.5f)
			s_mscr = s_mtgt;
	}
	const int q0 = s_paint_x0, q1 = s_paint_y0, q2 = s_paint_x1, q3 = s_paint_y1; // what the last paint covered
	s_paint_x0 = px - 8; // area this paint covers (panel and shadow; the choice window adds its own below)
	s_paint_y0 = py - 8;
	s_paint_x1 = px + PANEL_W + 14;
	s_paint_y1 = py + h + 16;
	if (overlay) { // over the game list: it is already dimmed there
		s_drawn_h = 0;
		if (s_under) {
			// the last paint's area outside this window, from the kept list (the opaque window covers its inside;
			// its rounded corners come back too)
			const int ix0 = px + 8, iy0 = py + 8, ix1 = px + PANEL_W - 8, iy1 = py + h - 8;
			if (q1 < iy0) ui_blit_rect(c, s_under, q0, q1, q2 - q0, iy0 - q1);
			if (q3 > iy1) ui_blit_rect(c, s_under, q0, iy1, q2 - q0, q3 - iy1);
			const int m0 = q1 > iy0 ? q1 : iy0, m1 = q3 < iy1 ? q3 : iy1;
			if (m1 > m0) {
				if (q0 < ix0) ui_blit_rect(c, s_under, q0, m0, ix0 - q0, m1 - m0);
				if (q2 > ix1) ui_blit_rect(c, s_under, ix1, m0, q2 - ix1, m1 - m0);
			}
		}
	} else if (s_drawn_h == h) {
		// if the panel size is unchanged, redraw only the panel (do not recompose and copy the whole dimmed screen every
		// time), with the shadow and the choice window of the last paint: they are outside the panel
		const int x0 = q0 < s_paint_x0 ? q0 : s_paint_x0, y0 = q1 < s_paint_y0 ? q1 : s_paint_y0;
		const int x1 = q2 > s_paint_x1 ? q2 : s_paint_x1, y1 = q3 > s_paint_y1 ? q3 : s_paint_y1;
		ui_fill(c, x0, y0, x1 - x0, y1 - y0, COL_BG);
	} else {
		ui_fill(c, 0, 0, SCREEN_W, SCREEN_H, COL_BG); // (set, not blended: a blend reads the whole screen, ~10 ms)
		s_drawn_h = h;
	}
	window_box(c, px, py, PANEL_W, h, COL_EDGE, COL_PANEL); // rounded panel, outline and shadow

	const char *title = "OPTIONS";
	snprintf(foot_buf, sizeof(foot_buf), "Up/Down: select   Left/Right/%s: change   %s: back", ok_name, back_name);
	const char *foot = foot_buf;
	switch (s_page) {
	case PAGE_DISPLAY:
		title = "DISPLAY";
		if (s_list_mode)
			foot = "Left/Right: change   (global settings)";
		break;
	case PAGE_TURBO:
		title = "TURBO";
		foot = "Hold the turbo key: the turbo button repeats";
		break;
	case PAGE_TOUCH:
		title = "TOUCH CONTROLS";
		foot = s_reset_armed ? "Press again to clear every touch button" : "Touch buttons on the screen press the keys you give them";
		break;
	case PAGE_CONTROLS:
		title = "CONTROLS";
		foot = s_list_mode ? "Left/Right: choose button   (global settings)" : "Left/Right: choose button   Menu: SELECT+START";
		break;
	case PAGE_ANALOG: {
		const int a = menu_analog();
		title = a == S12_ANALOG_GUN ? "GUN" : a == S12_ANALOG_WHEEL ? "WHEEL" : "LEVER";
		foot = a == S12_ANALOG_GUN ? (g_set.touch_gun ? "Touch gun: touch the screen to aim and fire (touch buttons still work)"
			: "The trigger also counts as the game's fire button") : "The d-pad works as well as the stick";
		break;
	}
	case PAGE_COMBO:
		title = "COMBO KEYS";
		snprintf(foot_buf, sizeof(foot_buf), "A key acts as two keys pressed together   Left/Right: change   %s: list",
			ok_name);
		break;
	case PAGE_STATES:
		title = "SAVE / LOAD STATE";
		foot = "Quick save: SELECT+L   Quick load: SELECT+R   (current slot)";
		break;
	case PAGE_ABOUT:
		title = "ABOUT";
		snprintf(foot_buf, sizeof(foot_buf), "Up/Down: scroll (%d/%d)   %s: back", s_about_scroll + 1,
			ABOUT_LINES > ABOUT_VISIBLE ? ABOUT_LINES - ABOUT_VISIBLE + 1 : 1, back_name);
		break;
	default:
		if (!s_list_mode && rs12_settings_has_game())
			foot = "This game uses its own settings (Delete game settings to undo)";
		break;
	}
	{
		// title: cut with "..." when it overflows the panel (MAME titles are long)
		char tbuf[128];
		snprintf(tbuf, sizeof(tbuf), "%s", s_page == PAGE_MAIN && !s_list_mode ? s_title : title);
		int len = (int)strlen(tbuf);
		while (len > 4 && ui_text_width(1.1f, tbuf) > PANEL_W - 48) {
			len--;
			memcpy(tbuf + len - 3, "...", 4);
		}
		ui_text(c, px + 24, py + 36, COL_TEXT, 1.1f, tbuf);
	}

	const int ra = py + TITLE_H, rb = ra + view, scr = (int)floorf(s_mscr + 0.5f);
	ui_clip_y(c, ra, rb);
	s_focus_cy0 = ra;
	s_focus_cy1 = rb;
	for (int i = 0; i < n; i++) {
		const int g = s_page == PAGE_MAIN && !s_list_mode ? gap_at[i] : 0;
		const int ty = ra + i * lh + g - scr + lh / 2 + 7;
		if (ty + lh < ra || ty - lh > rb)
			continue;
		switch (s_page) {
		case PAGE_MAIN: {
			static const char *const names[M_COUNT] = { "Resume game", "Reset game", "Test mode (DIP switch)", "Save / Load state  >",
				"Display  >", "Controls  >", "Save game settings", "Delete game settings", "About  >", "Select game", "Exit", "Back",
				"Refresh ROM list" };
			const int k = main_kind(i);
			const char *name = names[k];
			char rbuf[64];
			if (k == M_RESET && s_reset_armed) {
				snprintf(rbuf, sizeof(rbuf), "Reset game - press %s again", o_ok ? "Circle" : "Cross");
				name = rbuf;
			}
			item(c, px, ty, i, s_sel, name, k == M_SERVICE ? (rs12_dip_service ? VAL_ON : VAL_OFF) : NULL);
			break;
		}
		case PAGE_DISPLAY: {
			static const char *const names[D_COUNT] = { "Show FPS", "Screen", "Rotate", "Filter", "Border (4:3 / Integer)",
				"Border color", "Latency", "Theme", "Back" };
			const char *v = NULL;
			switch (i) {
			case D_FPS: v = g_set.fps ? VAL_ON : VAL_OFF; break;
			case D_ASPECT: v = aspect[g_set.aspect]; break;
			case D_ROTATE: v = rotate[g_set.rotate]; break;
			case D_FILTER: v = filter[g_set.filter]; break;
			case D_BORDER: v = border[g_set.border]; break;
			case D_BORDER_COL: v = border_col[g_set.border_color]; break;
			case D_LATENCY: v = s_latency_name[g_set.latency]; break;
			case D_THEME: v = s_theme_name[g_set.theme]; break;
			}
			item(c, px, ty, i, s_sel, names[i], v);
			break;
		}
		case PAGE_CONTROLS:
		case PAGE_TURBO:
		case PAGE_ANALOG:
		case PAGE_TOUCH: {
			int act, rk, rf;
			const int k = row_kind(i, &act);
			const int clash = row_key(k, act, &rk, &rf) && key_clash(rk, rf) != NULL;
			if (k == CK_ACT)
				item_key_w(c, px, ty, i, s_sel, act_name(act), g_set.keymap[act], clash);
			else if (k == CK_TURBO_KEY)
				item_key_w(c, px, ty, i, s_sel, "Turbo key", g_set.turbo_key, clash);
			else if (k == CK_TURBO)
				item(c, px, ty, i, s_sel, "Turbo  >", NULL);
			else if (k == CK_TOUCH)
				item(c, px, ty, i, s_sel, "Touch controls  >", NULL);
			else if (k == CK_TOUCH_GUN)
				item(c, px, ty, i, s_sel, "Touch gun", g_set.touch_gun ? VAL_ON : VAL_OFF);
			else if (k == CK_T_ON)
				item(c, px, ty, i, s_sel, "Touch buttons", g_set.touch_keys ? VAL_ON : VAL_OFF);
			else if (k == CK_T_SHOW)
				item(c, px, ty, i, s_sel, "Show touch buttons", g_set.touch_show ? VAL_ON : VAL_OFF);
			else if (k == CK_T_EDIT) {
				char eb[48];
				snprintf(eb, sizeof(eb), "Edit touch layout  >   (%d)", rs12_touch_count());
				item(c, px, ty, i, s_sel, eb, NULL);
			} else if (k == CK_T_CLEAR)
				item(c, px, ty, i, s_sel, s_reset_armed && i == s_sel ? "Clear touch layout - press again" : "Clear touch layout", NULL);
			else if (k == CK_ANALOG) {
				const int a = menu_analog();
				item(c, px, ty, i, s_sel, a == S12_ANALOG_GUN ? "Gun settings  >" : a == S12_ANALOG_WHEEL ? "Wheel settings  >"
					: "Lever settings  >", NULL);
			} else if (k == CK_TURBO_BTN) {
				char bbuf[16];
				snprintf(bbuf, sizeof(bbuf), "Button %d", g_set.turbo_btn);
				item(c, px, ty, i, s_sel, "Turbo button", bbuf);
			} else if (k == CK_TURBO_SPEED)
				item(c, px, ty, i, s_sel, "Turbo speed", turbo_speed[g_set.turbo_speed]);
			else if (k == CK_STICK)
				item(c, px, ty, i, s_sel, "Analog stick", stick[g_set.stick]);
			else if (k == CK_AIM_STICK)
				item(c, px, ty, i, s_sel, "Gun stick", g_set.aim_stick == RS12_AIM_RIGHT ? "Right" : "Left");
			else if (k == CK_AIM_MODE)
				item(c, px, ty, i, s_sel, "Gun aim", g_set.aim_mode == RS12_AIM_MOVE ? "Stick moves it" : "Stick position");
			else if (k == CK_CROSSHAIR)
				item(c, px, ty, i, s_sel, "Gun crosshair", g_set.crosshair ? VAL_ON : VAL_OFF);
			else if (k == CK_GUN_SENS) {
				char gbuf[16];
				snprintf(gbuf, sizeof(gbuf), "%d", g_set.gun_sens);
				item(c, px, ty, i, s_sel, "Gun sensitivity", gbuf);
			} else if (k == CK_WHEEL_STICK)
				item(c, px, ty, i, s_sel, "Wheel (+ d-pad)", g_set.wheel_stick == RS12_AIM_RIGHT ? "Right stick" : "Left stick");
			else if (k == CK_LEVER_STICK)
				item(c, px, ty, i, s_sel, "Lever (+ d-pad)", g_set.lever_stick == RS12_AIM_RIGHT ? "Right stick" : "Left stick");
			else if (k == CK_GUN_FIRE)
				item_key_w(c, px, ty, i, s_sel, "Gun trigger", g_set.gun_fire, clash);
			else if (k == CK_GUN_SENSOR)
				item_key_w(c, px, ty, i, s_sel, "Gun scope sensor", g_set.gun_sensor, clash);
			else if (k == CK_GAS)
				item_key_w(c, px, ty, i, s_sel, "Gas pedal", g_set.gas_key, clash);
			else if (k == CK_BRAKE)
				item_key_w(c, px, ty, i, s_sel, "Brake pedal", g_set.brake_key, clash);
			else if (k == CK_CONFIRM)
				item_key(c, px, ty, i, s_sel, "Menu confirm button", o_ok ? RS12_KEY_CIRCLE : RS12_KEY_CROSS);
			else if (k == CK_COMBO)
				item(c, px, ty, i, s_sel, "Combo keys  >", NULL);
			else
				item(c, px, ty, i, s_sel, k == CK_DEFAULTS ? "Restore defaults" : "Back", NULL);
			break;
		}
		case PAGE_COMBO:
			if (i == COMBO_ROWS - 1) {
				item(c, px, ty, i, s_sel, "Back", NULL);
			} else {
				// row: the key itself (glyph), value: < its combo >
				const int key = s_combo_order[i];
				const uint32_t col = (i == s_sel) ? COL_SEL : COL_TEXT;
				item(c, px, ty, i, s_sel, "", NULL);
				draw_key(c, px + 40, ty - 7, ty, key, i == s_sel ? COL_TEXT : COL_ITEM);
				int x = px + VALUE_X;
				x += ui_text(c, x, ty, col, 1.0f, "< ");
				x += draw_combo(c, x, ty, g_set.combo[key], col);
				ui_text(c, x + 6, ty, col, 1.0f, ">");
			}
			break;
		case PAGE_STATES:
			if (i == S_SLOT) {
				char sbuf[64];
				const int k = s_state_slot;
				if (s_slot_used[k]) { // file times are UTC
					SceDateTime lt;
					SceRtcTick tick;
					sceRtcGetTick(&s_slot_time[k], &tick);
					sceRtcConvertUtcToLocalTime(&tick, &tick);
					sceRtcSetTick(&lt, &tick);
					snprintf(sbuf, sizeof(sbuf), "%d   used  %02d-%02d %02d:%02d", k, lt.month, lt.day, lt.hour, lt.minute);
				} else
					snprintf(sbuf, sizeof(sbuf), "%d   empty", k);
				item(c, px, ty, i, s_sel, "Slot", sbuf);
			} else {
				static const char *const names[S_COUNT] = { "", "Save state", "Load state", "Back" };
				item(c, px, ty, i, s_sel, names[i], NULL);
			}
			break;
		case PAGE_ABOUT: {
			const int k = s_about_scroll + i;
			if (k < ABOUT_LINES)
				about_line(c, px + 24, ty, s_about[k]);
			break;
		}
		}
	}
	if (content > view) { // fades where rows are cut (towards the panel colour)
		enum { FADE = 14 };
		for (int k = 0; k < FADE; k++) {
			const unsigned al = (unsigned)(0xE0 - k * 0xE0 / FADE);
			if (scr > 0)
				ui_rect(c, px + 2, ra + k, PANEL_W - 22, 1, UI_RGBA(theme()->panel_rgb[0], theme()->panel_rgb[1], theme()->panel_rgb[2], al));
			if (scr < content - view)
				ui_rect(c, px + 2, rb - 1 - k, PANEL_W - 22, 1, UI_RGBA(theme()->panel_rgb[0], theme()->panel_rgb[1], theme()->panel_rgb[2], al));
		}
	}
	ui_clip_y(c, 0, SCREEN_H);
	s_focus_cy0 = 0;
	s_focus_cy1 = SCREEN_H;
	if (s_page != PAGE_ABOUT) { // scroll bar (display only)
		const int tx = px + PANEL_W - 16, t0 = ra + 4, th = view - 8;
		int bh = content > 0 ? th * view / content : th, by = content > 0 ? t0 + (int)(th * s_mscr / content) : t0;
		if (bh < 16) bh = 16;
		if (by + bh > t0 + th) by = t0 + th - bh;
		rrect(c, tx, t0, 4, th, 2, UI_RGBA(0xFF, 0xFF, 0xFF, 0x18));
		rrect(c, tx, by, 4, bh, 2, COL_THUMB);
	}
	text_keys(c, px + 24, py + h - 18, COL_DIM, foot);
	if (s_pop) { // choice window over the panel
		const char *names[MAX_CHOICES], *ptitle;
		int cur;
		const int nc = item_choices(names, &cur, &ptitle);
		if (nc) {
			// rows of LINE_H; many values (the combo pairs) get shorter rows so the window fits the screen
			const int plh = 64 + nc * LINE_H + 12 <= SCREEN_H - 24 ? LINE_H : (SCREEN_H - 24 - 76) / nc;
			const int ww = 380, wh = 64 + nc * plh + 12, wx = (SCREEN_W - ww) / 2, wy = (SCREEN_H - wh) / 2;
			if (wy - 8 < s_paint_y0) s_paint_y0 = wy - 8;
			if (wy + wh + 16 > s_paint_y1) s_paint_y1 = wy + wh + 16;
			ui_rect(c, px, py, PANEL_W, h, UI_RGBA(0x00, 0x00, 0x00, 0x80)); // dim the panel under it
			window_box(c, wx, wy, ww, wh, COL_SEL, COL_PANEL);
			const int tw = ui_text(c, wx + 24, wy + 38, COL_TEXT, 1.0f, ptitle);
			if (s_choice_kind == CH_COMBO) // "Combo for" the key
				draw_key(c, wx + 24 + tw + 8, wy + 31, wy + 38, s_combo_order[s_sel], COL_TEXT);
			ui_rect(c, wx + 20, wy + 50, ww - 40, 1, COL_DIM);
			for (int i = 0; i < nc; i++) {
				const int ty = wy + 60 + i * plh + plh / 2 + 7, by = ty - 7 - (plh - 6) / 2;
				const uint32_t col = i == s_pop_sel ? COL_SEL : COL_TEXT;
				if (i == s_pop_sel) {
					rrect(c, wx + 12, by + 3, ww - 24, plh - 6, 6, theme()->shade);
					rrect(c, wx + 12, by, ww - 24, plh - 6, 6, COL_HILITE);
					focus_at(c, wx + 12, by, ww - 24, plh - 6);
				}
				if (s_choice_kind == CH_KEYS) // button icons as in the item, not their names
					draw_key(c, wx + 44, ty - 7, ty, i, col);
				else if (s_choice_kind == CH_COMBO)
					draw_combo(c, wx + 44, ty, combo_choice(i), col);
				else
					ui_text(c, wx + 44, ty, col, 1.0f, names[i]);
				if (i == cur)
					ui_circle(c, (float)(wx + 28), (float)(ty - 7), 5.0f, col);
			}
		}
	}
}

// a touch button's key: the PlayStation symbols as icons, the others as names, centred on (cx, cy)
static void tkey_label(UiCanvas *c, int cx, int cy, int key, uint32_t col)
{
	if (key >= RS12_KEY_CROSS && key <= RS12_KEY_TRIANGLE)
		draw_key(c, cx - 10, cy, cy + 7, key, col);
	else {
		const char *nm = rs12_touch_key_name(key);
		ui_text(c, cx - ui_text_width(1.0f, nm) / 2, cy + 7, col, 1.0f, nm);
	}
}
// one touch button: a translucent rounded box with its key (sel: the selected one in the editor)
static void tarea(UiCanvas *c, const Rs12TouchArea *a, int sel, int edit)
{
	const int r = a->w < a->h ? a->w / 5 : a->h / 5;
	rrect(c, a->x, a->y, a->w, a->h, r, sel ? COL_HILITE : UI_RGBA(0xFF, 0xFF, 0xFF, edit ? 0x38 : 0x24));
	rrect_outline(c, a->x, a->y, a->w, a->h, r, 2, sel ? COL_SEL : UI_RGBA(0xFF, 0xFF, 0xFF, edit ? 0xB0 : 0x70));
	tkey_label(c, a->x + a->w / 2, a->y + a->h / 2, a->key, UI_RGBA(0xFF, 0xFF, 0xFF, edit ? 0xFF : 0xC0));
	if (edit) { // size handles: every corner, and the middle of every edge of the selected one
		const uint32_t hc = sel ? COL_SEL : UI_RGBA(0xFF, 0xFF, 0xFF, 0xB0);
		const int hs = sel ? 12 : 8;
		const int cx[3] = { a->x, a->x + a->w / 2, a->x + a->w }, cy[3] = { a->y, a->y + a->h / 2, a->y + a->h };
		for (int j = 0; j < 3; j++)
			for (int i = 0; i < 3; i++)
				if ((i != 1 || j != 1) && (sel || (i != 1 && j != 1)))
					ui_rect(c, cx[i] - hs / 2, cy[j] - hs / 2, hs, hs, hc);
		if (sel) { // its size (in TE_STEP steps)
			char sz[16];
			snprintf(sz, sizeof(sz), "%dx%d", a->w, a->h);
			ui_text(c, a->x + 4, a->y + a->h - 6, hc, 0.6f, sz);
		}
	}
}
static void tedit_paint(UiCanvas *c)
{
	// the whole screen: the dimmed list (game list options) or the game behind a light shade
	if (s_under)
		ui_blit(c, s_under);
	else
		ui_fill(c, 0, 0, SCREEN_W, SCREEN_H, UI_RGBA(0x00, 0x00, 0x00, 0x70));
	s_paint_x0 = s_paint_y0 = 0;
	s_paint_x1 = SCREEN_W;
	s_paint_y1 = SCREEN_H;
	s_drawn_h = 0;
	focus_off();
	for (int i = 0; i < rs12_touch_count(); i++)
		tarea(c, rs12_touch_area(i), i == te_sel, 1);
	if (te_mode == TE_NEW && te_nw > 2 && te_nh > 2)
		rrect_outline(c, te_nx, te_ny, te_nw, te_nh, 6, 2, te_nw >= TE_MIN && te_nh >= TE_MIN ? COL_SEL : COL_DIM);
	// header with Done, footer with the help
	ui_rect(c, 0, 0, SCREEN_W, 46, UI_RGBA(0x00, 0x00, 0x00, 0xA8));
	char t[64];
	snprintf(t, sizeof(t), "TOUCH LAYOUT   (%d button%s)", rs12_touch_count(), rs12_touch_count() == 1 ? "" : "s");
	ui_text(c, 20, 31, COL_TEXT, 1.0f, t);
	rrect(c, SCREEN_W - 120, 7, 108, 32, 6, COL_SEL);
	ui_text(c, SCREEN_W - 120 + (108 - ui_text_width(0.9f, "Done")) / 2, 29, COL_PANEL, 0.9f, "Done");
	ui_rect(c, 0, SCREEN_H - 52, SCREEN_W, 52, UI_RGBA(0x00, 0x00, 0x00, 0xA8));
	text_keys(c, 20, SCREEN_H - 30, COL_DIM, "Drag on an empty spot: new button   Tap a button: its key or Delete   Drag it: move   Drag an edge or corner: size (6px steps)");
	text_keys(c, 20, SCREEN_H - 10, COL_DIM, G_SQU ": new button   " G_TRI ": key of the selected one   START: done");
	if (te_pop >= 0) {
		int wx, wy, ww, wh;
		te_pop_rect(&wx, &wy, &ww, &wh);
		const Rs12TouchArea *a = rs12_touch_area(te_pop);
		window_box(c, wx, wy, ww, wh, COL_SEL, COL_PANEL);
		ui_text(c, wx + 24, wy + 38, COL_TEXT, 1.0f, "Button key");
		ui_rect(c, wx + 20, wy + 50, ww - 40, 1, COL_DIM);
		for (int i = 0; i < TE_POP_N; i++) {
			const int ty = wy + 60 + i * TE_POP_RH + TE_POP_RH / 2 + 7, by = ty - 7 - (TE_POP_RH - 6) / 2;
			const uint32_t col = i == te_pop_sel ? COL_SEL : COL_TEXT;
			if (i == te_pop_sel) {
				rrect(c, wx + 12, by, ww - 24, TE_POP_RH - 6, 6, COL_HILITE);
				rrect_outline(c, wx + 12, by, ww - 24, TE_POP_RH - 6, 6, 2, COL_FOCUS_STILL);
			}
			if (i == RS12_TKEY_COUNT)
				ui_text(c, wx + 44, ty, COL_WARN, 1.0f, "Delete");
			else if (i <= RS12_KEY_TRIANGLE)
				draw_key(c, wx + 44, ty - 7, ty, i, col);
			else
				ui_text(c, wx + 44, ty, col, 1.0f, rs12_touch_key_name(i));
			if (a && i == a->key)
				ui_circle(c, (float)(wx + 28), (float)(ty - 7), 5.0f, col);
		}
	}
}

// ---- touch buttons over the game (slot 4, under the other layers): drawn again only when the layout changes ----
static UiCanvas *s_touch_cv;
static int s_touch_drawn = -1, s_touch_vis;
int rs12_touch_gun_on(void)
{
	return g_set.touch_gun && s12_analog_kind(menu_game()) == S12_ANALOG_GUN;
}
void rs12_touch_overlay_update(int hidden)
{
	const int want = !hidden && g_set.touch_keys && g_set.touch_show && rs12_touch_count() > 0;
	if (!want) {
		if (s_touch_vis)
			ui_hide(s_touch_cv);
		s_touch_vis = 0;
		return;
	}
	if (s_touch_vis && s_touch_drawn == rs12_touch_gen())
		return;
	if (!s_touch_cv)
		s_touch_cv = ui_canvas(4, 0, 0, SCREEN_W, SCREEN_H);
	if (!s_touch_cv)
		return;
	ui_clear(s_touch_cv);
	for (int i = 0; i < rs12_touch_count(); i++)
		tarea(s_touch_cv, rs12_touch_area(i), 0, 0);
	ui_commit(s_touch_cv);
	s_touch_drawn = rs12_touch_gen();
	s_touch_vis = 1;
}

void rs12_menu_draw(void)
{
	focus_tick(); // (the blink: the focus layer's opacity only)
	if (menu_anim())
		s_dirty = 1;
	if (!s_dirty || !s_cv)
		return;
	s_dirty = 0;
	focus_begin();
	menu_paint(s_cv, 0);
	focus_end();
	ui_commit(s_cv);
}

#define TEST_BG UI_RGBA(0x50, 0x50, 0x58, 0xFF)

void rs12_menu_selftest(void)
{
	ensure_canvases();
	static const struct { int page, sel, pop; const char *name; } shots[] = {
		{ PAGE_MAIN, 0, 0, "main" }, { PAGE_STATES, 0, 0, "states" }, { PAGE_DISPLAY, 1, 0, "display" },
		{ PAGE_DISPLAY, 1, 1, "display_pop" }, { PAGE_CONTROLS, 6, 0, "controls" }, { PAGE_CONTROLS, 13, 0, "controls_end" },
		{ PAGE_TURBO, 0, 1, "turbo_pop" }, { PAGE_COMBO, 4, 0, "combo" }, { PAGE_COMBO, 4, 1, "combo_list" },
		{ PAGE_ABOUT, 0, 0, "about" },
	};
	char p[96];
	// states page: the first used slot (if any)
	rs12_menu_states_changed();
	const int slot0 = s_state_slot;
	for (int k = 0; k < STATE_SLOTS; k++)
		if (s_slot_used[k]) {
			s_state_slot = k;
			break;
		}
	// sample combo on L (Cross + Circle) for the combo images; the settings are put back below
	const int combo_l = g_set.combo[RS12_KEY_L];
	g_set.combo[RS12_KEY_L] = (1 << RS12_KEY_CROSS) | (1 << RS12_KEY_CIRCLE);
	for (unsigned k = 0; k < sizeof(shots) / sizeof(shots[0]); k++) {
		s_page = shots[k].page;
		s_sel = shots[k].sel < page_items() ? shots[k].sel : page_items() - 1;
		s_pop = shots[k].pop;
		if (s_pop) {
			const char *names[MAX_CHOICES], *title;
			int cur;
			item_choices(names, &cur, &title);
			s_pop_sel = cur;
		}
		s_drawn_h = 0;
		for (int f = 0; f < 30; f++) { // (until the rows have scrolled into place)
			s_dirty = 1;
			rs12_menu_draw();
		}
		snprintf(p, sizeof(p), DATA_DIR "/menu_%s.ppm", shots[k].name);
		ui_save_ppm(s_cv, p, TEST_BG);
		snprintf(p, sizeof(p), DATA_DIR "/screen_%s.ppm", shots[k].name);
		rs12_video_capture_ppm(p);
	}
	s_pop = 0;
	s_state_slot = slot0;
	g_set.combo[RS12_KEY_L] = combo_l;
	rs12_menu_hide();
	rs12_osd_show(TEST_NOTICE_MSG);
	ui_save_ppm(s_osd_cv, DATA_DIR "/menu_osd.ppm", TEST_BG);
	rs12_fps_update(59.9f, 1234);
	ui_save_ppm(s_fps_cv, DATA_DIR "/menu_fps.ppm", TEST_BG);
	rs12_video_capture_ppm(DATA_DIR "/screen_game.ppm"); // game + FPS box + notice
	// screen modes (restored afterwards; the settings file is not touched)
	rs12_video_set_mode(2, 1);
	rs12_video_capture_ppm(DATA_DIR "/screen_full.ppm");
	rs12_video_set_mode(1, 0);
	rs12_video_set_border(RS12_BORDER_COLOR, g_set.border_color);
	rs12_video_capture_ppm(DATA_DIR "/screen_int_border.ppm");
	rs12_settings_apply_video();
	s_page = PAGE_MAIN;
	s_sel = 0;
	s_dirty = 1;
}

void rs12_menu_hide(void)
{
	focus_off();
	if (s_cv)
		ui_hide(s_cv);
	s_dirty = 1;
	s_drawn_h = 0;
}

// ---- loading screen: the app's background art, the game name, the stage with its percentage, a spinner and a moving
// bar (same look as ReST-V) ----
static char s_loading_title[96];
static uint64_t s_loading_us; // time spent drawing (logged at the end)
static uint32_t s_loading_gap_max; // longest time between two shown frames
static int s_loading_gaps, s_loading_gap_at; // frames over 40 ms apart; progress at the longest
static uint32_t s_loading_gap_draw; // draw + present time of that frame
static uint32_t s_loading_ps_max, s_loading_present_max; // longest PS button poll, longest present
static int s_loading_draws;
// The background and the game name are drawn once (kept in s_load_base); each later draw puts back and redraws only the
// band with the stage, the spinner and the bar, so only that band is uploaded.
// The ROM load runs on its own thread and only sets the value (rs12_loading_set); the main thread draws every frame
// (rs12_loading_frame), so the spinner and the bar move at 60 fps whatever the loader is doing (owner: the screen
// stuttered when the loader drew between its steps)
static uint32_t *s_load_base;
static int s_load_base_ok;
void rs12_loading_title(const char *title)
{
	snprintf(s_loading_title, sizeof(s_loading_title), "%s", title ? title : "");
	s_load_base_ok = 0;
}

static volatile int s_loading_target; // set by any thread
static int s_loading_shown;           // drawn value, eases toward the target
void rs12_loading_set(int permille)
{
	if (permille < 0) permille = 0;
	if (permille > 1000) permille = 1000;
	__atomic_store_n(&s_loading_target, permille, __ATOMIC_RELEASE);
}
void rs12_loading_progress(int permille)
{
	rs12_loading_set(permille);
	s_loading_shown = s_loading_target; // (jump: called at the start and the end)
	rs12_loading_frame();
}
void rs12_loading_frame(void)
{
	const uint64_t t_ps = rs12_time_us();
	ps_poll(); // (loading: a single PS press does nothing, a double press still goes home)
	const uint64_t now = rs12_time_us();
	if (now - t_ps > s_loading_ps_max)
		s_loading_ps_max = (uint32_t)(now - t_ps);
	const int target = __atomic_load_n(&s_loading_target, __ATOMIC_ACQUIRE);
	if (s_loading_shown < target)
		s_loading_shown += (target - s_loading_shown + 3) / 4; // (the bar glides instead of jumping between steps)
	else
		s_loading_shown = target;
	const int permille = s_loading_shown;
	s_loading_draws++;
	ensure_canvases();
	UiCanvas *c = s_cv;
	// bar at the bottom with a small margin left, right and below; the title above it
	const int m = 36, h = 14, w = SCREEN_W - 2 * m, x = m, y = SCREEN_H - m - h;
	const int sb = y - 50, band = sb - 24; // stage text baseline; first row of the redrawn band
	s_drawn_h = 0;
	if (!s_load_base_ok) {
		ui_clear(c);
		// the app's background art 20% darker (80% brightness), loaded once
		static uint8_t *bg;
		static int bg_w, bg_h, bg_tried;
		if (!bg_tried) {
			bg_tried = 1;
			bg = rs12_video_load_bg(&bg_w, &bg_h);
		}
		if (bg)
			ui_image(c, bg, bg_w, bg_h, 205);
		else
			ui_rect(c, 0, 0, SCREEN_W, SCREEN_H, UI_RGBA(0, 0, 0, 0xFF));
		const char *title = s_loading_title[0] ? s_loading_title : "Loading...";
		const int ttw = ui_text_width(0.9f, title), tx = ttw < w ? (SCREEN_W - ttw) / 2 : x;
		ui_text_clip(c, tx, sb - 18 - 20, COL_DIM, 0.9f, title, x, x + w);
		if (!s_load_base)
			s_load_base = (uint32_t *)malloc((size_t)SCREEN_W * SCREEN_H * 4);
		if (s_load_base) {
			memcpy(s_load_base, ui_pixels(c), (size_t)SCREEN_W * SCREEN_H * 4);
			s_load_base_ok = 1;
		}
	} else
		ui_blit_rect(c, s_load_base, 0, band, SCREEN_W, SCREEN_H - band);
	// capsule bar with a vertical gradient and gloss, stripes moving inside the filled part, a glowing head, a spinner and
	// the stage with its percentage
	const float r = h * 0.5f;
	const int fw = w * permille / 1000;
	ui_rect(c, x - 3, y - 3, w + 6, h + 6, UI_RGBA(0x00, 0x00, 0x00, 0x70)); // shade under the bar
	for (int yy = 0; yy < h; yy++) { // track and fill row by row (rounded ends)
		const float dy = yy + 0.5f - r, in = r - sqrtf(r * r - dy * dy);
		const int t0 = x + (int)in, t1 = x + w - (int)in;
		ui_rect(c, t0, y + yy, t1 - t0, 1, theme()->bar_track);
		if (fw > 2) {
			const int f1 = x + fw - (fw > 2 * r ? (int)in : 0);
			const int g = yy * 255 / (h - 1);
			const uint32_t ca = theme()->bar_top, cb = theme()->bar_bot;
			const uint32_t col = UI_RGBA((int)(ca & 0xFF) + (((int)(cb & 0xFF) - (int)(ca & 0xFF)) * g >> 8),
				(int)((ca >> 8) & 0xFF) + (((int)((cb >> 8) & 0xFF) - (int)((ca >> 8) & 0xFF)) * g >> 8),
				(int)((ca >> 16) & 0xFF) + (((int)((cb >> 16) & 0xFF) - (int)((ca >> 16) & 0xFF)) * g >> 8), 0xFF);
			if (f1 > t0)
				ui_rect(c, t0, y + yy, f1 - t0, 1, col);
			if (yy < h / 3 && f1 > t0) // gloss
				ui_rect(c, t0, y + yy, f1 - t0, 1, UI_RGBA(0xFF, 0xFF, 0xFF, 0x38));
		}
	}
	if (fw > 2 * r) { // stripes drifting to the right
		const int step = 22, off = (int)((now / 25000) % step);
		for (int sx = -h + off; sx < fw; sx += step) {
			const float x0 = (float)(x + sx), x1 = x0 + h - 4;
			if (x0 >= x + r && x1 <= x + fw - r)
				ui_line(c, x0, (float)(y + h - 2), x1, (float)(y + 2), 6.0f, UI_RGBA(0xFF, 0xFF, 0xFF, 0x30));
		}
	}
	if (fw > 0) { // glowing head
		const float hx = (float)(x + fw), hy = y + r;
		const float pulse = 0.5f + 0.5f * sinf((float)(now % 1000000) * 6.2832f / 1e6f);
		ui_circle(c, hx, hy, r + 7 + 2 * pulse, theme()->glow1);
		ui_circle(c, hx, hy, r + 3, theme()->glow2);
		ui_circle(c, hx, hy, r - 1, theme()->glow3);
	}
	// centred text with room around everything. From the bottom: the bar, 50 px, the stage with its percentage and the
	// loading icon after it (26 px apart, centred as one group), 18 px, the game name (centred, clipped to the bar)
	const char *stg = permille < 950 ? "Loading ROMs" : "Starting";
	char stage[80];
	snprintf(stage, sizeof(stage), "%s   %d%%", stg, permille / 10);
	const float icon_r = 11.0f; // dots on this circle, 3 px each: the icon is 28 px wide
	const int tw = ui_text_width(0.9f, stage), gap = 26, gw = tw + gap + 28, gx0 = (SCREEN_W - gw) / 2;
	ui_text(c, gx0, sb, COL_DIM, 0.9f, stage);
	const float scx = (float)(gx0 + tw + gap + 14), scy = (float)(sb - 5);
	const int ph = (int)((now / 70000) % 12);
	for (int i = 0; i < 12; i++) {
		const float a = i * 6.2832f / 12.0f;
		const int age = (ph - i + 12) % 12, al = 0xFF - age * 0x14;
		ui_circle(c, scx + icon_r * sinf(a), scy - icon_r * cosf(a), 3.0f, (theme()->spinner & 0xFFFFFFu) | ((uint32_t)(al < 0x20 ? 0x20 : al) << 24));
	}
	ui_commit(c);
	const uint64_t t_pr = rs12_time_us();
	s_loading_us += t_pr - now;
	rs12_video_present();
	static uint64_t last_end;
	const uint64_t end = rs12_time_us();
	if (end - t_pr > s_loading_present_max)
		s_loading_present_max = (uint32_t)(end - t_pr);
	if (s_loading_draws > 1 && end - last_end > 40000) {
		s_loading_gaps++;
		if (end - last_end > s_loading_gap_max) {
			s_loading_gap_max = (uint32_t)(end - last_end);
			s_loading_gap_at = permille;
			s_loading_gap_draw = (uint32_t)(end - now);
		}
	}
	last_end = end;
	static int saved;
	if (saved < 2 && permille >= (saved ? 900 : 300)) { // test: two loading screen images while menutest is armed
		SceIoStat st;
		if (sceIoGetstat(DATA_DIR "/menutest", &st) >= 0)
			ui_save_ppm(c, saved ? DATA_DIR "/menu_loading2.ppm" : DATA_DIR "/menu_loading.ppm", TEST_BG);
		saved++;
	}
}

void rs12_loading_done(void)
{
	RS12_LOG("loading screen: %d draws, %llu ms drawing, %d gaps over 40 ms, longest %u us at %d (its draw %u us), "
		"PS poll max %u us, present max %u us", s_loading_draws, (unsigned long long)(s_loading_us / 1000), s_loading_gaps,
		(unsigned)s_loading_gap_max, s_loading_gap_at, (unsigned)s_loading_gap_draw, (unsigned)s_loading_ps_max,
		(unsigned)s_loading_present_max);
	s_loading_ps_max = s_loading_present_max = 0;
	s_loading_us = 0;
	s_loading_gap_max = 0;
	s_loading_gaps = 0;
	s_loading_draws = 0;
	s_load_base_ok = 0;
	s_loading_shown = s_loading_target = 0;
	if (s_cv)
		ui_hide(s_cv);
}

// ---- notice (bottom center, 2 s) ----
static uint64_t s_osd_until;
static int s_osd_on;

void rs12_osd_show(const char *fmt, ...)
{
	char msg[128];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(msg, sizeof(msg), fmt, ap);
	va_end(ap);
	RS12_LOG("notice: %s", msg);
	ensure_canvases();
	UiCanvas *c = s_osd_cv;
	const int w = ui_text_width(1.0f, msg) + 40, h = 36;
	const int x = (SCREEN_W - w) / 2;
	ui_clear(c);
	ui_rect(c, x, 0, w, h, UI_RGBA(0x00, 0x00, 0x00, 0xC8));
	ui_rect(c, x, 0, w, 2, COL_SEL);
	ui_text(c, x + 20, 26, COL_TEXT, 1.0f, msg);
	ui_canvas_move(c, 0, SCREEN_H - h - 16);
	ui_commit(c);
	s_osd_until = rs12_time_us() + 2000000;
	s_osd_on = 1;
}

void rs12_osd_tick(void)
{
	if (s_osd_on && rs12_time_us() > s_osd_until) {
		ui_hide(s_osd_cv);
		s_osd_on = 0;
	}
}

// ---- FPS (top right box; debug builds with the frame number) ----
static int s_fps_shown;
// hidden while the menu is open (it covered Done of the touch layout editor); rs12_fps_update draws it again
void rs12_fps_hide(void)
{
	if (s_fps_shown && s_fps_cv)
		ui_hide(s_fps_cv);
	s_fps_shown = 0;
}

void rs12_fps_update(float fps, unsigned frame)
{
	static unsigned last;
	int shown = s_fps_shown;
	if (!g_set.fps) {
		rs12_fps_hide();
		return;
	}
	if (shown && frame - last < 6)
		return;
	last = frame;
	s_fps_shown = 1;
	ensure_canvases();
	UiCanvas *c = s_fps_cv;
	char buf[48];
#ifdef RS12_DEBUG_TOOLS
	snprintf(buf, sizeof(buf), "%.1f FPS  F%u", (double)fps, frame);
#else
	snprintf(buf, sizeof(buf), "%d FPS", (int)(fps + 0.5f)); // release builds round (59.8 -> 60), no frame number
#endif
	int w = ui_text_width(1.0f, buf) + 16;
	if (w > 240)
		w = 240;
	const uint32_t col = fps >= 59.0f ? UI_RGBA(0x60, 0xFF, 0x60, 0xFF) : UI_RGBA(0xFF, 0xC0, 0x40, 0xFF);
	ui_clear(c);
	ui_rect(c, 240 - w, 0, w, 30, UI_RGBA(0x00, 0x00, 0x00, 0xA0));
	ui_text(c, 240 - w + 8, 22, col, 1.0f, buf);
	// in the game picture's top-right corner (the whole screen before the first frame); held upright (Vertical L / R)
	// the reader's top-right corner, turned with the picture (the menu stays as it is)
	int px, py, pw, ph;
	if (!rs12_video_picture_rect(&px, &py, &pw, &ph)) {
		px = py = 0;
		pw = SCREEN_W;
		ph = SCREEN_H;
	}
	int cx, cy;
	if (rs12_input_rot == 90) {
		rs12_video_layer_rot(3, 1);
		cx = px + pw - 4 - 32;
		cy = py + ph - 4 - 240;
	} else if (rs12_input_rot == 270) {
		rs12_video_layer_rot(3, 3);
		cx = px + 4;
		cy = py + 4;
	} else {
		rs12_video_layer_rot(3, 0);
		cx = px + pw - 4 - 240;
		cy = py + 4;
	}
	cx = cx < 0 ? 0 : cx > SCREEN_W - 32 ? SCREEN_W - 32 : cx;
	cy = cy < 0 ? 0 : cy > SCREEN_H - 32 ? SCREEN_H - 32 : cy;
	ui_canvas_move(c, cx, cy);
	ui_commit(c);
}

// ================================ start screen game select ================================
// Same look and behaviour as ReST-V rstv_game_select: tabs (here: support tier), switched with L/R. SELECT = filter (Playable: games
// with ROMs / All: everything, games without ROMs grey, picking one shows a notice / Favorites). Square = favorite on/off (star),
// START or PS = options (Display, Controls, Refresh ROM list; global settings), Triangle = About. The cursor starts on the last
// game; nothing starts on its own
#define COL_GREY     (theme()->grey)
#define COL_GREY_SEL (theme()->grey_sel)
#define GL_MAX 128
static int s_gl_n;                 // listed games
static const S12Game *s_gl[GL_MAX]; // listed games (table order)
static int s_gl_tier[GL_MAX], s_gl_present[GL_MAX];
static int gl_playable(int i) { return s_gl_present[i]; }
static const char *const s_gl_filter_name[RS12_LIST_FILTER_COUNT] = { "Playable", "All", "Favorites" };

// games visible under the current filter
static int gl_visible(int i)
{
	if (g_set.list_filter == RS12_LIST_ALL)
		return 1;
	if (g_set.list_filter == RS12_LIST_FAVORITES)
		return rs12_fav_is(s_gl[i]->name);
	return gl_playable(i);
}

static int s_gl_tabs[TIER_COUNT];
static int s_gl_ntabs, s_gl_tab, s_gl_nvis;

// Rebuild tabs from games matching the filter and move to the tab/position of want (list index, may be -1). Indices of the current tab go in idx
static int gl_build(int *idx, int want, int *sel)
{
	const int cur = s_gl_tab < s_gl_ntabs ? s_gl_tabs[s_gl_tab] : -1;
	s_gl_ntabs = 0;
	s_gl_nvis = 0;
	int has[TIER_COUNT] = { 0 };
	for (int i = 0; i < s_gl_n; i++)
		if (gl_visible(i)) {
			s_gl_nvis++;
			has[s_gl_tier[i]] = 1;
		}
	for (int t = 0; t < TIER_COUNT; t++)
		if (has[t])
			s_gl_tabs[s_gl_ntabs++] = t;
	s_gl_tab = 0;
	const int tb = want >= 0 && gl_visible(want) ? s_gl_tier[want] : cur;
	for (int t = 0; t < s_gl_ntabs; t++)
		if (s_gl_tabs[t] == tb)
			s_gl_tab = t;
	int n = 0;
	*sel = 0;
	for (int i = 0; i < s_gl_n && s_gl_ntabs; i++) {
		if (!gl_visible(i) || s_gl_tier[i] != s_gl_tabs[s_gl_tab])
			continue;
		if (i == want)
			*sel = n;
		idx[n++] = i;
	}
	return n;
}

// the listed games and which ROM zips are in rom_dir (start, and Refresh ROM list: ROMs copied while the app runs).
// Returns the games that can run
static int gl_scan(const char *rom_dir)
{
	int nplay = 0;
	s_gl_n = 0;
	for (int k = 0; k < s12_game_count() && s_gl_n < GL_MAX; k++) {
		const S12Game *g = s12_game(k);
		if (!game_listed(g))
			continue;
		const int i = s_gl_n++;
		s_gl[i] = g;
		s_gl_tier[i] = game_tier(g);
		char path[256];
		snprintf(path, sizeof(path), "%s/%s.zip", rom_dir, g->name);
		SceIoStat st;
		s_gl_present[i] = sceIoGetstat(path, &st) >= 0;
		nplay += gl_playable(i);
	}
	return nplay;
}

// one row of the game list (title at x + 48): the selected one with a soft shadow below, a bar and the blinking focus
// outline; favourites get a star; off = how far a long title has scrolled.
// selected: 0 no, 1 the focus (blinks), 2 the focus under an open window (steady outline)
static void gl_row(UiCanvas *c, int px, int w, int ty, int g, int selected, int off)
{
	const int okg = gl_playable(g);
	const uint32_t col = selected ? (okg ? COL_TEXT : COL_GREY_SEL) : (okg ? COL_ITEM : COL_GREY);
	if (selected) {
		const int by = ty - 7 - (GL_LINE - 6) / 2; // bar centred on the text
		rrect(c, px + 12, by + 3, w - 24, GL_LINE - 6, 6, theme()->shade); // shadow (below only)
		rrect(c, px + 13, by + 5, w - 26, GL_LINE - 6, 6, (theme()->shade & 0xFFFFFFu) | 0x20000000u);
		rrect(c, px + 12, by, w - 24, GL_LINE - 6, 6, COL_HILITE);
		if (selected == 2)
			rrect_outline(c, px + 12, by, w - 24, GL_LINE - 6, 6, 2, COL_FOCUS_STILL);
		else
			focus_at(c, px + 12, by, w - 24, GL_LINE - 6);
	}
	if (rs12_fav_is(s_gl[g]->name))
		star(c, (float)(px + 36), (float)(ty - 7), 7.0f, COL_STAR);
	ui_text_clip(c, px + 48 - off, ty, col, 1.0f, s_gl[g]->title, px + 48, px + w - 24);
}

// ---- update window over the game list (Update / Close / Skip this version) ----
#define UPD_LINES 400
#define UPD_W 840
#define UPD_H 470
#define UPD_TEXT_W (UPD_W - 64)
#define UPD_VIS 13
static char s_upd_line[UPD_LINES][160];
static int s_upd_n, s_upd_scroll, s_upd_btn;
static void upd_wrap(const char *notes) // the notes as lines of the window's width (word wrap)
{
	s_upd_n = 0;
	const char *p = notes;
	while (*p && s_upd_n < UPD_LINES) {
		const char *e = strchr(p, '\n');
		const size_t len = e ? (size_t)(e - p) : strlen(p);
		char para[1024];
		snprintf(para, sizeof(para), "%.*s", (int)(len < sizeof(para) - 1 ? len : sizeof(para) - 1), p);
		if (!para[0])
			s_upd_line[s_upd_n++][0] = 0;
		for (char *q = para; *q && s_upd_n < UPD_LINES;) {
			char line[160];
			int take = 0, last_sp = -1;
			while (q[take] && take < (int)sizeof(line) - 1) {
				line[take] = q[take];
				line[take + 1] = 0;
				if (q[take] == ' ')
					last_sp = take;
				if (ui_text_width(0.8f, line) > UPD_TEXT_W - (para[0] == '#' ? 0 : 16)) {
					if (last_sp > 0)
						take = last_sp;
					break;
				}
				take++;
			}
			snprintf(s_upd_line[s_upd_n++], sizeof(s_upd_line[0]), "%s%.*s", q == para || para[0] == '#' ? "" : "  ", take, q);
			q += take;
			while (*q == ' ')
				q++;
		}
		p = e ? e + 1 : p + len;
	}
}
static void upd_draw(UiCanvas *c, int o_ok)
{
	const int mx = (SCREEN_W - UPD_W) / 2, my = (SCREEN_H - UPD_H) / 2;
	window_box(c, mx, my, UPD_W, UPD_H, COL_SEL, COL_PANEL);
	char t[96];
	snprintf(t, sizeof(t), "Update available: %s  (this version %s)", rs12_update_version(), RS12_VERSION);
	ui_text(c, mx + 24, my + 40, COL_TEXT, 1.0f, t);
	for (int i = 0; i < UPD_VIS && s_upd_scroll + i < s_upd_n; i++) {
		const char *l = s_upd_line[s_upd_scroll + i];
		const int head = l[0] == '#';
		ui_text(c, mx + 32, my + 80 + i * 24, head ? COL_SEL : COL_TEXT, head ? 0.9f : 0.8f, l + head);
	}
	if (s_upd_n > UPD_VIS) { // scroll bar
		const int th = UPD_VIS * 24, ty = my + 62;
		int bh = th * UPD_VIS / s_upd_n, by = ty + th * s_upd_scroll / s_upd_n;
		if (bh < 16) bh = 16;
		rrect(c, mx + UPD_W - 18, ty, 5, th, 2, UI_RGBA(0xFF, 0xFF, 0xFF, 0x18));
		rrect(c, mx + UPD_W - 18, by, 5, bh, 2, COL_THUMB);
	}
	static const char *const names[3] = { "Update", "Close", "Skip this version" };
	int bx = mx + 24;
	for (int i = 0; i < 3; i++) {
		const int bw = ui_text_width(0.9f, names[i]) + 40, by = my + UPD_H - 74;
		rrect(c, bx, by, bw, 36, 6, i == s_upd_btn ? COL_SEL : UI_RGBA(0xFF, 0xFF, 0xFF, 0x20));
		ui_text(c, bx + 20, by + 25, i == s_upd_btn ? COL_PANEL : COL_TEXT, 0.9f, names[i]);
		bx += bw + 16;
	}
	text_keys(c, mx + 24, my + UPD_H - 16, COL_DIM,
		o_ok ? "Up/Down: notes   Left/Right: choose   " G_CIR ": OK   " G_CRO ": close"
		     : "Up/Down: notes   Left/Right: choose   " G_CRO ": OK   " G_CIR ": close");
}
static void upd_progress(const char *stage, int pct) // (called while the update downloads)
{
	UiCanvas *c = s_cv;
	const int mw = 760, mh = 140, mx = (SCREEN_W - mw) / 2, my = (SCREEN_H - mh) / 2;
	// the screen under the window is kept and put back first (the window's shadow would get darker with every redraw)
	static uint32_t *under;
	if (!under) {
		under = (uint32_t *)malloc((size_t)SCREEN_W * SCREEN_H * 4);
		if (under)
			memcpy(under, ui_pixels(c), (size_t)SCREEN_W * SCREEN_H * 4);
	} else
		ui_blit(c, under);
	window_box(c, mx, my, mw, mh, COL_SEL, COL_PANEL);
	char t[96];
	if (pct == -2)
		snprintf(t, sizeof(t), "%s", stage);
	else if (pct >= 0)
		snprintf(t, sizeof(t), "%s %s  %d%%", stage, rs12_update_version(), pct);
	else
		snprintf(t, sizeof(t), "%s %s ...", stage, rs12_update_version());
	ui_text(c, mx + 24, my + 46, COL_TEXT, pct == -2 ? 0.9f : 1.0f, t);
	if (pct != -2)
		rrect(c, mx + 24, my + 74, mw - 48, 14, 6, UI_RGBA(0xFF, 0xFF, 0xFF, 0x20));
	if (pct > 0)
		rrect(c, mx + 24, my + 74, (mw - 48) * (pct > 100 ? 100 : pct) / 100, 14, 6, COL_SEL);
	ui_text(c, mx + 24, my + mh - 20, COL_DIM, 0.8f, "Do not turn the Vita off");
	ui_commit(c);
	rs12_video_present();
}

const char *rs12_game_select(const char *initial_set, const char *rom_dir, volatile int *quit)
{
	ensure_canvases();
	UiCanvas *c = s_cv;
	int want = -1;
	const int nplay = gl_scan(rom_dir);
	for (int i = 0; i < s_gl_n; i++)
		if (initial_set && !strcmp(initial_set, s_gl[i]->name))
			want = i;
	int auto_all = 0; // the filter went to All only because no game could run
	if (!nplay && g_set.list_filter == RS12_LIST_PLAYABLE) {
		g_set.list_filter = RS12_LIST_ALL; // (show everything if no game can run)
		auto_all = 1;
	}
	if (want >= 0 && gl_playable(want) && !gl_visible(want))
		g_set.list_filter = RS12_LIST_PLAYABLE; // (the cursor goes to the last game, so it must be on screen)
	const int filter0 = g_set.list_filter;
	static int idx[GL_MAX];
	int sel = 0, about = 0, dirty = 1, upd = 0;
	static int upd_offered; // (once per run)
	uint64_t rep = 0;
	float scr = -1, tgt = 0; // list scroll (rows): drawn position easing to the target
	static uint32_t *under; // the dimmed list under the options window
	int under_ok = 0;
	int mq_sel = -1, mq_t = 0, mq_y = 0, mq_px = 0, mq_w = 0, mq_tw = 0; // scrolling title of the selected row (marquee)
	s_gl_ntabs = s_gl_tab = 0;
	int n = gl_build(idx, want, &sel);
	ui_clear(c); // whole canvas the first time (what loading/menu left there)
	char msg[256] = { 0 };
	uint32_t prev = 0xffffffffu; // ignore until released once, so the press that launched the app does not pick right away
	// test (ux0:data/resys12/gstest): first screen -> favorite -> filter -> filter -> options -> Controls -> up to the Turbo
	// row (the page scrolls) -> Turbo page -> turbo key choice window (button icons) -> back out, as menu_gs<k>.ppm
	// (canvas) and screen_gs<k>.ppm (whole screen); simulated presses, a step waits until scrolling has settled
	SceIoStat gst;
	int gstest = sceIoGetstat(DATA_DIR "/gstest", &gst) >= 0 ? 1 : 0, gstep = 0;
	if (gstest)
		sceIoRemove(DATA_DIR "/gstest");
	for (;;) {
		if (quit && *quit) {
			focus_off();
			ui_hide(c);
			return NULL;
		}
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		uint32_t b = pad.buttons;
		int capture = 0;
		if (gstest) {
			const uint32_t gok = g_set.confirm == RS12_CONFIRM_CIRCLE ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS;
			const uint32_t gbk = gok == SCE_CTRL_CIRCLE ? SCE_CTRL_CROSS : SCE_CTRL_CIRCLE;
			enum { GS_N = 19 };
			const uint32_t seq[GS_N] = { 0, SCE_CTRL_SQUARE, SCE_CTRL_SELECT, SCE_CTRL_SELECT, SCE_CTRL_START, SCE_CTRL_DOWN, gok,
				SCE_CTRL_UP, SCE_CTRL_UP, SCE_CTRL_UP, SCE_CTRL_UP, SCE_CTRL_UP, SCE_CTRL_UP, gok, gok, gbk, gbk, gbk, gbk };
			static int gs_rel; // a pass with no button between steps (the options see each press as new)
			if (gstep > 0 && (scr != tgt || menu_anim() || dirty))
				; // (still scrolling or drawing)
			else if (gs_rel) {
				gs_rel = 0;
				prev = 0;
				b = 0;
			} else {
				gs_rel = 1;
				if (gstep > 0) {
					char gp[64];
					snprintf(gp, sizeof(gp), DATA_DIR "/menu_gs%d.ppm", gstep);
					ui_save_ppm(c, gp, UI_RGBA(0, 0, 0, 0xFF));
					capture = gstep;
				}
				static const char *gs_set; // game the test marked favorite (restored at the end)
				if (gstep == 1 && n)
					gs_set = s_gl[idx[sel]]->name;
				if (gstep >= GS_N) {
					gstest = 0;
					if (gs_set)
						rs12_fav_toggle(gs_set);
					if (g_set.list_filter != filter0) { // (the test changed the filter: put the user's back)
						g_set.list_filter = filter0;
						rs12_settings_save();
					}
					n = gl_build(idx, want, &sel);
					scr = -1;
					dirty = 1;
					RS12_LOG("menu: game list test images written");
				} else {
					prev = 0;
					b = seq[gstep];
				}
				gstep++;
			}
		}
		if (pad.ly < 128 - 48) b |= SCE_CTRL_UP;
		if (pad.ly > 128 + 48) b |= SCE_CTRL_DOWN;
		if (pad.lx < 128 - 48) b |= SCE_CTRL_LEFT;
		if (pad.lx > 128 + 48) b |= SCE_CTRL_RIGHT;
		if (!s_list_mode && ps_poll() && !upd) { // the PS button opens the options here too (rs12_menu_update closes them)
			about = 0;
			msg[0] = 0;
			rs12_settings_use_global();
			s_list_mode = 1;
			s_page = PAGE_MAIN;
			s_sel = s_main_sel = 0;
			s_pop = 0;
			s_changed = 0;
			s_prev_buttons = b;
			dirty = 1;
		}
		uint32_t pressed = key_repeat(b, b & ~prev, &rep);
		prev = b;
		if (pressed)
			dirty = 1;
		const int o_ok = g_set.confirm == RS12_CONFIRM_CIRCLE;
		const uint32_t ok = o_ok ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS, back = o_ok ? SCE_CTRL_CROSS : SCE_CTRL_CIRCLE;
		if (!upd && !upd_offered && !s_list_mode && !about && !msg[0] && rs12_update_ready()) {
			upd = upd_offered = 1; // a newer release: Update / Close / Skip this version
			upd_wrap(rs12_update_notes());
			s_upd_scroll = s_upd_btn = 0;
			dirty = 1;
		}
		if (upd) {
			extern int rs12_update_auto;
			static int auto_step;
			if (rs12_update_auto && auto_step++ == 2) { // test: the window drawn, saved, then Update
				ui_save_ppm(c, DATA_DIR "/upd.ppm", UI_RGBA(0, 0, 0, 0xFF));
				s_upd_btn = 0;
				pressed |= ok;
			}
			if ((pressed & SCE_CTRL_UP) && s_upd_scroll > 0) s_upd_scroll--;
			if ((pressed & SCE_CTRL_DOWN) && s_upd_scroll + UPD_VIS < s_upd_n) s_upd_scroll++;
			if (pressed & SCE_CTRL_LEFT) s_upd_btn = wrap(s_upd_btn - 1, 3);
			if (pressed & SCE_CTRL_RIGHT) s_upd_btn = wrap(s_upd_btn + 1, 3);
			if (pressed & back)
				upd = 0;
			else if (pressed & ok) {
				upd = 0;
				if (s_upd_btn == 2)
					rs12_update_skip();
				else if (s_upd_btn == 0) {
					focus_off();
					const char *err = rs12_update_install(upd_progress); // (returns only on failure)
					RS12_LOG("update: failed: %s", err);
					snprintf(msg, sizeof(msg), "Update failed: %s\nThis version stays installed", err);
				}
			}
		} else if (s_list_mode) { // options (Display / Controls / Refresh, global settings. Saved by rs12_menu_update on close)
			const rs12_menu_result mr = rs12_menu_update(b);
			if (s_page == PAGE_TEDIT && s_dirty) { // (the touch layout editor changes without a button press)
				s_dirty = 0;
				dirty = 1;
			}
			if (mr != RS12_MENU_STAY) {
				s_list_mode = 0;
				s_page = PAGE_MAIN;
				ui_clear(c);
			}
			if (mr == RS12_MENU_REFRESH) { // ROMs copied while the app runs
				const char *cur = n ? s_gl[idx[sel]]->name : NULL;
				const int np = gl_scan(rom_dir);
				if (auto_all && np && g_set.list_filter == RS12_LIST_ALL) {
					g_set.list_filter = RS12_LIST_PLAYABLE; // ROMs now there: back to the games that run
					auto_all = 0;
				}
				int w2 = -1;
				for (int i = 0; cur && i < s_gl_n; i++)
					if (!strcmp(cur, s_gl[i]->name))
						w2 = i;
				n = gl_build(idx, w2, &sel);
				scr = -1; // (new list: no scroll animation)
				snprintf(msg, sizeof(msg), "ROM list refreshed: %d game%s ready\nin %s", np, np == 1 ? "" : "s", rom_dir);
				dirty = 1;
			}
		} else if (msg[0]) { // notice box: any button closes it
			if (pressed & (ok | back | SCE_CTRL_START | SCE_CTRL_TRIANGLE))
				msg[0] = 0;
		} else if (about) {
			const int max = ABOUT_LINES > ABOUT_VISIBLE ? ABOUT_LINES - ABOUT_VISIBLE : 0;
			if ((pressed & SCE_CTRL_UP) && s_about_scroll > 0) s_about_scroll--;
			if ((pressed & SCE_CTRL_DOWN) && s_about_scroll < max) s_about_scroll++;
			if (pressed & (ok | back | SCE_CTRL_TRIANGLE | SCE_CTRL_START))
				about = 0;
		} else {
			if (n && (pressed & SCE_CTRL_UP)) sel = wrap(sel - 1, n);
			if (n && (pressed & SCE_CTRL_DOWN)) sel = wrap(sel + 1, n);
			// left/right: one page, L/R: tier tab
			if (n && (pressed & SCE_CTRL_LEFT)) sel = sel >= 10 ? sel - 10 : 0;
			if (n && (pressed & SCE_CTRL_RIGHT)) sel = sel + 10 < n ? sel + 10 : n - 1;
			if (s_gl_ntabs > 1 && (pressed & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER))) {
				s_gl_tab = wrap(s_gl_tab + ((pressed & SCE_CTRL_LTRIGGER) ? -1 : 1), s_gl_ntabs);
				int first = -1; // first game of that tab
				for (int i = 0; i < s_gl_n && first < 0; i++)
					if (gl_visible(i) && s_gl_tier[i] == s_gl_tabs[s_gl_tab])
						first = i;
				n = gl_build(idx, first, &sel);
				scr = -1;
			} else if (pressed & SCE_CTRL_SELECT) { // filter: Playable -> All -> Favorites (current game kept if visible)
				g_set.list_filter = wrap(g_set.list_filter + 1, RS12_LIST_FILTER_COUNT);
				rs12_settings_save();
				n = gl_build(idx, n ? idx[sel] : -1, &sel);
				scr = -1;
			} else if (n && (pressed & SCE_CTRL_SQUARE)) { // favorite on/off (if it drops out of Favorites the cursor moves on)
				const int cur = idx[sel];
				rs12_fav_toggle(s_gl[cur]->name);
				int w2 = cur;
				if (g_set.list_filter == RS12_LIST_FAVORITES && !gl_visible(cur))
					w2 = sel + 1 < n ? idx[sel + 1] : (sel > 0 ? idx[sel - 1] : -1);
				n = gl_build(idx, w2, &sel);
			} else if (pressed & SCE_CTRL_TRIANGLE) {
				about = 1;
				s_about_scroll = 0;
			} else if (pressed & SCE_CTRL_START) { // options: edit and save global settings only
				rs12_settings_use_global();
				s_list_mode = 1;
				s_page = PAGE_MAIN;
				s_sel = s_main_sel = 0;
				s_pop = 0;
				s_changed = 0;
				s_prev_buttons = b; // (so the START press does not act inside options)
			} else if (n && (pressed & ok)) {
				const int g = idx[sel];
				if (gl_playable(g)) {
					s_drawn_h = 0;
					focus_off();
					ui_hide(c);
					return s_gl[g]->name;
				}
				const char *par = s_gl[g]->parent && s_gl[g]->parent[0] ? s_gl[g]->parent : NULL;
				snprintf(msg, sizeof(msg), "ROM not found: %s.zip%s%s%s\nPut the ROM zip in %s", s_gl[g]->name,
					par ? "  (clone of " : "", par ? par : "", par ? ")" : "", rom_dir);
			}
		}
		focus_tick();
		if (s_list_mode && menu_anim())
			dirty = 1; // the options window is still scrolling
		{ // another theme: the list under the options is drawn again in the new colours
			static int seen_gen = -1;
			theme();
			if (seen_gen != s_theme_gen) {
				seen_gen = s_theme_gen;
				under_ok = 0;
				dirty = 1;
			}
		}
		if (!s_list_mode && under_ok) { // options closed: the list is drawn again in full
			under_ok = 0;
			dirty = 1;
		}
		// the list scrolls smoothly and fades out at its top and bottom edges; rows need not fit
		const int tab_h = 40, la = TITLE_H + tab_h, lb = SCREEN_H - LIST_FOOT_H, lpad = 18, fade = 20;
		const float rows_f = (float)(lb - la - 2 * lpad) / GL_LINE;
		{
			float maxt = (float)n - rows_f;
			if (maxt < 0)
				maxt = 0;
			if (sel < tgt + 1)
				tgt = (float)sel - 1;
			if (sel > tgt + rows_f - 2)
				tgt = (float)sel - rows_f + 2;
			if (tgt > maxt) tgt = maxt;
			if (tgt < 0) tgt = 0;
			if (scr < 0)
				scr = tgt;
			else if (scr - tgt > 1.5f * rows_f || tgt - scr > 1.5f * rows_f) // long jumps: ease over the last half page only
				scr = scr < tgt ? tgt - 0.5f * rows_f : tgt + 0.5f * rows_f;
			if (scr != tgt && !s_list_mode) {
				scr += (tgt - scr) * 0.3f;
				if (fabsf(tgt - scr) < 0.01f)
					scr = tgt;
				dirty = 1;
			}
		}
		if (dirty) {
			dirty = 0;
			const int h = SCREEN_H, w = SCREEN_W;
			const int px = 0, py = 0;
			const uint32_t *bgr = theme_rows(1);
			s_drawn_h = 0;
			focus_begin();
			if (s_list_mode && under_ok) {
				// options over the list: menu_paint puts back the dimmed list kept when they opened, around the window
				s_under = under;
			} else if (about) {
				ui_fill_rows(c, bgr, 0, 0, SCREEN_W, SCREEN_H);
				ui_text(c, px + 24, py + 36, COL_TEXT, 1.1f, "ABOUT");
				for (int i = 0; i < ABOUT_VISIBLE && s_about_scroll + i < ABOUT_LINES; i++)
					about_line(c, px + 24, py + TITLE_H + i * ABOUT_LH + 22, s_about[s_about_scroll + i]);
				text_keys(c, px + 24, py + h - 13, COL_DIM,
					o_ok ? "Up/Down: scroll   " G_CRO " / " G_TRI ": back" : "Up/Down: scroll   " G_CIR " / " G_TRI ": back");
			} else {
				ui_fill_rows(c, bgr, 0, 0, SCREEN_W, SCREEN_H);
				const int rw = w - 16; // (room for the scroll bar)
				const int still = s_list_mode || msg[0] || upd; // (no blinking focus under a window)
				s_focus_cy0 = la;
				s_focus_cy1 = lb;
				const int i0 = (int)floorf(scr), i1 = (int)ceilf(scr + rows_f + 2.0f * lpad / GL_LINE);
				for (int i = i0 < 0 ? 0 : i0; i < n && i <= i1; i++) {
					const int ty = la + lpad + (int)floorf(((float)i - scr) * GL_LINE + 0.5f) + GL_LINE / 2 + 7;
					gl_row(c, px, rw, ty, idx[i], i == sel ? (still ? 2 : 1) : 0, 0);
					if (i == sel) { // titles longer than the cell scroll (only this row is redrawn each frame below)
						if (mq_sel != idx[i])
							mq_t = 0;
						mq_sel = idx[i];
						mq_y = ty;
						mq_px = px;
						mq_w = rw;
						mq_tw = ui_text_width(1.0f, s_gl[idx[i]]->title);
					}
				}
				if (!n)
					mq_sel = -1;
				s_focus_cy0 = 0;
				s_focus_cy1 = SCREEN_H;
				for (int k = 0; k < fade; k++) { // fade bands: the background over the rows, opaque at the edge
					const unsigned al = (unsigned)(255 - (k + 1) * 255 / (fade + 1));
					ui_rect(c, 0, la + k, rw, 1, (bgr[la + k] & 0xFFFFFFu) | (al << 24));
					ui_rect(c, 0, lb - 1 - k, rw, 1, (bgr[lb - 1 - k] & 0xFFFFFFu) | (al << 24));
				}
				ui_fill_rows(c, bgr, 0, 0, SCREEN_W, la); // rows that scrolled past the edges
				ui_fill_rows(c, bgr, 0, lb, SCREEN_W, SCREEN_H - lb);
				if (n) { // scroll bar (display only)
					const int tx = SCREEN_W - 14, ty0 = la + 8, th = lb - la - 16;
					const float vis = rows_f < (float)n ? rows_f : (float)n;
					int bh = (int)(th * vis / n), by = ty0 + (int)(th * scr / n);
					if (bh < 16) bh = 16;
					if (by + bh > ty0 + th) by = ty0 + th - bh;
					rrect(c, tx, ty0, 5, th, 2, UI_RGBA(0xFF, 0xFF, 0xFF, 0x18));
					rrect(c, tx, by, 5, bh, 2, COL_THUMB);
				}
				{ // header: the app name in its colours, the version, the position in the list
					char title[96];
					int tx = px + 24 + draw_brand(c, px + 24, py + 36, 1.1f);
					snprintf(title, sizeof(title), " (" RS12_VERSION ") - SELECT GAME (%d/%d)", n ? sel + 1 : 0, n);
					ui_text(c, tx, py + 36, COL_TEXT, 1.1f, title);
				}
				char have[40];
				snprintf(have, sizeof(have), "%s %d", s_gl_filter_name[g_set.list_filter], s_gl_nvis);
				ui_text(c, px + w - 24 - ui_text_width(0.85f, have), py + 34, COL_DIM, 0.85f, have);
				{ // tab row: current tab bright with an accent underline
					int tbx = px + 16;
					for (int t = 0; t < s_gl_ntabs; t++) {
						const char *tn = s_tier_name[s_gl_tabs[t]];
						const int tw = ui_text_width(0.95f, tn) + 20;
						if (tbx + tw > px + w - 16)
							break;
						if (t == s_gl_tab)
							rrect(c, tbx + 6, py + TITLE_H + 26, tw - 12, 4, 2, COL_SEL);
						ui_text(c, tbx + 10, py + TITLE_H + 20, t == s_gl_tab ? COL_TEXT : COL_DIM, 0.95f, tn);
						tbx += tw + 8;
					}
				}
				if (!n && g_set.list_filter == RS12_LIST_FAVORITES)
					text_keys(c, px + 48, la + 40, COL_DIM, "No favorites. " G_SQU ": add, SELECT: filter");
				else if (!n)
					ui_text(c, px + 48, la + 40, COL_DIM, 1.0f, "No playable games. SELECT: show all, START: Refresh ROM list");
				{ // button help on one line
					char foot[192];
					snprintf(foot, sizeof(foot), "%s: play   " G_SQU ": favorite   " G_TRI ": about   %sLeft/Right: page   SELECT: %s   START: options",
						o_ok ? G_CIR : G_CRO, s_gl_ntabs > 1 ? "L/R: type   " : "",
						s_gl_filter_name[wrap(g_set.list_filter + 1, RS12_LIST_FILTER_COUNT)]);
					text_keys(c, px + 24, py + h - 13, COL_DIM, foot);
				}
				if (msg[0]) { // notice box (over the list)
					const int mw = 720, mh = 150, mx = (SCREEN_W - mw) / 2, my = (SCREEN_H - mh) / 2;
					window_box(c, mx, my, mw, mh, COL_EDGE, COL_PANEL);
					char line[256];
					snprintf(line, sizeof(line), "%s", msg);
					char *nl = strchr(line, '\n');
					if (nl)
						*nl = 0;
					ui_text(c, mx + 24, my + 44, COL_WARN, 1.0f, line);
					if (nl)
						ui_text(c, mx + 24, my + 84, COL_TEXT, 0.9f, nl + 1);
					ui_text(c, mx + 24, my + mh - 20, COL_DIM, 0.8f, "Press any button");
				}
				if (upd)
					upd_draw(c, o_ok);
				s_under = NULL;
				if (s_list_mode) { // options just opened: dim the list once and keep it (the window redraws over it)
					ui_rect(c, 0, 0, SCREEN_W, SCREEN_H, COL_BG);
					if (!under)
						under = (uint32_t *)malloc((size_t)SCREEN_W * SCREEN_H * 4);
					if (under) {
						memcpy(under, ui_pixels(c), (size_t)SCREEN_W * SCREEN_H * 4);
						under_ok = 1;
					}
				}
			}
			if (s_list_mode && !about)
				menu_paint(c, 1);
			focus_end();
			ui_commit(c);
		} else if (!about && !msg[0] && !upd && !s_list_mode && mq_sel >= 0 && mq_tw > mq_w - 72) {
			// the selected row's title scrolls when longer than the row: 1 s pause -> 60 px/s to the end -> 1 s pause ->
			// from the start. Only this row is drawn again
			const int avail = mq_w - 72, over = mq_tw - avail, hold = 60;
			const int cyc = hold + over + hold;
			const int t = mq_t++ % cyc;
			const int off = t < hold ? 0 : t < hold + over ? t - hold : over;
			const int by = mq_y - 7 - (GL_LINE - 6) / 2;
			ui_fill_rows(c, theme_rows(1), mq_px + 6, by - 2, mq_w - 12, GL_LINE + 4);
			gl_row(c, mq_px, mq_w, mq_y, mq_sel, 1, off);
			ui_commit(c);
		}
		if (capture) {
			char sp[64];
			snprintf(sp, sizeof(sp), DATA_DIR "/screen_gs%d.ppm", capture);
			rs12_video_capture_ppm(sp);
		} else
			rs12_video_present();
	}
}
