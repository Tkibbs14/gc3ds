#!/usr/bin/env bash
# C41: is a port's charge on the emulated clock moving the game, or only speed_pct?
#
#   bash core/tools/c41_jobs.sh press            print the scripted Ordon walk's --press list (REMOTE_RUNS.md)
#   bash core/tools/c41_jobs.sh queue            queue the five runs below (prints a job id a line)
#   bash core/tools/c41_jobs.sh score JOB...     fetch those jobs' logs and score them
#
# The runs, all the Ordon walk (tp_ordon.state, 300 s, the same press list, scored from 100 s):
#   1. defaults                                  the session's anchor (the device drifts ~2% between days)
#   2. the HUD text port on, --port-charge 1      its charge all but removed
#   3. the HUD text port on, --port-charge 100    its own estimate, uncalibrated
#   4. the HUD text port on, --port-charge 330    the setting that read 27.90% (job 1430)
#   5. the HUD text port on, --ab portcharge 330  100 against 330 inside one run (no recompile: clean phases)
# Scored with runs.py (settled speed, real frames a second, VI fields per picture) and clock_split.py (where the clock
# came from, and the slack left in each field). **Judge these on frames a second, not on speed_pct:** speed_pct is
# cycles a real second, i.e. the clock the charge itself moves, while frames a second is what the game delivers.
set -euo pipefail
here="$(cd "$(dirname "$0")/.." && pwd)"
relay="$here/tools/relay.sh"
state=sdmc:/gc3ds/states/tp_ordon.state
hooks=1527   # the defaults (503) plus bit 10, J2DTextBoxEx::drawSelf (port_j2d.c; off by default since 10-25)

press_list() {
    local out="" off
    for off in 0 480 960 1440 1920 2400; do
        out+="SUP@$((40 + off))-$((159 + off)),SRIGHT@$((160 + off))-$((279 + off))"
        out+=",SDOWN@$((280 + off))-$((399 + off)),SLEFT@$((400 + off))-$((519 + off)),"
    done
    echo "${out%,}"
}

case "${1:-}" in
press) press_list ;;
queue)
    p="$(press_list)"
    common=(--game Twilight --state "$state" --seconds 300 --press "$p")
    bash "$relay" queue "${common[@]}"
    bash "$relay" queue "${common[@]}" --port-hooks $hooks --port-charge 1
    bash "$relay" queue "${common[@]}" --port-hooks $hooks --port-charge 100
    bash "$relay" queue "${common[@]}" --port-hooks $hooks --port-charge 330
    bash "$relay" queue "${common[@]}" --port-hooks $hooks --port-charge 330 --ab portcharge
    ;;
score)
    shift
    [ $# -gt 0 ] || { echo "usage: c41_jobs.sh score JOB..." >&2; exit 1; }
    logs=()
    for job in "$@"; do
        bash "$relay" log "$job" >/dev/null
        logs+=("$here/out/relay_$job.csv")
    done
    python3 "$here/tools/runs.py" --skip 100 "${logs[@]}"
    python3 "$here/tools/clock_split.py" --from 100 "${logs[@]}"
    for log in "${logs[@]}"; do  # the A/B run, if one of these is one: both arms side by side
        grep -q ',pcharge' "$log" 2>/dev/null || continue
        python3 "$here/tools/clock_split.py" --from 60 --by pcharge "$log"
        python3 "$here/tools/ab_phases.py" "$log" speed_pct 60 330 || true
    done
    ;;
*) sed -n '2,20p' "$0" ;;
esac
