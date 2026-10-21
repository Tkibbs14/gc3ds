#!/usr/bin/env bash
# Builds the JIT differential fuzzer for ARM Linux and runs it under qemu-arm, including planted-bug self-tests
# (a corrupted GPR, and a JIT that skips the rounding of single-precision float results).
#   bash core/tests/jit/run_jit_fuzz.sh [cases] [seed]
set -uo pipefail
cd "$(dirname "$0")/../.."
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
src=$(ls src/cpu/*.c src/sys/*.c src/hw/*.c src/gx/*.c src/boot/*.c src/disc/*.c src/jit/*.c src/host/log.c src/host/jit_glue.c 2>/dev/null)
if [[ -n "${JIT_FUZZ_USE_BINARY:-}" ]]; then
    cp "$JIT_FUZZ_USE_BINARY" "$out/jit_fuzz"
else
arm-linux-gnueabihf-gcc -std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -march=armv6k -mfpu=vfp -mfloat-abi=hard -marm \
    -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -DJIT_FUZZ_X3 ${FUZZ_CFLAGS:-} -static $src tests/jit/jit_fuzz.c -lm -o "$out/jit_fuzz" || exit 2
fi
if [[ -n "${JIT_FUZZ_KEEP_BINARY:-}" ]]; then cp "$out/jit_fuzz" "$JIT_FUZZ_KEEP_BINARY"; fi
qemu-arm -cpu cortex-a7 "$out/jit_fuzz" "${1:-20000}" "${2:-1}" | tee "$out/main.txt"
status=${PIPESTATUS[0]}
# A run that crashed prints no summary: that is a failure, never a pass (2026-10-12: a segfault read as a pass).
grep -qE "^JIT FUZZ .*fails=0" "$out/main.txt" || { echo "FUZZ RUN FAILED (crashed, or found differences)"; exit 4; }
# Chains of linked blocks through the dispatcher, traces off as on the 2DS (with traces on, seed 5 case 16475 differs:
# a known trace-mode bug, HANDOFF 2026-10-12).
JIT_FUZZ_NOTRACES=1 qemu-arm -cpu cortex-a7 "$out/jit_fuzz" "${1:-20000}" "${2:-1}" chain | grep -v '^\[' | tee "$out/chain.txt"
grep -qE "^JIT FUZZ .*fails=0" "$out/chain.txt" || { echo "CHAIN FUZZ RUN FAILED (crashed, or found differences)"; exit 4; }
for plant in 1 2 3 4 5 6 7 9 12 13 14 17 18 19; do
    JIT_FUZZ_PLANT=$plant qemu-arm -cpu cortex-a7 "$out/jit_fuzz" 8000 7 > "$out/plant$plant.txt"
    if ! grep -qE "^JIT FUZZ .*fails=[1-9]" "$out/plant$plant.txt"; then echo "SELF-TEST FAILED: planted bug $plant not detected (or the run crashed)"; exit 3; fi
    echo "self-test plant $plant: $(grep '^JIT FUZZ' "$out/plant$plant.txt")"
done
JIT_FUZZ_NOTRACES=1 JIT_FUZZ_PLANT=8 qemu-arm -cpu cortex-a7 "$out/jit_fuzz" 4000 7 chain > "$out/plant8.txt"
if ! grep -qE "^JIT FUZZ .*fails=[1-9]" "$out/plant8.txt"; then echo "SELF-TEST FAILED: planted bug 8 not detected (or the run crashed)"; exit 3; fi
JIT_FUZZ_NOTRACES=1 JIT_FUZZ_PLANT=11 JIT_FUZZ_OPT=$(( ${JIT_FUZZ_OPT:-0} | 32 )) qemu-arm -cpu cortex-a7 "$out/jit_fuzz" 4000 7 chain > "$out/plant11.txt"
if ! grep -qE "^JIT FUZZ .*fails=[1-9]" "$out/plant11.txt"; then echo "SELF-TEST FAILED: planted bug 11 not detected (or the run crashed)"; exit 3; fi
echo "self-test: planted corruption detected (GPR, float rounding, fused compare-branch, lost register-cache write, followed branch, side-exit count, register-use table, dispatcher entry count, page stamps for the vertex cache, sc handler count, X3 helper slot and callee-saved cache retention)"
exit $status
