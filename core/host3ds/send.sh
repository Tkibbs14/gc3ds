#!/usr/bin/env bash
# Sends gc3ds.3dsx to a 3DS waiting in NetLoader (Homebrew Launcher -> Y), retrying until it answers.
#   bash core/host3ds/send.sh <3ds-ip> [tries]     (or set GC3DS_IP)
set -uo pipefail
ip="${1:-${GC3DS_IP:?pass the 3DS IP address or set GC3DS_IP}}"
tries="${2:-200}"
cd "$(dirname "$0")"
for ((i = 1; i <= tries; i++)); do
    if /opt/devkitpro/tools/bin/3dslink -a "$ip" gc3ds.3dsx; then echo SENT; exit 0; fi
    sleep 3
done
echo GAVE_UP
exit 1
