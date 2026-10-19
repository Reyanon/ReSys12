// ReSys12: System 12 sound (C352) in frame batches. See s12_sound.h.
#include "s12_sound.h"

#include <string.h>

#include "c352.h"

// two batches: the one being filled by the emulation thread and the one the sound thread may still render
static S12SoundBatch s_batch[2];
static int s_cur;
static uint64_t s_batch_start;      // absolute sample index where the current batch starts
static uint16_t s_mirror[0x4000];   // last value written per register (word offset)
static uint16_t s_flags[32];        // voice flags as the H8 sees them
static uint16_t s_flags_pending[32]; // after a state load: flags to apply at the next frame end
static int s_have_pending;

// exact mode (s12_sound_sync = 1): the C352 runs on the emulation thread in step with the H8, as in MAME, so register
// reads see the current voice state. The frame's samples are rendered as the H8 runs and handed over pre-rendered.
int s12_sound_sync;
static int16_t s_sync_out[S12_SOUND_MAX_FRAMES * 2];
static int s_sync_n;

static void sync_render_to(uint64_t h8_states_now)
{
	const uint64_t t = h8_states_now / S12_H8_STATES_PER_SAMPLE;
	int target = t > s_batch_start ? (int)(t - s_batch_start) : 0;
	if (target > S12_SOUND_MAX_FRAMES)
		target = S12_SOUND_MAX_FRAMES;
	if (target > s_sync_n) {
		c352_render(s_sync_out + s_sync_n * 2, target - s_sync_n);
		s_sync_n = target;
	}
}

// Voice flag bookkeeping on the emulation side, the part of c352_write that does not depend on rendering: a flags
// write stores the value, a write to 0x202 turns KEYON into BUSY and KEYOFF into idle. Reads then see a voice busy
// right after its key-on, as on the chip; without this the firmware saw it idle for a frame or two and reused or
// stopped it (whole sounds missing). Only the renderer's own changes (sample end, loop history) still arrive late.
static void flags_apply(uint16_t *flags, uint32_t off, uint16_t data)
{
	if (off < 0x100) {
		if ((off & 7) == 3)
			flags[off >> 3] = data;
	} else if (off == 0x202) {
		for (int v = 0; v < 32; v++) {
			if (flags[v] & 0x4000) // KEYON -> BUSY, LOOPHIST cleared
				flags[v] = (uint16_t)((flags[v] | 0x8000) & ~(0x4000 | 0x0800));
			if (flags[v] & 0x2000) // KEYOFF -> idle
				flags[v] &= (uint16_t)~(0x8000 | 0x2000);
		}
	}
}

void s12_sound_reset(const uint8_t *wave, uint32_t wave_size)
{
	c352_init(wave, wave_size, 25401600, 288);
	c352_reset();
	memset(s_batch, 0, sizeof(s_batch));
	memset(s_mirror, 0, sizeof(s_mirror));
	memset(s_flags, 0, sizeof(s_flags));
	s_cur = 0;
	s_batch_start = 0;
	s_sync_n = 0;
}

uint16_t s12_sound_reg_read(uint32_t off, uint64_t h8_states_now)
{
	off &= 0x3fff;
	if (s12_sound_sync) {
		sync_render_to(h8_states_now);
		return c352_read(off);
	}
	// voice blocks are 8 words; word 3 is the voice flags register (busy, key off, loop history...)
	if (off < 0x100 && (off & 7) == 3)
		return s_flags[off >> 3];
	return s_mirror[off];
}

void s12_sound_reg_write(uint32_t off, uint16_t data, uint64_t h8_states_now)
{
	off &= 0x3fff;
	s_mirror[off] = data;
	flags_apply(s_flags, off, data);
	if (s12_sound_sync) {
		sync_render_to(h8_states_now);
		c352_write(off, data);
		return;
	}
	S12SoundBatch *b = &s_batch[s_cur];
	if (b->nlog >= S12_SOUND_LOG_MAX)
		return; // never seen; dropping beats stalling
	uint64_t t = h8_states_now / S12_H8_STATES_PER_SAMPLE;
	t = t > s_batch_start ? t - s_batch_start : 0;
	b->log[b->nlog].t = (uint32_t)t;
	b->log[b->nlog].off = (uint16_t)off;
	b->log[b->nlog].data = data;
	b->nlog++;
}

S12SoundBatch *s12_sound_end_frame(uint64_t h8_states_now, const S12SoundBatch *prev)
{
	S12SoundBatch *b = &s_batch[s_cur];
	if (prev || s_have_pending) {
		memcpy(s_flags, prev ? prev->flags_after : s_flags_pending, sizeof(s_flags));
		// the writes of the batch closing now are not rendered yet: replay them on top of the rendered flags
		for (int i = 0; i < b->nlog; i++)
			flags_apply(s_flags, b->log[i].off, b->log[i].data);
	}
	s_have_pending = 0;
	const uint64_t end = h8_states_now / S12_H8_STATES_PER_SAMPLE;
	uint64_t n = end > s_batch_start ? end - s_batch_start : 0;
	if (n > S12_SOUND_MAX_FRAMES)
		n = S12_SOUND_MAX_FRAMES;
	b->frames = (int)n;
	b->prerendered = 0;
	if (s12_sound_sync) {
		sync_render_to(h8_states_now);
		memcpy(b->pre, s_sync_out, (size_t)n * 4);
		b->prerendered = 1;
		b->nlog = 0;
		s_sync_n = 0;
	}
	s_batch_start = end;
	s_cur ^= 1;
	s_batch[s_cur].nlog = 0;
	return b;
}

// ---- save state: only at a frame boundary with the sound thread idle (the open batch is empty then) ----
typedef struct {
	uint64_t batch_start;
	uint16_t mirror[0x4000];
	uint16_t flags[32];
	uint16_t flags_next[32]; // flags after the last rendered batch: what the H8 sees from the next frame end on
} SoundState;

uint32_t s12_sound_state_size(void)
{
	return (uint32_t)sizeof(SoundState) + c352_state_size();
}

void s12_sound_state_save(void *dst)
{
	SoundState *s = (SoundState *)dst;
	s->batch_start = s_batch_start;
	memcpy(s->mirror, s_mirror, sizeof(s_mirror));
	memcpy(s->flags, s_flags, sizeof(s_flags));
	for (int v = 0; v < 32; v++)
		s->flags_next[v] = c352_read((uint32_t)v * 8 + 3);
	c352_state_save((uint8_t *)dst + sizeof(SoundState));
}

void s12_sound_state_load(const void *src)
{
	const SoundState *s = (const SoundState *)src;
	s_batch_start = s->batch_start;
	memcpy(s_mirror, s->mirror, sizeof(s_mirror));
	memcpy(s_flags, s->flags, sizeof(s_flags));
	memcpy(s_flags_pending, s->flags_next, sizeof(s_flags_pending));
	s_have_pending = 1; // the caller passes prev = NULL on the first frame after a load
	c352_state_load((const uint8_t *)src + sizeof(SoundState));
	s_batch[0].nlog = s_batch[1].nlog = 0;
}

int s12_sound_half_rate; // ReSys12: output at 44.1kHz (the Vita), see c352_render_half

int s12_sound_render(S12SoundBatch *b, int16_t *out)
{
	if (b->prerendered) {
		memcpy(out, b->pre, (size_t)b->frames * 4);
		return b->frames;
	}
	int done = 0, outn = 0;
	for (int i = 0; i < b->nlog; i++) {
		int t = (int)b->log[i].t;
		if (t > b->frames)
			t = b->frames;
		if (t > done) {
			if (s12_sound_half_rate)
				outn += c352_render_half(out + outn * 2, t - done);
			else
				c352_render(out + done * 2, t - done);
			done = t;
		}
		c352_write(b->log[i].off, b->log[i].data);
	}
	if (b->frames > done) {
		if (s12_sound_half_rate)
			outn += c352_render_half(out + outn * 2, b->frames - done);
		else
			c352_render(out + done * 2, b->frames - done);
	}
	for (int v = 0; v < 32; v++)
		b->flags_after[v] = c352_read((uint32_t)v * 8 + 3);
	return s12_sound_half_rate ? outn : b->frames;
}
