#!/usr/bin/env bash
# Compares the frames run_all.sh saved (core/out/all) with an earlier copy (default core/out/all_before).
#   bash core/tools/cmp_frames.sh [before-dir]
cd "$(dirname "$0")/../.."
before="${1:-core/out/all_before}"
same=0; diff=0
for f in core/out/all/*/last.png; do
    n="$(basename "$(dirname "$f")")"
    if cmp -s "$f" "$before/$n/last.png"; then same=$((same + 1)); else diff=$((diff + 1)); echo "DIFF $n"; fi
done
echo "$same same, $diff different"
