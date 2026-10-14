#!/usr/bin/env bash
# ARM instructions executed per guest instruction, inside the JIT's code and inside the ahead-of-time translated
# functions, measured under qemu with core/tools/qemu-plugins/librangecount.so (no 2DS needed). Helpers both call (slow
# paths, the interpreter) are not counted in either. Twilight Princess from the ARM state, the owner's own image.
#   QEMU_SRC=/root/qemu-src FR=870 bash core/tools/aot/rangecount.sh
set -euo pipefail
cd "$(dirname "$0")/../.."
src="${QEMU_SRC:?set QEMU_SRC to a qemu source tree built with --enable-plugins}"
Q="$src/build/qemu-arm"
iso="${GC3DS_ISO:?set GC3DS_ISO to your own disc image}"
state="${STATE:-/root/tp_arm_850.state}"
FR="${FR:-870}"
BIN="${BIN:-build-arm/gcemu}"
out="${OUT:-/tmp/rangecount}"
mkdir -p "$out"
QEMU_SRC="$src" bash tools/qemu-plugins/build.sh >/dev/null
run() { "$Q" -L /usr/arm-linux-gnueabihf -cpu cortex-a7 "$@"; }
# Spans: the JIT's code buffer (printed by the emulator) and every translated function (symbol table + load bias).
JIT_RANGES=1 AOT=1 GC3DS_NODRAW=1 timeout 300 "$Q" -L /usr/arm-linux-gnueabihf -cpu cortex-a7 "$BIN" "$iso" \
    --load-state "$state" --frames "$(( ${state//[^0-9]/} + 1 ))" --jit --out "$out" 2>&1 | grep '^range' > "$out/ranges.txt" || true
J=$(awk '/^range jit/ {print $3}' "$out/ranges.txt")
rt=$(grep -o 'aot_run at [0-9a-f]*' "$out/ranges.txt" | awk '{print $3}')
st=$(nm "$BIN" | awk '$3 == "aot_run" {print $1}')
bias=$(( 0x$rt - 0x$st ))
nm -S "$BIN" | awk '$4 ~ /^F_[0-9A-F]{8}$/ {print $1, $2}' | while read -r a z; do
    printf '%x %x\n' $(( 0x$a + bias )) $(( 0x$a + bias + 0x$z ))
done > "$out/aot_spans.txt"
P="$PWD/tools/qemu-plugins/librangecount.so"
JIT_RANGES=1 AOT=1 GC3DS_NODRAW=1 run -plugin "$P,range0=$J,file1=$out/aot_spans.txt" -d plugin -D "$out/aot.plugin" \
    "$BIN" "$iso" --load-state "$state" --frames "$FR" --jit --out "$out" > "$out/aot.out" 2>&1
[ -n "${SKIPJIT:-}" ] || GC3DS_NODRAW=1 run -plugin "$P,range0=$J" -d plugin -D "$out/jit.plugin" \
    "$BIN" "$iso" --load-state "$state" --frames "$FR" --jit --out "$out" > "$out/jit.out" 2>&1
python3 - "$out" <<'EOF'
import re, sys
o = sys.argv[1]
def num(path, pat):
    m = re.search(pat, open(path, errors='replace').read())
    return int(m.group(1)) if m else 0
ja = num(o + '/aot.plugin', r'range0 insns (\d+)'); aa = num(o + '/aot.plugin', r'range1 insns (\d+)')
jj = num(o + '/jit.plugin', r'range0 insns (\d+)')
gj_a = num(o + '/aot.out', r'JIT: (\d+) instructions run'); ga = num(o + '/aot.out', r'aot: \d+ runs, \d+ calls, (\d+) guest')
gj = num(o + '/jit.out', r'JIT: (\d+) instructions run')
print('JIT alone:        %10d guest instructions, %6.2f ARM instructions each in compiled code' % (gj, jj / max(gj, 1)))
print('with aot, JIT:    %10d guest instructions, %6.2f each' % (gj_a, ja / max(gj_a, 1)))
print('with aot, aot:    %10d guest instructions, %6.2f each in translated functions' % (ga, aa / max(ga, 1)))
EOF
# Per translated function (a build with AOT_PROF=-DAOT_PROF): ARM instructions per guest instruction, worst first.
python3 - "$out" "$bias" "$BIN" <<'EOF2'
import re, subprocess, sys
o, bias, binp = sys.argv[1], int(sys.argv[2]), sys.argv[3]
syms = {}
for line in subprocess.run(['nm', binp], capture_output=True, text=True).stdout.splitlines():
    p = line.split()
    if len(p) == 3 and re.fullmatch(r'F_[0-9A-F]{8}', p[2]): syms[int(p[0], 16) + bias] = int(p[2][2:], 16)
arm = {}
for m in re.finditer(r'^span ([0-9a-f]+) 1 (\d+)$', open(o + '/aot.plugin').read(), re.M):
    a = syms.get(int(m.group(1), 16))
    if a is not None: arm[a] = int(m.group(2))
guest = {int(a, 16): int(n) for a, n in re.findall(r'^aotprof ([0-9a-f]+) (\d+)$', open(o + '/aot.out').read(), re.M)}
rows = [(arm[a], guest.get(a, 0), a) for a in arm if guest.get(a)]
rows.sort(key=lambda r: -r[0])
tot_a = sum(r[0] for r in rows); tot_g = sum(r[1] for r in rows)
if rows: print('functions with both counts: %d, %.2f ARM per guest overall' % (len(rows), tot_a / max(tot_g, 1)))
for a, g, f in rows[:25]: print('  %08x  guest %9d  arm %10d  %5.1f per guest  (%.1f%% of arm)' % (f, g, a, a / g, 100.0 * a / max(tot_a, 1)))
EOF2
