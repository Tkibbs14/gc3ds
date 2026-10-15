#!/usr/bin/env bash
# Remote 2DS runs through the dev relay (host3ds/relay.c on the 2DS, a relay server in between: docs/REMOTE_RUNS.md), so the
# 2DS and this PC can be on different networks. The 2DS must be in gc3ds "Remote runs" (menu: L, or "auto 1").
#   bash core/tools/relay.sh run [gc3ds options]   upload this build if new, queue a job for it, follow its log
#        e.g. relay.sh run --game Twilight --state sdmc:/gc3ds/states/tp850.state --seconds 300 --ab traces
#   bash core/tools/relay.sh queue [options]       the same without following (prints the job id)
#   bash core/tools/relay.sh log JOB               follow a job's log into core/out/relay_JOB.csv until it ends
#   bash core/tools/relay.sh status                when the 2DS last polled for a job and last posted a log
#   bash core/tools/relay.sh jobs | cancel JOB | push
#   bash core/tools/relay.sh send FILE SDPATH | cat SDPATH   a file onto the SD card / a text file from it into a log
#   bash core/tools/relay.sh cmd JOB "set opt 1"     a live command for the running job (taken within ~2 s): shot,
#        set opt|sched|merge|vtrim|pld|splitopen|vwmem VALUE, stop (host3ds/main.c apply_cmd)
#   bash core/tools/relay.sh frames JOB              the job's frames so far into core/out/frames_JOB/ (BMP, + PNG)
# Settings: core/host3ds/relay.txt ("url <base>", "token <secret>"; never committed). Copy the same file to
# sdmc:/gc3ds/relay.txt on the card, adding "auto 1" there to start Remote runs at launch.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
cfg="$here/host3ds/relay.txt"
[ -f "$cfg" ] || { echo "missing $cfg" >&2; exit 1; }
url="$(sed -n 's/^url //p' "$cfg" | tr -d '\r')"
token="$(sed -n 's/^token //p' "$cfg" | tr -d '\r')"
build="${RELAY_BUILD:-$here/host3ds/gc3ds.3dsx}"  # RELAY_BUILD: another 3dsx (a variant build)
api() { curl -sS --fail-with-body -H "x-relay-token: $token" "$@"; }

push() {
    local file="${1:-$build}"
    [ -f "$file" ] || { echo "no $file (bash core/host3ds/build.sh)" >&2; exit 1; }
    local note
    note="$(git -C "$here" rev-parse --short HEAD 2>/dev/null || echo unknown)$(git -C "$here" diff --quiet 2>/dev/null || echo -dirty)"
    local id
    id="$(api -X PUT --data-binary @"$file" -H 'Content-Type: application/octet-stream' "$url/build?note=$note" | sed -n 's/^build //p')"
    [ "$file" = "$build" ] || { echo "$id"; return; }
    # The build's ELF, kept to decode crash addresses from the 2DS (builds are not byte-reproducible).
    mkdir -p "$here/out/elfs"
    [ -n "$id" ] && [ -f "$here/host3ds/gc3ds.elf" ] && cp "$here/host3ds/gc3ds.elf" "$here/out/elfs/$id.elf"
    echo "$id"
}

queue() {
    local id args
    id="$(push)"
    [ -n "$id" ] || { echo "build upload failed: no job queued" >&2; exit 1; }
    args="$*"
    printf '{"args": %s, "build": "%s"}' "$(printf '%s' "$args" | sed 's/\\/\\\\/g; s/"/\\"/g; s/.*/"&"/')" "$id" |
        api -X POST -H 'Content-Type: application/json' --data-binary @- "$url/jobs" | sed -n 's/^job //p'
}

follow() {
    local job="$1" out="$here/out/relay_$1.csv" after=-1 status=queued hdr
    mkdir -p "$here/out"
    echo "job $job -> $out" >&2
    hdr="$(mktemp)"
    : > "$out"  # the whole log is fetched again from the start (after=-1)
    local prev page
    page="$(mktemp)"
    while :; do
        prev="$after"
        # Appended only on success: a refused request's error body had landed in the log (a 429, job 182).
        if api -D "$hdr" "$url/jobs/$job/log?after=$after" > "$page" 2>/dev/null; then
            cat "$page" >> "$out"
            after="$(sed -n 's/^[Xx]-[Rr]elay-[Ll]ast: *//p' "$hdr" | tr -d '\r')"
            status="$(sed -n 's/^[Xx]-[Rr]elay-[Ss]tatus: *//p' "$hdr" | tr -d '\r')"
            printf '\r%s, %s lines   ' "$status" "$(wc -l < "$out")" >&2
            # The relay returns at most 200 chunks (~400 s of a run) per request: a finished job is read page by page
            # until a page brings nothing new (jobs 168-171 had been cut at ~410 s by stopping after the first page).
            [ "$after" != "$prev" ] && continue
            case "$status" in done|failed|stopped|cancelled) echo >&2; grep -E '^# (error|machine stopped|relay)' "$out" >&2 || true; break;; esac
        fi
        sleep 10
    done
    rm -f "$hdr" "$page"
}

frames() {
    local job="$1" dir="$here/out/frames_$1" name
    mkdir -p "$dir"
    for name in $(api "$url/jobs/$job/frames" | tr -d '\r'); do
        [ -f "$dir/$name" ] && continue
        # The relay's rate limit answers 429 to a burst: wait and try again (a frame is ~290 KB)
        local tries=0
        until api -o "$dir/$name" "$url/jobs/$job/frame/$name" 2>/dev/null; do
            rm -f "$dir/$name"; tries=$((tries + 1)); [ $tries -ge 5 ] && break; sleep $((tries * 3))
        done
        [ -f "$dir/$name" ] || { echo "frame $name: not fetched" >&2; continue; }
        # PNG beside it when ImageMagick or Python + Pillow is there (image viewers and Claude read PNG)
        if command -v magick >/dev/null 2>&1; then magick "$dir/$name" "$dir/${name%.bmp}.png" 2>/dev/null || true
        elif command -v python3 >/dev/null 2>&1; then python3 -c "import sys; from PIL import Image; Image.open(sys.argv[1]).save(sys.argv[2])" "$dir/$name" "$dir/${name%.bmp}.png" 2>/dev/null || true
        fi
        echo "$dir/$name"
    done
}

cmd="${1:-}"; shift || true
case "$cmd" in
    push) push ;;
    queue) queue "$@" ;;
    # send FILE SDPATH: any file onto the 2DS's SD card (e.g. native mode's 3dsx to sdmc:/3ds/gc3ds_native.3dsx); the
    # 2DS must already run a build with --fetch-to (main.c's file jobs). cat SDPATH: a text file from the SD into the log.
    send) id="$(push "$1")"; [ -n "$id" ] || { echo "upload failed" >&2; exit 1; }
          printf '{"args": "--fetch-to %s", "build": "%s"}' "$2" "$id" | api -X POST -H 'Content-Type: application/json' --data-binary @- "$url/jobs" | sed -n 's/^job //p' ;;
    # (the server wants a known build on every job: the latest machine job's, which the 2DS does not need for --cat)
    cat) b="$(api "$url/jobs" | awk '$5 != "--fetch-to" && $5 != "--cat" {print $3; exit}')"
         printf '{"args": "--cat %s", "build": "%s"}' "$1" "$b" | api -X POST -H 'Content-Type: application/json' --data-binary @- "$url/jobs" | sed -n 's/^job //p' ;;
    run) job="$(queue "$@")"; follow "$job" ;;
    log) follow "$1" ;;
    jobs) api "$url/jobs" ;;
    status) api "$url/status" ;;
    cancel) api -X POST "$url/jobs/$1/cancel" ;;
    cmd) job="$1"; shift; printf '%s' "$*" | api -X POST -H 'Content-Type: text/plain' --data-binary @- "$url/jobs/$job/cmd" ;;
    frames) frames "$1" ;;
    *) sed -n '2,10p' "$0"; exit 1 ;;
esac
