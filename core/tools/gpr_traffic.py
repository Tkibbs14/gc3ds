#!/usr/bin/env python3
"""Where compiled code's data accesses go, and how much guest-GPR traffic the register cache leaves behind (C51/C47).

    python3 core/tools/gpr_traffic.py /path/to/sched.bin [--top N]

Reads a `JIT_SCHEDDUMP` (the same dump `arm11_sched.py` takes: run the ARM build under qemu with
`JIT_REGSTAT=1 JIT_SCHEDDUMP=path`) and classifies every hot host instruction of every executed block, weighted by
the block's execution count, by **what it touches**:

  * **guest GPRs** - `ldr`/`str` through RG (r11) at an offset inside `Gekko.gpr` (0-127). These are the register
    cache's spills and fills: the accesses that exist *because* 5 host registers (r4-r8) hold 32 guest GPRs.
  * **guest floats** - the same through RG inside `Gekko.fpr` (128-639), after OPT_FCACHE's lanes.
  * **other Gekko state** - cr, xer, lr, ctr, pc, the jitFn table and the rest (640+).
  * **guest memory** - anything based on RRAM (r10), i.e. the emulated RAM the game is actually reading.
  * **elsewhere** - the stack, literals, and bases the dump cannot attribute.

Why this exists. **C51** asks whether guest GPRs should spill to VFP registers instead of memory, and its first
question is the ceiling: if GPR spills and fills are a small share of hot words, no spill scheme can pay, however
clever - ARM->VFP transfers alone are already 8.5% of modelled stalls. **C47** asks how core 0's 22.1% data stalls
divide between guest RAM, Gekko state and JIT metadata, and a weighted count of the *accesses* is the cheap first cut
of that split (it is not the miss split - these are accesses, most of which hit L1).

It also reports what the register cache is up against: the number of **distinct guest GPRs a block spills or fills**.
If hot blocks touch five or fewer, the cache is big enough and C51 is pointless; if they routinely touch a dozen, the
spilling is forced by pressure and a cheaper spill destination is worth having. Nothing had measured this.

**What it is not.** A jitbench case is a loop shaped like hot game code, not the game's instruction mix, so the
shares here rank candidates and bound them - they do not predict a device number. The model's own blind spot applies
too: this counts accesses, and says nothing about which of them miss.
"""
import struct
import sys
from collections import defaultdict

GPR_LO, GPR_HI = 0, 128          # Gekko.gpr[0..31]  (core/src/cpu/gekko.h, checked with offsetof)
FPR_LO, FPR_HI = 128, 640        # Gekko.fpr[0..31], 16 bytes each
RG, RRAM, SP = 11, 10, 13        # jit.c: RG = Gekko*, RRAM = ram - 0x80000000, r13 = host stack


def classify(w):
    """-> (kind, guest_gpr_or_None, is_store). kind is one of the bullet names in the docstring."""
    cond = w >> 28
    if cond == 15:
        return None, None, False
    top = (w >> 25) & 7
    if top in (2, 3):  # 010/011: single data transfer (immediate / register offset)
        imm = top == 2
        rn = (w >> 16) & 15
        load = (w >> 20) & 1
        off = w & 0xFFF if imm else None
        up = (w >> 23) & 1
        if rn == RG and imm and up:
            if GPR_LO <= off < GPR_HI:
                return 'guest GPRs', off // 4, not load
            if FPR_LO <= off < FPR_HI:
                return 'guest floats', None, not load
            return 'other Gekko state', None, not load
        if rn == RRAM:
            return 'guest memory', None, not load
        if rn == SP:
            return 'elsewhere', None, not load
        return 'elsewhere', None, not load
    if top == 4:  # 100: LDM/STM
        rn = (w >> 16) & 15
        load = (w >> 20) & 1
        if rn == RG:
            return 'other Gekko state', None, not load
        if rn == RRAM:
            return 'guest memory', None, not load
        return 'elsewhere', None, not load
    if top == 6:  # 110: coprocessor data transfer (VLDR/VSTR, VPUSH/VPOP)
        rn = (w >> 16) & 15
        load = (w >> 20) & 1
        off = (w & 0xFF) * 4
        up = (w >> 23) & 1
        if rn == RG and up:
            if FPR_LO <= off < FPR_HI:
                return 'guest floats', None, not load
            if GPR_LO <= off < GPR_HI:
                return 'guest GPRs', off // 4, not load
            return 'other Gekko state', None, not load
        if rn == RRAM:
            return 'guest memory', None, not load
        return 'elsewhere', None, not load
    return None, None, False


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    top = 12
    for a in sys.argv[1:]:
        if a.startswith('--top'):
            top = int(a.split('=', 1)[1]) if '=' in a else 12
    if not args:
        print(__doc__)
        return 2
    data = open(args[0], 'rb').read()

    kinds = defaultdict(float)          # kind -> executed host words
    stores = defaultdict(float)
    total = 0.0
    guest_total = 0.0
    gpr_hist = defaultdict(float)       # distinct guest GPRs spilled/filled in a block -> executed blocks
    per_gpr = defaultdict(float)        # guest GPR number -> executed accesses
    blocks = 0

    pos = 0
    while pos + 4 <= len(data):
        (magic,) = struct.unpack_from('<I', data, pos)
        if magic == 0x42555453:  # 'STUB': the shared stubs, not a block
            _, base, nw = struct.unpack_from('<3I', data, pos)
            pos += 12 + 4 * nw
            continue
        if magic != 0x4B4C4222:  # the block record rs_fold writes (jit.c: hdr[0])
            # Loud, not skipped: a dump whose format moved would otherwise be read as "few accesses" and the
            # conclusion ("C51 cannot pay") would be an artifact of the parser. arm11_sched.py exits here too.
            sys.exit('bad record at %d: the dump format moved, or this is not a JIT_SCHEDDUMP' % pos)
        _, pc, hot, pro, ndop, fn = struct.unpack_from('<6I', data, pos)
        (n,) = struct.unpack_from('<Q', data, pos + 24)
        pos += 32
        words = struct.unpack_from('<%dI' % hot, data, pos)
        pos += 4 * hot
        dops = struct.unpack_from('<%dI' % ndop, data, pos)
        pos += 4 * ndop
        if not n:
            continue
        blocks += 1
        seen = set()
        for w in words:
            kind, g, st = classify(w)
            total += n
            if kind is None:
                continue
            kinds[kind] += n
            if st:
                stores[kind] += n
            if g is not None:
                seen.add(g)
                per_gpr[g] += n
        gpr_hist[len(seen)] += n
        guest_total += n * max(ndop, 1)

    if not total:
        print('no executed blocks in %s (run the ARM build with JIT_REGSTAT=1 JIT_SCHEDDUMP=path first)' % args[0])
        return 1

    print('%s: %d blocks with a non-zero count, %.3g executed hot words' % (args[0], blocks, total))
    print()
    print('%-20s %12s %8s %10s' % ('what it touches', 'exec words', 'share', 'of them stores'))
    for k in ('guest GPRs', 'guest floats', 'other Gekko state', 'guest memory', 'elsewhere'):
        v = kinds.get(k, 0.0)
        if not v:
            continue
        print('%-20s %12.4g %7.2f%% %9.1f%%' % (k, v, 100.0 * v / total, 100.0 * stores.get(k, 0.0) / v))
    acc = sum(kinds.get(k, 0) for k in ('guest GPRs', 'guest floats', 'other Gekko state', 'guest memory', 'elsewhere'))
    print('%-20s %12.4g %7.2f%%' % ('(all accesses)', acc, 100.0 * acc / total))
    print()
    g = kinds.get('guest GPRs', 0.0)
    print('C51\'s ceiling: guest-GPR spills and fills are %.2f%% of executed hot words.' % (100.0 * g / total))
    print('   Even if a VFP spill made each one free, that is the most any spill scheme can remove - and ARM<->VFP')
    print('   transfers are not free (8.5%% of modelled stalls already, LAT_ARM_TO_VFP 4 cycles, VFP_TO_ARM 2).')
    print()
    print('Distinct guest GPRs a block spills or fills (weighted by executions):')
    tot_b = sum(gpr_hist.values()) or 1.0
    for k in sorted(gpr_hist):
        print('   %2d GPRs: %6.2f%% of executed blocks' % (k, 100.0 * gpr_hist[k] / tot_b))
    mean = sum(k * v for k, v in gpr_hist.items()) / tot_b
    print('   mean %.2f distinct GPRs a block; the cache holds 5 (r4-r8)' % mean)
    if per_gpr:
        print()
        print('Busiest guest GPRs through memory:', ', '.join(
            'r%d %.1f%%' % (k, 100.0 * v / g) for k, v in sorted(per_gpr.items(), key=lambda kv: -kv[1])[:top]))
    return 0


if __name__ == '__main__':
    sys.exit(main())
