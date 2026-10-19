#!/usr/bin/env bash
# ARM Linux harness (ari64 recompiler + NEON paths) -> build-arm/harness.
# Run: qemu-arm build-arm/harness --dynarec --roms roms --game tektagt ...
# Needs: gcc-arm-linux-gnueabihf, qemu-arm, ~/armzlib/inst (static ARM zlib)
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
GEN=()
command -v ninja >/dev/null 2>&1 && GEN=(-G Ninja)
cmake -S tools/harness -B build-arm "${GEN[@]}" -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_TOOLCHAIN_FILE="$ROOT/tools/harness/arm-linux.cmake" -DRS12_DYNAREC=ON >/dev/null
cmake --build build-arm -j8
