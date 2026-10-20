#!/usr/bin/env bash
# Run the unchanged interpreter suite with the optional hooks absent from its legacy link line.
set -euo pipefail
cd "$(dirname "$0")/../../.."
export GXCENSUS_CPU_HOOKS="$PWD/core/tests/gxcensus/cpu_hooks.c"
w=$(mktemp -d)
trap 'rm -rf "$w"' EXIT
cat > "$w/gcc" <<'WRAPPER'
#!/bin/bash
core=0
for arg in "$@"; do
    [[ "$arg" == -c ]] && exec /usr/bin/gcc "$@"
    [[ "$arg" == */gekko.c ]] && core=1
done
if [ "$core" = 1 ]; then
    exec /usr/bin/gcc "$@" "$GXCENSUS_CPU_HOOKS"
else
    exec /usr/bin/gcc "$@"
fi
WRAPPER
chmod +x "$w/gcc"
PATH="$w:$PATH" NO_BENCH=1 bash core/tests/cpu/run_tests.sh
