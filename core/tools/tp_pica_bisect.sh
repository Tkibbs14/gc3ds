#!/usr/bin/env bash
# Renders TP field N with --pica-tev for each PICA_TEV_FLAGS mask given, saving field_N_pica_<mask>.png.
#   bash core/tools/tp_pica_bisect.sh N mask...
cd "$(dirname "$0")/../.."
n="$1"; shift
for m in "$@"; do
    PICA_TEV_FLAGS="$m" bash core/tools/tp_frame.sh "$n" --press A@200-206,A@400-406 --pica-tev > /dev/null
    cp core/out/tp/field_$n.png "core/out/tp/field_${n}_pica_$m.png"
    echo "mask $m done"
done
