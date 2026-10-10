#!/usr/bin/env bash
# Runs test DOLs under qemu-arm with and without the JIT and compares the final frames byte for byte.
#   bash core/tools/arm_jit_check.sh [seconds] [dol names...]
set -uo pipefail
cd "$(dirname "$0")/.."
secs="${1:-1}"; shift || true
names=("$@")
[ ${#names[@]} -eq 0 ] && names=(graphics_gx_triangle graphics_gx_neheGX_lesson05 graphics_gx_neheGX_lesson07 graphics_gx_texturetest)
bash tools/arm_build.sh >/dev/null || exit 2
for n in "${names[@]}"; do
    dol="../gc-tests/examples-dol/$n.dol"
    mkdir -p "out/jitcheck/$n/interp" "out/jitcheck/$n/jit"
    qemu-arm -cpu cortex-a7 build-arm/gcemu "$dol" --seconds "$secs" --out "out/jitcheck/$n/interp" > "out/jitcheck/$n/interp.log" 2>&1
    qemu-arm -cpu cortex-a7 build-arm/gcemu "$dol" --seconds "$secs" --jit --out "out/jitcheck/$n/jit" > "out/jitcheck/$n/jit.log" 2>&1
    if cmp -s "out/jitcheck/$n/interp/last.png" "out/jitcheck/$n/jit/last.png"; then same=SAME; else same=DIFFERENT; fi
    native="$(grep -oE '[0-9.]+%\)' "out/jitcheck/$n/jit.log" | head -1)"
    printf '%-32s frames %-9s native %s\n' "$n" "$same" "$native"
done
