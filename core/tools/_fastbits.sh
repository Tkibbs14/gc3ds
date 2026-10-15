#!/usr/bin/env bash
# A 2DS run with a denser indirect-branch table (FAST_BITS=13: 8,192 slots, 64 KB a table, 2-way with --ind2 on),
# against the clean baselines (jobs 184, 191). Then the default build again.
set -e
cd /mnt/c/Users/tyler/TP3DS/emu
touch core/src/jit/jit.c
bash core/host3ds/build.sh CPPFLAGS=-DFAST_BITS=13 > /tmp/fb13.log 2>&1 || { tail -20 /tmp/fb13.log; exit 1; }
cp core/host3ds/gc3ds.3dsx core/out/gc3ds_fastbits13.3dsx
bash core/tools/relay.sh queue --game Twilight --state sdmc:/gc3ds/states/tp850.state --seconds 300 --ind2 on
touch core/src/jit/jit.c
bash core/host3ds/build.sh > /tmp/fbdef.log 2>&1 || { tail -20 /tmp/fbdef.log; exit 1; }
echo "default build again"
