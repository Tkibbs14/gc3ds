#!/usr/bin/env bash
# Builds the interpreter + JIT for a bare ARMv6K target and runs the comparison under qemu's ARM1176 model.
#   bash bench/test/run_qemu.sh
set -euo pipefail
cd "$(dirname "$0")"
CC=/opt/devkitpro/devkitARM/bin/arm-none-eabi-gcc
out="$(mktemp -d)"
trap 'rm -rf "$out"' EXIT
$CC -march=armv6k -mfpu=vfp -mfloat-abi=hard -marm -O2 -ffp-contract=off -ffreestanding -fno-builtin \
    -nostdlib -static -Wl,-e,_start -Ishim -I../src \
    qemu_check.c ../src/interp.c ../src/jit.c ../src/kernels.c -lgcc -o "$out/qemu_check.elf"
qemu-arm -cpu arm1176 "$out/qemu_check.elf"
