#!/usr/bin/env bash
# Builds the JIT differential fuzzer for ARM Linux and runs it under qemu-arm, including planted-bug self-tests
# (a corrupted GPR, and a JIT that skips the rounding of single-precision float results).
#   bash core/tests/jit/run_jit_fuzz.sh [cases] [seed]
set -uo pipefail
cd "$(dirname "$0")/../.."
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
src=$(ls src/cpu/*.c src/sys/*.c src/hw/*.c src/gx/*.c src/boot/*.c src/disc/*.c src/jit/*.c src/host/log.c src/host/jit_glue.c 2>/dev/null)
arm-linux-gnueabihf-gcc -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -march=armv6k -mfpu=vfp -mfloat-abi=hard -marm \
    -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -static $src tests/jit/jit_fuzz.c -lm -o "$out/jit_fuzz" || exit 2
qemu-arm -cpu cortex-a7 "$out/jit_fuzz" "${1:-20000}" "${2:-1}"
status=$?
for plant in 1 2 3 4; do
    JIT_FUZZ_PLANT=$plant qemu-arm -cpu cortex-a7 "$out/jit_fuzz" 2000 7 > "$out/plant$plant.txt"
    if grep -q "fails=0" "$out/plant$plant.txt"; then echo "SELF-TEST FAILED: planted bug $plant not detected"; exit 3; fi
done
echo "self-test: planted corruption detected (GPR, float rounding, fused compare-branch, lost register-cache write)"
exit $status
