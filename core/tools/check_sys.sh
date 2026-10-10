#!/usr/bin/env bash
# Compile-checks the system side (everything except src/cpu) for the PC and for the 3DS, without linking.
set -uo pipefail
cd "$(dirname "$0")/.."
status=0
for f in src/sys/*.c src/hw/*.c src/gx/*.c src/boot/*.c src/host/*.c; do
    gcc -std=c11 -O2 -Wall -Wextra -Wno-unused-parameter -D_POSIX_C_SOURCE=200809L -D_DEFAULT_SOURCE -fsyntax-only "$f" || status=1
done
for f in src/sys/*.c src/hw/*.c src/gx/*.c src/boot/*.c; do
    /opt/devkitpro/devkitARM/bin/arm-none-eabi-gcc -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter \
        -march=armv6k -mfloat-abi=hard -mfpu=vfp -fsyntax-only "$f" || status=1
done
[ $status = 0 ] && echo "system side: OK" || echo "system side: ERRORS"
exit $status
