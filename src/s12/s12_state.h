// ReSys12: save states for System 12 (main CPU/GPU/IO from pcsx_rearmed + board, H8, C352).
// Serialized to memory; the frontend writes/reads the file (fast sceIo on the Vita).
// Call only between frames, with the sound thread idle and no batch in flight (the GPU thread is synced here).
#ifndef S12_STATE_H
#define S12_STATE_H

#include <stddef.h>
#include <stdint.h>

// malloc'd buffer with the state of the running set; NULL on failure
uint8_t *s12_state_save(size_t *size);
// 0 on success; on failure (wrong set / version / size) the machine is left untouched
int s12_state_load(const uint8_t *buf, size_t size);

#endif
