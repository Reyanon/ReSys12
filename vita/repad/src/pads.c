// RePad: controller drivers (report formats of the controllers, as published for each of them).
#include "pads.h"

#include <psp2kern/ctrl.h>

#include "bt.h"
#include "repad.h"

static int clamp255(int v) { return v < 0 ? 0 : v > 255 ? 255 : v; }

// hat switch 0..7 (N, NE, E, SE, S, SW, W, NW; else released) -> d-pad bits
static uint32_t hat(int h)
{
	static const uint32_t dir[8] = {
		SCE_CTRL_UP, SCE_CTRL_UP | SCE_CTRL_RIGHT, SCE_CTRL_RIGHT, SCE_CTRL_RIGHT | SCE_CTRL_DOWN,
		SCE_CTRL_DOWN, SCE_CTRL_DOWN | SCE_CTRL_LEFT, SCE_CTRL_LEFT, SCE_CTRL_LEFT | SCE_CTRL_UP,
	};
	return h >= 0 && h < 8 ? dir[h] : 0;
}

// ---- DualShock 4 / DualSense ----
// Over Bluetooth both send the same basic report (id 0x01) until a host asks them for more, and RePad never does:
// a third-party controller that only knows the basic report works the same way.
//   [1] lx  [2] ly  [3] rx  [4] ry
//   [5] hat (low 4 bits), square 0x10, cross 0x20, circle 0x40, triangle 0x80
//   [6] L1 0x01, R1 0x02, L2 0x04, R2 0x08, share / create 0x10, options 0x20, L3 0x40, R3 0x80
//   [7] PS 0x01, touch pad click 0x02
static int sony_parse(RepadSlot *s, const uint8_t *r, int len)
{
	if (r[0] != 0x01 || len < 8)
		return 0;
	uint32_t b = hat(r[5] & 0x0F);
	if (r[5] & 0x10) b |= SCE_CTRL_SQUARE;
	if (r[5] & 0x20) b |= SCE_CTRL_CROSS;
	if (r[5] & 0x40) b |= SCE_CTRL_CIRCLE;
	if (r[5] & 0x80) b |= SCE_CTRL_TRIANGLE;
	if (r[6] & 0x01) b |= SCE_CTRL_L1;
	if (r[6] & 0x02) b |= SCE_CTRL_R1;
	if (r[6] & 0x04) b |= SCE_CTRL_LTRIGGER;
	if (r[6] & 0x08) b |= SCE_CTRL_RTRIGGER;
	if (r[6] & 0x10) b |= SCE_CTRL_SELECT;
	if (r[6] & 0x20) b |= SCE_CTRL_START;
	if (r[6] & 0x40) b |= SCE_CTRL_L3;
	if (r[6] & 0x80) b |= SCE_CTRL_R3;
	if (r[7] & 0x01) b |= SCE_CTRL_PSBUTTON;
	s->in.buttons = b;
	s->in.lx = r[1];
	s->in.ly = r[2];
	s->in.rx = r[3];
	s->in.ry = r[4];
	return 1;
}

// ---- Switch Pro Controller (and the many controllers that pair as one) ----
// Nintendo's face buttons by position: A (right) = circle, B (bottom) = cross, X (top) = triangle, Y (left) = square.
// L / R = L1 / R1, ZL / ZR = L2 / R2, - = select, + = start, HOME = PS.
static void switch_start(RepadSlot *s)
{
	// output report 0x01 (rumble data left neutral) with subcommand 0x03 "set input report mode", argument 0x30:
	// the full report with 12-bit sticks
	uint8_t r[12] = { 0 };
	r[0] = 0x01;
	r[1] = s->counter++ & 0x0F;
	r[10] = 0x03;
	r[11] = 0x30;
	repad_bt_write(s, r, sizeof(r));
}

// a 12-bit stick value -> 0..255 around the value at rest; up = smaller on the Vita, larger on the controller
static uint8_t switch_axis(RepadSlot *s, int i, int raw, int flip)
{
	const int v = (raw - (int)s->rest[i]) * 127 / 1400;
	return (uint8_t)clamp255(128 + (flip ? -v : v));
}

static int switch_parse(RepadSlot *s, const uint8_t *r, int len)
{
	uint32_t b = 0;
	if (r[0] == 0x30 && len >= 12) {
		// [3] Y 0x01, X 0x02, B 0x04, A 0x08, R 0x40, ZR 0x80   [4] - 0x01, + 0x02, RS 0x04, LS 0x08, HOME 0x10
		// [5] down 0x01, up 0x02, right 0x04, left 0x08, L 0x40, ZL 0x80   [6..8] left stick, [9..11] right stick
		if (r[3] & 0x01) b |= SCE_CTRL_SQUARE;
		if (r[3] & 0x02) b |= SCE_CTRL_TRIANGLE;
		if (r[3] & 0x04) b |= SCE_CTRL_CROSS;
		if (r[3] & 0x08) b |= SCE_CTRL_CIRCLE;
		if (r[3] & 0x40) b |= SCE_CTRL_R1;
		if (r[3] & 0x80) b |= SCE_CTRL_RTRIGGER;
		if (r[4] & 0x01) b |= SCE_CTRL_SELECT;
		if (r[4] & 0x02) b |= SCE_CTRL_START;
		if (r[4] & 0x04) b |= SCE_CTRL_R3;
		if (r[4] & 0x08) b |= SCE_CTRL_L3;
		if (r[4] & 0x10) b |= SCE_CTRL_PSBUTTON;
		if (r[5] & 0x01) b |= SCE_CTRL_DOWN;
		if (r[5] & 0x02) b |= SCE_CTRL_UP;
		if (r[5] & 0x04) b |= SCE_CTRL_RIGHT;
		if (r[5] & 0x08) b |= SCE_CTRL_LEFT;
		if (r[5] & 0x40) b |= SCE_CTRL_L1;
		if (r[5] & 0x80) b |= SCE_CTRL_LTRIGGER;
		const int raw[4] = { r[6] | (r[7] & 0x0F) << 8, r[7] >> 4 | r[8] << 4, r[9] | (r[10] & 0x0F) << 8, r[10] >> 4 | r[11] << 4 };
		if (!s->rest_ok) { // (the sticks are at rest when the controller connects)
			for (int i = 0; i < 4; i++)
				s->rest[i] = (uint16_t)raw[i];
			s->rest_ok = 1;
		}
		s->in.lx = switch_axis(s, 0, raw[0], 0);
		s->in.ly = switch_axis(s, 1, raw[1], 1);
		s->in.rx = switch_axis(s, 2, raw[2], 0);
		s->in.ry = switch_axis(s, 3, raw[3], 1);
	} else if (r[0] == 0x3F && len >= 12) {
		// the simple report the controller sends before (or instead of) the full one:
		// [1] B 0x01, A 0x02, Y 0x04, X 0x08, L 0x10, R 0x20, ZL 0x40, ZR 0x80   [2] - 0x01, + 0x02, LS 0x04, RS 0x08,
		// HOME 0x10   [3] hat   [4..11] LX, LY, RX, RY (16 bits, little endian)
		if (r[1] & 0x01) b |= SCE_CTRL_CROSS;
		if (r[1] & 0x02) b |= SCE_CTRL_CIRCLE;
		if (r[1] & 0x04) b |= SCE_CTRL_SQUARE;
		if (r[1] & 0x08) b |= SCE_CTRL_TRIANGLE;
		if (r[1] & 0x10) b |= SCE_CTRL_L1;
		if (r[1] & 0x20) b |= SCE_CTRL_R1;
		if (r[1] & 0x40) b |= SCE_CTRL_LTRIGGER;
		if (r[1] & 0x80) b |= SCE_CTRL_RTRIGGER;
		if (r[2] & 0x01) b |= SCE_CTRL_SELECT;
		if (r[2] & 0x02) b |= SCE_CTRL_START;
		if (r[2] & 0x04) b |= SCE_CTRL_L3;
		if (r[2] & 0x08) b |= SCE_CTRL_R3;
		if (r[2] & 0x10) b |= SCE_CTRL_PSBUTTON;
		b |= hat(r[3]);
		s->in.lx = r[5];
		s->in.ly = r[7];
		s->in.rx = r[9];
		s->in.ry = r[11];
	} else
		return 0;
	s->in.buttons = b;
	return 1;
}

static const RepadDriver s_drivers[] = {
	{ 0x054C, 0x05C4, REPAD_KIND_DS4, NULL, sony_parse },
	{ 0x054C, 0x09CC, REPAD_KIND_DS4, NULL, sony_parse },
	{ 0x054C, 0x0CE6, REPAD_KIND_DUALSENSE, NULL, sony_parse },
	{ 0x054C, 0x0DF2, REPAD_KIND_DUALSENSE, NULL, sony_parse },
	{ 0x057E, 0x2009, REPAD_KIND_SWITCH_PRO, switch_start, switch_parse },
};

const RepadDriver *repad_find_driver(uint16_t vid, uint16_t pid)
{
	for (unsigned i = 0; i < sizeof(s_drivers) / sizeof(s_drivers[0]); i++)
		if (s_drivers[i].vid == vid && s_drivers[i].pid == pid)
			return &s_drivers[i];
	return NULL;
}
