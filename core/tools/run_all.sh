#!/usr/bin/env bash
# Runs every test DOL for a few emulated seconds and saves its last frame and a one-line status.
#   bash core/tools/run_all.sh [seconds]
set -uo pipefail
cd "$(dirname "$0")/../.."
secs="${1:-3}"
out=core/out/all
mkdir -p "$out"
for dol in gc-tests/examples-dol/*.dol; do
    name="$(basename "$dol" .dol)"
    mkdir -p "$out/$name"
    log="$out/$name/log.txt"
    timeout 300 core/build/gcemu "$dol" --seconds "$secs" --out "$out/$name" > "$log" 2>&1
    rc=$?
    status="$(grep -E '^ran ' "$log" | sed -E 's/.*fields\) in ([0-9.]+) s wall: ([0-9.]+) MIPS; pc=([0-9a-f]+); GX bytes ([0-9]+)/wall \1s \2 MIPS pc=\3 gx=\4/')"
    errs="$(grep -cE '^\[(error|mem)\]' "$log")"
    printf '%-34s rc=%-3s %s errs=%s\n' "$name" "$rc" "$status" "$errs"
done
