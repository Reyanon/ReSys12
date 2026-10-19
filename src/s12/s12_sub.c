// ReSys12: System 12 sub CPU side: H8/3002 memory map, ports, VBLANK IRQ, RTC4543 and namco_settings on SCI1.
// Behaviour follows MAME src/mame/namco/namcos12.cpp (sub_program_map, sub_port*_r/w, namcos12_sub_irq, in_r),
// src/devices/machine/rtc4543.cpp (R. Belmont) and src/mame/namco/namco_settings.cpp (Olivier Galibert); BSD-3-Clause.
//
// Implemented: ROM/shared RAM (direct pointers in the H8 core), C352 (src/sound/c352.h), JAMMA inputs (s12_in_r),
// wait-state no-op registers, port 6/7/8/A/B, ADC inputs (all 0, as MAME's unused ADC ports), VBLANK -> IRQ1 + ADTRG +
// port B bit 7, RTC4543 (read only, as wired: WR is not connected) and the settings shift register (accepts writes).
// JVS: the I/O boards of aplarail / technodr / questff on SCI0 (s12_jvs.c); the other sets have none (sense = None).
// Timing: the H8 runs at 16934400 Hz = main CPU clock / 2. It is caught up to the main CPU time on every PSXINT_S12
// event (S12_SUB_QUANTUM main cycles) and on every VBLANK edge.
#include "s12.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "h8.h"
#include "psxcommon.h"
#include "r3000a.h"
#include "psxevents.h"
#include "s12_bus.h"
#include "s12_jvs.h"
#include "s12_sound.h"

// main CPU cycles between two sub CPU slices (~1 scanline)
#define S12_SUB_QUANTUM 2150
#define S12_SUB_CLOCK 16934400

static struct {
	uint32_t last_cycle; // psxRegs.cycle at the last catch-up
	uint32_t half;       // odd main CPU cycle carried to the next catch-up
	uint8_t port8, porta, portb;
	// RTC4543
	struct {
		int ce, clk, wr, data, curbit;
		uint8_t regs[7];
	} rtc;
	// namco_settings
	struct {
		int ce, clk, data, cur_bit;
		uint8_t adr, value;
	} set;
	int64_t rtc_base; // unix time at H8 reset
	int rtc_local;    // base from the host clock: report local time
	S12Jvs jvs;
} sub;

// ---- diagnostics ----
#define SUB_UNMAPPED_MAX 32
static struct { uint32_t addr, count; int write; } s_sub_unmapped[SUB_UNMAPPED_MAX];
static uint32_t s_port_logged;

static void sub_unmapped(uint32_t a, int write)
{
	for (int i = 0; i < SUB_UNMAPPED_MAX; i++) {
		if (s_sub_unmapped[i].count && s_sub_unmapped[i].addr == a && s_sub_unmapped[i].write == write) {
			s_sub_unmapped[i].count++;
			return;
		}
		if (!s_sub_unmapped[i].count) {
			s_sub_unmapped[i].addr = a;
			s_sub_unmapped[i].write = write;
			s_sub_unmapped[i].count = 1;
			fprintf(stderr, "s12_sub: unmapped H8 %s %06x (pc %06x)\n", write ? "write" : "read", a, h8.PPC);
			return;
		}
	}
}

static uint32_t s_c352_reads[0x4000]; // diagnostics: firmware reads of C352 registers (word offset)

void s12_sub_dump(void)
{
	for (int i = 0; i < SUB_UNMAPPED_MAX && s_sub_unmapped[i].count; i++)
		fprintf(stderr, "  h8 unmapped %s %06x x%u\n", s_sub_unmapped[i].write ? "w" : "r", s_sub_unmapped[i].addr,
			s_sub_unmapped[i].count);
	h8_dump_unimplemented();
	for (int i = 0; i < 0x4000; i++)
		if (s_c352_reads[i])
			fprintf(stderr, "  c352 read reg %03x x%u\n", i, s_c352_reads[i]);
}

// ---- RTC4543 ----
static uint8_t to_bcd(int v) { return (uint8_t)(((v / 10) % 10) << 4 | (v % 10)); }

static void rtc_latch_time(void)
{
	const time_t t = (time_t)(sub.rtc_base + (int64_t)(h8_total_cycles() / S12_SUB_CLOCK));
	struct tm tmv;
	const struct tm *p = sub.rtc_local ? localtime(&t) : gmtime(&t);
	if (!p)
		return;
	tmv = *p;
	static const int weekday[7] = { 7, 1, 2, 3, 4, 5, 6 }; // tm_wday 0 = Sunday
	sub.rtc.regs[0] = to_bcd(tmv.tm_sec);
	sub.rtc.regs[1] = to_bcd(tmv.tm_min);
	sub.rtc.regs[2] = to_bcd(tmv.tm_hour);
	sub.rtc.regs[3] = to_bcd(weekday[tmv.tm_wday]);
	sub.rtc.regs[4] = to_bcd(tmv.tm_mday);
	sub.rtc.regs[5] = to_bcd(tmv.tm_mon + 1);
	sub.rtc.regs[6] = to_bcd(tmv.tm_year % 100);
}

static void rtc_ce_w(int state)
{
	if (state && !sub.rtc.ce) {
		sub.rtc.curbit = 0;
		rtc_latch_time(); // MAME keeps the registers ticking every second; latching at CE is equivalent for reads
	}
	sub.rtc.ce = state;
}

static void rtc_clk_w(int state)
{
	if (sub.rtc.ce && !sub.rtc.clk && state && sub.rtc.curbit != 56) {
		const int reg = sub.rtc.curbit / 8;
		const int bit = sub.rtc.curbit & 7;
		if (!sub.rtc.wr) {
			sub.rtc.data = (sub.rtc.regs[reg] >> bit) & 1;
			h8_sci_rx_w(1, sub.rtc.data);
		}
		// WR is not wired on System 12: writes never happen
		sub.rtc.curbit++;
		if (sub.rtc.curbit == 28)
			sub.rtc.curbit += 4; // day-of-week register is 4 bits wide
	}
	sub.rtc.clk = state;
}

// ---- namco_settings (serial register written through SCI1, no readback) ----
static void settings_ce_w(int state)
{
	if (sub.set.ce != state) {
		sub.set.ce = state;
		if (state)
			sub.set.cur_bit = 0;
	}
}

static void settings_clk_w(int state)
{
	if (sub.set.clk != state) {
		sub.set.clk = state;
		if (state) {
			if (sub.set.cur_bit < 8)
				sub.set.adr = (uint8_t)((sub.set.adr >> 1) | (sub.set.data << 7));
			else
				sub.set.value = (uint8_t)((sub.set.value << 1) | sub.set.data);
			sub.set.cur_bit++;
			if (sub.set.cur_bit == 16)
				sub.set.cur_bit = 0;
		}
	}
}

static void update_ce(void)
{
	const int sel = (sub.portb >> 5) & 1;
	rtc_ce_w(sel && (sub.porta & 1));
	settings_ce_w(sel && !(sub.porta & 1));
}

// ---- H8 board callbacks ----
void h8_sci_tx(int sci, int state)
{
	if (sci == 1)
		sub.set.data = state;
	// SCI0: JVS port, bytes (h8_sci_tx_byte)
}

void h8_sci_tx_byte(int sci, uint8_t data)
{
	if (sci == 0)
		s12_jvs_host_byte(&sub.jvs, data, h8_total_cycles());
}

void h8_sci_clk(int sci, int state)
{
	if (sci == 1) {
		rtc_clk_w(!state);
		settings_clk_w(state);
	}
}

uint16_t h8_adc_in(int ch)
{
	return ch >= 0 && ch < 4 ? s12.in.adc[ch] : 0; // wheels, pedals, levers, golgo13's gun (s12_set_*)
}

uint8_t h8_port_in(int port)
{
	switch (port) {
	case H8_PORT_6:
		return s12_jvs_sense(&sub.jvs) == 2 ? 0xfd : 0xff; // bit 1: JVS sense != Initialized
	case H8_PORT_7:
		return (s12.in.system & S12_SYS_DSW_SERVICE) ? 0x7f : 0xff; // DSW: bit 7 service mode (active low)
	case H8_PORT_8:
		return s12_jvs_sense(&sub.jvs) ? 0xff : 0xef; // bit 4: JVS sense != None
	case H8_PORT_A:
		return 0xff;
	case H8_PORT_B:
		return sub.portb | 0x7f;
	default:
		if (!(s_port_logged & (1u << port))) {
			s_port_logged |= 1u << port;
			fprintf(stderr, "s12_sub: read of un-hooked H8 port index %d\n", port);
		}
		return 0xff;
	}
}

void h8_port_out(int port, uint8_t data, uint8_t ddr)
{
	(void)ddr;
	switch (port) {
	case H8_PORT_8:
		sub.port8 = data;
		break;
	case H8_PORT_A:
		sub.porta = data;
		update_ce();
		break;
	case H8_PORT_B:
		sub.portb = (uint8_t)((sub.portb & 0x80) | (data & 0x7f));
		update_ce();
		// bit 0: JVS RTS (line direction; the JVS model answers once a packet is complete)
		break;
	default:
		break; // ports 4, 6, 7, 9: not hooked in MAME
	}
}

uint8_t h8_ext_read8(uint32_t a)
{
	if (a >= 0x280000 && a < 0x288000) {
		s_c352_reads[((a - 0x280000) >> 1) & 0x3fff]++;
		const uint16_t v = s12_sound_reg_read((a - 0x280000) >> 1, h8_total_cycles());
		return (uint8_t)((a & 1) ? v : v >> 8);
	}
	if (a >= 0x300000 && a < 0x300004)
		return s12_in_r((int)(a - 0x300000));
	if ((a & ~1u) == 0x300010 || (a & ~1u) == 0x300030)
		return 0;
	sub_unmapped(a, 0);
	return 0;
}

uint16_t h8_ext_read16(uint32_t a)
{
	if (a >= 0x280000 && a < 0x288000) {
		s_c352_reads[((a - 0x280000) >> 1) & 0x3fff]++;
		return s12_sound_reg_read((a - 0x280000) >> 1, h8_total_cycles());
	}
	if (a >= 0x300000 && a < 0x300004)
		return (uint16_t)(s12_in_r((int)(a - 0x300000)) << 8 | s12_in_r((int)(a - 0x300000 + 1)));
	if (a == 0x300010 || a == 0x300030)
		return 0;
	sub_unmapped(a, 0);
	return 0;
}

void h8_ext_write8(uint32_t a, uint8_t d)
{
	if (a >= 0x280000 && a < 0x288000) {
		const uint32_t off = (a - 0x280000) >> 1;
		const uint16_t old = s12_sound_reg_read(off, h8_total_cycles());
		s12_sound_reg_write(off, (a & 1) ? (uint16_t)((old & 0xff00) | d) : (uint16_t)((old & 0x00ff) | (d << 8)),
			h8_total_cycles());
		return;
	}
	if ((a & ~1u) == 0x300010 || (a & ~1u) == 0x300030)
		return;
	sub_unmapped(a, 1);
}

void h8_ext_write16(uint32_t a, uint16_t d)
{
	if (a >= 0x280000 && a < 0x288000) {
		s12_sound_reg_write((a - 0x280000) >> 1, d, h8_total_cycles());
		return;
	}
	if (a == 0x300010 || a == 0x300030)
		return;
	sub_unmapped(a, 1);
}

// ---- scheduling ----
#ifdef RS12_SUB_PROFILE
static uint64_t s_prof_ns;
uint64_t s12_sub_profile_ns(void) { return s_prof_ns; }
#ifdef __vita__
extern uint64_t sceKernelGetProcessTimeWide(void);
static uint64_t now_ns(void) { return sceKernelGetProcessTimeWide() * 1000; } // cheaper than clock_gettime here
#else
static uint64_t now_ns(void)
{
	struct timespec ts;
	clock_gettime(CLOCK_MONOTONIC, &ts);
	return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
}
#endif
#else
uint64_t s12_sub_profile_ns(void) { return 0; }
#endif

static void catch_up(void)
{
	const uint32_t now = psxRegs.cycle;
	const uint32_t t = (now - sub.last_cycle) + sub.half;
	sub.last_cycle = now;
	sub.half = t & 1;
	if (t >> 1) {
#ifdef RS12_SUB_PROFILE
		const uint64_t t0 = now_ns();
		h8_run((int)(t >> 1));
		s_prof_ns += now_ns() - t0;
#else
		h8_run((int)(t >> 1));
#endif
		if (sub.jvs.tx_pos < sub.jvs.tx_len)
			s12_jvs_update(&sub.jvs, h8_total_cycles());
	}
}

void s12_sub_reset(void)
{
	memset(&sub, 0, sizeof(sub));
	// MAME machine_start
	sub.port8 = 0;
	sub.porta = 0;
	sub.portb = 0x50;
	if (s12.rtc_base) {
		sub.rtc_base = s12.rtc_base;
		sub.rtc_local = 0;
	} else {
		sub.rtc_base = (int64_t)time(NULL);
		sub.rtc_local = 1;
	}

	const S12Rom *r = s12.rom;
	if (r && r->rgn[S12_RGN_SUB])
		h8_set_rom(r->rgn[S12_RGN_SUB], r->rgn_size[S12_RGN_SUB] < 0x80000 ? r->rgn_size[S12_RGN_SUB] : 0x80000);
	else
		h8_set_rom(NULL, 0);
	h8_set_ram(s12.shared, 0x080000, sizeof(s12.shared));
	h8_sci_set_external_rate(0, 16934400.0 / (14745600.0 / 8)); // JVS clock (MAME sci_set_external_clock_period)
	h8_reset();
	s12_jvs_reset(&sub.jvs);
	// devices reset after the H8 in MAME's order
	memset(&sub.rtc, 0, sizeof(sub.rtc));
	memset(&sub.set, 0, sizeof(sub.set));
	sub.set.clk = 1;

	sub.last_cycle = psxRegs.cycle;
	sub.half = 0;
	set_event(PSXINT_S12, S12_SUB_QUANTUM);

	// C352 (25401600 Hz / 288 = 88200 Hz), rendered in frame batches (s12_sound.c)
	if (r && r->rgn[S12_RGN_C352])
		s12_sound_reset(r->rgn[S12_RGN_C352], r->rgn_size[S12_RGN_C352]);
	else
		s12_sound_reset(NULL, 0);
}

// ---- save state: board-side sub CPU state + the H8 core (pointers are re-attached on load) ----
uint32_t s12_sub_state_size(void)
{
	return (uint32_t)(sizeof(sub) + sizeof(h8));
}

void s12_sub_state_save(void *dst)
{
	memcpy(dst, &sub, sizeof(sub));
	memcpy((uint8_t *)dst + sizeof(sub), &h8, sizeof(h8));
}

void s12_sub_state_load(const void *src)
{
	const uint8_t *rom = h8.rom;
	const uint32_t rom_size = h8.rom_size;
	uint16_t *ram = h8.ram;
	const uint32_t ram_base = h8.ram_base, ram_size = h8.ram_size;
	memcpy(&sub, src, sizeof(sub));
	memcpy(&h8, (const uint8_t *)src + sizeof(sub), sizeof(h8));
	h8.rom = rom;
	h8.rom_size = rom_size;
	h8.ram = ram;
	h8.ram_base = ram_base;
	h8.ram_size = ram_size;
}

S12SoundBatch *s12_sub_sound_frame(const S12SoundBatch *prev)
{
	catch_up();
	return s12_sound_end_frame(h8_total_cycles(), prev);
}

#ifdef RS12_SUB_PROFILE
// diagnostics: histogram of the main CPU pc at sub CPU slices (where the main CPU spends its time)
#define PCHIST_N 64
static struct { uint32_t pc, n; } s_pchist[PCHIST_N];
static int s_pchist_on = -1;

static void pchist_add(uint32_t pc)
{
	for (int i = 0; i < PCHIST_N; i++) {
		if (s_pchist[i].n && s_pchist[i].pc == pc) {
			s_pchist[i].n++;
			return;
		}
		if (!s_pchist[i].n) {
			s_pchist[i].pc = pc;
			s_pchist[i].n = 1;
			return;
		}
	}
}

void s12_pchist_dump(void)
{
	for (int i = 0; i < PCHIST_N && s_pchist[i].n; i++)
		if (s_pchist[i].n > 20)
			fprintf(stderr, "  pc %08x x%u\n", s_pchist[i].pc, s_pchist[i].n);
}
#endif

void s12_event(void)
{
#ifdef RS12_SUB_PROFILE
	// RS12_PCHIST=<frame>: sample from that frame on
	extern unsigned int frame_counter;
	if (s_pchist_on < 0)
		s_pchist_on = getenv("RS12_PCHIST") ? atoi(getenv("RS12_PCHIST")) : 0x7fffffff;
	if ((int)frame_counter >= s_pchist_on)
		pchist_add(psxRegs.pc);
#endif
	catch_up();
	set_event(PSXINT_S12, S12_SUB_QUANTUM);
}

void s12_vblank(int state)
{
	catch_up();
	h8_set_irq_line(1, state);
	h8_adtrg_w(state);
	sub.portb = (uint8_t)((state ? 0x80 : 0) | (sub.portb & 0x7f));
	if (state && sub.jvs.nodes)
		s12_jvs_frame(&sub.jvs);
}
