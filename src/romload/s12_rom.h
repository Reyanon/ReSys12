// ReSys12: System 12 ROM set loader (MAME set layout from s12_games.h, files found by CRC then name)
#ifndef S12_ROM_H
#define S12_ROM_H

#include <stdint.h>
#include "s12_types.h"

typedef struct {
	const S12Game *game;
	uint8_t *rgn[S12_RGN_COUNT];
	uint32_t rgn_size[S12_RGN_COUNT];
	int missing;   // files not found (NO_DUMP entries not counted)
	int by_name;   // files found by name only (CRC differs, e.g. a redump)
	int patched;   // board code patches applied to the program (s12_attach, once per load)
} S12Rom;

// index into s12_games, or -1
int s12_find_game(const char *name);
const S12Game *s12_game(int index);
int s12_game_count(void);

// Load set `name` from roms_dir/<name>.zip (+ parent zip). Returns 0 on success, negative on error.
// msg receives a short reason on failure.
int s12_rom_load(S12Rom *r, const char *roms_dir, const char *name, char *msg, int msg_len);
void s12_rom_free(S12Rom *r);

// Same, through a cache of the assembled regions in cache_dir/<name>.bin (built on first use, ~80MB for tektagt).
// The cache is keyed by the zip directories (file names, CRCs, sizes), so a changed zip rebuilds it.
// progress (optional): called with 0..1000 while loading.
int s12_rom_load_cached(S12Rom *r, const char *roms_dir, const char *cache_dir, const char *name, char *msg, int msg_len,
	void (*progress)(int permille));

#endif
