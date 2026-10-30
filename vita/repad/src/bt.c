// RePad: Bluetooth side. A kernel thread takes the Bluetooth events: a controller that connects gets a slot and its
// driver, then each answered read request is parsed and the next one is sent at once.
#include "bt.h"

#include <psp2kern/ctrl.h>
#include <psp2kern/kernel/suspend.h>
#include <psp2kern/kernel/sysclib.h>
#include <psp2kern/kernel/threadmgr.h>
#include <taihen.h>

enum { HID_READ = 0, HID_WRITE = 1 };      // SceBtHidRequest.type
enum {                                     // SceBtEvent.id
	EV_CONNECTED = 0x05,
	EV_DISCONNECTED = 0x06,
	EV_READ_DONE = 0x0A,
	EV_WRITE_DONE = 0x0B,
	EV_FEATURE_DONE = 0x0C,
};
#define IDLE_US 500000 // the thread wakes this often without events

RepadSlot repad_slot[REPAD_SLOTS];
volatile int repad_on = 1;

static SceUID s_thread = -1, s_flag = -1;
#define FLAG_EXIT 1

static void transfer(RepadSlot *s, SceBtHidRequest *q, int type, void *buf, int len)
{
	memset(q, 0, sizeof(*q));
	q->type = (uint8_t)type;
	q->buffer = buf;
	q->length = (uint32_t)len;
	q->next = q; // (a list of one)
	ksceBtHidTransfer(s->mac0, s->mac1, q);
}

static void read_next(RepadSlot *s)
{
	memset(s->rd_buf, 0, sizeof(s->rd_buf));
	transfer(s, &s->rd_req, HID_READ, s->rd_buf, sizeof(s->rd_buf));
}

void repad_bt_write(RepadSlot *s, const uint8_t *r, int len)
{
	if (len > (int)sizeof(s->wr_buf))
		len = sizeof(s->wr_buf);
	memcpy(s->wr_buf, r, len);
	transfer(s, &s->wr_req, HID_WRITE, s->wr_buf, len);
}

static RepadSlot *slot_of(uint32_t mac0, uint32_t mac1)
{
	for (int i = 0; i < REPAD_SLOTS; i++)
		if (repad_slot[i].used && repad_slot[i].mac0 == mac0 && repad_slot[i].mac1 == mac1)
			return &repad_slot[i];
	return NULL;
}

static void connected(uint32_t mac0, uint32_t mac1)
{
	if (slot_of(mac0, mac1))
		return;
	uint16_t id[2] = { 0, 0 };
	if (ksceBtGetVidPid(mac0, mac1, id) < 0)
		return;
	const RepadDriver *d = repad_find_driver(id[0], id[1]);
	if (!d)
		return; // (not a controller RePad knows)
	if (repad_tv && d->vid == 0x054C && (d->pid == 0x0268 || d->pid == 0x05C4 || d->pid == 0x09CC))
		return; // (a PS TV reads DualShock 3 / 4 itself: two readers of one controller can stop the system)
	// the slot it had before if it is free (a slot is a port: a controller that comes back is on its old port again),
	// else the first free one
	int k = -1;
	for (int i = 0; i < REPAD_SLOTS && k < 0; i++)
		if (!repad_slot[i].used && repad_slot[i].mac0 == mac0 && repad_slot[i].mac1 == mac1)
			k = i;
	for (int i = 0; i < REPAD_SLOTS && k < 0; i++)
		if (!repad_slot[i].used)
			k = i;
	if (k >= 0) {
		RepadSlot *s = &repad_slot[k];
		memset(s, 0, sizeof(*s));
		s->mac0 = mac0;
		s->mac1 = mac1;
		s->drv = d;
		s->in.lx = s->in.ly = s->in.rx = s->in.ry = 128;
		s->used = 1;
		if (d->start)
			d->start(s); // (reads start when its write is answered)
		else
			read_next(s);
	}
}

static void disconnected(RepadSlot *s)
{
	// (its address stays: the controller gets this slot again when it comes back)
	s->used = 0;
	s->drv = NULL;
}

static void report(RepadSlot *s)
{
	s->reports++;
	if (s->drv->parse(s, s->rd_buf, sizeof(s->rd_buf)) && repad_on) {
		const RepadInput *in = &s->in;
		if (in->buttons & SCE_CTRL_PSBUTTON) // (the system's home menu)
			ksceCtrlSetButtonEmulation(0, 0, 0, SCE_CTRL_PSBUTTON, 16);
		if (in->buttons || in->lx < 96 || in->lx > 160 || in->ly < 96 || in->ly > 160 || in->rx < 96 || in->rx > 160 ||
			in->ry < 96 || in->ry > 160)
			ksceKernelPowerTick(SCE_KERNEL_POWER_TICK_DEFAULT); // (a controller in use keeps the Vita awake)
	}
	read_next(s);
}

static int bt_callback(int notify_id, int notify_count, int notify_arg, void *common)
{
	SceBtEvent ev;
	int r;
	while ((r = ksceBtReadEvent(&ev, 1)) == (int)SCE_BT_ERROR_CB_OVERFLOW)
		;
	if (r <= 0)
		return 0;
	RepadSlot *s = slot_of(ev.mac0, ev.mac1);
	switch (ev.id) {
	case EV_CONNECTED:
		connected(ev.mac0, ev.mac1);
		repad_ctrl_refresh(); // (the ports again: RePad's controllers on the free ones)
		break;
	case EV_DISCONNECTED:
		if (s)
			disconnected(s);
		repad_ctrl_refresh();
		break;
	case EV_READ_DONE:
		if (s)
			report(s);
		break;
	case EV_WRITE_DONE:
	case EV_FEATURE_DONE:
		if (s && !s->reports)
			read_next(s); // (the controller's set-up was answered: its reports from now on)
		break;
	}
	return 0;
}

static int bt_thread(SceSize args, void *argp)
{
	const SceUID cb = ksceKernelCreateCallback("repad_bt", 0, bt_callback, NULL);
	ksceBtRegisterCallback(cb, 0, 0xFFFFFFFF, 0xFFFFFFFF);
	// (controllers that connected before RePad started are not taken: reading the paired list and sending to them from
	// here stopped the system. They connect again (HOME) and come as any other)
	for (;;) {
		unsigned int bits = 0;
		SceUInt32 timeout = IDLE_US;
		const int r = ksceKernelWaitEventFlagCB(s_flag, FLAG_EXIT, SCE_EVENT_WAITOR | SCE_EVENT_WAITCLEAR_PAT, &bits, &timeout);
		if (r >= 0 && (bits & FLAG_EXIT))
			break;
		repad_ctrl_refresh(); // (the system's own controllers may have come or gone: the free ports again)
		// (no read is ever sent again while one is in flight: overwriting a request the Bluetooth stack holds can stop
		// the system. Reads go on only from their "done" events)
	}
	ksceBtUnregisterCallback(cb);
	ksceKernelDeleteCallback(cb);
	return 0;
}

// ---- pairing: the Vita accepts only Sony controllers; this lets the others pair too. A function of SceBt (offset
// 0x199C8 in its text, firmware 3.60-3.74) checks a new device's flags; clearing the check lets it through ----
static tai_hook_ref_t s_pair_ref;
static SceUID s_pair_hook = -1;
static int pair_check(void *a0, void *a1)
{
	const uint32_t flags = *(uint32_t *)((uintptr_t)a1 + 4);
	if (a0 && !(flags & 0x2)) {
		uint32_t *dev = (uint32_t *)(*(uintptr_t *)a0 + 8);
		*dev |= 0x11000;
	}
	return TAI_CONTINUE(int, s_pair_ref, a0, a1);
}

#define PAIR_OFF 0x199C8
int repad_code_ok(SceUID modid, uint32_t off);

int repad_bt_check(void)
{
	tai_module_info_t mi;
	mi.size = sizeof(mi);
	return taiGetModuleInfoForKernel(KERNEL_PID, "SceBt", &mi) >= 0 && repad_code_ok(mi.modid, PAIR_OFF) ? 0 : -1;
}

int repad_bt_start(void)
{
	tai_module_info_t mi;
	mi.size = sizeof(mi);
	if (taiGetModuleInfoForKernel(KERNEL_PID, "SceBt", &mi) >= 0)
		s_pair_hook = taiHookFunctionOffsetForKernel(KERNEL_PID, &s_pair_ref, mi.modid, 0, PAIR_OFF, 1, pair_check);
	s_flag = ksceKernelCreateEventFlag("repad_flag", 0, 0, NULL);
	s_thread = ksceKernelCreateThread("repad_bt", bt_thread, 0x3C, 0x2000, 0, 0x10000, NULL);
	if (s_flag < 0 || s_thread < 0)
		return -1;
	ksceKernelStartThread(s_thread, 0, NULL);
	return 0;
}

void repad_bt_stop(void)
{
	if (s_thread >= 0) {
		ksceKernelSetEventFlag(s_flag, FLAG_EXIT);
		ksceKernelWaitThreadEnd(s_thread, NULL, NULL);
		ksceKernelDeleteThread(s_thread);
		s_thread = -1;
	}
	if (s_flag >= 0) {
		ksceKernelDeleteEventFlag(s_flag);
		s_flag = -1;
	}
	if (s_pair_hook >= 0) {
		taiHookReleaseForKernel(s_pair_hook, s_pair_ref);
		s_pair_hook = -1;
	}
}
