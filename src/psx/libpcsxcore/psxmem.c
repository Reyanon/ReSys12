/***************************************************************************
 *   Copyright (C) 2007 Ryan Schultz, PCSX-df Team, PCSX team              *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) any later version.                                   *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program; if not, write to the                         *
 *   Free Software Foundation, Inc.,                                       *
 *   51 Franklin Street, Fifth Floor, Boston, MA 02111-1307 USA.           *
 ***************************************************************************/

/*
* PSX memory functions.
*/

// TODO: Implement caches & cycle penalty.

#include <stdlib.h>
#include <stdio.h>
#include <assert.h>
#include "psxmem.h"
#include "psxmem_map.h"
#include "r3000a.h"
#include "psxhw.h"
//#include "debug.h"
#define DebugCheckBP(...)

#include "lightrec/mem.h"
#include "memmap.h"
#include "compiler_features.h"
#include "s12_bus.h" // ReSys12: System 12 board I/O (0x1f000000-0x1fffffff outside RAM/scratchpad/hw regs)

#ifdef USE_LIBRETRO_VFS
#include <streams/file_stream_transforms.h>
#endif

#ifndef MAP_ANONYMOUS
#define MAP_ANONYMOUS MAP_ANON
#endif

static void * psxMapDefault(unsigned long addr, size_t size,
			    enum psxMapTag tag, int *can_retry_addr)
{
	void *ptr;
#if !P_HAVE_MMAP
	*can_retry_addr = 0;
	ptr = calloc(1, size);
	return ptr ? ptr : MAP_FAILED;
#else
	int flags = MAP_PRIVATE | MAP_ANONYMOUS;

	*can_retry_addr = 1;
	ptr = mmap((void *)(uintptr_t)addr, size,
		    PROT_READ | PROT_WRITE, flags, -1, 0);
#ifdef MADV_HUGEPAGE
	if (size >= 2*1024*1024) {
		if (ptr != MAP_FAILED && ((uintptr_t)ptr & (2*1024*1024 - 1))) {
			// try to manually realign assuming decreasing addr alloc
			munmap(ptr, size);
			addr = (uintptr_t)ptr & ~(2*1024*1024 - 1);
			ptr = mmap((void *)(uintptr_t)addr, size,
				PROT_READ | PROT_WRITE, flags, -1, 0);
		}
		if (ptr != MAP_FAILED)
			madvise(ptr, size, MADV_HUGEPAGE);
	}
#endif
	return ptr;
#endif
}

static void psxUnmapDefault(void *ptr, size_t size, enum psxMapTag tag)
{
#if !P_HAVE_MMAP
	free(ptr);
#else
	munmap(ptr, size);
#endif
}

void *(*psxMapHook)(unsigned long addr, size_t size,
		enum psxMapTag tag, int *can_retry_addr) = psxMapDefault;
void (*psxUnmapHook)(void *ptr, size_t size,
		     enum psxMapTag tag) = psxUnmapDefault;

void *psxMap(unsigned long addr, size_t size, int is_fixed,
		enum psxMapTag tag)
{
	int try_, can_retry_addr = 0;
	void *ret = MAP_FAILED;

	for (try_ = 0; try_ < 3; try_++)
	{
		if (ret != MAP_FAILED)
			psxUnmap(ret, size, tag);
		ret = psxMapHook(addr, size, tag, &can_retry_addr);
		if (ret == MAP_FAILED)
			return MAP_FAILED;

		if (addr != 0 && ret != (void *)(uintptr_t)addr) {
			SysMessage("psxMap: tried to map @%08lx, got %p\n",
				addr, ret);
			if (is_fixed) {
				psxUnmap(ret, size, tag);
				return MAP_FAILED;
			}

			if (can_retry_addr && ((addr ^ (uintptr_t)ret) & ~0xff000000l)) {
				unsigned long mask;

				// try to use similarly aligned memory instead
				// (recompiler prefers this)
				mask = try_ ? 0xffff : 0xffffff;
				addr = ((uintptr_t)ret + mask) & ~mask;
				continue;
			}
		}
		break;
	}

	return ret;
}

void psxUnmap(void *ptr, size_t size, enum psxMapTag tag)
{
	psxUnmapHook(ptr, size, tag);
}

/*  Playstation Memory Map (from Playstation doc by Joshua Walker)
0x0000_0000-0x0000_ffff		Kernel (64K)
0x0001_0000-0x001f_ffff		User Memory (1.9 Meg)

0x1f00_0000-0x1f00_ffff		Parallel Port (64K)

0x1f80_0000-0x1f80_03ff		Scratch Pad (1024 bytes)

0x1f80_1000-0x1f80_2fff		Hardware Registers (8K)

0x1fc0_0000-0x1fc7_ffff		BIOS (512K)

0x8000_0000-0x801f_ffff		Kernel and User Memory Mirror (2 Meg) Cached
0x9fc0_0000-0x9fc7_ffff		BIOS Mirror (512K) Cached

0xa000_0000-0xa01f_ffff		Kernel and User Memory Mirror (2 Meg) Uncached
0xbfc0_0000-0xbfc7_ffff		BIOS Mirror (512K) Uncached
*/

static int psxMemInitMap(void)
{
	u8 *ptr;

	// ReSys12: 4MB RAM (+64KB parallel port area after it, as before)
	ptr = psxMap(0x80000000, PSX_RAM_SIZE + 0x10000, 1, MAP_TAG_RAM);
	if (ptr == MAP_FAILED)
		ptr = psxMap(0x77000000, PSX_RAM_SIZE + 0x10000, 0, MAP_TAG_RAM);
	if (ptr == MAP_FAILED) {
		SysMessage("mapping main RAM failed");
		return -1;
	}
	psxRegs.ptrs.psxM = ptr;
	psxRegs.ptrs.psxP = ptr + PSX_RAM_SIZE;

	ptr = psxMap(0x1f800000, 0x10000, 0, MAP_TAG_OTHER);
	if (ptr == MAP_FAILED) {
		SysMessage("Error allocating psxH");
		return -1;
	}
	psxRegs.ptrs.psxH = ptr;

	ptr = psxMap(0x1fc00000, PSX_ROM_SIZE, 0, MAP_TAG_OTHER); // ReSys12: 4MB program flash
	if (ptr == MAP_FAILED) {
		SysMessage("Error allocating psxR");
		return -1;
	}
	psxRegs.ptrs.psxR = ptr;

	return 0;
}

static void psxMemFreeMap(void)
{
	if (psxRegs.ptrs.psxM) psxUnmap(psxRegs.ptrs.psxM, PSX_RAM_SIZE + 0x10000, MAP_TAG_RAM);
	if (psxRegs.ptrs.psxH) psxUnmap(psxRegs.ptrs.psxH, 0x10000, MAP_TAG_OTHER);
	if (psxRegs.ptrs.psxR) psxUnmap(psxRegs.ptrs.psxR, PSX_ROM_SIZE, MAP_TAG_OTHER);
	psxRegs.ptrs.psxM = psxRegs.ptrs.psxH = psxRegs.ptrs.psxR = NULL;
	psxRegs.ptrs.psxP = NULL;
}

static void lutMap(uintptr_t *lut, u8 *mem, u32 size, u32 start, u32 end)
{
	u32 i;
	assert((size  & ((1u << PSXM_SHIFT) - 1)) == 0);
	assert((start & ((1u << PSXM_SHIFT) - 1)) == 0);
	assert((end   & ((1u << PSXM_SHIFT) - 1)) == 0);

	for (i = start; i < end; i += (1u << PSXM_SHIFT))
		lut[i >> PSXM_SHIFT] = (uintptr_t)mem - (i & ~(size - 1));
}

// shouldn't this affect reads, more mirrors?
static void mapRam(int isMapped)
{
	//uintptr_t *rLUT = psxRegs.ptrs.memRLUT;
	uintptr_t *wLUT = psxRegs.ptrs.memWLUT;

	if (isMapped) {
		u8 *ram = psxRegs.ptrs.psxM;
		u32 i;
		for (i = 0; i < 0x800000; i += PSX_RAM_SIZE) { // ReSys12: 4MB mirrors
			lutMap(wLUT, ram, PSX_RAM_SIZE, 0x00000000u + i, 0x00000000u + PSX_RAM_SIZE + i);
			lutMap(wLUT, ram, PSX_RAM_SIZE, 0x80000000u + i, 0x80000000u + PSX_RAM_SIZE + i);
			lutMap(wLUT, ram, PSX_RAM_SIZE, 0xa0000000u + i, 0xa0000000u + PSX_RAM_SIZE + i);
		}
	}
	else {
		size_t len = (PSX_RAM_SIZE >> PSXM_SHIFT) * sizeof(wLUT[0]);
		//memset(rLUT + (0x00000000 >> PSXM_SHIFT), INVALID_PTR_VAL, len);
		//memset(rLUT + (0x80000000 >> PSXM_SHIFT), INVALID_PTR_VAL, len);
		//memset(rLUT + (0xa0000000 >> PSXM_SHIFT), INVALID_PTR_VAL, len);
		memset(wLUT + (0x00000000 >> PSXM_SHIFT), INVALID_PTR_VAL, len);
		memset(wLUT + (0x80000000 >> PSXM_SHIFT), INVALID_PTR_VAL, len);
		//memset(wLUT + (0xa0000000 >> PSXM_SHIFT), INVALID_PTR_VAL, len);
	}
}

int psxMemInit(void)
{
	size_t table_size = 1ul << (32 - PSXM_SHIFT);
	uintptr_t *memRLUT;
	uintptr_t *memWLUT;
	unsigned int i;
	int ret;

	if (LIGHTREC_CUSTOM_MAP)
		ret = lightrec_init_mmap();
	else
		ret = psxMemInitMap();
	if (ret) {
		if (LIGHTREC_CUSTOM_MAP)
			SysMessage("lightrec_init_mmap failed");
		psxMemShutdown();
		return -1;
	}

	if (DISABLE_MEM_LUTS)
		return 0;

	memRLUT = malloc(table_size * sizeof(memRLUT[0]) * 2);
	if (memRLUT == NULL) {
		SysMessage("Error allocating psxMem LUTs");
		psxMemShutdown();
		return -1;
	}

	assert((s8)INVALID_PTR_VAL == INVALID_PTR_VAL);
	memset(memRLUT, INVALID_PTR_VAL, table_size * sizeof(memRLUT[0]) * 2);
	memWLUT = memRLUT + table_size;
	psxRegs.ptrs.memRLUT = memRLUT;
	psxRegs.ptrs.memWLUT = memWLUT;

	// ram
	for (i = 0; i < 0x800000; i += PSX_RAM_SIZE) { // ReSys12: 4MB mirrors
		u8 *ram = psxRegs.ptrs.psxM;
		lutMap(memRLUT, ram, PSX_RAM_SIZE, 0x00000000u + i, 0x00000000u + PSX_RAM_SIZE + i);
		lutMap(memRLUT, ram, PSX_RAM_SIZE, 0x80000000u + i, 0x80000000u + PSX_RAM_SIZE + i);
		lutMap(memRLUT, ram, PSX_RAM_SIZE, 0xa0000000u + i, 0xa0000000u + PSX_RAM_SIZE + i);
	}
	mapRam(1);

	// ReSys12: 4MB program flash at 0x1fc00000 (System 12 boots from it; no BIOS)
	lutMap(memRLUT, psxRegs.ptrs.psxR, PSX_ROM_SIZE, 0x1fc00000u, 0x1fc00000u + PSX_ROM_SIZE);
	lutMap(memRLUT, psxRegs.ptrs.psxR, PSX_ROM_SIZE, 0x9fc00000u, 0x9fc00000u + PSX_ROM_SIZE);
	lutMap(memRLUT, psxRegs.ptrs.psxR, PSX_ROM_SIZE, 0xbfc00000u, 0xbfc00000u + PSX_ROM_SIZE);

	// Don't allow writes to PIO Expansion region (psxP) to take effect.
	// NOTE: Not sure if this is needed to fix any games but seems wise,
	//       seeing as some games do read from PIO as part of copy-protection
	//       check. (See fix in psxMemReset() regarding psxP region reads).
	//memWLUT[0x1f000000 >> PSXM_SHIFT] = INVALID_PTR_VAL;

	return 0;
}

void psxMemReset() {
	memset(psxRegs.ptrs.psxP, 0xff, 0x00010000);

	if (!DISABLE_MEM_LUTS)
		mapRam(1);

	// ReSys12: no PS1 BIOS. The board code (src/s12) fills psxR with the program flash and sets up its own mappings.
	Config.HLE = FALSE;
	s12_mem_reset();
}

void new_dyna_pcsx_mem_map_read(u32 phys, u32 size, void *mem);

// ReSys12: map (or unmap with mem == NULL) a read-only window of `size` bytes at physical `phys` in all three
// segments (kuseg/kseg0/kseg1). Used for the System 12 banked ROM window. Granularity 512KB.
void psxMemMapRead(u32 phys, u32 size, u8 *mem)
{
	uintptr_t *lut = psxRegs.ptrs.memRLUT;
	static const u32 segs[3] = { 0x00000000u, 0x80000000u, 0xa0000000u };
	u32 s, i;
#ifndef DRC_DISABLE
	new_dyna_pcsx_mem_map_read(phys, size, mem); // same window in the recompiler's tables
#endif
	if (DISABLE_MEM_LUTS || !lut)
		return;
	for (s = 0; s < 3; s++)
		for (i = 0; i < size; i += 1u << PSXM_SHIFT) {
			u32 a = segs[s] + phys + i;
			lut[a >> PSXM_SHIFT] = mem ? (uintptr_t)(mem + i) - a : (uintptr_t)INVALID_PTR_VAL;
		}
}

void psxMemShutdown() {
	if (LIGHTREC_CUSTOM_MAP)
		lightrec_free_mmap();
	else
		psxMemFreeMap();

	free(psxRegs.ptrs.memRLUT);
	psxRegs.ptrs.memRLUT = NULL;
	psxRegs.ptrs.memWLUT = NULL;
}

void psxMemOnIsolate(int enable)
{
	if (!DISABLE_MEM_LUTS) {
		mapRam(!enable);
	}

	psxCpu->Notify(enable ? R3000ACPU_NOTIFY_CACHE_ISOLATED
			: R3000ACPU_NOTIFY_CACHE_UNISOLATED, NULL);
}

u8 psxMemRead8(psxRegisters *regs, u32 mem) {
	u8 *p;
	u32 t;

	if (likely(psxm_(&p, regs, mem, 0)))
		return *p;

	t = mem >> 16;
	if (t == 0x1f80 || t == 0x9f80 || t == 0xbf80) {
		if ((mem & 0xffff) < 0x400)
			return regs->ptrs.psxH[mem & 0x3ff];
		else
			return psxHwRead8(regs, mem);
	}
	if (S12_IS_BOARD(mem))
		return s12_read8(mem & 0x1fffffff);
	return 0xFF;
}

u16 psxMemRead16(psxRegisters *regs, u32 mem) {
	u8 *p;
	u32 t;

	if (likely(psxm_(&p, regs, mem, 0)))
		return SWAPu16(*(u16 *)p);

	t = mem >> 16;
	if (t == 0x1f80 || t == 0x9f80 || t == 0xbf80) {
		if ((mem & 0xffff) < 0x400)
			return SWAPu16(*(u16 *)(regs->ptrs.psxH + (mem & 0x3fe)));
		else
			return psxHwRead16(regs, mem);
	}
	if (S12_IS_BOARD(mem))
		return s12_read16(mem & 0x1fffffff);
	return 0xFFFF;
}

u32 psxMemRead32(psxRegisters *regs, u32 mem) {
	u8 *p;
	u32 t;

	if (likely(psxm_(&p, regs, mem, 0)))
		return SWAPu32(*(u32 *)p);

	t = mem >> 16;
	if (t == 0x1f80 || t == 0x9f80 || t == 0xbf80) {
		if ((mem & 0xffff) < 0x400)
			return SWAPu32(*(u32 *)(regs->ptrs.psxH + (mem & 0x3fc)));
		else
			return psxHwRead32(regs, mem);
	}
	if (S12_IS_BOARD(mem))
		return s12_read32(mem & 0x1fffffff);
	if (mem == 0xfffe0130)
		return regs->biuReg;
	return 0xFFFFFFFF;
}

#ifdef RS12_HARNESS
// ReSys12 (diagnostics, PC harness RS12_WATCH=lo-hi): CPU stores into a RAM range, first time per pc
u32 rs12_watch_lo = 1, rs12_watch_hi = 0, rs12_watch_from;
extern unsigned int frame_counter;
static void rs12_watch(psxRegisters *regs, u32 mem, u32 value, int size)
{
	static u32 seen[256];
	static int nseen;
	const u32 a = mem & 0x1fffffff;
	if (a < rs12_watch_lo || a > rs12_watch_hi || frame_counter < rs12_watch_from)
		return;
	for (int i = 0; i < nseen; i++)
		if (seen[i] == regs->pc)
			return;
	if (nseen < 256)
		seen[nseen++] = regs->pc;
	if (nseen == 1 && getenv("RS12_WATCH_DUMP")) { // RAM at the first hit
		FILE *f = fopen(getenv("RS12_WATCH_DUMP"), "wb");
		if (f) {
			fwrite(regs->ptrs.psxM, 1, PSX_RAM_SIZE, f);
			fclose(f);
		}
	}
	fprintf(stderr, "watch w%d %08x = %08x @pc %08x a0 %08x a1 %08x t1 %08x ra %08x\n", size, mem, value, regs->pc,
		regs->GPR.n.a0, regs->GPR.n.a1, regs->GPR.n.t1, regs->GPR.n.ra);
}
#endif

void psxMemWrite8(psxRegisters *regs, u32 mem, u32 value) {
	u8 *p;
	u32 t;

#ifdef RS12_HARNESS
	rs12_watch(regs, mem, value, 8);
#endif
	if (likely(psxm_(&p, regs, mem, 1))) {
		*p = value;
#ifndef DRC_DISABLE
		psxCpu->Clear(mem & ~3, 1);
#endif
		return;
	}

	t = mem >> 16;
	if (t == 0x1f80 || t == 0x9f80 || t == 0xbf80) {
		if ((mem & 0xffff) < 0x400)
			regs->ptrs.psxH[mem & 0x3ff] = value;
		else
			psxHwWrite8(regs, mem, value);
		return;
	}
	if (S12_IS_BOARD(mem)) {
		s12_write8(mem & 0x1fffffff, value);
		return;
	}
	log_unhandled("unhandled w8  %08x %08x @%08x\n", mem, value, regs->pc);
}

void psxMemWrite16(psxRegisters *regs, u32 mem, u32 value) {
	u8 *p;
	u32 t;

#ifdef RS12_HARNESS
	rs12_watch(regs, mem, value, 16);
#endif
	if (likely(psxm_(&p, regs, mem, 1))) {
		*(u16 *)p = SWAPu16(value);
#ifndef DRC_DISABLE
		psxCpu->Clear(mem & ~3, 1);
#endif
		return;
	}

	t = mem >> 16;
	if (t == 0x1f80 || t == 0x9f80 || t == 0xbf80) {
		if ((mem & 0xffff) < 0x400)
			*(u16 *)(regs->ptrs.psxH + (mem & 0x3fe)) = SWAPu16(value);
		else
			psxHwWrite16(regs, mem, value);
		return;
	}
	if (S12_IS_BOARD(mem)) {
		s12_write16(mem & 0x1fffffff, value);
		return;
	}
	log_unhandled("unhandled w16 %08x %08x @%08x\n", mem, value, regs->pc);
}

void psxMemWrite32(psxRegisters *regs, u32 mem, u32 value) {
	u8 *p;
	u32 t;

#ifdef RS12_HARNESS
	rs12_watch(regs, mem, value, 32);
#endif
	if (likely(psxm_(&p, regs, mem, 1))) {
		*(u32 *)p = SWAPu32(value);
#ifndef DRC_DISABLE
		psxCpu->Clear(mem & ~3, 1);
#endif
		return;
	}

	t = mem >> 16;
	if (t == 0x1f80 || t == 0x9f80 || t == 0xbf80) {
		if ((mem & 0xffff) < 0x400)
			*(u32 *)(regs->ptrs.psxH + (mem & 0x3fc)) = SWAPu32(value);
		else
			psxHwWrite32(regs, mem, value);
		return;
	}
	if (S12_IS_BOARD(mem)) {
		s12_write32(mem & 0x1fffffff, value);
		return;
	}
	if (mem == 0xfffe0130) {
		regs->biuReg = value;
		return;
	}
	if (!(regs->CP0.n.SR & ((1u << 16))))
		log_unhandled("unhandled w32 %08x %08x @%08x\n", mem, value, regs->pc);
}
