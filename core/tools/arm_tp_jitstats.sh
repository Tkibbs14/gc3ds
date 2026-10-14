#!/usr/bin/env bash
# Runs Twilight Princess (the owner's own image) under qemu-arm with the JIT and prints the JIT statistics, including
# which instructions were interpreted at run time.
#   bash core/tools/arm_tp_jitstats.sh FRAMES [gcemu options...]
# The ARM build has its own state layout (32-bit), so its states are made here, e.g. for the title scene:
#   bash core/tools/arm_tp_jitstats.sh 700 --press A@200-206,A@400-406 --save-state 700:/root/tp_arm_700.state
#   bash core/tools/arm_tp_jitstats.sh 760 --load-state /root/tp_arm_700.state
set -uo pipefail
cd "$(dirname "$0")/.."
iso="${GC3DS_ISO:?set GC3DS_ISO to your own disc image}"
out=/root/jitdyn
mkdir -p "$out"
frames="$1"; shift
time timeout 3600 qemu-arm -L /usr/arm-linux-gnueabihf -cpu cortex-a7 build-arm/gcemu "$iso" --frames "$frames" --jit --out "$out" "$@" > "$out/out.txt" 2>&1
grep -vE '^\[' "$out/out.txt" | tail -60
