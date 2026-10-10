#!/usr/bin/env bash
# Renders TP field N with --pica-tev and PICA_TEV_CHECK: logs TEV programs whose PICA result differs from the
# reference, saves the frame as field_N_check.png and the log as tevcheck_N.txt. MASK limits the PICA path to draws
# whose approximation flags are within it (unset: all draws).
#   [MASK=0] bash core/tools/tp_tevcheck.sh N
cd "$(dirname "$0")/../.."
iso="$(ls /mnt/c/Users/tyler/TP3DS/game/*.iso | head -1)"
n="$1"
mkdir -p core/out/tp
if [ -n "${MASK:-}" ]; then export PICA_TEV_FLAGS="$MASK"; fi
PICA_TEV_CHECK=1 core/build/gcemu "$iso" --frames "$n" --press A@200-206,A@400-406 --pica-tev --out core/out/tp 2> "core/out/tp/tevcheck_$n.txt" > /dev/null
cp core/out/tp/last.png "core/out/tp/field_${n}_check.png"
grep -c TEVDIFF "core/out/tp/tevcheck_$n.txt"
