#!/usr/bin/env bash
# The required interpreter/JIT gates, with one identical compiled binary for all runtime masks and seeds.
set -euo pipefail
cd "$(dirname "$0")/../../.."
out=${X3_OUT:-/root/x3/final}
[[ ! -e "$out/jit_fuzz" ]] || { echo 'Use a fresh X3_OUT: a fuzzer binary already exists' >&2; exit 2; }
mkdir -p "$out"
python3 core/tools/ps_fast.py --check > "$out/generated.txt"
if python3 core/tools/ps_fast.py --check --plant > "$out/generated-plant.txt" 2>&1; then
    echo 'FAIL: generated-code comparison accepted its plant'; exit 1
fi
echo 'PASS: generated-code comparison rejects its plant' >> "$out/generated.txt"
JIT_FUZZ_KEEP_BINARY="$out/jit_fuzz" bash core/tests/jit/run_jit_fuzz.sh 20000 1 > "$out/default.log" 2>&1 &
base=$!
while [[ ! -f "$out/jit_fuzz" ]]; do kill -0 "$base" 2>/dev/null || { wait "$base"; exit 1; }; sleep 1; done
sha256sum "$out/jit_fuzz" > "$out/binary.sha256"
JIT_FUZZ_USE_BINARY="$out/jit_fuzz" JIT_FUZZ_OPT=7 bash core/tests/jit/run_jit_fuzz.sh 20000 1 > "$out/opt7.log" 2>&1 &
opt=$!
JIT_FUZZ_USE_BINARY="$out/jit_fuzz" JIT_FUZZ_NOFAULT=1 bash core/tests/jit/run_jit_fuzz.sh 20000 1 > "$out/checked1.log" 2>&1 &
one=$!
JIT_FUZZ_USE_BINARY="$out/jit_fuzz" JIT_FUZZ_NOFAULT=1 bash core/tests/jit/run_jit_fuzz.sh 20000 3 > "$out/checked3.log" 2>&1 &
three=$!
failed=0
for pid in "$base" "$opt" "$one" "$three"; do wait "$pid" || failed=1; done
for name in default opt7 checked1 checked3; do
    echo "== $name"
    grep -E '^X3 directed|^JIT FUZZ|^self-test' "$out/$name.log" || true
done
exit "$failed"
