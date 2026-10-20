#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/../../.."
make -C core -j"${GXCENSUS_JOBS:-4}"
w=$(mktemp -d)
trap 'rm -rf "$w"' EXIT
ulimit -c 0
objects=()
for p in core/build/census-obj/{cpu,sys,hw,gx,boot,host,disc,jit}/*.o core/build/obj/aotgen/*.o; do
    case "$p" in */pc_main.o|*/host/gxcensus.o) continue;; esac
    objects+=("$p")
done
gcc -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-type-limits -DGX_CENSUS_PC \
    core/tests/gxcensus/x1b_test.c "${objects[@]}" -lm -o "$w/test"
"$w/test" "$w/normal.txt"
GXCENSUS_PLANT=1 "$w/test" "$w/plant.txt"
python3 core/tests/gxcensus/verify_x1b.py --synthetic "$w/normal.txt" "$w/plant.txt"
# This fault charges the slow backend to decode; the normal timing oracle must reject it.
gcc -std=c11 -O2 -DGX_CENSUS_PC -DGX_CENSUS_TIMING_PLANT -c core/src/gx/fifo.c -o "$w/fifo-plant.o"
without_fifo=()
for p in "${objects[@]}"; do [[ "$p" == */gx/fifo.o ]] || without_fifo+=("$p"); done
gcc -std=c11 -O2 -DGX_CENSUS_PC core/tests/gxcensus/x1b_test.c "$w/fifo-plant.o" \
    "${without_fifo[@]}" -lm -o "$w/test-time-plant"
if "$w/test-time-plant" "$w/bad-time.txt" > "$w/bad-time.log" 2>&1; then
    echo 'FAIL: timing oracle accepted backend time'; exit 1
fi
grep -m1 'sampled_backend_ticks' "$w/bad-time.log"
echo 'PASS: timing oracle rejects planted backend contamination'
