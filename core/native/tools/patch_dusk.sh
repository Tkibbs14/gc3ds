#!/usr/bin/env bash
# Applies native mode's changes to a Dusklight checkout (DUSK=/root/ref/dusk) and saves them to core/native/patches/
# (dusk.patch + dusk.base, aurora.patch + aurora.base). Idempotent. Run in WSL: bash core/native/tools/patch_dusk.sh
# Local patches to the Dusklight checkout for a 32-bit ARM (3DS) build. Idempotent; the diff is saved to
# core/native/patches/dusk.patch so the checkout can be rebuilt from upstream + this patch.
set -uo pipefail
cd /root/ref/dusk
# 32-bit ARM is little-endian too.
grep -q "__arm__" include/helpers/endian.h || sed -i 's/defined(__aarch64__) ||/defined(__aarch64__) || defined(__arm__) ||/' include/helpers/endian.h
# The mod ABI's struct-size checks assume 64-bit pointers; mods are not built for the 3DS.
f=sdk/include/mods/api.h
if ! grep -q "MOD_ABI_ASSERT" $f; then
    sed -i 's/^static_assert(sizeof(/MOD_ABI_ASSERT(sizeof(/' $f
    sed -i '0,/^#pragma once/s//#pragma once\n#include <stdint.h>\n#if UINTPTR_MAX > 0xFFFFFFFFu\n#define MOD_ABI_ASSERT(...) static_assert(__VA_ARGS__)\n#else\n#define MOD_ABI_ASSERT(...) static_assert(true)  \/\/ 32-bit host (3DS): the mod ABI is 64-bit only\n#endif/' $f
fi
grep -c "MOD_ABI_ASSERT" $f
# devkitARM's int32_t is long (as CodeWarrior's s32 was), so int is not s32 there.
f=src/dusk/config_var.hpp
grep -q "is_same_v<T, int>" $f || sed -i 's/^    || std::is_same_v<T, u64>;/    || std::is_same_v<T, u64>\n    || std::is_same_v<T, int>          \/\/ 32-bit ARM: int32_t is long, so int is a separate type\n    || std::is_same_v<T, unsigned int>;/' $f
grep -c "is_same_v<T, int>" $f
# The same int32_t-is-long difference against miniz's mz_uint32 (unsigned int).
f=src/dusk/archive.cpp
grep -q "mz_uint32 fileIdx;" $f || sed -i 's/^    uint32_t fileIdx;$/    mz_uint32 fileIdx;  \/\/ 32-bit ARM: uint32_t is unsigned long/' $f
grep -c "mz_uint32 fileIdx;" $f
# int / unsigned int are distinct from s32 / u32 there: the BE<> swaps and the config instantiations for them.
python3 - <<'PY'
import re
p = 'include/helpers/endian.h'
s = open(p).read()
mark = '// 32-bit ARM: int is not s32'
if mark not in s:
    anchor = 'template<>\nconstexpr s64 BE<s64>::swap(s64 val) noexcept {'
    add = ('#if defined(__arm__) && !defined(__aarch64__)  ' + mark + ' (int32_t is long)\n'
           'template<>\nconstexpr int BE<int>::swap(int val) noexcept {\n    return (int)be32((u32)val);\n}\n'
           'template<>\nconstexpr unsigned int BE<unsigned int>::swap(unsigned int val) noexcept {\n    return (unsigned int)be32((u32)val);\n}\n'
           '#endif\n\n')
    assert anchor in s, 'endian.h anchor'
    s = s.replace(anchor, add + anchor, 1)
    open(p, 'w').write(s)
p = 'src/dusk/config.cpp'
s = open(p).read()
mark = '// 32-bit ARM: int is not s32'
if mark not in s:
    anchor = 'template class ConfigImpl<f32>;'
    assert anchor in s, 'config.cpp anchor'
    s = s.replace(anchor, anchor + '\n#if defined(__arm__) && !defined(__aarch64__)  ' + mark +
                  '\ntemplate class ConfigImpl<int>;\ntemplate class ConfigImpl<unsigned int>;\n#endif', 1)
    open(p, 'w').write(s)
print('int patches ok')
PY
# Aurora (submodule): its DVD layer's atomic helpers deduce T from both the field (u32 = unsigned long here) and the
# value (unsigned int); the value parameter becomes non-deduced.
python3 - <<'PY'
p = 'extern/aurora/lib/dolphin/dvd/dvd.cpp'
s = open(p).read()
n = 0
for name in ('atomic_store_relaxed', 'atomic_store_release'):
    old = 'void %s(T& ref, T val) {' % name
    if old in s:
        s = s.replace(old, 'void %s(T& ref, std::type_identity_t<T> val) {  // 32-bit ARM: u32 is unsigned long' % name, 1)
        n += 1
open(p, 'w').write(s)
print('aurora dvd atomics patched:', n)
p = 'extern/aurora/lib/card/CardGciFolder.cpp'
s = open(p).read()
old = '  bytesNotUsed = std::max(bytesNotUsed, 0);'
if old in s:
    s = s.replace(old, '  bytesNotUsed = std::max<int32_t>(bytesNotUsed, 0);  // 32-bit ARM: int32_t is long', 1)
    open(p, 'w').write(s)
    print('aurora card max patched')
PY
# Heap sizes on the 3DS: Dusklight's PC sizes (system 32 MB, archive / j2d x2, game x20: ~140 MB, for 64-bit
# pointers and spare memory) do not fit; 32-bit structures are the GameCube's sizes again, so the scaling is cut to a
# margin for Dusklight's PC-path additions (~52 MB, from native mode's 64 MB arena).
python3 - <<'PY'
p = 'src/m_Do/m_Do_machine.cpp'
s = open(p).read()
mark = '3DS (gc3ds native mode)'
if mark not in s:
    old = '    gameHeapSize *= 20; // NOTE: increased from 2 to 20 to try to solve heap alloc crashes. maybe do a better fix later\n#endif'
    assert old in s, 'm_Do_machine anchor'
    new = old + ('\n#if defined(__3DS__)  // ' + mark + ': 32-bit, GameCube-sized structures. The GameCube ran these heaps in'
                 '\n    // 24 MB (system ~3 MB, game 4.35 MB); a margin over that, not the PC sizes: ~37 MB from native mode\'s 44 MB arena.'
                 '\n    arenaSize = 8 * 1024 * 1024;'
                 '\n    archiveHeapSize = archiveHeapSize / 2 * 3 / 2;'
                 '\n    j2dHeapSize = j2dHeapSize / 2 * 3 / 2;'
                 '\n    gameHeapSize = gameHeapSize / 20 * 3;'
                 '\n#endif')
    s = s.replace(old, new, 1)
    open(p, 'w').write(s)
    print('heap sizes patched')
PY
# Audio off (native::audio_enabled() false, the init flag set as Dusklight's DUSK_AUDIO_DISABLED did): no JAUSectionHeap
# exists, and the scene manager's wave loads read through it (the 2DS's first native run, 2026-10-15: a data abort at
# 0xF8 in Z2SceneMgr::loadSeWave from the logo scene's loadStaticWaves; asserts are compiled out). Each use returns
# as "nothing there" without a section heap.
python3 - <<'PY'
import re
guards = {
    'src/Z2AudioLib/Z2SceneMgr.cpp': 'return 0;',
    'src/Z2AudioLib/Z2SoundMgr.cpp': 'return false;',
}
for p, ret in guards.items():
    s = open(p).read()
    n = 0
    def add(m):
        global n
        n += 1
        return m.group(0) + '\n' + m.group(1) + 'if (sectionHeap == NULL) ' + ret + '  // native, audio off: no section heap'
    if 'native, audio off: no section heap' not in s:
        s = re.sub(r'([ \t]*)JUT_ASSERT\(\d+, sectionHeap\);', add, s)
        open(p, 'w').write(s)
    print(p, 'guards added', n)
p = 'src/Z2AudioLib/Z2WaveArcLoader.cpp'
s = open(p).read()
if 'native, audio off' not in s:
    s = s.replace('''    JAUSectionHeap* sectionHeap = JASGlobalInstance<JAUSectionHeap>::getInstance();
    const JAUSectionHeap::TSectionHeapData& heapData''', '''    JAUSectionHeap* sectionHeap = JASGlobalInstance<JAUSectionHeap>::getInstance();
    if (sectionHeap == NULL) return;  // native, audio off: no section heap
    const JAUSectionHeap::TSectionHeapData& heapData''', 1)
    s = s.replace('''        JASWaveBank* bank = JASGlobalInstance<JAUSectionHeap>::getInstance()->getWaveBankTable().getWaveBank(i);''',
                  '''        if (JASGlobalInstance<JAUSectionHeap>::getInstance() == NULL) break;  // native, audio off
        JASWaveBank* bank = JASGlobalInstance<JAUSectionHeap>::getInstance()->getWaveBankTable().getWaveBank(i);''', 1)
    open(p, 'w').write(s)
    print(p, 'guards added')
PY
git -C extern/aurora diff > /mnt/c/Users/tyler/TP3DS/emu/core/native/patches/aurora.patch
git -C extern/aurora rev-parse HEAD > /mnt/c/Users/tyler/TP3DS/emu/core/native/patches/aurora.base
mkdir -p /mnt/c/Users/tyler/TP3DS/emu/core/native/patches
git diff > /mnt/c/Users/tyler/TP3DS/emu/core/native/patches/dusk.patch
git rev-parse HEAD > /mnt/c/Users/tyler/TP3DS/emu/core/native/patches/dusk.base
wc -l /mnt/c/Users/tyler/TP3DS/emu/core/native/patches/dusk.patch
