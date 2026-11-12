cmake_minimum_required(VERSION 3.16)

if(NOT DEFINED CMAKE_TOOLCHAIN_FILE)
  if(DEFINED ENV{VITASDK})
    set(CMAKE_TOOLCHAIN_FILE "$ENV{VITASDK}/share/vita.toolchain.cmake" CACHE PATH "toolchain file")
  else()
    message(FATAL_ERROR "VITASDK is not set")
  endif()
endif()

project(resys12 C CXX ASM)
# Vita build. Uses the shared core in the repository root (src/). Build: tools/build.sh vita (-> build-vita/)
set(RS12_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/..)
include("$ENV{VITASDK}/share/vita.cmake" REQUIRED)

set(VITA_APP_NAME "ReSys12")
set(VITA_TITLEID  "RSYS00012")
set(VITA_VERSION  "00.01")

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

set(RS12_ARCH_FLAGS "-mcpu=cortex-a9 -mfpu=neon -mfloat-abi=hard")
set(CMAKE_C_FLAGS   "${CMAKE_C_FLAGS} ${RS12_ARCH_FLAGS} -Wall")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${RS12_ARCH_FLAGS} -Wall")
set(CMAKE_C_FLAGS_RELEASE   "-O3 -ffast-math -DNDEBUG")
set(CMAKE_CXX_FLAGS_RELEASE "-O3 -ffast-math -DNDEBUG")
set(CMAKE_C_FLAGS_DEBUG   "-O0 -g")
set(CMAKE_CXX_FLAGS_DEBUG "-O0 -g")

# UDP logger: on during development (off for release VPKs). Target PC comes from ux0:data/resys12/loghost
option(RS12_LOG "include the UDP logger" ON)
set(RS12_ARM_MHZ 444 CACHE STRING "ARM clock requested at startup")

# emulator core: pcsx_rearmed subset + System 12 board (same sources as the PC harness)
include(${RS12_ROOT}/src/psx/psx.cmake)
add_library(rs12_core STATIC
  ${RS12_PSX_SOURCES}
  ${RS12_PSX}/plugins/gpu_neon/psx_gpu/psx_gpu_arm_neon.S
  ${RS12_PSX}/libpcsxcore/gte_arm.S
  ${RS12_PSX}/libpcsxcore/gte_nf_arm.S
  ${RS12_PSX}/libpcsxcore/gte_neon.S
  ${RS12_PSX}/libpcsxcore/gte_nf_neon.S
  # ari64 R3000A -> ARMv7 recompiler (assem_arm.c is included by new_dynarec.c)
  ${RS12_PSX}/libpcsxcore/new_dynarec/new_dynarec.c
  ${RS12_PSX}/libpcsxcore/new_dynarec/pcsxmem.c
  ${RS12_PSX}/libpcsxcore/new_dynarec/emu_if.c
  ${RS12_PSX}/libpcsxcore/new_dynarec/linkage_arm.S
  # threaded GPU (gpulib gpu_async on CPU2)
  ${RS12_PSX}/plugins/gpulib/gpu_async.c
  ${RS12_PSX}/shim/rs12_threads_vita.c
  ${RS12_ROOT}/src/s12/s12.c
  ${RS12_ROOT}/src/s12/s12_sub.c
  ${RS12_ROOT}/src/s12/s12_jvs.c
  ${RS12_ROOT}/src/s12/s12_hle.c
  ${RS12_ROOT}/src/s12/s12_sound.c
  ${RS12_ROOT}/src/s12/s12_state.c
  ${RS12_ROOT}/src/h8/h8.c
  ${RS12_ROOT}/src/h8/h8_periph.c
  ${RS12_ROOT}/src/sound/c352.c
  ${RS12_ROOT}/src/romload/s12_rom.c
  ${RS12_ROOT}/src/romload/rs12_zip.c
  ${RS12_ROOT}/src/romload/junzip.c
)
target_include_directories(rs12_core PUBLIC ${RS12_PSX_INCLUDES} ${RS12_ROOT}/src/s12 ${RS12_ROOT}/src/romload ${RS12_ROOT}/src/sound
  ${RS12_ROOT}/src/h8)
# pcsx_rearmed's Vita shims (sys/mman.h etc.), as -Ifrontend/vita in Makefile.libretro
target_include_directories(rs12_core PRIVATE ${RS12_PSX}/frontend/vita)
# ari64 recompiler on. HAVE_NEON_ASM: NEON GTE paths, as Makefile.libretro platform=vita
if("RS12_NDRC_PROFILE" IN_LIST RS12_EXTRA_DEFS)
  # measurement builds: the sampler sees which compiler function runs (new_dynarec.c __cyg_profile_func_enter)
  set_property(SOURCE ${RS12_PSX}/libpcsxcore/new_dynarec/new_dynarec.c APPEND PROPERTY COMPILE_OPTIONS "-finstrument-functions;-finstrument-functions-exclude-function-list=emit_,output_w32,genimm,rd_rn_rm,get_reg,get_reg_w")
endif()
target_compile_definitions(rs12_core PUBLIC ${RS12_PSX_DEFINES} VITA HAVE_NEON_ASM USE_ASYNC_GPU NDRC_THREAD ${RS12_EXTRA_DEFS})
# gpu_neon with the hand-written NEON assembly renderer (psx_gpu_arm_neon.S; the SIMD intrinsics variant duplicates it)
set_source_files_properties(${RS12_PSX}/plugins/gpu_neon/psx_gpu_if.c PROPERTIES COMPILE_DEFINITIONS "NEON_BUILD")
# -marm: pcsx_rearmed inline asm is ARM-only (as in Makefile.libretro platform=vita)
target_compile_options(rs12_core PRIVATE -marm -fno-optimize-sibling-calls -Wno-unused-function -Wno-unused-variable -Wno-unused-but-set-variable)

add_executable(${PROJECT_NAME}
  src/frontend/main.c
  src/frontend/audio.c
  src/frontend/resample.c
  src/frontend/ime.c
  src/frontend/ui.c
  src/frontend/menu.c
  src/frontend/update.c
  src/frontend/touch.c
  src/common/re_pad.c
  src/video/video_gl.c
  src/video/neon.c
  src/platform/clock.c
  src/platform/jit.c
  src/platform/log.c
  ${RS12_ROOT}/src/sys/rs12_jit.c
  ${RS12_ROOT}/src/sys/rs12_heap_vita.c
)

target_include_directories(${PROJECT_NAME} PRIVATE src ${RS12_ROOT}/src/sys ${CMAKE_CURRENT_BINARY_DIR})
# Version (About page, same v0.1.x scheme as ReST-V): rs12_version.h is regenerated on every build (tools/git_version.cmake)
add_custom_target(rs12_version
  COMMAND ${CMAKE_COMMAND} -DSRC=${RS12_ROOT} -DOUT=${CMAKE_CURRENT_BINARY_DIR}/rs12_version.h -P ${RS12_ROOT}/tools/git_version.cmake
  BYPRODUCTS ${CMAKE_CURRENT_BINARY_DIR}/rs12_version.h
  VERBATIM)
add_dependencies(${PROJECT_NAME} rs12_version)
target_compile_definitions(${PROJECT_NAME} PRIVATE RS12_ARM_MHZ=${RS12_ARM_MHZ})
if(RS12_LOG)
  target_compile_definitions(${PROJECT_NAME} PRIVATE RS12_ENABLE_LOG)
endif()
# debug-only tools (FPS with decimals and the frame number). Release builds: -DRS12_DEBUG_TOOLS=OFF
option(RS12_DEBUG_TOOLS "Include debug-only tools" ON)
if(RS12_DEBUG_TOOLS)
  target_compile_definitions(${PROJECT_NAME} PRIVATE RS12_DEBUG_TOOLS)
endif()
# bytes added to the code segment when vita-elf-create finds no room for its data before the data segment (set by
# tools/build.sh, which retries with it)
set(RS12_TEXT_PAD 0 CACHE STRING "Padding at the end of the code segment")
target_compile_definitions(${PROJECT_NAME} PRIVATE RS12_TEXT_PAD=${RS12_TEXT_PAD})

# kernel helper module (PS button, update install), in the VPK as module/rs12_kctrl.skprx (same as ReST-V). Its user stubs
# are made here directly (vita_create_stubs runs make on an unquoted path, and this tree's path has a space)
add_executable(rs12_kctrl kmod/rs12_kctrl.c)
set_target_properties(rs12_kctrl PROPERTIES LINK_FLAGS "-nostdlib")
target_compile_options(rs12_kctrl PRIVATE -fno-builtin)
target_link_libraries(rs12_kctrl SceCtrlForDriver_stub SceThreadmgrForDriver_stub SceIofilemgrForDriver_stub SceSysmemForDriver_stub
  taihenForKernel_stub taihenModuleUtils_stub)
# (vita_create_self splits its flags on spaces, so the export config path cannot be passed through it)
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/rs12_kctrl.skprx"
  COMMAND vita-elf-create -e "${CMAKE_SOURCE_DIR}/kmod/exports.yml" $<TARGET_FILE:rs12_kctrl> "${CMAKE_BINARY_DIR}/rs12_kctrl.velf"
  COMMAND vita-make-fself -c "${CMAKE_BINARY_DIR}/rs12_kctrl.velf" "${CMAKE_BINARY_DIR}/rs12_kctrl.skprx"
  DEPENDS rs12_kctrl "${CMAKE_SOURCE_DIR}/kmod/exports.yml"
  VERBATIM)
add_custom_target(kctrl_skprx ALL DEPENDS "${CMAKE_BINARY_DIR}/rs12_kctrl.skprx")
set(RS12_KSTUB_DIR "${CMAKE_BINARY_DIR}/kctrl_stubs")
set(RS12_KSTUB "${RS12_KSTUB_DIR}/librs12_kctrl_v5_stub_weak.a")
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/kctrl.yml"
  COMMAND vita-elf-export kernel $<TARGET_FILE:rs12_kctrl> "${CMAKE_SOURCE_DIR}/kmod/exports.yml" "${CMAKE_BINARY_DIR}/kctrl.yml"
  DEPENDS rs12_kctrl "${CMAKE_SOURCE_DIR}/kmod/exports.yml"
  VERBATIM)
add_custom_command(OUTPUT "${RS12_KSTUB}"
  COMMAND vita-libs-gen "${CMAKE_BINARY_DIR}/kctrl.yml" "${RS12_KSTUB_DIR}"
  COMMAND make -C "${RS12_KSTUB_DIR}"
  DEPENDS "${CMAKE_BINARY_DIR}/kctrl.yml"
  VERBATIM)
add_custom_target(kctrl_stubs DEPENDS "${RS12_KSTUB}")

# RePad: Bluetooth controllers (kernel plugin, MIT; vita/repad = the RePad project's files, keep them the same), in the
# VPK as module/repad.skprx. The app loads it when it is not running yet (re_pad_load_plugin)
add_executable(repad repad/src/main.c repad/src/bt.c repad/src/ctrl.c repad/src/pads.c)
set_target_properties(repad PROPERTIES LINK_FLAGS "-nostdlib")
target_compile_options(repad PRIVATE -fno-builtin -std=gnu11) # (taihen's TAI_CONTINUE: an empty () must mean any args)
target_link_libraries(repad SceBtForDriver_stub SceCtrlForDriver_stub SceKernelSuspendForDriver_stub SceSysclibForDriver_stub
  SceThreadmgrForDriver_stub taihenForKernel_stub taihenModuleUtils_stub SceSblAIMgrForDriver_stub)
add_custom_command(OUTPUT "${CMAKE_BINARY_DIR}/repad.skprx"
  COMMAND vita-elf-create -e "${CMAKE_SOURCE_DIR}/repad/repad.yml" $<TARGET_FILE:repad> "${CMAKE_BINARY_DIR}/repad.velf"
  COMMAND vita-make-fself -c "${CMAKE_BINARY_DIR}/repad.velf" "${CMAKE_BINARY_DIR}/repad.skprx"
  DEPENDS repad "${CMAKE_SOURCE_DIR}/repad/repad.yml"
  VERBATIM)
add_custom_target(repad_skprx ALL DEPENDS "${CMAKE_BINARY_DIR}/repad.skprx")
add_dependencies(${PROJECT_NAME} kctrl_stubs)
target_include_directories(${PROJECT_NAME} PRIVATE ${CMAKE_SOURCE_DIR}/kmod)

# vitaGL rebuilt without the startup splash (ReST-V tools/build_vitagl.sh -> ~/vitagl-nosplash). Falls back to the vitasdk package
set(RS12_VITAGL_NOSPLASH "$ENV{HOME}/vitagl-nosplash/libvitaGL.a")
if(EXISTS "${RS12_VITAGL_NOSPLASH}")
  set(RS12_VITAGL_LIB "${RS12_VITAGL_NOSPLASH}")
  message(STATUS "vitaGL: no splash (${RS12_VITAGL_NOSPLASH})")
else()
  set(RS12_VITAGL_LIB vitaGL)
  message(STATUS "vitaGL: vitasdk package (with splash)")
endif()

target_link_libraries(${PROJECT_NAME}
  "${RS12_KSTUB}"
  rs12_core
  ${RS12_VITAGL_LIB}
  vitashark
  zstd
  SceShaccCgExt
  mathneon
  taihen_stub
  SceShaccCg_stub
  SceKernelDmacMgr_stub
  SceGxm_stub
  SceDisplay_stub
  SceAppMgr_stub
  SceShellSvc_stub
  SceCommonDialog_stub
  SceIme_stub
  SceAudio_stub
  SceSysmodule_stub
  ScePgf_stub
  SceCtrl_stub
  SceRtc_stub
  SceTouch_stub
  SceMotion_stub
  ScePower_stub
  SceNet_stub
  SceNetCtl_stub
  SceLibKernel_stub
  stdc++
  pthread
  png
  mbedtls
  mbedx509
  mbedcrypto
  pthread
  z
  m
  c
)

# keep the unstripped .elf of every build as artifacts/<commit hash>.elf (core dump analysis)
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -DELF=$<TARGET_FILE:${PROJECT_NAME}> -DSRC=${RS12_ROOT}
          -P ${RS12_ROOT}/tools/archive_elf.cmake
  VERBATIM)

vita_create_self(eboot.bin ${PROJECT_NAME} UNSAFE)

# VPK: the path contains a space ("Vita Port"), so define it with VERBATIM instead of vita_create_vpk (ReiREM experience)
set(RS12_SCE_SYS_FILES
  sce_sys/icon0.png
  sce_sys/livearea/contents/bg.png
  sce_sys/livearea/contents/startup.png
  sce_sys/livearea/contents/template.xml
)
# module/rs12_kctrl.skprx: the kernel helper; module/repad.skprx: Bluetooth controllers; cacert.pem: certificates for the
# update check (HTTPS)
set(RS12_VPK_ADD_ARGS -a "${CMAKE_BINARY_DIR}/rs12_kctrl.skprx=module/rs12_kctrl.skprx"
  -a "${CMAKE_BINARY_DIR}/repad.skprx=module/repad.skprx"
  -a "${CMAKE_SOURCE_DIR}/res/cacert.pem=cacert.pem")
set(RS12_VPK_DEPS "${CMAKE_BINARY_DIR}/rs12_kctrl.skprx" "${CMAKE_BINARY_DIR}/repad.skprx"
  "${CMAKE_SOURCE_DIR}/res/cacert.pem")
foreach(f ${RS12_SCE_SYS_FILES})
  list(APPEND RS12_VPK_ADD_ARGS -a "${CMAKE_SOURCE_DIR}/${f}=${f}")
  list(APPEND RS12_VPK_DEPS "${CMAKE_SOURCE_DIR}/${f}")
endforeach()

set(RS12_SFO "${CMAKE_BINARY_DIR}/param.sfo")
set(RS12_VPK "${CMAKE_BINARY_DIR}/${PROJECT_NAME}.vpk")
add_custom_command(OUTPUT "${RS12_SFO}"
  COMMAND vita-mksfoex -s APP_VER=${VITA_VERSION} -s TITLE_ID=${VITA_TITLEID}
          "${VITA_APP_NAME}" "${RS12_SFO}"
  VERBATIM)
add_custom_command(OUTPUT "${RS12_VPK}"
  COMMAND vita-pack-vpk -s "${RS12_SFO}" -b "${CMAKE_BINARY_DIR}/eboot.bin"
          ${RS12_VPK_ADD_ARGS} "${RS12_VPK}"
  DEPENDS "${RS12_SFO}" "${CMAKE_BINARY_DIR}/eboot.bin" ${RS12_VPK_DEPS}
  VERBATIM)
# made only on request (tools/build.sh with VPK=1, or the target vpk)
add_custom_target(vpk DEPENDS "${RS12_VPK}")
