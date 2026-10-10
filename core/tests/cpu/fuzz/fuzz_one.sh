#!/usr/bin/env bash
# One differential fuzz batch: generate -> assemble -> run under qemu-ppc -cpu 750 -> run the interpreter -> compare.
#   fuzz_one.sh <seed> <workdir> <ncases> <runner> <compat> [classes]
# Leaves <workdir>/<seed>.log; keeps the .s/.elf/.qemu files only when the batch fails.
set -uo pipefail
seed=$1; w=$2; n=$3; runner=$4; compat=$5; classes=${6:-}
here="$(cd "$(dirname "$0")" && pwd)"
PPC=/opt/devkitpro/devkitPPC/bin/powerpc-eabi
b="$w/f$seed"
{
  python3 "$here/gen_fuzz.py" "$seed" "$n" "$b.s" $classes &&
  $PPC-as -mgekko -o "$b.o" "$b.s" &&
  $PPC-ld -T "$here/link.ld" -o "$b.elf" "$b.o" &&
  qemu-ppc -cpu 750 "$b.elf" > "$b.qemu"
} > "$b.log" 2>&1 || { echo "RESULT $b.elf HARNESS-ERROR" >> "$b.log"; exit 2; }
"$runner" -c "$compat" -m "$b.hist" "$b.elf" "$b.qemu" >> "$b.log" 2>&1
rc=$?
if [ $rc -eq 0 ]; then rm -f "$b.s" "$b.o" "$b.elf" "$b.qemu"; fi
exit $rc
