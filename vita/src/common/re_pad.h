// re_pad: controllers and players, shared by the Re series of Vita emulators (ReSys12, ReST-V, ReiREM).
// The same file is kept in each project (vita/src/common/re_pad.{c,h}); change it in all of them together.
//
// Port 0 is the Vita's own controls (on a PS TV: every controller together). Ports 1-4 are the controllers a PS TV
// pairs (DualShock 3 / 4, wired or Bluetooth), or a handheld Vita with a plugin that gives them their own ports
// (MiniVitaTV, ds34vita). A player reads one port: Auto picks the Vita for 1P and the first controller for 2P on a
// handheld Vita, the controllers in order on a PS TV; a player can also be set to one port.
// Controller buttons come back as Vita buttons: L1 = L, R1 = R; L2 / R2 / L3 / R3 as the bits below (keys of their own;
// re_pad_read_all, for the menus, also gives L2 / R2 as L / R).
#ifndef RE_PAD_H
#define RE_PAD_H

#include <stdint.h>
#include <psp2/ctrl.h>

#define RE_PAD_PLAYERS 4
#define RE_PAD_PORTS 5
// a controller's L2 / R2 / L3 / R3 (bits SceCtrl does not use; the Vita has no such buttons)
#define RE_PAD_L2 0x01000000u
#define RE_PAD_R2 0x02000000u
#define RE_PAD_L3 0x04000000u
#define RE_PAD_R3 0x08000000u

// a player's controller (saved in the settings as this number)
enum {
	RE_PAD_SRC_AUTO, RE_PAD_SRC_VITA, RE_PAD_SRC_PORT1, RE_PAD_SRC_PORT2, RE_PAD_SRC_PORT3, RE_PAD_SRC_PORT4,
	RE_PAD_SRC_NONE, RE_PAD_SRC_COUNT
};

void re_pad_init(void);  // sampling modes, model, first look at the ports
// the RePad kernel plugin (Bluetooth controllers) at path, loaded unless it already runs (installed in ur0:tai, or
// loaded by another Re app since the last reboot). 1 = it was running, else taiLoadStartKernelModule's result
int re_pad_load_plugin(const char *path);
int re_pad_plugin(void); // RePad is running (re_pad_init / re_pad_load_plugin found it)

// input engine (a setting of the app): Native = the system's controls and whatever controller plugin is installed;
// RePad = the RePad plugin bundled with the app. Both ways at once: RePad is loaded (or told to serve again), or told
// to stand aside (it stays loaded until the Vita restarts: unloading it while Bluetooth requests are in flight is not
// safe, but standing aside leaves the controls exactly as without it)
// VitaControl = that plugin (installed by the user; offered only when a plugin list names it): RePad stands aside as for
// Native, and the app checks VitaControl really runs (else a restart, its list line switched on first if needed)
enum { RE_PAD_ENGINE_NATIVE, RE_PAD_ENGINE_REPAD, RE_PAD_ENGINE_VITACONTROL, RE_PAD_ENGINE_COUNT };
const char *re_pad_engine_name(int engine);         // "Native", "RePad", "VitaControl"
// the engine after `engine` in direction dir (-1 / +1), VitaControl skipped when no plugin list offers it
int re_pad_engine_step(int engine, int dir);
// the plugin's path (the app's module folder) and the engine from the settings, at start. Returns the engine as the
// Vita is (keep it in the settings): Native becomes VitaControl when a plugin list loads VitaControl, VitaControl
// becomes Native when no list names it
int re_pad_engine_init(int engine, const char *path);
int re_pad_engine_fit(int engine); // that fitting alone (for a settings load)
// the engine changed in the menu (0)
int re_pad_engine_set(int engine);
// the engine row's value: "RePad", "Native", "RePad (blocked)" (another controller plugin keeps RePad out)
const char *re_pad_engine_text(int engine);

// VitaControl (another Bluetooth controller plugin) keeps RePad out. These look at and change the plugin lists
// (ux0:tai/config.txt and ur0:tai/config.txt): 1 if one of them loads VitaControl
int re_pad_vitacontrol_listed(void);
// comment its lines out in both lists (each list first copied to config.txt.repad_bak). Lines changed, < 0 on an error.
// It takes a restart of the Vita (VitaControl stays loaded until then)
int re_pad_vitacontrol_off(void);
// how the lists name VitaControl: 0 not at all, 1 a line loads it, 2 only commented-out lines whose file exists
// (then it can be switched on)
enum { RE_PAD_VC_NONE, RE_PAD_VC_ON, RE_PAD_VC_OFF };
int re_pad_vitacontrol_state(void);
// switch on the first commented-out line whose file exists (list copied to config.txt.repad_bak first). 1 done
int re_pad_vitacontrol_on(void);
// VitaControl runs now (since the last restart). Found out by loading RePad, which refuses to start beside it (if
// RePad then starts, it is told to stand aside at once)
int re_pad_vitacontrol_running(void);
void re_pad_poll(void);  // once a frame: reads the ports the players use (and looks for new controllers twice a second)

void re_pad_set_source(int player, int src);
int re_pad_source(int player);
int re_pad_port(int player);                     // port the player reads now, -1 = none
int re_pad_read(int player, SceCtrlData *pad);   // that port's input from the last poll; 0 (nothing held) if none
void re_pad_read_all(SceCtrlData *pad);          // every port together (menus)

int re_pad_is_tv(void);
int re_pad_connected(int port);
int re_pad_count(void);                          // controllers on ports 1-4
// a player's Port row in the menus: as many players as devices to play with (the Vita's own controls count; a player
// whose controller is away reads another free device meanwhile, never shown as none)
int re_pad_player_shown(int player);
int re_pad_devices(void);                        // devices to play with: the Vita's own controls (not a PS TV) + controllers, at least 1
int re_pad_changed(void);                        // 1 once after a controller came or went
const char *re_pad_port_name(int port);          // "Vita", "PS TV", "DualShock 3", "DualShock 4", "Controller"
const char *re_pad_source_name(int src);         // "Auto", "Vita", "Controller 1".., "None"
// a source with the device on it, for a choice list: "Controller 1: Switch Pro", "Controller 2 (none)", "Vita", ...
const char *re_pad_source_text(int src, char *buf, int len);
// the sources a player can pick now: Auto, Vita, the connected controllers, None (a controller set by hand that is away
// is not listed: *cur is then Auto's). Returns the count (at most RE_PAD_SRC_COUNT); *cur = the player's in the list
int re_pad_sources(int player, int *src, int *cur);
// entry idx of re_pad_sources set as the player's source and returned. Set even when it is the one shown: a controller
// set by hand that is away shows as Auto, and picking Auto then drops it (it does not come back to the player with it)
int re_pad_pick_source(int player, int idx);
// the player's source moved by dir (-1 / +1) through re_pad_sources, set and returned
int re_pad_step_source(int player, int dir);
// controls profiles (keys, stick, turbo, combo keys) belong to a device, not to a player: the Vita's own controls, and each
// kind of controller (a second one of the same kind, on a later port, has its own). A player uses the profile of the
// device it reads now, so a controller keeps its keys when it moves to another player
#define RE_PAD_PROFILES 13
int re_pad_profile(int port);                    // the profile of the device on a port (0: the Vita's own / PS TV)
int re_pad_player_profile(int player);           // the profile of the device the player reads now (0 if none)
const char *re_pad_profile_key(int prof);        // its name in the settings files: "vita", "ds4", "switchpro", "switchprob"..
const char *re_pad_profile_name(int prof);       // for the menus: "Vita", "Switch Pro", "Switch Pro (2)"..
// "Auto: DualShock 4 (1)", "Controller 2 (none)", ... for a menu row
const char *re_pad_player_text(int player, char *buf, int len);

#endif
