// ReSys12: System 12 save states. Layout follows pcsx_rearmed misc.c SaveState/LoadState for the PS1-derived part
// (RAM, hw registers, CPU registers, GPU registers + VRAM, root counters, MDEC), then the System 12 parts.
// GPL-2.0-or-later.
#include "s12_state.h"

#include <stdlib.h>
#include <string.h>

#include "psxcommon.h"
#include "psxhw.h"
#include "psxmem.h"
#include "psxevents.h"
#include "plugins.h"
#include "r3000a.h"
#include "mdec.h"
#include "gpu.h"
#include "s12.h"
#include "s12_sound.h"

#define STATE_MAGIC "RS12STA1"
#define VRAM_BYTES (1024 * 1024 * 2)

extern unsigned int frame_counter;
extern s32 psxRcntFreeze(void *f, s32 Mode);
uint32_t s12_board_state_size(void);
void s12_board_state_save(void *dst);
void s12_board_state_load(const void *src);
uint32_t s12_sub_state_size(void);
void s12_sub_state_save(void *dst);
void s12_sub_state_load(const void *src);

// ---- memory stream behind pcsx_rearmed's SaveFuncs (used by the *Freeze helpers) ----
typedef struct {
	uint8_t *buf;
	size_t size, cap, pos;
	int failed;
} MemStream;

static void *ms_open(const char *name, const char *mode) { (void)name; (void)mode; return NULL; }

static int ms_write(void *f, const void *p, u32 len)
{
	MemStream *s = (MemStream *)f;
	if (s->failed)
		return 0;
	if (s->pos + len > s->cap) {
		size_t cap = s->cap ? s->cap * 2 : (size_t)8 << 20;
		while (cap < s->pos + len)
			cap *= 2;
		uint8_t *b = (uint8_t *)realloc(s->buf, cap);
		if (!b) {
			s->failed = 1;
			return 0;
		}
		s->buf = b;
		s->cap = cap;
	}
	memcpy(s->buf + s->pos, p, len);
	s->pos += len;
	if (s->pos > s->size)
		s->size = s->pos;
	return (int)len;
}

static int ms_read(void *f, void *p, u32 len)
{
	MemStream *s = (MemStream *)f;
	if (s->failed || s->pos + len > s->size) {
		s->failed = 1;
		memset(p, 0, len);
		return 0;
	}
	memcpy(p, s->buf + s->pos, len);
	s->pos += len;
	return (int)len;
}

static long ms_seek(void *f, long offs, int whence)
{
	MemStream *s = (MemStream *)f;
	s->pos = whence == 0 ? (size_t)offs : s->pos + (size_t)offs;
	return (long)s->pos;
}

static void ms_close(void *f) { (void)f; }

static void use_mem_stream(void)
{
	SaveFuncs.open = ms_open;
	SaveFuncs.read = ms_read;
	SaveFuncs.write = ms_write;
	SaveFuncs.seek = ms_seek;
	SaveFuncs.close = ms_close;
}

// CPU state saved besides the psxRegisters prefix (fields after gteBusyCycle; pointers are never saved)
typedef struct {
	u32 gteBusyCycle, muldivBusyCycle, biuReg, gpuIdleAfter, gpu_sr, frame_counter, subCycle;
} CpuExtra;

static void put_block(MemStream *s, const void *p, u32 len)
{
	ms_write(s, &len, sizeof(len));
	ms_write(s, p, len);
}

uint8_t *s12_state_save(size_t *size)
{
	MemStream ms;
	memset(&ms, 0, sizeof(ms));
	use_mem_stream();
	psxCpu->Notify(R3000ACPU_NOTIFY_BEFORE_SAVE, NULL);

	char hdr[32];
	memset(hdr, 0, sizeof(hdr));
	memcpy(hdr, STATE_MAGIC, 8);
	strncpy(hdr + 8, s12.game ? s12.game->name : "", 23);
	ms_write(&ms, hdr, sizeof(hdr));

	ms_write(&ms, psxRegs.ptrs.psxM, PSX_RAM_SIZE);
	ms_write(&ms, psxRegs.ptrs.psxH, 0x10000);
	ms_write(&ms, &psxRegs, offsetof(psxRegisters, gteBusyCycle));
	CpuExtra cx = { psxRegs.gteBusyCycle, psxRegs.muldivBusyCycle, psxRegs.biuReg, psxRegs.gpuIdleAfter,
		HW_GPU_STATUS, frame_counter, psxRegs.subCycle };
	ms_write(&ms, &cx, sizeof(cx));

	GPUFreeze_t gh;
	uint16_t *vram = NULL;
	memset(&gh, 0, sizeof(gh));
	gh.ulFreezeVersion = 1;
	GPU_freeze(1, &gh, &vram);
	ms_write(&ms, &gh, sizeof(gh));
	ms_write(&ms, vram, VRAM_BYTES);

	psxHwFreeze(&ms, 1);
	psxRcntFreeze(&ms, 1);
	mdecFreeze(&ms, 1);

	// System 12 parts, each length-prefixed so a size change is caught on load
	uint32_t n = s12_board_state_size();
	uint8_t *tmp = (uint8_t *)malloc(n);
	if (tmp) {
		s12_board_state_save(tmp);
		put_block(&ms, tmp, n);
		free(tmp);
	} else
		ms.failed = 1;
	n = s12_sub_state_size();
	tmp = (uint8_t *)malloc(n);
	if (tmp) {
		s12_sub_state_save(tmp);
		put_block(&ms, tmp, n);
		free(tmp);
	} else
		ms.failed = 1;
	n = s12_sound_state_size();
	tmp = (uint8_t *)malloc(n);
	if (tmp) {
		s12_sound_state_save(tmp);
		put_block(&ms, tmp, n);
		free(tmp);
	} else
		ms.failed = 1;

	if (ms.failed) {
		free(ms.buf);
		return NULL;
	}
	*size = ms.size;
	return ms.buf;
}

static int get_block(MemStream *s, void (*load)(const void *), uint32_t expect)
{
	uint32_t len = 0;
	ms_read(s, &len, sizeof(len));
	if (s->failed || len != expect || s->pos + len > s->size)
		return -1;
	load(s->buf + s->pos);
	s->pos += len;
	return 0;
}

int s12_state_load(const uint8_t *buf, size_t size)
{
	MemStream ms;
	memset(&ms, 0, sizeof(ms));
	ms.buf = (uint8_t *)buf;
	ms.size = ms.cap = size;
	use_mem_stream();

	// validate before touching anything: header, set, and the sizes of the System 12 blocks at the end
	char hdr[32];
	ms_read(&ms, hdr, sizeof(hdr));
	if (ms.failed || memcmp(hdr, STATE_MAGIC, 8) != 0 || !s12.game || strncmp(hdr + 8, s12.game->name, 23) != 0)
		return -1;
	const size_t fixed = sizeof(hdr) + PSX_RAM_SIZE + 0x10000 + offsetof(psxRegisters, gteBusyCycle) + sizeof(CpuExtra) +
		sizeof(GPUFreeze_t) + VRAM_BYTES;
	if (size < fixed)
		return -1;
	{ // ReSys12: the blocks are raw structs, so a state of another build or CPU (64-bit PC harness) differs in size:
	  // refuse it here instead of half loading it
		size_t cur = 0;
		uint8_t *probe = s12_state_save(&cur);
		free(probe);
		if (!probe || cur != size)
			return -1;
		ms.pos = 0;
		use_mem_stream();
		ms_read(&ms, hdr, sizeof(hdr));
	}

	ms_read(&ms, psxRegs.ptrs.psxM, PSX_RAM_SIZE);
	ms_read(&ms, psxRegs.ptrs.psxH, 0x10000);
	ms_read(&ms, &psxRegs, offsetof(psxRegisters, gteBusyCycle));
	CpuExtra cx;
	ms_read(&ms, &cx, sizeof(cx));
	psxRegs.gteBusyCycle = cx.gteBusyCycle;
	psxRegs.muldivBusyCycle = cx.muldivBusyCycle;
	psxRegs.biuReg = cx.biuReg;
	psxRegs.gpuIdleAfter = cx.gpuIdleAfter;
	psxRegs.branching = 0;
	psxRegs.biosBranchCheck = ~0u;
	psxRegs.cpuInRecursion = 0;
	HW_GPU_STATUS = cx.gpu_sr;
	frame_counter = cx.frame_counter;

	GPUFreeze_t gh;
	uint16_t *vram = NULL;
	ms_read(&ms, &gh, sizeof(gh));
	GPU_freeze(0, &gh, &vram);
	ms_read(&ms, vram, VRAM_BYTES);
	gpuSyncPluginSR();

	psxHwFreeze(&ms, 0);
	psxRcntFreeze(&ms, 0);
	mdecFreeze(&ms, 0);

	int r = get_block(&ms, s12_board_state_load, s12_board_state_size());
	if (!r)
		r = get_block(&ms, s12_sub_state_load, s12_sub_state_size());
	if (!r)
		r = get_block(&ms, s12_sound_state_load, s12_sound_state_size());

	events_restore();
	psxCpu->Notify(R3000ACPU_NOTIFY_AFTER_LOAD_STATE, NULL);
	psxRegs.subCycle = cx.subCycle; // the interpreter's fractional cycle (cleared by the notify above)
	return r || ms.failed ? -2 : 0;
}
