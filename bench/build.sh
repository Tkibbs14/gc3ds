#!/usr/bin/env bash
# Builds gcbench.3dsx in WSL with devkitPro (installed at /opt/devkitpro).
#   bash bench/build.sh
set -euo pipefail
export DEVKITPRO=/opt/devkitpro DEVKITARM=/opt/devkitpro/devkitARM
export PATH="$DEVKITPRO/tools/bin:$DEVKITARM/bin:$PATH"
cd "$(dirname "$0")"
make "$@"
