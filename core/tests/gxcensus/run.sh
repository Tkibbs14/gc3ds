#!/usr/bin/env bash
# X1 census self-test, no commercial data. GXCENSUS_SYMBOLS can override the default TP symbols path.
set -euo pipefail
cd "$(dirname "$0")/../../.."
w=$(mktemp -d)
trap 'rm -rf "$w"' EXIT
gcc -std=c11 -O2 -Wall -Wextra -Werror -Wno-unused-parameter -Wno-type-limits -DGX_CENSUS_PC \
    core/tests/gxcensus/census_test.c core/src/gx/vertex.c -lm -o "$w/test"
"$w/test" "$w/normal.txt"
GXCENSUS_PLANT=1 "$w/test" "$w/plant.txt"
python3 core/tests/gxcensus/verify.py --synthetic "$w/normal.txt" "$w/plant.txt"
