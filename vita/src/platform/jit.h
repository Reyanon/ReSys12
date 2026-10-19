#ifndef RS12_PLATFORM_JIT_H
#define RS12_PLATFORM_JIT_H

#include <stddef.h>
#include <stdint.h>

// R3000A recompiler code area: pcsx_rearmed new_dynarec struct ndrc_mem
// (16MB translation cache minus TC_REDUCE_BYTES 4KB + trampolines = 16MB). ReST-V saw ~17MB VM blocks fail.
#define RS12_JIT_CODE_SIZE (16u << 20)

// Allocate the code area at startup and check generate → run → overwrite → rerun on real hardware.
// Result in msg; returns 0 (success) or negative. The area is never freed (the recompiler uses it)
int rs12_jit_selftest(char *msg, size_t msg_len);

#endif
