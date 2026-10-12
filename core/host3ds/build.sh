#!/usr/bin/env bash
# Builds gc3ds.3dsx, bundling the libogc test programs (gc-tests/examples-dol, open source) into RomFS.
#   bash core/host3ds/build.sh                    menu build
#   AUTORUN=graphics_gx_triangle bash core/host3ds/build.sh   test build that starts that program directly
#   GPUPOS=0 AUTORUN=...                          the same with the position transform on the CPU (default 1: shader)
set -euo pipefail
export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
export PATH="$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH"
cd "$(dirname "$0")"
mkdir -p romfs/dols
cp ../../gc-tests/examples-dol/*.dol romfs/dols/ 2>/dev/null || echo "no test DOLs (run gc-tests/build_examples.sh)"
cp certs/relay_ca.pem romfs/  # roots for the dev relay's HTTPS (relay.c)
rm -f romfs/autorun.txt
if [ -n "${AUTORUN:-}" ]; then printf '%s\ngpupos=%s\n' "$AUTORUN" "${GPUPOS:-1}" > romfs/autorun.txt; fi
# RomFS contents are not tracked by make; force the final link so they are always packed.
rm -f gc3ds.3dsx
make "$@"
