#!/usr/bin/env bash
# Runs Twilight Princess (the owner's own image) on the PC build with the guest profiler for N emulated seconds.
#   bash core/tools/tp_profile.sh [seconds]
cd "$(dirname "$0")/../.."
iso="$(ls /mnt/c/Users/tyler/TP3DS/game/*.iso | head -1)"
mkdir -p /tmp/tpprof
time core/build/gcemu "$iso" --seconds "${1:-4}" --profile --out /tmp/tpprof 2>&1 | tail -60
