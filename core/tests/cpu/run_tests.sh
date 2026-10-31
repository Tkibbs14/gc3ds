#!/usr/bin/env bash
# Single entry point for the Gekko interpreter tests. Run inside WSL Debian:
#   bash core/tests/cpu/run_tests.sh   (from the repository root)
# Environment: FUZZ_BATCHES (default 200), FUZZ_CASES (cases per batch, default 64), FUZZ_SEED (default 1),
#              NO_BENCH=1 to skip the speed measurement.
# Steps: warning-free builds (gcc, clang, devkitARM) -> directed tests -> differential fuzzing against qemu-ppc ->
# harness self-test (two planted bugs must be caught) -> disassembler cross-check against objdump -> speed.
set -uo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
src="$here/../../src/cpu"
PPC=/opt/devkitpro/devkitPPC/bin/powerpc-eabi
BATCHES=${FUZZ_BATCHES:-200}
CASES=${FUZZ_CASES:-64}
SEED=${FUZZ_SEED:-1}
w="$(mktemp -d)"
trap 'rm -rf "$w"' EXIT
fail=0
summary=()
note() { summary+=("$1"); echo "$1"; }

CFLAGS="-std=c11 -O2 -Wall -Wextra -Werror"
CORE="$src/gekko.c $src/gekko_mem.c $src/gekko_decode.c $src/gekko_int.c $src/gekko_fpu.c $src/gekko_softfp.c $src/gekko_disasm.c $here/optional_hooks.c"

echo "== build"
if bash "$here/build_core.sh" "$w"; then
    note "PASS build: core compiles with zero warnings (gcc -std=c11, clang -std=c11, arm-none-eabi-gcc -std=gnu11 armv6k/vfp)"
else
    note "FAIL build"; fail=1
fi
build() {  # build <out> <extra flags> <sources...>
    local out=$1 flags=$2; shift 2
    gcc $CFLAGS $flags -o "$w/$out" "$@" -lm || { note "FAIL build of $out"; fail=1; }
}
build fuzz_compare "" "$here/fuzz/fuzz_compare.c" "$here/tb.c" $CORE
build fuzz_plant1 "-DGEKKO_PLANT_BUG=1" "$here/fuzz/fuzz_compare.c" "$here/tb.c" $CORE
build fuzz_plant2 "-DGEKKO_PLANT_BUG=2" "$here/fuzz/fuzz_compare.c" "$here/tb.c" $CORE
build directed "" "$here/directed.c" "$here/tb.c" $CORE
build disasm_check "" "$here/disasm_check.c" "$src/gekko_disasm.c"
build bench "" "$here/bench.c" "$here/tb.c" $CORE

echo "== directed tests"
out=$("$w/directed"); rc=$?
echo "$out" | grep -v '^DIRECTED' | head -50
line=$(echo "$out" | grep '^DIRECTED')
if [ $rc -eq 0 ]; then note "PASS directed: $line"; else note "FAIL directed: $line"; fail=1; fi

echo "== differential fuzzing vs qemu-ppc -cpu 750 ($BATCHES batches x $CASES cases)"
out=$(bash "$here/fuzz/run_fuzz.sh" "$w/fuzz_compare" "$w/fz" "$SEED" "$BATCHES" "$CASES")
echo "$out" | grep -v '^FUZZ' | head -80
line=$(echo "$out" | grep '^FUZZ')
if [[ "$line" =~ failed_batches=0\  ]]; then note "PASS fuzz: $line"; else note "FAIL fuzz: $line"; fail=1; fi
cat "$w"/fz/*.hist 2>/dev/null | sort | uniq -c | sort -rn > "$w/coverage.txt"
nmn=$(wc -l < "$w/coverage.txt")
ninst=$(awk '{s += $1} END {print s + 0}' "$w/coverage.txt")
note "INFO fuzz coverage: $ninst generated instructions, $nmn distinct mnemonics (incl. record/overflow forms)"
echo "   top mnemonics:" $(head -12 "$w/coverage.txt" | awk '{printf "%s:%s ", $2, $1}')
echo "   all mnemonics:" $(awk '{printf "%s ", $2}' "$w/coverage.txt" | fold -s -w 200)

echo "== harness self-test (planted bugs must be detected)"
for p in 1 2; do
    cls=$([ $p = 1 ] && echo logic || echo fp)
    out=$(bash "$here/fuzz/run_fuzz.sh" "$w/fuzz_plant$p" "$w/pl$p" 5000 8 64 "$cls")
    line=$(echo "$out" | grep '^FUZZ')
    if [[ "$line" =~ mismatched_cases=([0-9]+) ]] && [ "${BASH_REMATCH[1]}" -gt 0 ]; then
        note "PASS self-test plant $p ($([ $p = 1 ] && echo 'rlwimi result bit 8 flipped' || echo 'fadds result LSB flipped')): caught, $line"
        echo "$out" | grep -m1 -A3 '^MISMATCH' | sed 's/^/   /'
    else
        note "FAIL self-test plant $p not detected: $line"; fail=1
    fi
done

echo "== disassembler cross-check against objdump -M gekko,raw"
python3 "$here/fuzz/gen_fuzz.py" 424242 64 "$w/dis.s" && "$PPC-as" -mgekko -o "$w/dis.o" "$w/dis.s" &&
    "$PPC-as" -mgekko -o "$w/all.o" "$here/all_insns.s" || { note "FAIL disasm inputs"; fail=1; }
out=$( ("$PPC-objdump" -d -M gekko,raw "$w/dis.o"; "$PPC-objdump" -d -M gekko,raw "$w/all.o") | "$w/disasm_check"); rc=$?
echo "$out" | grep -v '^DISASM' | head -20
line=$(echo "$out" | grep '^DISASM')
if [ $rc -eq 0 ]; then note "PASS disasm: $line"; else note "FAIL disasm: $line"; fail=1; fi

# The guest frame pacer (--gfskip-target, hle_tp.c): feedback over real time, so it is driven here with a synthetic
# clock instead of being judged by one console run. Needs no disc image.
#
# It runs core/build/gcemu, which this script does not otherwise build, so **build it first**: a gcemu left over from
# an older checkout does not know the env var, falls through to the usage text and fails the case for the wrong
# reason (2026-10-30: it did exactly that on a machine that had only run core/host3ds/build.sh). If it still prints
# usage after the build, say so plainly instead of dumping it.
gcemu="$here/../../build/gcemu"
echo "== guest frame pacer (--gfskip-target, synthetic clock)"
make -C "$here/../.." >/dev/null 2>&1 || true
if [ -x "$gcemu" ]; then
    out=$(GC3DS_GFSKIP_SELFTEST=1 "$gcemu" 2>&1); rc=$?
    if [ $rc -eq 0 ] && [ "${out#*gfskip selftest}" != "$out" ]; then
        note "PASS gfskip pacer: ${out#gfskip selftest: }"
    elif [ "${out#*usage:}" != "$out" ]; then
        note "FAIL gfskip pacer: core/build/gcemu predates this branch (it printed its usage text). Run: make -C core"
        fail=1
    else
        echo "$out" | head -8
        note "FAIL gfskip pacer"
        fail=1
    fi
else
    note "SKIP gfskip pacer: no core/build/gcemu and make did not produce one (try: make -C core)"
fi

# The ports' give-up line histogram (port_j2d.c why_line). It cannot be exercised without a disc image, so it has
# its own self-test, and a histogram that teaches the wrong lesson is the bug it replaced.
echo "== port give-up line histogram"
if [ -x "$gcemu" ]; then
    out=$(GC3DS_PORTWHY_SELFTEST=1 "$gcemu" 2>&1); rc=$?
    if [ $rc -eq 0 ] && [ "${out#*port whyline selftest}" != "$out" ]; then
        note "PASS port whyline: ${out#port whyline selftest: }"
    elif [ "${out#*usage:}" != "$out" ]; then
        note "FAIL port whyline: core/build/gcemu predates this branch (it printed its usage text). Run: make -C core"
        fail=1
    else
        echo "$out" | head -8
        note "FAIL port whyline"
        fail=1
    fi
else
    note "SKIP port whyline: no core/build/gcemu"
fi

if [ "${NO_BENCH:-0}" != 1 ]; then
    echo "== speed (PC, single thread)"
    "$w/bench" | while read -r l; do note "INFO $l"; done
fi

echo
echo "================ SUMMARY ================"
for s in "${summary[@]}"; do echo "$s"; done
if [ $fail -eq 0 ]; then echo "ALL TESTS PASSED"; else echo "SOME TESTS FAILED"; fi
exit $fail
