// RePad: what apps read. The SceCtrl read functions are hooked: ports 1-4 give their controller, port 0 the Vita's own
// controls with controller 1 added - unless an app reads ports 1-4 itself, then each stays on its own port.
// On a PS TV the system serves its own controllers (DualShock 3 / 4) on ports 1-4: RePad's controllers take only the
// ports the system leaves free, and the system's are never touched.
#include <psp2kern/ctrl.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/threadmgr.h>
#include <taihen.h>

#include "bt.h"
#include "repad.h"

#define OWN_PORTS_US 500000 // an app read ports 1-4 this recently: controller 1 is not added to port 0
#define PORTS (REPAD_SLOTS + 1)

static SceInt64 s_port_read; // last read of a port with a controller (0 = never)
// ports 1-4: the type the system itself reports there (PS TV: its own controllers), and the RePad slot shown there
static uint8_t s_native[PORTS];
static int8_t s_slot_at[PORTS] = { -1, -1, -1, -1, -1 };

// RePad's controllers, in slot order, on the ports the system leaves free (a handheld Vita: slot i on port i + 1)
static void map_ports(void)
{
	// slot i on port i + 1 (a slot stays while its controller is connected, and a controller that comes back gets its
	// slot again: its port stays put); a port the system's own controller takes (PS TV): the lowest free one instead
	int8_t at[PORTS] = { -1, -1, -1, -1, -1 };
	for (int i = 0; i < REPAD_SLOTS; i++)
		if (repad_slot[i].used && !s_native[i + 1])
			at[i + 1] = (int8_t)i;
	for (int i = 0; i < REPAD_SLOTS; i++) {
		if (!repad_slot[i].used || !s_native[i + 1])
			continue;
		for (int p = 1; p < PORTS; p++)
			if (!s_native[p] && at[p] < 0) {
				at[p] = (int8_t)i;
				break;
			}
	}
	memcpy(s_slot_at, at, sizeof(at));
}

static int stick_off(int v) { return v < 128 ? 128 - v : v - 128; }

// one read of `count` entries of port `port`. ext: the extended functions (L1 / R1 / L2 / R2 / L3 / R3 apart); else
// the Vita's L / R: L1 or L2 = L, R1 or R2 = R. neg: negative logic (a set bit = not pressed)
static int patch(int port, SceCtrlData *d, int count, int ret, int ext, int neg)
{
	RepadSlot *s;
	if (!repad_on)
		return ret;
	if (port == 0) {
		if (s_port_read && ksceKernelGetSystemTimeWide() - s_port_read < OWN_PORTS_US)
			return ret;
		const int k = s_slot_at[1] >= 0 ? s_slot_at[1] : s_slot_at[2] >= 0 ? s_slot_at[2] : s_slot_at[3] >= 0 ?
			s_slot_at[3] : s_slot_at[4]; // (RePad's first controller)
		if (k < 0)
			return ret;
		s = &repad_slot[k];
	} else if (port >= 1 && port < PORTS && s_slot_at[port] >= 0)
		s = &repad_slot[s_slot_at[port]];
	else
		return ret; // (no RePad controller there: the system's own, or nothing)
	if (!s->used)
		return ret;
	if (port > 0) {
		s_port_read = ksceKernelGetSystemTimeWide();
		if (ret < 0 && count > 0)
			ret = count; // (a handheld Vita has no ports 1-4: RePad answers them)
	}
	if (ret < 0)
		return ret;
	uint32_t b = s->in.buttons;
	if (!ext) {
		if (b & (SCE_CTRL_L1 | SCE_CTRL_LTRIGGER))
			b |= SCE_CTRL_LTRIGGER;
		if (b & (SCE_CTRL_R1 | SCE_CTRL_RTRIGGER))
			b |= SCE_CTRL_RTRIGGER;
		b &= ~(uint32_t)(SCE_CTRL_L1 | SCE_CTRL_R1 | SCE_CTRL_L3 | SCE_CTRL_R3);
	}
	b &= ~(uint32_t)SCE_CTRL_PSBUTTON; // (passed to the system as a button press instead)
	for (int i = 0; i < count && i < ret; i++) {
		SceCtrlData *e = &d[i];
		if (port > 0) { // the controller alone
			e->buttons = neg ? ~b : b;
			e->lx = s->in.lx;
			e->ly = s->in.ly;
			e->rx = s->in.rx;
			e->ry = s->in.ry;
		} else { // added to the Vita's own controls: buttons of both, each stick from whichever is moved more
			e->buttons = neg ? e->buttons & ~b : e->buttons | b;
			if (stick_off(s->in.lx) + stick_off(s->in.ly) > stick_off(e->lx) + stick_off(e->ly)) {
				e->lx = s->in.lx;
				e->ly = s->in.ly;
			}
			if (stick_off(s->in.rx) + stick_off(s->in.ry) > stick_off(e->rx) + stick_off(e->ry)) {
				e->rx = s->in.rx;
				e->ry = s->in.ry;
			}
		}
	}
	return ret;
}

// the hooked read functions: exported ones by NID, the others at their offsets in SceCtrl's text (firmware 3.60-3.74)
#define HOOKS(X)                                                \
	X(peek_pos,      0, 0, 0xEA1D3A34, 0)      \
	X(read_pos,      0, 0, 0x9B96A1AA, 0)      \
	X(peek_neg,      0, 1, 0x19895843, 0)      \
	X(read_neg,      0, 1, 0x8D4E0DD1, 0)      \
	X(peek_pos_ext,  1, 0, 0, 0x3928)          \
	X(read_pos_ext,  1, 0, 0, 0x3BCC)          \
	X(peek_pos2,     0, 0, 0, 0x3EF8)          \
	X(read_pos2,     0, 0, 0, 0x449C)          \
	X(peek_neg2,     0, 1, 0, 0x41C8)          \
	X(read_neg2,     0, 1, 0, 0x47F0)          \
	X(peek_pos_ext2, 1, 0, 0, 0x4B48)          \
	X(read_pos_ext2, 1, 0, 0, 0x4E14)

// an app's command (repad.h: REPAD_CMD_PORT), answered here: the read itself does not happen
static int command(int cmd)
{
	if (cmd == REPAD_CMD_OFF)
		repad_on = 0;
	else if (cmd == REPAD_CMD_ON)
		repad_on = 1;
	else
		return -1;
	return 0;
}

#define DECL(name, ext, neg, nid, off)                                                       \
	static tai_hook_ref_t name##_ref;                                                        \
	static SceUID name##_uid = -1;                                                           \
	static int name##_hook(int port, SceCtrlData *d, int count)                              \
	{                                                                                        \
		if (port == REPAD_CMD_PORT)                                                          \
			return command(count);                                                           \
		const int r = TAI_CONTINUE(int, name##_ref, port, d, count);                         \
		return patch(port, d, count, r, (ext), (neg));                                       \
	}
HOOKS(DECL)

// port info: what the system reports stays; RePad's controllers on the free ports (as DualShock 4, so apps take them),
// their real kind in the spare bytes
static tai_hook_ref_t s_info_ref;
static SceUID s_info_uid = -1;
static int info_hook(SceCtrlPortInfo *info)
{
	const int r = TAI_CONTINUE(int, s_info_ref, info);
	if (r < 0)
		return r;
	for (int p = 1; p < PORTS; p++)
		s_native[p] = info->port[p];
	map_ports();
	for (int p = 1; p < PORTS && repad_on; p++) {
		const int k = s_slot_at[p];
		info->unk[p - 1] = 0;
		if (k < 0)
			continue;
		const RepadSlot *s = &repad_slot[k];
		info->unk[p - 1] = s->drv->kind;
		info->port[p] = s->drv->kind == REPAD_KIND_DS3 ? SCE_CTRL_TYPE_DS3 : SCE_CTRL_TYPE_DS4;
	}
	memcpy(&info->unk[4], REPAD_MARKER, 4);
	info->unk[8] = repad_on ? REPAD_FLAG_ON : 0;
	return r;
}

// the ports again (the system's controllers may have come or gone): from RePad's thread, twice a second
void repad_ctrl_refresh(void)
{
	SceCtrlPortInfo pi;
	ksceCtrlGetControllerPortInfo(&pi); // (through info_hook)
}

int repad_code_ok(SceUID modid, uint32_t off);

int repad_ctrl_start(void)
{
	tai_module_info_t mi;
	mi.size = sizeof(mi);
	if (taiGetModuleInfoForKernel(KERNEL_PID, "SceCtrl", &mi) < 0)
		return -1;
	// every offset must be the start of a function in this firmware's SceCtrl, else nothing is hooked
#define CHECK(name, ext, neg, nid, off) \
	if ((off) && !repad_code_ok(mi.modid, (off))) \
		return -1;
	HOOKS(CHECK)
	s_info_uid = taiHookFunctionExportForKernel(KERNEL_PID, &s_info_ref, "SceCtrl", TAI_ANY_LIBRARY, 0xF11D0D30, info_hook);
#define BIND(name, ext, neg, nid, off)                                                                        \
	name##_uid = (nid) ? taiHookFunctionExportForKernel(KERNEL_PID, &name##_ref, "SceCtrl", TAI_ANY_LIBRARY, (nid), \
		name##_hook) : taiHookFunctionOffsetForKernel(KERNEL_PID, &name##_ref, mi.modid, 0, (off) | 1, 1, name##_hook);
	HOOKS(BIND)
	return 0;
}

void repad_ctrl_stop(void)
{
#define UNBIND(name, ext, neg, nid, off)                \
	if (name##_uid >= 0) {                              \
		taiHookReleaseForKernel(name##_uid, name##_ref); \
		name##_uid = -1;                                \
	}
	HOOKS(UNBIND)
	if (s_info_uid >= 0) {
		taiHookReleaseForKernel(s_info_uid, s_info_ref);
		s_info_uid = -1;
	}
}
