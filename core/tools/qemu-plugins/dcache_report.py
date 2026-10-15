#!/usr/bin/env python3
"""Where core 0's data-cache misses come from: dcache.c's per-line misses mapped to regions and symbols.

    python3 dcache_report.py dcache.txt run_stderr.txt build-arm/gcemu [--top 30]

run_stderr.txt: the emulator's stderr with JIT_RANGES=1 (range jit / mem1 / jitstate / gekko lines and the regions
line with jit_create's address). Static data comes from the ELF's symbols (nm), moved by the load offset that
jit_create's printed address gives. Everything else is heap (between the ELF's end and the stack) or stack.
"""
import bisect
import re
import subprocess
import sys
from collections import defaultdict


def main():
    a = sys.argv[1:]
    top = 30
    if '--top' in a:
        i = a.index('--top'); top = int(a[i + 1]); del a[i:i + 2]
    dpath, spath, elf = a
    ranges = []
    text_jit = None
    for line in open(spath, errors='replace'):
        m = re.match(r'range (\w+) ([0-9a-f]+)-([0-9a-f]+)', line)
        if m:
            ranges.append((int(m.group(2), 16), int(m.group(3), 16), m.group(1)))
        m = re.match(r'range regions stubs ([0-9a-f]+) hot ([0-9a-f]+) cold ([0-9a-f]+) end ([0-9a-f]+) text jit_create ([0-9a-f]+)', line)
        if m:
            s, h, c, e, t = (int(x, 16) for x in m.groups())
            ranges += [(s, h, 'jit stubs'), (h, c, 'jit hot code'), (c, e, 'jit cold code')]
            text_jit = t
    nm = subprocess.run(['arm-linux-gnueabihf-nm', '-S', '-C', elf], capture_output=True, text=True).stdout
    syms, funcs, elf_jit = [], [], None
    for line in nm.splitlines():
        p = line.split()
        if len(p) >= 4 and p[2] in 'dDbBrR':
            syms.append((int(p[0], 16), int(p[1], 16), p[3]))
        if len(p) >= 4 and p[2] in 'tT':
            funcs.append((int(p[0], 16), int(p[1], 16), p[3]))
        if len(p) >= 3 and p[-1] == 'jit_create':
            elf_jit = int(p[0], 16)
    off = (text_jit - elf_jit) if (text_jit is not None and elf_jit is not None) else 0
    syms = sorted((lo + off, lo + off + max(sz, 1), name) for lo, sz, name in syms)
    funcs = sorted((lo + off, lo + off + max(sz, 1), name) for lo, sz, name in funcs)
    fstarts = [f[0] for f in funcs]
    byfn = defaultdict(lambda: [0, 0, 0])

    def code_of(pc):
        for lo, hi, n in ranges:
            if n.startswith('jit ') and lo <= pc < hi:
                return n
        k = bisect.bisect_right(fstarts, pc) - 1
        if k >= 0 and pc < funcs[k][1]:
            return funcs[k][2]
        return 'other code'
    data_end = max((s[1] for s in syms), default=0)
    total = None
    per = defaultdict(lambda: [0, 0, 0])  # region -> rd, wr, l2
    persym = defaultdict(lambda: [0, 0, 0])
    starts = [s[0] for s in syms]
    for line in open(dpath):
        if line.startswith('#'):
            total = line.strip(); continue
        p = line.split()
        if p and p[0] == 'pc':
            v = byfn[code_of(int(p[1], 16))]
            v[0] += int(p[2]); v[1] += int(p[3]); v[2] += int(p[4])
            continue
        if len(p) < 5:
            continue
        addr = int(p[0], 16); rd, wr, l2 = int(p[1]), int(p[2]), int(p[3])
        name = None
        for lo, hi, n in ranges:  # the most specific (smallest) range that holds it
            if lo <= addr < hi and (name is None or hi - lo < name[1]):
                name = (n, hi - lo)
        region = name[0] if name else None
        sym = None
        if region is None:
            k = bisect.bisect_right(starts, addr) - 1
            if k >= 0 and addr < syms[k][1]:
                region, sym = 'static data', syms[k][2]
            elif addr < 0x40000000 and addr >= data_end:
                region = 'heap'
            elif addr >= 0x40000000:
                region = 'stack / mmap'
            else:
                region = 'other'
        per[region][0] += rd; per[region][1] += wr; per[region][2] += l2
        if sym:
            persym[sym][0] += rd; persym[sym][1] += wr; persym[sym][2] += l2
    print(total)
    tot = [sum(v[i] for v in per.values()) for i in range(3)]
    print(f'{"region":20s} {"load miss":>12s} {"store miss":>12s} {"L2 miss":>10s}   share of L1 misses')
    for r, v in sorted(per.items(), key=lambda kv: -(kv[1][0] + kv[1][1])):
        print(f'{r:20s} {v[0]:12d} {v[1]:12d} {v[2]:10d}   {100.0 * (v[0] + v[1]) / max(1, tot[0] + tot[1]):5.1f}%')
    print('\nwho made them (JIT code regions, or C functions), top by L1 misses:')
    ft = sum(v[0] + v[1] for v in byfn.values())
    for n, v in sorted(byfn.items(), key=lambda kv: -(kv[1][0] + kv[1][1]))[:top]:
        print(f'  {n:40s} {v[0]:10d} {v[1]:10d} {v[2]:8d}   {100.0 * (v[0] + v[1]) / max(1, ft):5.1f}%')
    print(f'\ntop static-data symbols by L1 misses:')
    for s, v in sorted(persym.items(), key=lambda kv: -(kv[1][0] + kv[1][1]))[:top]:
        print(f'  {s:40s} {v[0]:10d} {v[1]:10d} {v[2]:8d}')


main()
