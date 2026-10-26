// ReSys12: game routines run natively (HLE). See s12_hle.c
#ifndef S12_HLE_H
#define S12_HLE_H

#include <stdint.h>

// the R3000 HLE opcode of these routines (0x3b, as PCSX's HLE calls, with a code no BIOS call uses)
#define RS12_HLE_OP 0xec000000u
#define RS12_HLE_CODE_LZ 0x3ffff0u

extern uint32_t rs12_hle_lz_head; // the LZ routine's loop head, 0 = not found / off

// look for the routines in the program flash (at each board reset)
void s12_hle_scan(const uint8_t *flash, uint32_t size);
// the LZ routine from its loop head: runs until it ends or the next event is due
void rs12_hle_lz(void);

#endif
