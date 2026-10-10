#!/usr/bin/env bash
# Compile-checks one or more source files with the 3DS build flags, without touching the shared build directory
# (several people edit this project at once; a full `make` would race).
#   bash bench/tools/cc1.sh src/gpu.c
set -euo pipefail
export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
export PATH="$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH"
cd "$(dirname "$0")/.."
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT
# Shader headers the build would generate (name_shbin.h declaring name_shbin[], _end[], _size).
for f in src/*.v.pica; do
    [ -e "$f" ] || continue
    base="$(basename "$f" .v.pica)"
    picasso -o "$tmp/$base.shbin" "$f"
    printf 'extern const u8 %s_shbin_end[];\nextern const u8 %s_shbin[];\nextern const u32 %s_shbin_size;\n' \
        "$base" "$base" "$base" > "$tmp/${base}_shbin.h"
done
for src in "$@"; do
    arm-none-eabi-gcc -c -g -Wall -Wextra -Wno-unused-parameter -O2 -mword-relocations -ffunction-sections \
        -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft -D__3DS__ -std=gnu11 -ffp-contract=off \
        -Isrc -I"$tmp" -I"$DEVKITPRO/libctru/include" -I"$DEVKITPRO/portlibs/3ds/include" \
        "$src" -o "$tmp/$(basename "$src").o"
    echo "OK $src"
done
