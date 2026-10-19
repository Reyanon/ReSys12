// license:BSD-3-Clause
// copyright-holders:Olivier Galibert
// ReSys12: Hitachi H8/3002 (H8/300H core + on-chip peripherals) interpreter.
// Port of MAME src/devices/cpu/h8 (h8.cpp, h8h.cpp, h83002.cpp, h8_intc, h8_timer16, h8_sci, h8_port, h8_adc,
// h8_watchdog, h8_refresh; BSD-3-Clause, Olivier Galibert) to C. Single instance, full-instruction execution.
#ifndef RS12_H8_H
#define RS12_H8_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// ---- peripheral state (layout private to src/h8, exposed for save states / debugging) ----
typedef struct {
	uint64_t pending;      // pending vectors (0-63)
	uint8_t irq_type[8];   // 0 = level low, 1 = falling edge
	uint8_t irq_input, ier, isr, iscr;
	uint32_t icr;
	int icr_filter, ipr_filter;
} H8Intc;

typedef struct {
	int index, irq_base;
	int tgr_clearing;
	uint8_t tcr, tier, ier, isr;
	int clock_type, clock_divider;
	uint16_t tcnt, tgr[4]; // GRA, GRB, BRA, BRB
	uint64_t last_clock_update, event_time;
	uint32_t phase, counter_cycle;
	int channel_active;
} H8Timer16;

typedef struct {
	int id, eri_int, rxi_int, txi_int, tei_int;
	int tx_state, rx_state, tx_bit, rx_bit, clock_state, tx_parity, rx_parity, tx_clock_counter, rx_clock_counter;
	int clock_mode;
	int ext_clock_value, rx_value;
	uint8_t rdr, tdr, smr, scr, ssr, ssr_read, brr, rsr, tsr;
	uint64_t clock_event, clock_step, divider;
} H8Sci;

typedef struct {
	int address;
	uint8_t default_ddr, mask, ddr, dr, pcr;
	int32_t last_output;
} H8Port;

typedef struct {
	uint16_t addr[8], buf[2];
	uint8_t adcsr, adcr;
	int trigger, start_mode, start_channel, end_channel, start_count;
	int mode, channel, count;
	int analog_powered, adtrg;
	uint64_t next_event;
} H8Adc;

typedef struct {
	uint8_t tcnt, tcsr, rst;
	uint64_t tcnt_cycle_base;
} H8Watchdog;

typedef struct {
	uint8_t rtcnt, rtcor, rtmcsr;
	uint64_t cycle_base;
} H8Refresh;

typedef struct {
	// core
	uint32_t PPC, NPC, PC;
	uint16_t PIR;
	uint16_t IR[5];
	uint16_t R[16]; // R0-R7, E0-E7
	uint8_t CCR, EXR;
	uint32_t TMP1, TMP2;
	int inst_state;
	int icount, bcount;
	uint64_t end_time; // total_cycles() == end_time - icount
	int irq_vector, taken_irq_vector, irq_level, taken_irq_level, irq_nmi;
	int sleep_fetched;
	uint8_t syscr;
	uint32_t illegal_count;
	uint64_t idle_cycles; // states skipped while sleeping (diagnostics)
	int in_update;      // inside the peripheral event pass (nested update requests are folded into it)
	int reset_request;  // watchdog reset pending

	// memory fast paths (set by the board)
	const uint8_t *rom; // big-endian byte order: word at a = rom[a] << 8 | rom[a + 1]
	uint32_t rom_size;
	uint16_t *ram;      // host-endian 16-bit words, word k = H8 big-endian word at ram_base + 2k
	uint32_t ram_base, ram_size;
	uint8_t iram[0x200]; // 0xfffd10-0xffff0f

	// peripherals
	H8Intc intc;
	H8Timer16 tmr[5];
	uint8_t tstr;
	H8Sci sci[2];
	H8Port port[7]; // 4, 6, 7, 8, 9, A, B
	H8Adc adc;
	H8Watchdog wdt;
	H8Refresh ref;
} H8State;

extern H8State h8;

enum { H8_PORT_4, H8_PORT_6, H8_PORT_7, H8_PORT_8, H8_PORT_9, H8_PORT_A, H8_PORT_B };

// ---- API ----
// also scans the ROM for interrupt wait loops (idle skipping, h8_idle_enable)
void h8_set_rom(const uint8_t *rom, uint32_t size);
extern int h8_idle_enable;
void h8_set_ram(uint16_t *words, uint32_t base, uint32_t size);
void h8_reset(void);
// run for `cycles` H8 states (overrun is carried over to the next call)
void h8_run(int cycles);
// total executed H8 states since reset
uint64_t h8_total_cycles(void);
// external IRQ0-7 pin (1 = asserted)
void h8_set_irq_line(int irq, int state);
// SCI receive pin / external clock pin
void h8_sci_rx_w(int sci, int state);
void h8_sci_clk_w(int sci, int state);
// external clock of an SCI as a fixed rate (MAME sci_set_external_clock_period): H8 states per clock tick, 0 = a real
// pin (h8_sci_clk_w). Kept across resets.
void h8_sci_set_external_rate(int sci, double states_per_tick);
// byte-level receive (a device model instead of the receive pin): returns 0 if the receiver is off or still holds the
// previous byte (RDRF), else stores it in RDR as a completed frame would and raises RXI
int h8_sci_rx_byte(int sci, uint8_t data);
// ADC external trigger pin
void h8_adtrg_w(int state);
// print on-chip registers the firmware touched that are not implemented (once each, also logged live)
void h8_dump_unimplemented(void);

// ---- board callbacks (implemented by the board, e.g. src/s12/s12_sub.c) ----
// external bus outside the ROM / RAM fast paths and the on-chip area
uint8_t h8_ext_read8(uint32_t a);
uint16_t h8_ext_read16(uint32_t a); // a even
void h8_ext_write8(uint32_t a, uint8_t d);
void h8_ext_write16(uint32_t a, uint16_t d); // a even
// digital ports (H8_PORT_*): input pins, output change (data, ddr: 1 = output)
uint8_t h8_port_in(int port);
void h8_port_out(int port, uint8_t data, uint8_t ddr);
// analog inputs AN0-AN7 (10 bits)
uint16_t h8_adc_in(int ch);
// SCI output pins
void h8_sci_tx(int sci, int state);
void h8_sci_clk(int sci, int state);
// a byte moved into the transmit shift register (asynchronous mode): device models that work on bytes
void h8_sci_tx_byte(int sci, uint8_t data);

#ifdef __cplusplus
}
#endif

#endif
