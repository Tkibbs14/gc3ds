#!/usr/bin/env python3
"""Offline model of hammock blocks (docs/research/EMULATOR_CEILING.md H1) over a PC interpreter profile.

    PCHIST=pc.bin EDGEHIST=edges.bin MEMDUMP=mem.bin gcemu <disc> --load-state S --frames N ...
    python3 core/tools/hammock.py pc.bin edges.bin mem.bin [--window 8,16,32,64] [--ps-lo 0x80346000 --ps-hi 0x80348000]

Every dynamic control transfer is classified as staying inside a compiled block or crossing a block boundary under:

- today's JIT (core/src/jit/jit.c): every branch ends a block (b, bc, bclr, bcctr, isync), as do sc, rfi, twi and the
  state-changing op-31 forms; `bl` to the paired-single library (called in place, hle_ps.c) does not;
- **hammock A**: a forward b / bc (no LK) whose target lies within W instructions stays inside the block, both ways;
  every other branch still ends it;
- **hammock B**: as A, and a conditional bc that does not qualify leaves only when taken (its fall-through stays in
  the block, a side exit; no code is followed, so nothing is duplicated).

Reported per model: guest instructions per block boundary (the average dynamic block length) and the share of
boundaries removed. Also the conditional branches whose CR field was set by a compare (or a record form) earlier in the
same block, in each model: the pairs that could keep the compare in ARM flags, and how many of those compares have
their field overwritten in the same block before any exit (dead at the exits: never written back).

Also the calls (bl): the share whose target is a small leaf (reaches its blr within N instructions with no call, no
stack frame, no loop and no tail jump), each a call and a return boundary an inlining JIT would remove (H5), and the
share that goes to the GPR save / restore entries (--gpr-lo / --gpr-hi, TP's _savegpr_14.._restgpr_31). And
function-scope regions (every direct b / bc within a span stays inside, loops included, with and without inlined
leaves) and how much guest code holds 50-99% of the executed instructions.

Limits: a PC profile (the title's cutscene / later stretch, not gameplay); the 64-instruction block cap, page ends
and register pressure across internal edges are not modelled.
"""
import struct
import sys
from collections import defaultdict

BASE = 0x80000000


def load(pcp, edgep, memp):
    counts = memoryview(open(pcp, 'rb').read()).cast('I')
    raw = open(edgep, 'rb').read()
    edges = defaultdict(list)
    taken = defaultdict(int)
    for k in range(0, len(raw), 12):
        f, t, c = struct.unpack_from('<III', raw, k)
        edges[f].append((t, c))
        taken[f] += c
    mem = open(memp, 'rb').read()
    return counts, edges, taken, mem


def word(mem, pc):
    a = pc - BASE
    if a < 0 or a + 4 > len(mem):
        return 0
    return struct.unpack_from('>I', mem, a)[0]


def kind(inst):
    """('b'|'bc'|'bclr'|'bcctr'|'end'|None, lk, target offset or None)."""
    op = inst >> 26
    if op == 18:
        li = ((inst & 0x03FFFFFC) ^ 0x02000000) - 0x02000000
        return 'b', inst & 1, None if inst & 2 else li
    if op == 16:
        bd = ((inst & 0xFFFC) ^ 0x8000) - 0x8000
        return 'bc', inst & 1, None if inst & 2 else bd
    if op == 19:
        xo = (inst >> 1) & 0x3FF
        if xo == 16:
            return 'bclr', inst & 1, None
        if xo == 528:
            return 'bcctr', inst & 1, None
        if xo in (50, 150):
            return 'end', 0, None
    if op == 17 or op == 3:
        return 'end', 0, None
    if op == 31 and ((inst >> 1) & 0x3FF) == 467:  # mtspr: compiled in place for these SPRs (jit.c case 467)
        spr = ((inst >> 16) & 31) | ((inst >> 6) & 0x3E0)
        if spr in (1, 8, 9, 18, 19, 26, 27, 1008) or 272 <= spr < 276 or 912 <= spr < 920:
            return None, 0, None
        return 'end', 0, None
    if op == 31 and ((inst >> 1) & 0x3FF) in (146, 210, 242, 982, 4, 598):
        return 'end', 0, None
    if op == 63 and ((inst >> 1) & 0x3FF) in (38, 70, 134, 711):  # mtfsb1 mtfsb0 mtfsfi mtfsf (jit.c ends_block)
        return 'end', 0, None
    return None, 0, None


def cr_written(inst):
    """The CR field (0-7) a compare or record form writes, else None; -1 for 'unknown, writes CR broadly'."""
    op = inst >> 26
    if op in (10, 11):  # cmpli, cmpi
        return (inst >> 23) & 7
    if op == 31 and ((inst >> 1) & 0x3FF) in (0, 32):  # cmp, cmpl
        return (inst >> 23) & 7
    if op == 63 and ((inst >> 1) & 0x3FF) in (0, 32):  # fcmpu, fcmpo
        return (inst >> 23) & 7
    if op == 4 and ((inst >> 1) & 0x3FF) in (0, 32, 64, 96):  # ps_cmp*
        return (inst >> 23) & 7
    if op in (28, 29):  # andi., andis.
        return 0
    if op == 31 and (inst & 1):  # record forms
        return 0
    if op in (59, 63, 4) and (inst & 1):  # float record forms: CR1
        return 1
    if op == 19 and ((inst >> 1) & 0x3FF) in (33, 129, 193, 225, 257, 289, 417, 449, 0):  # CR logic, mcrf
        return (inst >> 23) & 7 if ((inst >> 1) & 0x3FF) == 0 else ((inst >> 21) & 31) >> 2
    if op == 31 and ((inst >> 1) & 0x3FF) in (144, 512):  # mtcrf, mcrxr
        return -1
    if op == 63 and ((inst >> 1) & 0x3FF) == 64:  # mcrfs
        return (inst >> 23) & 7
    if op in (21, 20, 23) and (inst & 1):  # rlwinm., rlwimi., rlwnm.
        return 0
    if op in (24, 25, 26, 27):  # ori / xori ... (not record)
        return None
    return None


def reads_cr(inst):
    """CR fields a non-branch instruction reads (mfcr, CR logic, mcrf): a set, or None."""
    op = inst >> 26
    if op == 31 and ((inst >> 1) & 0x3FF) == 19:  # mfcr
        return set(range(8))
    if op == 19:
        xo = (inst >> 1) & 0x3FF
        if xo == 0:
            return {(inst >> 18) & 7}
        if xo in (33, 129, 193, 225, 257, 289, 417, 449):
            return {((inst >> 16) & 31) >> 2, ((inst >> 11) & 31) >> 2}
    return None


def main():
    args = sys.argv[1:]
    windows = [8, 16, 32, 64]
    ps_lo, ps_hi = 0x80346000, 0x80348000
    gpr_lo, gpr_hi = 0x803621A0, 0x8036223C
    if '--window' in args:
        i = args.index('--window'); windows = [int(x) for x in args[i + 1].split(',')]; del args[i:i + 2]
    if '--ps-lo' in args:
        i = args.index('--ps-lo'); ps_lo = int(args[i + 1], 0); del args[i:i + 2]
    if '--ps-hi' in args:
        i = args.index('--ps-hi'); ps_hi = int(args[i + 1], 0); del args[i:i + 2]
    if '--gpr-lo' in args:
        i = args.index('--gpr-lo'); gpr_lo = int(args[i + 1], 0); del args[i:i + 2]
    if '--gpr-hi' in args:
        i = args.index('--gpr-hi'); gpr_hi = int(args[i + 1], 0); del args[i:i + 2]
    counts, edges, taken, mem = load(*args[:3])

    executed = [(BASE + 4 * i, c) for i, c in enumerate(counts) if c]
    total = sum(c for _, c in executed)
    info = {}
    for pc, c in executed:
        inst = word(mem, pc)
        k, lk, off = kind(inst)
        if k is None:
            continue
        if k == 'b' and lk and off is not None and ps_lo <= pc + off < ps_hi:
            continue  # bl to the PS library: in place
        info[pc] = (inst, k, lk, off, c)

    def near(pc, k, lk, off, w):
        return k in ('b', 'bc') and not lk and off is not None and 0 < off <= 4 * w

    print('guest instructions profiled: %d (%d distinct addresses); block-ending instructions executed: %d'
          % (total, len(executed), sum(v[4] for v in info.values())))
    cur = sum(v[4] for v in info.values())
    print('today: %.2f guest instructions per block boundary (%d boundaries)' % (total / cur, cur))
    # where today's boundaries come from
    by = defaultdict(int)
    for pc, (inst, k, lk, off, c) in info.items():
        tag = k + ('l' if lk else '')
        if k in ('b', 'bc') and off is not None:
            tag += ' fwd' if off > 0 else ' back'
        by[tag] += c
    print('  boundaries by kind: ' + ', '.join('%s %.1f%%' % (t, 100.0 * n / cur) for t, n in sorted(by.items(), key=lambda x: -x[1])))
    for w in windows:
        a = b_ = 0
        for pc, (inst, k, lk, off, c) in info.items():
            if near(pc, k, lk, off, w):
                continue
            a += c
            if k == 'bc' and not lk:
                b_ += taken.get(pc, 0)
            else:
                b_ += c
        print('window %2d: hammock A %.2f per boundary (-%.1f%% boundaries), hammock B %.2f (-%.1f%%)'
              % (w, total / max(a, 1), 100.0 * (cur - a) / cur, total / max(b_, 1), 100.0 * (cur - b_) / cur))

    # compare / branch pairs: for each executed conditional bc reading a CR field, look back within its block for the
    # instruction that wrote that field. Block starts: today = after the previous block-ending instruction; hammock A
    # (window w) = after the previous instruction that still ends a block.
    def pairs(w):
        fused = dead = condexec = 0
        for pc, (inst, k, lk, off, c) in info.items():
            if k not in ('bc', 'bclr', 'bcctr'):
                continue
            bo = (inst >> 21) & 31
            if bo & 0x10:
                continue  # no CR test
            condexec += c
            field = ((inst >> 16) & 31) >> 2
            p = pc - 4
            found = False
            for _ in range(4 * 64):
                if p < BASE:
                    break
                q = word(mem, p)
                qk, qlk, qoff = kind(q)
                if qk is not None and not (qk == 'b' and qlk and qoff is not None and ps_lo <= p + qoff < ps_hi):
                    if w is None or not near(p, qk, qlk, qoff, w):
                        break  # a block boundary before the writer
                f = cr_written(q)
                if f == field or f == -1:
                    found = f == field
                    break
                p -= 4
            if not found:
                continue
            fused += c
            # Dead at the exits: the field written again later in the same block before any read or exit, on both
            # paths. Today the branch is itself an exit (never dead); in a hammock block the rewrite must come at or
            # after the branch's target, where both paths have joined.
            if w is None or not near(pc, k, lk, off, w):
                continue
            join = pc + off
            p = pc + 4
            for _ in range(4 * 64):
                q = word(mem, p)
                qk, qlk, qoff = kind(q)
                rd = reads_cr(q)
                if rd and field in rd:
                    break
                if qk is not None:
                    if qk in ('bc', 'bclr', 'bcctr') and not ((q >> 21) & 0x10) and (((q >> 16) & 31) >> 2) == field:
                        break  # read again
                    if w is None or not near(p, qk, qlk, qoff, w):
                        break  # an exit: the field must be in guest state there
                f = cr_written(q)
                if f == field:
                    if p >= join:
                        dead += c
                    break
                p += 4
        return condexec, fused, dead

    ce, f0, d0 = pairs(None)
    print('conditional branches on CR: %d executed; compare in the same block today %.1f%% (dead at its exits %.1f%%)'
          % (ce, 100.0 * f0 / max(ce, 1), 100.0 * d0 / max(ce, 1)))
    for w in windows:
        _, f, d = pairs(w)
        print('  window %2d: compare in the same hammock block %.1f%%, its field dead at every exit %.1f%%'
              % (w, 100.0 * f / max(ce, 1), 100.0 * d / max(ce, 1)))

    # calls: the share to small leaves and to the GPR save / restore entries
    calls = defaultdict(int)
    for f, outs in edges.items():
        w = word(mem, f)
        if (w >> 26) == 18 and (w & 1):
            for t, c in outs:
                calls[t] += c
    ncalls = sum(calls.values())

    def leaf(t, n):
        for i in range(n):
            q = word(mem, t + 4 * i)
            op = q >> 26
            if op == 19 and ((q >> 1) & 0x3FF) == 16 and ((q >> 21) & 20) == 20:
                return True  # an unconditional blr
            if op == 18 and (q & 1):
                return False  # a call
            if op == 37 and ((q >> 16) & 31) == 1:
                return False  # stwu r1: a stack frame
            if op in (16, 18) and not (q & 2):
                off = ((q & 0x03FFFFFC) ^ 0x02000000) - 0x02000000 if op == 18 else ((q & 0xFFFC) ^ 0x8000) - 0x8000
                if off <= 0 or op == 18:
                    return False  # a loop or a tail jump
        return False
    gpr = sum(c for t, c in calls.items() if gpr_lo <= t < gpr_hi)
    print('calls: %d (%.2f%% of guest instructions); to GPR save / restore %.1f%%; to leaves within '
          % (ncalls, 100.0 * ncalls / total, 100.0 * gpr / max(ncalls, 1))
          + ', '.join('%d: %.1f%%' % (n, 100.0 * sum(c for t, c in calls.items() if leaf(t, n)) / max(ncalls, 1))
                      for n in (8, 16, 32)))
    top = sorted(calls.items(), key=lambda x: -x[1])[:8]
    print('  top call targets: ' + ', '.join('%08x %.1f%%' % (t, 100.0 * c / max(ncalls, 1)) for t, c in top))

    # function-scope regions (GENERAL_ARCHITECTURE.md section 2): every direct non-linking b / bc within +-span bytes
    # stays inside, both directions (loops too); then also calls to leaves under 16 instructions inlined, each removing
    # its bl and one execution of the leaf's blr
    inl = 0
    for f, outs in edges.items():
        w = word(mem, f)
        if (w >> 26) == 18 and (w & 1) and f in info:
            inl += 2 * sum(c for t, c in outs if leaf(t, 16))
    for span in (256, 4096):
        rem = sum(c for pc, (inst, k, lk, off, c) in info.items()
                  if k in ('b', 'bc') and not lk and off is not None and -span <= off <= span)
        a_, b_ = cur - rem, cur - rem - inl
        print('regions +-%d bytes: %.2f per boundary (-%.1f%%); with leaves under 16 inlined %.2f (-%.1f%%)'
              % (span, total / max(a_, 1), 100.0 * rem / cur, total / max(b_, 1), 100.0 * (cur - b_) / cur))
    hot = sorted((c for _, c in executed), reverse=True)
    acc, marks, out = 0, [0.5, 0.8, 0.9, 0.95, 0.99], []
    for n, c in enumerate(hot, 1):
        acc += c
        while marks and acc >= marks[0] * total:
            out.append('%d%% in %d KB' % (marks[0] * 100, n * 4 // 1024))
            marks.pop(0)
    print('guest code holding the executed instructions: ' + ', '.join(out))


if __name__ == '__main__':
    main()
