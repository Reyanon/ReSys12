// ReSys12: System 12 set table types (tools/gen_s12_games.py fills s12_games.h with these)
#ifndef S12_TYPES_H
#define S12_TYPES_H

#include <stdint.h>

enum { S12_RGN_MAIN, S12_RGN_BANK, S12_RGN_SUB, S12_RGN_C352, S12_RGN_EEPROM, S12_RGN_M148, S12_RGN_IOCPU, S12_RGN_COUNT };
enum { S12_LD_PLAIN, S12_LD_16_BYTE, S12_LD_16_WORD_SWAP, S12_LD_32_BYTE, S12_LD_32_WORD, S12_LD_RELOAD, S12_LD_FILL };
enum { S12_F_NODUMP = 1, S12_F_BADDUMP = 2 };
// CPU board / machine config (MAME machine_config functions)
enum { S12_M_COH700, S12_M_COH700B, S12_M_COH716, S12_M_APLARAIL, S12_M_TECHNODR, S12_M_CDXA, S12_M_QUESTFF,
       S12_M_GOLGO13, S12_M_TRUCKK, S12_M_KARTDUEL };
// driver state class (MAME), decides banking and protection
enum { S12_S_BASE, S12_S_ALTBANK, S12_S_CDXA, S12_S_TEKTAGT, S12_S_PTBLANK2, S12_S_TECHNODR, S12_S_GOLGO13,
       S12_S_TRUCKK, S12_S_KARTDUEL };
enum { S12_G_NOT_WORKING = 1 };

typedef struct {
	uint8_t region, load, flags;
	uint32_t offset, size, crc; // FILL: crc holds the fill value
	const char *name;
} S12RomEntry;

typedef struct {
	const char *name, *parent, *title, *maker;
	int year;
	uint8_t machine, state;
	const char *inputs;
	int rot;
	int flags;
	const char *keycus;
	uint32_t main_size, bank_size, sub_size, c352_size;
	int rom_start, rom_count;
} S12Game;

#endif
