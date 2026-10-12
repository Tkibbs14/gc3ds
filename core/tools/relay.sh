#!/usr/bin/env bash
# Remote 2DS runs through the dev relay (host3ds/relay.c on the 2DS, nakama-backend /api/devrelay in between), so the
# 2DS and this PC can be on different networks. The 2DS must be in gc3ds "Remote runs" (menu: L, or "auto 1").
#   bash core/tools/relay.sh run [gc3ds options]   upload this build if new, queue a job for it, follow its log
#        e.g. relay.sh run --game Twilight --state sdmc:/gc3ds/states/tp850.state --seconds 300 --ab traces
#   bash core/tools/relay.sh queue [options]       the same without following (prints the job id)
#   bash core/tools/relay.sh log JOB               follow a job's log into core/out/relay_JOB.csv until it ends
#   bash core/tools/relay.sh jobs | cancel JOB | push
# Settings: core/host3ds/relay.txt ("url <base>", "token <secret>"; never committed). Copy the same file to
# sdmc:/gc3ds/relay.txt on the card, adding "auto 1" there to start Remote runs at launch.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
cfg="$here/host3ds/relay.txt"
[ -f "$cfg" ] || { echo "missing $cfg" >&2; exit 1; }
url="$(sed -n 's/^url //p' "$cfg" | tr -d '\r')"
token="$(sed -n 's/^token //p' "$cfg" | tr -d '\r')"
build="$here/host3ds/gc3ds.3dsx"
api() { curl -sS --fail-with-body -H "x-relay-token: $token" "$@"; }

push() {
    [ -f "$build" ] || { echo "no $build (bash core/host3ds/build.sh)" >&2; exit 1; }
    local note
    note="$(git -C "$here" rev-parse --short HEAD 2>/dev/null || echo unknown)$(git -C "$here" diff --quiet 2>/dev/null || echo -dirty)"
    api -X PUT --data-binary @"$build" -H 'Content-Type: application/octet-stream' "$url/build?note=$note" | sed -n 's/^build //p'
}

queue() {
    local id args
    id="$(push)"
    args="$*"
    printf '{"args": %s, "build": "%s"}' "$(printf '%s' "$args" | sed 's/\\/\\\\/g; s/"/\\"/g; s/.*/"&"/')" "$id" |
        api -X POST -H 'Content-Type: application/json' --data-binary @- "$url/jobs" | sed -n 's/^job //p'
}

follow() {
    local job="$1" out="$here/out/relay_$1.csv" after=-1 status=queued hdr
    mkdir -p "$here/out"
    echo "job $job -> $out" >&2
    hdr="$(mktemp)"
    while :; do
        if api -D "$hdr" "$url/jobs/$job/log?after=$after" >> "$out"; then
            after="$(sed -n 's/^[Xx]-[Rr]elay-[Ll]ast: *//p' "$hdr" | tr -d '\r')"
            status="$(sed -n 's/^[Xx]-[Rr]elay-[Ss]tatus: *//p' "$hdr" | tr -d '\r')"
            printf '\r%s, %s lines   ' "$status" "$(wc -l < "$out")" >&2
            case "$status" in done|failed|stopped|cancelled) echo >&2; grep -E '^# (error|machine stopped|relay)' "$out" >&2 || true; break;; esac
        fi
        sleep 10
    done
    rm -f "$hdr"
}

cmd="${1:-}"; shift || true
case "$cmd" in
    push) push ;;
    queue) queue "$@" ;;
    run) job="$(queue "$@")"; follow "$job" ;;
    log) follow "$1" ;;
    jobs) api "$url/jobs" ;;
    cancel) api -X POST "$url/jobs/$1/cancel" ;;
    *) sed -n '2,10p' "$0"; exit 1 ;;
esac
