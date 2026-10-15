#!/usr/bin/env bash
# Data-cache simulation of TP's compiled code only (the JIT buffer's accesses), steady play: from the ARM 850 state,
# 60 frames, the first ~100 M compiled-code accesses only warm the caches. Then the region report.
set -u
cd /mnt/c/Users/tyler/TP3DS/emu/core
export GC3DS_ISO="/mnt/c/Users/tyler/TP3DS/game/Legend of Zelda, The - Twilight Princess (USA).nkit.iso"
cd tools/qemu-plugins
gcc -O2 -shared -fPIC -Wall -I /root/qemu-src/include/qemu $(pkg-config --cflags glib-2.0) dcache.c -o libdcache.so || exit 1
cd ../..
mkdir -p /root/dcache2
bin=/root/relayout/build-arm-snap/gcemu  # the build whose JIT buffer was at 43804000-44004000 (qemu-user maps alike)
JIT_RANGES=1 GC3DS_OPT=4723 timeout 9000 /root/qemu-src/build/qemu-arm -L /usr/arm-linux-gnueabihf -cpu cortex-a7 \
    -plugin tools/qemu-plugins/libdcache.so,out=/root/dcache2/dcache.txt,code=43804000-44004000,skip=100000000 -d plugin \
    $bin "$GC3DS_ISO" --frames 910 --jit --load-state /root/tp_arm_850.state --out /root/dcache2 > /root/dcache2/out.txt 2>&1
echo "exit $?"
grep "^range jit" /root/dcache2/out.txt
tail -1 /root/dcache2/out.txt
python3 tools/qemu-plugins/dcache_report.py /root/dcache2/dcache.txt /root/dcache2/out.txt $bin --top 25
