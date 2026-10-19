#!/usr/bin/env bash
# Linux/WSL build helper.
#   tools/build.sh vita [Release|Debug] [extra cmake args...]   → build-vita/ (source vita/)
#   tools/build.sh pc   [extra cmake args...]                   → build-pc/ (PC harness, source tools/harness/)
# On Windows: wsl -d Ubuntu-24.04 -- bash tools/build.sh vita
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
export VITASDK="${VITASDK:-$HOME/vitasdk}"
export PATH="$VITASDK/bin:$PATH"
GEN=()
command -v ninja >/dev/null 2>&1 && GEN=(-G Ninja)

case "${1:-}" in
vita)
	TYPE="${2:-Release}"
	shift $(( $# >= 2 ? 2 : 1 ))
	DIR=build-vita
	[ "$TYPE" = Release ] || DIR=build-vita-dbg
	DIR="${BUILD_DIR:-$DIR}"
	cmake -S vita -B "$DIR" "${GEN[@]}" -DCMAKE_TOOLCHAIN_FILE="$VITASDK/share/vita.toolchain.cmake" \
		-DCMAKE_BUILD_TYPE="$TYPE" "$@"
	cmake --build "$DIR" -j
	;;
pc)
	shift
	DIR="${BUILD_DIR:-build-pc}"
	cmake -S tools/harness -B "$DIR" "${GEN[@]}" -DCMAKE_BUILD_TYPE=Release "$@"
	cmake --build "$DIR" -j
	;;
*)
	sed -n '2,5p' "$0" >&2
	exit 2
	;;
esac
