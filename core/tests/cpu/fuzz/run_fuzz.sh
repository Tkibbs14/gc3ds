#!/usr/bin/env bash
# Differential fuzzing of the Gekko interpreter against qemu-ppc -cpu 750.
#   run_fuzz.sh <runner> <workdir> <first-seed> <batches> <cases-per-batch> [classes]
# Prints per-batch failures and a final line: FUZZ batches=<b> cases=<n> failed_batches=<f> mismatched_cases=<m>
#
# The interpreter runs with these qemu-compatibility switches (GK_COMPAT_*, gekko_internal.h), each replacing a
# behaviour where qemu's 750 model is known to differ from Gekko or from the PEM; everything else is compared bit
# for bit:
#   0x01 EXACT_SINGLE_MUL  Gekko rounds frC to 25 bits in single-precision multiplies (hardware quirk; qemu exact)
#   0x02 MFFS_UPPER_ZERO   mffs upper word: Gekko 0xFFF80000, qemu 0
#   0x04 FCTIW_QEMU        fctiw upper word: Gekko 0xFFF80000 (+1 for -0), qemu sign extension; qemu also sets
#                          FPRF to the QNaN class on VXCVI (PEM: FPRF undefined)
#   0x08 STFS_TINY_ZERO    stfs below the single denormal range: hardware stores the bit-extracted pattern
#                          (architecturally undefined), qemu stores a signed zero
#   0x10 NEG_THEN_ROUND    fnmadd/fnmsub: PEM defines the result as the negated fmadd/fmsub result (round, then
#                          negate); qemu rounds the negated value, which differs in the two directed rounding modes
#   0x20 FR_QEMU           qemu does not implement FPSCR[FR] (never set; cleared only on invalid/zero-divide)
#   0x40 FX_ALWAYS         qemu sets FX on every signalled exception; the PEM sets it only on a 0 -> 1 transition
#   0x80 FCMP_QEMU         qemu clears FI on fcmpu/fcmpo and sets FPRF[C] when fcmpo signals VXVC (PEM: neither
#                          is altered)
# Not generated: paired singles, psq_*, dcbz_l and Gekko SPRs (qemu does not model them; covered by directed
# tests); privileged instructions, sc, tw/twi (would leave the user-mode process; directed tests); mftb
# (nondeterministic); fres/frsqrte (estimates: qemu computes exact values, the 750 a table lookup); FPSCR enable
# bits and NI (qemu does not model enabled-exception result suppression or non-IEEE mode); XER bits other than
# SO/OV/CA/byte count (reserved on the 750); invalid instruction forms.
set -uo pipefail
runner=$1; w=$2; first=$3; batches=$4; n=$5; classes=${6:-}
here="$(cd "$(dirname "$0")" && pwd)"
COMPAT=0x7FF
mkdir -p "$w"
seq "$first" $((first + batches - 1)) | xargs -P "$(nproc)" -I{} bash "$here/fuzz_one.sh" {} "$w" "$n" "$runner" "$COMPAT" "$classes" || true
fb=0; mc=0; cases=0
for s in $(seq "$first" $((first + batches - 1))); do
    log="$w/f$s.log"
    line=$(grep '^RESULT' "$log" 2>/dev/null | tail -1)
    if [[ "$line" =~ cases=([0-9]+)\ mismatched=([0-9]+) ]]; then
        cases=$((cases + BASH_REMATCH[1]))
        if [ "${BASH_REMATCH[2]}" != 0 ]; then
            fb=$((fb + 1)); mc=$((mc + BASH_REMATCH[2]))
            echo "--- batch seed $s: ${BASH_REMATCH[2]} mismatched case(s); first report:"
            sed -n '1,40p' "$log"
        fi
    else
        fb=$((fb + 1))
        echo "--- batch seed $s: harness error"; sed -n '1,20p' "$log"
    fi
done
echo "FUZZ batches=$batches cases=$cases failed_batches=$fb mismatched_cases=$mc"
