// ReSys12: Namco System 12 board (main CPU side): ROM banking, ROM DMA (channel 5), shared RAM, EEPROM, KC044 protection.
// Behaviour follows MAME src/mame/namco/namcos12.cpp (BSD-3-Clause, copyright smf and contributors).
#include "s12.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "psxcommon.h"
#include "psxhw.h"
#include "psxmem.h"
#include "r3000a.h"
#include "s12_bus.h"

S12Board s12;
static uint16_t s_link_ram[0x8000], s_link_regs[16]; // kartduel link board (kartduel_ram)

#define BANK_SIZE 0x200000u

void s12_attach(S12Rom *rom)
{
	memset(&s12, 0, sizeof(s12));
	memset(s_link_ram, 0, sizeof(s_link_ram));
	memset(s_link_regs, 0, sizeof(s_link_regs));
	s12.rom = rom;
	s12.game = rom->game;
	const int st = rom->game->state;
	s12.alt_bank = (st == S12_S_ALTBANK || st == S12_S_CDXA || st == S12_S_GOLGO13 || st == S12_S_TRUCKK);
	s12.tektagt = (st == S12_S_TEKTAGT);
	const int m = rom->game->machine;
	s12.boot_hack = (m == S12_M_COH700B || m == S12_M_COH716 || m == S12_M_CDXA || m == S12_M_QUESTFF ||
		m == S12_M_GOLGO13 || m == S12_M_TRUCKK || m == S12_M_KARTDUEL);
	s12.bank_count = rom->rgn_size[S12_RGN_BANK] / BANK_SIZE;
	if (!s12.bank_count)
		s12.bank_count = 1;
	// MAME ptblank2_state::machine_start: "patch out wait for dma 5 to complete" (all its sets)
	if (st == S12_S_PTBLANK2 && rom->rgn[S12_RGN_MAIN] && rom->rgn_size[S12_RGN_MAIN] >= 0x331c8)
		memset(rom->rgn[S12_RGN_MAIN] + 0x331c4, 0, 4);
	// MAME truckk_state::machine_start: "Change order of code so that the status flags are set before DMA 5 is
	// started" (5 instructions at 0x376ec go before the 2 DMA start instructions at 0x376e4)
	if (st == S12_S_TRUCKK && !rom->patched && rom->rgn[S12_RGN_MAIN] && rom->rgn_size[S12_RGN_MAIN] >= 0x37700) {
		uint8_t *p = rom->rgn[S12_RGN_MAIN], temp[5 * 4];
		memcpy(temp, p + 0x376ec, sizeof(temp));
		memmove(p + 0x376e4 + sizeof(temp), p + 0x376e4, 2 * 4);
		memcpy(p + 0x376e4, temp, sizeof(temp));
	}
	rom->patched = 1;
	// analog controls at rest (guns centred)
	s12_set_gun(0, 0.5f, 0.5f);
	s12_set_gun(1, 0.5f, 0.5f);
	if (s12_analog_kind(s12.game) == S12_ANALOG_WHEEL)
		s12_set_wheel(0.0f, 0.0f, 0.0f);
	if (s12_analog_kind(s12.game) == S12_ANALOG_LEVER)
		s12_set_lever(0.0f);
	if (rom->rgn[S12_RGN_EEPROM])
		memcpy(s12.eeprom, rom->rgn[S12_RGN_EEPROM], sizeof(s12.eeprom));
	else
		memset(s12.eeprom, 0xff, sizeof(s12.eeprom));
}

// ---- ROM bank window 0x1fa00000-0x1fbfffff ----
// Mapped through the core read LUT (512KB granularity) for speed. tektagt has protection registers at 0x1fb00000 and
// 0x1fb80000, so that half of the window stays on the handler path.
static void map_bank(void)
{
	uint8_t *base = s12.rom->rgn[S12_RGN_BANK];
	if (!base)
		return;
	uint8_t *p = base + (s12.bank_entry % s12.bank_count) * BANK_SIZE;
	if (s12.tektagt) {
		psxMemMapRead(0x1fa00000u, 0x100000u, p);
		psxMemMapRead(0x1fb00000u, 0x100000u, NULL);
	} else
		psxMemMapRead(0x1fa00000u, BANK_SIZE, p);
}

static uint16_t bank_read16(uint32_t phys)
{
	const uint8_t *base = s12.rom->rgn[S12_RGN_BANK];
	if (!base)
		return 0;
	const uint32_t o = (s12.bank_entry % s12.bank_count) * BANK_SIZE + ((phys - 0x1fa00000u) & (BANK_SIZE - 2));
	return (uint16_t)(base[o] | (base[o + 1] << 8));
}

static void bankoffset_w(uint16_t data)
{
	if (s12.alt_bank) {
		if (data & 8)
			s12.bankoffset = (uint16_t)((data - 8) << 2);
		else
			s12.bankoffset = (uint16_t)((s12.bankoffset & ~7) | (data & 7));
		s12.bank_entry = s12.bankoffset;
	} else
		s12.bank_entry = data;
	map_bank();
}

void s12_mem_reset(void)
{
	// program flash -> boot ROM area
	const S12Rom *r = s12.rom;
	if (r && r->rgn[S12_RGN_MAIN]) {
		uint32_t n = r->rgn_size[S12_RGN_MAIN];
		if (n > PSX_ROM_SIZE)
			n = PSX_ROM_SIZE;
		memcpy(psxRegs.ptrs.psxR, r->rgn[S12_RGN_MAIN], n);
	}
	s12.dmaoffset = 0;
	s12.bankoffset = 0;
	s12.bank_entry = 0;
	s12.tektagdmaoffset = 0;
	s12.has_tektagt_dma = 0;
	s12.ttt_cnt = 0;
	s12.ttt_val[0] = s12.ttt_val[1] = 0;
	map_bank();
	// tektagt reads a protection register at 0x1ff00000 (mirror of 0x1f700000) inside the flash area:
	// keep that 512KB on the handler path
	if (s12.tektagt)
		psxMemMapRead(0x1ff00000u, 0x80000u, NULL);
}

// ---- save state (board registers, shared RAM, EEPROM, protection); ROM pointers stay, bank mapping is rebuilt ----
uint32_t s12_board_state_size(void)
{
	return (uint32_t)sizeof(s12);
}

void s12_board_state_save(void *dst)
{
	memcpy(dst, &s12, sizeof(s12));
}

void s12_board_state_load(const void *src)
{
	S12Rom *rom = s12.rom;
	const S12Game *game = s12.game;
	const S12Input in = s12.in; // inputs belong to the frontend
	memcpy(&s12, src, sizeof(s12));
	s12.rom = rom;
	s12.game = game;
	s12.in = in;
	map_bank();
	if (s12.tektagt)
		psxMemMapRead(0x1ff00000u, 0x80000u, NULL);
}

void s12_cpu_reset(void)
{
	// MAME coh700b: "12 bytes in ram are checked early in the boot sequences, how it gets into memory is not currently known"
	if (s12.boot_hack && s12.rom && s12.rom->rgn[S12_RGN_MAIN])
		memcpy(psxRegs.ptrs.psxM + 0x10000, s12.rom->rgn[S12_RGN_MAIN] + 0x20280, 12);
	s12_sub_reset();
}

// ---- tektagt protection (KC044) ----
static uint16_t tektagt_prot2_r(int offset)
{
	if (s12.ttt_cnt == 2) {
		const uint32_t a = s12.ttt_val[0], b = s12.ttt_val[1];
		if (a == 0x806d2c24 && b == 0xd5545715)
			return offset == 0 ? 0x36e2 : 0;
		if (a == 0x804c2c84 && b == 0xd5545615)
			return offset == 0 ? 0x2651 : 0;
		if (a == 0x2aaba8e6 && b == 0x00820040)
			return offset == 1 ? 0x4186 : 0;
		if (a == 0x2aaba592 && b == 0x01780544)
			return offset == 1 ? 0x3c7d : 0;
		if (((b >> 16) & 0xff) == 0xa9)
			return 0x552e;
	}
	return 0;
}

static void tektagt_prot1_w(int offset, uint16_t data)
{
	if (offset == 0) {
		s12.tektagdmaoffset = data;
		if (s12.ttt_cnt != 2)
			s12.ttt_val[s12.ttt_cnt] = data;
	} else {
		s12.tektagdmaoffset |= (uint32_t)data << 16;
		if (s12.ttt_cnt != 2)
			s12.ttt_val[s12.ttt_cnt++] |= (uint32_t)data << 16;
	}
}

// ---- diagnostics: distinct unhandled addresses (harness prints them) ----
#define UNMAPPED_MAX 64
static struct { uint32_t addr, count; int write; } s_unmapped[UNMAPPED_MAX];

static void note_unmapped(uint32_t a, int write)
{
	s12.log_unmapped++;
	for (int i = 0; i < UNMAPPED_MAX; i++) {
		if (s_unmapped[i].count && s_unmapped[i].addr == a && s_unmapped[i].write == write) {
			s_unmapped[i].count++;
			return;
		}
		if (!s_unmapped[i].count) {
			s_unmapped[i].addr = a;
			s_unmapped[i].write = write;
			s_unmapped[i].count = 1;
			return;
		}
	}
}

void s12_dump_unmapped(void)
{
	for (int i = 0; i < UNMAPPED_MAX && s_unmapped[i].count; i++)
		fprintf(stderr, "  unmapped %s %08x x%u\n", s_unmapped[i].write ? "w" : "r", s_unmapped[i].addr, s_unmapped[i].count);
}

// ---- kartduel link board (MAME kartduel_state::maincpu_map): link CPU shared RAM and a few RAM registers ----
static uint16_t *kartduel_ram(uint32_t a)
{
	if (a >= 0x1f780000u && a < 0x1f790000u)
		return &s_link_ram[(a - 0x1f780000u) >> 1];
	if (a >= 0x1f796004u && a < 0x1f79600cu)
		return &s_link_regs[(a - 0x1f796004u) >> 1];      // 0..3
	if (a >= 0x1f796014u && a < 0x1f796018u)
		return &s_link_regs[4 + ((a - 0x1f796014u) >> 1)]; // 4..5
	if (a >= 0x1f796020u && a < 0x1f796024u)
		return &s_link_regs[6 + ((a - 0x1f796020u) >> 1)]; // 6..7
	if (a >= 0x1f797000u && a < 0x1f797004u)
		return &s_link_regs[8 + ((a - 0x1f797000u) >> 1)]; // 8..9
	return NULL;
}

// ---- 16-bit bus (everything on the System 12 expansion bus is 16 bits wide in MAME) ----
static uint16_t board_r16(uint32_t a)
{
	if (a >= 0x1f080000u && a < 0x1f084000u)
		return s12.shared[(a - 0x1f080000u) >> 1];
	if (a >= 0x1f140000u && a < 0x1f141000u) {
		// AT28C16 on byte lanes 0 and 2 (umask32 0x00ff00ff)
		const uint32_t o = (a - 0x1f140000u) >> 1;
		return s12.eeprom[o & 0x7ff];
	}
	if (s12.tektagt) {
		if ((a & ~0x800000u) == 0x1f700000u || (a & ~0x800000u) == 0x1f700002u) {
			s12.has_tektagt_dma = 1;
			return 0;
		}
		if ((a & ~3u) == 0x1fb00000u)
			return (a & 2) ? 0 : 0x8000;
		if ((a & ~3u) == 0x1fb80000u)
			return tektagt_prot2_r((a >> 1) & 1);
	}
	if (s12.game && s12.game->state == S12_S_PTBLANK2 && (a & ~0x800000u) >= 0x1f780000u &&
		(a & ~0x800000u) < 0x1f780010u) {
		// gun board (MAME ptblank2_state::system11gun_r): word offset >> 1 = P1 X, P1 Y, P2 X, P2 Y
		uint16_t v = s12.in.gun[(((a & ~0x800000u) - 0x1f780000u) >> 2) & 3];
		if ((v & 3) == 3)
			v++; // yend
		return v;
	}
	if (s12.game && s12.game->state == S12_S_KARTDUEL) {
		uint16_t *r = kartduel_ram(a);
		if (r)
			return *r;
	}
	if (a >= 0x1fa00000u && a < 0x1fc00000u)
		return bank_read16(a);
	if (a >= 0x1fc00000u) {
		const uint8_t *r = psxRegs.ptrs.psxR + ((a - 0x1fc00000u) & PSX_ROM_MASK & ~1u);
		return (uint16_t)(r[0] | (r[1] << 8));
	}
	note_unmapped(a, 0);
	return 0;
}

static void board_w16(uint32_t a, uint16_t d)
{
	if (a == 0x1f000000u) {
		bankoffset_w(d);
		return;
	}
	if (a >= 0x1f080000u && a < 0x1f084000u) {
		s12.shared[(a - 0x1f080000u) >> 1] = d;
		return;
	}
	if (a >= 0x1f140000u && a < 0x1f141000u) {
		const uint32_t o = ((a - 0x1f140000u) >> 1) & 0x7ff;
		if (s12.eeprom[o] != (uint8_t)d) {
			s12.eeprom[o] = (uint8_t)d;
			s12.eeprom_dirty = 1;
		}
		return;
	}
	if (s12.tektagt) {
		if ((a & ~3u) == 0x1fb00000u) {
			tektagt_prot1_w((a >> 1) & 1, d);
			return;
		}
		if ((a & ~3u) == 0x1fb80000u) {
			if (!(a & 2))
				s12.ttt_cnt = 0; // start of a challenge
			return;
		}
	}
	if (a >= 0x1f700000u && a < 0x1f710000u) {
		s12.dmaoffset = (a - 0x1f700000u) | ((uint32_t)d << 16);
		return;
	}
	if (s12.game && s12.game->state == S12_S_PTBLANK2 && (a & ~0x800000u) >= 0x1f788000u &&
		(a & ~0x800000u) < 0x1f788004u)
		return; // gun board outputs (recoil, start lamps) and "start reading"
	if (s12.game && s12.game->state == S12_S_KARTDUEL) {
		uint16_t *r = kartduel_ram(a);
		if (r) {
			*r = d;
			return;
		}
		if ((a & ~3u) == 0x1f796000u)
			return; // link CPU reset line (the link CPU is not emulated, as MAME without a LAN peer)
	}
	// MAME nopw: 0x1f008000, 0x1f010000, 0x1f018000, 0x1f1bff08-0f, and the program flash
	if ((a & ~3u) == 0x1f008000u || (a & ~3u) == 0x1f010000u || (a & ~3u) == 0x1f018000u ||
		(a >= 0x1f1bff08u && a < 0x1f1bff10u) || a >= 0x1fc00000u)
		return;
	note_unmapped(a, 1);
}

uint16_t s12_read16(uint32_t phys)
{
	return board_r16(phys & ~1u);
}

uint8_t s12_read8(uint32_t phys)
{
	const uint16_t v = board_r16(phys & ~1u);
	return (uint8_t)((phys & 1) ? v >> 8 : v);
}

uint32_t s12_read32(uint32_t phys)
{
	phys &= ~3u;
	return board_r16(phys) | ((uint32_t)board_r16(phys + 2) << 16);
}

void s12_write16(uint32_t phys, uint32_t v)
{
	board_w16(phys & ~1u, (uint16_t)v);
}

void s12_write8(uint32_t phys, uint32_t v)
{
	// byte writes reach 16-bit devices as a word with the byte in its lane
	const uint32_t a = phys & ~1u;
	if (a >= 0x1f080000u && a < 0x1f084000u) {
		uint16_t *w = &s12.shared[(a - 0x1f080000u) >> 1];
		*w = (phys & 1) ? (uint16_t)((*w & 0x00ff) | ((v & 0xff) << 8)) : (uint16_t)((*w & 0xff00) | (v & 0xff));
		return;
	}
	board_w16(a, (uint16_t)((phys & 1) ? (v & 0xff) << 8 : (v & 0xff)));
}

void s12_write32(uint32_t phys, uint32_t v)
{
	phys &= ~3u;
	board_w16(phys, (uint16_t)v);
	board_w16(phys + 2, (uint16_t)(v >> 16));
}

// ---- ROM DMA (MAME namcos12_rom_read on PSX DMA channel 5) ----
void s12_dma5_done(void)
{
	HW_DMA5_CHCR &= SWAP32(~0x01000000u);
	DMA_INTERRUPT(5);
}

void psxDma5(uint32_t madr, uint32_t bcr, uint32_t chcr)
{
	int32_t size;
	if (chcr & 0x200) {
		uint32_t ba = bcr >> 16;
		if (!ba)
			ba = 0x10000;
		size = (int32_t)((bcr & 0xffff) * ba);
	} else if ((chcr & 0xffbffeffu) == 0x11000000u) {
		int32_t n = (int32_t)(bcr >> 16);
		size = (n > 1 ? n : 1) * (int32_t)(bcr & 0xffff);
	} else // ReSys12: manual mode word count, 0 = 0x10000 as on the PS1 (MAME takes 0: toukon3's title picture was never
		// copied in, the "garbage graphics" of the MAME notes)
		size = (bcr & 0xffff) ? (int32_t)(bcr & 0xffff) : 0x10000;

	uint32_t offset = s12.has_tektagt_dma ? s12.tektagdmaoffset : s12.dmaoffset;
	const uint32_t exp_base = psxHu32(0x1000) & 0x00ffffffu; // expansion 1 base (0x1f801000)
	const int is_main = (offset & 0x80000000u) || exp_base == 0x300000u;
	const uint8_t *src;
	uint32_t src_bytes;
	if (is_main) {
		src = s12.rom->rgn[S12_RGN_MAIN];
		src_bytes = s12.rom->rgn_size[S12_RGN_MAIN];
		offset &= 0xffffff;
	} else {
		src = s12.rom->rgn[S12_RGN_BANK];
		src_bytes = s12.rom->rgn_size[S12_RGN_BANK];
		offset &= 0x7fffffff;
		if (!s12.has_tektagt_dma && s12.alt_bank)
			offset += (uint32_t)s12.bankoffset * BANK_SIZE;
	}
	s12.has_tektagt_dma = 0;

	uint32_t addr = (madr & PSX_RAM_MASK) >> 2;
#ifdef RS12_HARNESS
	if (getenv("RS12_DMA5LOG")) // diagnostics: every ROM DMA
		fprintf(stderr, "dma5 madr %08x bcr %08x chcr %08x size %d off %08x raw %08x main %d exp %06x bank %u alt %d tag %d\n", madr, bcr, chcr, size, offset,
			s12.dmaoffset, is_main, exp_base, (unsigned)s12.bankoffset, s12.alt_bank, s12.has_tektagt_dma);
#endif
	// MAME: word offset into the 16-bit ROM, copies 32 bits per step
	const int32_t romleft = src_bytes > offset ? (int32_t)((src_bytes - offset) / 4) : 0;
	if (size > romleft)
		size = romleft;
	const int32_t ramleft = (int32_t)(PSX_RAM_SIZE / 4 - addr);
	if (size > ramleft)
		size = ramleft;
#ifdef RS12_HARNESS
	{ // ReSys12 (diagnostics): DMA writes into the RS12_WATCH range
		extern uint32_t rs12_watch_lo, rs12_watch_hi, rs12_watch_from;
		extern unsigned int frame_counter;
		const uint32_t a0 = madr & 0x1fffff, a1 = a0 + size * 4;
		if (frame_counter >= rs12_watch_from && a1 > rs12_watch_lo && a0 <= rs12_watch_hi)
			fprintf(stderr, "watch " "dma5 rom" " %08x words %u f%u\n", madr, (unsigned)(size), frame_counter);
	}
#endif
	if (size > 0 && src) {
		uint32_t *ram = (uint32_t *)psxRegs.ptrs.psxM;
		const uint8_t *s = src + (offset & ~1u);
		for (int32_t i = 0; i < size; i++, s += 4)
			ram[addr + i] = (uint32_t)s[0] | ((uint32_t)s[1] << 8) | ((uint32_t)s[2] << 16) | ((uint32_t)s[3] << 24);
		psxCpu->Clear(addr << 2, (uint32_t)size);
	}
	(void)chcr;
	s12_dma5_done();
}

// ---- analog controls (MAME input port ranges; a light gun's crosshair maps its range linearly onto the screen) ----
int s12_analog_kind(const S12Game *g)
{
	if (!g)
		return S12_ANALOG_NONE;
	if (g->state == S12_S_PTBLANK2 || g->state == S12_S_GOLGO13)
		return S12_ANALOG_GUN;
	if (g->machine == S12_M_KARTDUEL || g->machine == S12_M_TRUCKK || g->machine == S12_M_TECHNODR)
		return S12_ANALOG_WHEEL;
	if (g->machine == S12_M_APLARAIL)
		return S12_ANALOG_LEVER;
	return S12_ANALOG_NONE;
}

static uint16_t lerp_u16(float f, int lo, int hi)
{
	if (f < 0.0f)
		f = 0.0f;
	if (f > 1.0f)
		f = 1.0f;
	return (uint16_t)(lo + (int)(f * (float)(hi - lo) + 0.5f));
}

void s12_set_gun(int player, float fx, float fy)
{
	const S12Game *g = s12.game;
	if (!g || player < 0 || player > 1)
		return;
	if (g->state == S12_S_GOLGO13) { // GUN0/1 0..0x1ff (Y reversed) -> ADC 1/2 (golgo13_state gunx_r / guny_r)
		if (player)
			return;
		const int gx = lerp_u16(fx, 0, 0x1ff), gy = lerp_u16(1.0f - fy, 0, 0x1ff);
		s12.in.adc[1] = (uint16_t)((gx * 0x240) / 0x1ff + 0x7a);
		s12.in.adc[2] = (uint16_t)((gy * 0x1bf) / 0x1ff + 0x1f);
	} else if (!strcmp(g->inputs, "ghlpanic")) {
		s12.in.gun[player * 2] = lerp_u16(fx, 0xbe, 0x35f);
		s12.in.gun[player * 2 + 1] = lerp_u16(fy, 0x1a, 0x109);
	} else {
		s12.in.gun[player * 2] = lerp_u16(fx, 0xdc, 0x387);
		s12.in.gun[player * 2 + 1] = lerp_u16(fy, 0x28, 0x11b);
	}
}

void s12_set_wheel(float steer, float gas, float brake)
{
	const S12Game *g = s12.game;
	if (!g)
		return;
	if (g->machine == S12_M_KARTDUEL) { // ADC 0 brake, 1 gas (0..0x15a), 2 wheel (0xe0..0x320, centre 0x200)
		s12.in.adc[0] = lerp_u16(brake, 0, 0x15a);
		s12.in.adc[1] = lerp_u16(gas, 0, 0x15a);
		s12.in.adc[2] = steer < 0.0f ? lerp_u16(1.0f + steer, 0xe0, 0x200) : lerp_u16(steer, 0x200, 0x320);
	} else if (g->machine == S12_M_TECHNODR) { // JVS analog 0 wheel 0x1600..0xa600, 1 brake, 2 gas (all reversed)
		s12.in.jvs_analog[0] = lerp_u16((1.0f - steer) * 0.5f, 0x1600, 0xa600);
		s12.in.jvs_analog[1] = lerp_u16(1.0f - brake, 0x3e00, 0x7e00);
		s12.in.jvs_analog[2] = lerp_u16(1.0f - gas, 0x4680, 0xed80);
	} else { // truckk: ADC 0 wheel, 1 brake, 2 gas (0..0x3ff, pedals reversed)
		s12.in.adc[0] = lerp_u16((steer + 1.0f) * 0.5f, 0, 0x3ff);
		s12.in.adc[1] = lerp_u16(1.0f - brake, 0, 0x3ff);
		s12.in.adc[2] = lerp_u16(1.0f - gas, 0, 0x3ff);
	}
}

void s12_set_lever(float v)
{
	// aplarail speed lever: ADC 0, 0x8b..0x397 (rest 0x216), reversed
	const float f = (1.0f - v) * 0.5f;
	s12.in.adc[0] = f < 0.5f ? lerp_u16(f * 2.0f, 0x8b, 0x216) : lerp_u16((f - 0.5f) * 2.0f, 0x216, 0x397);
}

// ---- inputs as the H8 sees them (MAME in_r, JAMMA mode) ----
uint8_t s12_in_r(int offset)
{
	const uint32_t p1 = s12.in.player[0], p2 = s12.in.player[1];
	uint8_t v = 0;
	switch (offset) {
	case 0:
	case 1: {
		const uint32_t p = offset == 0 ? p2 : p1;
		v = (uint8_t)(((p & S12_IN_START) ? 0x80 : 0) | ((p & S12_IN_B3) ? 0x40 : 0) | ((p & S12_IN_B2) ? 0x20 : 0) |
			((p & S12_IN_B1) ? 0x10 : 0) | ((p & S12_IN_UP) ? 0x08 : 0) | ((p & S12_IN_DOWN) ? 0x04 : 0) |
			((p & S12_IN_LEFT) ? 0x02 : 0) | ((p & S12_IN_RIGHT) ? 0x01 : 0));
		break;
	}
	case 2:
		v = (uint8_t)(((p1 & S12_IN_SERVICE) ? 0x80 : 0) | ((s12.in.system & S12_SYS_TEST) ? 0x40 : 0) |
			((s12.in.system & S12_SYS_COIN1) ? 0x20 : 0) | ((s12.in.system & S12_SYS_COIN2) ? 0x10 : 0) |
			((p1 & S12_IN_B7) ? 0x08 : 0) | ((p2 & S12_IN_B7) ? 0x04 : 0) | ((p1 & S12_IN_B8) ? 0x02 : 0) |
			((p2 & S12_IN_B8) ? 0x01 : 0));
		break;
	case 3:
		v = (uint8_t)(((p2 & S12_IN_B6) ? 0x80 : 0) | ((p2 & S12_IN_B5) ? 0x40 : 0) | ((p2 & S12_IN_B4) ? 0x20 : 0) |
			((p1 & S12_IN_B6) ? 0x08 : 0) | ((p1 & S12_IN_B5) ? 0x04 : 0) | ((p1 & S12_IN_B4) ? 0x02 : 0));
		break;
	}
	return (uint8_t)~v;
}
