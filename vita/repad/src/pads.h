// RePad: controller drivers. Each driver turns its controller's input reports into one RepadInput.
#ifndef REPAD_PADS_H
#define REPAD_PADS_H

#include <stdint.h>

// one controller's state, in Vita terms: buttons as SCE_CTRL_* bits with the extended shoulder bits (L1 / R1 =
// SCE_CTRL_L1 / R1, L2 / R2 = SCE_CTRL_LTRIGGER / RTRIGGER, L3 / R3), sticks 0..255 with 128 at rest
typedef struct {
	uint32_t buttons;
	uint8_t lx, ly, rx, ry;
} RepadInput;

typedef struct RepadSlot RepadSlot;

typedef struct {
	uint16_t vid, pid;
	uint8_t kind;  // REPAD_KIND_*
	// after the connection: anything the controller needs before it sends what parse reads (NULL: nothing)
	void (*start)(RepadSlot *s);
	// an input report (r[0] = report id) into s->in. 0 if the report is not one it reads
	int (*parse)(RepadSlot *s, const uint8_t *r, int len);
} RepadDriver;

const RepadDriver *repad_find_driver(uint16_t vid, uint16_t pid);

#endif
