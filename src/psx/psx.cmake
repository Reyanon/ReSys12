# pcsx_rearmed core subset used by ReSys12 (shared by the PC harness and the Vita build).
# Revision in PCSX_REARMED_REVISION.txt. Files changed for System 12 carry "ReSys12:" comments (GPL-2 section 2a).
set(RS12_PSX ${CMAKE_CURRENT_LIST_DIR})

set(RS12_PSX_SOURCES
  ${RS12_PSX}/libpcsxcore/r3000a.c
  ${RS12_PSX}/libpcsxcore/psxmem.c
  ${RS12_PSX}/libpcsxcore/psxinterpreter.c
  ${RS12_PSX}/libpcsxcore/psxhw.c
  ${RS12_PSX}/libpcsxcore/psxdma.c
  ${RS12_PSX}/libpcsxcore/psxcounters.c
  ${RS12_PSX}/libpcsxcore/psxevents.c
  ${RS12_PSX}/libpcsxcore/gte.c
  ${RS12_PSX}/libpcsxcore/gte_nf.c
  ${RS12_PSX}/libpcsxcore/gte_divider.c
  ${RS12_PSX}/libpcsxcore/mdec.c
  ${RS12_PSX}/libpcsxcore/gpu.c
  ${RS12_PSX}/plugins/gpulib/gpu.c
  ${RS12_PSX}/plugins/gpulib/vout_pl.c
  ${RS12_PSX}/plugins/gpulib/prim.c
  ${RS12_PSX}/plugins/gpu_neon/psx_gpu_if.c
  ${RS12_PSX}/shim/rs12_psx_glue.c
  ${RS12_PSX}/shim/rs12_gpu_bind.c
)

set(RS12_PSX_INCLUDES
  ${RS12_PSX}/libpcsxcore
  ${RS12_PSX}/include
  ${RS12_PSX}/shim
)

set(RS12_PSX_DEFINES
  P_HAVE_MMAP=0
  P_HAVE_POSIX_MEMALIGN=0
  DISABLE_MEM_LUTS=0
  GPU_NEON
  TEXTURE_CACHE_4BPP
  TEXTURE_CACHE_8BPP
  NDEBUG
)
