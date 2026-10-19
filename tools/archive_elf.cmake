# Archive the unstripped .elf as artifacts/<prefix><commit hash>.elf (crash dump analysis).
# Used to decode .psp2dmp core dumps on Vita (vita-parse-core) and PSPLink exception addresses on PSP (psp-addr2line).
#
# usage: cmake -DELF=<path> -DSRC=<repo root> [-DPREFIX=psp-] -P tools/archive_elf.cmake
# Appends -dirty to the name if there are uncommitted changes.

if(NOT ELF OR NOT SRC)
  message(FATAL_ERROR "archive_elf: ELF and SRC are required")
endif()

find_package(Git QUIET)
set(HASH "nogit")
if(GIT_FOUND)
  execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse --short=10 HEAD
    WORKING_DIRECTORY "${SRC}" OUTPUT_VARIABLE OUT RESULT_VARIABLE RC
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  if(RC EQUAL 0)
    set(HASH "${OUT}")
    execute_process(COMMAND "${GIT_EXECUTABLE}" status --porcelain --untracked-files=no
      WORKING_DIRECTORY "${SRC}" OUTPUT_VARIABLE DIRTY ERROR_QUIET)
    if(NOT DIRTY STREQUAL "")
      set(HASH "${HASH}-dirty")
    endif()
  endif()
endif()

file(MAKE_DIRECTORY "${SRC}/artifacts")
execute_process(COMMAND "${CMAKE_COMMAND}" -E copy "${ELF}" "${SRC}/artifacts/${PREFIX}${HASH}.elf"
  RESULT_VARIABLE RC)
if(NOT RC EQUAL 0)
  message(FATAL_ERROR "archive_elf: copy failed (${ELF})")
endif()
message(STATUS "archive_elf: artifacts/${PREFIX}${HASH}.elf")
