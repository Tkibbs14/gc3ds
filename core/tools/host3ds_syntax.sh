#!/usr/bin/env bash
# Syntax-check core/host3ds/ on a PC that has no devkitARM.
#
#     bash core/tools/host3ds_syntax.sh [file.c ...]
#
# Why: a cloud session cannot build gc3ds.3dsx, so a plain C mistake in host3ds used to reach the machine that can.
# On 2026-10-30 a `double tot` declared inside an `if` block and used after it broke the build and cost a device
# session. This catches that class (declarations, types, arity, format strings) in a second, against the minimal
# stubs in core/tools/stub3ds. It is NOT a build: nothing is linked, the stubs do not behave like the SDK, and a
# clean run never replaces `bash core/host3ds/build.sh` before pushing a build to the relay.
set -u
here="$(cd "$(dirname "$0")/.." && pwd)"   # core/
cd "$here/.."

# Files the stubs cannot reach yet, with what each needs. Shrink this list rather than working around it.
declare -A skip=(
    [relay.c]="curl/curl.h (3ds portlib)"
    [gpuprobe.c]="gpuprobe_shbin.h (generated from the PICA shader at build time)"
    [pmu.c]="ARM11 CP15 inline asm and coprocessor builtins"
    [jit_3ds.c]="devkitARM cache and memory-permission svcs"
    [crashlog.c]="ERRF/exception layout beyond the stub's shape"
    [timingprobe.c]="hardware probe asm"
    [faultprobe.c]="hardware probe asm"
)

files=("$@")
[ ${#files[@]} -gt 0 ] || files=(core/host3ds/*.c)
fail=0 checked=0 skipped=0
for f in "${files[@]}"; do
    b="$(basename "$f")"
    if [ -n "${skip[$b]:-}" ] && [ $# -eq 0 ]; then
        printf 'skip  %-16s %s\n' "$b" "${skip[$b]}"
        skipped=$((skipped + 1))
        continue
    fi
    out="$(gcc -fsyntax-only -std=gnu11 -D__3DS__ -Icore/tools/stub3ds -Icore/src -Icore/host3ds "$f" 2>&1)"
    if printf '%s' "$out" | grep -qE 'error|fatal'; then
        printf 'FAIL  %s\n' "$b"
        printf '%s\n' "$out" | grep -E 'error|fatal' | head -20
        fail=$((fail + 1))
    else
        printf 'ok    %s\n' "$b"
        checked=$((checked + 1))
    fi
done
echo "$checked ok, $fail failed, $skipped not checkable (see the list in this script)"
[ "$fail" -eq 0 ]
