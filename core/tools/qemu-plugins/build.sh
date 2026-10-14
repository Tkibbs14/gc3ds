#!/usr/bin/env bash
# Builds the qemu plugins here against a qemu source tree configured with --enable-plugins.
#   QEMU_SRC=/root/qemu-src bash core/tools/qemu-plugins/build.sh   ->  core/tools/qemu-plugins/librangecount.so
set -euo pipefail
cd "$(dirname "$0")"
src="${QEMU_SRC:?set QEMU_SRC to a qemu source tree (configured with --enable-plugins)}"
gcc -O2 -shared -fPIC -Wall -I "$src/include/qemu" $(pkg-config --cflags glib-2.0) rangecount.c -o librangecount.so
echo built librangecount.so
