// license:BSD-3-Clause
// copyright-holders:Olivier Galibert
// ReSys12: H8/3002 internals shared by h8.c (core, INTC) and h8_periph.c (on-chip peripherals).
#ifndef RS12_H8_INT_H
#define RS12_H8_INT_H

#include "h8.h"

// CCR flags
enum {
	F_I = 0x80, F_UI = 0x40, F_H = 0x20, F_U = 0x10, F_N = 0x08, F_Z = 0x04, F_V = 0x02, F_C = 0x01,
	EXR_T = 0x80, EXR_NC = 0x78, EXR_I = 0x07
};

enum {
	H8_STATE_RESET = 0x10000,
	H8_STATE_IRQ = 0x10001,
};

#define H8_INPUT_LINE_NMI 32

static inline uint64_t h8_now(void) { return h8.end_time - (int64_t)h8.icount; }

// h8.c
void h8_internal_update(uint64_t current_time); // run peripheral events up to current_time, recompute bcount
void h8_internal_update_now(void);
void h8_intc_internal_interrupt(int vector);
void h8_intc_set_input(int inputnum, int state);
void h8_note_unimplemented(uint32_t a, int write);

// h8_periph.c
void h8_periph_reset(int power_on);
uint64_t h8_periph_update(uint64_t current_time); // returns the next event time (0 = none)
uint8_t h8_onchip_read8(uint32_t a);
// states from now during which the 16-bit timer counter at on-chip address a stays below thr (0 = unknown / not)
uint64_t h8_tcnt_states_below(uint32_t a, uint16_t thr);
uint16_t h8_onchip_read16(uint32_t a);
void h8_onchip_write8(uint32_t a, uint8_t d);
void h8_onchip_write16(uint32_t a, uint16_t d);
// INTC registers (h8.c)
uint8_t h8_intc_iscr_r(void);
void h8_intc_iscr_w(uint8_t d);
uint8_t h8_intc_ier_r(void);
void h8_intc_ier_w(uint8_t d);
uint8_t h8_intc_isr_r(void);
void h8_intc_isr_w(uint8_t d);
uint8_t h8_intc_icr_r(int offset);
void h8_intc_icr_w(int offset, uint8_t d);
void h8_syscr_w(uint8_t d);

#endif
