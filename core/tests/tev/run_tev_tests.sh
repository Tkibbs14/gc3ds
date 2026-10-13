#!/usr/bin/env bash
# Builds the GX TEV -> PICA200 translation test for the PC and runs it, including a planted-bug self-test.
#   bash core/tests/tev/run_tev_tests.sh [fuzz-cases] [seed]
set -uo pipefail
cd "$(dirname "$0")/../.."
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
cflags="-std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE"
gcc $cflags src/gx/soft.c src/gx/tev_pica.c src/gx/texture.c src/gx/xform.c src/gx/vertex.c src/gx/vfast.c src/gx/fifo.c src/gx/tev_pica_sim.c src/host/log.c \
    tests/tev/tev_test.c -lm -o "$out/tev_test" || exit 2
# The 3DS build uses devkitARM; check the translator also builds warning-free for 32-bit ARM when a cross compiler exists.
if command -v arm-linux-gnueabihf-gcc > /dev/null; then
    arm-linux-gnueabihf-gcc $cflags -march=armv6k -mfpu=vfp -mfloat-abi=hard -marm -c src/gx/tev_pica.c -o "$out/tev_pica_arm.o" || exit 2
fi
"$out/tev_test" "${1:-40000}" "${2:-1}"
status=$?
TEV_PLANT=1 "$out/tev_test" 2000 7 > "$out/plant.txt"
if grep -q "fails=0" "$out/plant.txt"; then echo "SELF-TEST FAILED: planted corruption not detected"; exit 3; fi
echo "self-test: planted corruption detected ($(grep -o 'fails=[0-9]*' "$out/plant.txt"))"
exit $status
