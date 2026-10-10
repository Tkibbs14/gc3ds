#!/usr/bin/env bash
# Compiles the CPU core for the host (gcc and clang) and for the 3DS (devkitARM) with warnings as errors.
# Usage: build_core.sh <outdir>   (prints nothing on success)
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
src="$here/../../src/cpu"
out="$1"
CORE="gekko.c gekko_mem.c gekko_decode.c gekko_int.c gekko_fpu.c gekko_softfp.c gekko_disasm.c"
for f in $CORE; do
  gcc -std=c11 -O2 -Wall -Wextra -Werror -c "$src/$f" -o "$out/${f%.c}.o"
  clang -std=c11 -O2 -Wall -Wextra -Werror -c "$src/$f" -o "$out/${f%.c}.clang.o"
  /opt/devkitpro/devkitARM/bin/arm-none-eabi-gcc -std=gnu11 -O2 -Wall -Wextra -Werror -march=armv6k \
      -mfloat-abi=hard -mfpu=vfp -c "$src/$f" -o "$out/${f%.c}.arm.o"
done
