#!/usr/bin/env bash
# Vita core dump analysis (WSL). Fetch the latest eboot dump over FTP and decode it with vita-parse-core.
#   vita/tools/crash.sh [elf path=latest in artifacts/]   (needs VITA_IP, default 192.168.0.33)
set -euo pipefail
cd "$(dirname "$0")/../.."
export PATH="$HOME/vitasdk/bin:$PATH"
IP="${VITA_IP:-192.168.0.33}"
ELF="${1:-$(ls -t artifacts/*.elf | head -1)}"
DUMP=$(curl -s --max-time 10 --list-only "ftp://$IP:1337/ux0:/data/" | grep 'eboot.bin.psp2dmp' | sort | tail -1)
[ -n "$DUMP" ] || { echo "no dump"; exit 1; }
mkdir -p artifacts/dumps
curl -s --max-time 60 -o "artifacts/dumps/$DUMP" "ftp://$IP:1337/ux0:/data/$DUMP"
echo "dump: $DUMP  elf: $ELF"
(cd ~/vita-parse-core && python3 main.py "$OLDPWD/artifacts/dumps/$DUMP" "$OLDPWD/$ELF") 2>&1 | tr -cd '\11\12\15\40-\176' | sed 's/\x1b\[[0-9;]*m//g; s/\[[0-9]*m//g'
