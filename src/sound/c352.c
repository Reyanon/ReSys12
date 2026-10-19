// license:BSD-3-Clause
// copyright-holders:R. Belmont, superctr
// ReSys12: Namco C352 custom PCM chip.
// Port of MAME src/devices/sound/c352.cpp v2.0 (R. Belmont, rewritten and improved by superctr, additional code by cync
// and the hoot development team). Behaviour follows MAME master; the only deliberate difference is the final int16
// conversion (saturating here, truncating in MAME; define C352_MAME_WRAP for MAME's wrap-around).
//
// Chip specs: 32 voices, 8-bit linear and 8-bit mu-law samples, 4 x 16-bit outputs. Only the front pair (outputs 0 and
// 1) is rendered here, which is all System 12 routes.
//
// Rendering is voice-major (each busy voice runs its whole block with its state in registers, accumulating into a 32-bit
// mix buffer) instead of MAME's sample-major loop. This is exact: voices only share the noise LFSR, so noise voices are
// rendered in a separate sample-major pass in voice order, and integer accumulation order does not matter.
#include "c352.h"

#include <string.h>

enum {
	C352_FLG_BUSY     = 0x8000, // channel is busy
	C352_FLG_KEYON    = 0x4000, // keyon
	C352_FLG_KEYOFF   = 0x2000, // keyoff
	C352_FLG_LOOPTRG  = 0x1000, // loop trigger
	C352_FLG_LOOPHIST = 0x0800, // loop history
	C352_FLG_FM       = 0x0400, // frequency modulation
	C352_FLG_PHASERL  = 0x0200, // rear left invert phase 180 degrees
	C352_FLG_PHASEFL  = 0x0100, // front left invert phase 180 degrees
	C352_FLG_PHASEFR  = 0x0080, // invert phase 180 degrees (front right, and rear right in MAME)
	C352_FLG_LDIR     = 0x0040, // loop direction
	C352_FLG_LINK     = 0x0020, // "long-format" sample
	C352_FLG_NOISE    = 0x0010, // play noise instead of sample
	C352_FLG_MULAW    = 0x0008, // sample is mu-law instead of linear 8-bit PCM
	C352_FLG_FILTER   = 0x0004, // don't apply filter (no interpolation)
	C352_FLG_REVLOOP  = 0x0003, // loop backwards
	C352_FLG_LOOP     = 0x0002, // loop forward
	C352_FLG_REVERSE  = 0x0001  // play sample backwards
};

// Same fields and order as MAME's c352_voice_t (also the save state layout).
typedef struct {
	uint32_t pos;
	uint32_t counter;
	int16_t sample;
	int16_t last_sample;
	uint16_t vol_f;
	uint16_t vol_r;
	uint8_t curr_vol[4];
	uint16_t freq;
	uint16_t flags;
	uint16_t wave_bank;
	uint16_t wave_start;
	uint16_t wave_end;
	uint16_t wave_loop;
} C352Voice;

typedef struct {
	C352Voice v[32];
	uint16_t random;
	uint16_t control; // control flags, purpose unknown
} C352State;

#define C352_CHUNK 256 // frames per mix pass (2KB of int32 stack)

static const uint8_t c352_zero_rom[1];

static struct {
	C352State st;
	const uint8_t *rom;
	uint32_t rom_mask;
	uint32_t rate;
	int16_t mulaw[256];
} c352;

void c352_init(const uint8_t *rom, uint32_t rom_size, uint32_t clock, int divider)
{
	memset(&c352, 0, sizeof(c352));
	// device_rom_interface<24>: 24-bit address space; a smaller power-of-two ROM is mirrored, a larger one is banked
	// (bank 0 only, System 12 does not bank).
	if (rom && rom_size) {
		c352.rom = rom;
		c352.rom_mask = (rom_size - 1 >= 0xffffffu) ? 0xffffffu : rom_size - 1;
	} else {
		c352.rom = c352_zero_rom;
		c352.rom_mask = 0;
	}
	if (divider <= 0)
		divider = 288;
	c352.rate = clock / (uint32_t)divider;

	// mu-law table (output similar to Namco's VC emulator)
	int j = 0;
	for (int i = 0; i < 128; i++) {
		c352.mulaw[i] = (int16_t)(j << 5);
		if (i < 16)
			j += 1;
		else if (i < 24)
			j += 2;
		else if (i < 48)
			j += 4;
		else if (i < 100)
			j += 8;
		else
			j += 16;
	}
	for (int i = 0; i < 128; i++)
		c352.mulaw[i + 128] = (int16_t)((~(uint16_t)c352.mulaw[i]) & 0xffe0);

	c352_reset();
}

void c352_reset(void)
{
	memset(&c352.st, 0, sizeof(c352.st));
	c352.st.random = 0x1234;
	c352.st.control = 0;
}

uint32_t c352_sample_rate(void)
{
	return c352.rate;
}

// ---- registers ----
// Per voice (offset < 0x100, 8 words each): vol_f, vol_r, freq, flags, wave_bank, wave_start, wave_end, wave_loop.
static uint16_t *voice_reg(uint32_t offset)
{
	C352Voice *v = &c352.st.v[offset >> 3];
	switch (offset & 7) {
	case 0: return &v->vol_f;
	case 1: return &v->vol_r;
	case 2: return &v->freq;
	case 3: return &v->flags;
	case 4: return &v->wave_bank;
	case 5: return &v->wave_start;
	case 6: return &v->wave_end;
	default: return &v->wave_loop;
	}
}

// The caller must have rendered up to the current time first (MAME does m_stream->update() here): flags carry BUSY,
// KEYOFF and LOOPHIST status updated by the renderer.
uint16_t c352_read(uint32_t offset)
{
	if (offset < 0x100)
		return *voice_reg(offset);
	if (offset == 0x200)
		return c352.st.control;
	return 0;
}

// Full 16-bit writes only (mem_mask 0xffff in MAME terms).
void c352_write(uint32_t offset, uint16_t data)
{
	if (offset < 0x100) {
		*voice_reg(offset) = data;
	} else if (offset == 0x200) {
		c352.st.control = data;
	} else if (offset == 0x202) { // execute keyons/keyoffs
		for (int i = 0; i < 32; i++) {
			C352Voice *v = &c352.st.v[i];
			if (v->flags & C352_FLG_KEYON) {
				v->pos = ((uint32_t)v->wave_bank << 16) | v->wave_start;
				v->sample = 0;
				v->last_sample = 0;
				v->counter = 0xffff;
				v->flags |= C352_FLG_BUSY;
				v->flags &= (uint16_t)~(C352_FLG_KEYON | C352_FLG_LOOPHIST);
				v->curr_vol[0] = v->curr_vol[1] = 0;
				v->curr_vol[2] = v->curr_vol[3] = 0;
			}
			if (v->flags & C352_FLG_KEYOFF) {
				v->flags &= (uint16_t)~(C352_FLG_BUSY | C352_FLG_KEYOFF);
				v->counter = 0xffff;
			}
		}
	}
}

// ---- rendering ----
static inline int ramp(int cur, int target)
{
	return cur > target ? cur - 1 : (cur < target ? cur + 1 : cur);
}

// Interpolated output, bit-exact with MAME: last + (u32)(counter * (sample - last)) >> 16 in unsigned arithmetic,
// truncated to s16.
static inline int32_t interp(int32_t last, int32_t sample, uint32_t counter)
{
	return (int16_t)(uint16_t)((uint32_t)last + ((counter * (uint32_t)(sample - last)) >> 16));
}

// One non-noise voice for n frames, state held in locals. filter is a compile-time constant at each call site so the
// interpolation branch is hoisted out of the loop. half (compile-time): the voice still steps at the chip rate, but only
// every second step (the ones where (ph + i) is odd) is interpolated and mixed, into mix[0], mix[2]...
static inline __attribute__((always_inline)) void render_pcm_voice_(C352Voice *v, int32_t *mix, int n, const int filter,
	const int half, const int ph)
{
	const uint8_t *rom = c352.rom;
	const uint32_t mask = c352.rom_mask;
	const int16_t *mulaw = c352.mulaw;
	const uint32_t freq = v->freq;
	const uint32_t wave_end = v->wave_end;
	const uint32_t wave_loop = v->wave_loop;
	const uint32_t link_pos = ((uint32_t)v->wave_start << 16) | wave_loop;
	const int tgt0 = v->vol_f >> 8, tgt1 = v->vol_f & 0xff, tgt2 = v->vol_r >> 8, tgt3 = v->vol_r & 0xff;
	uint32_t flags = v->flags;
	const int sgn_l = (flags & C352_FLG_PHASEFL) ? -1 : 1;
	const int sgn_r = (flags & C352_FLG_PHASEFR) ? -1 : 1;
	uint32_t pos = v->pos;
	uint32_t counter = v->counter;
	int32_t sample = v->sample;
	int32_t last = v->last_sample;
	int cv0 = v->curr_vol[0], cv1 = v->curr_vol[1], cv2 = v->curr_vol[2], cv3 = v->curr_vol[3];
	// (-s * vol) >> 8 == (s * -vol) >> 8, so the phase flags fold into signed effective volumes
	int32_t el = sgn_l * cv0, er = sgn_r * cv1;

	for (int i = 0; i < n; i++) {
		const uint32_t next = counter + freq;
		if (next & 0x10000) {
			// fetch_sample()
			last = sample;
			const int8_t s = (int8_t)rom[pos & mask];
			sample = (flags & C352_FLG_MULAW) ? mulaw[(uint8_t)s] : s * 256;
			const uint32_t p = pos & 0xffff;
			if ((flags & C352_FLG_LOOP) && (flags & C352_FLG_REVERSE)) {
				if ((flags & C352_FLG_LDIR) && p == wave_loop) // backwards > forwards
					flags &= ~(uint32_t)C352_FLG_LDIR;
				else if (!(flags & C352_FLG_LDIR) && p == wave_end) // forwards > backwards
					flags |= C352_FLG_LDIR;
				pos += (flags & C352_FLG_LDIR) ? (uint32_t)-1 : 1u;
			} else if (p == wave_end) {
				if ((flags & C352_FLG_LINK) && (flags & C352_FLG_LOOP)) {
					pos = link_pos;
					flags |= C352_FLG_LOOPHIST;
				} else if (flags & C352_FLG_LOOP) {
					pos = (pos & 0xff0000) | wave_loop;
					flags |= C352_FLG_LOOPHIST;
				} else {
					flags |= C352_FLG_KEYOFF;
					flags &= ~(uint32_t)C352_FLG_BUSY;
					sample = 0;
				}
			} else {
				pos += (flags & C352_FLG_REVERSE) ? (uint32_t)-1 : 1u;
			}
		}
		if ((next ^ counter) & 0x18000) {
			cv0 = ramp(cv0, tgt0);
			cv1 = ramp(cv1, tgt1);
			cv2 = ramp(cv2, tgt2); // rear pair is not mixed but its ramp state is kept exact
			cv3 = ramp(cv3, tgt3);
			el = sgn_l * cv0;
			er = sgn_r * cv1;
		}
		counter = next & 0xffff;

		if (!half || ((ph + i) & 1)) {
			const int32_t s = filter ? sample : interp(last, sample, counter);
			const int o = half ? (ph + i) >> 1 : i;
			mix[2 * o] += (s * el) >> 8;
			mix[2 * o + 1] += (s * er) >> 8;
		}

		if (!(flags & C352_FLG_BUSY)) // sample ended: this frame is still mixed, later ones are silent
			break;
	}

	v->pos = pos;
	v->counter = counter;
	v->sample = (int16_t)sample;
	v->last_sample = (int16_t)last;
	v->flags = (uint16_t)flags;
	v->curr_vol[0] = (uint8_t)cv0;
	v->curr_vol[1] = (uint8_t)cv1;
	v->curr_vol[2] = (uint8_t)cv2;
	v->curr_vol[3] = (uint8_t)cv3;
}

static void render_pcm_interp(C352Voice *v, int32_t *mix, int n)
{
	render_pcm_voice_(v, mix, n, 0, 0, 0);
}

static void render_pcm_filter(C352Voice *v, int32_t *mix, int n)
{
	render_pcm_voice_(v, mix, n, 1, 0, 0);
}

static void render_pcm_interp_half(C352Voice *v, int32_t *mix, int n, int ph)
{
	render_pcm_voice_(v, mix, n, 0, 1, ph);
}

static void render_pcm_filter_half(C352Voice *v, int32_t *mix, int n, int ph)
{
	render_pcm_voice_(v, mix, n, 1, 1, ph);
}

// One sample of a noise voice, straight from MAME's sound_stream_update() (noise voices never end on their own).
static inline int32_t step_noise_voice(C352Voice *v)
{
	const uint32_t next = v->counter + v->freq;
	if (next & 0x10000) {
		uint16_t r = c352.st.random;
		r = (uint16_t)((r >> 1) ^ ((-(r & 1)) & 0xfff6));
		c352.st.random = r;
		v->last_sample = v->sample;
		v->sample = (int16_t)r;
	}
	if ((next ^ v->counter) & 0x18000) {
		v->curr_vol[0] = (uint8_t)ramp(v->curr_vol[0], v->vol_f >> 8);
		v->curr_vol[1] = (uint8_t)ramp(v->curr_vol[1], v->vol_f & 0xff);
		v->curr_vol[2] = (uint8_t)ramp(v->curr_vol[2], v->vol_r >> 8);
		v->curr_vol[3] = (uint8_t)ramp(v->curr_vol[3], v->vol_r & 0xff);
	}
	v->counter = next & 0xffff;
	if (v->flags & C352_FLG_FILTER)
		return v->sample;
	return interp(v->last_sample, v->sample, v->counter);
}

// half: n chip-rate steps starting at phase ph produce (ph + n) / 2 frames (ReSys12: half-rate output on the Vita)
static void render_chunk(int32_t *mix, int n, const int half, const int ph)
{
	C352Voice *const vv = c352.st.v;
	uint32_t noise_mask = 0;
	const int nout = half ? (ph + n) >> 1 : n;

	memset(mix, 0, sizeof(int32_t) * 2 * (size_t)(nout > 0 ? nout : 0));
	for (int j = 0; j < 32; j++) {
		C352Voice *v = &vv[j];
		const uint16_t f = v->flags;
		if (!(f & C352_FLG_BUSY)) // idle voices output 0 and keep their state
			continue;
		if (f & C352_FLG_NOISE)
			noise_mask |= 1u << j;
		else if (half)
			(f & C352_FLG_FILTER) ? render_pcm_filter_half(v, mix, n, ph) : render_pcm_interp_half(v, mix, n, ph);
		else if (f & C352_FLG_FILTER)
			render_pcm_filter(v, mix, n);
		else
			render_pcm_interp(v, mix, n);
	}
	if (!noise_mask)
		return;

	// Noise voices share the LFSR, so they advance sample-major in voice order exactly like MAME.
	for (int i = 0; i < n; i++) {
		const int emit = !half || ((ph + i) & 1);
		const int o = half ? (ph + i) >> 1 : i;
		for (uint32_t m = noise_mask; m; m &= m - 1) {
			C352Voice *v = &vv[__builtin_ctz(m)];
			const int32_t s = step_noise_voice(v);
			if (emit) {
				mix[2 * o] += (((v->flags & C352_FLG_PHASEFL) ? -s : s) * v->curr_vol[0]) >> 8;
				mix[2 * o + 1] += (((v->flags & C352_FLG_PHASEFR) ? -s : s) * v->curr_vol[1]) >> 8;
			}
		}
	}
}

static inline int16_t out16(int32_t x)
{
	x >>= 3;
#ifdef C352_MAME_WRAP
	return (int16_t)x;
#else
	return (int16_t)(x > 32767 ? 32767 : (x < -32768 ? -32768 : x));
#endif
}

void c352_render(int16_t *out, int n)
{
	int32_t mix[2 * C352_CHUNK];

	while (n > 0) {
		const int k = n < C352_CHUNK ? n : C352_CHUNK;
		render_chunk(mix, k, 0, 0);
		for (int i = 0; i < 2 * k; i++)
			out[i] = out16(mix[i]);
		out += 2 * k;
		n -= k;
	}
}

// ReSys12: half-rate output (every second chip-rate step), for the Vita: about half the mixing work.
static int s_half_phase; // steps done since the last output frame (0/1)

int c352_render_half(int16_t *out, int n)
{
	int32_t mix[2 * C352_CHUNK];
	int total = 0;

	while (n > 0) {
		const int k = n < C352_CHUNK ? n : C352_CHUNK;
		const int nout = (s_half_phase + k) >> 1;
		render_chunk(mix, k, 1, s_half_phase);
		for (int i = 0; i < 2 * nout; i++)
			out[i] = out16(mix[i]);
		out += 2 * nout;
		total += nout;
		s_half_phase = (s_half_phase + k) & 1;
		n -= k;
	}
	return total;
}

// ---- save state ----
uint32_t c352_state_size(void)
{
	return (uint32_t)sizeof(C352State);
}

void c352_state_save(void *dst)
{
	memcpy(dst, &c352.st, sizeof(C352State));
}

void c352_state_load(const void *src)
{
	memcpy(&c352.st, src, sizeof(C352State));
}
