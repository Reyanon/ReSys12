cmake_minimum_required(VERSION 3.16)
project(resys12_harness C)
# PC harness: run N frames headless -> frame/audio hashes, PPM dumps.
# x86-64: interpreter only (deterministic reference).
# ARM Linux (tools/build_arm_harness.sh, run under qemu-arm): -DRS12_DYNAREC=ON adds the ari64 recompiler and the
# NEON GPU/GTE paths, i.e. the same CPU/GPU code as the Vita build, so it can be compared with the interpreter.
set(CMAKE_C_STANDARD 11)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
set(CMAKE_C_FLAGS_RELEASE "-O2 -g -fno-strict-aliasing")
option(RS12_DYNAREC "ari64 recompiler + NEON paths (ARM builds only, arm-linux.cmake)" OFF)
if(RS12_DYNAREC)
  enable_language(ASM)
endif()

set(RS12_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../..)
include(${RS12_ROOT}/src/psx/psx.cmake)

# C352: the real core once it exists, a silent stub until then
if(EXISTS ${RS12_ROOT}/src/sound/c352.c)
  set(RS12_C352_SOURCE ${RS12_ROOT}/src/sound/c352.c)
else()
  set(RS12_C352_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/c352_stub.c)
endif()

set(RS12_ARM_SOURCES "")
if(RS12_DYNAREC)
  set(RS12_ARM_SOURCES
    ${RS12_PSX}/plugins/gpu_neon/psx_gpu/psx_gpu_arm_neon.S
    ${RS12_PSX}/libpcsxcore/gte_arm.S
    ${RS12_PSX}/libpcsxcore/gte_nf_arm.S
    ${RS12_PSX}/libpcsxcore/gte_neon.S
    ${RS12_PSX}/libpcsxcore/gte_nf_neon.S
    ${RS12_PSX}/libpcsxcore/new_dynarec/new_dynarec.c
    ${RS12_PSX}/libpcsxcore/new_dynarec/pcsxmem.c
    ${RS12_PSX}/libpcsxcore/new_dynarec/emu_if.c
    ${RS12_PSX}/libpcsxcore/new_dynarec/linkage_arm.S
  )
endif()

add_library(rs12_core STATIC
  ${RS12_PSX_SOURCES}
  ${RS12_ARM_SOURCES}
  ${RS12_ROOT}/src/s12/s12.c
  ${RS12_ROOT}/src/s12/s12_sub.c
  ${RS12_ROOT}/src/s12/s12_jvs.c
  ${RS12_ROOT}/src/s12/s12_sound.c
  ${RS12_ROOT}/src/s12/s12_state.c
  ${RS12_ROOT}/src/h8/h8.c
  ${RS12_ROOT}/src/h8/h8_periph.c
  ${RS12_C352_SOURCE}
  ${RS12_ROOT}/src/romload/s12_rom.c
  ${RS12_ROOT}/src/romload/rs12_zip.c
  ${RS12_ROOT}/src/romload/junzip.c
)
target_include_directories(rs12_core PUBLIC ${RS12_PSX_INCLUDES} ${RS12_ROOT}/src/s12 ${RS12_ROOT}/src/romload
  ${RS12_ROOT}/src/h8 ${RS12_ROOT}/src/sound)
target_compile_options(rs12_core PRIVATE -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable)
if(RS12_DYNAREC)
  target_compile_definitions(rs12_core PUBLIC ${RS12_PSX_DEFINES} HAVE_NEON_ASM RS12_SUB_PROFILE RS12_HARNESS)
  set_source_files_properties(${RS12_PSX}/plugins/gpu_neon/psx_gpu_if.c PROPERTIES COMPILE_DEFINITIONS "NEON_BUILD")
  target_compile_options(rs12_core PRIVATE -marm)
else()
  target_compile_definitions(rs12_core PUBLIC ${RS12_PSX_DEFINES} DRC_DISABLE RS12_SUB_PROFILE RS12_H8_PCHIST RS12_HARNESS)
endif()

add_executable(harness main.c)
target_link_libraries(harness rs12_core z m)
if(RS12_DYNAREC)
  target_compile_definitions(harness PRIVATE RS12_HARNESS_DYNAREC=1)
  target_link_options(harness PRIVATE -static)
endif()
