#ifndef RS12_FRONTEND_AUDIO_H
#define RS12_FRONTEND_AUDIO_H

#include <stdint.h>

// SceAudio output
// The emu thread pushes samples into the ring buffer every frame; a highest-priority output thread
// feeds them to sceAudioOutOutput in 256-sample chunks. Push resamples by ±0.5% depending on fill (DRC).

#define RS12_AUDIO_RATE 48000    // output
#define RS12_AUDIO_IN_RATE 88200 // C352 output (25401600 / 288)

int rs12_audio_init(void);
void rs12_audio_fini(void);

// interleaved stereo INT16, frames = number of sample pairs. Excess is dropped when there is no room
void rs12_audio_push(const int16_t *samples, int frames);

typedef struct {
	uint32_t underruns;  // times filled with silence for lack of samples
	uint32_t overflows;  // times dropped because the ring buffer was full
	int fill;            // current ring buffer fill (sample pairs)
	int drc_ppm;         // current rate adjustment (ppm, + reduces output)
	uint32_t in_total;   // total C352 sample pairs in (88.2kHz)
	uint32_t out_total;  // total sample pairs taken by the output thread (48kHz)
} rs12_audio_stats;

void rs12_audio_get_stats(rs12_audio_stats *out);

#ifdef RS12_AUDIO_DUMP
// Diagnostics: collect the first RS12_AUDIO_DUMP_FRAMES in memory, then write them to a file in one go.
// slot 0 = sound thread output (before DRC), 1 = what was sent to the output device (after DRC and ring buffer)
void rs12_audio_dump(const char *path, int slot, const int16_t *samples, int frames);
#endif

#endif
