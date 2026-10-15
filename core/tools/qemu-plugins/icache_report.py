#!/usr/bin/env python3
"""Where simulated L1 instruction-cache misses come from (core/tools/qemu-plugins/icache.c).

    python3 icache_report.py RUNDIR GCEMU [--minus RUNDIR2] [--blockmap PATH] [--symbols TP_SYMBOLS] [--top 25]

RUNDIR holds out.txt (gcemu's stderr with JIT_RANGES=1: the "range regions" line) and icache.txt (the plugin's output).
Misses are split into the JIT's shared stubs, its hot and cold regions, gcemu's own functions (from `nm GCEMU`, rebased
by jit_create's address) and everything else (libc, the loader). With --blockmap (gcemu's JIT_BLOCKMAP file: block
host start and guest PC), hot-region misses are attributed to blocks, and with --symbols (a TP symbols.txt) to the
guest functions those blocks are in. --minus subtracts a shorter run from the same state (emulation is deterministic, so
its counts are an exact prefix): what is left is the steady state after the warm-up compiles.
"""
import bisect, re, subprocess, sys
from collections import defaultdict


def main():
    a = sys.argv[1:]
    run, gcemu = a[0], a[1]
    opt = lambda k: a[a.index(k) + 1] if k in a else None
    top = int(opt("--top") or 25)
    m = re.search(r"range regions stubs (\w+) hot (\w+) cold (\w+) end (\w+) text jit_create (\w+)", open(run + "/out.txt", errors="replace").read())
    stubs, hot, cold, end, jc = (int(x, 16) for x in m.groups())
    syms = []
    for line in subprocess.run(["arm-linux-gnueabihf-nm", "-n", "-C", gcemu], capture_output=True, text=True).stdout.splitlines():
        p = line.split(None, 2)
        if len(p) == 3 and p[1] in "tTwW":
            syms.append((int(p[0], 16), p[2]))
    base = jc - dict((n, ad) for ad, n in syms)["jit_create"]
    saddr = [ad + base for ad, _ in syms]
    total = None
    lines = []
    def read(d):
        t, out = None, {}
        for line in open(d + "/icache.txt"):
            if line.startswith("#"):
                t = line.strip()
                continue
            x, miss, acc = line.split()
            out[int(x, 16)] = (int(miss), int(acc))
        return t, out
    total, cur = read(run)
    if opt("--minus"):
        t2, prev = read(opt("--minus"))
        total += "  minus  " + t2
        for k, (mi, ac) in prev.items():
            if k in cur: cur[k] = (cur[k][0] - mi, cur[k][1] - ac)
    lines = [(k, mi, ac) for k, (mi, ac) in cur.items() if mi > 0]
    allm = sum(l[1] for l in lines)
    print(total)
    region = defaultdict(int)
    cfun = defaultdict(int)
    hotl = []
    for ad, miss, acc in lines:
        if stubs <= ad < hot: region["JIT shared stubs"] += miss
        elif hot <= ad < cold: region["JIT hot region (blocks, exit stubs)"] += miss; hotl.append((ad, miss, acc))
        elif cold <= ad < end: region["JIT cold region (slow paths, block stubs)"] += miss
        else:
            i = bisect.bisect_right(saddr, ad) - 1
            if i >= 0 and ad - saddr[i] < 1 << 16:
                region["gcemu C code"] += miss
                cfun[syms[i][1]] += miss
            else:
                region["other (libc, loader)"] += miss
    print("== misses by region")
    for k, v in sorted(region.items(), key=lambda kv: -kv[1]):
        print(f"  {k:42s} {v:12d}  {100.0 * v / allm:5.1f}%")
    print(f"== gcemu functions (top {top})")
    for k, v in sorted(cfun.items(), key=lambda kv: -kv[1])[:top]:
        print(f"  {k[:60]:60s} {v:10d}  {100.0 * v / allm:5.1f}%")
    hotl.sort(key=lambda t: -t[1])
    hm = sum(t[1] for t in hotl)
    if hm:
        print(f"== JIT hot region: {len(hotl)} lines missed; share of its misses in the top N lines")
        acc = 0
        marks = [16, 64, 256, 1024, 4096, 16384]
        for i, t in enumerate(hotl):
            acc += t[1]
            if i + 1 in marks:
                print(f"  top {i + 1:6d} lines: {100.0 * acc / hm:5.1f}%")
        hits = sum(t[2] - t[1] for t in hotl)
        print(f"  accesses {sum(t[2] for t in hotl)}, misses {hm} ({100.0 * hm / max(1, sum(t[2] for t in hotl)):.2f}% of accesses)")
    bm = opt("--blockmap")
    if bm and hotl:
        blocks = sorted((int(x, 16), int(y, 16)) for x, y in (l.split() for l in open(bm)))
        starts = [b[0] for b in blocks]
        per = defaultdict(int)
        for ad, miss, _ in hotl:
            i = bisect.bisect_right(starts, ad + 31) - 1  # a line can hold the end of one block and the start of the next
            if i >= 0: per[blocks[i][1]] += miss
        print(f"== JIT blocks: {len(per)} blocks with misses; share in the top N blocks")
        ranked = sorted(per.items(), key=lambda kv: -kv[1])
        acc = 0
        for i, (pc, v) in enumerate(ranked):
            acc += v
            if i + 1 in (10, 100, 1000, 3000, 10000):
                print(f"  top {i + 1:6d} blocks: {100.0 * acc / hm:5.1f}%")
        symf = opt("--symbols")
        if symf:
            fs = []
            for line in open(symf, errors="replace"):
                mm = re.match(r"\s*(\S+)\s*=\s*\.text:0x([0-9A-Fa-f]+);.*size:0x([0-9A-Fa-f]+)", line)
                if mm: fs.append((int(mm.group(2), 16), int(mm.group(3), 16), mm.group(1)))
            fs.sort()
            fst = [f[0] for f in fs]
            pf = defaultdict(int)
            for pc, v in per.items():
                i = bisect.bisect_right(fst, pc) - 1
                pf[fs[i][2] if i >= 0 and pc < fs[i][0] + fs[i][1] else f"?{pc:08x}"] += v
            print(f"== guest functions by hot-region misses (top {top})")
            for k, v in sorted(pf.items(), key=lambda kv: -kv[1])[:top]:
                print(f"  {k[:60]:60s} {v:10d}  {100.0 * v / hm:5.1f}%")


main()
