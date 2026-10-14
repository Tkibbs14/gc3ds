#!/usr/bin/env bash
# Runs one test DOL under qemu-arm with the JIT for each given duration and prints the JIT statistics.
#   bash core/tools/arm_jit_stats.sh <dol name> <seconds> [seconds...]
set -uo pipefail
cd "$(dirname "$0")/.."
name="$1"; shift
for s in "$@"; do
    echo "== $name, $s emulated s"
    qemu-arm -L /usr/arm-linux-gnueabihf -cpu cortex-a7 build-arm/gcemu "../gc-tests/examples-dol/$name.dol" --seconds "$s" --jit --out "out/jitstats/$name" 2>&1 | grep -E '^JIT|^ran '
done
