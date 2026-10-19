// ReSys12: Namco C352 PCM sound chip (port of MAME src/devices/sound/c352.cpp, BSD-3-Clause, R. Belmont / superctr).
#ifndef RS12_C352_H
#define RS12_C352_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// rom: wave ROM (8-bit samples / mu-law per voice flags), size in bytes.
// clock: chip clock in Hz (System 12: 25401600), divider: clock divider (System 12: 288) -> sample rate clock/divider.
void c352_init(const uint8_t *rom, uint32_t rom_size, uint32_t clock, int divider);
void c352_reset(void);
uint32_t c352_sample_rate(void); // clock / divider (88200 on System 12)

// 16-bit register interface as the H8 sees it: offset = (address - 0x280000) / 2 (word index)
uint16_t c352_read(uint32_t offset);
void c352_write(uint32_t offset, uint16_t data);

// Render n stereo frames at c352_sample_rate() into out (interleaved L,R, int16). Only the front pair is mixed
// (System 12 routes outputs 0 and 1).
void c352_render(int16_t *out, int n);
// ReSys12: same chip-rate stepping, output at half the rate; returns the frames written ((phase + n) / 2)
int c352_render_half(int16_t *out, int n);

// Save state helpers (size in bytes, then copy out / in)
uint32_t c352_state_size(void);
void c352_state_save(void *dst);
void c352_state_load(const void *src);

#ifdef __cplusplus
}
#endif

#endif
