#!/usr/bin/env bash
# One native-mode run on the 2DS through the relay, on THIS gc3ds build (relay.sh native uses the latest machine job's):
# sends core/out/native.3dsx to the card, uploads core/host3ds/gc3ds.3dsx if new, queues --native SECONDS [PRESS]
# [STAGE] for it and prints the job ids. Follow with: relay.sh log JOB, then relay.sh frames JOB for the captures.
#   bash core/native/tools/device_run.sh 150 START@600-606
set -euo pipefail
cd /mnt/c/Users/tyler/TP3DS/emu
R=core/tools/relay.sh
cfg=core/host3ds/relay.txt
url="$(sed -n 's/^url //p' "$cfg" | tr -d '\r')"
token="$(sed -n 's/^token //p' "$cfg" | tr -d '\r')"
if [ -z "${NOSEND:-}" ]; then send_job="$(bash $R send core/out/native.3dsx sdmc:/3ds/gc3ds_native.3dsx)"; else send_job="(not sent: NOSEND)"; fi
echo "send job $send_job"
build="$(bash $R push | tail -1)"
echo "gc3ds build $build"
args="--native $1"
[ -n "${2:-}" ] && [ "$2" != "-" ] && args="$args --press $2"
[ -n "${3:-}" ] && [ "$3" != "-" ] && args="$args --stage $3"
[ -n "${GXOPT:-}" ] && args="$args --gxopt $GXOPT"  # GX options (native/platform/gx/gx_3ds.c)
printf '{"args": "%s", "build": "%s"}' "$args" "$build" |
    curl -sS --fail-with-body -H "x-relay-token: $token" -X POST -H 'Content-Type: application/json' --data-binary @- "$url/jobs"
echo
