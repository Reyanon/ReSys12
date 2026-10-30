// re_pad: controllers and players (see re_pad.h). Shared by the Re series of Vita emulators: keep the copies the same.
#include "common/re_pad.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <psp2/io/fcntl.h>
#include <psp2/io/stat.h>
#include <psp2/kernel/processmgr.h>
#include <psp2/kernel/sysmem.h>
#include <taihen.h>

#define RESCAN_POLLS 30 // look for new controllers every 30 polls (twice a second)

static uint8_t s_type[RE_PAD_PORTS];     // SCE_CTRL_TYPE_* of each port (0 = none; port 0 always there)
// real kind of ports 1-4 when the RePad plugin tells it (it reports every controller as a DualShock 4 so that apps take
// it): its spare port info bytes unk[0..3] after the marker "RPAD" in unk[4..7]. 1 DS3, 2 DS4, 3 DualSense,
// 4 Xbox One, 5 Switch Pro; 0 = not told
static uint8_t s_kind[RE_PAD_PORTS];
static SceCtrlData s_data[RE_PAD_PORTS]; // last poll
static int s_src[RE_PAD_PLAYERS];
static int s_port[RE_PAD_PLAYERS];
static int s_tv, s_changed;
static unsigned s_polls;

static void clear(SceCtrlData *pad)
{
	memset(pad, 0, sizeof(*pad));
	pad->lx = pad->ly = pad->rx = pad->ry = 128;
}

// controller buttons as Vita buttons: L1 = L, R1 = R; L2 / R2 / L3 / R3 as RE_PAD_L2.. (keys of their own)
static uint32_t fold(uint32_t b)
{
	uint32_t o = b & ~(uint32_t)(SCE_CTRL_L1 | SCE_CTRL_R1 | SCE_CTRL_L2 | SCE_CTRL_R2 | SCE_CTRL_L3 | SCE_CTRL_R3);
	if (b & SCE_CTRL_L1) o |= SCE_CTRL_LTRIGGER;
	if (b & SCE_CTRL_R1) o |= SCE_CTRL_RTRIGGER;
	if (b & SCE_CTRL_L2) o |= RE_PAD_L2;
	if (b & SCE_CTRL_R2) o |= RE_PAD_R2;
	if (b & SCE_CTRL_L3) o |= RE_PAD_L3;
	if (b & SCE_CTRL_R3) o |= RE_PAD_R3;
	return o;
}

// players -> ports. Ports set by hand first; Auto players then take the free ports in order: the Vita, then the
// controllers (handheld), or the controllers only (PS TV; the Vita port if there are none)
static void resolve(void)
{
	int used[RE_PAD_PORTS] = { 0 };
	const int shown = re_pad_devices(); // (a player without a row takes nothing: 2P set to the Vita while there is no
	                                    // controller would leave 1P without one)
	for (int p = 0; p < RE_PAD_PLAYERS; p++) {
		const int s = s_src[p];
		s_port[p] = -1;
		if (p >= shown)
			continue;
		if (s == RE_PAD_SRC_VITA)
			s_port[p] = 0;
		else if (s >= RE_PAD_SRC_PORT1 && s <= RE_PAD_SRC_PORT4 && s_type[s - RE_PAD_SRC_PORT1 + 1])
			s_port[p] = s - RE_PAD_SRC_PORT1 + 1;
		if (s_port[p] >= 0)
			used[s_port[p]] = 1;
	}
	int order[RE_PAD_PORTS], n = 0;
	if (!s_tv || !re_pad_count())
		order[n++] = 0;
	for (int i = 1; i < RE_PAD_PORTS; i++)
		if (s_type[i])
			order[n++] = i;
	int next = 0;
	for (int p = 0; p < shown; p++) {
		// Auto, and a controller set by hand that is away (meanwhile the next free device, the Vita's own controls too:
		// the setting stays, and the controller is the player's again when it comes back on its port)
		const int s = s_src[p];
		if (s != RE_PAD_SRC_AUTO && !(s >= RE_PAD_SRC_PORT1 && s <= RE_PAD_SRC_PORT4 && s_port[p] < 0))
			continue;
		while (next < n && used[order[next]])
			next++;
		if (next < n)
			s_port[p] = order[next++];
	}
}

static void scan(void)
{
	SceCtrlPortInfo pi;
	memset(&pi, 0, sizeof(pi));
	uint8_t t[RE_PAD_PORTS] = { 0 };
	uint8_t k[RE_PAD_PORTS] = { 0 };
	if (sceCtrlGetControllerPortInfo(&pi) >= 0)
		for (int i = 1; i < RE_PAD_PORTS; i++) {
			if (!memcmp(&pi.unk[4], "RPAD", 4))
				k[i] = pi.unk[i - 1];
			// a port counts only if it can be read (a controller plugin may report a controller on a port that every
			// read refuses: it adds the controller to port 0 only)
			SceCtrlData d;
			if (pi.port[i] && sceCtrlPeekBufferPositiveExt2(i, &d, 1) >= 0)
				t[i] = pi.port[i];
		}
	t[0] = s_tv ? SCE_CTRL_TYPE_VIRT : SCE_CTRL_TYPE_PHY;
	if (memcmp(t, s_type, sizeof(t)) || memcmp(k, s_kind, sizeof(k))) {
		memcpy(s_type, t, sizeof(t));
		memcpy(s_kind, k, sizeof(k));
		s_changed = 1;
		resolve();
	}
}

// RePad marks the port info it reports ("RPAD" in unk[4..7]); unk[8] bit 0: it serves the controllers (it can be told
// to stand aside without unloading: see the commands below)
#define PLUGIN_LOADED 1
#define PLUGIN_ON 2
static int plugin_state(void)
{
	SceCtrlPortInfo pi;
	memset(&pi, 0, sizeof(pi));
	if (sceCtrlGetControllerPortInfo(&pi) < 0 || memcmp(&pi.unk[4], "RPAD", 4))
		return 0;
	return PLUGIN_LOADED | ((pi.unk[8] & 0x01) ? PLUGIN_ON : 0);
}
// a command to RePad: a read of its command port, which it answers itself (REPAD_CMD_OFF 1 / REPAD_CMD_ON 2)
#define REPAD_CMD_PORT 0x5250
static void plugin_command(int cmd)
{
	SceCtrlData d;
	sceCtrlPeekBufferPositive2(REPAD_CMD_PORT, &d, cmd);
}

static int s_plugin;
int re_pad_plugin(void) { return s_plugin; }

int re_pad_load_plugin(const char *path)
{
	int st = plugin_state();
	int r = 1;
	if (!(st & PLUGIN_LOADED))
		r = taiLoadStartKernelModule(path, 0, NULL, 0);
	else if (!(st & PLUGIN_ON))
		plugin_command(2); // (it was told to stand aside: on again)
	s_plugin = (plugin_state() & PLUGIN_ON) != 0;
	s_polls = RESCAN_POLLS - 1; // (look at the ports again on the next poll)
	return r;
}

static char s_plugin_path[96];

const char *re_pad_engine_name(int engine)
{
	return engine == RE_PAD_ENGINE_REPAD ? "RePad" : engine == RE_PAD_ENGINE_VITACONTROL ? "VitaControl" : "Native";
}

int re_pad_engine_step(int engine, int dir)
{
	const int vc = re_pad_vitacontrol_state() != RE_PAD_VC_NONE; // (offered only when a plugin list names it)
	for (int k = 0; k < RE_PAD_ENGINE_COUNT; k++) {
		engine = ((engine + dir) % RE_PAD_ENGINE_COUNT + RE_PAD_ENGINE_COUNT) % RE_PAD_ENGINE_COUNT;
		if (engine != RE_PAD_ENGINE_VITACONTROL || vc)
			break;
	}
	return engine;
}

int re_pad_engine_init(int engine, const char *path)
{
	snprintf(s_plugin_path, sizeof(s_plugin_path), "%s", path);
	engine = re_pad_engine_fit(engine);
	re_pad_engine_set(engine);
	return engine;
}

int re_pad_engine_fit(int engine)
{
	// Native while a plugin list loads VitaControl is VitaControl (that is what runs); VitaControl that no list names
	// any more is Native
	const int vc = re_pad_vitacontrol_state();
	if (engine == RE_PAD_ENGINE_NATIVE && vc == RE_PAD_VC_ON)
		return RE_PAD_ENGINE_VITACONTROL;
	if (engine == RE_PAD_ENGINE_VITACONTROL && vc == RE_PAD_VC_NONE)
		return RE_PAD_ENGINE_NATIVE;
	return engine;
}

int re_pad_engine_set(int engine)
{
	if (engine == RE_PAD_ENGINE_REPAD) {
		if (s_plugin_path[0])
			re_pad_load_plugin(s_plugin_path);
	} else if (plugin_state() & PLUGIN_ON) {
		plugin_command(1); // Native: RePad stays loaded but stands aside (the controls as they are)
		s_polls = RESCAN_POLLS - 1;
	}
	s_plugin = (plugin_state() & PLUGIN_ON) != 0;
	return 0;
}

const char *re_pad_engine_text(int engine)
{
	if (engine == RE_PAD_ENGINE_REPAD)
		return s_plugin ? "RePad" : "RePad (blocked)";
	return re_pad_engine_name(engine);
}

// ---- VitaControl in the plugin lists ----
static const char *const s_tai_lists[2] = { "ux0:tai/config.txt", "ur0:tai/config.txt" };
#define TAI_LIST_MAX (64 * 1024)

// the whole list, NUL-terminated (NULL if there is none); free it
static char *read_list(const char *path, int *len)
{
	const SceUID fd = sceIoOpen(path, SCE_O_RDONLY, 0);
	if (fd < 0)
		return NULL;
	char *b = malloc(TAI_LIST_MAX + 1);
	int n = b ? sceIoRead(fd, b, TAI_LIST_MAX) : -1;
	sceIoClose(fd);
	if (n < 0 || n >= TAI_LIST_MAX) { // (not read, or longer than any plugin list)
		free(b);
		return NULL;
	}
	b[n] = 0;
	*len = n;
	return b;
}

static int write_list(const char *path, const char *b, int len)
{
	const SceUID fd = sceIoOpen(path, SCE_O_WRONLY | SCE_O_CREAT | SCE_O_TRUNC, 0777);
	if (fd < 0)
		return fd;
	const int n = sceIoWrite(fd, b, len);
	sceIoClose(fd);
	return n == len ? 0 : -1;
}

// a line of the list that loads VitaControl: not a comment, names vitacontrol.skprx (any case)
static int vc_line(const char *s, const char *e)
{
	while (s < e && (*s == ' ' || *s == '\t'))
		s++;
	if (s >= e || *s == '#')
		return 0;
	static const char name[] = "vitacontrol.skprx";
	const int nl = sizeof(name) - 1;
	for (const char *p = s; p + nl <= e; p++) {
		int k = 0;
		while (k < nl && (p[k] | 0x20) == name[k])
			k++;
		if (k == nl)
			return 1;
	}
	return 0;
}

int re_pad_vitacontrol_listed(void)
{
	for (int i = 0; i < 2; i++) {
		int len;
		char *b = read_list(s_tai_lists[i], &len);
		if (!b)
			continue;
		int found = 0;
		for (char *s = b; *s && !found;) {
			char *e = strchr(s, '\n');
			if (!e)
				e = s + strlen(s);
			found = vc_line(s, e);
			s = *e ? e + 1 : e;
		}
		free(b);
		if (found)
			return 1;
	}
	return 0;
}

// a commented-out VitaControl line's path (the line without its '#'s and spaces) into path; 1 if its file exists
static int vc_off_line_file(const char *s, const char *e, char *path, int len)
{
	while (s < e && (*s == ' ' || *s == '\t' || *s == '#'))
		s++;
	int n = 0;
	while (s < e && *s != '\r' && *s != '\n' && n < len - 1)
		path[n++] = *s++;
	while (n > 0 && (path[n - 1] == ' ' || path[n - 1] == '\t'))
		n--;
	path[n] = 0;
	SceIoStat st;
	return n > 0 && sceIoGetstat(path, &st) >= 0;
}

// a line naming vitacontrol.skprx, commented out or not
static int vc_named(const char *s, const char *e)
{
	while (s < e && (*s == ' ' || *s == '\t' || *s == '#'))
		s++;
	return vc_line(s, e);
}

// the lists are read again at most every 2 s (menus ask every frame); changing them forgets the last look
static int s_vc_state = -1;
static SceUInt64 s_vc_state_t;

int re_pad_vitacontrol_state(void)
{
	const SceUInt64 now = sceKernelGetProcessTimeWide();
	if (s_vc_state >= 0 && now - s_vc_state_t < 2000000)
		return s_vc_state;
	int state = RE_PAD_VC_NONE;
	for (int i = 0; i < 2 && state != RE_PAD_VC_ON; i++) {
		int len;
		char *b = read_list(s_tai_lists[i], &len);
		if (!b)
			continue;
		for (char *s = b; *s;) {
			char *e = strchr(s, '\n');
			if (!e)
				e = s + strlen(s);
			char path[128];
			if (vc_line(s, e)) {
				state = RE_PAD_VC_ON;
				break;
			}
			if (vc_named(s, e) && vc_off_line_file(s, e, path, sizeof(path)))
				state = RE_PAD_VC_OFF;
			s = *e ? e + 1 : e;
		}
		free(b);
	}
	s_vc_state = state;
	s_vc_state_t = now;
	return state;
}

int re_pad_vitacontrol_on(void)
{
	s_vc_state = -1;
	for (int i = 0; i < 2; i++) {
		int len;
		char *b = read_list(s_tai_lists[i], &len);
		if (!b)
			continue;
		for (char *s = b; *s;) {
			char *e = strchr(s, '\n');
			if (!e)
				e = s + strlen(s);
			char path[128];
			if (!vc_line(s, e) && vc_named(s, e) && vc_off_line_file(s, e, path, sizeof(path))) {
				// the list with that line's '#'s taken off
				char *q = s;
				while (q < e && (*q == ' ' || *q == '\t' || *q == '#'))
					q++;
				char *o = malloc(len + 1);
				int r = -1;
				if (o) {
					const int head = (int)(s - b), cut = (int)(q - s);
					memcpy(o, b, head);
					memcpy(o + head, q, len - head - cut);
					char bak[64];
					snprintf(bak, sizeof(bak), "%s.repad_bak", s_tai_lists[i]);
					if (write_list(bak, b, len) >= 0 && write_list(s_tai_lists[i], o, len - cut) >= 0)
						r = 1;
					free(o);
				}
				free(b);
				return r;
			}
			s = *e ? e + 1 : e;
		}
		free(b);
	}
	return 0;
}

int re_pad_vitacontrol_running(void)
{
	if (plugin_state() & PLUGIN_LOADED)
		return 0; // (RePad runs: it would not have started beside VitaControl)
	SceIoStat st;
	if (!s_plugin_path[0] || sceIoGetstat(s_plugin_path, &st) < 0)
		return 0;
	taiLoadStartKernelModule(s_plugin_path, 0, NULL, 0); // (it refuses to start beside VitaControl)
	const int ps = plugin_state();
	if (!(ps & PLUGIN_LOADED))
		return 1;
	if (ps & PLUGIN_ON)
		plugin_command(1); // (it started: VitaControl is not there. Standing aside, as the engine is not RePad)
	s_plugin = 0;
	s_polls = RESCAN_POLLS - 1;
	return 0;
}

int re_pad_vitacontrol_off(void)
{
	s_vc_state = -1;
	int changed = 0;
	for (int i = 0; i < 2; i++) {
		int len;
		char *b = read_list(s_tai_lists[i], &len);
		if (!b)
			continue;
		// the new list: every VitaControl line with '#' in front
		char *o = malloc(len + 256 + 1);
		int ol = 0, here = 0;
		for (char *s = b; o && *s;) {
			char *e = strchr(s, '\n');
			if (!e)
				e = s + strlen(s);
			if (vc_line(s, e) && here < 256) {
				o[ol++] = '#';
				here++;
			}
			const int n = (int)(*e ? e + 1 - s : e - s);
			memcpy(o + ol, s, n);
			ol += n;
			s += n;
		}
		if (o && here) {
			char bak[64];
			snprintf(bak, sizeof(bak), "%s.repad_bak", s_tai_lists[i]);
			if (write_list(bak, b, len) < 0 || write_list(s_tai_lists[i], o, ol) < 0) {
				free(o);
				free(b);
				return -1;
			}
			changed += here;
		}
		free(o);
		free(b);
	}
	return changed;
}

void re_pad_init(void)
{
	sceCtrlSetSamplingMode(SCE_CTRL_MODE_ANALOG);
	sceCtrlSetSamplingModeExt(SCE_CTRL_MODE_ANALOG);
	s_tv = sceKernelGetModelForCDialog() == SCE_KERNEL_MODEL_VITATV;
	for (int i = 0; i < RE_PAD_PORTS; i++)
		clear(&s_data[i]);
	memset(s_type, 0xff, sizeof(s_type)); // (the first scan always counts as a change)
	scan();
	s_changed = 0;
}

void re_pad_poll(void)
{
	if (++s_polls >= RESCAN_POLLS) {
		s_polls = 0;
		scan();
	}
	for (int i = 0; i < RE_PAD_PORTS; i++) {
		SceCtrlData *d = &s_data[i];
		if (!s_type[i]) {
			clear(d);
			continue;
		}
		int r;
		if (i == 0 && !s_tv)
			r = sceCtrlPeekBufferPositive(0, d, 1); // (the handheld's own L / R; plugins that add a controller to port 0)
		else
			r = sceCtrlPeekBufferPositiveExt2(i, d, 1);
		if (r < 0)
			clear(d);
		else if (i || s_tv)
			d->buttons = fold(d->buttons);
	}
}

void re_pad_set_source(int player, int src)
{
	if (player < 0 || player >= RE_PAD_PLAYERS)
		return;
	s_src[player] = src >= 0 && src < RE_PAD_SRC_COUNT ? src : RE_PAD_SRC_AUTO;
	resolve();
}

int re_pad_source(int player) { return player >= 0 && player < RE_PAD_PLAYERS ? s_src[player] : RE_PAD_SRC_NONE; }
int re_pad_port(int player) { return player >= 0 && player < RE_PAD_PLAYERS ? s_port[player] : -1; }

int re_pad_read(int player, SceCtrlData *pad)
{
	const int port = re_pad_port(player);
	if (port < 0) {
		clear(pad);
		return 0;
	}
	*pad = s_data[port];
	return 1;
}

static int dev(int v) { return v < 128 ? 128 - v : v - 128; }

void re_pad_read_all(SceCtrlData *pad)
{
	clear(pad);
	int bl = 0, br = 0;
	for (int i = 0; i < RE_PAD_PORTS; i++) {
		const SceCtrlData *d = &s_data[i];
		pad->buttons |= d->buttons;
		if (d->buttons & RE_PAD_L2) // (in the menus L2 / R2 are L / R too)
			pad->buttons |= SCE_CTRL_LTRIGGER;
		if (d->buttons & RE_PAD_R2)
			pad->buttons |= SCE_CTRL_RTRIGGER;
		const int l = dev(d->lx) + dev(d->ly), r = dev(d->rx) + dev(d->ry);
		if (l > bl) {
			bl = l;
			pad->lx = d->lx;
			pad->ly = d->ly;
		}
		if (r > br) {
			br = r;
			pad->rx = d->rx;
			pad->ry = d->ry;
		}
	}
}

int re_pad_is_tv(void) { return s_tv; }
int re_pad_connected(int port) { return port >= 0 && port < RE_PAD_PORTS && s_type[port]; }

int re_pad_count(void)
{
	int n = 0;
	for (int i = 1; i < RE_PAD_PORTS; i++)
		n += s_type[i] != 0;
	return n;
}

int re_pad_player_shown(int player)
{
	return player < re_pad_devices(); // (as many players as devices to play with: the Vita's own controls count)
}

int re_pad_devices(void)
{
	const int n = (s_tv ? 0 : 1) + re_pad_count();
	return n > 0 ? n : 1;
}

int re_pad_changed(void)
{
	const int c = s_changed;
	s_changed = 0;
	return c;
}

const char *re_pad_port_name(int port)
{
	if (port == 0)
		return s_tv ? "PS TV" : "Vita";
	if (!re_pad_connected(port))
		return "None";
	static const char *const kind[] = { NULL, "DualShock 3", "DualShock 4", "DualSense", "Xbox One", "Switch Pro" };
	if (s_kind[port] && s_kind[port] < sizeof(kind) / sizeof(kind[0]))
		return kind[s_kind[port]];
	switch (s_type[port]) {
	case SCE_CTRL_TYPE_DS3: return "DualShock 3";
	case SCE_CTRL_TYPE_DS4: return "DualShock 4";
	default: return "Controller";
	}
}

// a port's device kind: 1 DualShock 3, 2 DualShock 4, 3 DualSense, 4 Xbox One, 5 Switch Pro, 6 another controller
static int port_kind(int port)
{
	if (s_kind[port] >= 1 && s_kind[port] <= 5)
		return s_kind[port];
	return s_type[port] == SCE_CTRL_TYPE_DS3 ? 1 : s_type[port] == SCE_CTRL_TYPE_DS4 ? 2 : 6;
}
int re_pad_profile(int port)
{
	if (port <= 0 || port >= RE_PAD_PORTS || !re_pad_connected(port))
		return 0;
	const int k = port_kind(port);
	int second = 0; // (a second controller of the same kind, on a later port)
	for (int q = 1; q < port; q++)
		if (re_pad_connected(q) && port_kind(q) == k)
			second = 1;
	return 1 + (k - 1) * 2 + second;
}
int re_pad_player_profile(int player)
{
	const int port = re_pad_port(player);
	return port < 0 ? 0 : re_pad_profile(port);
}
const char *re_pad_profile_key(int prof)
{
	static const char *const k[RE_PAD_PROFILES] = { "vita", "ds3", "ds3b", "ds4", "ds4b", "dualsense", "dualsenseb", "xbox",
		"xboxb", "switchpro", "switchprob", "pad", "padb" };
	return prof >= 0 && prof < RE_PAD_PROFILES ? k[prof] : "vita";
}
const char *re_pad_profile_name(int prof)
{
	static const char *const n[RE_PAD_PROFILES] = { "Vita", "DualShock 3", "DualShock 3 (2)", "DualShock 4", "DualShock 4 (2)",
		"DualSense", "DualSense (2)", "Xbox One", "Xbox One (2)", "Switch Pro", "Switch Pro (2)", "Controller", "Controller (2)" };
	if (prof == 0 && s_tv)
		return "PS TV";
	return prof >= 0 && prof < RE_PAD_PROFILES ? n[prof] : "Vita";
}

const char *re_pad_source_name(int src)
{
	static const char *const n[RE_PAD_SRC_COUNT] = { "Auto", "Vita", "Controller 1", "Controller 2", "Controller 3",
		"Controller 4", "None" };
	if (src == RE_PAD_SRC_VITA && s_tv)
		return "All controllers";
	return src >= 0 && src < RE_PAD_SRC_COUNT ? n[src] : "None";
}

int re_pad_sources(int player, int *src, int *cur)
{
	const int own = re_pad_source(player);
	int n = 0;
	*cur = 0;
	for (int s = 0; s < RE_PAD_SRC_COUNT; s++) {
		const int port = s >= RE_PAD_SRC_PORT1 && s <= RE_PAD_SRC_PORT4 ? s - RE_PAD_SRC_PORT1 + 1 : 0;
		if (port && !re_pad_connected(port))
			continue; // (only what is there; a controller set by hand and away is not listed)
		if (s == own)
			*cur = n;
		src[n++] = s;
	}
	return n;
}

int re_pad_pick_source(int player, int idx)
{
	int src[RE_PAD_SRC_COUNT], cur;
	const int n = re_pad_sources(player, src, &cur);
	if (idx < 0 || idx >= n)
		return re_pad_source(player);
	re_pad_set_source(player, src[idx]); // (also when it looks the same: Auto picked over an away controller drops it)
	return src[idx];
}
int re_pad_step_source(int player, int dir)
{
	int src[RE_PAD_SRC_COUNT], cur;
	const int n = re_pad_sources(player, src, &cur);
	const int s = src[((cur + dir) % n + n) % n];
	re_pad_set_source(player, s);
	return s;
}

const char *re_pad_source_text(int src, char *buf, int len)
{
	if (src >= RE_PAD_SRC_PORT1 && src <= RE_PAD_SRC_PORT4) {
		const int port = src - RE_PAD_SRC_PORT1 + 1;
		if (re_pad_connected(port))
			snprintf(buf, len, "%s: %s", re_pad_source_name(src), re_pad_port_name(port));
		else
			snprintf(buf, len, "%s (none)", re_pad_source_name(src));
	} else
		snprintf(buf, len, "%s", re_pad_source_name(src));
	return buf;
}

const char *re_pad_player_text(int player, char *buf, int len)
{
	const int src = re_pad_source(player), port = re_pad_port(player);
	if (src == RE_PAD_SRC_NONE)
		snprintf(buf, len, "None");
	else if (port < 0)
		snprintf(buf, len, "%s (none)", re_pad_source_name(src));
	else if (src == RE_PAD_SRC_AUTO)
		snprintf(buf, len, port ? "Auto: %s (%d)" : "Auto: %s", re_pad_port_name(port), port);
	else if (src >= RE_PAD_SRC_PORT1 && src <= RE_PAD_SRC_PORT4 && port != src - RE_PAD_SRC_PORT1 + 1)
		re_pad_source_text(port ? RE_PAD_SRC_PORT1 + port - 1 : RE_PAD_SRC_VITA, buf, len); // (its own is away: the
		                                                                                     // one it reads, named alike)
	else if (port)
		snprintf(buf, len, "%s: %s", re_pad_source_name(src), re_pad_port_name(port));
	else
		snprintf(buf, len, "%s", re_pad_source_name(src));
	return buf;
}
