#!/usr/bin/env bash
# Builds devkitPro's GameCube examples (libogc, open source) into ./examples-dol: the emulator's first test programs,
# no game data involved.
#   bash gc-tests/build_examples.sh
set -uo pipefail
export DEVKITPRO=/opt/devkitpro DEVKITPPC=/opt/devkitpro/devkitPPC
export PATH="$DEVKITPRO/tools/bin:$DEVKITPPC/bin:$PATH"
here="$(cd "$(dirname "$0")" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
cp -r /opt/devkitpro/examples/gamecube "$work/src"
mkdir -p "$here/examples-dol"
ok=0; fail=0
while IFS= read -r mk; do
    dir="$(dirname "$mk")"
    [ "$dir" = "$work/src" ] && continue
    grep -q "gamecube_rules\|ogc_rules\|base_rules" "$mk" || continue
    name="$(realpath --relative-to="$work/src" "$dir" | tr / _)"
    if make -C "$dir" >"$work/$name.log" 2>&1; then
        for f in "$dir"/*.dol; do [ -e "$f" ] && cp "$f" "$here/examples-dol/$name.dol"; done
        for f in "$dir"/*.elf; do [ -e "$f" ] && cp "$f" "$here/examples-dol/$name.elf"; done
        echo "OK   $name"; ok=$((ok + 1))
    else
        echo "FAIL $name: $(tail -n 2 "$work/$name.log" | tr '\n' ' ')"; fail=$((fail + 1))
    fi
done < <(find "$work/src" -name Makefile | sort)
echo "built $ok, failed $fail"
