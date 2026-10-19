// In-game menu, start screen game select, notices, FPS box, loading bar. Ported from ReST-V vita/src/frontend/menu.h
// (common UI of the Re series of Vita emulators): same layout, controls and features, red theme.
#ifndef RS12_MENU_H
#define RS12_MENU_H

#include <stdint.h>

// ---- settings (ux0:data/resys12/settings.cfg, per-game cfg/<set>.cfg, favorites favorites.cfg; key=value) ----
// Vita buttons that can be picked (cycled with left/right on the Controls page). RS12_KEY_NONE = none
enum {
	RS12_KEY_CROSS, RS12_KEY_CIRCLE, RS12_KEY_SQUARE, RS12_KEY_TRIANGLE,
	RS12_KEY_L, RS12_KEY_R, RS12_KEY_SELECT, RS12_KEY_START,
	RS12_KEY_COUNT
};
#define RS12_KEY_NONE RS12_KEY_COUNT
// game inputs to map: buttons 1-6 (System 12 JAMMA), coin, start
enum { RS12_ACT_B1, RS12_ACT_B2, RS12_ACT_B3, RS12_ACT_B4, RS12_ACT_B5, RS12_ACT_B6, RS12_ACT_COIN, RS12_ACT_START, RS12_ACT_COUNT };
#define RS12_GAME_BUTTONS 6
enum { RS12_STICK_OFF, RS12_STICK_LEFT, RS12_STICK_RIGHT, RS12_STICK_BOTH, RS12_STICK_COUNT };
// gun / wheel / lever games: the analog stick that aims or steers, and how a gun follows it
enum { RS12_AIM_LEFT, RS12_AIM_RIGHT, RS12_AIM_STICK_COUNT };
enum { RS12_AIM_POSITION, RS12_AIM_MOVE, RS12_AIM_MODE_COUNT };
// screen: 4:3, full, integer
enum { RS12_ASPECT_4_3, RS12_ASPECT_FULL, RS12_ASPECT_INT, RS12_ASPECT_COUNT };
enum { RS12_FILTER_SMOOTH, RS12_FILTER_SHARP, RS12_FILTER_COUNT };
// scanlines (kept in the settings file for later; no shader yet, so the Display page does not show them)
enum { RS12_SCAN_OFF, RS12_SCAN_CRT_SOFT, RS12_SCAN_CRT_SHARP, RS12_SCAN_PX2, RS12_SCAN_PX3, RS12_SCAN_PX4, RS12_SCAN_COUNT };
enum { RS12_SCAN_LIGHT, RS12_SCAN_MEDIUM, RS12_SCAN_HEAVY, RS12_SCAN_STRENGTH_COUNT };
// outside the game screen (4:3 / integer): black, artwork (ux0:data/resys12/artwork/<set>.png), solid colour
enum { RS12_BORDER_BLACK, RS12_BORDER_ARTWORK, RS12_BORDER_COLOR, RS12_BORDER_COUNT };
enum { RS12_BCOL_NAVY, RS12_BCOL_GRAY, RS12_BCOL_MAROON, RS12_BCOL_GREEN, RS12_BCOL_PURPLE, RS12_BCOL_BRICK, RS12_BCOL_COUNT };
// menu confirm button (the other one is back)
enum { RS12_CONFIRM_CROSS, RS12_CONFIRM_CIRCLE, RS12_CONFIRM_COUNT };
// game list filter (SELECT cycles in this order)
enum { RS12_LIST_PLAYABLE, RS12_LIST_ALL, RS12_LIST_FAVORITES, RS12_LIST_FILTER_COUNT };
enum { RS12_TURBO_FAST, RS12_TURBO_MEDIUM, RS12_TURBO_SLOW, RS12_TURBO_SPEED_COUNT };
// combo keys (as in Tekken 6): a Vita button (Square, Triangle, Cross, Circle, L, R) can act as two other Vita buttons
// pressed together. RS12_COMBO_KEYS = keys RS12_KEY_CROSS..RS12_KEY_R
#define RS12_COMBO_KEYS 6

typedef struct {
	int fps;           // FPS display
	int aspect;        // RS12_ASPECT_*
	int filter;        // RS12_FILTER_*
	int scan_type;     // RS12_SCAN_* (not applied yet)
	int scan_strength; // RS12_SCAN_LIGHT..HEAVY (not applied yet)
	int border;        // RS12_BORDER_*
	int border_color;  // RS12_BCOL_*
	int keymap[RS12_ACT_COUNT]; // RS12_KEY_* (buttons 2-6 may be RS12_KEY_NONE)
	int stick;         // RS12_STICK_* (analog stick used as lever)
	int aim_stick;     // RS12_AIM_LEFT / RIGHT: the stick that aims a gun
	int aim_mode;      // RS12_AIM_POSITION (stick position = screen position) / MOVE (stick moves the aim)
	int gun_sens;      // gun aim sensitivity 1..10 (10 = direct)
	int crosshair;     // show the gun aim
	int wheel_stick;   // RS12_AIM_LEFT / RIGHT: the stick that steers (the d-pad steers too)
	int lever_stick;   // RS12_AIM_LEFT / RIGHT: the stick that moves a lever (the d-pad moves it too)
	int gun_fire;      // RS12_KEY_*: gun trigger (also the game's own button mapping), None = off
	int gun_sensor;    // RS12_KEY_*: golgo13 scope sensor
	int gas_key;       // RS12_KEY_*: wheel games' gas pedal
	int brake_key;     // RS12_KEY_*: wheel games' brake pedal
	int turbo_key;     // Vita button that triggers turbo, RS12_KEY_NONE = off
	int turbo_btn;     // game button to turbo, 1..6
	int turbo_speed;   // RS12_TURBO_*
	int combo[RS12_COMBO_KEYS]; // per Vita key: 0 = normal, else the two keys it presses (bit = RS12_KEY_*)
	int confirm;       // RS12_CONFIRM_*
	int list_filter;   // RS12_LIST_*
} rs12_settings;
extern rs12_settings g_set;
// test mode: the board's service DIP switch is on (menu "Test mode (DIP switch)" switches it on/off; not saved)
extern int rs12_dip_service;

// frames per turbo period (press + release): Fast 4, Medium 6, Slow 8
static inline int rs12_turbo_period(int speed) { return speed == 0 ? 4 : speed == 1 ? 6 : 8; }
void rs12_settings_defaults(void);
void rs12_settings_default_keys(void);
void rs12_settings_load(void);
void rs12_settings_save(void);
uint32_t rs12_key_code(int key);       // RS12_KEY_* -> SCE_CTRL_* (0 for RS12_KEY_NONE)
const char *rs12_key_name(int key);    // "Cross" ... "None"
// per-game settings (cfg/<set>.cfg). If present, rs12_settings_load_game overrides the global values.
// While active, rs12_settings_save writes those items only to the game file and keeps global values in the global file
void rs12_settings_load_game(const char *set);
int rs12_settings_has_game(void);
int rs12_settings_save_game(void);     // current values as this game's settings (1 on success)
int rs12_settings_delete_game(void);   // delete game settings and fall back to global (1 on success)
void rs12_settings_use_global(void);   // game list options: drop game settings and edit global values only
// favorites (favorites.cfg, one set name per line)
int rs12_fav_is(const char *set);
void rs12_fav_toggle(const char *set);
// apply current settings to video output (screen, filter, border)
void rs12_settings_apply_video(void);

typedef enum {
	RS12_MENU_STAY,
	RS12_MENU_CLOSE,
	RS12_MENU_RESET,
	RS12_MENU_SAVE,        // save to current slot (menu stays open)
	RS12_MENU_LOAD,        // load current slot, then close
	RS12_MENU_SELECT_GAME, // relaunch the app to game select
	RS12_MENU_EXIT,
} rs12_menu_result;

void rs12_menu_init(const char *set, const char *title, int buttons); // after the game is known (buttons = game buttons 2..6)
int rs12_menu_buttons(void); // button count of the current game
int rs12_game_buttons(const char *set); // button count used for a set (from its MAME input port type)
int rs12_menu_should_open(uint32_t buttons);              // 1 at the moment SELECT+START is pressed
rs12_menu_result rs12_menu_update(uint32_t buttons);
void rs12_menu_draw(void);  // redraws only when changed
void rs12_menu_hide(void);
int rs12_menu_slot(void);
void rs12_menu_set_game(const char *set);   // the running set (its save state slots)
void rs12_menu_states_changed(void);        // a state was saved: read the slots again

// loading screen: game name + bar (0-1000)
void rs12_loading_title(const char *title);
void rs12_loading_progress(int permille);
void rs12_loading_done(void);

void rs12_osd_show(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void rs12_osd_tick(void);                   // every frame (hides after 2 s)
void rs12_fps_update(float fps, unsigned frame); // every frame (display refreshed every 0.1 s)

// test (ux0:data/resys12/menutest): draw each menu page, notice and FPS box to ux0:data/resys12/menu_<name>.ppm
// (canvas only) and screen_<name>.ppm (whole screen read back from the GPU)
void rs12_menu_selftest(void);

// Start screen game select: tabs by support tier, SELECT filter (Playable/All/Favorites), Square favorite, START options,
// Triangle About. The cursor starts on the last game. Returns the picked set name,
// or NULL when *quit becomes set (exit request)
const char *rs12_game_select(const char *initial_set, const char *rom_dir, volatile int *quit);

#endif
