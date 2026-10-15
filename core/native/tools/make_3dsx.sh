#!/usr/bin/env bash
export PATH=/opt/devkitpro/tools/bin:$PATH
cd /root/native/build
smdhtool --create "gc3ds native" "Twilight Princess (native)" "gc3ds" /opt/devkitpro/libctru/default_icon.png native.smdh 2>&1 | tail -1
3dsxtool trial.elf native.3dsx --smdh=native.smdh && ls -la native.3dsx
cp native.3dsx /mnt/c/Users/tyler/TP3DS/emu/core/out/native.3dsx
