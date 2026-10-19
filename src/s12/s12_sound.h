// ReSys12: System 12 sound (C352) in frame batches, so it can run on another core.
// The H8 side (emulation thread) only logs register writes with their sample time and answers register reads from a
// mirror (written values) and a snapshot of the voice flags taken when the previous batch finished. A batch is then
// rendered on any thread (s12_sound_render): the output depends only on the batch, so it is deterministic whether
// it runs synchronously (PC harness) or one frame behind on the sound core (Vita).
#ifndef S12_SOUND_H
#define S12_SOUND_H

#include <stdint.h>

#define S12_SOUND_RATE 88200                // C352: 25401600 / 288
#define S12_H8_STATES_PER_SAMPLE 192        // 16934400 / 88200
#define S12_SOUND_LOG_MAX 4096              // register writes per frame
#define S12_SOUND_MAX_FRAMES 4096           // sample pairs per batch (one frame is ~1470)

typedef struct {
	uint32_t t;       // sample index relative to the batch start
	uint16_t off, data;
} S12SoundWrite;

typedef struct {
	int frames;                             // samples to render
	int nlog;
	S12SoundWrite log[S12_SOUND_LOG_MAX];
	uint16_t flags_after[32];               // voice flags when the batch finished (filled by s12_sound_render)
	int prerendered;                        // exact mode: samples already in pre[]
	int16_t pre[S12_SOUND_MAX_FRAMES * 2];
} S12SoundBatch;

// 1 = exact mode: C352 rendered on the emulation thread in step with the H8 (register reads current, as MAME);
// 0 = batch mode (renders one frame behind on the sound thread; voice flag reads lag 1-2 frames)
extern int s12_sound_sync;

// emulation side
void s12_sound_reset(const uint8_t *wave, uint32_t wave_size);
uint16_t s12_sound_reg_read(uint32_t off, uint64_t h8_states_now); // H8 read of C352 register (word offset)
void s12_sound_reg_write(uint32_t off, uint16_t data, uint64_t h8_states_now);
// close the batch covering the H8 time up to h8_states_now and start the next one.
// prev = the batch handed out last time, already rendered (or NULL): its voice flags become what the H8 reads next.
S12SoundBatch *s12_sound_end_frame(uint64_t h8_states_now, const S12SoundBatch *prev);

// render side (sound thread): out holds batch->frames stereo pairs; returns the frames written (batch->frames, or
// about half of it with s12_sound_half_rate)
extern int s12_sound_half_rate;
int s12_sound_render(S12SoundBatch *batch, int16_t *out);

// save state (frame boundary, sound thread idle)
uint32_t s12_sound_state_size(void);
void s12_sound_state_save(void *dst);
void s12_sound_state_load(const void *src);

#endif
