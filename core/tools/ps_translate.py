#!/usr/bin/env python3
"""Translates small SDK leaf functions (the Dolphin SDK's PSMTX / PSVEC library, GX register setters, J3D FIFO and
display-list writers and the like, as compiled into a game's main.dol) into C for gc3ds's native stand-ins
(core/src/cpu/hle_ps.c): one statement per guest instruction. Input: objdump's disassembly of each function's bytes
(devkitPPC: powerpc-eabi-objdump -D -b binary -m powerpc:750 -M gekko -EB --adjust-vma=ADDR fn.bin). Run:
    python3 ps_translate.py NAME.dis ... > ../src/cpu/hle_ps_gen.inc

Each function becomes `static u32 ps_NAME(Gekko* g)`, returning the guest instructions it ran (0: refused, nothing
changed). Before any store it checks every guest range it will touch: through argument registers and r2 / r13 (never
written here), through constant addresses (lis / addi), and through pointers it loads from those before any store to
RAM (fetched at entry). Then it runs the instructions in order, so overlapping arguments behave as on the guest.
Stores to the gather pipe (0xCC008000) are collected and handed to the GP once at the end. The function's own stack
frame (and the LR save word above it) is a local array: dead state once it returns. GPRs, CR fields, XER[CA] and LR
are C locals, taken from the guest when read before written; f1 (the return value), f14-f31 and LR are left as the
guest leaves them, and f0, f2-f13, the volatile GPRs and CR fields and XER are dead after the call (the ABI; owner
decision 2026-10-15). An FPR argument is taken only when both lanes are singles (checked at entry). Branches go
forward only; loops, calls, indexed or update loads and anything not handled here are refused at translation time.

Direct mode (NAME.dis!): for bigger functions with loops (J3D's matrix work). Every access is checked where it runs:
an address outside MEM1's cached mapping stops the function before that instruction, with every GPR, CR field,
XER[CA], LR and written FPR in guest state and g->pc there (g_psMid), for guest code to go on from it. The stack
frame is guest memory; branches may go backward; `bl` to the CodeWarrior GPR save / restore helpers runs in place
(--helpers SAVE14,REST14: the addresses of _savegpr_14 and _restgpr_14 in the game)."""
import re, sys

FNV = 0xCBF29CE484222325
PIPE = 0xCC008000


def parse(path):
    ins = []
    for line in open(path):
        m = re.match(r'\s*([0-9a-f]+):\t([0-9a-f ]+)\t(\S+)\s*(.*)$', line)
        if m:
            word = int(m.group(2).replace(' ', ''), 16)
            ins.append((int(m.group(1), 16), word, m.group(3), [a.strip() for a in m.group(4).split(',')] if m.group(4) else []))
    return ins


def mem(op):
    m = re.match(r'(-?\d+)\((r\d+)\)$', op)
    return m.group(2), int(m.group(1))


def fnv(words):
    h = FNV
    for w in words:
        for b in (24, 16, 8, 0):
            h ^= (w >> b) & 0xFF
            h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def rotl_mask(mb, me):
    m, i = 0, mb
    while True:
        m |= 1 << (31 - i)
        if i == me:
            break
        i = (i + 1) & 31
    return m


class St:
    """What the translator knows at a program point (forward branches only: merged where paths join)."""
    def __init__(self):
        self.const, self.derived, self.wr, self.fmust, self.fmay = {}, {}, set(), set(), set()
        self.wmust, self.stkw = set(), set()  # GPRs written on every path; stack bytes written on every path
        self.stored, self.frame, self.live = False, 0, True

    def copy(self):
        s = St()
        s.const, s.derived, s.wr = dict(self.const), dict(self.derived), set(self.wr)
        s.fmust, s.fmay = set(self.fmust), set(self.fmay)
        s.wmust, s.stkw = set(self.wmust), set(self.stkw)
        s.stored, s.frame, s.live = self.stored, self.frame, self.live
        return s


def merge(a, b, path):
    if not a.live:
        return b.copy()
    if not b.live:
        return a.copy()
    if a.frame != b.frame:
        raise SystemExit(f'{path}: the stack frame differs where paths join')
    s = St()
    s.const = {k: v for k, v in a.const.items() if b.const.get(k) == v}
    s.derived = {k: v for k, v in a.derived.items() if b.derived.get(k) == v}
    s.wr, s.fmust, s.fmay = a.wr | b.wr, a.fmust & b.fmust, a.fmay | b.fmay
    s.wmust, s.stkw = a.wmust & b.wmust, a.stkw & b.stkw
    s.stored, s.frame = a.stored or b.stored, a.frame
    return s


HELPERS = None  # direct mode: (_savegpr_14, _restgpr_14) addresses, from --helpers


def sig(t):  # a label state, to find where direct mode's passes settle
    return (tuple(sorted(t.const.items())), frozenset(t.fmust), frozenset(t.fmay), frozenset(t.wmust), frozenset(t.wr), t.live)


def translate(path, logged=False, direct=False, backin=None, backout=None):
    # NAME.dis@K: the function entered at its K-th instruction (__save_gpr / __restore_gpr: callers enter at
    # _savegpr_N / _restgpr_N, N = 14 + K): a definition of its own, named NAME_K.
    # Direct mode: backin holds each loop head's state from the backward branches of the previous pass; backout
    # collects them for the next.
    path, _, skip = path.partition("@")
    ins = parse(path)[int(skip or 0):]
    name = re.sub(r"[^A-Za-z0-9_]", "_", re.sub(r"(?<=[A-Za-z0-9>])__[A-Z0-9][A-Za-z0-9_]*$", "", path.split("/")[-1][:-4])) + (f"_{skip}" if skip else "")
    if ins[-1][1] != 0x4E800020:
        raise SystemExit(f'{path}: does not end in blr')
    end = ins[-1][0]
    targets = {}
    for off, word, op, a in ins:
        o = word >> 26
        if o == 18 and (word & 3) == 1 and not direct:
            raise SystemExit(f'{path}: a call')
        if o == 18 and not (word & 3):
            li = word & 0x03FFFFFC
            li = li - 0x04000000 if li & 0x02000000 else li
            targets[off + li] = True
        elif o == 16 and not (word & 3):
            bd = word & 0xFFFC
            bd = bd - 0x10000 if bd & 0x8000 else bd
            targets[off + bd] = True
    for t in targets:
        if t < ins[0][0] or t > end:
            raise SystemExit(f'{path}: a branch out of the function')
    st = St()
    pending = {}  # label offset -> St of the branches to it
    access, dsrc = {}, {}
    gpr_in, fpr_in, cr_in, gpr_used = [], [], set(), set()
    flags = {'pipe': False, 'stack': 0, 'ca_in': False, 'lr': False, 'lr_w': False, 'xso': False, 'fsave': False,
             'branch': False, 'logged': False, 'fpu': False}
    cur = [0]  # direct mode: the instruction being translated (where a failed access stops)
    fw_last = []  # direct mode: FPRs the instruction writes (their bits in the written mask)
    frest = set()  # direct mode: FPRs lfd restored (ps0 from the saved double)
    all_fw = set()  # FPRs written anywhere
    gpr_nv = set()  # non-volatile GPRs it writes (written back; compiled code forgets its cached GPRs after the call)
    fpr_saved, fpr_restored = set(), set()
    body = []

    # ---- operands ----
    def rd(r):  # a GPR read
        gpr_used.add(r)
        if r not in st.wmust and r not in gpr_in:  # (written on only some paths: the guest value on the others)
            gpr_in.append(r)
        return r

    def wr(r, const=None):
        if r in ('r2', 'r13') or (r == 'r1' and not direct):
            raise SystemExit(f'{path}: writes {r} (reserved)')
        if int(r[1:]) >= 14:
            gpr_nv.add(r)
        gpr_used.add(r)
        st.wr.add(r)
        st.wmust.add(r)
        st.derived.pop(r, None)
        if const is None:
            st.const.pop(r, None)
        else:
            st.const[r] = const & 0xFFFFFFFF

    def frd(f):  # an FPR read
        if f in frest:
            raise SystemExit(f'{path}: {f} read after lfd restored it')
        if f in fpr_restored:
            raise SystemExit(f'{path}: {f} read after lfd restored it')
        if f not in st.fmust and f not in fpr_in:
            if f in st.fmay:
                raise SystemExit(f'{path}: {f} read where only some paths wrote it')
            fpr_in.append(f)
        return f

    def fwr(f):
        st.fmust.add(f)
        st.fmay.add(f)
        fw_last.append(f)
        all_fw.add(f)

    def crf_rd(n):
        cr_in.add(n)  # (written on only some paths: the guest value on the others)
        return f'cr{n}'

    written_cr = set()

    def crf_wr(n):
        if 2 <= n <= 4:
            raise SystemExit(f'{path}: writes cr{n} (non-volatile)')
        written_cr.add(n)

    # ---- memory ----
    def key_of(base):
        if base in st.const:
            return f'c{st.const[base]:08x}', st.const[base]
        if base in st.derived:
            return st.derived[base], None
        if base in st.wr:
            raise SystemExit(f'{path}: memory through {base} after it changed')
        rd(base)
        return base, None

    def checked(k, off, size):  # a range checked at entry: C for its host address
        lo, hi = access.get(k, (off, off + size))
        access[k] = (min(lo, off), max(hi, off + size))
        return f'H{k}({off})'

    def addr(base, off, size, store):
        """(kind, C): 'H' a host address checked at entry, 'STK' the private frame, 'PIPE', or (logged mode) 'D' a
        guest address the access checks itself (stores go to the log, applied at the end)."""
        if base == 'r0':
            raise SystemExit(f'{path}: d(0) addressing')
        if direct:  # 'X': a guest address checked where it runs (XH stops the function there)
            if base in st.const and (st.const[base] + off) & 0xFFFFFFFF == PIPE:
                if not store:
                    raise SystemExit(f'{path}: a load from the pipe')
                flags['pipe'] = True
                return 'PIPE', 'PIPE'
            return 'X', f'{rd(base)} + {off & 0xFFFFFFFF:#x}u'
        if base == 'r1':
            idx = st.frame + off
            flags['stack'] = max(flags['stack'], -st.frame)
            if idx < -4096:
                raise SystemExit(f'{path}: the stack beyond 4 KB')
            if store:
                if idx + size > 16:
                    raise SystemExit(f'{path}: a store to the caller\'s frame beyond the LR save word')
                st.stkw.update(range(idx, idx + size))
                return 'STK', f'STK({idx})'
            if all(i in st.stkw for i in range(idx, idx + size)):
                return 'STK', f'STK({idx})'
            if any(i in st.stkw for i in range(idx, idx + size)) or idx < 0:
                raise SystemExit(f'{path}: a stack read mixing its own and the caller\'s bytes')
            rd('r1')
            return 'H', checked('r1', idx, size)  # the caller's frame (an argument passed on the stack)
        if base in st.const and (st.const[base] + off) & 0xFFFFFFFF == PIPE:
            if not store:
                raise SystemExit(f'{path}: a load from the pipe')
            flags['pipe'] = True
            return 'PIPE', 'PIPE'
        if logged:
            return 'D', f'{rd(base)} + {off & 0xFFFFFFFF:#x}u'
        k, cv = key_of(base)
        if store:
            st.stored = True
        return 'H', checked(k, off, size)

    def load(base, off, size):  # C for the loaded value (u32); sign extension is the caller's
        kind, e = addr(base, off, size, False)
        if kind == 'X':
            return xload(e, size)
        if kind == 'D':
            flags['logged'] = True
            return f'DLD({e}, {size})'
        return {4: f'BE32({e})', 2: f'BE16({e})', 1: f'*({e})'}[size]

    def store(base, off, size, v):  # C statement
        kind, e = addr(base, off, size, True)
        if kind == 'X':
            return xstore(e, size, v)
        if kind == 'PIPE':
            if direct:  # (a loop may fill the buffer: what is there goes to the GP first)
                return f'if (pn > PIPE_MAX - 8) {{ PIPE_OUT(); pn = 0; }} PIPE{size * 8}({v});'
            return f'PIPE{size * 8}({v});'
        if kind == 'D':
            flags['logged'] = True
            return f'DST({e}, {v}, {size});'
        if kind == 'STK':  # the private frame
            return {4: f'SBE32({e}, {v});', 2: f'SBE16({e}, {v});', 1: f'*({e}) = (u8)({v});'}[size]
        return f'STW{size * 8}({e}, {v});'  # guest RAM (HLE_CHECK records each store)

    def xload(e, size):  # direct mode: a load from a guest address, stopping here when it is not in MEM1
        flags['stops'] = True
        return {4: f'BE32(XH({e}, 4, {cur[0]:#010x}u))', 2: f'BE16(XH({e}, 2, {cur[0]:#010x}u))',
                1: f'*XH({e}, 1, {cur[0]:#010x}u)'}[size]

    def xstore(e, size, v):
        flags['stops'] = True
        return f'STW{size * 8}(XH({e}, {size}, {cur[0]:#010x}u), {v});'

    def ea_idx(rA, rB):  # an indexed address (rA 0: rB alone)
        return rd(rB) if rA == 'r0' else f'{rd(rA)} + {rd(rB)}'

    def cmp0(res):  # Rc=1: CR0 from a result compared with 0 (and XER[SO])
        crf_wr(0)
        flags['xso'] = True
        return f' cr0 = CMPS({res}, 0) | xso;'

    def cond(word):  # bc / bclr: the C condition, or None for always
        bo, bi = (word >> 21) & 31, (word >> 16) & 31
        if not (bo & 4):
            raise SystemExit(f'{path}: a branch that decrements CTR')
        if bo & 16:
            return None
        c = f'({crf_rd(bi >> 2)} & {8 >> (bi & 3)})'
        return c if bo & 8 else f'!{c}'

    def integer(word, off):
        op, rD, rA, rB = word >> 26, f'r{(word >> 21) & 31}', f'r{(word >> 16) & 31}', f'r{(word >> 11) & 31}'
        simm = word & 0xFFFF
        simm = simm - 0x10000 if simm & 0x8000 else simm
        uimm, rc = word & 0xFFFF, word & 1
        if op in (14, 15):  # addi / addis (li / lis with rA = 0)
            v = simm << 16 if op == 15 else simm
            if rA == 'r0':
                wr(rD, v)
                return f'{rD} = {v & 0xFFFFFFFF:#x}u;'
            if rA == 'r1' and rD == 'r1' and not direct:
                st.frame += v
                return f'/* addi r1 (the frame: {st.frame}) */'
            c = st.const.get(rA)
            e = f'{rd(rA)} + {v & 0xFFFFFFFF:#x}u'
            wr(rD, None if c is None else c + v)
            return f'{rD} = {e};'
        if op in (12, 13):  # addic, addic.
            e = rd(rA)
            wr(rD)
            s = f'{{ u64 t_ = (u64){e} + {simm & 0xFFFFFFFF:#x}u; {rD} = (u32)t_; ca = (u32)(t_ >> 32); }}'
            return s + (cmp0(rD) if op == 13 else '')
        if op == 7:
            e = f'{rd(rA)} * (u32){simm}'
            wr(rD)
            return f'{rD} = {e};'
        if op == 8:  # subfic: ~rA + simm + 1
            e = rd(rA)
            wr(rD)
            return f'{{ u64 t_ = (u64)(u32)~{e} + {simm & 0xFFFFFFFF:#x}u + 1u; {rD} = (u32)t_; ca = (u32)(t_ >> 32); }}'
        if op in (10, 11):  # cmpli, cmpi
            bf = (word >> 23) & 7
            if word & (1 << 21):
                raise SystemExit(f'{path}: a 64-bit compare')
            e = f'CMP{"U" if op == 10 else "S"}({rd(rA)}, {(uimm if op == 10 else simm & 0xFFFFFFFF):#x}u)'
            crf_wr(bf)
            flags['xso'] = True
            return f'cr{bf} = {e} | xso;'
        if op in (24, 25, 26, 27, 28, 29):  # ori, oris, xori, xoris, andi., andis.
            v = uimm << 16 if op in (25, 27, 29) else uimm
            o = {24: '|', 25: '|', 26: '^', 27: '^', 28: '&', 29: '&'}[op]
            c = st.const.get(rD)
            e = f'{rd(rD)} {o} {v:#x}u'
            cv = None
            if c is not None:
                cv = c | v if o == '|' else c ^ v if o == '^' else c & v
            wr(rA, cv)
            return f'{rA} = {e};' + (cmp0(rA) if op in (28, 29) else '')
        if op in (20, 21):  # rlwimi, rlwinm
            sh, mb, me = (word >> 11) & 31, (word >> 6) & 31, (word >> 1) & 31
            m = rotl_mask(mb, me)
            if op == 21:
                e = f'ROTL({rd(rD)}, {sh}) & {m:#010x}u'
            else:
                e = f'(ROTL({rd(rD)}, {sh}) & {m:#010x}u) | ({rd(rA)} & {(~m) & 0xFFFFFFFF:#010x}u)'
            wr(rA)
            return f'{rA} = {e};' + (cmp0(rA) if rc else '')
        if op == 31:
            xo = (word >> 1) & 0x3FF
            if xo in (0, 32):  # cmp, cmpl
                bf = (word >> 23) & 7
                e = f'CMP{"S" if xo == 0 else "U"}({rd(rA)}, {rd(rB)})'
                crf_wr(bf)
                flags['xso'] = True
                return f'cr{bf} = {e} | xso;'
            if xo == 339 or xo == 467:  # mfspr / mtspr: LR only
                spr = ((word >> 16) & 31) | (((word >> 11) & 31) << 5)
                if spr != 8:
                    raise SystemExit(f'{path}: mfspr / mtspr {spr}')
                flags['lr'] = True
                if xo == 339:
                    wr(rD)
                    return f'{rD} = lr;'
                flags['lr_w'] = True
                return f'lr = {rd(rD)};'
            logic = {28: '&', 60: '& ~', 444: '|', 412: '| ~', 316: '^', 124: 'NOR', 476: 'NAND', 284: 'EQV'}
            if xo in logic:
                o = logic[xo]
                if xo == 316 and rD == rB:
                    wr(rA, 0)
                    return f'{rA} = 0;' + (cmp0(rA) if rc else '')
                if o in ('NOR', 'NAND', 'EQV'):
                    base = {'NOR': '|', 'NAND': '&', 'EQV': '^'}[o]
                    e = f'~({rd(rD)} {base} {rd(rB)})'
                else:
                    e = f'{rd(rD)} {o} {rd(rB)}'
                c = st.const.get(rD) if xo == 444 and rD == rB else None  # mr
                wr(rA, c)
                return f'{rA} = {e};' + (cmp0(rA) if rc else '')
            if xo in (26, 922, 954):  # cntlzw, extsh, extsb
                e = {26: f'CLZ({rd(rD)})', 922: f'(u32)(s32)(s16){rd(rD)}', 954: f'(u32)(s32)(s8){rd(rD)}'}[xo]
                wr(rA)
                return f'{rA} = {e};' + (cmp0(rA) if rc else '')
            if xo in (24, 536, 792):  # slw, srw, sraw
                s, v = rd(rB), rd(rD)
                if xo == 24:
                    e = f'({s} & 0x20) ? 0u : {v} << ({s} & 31)'
                    wr(rA)
                    return f'{rA} = {e};' + (cmp0(rA) if rc else '')
                if xo == 536:
                    e = f'({s} & 0x20) ? 0u : {v} >> ({s} & 31)'
                    wr(rA)
                    return f'{rA} = {e};' + (cmp0(rA) if rc else '')
                wr(rA)
                return (f'{{ u32 s_ = {s} & 63, v_ = {v}; {rA} = s_ & 32 ? (u32)((s32)v_ >> 31) : (u32)((s32)v_ >> s_); '
                        f'ca = (s32)v_ < 0 && (s_ & 32 ? 1 : (v_ & ((1u << s_) - 1u)) != 0); }}') + (cmp0(rA) if rc else '')
            if xo == 824:  # srawi
                sh = (word >> 11) & 31
                v = rd(rD)
                wr(rA)
                mask = (1 << sh) - 1
                return (f'{{ u32 v_ = {v}; {rA} = (u32)((s32)v_ >> {sh}); ca = (s32)v_ < 0 && (v_ & {mask:#x}u) != 0; }}'
                        + (cmp0(rA) if rc else ''))
            if xo in (266, 40, 235, 104):  # add, subf, mullw, neg
                if word & (1 << 10):
                    raise SystemExit(f'{path}: an OE form')
                e = {266: f'{rd(rA)} + {rd(rB)}', 40: f'{rd(rB)} - {rd(rA)}', 235: f'{rd(rA)} * {rd(rB)}',
                     104: f'(u32)-(s32){rd(rA)}'}[xo]
                wr(rD)
                return f'{rD} = {e};' + (cmp0(rD) if rc else '')
            if direct and xo in (23, 87, 279, 343):  # lwzx, lbzx, lhzx, lhax
                size = {23: 4, 87: 1, 279: 2, 343: 2}[xo]
                e = xload(ea_idx(rA, rB), size)
                if xo == 343:
                    e = f'(u32)(s32)(s16){e}'
                wr(rD)
                return f'{rD} = {e};'
            if direct and xo in (151, 215, 407):  # stwx, stbx, sthx
                size = {151: 4, 215: 1, 407: 2}[xo]
                return xstore(ea_idx(rA, rB), size, rd(rD))
            if direct and xo in (55, 119, 311):  # lwzux, lbzux, lhzux
                if rA == 'r0' or rA == rD:
                    raise SystemExit(f'{path}: an invalid update form')
                size = {55: 4, 119: 1, 311: 2}[xo]
                e = xload('ea_', size)
                ea = ea_idx(rA, rB)
                wr(rD)
                wr(rA)
                return f'{{ u32 ea_ = {ea}; {rD} = {e}; {rA} = ea_; }}'
            if xo in (10, 8, 138, 136, 202, 200, 234):  # addc, subfc, adde, subfe, addze, subfze, addme
                if word & (1 << 10):
                    raise SystemExit(f'{path}: an OE form')
                a_, b_ = rd(rA), (rd(rB) if xo in (10, 8, 138, 136) else None)
                if xo in (138, 136, 202, 200, 234):
                    flags['ca_in'] = True
                e = {10: f'(u64){a_} + {b_}', 8: f'(u64)(u32)~{a_} + {b_} + 1u', 138: f'(u64){a_} + {b_} + ca',
                     136: f'(u64)(u32)~{a_} + {b_} + ca', 202: f'(u64){a_} + ca', 200: f'(u64)(u32)~{a_} + ca',
                     234: f'(u64){a_} + ca + 0xFFFFFFFFu'}[xo]
                wr(rD)
                return f'{{ u64 t_ = {e}; {rD} = (u32)t_; ca = (u32)(t_ >> 32); }}' + (cmp0(rD) if rc else '')
            return None
        if direct and op in (33, 35, 41, 43):  # lwzu, lbzu, lhzu, lhau
            if rA == 'r0' or rA == rD:
                raise SystemExit(f'{path}: an invalid update form')
            size = {33: 4, 35: 1, 41: 2, 43: 2}[op]
            e = xload('ea_', size)
            if op == 43:
                e = f'(u32)(s32)(s16){e}'
            src = rd(rA)
            wr(rD)
            wr(rA)
            return f'{{ u32 ea_ = {src} + {simm & 0xFFFFFFFF:#x}u; {rD} = {e}; {rA} = ea_; }}'
        if direct and op in (39, 45):  # stbu, sthu
            size = {39: 1, 45: 2}[op]
            v, src = rd(rD), rd(rA)
            s = xstore('ea_', size, v)
            wr(rA)
            return f'{{ u32 ea_ = {src} + {simm & 0xFFFFFFFF:#x}u; {s} {rA} = ea_; }}'
        if op in (32, 34, 40, 42):  # lwz, lbz, lhz, lha
            size = {32: 4, 34: 1, 40: 2, 42: 2}[op]
            e = load(rA, simm, size)
            if op == 42:
                e = f'(u32)(s32)(s16){e}'
            # A pointer it may go through, loaded before any store to RAM from a range checked at entry: fetchable at
            # entry (static mode).
            fetchable = op == 32 and not logged and not direct and rA != 'r1' and not st.stored
            k = key_of(rA)[0] if fetchable else None
            wr(rD)
            if fetchable:
                dk = f'd{len(dsrc)}'
                dsrc[dk] = (k, simm)
                st.derived[rD] = dk
            return f'{rD} = {e};'
        if op in (36, 37, 38, 44):  # stw, stwu, stb, sth
            size = {36: 4, 37: 4, 38: 1, 44: 2}[op]
            v = rd(rD)
            if op == 37 and direct:  # stwu: the store, then rA (r1 too: the frame is guest memory here)
                src = rd(rA)
                s = xstore('ea_', 4, v)
                wr(rA)
                return f'{{ u32 ea_ = {src} + {simm & 0xFFFFFFFF:#x}u; {s} {rA} = ea_; }}'
            if op == 37 and rA == 'r1':  # the frame: the old r1 stored at the new top
                if simm >= 0:
                    raise SystemExit(f'{path}: stwu r1 with a positive offset')
                st.frame += simm
                flags['stack'] = max(flags['stack'], -st.frame)
                st.stkw.update(range(st.frame, st.frame + 4))
                return f'SBE32(STK({st.frame}), g->gpr[1] + {st.frame - simm}); /* stwu r1 (the frame: {st.frame}) */'
            s = store(rA, simm, size, v)
            if op == 37:
                c = st.const.get(rA)
                src = rd(rA)
                wr(rA, None if c is None else c + simm)
                s += f' {rA} = {src} + {simm & 0xFFFFFFFF:#x}u;'
            return s
        return None

    def fp(off, word, op, a):
        if op in ('psq_l', 'psq_st'):
            if a[3] != '0':
                raise SystemExit(f'{path}: {op} with GQR{a[3]}')
            w1 = a[2] == '1'
            base, o = mem(a[1])
            if op == 'psq_l':  # GQR0 (float): W=1 sets ps1 to 1.0
                e0 = load(base, o, 4)
                e1 = '1.0f' if w1 else f'U2F({load(base, o + 4, 4)})'
                fwr(a[0])
                return f'{a[0]} = (P){{ U2F({e0}), {e1} }};'
            f = frd(a[0])
            s = store(base, o, 4, f'PSTF({f}.p0)')  # (a single-denormal value stores as signed zero)
            return s if w1 else s + ' ' + store(base, o + 4, 4, f'PSTF({f}.p1)')
        if op == 'lfs':  # both lanes
            e = load(*mem(a[1]), 4)
            fwr(a[0])
            return f'{a[0]} = PS(U2F({e}));'
        if op == 'lfsu' and direct:
            base, o = mem(a[1])
            if base == 'r0':
                raise SystemExit(f'{path}: an invalid update form')
            src = rd(base)
            e = xload('ea_', 4)
            fwr(a[0])
            wr(base)
            return f'{{ u32 ea_ = {src} + {o & 0xFFFFFFFF:#x}u; {a[0]} = PS(U2F({e})); {base} = ea_; }}'
        if op == 'stfd' and direct:  # (the frame's FPR saves: an FPR this function has not written, as the guest holds it)
            f = a[0]
            if f in st.fmay:
                raise SystemExit(f'{path}: stfd {f} after it was written')
            base, o = mem(a[1])
            e = f'{rd(base)} + {o & 0xFFFFFFFF:#x}u'
            return (f'{{ u64 d_ = DBITS(g->fpr[{f[1:]}].ps0); u32 ea_ = {e}; {xstore("ea_", 4, "(u32)(d_ >> 32)")} '
                    f'{xstore("ea_ + 4u", 4, "(u32)d_")} }}')
        if op == 'lfd' and direct:  # (the epilogue's restores: ps0 the saved double; nothing reads it afterwards)
            f = a[0]
            base, o = mem(a[1])
            e = f'{rd(base)} + {o & 0xFFFFFFFF:#x}u'
            frest.add(f)
            flags['frest'] = True
            k = f[1:]
            return (f'{{ u32 ea_ = {e}; u32 hi_ = {xload("ea_", 4)}, lo_ = {xload("ea_ + 4u", 4)}; '
                    f'd{k} = (u64)hi_ << 32 | lo_; frs |= 1u << {k}; }}')
        if op == 'stfs':  # ps0 rounded to single (an FPR argument is a double: as the interpreter rounds it)
            f = a[0]
            if f in frest:
                raise SystemExit(f'{path}: stfs {f} after lfd restored it')
            if f in st.fmust:
                v = f'FBITS({f}.p0)'
            else:
                if f in st.fmay:
                    raise SystemExit(f'{path}: stfs {f} written on only some paths')
                v = f'S2S({f[1:]})'
            return store(*mem(a[1]), 4, v)
        if op == 'frsp':  # (the source is decided before the destination counts as written: frsp f1,f1 reads the argument)
            src = a[1]
            if src in st.fmust:
                e = f'PS({src}.p0)'
            elif src in st.fmay:
                raise SystemExit(f'{path}: frsp {src} written on only some paths')
            else:
                e = f'PS(F2S({src[1:]}))'
            fwr(a[0])
            return f'{a[0]} = {e};'
        if op in ('ps_add', 'ps_sub'):
            e = f'{"PADD" if op == "ps_add" else "PSUB"}({frd(a[1])}, {frd(a[2])})'
            fwr(a[0])
            return f'{a[0]} = {e};'
        if op in ('ps_mul', 'ps_muls0', 'ps_muls1'):
            e = f'P{op[3:].upper()}({frd(a[1])}, {frd(a[2])})'
            fwr(a[0])
            return f'{a[0]} = {e};'
        if op in ('ps_madd', 'ps_msub', 'ps_nmsub', 'ps_madds0', 'ps_madds1', 'ps_sum0', 'ps_sum1'):
            e = f'P{op[3:].upper()}({frd(a[1])}, {frd(a[2])}, {frd(a[3])})'
            fwr(a[0])
            return f'{a[0]} = {e};'
        if op.startswith('ps_merge'):
            e = f'PMERGE{op[-2:]}({frd(a[1])}, {frd(a[2])})'
            fwr(a[0])
            return f'{a[0]} = {e};'
        if op in ('ps_neg', 'ps_mr'):
            e = f'PNEG({frd(a[1])})' if op == 'ps_neg' else frd(a[1])
            fwr(a[0])
            return f'{a[0]} = {e};'
        if op == 'stfd' and mem(a[1])[0] == 'r1':
            flags['fsave'] = True
            fpr_saved.add(a[0])
            return f'/* stfd {a[0]} (saved) */'
        if op == 'lfd' and mem(a[1])[0] == 'r1':
            if a[0] not in fpr_saved:
                raise SystemExit(f'{path}: lfd {a[0]} not saved first')
            fpr_restored.add(a[0])
            return f'/* lfd {a[0]} (ps0 restored) */'
        return None

    def branch_to(t):  # the state at a branch, for its target
        if t <= cur[0]:
            if not direct:
                raise SystemExit(f'{path}: a backward branch (a loop)')
            backout[t] = merge(backout[t], st, path) if t in backout else st.copy()
        else:
            pending[t] = merge(pending[t], st, path) if t in pending else st.copy()

    for off, word, op, a in ins:
        cur[0] = off
        if off in targets:
            st = merge(st, pending.pop(off), path) if off in pending else st
            if direct and off in backin:  # a loop head: what the previous pass's backward branches brought
                st = merge(st, backin[off], path)
            elif direct and not st.live:  # (first pass: reached only from below; nothing known yet)
                st = St()
            st.live = True
            body.append(f'L_{off:x}:;')
        if not st.live:
            raise SystemExit(f'{path}: code at {off:x} nothing reaches')
        o = word >> 26
        if off == end:  # the final blr
            body.append('    n++;')
            break
        if o == 18 and (word & 3) == 1:  # bl (direct mode): the GPR save / restore helpers, in place
            li = word & 0x03FFFFFC
            li = li - 0x04000000 if li & 0x02000000 else li
            t = (off + li) & 0xFFFFFFFF
            if not HELPERS:
                raise SystemExit(f'{path}: a call (no --helpers)')
            kind = next((k for k, base in (('save', HELPERS[0]), ('rest', HELPERS[1])) if 0 <= t - base < 18 * 4 and not (t - base) & 3), None)
            if kind is None:
                raise SystemExit(f'{path}: a call to {t:x}')
            first = 14 + (t - (HELPERS[0] if kind == 'save' else HELPERS[1])) // 4
            regs = [f'r{k}' for k in range(first, 32)]
            cnt = len(regs) + 1  # the helper's stw / lwz and its blr
            src = rd('r11')
            flags['stops'] = True
            flags['lr'] = flags['lr_w'] = True
            if kind == 'save':
                ops = ' '.join(f'STW32(h_ + {4 * i}, {rd(r)});' for i, r in enumerate(regs))
                body.append(f'    n++; {{ u8* h_ = XH({src} - {4 * len(regs)}u, {4 * len(regs)}, {off:#010x}u); '
                            f'{ops} }} lr = {off + 4:#010x}u; n += {cnt};')
            else:
                for r in regs:
                    wr(r)
                ops = ' '.join(f'{r} = BE32(h_ + {4 * i});' for i, r in enumerate(regs))
                body.append(f'    n++; {{ u8* h_ = XH({src} - {4 * len(regs)}u, {4 * len(regs)}, {off:#010x}u); {ops} }} '
                            f'lr = {off + 4:#010x}u; n += {cnt};')
            continue
        if o == 18 and not (word & 3):  # b
            flags['branch'] = True
            li = word & 0x03FFFFFC
            li = li - 0x04000000 if li & 0x02000000 else li
            t = off + li
            branch_to(t)
            body.append(f'    n++; goto L_{t:x};')
            st.live = False
            continue
        if o == 16 and not (word & 3):  # bc
            flags['branch'] = True
            bd = word & 0xFFFC
            bd = bd - 0x10000 if bd & 0x8000 else bd
            t = off + bd
            c = cond(word)
            branch_to(t)
            if c is None:
                body.append(f'    n++; goto L_{t:x};')
                st.live = False
            else:
                body.append(f'    n++; if ({c}) goto L_{t:x};')
            continue
        if o == 19 and ((word >> 1) & 0x3FF) == 16 and not (word & 1):  # bclr (a return before the end)
            flags['branch'] = True
            c = cond(word)
            if st.frame != 0:
                raise SystemExit(f'{path}: a return inside the frame')
            body.append(f'    n++; {"goto L_ret;" if c is None else f"if ({c}) goto L_ret;"}')
            if c is None:
                st.live = False
            continue
        fw_last.clear()
        if op.startswith('ps') or op in ('lfs', 'lfsu', 'stfs', 'frsp', 'stfd', 'lfd'):
            flags['fpu'] = True
            s = fp(off, word, op, a)
        else:
            s = integer(word, off)
        if s is None:
            raise SystemExit(f'{path}: unsupported at {off:x}: {op} {a}')
        if direct:  # the FPRs it wrote, for the write-back (only those go back: the others stay as the guest has them)
            s += ''.join(f' fw |= 1u << {f[1:]};' for f in fw_last)
        body.append(f'    n++; {s}')
    if st.frame != 0:
        raise SystemExit(f'{path}: the frame is not given back')
    if flags['fsave'] and flags['branch']:
        raise SystemExit(f'{path}: saved FPRs with branches')
    if any('L_ret' in s for s in body):
        body.append('L_ret:;')

    if direct:
        return direct_out(path, name, ins, body, flags, st, gpr_used, gpr_nv, fpr_in, written_cr, cr_in, frest, all_fw)

    # ---- entry: range checks, then the pointers fetched through the checked ranges ----
    pre = []
    order = sorted(k for k in access if k[0] != 'd') + sorted((k for k in access if k[0] == 'd'), key=lambda k: int(k[1:]))
    for k in order:
        lo, hi = access[k]
        if k[0] == 'r':
            ea = f'g->gpr[{k[1:]}]'
        elif k[0] == 'c':
            ea = f'0x{k[1:]}u'
        else:
            src, so = dsrc[k]
            if src not in access:
                raise SystemExit(f'{path}: {k} from an unchecked base')
            ea = f'BE32(H{src}({so}))'
        pre.append(f'    u8* const h{k} = region({ea} + (u32)({lo}), {hi - lo});')
        pre.append(f'    if (!h{k}) return 0;')
    defs = [f'#define H{k}(o) (h{k} + ((o) - ({access[k][0]})))' for k in order]
    undefs = [f'#undef H{k}' for k in order]
    # FPRs written back (f1, f14-f31) that some path leaves unwritten: their guest value is the starting value.
    wbf = sorted((f for f in st.fmay if f == 'f1' or int(f[1:]) >= 14), key=lambda x: int(x[1:]))
    for f in wbf:
        if f not in st.fmust and f not in fpr_in:
            fpr_in.append(f)
    for r in sorted(gpr_nv, key=lambda r: int(r[1:])):  # written back: the guest value where a path leaves it
        if r not in st.wmust and r not in gpr_in:
            gpr_in.append(r)
    out = defs + [f'static u32 ps_{name}(Gekko* g) {{  // {len(ins)} instructions']
    if flags['pipe']:
        out.append('    if (!g_pipeWrite) return 0;  // (no GP to take the bytes)')
    for f in fpr_in:
        out.append(f'    if (!fexact(g, {f[1:]})) return 0;')
    out += [f'    u32 {r} = g->gpr[{r[1:]}];' for r in gpr_in]
    out += pre
    others = sorted(gpr_used - set(gpr_in), key=lambda r: int(r[1:]))
    if others:
        out.append(f'    u32 {", ".join(others)};')
    allg = list(gpr_in) + others
    if allg:
        out.append('    ' + ' '.join(f'(void){r};' for r in allg))
    if flags['stack']:
        out.append(f'    u8 stk[{flags["stack"] + 16}];  // the frame and the LR save word above it (dead after the call)')
        out.append(f'#define STK(i) (stk + {flags["stack"]} + (i))')
    if flags['pipe']:
        out.append('    u8 pb[PIPE_MAX];')
        out.append('    u32 pn = 0;')
    if flags['logged']:
        out.append('    Wl wl[WL_MAX];  // its stores to RAM, applied at the end (any address out of range refuses first)')
        out.append('    u32 wn = 0;')
    if flags['lr']:
        out.append('    u32 lr = g->lr;')
    if flags['xso']:
        out.append('    const u32 xso = g->xer >> 31;  // XER[SO] into each compare')
    if written_cr or cr_in:
        crs = sorted(written_cr | cr_in)
        out.append('    u32 ' + ', '.join(f'cr{n} = (g->cr >> {28 - 4 * n}) & 15' if n in cr_in else f'cr{n}' for n in crs) + ';')
        out.append('    ' + ' '.join(f'(void)cr{n};' for n in crs))
    out.append(f'    u32 ca = {"(g->xer >> 29) & 1" if flags["ca_in"] else "0"}, n = 0;')
    out.append('    (void)ca;')
    for f in fpr_in:
        out.append(f'    P {f} = FIN({f[1:]});')
    fl = sorted(st.fmay - set(fpr_in), key=lambda x: int(x[1:]))
    if fl:
        out.append(f'    P {", ".join(fl)};')
    wb = []
    for f in sorted(st.fmay | set(fpr_in), key=lambda x: int(x[1:])):
        k = int(f[1:])
        if k != 1 and k < 14:
            continue
        if f in fpr_restored:
            wb.append(f'    g->fpr[{k}].ps1 = (double){f}.p1;')
        elif f in st.fmay:
            wb.append(f'    g->fpr[{k}].ps0 = (double){f}.p0; g->fpr[{k}].ps1 = (double){f}.p1;')
    tail = (['    WL_APPLY();'] if flags['logged'] else []) + (['    PIPE_OUT();'] if flags['pipe'] else [])
    tail += (['    g->lr = lr;'] if flags['lr_w'] else []) + [f'    g->gpr[{r[1:]}] = {r};' for r in sorted(gpr_nv, key=lambda r: int(r[1:]))]
    if flags['stack']:
        tail.append('#undef STK')
    out += body + wb + tail + ['    return n;', '}'] + undefs
    words = [w for _, w, _, _ in ins]
    return name, len(ins) * 4, words[0], fnv(words), out, flags['pipe'], bool(gpr_nv), flags['fpu']


def direct_out(path, name, ins, body, flags, st, gpr_used, gpr_nv, fpr_in, written_cr, cr_in, frest, all_fw):
    """Direct mode's C: every used GPR, CR field and XER[CA] taken from the guest at entry (so any of them can go back
    exactly wherever it stops); FPRs read before written must be singles in both lanes (fexact) and go back only when
    written (the mask fw); lfd's restores keep the saved double (d<k>, mask frs)."""
    num = lambda x: int(x[1:])
    regs = sorted(gpr_used, key=num)
    fset = sorted(all_fw | set(fpr_in), key=num)
    crs = sorted(written_cr | cr_in)
    uses_ca = any('ca' in s for s in body)
    out = [f'static u32 ps_{name}(Gekko* g) {{  // {len(ins)} instructions (direct: stops where an access is not in MEM1)']
    if flags['pipe']:
        out.append('    if (!g_pipeWrite) return 0;  // (no GP to take the bytes)')
    for f in fpr_in:
        out.append(f'    if (!fexact(g, {f[1:]})) return 0;')
    if regs:
        out.append('    u32 ' + ', '.join(f'{r} = g->gpr[{r[1:]}]' for r in regs) + ';')
    if flags['pipe']:
        out += ['    u8 pb[PIPE_MAX];', '    u32 pn = 0;']
    if flags['lr'] or flags['lr_w']:
        out.append('    u32 lr = g->lr;')
    if flags['xso']:
        out.append('    const u32 xso = g->xer >> 31;  // XER[SO] into each compare')
    if crs:
        out.append('    u32 ' + ', '.join(f'cr{n} = (g->cr >> {28 - 4 * n}) & 15' for n in crs) + ';')
    out.append('    u8* const xm = g_m;  // (locals: through the globals, every store would reload them)')
    out.append('    const u32 xsz = g_size;')
    out.append('    u32 ca = (g->xer >> 29) & 1, n = 0, spc = 0, fw = 0, frs = 0;')
    for f in fset:
        out.append(f'    P {f} = {"FIN(" + f[1:] + ")" if f in fpr_in else "(P){ 0, 0 }"};')
    for f in sorted(frest, key=num):
        out.append(f'    u64 d{f[1:]} = 0;')
    out.append('    ' + ' '.join(f'(void){v};' for v in regs + [f'cr{n}' for n in crs] + ['ca', 'spc', 'fw', 'frs']))

    def fprs_back(which):
        w = []
        for f in fset:
            k = num(f)
            if which == 'end' and k != 1 and k < 14:
                continue
            w.append(f'    if (fw & (1u << {k})) {{ g->fpr[{k}].ps0 = (double){f}.p0; g->fpr[{k}].ps1 = (double){f}.p1; }}')
        for f in sorted(frest, key=num):
            k = num(f)
            w.append(f'    if (frs & (1u << {k})) g->fpr[{k}].ps0 = BITSD(d{k});')
        return w
    end = fprs_back('end') + (['    PIPE_OUT();'] if flags['pipe'] else [])
    end += (['    g->lr = lr;'] if flags['lr_w'] else [])
    end += [f'    g->gpr[{r[1:]}] = {r};' for r in sorted(gpr_nv | ({'r1'} & gpr_used), key=num)]
    end += ['    return n;']
    stop = ['stop:', '    n--;  // (the instruction that stopped did not run)', '    if (!n) return 0;']
    stop += (['    PIPE_OUT();'] if flags['pipe'] else [])
    stop += [f'    g->gpr[{r[1:]}] = {r};' for r in regs if r not in ('r2', 'r13')]
    stop += fprs_back('stop')
    if crs:
        mask = 0
        for n_ in crs:
            mask |= 0xF << (28 - 4 * n_)
        stop.append(f'    g->cr = (g->cr & {(~mask) & 0xFFFFFFFF:#010x}u) | ' + ' | '.join(f'(cr{n_} << {28 - 4 * n_})' for n_ in crs) + ';')
    if uses_ca:
        stop.append('    g->xer = (g->xer & ~XER_CA) | (ca ? XER_CA : 0);')
    if flags['lr_w']:
        stop.append('    g->lr = lr;')
    stop += ['    g->pc = spc;', '    g_psMid = true;', '    return n;']
    if not flags.get('stops'):
        stop = []
    out += body + end + stop + ['}']
    words = [w for _, w, _, _ in ins]
    return name, len(ins) * 4, words[0], fnv(words), out, flags['pipe'], bool(gpr_nv), flags['fpu']


def translate_direct(path):  # passes until each loop head's state settles (what its backward branches bring)
    backin = {}
    for _ in range(16):
        backout = {}
        r = translate(path, direct=True, backin=backin, backout=backout)
        if {k: sig(v) for k, v in backout.items()} == {k: sig(v) for k, v in backin.items()}:
            return r
        backin = backout
    raise SystemExit(f'{path}: the loop states did not settle')


def translate_any(path):  # checked at entry where every address is known there; else logged
    if path.endswith('!'):
        return translate_direct(path[:-1])
    try:
        return translate(path)
    except SystemExit as e:
        if 'after it changed' not in str(e):
            raise
        return translate(path, logged=True)


if __name__ == '__main__':
    args = sys.argv[1:]
    if args[:1] == ['--helpers']:
        HELPERS = tuple(int(x, 16) for x in args[1].split(','))
        args = args[2:]
    fns = [translate_any(p) for p in args]
    print('// Generated by core/tools/ps_translate.py from SDK functions as compiled into a game\'s main.dol: one C')
    print('// statement per guest instruction. Included by hle_ps.c; do not edit.')
    for name, size, first, h, out, pipe, gprs, fpu in fns:
        print('\n'.join(out))
    print('static const PsDef g_psDefs[] = {')
    for name, size, first, h, out, pipe, gprs, fpu in fns:
        b = lambda x: 'true' if x else 'false'
        print(f'    {{ "{name}", {size:#x}, {first:#010x}u, {h:#018x}ull, ps_{name}, {b(pipe)}, {b(gprs)}, {b(fpu)} }},')
    print('};')
