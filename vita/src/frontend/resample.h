#ifndef RS12_FRONTEND_RESAMPLE_H
#define RS12_FRONTEND_RESAMPLE_H

#include <stdint.h>

// Resampler for audio DRC (stereo INT16, 16-tap Lanczos, 1024 phases)
// With linear interpolation the HF rolloff varied periodically as the fractional position drifted, and in-band SNR
// stayed at 31dB (crackle). 16 taps give 68dB under the same conditions. Latency is 8 samples.

#define RS12_RESAMPLE_MAX_IN 4096 // max sample pairs per call

void rs12_resample_init(void);

// Resample frames sample pairs of in with step (16.16, input advance per output sample) into out.
// Returns the number of output sample pairs. out must hold at least frames * 65536 / step + 2 pairs.
int rs12_resample(const int16_t *in, int frames, uint32_t step, int16_t *out);

#endif
