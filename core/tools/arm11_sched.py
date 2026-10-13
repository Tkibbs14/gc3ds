#!/usr/bin/env python3
"""ARM11 interlock model over compiled blocks (JIT_SCHEDDUMP, see jit.c rs_fold).

Each executed block's hot code is run through an in-order issue model with the ARM11 MPCore / VFP11 result
latencies (ARM DDI 0360F ch. 15-16, as recorded in docs/HANDOFF.md) and weighted by the block's execution count.
Branches are taken as falling through (the hot path); the budget check and the statistics counter at a block's start
are skipped. Reports stall cycles per executed instruction (the 2DS counter: ~0.64 data-dependency stall cycles per
instruction), by class of producer and by guest opcode, so that scheduling work goes where the stalls are.

    python3 arm11_sched.py /root/sched.bin [--top N]
"""
import struct
import sys
from collections import defaultdict

# Result latencies in cycles (the consumer issues at least this many cycles after the producer). EARLY: one more
# when the consumer needs the register in its first stage (shifted operand or shift amount, address base or index).
LAT_ALU, LAT_LOAD, LAT_MUL, LAT_MEDIA = 1, 3, 3, 1
LAT_VFP_ADD, LAT_VFP_MULS, LAT_VFP_MULD, LAT_VFP_LOAD, LAT_VFP_MISC, LAT_VFP_CVT = 8, 8, 9, 4, 4, 4
LAT_VFP_TO_ARM, LAT_ARM_TO_VFP, LAT_VFP_DIVS, LAT_VFP_DIVD, LAT_FCMP = 2, 4, 19, 33, 4
EARLY = 1
LATE = 1  # store data and VFP store data are read a cycle after issue

FLAGS = 16  # pseudo register: NZCV (ARM)
FPFLAGS = 17  # FPSCR flags (FCMP -> FMSTAT)
S0 = 32  # VFP single registers S0..S31 -> 32..63 (a double Dn is S2n, S2n+1)


def sreg_single(w, lo, hibit):
    return S0 + (((w >> lo) & 15) << 1 | ((w >> hibit) & 1))


def sreg_double(w, lo):
    d = (w >> lo) & 15
    return [S0 + 2 * d, S0 + 2 * d + 1]


def decode(w):
    """-> (srcs [(reg, early_or_late_adjust)], dsts [reg], latency, issue cycles, class name)"""
    cond = w >> 28
    srcs, dsts = [], []
    if cond != 14 and cond != 15:
        srcs.append((FLAGS, 0))
    if cond == 15:
        return srcs, dsts, 1, 1, 'other'
    op = (w >> 25) & 7
    rn, rd, rs, rm = (w >> 16) & 15, (w >> 12) & 15, (w >> 8) & 15, w & 15
    if op == 0:
        if (w & 0x0FFFFFF0) in (0x012FFF10, 0x012FFF30):  # bx / blx reg
            return [(rm, 0)] + srcs, ([14] if w & 0x20 else []), 1, 1, 'branch'
        if (w >> 4) & 15 == 9 and (w >> 24) & 15 == 0:  # multiply
            if (w >> 23) & 1:  # long
                d = [rn, rd]
                s = [(rm, 0), (rs, 0)] + ([(rn, 0), (rd, 0)] if (w >> 21) & 1 else [])
            else:
                d = [rn]
                s = [(rm, 0), (rs, 0)] + ([(rd, 0)] if (w >> 21) & 1 else [])
            if (w >> 20) & 1:
                d.append(FLAGS)
            return s + srcs, d, LAT_MUL, 2, 'mul'
        if (w >> 7) & 1 and (w >> 4) & 1:  # extra load / store: ldrh strh ldrsb ldrsh ldrd strd
            load = (w >> 20) & 1
            sh = (w >> 5) & 3
            s = [(rn, EARLY)] + ([] if (w >> 22) & 1 else [(rm, EARLY)])
            wb = [rn] if ((w >> 21) & 1) or not ((w >> 24) & 1) else []
            if not load and sh in (2, 3):  # ldrd (sh 2) / strd (sh 3) with L = 0
                if sh == 2:
                    return s + srcs, [rd, rd + 1] + wb, LAT_LOAD, 1, 'load'
                return s + [(rd, -LATE), (rd + 1, -LATE)] + srcs, wb, LAT_ALU, 1, 'store'
            if load:
                return s + srcs, [rd] + wb, LAT_LOAD, 1, 'load'
            return s + [(rd, -LATE)] + srcs, wb, LAT_ALU, 1, 'store'
        if (w & 0x0FBF0FFF) == 0x010F0000:  # mrs
            return [(FLAGS, 0)] + srcs, [rd], LAT_ALU, 1, 'alu'
        if (w & 0x0FB0FFF0) == 0x0120F000:  # msr reg
            return [(rm, 0)] + srcs, [FLAGS], LAT_ALU, 1, 'alu'
        if (w & 0x0FFF0FF0) == 0x016F0F10:  # clz
            return [(rm, 0)] + srcs, [rd], LAT_ALU, 1, 'alu'
        # data processing, register operand
        opc, s_bit = (w >> 21) & 15, (w >> 20) & 1
        s = []
        if opc not in (13, 15):  # mov, mvn have no Rn
            s.append((rn, 0))
        if (w >> 4) & 1:
            s += [(rm, EARLY), (rs, EARLY)]
        else:
            shifted = ((w >> 7) & 31) != 0 or ((w >> 5) & 3) != 0
            s.append((rm, EARLY if shifted else 0))
            if ((w >> 5) & 3) == 3 and ((w >> 7) & 31) == 0:  # rrx reads the carry
                s.append((FLAGS, 0))
        d = [] if opc in (8, 9, 10, 11) else [rd]
        if s_bit or opc in (8, 9, 10, 11):
            d.append(FLAGS)
        if opc in (5, 6, 7):  # adc sbc rsc
            s.append((FLAGS, 0))
        return s + srcs, d, LAT_ALU, 1, 'alu'
    if op == 1:
        if (w >> 23) & 3 == 2 and not (w >> 20) & 1:  # msr imm
            return srcs, [FLAGS], LAT_ALU, 1, 'alu'
        opc, s_bit = (w >> 21) & 15, (w >> 20) & 1
        s = [] if opc in (13, 15) else [(rn, 0)]
        d = [] if opc in (8, 9, 10, 11) else [rd]
        if s_bit or opc in (8, 9, 10, 11):
            d.append(FLAGS)
        if opc in (5, 6, 7):
            s.append((FLAGS, 0))
        return s + srcs, d, LAT_ALU, 1, 'alu'
    if op in (2, 3):
        if op == 3 and (w >> 4) & 1:  # media: rev, rev16, revsh, sxth, uxth, sxtb, uxtb (+ add forms)
            s = [(rm, 0)]
            if rn != 15 and (w >> 23) & 3 == 1:
                s.append((rn, 0))
            return s + srcs, [rd], LAT_MEDIA, 1, 'media'
        load = (w >> 20) & 1
        s = [(rn, EARLY)]
        if op == 3:
            s.append((rm, EARLY))
        wb = [rn] if ((w >> 21) & 1) or not ((w >> 24) & 1) else []
        if load:
            if rd == 15:
                return s + srcs, wb, 1, 1, 'branch'
            return s + srcs, [rd] + wb, LAT_LOAD, 1, 'load'
        return s + [(rd, -LATE)] + srcs, wb, LAT_ALU, 1, 'store'
    if op == 4:  # ldm / stm
        regs = [r for r in range(16) if (w >> r) & 1]
        load = (w >> 20) & 1
        wb = [rn] if (w >> 21) & 1 else []
        cycles = max(1, (len(regs) + 1) // 2)
        if load:
            return [(rn, EARLY)] + srcs, [r for r in regs if r != 15] + wb, LAT_LOAD + cycles - 1, cycles, 'load'
        return [(rn, EARLY)] + [(r, -LATE) for r in regs] + srcs, wb, LAT_ALU, cycles, 'store'
    if op == 5:  # b / bl
        return srcs, ([14] if (w >> 24) & 1 else []), 1, 1, 'branch'
    cp = (w >> 8) & 15
    if op == 6:
        if (w >> 21) & 0x7F == 0x62 and cp in (10, 11):  # mcrr / mrrc: fmdrr / fmrrd
            dd = sreg_double(w, 0)
            if (w >> 20) & 1:  # fmrrd: Dm -> rd, rn
                return [(r, 0) for r in dd] + srcs, [rd, rn], LAT_VFP_TO_ARM, 1, 'vfp->arm'
            return [(rd, 0), (rn, 0)] + srcs, dd, LAT_ARM_TO_VFP, 1, 'arm->vfp'
        if cp in (10, 11):  # vldr / vstr / vldm / vstm
            load = (w >> 20) & 1
            regs = sreg_double(w, 12) if cp == 11 else [sreg_single(w, 12, 22)]
            n = (w & 255) // (2 if cp == 11 else 1) if (w >> 23) & 1 or (w >> 21) & 1 else 1
            if ((w >> 24) & 1) and not ((w >> 21) & 1):  # vldr / vstr: one register
                n = 1
            allregs = []
            first = (w >> 12) & 15
            for k in range(max(1, n)):
                if cp == 11:
                    allregs += [S0 + 2 * (first + k), S0 + 2 * (first + k) + 1]
                else:
                    allregs.append(S0 + ((first << 1 | ((w >> 22) & 1)) + k))
            wb = [rn] if (w >> 21) & 1 else []
            if load:
                return [(rn, EARLY)] + srcs, allregs + wb, LAT_VFP_LOAD, max(1, n), 'vfp load'
            return [(rn, EARLY)] + [(r, -LATE) for r in allregs] + srcs, wb, LAT_ALU, max(1, n), 'vfp store'
        return srcs, [], 1, 1, 'other'
    # op == 7
    if (w >> 24) & 15 == 15:
        return srcs, [], 1, 1, 'other'  # svc
    if cp not in (10, 11):
        return srcs, [], 1, 1, 'other'
    if (w >> 4) & 1:  # mcr / mrc: fmsr / fmrs / fmrx / fmxr / fmstat
        load = (w >> 20) & 1
        if (w & 0x0FFF0FFF) == 0x0EF1FA10:  # fmstat
            return [(FPFLAGS, 0)] + srcs, [FLAGS], 1, 1, 'vfp->arm'
        if ((w >> 21) & 7) == 7:  # fmrx / fmxr (system registers)
            return ([] if load else [(rd, 0)]) + srcs, ([rd] if load else []), 2, 1, 'other'
        sn = sreg_single(w, 16, 7)
        if load:  # fmrs
            return [(sn, 0)] + srcs, [rd], LAT_VFP_TO_ARM, 1, 'vfp->arm'
        return [(rd, 0)] + srcs, [sn], LAT_ARM_TO_VFP, 1, 'arm->vfp'
    # VFP data processing
    dbl = cp == 11
    p, q, r, s = (w >> 23) & 1, (w >> 21) & 1, (w >> 20) & 1, (w >> 6) & 1
    code = p << 3 | q << 2 | r << 1 | s

    def vd():
        return sreg_double(w, 12) if dbl else [sreg_single(w, 12, 22)]

    def vn():
        return sreg_double(w, 16) if dbl else [sreg_single(w, 16, 7)]

    def vm(d=None):
        d = dbl if d is None else d
        return sreg_double(w, 0) if d else [sreg_single(w, 0, 5)]

    if code == 15:  # extension: unary
        ext = ((w >> 16) & 15) << 1 | ((w >> 7) & 1)
        if ext in (8, 9, 10, 11):  # fcmp fcmpe fcmpz fcmpez
            s_ = [(x, 0) for x in vd()] + ([] if ext in (10, 11) else [(x, 0) for x in vm()])
            return s_ + srcs, [FPFLAGS], LAT_FCMP, 1, 'vfp'
        if ext == 15:  # fcvtds / fcvtsd: the other precision for the destination
            d_ = [sreg_single(w, 12, 22)] if dbl else sreg_double(w, 12)
            return [(x, 0) for x in vm()] + srcs, d_, LAT_VFP_CVT, 1, 'vfp'
        if ext in (16, 17):  # fuito / fsito: single source
            return [(x, 0) for x in vm(False)] + srcs, vd(), LAT_VFP_ADD, 1, 'vfp'
        if ext in (24, 25, 26, 27):  # ftoui / ftosi: single destination
            return [(x, 0) for x in vm()] + srcs, [sreg_single(w, 12, 22)], LAT_VFP_ADD, 1, 'vfp'
        if ext == 3:  # fsqrt
            return [(x, 0) for x in vm()] + srcs, vd(), LAT_VFP_DIVD if dbl else LAT_VFP_DIVS, (29 if dbl else 15), 'vfp div'
        return [(x, 0) for x in vm()] + srcs, vd(), LAT_VFP_MISC, 1, 'vfp'  # fcpy fabs fneg
    s_ = [(x, 0) for x in vn()] + [(x, 0) for x in vm()]
    if code <= 3:  # fmac fnmac fmsc fnmsc: read the accumulator
        s_ += [(x, 0) for x in vd()]
        return s_ + srcs, vd(), LAT_VFP_MULD if dbl else LAT_VFP_MULS, 2 if dbl else 1, 'vfp'
    if code in (4, 5):
        return s_ + srcs, vd(), LAT_VFP_MULD if dbl else LAT_VFP_MULS, 2 if dbl else 1, 'vfp'
    if code in (6, 7):
        return s_ + srcs, vd(), LAT_VFP_ADD, 1, 'vfp'
    if code == 8:
        return s_ + srcs, vd(), LAT_VFP_DIVD if dbl else LAT_VFP_DIVS, (29 if dbl else 15), 'vfp div'
    return s_ + srcs, vd(), LAT_VFP_ADD, 1, 'vfp'


DECODED = {}
STUBS = {'base': 0, 'words': ()}


def strip_counters(words):
    """Indices of the statistics build's counters (load_const r12; ldr lr, [r12]; add lr, lr, #1; str lr, [r12]),
    which the 2DS build does not have."""
    skip = set()
    for k in range(len(words) - 2):
        if words[k] == 0xE59CE000 and words[k + 1] == 0xE28EE001 and words[k + 2] == 0xE58CE000:
            skip.update((k, k + 1, k + 2))
            i = k - 1
            while i >= 0 and ((words[i] & 0x0FEFF000) == 0x03A0C000 or (words[i] & 0x0FFFF000) == 0x038CC000):
                skip.add(i)
                i -= 1
    return skip


def stub_path(target):
    """The words a call into the shared stubs runs on its fast path: conditional branches and returns not taken,
    unconditional branches followed, up to the unconditional return."""
    base, sw = STUBS['base'], STUBS['words']
    k, out = (target - base) // 4, []
    while 0 <= k < len(sw) and len(out) < 300:
        w = sw[k]
        out.append(w)
        if w == 0xE12FFF1E:  # bx lr
            break
        if (w >> 24) == 0xEA:  # b (always)
            k += 1 + ((w & 0xFFFFFF) ^ 0x800000) - 0x800000 + 1
            continue
        k += 1
    return out


def issue(w, ready, t):
    if w not in DECODED:
        DECODED[w] = decode(w)
    srcs, dsts, lat, cyc, cls = DECODED[w]
    need, why = t, None
    for reg, adj in srcs:
        if reg in ready:
            avail, pcls = ready[reg]
            at = avail + adj
            if at > need:
                need, why = at, pcls
    for reg in dsts:
        ready[reg] = (need + lat, cls)
    return need + cyc, need - t, why


def run_block(words, addr, skip):
    """-> per word: (stall cycles including a called stub's, producer class, words executed including the stub's)"""
    ready = {}  # reg -> (cycle available, producer class)
    t = 0
    out = []
    for k, w in enumerate(words):
        if k in skip:
            out.append(None)
            continue
        t, stall, why = issue(w, ready, t)
        ran = 1
        if (w >> 24) == 0xEB and STUBS['words']:  # bl: into the shared stubs?
            target = addr + 4 * k + 8 + 4 * (((w & 0xFFFFFF) ^ 0x800000) - 0x800000)
            if STUBS['base'] <= target < STUBS['base'] + 4 * len(STUBS['words']):
                ready[14] = (t, 'branch')
                for sw in stub_path(target):
                    t, st, wy = issue(sw, ready, t)
                    if st > stall:
                        why = wy
                    stall += st
                    ran += 1
        out.append((stall, why, ran))
    return out


def main():
    path = sys.argv[1]
    top = int(sys.argv[sys.argv.index('--top') + 1]) if '--top' in sys.argv else 25
    data = open(path, 'rb').read()
    pos = 0
    insts = stalls = 0
    by_prod = defaultdict(float)
    by_op = defaultdict(lambda: [0.0, 0.0, 0.0])  # opcode key -> [executed host words, stall cycles, executed guest]
    blocks = 0
    show = int(sys.argv[sys.argv.index('--show') + 1], 0) if '--show' in sys.argv else None
    best = [0, None]
    while pos + 12 <= len(data):
        (magic,) = struct.unpack_from('<I', data, pos)
        if magic == 0x42555453:  # the shared stubs
            _, base, nw = struct.unpack_from('<3I', data, pos)
            STUBS['base'], STUBS['words'] = base, struct.unpack_from('<%dI' % nw, data, pos + 12)
            pos += 12 + 4 * nw
            continue
        if magic != 0x4B4C4222:
            sys.exit('bad record at %d' % pos)
        _, pc, hot, pro, ndop, fn = struct.unpack_from('<6I', data, pos)
        (n,) = struct.unpack_from('<Q', data, pos + 24)
        pos += 32
        words = struct.unpack_from('<%dI' % hot, data, pos)
        pos += 4 * hot
        dops = struct.unpack_from('<%dI' % ndop, data, pos)
        pos += 4 * ndop
        blocks += 1
        body = words[pro:]
        res = run_block(body, fn + 4 * pro, strip_counters(body))
        # host words -> guest opcode (each guest instruction's hot words in order)
        owner = []
        for d in dops:
            owner += [d >> 8] * (d & 255)
        if show is not None and any((d >> 8) == show for d in dops) and n > best[0]:
            best[0], best[1] = n, (body, res, owner, fn + 4 * pro)
        for k, r in enumerate(res):
            if r is None:
                continue
            st, why, ran = r
            insts += n * ran
            stalls += n * st
            if st:
                by_prod[why] += n * st
            key = owner[k] if k < len(owner) else -1
            by_op[key][0] += n * ran
            by_op[key][1] += n * st
        for d in dops:
            by_op[d >> 8][2] += n
    print('%d block records, %.3g host instructions executed, %.3g stall cycles: %.3f per instruction' %
          (blocks, insts, stalls, stalls / max(insts, 1)))
    print('stall cycles by the producing instruction class:')
    for k, v in sorted(by_prod.items(), key=lambda x: -x[1]):
        print('  %-10s %5.1f%%  (%.3f per instruction)' % (k, 100 * v / stalls, v / insts))
    if show is not None:
        show_block(best, show)
    print('by guest opcode (key = op << 10 | xo): share of stall cycles, stalls per execution, host words per execution:')
    rows = sorted(by_op.items(), key=lambda x: -x[1][1])[:top]
    for key, (hw, st, g) in rows:
        name = 'tail' if key < 0 else 'op %d xo %d' % (key >> 10, key & 1023)
        print('  %-16s %5.1f%%  %5.2f stalls  %5.1f words  (%.3g executions)' % (name, 100 * st / stalls, st / max(g, 1), hw / max(g, 1), g))


def show_block(best, show):
    """--show KEY: the most executed block holding guest opcode KEY, word by word (hex, stall, owner), a stub call
    followed by the words it runs, for objdump."""
    if not best[1]:
        return
    body, res, owner, addr = best[1]
    print('most executed block with key %#x: %d executions, at %08x' % (show, best[0], addr))
    for k, w in enumerate(body):
        r = res[k]
        o = owner[k] if k < len(owner) else -1
        mark = '*' if o == show else ' '
        print('%s %08x %08x  stall %s  op %d xo %d' % (mark, addr + 4 * k, w, '-' if r is None else r[0], o >> 10, o & 1023))
        if (w >> 24) == 0xEB and STUBS['words']:
            target = addr + 4 * k + 8 + 4 * (((w & 0xFFFFFF) ^ 0x800000) - 0x800000)
            if STUBS['base'] <= target < STUBS['base'] + 4 * len(STUBS['words']):
                for sw in stub_path(target):
                    print('      stub %08x' % sw)


if __name__ == '__main__':
    main()
