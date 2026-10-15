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
# Audio off on the 3DS, as Dusklight's own switch meant to do: DUSK_AUDIO_SKIP(default) at the top of each mDoAud_*
# returns its default when native mode has no audio engine (it is empty on PC). Without it the play scene waited for
# good on mDoAud_load1stDynamicWave / check1stDynamicWave (Azahar, boot to F_SP121, 2026-10-15).
python3 - <<'PY'
p = 'src/dusk/audio.h'
s = open(p).read()
old = '#if TARGET_PC\n#define DUSK_AUDIO_SKIP(...)\n#else'
new = ('#if TARGET_PC && defined(__3DS__)\nbool gc3ds_native_audio_enabled();  // core/native/platform/native_3ds.cpp\n'
       '#define DUSK_AUDIO_SKIP(...) if (!gc3ds_native_audio_enabled()) return __VA_ARGS__;\n#elif TARGET_PC\n'
       '#define DUSK_AUDIO_SKIP(...)\n#else')
if 'gc3ds_native_audio_enabled' not in s:
    assert s.count(old) == 1
    s = s.replace(old, new)
    open(p, 'w').write(s)
print('audio skip on the 3DS:', 'gc3ds_native_audio_enabled' in s)
PY
# Link's archive heaps: Dusklight's PC build makes them 20x larger for mods' models; on the 3DS (no mods) that is a 13.3 MB
# request the 13 MB game heap cannot meet (Azahar, boot to F_SP121: abort creating fpcNm_ALINK_e). 2x keeps headroom.
python3 - <<'PY'
p = 'src/d/actor/d_a_alink_swindow.inc'
s = open(p).read()
old = '        size *= 20; // Increase Link\'s heap size to prevent mods from crashing with higher-quality models.\n'
new = ('#if defined(__3DS__)\n        size *= 2;  // native mode on the 3DS: no mods, and a 13 MB game heap\n#else\n' + old + '#endif\n')
if 'native mode on the 3DS: no mods' not in s:
    assert s.count(old) == 1
    s = s.replace(old, new)
    open(p, 'w').write(s)
print('alink heap on the 3DS:', 'native mode on the 3DS: no mods' in s)
PY
# Aurora's FIFO consumer runs in Thread mode: a worker that took the stream under a mutex the game's writes take too,
# woken through atomic wait / notify. On the 3DS the worker's "shared cache" pinning does nothing, so it shared core 0
# with the game (the title scene's demo: 80-100% busy, job 213). Inline mode processes the stream as it is published.
python3 - <<'PY'
p = 'extern/aurora/lib/gx/fifo.cpp'
s = open(p).read()
old = 'constexpr auto kProcessingMode = ProcessingMode::Thread;\n'
new = ('#if defined(__3DS__)\nconstexpr auto kProcessingMode = ProcessingMode::Inline;  // gc3ds native mode: no worker on the game\'s core\n'
       '#else\n' + old + '#endif\n')
if 'gc3ds native mode: no worker' not in s:
    assert s.count(old) == 1
    s = s.replace(old, new)
    open(p, 'w').write(s)
print('aurora fifo inline on the 3DS:', 'gc3ds native mode: no worker' in s)
PY
# Display lists: Aurora copies each called list's bytes into the FIFO (GXCallDisplayList), where the GameCube's GPU read
# them in place. On the 3DS the copies' bytes and time are counted (gc3ds_dl_account, aurora_3ds.cpp): the title
# scene's demo wrote ~900 KB of GX a frame at 80-100% busy (jobs 213, 215).
python3 - <<'PY'
p = 'extern/aurora/lib/dolphin/gx/GXDispList.cpp'
s = open(p).read()
old = '''  // Write display list contents to the FIFO
  aurora::gx::fifo::write_data(data, nbytes);
'''
new = '''  // Write display list contents to the FIFO
#if defined(__3DS__)
  const uint64_t gc3dsT0 = gc3ds_ticks();
#endif
  aurora::gx::fifo::write_data(data, nbytes);
#if defined(__3DS__)
  gc3ds_dl_account(nbytes, gc3ds_ticks() - gc3dsT0);  // gc3ds native mode: list copies' bytes and time
#endif
'''
decl = '''void GXCallDisplayList(const void* data, u32 nbytes) {'''
newdecl = '''#if defined(__3DS__)
extern "C" uint64_t gc3ds_ticks(void);
extern "C" void gc3ds_dl_account(uint32_t bytes, uint64_t ticks);
#endif
void GXCallDisplayList(const void* data, u32 nbytes) {'''
if 'gc3ds_dl_account' not in s:
    assert s.count(old) == 1 and s.count(decl) == 1
    s = s.replace(old, new).replace(decl, newdecl)
    open(p, 'w').write(s)
print('aurora display-list accounting:', 'gc3ds_dl_account' in s)
# Then by reference: job 217 measured the copies at 680 KB and 4.2 ms of each heavy frame, plus 7.3 ms scanning the
# stream they bloated. On the 3DS a called list goes into the stream as GX_AURORA (0x50) subcommand 0x0060, a 64-bit
# pointer and a 32-bit size, read in place by the consumer, as the GameCube's GPU read display lists.
s = open(p).read()
old = '''  aurora::gx::fifo::write_data(data, nbytes);
#if defined(__3DS__)
  gc3ds_dl_account'''
new = '''#if defined(__3DS__)
  GX_WRITE_AURORA(0x0060);  // gc3ds native mode: CALL_DL by reference (u64 pointer, u32 size)
  GX_WRITE_U64(reinterpret_cast<uintptr_t>(data));
  GX_WRITE_U32(nbytes);
#else
  aurora::gx::fifo::write_data(data, nbytes);
#endif
#if defined(__3DS__)
  gc3ds_dl_account'''
if 'CALL_DL by reference' not in s:
    assert s.count(old) == 1
    s = s.replace(old, new)
    open(p, 'w').write(s)
print('aurora display lists by reference:', 'CALL_DL by reference' in s)
PY
git -C extern/aurora diff > /mnt/c/Users/tyler/TP3DS/emu/core/native/patches/aurora.patch
git -C extern/aurora rev-parse HEAD > /mnt/c/Users/tyler/TP3DS/emu/core/native/patches/aurora.base
mkdir -p /mnt/c/Users/tyler/TP3DS/emu/core/native/patches
git diff > /mnt/c/Users/tyler/TP3DS/emu/core/native/patches/dusk.patch
git rev-parse HEAD > /mnt/c/Users/tyler/TP3DS/emu/core/native/patches/dusk.base
wc -l /mnt/c/Users/tyler/TP3DS/emu/core/native/patches/dusk.patch
