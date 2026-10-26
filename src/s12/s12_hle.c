// ReSys12: game routines run natively (HLE, RULES 6-7). Recognised by a hash of their code (no ROM bytes here).
//
// LZ decompressor of Namco's library (tektagt keeps it in the program flash): src (a0), dst (a1) -> length. A
// control byte gives 8 items, low bit first, above a sentinel bit: 1 = a literal byte, 0 = two bytes
// (distance 11 bits, 0 = 2048; length 5 bits, 0 = 32) copying from the output. A control byte 0 ends, 1 is skipped.
// Every byte goes through a helper call reading 16-bit words (a one-byte cache on the stack for the odd byte of the
// input) and one writing 16-bit words (the even byte with a 0 above it, the odd one with the byte kept on the stack),
// and every item stores 0 to 0x1f018000. In a tektagt fight this loop was ~90% of the emulated CPU time.
//
// The loop head (the control byte read, entry + 0x3c) is replaced: the prologue runs as code, then each visit does
// control bytes natively with the same memory accesses, stack slots and register results as the code, and counts the
// instructions the code would run (per path, see the comments) as cycles. When the next event is due it stops at the
// end of a control byte (the jump back to the loop head) with the registers the code would have there: running
// that jump makes the interpreter or the recompiled code take the event (H8 slice, vblank) at the same point as
// with the code, and the loop head brings the next visit. (Coming back to the loop head itself, the interpreter
// never took the events, which ran together at the routine's end: the H8 stalled while tektagt unpacked a stage.)
// At the end it does the epilogue (registers from the stack) and returns.
#include "s12_hle.h"

#include <string.h>

#include "psxcommon.h"
#include "psxmem.h"
#include "r3000a.h"

uint32_t rs12_hle_lz_head; // loop head of the LZ routine, 0 = not found / off
volatile uint32_t rs12_hle_lz_bytes, rs12_hle_lz_visits; // output bytes and visits (log)

// code window hashed: the two helpers before the routine and the routine (142 words), J/JAL targets masked
#define LZ_WIN_BEFORE 0x9c
#define LZ_WIN_WORDS 142
#define LZ_SIG 0xa8141a38u
#define LZ_HEAD 0x3c // entry -> loop head
#define LZ_FRAME 72  // stack frame of the routine
#define LZ_JHEAD 0x124 // loop head -> "j loop head / move a0, s2" (the end of an item group)

static uint32_t sig_hash(const uint8_t *p)
{
	uint32_t h = 0x811c9dc5u;
	for (int k = 0; k < LZ_WIN_WORDS; k++) {
		uint32_t w = (uint32_t)p[k * 4] | ((uint32_t)p[k * 4 + 1] << 8) | ((uint32_t)p[k * 4 + 2] << 16) |
			((uint32_t)p[k * 4 + 3] << 24);
		if ((w >> 26) == 2 || (w >> 26) == 3)
			w &= 0xfc000000u;
		for (int b = 0; b < 4; b++)
			h = (h ^ ((w >> (b * 8)) & 0xff)) * 0x01000193u;
	}
	return h;
}

void s12_hle_scan(const uint8_t *flash, uint32_t size)
{
	rs12_hle_lz_head = 0;
	if (!flash)
		return;
	for (uint32_t o = LZ_WIN_BEFORE; o + LZ_WIN_WORDS * 4 <= size; o += 4) {
		const uint32_t w = (uint32_t)flash[o] | ((uint32_t)flash[o + 1] << 8) | ((uint32_t)flash[o + 2] << 16) |
			((uint32_t)flash[o + 3] << 24);
		if (w != 0x27bdffb8u) // addiu sp, sp, -72
			continue;
		if (sig_hash(flash + o - LZ_WIN_BEFORE) == LZ_SIG) {
			rs12_hle_lz_head = 0x1fc00000u + o + LZ_HEAD;
			return;
		}
	}
}

// memory as the routine accesses it
static inline uint32_t rd16(uint32_t a)
{
	u8 *p;
	if (psxm_(&p, &psxRegs, a, 0))
		return SWAPu16(*(u16 *)p);
	return psxMemRead16(&psxRegs, a);
}
static inline uint32_t rd8(uint32_t a)
{
	u8 *p;
	if (psxm_(&p, &psxRegs, a, 0))
		return *p;
	return psxMemRead8(&psxRegs, a);
}
static inline uint32_t rd32(uint32_t a)
{
	u8 *p;
	if (psxm_(&p, &psxRegs, a, 0))
		return SWAPu32(*(u32 *)p);
	return psxMemRead32(&psxRegs, a);
}
// a 16-bit store; RAM directly (the written range is cleared for the recompiler once, at the end)
static inline void wr16(uint32_t a, uint32_t v, uint32_t *lo, uint32_t *hi)
{
	u8 *p;
	if (psxm_(&p, &psxRegs, a, 1)) {
		*(u16 *)p = SWAPu16((u16)v);
		const uint32_t m = a & 0x1ffffffe;
		if (m < *lo) *lo = m;
		if (m + 2 > *hi) *hi = m + 2;
	} else
		psxMemWrite16(&psxRegs, a, v & 0xffff);
}
static inline void wr32(uint32_t a, uint32_t v)
{
	u8 *p;
	if (psxm_(&p, &psxRegs, a, 1))
		*(u32 *)p = SWAPu32(v);
	else
		psxMemWrite32(&psxRegs, a, v);
}

// state of the routine during a visit
typedef struct {
	uint32_t slot16, slot20, slot24; // the stack slots: read cache address, cached byte, last even byte written
	uint32_t v1;                     // v1 as the helpers leave it
	uint32_t n;                      // instructions the code would run
	uint32_t lo, hi;                 // RAM written
} lz_st;

// read helper (entry - 0x9c), mode 0 (the input, with the cache) or 1 (the output)
static inline uint32_t lz_read(lz_st *s, uint32_t a, int mode)
{
	if (!(a & 1)) {
		if (mode) { // lbu: 8
			s->n += 8;
			return rd8(a);
		}
		const uint32_t h = rd16(a); // lhu, the odd byte cached: 13
		s->n += 13;
		s->v1 = h;
		s->slot16 = a + 1;
		s->slot20 = h >> 8;
		return h & 0xff;
	}
	if (!mode && s->slot16 == a) { // cached: 14
		s->n += 14;
		return s->slot20 & 0xff;
	}
	const uint32_t h = rd16(a - 1); // lhu of the even address: 10 (mode 1), 14 (mode 0)
	s->n += mode ? 10 : 14;
	s->v1 = h;
	return h >> 8;
}

// write helper (entry - 0x34): 8 (even), 10 (odd)
static inline void lz_write(lz_st *s, uint32_t v, uint32_t a)
{
	s->v1 = v << 8;
	if (!(a & 1)) {
		wr16(a, v, &s->lo, &s->hi);
		s->slot24 = v & 0xff;
		s->n += 8;
	} else {
		wr16(a - 1, (s->slot24 & 0xffff) + (v << 8), &s->lo, &s->hi);
		s->n += 10;
	}
}

void rs12_hle_lz(void)
{
	u32 *r = psxRegs.GPR.r;
	const uint32_t sp = r[29];
	uint32_t src = r[18], dst = r[20];       // s2, s4
	const uint32_t start = r[23];            // s7
	lz_st s;
	s.slot16 = rd32(sp + 16);
	s.slot20 = rd32(sp + 20);
	s.slot24 = rd32(sp + 24);
	s.v1 = r[3];
	s.n = 0;
	s.lo = ~0u;
	s.hi = 0;
	const uint32_t mult = Config.cycle_multiplier ? Config.cycle_multiplier : 100;
	uint32_t last = src, ctl = 0;
	int done = 0;
	for (;;) {
		// loop head (a0 = s2): s2++, a1 = 0, a2 = sp+16, jal, a3 = sp+20 (5), the read, s5 = v0, beqz, slti (3)
		last = src;
		ctl = lz_read(&s, src++, 0);
		s.n += 8;
		if (!ctl) { // the end: subu, 9 lw, addiu sp, jr, nop (13)
			s.n += 13;
			done = 1;
			break;
		}
		s.n += 2; // bnez, move a0 (to the loop head again when the byte is 1)
		if (ctl < 2)
			goto next;
		s.n += 1; // lui
		while (ctl >= 2) {
			wr16(0x1f018000u, 0, &s.lo, &s.hi); // ori, sh, andi, beqz, move (5)
			s.n += 5;
			if (ctl & 1) { // literal: 5, read, 5, write, j + sra (2)
				const uint32_t v = lz_read(&s, src++, 0);
				lz_write(&s, v, dst++);
				s.n += 12;
			} else { // copy: 5, read, 2, 5, read, 5 (+2 distance 0), 4 (+1 length 0), 2, 1, per byte 14 + read + write, sra
				const uint32_t b1 = lz_read(&s, src++, 0);
				const uint32_t b2 = lz_read(&s, src++, 0);
				const uint32_t v = (b1 << 8) + b2;
				uint32_t dist = v & 0x7ff, len = (v >> 11) & 0x1f;
				s.n += 5 + 2 + 5 + 5 + 4 + 2 + 1 + 1;
				if (!dist) { dist = 2048; s.n += 2; }
				if (!len) { len = 32; s.n += 1; }
				uint32_t p = dst - dist;
				for (uint32_t k = 0; k < len; k++) {
					const uint32_t b = lz_read(&s, p++, 1);
					lz_write(&s, b, dst++);
					s.n += 14;
				}
			}
			ctl >>= 1;
			s.n += 3; // slti, beqz, lui
		}
		s.n += 2; // j loop head, move a0
	next:
		// the next event due: stop at "j loop head" (its 2 instructions then run as code: not counted here)
		if ((s32)(psxRegs.cycle + (uint32_t)((uint64_t)s.n * mult / 100) - psxRegs.next_interupt) >= 0) {
			s.n -= 2;
			break;
		}
	}
	psxRegs.cycle += (uint32_t)((uint64_t)s.n * mult / 100);
	rs12_hle_lz_bytes += dst - r[20];
	rs12_hle_lz_visits++;
	wr32(sp + 16, s.slot16);
	wr32(sp + 20, s.slot20);
	wr32(sp + 24, s.slot24);
	if (s.hi > s.lo)
		psxCpu->Clear(s.lo, (s.hi - s.lo + 3) / 4);
	r[3] = s.v1;
	if (done) { // as the epilogue leaves the registers
		r[2] = dst - start;          // v0
		r[4] = last;                 // a0: the end byte's address
		r[5] = 0;                    // a1
		r[6] = sp + 16;              // a2
		r[7] = sp + 20;              // a3
		r[18] = src;
		r[20] = dst;
		r[31] = rd32(sp + 64);       // ra, s7..s0
		r[23] = rd32(sp + 60);
		r[22] = rd32(sp + 56);
		r[21] = rd32(sp + 52);
		r[20] = rd32(sp + 48);
		r[19] = rd32(sp + 44);
		r[18] = rd32(sp + 40);
		r[17] = rd32(sp + 36);
		r[16] = rd32(sp + 32);
		r[29] = sp + LZ_FRAME;
		psxRegs.pc = r[31];
	} else { // at "j loop head / move a0, s2"; the jump's 2 instructions follow as code (counted above)
		r[18] = src;
		r[20] = dst;
		psxRegs.pc = rs12_hle_lz_head + LZ_JHEAD;
	}
}
