#!/usr/bin/env bash
# Renders Twilight Princess (the owner's own image) on the PC to field N with the software renderer and saves it.
#   bash core/tools/tp_frame.sh FIELD [extra gcemu args]
cd "$(dirname "$0")/../.."
iso="$(ls /mnt/c/Users/tyler/TP3DS/game/*.iso | head -1)"
n="$1"; shift
mkdir -p core/out/tp
core/build/gcemu "$iso" --frames "$n" --out core/out/tp "$@" 2>&1 | grep -E '^ran|polling|DI:|GX:'
cp core/out/tp/last.png "core/out/tp/field_$n.png"
