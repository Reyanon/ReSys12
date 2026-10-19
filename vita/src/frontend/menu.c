// In-game menu, start screen game select, notices, FPS box, loading bar. Ported from ReST-V vita/src/frontend/menu.c
// (common UI of the Re series of Vita emulators): same layout, controls and features; red theme colours and the System 12 game table.
// Drawing is in ui.c (system default PGF font drawn to a CPU canvas and overlaid on the screen).
#include "frontend/menu.h"

#include <psp2/ctrl.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/rtc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "frontend/ui.h"
#include "platform/log.h"
#include "platform/timer.h"
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
#define LINE_H   30
#define TITLE_H  56
#define FOOT_H   48
#define VALUE_X  330 // from panel left edge to the value column

// red theme
#define COL_BG    UI_RGBA(0x00, 0x00, 0x00, 0xC0)
#define COL_PANEL UI_RGBA(0x3A, 0x0C, 0x10, 0xF0)
#define COL_TEXT  UI_RGBA(0xFF, 0xFF, 0xFF, 0xFF)
#define COL_DIM   UI_RGBA(0xC8, 0xA8, 0xAC, 0xFF)
#define COL_SEL   UI_RGBA(0xFF, 0x34, 0x34, 0xFF)
#define COL_WARN  UI_RGBA(0xFF, 0xC0, 0x40, 0xFF)
#define COL_GREY     UI_RGBA(0x84, 0x70, 0x74, 0xFF)
#define COL_GREY_SEL UI_RGBA(0xC0, 0x70, 0x70, 0xFF)

// button symbols inside text (drawn by draw_key)
#define G_TRI "\x10"
#define G_CIR "\x11"
#define G_CRO "\x12"
#define G_SQU "\x13"

#define STATE_SLOTS 10
#define TEST_NOTICE_MSG "State 1 saved"

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
	g_set.turbo_key = RS12_KEY_NONE;
	g_set.turbo_btn = 1;
	g_set.turbo_speed = RS12_TURBO_FAST;
	memset(g_set.combo, 0, sizeof(g_set.combo)); // no combos
}

void rs12_settings_defaults(void)
{
	g_set.fps = 1;
	g_set.aspect = RS12_ASPECT_4_3;
	g_set.filter = RS12_FILTER_SMOOTH;
	g_set.scan_type = RS12_SCAN_OFF;
	g_set.scan_strength = RS12_SCAN_MEDIUM;
	g_set.border = RS12_BORDER_BLACK;
	g_set.border_color = RS12_BCOL_MAROON;
	g_set.confirm = RS12_CONFIRM_CROSS;
	g_set.list_filter = RS12_LIST_PLAYABLE;
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

void rs12_settings_apply_video(void)
{
	// rs12_video_set_mode: 0 = 4:3, 1 = integer, 2 = full
	const int aspect = g_set.aspect == RS12_ASPECT_FULL ? 2 : g_set.aspect == RS12_ASPECT_INT ? 1 : 0;
	rs12_video_set_mode(aspect, g_set.filter == RS12_FILTER_SMOOTH);
	rs12_video_set_border(g_set.border, g_set.border_color);
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
	int fps, aspect, filter, scan_type, scan_strength, border, border_color;
	int keymap[RS12_ACT_COUNT], stick, turbo_key, turbo_btn, turbo_speed;
	int combo[RS12_COMBO_KEYS];
	int aim_stick, aim_mode, crosshair, gun_sens, wheel_stick, lever_stick;
	int gun_fire, gun_sensor, gas_key, brake_key;
} game_part;
static game_part s_global;
static int s_game_on;   // per-game settings are active for the current game
static char s_game[32]; // current game set name (cfg/<set>.cfg)
static void part_get(game_part *g)
{
	g->fps = g_set.fps; g->aspect = g_set.aspect; g->filter = g_set.filter;
	g->scan_type = g_set.scan_type; g->scan_strength = g_set.scan_strength;
	g->border = g_set.border; g->border_color = g_set.border_color;
	memcpy(g->keymap, g_set.keymap, sizeof(g->keymap));
	g->stick = g_set.stick; g->turbo_key = g_set.turbo_key; g->turbo_btn = g_set.turbo_btn; g->turbo_speed = g_set.turbo_speed;
	memcpy(g->combo, g_set.combo, sizeof(g->combo));
	g->aim_stick = g_set.aim_stick; g->aim_mode = g_set.aim_mode; g->crosshair = g_set.crosshair;
	g->gun_sens = g_set.gun_sens; g->wheel_stick = g_set.wheel_stick; g->lever_stick = g_set.lever_stick;
	g->gun_fire = g_set.gun_fire; g->gun_sensor = g_set.gun_sensor; g->gas_key = g_set.gas_key; g->brake_key = g_set.brake_key;
}
static void part_set(const game_part *g)
{
	g_set.fps = g->fps; g_set.aspect = g->aspect; g_set.filter = g->filter;
	g_set.scan_type = g->scan_type; g_set.scan_strength = g->scan_strength;
	g_set.border = g->border; g_set.border_color = g->border_color;
	memcpy(g_set.keymap, g->keymap, sizeof(g->keymap));
	g_set.stick = g->stick; g_set.turbo_key = g->turbo_key; g_set.turbo_btn = g->turbo_btn; g_set.turbo_speed = g->turbo_speed;
	memcpy(g_set.combo, g->combo, sizeof(g->combo));
	g_set.aim_stick = g->aim_stick; g_set.aim_mode = g->aim_mode; g_set.crosshair = g->crosshair;
	g_set.gun_sens = g->gun_sens; g_set.wheel_stick = g->wheel_stick; g_set.lever_stick = g->lever_stick;
	g_set.gun_fire = g->gun_fire; g_set.gun_sensor = g->gun_sensor; g_set.gas_key = g->gas_key; g_set.brake_key = g->brake_key;
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
	else if (!strcmp(key, "stick")) g_set.stick = clampi(val, 0, RS12_STICK_COUNT - 1);
	else if (!strcmp(key, "aim_stick")) g_set.aim_stick = clampi(val, 0, RS12_AIM_STICK_COUNT - 1);
	else if (!strcmp(key, "aim_mode")) g_set.aim_mode = clampi(val, 0, RS12_AIM_MODE_COUNT - 1);
	else if (!strcmp(key, "crosshair")) g_set.crosshair = val != 0;
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
	fprintf(f, "border=%d\nborder_color=%d\n", g->border, g->border_color);
	for (int a = 0; a < RS12_ACT_COUNT; a++)
		fprintf(f, "%s=%d\n", s_keymap_keys[a], g->keymap[a]);
	fprintf(f, "stick=%d\nturbo_key=%d\nturbo_btn=%d\nturbo_speed=%d\n", g->stick, g->turbo_key, g->turbo_btn, g->turbo_speed);
	fprintf(f, "aim_stick=%d\naim_mode=%d\ncrosshair=%d\n", g->aim_stick, g->aim_mode, g->crosshair);
	fprintf(f, "gun_sens=%d\nwheel_stick=%d\nlever_stick=%d\n", g->gun_sens, g->wheel_stick, g->lever_stick);
	fprintf(f, "gun_fire=%d\ngun_sensor=%d\ngas_key=%d\nbrake_key=%d\n", g->gun_fire, g->gun_sensor, g->gas_key, g->brake_key);
	for (int k = 0; k < RS12_COMBO_KEYS; k++)
		fprintf(f, "combo_%s=%d\n", s_combo_keys[k], g->combo[k]);
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
		}
		fclose(f);
	}
	part_get(&s_global);
	RS12_LOG("settings: fps %d aspect %d filter %d border %d/%d keys %d %d %d %d %d %d coin %d start %d stick %d turbo %d/%d",
		g_set.fps, g_set.aspect, g_set.filter, g_set.border, g_set.border_color, g_set.keymap[0], g_set.keymap[1],
		g_set.keymap[2], g_set.keymap[3], g_set.keymap[4], g_set.keymap[5], g_set.keymap[6], g_set.keymap[7], g_set.stick,
		g_set.turbo_key, g_set.turbo_btn);
}

static void game_cfg_path(char *p, int len)
{
	snprintf(p, len, CFG_DIR "/%s.cfg", s_game);
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
	if (!f)
		return;
	char key[32], sval[32];
	while (read_kv(f, key, sizeof(key), sval, sizeof(sval)))
		read_part(key, atoi(sval));
	fclose(f);
	s_game_on = 1;
	RS12_LOG("settings: game settings %s", p);
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
	fprintf(f, "menu_confirm=%d\nlist_filter=%d\n", g_set.confirm, g_set.list_filter);
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
	part_set(&s_global); // (global values right away)
	rs12_settings_apply_video();
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
enum { PAGE_MAIN, PAGE_STATES, PAGE_DISPLAY, PAGE_CONTROLS, PAGE_ABOUT, PAGE_COMBO, PAGE_ANALOG };
enum { M_RESUME, M_RESET, M_SERVICE, M_STATES, M_DISPLAY, M_CONTROLS, M_GAME_SAVE, M_GAME_DEL, M_ABOUT, M_SELECT_GAME, M_EXIT, M_BACK, M_COUNT };
// game list options (START in the list): no game. Only Display / Controls / Back, global settings only
static int s_list_mode;
// main menu row count and row -> item (Delete only if per-game settings exist, else Save only. Back only in list options)
static int main_rows(void) { return s_list_mode ? 3 : M_COUNT - 2; }
static int main_kind(int row)
{
	if (s_list_mode) {
		static const int k[3] = { M_DISPLAY, M_CONTROLS, M_BACK };
		return k[row];
	}
	const int hidden = rs12_settings_has_game() ? M_GAME_SAVE : M_GAME_DEL;
	return row >= hidden ? row + 1 : row;
}
enum { S_SLOT, S_SAVE, S_LOAD, S_BACK, S_COUNT };
// Display rows (scanline rows hidden until the shader exists; values stay in the settings file)
enum { D_FPS, D_ASPECT, D_FILTER, D_BORDER, D_BORDER_COL, D_BACK, D_COUNT };
// Controls rows: game buttons (as many as the game uses, 6 in list options), coin, start, turbo key, turbo button, turbo speed,
// analog stick, the gun / wheel / lever page (games that have one), confirm button, combo keys, defaults, back.
// Gun / wheel / lever page (PAGE_ANALOG): only that game's kind of rows, then back (CK_ABACK)
enum { CK_ACT, CK_TURBO_KEY, CK_TURBO_BTN, CK_TURBO_SPEED, CK_STICK, CK_AIM_STICK, CK_AIM_MODE, CK_GUN_SENS, CK_CROSSHAIR,
	CK_GUN_FIRE, CK_GUN_SENSOR, CK_WHEEL_STICK, CK_GAS, CK_BRAKE, CK_LEVER_STICK, CK_CONFIRM, CK_COMBO, CK_DEFAULTS, CK_BACK,
	CK_ANALOG, CK_ABACK };
// Combo keys page: one row per Vita key (s_combo_order), then Back. Confirm opens the choice list (s_pick)
#define COMBO_ROWS (RS12_COMBO_KEYS + 1)
static int s_pick = -1; // choice list open: highlighted choice index, -1 = closed
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
	k[n++] = CK_TURBO_KEY;
	k[n++] = CK_TURBO_BTN;
	k[n++] = CK_TURBO_SPEED;
	k[n++] = CK_STICK;
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
static int s_page; // (defined with the other menu state below)
// current row's kind: Controls or the gun / wheel / lever page
static int row_kind(int i, int *act)
{
	if (s_page == PAGE_ANALOG) {
		int k[16];
		const int n = analog_rows(k);
		*act = -1;
		return i < n ? k[i] : CK_ABACK;
	}
	return ctrl_kind(i, act);
}
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
// key and func of a Controls / gun-wheel row that assigns a key (0 = not such a row)
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
// notice after a key change that made a duplicate
static void warn_clash(int key, int func)
{
	const char *other = key_clash(key, func);
	if (other)
		rs12_osd_show("%s is also used for %s", rs12_key_name(key), other);
}
static int s_page, s_sel, s_main_sel, s_reset_armed, s_state_slot, s_about_scroll, s_dirty = 1;
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

int rs12_menu_should_open(uint32_t buttons)
{
	const int combo = (buttons & MENU_OPEN_COMBO) == MENU_OPEN_COMBO;
	const int prev_combo = (s_prev_buttons & MENU_OPEN_COMBO) == MENU_OPEN_COMBO;
	const int open = combo && !prev_combo;
	s_prev_buttons = buttons; // so the same press does not act right away inside the menu
	if (open) {
		s_page = PAGE_MAIN;
		s_sel = 0;
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

// pages longer than this scroll (Controls with 6 buttons has 16 rows); 13 rows of LINE_H fit 544 lines with title and foot
#define MAX_ROWS 13
static int s_row_top; // first row shown on a scrolling page

static void go_page(int page)
{
	if (s_page == PAGE_MAIN)
		s_main_sel = s_sel;
	s_page = page;
	s_sel = (page == PAGE_MAIN) ? s_main_sel : 0;
	s_row_top = 0;
	if (page == PAGE_MAIN && s_sel >= main_rows())
		s_sel = 0;
	s_reset_armed = 0;
}

// repeat while a direction is held (after about 0.4 s, every 0.07 s)
#define REPEAT_KEYS (SCE_CTRL_UP | SCE_CTRL_DOWN | SCE_CTRL_LEFT | SCE_CTRL_RIGHT)
static uint32_t key_repeat(uint32_t held, uint32_t pressed, int *count)
{
	const uint32_t dir = held & REPEAT_KEYS;
	if (!dir || (pressed & REPEAT_KEYS)) {
		*count = 0;
		return pressed;
	}
	++*count;
	if (*count >= 24 && (*count - 24) % 4 == 0)
		pressed |= dir;
	return pressed;
}

static int wrap(int v, int n)
{
	return (v % n + n) % n;
}

static void change(int dir)
{
	switch (s_page) {
	case PAGE_DISPLAY:
		switch (s_sel) {
		case D_FPS: g_set.fps = !g_set.fps; break;
		case D_ASPECT: g_set.aspect = wrap(g_set.aspect + dir, RS12_ASPECT_COUNT); break;
		case D_FILTER: g_set.filter = wrap(g_set.filter + dir, RS12_FILTER_COUNT); break;
		case D_BORDER: g_set.border = wrap(g_set.border + dir, RS12_BORDER_COUNT); break;
		case D_BORDER_COL: g_set.border_color = wrap(g_set.border_color + dir, RS12_BCOL_COUNT); break;
		default: return;
		}
		rs12_settings_apply_video();
		s_changed = 1;
		break;
	case PAGE_CONTROLS:
	case PAGE_ANALOG: {
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

// Combo keys page -> Controls, on the "Combo keys" row
static void combo_back(void)
{
	go_page(PAGE_CONTROLS);
	s_sel = ctrl_row_of(CK_COMBO);
}

// gun / wheel / lever page -> Controls, on its row
static void analog_back(void)
{
	go_page(PAGE_CONTROLS);
	s_sel = ctrl_row_of(CK_ANALOG);
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
		case M_EXIT: return RS12_MENU_EXIT;
		case M_BACK: return RS12_MENU_CLOSE;
		}
		break;
	case PAGE_DISPLAY:
		if (s_sel == D_BACK) go_page(PAGE_MAIN);
		else change(+1);
		break;
	case PAGE_CONTROLS: {
		int act;
		const int k = ctrl_kind(s_sel, &act);
		if (k == CK_BACK) go_page(PAGE_MAIN);
		else if (k == CK_COMBO) go_page(PAGE_COMBO);
		else if (k == CK_ANALOG) go_page(PAGE_ANALOG);
		else if (k == CK_DEFAULTS) {
			rs12_settings_default_keys();
			s_changed = 1;
		} else change(+1);
		break;
	}
	case PAGE_ANALOG: {
		int act;
		if (row_kind(s_sel, &act) == CK_ABACK)
			analog_back();
		else
			change(+1);
		break;
	}
	case PAGE_ABOUT:
		go_page(PAGE_MAIN);
		break;
	case PAGE_COMBO:
		if (s_sel == COMBO_ROWS - 1)
			combo_back();
		else
			s_pick = combo_index(g_set.combo[s_combo_order[s_sel]]); // open the choice list
		break;
	case PAGE_STATES:
		if (s_sel == S_SAVE) return RS12_MENU_SAVE;
		if (s_sel == S_LOAD) return RS12_MENU_LOAD;
		if (s_sel == S_BACK) go_page(PAGE_MAIN);
		if (s_sel == S_SLOT) change(+1);
		break;
	}
	return RS12_MENU_STAY;
}

rs12_menu_result rs12_menu_update(uint32_t buttons)
{
	static int rep_count;
	uint32_t pressed = key_repeat(buttons, buttons & ~s_prev_buttons, &rep_count);
	s_prev_buttons = buttons;
	if (pressed)
		s_dirty = 1;
	rs12_menu_result r = RS12_MENU_STAY;
	if (s_pick >= 0) { // combo choice list: up/down, confirm picks, back closes
		const uint32_t ok_b = g_set.confirm == RS12_CONFIRM_CIRCLE ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS;
		const uint32_t back_b = ok_b == SCE_CTRL_CROSS ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS;
		if (pressed & SCE_CTRL_UP)
			s_pick = wrap(s_pick - 1, COMBO_CHOICES);
		if (pressed & SCE_CTRL_DOWN)
			s_pick = wrap(s_pick + 1, COMBO_CHOICES);
		if (pressed & ok_b) {
			g_set.combo[s_combo_order[s_sel]] = combo_choice(s_pick);
			s_changed = 1;
			s_pick = -1;
			if (g_set.combo[s_combo_order[s_sel]])
				warn_clash(s_combo_order[s_sel], KF_COMBO + s_combo_order[s_sel]);
		} else if (pressed & back_b) {
			s_pick = -1;
		}
		if (pressed)
			s_drawn_h = 0; // the list covers more than the panel: redraw everything
		return RS12_MENU_STAY;
	}
	const int n = page_items();
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
		else if (s_page == PAGE_COMBO)
			combo_back();
		else if (s_page == PAGE_ANALOG)
			analog_back();
		else
			go_page(PAGE_MAIN);
	}
	if (pressed & SCE_CTRL_START)
		r = RS12_MENU_CLOSE;
	// memory card write only when the user changed something (no card I/O otherwise while a game is loaded)
	if (r != RS12_MENU_STAY && r != RS12_MENU_SAVE && s_changed) {
		rs12_settings_save();
		s_changed = 0;
	}
	return r;
}

// ================================ drawing ================================
// "Re" white + "Sys12" in the accent colour (Re franchise naming rule). Returns the width
static int draw_brand(UiCanvas *c, int x, int y, float scale)
{
	int w = ui_text(c, x, y, COL_TEXT, scale, "Re");
	w += ui_text(c, x + w, y, COL_SEL, scale, "Sys12");
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

static void item(UiCanvas *c, int px, int ty, int i, int sel, const char *name, const char *value)
{
	const uint32_t col = (i == sel) ? COL_SEL : COL_TEXT;
	if (i == sel)
		ui_text(c, px + 16, ty, COL_SEL, 1.0f, ">");
	ui_text(c, px + 40, ty, col, 1.0f, name);
	if (value) {
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

// Draw the menu panel to the canvas (caller uploads). overlay = over the game list (dim without clearing the canvas, then the panel)
static void menu_paint(UiCanvas *c, int overlay)
{
	static const char *const aspect[] = { "4:3", "Full screen", "Integer" };
	static const char *const filter[] = { "Smooth", "Sharp" };
	static const char *const border[] = { "Black", "Artwork", "Color" };
	static const char *const border_col[] = { "Navy", "Gray", "Maroon", "Green", "Purple", "Brick" };
	static const char *const stick[] = { "Off", "Left", "Right", "Both" };
	static const char *const turbo_speed[] = { "Fast (15/s)", "Medium (10/s)", "Slow (7.5/s)" };
	const int o_ok = g_set.confirm == RS12_CONFIRM_CIRCLE;
	const char *ok_name = o_ok ? G_CIR : G_CRO, *back_name = o_ok ? G_CRO : G_CIR;
	char foot_buf[128];
	const int n = page_items();
	const int lh = LINE_H;
	// scrolling: keep the selected row inside the visible window
	const int vis = n > MAX_ROWS ? MAX_ROWS : n;
	if (s_sel < s_row_top)
		s_row_top = s_sel;
	if (s_sel >= s_row_top + vis)
		s_row_top = s_sel - vis + 1;
	if (s_row_top > n - vis)
		s_row_top = n - vis;
	if (s_row_top < 0)
		s_row_top = 0;
	const int h = TITLE_H + vis * lh + FOOT_H;
	const int px = (SCREEN_W - PANEL_W) / 2, py = (SCREEN_H - h) / 2;
	if (overlay) {
		ui_rect(c, 0, 0, SCREEN_W, SCREEN_H, COL_BG);
		s_drawn_h = 0;
	} else if (s_drawn_h == h) {
		// if the panel size is unchanged, redraw only the panel (do not recompose and copy the whole dimmed screen every time)
		ui_clear_rect(c, px, py, PANEL_W, h);
		ui_rect(c, px, py, PANEL_W, h, COL_BG);
	} else {
		ui_clear(c);
		ui_rect(c, 0, 0, SCREEN_W, SCREEN_H, COL_BG);
		s_drawn_h = h;
	}
	ui_rect(c, px, py, PANEL_W, h, COL_PANEL);

	const char *title = "OPTIONS";
	snprintf(foot_buf, sizeof(foot_buf), "Up/Down: select   Left/Right/%s: change   %s: back", ok_name, back_name);
	const char *foot = foot_buf;
	switch (s_page) {
	case PAGE_DISPLAY:
		title = "DISPLAY";
		if (s_list_mode)
			foot = "Left/Right: change   (global settings)";
		break;
	case PAGE_CONTROLS:
		title = "CONTROLS";
		foot = s_list_mode ? "Left/Right: choose button   (global settings)" : "Left/Right: choose button   Menu: SELECT+START";
		break;
	case PAGE_ANALOG: {
		const int a = menu_analog();
		title = a == S12_ANALOG_GUN ? "GUN" : a == S12_ANALOG_WHEEL ? "WHEEL" : "LEVER";
		foot = a == S12_ANALOG_GUN ? "The trigger also counts as the game's fire button" : "The d-pad works as well as the stick";
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

	// more rows above / below the window
	if (s_row_top > 0)
		ui_text(c, px + PANEL_W - 40, py + TITLE_H + 14, COL_DIM, 0.9f, "^");
	if (s_row_top + vis < n)
		ui_text(c, px + PANEL_W - 40, py + TITLE_H + vis * lh + 4, COL_DIM, 0.9f, "v");
	for (int i = s_row_top; i < s_row_top + vis; i++) {
		const int ty = py + TITLE_H + (i - s_row_top) * lh + 22;
		switch (s_page) {
		case PAGE_MAIN: {
			static const char *const names[M_COUNT] = { "Resume game", "Reset game", "Test mode (DIP switch)", "Save / Load state  >", "Display  >",
				"Controls  >", "Save game settings", "Delete game settings", "About  >", "Select game", "Exit", "Back" };
			const int k = main_kind(i);
			const char *name = names[k];
			char rbuf[64];
			if (k == M_RESET && s_reset_armed) {
				snprintf(rbuf, sizeof(rbuf), "Reset game - press %s again", o_ok ? "Circle" : "Cross");
				name = rbuf;
			}
			item(c, px, ty, i, s_sel, name, k == M_SERVICE ? (rs12_dip_service ? "On" : "Off") : NULL);
			break;
		}
		case PAGE_DISPLAY: {
			static const char *const names[D_COUNT] = { "Show FPS", "Screen", "Filter", "Border (4:3 / Integer)", "Border color",
				"Back" };
			const char *v = NULL;
			switch (i) {
			case D_FPS: v = g_set.fps ? "On" : "Off"; break;
			case D_ASPECT: v = aspect[g_set.aspect]; break;
			case D_FILTER: v = filter[g_set.filter]; break;
			case D_BORDER: v = border[g_set.border]; break;
			case D_BORDER_COL: v = border_col[g_set.border_color]; break;
			}
			item(c, px, ty, i, s_sel, names[i], v);
			break;
		}
		case PAGE_CONTROLS:
		case PAGE_ANALOG: {
			int act, rk, rf;
			const int k = row_kind(i, &act);
			const int clash = row_key(k, act, &rk, &rf) && key_clash(rk, rf) != NULL;
			if (k == CK_ACT)
				item_key_w(c, px, ty, i, s_sel, act_name(act), g_set.keymap[act], clash);
			else if (k == CK_TURBO_KEY)
				item_key_w(c, px, ty, i, s_sel, "Turbo key", g_set.turbo_key, clash);
			else if (k == CK_ANALOG) {
				const int a = menu_analog();
				item(c, px, ty, i, s_sel, a == S12_ANALOG_GUN ? "Gun settings  >" : a == S12_ANALOG_WHEEL ? "Wheel settings  >"
					: "Lever settings  >", NULL);
			} else if (k == CK_ABACK)
				item(c, px, ty, i, s_sel, "Back", NULL);
			else if (k == CK_TURBO_BTN) {
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
				item(c, px, ty, i, s_sel, "Gun crosshair", g_set.crosshair ? "On" : "Off");
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
				draw_key(c, px + 40, ty - 7, ty, key, col);
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
	text_keys(c, px + 24, py + h - 18, COL_DIM, foot);

	if (s_page == PAGE_COMBO && s_pick >= 0) { // choice list over the page
		const int lh2 = 26, lw = 300, lh = 50 + COMBO_CHOICES * lh2 + 10;
		const int lx = (SCREEN_W - lw) / 2, ly = (SCREEN_H - lh) / 2;
		ui_rect(c, lx - 2, ly - 2, lw + 4, lh + 4, COL_SEL);
		ui_rect(c, lx, ly, lw, lh, COL_PANEL);
		const int key = s_combo_order[s_sel];
		const int tw = ui_text(c, lx + 20, ly + 32, COL_TEXT, 1.0f, "Combo for ");
		draw_key(c, lx + 20 + tw, ly + 25, ly + 32, key, COL_TEXT);
		for (int j = 0; j < COMBO_CHOICES; j++) {
			const int ty = ly + 50 + j * lh2 + 18;
			const uint32_t col = j == s_pick ? COL_SEL : COL_TEXT;
			if (j == s_pick)
				ui_text(c, lx + 16, ty, COL_SEL, 1.0f, ">");
			draw_combo(c, lx + 40, ty, combo_choice(j), col);
		}
	}
}

void rs12_menu_draw(void)
{
	if (!s_dirty || !s_cv)
		return;
	s_dirty = 0;
	menu_paint(s_cv, 0);
	ui_commit(s_cv);
}

#define TEST_BG UI_RGBA(0x50, 0x50, 0x58, 0xFF)

void rs12_menu_selftest(void)
{
	ensure_canvases();
	static const struct { int page, sel, pick; const char *name; } shots[] = {
		{ PAGE_MAIN, 0, -1, "main" }, { PAGE_STATES, 0, -1, "states" }, { PAGE_DISPLAY, 1, -1, "display" },
		{ PAGE_CONTROLS, 6, -1, "controls" }, { PAGE_CONTROLS, 14, -1, "controls_end" }, { PAGE_COMBO, 4, -1, "combo" },
		{ PAGE_COMBO, 4, 9, "combo_list" }, { PAGE_ABOUT, 0, -1, "about" },
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
		s_sel = shots[k].sel;
		s_pick = shots[k].pick;
		s_dirty = 1;
		rs12_menu_draw();
		snprintf(p, sizeof(p), DATA_DIR "/menu_%s.ppm", shots[k].name);
		ui_save_ppm(s_cv, p, TEST_BG);
		snprintf(p, sizeof(p), DATA_DIR "/screen_%s.ppm", shots[k].name);
		rs12_video_capture_ppm(p);
	}
	s_pick = -1;
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
	if (s_cv)
		ui_hide(s_cv);
	s_dirty = 1;
	s_drawn_h = 0;
}

// ---- loading screen ----
static char s_loading_title[96];
static uint64_t s_loading_us; // time spent drawing the bar (logged at the end)
static int s_loading_draws, s_loading_shown;
void rs12_loading_title(const char *title)
{
	snprintf(s_loading_title, sizeof(s_loading_title), "%s", title ? title : "");
}

void rs12_loading_progress(int permille)
{
	static uint64_t last_t;
	const uint64_t now = rs12_time_us();
	if (s_loading_shown && permille > 0 && permille < 1000 && now - last_t < 250000)
		return; // at most 4 draws a second (each draw waits for vblank and slows the load)
	if (permille < 0) permille = 0;
	if (permille > 1000) permille = 1000;
	last_t = now;
	s_loading_draws++;
	ensure_canvases();
	UiCanvas *c = s_cv;
	const int w = 600, h = 18, x = (SCREEN_W - w) / 2, y = SCREEN_H / 2;
	if (!s_loading_shown) { // whole screen once, then only the bar (small texture upload)
		s_drawn_h = 0;
		ui_clear(c);
		ui_rect(c, 0, 0, SCREEN_W, SCREEN_H, UI_RGBA(0, 0, 0, 0xFF));
		ui_text(c, x, y - 20, COL_TEXT, 1.0f, s_loading_title[0] ? s_loading_title : "Loading...");
		ui_rect(c, x - 2, y - 2, w + 4, h + 4, COL_DIM);
		s_loading_shown = 1;
	}
	ui_clear_rect(c, x, y, w, h); // (bar area back to opaque black before the translucent panel colour)
	ui_rect(c, x, y, w, h, UI_RGBA(0, 0, 0, 0xFF));
	ui_rect(c, x, y, w, h, COL_PANEL);
	ui_rect(c, x, y, w * permille / 1000, h, COL_SEL);
	ui_commit(c);
	rs12_video_present();
	s_loading_us += rs12_time_us() - now;
	static int saved;
	if (!saved && permille >= 300) { // test: one loading bar image while menutest is armed (no file I/O otherwise)
		saved = 1;
		SceIoStat st;
		if (sceIoGetstat(DATA_DIR "/menutest", &st) >= 0)
			ui_save_ppm(c, DATA_DIR "/menu_loading.ppm", TEST_BG);
	}
}

void rs12_loading_done(void)
{
	RS12_LOG("loading bar: %d draws, %llu ms", s_loading_draws, (unsigned long long)(s_loading_us / 1000));
	s_loading_us = 0;
	s_loading_draws = 0;
	s_loading_shown = 0;
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

// ---- FPS (top right box, with frame number) ----
void rs12_fps_update(float fps, unsigned frame)
{
	static unsigned last;
	static int shown;
	if (!g_set.fps) {
		if (shown && s_fps_cv)
			ui_hide(s_fps_cv);
		shown = 0;
		return;
	}
	if (shown && frame - last < 6)
		return;
	last = frame;
	shown = 1;
	ensure_canvases();
	UiCanvas *c = s_fps_cv;
	char buf[48];
	snprintf(buf, sizeof(buf), "%.1f FPS  F%u", (double)fps, frame);
	int w = ui_text_width(1.0f, buf) + 16;
	if (w > 240)
		w = 240;
	const uint32_t col = fps >= 59.0f ? UI_RGBA(0x60, 0xFF, 0x60, 0xFF) : UI_RGBA(0xFF, 0xC0, 0x40, 0xFF);
	ui_clear(c);
	ui_rect(c, 240 - w, 0, w, 30, UI_RGBA(0x00, 0x00, 0x00, 0xA0));
	ui_text(c, 240 - w + 8, 22, col, 1.0f, buf);
	ui_commit(c);
}

// ================================ start screen game select ================================
// Same look and behaviour as ReST-V rstv_game_select: tabs (here: support tier), switched with L/R. SELECT = filter (Playable: games with
// ROMs / All: everything, games without ROMs grey, picking one shows a notice / Favorites). Square = favorite on/off, START = options
// (Display, Controls, global settings), Triangle = About. The cursor starts on the last game; nothing starts on its own.
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

const char *rs12_game_select(const char *initial_set, const char *rom_dir, volatile int *quit)
{
	ensure_canvases();
	UiCanvas *c = s_cv;
	int nplay = 0, want = -1;
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
		if (initial_set && !strcmp(initial_set, g->name))
			want = i;
	}
	if (!nplay && g_set.list_filter == RS12_LIST_PLAYABLE)
		g_set.list_filter = RS12_LIST_ALL; // (show everything if no game can run)
	if (want >= 0 && gl_playable(want) && !gl_visible(want))
		g_set.list_filter = RS12_LIST_PLAYABLE; // (the cursor goes to the last game, so it must be on screen)
	const int filter0 = g_set.list_filter;
	static int idx[GL_MAX];
	int sel = 0, top = 0, about = 0, dirty = 1, rep = 0;
	int mq_sel = -1, mq_t = 0, mq_y = 0, mq_px = 0, mq_w = 0, mq_tw = 0; // scrolling title of the selected row (marquee)
	s_gl_ntabs = s_gl_tab = 0;
	int n = gl_build(idx, want, &sel);
	ui_clear(c); // whole canvas the first time (what loading/menu left there)
	char msg[256] = { 0 };
	uint32_t prev = 0xffffffffu; // ignore until released once, so the press that launched the app does not pick right away
	// test (ux0:data/resys12/gstest): first screen -> favorite -> filter -> filter -> options -> down -> close options, as
	// menu_gs<k>.ppm (canvas) and screen_gs<k>.ppm (whole screen); simulated presses
	SceIoStat gst;
	int gstest = sceIoGetstat(DATA_DIR "/gstest", &gst) >= 0 ? 1 : 0, gstep = 0;
	if (gstest)
		sceIoRemove(DATA_DIR "/gstest");
	for (;;) {
		if (quit && *quit) {
			ui_hide(c);
			return NULL;
		}
		SceCtrlData pad;
		sceCtrlPeekBufferPositive(0, &pad, 1);
		uint32_t b = pad.buttons;
		int capture = 0;
		if (gstest) {
			static const uint32_t seq[8] = { 0, 0, SCE_CTRL_SQUARE, SCE_CTRL_SELECT, SCE_CTRL_SELECT, SCE_CTRL_START, SCE_CTRL_DOWN,
				SCE_CTRL_START };
			if (gstep > 0 && !dirty) {
				char gp[64];
				snprintf(gp, sizeof(gp), DATA_DIR "/menu_gs%d.ppm", gstep);
				ui_save_ppm(c, gp, UI_RGBA(0, 0, 0, 0xFF));
				capture = gstep;
			}
			static const char *gs_set; // game the test marked favorite (restored at the end)
			if (gstep == 2 && n)
				gs_set = s_gl[idx[sel]]->name;
			if (gstep >= 8) {
				gstest = 0;
				if (gs_set)
					rs12_fav_toggle(gs_set);
				if (g_set.list_filter != filter0) { // (the test changed the filter: put the user's back)
					g_set.list_filter = filter0;
					rs12_settings_save();
					n = gl_build(idx, want, &sel);
					dirty = 1;
				}
				RS12_LOG("menu: game list test images written");
			} else {
				prev = 0;
				b = seq[gstep];
			}
			gstep++;
		}
		if (pad.ly < 128 - 48) b |= SCE_CTRL_UP;
		if (pad.ly > 128 + 48) b |= SCE_CTRL_DOWN;
		if (pad.lx < 128 - 48) b |= SCE_CTRL_LEFT;
		if (pad.lx > 128 + 48) b |= SCE_CTRL_RIGHT;
		const uint32_t pressed = key_repeat(b, b & ~prev, &rep);
		prev = b;
		if (pressed)
			dirty = 1;
		const int o_ok = g_set.confirm == RS12_CONFIRM_CIRCLE;
		const uint32_t ok = o_ok ? SCE_CTRL_CIRCLE : SCE_CTRL_CROSS, back = o_ok ? SCE_CTRL_CROSS : SCE_CTRL_CIRCLE;
		if (s_list_mode) { // options (Display / Controls, global settings. Saved by rs12_menu_update on close)
			if (rs12_menu_update(b) != RS12_MENU_STAY) {
				s_list_mode = 0;
				s_page = PAGE_MAIN;
				ui_clear(c);
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
			if (n && (pressed & SCE_CTRL_LEFT)) sel = sel >= 12 ? sel - 12 : 0;
			if (n && (pressed & SCE_CTRL_RIGHT)) sel = sel + 12 < n ? sel + 12 : n - 1;
			if (s_gl_ntabs > 1 && (pressed & (SCE_CTRL_LTRIGGER | SCE_CTRL_RTRIGGER))) {
				s_gl_tab = wrap(s_gl_tab + ((pressed & SCE_CTRL_LTRIGGER) ? -1 : 1), s_gl_ntabs);
				int first = -1; // first game of that tab
				for (int i = 0; i < s_gl_n && first < 0; i++)
					if (gl_visible(i) && s_gl_tier[i] == s_gl_tabs[s_gl_tab])
						first = i;
				n = gl_build(idx, first, &sel);
				top = 0;
			} else if (pressed & SCE_CTRL_SELECT) { // filter: Playable -> All -> Favorites (current game kept if visible)
				g_set.list_filter = wrap(g_set.list_filter + 1, RS12_LIST_FILTER_COUNT);
				rs12_settings_save();
				n = gl_build(idx, n ? idx[sel] : -1, &sel);
				top = 0;
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
				s_changed = 0;
				s_prev_buttons = b; // (so the START press does not act inside options)
			} else if (n && (pressed & ok)) {
				const int g = idx[sel];
				if (gl_playable(g)) {
					s_drawn_h = 0;
					ui_hide(c);
					return s_gl[g]->name;
				}
				const char *par = s_gl[g]->parent && s_gl[g]->parent[0] ? s_gl[g]->parent : NULL;
				snprintf(msg, sizeof(msg), "ROM not found: %s.zip%s%s%s\nPut the ROM zip in %s", s_gl[g]->name,
					par ? "  (clone of " : "", par ? par : "", par ? ")" : "", rom_dir);
			}
		}
		if (dirty) {
			dirty = 0;
			const int rows = 12, vis = n < rows ? n : rows;
			if (sel < top) top = sel;
			if (sel >= top + vis) top = sel - vis + 1;
			if (top > n - vis) top = n - vis;
			if (top < 0) top = 0;
			const int tab_h = 40;
			const int h = TITLE_H + tab_h + rows * LINE_H + FOOT_H, w = 800;
			const int px = (SCREEN_W - w) / 2, py = (SCREEN_H - h) / 2;
			s_drawn_h = 0;
			if (s_list_mode)
				ui_clear(c); // (including the dim layer options drew)
			else
				ui_clear_rect(c, px - 2, py - 2, w + 4, h + 4);
			ui_rect(c, px, py, w, h, COL_PANEL);
			if (about) {
				ui_text(c, px + 24, py + 36, COL_TEXT, 1.1f, "ABOUT");
				for (int i = 0; i < ABOUT_VISIBLE && s_about_scroll + i < ABOUT_LINES; i++)
					about_line(c, px + 24, py + TITLE_H + i * LINE_H + 22, s_about[s_about_scroll + i]);
				text_keys(c, px + 24, py + h - 18, COL_DIM,
					o_ok ? "Up/Down: scroll   " G_CRO " / " G_TRI ": back" : "Up/Down: scroll   " G_CIR " / " G_TRI ": back");
			} else {
				char title[64];
				int tx = px + 24 + draw_brand(c, px + 24, py + 36, 1.1f);
				snprintf(title, sizeof(title), " - SELECT GAME (%d/%d)", n ? sel + 1 : 0, n);
				ui_text(c, tx, py + 36, COL_TEXT, 1.1f, title);
				char have[40];
				snprintf(have, sizeof(have), "%s %d", s_gl_filter_name[g_set.list_filter], s_gl_nvis);
				ui_text(c, px + w - 24 - ui_text_width(0.85f, have), py + 34, COL_DIM, 0.85f, have);
				{ // tab row: current tab highlighted
					int tbx = px + 16;
					for (int t = 0; t < s_gl_ntabs; t++) {
						const char *tn = s_tier_name[s_gl_tabs[t]];
						const int tw = ui_text_width(0.95f, tn) + 20;
						if (tbx + tw > px + w - 16)
							break;
						if (t == s_gl_tab)
							ui_rect(c, tbx, py + TITLE_H - 4, tw, 32, COL_SEL);
						ui_text(c, tbx + 10, py + TITLE_H + 20, t == s_gl_tab ? COL_TEXT : COL_DIM, 0.95f, tn);
						tbx += tw + 8;
					}
				}
				if (!n && g_set.list_filter == RS12_LIST_FAVORITES)
					text_keys(c, px + 48, py + TITLE_H + tab_h + 22, COL_DIM, "No favorites. " G_SQU ": add, SELECT: filter");
				else if (!n)
					ui_text(c, px + 48, py + TITLE_H + tab_h + 22, COL_DIM, 1.0f, "No playable games. SELECT: show all");
				for (int i = top; i < top + vis; i++) {
					const int ty = py + TITLE_H + tab_h + (i - top) * LINE_H + 22;
					const int okg = gl_playable(idx[i]);
					const uint32_t col = (i == sel) ? (okg ? COL_SEL : COL_GREY_SEL) : (okg ? COL_TEXT : COL_GREY);
					if (i == sel)
						ui_text(c, px + 16, ty, col, 1.0f, ">");
					if (rs12_fav_is(s_gl[idx[i]]->name)) // favorite mark
						ui_text(c, px + 32, ty, COL_WARN, 1.0f, "*");
					ui_text_clip(c, px + 48, ty, col, 1.0f, s_gl[idx[i]]->title, px + 48, px + w - 24);
					if (i == sel) { // titles longer than the cell scroll (only this row is redrawn each frame below)
						if (mq_sel != idx[i])
							mq_t = 0;
						mq_sel = idx[i];
						mq_y = ty;
						mq_px = px;
						mq_w = w;
						mq_tw = ui_text_width(1.0f, s_gl[idx[i]]->title);
					}
				}
				if (!n)
					mq_sel = -1;
				char foot[160];
				snprintf(foot, sizeof(foot), "%sLeft/Right: page   %s: play   " G_TRI ": about", s_gl_ntabs > 1 ? "L/R: type   " : "",
					o_ok ? G_CIR : G_CRO);
				text_keys(c, px + 24, py + h - 28, COL_DIM, foot);
				snprintf(foot, sizeof(foot), "START: options   " G_SQU ": favorite   SELECT: %s",
					s_gl_filter_name[wrap(g_set.list_filter + 1, RS12_LIST_FILTER_COUNT)]);
				text_keys(c, px + 24, py + h - 8, COL_DIM, foot);
				if (msg[0]) { // notice box (over the list)
					const int mw = 720, mh = 150, mx = (SCREEN_W - mw) / 2, my = (SCREEN_H - mh) / 2;
					ui_rect(c, mx - 2, my - 2, mw + 4, mh + 4, COL_WARN);
					ui_rect(c, mx, my, mw, mh, COL_PANEL);
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
				if (s_list_mode) // options (over the list)
					menu_paint(c, 1);
			}
			ui_commit(c);
		} else if (!about && !msg[0] && !s_list_mode && mq_sel >= 0 && mq_tw > mq_w - 72) {
			// scrolling title: 1 s pause -> 60 px/s to the end -> 1 s pause -> from the start
			const int avail = mq_w - 72, over = mq_tw - avail, hold = 60;
			const int cyc = hold + over + hold;
			const int t = mq_t++ % cyc;
			const int off = t < hold ? 0 : t < hold + over ? t - hold : over;
			const int x0 = mq_px + 48, x1 = mq_px + mq_w - 24;
			ui_clear_rect(c, x0, mq_y - 22, x1 - x0, LINE_H - 2);
			ui_rect(c, x0, mq_y - 22, x1 - x0, LINE_H - 2, COL_PANEL);
			ui_text_clip(c, x0 - off, mq_y, gl_playable(mq_sel) ? COL_SEL : COL_GREY_SEL, 1.0f, s_gl[mq_sel]->title, x0, x1);
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
