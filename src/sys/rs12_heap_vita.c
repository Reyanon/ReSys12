/*  ReSys12: newlib heap setup for the Vita, replacing VitaSDK's lib_a-sbrk.o (same four entry points).
    The R3000A recompiler's translation cache is reserved *before* the heap, so it lands right after the executable:
    generated code calls helpers in .text with direct ARM branches (+-32MB), which fails when a large heap sits between
    them (pcsx_rearmed new_dynarec, INVALIDATE_USE_COND_CALL). RetroArch's Vita bootstrap does the same for its cores.
    GPL-2.0-or-later. */
#ifdef __vita__
#include <errno.h>
#include <reent.h>
#include <stddef.h>
#include <psp2/kernel/sysmem.h>
#include <psp2/kernel/threadmgr.h>

#include "rs12_jit.h"

extern int _newlib_heap_size_user __attribute__((weak));

static int s_heap_block = -1;
static unsigned s_heap_size;
static char *s_heap_base, *s_heap_end, *s_heap_cur;
static SceKernelLwMutexWork s_sbrk_mutex __attribute__((aligned(8)));

void *_sbrk_r(struct _reent *reent, ptrdiff_t incr)
{
	if (sceKernelLockLwMutex(&s_sbrk_mutex, 1, NULL) < 0)
		goto fail;
	if (!s_heap_base || s_heap_cur + incr >= s_heap_end) {
		sceKernelUnlockLwMutex(&s_sbrk_mutex, 1);
	fail:
		reent->_errno = ENOMEM;
		return (void *)-1;
	}
	char *prev = s_heap_cur;
	s_heap_cur += incr;
	sceKernelUnlockLwMutex(&s_sbrk_mutex, 1);
	return prev;
}

void _init_vita_heap(void)
{
	// translation cache first (see above); failure is reported later by the JIT self test
	rs12_jit_code_reserve(RS12_JIT_RESERVE_SIZE);

	if (sceKernelCreateLwMutex(&s_sbrk_mutex, "sbrk mutex", 0, 0, NULL) < 0)
		return;
	s_heap_size = &_newlib_heap_size_user ? (unsigned)_newlib_heap_size_user : 32u * 1024 * 1024;
	s_heap_block = sceKernelAllocMemBlock("Newlib heap", SCE_KERNEL_MEMBLOCK_TYPE_USER_RW, s_heap_size, NULL);
	if (s_heap_block < 0)
		return;
	if (sceKernelGetMemBlockBase(s_heap_block, (void **)&s_heap_base) < 0) {
		s_heap_base = NULL;
		return;
	}
	s_heap_end = s_heap_base + s_heap_size;
	s_heap_cur = s_heap_base;
}

void _free_vita_heap(void)
{
	if (s_heap_block >= 0) {
		sceKernelDeleteLwMutex(&s_sbrk_mutex);
		sceKernelFreeMemBlock(s_heap_block);
	}
	s_heap_block = -1;
	s_heap_base = s_heap_end = s_heap_cur = NULL;
}

int _get_vita_heap_size(void)
{
	return (int)s_heap_size;
}
#endif
