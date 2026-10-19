/*  ReSys12: executable memory for the R3000A dynamic recompiler (pcsx_rearmed new_dynarec) (see rs12_jit.h).
    GPL-2.0-or-later. */
#include "rs12_jit.h"

#include <stdint.h>
#include <string.h>

#ifdef __vita__
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

static SceUID s_uid = -1;
static void *s_base;
static size_t s_size;
static size_t s_used; // bump allocation (SH-2 blocks, then the SCSP DSP code)

int rs12_jit_code_reserve(size_t size)
{
	if (s_uid >= 0)
		return 0;
	size = (size + 0xFFFFF) & ~(size_t)0xFFFFF;
	SceUID uid = sceKernelAllocMemBlockForVM("rs12_r3k_code", size);
	if (uid < 0)
		return uid;
	void *base = NULL;
	int r = sceKernelGetMemBlockBase(uid, &base);
	if (r < 0) {
		sceKernelFreeMemBlock(uid);
		return r;
	}
	s_uid = uid;
	s_base = base;
	s_size = size;
	return 0;
}

void *rs12_jit_code_alloc(size_t size)
{
	if (s_uid < 0 && rs12_jit_code_reserve(size) != 0)
		return NULL;
	size = (size + 63) & ~(size_t)63;
	if (size > s_size - s_used)
		return NULL;
	void *p = (char *)s_base + s_used;
	s_used += size;
	return p;
}

void *rs12_jit_code_region(size_t *size)
{
	if (size)
		*size = s_size;
	return s_uid >= 0 ? s_base : NULL;
}

int rs12_jit_code_uid(void)
{
	return s_uid;
}

// pcsx_rearmed new_dynarec on Vita takes its translation cache block from the RetroArch bootstrap's getVMBlock();
// hand it the region reserved at startup (rs12_jit_code_reserve). The recompiler expects zeroed memory (its trampoline
// table at the end is searched for NULL slots; VM blocks are not cleared by the kernel, which aborted with "table full").
int getVMBlock(void)
{
	if (s_uid >= 0) {
		sceKernelOpenVMDomain();
		memset(s_base, 0, s_size);
		sceKernelSyncVMDomain(s_uid, s_base, s_size);
	}
	return s_uid;
}

int rs12_jit_keep_open = 1; // open the VM domain once and leave it open (no open/close per block)
// the domain is opened per thread (the sound thread writing the SCSP DSP code got a data abort while the
// emu thread had it open), so remember the threads that opened it
static SceUID s_open_tid[4];
static int s_open;

void rs12_jit_code_write_begin(void)
{
	if (rs12_jit_keep_open) {
		const SceUID tid = sceKernelGetThreadId();
		for (int i = 0; i < s_open; i++)
			if (s_open_tid[i] == tid)
				return;
		sceKernelOpenVMDomain();
		if (s_open < 4)
			s_open_tid[s_open++] = tid;
		return;
	}
	sceKernelOpenVMDomain();
}

void rs12_jit_code_write_end(void *p, size_t len)
{
	sceKernelSyncVMDomain(s_uid, p, len);
	if (!rs12_jit_keep_open)
		sceKernelCloseVMDomain();
}
void rs12_jit_code_sync(void *p, size_t len)
{
	sceKernelSyncVMDomain(s_uid, p, len);
}

/* ReSys12: close the VM domain left open and free the code block before the process exits */
void rs12_jit_shutdown(void)
{
	if (s_open) {
		sceKernelCloseVMDomain();
		s_open = 0;
	}
	if (s_uid >= 0) {
		sceKernelFreeMemBlock(s_uid);
		s_uid = -1;
		s_base = NULL;
		s_used = 0;
	}
}

#else
void rs12_jit_shutdown(void) {}
#include <sys/mman.h>

int rs12_jit_keep_open = 1; // code memory is always writable here

int rs12_jit_code_reserve(size_t size) { (void)size; return 0; }
void *rs12_jit_code_region(size_t *size) { if (size) *size = 0; return NULL; }
int rs12_jit_code_uid(void) { return -1; }

void *rs12_jit_code_alloc(size_t size)
{
	void *p = mmap(NULL, size, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_ANONYMOUS | MAP_PRIVATE, -1, 0);
	return p == MAP_FAILED ? NULL : p;
}

void rs12_jit_code_write_begin(void) {}

void rs12_jit_code_write_end(void *p, size_t len)
{
	__builtin___clear_cache((char *)p, (char *)p + len);
}
void rs12_jit_code_sync(void *p, size_t len)
{
	__builtin___clear_cache((char *)p, (char *)p + len);
}
#endif
