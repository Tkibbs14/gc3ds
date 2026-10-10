#!/usr/bin/env bash
# Logs Twilight Princess's Z-texture draws in fields FROM..TO (PC build).
#   bash core/tools/tp_ztex.sh FROM TO
cd "$(dirname "$0")/../.."
iso="$(ls /mnt/c/Users/tyler/TP3DS/game/*.iso | head -1)"
make -C core -s 2>&1 | grep -E 'error|warning'
ZTEX_LOG=1 core/build/gcemu "$iso" --frames "$2" --press A@200-206,A@400-406 --from "$1" --log gx --out /tmp/tpz 2>&1 |
    grep -E 'ZTEXDRAW|EFB copy' | sed -E 's/draw [0-9]+ //; s/\([0-9]+ draws so far\)//' | uniq -c | tail -40
