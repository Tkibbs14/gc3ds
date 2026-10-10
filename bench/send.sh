#!/usr/bin/env bash
# Sends gcbench.3dsx to the 2DS NetLoader (Homebrew Launcher -> Y), retrying until it is listening.
#   bash bench/send.sh [ip] [tries]
set -uo pipefail
ip="${1:-${GC3DS_IP:?pass the 3DS IP address or set GC3DS_IP}}"
tries="${2:-100}"
cd "$(dirname "$0")"
for ((i = 1; i <= tries; i++)); do
    if /opt/devkitpro/tools/bin/3dslink -a "$ip" gcbench.3dsx; then echo SENT; exit 0; fi
    sleep 3
done
echo GAVE_UP
exit 1
