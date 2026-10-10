#!/usr/bin/env bash
# Lists the EFB copies Twilight Princess (the owner's image) makes in fields FROM..TO on the PC build.
#   bash core/tools/tp_copies.sh FROM TO [presses]
cd "$(dirname "$0")/../.."
iso="$(ls /mnt/c/Users/tyler/TP3DS/game/*.iso | head -1)"
core/build/gcemu "$iso" --frames "$2" --press "${3:-A@200-206,A@400-406}" --from "$1" --log gx --out /tmp/tpg 2>&1 |
    grep -E 'EFB copy' | sed -E 's/\([0-9]+ draws so far\)//' | sort | uniq -c | sort -rn | head -25
