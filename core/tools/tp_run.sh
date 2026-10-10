#!/usr/bin/env bash
# Runs the PC build on Twilight Princess (the owner's own image) with the given gcemu options; output lines that
# are not log lines ("[cat] ...") are printed. Environment variables pass through (VFAST_CHECK, VFAST_STATS, ...).
#   bash core/tools/tp_run.sh --load-state /root/tp_900.state --frames 960
set -uo pipefail
cd "$(dirname "$0")/.."
iso="$(ls /mnt/c/Users/tyler/TP3DS/game/*.iso | head -1)"
mkdir -p /tmp/tprun
timeout 3600 build/gcemu "$iso" --out /tmp/tprun "$@" 2>&1 | grep -vE '^\[' | tail -60
