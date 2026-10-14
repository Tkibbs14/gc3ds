#!/usr/bin/env bash
# Builds the PC frontend for 32-bit ARM Linux (ARMv6K + VFP, like the 3DS CPU) and runs it under qemu-arm, so code
# that only exists for ARM (the JIT) can be run and checked on the PC.
#   bash core/tools/arm_build.sh                    build core/build-arm/gcemu-arm
#   bash core/tools/arm_build.sh run prog.dol ...   build, then run with the given gcemu arguments
set -euo pipefail
cd "$(dirname "$0")/.."
make -s -j"$(nproc)" CC=arm-linux-gnueabihf-gcc \
    CFLAGS="-std=c11 -O2 -g -Wall -Wextra -Wno-unused-parameter -march=armv6k -mfpu=vfp -mfloat-abi=hard -marm -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE" \
    LDFLAGS=-static BUILD=build-arm build-arm/gcemu
if [ "${1:-}" = run ]; then
    shift
    # cortex-a7 runs everything ARMv6K code can do; glibc itself is built for ARMv7.
    exec qemu-arm -cpu cortex-a7 build-arm/gcemu "$@"
fi
