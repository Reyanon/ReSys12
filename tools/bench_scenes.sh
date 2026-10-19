#!/usr/bin/env bash
# Measurement scenes: gameplay save states for the Vita (vita/tools/run_game.ps1 -Bench).
#   tools/bench_scenes.sh find OUT FRAMES game...   PPM every 500 frames (pick a gameplay frame from them)
#   tools/bench_scenes.sh save OUT game:frame ...   save states OUT/<set>.st, made by the ARM harness under qemu-arm
#                                                   (the state blocks are raw structs: 32-bit ARM layout = the Vita's)
# Both runs feed the same input: two coins, two starts, then player 1 mashes buttons (RS12_INPUT_MASH).
# Upload with vita/tools/upload_bench.ps1. The states hold game RAM: keep them out of the repository and the VPK.
# Scenes used so far (10-18): tektagt:3500 tekken3:3500 ehrgeiz:3000 soulclbr:3000 toukon3:3000 fgtlayer:3500
#   pacapp2:5000 sws99:3000 tenkomor:4000 kaiunqz:3000 ghlpanic:5500 aquarush:5000
set -u
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT"
INPUT="1200:0:1,1210:0:0,1250:0:1,1260:0:0,1300:400:0,1310:0:0,1350:400:0,1360:0:0"
MASH=1400
mode=$1; out=$2; shift 2
mkdir -p "$out"
case "$mode" in
find)
	frames=$1; shift
	for g in "$@"; do
		mkdir -p "$out/$g"
		RS12_INPUT=$INPUT RS12_INPUT_MASH=$MASH ./build-pc/harness --roms roms --game "$g" --frames "$frames" \
			--dump "$(seq -s, 1000 500 "$frames")" --out "$out/$g" > "$out/$g/log.txt" 2>&1 &
	done
	wait
	;;
save)
	for gf in "$@"; do
		g=${gf%%:*}; f=${gf##*:}
		RS12_INPUT=$INPUT RS12_INPUT_MASH=$MASH qemu-arm build-arm/harness --roms roms --game "$g" --frames "$f" \
			--savestate "$f" "$out/$g.st" 2>&1 | grep savestate | sed "s/^/$g /" &
	done
	wait
	;;
*)
	echo "usage: $0 find|save OUT ..." >&2
	exit 1
	;;
esac
