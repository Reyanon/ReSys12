/*  ReSys12: executable memory for the R3000A dynamic recompiler (pcsx_rearmed new_dynarec).
    Vita: sceKernelAllocMemBlockForVM + Open/CloseVMDomain (writes must be bracketed).
    Other ARM Linux (qemu test harness): mmap RWX, writes need no bracketing.
    GPL-2.0-or-later. */
#ifndef RS12_JIT_H
#define RS12_JIT_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

// Size reserved for the R3000A recompiler (pcsx_rearmed struct ndrc_mem: 16MB translation cache incl. trampolines);
// rs12_heap_vita.c reserves it before the newlib heap
#define RS12_JIT_RESERVE_SIZE (16u << 20)

// Vita: call early (before the GPU allocations take the free memory) to reserve the code region.
// rs12_jit_code_alloc then hands out this reservation.
int rs12_jit_code_reserve(size_t size);
void *rs12_jit_code_alloc(size_t size);
// Reserved region (Vita), for the startup self test. NULL if nothing reserved
void *rs12_jit_code_region(size_t *size);
// Call before writing generated code (or any data inside the code region)
void rs12_jit_code_write_begin(void);
// Call after writing: makes [p, p+len) executable and coherent with the I-cache
void rs12_jit_code_write_end(void *p, size_t len);
// Makes [p, p+len) coherent without ending the write section (several ranges, then write_end on the last)
void rs12_jit_code_sync(void *p, size_t len);
void rs12_jit_shutdown(void); /* before the process exits */
// memory block id of the reserved region (pcsx_rearmed new_dynarec takes it through getVMBlock())
int rs12_jit_code_uid(void);
// code area of the H8 blocks (src/h8): Vita: the last RS12_JIT_H8_SIZE of the region (left out of the R3000A
// translation cache, TC_REDUCE_BYTES), usable once the recompiler has cleared the block; elsewhere mapped RWX
#define RS12_JIT_H8_SIZE (1u << 20)
void *rs12_jit_h8_region(size_t *size);

#ifdef __cplusplus
}
#endif

#endif
