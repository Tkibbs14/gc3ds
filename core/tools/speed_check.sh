#!/usr/bin/env bash
# PC build: runs a few test DOLs for N emulated seconds and prints emulated-vs-wall speed.
#   bash core/tools/speed_check.sh [seconds] [names...]
set -uo pipefail
cd "$(dirname "$0")/../.."
secs="${1:-3}"; shift || true
names=("$@")
[ ${#names[@]} -eq 0 ] && names=(graphics_fb_consoletest graphics_gx_triangle graphics_gx_neheGX_lesson07)
make -s -C core -j8 2>&1 | grep -E 'error|warning'
for n in "${names[@]}"; do
    line="$(core/build/gcemu "gc-tests/examples-dol/$n.dol" --seconds "$secs" --out "core/out/speed/$n" 2>&1 | grep -E '^ran ')"
    printf '%-32s %s\n' "$n" "$line"
done
