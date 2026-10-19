// ReSys12: Namco System 12 board (main CPU side). Behaviour follows MAME src/mame/namco/namcos12.cpp (BSD-3-Clause, smf et al.).
#ifndef S12_H
#define S12_H

#include <stdint.h>
#include "s12_rom.h"

// JAMMA inputs, active high here (inverted when the H8 reads them)
enum {
	S12_IN_UP = 1 << 0, S12_IN_DOWN = 1 << 1, S12_IN_LEFT = 1 << 2, S12_IN_RIGHT = 1 << 3,
	S12_IN_B1 = 1 << 4, S12_IN_B2 = 1 << 5, S12_IN_B3 = 1 << 6, S12_IN_B4 = 1 << 7,
	S12_IN_B5 = 1 << 8, S12_IN_B6 = 1 << 9, S12_IN_START = 1 << 10, S12_IN_SERVICE = 1 << 11,
	S12_IN_B7 = 1 << 12, S12_IN_B8 = 1 << 13,
};
enum { S12_SYS_COIN1 = 1 << 0, S12_SYS_COIN2 = 1 << 1, S12_SYS_TEST = 1 << 2, S12_SYS_DSW_SERVICE = 1 << 3 };

typedef struct {
	uint32_t player[2]; // S12_IN_*
	uint32_t system;    // S12_SYS_*
	uint16_t adc[4];    // H8 analog inputs AN0-AN3 (10 bits): wheels, pedals, levers, golgo13's gun
	uint16_t gun[4];    // gun board (ptblank2, ghlpanic): P1 X, P1 Y, P2 X, P2 Y
	uint16_t jvs_analog[8]; // JVS I/O board analog inputs (technodr wheel, brake, gas), 16 bits
} S12Input;

// analog controls of a set (MAME input ports)
enum { S12_ANALOG_NONE, S12_ANALOG_GUN, S12_ANALOG_WHEEL, S12_ANALOG_LEVER };
int s12_analog_kind(const S12Game *g);
// player's gun at screen fraction fx, fy (0..1, left/top = 0); the trigger is a button
void s12_set_gun(int player, float fx, float fy);
// wheel -1 (left) .. 1, pedals 0..1
void s12_set_wheel(float steer, float gas, float brake);
// lever -1 (down/back) .. 1 (up/forward)
void s12_set_lever(float v);

typedef struct {
	S12Rom *rom;
	const S12Game *game;
	uint16_t shared[0x8000]; // 64KB shared with the H8 (word = H8 big-endian word)
	uint8_t eeprom[0x800];   // AT28C16
	int eeprom_dirty;
	uint32_t dmaoffset;
	uint16_t bankoffset;
	uint32_t bank_entry, bank_count;
	int alt_bank, boot_hack, tektagt;
	// tektagt protection (KC044)
	int has_tektagt_dma;
	uint32_t tektagdmaoffset;
	int ttt_cnt;
	uint32_t ttt_val[2];
	S12Input in;
	uint32_t log_unmapped; // count of unhandled accesses (diagnostics)
	int64_t rtc_base;      // RTC4543 time at sub CPU reset (unix seconds, UTC); 0 = host local time
} S12Board;

extern S12Board s12;

// pcsx_rearmed cycle multiplier (100 = one 33.8688MHz cycle per instruction). MAME runs the System 12 CPU at
// 100MHz/4 = 25M instruction steps per second: 33.8688 / 25 * 100 = 135. Fewer instructions per frame than 100.
#define S12_CYCLE_MULTIPLIER 135
// The ari64 recompiler charges more cycles than the interpreter for the same code: at 135 the game missed 5-15 of
// 60 frames a second in tektagt fights (ARM harness, interpreter 60). At 120 it matches the interpreter.
#define S12_CYCLE_MULTIPLIER_DYNAREC 120

// attach a loaded set before psxInit/psxReset
void s12_attach(S12Rom *rom);
// JAMMA input bytes as the H8 reads them at 0x300000-0x300003
uint8_t s12_in_r(int offset);
// print distinct unhandled board addresses (diagnostics)
void s12_dump_unmapped(void);
// print unhandled sub CPU (H8) bus addresses and on-chip registers (diagnostics)
void s12_sub_dump(void);
void s12_pchist_dump(void); // RS12_SUB_PROFILE builds
void h8_pchist_dump(int top); // RS12_H8_PCHIST builds
// End of frame for the sound side: catches the H8 up and returns this frame's C352 batch for s12_sound_render.
// prev = the batch returned last time, already rendered (NULL the first time). See s12_sound.h.
#include "s12_sound.h"
S12SoundBatch *s12_sub_sound_frame(const S12SoundBatch *prev);

#endif
