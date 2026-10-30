// RePad: Bluetooth side. Connected controllers live in slots (slot i = controller port i + 1).
#ifndef REPAD_BT_H
#define REPAD_BT_H

#include <stdint.h>
#include <psp2kern/bt.h>

#include "pads.h"
#include "repad.h"

#define REPAD_REPORT_MAX 0x100

struct RepadSlot {
	int used;
	uint32_t mac0, mac1;
	const RepadDriver *drv;
	RepadInput in;
	uint32_t reports;       // input reports read so far
	uint8_t counter;        // output report counter (Switch Pro)
	// sticks at rest, taken from the first reports (Switch Pro: 12-bit values that differ between controllers)
	int rest_ok;
	uint16_t rest[4];
	// HID requests in flight: each slot has its own, so two controllers never share a buffer
	SceBtHidRequest rd_req, wr_req;
	uint8_t rd_buf[REPAD_REPORT_MAX];
	uint8_t wr_buf[64];
};

extern RepadSlot repad_slot[REPAD_SLOTS];
extern volatile int repad_on; // serves the controllers (REPAD_CMD_OFF / ON from apps); reads go on either way

extern int repad_tv; // a PS TV: the system serves DualShock 3 / 4 itself, RePad leaves them alone
void repad_ctrl_refresh(void); // the port map again (ctrl.c)
int repad_bt_check(void); // the SceBt offset RePad hooks is a function start in this firmware (0 = no)
int repad_bt_start(void);
void repad_bt_stop(void);
// send an output report (r[0] = report id) to the controller
void repad_bt_write(RepadSlot *s, const uint8_t *r, int len);

#endif
