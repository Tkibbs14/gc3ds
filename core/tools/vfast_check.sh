#!/usr/bin/env bash
# Compares the 3DS backend's fast vertex path (gx/vfast.c) with the reference transform on every test DOL, and on
# Twilight Princess (the owner's own image) when FRAMES is given.
#   bash core/tools/vfast_check.sh [seconds] [tp-frames]
set -uo pipefail
cd "$(dirname "$0")/../.."
secs="${1:-2}"
out=/tmp/vfast
mkdir -p "$out"
[ -n "${SKIP_DOLS:-}" ] || for dol in gc-tests/examples-dol/*.dol; do
    name="$(basename "$dol" .dol)"
    res="$(VFAST_CHECK=1 timeout 300 core/build/gcemu "$dol" --seconds "$secs" --out "$out/$name" 2>&1 | grep -E '^VFAST')"
    [ -n "$res" ] && printf '%-34s %s\n' "$name" "$(echo "$res" | tail -1)"
    echo "$res" | grep -E '^VFAST draw' | head -3
done
if [ -n "${2:-}" ]; then
    iso="$(ls /mnt/c/Users/tyler/TP3DS/game/*.iso | head -1)"
    VFAST_CHECK=1 timeout 3600 core/build/gcemu "$iso" --frames "$2" --press A@200-206,A@400-406 --out "$out/tp" 2>&1 | grep -E '^VFAST|^  vs double' | tail -45
fi
