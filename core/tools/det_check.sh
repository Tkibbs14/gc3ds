#!/usr/bin/env bash
# Runs each named test DOL twice and reports whether its last frame is the same both times.
#   bash core/tools/det_check.sh name...
cd "$(dirname "$0")/../.."
for n in "$@"; do
    for r in 1 2; do
        mkdir -p "/tmp/det/$n$r"
        timeout 300 core/build/gcemu "gc-tests/examples-dol/$n.dol" --seconds 2 --out "/tmp/det/$n$r" > /dev/null 2>&1
    done
    if cmp -s "/tmp/det/${n}1/last.png" "/tmp/det/${n}2/last.png"; then echo "$n deterministic"; else echo "$n varies run to run"; fi
done
