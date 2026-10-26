// ReSys12: ARM code for H8 blocks (included by src/h8/h8.c after the block code; ARM hosts only).
// A block's code does what blk_run does for it, with the loop unrolled: for each instruction the icount / bcount
// check, the prefix words, the instruction, the interrupt check, and the exits set the state as blk_run would
// (blk_exit_*). The common instructions are made here (same steps as their block variants: the icount decrements at
// the same points, the flags through the same C helpers, on-chip RAM at a fixed address read in place); the others
// call their block variants (b_* / t_*). icount lives in r5 inside a block and goes back to h8 around every C call.
// Code goes into the area of rs12_jit_h8_region (Vita: the end of the recompiler's VM block, written only while this
// thread holds the VM domain; ADR-062) and stays until the ROM changes (h8_set_rom).
#include <stddef.h>

#if defined(__vita__)
extern void *rs12_jit_h8_region(size_t *size);
extern void rs12_jit_code_sync(void *p, size_t len);
extern int rs12_ndrc_code_writable(void);
static void *jit_area(size_t *size) { return rs12_jit_h8_region(size); }
static void jit_sync(void *p, size_t len) { rs12_jit_code_sync(p, len); }
static int jit_writable(void) { return rs12_ndrc_code_writable(); }
extern int rs12_ndrc_code_executable(void);
static int jit_executable(void) { return rs12_ndrc_code_executable(); }
#else
#include <sys/mman.h>
static void *jit_area(size_t *size)
{
	static void *p;
	if (!p) {
		p = mmap(NULL, 1u << 20, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
		if (p == MAP_FAILED)
			p = NULL;
	}
	*size = p ? 1u << 20 : 0;
	return p;
}
static void jit_sync(void *p, size_t len) { __builtin___clear_cache((char *)p, (char *)p + len); }
static int jit_writable(void) { return 1; }
static int jit_executable(void) { return 1; }
#endif

int h8_jit_enable = 1;     // 0: blocks run through blk_run (comparisons: RS12_H8JIT=0, noh8jit)
// which instructions are made here (comparisons: RS12_H8JITNATIVE, 0 = every one calls its block variant): 1 register
// ALU, 2 shifts, 4 mulxu, 8 jmp @ERn / rts, 16 jsr / bsr, 32 the rest (mov forms, adds, jmp, bcc)
int h8_jit_native = 0xff;
int h8_jit_max = -1; // comparisons (RS12_H8JITMAX): code for the first blocks only
int h8_jit_chain = 1;      // 0: every block returns to h8_run (comparisons: RS12_H8JITCHAIN=0)
int h8_jit_link = 1;       // 0: no direct branches between blocks (comparisons: RS12_H8JITLINK=0)
static int s_shared_made;
static uint32_t *s_jit_base, *s_jit_cur, *s_jit_end; // (veneers take the end of the area, s_jit_end moves down)
static int s_jit_full;
static struct { uint32_t fn; uint32_t *at; } s_ven[256]; // veneers: C functions the code calls with bl
static unsigned s_ven_n;
static uint32_t *s_chain; // shared block end: on to the next block's code (jit_shared_make)
static uint32_t *s_rd8, *s_rd16, *s_wr8, *s_wr16; // shared memory accesses (jit_mem_make)
static uint32_t *s_pfd, *s_goto_slow; // shared block ends (jit_shared_make)
static int s_mem_used; // the instruction being made used them
// direct branches to blocks without code yet: patched when they get it (jit_block)
#define LINK_MAX 4096
static struct { uint32_t tgt; uint32_t *site; } s_link[LINK_MAX], s_link_new[H8BLK_MAX * 4];
static unsigned s_link_n, s_link_new_n;
static const H8Blk *s_cur;   // the block being made
static uint32_t *s_cur_body; // ... and its body
static int s_dry;            // a dry run (jit_block's flag pass): nothing recorded
uint32_t h8_jit_blocks, h8_jit_bytes, h8_jit_native_n, h8_jit_call_n; // diagnostics

static void jit_reset(void)
{
	size_t size = 0;
	s_jit_base = (uint32_t *)jit_area(&size);
	s_jit_cur = s_jit_base;
	s_jit_end = s_jit_base ? s_jit_base + size / 4 : NULL;
	s_jit_full = !s_jit_base;
	s_ven_n = 0;
	s_link_n = 0;
	s_chain = NULL;
	s_shared_made = 0;
	h8_jit_blocks = h8_jit_bytes = h8_jit_native_n = h8_jit_call_n = 0;
}

// ---- C entry points for the generated code ----
static void jit_take_interrupt(void) { take_interrupt(); }
static uint32_t jit_read8(uint32_t a) { return read8(a); }
static uint32_t jit_read16(uint32_t a) { return read16(a); }
static void jit_write8(uint32_t a, uint32_t d) { write8(a, (uint8_t)d); }
static void jit_write16(uint32_t a, uint32_t d) { write16(a, (uint16_t)d); }
static uint32_t jit_do_add16(uint32_t a, uint32_t b) { return do_add16((uint16_t)a, (uint16_t)b); }
static uint32_t jit_do_inc8(uint32_t a, uint32_t b) { return do_inc8((uint8_t)a, (uint8_t)b); }
static uint32_t jit_do_dec8(uint32_t a, uint32_t b) { return do_dec8((uint8_t)a, (uint8_t)b); }
static uint32_t jit_do_inc16(uint32_t a, uint32_t b) { return do_inc16((uint16_t)a, (uint16_t)b); }
static uint32_t jit_do_sub8(uint32_t a, uint32_t b) { return do_sub8((uint8_t)a, (uint8_t)b); }
static uint32_t jit_do_sub16(uint32_t a, uint32_t b) { return do_sub16((uint16_t)a, (uint16_t)b); }
static uint32_t jit_do_sub32(uint32_t a, uint32_t b) { return do_sub32(a, b); }
static uint32_t jit_do_add32(uint32_t a, uint32_t b) { return do_add32(a, b); }
static uint32_t jit_do_shal16(uint32_t a, uint32_t b) { (void)b; return do_shal16((uint16_t)a); }
static uint32_t jit_do_shlr8(uint32_t a, uint32_t b) { (void)b; return do_shlr8((uint8_t)a); }
static uint32_t jit_do_shll8(uint32_t a, uint32_t b) { (void)b; return do_shll8((uint8_t)a); }
static uint32_t jit_do_rotxl8(uint32_t a, uint32_t b) { (void)b; return do_rotxl8((uint8_t)a); }

// ---- A32 encodings ----
enum { R0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11, R12, SP, LR, PC_ };
enum { C_EQ = 0, C_NE = 1, C_HS = 2, C_MI = 4, C_LS = 9, C_LE = 13, C_AL = 14 };
static uint32_t *s_out;
static int s_overflow;
static void emit(uint32_t w)
{
	if (s_out >= s_jit_end) {
		s_overflow = 1;
		return;
	}
	*s_out++ = w;
}
#define RD(r) ((uint32_t)(r) << 12)
#define RN(r) ((uint32_t)(r) << 16)
static void e_movw(int rd, uint32_t v) { emit(0xE3000000u | ((v & 0xf000u) << 4) | RD(rd) | (v & 0xfffu)); }
static void e_movt(int rd, uint32_t v) { emit(0xE3400000u | ((v & 0xf000u) << 4) | RD(rd) | (v & 0xfffu)); }
static void e_mov32(int rd, uint32_t v)
{
	e_movw(rd, v & 0xffffu);
	if (v >> 16)
		e_movt(rd, v >> 16);
}
static void e_ldr(int rd, int rn, uint32_t off) { emit(0xE5900000u | RN(rn) | RD(rd) | off); }
static void e_str(int rd, int rn, uint32_t off) { emit(0xE5800000u | RN(rn) | RD(rd) | off); }
static void e_ldrb(int rd, int rn, uint32_t off) { emit(0xE5D00000u | RN(rn) | RD(rd) | off); }
static void e_strb(int rd, int rn, uint32_t off) { emit(0xE5C00000u | RN(rn) | RD(rd) | off); }
static void e_ldrh(int rd, int rn, uint32_t off) { emit(0xE1D000B0u | RN(rn) | RD(rd) | ((off >> 4) << 8) | (off & 0xfu)); }
static void e_strh(int rd, int rn, uint32_t off) { emit(0xE1C000B0u | RN(rn) | RD(rd) | ((off >> 4) << 8) | (off & 0xfu)); }
static void e_sub_imm(int rd, int rn, uint32_t imm8) { emit(0xE2400000u | RN(rn) | RD(rd) | imm8); }
static void e_add_imm(int rd, int rn, uint32_t imm8) { emit(0xE2800000u | RN(rn) | RD(rd) | imm8); }
static void e_bic_imm(int rd, int rn, uint32_t imm8) { emit(0xE3C00000u | RN(rn) | RD(rd) | imm8); }
static void e_orr_imm_cond(int cond, int rd, int rn, uint32_t imm8) { emit(((uint32_t)cond << 28) | 0x03800000u | RN(rn) | RD(rd) | imm8); }
static void e_orr_lsl(int rd, int rn, int rm, int sh) { emit(0xE1800000u | RN(rn) | RD(rd) | ((uint32_t)sh << 7) | (uint32_t)rm); }
static void e_lsr(int rd, int rm, int sh) { emit(0xE1A00020u | RD(rd) | ((uint32_t)sh << 7) | (uint32_t)rm); }
static void e_movs_lsl(int rd, int rm, int sh) { emit(0xE1B00000u | RD(rd) | ((uint32_t)sh << 7) | (uint32_t)rm); }
static void e_tst_imm(int rn, uint32_t imm8) { emit(0xE3100000u | RN(rn) | imm8); }
static void e_cmp(int rn, int rm) { emit(0xE1500000u | RN(rn) | (uint32_t)rm); }
static void e_cmp_imm(int rn, uint32_t imm8) { emit(0xE3500000u | RN(rn) | imm8); }
static void e_mov_imm_cond(int cond, int rd, uint32_t imm8) { emit(((uint32_t)cond << 28) | 0x03A00000u | RD(rd) | imm8); }
static void e_eor_imm(int rd, int rn, uint32_t imm8) { emit(0xE2200000u | RN(rn) | RD(rd) | imm8); }
static void e_sub(int rd, int rn, int rm) { emit(0xE0400000u | RN(rn) | RD(rd) | (uint32_t)rm); }
static void e_ldrh_r(int rd, int rn, int rm) { emit(0xE19000B0u | RN(rn) | RD(rd) | (uint32_t)rm); }
static void e_strh_r(int rd, int rn, int rm) { emit(0xE18000B0u | RN(rn) | RD(rd) | (uint32_t)rm); }
static void e_ldrb_r(int rd, int rn, int rm) { emit(0xE7D00000u | RN(rn) | RD(rd) | (uint32_t)rm); }
static void e_strb_r(int rd, int rn, int rm) { emit(0xE7C00000u | RN(rn) | RD(rd) | (uint32_t)rm); }
static void e_bx_lr(int cond) { emit(((uint32_t)cond << 28) | 0x012FFF1Eu); }
static void e_blx(int rm) { emit(0xE12FFF30u | (uint32_t)rm); }
// call C function fn: bl to its veneer (ldr pc, [pc, #-4]; .word fn) at the end of the area, else mov32 + blx
static void e_bl_fn(uint32_t fn)
{
	uint32_t *v = NULL;
	for (unsigned i = 0; i < s_ven_n; i++)
		if (s_ven[i].fn == fn) {
			v = s_ven[i].at;
			break;
		}
	if (!v && s_ven_n < sizeof(s_ven) / sizeof(s_ven[0]) && s_jit_end - 2 > s_out) {
		s_jit_end -= 2;
		v = s_jit_end;
		v[0] = 0xE51FF004u;
		v[1] = fn;
		jit_sync(v, 8);
		s_ven[s_ven_n].fn = fn;
		s_ven[s_ven_n++].at = v;
	}
	if (!v) {
		e_mov32(R12, fn);
		e_blx(R12);
		return;
	}
	emit(0xEB000000u | ((uint32_t)(v - (s_out + 2)) & 0x00ffffffu));
}
static void e_push(void) { emit(0xE92D41F0u); } // push {r4-r8, lr}
static void e_ret(void) { emit(0xE8BD81F0u); }  // pop {r4-r8, pc}
static void e_pop(void) // the end of a block (state in h8, r5 = icount): on through s_chain, else return
{
	if (!s_chain) {
		e_ret();
		return;
	}
	emit(0xEA000000u | ((uint32_t)(s_chain - (s_out + 2)) & 0x00ffffffu)); // b s_chain
}
static uint32_t *e_bcond(int cond) // patched by e_fix
{
	uint32_t *at = s_out;
	emit(((uint32_t)cond << 28) | 0x0A000000u);
	return at;
}
static void e_fix(uint32_t *at, const uint32_t *target)
{
	if (!at || at >= s_jit_end)
		return;
	const int32_t off = (int32_t)(target - (at + 2)); // in words, from PC = at + 8 bytes
	*at = (*at & 0xff000000u) | ((uint32_t)off & 0x00ffffffu);
}

// offsets in h8
#define O_ICOUNT ((uint32_t)offsetof(H8State, icount))
#define O_BCOUNT ((uint32_t)offsetof(H8State, bcount))
#define O_PPC ((uint32_t)offsetof(H8State, PPC))
#define O_NPC ((uint32_t)offsetof(H8State, NPC))
#define O_PC ((uint32_t)offsetof(H8State, PC))
#define O_PIR ((uint32_t)offsetof(H8State, PIR))
#define O_IR ((uint32_t)offsetof(H8State, IR))
#define O_R ((uint32_t)offsetof(H8State, R))
#define O_CCR ((uint32_t)offsetof(H8State, CCR))
#define O_IRQ ((uint32_t)offsetof(H8State, irq_vector))
#define O_STATE ((uint32_t)offsetof(H8State, inst_state))
#define O_IRAM ((uint32_t)offsetof(H8State, iram))
_Static_assert(offsetof(H8State, iram) + 0x200 < 0x1000, "on-chip RAM within a 12-bit load offset");

// a C call: icount to h8 first, back after (r0 / r1 set). (PPC, only for diagnostics in C, is the block's first
// instruction: stored once on entry)
static int s_ppc_set;   // the instruction being made called C
static uint32_t s_pend; // states not yet taken from r5
static void e_flush(void)
{
	while (s_pend) {
		const uint32_t n = s_pend < 255 ? s_pend : 254;
		e_sub_imm(R5, R5, n);
		s_pend -= n;
	}
}
static void e_mem(const uint32_t *routine) // r0 = address (r1 = data): read8 / read16 / write8 / write16
{
	e_flush();
	s_mem_used = 1;
	emit(0xEB000000u | ((uint32_t)(routine - (s_out + 2)) & 0x00ffffffu)); // bl
}
int h8_jit_prof;
static void e_phase(int ph) // measurement builds: h8_phase = ph (uses r2, r3)
{
#ifdef RS12_SUB_PROFILE
	if (h8_jit_prof) {
		e_mov32(R2, (uint32_t)(uintptr_t)&h8_phase);
		e_movw(R3, (uint32_t)ph);
		e_strb(R3, R2, 0);
	}
#else
	(void)ph;
#endif
}
// (CCR lives in r8 inside block code: to h8 before C, back after)
static uint8_t s_fl_r; // flags the instruction being made reads (all for a C call)
static void e_call_ph(uint32_t fn, int ph)
{
	s_ppc_set = 1;
	s_fl_r = F_H | F_N | F_Z | F_V | F_C;
	e_flush();
	e_str(R5, R4, O_ICOUNT);
	e_strb(R8, R4, O_CCR);
	e_phase(ph);
	e_bl_fn(fn);
	e_phase(H8PH_CODE);
	e_ldr(R5, R4, O_ICOUNT);
	e_ldrb(R8, R4, O_CCR);
}
static void e_call(uint32_t fn) { e_call_ph(fn, H8PH_HELP); }
static void e_icount(uint32_t states) // icount -= states: pending until r5 is used (e_flush)
{
	s_pend += states;
}

// registers (h8.R: R0-R7 then E0-E7, 16 bits each)
static void e_r16_r(int rd, int reg) { e_ldrh(rd, R4, O_R + 2u * (uint32_t)(reg & 0xf)); }
static void e_r16_w(int rs, int reg) { e_strh(rs, R4, O_R + 2u * (uint32_t)(reg & 0xf)); }
static uint32_t r8_off(int reg) { return O_R + 2u * (uint32_t)(reg & 7) + ((reg & 8) ? 0u : 1u); } // (little endian host)
static void e_r8_r(int rd, int reg) { e_ldrb(rd, R4, r8_off(reg)); }
static void e_r8_w(int rs, int reg) { e_strb(rs, R4, r8_off(reg)); }
static void e_r32_r(int rd, int reg) // uses r3
{
	e_ldrh(rd, R4, O_R + 2u * (uint32_t)(reg & 7));
	e_ldrh(R3, R4, O_R + 2u * (uint32_t)((reg & 7) | 8));
	e_orr_lsl(rd, rd, R3, 16);
}
static void e_r32_w(int rs, int reg) // uses r3
{
	e_strh(rs, R4, O_R + 2u * (uint32_t)(reg & 7));
	e_lsr(R3, rs, 16);
	e_strh(R3, R4, O_R + 2u * (uint32_t)((reg & 7) | 8));
}
static uint8_t s_fl_w;    // flags the instruction being made writes
static int s_flags_skip;  // ... nothing reads them before they are written again: not made
static int s_fix_reg = -1, s_fix_bits; // its skipped set_nzv, made by the out of line checks if they run
// set_nzv8/16/32 of the value in rv (its low bits): CCR in r8; uses r3
static void e_set_nzv(int rv, int bits)
{
	s_fl_w |= F_N | F_V | F_Z;
	if (s_flags_skip) {
		s_fix_reg = rv;
		s_fix_bits = bits;
		return;
	}
	e_bic_imm(R8, R8, F_N | F_V | F_Z);
	e_movs_lsl(R3, rv, 32 - bits);
	e_orr_imm_cond(C_EQ, R8, R8, F_Z);
	e_orr_imm_cond(C_MI, R8, R8, F_N);
}
// r0 = r0 + / - r1 (bits wide, zero-extended in) with the flags of do_add / do_sub (H N Z V C), or of do_inc /
// do_dec (N Z V) when nzv: the ARM flags of the operation on the values shifted to the top; H from the low 4 / 12 /
// 28 bits the same way (H8 C and H are the borrows of a subtraction, ARM C its inverse). Uses r2, r3, r12
static void e_addsub(int sub, int bits, int nzv, int store)
{
	const uint32_t sh = 32u - (uint32_t)bits, hsh = bits == 8 ? 28u : bits == 16 ? 20u : 4u;
	s_fl_w |= nzv ? (F_N | F_Z | F_V) : (F_H | F_N | F_Z | F_V | F_C);
	if (s_flags_skip) {
		if (store)
			emit(sub ? 0xE0400001u : 0xE0800001u); // sub / add r0, r0, r1
		return;
	}
	e_movw(R3, 0);
	emit(0xE1A02000u | (sh << 7) | R0); // lsl r2, r0, #sh (mov r2, r0 when 0)
	emit((sub ? 0xE0522001u : 0xE0922001u) | (sh << 7)); // adds / subs r2, r2, r1, lsl #sh
	e_orr_imm_cond(C_MI, R3, R3, F_N);
	e_orr_imm_cond(C_EQ, R3, R3, F_Z);
	e_orr_imm_cond(6, R3, R3, F_V); // vs
	if (!nzv) {
		e_orr_imm_cond(sub ? 3 : 2, R3, R3, F_C); // cc / cs
		emit(0xE1A0C000u | (hsh << 7) | R0); // lsl r12, r0, #hsh
		emit((sub ? 0xE15C0001u : 0xE17C0001u) | (hsh << 7)); // cmp / cmn r12, r1, lsl #hsh
		e_orr_imm_cond(sub ? 3 : 2, R3, R3, F_H);
	}
	e_bic_imm(R8, R8, nzv ? (F_N | F_Z | F_V) : (F_H | F_N | F_Z | F_V | F_C));
	e_orr_lsl(R8, R8, R3, 0);
	if (store) {
		if (sh)
			e_lsr(R0, R2, (int)sh);
		else
			emit(0xE1A00002u); // mov r0, r2
	}
}
static void e_mov(int rd, int rm) { if (rd != rm) emit(0xE1A00000u | RD(rd) | (uint32_t)rm); }
static void e_lsl(int rd, int rm, int sh) { emit(0xE1A00000u | RD(rd) | ((uint32_t)sh << 7) | (uint32_t)rm); }
static void e_add(int rd, int rn, int rm) { emit(0xE0800000u | RN(rn) | RD(rd) | (uint32_t)rm); }
static void e_add_const(int rd, int rn, uint32_t v) // uses r12 for large values
{
	if (v == 0)
		e_mov(rd, rn);
	else if (v < 256)
		e_add_imm(rd, rn, v);
	else {
		e_mov32(R12, v);
		e_add(rd, rn, R12);
	}
}
// the value of IR[j] while instruction e runs (a prefix word, or a word of the instruction)
static int ir_word(const H8BIns *e, int j, uint32_t *v)
{
	for (unsigned p = 0; p < e->npre; p++)
		if (e->pre_j[p] == j) {
			*v = e->pre_w[p];
			return 1;
		}
	return 0;
}

// read / write at a fixed address: on-chip RAM in place (no C path between: the decrement is the same), else the
// normal accessors (which count the 2 states themselves)
static int iram_at(uint32_t a) { return a - 0xfffd10u < 0x200u; }
static void e_read_at(int rd, uint32_t a, int bits)
{
	a &= 0xffffff;
	if (iram_at(a & (bits == 16 ? 0xfffffeu : 0xffffffu))) {
		e_icount(2);
		if (bits == 16) {
			const uint32_t o = O_IRAM + ((a & ~1u) - 0xfffd10u);
			e_ldrb(rd, R4, o);
			e_ldrb(R3, R4, o + 1);
			e_orr_lsl(rd, R3, rd, 8);
		} else
			e_ldrb(rd, R4, O_IRAM + (a - 0xfffd10u));
		return;
	}
	e_mov32(R0, a);
	e_mem(bits == 16 ? s_rd16 : s_rd8);
	if (rd != R0)
		emit(0xE1A00000u | RD(rd) | R0);
}
static void e_write_at(int rs, uint32_t a, int bits) // value in rs (not r0 / r1)
{
	a &= 0xffffff;
	if (iram_at(a & (bits == 16 ? 0xfffffeu : 0xffffffu))) {
		e_icount(2);
		if (bits == 16) {
			const uint32_t o = O_IRAM + ((a & ~1u) - 0xfffd10u);
			e_lsr(R3, rs, 8);
			e_strb(R3, R4, o);
			e_strb(rs, R4, o + 1);
		} else
			e_strb(rs, R4, O_IRAM + (a - 0xfffd10u));
		return;
	}
	e_mov32(R0, a);
	emit(0xE1A00000u | RD(R1) | (uint32_t)rs);
	e_mem(bits == 16 ? s_wr16 : s_wr8);
}

// a conditional branch whose condition is one CCR bit; target and both next words known (ROM)
static int bcc_bit(h8_blk_fn fn, uint32_t *bit, int *taken_if_set, int *rel16)
{
	static const struct { h8_blk_fn fn; uint32_t bit; int set, rel16; } t[] = {
		{ t_beq_rel8, F_Z, 1, 0 }, { t_bne_rel8, F_Z, 0, 0 }, { t_bmi_rel8, F_N, 1, 0 }, { t_bpl_rel8, F_N, 0, 0 },
		{ t_bcs_rel8, F_C, 1, 0 }, { t_bcc_rel8, F_C, 0, 0 }, { t_bvs_rel8, F_V, 1, 0 }, { t_bvc_rel8, F_V, 0, 0 },
		{ t_beq_rel16, F_Z, 1, 1 }, { t_bne_rel16, F_Z, 0, 1 }, { t_bmi_rel16, F_N, 1, 1 }, { t_bpl_rel16, F_N, 0, 1 },
		{ t_bcs_rel16, F_C, 1, 1 }, { t_bcc_rel16, F_C, 0, 1 }, { t_bvs_rel16, F_V, 1, 1 }, { t_bvc_rel16, F_V, 0, 1 },
	};
	for (unsigned i = 0; i < sizeof(t) / sizeof(t[0]); i++)
		if (t[i].fn == fn) {
			*bit = t[i].bit;
			*taken_if_set = t[i].set;
			*rel16 = t[i].rel16;
			return 1;
		}
	return 0;
}

// the end of a block after a branch: NPC / PC / PIR set; prefetch_done (s_pfd, with IR[0] = the branch while it runs)
static void e_prefetch_done(const H8BIns *e)
{
	e_flush();
	e_movw(R2, e->op);
	e_fix(e_bcond(C_AL), s_pfd);
}

// register forms: add / sub / cmp / inc / dec with the ARM flags, mov / and / or / xor / not with set_nzv
enum { A_ADD, A_SUB, A_CMP, A_INC, A_DEC, A_MOV, A_AND, A_OR, A_XOR, A_NOT };
enum { S_R, S_IMM8, S_IMM16, S_IMM32, S_ONE, S_TWO, S_NONE };
static const struct { h8_blk_fn fn; uint8_t kind, bits, src; } s_alu[] = {
	{ b_add_b_r8h_r8l, A_ADD, 8, S_R }, { b_add_w_r16h_r16l, A_ADD, 16, S_R }, { b_add_l_r32h_r32l, A_ADD, 32, S_R },
	{ b_sub_b_r8h_r8l, A_SUB, 8, S_R }, { b_sub_w_r16h_r16l, A_SUB, 16, S_R }, { b_sub_l_r32h_r32l, A_SUB, 32, S_R },
	{ b_cmp_b_r8h_r8l, A_CMP, 8, S_R }, { b_cmp_w_r16h_r16l, A_CMP, 16, S_R }, { b_cmp_l_r32h_r32l, A_CMP, 32, S_R },
	{ b_add_b_imm8_r8u, A_ADD, 8, S_IMM8 }, { b_cmp_b_imm8_r8u, A_CMP, 8, S_IMM8 },
	{ b_add_w_imm16_r16l, A_ADD, 16, S_IMM16 }, { b_sub_w_imm16_r16l, A_SUB, 16, S_IMM16 },
	{ b_cmp_w_imm16_r16l, A_CMP, 16, S_IMM16 },
	{ b_add_l_imm32_r32l, A_ADD, 32, S_IMM32 }, { b_sub_l_imm32_r32l, A_SUB, 32, S_IMM32 },
	{ b_cmp_l_imm32_r32l, A_CMP, 32, S_IMM32 },
	{ b_inc_b_one_r8l, A_INC, 8, S_ONE }, { b_dec_b_one_r8l, A_DEC, 8, S_ONE },
	{ b_inc_w_one_r16l, A_INC, 16, S_ONE }, { b_inc_w_two_r16l, A_INC, 16, S_TWO },
	{ b_dec_w_one_r16l, A_DEC, 16, S_ONE }, { b_dec_w_two_r16l, A_DEC, 16, S_TWO },
	{ b_inc_l_one_r32l, A_INC, 32, S_ONE }, { b_dec_l_one_r32l, A_DEC, 32, S_ONE },
	{ b_mov_b_r8h_r8l, A_MOV, 8, S_R }, { b_mov_l_r32h_r32l, A_MOV, 32, S_R },
	{ b_and_b_r8h_r8l, A_AND, 8, S_R }, { b_and_w_r16h_r16l, A_AND, 16, S_R },
	{ b_or_b_r8h_r8l, A_OR, 8, S_R }, { b_or_w_r16h_r16l, A_OR, 16, S_R },
	{ b_xor_b_r8h_r8l, A_XOR, 8, S_R }, { b_xor_w_r16h_r16l, A_XOR, 16, S_R },
	{ b_and_b_imm8_r8u, A_AND, 8, S_IMM8 }, { b_or_b_imm8_r8u, A_OR, 8, S_IMM8 }, { b_xor_b_imm8_r8u, A_XOR, 8, S_IMM8 },
	{ b_and_w_imm16_r16l, A_AND, 16, S_IMM16 }, { b_or_w_imm16_r16l, A_OR, 16, S_IMM16 },
	{ b_and_l_imm32_r32l, A_AND, 32, S_IMM32 },
	{ b_not_b_r8l, A_NOT, 8, S_NONE }, { b_not_w_r16l, A_NOT, 16, S_NONE },
};
static void e_reg_r(int rd, int reg, int bits)
{
	if (bits == 8)
		e_r8_r(rd, reg);
	else if (bits == 16)
		e_r16_r(rd, reg);
	else
		e_r32_r(rd, reg);
}
static void e_reg_w(int rs, int reg, int bits)
{
	if (bits == 8)
		e_r8_w(rs, reg);
	else if (bits == 16)
		e_r16_w(rs, reg);
	else
		e_r32_w(rs, reg);
}
// N Z V C from the ARM flags (V = 0 unless with_v; H kept), as the shift / rotate helpers set them
static void e_nzvc(int with_v)
{
	s_fl_w |= F_N | F_Z | F_V | F_C;
	e_movw(R12, 0);
	e_orr_imm_cond(C_MI, R12, R12, F_N);
	e_orr_imm_cond(C_EQ, R12, R12, F_Z);
	if (with_v)
		e_orr_imm_cond(6, R12, R12, F_V); // vs
	e_orr_imm_cond(2, R12, R12, F_C);     // cs
	e_bic_imm(R8, R8, F_N | F_Z | F_V | F_C);
	e_orr_lsl(R8, R8, R12, 0);
}
// shal / shll / shlr / rotxl of a register (do_shal8 ...): r0 in, r0 out
static int jit_shift(const H8BIns *e)
{
	static const struct { h8_blk_fn fn; uint8_t kind, bits; } t[] = {
		{ b_shal_b_r8l, 0, 8 }, { b_shal_w_r16l, 0, 16 }, { b_shll_b_r8l, 1, 8 }, { b_shll_w_r16l, 1, 16 },
		{ b_shlr_b_r8l, 2, 8 }, { b_shlr_w_r16l, 2, 16 }, { b_rotxl_b_r8l, 3, 8 }, { b_rotxl_w_r16l, 3, 16 },
	};
	unsigned k = 0;
	while (k < sizeof(t) / sizeof(t[0]) && t[k].fn != e->fn)
		k++;
	if (k == sizeof(t) / sizeof(t[0]))
		return 0;
	const int kind = t[k].kind, bits = t[k].bits;
	const uint32_t sh = 32u - (uint32_t)bits;
	e_icount(2);
	e_reg_r(R0, e->op, bits);
	if (kind == 0 || kind == 1) { // v + v: C = top, V = top ^ second (shal), N Z of the result
		emit(0xE1A02000u | (sh << 7) | R0); // lsl r2, r0, #sh
		emit(0xE0922002u);                  // adds r2, r2, r2
		if (s_flags_skip)
			s_fl_w |= F_N | F_Z | F_V | F_C;
		else
			e_nzvc(kind == 0);
		e_lsr(R0, R2, (int)sh);             // v << 1 (bits wide)
	} else if (kind == 2) { // C = bit 0, N = 0
		emit(0xE1B000A0u); // lsrs r0, r0, #1
		if (s_flags_skip)
			s_fl_w |= F_N | F_Z | F_V | F_C;
		else
			e_nzvc(0);
	} else { // rotxl: (v << 1) | C, C = top
		s_fl_r |= F_C;
		emit(0xE2082001u);                  // and r2, r8, #1
		emit(0xE1820080u);                  // orr r0, r2, r0, lsl #1
		emit(0xE1B03000u | (sh << 7) | R0); // lsls r3, r0, #sh
		if (s_flags_skip)
			s_fl_w |= F_N | F_Z | F_V | F_C;
		else
			e_nzvc(0);
	}
	e_reg_w(R0, e->op, bits);
	return 1;
}

static int jit_alu(const H8BIns *e)
{
	const h8_blk_fn f = e->fn;
	const int op = e->op;
	unsigned k = 0;
	while (k < sizeof(s_alu) / sizeof(s_alu[0]) && s_alu[k].fn != f)
		k++;
	if (k == sizeof(s_alu) / sizeof(s_alu[0]))
		return 0;
	const int kind = s_alu[k].kind, bits = s_alu[k].bits, src = s_alu[k].src;
	const int rd = src == S_IMM8 ? op >> 8 : op;
	// states: the immediate words, then the next instruction's first word
	e_icount(src == S_IMM16 ? 4u : src == S_IMM32 ? 6u : 2u);
	if (kind != A_MOV)
		e_reg_r(R0, rd, bits);
	if (src == S_R)
		e_reg_r(R1, op >> 4, bits);
	else if (src == S_IMM8)
		e_movw(R1, (uint32_t)op & 0xff);
	else if (src == S_IMM16)
		e_movw(R1, e->w[0]);
	else if (src == S_IMM32)
		e_mov32(R1, (uint32_t)e->w[0] << 16 | e->w[1]);
	else if (src == S_ONE || src == S_TWO)
		e_movw(R1, src == S_ONE ? 1u : 2u);
	switch (kind) {
	case A_ADD: case A_SUB: case A_CMP: case A_INC: case A_DEC:
		e_addsub(kind == A_SUB || kind == A_CMP || kind == A_DEC, bits, kind == A_INC || kind == A_DEC, kind != A_CMP);
		if (kind != A_CMP)
			e_reg_w(R0, rd, bits);
		return 1;
	case A_MOV:
		e_set_nzv(R1, bits);
		e_reg_w(R1, rd, bits);
		return 1;
	default:
		if (kind == A_AND)
			emit(0xE0000001u); // and r0, r0, r1
		else if (kind == A_OR)
			emit(0xE1800001u); // orr r0, r0, r1
		else if (kind == A_XOR)
			emit(0xE0200001u); // eor r0, r0, r1
		else
			emit(0xE1E00000u); // mvn r0, r0
		e_set_nzv(R0, bits);
		e_reg_w(R0, rd, bits);
		return 1;
	}
}

// on to the block at tgt without leaving the code (r4 / r5 / r8 as they are; r7 = 0 there) when no instruction
// boundary of it can reach bcount and it has code, or gets it later (the branch is patched then); else to *slow
// (both branches returned, for the caller to fix). 0: no such block (an idle loop head, no code ever, ...)
static int e_link(uint32_t tgt, uint32_t **slow_le, uint32_t **slow_b)
{
	*slow_le = *slow_b = NULL;
	const uint32_t blk_end = h8.rom_size < sizeof(s_blk_page) / sizeof(s_blk_page[0]) * H8BLK_PAGE * 2 ?
		h8.rom_size : (uint32_t)(sizeof(s_blk_page) / sizeof(s_blk_page[0]) * H8BLK_PAGE * 2);
	if (!h8_jit_link || (tgt & 1) || tgt >= blk_end || !h8_blk_enable)
		return 0;
	const H8Blk *t = tgt == s_cur->ins[0].a ? s_cur : blk_get(tgt);
	if (t == H8BLK_NONE || !t || t->max_cost >= 0xffff || (t != s_cur && !t->code && t->jit_tried))
		return 0;
	e_flush();
	e_ldr(R1, R4, O_BCOUNT);
	if (t->max_cost < 256)
		e_sub_imm(R0, R5, t->max_cost);
	else {
		e_mov32(R12, t->max_cost);
		e_sub(R0, R5, R12);
	}
	e_cmp(R0, R1);
	*slow_le = e_bcond(C_LE);
	uint32_t *const site = s_out;
	if (t == s_cur)
		e_fix(e_bcond(C_AL), s_cur_body);
	else if (t->body)
		e_fix(e_bcond(C_AL), (const uint32_t *)t->body);
	else {
		*slow_b = e_bcond(C_AL);
		if (!s_dry && s_link_new_n < sizeof(s_link_new) / sizeof(s_link_new[0])) {
			s_link_new[s_link_new_n].tgt = tgt;
			s_link_new[s_link_new_n++].site = site;
		}
	}
	return 1;
}

// the end of a block at a known address (after a jump or branch): on to its block (e_link) unless an interrupt is
// pending, else NPC / PC / PIR and prefetch_done
static void e_goto(const H8BIns *e, uint32_t tgt, uint16_t pir)
{
	uint32_t *slow_irq = NULL, *slow_le, *slow_b;
	e_flush();
	e_ldr(R1, R4, O_IRQ);
	e_cmp_imm(R1, 0);
	slow_irq = e_bcond(C_NE);
	if (!e_link(tgt, &slow_le, &slow_b)) {
		e_fix(slow_irq, s_out); // (falls through)
		slow_irq = NULL;
	}
	e_fix(slow_irq, s_out);
	e_fix(slow_le, s_out);
	e_fix(slow_b, s_out);
	e_mov32(R0, tgt); // (s_goto_slow: NPC, PC, PIR, then prefetch_done)
	e_movw(R1, pir);
	e_movw(R2, e->op);
	e_fix(e_bcond(C_AL), s_goto_slow);
}

static int jit_native(const H8BIns *e)
{
	const h8_blk_fn f = e->fn;
	const int op = e->op;
	uint32_t bit;
	int set, rel16;
	if (((h8_jit_native & 1) && jit_alu(e)) || ((h8_jit_native & 2) && jit_shift(e)))
		return 1;
	if (f == b_mulxu_w_r16h_r32l && (h8_jit_native & 4)) { // ERd = Rd * Rs (16 x 16), internal(19)
		e_icount(22);
		e_r16_r(R0, op & 7);
		e_r16_r(R1, op >> 4);
		emit(0xE0000190u); // mul r0, r0, r1
		e_r32_w(R0, op);
		return 1;
	}
	if ((f == t_jmp_r32h || f == t_rts) && (h8_jit_native & 8)) { // the target from a register / the stack: on through s_chain
		e_icount(2); // (PIR = the next word)
		if (f == t_jmp_r32h)
			e_r32_r(R6, op >> 4);
		else { // tmp1 = ER7; tmp2 = @tmp1 << 16 | @(tmp1 + 2); ER7 = tmp1 + 4; internal(1)
			e_r32_r(R6, 7);
			e_mov(R0, R6);
			e_mem(s_rd16);
			e_lsl(R0, R0, 16);
			e_str(R0, R4, O_PC); // (PC: a place for the high word)
			e_add_imm(R0, R6, 2);
			e_mem(s_rd16);
			e_add_imm(R2, R6, 4);
			e_r32_w(R2, 7);
			e_ldr(R1, R4, O_PC);
			e_orr_lsl(R6, R1, R0, 0);
			e_icount(2);
		}
		e_mov(R0, R6); // PIR = read16i(PC); NPC = PC & 0xffffff; PC += 2
		e_mem(s_rd16);
		e_strh(R0, R4, O_PIR);
		e_bic_imm(R1, R6, 0x4FFu);
		e_str(R1, R4, O_NPC);
		e_add_imm(R1, R6, 2);
		e_str(R1, R4, O_PC);
		e_prefetch_done(e);
		return 2;
	}
	if ((f == t_jsr_abs24e || f == t_bsr_rel8) && (h8_jit_native & 16)) { // push the return address, on to the target
		const uint32_t tgt = f == t_jsr_abs24e ? ((uint32_t)op & 0xff) << 16 | e->w[0] : e->pc + (uint32_t)(int32_t)(int8_t)op;
		const uint32_t ret = f == t_jsr_abs24e ? e->pc + 2u : e->pc;
		if ((tgt & 1) || tgt + 2 > h8.rom_size || (tgt & 0xffffff) != tgt)
			return 0;
		e_icount(f == t_jsr_abs24e ? 6u : 4u); // (IR1 / PIR, internal(1) for jsr, the word at the target)
		e_r32_r(R6, 7);
		e_sub_imm(R6, R6, 4);
		e_r32_w(R6, 7);
		e_mov(R0, R6);
		e_movw(R1, ret >> 16);
		e_mem(s_wr16);
		e_add_imm(R0, R6, 2);
		e_movw(R1, ret & 0xffff);
		e_mem(s_wr16);
		e_goto(e, tgt, s_rom16[tgt >> 1]);
		return 2;
	}
	if (!(h8_jit_native & 32))
		return 0;
	if (f == b_mov_w_abs16_r16l || f == b_mov_b_abs16_r8l) { // IR1, PIR, read, flags, register
		const int bits = f == b_mov_w_abs16_r16l ? 16 : 8;
		e_icount(4);
		e_read_at(R6, (uint32_t)(int32_t)(int16_t)e->w[0], bits);
		e_set_nzv(R6, bits);
		if (bits == 16)
			e_r16_w(R6, op);
		else
			e_r8_w(R6, op);
		return 1;
	}
	if (f == b_mov_w_r16l_abs16 || f == b_mov_b_r8l_abs16) { // IR1, PIR, register, flags, write
		const int bits = f == b_mov_w_r16l_abs16 ? 16 : 8;
		e_icount(4);
		if (bits == 16)
			e_r16_r(R6, op);
		else
			e_r8_r(R6, op);
		e_set_nzv(R6, bits);
		e_write_at(R6, (uint32_t)(int32_t)(int16_t)e->w[0], bits);
		return 1;
	}
	if (f == b_mov_w_r16h_r16l) {
		e_r16_r(R6, op >> 4);
		e_set_nzv(R6, 16);
		e_r16_w(R6, op);
		e_icount(2);
		return 1;
	}
	if (f == b_mov_w_imm16_r16l) {
		e_icount(2);
		e_movw(R6, e->w[0]);
		e_set_nzv(R6, 16);
		e_r16_w(R6, op);
		e_icount(2);
		return 1;
	}
	if (f == b_mov_b_imm8_r8u) {
		e_movw(R6, (uint32_t)op & 0xff);
		e_set_nzv(R6, 8);
		e_r8_w(R6, op >> 8);
		e_icount(2);
		return 1;
	}
	if (f == b_mov_l_imm32_r32l) {
		e_icount(4);
		e_mov32(R6, (uint32_t)e->w[0] << 16 | e->w[1]);
		e_set_nzv(R6, 32);
		e_r32_w(R6, op);
		e_icount(2);
		return 1;
	}
	if (f == b_adds_l_one_r32l || f == b_adds_l_two_r32l || f == b_adds_l_four_r32l) {
		e_r32_r(R6, op);
		e_add_imm(R6, R6, f == b_adds_l_one_r32l ? 1u : f == b_adds_l_two_r32l ? 2u : 4u);
		e_r32_w(R6, op);
		e_icount(2);
		return 1;
	}
	if (f == b_mov_b_abs32_r8l || f == b_mov_w_abs32_r16l) { // IR1, IR2, PIR, read, flags, register
		const int bits = f == b_mov_w_abs32_r16l ? 16 : 8;
		e_icount(6);
		e_read_at(R6, (uint32_t)e->w[0] << 16 | e->w[1], bits);
		e_set_nzv(R6, bits);
		if (bits == 16)
			e_r16_w(R6, op);
		else
			e_r8_w(R6, op);
		return 1;
	}
	if (f == b_mov_b_r8l_abs32 || f == b_mov_w_r16l_abs32) {
		const int bits = f == b_mov_w_r16l_abs32 ? 16 : 8;
		e_icount(6);
		if (bits == 16)
			e_r16_r(R6, op);
		else
			e_r8_r(R6, op);
		e_set_nzv(R6, bits);
		e_write_at(R6, (uint32_t)e->w[0] << 16 | e->w[1], bits);
		return 1;
	}
	if (f == b_mov_w_r32d16h_r16l || f == b_mov_b_r32d16h_r8l) { // IR1, PIR, address, read, flags, register
		const int bits = f == b_mov_w_r32d16h_r16l ? 16 : 8;
		e_icount(4);
		e_r32_r(R0, op >> 4);
		e_add_const(R0, R0, (uint32_t)(int32_t)(int16_t)e->w[0]);
		e_mem(bits == 16 ? s_rd16 : s_rd8);
		e_mov(R6, R0);
		e_set_nzv(R6, bits);
		if (bits == 16)
			e_r16_w(R6, op);
		else
			e_r8_w(R6, op);
		return 1;
	}
	if (f == b_mov_w_r16l_r32d16h || f == b_mov_b_r8l_r32d16h) { // IR1, PIR, address, register, flags, write
		const int bits = f == b_mov_w_r16l_r32d16h ? 16 : 8;
		e_icount(4);
		if (bits == 16)
			e_r16_r(R6, op);
		else
			e_r8_r(R6, op);
		e_set_nzv(R6, bits);
		e_r32_r(R0, op >> 4);
		e_add_const(R0, R0, (uint32_t)(int32_t)(int16_t)e->w[0]);
		e_mov(R1, R6);
		e_mem(bits == 16 ? s_wr16 : s_wr8);
		return 1;
	}
	if (f == b_mov_w_r32ih_r16l) { // PIR, read @ERs, flags, register
		e_icount(2);
		e_r32_r(R0, op >> 4);
		e_mem(s_rd16);
		e_mov(R6, R0);
		e_set_nzv(R6, 16);
		e_r16_w(R6, op);
		return 1;
	}
	uint32_t ir1;
	if ((f == b_mov_l_r32ph_r32l || f == b_mov_l_r32l_pr32h) && ir_word(e, 1, &ir1)) {
		const int rs = (int)(ir1 >> 4), rd = (int)ir1;
		e_icount(4); // PIR, internal(1)
		if (f == b_mov_l_r32ph_r32l) { // pop: tmp1 = @ERs (two words), ERs += 4, flags, ERd = tmp1
			e_r32_r(R0, rs);
			e_mem(s_rd16);
			e_mov(R6, R0);
			e_r32_r(R0, rs);
			e_add_imm(R0, R0, 2);
			e_mem(s_rd16);
			e_orr_lsl(R6, R0, R6, 16);
			e_r32_r(R0, rs);
			e_add_imm(R0, R0, 4);
			e_r32_w(R0, rs);
			e_set_nzv(R6, 32);
			e_r32_w(R6, rd);
		} else { // push: tmp1 = ERs; ERd -= 4; flags; @ERd = tmp1 (two words)
			e_r32_r(R6, rd);
			e_r32_r(R0, rs);
			e_sub_imm(R0, R0, 4);
			e_r32_w(R0, rs);
			e_set_nzv(R6, 32);
			e_lsr(R1, R6, 16);
			e_mem(s_wr16);
			e_r32_r(R0, rs);
			e_add_imm(R0, R0, 2);
			e_mov(R1, R6);
			e_mem(s_wr16);
		}
		return 1;
	}
	if (f == t_jmp_abs24e) { // IR1, internal(1), PC = target, NPC, PIR = word at the target, prefetch_done
		const uint32_t tgt = ((uint32_t)op & 0xff) << 16 | e->w[0];
		if ((tgt & 1) || tgt + 2 > h8.rom_size)
			return 0;
		e_icount(6);
		e_goto(e, tgt, s_rom16[tgt >> 1]);
		return 2;
	}
	if (bcc_bit(f, &bit, &set, &rel16)) { // tmp1 = target; NPC = PC; PIR = next word; tmp2 = word at the target
		// rel8: the displacement is in the first word, the next word is w[0]; rel16: displacement w[0], next word w[1]
		const uint32_t pc = e->pc + (rel16 ? 2u : 0u);
		const uint32_t tgt = pc + (rel16 ? (uint32_t)(int32_t)(int16_t)e->w[0] : (uint32_t)(int32_t)(int8_t)op);
		const uint16_t next = rel16 ? e->w[1] : e->w[0];
		if ((tgt & 1) || tgt + 2 > h8.rom_size || (tgt & 0xffffff) != tgt)
			return 0; // (the target word is read through the normal path)
		e_icount(rel16 ? 6 : 4);
		e_flush(); // (before the paths split)
		s_fl_r |= (uint8_t)bit;
		e_tst_imm(R8, bit);
		uint32_t *nt = e_bcond(set ? C_EQ : C_NE); // not taken
		e_goto(e, tgt, s_rom16[tgt >> 1]);
		e_fix(nt, s_out);
		e_goto(e, pc, next);
		return 2; // (ends the block)
	}
	return 0;
}

// a read or write as read8 / read16 / write8 / write16 (2 states, then mem_*): shared RAM, on-chip RAM and ROM here,
// the rest through C (r7 = 1 then: the instruction's checks follow). In r0 = address, r1 = data; out r0; uses r1-r3.
static uint32_t *jit_mem_make(int bits, int write)
{
	uint32_t *const start = s_out;
	e_sub_imm(R5, R5, 2);
	e_bic_imm(R0, R0, 0x4FFu); // & 0xffffff
	if (bits == 16)
		e_bic_imm(R0, R0, 1);
	// shared RAM (host-endian words)
	e_mov32(R2, h8.ram_base);
	e_sub(R2, R0, R2);
	e_mov32(R3, h8.ram_size);
	e_cmp(R2, R3);
	uint32_t *const not_ram = e_bcond(C_HS);
	e_mov32(R3, (uint32_t)(uintptr_t)h8.ram);
	if (bits == 8)
		e_eor_imm(R2, R2, 1);
	if (write)
		bits == 16 ? e_strh_r(R1, R3, R2) : e_strb_r(R1, R3, R2);
	else
		bits == 16 ? e_ldrh_r(R0, R3, R2) : e_ldrb_r(R0, R3, R2);
	e_bx_lr(C_AL);
	e_fix(not_ram, s_out);
	// on-chip RAM (big-endian bytes)
	e_mov32(R2, 0xfffd10u);
	e_sub(R2, R0, R2);
	e_cmp_imm(R2, 0xC02u); // 0x200
	uint32_t *const not_iram = e_bcond(C_HS);
	e_add(R2, R2, R4);
	if (write) {
		if (bits == 16) {
			e_lsr(R3, R1, 8);
			e_strb(R3, R2, O_IRAM);
			e_strb(R1, R2, O_IRAM + 1);
		} else
			e_strb(R1, R2, O_IRAM);
	} else {
		if (bits == 16) {
			e_ldrb(R0, R2, O_IRAM);
			e_ldrb(R1, R2, O_IRAM + 1);
			e_orr_lsl(R0, R1, R0, 8);
		} else
			e_ldrb(R0, R2, O_IRAM);
	}
	e_bx_lr(C_AL);
	e_fix(not_iram, s_out);
	// ROM (writes ignored)
	e_mov32(R2, h8.rom_size);
	e_cmp(R0, R2);
	uint32_t *not_rom = NULL;
	if (write)
		e_bx_lr(3); // bxlo lr
	else {
		not_rom = e_bcond(C_HS);
		if (bits == 16) {
			e_mov32(R3, (uint32_t)(uintptr_t)s_rom16);
			e_ldrh_r(R0, R3, R0);
		} else {
			e_mov32(R3, (uint32_t)(uintptr_t)h8.rom);
			e_ldrb_r(R0, R3, R0);
		}
		e_bx_lr(C_AL);
		e_fix(not_rom, s_out);
	}
	// the rest through C (which counts the 2 states itself)
	e_add_imm(R5, R5, 2);
	emit(0xE92D5000u); // push {r12, lr}
	e_str(R5, R4, O_ICOUNT);
	e_mov32(R12, (uint32_t)(uintptr_t)(write ? (bits == 16 ? (void *)jit_write16 : (void *)jit_write8) :
		(bits == 16 ? (void *)jit_read16 : (void *)jit_read8)));
	e_blx(R12);
	e_ldr(R5, R4, O_ICOUNT);
	emit(0xE3A07001u); // mov r7, #1
	emit(0xE8BD9000u); // pop {r12, pc}
	return start;
}

// the shared block end: as h8_run's loop would, run the block at NPC if it has code, its first word is the state
// (no interrupt or exception sequence) and no instruction boundary in it can reach bcount; else return to h8_run.
// A tail call (the next block pushes again), so blocks may follow one another without limit: icount falls each time.
static void jit_shared_make(void)
{
	const uint32_t blk_end = h8.rom_size < sizeof(s_blk_page) / sizeof(s_blk_page[0]) * H8BLK_PAGE * 2 ?
		h8.rom_size : (uint32_t)(sizeof(s_blk_page) / sizeof(s_blk_page[0]) * H8BLK_PAGE * 2);
	s_out = s_jit_cur;
	s_overflow = 0;
	uint32_t *const start = s_out;
	uint32_t *ret[8];
	int nr = 0;
	e_ldr(R5, R4, O_ICOUNT); // (an interrupt taken at the end moved it)
	e_ldr(R3, R4, O_STATE);
	emit(0xE1B01823u); // lsrs r1, r3, #16
	ret[nr++] = e_bcond(C_NE);
	e_ldr(R0, R4, O_NPC);
	e_mov32(R1, blk_end);
	e_cmp(R0, R1);
	ret[nr++] = e_bcond(C_HS);
	e_mov32(R1, (uint32_t)(uintptr_t)s_blk_page);
	e_lsr(R2, R0, 9);           // page: NPC / (H8BLK_PAGE * 2)
	emit(0xE7911102u);          // ldr r1, [r1, r2, lsl #2]
	e_cmp_imm(R1, 0);
	ret[nr++] = e_bcond(C_EQ);
	emit(0xE7E720D0u);          // ubfx r2, r0, #1, #8: word in the page
	emit(0xE7911102u);          // ldr r1, [r1, r2, lsl #2]
	e_cmp_imm(R1, 1);           // NULL or H8BLK_NONE
	ret[nr++] = e_bcond(C_LS);
	e_ldr(R2, R1, (uint32_t)offsetof(H8Blk, code));
	e_cmp_imm(R2, 0);
	ret[nr++] = e_bcond(C_EQ);
	e_ldr(R0, R2, 0); // (its entry must be the push of a block: h8_run checks and logs, ADR-064)
	e_mov32(R12, 0xE92D41F0u);
	e_cmp(R0, R12);
	ret[nr++] = e_bcond(C_NE);
	e_ldrh(R0, R1, (uint32_t)(offsetof(H8Blk, ins) + offsetof(H8BIns, op)));
	e_cmp(R0, R3);
	ret[nr++] = e_bcond(C_NE);
	e_ldr(R3, R1, (uint32_t)offsetof(H8Blk, max_cost));
	emit(0xE0450003u);          // sub r0, r5, r3
	e_ldr(R1, R4, O_BCOUNT);
	e_cmp(R0, R1);
	ret[nr++] = e_bcond(C_LE);
	emit(0xE8BD41F0u);          // pop {r4-r8, lr}
	emit(0xE12FFF12u);          // bx r2
	uint32_t *const r = s_out;
	e_ret();
	for (int i = 0; i < nr; i++)
		e_fix(ret[i], r);
	s_rd8 = jit_mem_make(8, 0);
	s_rd16 = jit_mem_make(16, 0);
	s_wr8 = jit_mem_make(8, 1);
	s_wr16 = jit_mem_make(16, 1);
	// block ends: s_goto_slow (r0 = the target, r1 = its first word, r2 = the branch's first word): NPC, PC, PIR;
	// s_pfd (r2): prefetch_done with IR[0] = the branch while an interrupt is taken, then on through the chain
	s_goto_slow = s_out;
	e_str(R0, R4, O_NPC);
	e_add_imm(R3, R0, 2);
	e_str(R3, R4, O_PC);
	e_strh(R1, R4, O_PIR);
	s_pfd = s_out;
	e_str(R5, R4, O_ICOUNT);
	e_strb(R8, R4, O_CCR);
	e_strh(R2, R4, O_IR);
	e_ldr(R1, R4, O_IRQ);
	e_cmp_imm(R1, 0);
	uint32_t *const irq = e_bcond(C_NE);
	e_ldrh(R0, R4, O_PIR);
	e_strh(R0, R4, O_IR);
	e_str(R0, R4, O_STATE);
	if (h8_jit_chain)
		e_fix(e_bcond(C_AL), start);
	else
		e_ret();
	e_fix(irq, s_out);
	e_bl_fn((uint32_t)(uintptr_t)jit_take_interrupt);
	if (h8_jit_chain)
		e_fix(e_bcond(C_AL), start);
	else
		e_ret();
	if (s_overflow) {
		s_jit_full = 1;
		return;
	}
	jit_sync(start, (size_t)(s_out - start) * 4);
	s_jit_cur = s_out;
	s_chain = h8_jit_chain ? start : NULL;
}

// code for block b; NULL if it does not fit (the block then keeps running through blk_run)
static void *jit_block(const H8Blk *b)
{
	if (s_jit_full || !s_jit_base || (h8_jit_max >= 0 && h8_jit_blocks >= (uint32_t)h8_jit_max))
		return NULL;
	if (!s_shared_made) {
		s_shared_made = 1;
		jit_shared_make();
		if (s_jit_full)
			return NULL;
	}
	s_out = s_jit_cur;
	s_overflow = 0;
	uint32_t *const start = s_out;
	if (b->max_cost >= 0xffff)
		return NULL; // (an instruction of unknown length in states: blk_run)
	// exits to blk_exit_before / blk_exit_irq (r0 = index of the instruction, set with the branch)
	uint32_t *fx_before[2 * H8BLK_MAX], *fx_irq[2 * H8BLK_MAX];
	unsigned n_before = 0, n_irq = 0;
	// out of line checks after an instruction whose memory access went through C (r7)
	struct { uint32_t *from, *back, rem, pend; unsigned i; int irq, fix_reg, fix_bits; } stub[H8BLK_MAX];
	unsigned n_stub = 0;
	// flags: an instruction makes the ones it writes only if something can read them before they are written again
	// (an instruction, C, an exit); found from a dry run of each instruction (flags written / read, C calls, memory
	// accesses: those exit only through the out of line checks, which then make the skipped set_nzv)
	enum { ALLF = F_H | F_N | F_Z | F_V | F_C };
	uint8_t need[H8BLK_MAX];
	{
		uint8_t w[H8BLK_MAX], r[H8BLK_MAX], cinst[H8BLK_MAX], mem[H8BLK_MAX];
		unsigned nn = 0;
		uint32_t *const save = s_out;
		const int save_ovf = s_overflow;
		s_cur = b;
		s_cur_body = s_out;
		s_dry = 1;
		for (unsigned i = 0; i < b->n; i++) {
			s_ppc_set = s_mem_used = 0;
			s_fl_w = s_fl_r = 0;
			s_flags_skip = 0;
			s_pend = 0;
			const int nat = h8_jit_native ? jit_native(&b->ins[i]) : 0;
			w[i] = nat ? s_fl_w : 0;
			r[i] = nat && !s_ppc_set ? s_fl_r : ALLF;
			cinst[i] = !nat || s_ppc_set;
			mem[i] = (uint8_t)(s_mem_used && !s_ppc_set);
			s_out = save;
			nn++;
			if (nat == 2)
				break;
		}
		s_overflow = save_ovf;
		s_pend = 0;
		s_dry = 0;
		s_link_new_n = 0;
		uint8_t live = ALLF;
		for (unsigned i = nn; i-- > 0;) {
			const uint8_t after = i == 0 || cinst[i] ? (uint8_t)ALLF : live;
			need[i] = (w[i] & after) != 0;
			uint8_t before = (uint8_t)((after & ~w[i]) | r[i]);
			if (mem[i])
				before |= (uint8_t)(ALLF & ~w[i]);
			live = before;
		}
	}
	uint32_t remaining = b->max_cost; // states of the instructions from the next one on
	int ended = 0;
	e_push();
	e_mov32(R4, (uint32_t)(uintptr_t)&h8);
	e_ldr(R5, R4, O_ICOUNT);
	e_ldrb(R8, R4, O_CCR);
	e_mov32(R12, b->ins[0].a);
	e_str(R12, R4, O_PPC);
	s_cur_body = s_out; // (linked blocks enter here)
	emit(0xE3A07000u); // mov r7, #0
	for (unsigned i = 0; i < b->n; i++) {
		const H8BIns *e = &b->ins[i];
		const int last_term = b->term && i == b->n - 1u;
		s_ppc_set = 0;
		s_mem_used = 0;
		s_fl_w = s_fl_r = 0;
		s_flags_skip = !need[i];
		s_fix_reg = -1;
		remaining -= e->cost;
		for (unsigned p = 0; p < e->npre; p++) { // (no boundary check: the entry check covers them)
			e_icount(2);
			e_movw(R1, e->pre_w[p]);
			e_strh(R1, R4, O_IR + 2u * e->pre_j[p]);
		}
		const int nat = h8_jit_native ? jit_native(e) : 0;
		((H8BIns *)e)->native = (uint8_t)(nat ? (s_ppc_set ? 2 : 1) : 0); // (2: made here but calls C)
		if (nat == 2) {
			ended = 1;
			h8_jit_native_n++;
			break;
		}
		if (nat)
			h8_jit_native_n++;
		else {
			h8_jit_call_n++;
			if (last_term) {
				e_movw(R0, e->op);
				e_strh(R0, R4, O_IR);
			}
			e_mov32(R0, (uint32_t)(uintptr_t)e);
			e_call_ph((uint32_t)(uintptr_t)e->fn, H8PH_CALL);
		}
		s_flags_skip = 0;
		if (s_mem_used && s_ppc_set)
			emit(0xE3A07000u); // mov r7, #0 (the checks follow anyway)
		// interrupt check: after the first instruction (one may be pending on entry), then only after instructions
		// that called C (nothing else changes irq_vector)
		if (!last_term && (i == 0 || s_ppc_set)) {
			e_flush();
			e_ldr(R0, R4, O_IRQ);
			e_cmp_imm(R0, 0);
			e_mov_imm_cond(C_NE, R0, i);
			fx_irq[n_irq++] = e_bcond(C_NE);
		}
		// after a C call bcount may have moved: stop before the next instruction unless the rest still fits
		if (s_ppc_set && i + 1 < b->n && remaining) {
			e_flush();
			e_ldr(R1, R4, O_BCOUNT);
			if (remaining < 256)
				e_sub_imm(R0, R5, remaining);
			else {
				e_mov32(R12, remaining);
				e_sub(R0, R5, R12);
			}
			e_cmp(R0, R1);
			e_mov_imm_cond(C_LE, R0, i + 1);
			fx_before[n_before++] = e_bcond(C_LE);
		}
		// a memory access only: the same checks out of line when it went through C
		if (s_mem_used && !s_ppc_set && ((!last_term && i != 0) || (i + 1 < b->n && remaining))) {
			e_cmp_imm(R7, 0);
			stub[n_stub].from = e_bcond(C_NE);
			stub[n_stub].back = s_out;
			stub[n_stub].rem = remaining;
			stub[n_stub].pend = s_pend;
			stub[n_stub].i = i;
			stub[n_stub].irq = !last_term && i != 0;
			stub[n_stub].fix_reg = s_fix_reg;
			stub[n_stub].fix_bits = s_fix_bits;
			n_stub++;
		}
	}
	if (!ended) {
		if (!b->term) { // (no interrupt can be pending here that the checks above did not see)
			uint32_t *slow_le, *slow_b;
			if (e_link(b->ins[b->n - 1].next, &slow_le, &slow_b)) {
				e_fix(slow_le, s_out);
				e_fix(slow_b, s_out);
			}
			e_mov32(R0, (uint32_t)(uintptr_t)&b->ins[b->n - 1]);
			e_call((uint32_t)(uintptr_t)blk_exit_end);
		} else {
			e_flush();
			e_str(R5, R4, O_ICOUNT); // (CCR: the call of the last instruction stored it)
		}
		e_pop();
	}
	s_pend = 0;
	for (unsigned k = 0; k < n_stub; k++) {
		const unsigned i = stub[k].i;
		e_fix(stub[k].from, s_out);
		emit(0xE3A07000u); // mov r7, #0
		if (stub[k].fix_reg >= 0)
			e_set_nzv(stub[k].fix_reg, stub[k].fix_bits);
		s_pend = stub[k].pend; // (taken here, given back on the way back)
		e_flush();
		if (stub[k].irq) {
			e_ldr(R0, R4, O_IRQ);
			e_cmp_imm(R0, 0);
			e_mov_imm_cond(C_NE, R0, i);
			fx_irq[n_irq++] = e_bcond(C_NE);
		}
		if (i + 1 < b->n && stub[k].rem) {
			e_ldr(R1, R4, O_BCOUNT);
			if (stub[k].rem < 256)
				e_sub_imm(R0, R5, stub[k].rem);
			else {
				e_mov32(R12, stub[k].rem);
				e_sub(R0, R5, R12);
			}
			e_cmp(R0, R1);
			e_mov_imm_cond(C_LE, R0, i + 1);
			fx_before[n_before++] = e_bcond(C_LE);
		}
		if (stub[k].pend)
			e_add_imm(R5, R5, stub[k].pend);
		e_fix(e_bcond(C_AL), stub[k].back);
	}
	// exits (r0 = index of the instruction): r0 = &b->ins[r0], then blk_exit_before / blk_exit_irq
	for (unsigned t = 0; t < 2; t++) {
		const unsigned nf = t == 0 ? n_before : n_irq;
		if (!nf)
			continue;
		for (unsigned k = 0; k < nf; k++)
			e_fix(t == 0 ? fx_before[k] : fx_irq[k], s_out);
		e_str(R5, R4, O_ICOUNT);
		e_strb(R8, R4, O_CCR);
		e_mov32(R1, (uint32_t)(uintptr_t)b->ins);
		e_movw(R2, sizeof(H8BIns));
		emit(0xE0201290u); // mla r0, r0, r2, r1
		e_bl_fn((uint32_t)(uintptr_t)(t == 0 ? (void *)blk_exit_before : (void *)blk_exit_irq));
		e_pop();
	}
	if (s_overflow) {
		s_jit_full = 1; // (the area is used up: the remaining blocks run through blk_run)
		return NULL;
	}
	jit_sync(start, (size_t)(s_out - start) * 4);
	s_jit_cur = s_out;
	((H8Blk *)b)->body = s_cur_body;
	// its new branches to blocks without code wait for them; branches waiting for it go to its body now
	for (unsigned k = 0; k < s_link_new_n && s_link_n < LINK_MAX; k++)
		s_link[s_link_n++] = s_link_new[k];
	for (unsigned k = 0; k < s_link_n;)
		if (s_link[k].tgt == b->ins[0].a) {
			uint32_t *const site = s_link[k].site;
			*site = 0xEA000000u | ((uint32_t)((uint32_t *)s_cur_body - (site + 2)) & 0x00ffffffu);
			jit_sync(site, 4);
			s_link[k] = s_link[--s_link_n];
		} else
			k++;
	h8_jit_blocks++;
	h8_jit_bytes += (uint32_t)((s_out - start) * 4);
	((H8Blk *)b)->code_bytes = (uint32_t)((s_out - start) * 4);
	return start;
}
