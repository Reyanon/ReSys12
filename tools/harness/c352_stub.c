// ReSys12 PC harness: placeholder C352 used until src/sound/c352.c exists (reads 0, ignores writes, renders silence).
#include <string.h>

#include "c352.h"

void c352_init(const uint8_t *rom, uint32_t rom_size, uint32_t clock, int divider)
{
	(void)rom;
	(void)rom_size;
	(void)clock;
	(void)divider;
}

void c352_reset(void) {}

uint32_t c352_sample_rate(void) { return 88200; }

uint16_t c352_read(uint32_t offset)
{
	(void)offset;
	return 0;
}

void c352_write(uint32_t offset, uint16_t data)
{
	(void)offset;
	(void)data;
}

void c352_render(int16_t *out, int n) { memset(out, 0, (size_t)n * 2 * sizeof(int16_t)); }

uint32_t c352_state_size(void) { return 0; }

void c352_state_save(void *dst) { (void)dst; }

void c352_state_load(const void *src) { (void)src; }
