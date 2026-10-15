#!/usr/bin/env bash
# Native mode: build what is left, then a trial link against libctru. Prints the undefined symbols (what the 3DS
# platform layer still has to provide), grouped by prefix, and the code / data size of everything compiled.
cd /mnt/c/Users/tyler/TP3DS/emu
export DEVKITPRO=/opt/devkitpro
B=/root/native/build
make -s -k -j6 -f core/native/Makefile objs 2>&1 | tail -20
find $B -name '*.err' -size +0 | head
objs=$(find $B -name '*.o')
echo "objects: $(echo "$objs" | wc -l)"
$DEVKITPRO/devkitARM/bin/arm-none-eabi-size -t $objs | tail -1
$DEVKITPRO/devkitARM/bin/arm-none-eabi-g++ -specs=3dsx.specs -march=armv6k -mtune=mpcore -mfloat-abi=hard -mtp=soft \
    -Wl,--gc-sections -Wl,--no-enum-size-warning -o $B/trial.elf $objs -L$DEVKITPRO/libctru/lib -lctru -lm \
    2> $B/link.txt
echo "link exit $?"
grep -oE "undefined reference to \`[^']+'" $B/link.txt | sed "s/undefined reference to \`//; s/'$//" | sort -u > $B/undefined.txt
echo "undefined symbols: $(wc -l < $B/undefined.txt)"
sed -E 's/\(.*//; s/<.*//; s/::[^:]*$//' $B/undefined.txt | sort | uniq -c | sort -rn | head -40
echo "== all (demangled)"; $DEVKITPRO/devkitARM/bin/arm-none-eabi-c++filt < $B/undefined.txt | cut -c1-170
[ -f $B/trial.elf ] &&$DEVKITPRO/devkitARM/bin/arm-none-eabi-size $B/trial.elf
