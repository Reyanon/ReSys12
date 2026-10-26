// license:BSD-3-Clause
// copyright-holders:Olivier Galibert
// ReSys12: H8/300H core (H8/3002) and its interrupt controller.
// Port of MAME src/devices/cpu/h8/h8.cpp, h8h.cpp, h83002.cpp, h8_intc.cpp (BSD-3-Clause, Olivier Galibert).
// Differences from MAME: C, single instance, one call executes a whole instruction (MAME "full" mode only, no
// mid-instruction peripheral sync), no DMA/DTC/trace/EXR/MAC, no software standby (never armed by the H8/3002 map).
#include "h8_int.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

H8State h8;
uint32_t h8_prof[H8P_N];
uint32_t h8_prof_chip[0x300][2]; // on-chip accesses by address - 0xfffd10 (read, write)
uint64_t h8_prof_ns; // RS12_SUB_PROFILE: time in peripheral updates
#ifdef RS12_SUB_PROFILE
volatile uint8_t h8_phase;
extern uint64_t s12_prof_now_ns(void);
#define H8P(i) (h8_prof[i]++)
#else
#define H8P(i) ((void)0)
#endif

// ---- memory ----
// On the H8/300H every access (fetch, byte, word) takes 2 states in MAME; wait states are not modelled.

static inline uint16_t mem_read16(uint32_t a)
{
	a &= 0xfffffe;
	if (a < h8.rom_size)
		return (uint16_t)(h8.rom[a] << 8 | h8.rom[a + 1]);
	if (a - h8.ram_base < h8.ram_size)
		return h8.ram[(a - h8.ram_base) >> 1];
	if (a >= 0xfffd10) {
		H8P(H8P_CHIP_R);
#ifdef RS12_SUB_PROFILE
		h8_prof_chip[(a - 0xfffd10) & 0x2ff][0]++;
#endif
		if (a < 0xffff10) // on-chip RAM (variables, stack): here instead of the on-chip register path
			return (uint16_t)(h8.iram[a - 0xfffd10] << 8 | h8.iram[a - 0xfffd10 + 1]);
		H8PH_ENTER(H8PH_CHIP);
		const uint16_t v = h8_onchip_read16(a);
		H8PH_LEAVE();
		return v;
	}
	H8P(H8P_EXT_R);
	H8PH_ENTER(H8PH_EXT);
	const uint16_t v = h8_ext_read16(a);
	H8PH_LEAVE();
	return v;
}

static inline uint8_t mem_read8(uint32_t a)
{
	a &= 0xffffff;
	if (a < h8.rom_size)
		return h8.rom[a];
	if (a - h8.ram_base < h8.ram_size) {
		const uint16_t w = h8.ram[(a - h8.ram_base) >> 1];
		return (uint8_t)((a & 1) ? w : w >> 8);
	}
	if (a >= 0xfffd10) {
		H8P(H8P_CHIP_R);
#ifdef RS12_SUB_PROFILE
		h8_prof_chip[(a - 0xfffd10) & 0x2ff][0]++;
#endif
		if (a < 0xffff10)
			return h8.iram[a - 0xfffd10];
		H8PH_ENTER(H8PH_CHIP);
		const uint8_t v = h8_onchip_read8(a);
		H8PH_LEAVE();
		return v;
	}
	H8P(H8P_EXT_R);
	H8PH_ENTER(H8PH_EXT);
	const uint8_t v = h8_ext_read8(a);
	H8PH_LEAVE();
	return v;
}

static inline void mem_write16(uint32_t a, uint16_t d)
{
	a &= 0xfffffe;
	if (a - h8.ram_base < h8.ram_size) {
		h8.ram[(a - h8.ram_base) >> 1] = d;
		return;
	}
	if (a >= 0xfffd10) {
		H8P(H8P_CHIP_W);
#ifdef RS12_SUB_PROFILE
		h8_prof_chip[(a - 0xfffd10) & 0x2ff][1]++;
#endif
		if (a < 0xffff10) {
			h8.iram[a - 0xfffd10] = (uint8_t)(d >> 8);
			h8.iram[a - 0xfffd10 + 1] = (uint8_t)d;
			return;
		}
		H8PH_ENTER(H8PH_CHIP);
		h8_onchip_write16(a, d);
		H8PH_LEAVE();
		return;
	}
	if (a < h8.rom_size)
		return; // ROM
	H8P(H8P_EXT_W);
	H8PH_ENTER(H8PH_EXT);
	h8_ext_write16(a, d);
	H8PH_LEAVE();
}

static inline void mem_write8(uint32_t a, uint8_t d)
{
	a &= 0xffffff;
	if (a - h8.ram_base < h8.ram_size) {
		uint16_t *w = &h8.ram[(a - h8.ram_base) >> 1];
		*w = (a & 1) ? (uint16_t)((*w & 0xff00) | d) : (uint16_t)((*w & 0x00ff) | (d << 8));
		return;
	}
	if (a >= 0xfffd10) {
		H8P(H8P_CHIP_W);
#ifdef RS12_SUB_PROFILE
		h8_prof_chip[(a - 0xfffd10) & 0x2ff][1]++;
#endif
		if (a < 0xffff10) {
			h8.iram[a - 0xfffd10] = d;
			return;
		}
		H8PH_ENTER(H8PH_CHIP);
		h8_onchip_write8(a, d);
		H8PH_LEAVE();
		return;
	}
	if (a < h8.rom_size)
		return;
	H8P(H8P_EXT_W);
	H8PH_ENTER(H8PH_EXT);
	h8_ext_write8(a, d);
	H8PH_LEAVE();
}

// ReSys12: instruction fetches come from the program ROM almost always; it is kept pre-swapped as host-order words
// (s_rom16) so a fetch is one load instead of the generic address decode and two byte loads
static uint16_t *s_rom16;

static inline uint16_t read16i(uint32_t adr)
{
	h8.icount -= 2;
	adr &= 0xfffffe;
	if (adr < h8.rom_size)
		return s_rom16[adr >> 1];
	return mem_read16(adr);
}

static inline uint8_t read8(uint32_t adr)
{
	h8.icount -= 2;
	return mem_read8(adr);
}

static inline void write8(uint32_t adr, uint8_t data)
{
	h8.icount -= 2;
	mem_write8(adr, data);
}

static inline uint16_t read16(uint32_t adr)
{
	h8.icount -= 2;
	return mem_read16(adr);
}

static inline void write16(uint32_t adr, uint16_t data)
{
	h8.icount -= 2;
	mem_write16(adr, data);
}

static inline void internal(int cycles)
{
	// on H8/300H all internal operations take an even number of states (at least 2 each)
	h8.icount -= cycles + 1;
}

// ---- C stand-ins for the C++ casts used in h8.lst ----
static inline int8_t s8(uint32_t v) { return (int8_t)v; }
static inline int16_t s16(uint32_t v) { return (int16_t)v; }
static inline int32_t s32(uint32_t v) { return (int32_t)v; }
static inline uint8_t u8(uint32_t v) { return (uint8_t)v; }
static inline uint16_t u16(uint32_t v) { return (uint16_t)v; }
static inline uint64_t u64(uint64_t v) { return v; }

// ---- registers ----
static inline void r8_w(int reg, uint8_t val)
{
	if (reg & 8)
		h8.R[reg & 7] = (uint16_t)((h8.R[reg & 7] & 0xff00) | val);
	else
		h8.R[reg & 7] = (uint16_t)((h8.R[reg & 7] & 0xff) | (val << 8));
}

static inline uint8_t r8_r(int reg)
{
	if (reg & 8)
		return (uint8_t)h8.R[reg & 7];
	return (uint8_t)(h8.R[reg & 7] >> 8);
}

static inline void r16_w(int reg, uint16_t val) { h8.R[reg & 0xf] = val; }
static inline uint16_t r16_r(int reg) { return h8.R[reg & 0xf]; }
static inline void r32_w(int reg, uint32_t val)
{
	h8.R[reg & 7] = (uint16_t)val;
	h8.R[(reg & 7) | 8] = (uint16_t)(val >> 16);
}
static inline uint32_t r32_r(int reg) { return h8.R[reg & 7] | ((uint32_t)h8.R[(reg & 7) | 8] << 16); }

// ---- interrupt plumbing (h83002.cpp) ----
static int h8_intc_interrupt_taken(int vector);
static void h8_intc_set_filter(int icr_filter, int ipr_filter);

static void update_irq_filter(void)
{
	switch (h8.syscr & 0x08) {
	case 0x00:
		if ((h8.CCR & (F_I | F_UI)) == (F_I | F_UI))
			h8_intc_set_filter(2, -1);
		else if (h8.CCR & F_I)
			h8_intc_set_filter(1, -1);
		else
			h8_intc_set_filter(0, -1);
		break;
	case 0x08:
		if (h8.CCR & F_I)
			h8_intc_set_filter(2, -1);
		else
			h8_intc_set_filter(0, -1);
		break;
	}
}

static inline void irq_setup(void)
{
	if (h8.syscr & 0x08)
		h8.CCR |= F_I;
	else
		h8.CCR |= F_I | F_UI;
}

static inline int trapa_setup(void)
{
	irq_setup();
	return 8;
}

static inline void interrupt_taken(void) { h8_intc_interrupt_taken(h8.taken_irq_vector); }
static inline int exr_in_stack(void) { return 0; }
#define debugger_exception_hook(v) ((void)0)

static inline void take_interrupt(void)
{
	h8.inst_state = H8_STATE_IRQ;
	h8.taken_irq_vector = h8.irq_vector;
	h8.taken_irq_level = h8.irq_level;
}

static inline void prefetch_switch(uint32_t pc, uint16_t ir)
{
	h8.NPC = pc & 0xffffff;
	h8.PC = pc + 2;
	h8.PIR = ir;
}

static inline void prefetch_done(void)
{
	if (h8.irq_vector)
		take_interrupt();
	else
		h8.inst_state = h8.IR[0] = h8.PIR;
}

static inline void prefetch_done_noirq(void) { h8.inst_state = h8.IR[0] = h8.PIR; }
static inline void prefetch_done_notrace(void) { prefetch_done(); }
static inline void prefetch_done_noirq_notrace(void) { h8.inst_state = h8.IR[0] = h8.PIR; }

static void illegal(void)
{
	if (h8.illegal_count++ < 16)
		fprintf(stderr, "h8: illegal instruction %04x at %06x\n", h8.IR[0], h8.PPC);
	h8.icount = -10000000; // as MAME: stalls the cpu
}

static void h8_unhandled_state(void)
{
	fprintf(stderr, "h8: unhandled special state %x\n", h8.inst_state);
	h8.inst_state = H8_STATE_RESET;
}

static inline void h8_eat_all_cycles(void)
{
	if (h8.icount > h8.bcount) {
		h8.idle_cycles += (uint64_t)(h8.icount - h8.bcount);
		h8.icount = h8.bcount;
	}
}

// ---- ALU helpers (h8.cpp, has_hc = true) ----
static uint8_t do_addx8(uint8_t v1, uint8_t v2)
{
	uint8_t c = h8.CCR & F_C ? 1 : 0;
	uint16_t res = (uint16_t)(v1 + v2 + c);
	h8.CCR &= ~(F_N | F_V | F_C);
	if (((v1 & 0xf) + (v2 & 0xf) + c) & 0x10)
		h8.CCR |= F_H;
	else
		h8.CCR &= ~F_H;
	if ((uint8_t)res)
		h8.CCR &= ~F_Z;
	if ((int8_t)res < 0)
		h8.CCR |= F_N;
	if (~(v1 ^ v2) & (v1 ^ res) & 0x80)
		h8.CCR |= F_V;
	if (res & 0x100)
		h8.CCR |= F_C;
	return (uint8_t)res;
}

static uint8_t do_subx8(uint8_t v1, uint8_t v2)
{
	uint8_t c = h8.CCR & F_C ? 1 : 0;
	uint16_t res = (uint16_t)(v1 - v2 - c);
	h8.CCR &= ~(F_N | F_V | F_C);
	if (((v1 & 0xf) - (v2 & 0xf) - c) & 0x10)
		h8.CCR |= F_H;
	else
		h8.CCR &= ~F_H;
	if ((uint8_t)res)
		h8.CCR &= ~F_Z;
	if ((int8_t)res < 0)
		h8.CCR |= F_N;
	if ((v1 ^ v2) & (v1 ^ res) & 0x80)
		h8.CCR |= F_V;
	if (res & 0x100)
		h8.CCR |= F_C;
	return (uint8_t)res;
}

static uint8_t do_inc8(uint8_t v1, uint8_t v2)
{
	uint8_t res = (uint8_t)(v1 + v2);
	h8.CCR &= ~(F_N | F_V | F_Z);
	if (!res)
		h8.CCR |= F_Z;
	else if ((int8_t)res < 0)
		h8.CCR |= F_N;
	if (~(v1 ^ v2) & (v1 ^ res) & 0x80)
		h8.CCR |= F_V;
	return res;
}

static uint16_t do_inc16(uint16_t v1, uint16_t v2)
{
	uint16_t res = (uint16_t)(v1 + v2);
	h8.CCR &= ~(F_N | F_V | F_Z);
	if (!res)
		h8.CCR |= F_Z;
	else if ((int16_t)res < 0)
		h8.CCR |= F_N;
	if (~(v1 ^ v2) & (v1 ^ res) & 0x8000)
		h8.CCR |= F_V;
	return res;
}

static uint32_t do_inc32(uint32_t v1, uint32_t v2)
{
	uint32_t res = v1 + v2;
	h8.CCR &= ~(F_N | F_V | F_Z);
	if (!res)
		h8.CCR |= F_Z;
	else if ((int32_t)res < 0)
		h8.CCR |= F_N;
	if (~(v1 ^ v2) & (v1 ^ res) & 0x80000000u)
		h8.CCR |= F_V;
	return res;
}

static uint8_t do_add8(uint8_t v1, uint8_t v2)
{
	uint16_t res = (uint16_t)(v1 + v2);
	h8.CCR &= ~(F_N | F_V | F_Z | F_C);
	if (((v1 & 0xf) + (v2 & 0xf)) & 0x10)
		h8.CCR |= F_H;
	else
		h8.CCR &= ~F_H;
	if (!(uint8_t)res)
		h8.CCR |= F_Z;
	else if ((int8_t)res < 0)
		h8.CCR |= F_N;
	if (~(v1 ^ v2) & (v1 ^ res) & 0x80)
		h8.CCR |= F_V;
	if (res & 0x100)
		h8.CCR |= F_C;
	return (uint8_t)res;
}

static uint16_t do_add16(uint16_t v1, uint16_t v2)
{
	uint32_t res = (uint32_t)v1 + v2;
	h8.CCR &= ~(F_N | F_V | F_Z | F_C);
	if (((v1 & 0xfff) + (v2 & 0xfff)) & 0x1000)
		h8.CCR |= F_H;
	else
		h8.CCR &= ~F_H;
	if (!(uint16_t)res)
		h8.CCR |= F_Z;
	else if ((int16_t)res < 0)
		h8.CCR |= F_N;
	if (~(v1 ^ v2) & (v1 ^ res) & 0x8000)
		h8.CCR |= F_V;
	if (res & 0x10000)
		h8.CCR |= F_C;
	return (uint16_t)res;
}

static uint32_t do_add32(uint32_t v1, uint32_t v2)
{
	uint64_t res = (uint64_t)v1 + (uint64_t)v2;
	h8.CCR &= ~(F_N | F_V | F_Z | F_C);
	if (((v1 & 0xfffffff) + (v2 & 0xfffffff)) & 0x10000000)
		h8.CCR |= F_H;
	else
		h8.CCR &= ~F_H;
	if (!(uint32_t)res)
		h8.CCR |= F_Z;
	else if ((int32_t)res < 0)
		h8.CCR |= F_N;
	if (~(v1 ^ v2) & (v1 ^ res) & 0x80000000u)
		h8.CCR |= F_V;
	if (res & 0x100000000ULL)
		h8.CCR |= F_C;
	return (uint32_t)res;
}

static uint8_t do_dec8(uint8_t v1, uint8_t v2)
{
	uint8_t res = (uint8_t)(v1 - v2);
	h8.CCR &= ~(F_N | F_V | F_Z);
	if (!res)
		h8.CCR |= F_Z;
	else if ((int8_t)res < 0)
		h8.CCR |= F_N;
	if ((v1 ^ v2) & (v1 ^ res) & 0x80)
		h8.CCR |= F_V;
	return res;
}

static uint16_t do_dec16(uint16_t v1, uint16_t v2)
{
	uint16_t res = (uint16_t)(v1 - v2);
	h8.CCR &= ~(F_N | F_V | F_Z);
	if (!res)
		h8.CCR |= F_Z;
	else if ((int16_t)res < 0)
		h8.CCR |= F_N;
	if ((v1 ^ v2) & (v1 ^ res) & 0x8000)
		h8.CCR |= F_V;
	return res;
}

static uint32_t do_dec32(uint32_t v1, uint32_t v2)
{
	uint32_t res = v1 - v2;
	h8.CCR &= ~(F_N | F_V | F_Z);
	if (!res)
		h8.CCR |= F_Z;
	else if ((int32_t)res < 0)
		h8.CCR |= F_N;
	if ((v1 ^ v2) & (v1 ^ res) & 0x80000000u)
		h8.CCR |= F_V;
	return res;
}

static uint8_t do_sub8(uint8_t v1, uint8_t v2)
{
	uint16_t res = (uint16_t)(v1 - v2);
	h8.CCR &= ~(F_N | F_V | F_Z | F_C);
	if (((v1 & 0xf) - (v2 & 0xf)) & 0x10)
		h8.CCR |= F_H;
	else
		h8.CCR &= ~F_H;
	if (!(uint8_t)res)
		h8.CCR |= F_Z;
	else if ((int8_t)res < 0)
		h8.CCR |= F_N;
	if ((v1 ^ v2) & (v1 ^ res) & 0x80)
		h8.CCR |= F_V;
	if (res & 0x100)
		h8.CCR |= F_C;
	return (uint8_t)res;
}

static uint16_t do_sub16(uint16_t v1, uint16_t v2)
{
	uint32_t res = (uint32_t)v1 - v2;
	h8.CCR &= ~(F_N | F_V | F_Z | F_C);
	if (((v1 & 0xfff) - (v2 & 0xfff)) & 0x1000)
		h8.CCR |= F_H;
	else
		h8.CCR &= ~F_H;
	if (!(uint16_t)res)
		h8.CCR |= F_Z;
	else if ((int16_t)res < 0)
		h8.CCR |= F_N;
	if ((v1 ^ v2) & (v1 ^ res) & 0x8000)
		h8.CCR |= F_V;
	if (res & 0x10000)
		h8.CCR |= F_C;
	return (uint16_t)res;
}

static uint32_t do_sub32(uint32_t v1, uint32_t v2)
{
	uint64_t res = (uint64_t)v1 - (uint64_t)v2;
	h8.CCR &= ~(F_N | F_V | F_Z | F_C);
	if (((v1 & 0xfffffff) - (v2 & 0xfffffff)) & 0x10000000)
		h8.CCR |= F_H;
	else
		h8.CCR &= ~F_H;
	if (!(uint32_t)res)
		h8.CCR |= F_Z;
	else if ((int32_t)res < 0)
		h8.CCR |= F_N;
	if ((v1 ^ v2) & (v1 ^ res) & 0x80000000u)
		h8.CCR |= F_V;
	if (res & 0x100000000ULL)
		h8.CCR |= F_C;
	return (uint32_t)res;
}

// shifts / rotates: generated for 8/16/32 bits from one template each
#define NZ_FLAGS(T, S, v) do { if (!(v)) h8.CCR |= F_Z; else if ((S)(v) < 0) h8.CCR |= F_N; } while (0)

#define DEF_SHIFTS(B, T, S, TOP, SECOND)                                                                      \
	static T do_shal##B(T v)                                                                                  \
	{                                                                                                         \
		h8.CCR &= ~(F_N | F_V | F_Z | F_C);                                                                   \
		if (v & TOP)                                                                                          \
			h8.CCR |= F_C;                                                                                    \
		if ((v & (TOP | SECOND)) == SECOND || (v & (TOP | SECOND)) == TOP)                                    \
			h8.CCR |= F_V;                                                                                    \
		v = (T)(v << 1);                                                                                      \
		NZ_FLAGS(T, S, v);                                                                                    \
		return v;                                                                                             \
	}                                                                                                         \
	static T do_shar##B(T v)                                                                                  \
	{                                                                                                         \
		h8.CCR &= ~(F_N | F_V | F_Z | F_C);                                                                   \
		if (v & 1)                                                                                            \
			h8.CCR |= F_C;                                                                                    \
		v >>= 1;                                                                                              \
		if (!v)                                                                                               \
			h8.CCR |= F_Z;                                                                                    \
		else if (v & SECOND) {                                                                                \
			v |= TOP;                                                                                         \
			h8.CCR |= F_N;                                                                                    \
		}                                                                                                     \
		return v;                                                                                             \
	}                                                                                                         \
	static T do_shll##B(T v)                                                                                  \
	{                                                                                                         \
		h8.CCR &= ~(F_N | F_V | F_Z | F_C);                                                                   \
		if (v & TOP)                                                                                          \
			h8.CCR |= F_C;                                                                                    \
		v = (T)(v << 1);                                                                                      \
		NZ_FLAGS(T, S, v);                                                                                    \
		return v;                                                                                             \
	}                                                                                                         \
	static T do_shlr##B(T v)                                                                                  \
	{                                                                                                         \
		h8.CCR &= ~(F_N | F_V | F_Z | F_C);                                                                   \
		if (v & 1)                                                                                            \
			h8.CCR |= F_C;                                                                                    \
		v >>= 1;                                                                                              \
		if (!v)                                                                                               \
			h8.CCR |= F_Z;                                                                                    \
		return v;                                                                                             \
	}                                                                                                         \
	static T do_rotl##B(T v)                                                                                  \
	{                                                                                                         \
		h8.CCR &= ~(F_N | F_V | F_Z | F_C);                                                                   \
		if (v & TOP)                                                                                          \
			h8.CCR |= F_C;                                                                                    \
		v = (T)((v << 1) | (v >> (B - 1)));                                                                   \
		NZ_FLAGS(T, S, v);                                                                                    \
		return v;                                                                                             \
	}                                                                                                         \
	static T do_rotr##B(T v)                                                                                  \
	{                                                                                                         \
		h8.CCR &= ~(F_N | F_V | F_Z | F_C);                                                                   \
		if (v & 1)                                                                                            \
			h8.CCR |= F_C;                                                                                    \
		v = (T)((v << (B - 1)) | (v >> 1));                                                                   \
		NZ_FLAGS(T, S, v);                                                                                    \
		return v;                                                                                             \
	}                                                                                                         \
	static T do_rotxl##B(T v)                                                                                 \
	{                                                                                                         \
		T c = h8.CCR & F_C ? 1 : 0;                                                                           \
		h8.CCR &= ~(F_N | F_V | F_Z | F_C);                                                                   \
		if (v & TOP)                                                                                          \
			h8.CCR |= F_C;                                                                                    \
		v = (T)((v << 1) | c);                                                                                \
		NZ_FLAGS(T, S, v);                                                                                    \
		return v;                                                                                             \
	}                                                                                                         \
	static T do_rotxr##B(T v)                                                                                 \
	{                                                                                                         \
		T c = h8.CCR & F_C ? 1 : 0;                                                                           \
		h8.CCR &= ~(F_N | F_V | F_Z | F_C);                                                                   \
		if (v & 1)                                                                                            \
			h8.CCR |= F_C;                                                                                    \
		v = (T)((v >> 1) | ((T)(c << (B - 1))));                                                              \
		NZ_FLAGS(T, S, v);                                                                                    \
		return v;                                                                                             \
	}

DEF_SHIFTS(8, uint8_t, int8_t, 0x80u, 0x40u)
DEF_SHIFTS(16, uint16_t, int16_t, 0x8000u, 0x4000u)
DEF_SHIFTS(32, uint32_t, int32_t, 0x80000000u, 0x40000000u)

static void set_nzv8(uint8_t v)
{
	h8.CCR &= ~(F_N | F_V | F_Z);
	if (!v)
		h8.CCR |= F_Z;
	else if ((int8_t)v < 0)
		h8.CCR |= F_N;
}

static void set_nzv16(uint16_t v)
{
	h8.CCR &= ~(F_N | F_V | F_Z);
	if (!v)
		h8.CCR |= F_Z;
	else if ((int16_t)v < 0)
		h8.CCR |= F_N;
}

static void set_nzv32(uint32_t v)
{
	h8.CCR &= ~(F_N | F_V | F_Z);
	if (!v)
		h8.CCR |= F_Z;
	else if ((int32_t)v < 0)
		h8.CCR |= F_N;
}

static void set_nz16(uint16_t v)
{
	h8.CCR &= ~(F_N | F_Z);
	if (!v)
		h8.CCR |= F_Z;
	else if ((int16_t)v < 0)
		h8.CCR |= F_N;
}

static void set_nz32(uint32_t v)
{
	h8.CCR &= ~(F_N | F_Z);
	if (!v)
		h8.CCR |= F_Z;
	else if ((int32_t)v < 0)
		h8.CCR |= F_N;
}

#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wparentheses"
#endif
#include "h8_ops.inc"
#include "h8_lookup.inc"

// ReSys12: handler per first instruction word (inst_state < 0x10000), filled once from h8_lookup(): one indirect call
// instead of h8_exec_full()'s three nested switches
static h8_handler s_op_table[0x10000];

// Idle loops (see idle_scan below) start with mov.b/mov.w @abs (first words 0x6a00-0x6bff) or btst #n, @aa:8
// (0x7exx). Only those opcodes go through op_idle_head, which does the idle check the main loop used to do before
// every instruction.
static h8_handler s_op_idle_orig[0x200], s_op_idle_orig7e[0x100], s_op_idle_orig_nop;
static int idle_try(uint32_t pc);
static int idle_try_p(uint32_t pc)
{
	H8PH_ENTER(H8PH_IDLE);
	const int r = idle_try(pc);
	H8PH_LEAVE();
	return r;
}
static uint8_t *s_idle_kind;
int h8_idle_enable = 1;

static void op_idle_head(void)
{
	if (s_idle_kind && h8.NPC < h8.rom_size && s_idle_kind[h8.NPC >> 1] && !h8.irq_vector && h8_idle_enable &&
		idle_try_p(h8.NPC))
		return; // not executed: time jumped ahead, the loop checks icount again
	s_op_idle_orig[h8.inst_state - 0x6a00]();
}

static void op_idle_head7e(void)
{
	if (s_idle_kind && h8.NPC < h8.rom_size && s_idle_kind[h8.NPC >> 1] && !h8.irq_vector && h8_idle_enable &&
		idle_try_p(h8.NPC))
		return;
	s_op_idle_orig7e[h8.inst_state - 0x7e00]();
}

static void op_idle_head_nop(void)
{
	if (s_idle_kind && h8.NPC < h8.rom_size && s_idle_kind[h8.NPC >> 1] && !h8.irq_vector && h8_idle_enable &&
		idle_try_p(h8.NPC))
		return;
	s_op_idle_orig_nop();
}

static void op_table_init(void)
{
	if (s_op_table[0])
		return;
	for (uint32_t i = 0; i < 0x10000; i++)
		s_op_table[i] = h8_lookup(i);
	for (uint32_t i = 0; i < 0x200; i++) {
		s_op_idle_orig[i] = s_op_table[0x6a00 + i];
		s_op_table[0x6a00 + i] = op_idle_head;
	}
	for (uint32_t i = 0; i < 0x100; i++) {
		s_op_idle_orig7e[i] = s_op_table[0x7e00 + i];
		s_op_table[0x7e00 + i] = op_idle_head7e;
	}
	s_op_idle_orig_nop = s_op_table[0x0000];
	s_op_table[0x0000] = op_idle_head_nop;
}
#ifdef RS12_H8_PCHIST
// diagnostics: instructions executed per program address (word index into the 512KB program ROM)
#define H8_PCHIST_SIZE 0x40000
static uint32_t h8_pchist[H8_PCHIST_SIZE];
#endif

// ---- blocks: runs of ROM instructions decoded once (tools/gen_h8_blk.py, h8_blk.inc) ----
// Each instruction of a block calls the block variant of its handler with its words; the block sets NPC / PC / PIR /
// IR[0] / inst_state only where it stops (the interpreter does it for every instruction). It stops at the same points
// as the interpreter: before an instruction when icount <= bcount (also after each prefix word of a prefixed opcode),
// after one when an interrupt is pending, and after an instruction that changes PC (its own prefetch_done).
#define H8BLK_MAX 32
typedef struct H8BIns H8BIns;
typedef void (*h8_blk_fn)(const H8BIns *);
struct H8BIns {
	h8_blk_fn fn;
	uint32_t a;          // address of the instruction (its first word)
	uint32_t pc;         // PC when its handler starts (after the prefix words)
	uint32_t next;       // address of the following instruction (straight ones)
	uint32_t pre_st[2];  // inst_state after each prefix word (an event between them leaves the rest to h8_exec_full)
	uint16_t op;         // first word (IR[0])
	uint16_t next_op;    // first word of the following instruction (straight ones)
	uint16_t w[5];       // the words from pc on
	uint16_t pre_w[2];   // prefix words
	uint16_t cost;       // the most states it takes, prefix words included (0xffff: unknown)
	uint8_t pre_j[2];    // IR index of each prefix word
	uint8_t npre;
	uint8_t native;      // diagnostics: its block code makes it (1), made but calls C (2)
};
typedef struct {
	void *code;      // ARM code of the block (h8_jit_arm.inc), NULL: blk_run
	void *body;      // ... after its entry: other blocks' code goes on here directly (r4, r5, r8 already set)
	uint32_t max_cost; // the most states the block takes (its code needs icount - max_cost > bcount on entry)
	uint16_t n;
	uint8_t term;    // the last instruction changes PC
	uint8_t jit_tried;
	uint32_t runs;   // diagnostics: times its code ran (from h8_run)
	uint32_t code_bytes; // diagnostics
	H8BIns ins[];
} H8Blk;
#include "h8_blk.inc"

#define H8BLK_NONE ((H8Blk *)1)
#define H8BLK_PAGE 256 // instruction words a page of pointers covers
static H8Blk **s_blk_page[0x80000 / (H8BLK_PAGE * 2)];
int h8_blk_enable = 1; // 0: every instruction through the interpreter (comparisons: RS12_H8BLK=0)

static void blk_free(void)
{
	for (unsigned i = 0; i < sizeof(s_blk_page) / sizeof(s_blk_page[0]); i++) {
		if (!s_blk_page[i])
			continue;
		for (int k = 0; k < H8BLK_PAGE; k++)
			if (s_blk_page[i][k] && s_blk_page[i][k] != H8BLK_NONE)
				free(s_blk_page[i][k]);
		free(s_blk_page[i]);
		s_blk_page[i] = NULL;
	}
}

static h8_handler orig_handler(uint16_t op)
{
	if (op >= 0x6a00 && op < 0x6c00)
		return s_op_idle_orig[op - 0x6a00];
	if (op >= 0x7e00 && op < 0x7f00)
		return s_op_idle_orig7e[op - 0x7e00];
	if (op == 0)
		return s_op_idle_orig_nop;
	return s_op_table[op];
}

static H8Blk *blk_build(uint32_t a)
{
	H8BIns ins[H8BLK_MAX];
	int n = 0, term = 0;
	while (n < H8BLK_MAX && a + 2 <= h8.rom_size) {
		if (s_idle_kind && s_idle_kind[a >> 1])
			break; // (idle loop heads keep their check: the interpreter runs them)
		H8BIns *e = &ins[n];
		memset(e, 0, sizeof(*e));
		const uint16_t op = s_rom16[a >> 1];
		h8_handler h = orig_handler(op);
		uint32_t pc = a + 2;
		int j;
		uint32_t base;
		while (h && h8_blk_dispatch(h, &j, &base)) {
			if (e->npre == 2 || pc + 2 > h8.rom_size) {
				h = NULL;
				break;
			}
			const uint16_t w = s_rom16[pc >> 1];
			e->pre_j[e->npre] = (uint8_t)j;
			e->pre_w[e->npre] = w;
			e->pre_st[e->npre] = base | w;
			e->npre++;
			pc += 2;
			h = h8_lookup(base | w);
		}
		int kind = 0, words = 0, next = 0, cost = 0;
		const h8_blk_fn fn = h ? h8_blk_of(h, &kind, &words, &next, &cost) : NULL;
		if (!fn || pc + 2 * (uint32_t)words > h8.rom_size)
			break;
		e->fn = fn;
		e->a = a;
		e->pc = pc;
		e->op = op;
		e->cost = (uint16_t)(cost == 0xffff ? 0xffff : cost + 2 * e->npre);
		for (int k = 0; k < words; k++)
			e->w[k] = s_rom16[(pc >> 1) + (uint32_t)k];
		n++;
		if (kind == 2) {
			term = 1;
			break;
		}
		e->next = pc + 2 * (uint32_t)next;
		e->next_op = e->w[next];
		a = e->next;
	}
	if (!n)
		return H8BLK_NONE;
	H8Blk *b = (H8Blk *)malloc(sizeof(H8Blk) + (size_t)n * sizeof(H8BIns));
	if (!b)
		return H8BLK_NONE;
	b->code = NULL;
	b->body = NULL;
	b->jit_tried = 0;
	b->runs = 0;
	b->max_cost = 0;
	for (int i = 0; i < n; i++)
		b->max_cost += ins[i].cost;
	b->n = (uint16_t)n;
	b->term = (uint8_t)term;
	memcpy(b->ins, ins, (size_t)n * sizeof(H8BIns));
	return b;
}

static H8Blk *blk_get(uint32_t a)
{
	H8Blk **pg = s_blk_page[a / (H8BLK_PAGE * 2)];
	if (!pg) {
		pg = (H8Blk **)calloc(H8BLK_PAGE, sizeof(H8Blk *));
		if (!pg)
			return H8BLK_NONE;
		s_blk_page[a / (H8BLK_PAGE * 2)] = pg;
	}
	H8Blk **slot = &pg[(a >> 1) & (H8BLK_PAGE - 1)];
	if (!*slot)
		*slot = blk_build(a);
	return *slot;
}

// ---- exits: the state where a block stops (blk_run and the generated code) ----
static void blk_exit_before(const H8BIns *e)
{
	h8.NPC = e->a;
	h8.PC = e->a + 2;
	h8.PIR = h8.IR[0] = e->op;
	h8.inst_state = e->op;
}
static void blk_exit_mid(const H8BIns *e, unsigned p)
{
	h8.NPC = e->a;
	h8.PC = e->a + 4 + 2 * p;
	h8.PIR = h8.IR[0] = e->op;
	h8.inst_state = (int)e->pre_st[p];
}
static void blk_exit_irq(const H8BIns *e)
{
	h8.NPC = e->next;
	h8.PC = e->next + 2;
	h8.PIR = e->next_op;
	h8.IR[0] = e->op;
	take_interrupt();
}
static void blk_exit_end(const H8BIns *e)
{
	h8.NPC = e->next;
	h8.PC = e->next + 2;
	h8.PIR = h8.IR[0] = e->next_op;
	h8.inst_state = e->next_op;
}


static void blk_run(const H8Blk *b)
{
	for (unsigned i = 0; i < b->n; i++) {
		const H8BIns *e = &b->ins[i];
		if (h8.icount <= h8.bcount) { // stop before e, as at its start
			blk_exit_before(e);
			return;
		}
		h8.PPC = e->a;
#ifdef RS12_H8_PCHIST
		h8_pchist[(e->a >> 1) & (H8_PCHIST_SIZE - 1)]++;
#endif
		H8P(H8P_INSN);
		for (unsigned p = 0; p < e->npre; p++) { // a prefix word: as dispatch_*, then the interpreter's loop check
			h8.icount -= 2;
			h8.IR[e->pre_j[p]] = e->pre_w[p];
			if (h8.icount <= h8.bcount) {
				blk_exit_mid(e, p);
				return;
			}
#ifdef RS12_H8_PCHIST
			h8_pchist[(e->a >> 1) & (H8_PCHIST_SIZE - 1)]++;
#endif
			H8P(H8P_INSN);
		}
		if (b->term && i == b->n - 1u) {
			h8.IR[0] = e->op; // (as in the interpreter while it runs: its prefetch_done may take an interrupt)
			e->fn(e);
			return; // (its prefetch_done set the state)
		}
		e->fn(e);
		if (h8.irq_vector) { // prefetch_done of e with an interrupt pending
			blk_exit_irq(e);
			return;
		}
	}
	blk_exit_end(&b->ins[b->n - 1]); // (no instruction changing PC at the end)
}

#if defined(__arm__)
#include "h8_jit_arm.inc"
#else
int h8_jit_enable, h8_jit_native, h8_jit_chain, h8_jit_prof, h8_jit_link, h8_jit_max;
uint32_t h8_jit_blocks, h8_jit_bytes, h8_jit_native_n, h8_jit_call_n;
static void jit_reset(void) {}
static int jit_writable(void) { return 0; }
static int jit_executable(void) { return 0; }
static void *jit_block(const H8Blk *b) { (void)b; return NULL; }
#endif

#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

// ---- interrupt controller (h8_intc.cpp, H8H variant: IRQ0-7 at vectors 12-19, NMI at 7) ----
enum { INTC_VECTOR_BASE = 12, INTC_VECTOR_COUNT = 8, INTC_VECTOR_NMI = 7 };
enum { IRQ_LEVEL_LOW = 0, IRQ_EDGE_FALL = 1 };

static const int8_t vector_to_slot[64] = {
	-1, -1, -1, -1, -1, -1, -1, -1, // NMI at 7
	-1, -1, -1, -1, 0, 1, 2, 2,     // IRQ 0-3
	3, 3, 3, 3, 4, 4, 4, 4,         // IRQ 4-5, (reservedx2), WOVI, CMI, (reserved), ADI
	5, 5, 5, 5, 6, 6, 6, 6,         // IMIA0, IMIB0, OVI0, (reserved), IMIA1, IMIB1, OVI1, (reserved)
	7, 7, 7, 7, 8, 8, 8, 8,         // IMIA2, IMIB2, OVI2, (reserved), CMIA0, CMIB0, CMIx1, TOVI0/1
	9, 9, 9, 9, 10, 10, 10, 10,     // CMIA2, CMIB2, CMIx3, TOVI2/3, DEND0A, DEND0B, DEND1A, DEND1B
	11, 11, 11, 11, 12, 12, 12, 12, // (reservedx4), ERI0, RXI0, TXI0, TEI0
	13, 13, 13, 13, 14, 14, 14, 14  // ERI1, RXI1, TXI1, TEI1, ERI2, RXI2, TXI2, TEI2
};

static void h8_set_irq(int vector, int level, int nmi)
{
	h8.irq_vector = vector;
	h8.irq_level = level;
	h8.irq_nmi = nmi;
}

static int icr_priority(int vect)
{
	if (vect == 7)
		return 2;
	const int slot = vector_to_slot[vect];
	if (slot < 0)
		return 0;
	return (h8.intc.icr >> (slot ^ 7)) & 1;
}

static void intc_update_irq_state(void)
{
	H8Intc *c = &h8.intc;
	const uint64_t mask = (1u << INTC_VECTOR_COUNT) - 1;
	c->pending &= ~(mask << INTC_VECTOR_BASE);
	c->pending |= (uint64_t)(c->isr & c->ier & mask) << INTC_VECTOR_BASE;

	int cur_vector = 0, cur_level = -1;
	uint64_t p = c->pending;
	while (p) {
		const int vect = __builtin_ctzll(p);
		p &= p - 1;
		const int icr_pri = icr_priority(vect);
		const int ipr_pri = 0;
		if (icr_pri >= c->icr_filter && ipr_pri > c->ipr_filter) {
			const int level = c->ipr_filter == -1 ? icr_pri : ipr_pri;
			if (level > cur_level) {
				cur_vector = vect;
				cur_level = level;
			}
		}
	}
	h8_set_irq(cur_vector, cur_level, cur_vector == INTC_VECTOR_NMI);
}

static void intc_check_level_irqs(int update)
{
	H8Intc *c = &h8.intc;
	int set = 0;
	for (int i = 0; i < INTC_VECTOR_COUNT; i++) {
		const uint8_t mask = (uint8_t)(1 << i);
		if (c->irq_type[i] == IRQ_LEVEL_LOW && (c->irq_input & mask) && !(c->isr & mask)) {
			c->isr |= mask;
			set = 1;
		}
	}
	if (set && update)
		intc_update_irq_state();
}

static int h8_intc_interrupt_taken(int vector)
{
	H8Intc *c = &h8.intc;
	c->pending &= ~(1ull << vector);
	if (vector >= INTC_VECTOR_BASE && vector < INTC_VECTOR_BASE + INTC_VECTOR_COUNT) {
		const int irq = vector - INTC_VECTOR_BASE;
		const uint8_t mask = (uint8_t)(1 << irq);
		if (c->irq_type[irq] != IRQ_LEVEL_LOW || !(c->irq_input & mask))
			c->isr &= ~mask;
		intc_update_irq_state();
		return irq;
	}
	intc_update_irq_state();
	return vector == INTC_VECTOR_NMI ? H8_INPUT_LINE_NMI : 8;
}

void h8_intc_internal_interrupt(int vector)
{
	// no DMA/DTC to steal the interrupt
	h8.intc.pending |= 1ull << vector;
	intc_update_irq_state();
}

void h8_intc_set_input(int inputnum, int state)
{
	H8Intc *c = &h8.intc;
	int set = 0;
	const uint8_t mask = (uint8_t)(1 << inputnum);
	const uint8_t cur = c->irq_input & mask;
	switch (c->irq_type[inputnum]) {
	case IRQ_LEVEL_LOW:
		set = state != 0;
		break; // the H8H latches level IRQs in ISR (has_isr)
	case IRQ_EDGE_FALL:
		set = state && !cur;
		break;
	}
	if (state)
		c->irq_input |= mask;
	else
		c->irq_input &= ~mask;
	if (set) {
		c->isr |= mask;
		intc_update_irq_state();
	}
}

static void h8_intc_set_filter(int icr_filter, int ipr_filter)
{
	h8.intc.icr_filter = icr_filter;
	h8.intc.ipr_filter = ipr_filter;
	intc_update_irq_state();
}

uint8_t h8_intc_ier_r(void) { return h8.intc.ier; }

void h8_intc_ier_w(uint8_t d)
{
	h8.intc.ier = d;
	intc_update_irq_state();
}

uint8_t h8_intc_iscr_r(void) { return h8.intc.iscr; }

void h8_intc_iscr_w(uint8_t d)
{
	H8Intc *c = &h8.intc;
	c->iscr = d;
	for (int i = 0; i < INTC_VECTOR_COUNT; i++)
		c->irq_type[i] = ((d >> i) & 1) ? IRQ_EDGE_FALL : IRQ_LEVEL_LOW;
	intc_check_level_irqs(1);
}

uint8_t h8_intc_isr_r(void) { return h8.intc.isr; }

void h8_intc_isr_w(uint8_t d)
{
	h8.intc.isr &= d;
	intc_check_level_irqs(0);
	intc_update_irq_state();
}

uint8_t h8_intc_icr_r(int offset) { return (uint8_t)(h8.intc.icr >> (8 * offset)); }

void h8_intc_icr_w(int offset, uint8_t d)
{
	// MAME h8h_intc_device::icr_w keeps the written lane's old bits (OR) and drops the other byte; reproduced as is
	h8.intc.icr = (h8.intc.icr & (0xffu << (8 * offset))) | ((uint32_t)d << (8 * offset));
}

void h8_syscr_w(uint8_t d)
{
	h8.syscr = d;
	update_irq_filter();
}

void h8_set_irq_line(int irq, int state)
{
	h8_intc_set_input(irq, state);
}

// ---- execution ----
static void recompute_bcount(uint64_t event_time)
{
	if (!event_time || event_time >= h8.end_time) {
		h8.bcount = 0;
		return;
	}
	h8.bcount = (int)(h8.end_time - event_time);
}

void h8_internal_update(uint64_t current_time)
{
	H8P(H8P_UPDATE);
#ifdef RS12_SUB_PROFILE
	const uint64_t t0 = s12_prof_now_ns();
#endif
	h8.in_update = 1;
	H8PH_ENTER(H8PH_UPDATE);
	const uint64_t ev = h8_periph_update(current_time);
	H8PH_LEAVE();
	h8.in_update = 0;
#ifdef RS12_SUB_PROFILE
	h8_prof_ns += s12_prof_now_ns() - t0;
#endif
	recompute_bcount(ev);
}

void h8_internal_update_now(void)
{
	// requests from inside the event pass are folded into it (MAME recurses; the outer pass recomputes anyway)
	if (!h8.in_update)
		h8_internal_update(h8_now());
}

uint64_t h8_total_cycles(void)
{
	return h8_now();
}

// ---- idle loop skipping (ReSys12 speed hack, on by default; h8_idle_enable = 0 turns it off) ----
// The System 12 sound/IO firmware waits for its interrupt handlers by spinning on a RAM flag:
//     loop: mov.b @flag, r0l      (6a2r 00aa aaaa, or mov.w 6b2r ...; abs16 forms 6a0r/6b0r aaaa)
//           bne   loop            (46f8 / 47f8 = beq; 46fa / 47fa for the abs16 forms)
// Nothing but an interrupt can end such a loop, so when the H8 is about to run it and no interrupt is pending,
// time jumps straight to the next on-chip event (timer, SCI, ...) or to the end of the slice. The loops are found by
// scanning the program ROM for the pattern (no per-game addresses). The flag must be RAM (shared or on-chip RAM).
// A second shape polls a bit of an on-chip register (port B bit 7 = VBLANK, ADCSR.ADF = conversion done) or RAM,
// optionally with a write to the board's wait-state register (0x300010/0x300030, a no-op) in the loop:
//     loop: [mov.b rN, @0x300030:32]  (6aar 0030 0030), or [nop] (0000)
//           btst  #n, @aa:8           (7eaa 73n0)
//           beq   loop                (47fa / 46fa = bne; 47f4 / 46f4 with the write, 47f8 / 46f8 with the nop)
// (technodr also waits like this for its JVS transmitter: SCI SSR.TDRE, SCR.TIE)
// golgo13, kartduel, truckk and technodr spin like this until the main CPU is ready (40 s of boot, H8 14 ms/frame).
// Ports change only at VBLANK edges (slice boundaries) and the ADC at its own event, so the skip is exact.
// Two more shapes (technodr waiting for the main CPU):
//     loop: mov.b @flag:24, rNl ; bne out ; mov.b rNl, @0x300030:32 ; mov.w @TCNT:16, rM ; cmp.w Ek, rM ; bcs loop
//           (a flag with a timer timeout: skipped only while the timer surely stays below Ek)
//     loop: mov.w @flag:24, rN ; beq out ; mov.b rNl, @a:24 ; mov.b rNl, @b:24 ; mov.b rNl, @0x300030:32 ; bra loop
//           (stores the flag's low byte: skipped only when a and b already hold it)
// Their iteration cost is measured (two arrivals at the head with nothing in between), so a skip ends on the same
// iteration as running the loop would: the frames stay bit-identical.
// s_idle_kind (declared above): per ROM halfword, 0 = none, else IDLE_* | IDLE_BEQ
enum { IDLE_B24 = 1, IDLE_W24 = 2, IDLE_B16 = 3, IDLE_W16 = 4, IDLE_BTST = 5, IDLE_WS_BTST = 6, IDLE_NOP_BTST = 7,
	IDLE_TIMEOUT = 8, IDLE_STORE = 9, IDLE_KIND = 15, IDLE_BEQ = 16 };
#define IDLE_ITER_STATES 12 // fetch + RAM read + taken branch, close enough for the timing of a spin loop

static uint16_t rom16(uint32_t a) { return (uint16_t)(h8.rom[a] << 8 | h8.rom[a + 1]); }

static uint8_t *s_idle_cost;     // measured states per iteration of the btst loops, per ROM halfword (0 = not yet)
static uint32_t s_cost_pc = ~0u; // last btst loop head seen and icount there
static int s_cost_icount;

static void idle_scan(void)
{
	free(s_idle_kind);
	free(s_idle_cost);
	s_idle_kind = NULL;
	s_idle_cost = NULL;
	s_cost_pc = ~0u;
	if (!h8.rom)
		return;
	s_idle_kind = (uint8_t *)calloc(h8.rom_size >> 1, 1);
	s_idle_cost = (uint8_t *)calloc(h8.rom_size >> 1, 1);
	if (!s_idle_kind || !s_idle_cost)
		return;
	int found = 0;
	for (uint32_t a = 0; a + 8 <= h8.rom_size; a += 2) {
		const uint16_t w0 = rom16(a);
		int kind = 0, br_at = 0;
		const uint16_t w1 = rom16(a + 2);
		if (a + 28 <= h8.rom_size && (w0 & 0xfff8) == 0x6a28 && w1 <= 0x00ff && (rom16(a + 6) >> 8) == 0x46 &&
			rom16(a + 8) == (0x6aa8 | (w0 & 7)) && rom16(a + 10) == 0x0030 &&
			(rom16(a + 12) & ~0x20) == 0x0010 && (rom16(a + 14) & 0xfff8) == 0x6b00 &&
			(rom16(a + 16) & 0xff00) == 0xff00 && (rom16(a + 18) & 0xff88) == 0x1d80 &&
			(rom16(a + 18) & 7) == (rom16(a + 14) & 7) && rom16(a + 20) == 0x45ea) {
			s_idle_kind[a >> 1] = IDLE_TIMEOUT;
			found++;
			continue;
		}
		if (a + 28 <= h8.rom_size && (w0 & 0xfff8) == 0x6b20 && w1 <= 0x00ff && (rom16(a + 6) >> 8) == 0x47 &&
			rom16(a + 8) == (0x6aa8 | (w0 & 7)) && rom16(a + 10) <= 0x00ff &&
			rom16(a + 14) == (0x6aa8 | (w0 & 7)) && rom16(a + 16) <= 0x00ff &&
			rom16(a + 20) == (0x6aa8 | (w0 & 7)) && rom16(a + 22) == 0x0030 && (rom16(a + 24) & ~0x20) == 0x0010 &&
			rom16(a + 26) == 0x40e4) {
			s_idle_kind[a >> 1] = IDLE_STORE;
			found++;
			continue;
		}
		if ((w0 >> 8) == 0x7e && (w1 & 0xff8f) == 0x7300) { // btst #n, @aa:8
			kind = IDLE_BTST;
			br_at = 4;
		} else if (w0 == 0x0000 && (w1 >> 8) == 0x7e && (rom16(a + 4) & 0xff8f) == 0x7300) {
			kind = IDLE_NOP_BTST; // nop + btst
			br_at = 6;
		} else if ((w0 & 0xfff0) == 0x6aa0 && w1 == 0x0030 && (rom16(a + 4) & ~0x20) == 0x0010 &&
			a + 12 <= h8.rom_size && (rom16(a + 6) >> 8) == 0x7e && (rom16(a + 8) & 0xff8f) == 0x7300) {
			kind = IDLE_WS_BTST; // mov.b rN, @0x300010/0x300030:32 + btst
			br_at = 10;
		} else if ((w0 & 0xfff0) == 0x6a20 && (w0 & 8) && rom16(a + 2) <= 0x00ff) { // mov.b @aa:24, rNl
			kind = IDLE_B24;
			br_at = 6;
		} else if ((w0 & 0xfff0) == 0x6b20 && rom16(a + 2) <= 0x00ff) { // mov.w @aa:24, rN
			kind = IDLE_W24;
			br_at = 6;
		} else if ((w0 & 0xfff0) == 0x6a00 && (w0 & 8)) { // mov.b @aa:16, rNl
			kind = IDLE_B16;
			br_at = 4;
		} else if ((w0 & 0xfff0) == 0x6b00) { // mov.w @aa:16, rN
			kind = IDLE_W16;
			br_at = 4;
		}
		if (!kind)
			continue;
		const uint16_t br = rom16(a + br_at);
		const uint8_t disp = (uint8_t)(0x100 - br_at - 2); // back to a
		if ((br >> 8) == 0x46 && (br & 0xff) == disp)
			s_idle_kind[a >> 1] = (uint8_t)kind;
		else if ((br >> 8) == 0x47 && (br & 0xff) == disp)
			s_idle_kind[a >> 1] = (uint8_t)(kind | IDLE_BEQ);
		else
			continue;
		found++;
	}
	fprintf(stderr, "h8: %d idle loop(s) found in the program ROM\n", found);
}

// value of the loop flag if it is plain RAM or a side-effect-free on-chip register (ports, ADC), else -1
static int idle_flag(uint32_t a, int word)
{
	a &= 0xffffff;
	uint16_t w;
	if (!word && a >= 0xffffb0) {
		switch (a & 0xff) {
		case 0xc7: case 0xcb: case 0xce: case 0xcf: case 0xd2: case 0xd3: case 0xd6: case 0xe8: case 0xe9:
		case 0xb2: case 0xba: // SCI SCR
		case 0xb4: case 0xbc: // SCI SSR (the read only arms the flag clear, as every loop iteration does)
			return h8_onchip_read8(a);
		default:
			return -1;
		}
	}
	if (a - h8.ram_base < h8.ram_size)
		w = h8.ram[(a - h8.ram_base) >> 1];
	else if (a >= 0xfffd10 && a < 0xffff10)
		w = (uint16_t)(h8.iram[(a - 0xfffd10) & ~1u] << 8 | h8.iram[((a - 0xfffd10) & ~1u) + 1]);
	else
		return -1;
	if (word)
		return w;
	return (a & 1) ? (w & 0xff) : (w >> 8);
}

// at the head of a known spin loop with no interrupt pending: skip whole iterations up to the next event
static int idle_try(uint32_t pc)
{
	const int k = s_idle_kind[pc >> 1];
	const int kind = k & IDLE_KIND;
	uint32_t a;
	int v, cost = IDLE_ITER_STATES;
	const int btst = kind == IDLE_BTST || kind == IDLE_WS_BTST || kind == IDLE_NOP_BTST || kind == IDLE_TIMEOUT ||
		kind == IDLE_STORE; // (measured cost)
	uint64_t limit = ~0ull >> 1; // states the skip may cover (timer threshold)
	if (kind == IDLE_TIMEOUT) {
		v = idle_flag((uint32_t)rom16(pc + 2) << 16 | rom16(pc + 4), 0);
		if (v != 0)
			return 0; // flag set (or not RAM): the loop ends
		limit = h8_tcnt_states_below(0xff0000u | rom16(pc + 16), h8.R[8 | ((rom16(pc + 18) >> 4) & 7)]);
		v = limit > 0; // goes round while the timer is surely below the limit
	} else if (kind == IDLE_STORE) {
		v = idle_flag((uint32_t)rom16(pc + 2) << 16 | rom16(pc + 4), 1);
		if (v <= 0)
			return 0; // zero ends the loop, -1: not RAM
		const int b1 = idle_flag((uint32_t)rom16(pc + 10) << 16 | rom16(pc + 12), 0);
		const int b2 = idle_flag((uint32_t)rom16(pc + 16) << 16 | rom16(pc + 18), 0);
		if (b1 != (v & 0xff) || b2 != (v & 0xff))
			return 0; // the stores still change memory: run them
	} else if (btst) {
		const uint32_t at = kind == IDLE_WS_BTST ? pc + 6 : kind == IDLE_NOP_BTST ? pc + 2 : pc;
		v = idle_flag(0xffff00u | (rom16(at) & 0xff), 0);
		if (v < 0)
			return 0;
		v &= 1 << ((rom16(at + 2) >> 4) & 7); // btst: Z = bit clear
	} else {
		if (kind == IDLE_B24 || kind == IDLE_W24)
			a = (uint32_t)rom16(pc + 2) << 16 | rom16(pc + 4);
		else
			a = 0xff0000u | rom16(pc + 2); // abs16 is sign-extended; on-chip RAM/IO live at 0xffxxxx
		v = idle_flag(a, kind == IDLE_W24 || kind == IDLE_W16);
		if (v < 0)
			return 0;
	}
	const int loops = (k & IDLE_BEQ) ? (v == 0) : (v != 0);
	if (btst) {
		cost = s_idle_cost[pc >> 1];
		if (!cost) {
			// learn it: icount difference between two consecutive arrivals here when the first one went round the
			// loop (the head can also be the target of a longer outer loop; an interrupt in between makes it big)
			// (the same difference twice in a row, so a bit that changed mid-iteration cannot teach a wrong one)
			static int s_cost_looped, s_cost_prev;
			const int d = (s_cost_pc == pc && s_cost_looped) ? s_cost_icount - h8.icount : 0;
			if (d > 0 && d < 64 && d == s_cost_prev)
				s_idle_cost[pc >> 1] = (uint8_t)d;
			s_cost_prev = d;
			s_cost_pc = pc;
			s_cost_icount = h8.icount;
			s_cost_looped = loops;
			return 0;
		}
	}
	if (!loops)
		return 0;
	int64_t span = h8.icount - h8.bcount;
	if ((uint64_t)span > limit)
		span = (int64_t)limit;
	const int n = (int)(span / cost);
	if (n <= 0)
		return 0;
	h8.icount -= n * cost;
	h8.idle_cycles += (uint64_t)n * cost;
	H8P(H8P_IDLE);
	return 1;
}

void h8_set_rom(const uint8_t *rom, uint32_t size)
{
	h8.rom = rom;
	h8.rom_size = rom ? size : 0;
	free(s_rom16);
	s_rom16 = NULL;
	blk_free();
	jit_reset();
	if (rom) {
		s_rom16 = (uint16_t *)malloc((size + 1) & ~1u);
		for (uint32_t a = 0; a + 1 < size; a += 2)
			s_rom16[a >> 1] = (uint16_t)(rom[a] << 8 | rom[a + 1]);
	}
	op_table_init();
	idle_scan();
}

void h8_set_ram(uint16_t *words, uint32_t base, uint32_t size)
{
	h8.ram = words;
	h8.ram_base = base;
	h8.ram_size = words ? size : 0;
}

// CPU + peripheral reset. power_on also clears the time base and the port registers (MAME sets those at device start).
static void h8_do_reset(int power_on)
{
	const uint8_t *rom = h8.rom;
	const uint32_t rom_size = h8.rom_size;
	uint16_t *ram = h8.ram;
	const uint32_t ram_base = h8.ram_base, ram_size = h8.ram_size;
	const uint64_t end_time = h8.end_time;
	const int icount = h8.icount;
	const uint8_t irq_input = h8.intc.irq_input;
	H8Port ports[7];
	memcpy(ports, h8.port, sizeof(ports));
	uint8_t iram[sizeof(h8.iram)];
	memcpy(iram, h8.iram, sizeof(iram));

	memset(&h8, 0, sizeof(h8));
	h8.rom = rom;
	h8.rom_size = rom_size;
	h8.ram = ram;
	h8.ram_base = ram_base;
	h8.ram_size = ram_size;
	if (!power_on) {
		h8.end_time = end_time;
		h8.icount = icount;
		memcpy(h8.port, ports, sizeof(ports));
		memcpy(h8.iram, iram, sizeof(iram));
		h8.intc.irq_input = irq_input;
	}

	h8.inst_state = H8_STATE_RESET;
	h8.irq_level = -1;
	h8.taken_irq_level = -1;
	h8.syscr = 0x09;
	h8_periph_reset(power_on);
}

void h8_reset(void)
{
	h8_do_reset(1);
}

#ifdef RS12_H8_PCHIST
void h8_pchist_reset(void)
{
	memset(h8_pchist, 0, sizeof(h8_pchist));
}

void h8_pchist_dump(int top)
{
	for (int n = 0; n < top; n++) {
		uint32_t best = 0, bi = 0;
		for (uint32_t i = 0; i < H8_PCHIST_SIZE; i++)
			if (h8_pchist[i] > best) {
				best = h8_pchist[i];
				bi = i;
			}
		if (!best)
			break;
		fprintf(stderr, "  h8 pc %06x x%u\n", bi << 1, best);
		h8_pchist[bi] = 0;
	}
}
#endif

uint32_t h8_jit_run_n, h8_jit_short_n; // diagnostics: block code run / not run as bcount was too close
uint32_t h8_jit_bad_n; // blocks whose code was found overwritten (logged by the frontend)
int h8_jit_check; // harness (RS12_H8JITCHECK): a block run taking more states than its max_cost is reported

// diagnostics (harness): the block variants the code still calls, by times run (block runs, so an approximation)
void h8_jit_report(FILE *f)
{
	enum { N = 512 };
	static struct { h8_blk_fn fn; uint64_t runs; unsigned sites; } t[N];
	unsigned n = 0;
	uint64_t all = 0, called = 0;
	for (unsigned i = 0; i < sizeof(s_blk_page) / sizeof(s_blk_page[0]); i++) {
		if (!s_blk_page[i])
			continue;
		for (int k = 0; k < H8BLK_PAGE; k++) {
			const H8Blk *b = s_blk_page[i][k];
			if (!b || b == H8BLK_NONE || !b->code)
				continue;
			for (unsigned j = 0; j < b->n; j++) {
				const H8BIns *e = &b->ins[j];
				all += b->runs;
				if (e->native == 1)
					continue;
				called += b->runs;
				unsigned x = 0;
				while (x < n && t[x].fn != e->fn)
					x++;
				if (x == n) {
					if (n == N)
						continue;
					t[n].fn = e->fn;
					t[n].runs = 0;
					t[n++].sites = 0;
				}
				t[x].runs += b->runs;
				t[x].sites++;
			}
		}
	}
	fprintf(f, "h8 jit instructions run %llu, through C %llu\n", (unsigned long long)all, (unsigned long long)called);
	{ // code bytes of the most run blocks that cover 50 / 90 / 99% of the instructions run
		static struct { uint64_t w; uint32_t bytes; const H8Blk *b; } hb[8192];
		unsigned nh = 0;
		for (unsigned i = 0; i < sizeof(s_blk_page) / sizeof(s_blk_page[0]); i++)
			for (int k = 0; s_blk_page[i] && k < H8BLK_PAGE; k++) {
				const H8Blk *b = s_blk_page[i][k];
				if (b && b != H8BLK_NONE && b->code && b->runs && nh < 8192) {
					hb[nh].w = (uint64_t)b->runs * b->n;
					hb[nh].b = b;
					hb[nh++].bytes = b->code_bytes;
				}
			}
		for (unsigned a = 1; a < nh; a++) // (insertion sort, by weight down)
			for (unsigned c = a; c > 0 && hb[c].w > hb[c - 1].w; c--) {
				const uint64_t w = hb[c].w;
				const uint32_t by = hb[c].bytes;
				const H8Blk *bb = hb[c].b;
				hb[c] = hb[c - 1];
				hb[c - 1].w = w;
				hb[c - 1].bytes = by;
				hb[c - 1].b = bb;
			}
		uint64_t cw = 0, cb = 0;
		unsigned nb = 0, lv = 0;
		static const double lvl[3] = { 0.5, 0.9, 0.99 };
		for (unsigned a = 0; a < nh && lv < 3; a++) {
			cw += hb[a].w;
			cb += hb[a].bytes;
			nb++;
			while (lv < 3 && (double)cw >= lvl[lv] * (double)all) {
				fprintf(f, "h8 jit hot %.0f%%: %u blocks, %llu bytes\n", lvl[lv] * 100, nb, (unsigned long long)cb);
				lv++;
			}
		}
		const char *dump = getenv("RS12_H8JITDUMP"); // the hottest blocks' code (raw) and their instructions
		for (unsigned a = 0; dump && a < nh && a < 4; a++) {
			char path[512];
			snprintf(path, sizeof(path), "%s.%u.bin", dump, a);
			FILE *df = fopen(path, "wb");
			if (df) {
				fwrite(hb[a].b->code, 1, hb[a].bytes, df);
				fclose(df);
			}
			fprintf(f, "h8 jit dump %u: %06x runs %u bytes %u code %p\n", a, hb[a].b->ins[0].a, hb[a].b->runs, hb[a].bytes,
				hb[a].b->code);
			for (unsigned j = 0; j < hb[a].b->n; j++)
				fprintf(f, "h8 jit dump %u ins %06x %04x fn %p native %u\n", a, hb[a].b->ins[j].a, hb[a].b->ins[j].op,
					(void *)hb[a].b->ins[j].fn, hb[a].b->ins[j].native);
		}
	}
	for (unsigned a = 0; a < n; a++)
		for (unsigned c = a + 1; c < n; c++)
			if (t[c].runs > t[a].runs) {
				const h8_blk_fn fn = t[a].fn;
				const uint64_t r = t[a].runs;
				const unsigned s = t[a].sites;
				t[a] = t[c];
				t[c].fn = fn;
				t[c].runs = r;
				t[c].sites = s;
			}
	for (unsigned a = 0; a < n && a < 40; a++)
		fprintf(f, "h8 jit calls %p %llu %u\n", (void *)t[a].fn, (unsigned long long)t[a].runs, t[a].sites);
}
void h8_run(int cycles)
{
	H8P(H8P_RUN);
	h8.end_time += (uint64_t)(int64_t)cycles;
	h8.icount += cycles;
	h8_internal_update(h8_now());

	const uint32_t blk_end = h8.rom_size < sizeof(s_blk_page) / sizeof(s_blk_page[0]) * H8BLK_PAGE * 2 ?
		h8.rom_size : (uint32_t)(sizeof(s_blk_page) / sizeof(s_blk_page[0]) * H8BLK_PAGE * 2);
	while (h8.icount > 0) {
		while (h8.icount > h8.bcount) {
			if (h8.inst_state < 0x10000 && h8.NPC < blk_end && h8_blk_enable) {
				H8Blk *b = blk_get(h8.NPC);
				if (b != H8BLK_NONE && b->ins[0].op == h8.inst_state) {
					if (!b->code && !b->jit_tried && h8_jit_enable && jit_writable()) {
						b->jit_tried = 1;
						b->code = jit_block(b);
					}
					// the code checks icount against bcount only after its C calls (only they move bcount): on entry
					// no instruction boundary of the block may reach bcount, else blk_run (checks every one)
					if (b->code && jit_executable() && *(const uint32_t *)b->code != 0xE92D41F0u) {
						// not the entry of a block (its push): the code area was overwritten (a crash at the end of
						// the area, executing zeros, was seen once on the device): the block runs without it, counted
						h8_jit_bad_n++;
						b->code = b->body = NULL;
					}
					if (b->code && h8.icount - (int)b->max_cost > h8.bcount && jit_executable()) {
						h8_jit_run_n++;
						b->runs++;
						const int ic0 = h8.icount;
						H8PH_ENTER(H8PH_CODE);
						((void (*)(void))b->code)();
						H8PH_LEAVE();
						if (h8_jit_check && ic0 - h8.icount > (int)b->max_cost) // (harness: no chain / links then)
							fprintf(stderr, "h8 jit check: block %06x took %d states, at most %u\n", b->ins[0].a,
								ic0 - h8.icount, b->max_cost);
					} else {
						h8_jit_short_n += b->code != NULL;
						H8PH_ENTER(H8PH_BLK);
						blk_run(b);
						H8PH_LEAVE();
					}
					continue;
				}
			}
			if (h8.inst_state < 0x10000)
				h8.PPC = h8.NPC; // idle loop check: op_idle_head
#ifdef RS12_H8_PCHIST
			h8_pchist[(h8.PPC >> 1) & (H8_PCHIST_SIZE - 1)]++;
#endif
			H8P(H8P_INSN);
			H8PH_ENTER(H8PH_INTERP);
			if (h8.inst_state < 0x10000)
				s_op_table[h8.inst_state]();
			else
				h8_exec_full();
			H8PH_LEAVE();
		}
		if (h8.icount > 0)
			while (h8.bcount && h8.icount <= h8.bcount)
				h8_internal_update(h8.end_time - (uint64_t)h8.bcount);
		if (h8.reset_request) {
			fprintf(stderr, "h8: watchdog reset\n");
			h8_do_reset(0);
			h8_internal_update_now();
		}
	}
}

// ---- diagnostics: on-chip registers touched but not implemented ----
static uint8_t s_unimpl[0x100][2];

void h8_note_unimplemented(uint32_t a, int write)
{
	const uint32_t o = a & 0xff;
	if (!s_unimpl[o][write]) {
		s_unimpl[o][write] = 1;
		fprintf(stderr, "h8: unimplemented on-chip register %s %06x (pc %06x)\n", write ? "write" : "read", a, h8.PPC);
	}
}

void h8_dump_unimplemented(void)
{
	const uint64_t total = h8_now();
	fprintf(stderr, "  h8 %llu states, %llu idle in SLEEP (%.1f%%)\n", (unsigned long long)total,
		(unsigned long long)h8.idle_cycles, total ? 100.0 * (double)h8.idle_cycles / (double)total : 0.0);
	for (int o = 0; o < 0x100; o++)
		for (int w = 0; w < 2; w++)
			if (s_unimpl[o][w])
				fprintf(stderr, "  h8 unimplemented %s ffff%02x\n", w ? "w" : "r", o);
}
