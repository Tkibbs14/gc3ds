#!/usr/bin/env bash
# Builds gc3ds.3dsx, bundling the libogc test programs (gc-tests/examples-dol, open source) into RomFS.
#   bash core/host3ds/build.sh                    menu build
#   AUTORUN=graphics_gx_triangle bash core/host3ds/build.sh   test build that starts that program directly
#   GPUPOS=0 AUTORUN=...                          the same with the position transform on the CPU (default 1: shader)
#   AUTORUN_OPTS="--timing-probe on --seconds 30" AUTORUN=...   with run options, as relay jobs take them
set -euo pipefail
export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
export PATH="$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH"
cd "$(dirname "$0")"
mkdir -p romfs/dols
cp ../../gc-tests/examples-dol/*.dol romfs/dols/ 2>/dev/null || echo "no test DOLs (run gc-tests/build_examples.sh)"
cp certs/relay_ca.pem romfs/  # roots for the dev relay's HTTPS (relay.c)
rm -f romfs/autorun.txt
if [ -n "${AUTORUN:-}" ]; then printf '%s\ngpupos=%s\n%s\n' "$AUTORUN" "${GPUPOS:-1}" "${AUTORUN_OPTS:-}" > romfs/autorun.txt; fi
# The commit this build came from, embedded so every perf.csv says which source produced it. Every figure in
# docs/research/MEASUREMENTS.md names a build id (a content hash of the 3dsx) and the method is "one build, one
# session", so a log that cannot be tied to source is a number without a provenance. relay.sh sends the same commit
# to the relay as the build's note; this puts it in the log itself, which is what a reader of core/out/ actually has.
# Written only when it changes, so make's header dependency rebuilds main.o exactly when the commit moved and not on
# every build.
rev="$(git -C ../.. rev-parse --short=12 HEAD 2>/dev/null || echo unknown)"
git -C ../.. diff --quiet HEAD 2>/dev/null || rev="$rev-dirty"
printf '#define GC3DS_REV "%s"\n' "$rev" > version.inc.new
if ! cmp -s version.inc.new version.inc; then mv version.inc.new version.inc; else rm -f version.inc.new; fi
echo "build rev $rev"
# RomFS contents are not tracked by make; force the final link so they are always packed.
rm -f gc3ds.3dsx
make "$@"
