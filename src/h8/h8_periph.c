// license:BSD-3-Clause
// copyright-holders:Olivier Galibert
// ReSys12: H8/3002 on-chip peripherals and register map.
// Port of MAME src/devices/cpu/h8/h83002.cpp (map), h8_timer16.cpp (H8H channels), h8_sci.cpp, h8_port.cpp,
// h8_adc.cpp (3337 variant), h8_watchdog.cpp (type H), h8_refresh.cpp (BSD-3-Clause, Olivier Galibert).
//
// Implemented: internal RAM, SYSCR, INTC (h8.c), ITU channels 0-4 (TSTR, TCR, TIER, TSR, TCNT, GRA/B, BRA/B),
// SCI0/SCI1 (async + clocked sync, internal clock; bit-level as MAME), ports 4/6/7/8/9/A/B, ADC, watchdog, refresh timer.
// MAME no-ops kept as no-ops (logged once): TSYR, TMDR, TFCR, TOER, TOCR, TIOR, port 4 PCR, watchdog RSTCSR writes.
// Not implemented (logged once): DMAC (0xffff20-0xffff3f) and anything outside the MAME map.
#include "h8_int.h"

#include <stdio.h>
#include <string.h>

#define COMBINE(dst, data, mask) ((dst) = (uint16_t)(((dst) & ~(mask)) | ((data) & (mask))))

// =====================================================================================================================
// 16-bit timer (ITU), H8H channel flavour
// =====================================================================================================================
enum { TGR_CLEAR_NONE = -1, TGR_CLEAR_EXT = -2 };
enum { TMR_IRQ_A = 0x01, TMR_IRQ_B = 0x02, TMR_IRQ_V = 0x10 };
enum { TMR_CLK_DIV = 0, TMR_CLK_INPUT = 1 };
enum { TMR_TGR_COUNT = 2 };

static void tmr_update_counter(H8Timer16 *t, uint64_t cur_time)
{
	if (t->clock_type != TMR_CLK_DIV)
		return;
	if (!cur_time)
		cur_time = h8_now();
	if (!t->channel_active) {
		t->last_clock_update = cur_time;
		return;
	}
	uint64_t base_time = t->last_clock_update;
	t->last_clock_update = cur_time;
	uint64_t new_time = cur_time;
	if (t->clock_divider) {
		base_time = (base_time + t->phase) >> t->clock_divider;
		new_time = (new_time + t->phase) >> t->clock_divider;
	}
	if (new_time == base_time)
		return;

	const uint16_t prev = t->tcnt;
	const uint64_t delta = new_time - base_time;
	const uint64_t tt = t->tcnt + delta;
	if (prev >= t->counter_cycle) {
		if (tt >= 0x10000)
			t->tcnt = (uint16_t)((tt - 0x10000) % t->counter_cycle);
		else
			t->tcnt = (uint16_t)tt;
	} else
		t->tcnt = (uint16_t)(tt % t->counter_cycle);

	for (int i = 0; i < TMR_TGR_COUNT; i++) {
		const uint16_t cmp = (uint16_t)(t->tgr[i] + 1);
		int match = t->tcnt == cmp || (tt == cmp && tt == t->counter_cycle);
		if (!match) {
			// software that polls the flags with interrupts disabled (recalc_event only schedules IRQ events)
			if (prev >= t->counter_cycle)
				match = (cmp > prev && tt >= cmp) ||
					(cmp <= t->counter_cycle && t->tcnt < t->counter_cycle && (delta - (0x10000 - prev)) >= cmp);
			else if (cmp <= t->counter_cycle)
				match = delta >= t->counter_cycle || (prev < cmp && tt >= cmp) || (t->tcnt <= prev && t->tcnt >= cmp);
		}
		if (match) {
			t->isr |= (uint8_t)(1 << i);
			if (t->ier & (1 << i))
				h8_intc_internal_interrupt(t->irq_base + i);
		}
	}
	if (tt >= 0x10000 && (t->counter_cycle == 0x10000 || prev >= t->counter_cycle)) {
		t->isr |= TMR_IRQ_V;
		if (t->ier & TMR_IRQ_V)
			h8_intc_internal_interrupt(t->irq_base + 2);
	}
}

static void tmr_recalc_event(H8Timer16 *t, uint64_t cur_time)
{
	if (!t->channel_active) {
		t->event_time = 0;
		return;
	}
	const int update_cpu = cur_time == 0;
	const uint64_t old_event_time = t->event_time;
	if (t->clock_type != TMR_CLK_DIV) {
		t->event_time = 0;
		if (old_event_time && update_cpu)
			h8_internal_update_now();
		return;
	}
	if (!cur_time)
		cur_time = h8_now();

	uint32_t event_delay = 0xffffffff;
	if (t->tgr_clearing >= 0)
		t->counter_cycle = (uint32_t)t->tgr[t->tgr_clearing] + 1;
	else
		t->counter_cycle = 0x10000;
	if ((t->ier & TMR_IRQ_V) && (t->counter_cycle == 0x10000 || t->tcnt >= t->counter_cycle))
		event_delay = 0x10000 - t->tcnt;

	for (int i = 0; i < TMR_TGR_COUNT; i++)
		if (t->ier & (1 << i)) {
			uint32_t new_delay = 0xffffffff;
			const uint16_t cmp = (uint16_t)(t->tgr[i] + 1);
			if (cmp > t->tcnt) {
				if (t->tcnt >= t->counter_cycle || cmp <= t->counter_cycle)
					new_delay = cmp - t->tcnt;
			} else if (cmp <= t->counter_cycle) {
				if (t->tcnt < t->counter_cycle)
					new_delay = (t->counter_cycle - t->tcnt) + cmp;
				else
					new_delay = (0x10000 - t->tcnt) + cmp;
			}
			if (event_delay > new_delay)
				event_delay = new_delay;
		}

	if (event_delay != 0xffffffff)
		t->event_time = ((((cur_time + (1ULL << t->clock_divider) - t->phase) >> t->clock_divider) + event_delay - 1)
			<< t->clock_divider) + t->phase;
	else
		t->event_time = 0;

	if (old_event_time != t->event_time && update_cpu)
		h8_internal_update_now();
}

static uint64_t tmr_internal_update(H8Timer16 *t, uint64_t current_time)
{
	while (t->event_time && current_time >= t->event_time) {
		const uint64_t ev = t->event_time;
		tmr_update_counter(t, ev);
		tmr_recalc_event(t, ev);
	}
	return t->event_time;
}

static void tmr_tcr_update(H8Timer16 *t)
{
	switch (t->tcr & 0x60) {
	case 0x00:
		t->tgr_clearing = TGR_CLEAR_NONE;
		break;
	case 0x20:
	case 0x40:
		t->tgr_clearing = (t->tcr & 0x20) ? 0 : 1;
		break;
	case 0x60:
		t->tgr_clearing = TGR_CLEAR_EXT;
		break;
	}
	const int count_type = t->tcr & 7;
	if (count_type < 4) {
		t->clock_type = TMR_CLK_DIV;
		t->clock_divider = count_type;
		if (count_type <= 1)
			t->phase = 0;
		else {
			switch (t->tcr & 0x18) {
			case 0x00:
				t->phase = 0;
				break;
			case 0x08:
				t->phase = 1u << (t->clock_divider - 1);
				break;
			case 0x10:
			case 0x18:
				t->phase = 0;
				t->clock_divider--;
				break;
			}
		}
	} else {
		// external clock inputs TCLKA-D: not wired on System 12
		t->clock_type = TMR_CLK_INPUT;
		t->clock_divider = 0;
		t->phase = 0;
	}
}

static void tmr_tcr_w(H8Timer16 *t, uint8_t d)
{
	tmr_update_counter(t, 0);
	t->tcr = d;
	tmr_tcr_update(t);
	tmr_recalc_event(t, 0);
}

static void tmr_tier_w(H8Timer16 *t, uint8_t d)
{
	tmr_update_counter(t, 0);
	t->tier = d | 0xf8;
	t->ier = (uint8_t)(((d & 0x01) ? TMR_IRQ_A : 0) | ((d & 0x02) ? TMR_IRQ_B : 0) | ((d & 0x04) ? TMR_IRQ_V : 0));
	tmr_recalc_event(t, 0);
}

static uint8_t tmr_tsr_r(H8Timer16 *t)
{
	tmr_update_counter(t, 0);
	return (uint8_t)(0xf8 | ((t->isr & TMR_IRQ_V) ? 4 : 0) | ((t->isr & TMR_IRQ_B) ? 2 : 0) | ((t->isr & TMR_IRQ_A) ? 1 : 0));
}

static void tmr_tsr_w(H8Timer16 *t, uint8_t d)
{
	tmr_update_counter(t, 0);
	if (!(d & 1))
		t->isr &= ~TMR_IRQ_A;
	if (!(d & 2))
		t->isr &= ~TMR_IRQ_B;
	if (!(d & 4))
		t->isr &= ~TMR_IRQ_V;
	tmr_recalc_event(t, 0);
}

static uint16_t tmr_tcnt_r(H8Timer16 *t)
{
	tmr_update_counter(t, 0);
	return t->tcnt;
}

static void tmr_tcnt_w(H8Timer16 *t, uint16_t d, uint16_t mask)
{
	tmr_update_counter(t, 0);
	COMBINE(t->tcnt, d, mask);
	tmr_recalc_event(t, 0);
}

static void tmr_tgr_w(H8Timer16 *t, int i, uint16_t d, uint16_t mask)
{
	tmr_update_counter(t, 0);
	COMBINE(t->tgr[i], d, mask);
	tmr_recalc_event(t, 0);
}

static void tmr_set_enable(H8Timer16 *t, int enable)
{
	tmr_update_counter(t, 0);
	t->channel_active = enable;
	tmr_recalc_event(t, 0);
}

static void tmr_reset(H8Timer16 *t, int index)
{
	const int active = t->channel_active;
	memset(t, 0, sizeof(*t));
	t->channel_active = active;
	t->index = index;
	t->irq_base = 24 + 4 * index; // IMIA, IMIB, OVI
	memset(t->tgr, 0xff, sizeof(t->tgr));
	t->tgr_clearing = TGR_CLEAR_NONE;
	t->clock_type = TMR_CLK_DIV;
	t->counter_cycle = 0x10000;
	t->last_clock_update = h8_now();
}

static void tstr_w(uint8_t d)
{
	h8.tstr = d;
	for (int i = 0; i < 5; i++)
		tmr_set_enable(&h8.tmr[i], (d >> i) & 1);
}

// =====================================================================================================================
// SCI
// =====================================================================================================================
enum { ST_IDLE, ST_START, ST_BIT, ST_PARITY, ST_STOP, ST_LAST_TICK };
enum { CLK_TX = 1, CLK_RX = 2 };
enum { INTERNAL_ASYNC, INTERNAL_ASYNC_OUT, EXTERNAL_ASYNC, EXTERNAL_RATE_ASYNC, INTERNAL_SYNC_OUT, EXTERNAL_SYNC, EXTERNAL_RATE_SYNC };
enum {
	SMR_CA = 0x80, SMR_CHR = 0x40, SMR_PE = 0x20, SMR_OE = 0x10, SMR_STOP = 0x08, SMR_MP = 0x04, SMR_CKS = 0x03,
	SCR_TIE = 0x80, SCR_RIE = 0x40, SCR_TE = 0x20, SCR_RE = 0x10, SCR_MPIE = 0x08, SCR_TEIE = 0x04, SCR_CKE = 0x03,
	SCR_CKE1 = 0x02, SCR_CKE0 = 0x01,
	SSR_TDRE = 0x80, SSR_RDRF = 0x40, SSR_ORER = 0x20, SSR_FER = 0x10, SSR_PER = 0x08, SSR_TEND = 0x04, SSR_MPB = 0x02,
	SSR_MPBT = 0x01
};

static void sci_rx_start(H8Sci *s);
static void sci_tx_start(H8Sci *s);
static double s_sci_ext_rate[2]; // h8_sci_set_external_rate: H8 states per external clock tick (0 = clock pin)
static uint8_t s_sci_rx_pin[2];   // the receive pin has been driven (h8_sci_rx_w): the receiver may start any tick

static int sci_has_recv_error(const H8Sci *s) { return s->ssr & (SSR_ORER | SSR_PER | SSR_FER); }

static void sci_clock_update(H8Sci *s)
{
	s->divider = 2ull << (2 * (s->smr & SMR_CKS));
	s->divider *= (uint64_t)s->brr + 1;
	if (s->smr & SMR_CA)
		s->clock_mode = (s->scr & SCR_CKE1) ? EXTERNAL_SYNC : INTERNAL_SYNC_OUT;
	else if (s->scr & SCR_CKE1)
		s->clock_mode = s_sci_ext_rate[s->id] > 0 ? EXTERNAL_RATE_ASYNC : EXTERNAL_ASYNC;
	else if (s->scr & SCR_CKE0)
		s->clock_mode = INTERNAL_ASYNC_OUT;
	else
		s->clock_mode = INTERNAL_ASYNC;
	// System 12: SCI0 has an external clock period in MAME (JVS, 14.7456MHz/8); technodr's firmware selects it (CKE1).
	// Synchronous mode with a rate clock (EXTERNAL_RATE_SYNC) is not used by any set and not modelled.
}

static void sci_sync_rx_start(H8Sci *s)
{
	if (s->rx_state == ST_IDLE && (s->smr & SMR_CA) && (s->scr & SCR_RE) && !(s->scr & SCR_TE) && !sci_has_recv_error(s))
		sci_rx_start(s);
}

static void sci_clock_start(H8Sci *s, int mode)
{
	if (s->clock_state & mode)
		return;
	if (mode == CLK_TX)
		s->tx_clock_counter = 15;
	else
		s->rx_clock_counter = 15;
	s->clock_state |= mode;
	if (s->clock_state != mode)
		return;
	s->clock_step = 0;
	switch (s->clock_mode) {
	case INTERNAL_ASYNC:
	case INTERNAL_ASYNC_OUT:
	case INTERNAL_SYNC_OUT: {
		s->clock_step = s->divider;
		const uint64_t now = h8_now();
		s->clock_event = (now / s->clock_step + 1) * s->clock_step;
		h8_internal_update_now();
		break;
	}
	case EXTERNAL_RATE_ASYNC: { // MAME: next external tick after now, in H8 states
		const double r = s_sci_ext_rate[s->id];
		s->clock_event = (uint64_t)((double)(uint64_t)((double)h8_now() / r + 1) * r + 1);
		h8_internal_update_now();
		break;
	}
	default:
		// EXTERNAL_ASYNC / EXTERNAL_SYNC: wait for the external clock pin
		break;
	}
}

static void sci_clock_stop(H8Sci *s, int mode)
{
	s->clock_state &= ~mode;
	if (!s->clock_state) {
		s->clock_event = 0;
		s->clock_step = 0;
	}
	h8_internal_update_now();
}

static void sci_tx_start(H8Sci *s)
{
	s->ssr |= SSR_TDRE;
	s->tsr = s->tdr;
	if (!(s->smr & SMR_CA))
		h8_sci_tx_byte(s->id, s->tsr);
	s->tx_parity = (s->smr & SMR_OE) ? 0 : 1;
	if (s->scr & SCR_TIE)
		h8_intc_internal_interrupt(s->txi_int);
	if (s->smr & SMR_CA) {
		s->tx_state = ST_BIT;
		s->tx_bit = 8;
	} else {
		s->tx_state = ST_START;
		s->tx_bit = 1;
	}
	sci_clock_start(s, CLK_TX);
	if (s->rx_state == ST_IDLE && (s->smr & SMR_CA) && (s->scr & SCR_TE) && (s->scr & SCR_RE) && !sci_has_recv_error(s))
		sci_rx_start(s);
}

static void sci_tx_async_step(H8Sci *s)
{
	switch (s->tx_state) {
	case ST_START:
		h8_sci_tx(s->id, 0);
		s->tx_state = ST_BIT;
		s->tx_bit = (s->smr & SMR_CHR) ? 7 : 8;
		break;
	case ST_BIT:
		s->tx_parity ^= (s->tsr & 1);
		h8_sci_tx(s->id, s->tsr & 1);
		s->tsr >>= 1;
		s->tx_bit--;
		if (!s->tx_bit) {
			if (s->smr & SMR_CA) {
				if (!(s->ssr & SSR_TDRE))
					sci_tx_start(s);
				else {
					s->tx_state = ST_LAST_TICK;
					s->tx_bit = 0;
				}
			} else if (s->smr & SMR_PE) {
				s->tx_state = ST_PARITY;
				s->tx_bit = 1;
			} else {
				s->tx_state = ST_STOP;
				s->tx_bit = (s->smr & SMR_STOP) ? 2 : 1;
			}
		}
		break;
	case ST_PARITY:
		h8_sci_tx(s->id, s->tx_parity);
		s->tx_state = ST_STOP;
		s->tx_bit = (s->smr & SMR_STOP) ? 2 : 1;
		break;
	case ST_STOP:
		h8_sci_tx(s->id, 1);
		s->tx_bit--;
		if (!s->tx_bit) {
			if (!(s->ssr & SSR_TDRE))
				sci_tx_start(s);
			else {
				s->tx_state = ST_LAST_TICK;
				s->tx_bit = 0;
			}
		}
		break;
	case ST_LAST_TICK:
		s->tx_state = ST_IDLE;
		s->tx_bit = 0;
		sci_clock_stop(s, CLK_TX);
		h8_sci_tx(s->id, 1);
		if (s->scr & SCR_TEIE)
			h8_intc_internal_interrupt(s->tei_int);
		if ((s->scr & SCR_TE) && !(s->ssr & SSR_TDRE))
			sci_tx_start(s);
		else
			s->ssr |= SSR_TEND;
		break;
	default:
		break;
	}
}

static void sci_tx_async_tick(H8Sci *s)
{
	s->tx_clock_counter = (s->tx_clock_counter + 1) & 15;
	if (s->tx_clock_counter == 0) {
		sci_tx_async_step(s);
		if (s->clock_mode == INTERNAL_ASYNC_OUT)
			h8_sci_clk(s->id, 0);
	} else if (s->tx_clock_counter == 8 && s->clock_mode == INTERNAL_ASYNC_OUT)
		h8_sci_clk(s->id, 1);
}

static void sci_tx_sync_step(H8Sci *s)
{
	if (!s->tx_bit) {
		s->tx_state = ST_IDLE;
		sci_clock_stop(s, CLK_TX);
		h8_sci_tx(s->id, 1);
		if (s->scr & SCR_TEIE)
			h8_intc_internal_interrupt(s->tei_int);
		if ((s->scr & SCR_TE) && !(s->ssr & SSR_TDRE))
			sci_tx_start(s);
		else
			s->ssr |= SSR_TEND;
	} else {
		h8_sci_tx(s->id, s->tsr & 1);
		s->tsr >>= 1;
		s->tx_bit--;
	}
}

static void sci_tx_sync_tick(H8Sci *s)
{
	s->tx_clock_counter = (s->tx_clock_counter + 1) & 1;
	if (s->tx_clock_counter == 0) {
		sci_tx_sync_step(s);
		if (s->clock_mode == INTERNAL_SYNC_OUT && s->tx_state != ST_IDLE)
			h8_sci_clk(s->id, 0);
	} else if (s->tx_clock_counter == 1 && s->clock_mode == INTERNAL_SYNC_OUT)
		h8_sci_clk(s->id, 1);
}

static void sci_rx_start(H8Sci *s)
{
	s->rx_parity = (s->smr & SMR_OE) ? 0 : 1;
	s->rsr = 0x00;
	if (s->smr & SMR_CA) {
		s->rx_state = ST_BIT;
		s->rx_bit = 8;
	} else {
		s->rx_state = ST_START;
		s->rx_bit = 1;
	}
	sci_clock_start(s, CLK_RX);
}

static void sci_rx_done(H8Sci *s)
{
	if (!(s->ssr & SSR_FER)) {
		if ((s->smr & SMR_PE) && s->rx_parity)
			s->ssr |= SSR_PER;
		else if (s->ssr & SSR_RDRF)
			s->ssr |= SSR_ORER;
		else {
			s->ssr |= SSR_RDRF;
			s->rdr = s->rsr;
		}
	}
	if (s->scr & SCR_RIE) {
		if (sci_has_recv_error(s))
			h8_intc_internal_interrupt(s->eri_int);
		else
			h8_intc_internal_interrupt(s->rxi_int);
	}
	s->rx_state = ST_IDLE;
	sci_clock_stop(s, CLK_RX);
	sci_sync_rx_start(s);
}

static void sci_rx_async_step(H8Sci *s)
{
	switch (s->rx_state) {
	case ST_START:
		if (s->rx_value) {
			s->rx_state = ST_IDLE;
			sci_clock_stop(s, CLK_RX);
			break;
		}
		s->rx_state = ST_BIT;
		s->rx_bit = (s->smr & SMR_CHR) ? 7 : 8;
		break;
	case ST_BIT:
		s->rsr >>= 1;
		if (s->rx_value) {
			s->rx_parity = !s->rx_parity;
			s->rsr |= ((s->smr & (SMR_CA | SMR_CHR)) == SMR_CHR) ? 0x40 : 0x80;
		}
		s->rx_bit--;
		if (!s->rx_bit) {
			s->rx_state = (s->smr & SMR_PE) ? ST_PARITY : ST_STOP;
			s->rx_bit = 1;
		}
		break;
	case ST_PARITY:
		s->rx_parity ^= s->rx_value;
		s->rx_state = ST_STOP;
		s->rx_bit = 1;
		break;
	case ST_STOP:
		if (!s->rx_value)
			s->ssr |= SSR_FER;
		else if ((s->smr & SMR_PE) && s->rx_parity)
			s->ssr |= SSR_PER;
		sci_rx_done(s);
		break;
	default:
		break;
	}
}

static void sci_rx_async_tick(H8Sci *s)
{
	s->rx_clock_counter = (s->rx_clock_counter + 1) & 15;
	if (s->rx_clock_counter == 8)
		sci_rx_async_step(s);
}

static void sci_rx_sync_step(H8Sci *s)
{
	s->rsr >>= 1;
	if (s->rx_value)
		s->rsr |= 0x80;
	s->rx_bit--;
	if (!s->rx_bit)
		sci_rx_done(s);
}

static void sci_rx_sync_tick(H8Sci *s)
{
	s->rx_clock_counter = (s->rx_clock_counter + 1) & 1;
	if (s->rx_clock_counter == 0 && s->clock_mode == INTERNAL_SYNC_OUT)
		h8_sci_clk(s->id, 0);
	else if (s->rx_clock_counter == 1) {
		if (s->clock_mode == INTERNAL_SYNC_OUT)
			h8_sci_clk(s->id, 1);
		sci_rx_sync_step(s);
	}
}

static uint64_t sci_internal_update(H8Sci *s, uint64_t current_time)
{
	if (!s->clock_event || current_time < s->clock_event)
		return s->clock_event;
	if (s->clock_mode == INTERNAL_ASYNC || s->clock_mode == INTERNAL_ASYNC_OUT || s->clock_mode == EXTERNAL_RATE_ASYNC) {
		if (s->clock_state & CLK_TX)
			sci_tx_async_tick(s);
		if (s->clock_state & CLK_RX)
			sci_rx_async_tick(s);
	} else if (s->clock_mode == INTERNAL_SYNC_OUT || s->clock_mode == EXTERNAL_RATE_SYNC) {
		if (s->clock_state & CLK_TX)
			sci_tx_sync_tick(s);
		if (s->clock_state & CLK_RX)
			sci_rx_sync_tick(s);
	}
	if (s->clock_state) {
		// ReSys12: an asynchronous transmitter alone only acts on every 16th tick (tx_clock_counter 0); when nothing
		// else can use the ticks in between (no receive pin, no clock output), the next event is that tick. Same
		// times as ticking through, far fewer events (technodr's JVS transmit waits can then be skipped).
		int ticks = 1;
		if (s->clock_state == CLK_TX && !s_sci_rx_pin[s->id] &&
			(s->clock_mode == INTERNAL_ASYNC || s->clock_mode == EXTERNAL_RATE_ASYNC)) {
			ticks = 16 - s->tx_clock_counter;
			s->tx_clock_counter = 15;
		}
		if (s->clock_mode == EXTERNAL_RATE_ASYNC) {
			const double r = s_sci_ext_rate[s->id];
			for (int t = 0; t < ticks; t++)
				s->clock_event = (uint64_t)((double)(uint64_t)((double)s->clock_event / r + 1) * r + 1);
		} else if (s->clock_step)
			s->clock_event += s->clock_step * (uint64_t)ticks;
		else
			s->clock_event = 0;
		if (s->clock_event)
			h8_internal_update_now();
	} else {
		s->clock_event = 0;
		if (s->clock_mode == INTERNAL_ASYNC_OUT || s->clock_mode == INTERNAL_SYNC_OUT)
			h8_sci_clk(s->id, 1);
	}
	return s->clock_event;
}

static void sci_smr_w(H8Sci *s, uint8_t d)
{
	s->smr = d;
	sci_clock_update(s);
	sci_sync_rx_start(s);
}

static void sci_brr_w(H8Sci *s, uint8_t d)
{
	s->brr = d;
	sci_clock_update(s);
}

static void sci_scr_w(H8Sci *s, uint8_t d)
{
	const uint8_t delta = s->scr ^ d;
	s->scr = d;
	sci_clock_update(s);
	if ((delta & SCR_RE) && !(s->scr & SCR_RE)) {
		s->rx_state = ST_IDLE;
		sci_clock_stop(s, CLK_RX);
	}
	if (delta & (SCR_RE | SCR_TE))
		sci_sync_rx_start(s);
	if ((delta & SCR_TIE) && (s->scr & SCR_TIE) && (s->ssr & SSR_TDRE))
		h8_intc_internal_interrupt(s->txi_int);
	if ((delta & SCR_TEIE) && (s->scr & SCR_TEIE) && (s->ssr & SSR_TEND))
		h8_intc_internal_interrupt(s->tei_int);
	if ((delta & SCR_RIE) && (s->scr & SCR_RIE) && (s->ssr & SSR_RDRF))
		h8_intc_internal_interrupt(s->rxi_int);
	if ((delta & SCR_RIE) && (s->scr & SCR_RIE) && sci_has_recv_error(s))
		h8_intc_internal_interrupt(s->eri_int);
}

static void sci_ssr_w(H8Sci *s, uint8_t d)
{
	if ((s->scr & SCR_TE) && (s->ssr & s->ssr_read & SSR_TDRE) && !(d & SSR_TDRE))
		s->ssr &= ~(SSR_TDRE | SSR_TEND);
	s->ssr = (uint8_t)((s->ssr & (~s->ssr_read | d | SSR_TDRE | SSR_TEND | SSR_MPB) & ~SSR_MPBT) | (d & SSR_MPBT));
	s->ssr_read &= s->ssr;
	if (s->tx_state == ST_IDLE && !(s->ssr & SSR_TDRE))
		sci_tx_start(s);
	sci_sync_rx_start(s);
}

static uint8_t sci_ssr_r(H8Sci *s)
{
	s->ssr_read = s->ssr;
	return s->ssr;
}

static void sci_reset(H8Sci *s, int id)
{
	memset(s, 0, sizeof(*s));
	s->id = id;
	s->eri_int = 52 + 4 * id;
	s->rxi_int = 53 + 4 * id;
	s->txi_int = 54 + 4 * id;
	s->tei_int = 55 + 4 * id;
	s->tdr = 0xff;
	s->ssr = 0x84;
	s->brr = 0xff;
	s->tsr = 0xff;
	s->tx_state = ST_IDLE;
	s->rx_state = ST_IDLE;
	s->clock_mode = INTERNAL_ASYNC;
	sci_clock_update(s);
	s->ext_clock_value = 1;
	s->rx_value = 1;
	h8_sci_clk(id, 1);
	h8_sci_tx(id, 1);
}

void h8_sci_rx_w(int sci, int state)
{
	H8Sci *s = &h8.sci[sci];
	state = state ? 1 : 0;
	if (!state)
		s_sci_rx_pin[sci & 1] = 1;
	if (state != s->rx_value && (s->clock_state & CLK_RX))
		if (s->rx_clock_counter == 1 || s->rx_clock_counter == 15)
			s->rx_clock_counter = 0;
	if (s->rx_state == ST_IDLE && !(s->smr & SMR_CA) && (s->scr & SCR_RE) && s->rx_value && !state)
		sci_rx_start(s);
	s->rx_value = state;
}

void h8_sci_set_external_rate(int sci, double states_per_tick)
{
	s_sci_ext_rate[sci & 1] = states_per_tick;
	sci_clock_update(&h8.sci[sci & 1]);
}

int h8_sci_rx_byte(int sci, uint8_t data)
{
	H8Sci *s = &h8.sci[sci & 1];
	if (!(s->scr & SCR_RE) || (s->ssr & SSR_RDRF) || sci_has_recv_error(s))
		return 0;
	s->ssr |= SSR_RDRF;
	s->rdr = data;
	if (s->scr & SCR_RIE)
		h8_intc_internal_interrupt(s->rxi_int);
	return 1;
}

void h8_sci_clk_w(int sci, int state)
{
	H8Sci *s = &h8.sci[sci];
	state = state ? 1 : 0;
	if (s->ext_clock_value == state)
		return;
	s->ext_clock_value = state;
	if (!s->clock_state)
		return;
	if (s->clock_mode == EXTERNAL_ASYNC) {
		if (s->clock_state & CLK_TX)
			sci_tx_async_tick(s);
		if (s->clock_state & CLK_RX)
			sci_rx_async_tick(s);
	} else if (s->clock_mode == EXTERNAL_SYNC) {
		if (s->clock_state & CLK_TX)
			sci_tx_sync_tick(s);
		if (s->clock_state & CLK_RX)
			sci_rx_sync_tick(s);
	}
}

// =====================================================================================================================
// digital ports
// =====================================================================================================================
static void port_update_output(H8Port *p)
{
	const uint8_t data = (uint8_t)((p->dr | ~p->ddr) & ~p->mask);
	const uint8_t ddr = (uint8_t)(p->ddr & ~p->mask);
	const int32_t res = ddr << 8 | data;
	if (res != p->last_output) {
		p->last_output = res;
		h8_port_out(p->address, data, ddr);
	}
}

static uint8_t port_r(H8Port *p)
{
	uint8_t res = (uint8_t)(p->mask | (p->dr & p->ddr));
	if ((uint8_t)(p->ddr & ~p->mask) != (uint8_t)~p->mask)
		res |= h8_port_in(p->address) & (uint8_t)~p->ddr;
	return res;
}

static void port_ddr_w(H8Port *p, uint8_t d)
{
	p->ddr = d;
	port_update_output(p);
}

static void port_dr_w(H8Port *p, uint8_t d)
{
	p->dr = d;
	port_update_output(p);
}

static void port_init(H8Port *p, int address, uint8_t default_ddr, uint8_t mask)
{
	p->address = address;
	p->default_ddr = default_ddr;
	p->mask = mask;
	p->ddr = default_ddr;
	p->dr = 0;
	p->pcr = 0;
	p->last_output = -1;
}

// =====================================================================================================================
// ADC (H8/3337 flavour as used by MAME for the H8/3002)
// =====================================================================================================================
enum { T_SOFT = 1, T_EXT = 8 };
enum { F_ADF = 0x80, F_ADIE = 0x40, F_ADST = 0x20 };
enum { ADC_IDLE = 0, ADC_ACTIVE = 1, ADC_HALTED = 2, ADC_REPEAT = 4, ADC_ROTATE = 8 };
#define ADC_VECTOR 60
#define ADC_REGISTER_MASK 3

static void adc_mode_update(void)
{
	H8Adc *a = &h8.adc;
	a->trigger = (a->adcr & 0x80) ? T_EXT : T_SOFT;
	if (a->adcsr & 0x10) {
		a->start_mode = ADC_ACTIVE | ADC_REPEAT | ADC_ROTATE;
		a->start_channel = a->adcsr & 4;
		a->end_channel = a->adcsr & 7;
	} else {
		a->start_mode = ADC_ACTIVE;
		a->start_channel = a->end_channel = a->adcsr & 7;
	}
}

static int adc_conversion_time(int first)
{
	if (first)
		return (h8.adc.adcsr & 0x08) ? 134 : 266;
	return (h8.adc.adcsr & 0x08) ? 128 : 256;
}

static void adc_conversion_wait(int first, uint64_t current_time)
{
	if (current_time)
		h8.adc.next_event = current_time + (uint64_t)adc_conversion_time(first);
	else {
		h8.adc.next_event = h8_now() + (uint64_t)adc_conversion_time(first);
		h8_internal_update_now();
	}
}

static void adc_sampling(void)
{
	h8.adc.buf[0] = h8_adc_in(h8.adc.channel);
}

static void adc_done(void)
{
	h8.adc.mode = ADC_IDLE;
	h8.adc.adcsr &= ~F_ADST;
}

static void adc_start_conversion(void)
{
	H8Adc *a = &h8.adc;
	a->mode = a->start_mode;
	a->channel = a->start_channel;
	a->count = a->start_count;
	adc_sampling();
	adc_conversion_wait(1, 0);
	a->analog_powered = 1;
}

static void adc_timeout(uint64_t current_time)
{
	H8Adc *a = &h8.adc;
	a->addr[a->channel & ADC_REGISTER_MASK] = a->buf[0];
	if (a->mode & ADC_ROTATE) {
		if (a->channel != a->end_channel) {
			a->channel = (a->channel + 1) & 7;
			adc_sampling();
			adc_conversion_wait(0, current_time);
			return;
		}
		a->channel = a->start_channel;
	}
	a->adcsr |= F_ADF;
	if (a->adcsr & F_ADIE)
		h8_intc_internal_interrupt(ADC_VECTOR);
	if (a->mode & ADC_REPEAT) {
		a->channel = a->start_channel;
		a->count = a->start_count;
		adc_sampling();
		adc_conversion_wait(0, current_time);
		return;
	}
	adc_done();
}

static uint64_t adc_internal_update(uint64_t current_time)
{
	if (h8.adc.next_event && h8.adc.next_event <= current_time) {
		h8.adc.next_event = 0;
		adc_timeout(current_time);
	}
	return h8.adc.next_event;
}

static void adc_adcsr_w(uint8_t d)
{
	H8Adc *a = &h8.adc;
	const uint8_t prev = a->adcsr;
	a->adcsr = (uint8_t)((d & 0x7f) | (a->adcsr & d & F_ADF));
	adc_mode_update();
	if ((prev & F_ADF) && !(a->adcsr & F_ADF)) {
		if (a->mode & ADC_HALTED) {
			a->mode &= ~ADC_HALTED;
			if (!(a->adcsr & F_ADST)) {
				adc_sampling();
				adc_conversion_wait(0, 0);
			} else
				adc_done();
		}
	}
	if (!(prev & F_ADST) && (a->adcsr & F_ADST))
		adc_start_conversion();
}

static void adc_adcr_w(uint8_t d)
{
	h8.adc.adcr = d;
	adc_mode_update();
}

void h8_adtrg_w(int state)
{
	H8Adc *a = &h8.adc;
	state = state ? 1 : 0;
	if (state != a->adtrg) {
		a->adtrg = state;
		if (!a->adtrg && (a->trigger & T_EXT) && !(a->adcsr & F_ADST)) {
			a->adcsr |= F_ADST;
			adc_start_conversion();
		}
	}
}

static void adc_reset(void)
{
	H8Adc *a = &h8.adc;
	memset(a, 0, sizeof(*a));
	a->trigger = T_SOFT;
	a->start_count = 1;
	adc_mode_update();
	a->analog_powered = 1;
	a->adtrg = 1;
}

// =====================================================================================================================
// watchdog (type H) and refresh timer
// =====================================================================================================================
enum { TCSR_CKS = 0x07, TCSR_NMI = 0x08, TCSR_TME = 0x20, TCSR_WT = 0x40, TCSR_OVF = 0x80 };
static const int wdt_div[8] = { 1, 5, 6, 7, 8, 9, 11, 12 };
#define WDT_IRQ 20

static void wdt_tcnt_update(uint64_t cur_time)
{
	H8Watchdog *w = &h8.wdt;
	if (w->tcsr & TCSR_TME) {
		const int shift = wdt_div[w->tcsr & TCSR_CKS];
		if (!cur_time)
			cur_time = h8_now();
		const uint64_t spos = w->tcnt_cycle_base >> shift;
		const uint64_t epos = cur_time >> shift;
		const uint64_t next_tcnt = w->tcnt + (epos - spos);
		w->tcnt = (uint8_t)next_tcnt;
		w->tcnt_cycle_base = cur_time;
		if (next_tcnt >= 0x100) {
			if (w->tcsr & TCSR_WT)
				h8.reset_request = 1;
			else if (!(w->tcsr & TCSR_OVF)) {
				w->tcsr |= TCSR_OVF;
				h8_intc_internal_interrupt(WDT_IRQ);
			}
		}
	} else
		w->tcnt = 0;
}

static uint64_t wdt_internal_update(uint64_t current_time)
{
	H8Watchdog *w = &h8.wdt;
	wdt_tcnt_update(current_time);
	if (w->tcsr & TCSR_TME) {
		const int shift = wdt_div[w->tcsr & TCSR_CKS];
		const uint64_t spos = w->tcnt_cycle_base >> shift;
		return (spos + 0x100 - w->tcnt) << shift;
	}
	return 0;
}

static uint16_t wdt_wd_r(void)
{
	wdt_tcnt_update(0);
	return (uint16_t)(((h8.wdt.tcsr | 0x18) << 8) | h8.wdt.tcnt);
}

static void wdt_wd_w(uint16_t d, uint16_t mask)
{
	H8Watchdog *w = &h8.wdt;
	if (mask != 0xffff)
		return;
	if ((d & 0xff00) == 0xa500) {
		wdt_tcnt_update(0);
		if (!(w->tcsr & TCSR_TME) && (d & TCSR_TME))
			w->tcnt_cycle_base = h8_now();
		w->tcsr = (uint8_t)((w->tcsr & d & TCSR_OVF) | (d & 0x7f));
		h8_internal_update_now();
	}
	if ((d & 0xff00) == 0x5a00) {
		if (w->tcsr & TCSR_TME) {
			w->tcnt = (uint8_t)d;
			w->tcnt_cycle_base = h8_now();
		}
		h8_internal_update_now();
	}
}

enum { RTMCSR_CMF = 0x80, RTMCSR_CMIE = 0x40, RTMCSR_CKS = 0x38 };
static const int ref_div_shift[8] = { -1, 1, 3, 5, 7, 9, 11, 12 };
#define REF_IRQ 21

static void ref_rtcnt_update(uint64_t cur_time)
{
	H8Refresh *r = &h8.ref;
	const int shift = ref_div_shift[(r->rtmcsr & RTMCSR_CKS) >> 3];
	if (shift < 0)
		return;
	if (!cur_time)
		cur_time = h8_now();
	const uint64_t spos = r->cycle_base >> shift;
	const uint64_t epos = cur_time >> shift;
	const uint64_t elapsed = epos - spos;
	r->cycle_base = cur_time;
	if (!elapsed)
		return;
	const uint32_t period = (uint32_t)r->rtcor + 1;
	uint64_t next = r->rtcnt + elapsed;
	if (next >= period) {
		if (!(r->rtmcsr & RTMCSR_CMF)) {
			r->rtmcsr |= RTMCSR_CMF;
			if (r->rtmcsr & RTMCSR_CMIE)
				h8_intc_internal_interrupt(REF_IRQ);
		}
		next %= period;
	}
	r->rtcnt = (uint8_t)next;
}

static uint64_t ref_internal_update(uint64_t current_time)
{
	H8Refresh *r = &h8.ref;
	ref_rtcnt_update(current_time);
	const int shift = ref_div_shift[(r->rtmcsr & RTMCSR_CKS) >> 3];
	if (shift >= 0 && (r->rtmcsr & RTMCSR_CMIE)) {
		const uint64_t spos = r->cycle_base >> shift;
		return (spos + ((uint64_t)r->rtcor + 1) - r->rtcnt) << shift;
	}
	return 0;
}

static void ref_rtmcsr_w(uint8_t d)
{
	H8Refresh *r = &h8.ref;
	ref_rtcnt_update(0);
	r->rtmcsr = (uint8_t)((r->rtmcsr & RTMCSR_CMF) | (d & (RTMCSR_CMIE | RTMCSR_CKS)));
	if (!(d & RTMCSR_CMF))
		r->rtmcsr &= ~RTMCSR_CMF;
	if ((r->rtmcsr & RTMCSR_CMF) && (r->rtmcsr & RTMCSR_CMIE))
		h8_intc_internal_interrupt(REF_IRQ);
	r->cycle_base = h8_now();
	h8_internal_update_now();
}

// =====================================================================================================================
// reset / event pass
// =====================================================================================================================
static void add_event(uint64_t *event_time, uint64_t new_event)
{
	if (!new_event)
		return;
	if (!*event_time || *event_time > new_event)
		*event_time = new_event;
}

uint64_t h8_periph_update(uint64_t current_time)
{
	uint64_t ev = 0;
	add_event(&ev, adc_internal_update(current_time));
	add_event(&ev, sci_internal_update(&h8.sci[0], current_time));
	add_event(&ev, sci_internal_update(&h8.sci[1], current_time));
	for (int i = 0; i < 5; i++)
		add_event(&ev, tmr_internal_update(&h8.tmr[i], current_time));
	add_event(&ev, wdt_internal_update(current_time));
	add_event(&ev, ref_internal_update(current_time));
	return ev;
}

void h8_periph_reset(int power_on)
{
	h8.in_update = 1; // no event recomputation while the devices are half reset
	adc_reset();
	if (power_on) {
		port_init(&h8.port[H8_PORT_4], H8_PORT_4, 0x00, 0x00);
		port_init(&h8.port[H8_PORT_6], H8_PORT_6, 0x80, 0x80);
		port_init(&h8.port[H8_PORT_7], H8_PORT_7, 0x00, 0x00);
		port_init(&h8.port[H8_PORT_8], H8_PORT_8, 0xf0, 0xe0);
		port_init(&h8.port[H8_PORT_9], H8_PORT_9, 0x00, 0xc0);
		port_init(&h8.port[H8_PORT_A], H8_PORT_A, 0x00, 0x00);
		port_init(&h8.port[H8_PORT_B], H8_PORT_B, 0x00, 0x00);
	}
	for (int i = 0; i < 7; i++)
		port_update_output(&h8.port[i]);
	for (int i = 0; i < 5; i++)
		tmr_reset(&h8.tmr[i], i);
	sci_reset(&h8.sci[0], 0);
	sci_reset(&h8.sci[1], 1);
	h8.wdt.tcnt_cycle_base = h8_now();
	h8.wdt.tcnt = h8.wdt.tcsr = h8.wdt.rst = 0;
	h8.ref.rtcnt = 0;
	h8.ref.rtcor = 0xff;
	h8.ref.rtmcsr = 0;
	h8.ref.cycle_base = h8_now();
	tstr_w(0xe0); // default TSTR (channels stopped)
	h8.in_update = 0;
}

// =====================================================================================================================
// on-chip register map (0xfffd10-0xffffff)
// =====================================================================================================================
static H8Timer16 *tmr_at(uint32_t o, uint32_t *rel)
{
	static const uint16_t base[5] = { 0x64, 0x6e, 0x78, 0x82, 0x92 };
	static const uint8_t size[5] = { 10, 10, 10, 14, 14 };
	for (int i = 0; i < 5; i++)
		if (o >= base[i] && o < (uint32_t)base[i] + size[i]) {
			*rel = o - base[i];
			return &h8.tmr[i];
		}
	return NULL;
}

// ReSys12 (idle loop skipping): a counter on an internal clock steps once every 2^divider states, so after the
// current count it takes more than (thr - 1 - tcnt) << divider states to reach thr
uint64_t h8_tcnt_states_below(uint32_t a, uint16_t thr)
{
	uint32_t rel;
	H8Timer16 *t = tmr_at(a & 0xfe, &rel);
	if ((a & 0xffff00) != 0xffff00 || !t || rel < 4 || rel >= 6)
		return 0;
	if (!t->channel_active)
		return ~0ull >> 1; // stopped
	if (t->clock_type != TMR_CLK_DIV)
		return 0;
	tmr_update_counter(t, 0);
	if (t->tcnt >= t->counter_cycle || t->tcnt >= thr)
		return 0;
	if (t->counter_cycle < thr)
		return ~0ull >> 1; // clears before it gets there
	return (uint64_t)(thr - 1 - t->tcnt) << t->clock_divider;
}

// 16-bit registers: TCNT, GRA/GRB, BRA/BRB, watchdog TCSR/TCNT and RSTCSR
static int reg16_read(uint32_t o, uint16_t *v)
{
	uint32_t rel;
	H8Timer16 *t = tmr_at(o & ~1u, &rel);
	if (t && rel >= 4) {
		if (rel < 6)
			*v = tmr_tcnt_r(t);
		else
			*v = t->tgr[(rel - 6) >> 1];
		return 1;
	}
	if ((o & ~1u) == 0xa8) {
		*v = wdt_wd_r();
		return 1;
	}
	if ((o & ~1u) == 0xaa) {
		*v = h8.wdt.rst | 0x3f;
		return 1;
	}
	return 0;
}

static int reg16_write(uint32_t o, uint16_t d, uint16_t mask)
{
	uint32_t rel;
	H8Timer16 *t = tmr_at(o & ~1u, &rel);
	if (t && rel >= 4) {
		if (rel < 6)
			tmr_tcnt_w(t, d, mask);
		else if (rel < 10)
			tmr_tgr_w(t, (int)(rel - 6) >> 1, d, mask);
		else
			COMBINE(t->tgr[(rel - 6) >> 1], d, mask); // BRA/BRB: stored only
		return 1;
	}
	if ((o & ~1u) == 0xa8) {
		wdt_wd_w(d, mask);
		return 1;
	}
	if ((o & ~1u) == 0xaa) {
		if (mask == 0xffff && ((d & 0xff00) == 0xa500 || (d & 0xff00) == 0x5a00))
			h8_note_unimplemented(0xffff00 | o, 1); // RSTCSR (MAME logs only)
		return 1;
	}
	return 0;
}

static uint8_t reg8_read(uint32_t a)
{
	const uint32_t o = a & 0xff;
	uint32_t rel;
	H8Timer16 *t = tmr_at(o, &rel);
	if (t) {
		switch (rel) {
		case 0: return t->tcr;
		case 1: h8_note_unimplemented(a, 0); return 0x00; // TIOR (MAME: 0)
		case 2: return t->tier;
		case 3: return tmr_tsr_r(t);
		}
	}
	switch (o) {
	case 0x60: return h8.tstr;
	case 0x61: case 0x62: case 0x63: case 0x90: case 0x91:
		h8_note_unimplemented(a, 0); // TSYR, TMDR, TFCR, TOER, TOCR (MAME: 0)
		return 0x00;
	case 0xad: ref_rtcnt_update(0); return h8.ref.rtmcsr | 0x07;
	case 0xae: ref_rtcnt_update(0); return h8.ref.rtcnt;
	case 0xaf: return h8.ref.rtcor;
	case 0xb0: case 0xb8: return h8.sci[o >= 0xb8].smr;
	case 0xb1: case 0xb9: return h8.sci[o >= 0xb8].brr;
	case 0xb2: case 0xba: return h8.sci[o >= 0xb8].scr;
	case 0xb3: case 0xbb: return h8.sci[o >= 0xb8].tdr;
	case 0xb4: case 0xbc: return sci_ssr_r(&h8.sci[o >= 0xb8]);
	case 0xb5: case 0xbd: return h8.sci[o >= 0xb8].rdr;
	case 0xc5: case 0xc9: case 0xcd: case 0xd0: case 0xd1: case 0xd4: return 0xff; // DDRs are write-only
	case 0xc7: return port_r(&h8.port[H8_PORT_4]);
	case 0xcb: return port_r(&h8.port[H8_PORT_6]);
	case 0xce: return port_r(&h8.port[H8_PORT_7]);
	case 0xcf: return port_r(&h8.port[H8_PORT_8]);
	case 0xd2: return port_r(&h8.port[H8_PORT_9]);
	case 0xd3: return port_r(&h8.port[H8_PORT_A]);
	case 0xd6: return port_r(&h8.port[H8_PORT_B]);
	case 0xda: return h8.port[H8_PORT_4].pcr | h8.port[H8_PORT_4].mask;
	case 0xe0: case 0xe1: case 0xe2: case 0xe3: case 0xe4: case 0xe5: case 0xe6: case 0xe7: {
		const int off = (int)(o - 0xe0);
		return (uint8_t)((off & 1) ? h8.adc.addr[off >> 1] << 6 : h8.adc.addr[off >> 1] >> 2);
	}
	case 0xe8: return h8.adc.adcsr;
	case 0xe9: return h8.adc.adcr;
	case 0xf2: return h8.syscr;
	case 0xf4: return h8_intc_iscr_r();
	case 0xf5: return h8_intc_ier_r();
	case 0xf6: return h8_intc_isr_r();
	case 0xf8: return h8_intc_icr_r(0);
	case 0xf9: return h8_intc_icr_r(1);
	}
	h8_note_unimplemented(a, 0);
	return 0;
}

static void reg8_write(uint32_t a, uint8_t d)
{
	const uint32_t o = a & 0xff;	uint32_t rel;
	H8Timer16 *t = tmr_at(o, &rel);
	if (t) {
		switch (rel) {
		case 0: tmr_tcr_w(t, d); return;
		case 1: h8_note_unimplemented(a, 1); return; // TIOR (MAME: log only)
		case 2: tmr_tier_w(t, d); return;
		case 3: tmr_tsr_w(t, d); return;
		}
	}
	switch (o) {
	case 0x60: tstr_w(d); return;
	case 0x61: case 0x62: case 0x63: case 0x90: case 0x91:
		h8_note_unimplemented(a, 1); // TSYR, TMDR, TFCR, TOER, TOCR (MAME: log only)
		return;
	case 0xad: ref_rtmcsr_w(d); return;
	case 0xae: ref_rtcnt_update(0); h8.ref.rtcnt = d; h8_internal_update_now(); return;
	case 0xaf: ref_rtcnt_update(0); h8.ref.rtcor = d; h8_internal_update_now(); return;
	case 0xb0: case 0xb8: sci_smr_w(&h8.sci[o >= 0xb8], d); return;
	case 0xb1: case 0xb9: sci_brr_w(&h8.sci[o >= 0xb8], d); return;
	case 0xb2: case 0xba: sci_scr_w(&h8.sci[o >= 0xb8], d); return;
	case 0xb3: case 0xbb: h8.sci[o >= 0xb8].tdr = d; return; // not a DMA access: no auto start
	case 0xb4: case 0xbc: sci_ssr_w(&h8.sci[o >= 0xb8], d); return;
	case 0xc5: port_ddr_w(&h8.port[H8_PORT_4], d); return;
	case 0xc7: port_dr_w(&h8.port[H8_PORT_4], d); return;
	case 0xc9: port_ddr_w(&h8.port[H8_PORT_6], d); return;
	case 0xcb: port_dr_w(&h8.port[H8_PORT_6], d); return;
	case 0xcd: port_ddr_w(&h8.port[H8_PORT_8], d); return;
	case 0xcf: port_dr_w(&h8.port[H8_PORT_8], d); return;
	case 0xd0: port_ddr_w(&h8.port[H8_PORT_9], d); return;
	case 0xd1: port_ddr_w(&h8.port[H8_PORT_A], d); return;
	case 0xd2: port_dr_w(&h8.port[H8_PORT_9], d); return;
	case 0xd3: port_dr_w(&h8.port[H8_PORT_A], d); return;
	case 0xd4: port_ddr_w(&h8.port[H8_PORT_B], d); return;
	case 0xd6: port_dr_w(&h8.port[H8_PORT_B], d); return;
	case 0xda: h8.port[H8_PORT_4].pcr = d; return; // pull-ups: no effect
	case 0xe8: adc_adcsr_w(d); return;
	case 0xe9: adc_adcr_w(d); return;
	case 0xf2: h8_syscr_w(d); return;
	case 0xf4: h8_intc_iscr_w(d); return;
	case 0xf5: h8_intc_ier_w(d); return;
	case 0xf6: h8_intc_isr_w(d); return;
	case 0xf8: h8_intc_icr_w(0, d); return;
	case 0xf9: h8_intc_icr_w(1, d); return;
	}
	h8_note_unimplemented(a, 1);
}

static inline int is_iram(uint32_t a) { return a >= 0xfffd10 && a < 0xffff10; }

uint8_t h8_onchip_read8(uint32_t a)
{
	if (is_iram(a))
		return h8.iram[a - 0xfffd10];
	if (a < 0xffff20) {
		h8_note_unimplemented(a, 0);
		return 0;
	}
	uint16_t v;
	if (reg16_read(a & 0xff, &v))
		return (uint8_t)((a & 1) ? v : v >> 8);
	return reg8_read(a);
}

uint16_t h8_onchip_read16(uint32_t a)
{
	if (is_iram(a))
		return (uint16_t)(h8.iram[a - 0xfffd10] << 8 | h8.iram[a - 0xfffd10 + 1]);
	if (a < 0xffff20) {
		h8_note_unimplemented(a, 0);
		return 0;
	}
	uint16_t v;
	if (reg16_read(a & 0xff, &v))
		return v;
	const uint8_t hi = reg8_read(a);
	return (uint16_t)(hi << 8 | reg8_read(a + 1));
}

void h8_onchip_write8(uint32_t a, uint8_t d)
{
	if (is_iram(a)) {
		h8.iram[a - 0xfffd10] = d;
		return;
	}
	if (a < 0xffff20) {
		h8_note_unimplemented(a, 1);
		return;
	}
	if (reg16_write(a & 0xff, (a & 1) ? d : (uint16_t)(d << 8), (a & 1) ? 0x00ff : 0xff00))
		return;
	reg8_write(a, d);
}

void h8_onchip_write16(uint32_t a, uint16_t d)
{
	if (is_iram(a)) {
		h8.iram[a - 0xfffd10] = (uint8_t)(d >> 8);
		h8.iram[a - 0xfffd10 + 1] = (uint8_t)d;
		return;
	}
	if (a < 0xffff20) {
		h8_note_unimplemented(a, 1);
		return;
	}
	if (reg16_write(a & 0xff, d, 0xffff))
		return;
	reg8_write(a, (uint8_t)(d >> 8));
	reg8_write(a + 1, (uint8_t)d);
}
