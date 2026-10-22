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
        self.gd, self.cd = set(), set()  # direct mode: GPRs / CR fields written since the last call's sync
        self.fpck = False  # direct mode: the FP state (MSR[FP], RN, HID2, GQR0) checked on every path to here
        # Static mode: a GPR's value as a C expression of entry values (g->gpr[N] before anything ran), a GPR that is a
        # checked base plus such an offset (key, expression), and srawi's carry as an expression (for addze).
        self.expr, self.ptr, self.caex = {}, {}, None

    def copy(self):
        s = St()
        s.gd, s.cd = set(self.gd), set(self.cd)
        s.fpck = self.fpck
        s.const, s.derived, s.wr = dict(self.const), dict(self.derived), set(self.wr)
        s.fmust, s.fmay = set(self.fmust), set(self.fmay)
        s.wmust, s.stkw = set(self.wmust), set(self.stkw)
        s.stored, s.frame, s.live = self.stored, self.frame, self.live
        s.expr, s.ptr, s.caex = dict(self.expr), dict(self.ptr), self.caex
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
    s.gd, s.cd = a.gd | b.gd, a.cd | b.cd
    s.fpck = a.fpck and b.fpck
    s.expr = {k: v for k, v in a.expr.items() if b.expr.get(k) == v}
    s.ptr = {k: v for k, v in a.ptr.items() if b.ptr.get(k) == v}
    s.caex = a.caex if a.caex == b.caex else None
    return s


HELPERS = None  # direct mode: (_savegpr_14, _restgpr_14) addresses, from --helpers


def sig(t):  # a label state, to find where direct mode's passes settle
    return (tuple(sorted(t.const.items())), frozenset(t.fmust), frozenset(t.fmay), frozenset(t.wmust), frozenset(t.wr), t.live,
            frozenset(t.gd), frozenset(t.cd), t.fpck)


def translate(path, logged=False, direct=False, backin=None, backout=None):
    # NAME.dis@K: the function entered at its K-th instruction (__save_gpr / __restore_gpr: callers enter at
    # _savegpr_N / _restgpr_N, N = 14 + K): a definition of its own, named NAME_K.
    # Direct mode: backin holds each loop head's state from the backward branches of the previous pass; backout
    # collects them for the next.
    # Region mode (direct, NAME.dis^END!): a stretch of a function, entered at its first instruction and left at address
    # END (where every register goes back to guest state, as at a stop, and guest code or another stand-in goes on).
    path, _, rend = path.partition("^")
    rend = int(rend, 16) if rend else None
    path, _, skip = path.partition("@")
    ins = parse(path)[int(skip or 0):]
    name = re.sub(r"[^A-Za-z0-9_]", "_", re.sub(r"(?<=[A-Za-z0-9>])__[A-Z0-9][A-Za-z0-9_]*$", "", path.split("/")[-1][:-4])) + (f"_{skip}" if skip else "")
    if rend is not None:
        if not direct:
            raise SystemExit(f'{path}: a region is direct mode only')
        end = rend
    elif ins[-1][1] != 0x4E800020:
        raise SystemExit(f'{path}: does not end in blr')
    else:
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
    # x keys: a checked base (an entry GPR, a constant, a fetched pointer) plus an offset computed from entry values
    # (__GXData + 4 * stage in the SDK's GX setters), checked at entry as one range. korder: d and x keys as created
    # (each one's base exists before it). late: d keys fetched after a store, through r2 / r13 (the SDK reloads __GXData
    # from small data): fetched at entry, which holds when no store of the function overlaps the word (checked at entry).
    xsrc, korder, late, stores = {}, [], {}, {}
    gpr_in, fpr_in, cr_in, gpr_used = [], [], set(), set()
    flags = {'pipe': False, 'stack': 0, 'ca_in': False, 'lr': False, 'lr_w': False, 'xso': False, 'fsave': False,
             'branch': False, 'logged': False, 'fpu': False}
    cur = [0]  # direct mode: the instruction being translated (where a failed access stops)
    fw_last = []  # direct mode: FPRs the instruction writes (their bits in the written mask)
    frest = set()  # direct mode: FPRs lfd restored (ps0 from the saved double)
    all_fw = set()  # FPRs written anywhere
    unsupported = []  # direct mode: instructions that stop the function (reported to stderr)
    p0only = set()  # direct mode: FPRs only lane 0 of which is known (fneg / fabs / fmr into an unknown register)
    fdecl = set()  # direct mode: FPRs named only through guest state or a double (declared as locals all the same)
    gpr_nv = set()  # non-volatile GPRs it writes (written back; compiled code forgets its cached GPRs after the call)
    gpr_vw = set()  # volatile GPRs it writes (static mode: written back only when a stand-in called it, g_psNested)
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
        elif r != 'r1':
            gpr_vw.add(r)
        gpr_used.add(r)
        st.wr.add(r)
        st.wmust.add(r)
        st.gd.add(r)
        st.derived.pop(r, None)
        st.expr.pop(r, None)
        st.ptr.pop(r, None)
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
            if f in st.fmay and not direct:
                raise SystemExit(f'{path}: {f} read where only some paths wrote it')
            fpr_in.append(f)  # (direct mode: the local holds the entry value on the paths that did not write it)
        return f

    def fval(f):  # direct mode: ps0 as a double (lfd's / fctiwz's when its frs bit is set, else the single)
        if f in frest:
            if f not in st.fmust and f not in fpr_in:
                fpr_in.append(f)
            return f'(((frs >> {f[1:]}) & 1u) ? BITSD(d{f[1:]}) : (double){f}.p0)'
        return f'(double){frd(f)}.p0'

    def fdbl(f):  # direct mode: a double now in ps0 (lfd, fctiwz, a move of one); ps1 as it was
        frest.add(f); fdecl.add(f)
        st.fmust.add(f); st.fmay.add(f)

    def fwr(f):
        st.fmust.add(f)
        st.fmay.add(f)
        fw_last.append(f)
        all_fw.add(f)
        p0only.discard(f)

    def crf_rd(n):
        cr_in.add(n)  # (written on only some paths: the guest value on the others)
        return f'cr{n}'

    written_cr = set()

    def crf_wr(n):
        if 2 <= n <= 4 and not direct:  # (direct mode writes every CR field back)
            raise SystemExit(f'{path}: writes cr{n} (non-volatile)')
        written_cr.add(n)
        st.cd.add(n)

    # ---- memory ----
    def key_of(base):
        if base in st.const:
            return f'c{st.const[base]:08x}', st.const[base]
        if base in st.derived:
            return st.derived[base], None
        if base in st.ptr and not direct and not logged:
            bk, oe = st.ptr[base]
            if oe == '0':
                return bk, None  # (an entry GPR's key is its value at entry, whatever the GPR holds now)
            for k, v in xsrc.items():
                if v == (bk, oe):
                    return k, None
            xk = f'x{len(xsrc)}'
            xsrc[xk] = (bk, oe)
            korder.append(xk)
            return xk, None
        if base in st.wr:
            raise SystemExit(f'{path}: memory through {base} after it changed')
        rd(base)
        return base, None

    def checked(k, off, size, store=False):  # a range checked at entry: C for its host address
        lo, hi = access.get(k, (off, off + size))
        access[k] = (min(lo, off), max(hi, off + size))
        if store:
            lo, hi = stores.get(k, (off, off + size))
            stores[k] = (min(lo, off), max(hi, off + size))
        return f'H{k}({off})'

    def ex(r):  # r's value as a C expression of entry values, or None
        if direct or logged:
            return None
        if r in st.expr:
            return st.expr[r]
        if r in st.const:
            return f'{st.const[r]:#x}u'
        if r not in st.wr and r not in ('r1',):
            return f'g->gpr[{r[1:]}]'
        return None

    def base_of(r):  # r as a pointer: (key, offset expression), or None; strong: a pointer the code made (not an entry GPR)
        if direct or logged or r == 'r1':
            return None, False
        if r in st.ptr:
            return st.ptr[r], True
        if r in st.derived:
            return (st.derived[r], '0'), True
        if r in st.const:
            return (f'c{st.const[r]:08x}', '0'), False
        if r not in st.wr:
            return (r, '0'), False
        return None, False

    def addr(base, off, size, store):
        """(kind, C): 'H' a host address checked at entry, 'STK' the private frame, 'PIPE', or (logged mode) 'D' a
        guest address the access checks itself (stores go to the log, applied at the end)."""
        if base == 'r0':
            raise SystemExit(f'{path}: d(0) addressing')
        if direct:  # 'X': a guest address checked where it runs (XH stops the function there)
            if base in st.const and 0 <= ((st.const[base] + off) & 0xFFFFFFFF) - PIPE < 32:  # (the pipe takes any address in its 32 bytes)
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
        if base in st.const and 0 <= ((st.const[base] + off) & 0xFFFFFFFF) - PIPE < 32:  # (the pipe takes any address in its 32 bytes)
            if not store:
                raise SystemExit(f'{path}: a load from the pipe')
            flags['pipe'] = True
            return 'PIPE', 'PIPE'
        if logged:
            return 'D', f'{rd(base)} + {off & 0xFFFFFFFF:#x}u'
        k, cv = key_of(base)
        if store:
            st.stored = True
        return 'H', checked(k, off, size, store)

    def load(base, off, size):  # C for the loaded value (u32); sign extension is the caller's
        kind, e = addr(base, off, size, False)
        if kind == 'X':
            return xload(e, size)
        if kind == 'D':
            flags['logged'] = True
            return f'DLD({e}, {size})'
        return {4: f'BE32({e})', 2: f'BE16({e})', 1: f'BE8({e})'}[size]

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
            return {4: f'SBE32({e}, {v});', 2: f'SBE16({e}, {v});', 1: f'SBE8({e}, {v});'}[size]
        return f'STW{size * 8}({e}, {v});'  # guest RAM (HLE_CHECK records each store)

    def xload(e, size):  # direct mode: a load from a guest address, stopping here when it is not in MEM1
        flags['stops'] = True
        return {4: f'BE32(XH({e}, 4, {cur[0]:#010x}u))', 2: f'BE16(XH({e}, 2, {cur[0]:#010x}u))',
                1: f'BE8(XH({e}, 1, {cur[0]:#010x}u))'}[size]

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
            if not direct:
                raise SystemExit(f'{path}: a branch that decrements CTR')
            flags['ctr'] = True  # bdnz / bdz (and with a CR bit): CTR goes down first (the caller emits ctr--)
            cc = '(ctr == 0)' if bo & 2 else '(ctr != 0)'
            if bo & 16:
                return cc
            c = f'({crf_rd(bi >> 2)} & {8 >> (bi & 3)})'
            return f'{cc} && {c if bo & 8 else "!" + c}'
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
            (b_, _s), xa = base_of(rA), ex(rA)
            e = f'{rd(rA)} + {v & 0xFFFFFFFF:#x}u'
            wr(rD, None if c is None else c + v)
            if c is None and b_ is not None:
                st.ptr[rD] = (b_[0], f'{b_[1]} + {v & 0xFFFFFFFF:#x}u' if b_[1] != '0' else f'{v & 0xFFFFFFFF:#x}u')
            if c is None and xa is not None:
                st.expr[rD] = f'({xa} + {v & 0xFFFFFFFF:#x}u)'
            return f'{rD} = {e};'
        if op in (12, 13):  # addic, addic.
            st.caex = None
            e = rd(rA)
            wr(rD)
            s = f'{{ u64 t_ = (u64){e} + {simm & 0xFFFFFFFF:#x}u; {rD} = (u32)t_; ca = (u32)(t_ >> 32); }}'
            return s + (cmp0(rD) if op == 13 else '')
        if op == 7:
            xa = ex(rA)
            e = f'{rd(rA)} * (u32){simm}'
            wr(rD)
            if xa is not None:
                st.expr[rD] = f'({xa} * (u32){simm})'
            return f'{rD} = {e};'
        if op == 8:  # subfic: ~rA + simm + 1
            st.caex = None
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
            xs = ex(rD) if op == 21 else None
            if op == 21:
                e = f'ROTL({rd(rD)}, {sh}) & {m:#010x}u'
            else:
                e = f'(ROTL({rd(rD)}, {sh}) & {m:#010x}u) | ({rd(rA)} & {(~m) & 0xFFFFFFFF:#010x}u)'
            wr(rA)
            if xs is not None:
                st.expr[rA] = f'(ROTL({xs}, {sh}) & {m:#010x}u)'
            return f'{rA} = {e};' + (cmp0(rA) if rc else '')
        if op == 19 and direct and ((word >> 1) & 0x3FF) in (257, 129, 289, 225, 33, 449, 417, 193):
            xo = (word >> 1) & 0x3FF
            bd, ba, bb = (word >> 21) & 31, (word >> 16) & 31, (word >> 11) & 31
            fa, fb = crf_rd(ba >> 2), crf_rd(bb >> 2)
            fd = crf_rd(bd >> 2)
            crf_wr(bd >> 2)
            x, y = f'(({fa} >> {3 - (ba & 3)}) & 1u)', f'(({fb} >> {3 - (bb & 3)}) & 1u)'
            e = {257: f'{x} & {y}', 129: f'{x} & ~{y}', 289: f'~({x} ^ {y})', 225: f'~({x} & {y})', 33: f'~({x} | {y})',
                 449: f'{x} | {y}', 417: f'{x} | ~{y}', 193: f'{x} ^ {y}'}[xo]
            m = 8 >> (bd & 3)
            return f'{{ u32 v_ = ({e}) & 1u; {fd} = ({fd} & ~{m}u) | (v_ ? {m}u : 0u); }}'
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
                if spr == 9 and direct:  # CTR (direct mode)
                    flags['ctr'] = True
                    if xo == 339:
                        wr(rD)
                        return f'{rD} = ctr;'
                    return f'ctr = {rd(rD)};'
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
                if xo == 792:
                    st.caex = None
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
                xs = ex(rD)
                v = rd(rD)
                wr(rA)
                if xs is not None:
                    st.expr[rA] = f'(u32)((s32){xs} >> {sh})'
                    st.caex = f'(u32)((s32){xs} < 0 && ({xs} & {(1 << sh) - 1:#x}u) != 0)'
                else:
                    st.caex = None
                mask = (1 << sh) - 1
                return (f'{{ u32 v_ = {v}; {rA} = (u32)((s32)v_ >> {sh}); ca = (s32)v_ < 0 && (v_ & {mask:#x}u) != 0; }}'
                        + (cmp0(rA) if rc else ''))
            if xo in (266, 40, 235, 104):  # add, subf, mullw, neg
                if word & (1 << 10):
                    raise SystemExit(f'{path}: an OE form')
                pa_ = pb_ = xa_ = xb_ = None
                if xo == 266:
                    (pa_, sa_), (pb_, sb_) = base_of(rA), base_of(rB)
                    xa_, xb_ = ex(rA), ex(rB)
                e = {266: f'{rd(rA)} + {rd(rB)}', 40: f'{rd(rB)} - {rd(rA)}', 235: f'{rd(rA)} * {rd(rB)}',
                     104: f'(u32)-(s32){rd(rA)}'}[xo]
                wr(rD)
                if xo == 266:
                    cand = [(pa_, sa_, xb_), (pb_, sb_, xa_)]
                    cand.sort(key=lambda t: not t[1])  # a pointer the code made first
                    for p_, _s, x_ in cand:
                        if p_ is not None and x_ is not None:
                            st.ptr[rD] = (p_[0], x_ if p_[1] == '0' else f'{p_[1]} + {x_}')
                            break
                    if xa_ is not None and xb_ is not None:
                        st.expr[rD] = f'({xa_} + {xb_})'
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
                xz = ex(rA) if xo == 202 else None
                cz = st.caex
                a_, b_ = rd(rA), (rd(rB) if xo in (10, 8, 138, 136) else None)
                if xo in (138, 136, 202, 200, 234):
                    flags['ca_in'] = True
                e = {10: f'(u64){a_} + {b_}', 8: f'(u64)(u32)~{a_} + {b_} + 1u', 138: f'(u64){a_} + {b_} + ca',
                     136: f'(u64)(u32)~{a_} + {b_} + ca', 202: f'(u64){a_} + ca', 200: f'(u64)(u32)~{a_} + ca',
                     234: f'(u64){a_} + ca + 0xFFFFFFFFu'}[xo]
                wr(rD)
                st.caex = None
                if xz is not None and cz is not None:
                    st.expr[rD] = f'({xz} + {cz})'
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
            fetchable = op == 32 and not logged and not direct and rA != 'r1' and (not st.stored or rA in ('r2', 'r13'))
            k = key_of(rA)[0] if fetchable else None
            wr(rD)
            if fetchable:
                dk = next((d for d, v in dsrc.items() if v == (k, simm) and (d in late) == st.stored), None)
                if dk is None:
                    dk = f'd{len(dsrc)}'
                    dsrc[dk] = (k, simm)
                    korder.append(dk)
                    if st.stored:
                        late[dk] = (k, simm)
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
        if direct and (op.startswith('ps') or op.startswith('psq_st')):  # paired use of a register only lane 0 of which is known
            srcs = a[:1] if op.startswith('psq_st') else [] if op.startswith('psq_l') else a[1:]
            for x in srcs:
                if x in p0only:
                    raise SystemExit(f'{path}: paired use of {x} (lane 0 only)')
        if direct and op in ('psq_l', 'psq_lu') and (a[3] != '0' or op == 'psq_lu'):
            # Through GQRn's load half as it is at run time (u8 / u16 / s8 / s16 with its scale, or float); psq_lu: rA = ea.
            base, o = mem(a[1])
            w1 = a[2] == '1'
            src = rd(base)
            fwr(a[0])
            if op == 'psq_lu':
                wr(base)
            flags['stops'] = True
            q = f'g->spr[SPR_GQR0 + {a[3]}]'
            e = (f'{{ u32 ea_ = {src} + {o & 0xFFFFFFFF:#x}u, q_ = {q}, sz_ = QSZ(q_); '
                 f'const u8* h_ = XH(ea_, sz_ * {1 if w1 else 2}u, {cur[0]:#010x}u); '
                 f'{a[0]} = (P){{ QLD(h_, q_), {"1.0f" if w1 else "QLD(h_ + sz_, q_)"} }};')
            return e + (f' {base} = ea_; }}' if op == 'psq_lu' else ' }')
        if direct and op == 'psq_stu' and a[3] == '0':
            base, o = mem(a[1])
            w1 = a[2] == '1'
            f = frd(a[0])
            src = rd(base)
            wr(base)
            flags['stops'] = True
            n_ = 4 if w1 else 8
            s1 = '' if w1 else f' STW32(h_ + 4, PSTF({f}.p1));'
            return (f'{{ u32 ea_ = {src} + {o & 0xFFFFFFFF:#x}u; u8* h_ = XH(ea_, {n_}, {cur[0]:#010x}u); '
                    f'STW32(h_, PSTF({f}.p0));{s1} {base} = ea_; }}')
        if direct and op in ('lfsx', 'lfsux', 'stfsx', 'stfsux'):
            ra, rb = a[1], a[2]
            if op.endswith('ux') and ra == 'r0':
                raise SystemExit(f'{path}: an invalid update form')
            ea = ea_idx(ra, rb)
            if op.startswith('lfs'):
                e = xload('ea_', 4)
                fwr(a[0])
                upd = f' {ra} = ea_;' if op == 'lfsux' else ''
                if op == 'lfsux':
                    wr(ra)
                return f'{{ u32 ea_ = {ea}; {a[0]} = PS(U2F({e}));{upd} }}'
            f = a[0]
            if not (f in st.fmust or f in st.fmay or f in fpr_in):
                v = f'S2S({f[1:]})'
            else:
                v = f'FBITS({frd(f)}.p0)'
            st_ = xstore('ea_', 4, v)
            upd = f' {ra} = ea_;' if op == 'stfsux' else ''
            if op == 'stfsux':
                wr(ra)
            return f'{{ u32 ea_ = {ea}; {st_}{upd} }}'
        if direct and op in ('ps_cmpu0', 'ps_cmpo0', 'ps_cmpu1', 'ps_cmpo1'):  # fcmpu / fcmpo on one lane
            bf = int(a[0][2:])
            ln = 'p1' if op.endswith('1') else 'p0'
            x, y = frd(a[1]), frd(a[2])
            crf_wr(bf)
            flags['stops'] = True
            return (f'if (__builtin_isnan({x}.{ln}) || __builtin_isnan({y}.{ln})) {{ spc = {cur[0]:#010x}u; goto stop; }} '
                    f'cr{bf} = {x}.{ln} < {y}.{ln} ? 8u : {x}.{ln} > {y}.{ln} ? 4u : 2u; '
                    f'g->fpscr = (g->fpscr & ~FPSCR_FPCC) | (cr{bf} << 12);')
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
            if direct and not w1:
                kind, ea = addr(base, o, 8, True)
                if kind == 'X':  # both words checked before either is written
                    flags['stops'] = True
                    return (f'{{ u8* h_ = XH({ea}, 8, {cur[0]:#010x}u); STW32(h_, PSTF({f}.p0)); '
                            f'STW32(h_ + 4, PSTF({f}.p1)); }}')
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
        if op == 'stfd' and direct:  # ps0's bits: a double it holds (frs), the single it wrote, or the guest's
            f = a[0]
            k = f[1:]
            fdecl.add(f)
            base, o = mem(a[1])
            e = f'{rd(base)} + {o & 0xFFFFFFFF:#x}u'
            bits = f'(((frs >> {k}) & 1u) ? d{k} : (((fw | fw0) >> {k}) & 1u) ? DBITS((double){f}.p0) : DBITS(g->fpr[{k}].ps0))'
            flags['stops'] = True
            return (f'{{ u64 d_ = {bits}; u8* h_ = XH({e}, 8, {cur[0]:#010x}u); STW32(h_, (u32)(d_ >> 32)); '
                    f'STW32(h_ + 4, (u32)d_); }}')
        if op == 'lfd' and direct:  # ps0 a double (d<k>, frs): scalar ops, compares, moves and stores read it as one
            f = a[0]
            base, o = mem(a[1])
            e = f'{rd(base)} + {o & 0xFFFFFFFF:#x}u'
            fdbl(f)
            flags['frest'] = True
            k = f[1:]
            return (f'{{ u32 ea_ = {e}; u32 hi_ = {xload("ea_", 4)}, lo_ = {xload("ea_ + 4u", 4)}; '
                    f'd{k} = (u64)hi_ << 32 | lo_; frs |= 1u << {k}; }}')
        if op == 'stfs':  # ps0 rounded to single (an FPR argument is a double: as the interpreter rounds it)
            f = a[0]
            if direct and f in frest:
                return store(*mem(a[1]), 4, f'D2S({fval(f)})')
            if f in st.fmust or (direct and (f in st.fmay or f in fpr_in)):
                v = f'FBITS({frd(f)}.p0)'
            else:
                if f in st.fmay:
                    raise SystemExit(f'{path}: stfs {f} written on only some paths')
                v = f'S2S({f[1:]})'
            return store(*mem(a[1]), 4, v)
        if op == 'frsp':  # (the source is decided before the destination counts as written: frsp f1,f1 reads the argument)
            src = a[1]
            if direct and src in frest:
                e = f'PS((float){fval(src)})'
                fwr(a[0])
                return f'{a[0]} = {e};'
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
        if direct and op == 'ps_muls0' and a[2] in frest and a[1] not in frest:  # by a double (frsqrte's): in double, then single
            x, c_ = frd(a[1]), fval(a[2])
            fwr(a[0])
            return f'{a[0]} = (P){{ SOP(g, 2, (double){x}.p0, {c_}), SOP(g, 2, (double){x}.p1, {c_}) }};'
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
        if direct and op in ('fadds', 'fsubs', 'fmuls', 'fdivs'):  # single result in both lanes (FPSCR not kept, as the JIT)
            o = {'fadds': '+', 'fsubs': '-', 'fmuls': '*', 'fdivs': '/'}[op]
            if a[1] in frest or a[2] in frest:  # a double operand: the interpreter's soft float (frC rounded to 25 bits)
                e = f'PS(SOP(g, {"+-*/".index(o)}, {fval(a[1])}, {fval(a[2])}))'
            else:
                x, y = frd(a[1]), frd(a[2])
                e = f'PS({x}.p0 {o} {y}.p0)'
            fwr(a[0])
            return f'{a[0]} = {e};'
        if direct and op in ('fmadds', 'fmsubs', 'fnmadds', 'fnmsubs'):  # a*c +- b in double, then single (as PMADD)
            neg = '-' if op.startswith('fn') else ''
            sg = '+' if op in ('fmadds', 'fnmadds') else '-'
            if any(x in frest for x in a[1:4]):  # the interpreter's soft float (frC rounded to 25 bits, rounded once)
                e = f'PS(SMADD(g, {fval(a[1])}, {fval(a[2])}, {fval(a[3])}, {"true" if sg == "-" else "false"}, {"true" if neg else "false"}))'
            else:
                x, c_, y = frd(a[1]), frd(a[2]), frd(a[3])
                fn = 'madd1' if sg == '+' else 'msub1'
                e = f'PS({neg}{fn}({x}.p0, {c_}.p0, {y}.p0))'
            fwr(a[0])
            return f'{a[0]} = {e};'
        if direct and op in ('fneg', 'fabs', 'fmr') and a[1] in frest:  # a double moved: it stays a double
            v = {'fneg': f'-{fval(a[1])}', 'fabs': f'__builtin_fabs({fval(a[1])})', 'fmr': fval(a[1])}[op]
            k = a[0][1:]
            fdbl(a[0])
            return f'd{k} = DBITS({v}); frs |= 1u << {k};'
        if direct and op == 'fsel' and not a[0].startswith('.'):  # ps0 = frA >= 0 (NaN: no) ? frC : frB, its bits (a double)
            va, vc, vb = fval(a[1]), fval(a[2]), fval(a[3])
            k = a[0][1:]
            fdbl(a[0])
            return f'd{k} = DBITS(({va} >= 0.0) ? {vc} : {vb}); frs |= 1u << {k};'
        if direct and op == 'frsqrte':  # the interpreter's estimate (a double in ps0, ps1 as it was); positive finite only
            v = fval(a[1])
            k = a[0][1:]
            flags['stops'] = True
            fdbl(a[0])
            return (f'if (!({v} > 0.0) || __builtin_isinf({v})) {{ spc = {cur[0]:#010x}u; goto stop; }} '
                    f'd{k} = RSQRTE(g, {v}); frs |= 1u << {k};')
        if direct and op == 'fres':  # the interpreter's estimate, a single in both lanes; normal-range inputs only
            v = fval(a[1])
            flags['stops'] = True
            fwr(a[0])
            return (f'if (!(__builtin_fabs({v}) >= 0x1p-125 && __builtin_fabs({v}) <= 0x1p125)) {{ spc = {cur[0]:#010x}u; goto stop; }} '
                    f'{a[0]} = PS(RES(g, {v}));')
        if direct and op == 'fctiwz':  # the integer in ps0's low word (the interpreter's upper word), as a double's bits
            v = fval(a[1])
            k = a[0][1:]
            fdbl(a[0])
            return f'd{k} = FCTIWZ({v}); frs |= 1u << {k};'
        if direct and op in ('fneg', 'fabs', 'fmr'):  # ps0 only (ps1 stays as it was)
            x = frd(a[1])
            v = {'fneg': f'-{x}.p0', 'fabs': f'__builtin_fabsf({x}.p0)', 'fmr': f'{x}.p0'}[op]
            if a[0] == a[1] or (a[0] in st.fmust and a[0] not in p0only):
                fwr(a[0])
                return f'{a[0]} = (P){{ {v}, {a[0]}.p1 }};'
            # The destination's ps1 is not known here: lane 0 alone is written (and goes back alone, mask fw0).
            st.fmust.add(a[0]); st.fmay.add(a[0]); all_fw.add(a[0]); p0only.add(a[0])
            return f'{a[0]}.p0 = {v}; FWB0({a[0][1:]});'
        if direct and op in ('fcmpo', 'fcmpu'):  # a NaN stops before it (guest code sets the FPSCR flags); else FPCC and the field
            bf = int(a[0][2:])
            x, y = fval(a[1]), fval(a[2])
            crf_wr(bf)
            flags['stops'] = True
            return (f'{{ double x_ = {x}, y_ = {y}; if (__builtin_isnan(x_) || __builtin_isnan(y_)) {{ spc = {cur[0]:#010x}u; goto stop; }} '
                    f'cr{bf} = x_ < y_ ? 8u : x_ > y_ ? 4u : 2u; }} '
                    f'g->fpscr = (g->fpscr & ~FPSCR_FPCC) | (cr{bf} << 12);')
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
            if direct:  # after a stop: nothing reaches it except through a label
                if off == end:
                    break
                continue
            raise SystemExit(f'{path}: code at {off:x} nothing reaches')
        o = word >> 26
        if off == end:  # the final blr
            body.append('    n++;')
            break

        def emit_stop():  # direct mode: stop before this instruction (guest code runs it and goes on from there)
            flags['stops'] = True
            body.append(f'    n++; {{ spc = {off:#010x}u; goto stop; }}  /* {op} {",".join(a)} */')
            st.live = False

        def emit_call(target_c):  # direct mode: another stand-in, called in place (ps_icall finds it by address)
            flags['stops'] = flags['calls'] = True
            flags['lr'] = flags['lr_w'] = True
            out_ = [f'g->gpr[{r[1:]}] = {r};' for r in sorted(st.gd - {'r2', 'r13'}, key=lambda r: int(r[1:]))]
            if st.cd:
                m_ = 0
                for n_ in st.cd:
                    m_ |= 0xF << (28 - 4 * n_)
                out_.append(f'g->cr = (g->cr & {(~m_) & 0xFFFFFFFF:#010x}u) | ' + ' | '.join(f'(cr{n_} << {28 - 4 * n_})' for n_ in sorted(st.cd)) + ';')
            body.append(f'    n++; FSYNC(); {" ".join(out_)} {{ u32 lr0_ = g->lr, st_ = g_psStop; g->lr = {off + 4:#010x}u; '
                        f'u32 cn_ = ps_icall(g, {target_c}); g_psStop |= st_; '
                        f'if (!cn_) {{ g->lr = lr0_; spc = {off:#010x}u; goto stop; }} '
                        f'if (g_psMid) return n + cn_; n += cn_; }} SYNC_IN(); lr = {off + 4:#010x}u;')
            st.gd, st.cd = set(), set()  # (everything is in guest state now; what came back is too)
            gpr_used.update({'r3', 'r4'})  # (the result: reloaded, so a later read sees it)
            # After the call: volatile FPRs are dead (the ABI), f1 holds the result.
            for f in [f'f{k}' for k in range(0, 14)]:
                st.fmust.discard(f)
            st.fmust.add('f1'); st.fmay.add('f1'); all_fw.add('f1')

        if o == 18 and (word & 3) == 1:  # bl (direct mode): the GPR save / restore helpers in place, else a call
            li = word & 0x03FFFFFC
            li = li - 0x04000000 if li & 0x02000000 else li
            t = (off + li) & 0xFFFFFFFF
            if not HELPERS:
                raise SystemExit(f'{path}: a call (no --helpers)')
            kind = next((k for k, base in (('save', HELPERS[0]), ('rest', HELPERS[1])) if 0 <= t - base < 18 * 4 and not (t - base) & 3), None)
            if kind is None:
                emit_call(f'{t:#010x}u')
                continue
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
            dec = '' if (word >> 21) & 4 else ' ctr--;'
            if c is None:
                body.append(f'    n++;{dec} goto L_{t:x};')
                st.live = False
            else:
                body.append(f'    n++;{dec} if ({c}) goto L_{t:x};')
            continue
        if direct and o == 19 and ((word >> 1) & 0x3FF) in (16, 528) and (word & 1):  # blrl / bctrl: a call through LR / CTR
            if ((word >> 21) & 0x14) != 0x14:  # conditional: guest code
                emit_stop()
                continue
            if ((word >> 1) & 0x3FF) == 528:
                flags['ctr'] = True
                emit_call('ctr & ~3u')
            else:
                flags['lr'] = True
                emit_call('lr & ~3u')
            continue
        if direct and o == 19 and ((word >> 1) & 0x3FF) == 528:  # bctr (a jump table, a tail call): guest code
            emit_stop()
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
        fpins = op.startswith('ps') or op.startswith('lf') or op.startswith('stf') or op in (
            'frsp', 'fadds', 'fsubs', 'fmuls', 'fdivs', 'fmadds', 'fmsubs', 'fnmadds', 'fnmsubs', 'fneg', 'fabs', 'fmr', 'fcmpo',
            'fcmpu', 'fctiwz', 'frsqrte', 'fres', 'fsel')
        if direct and fpins and not st.fpck:
            # The FP state, at the first FP instruction on this path (not at entry: a path without FP code runs whatever
            # it is; the OS turns the FPU on lazily). Off: stop before it, and guest code takes the exception.
            flags['stops'] = True
            body.append(f'    if (!FPGATE()) {{ n++; spc = {off:#010x}u; goto stop; }}')
            st.fpck = True
        try:
            if op.startswith('ps') or op in ('lfs', 'lfsu', 'lfsx', 'lfsux', 'stfsx', 'stfsux', 'stfs', 'frsp', 'stfd', 'lfd', 'fadds', 'fsubs', 'fmuls', 'fdivs', 'fmadds',
                                             'fmsubs', 'fnmadds', 'fnmsubs', 'fneg', 'fabs', 'fmr', 'fcmpo', 'fcmpu', 'fctiwz', 'frsqrte',
                                             'fres', 'fsel'):
                flags['fpu'] = True
                s = fp(off, word, op, a)
            else:
                s = integer(word, off)
        except SystemExit as e_:
            if not direct:
                raise
            s = None
            why_ = str(e_).split(': ', 1)[-1]
        else:
            why_ = ''
        if s is None:
            if direct:  # anything not handled here: stop before it, guest code goes on from there
                unsupported.append(f'{off:x} {op}' + (f' ({why_})' if why_ else ''))
                emit_stop()
                continue
            raise SystemExit(f'{path}: unsupported at {off:x}: {op} {a}')
        if direct:  # the FPRs it wrote, for the write-back (only those go back: the others stay as the guest has them)
            s += ''.join(f' FWB({f[1:]});' for f in fw_last)
            s += ''.join(f' frs &= ~(1u << {f[1:]});' for f in fw_last if f in frest)  # a single written over a double
        body.append(f'    n++; {s}')
    if rend is not None:  # the region's end: everything back in guest state, the PC there
        if rend in targets:
            body.append(f'L_{rend:x}:;')
        body.append(f'    n++; {{ spc = {rend:#010x}u; goto stop; }}  /* the region ends */')
        flags['stops'] = True
    if st.frame != 0:
        raise SystemExit(f'{path}: the frame is not given back')
    if flags['fsave'] and flags['branch']:
        raise SystemExit(f'{path}: saved FPRs with branches')
    if any('L_ret' in s for s in body):
        body.append('L_ret:;')

    if direct:
        return direct_out(path, name, ins, body, flags, st, gpr_used, gpr_nv, fpr_in, written_cr, cr_in, frest, all_fw, unsupported, fdecl)

    # ---- entry: range checks, then the pointers fetched through the checked ranges ----
    pre = []
    for k in korder:  # a fetched pointer only bases x keys: it needs its source checked even when not accessed itself
        if k[0] == 'x' and xsrc[k][0][0] == 'd' and xsrc[k][0] not in access:
            access[xsrc[k][0]] = (0, 0)
    order = sorted(k for k in access if k[0] in 'rc') + [k for k in korder if k in access]

    def val(k):  # a base key's guest value at entry
        if k[0] == 'r':
            return f'g->gpr[{k[1:]}]'
        if k[0] == 'c':
            return f'0x{k[1:]}u'
        src, so = dsrc[k]
        if src not in access:
            raise SystemExit(f'{path}: {k} from an unchecked base')
        return f'BE32(H{src}({so}))'
    for k in order:
        lo, hi = access[k]
        if k[0] == 'x':
            bk, oe = xsrc[k]
            ea = f'(u32)({val(bk)} + {oe})'
        else:
            ea = val(k)
        if hi == lo:  # (only the base of x keys: its value, no access through it)
            continue
        pre.append(f'    u8* const h{k} = region({ea} + (u32)({lo}), {hi - lo});')
        pre.append(f'    if (!h{k}) return 0;')
    for dk, (src, so) in late.items():  # fetched at entry for a load after a store: no store may overlap that word
        for sk, (slo, shi) in stores.items():
            pre.append(f'    if (H{src}({so}) < H{sk}({shi}) && H{src}({so}) + 4 > H{sk}({slo})) return 0;')
    order = [k for k in order if access[k][1] > access[k][0]]
    defs = [f'#define H{k}(o) (h{k} + ((o) - ({access[k][0]})))' for k in order]
    undefs = [f'#undef H{k}' for k in order]
    # FPRs written back (f1, f14-f31) that some path leaves unwritten: their guest value is the starting value.
    wbf = sorted((f for f in st.fmay if f == 'f1' or int(f[1:]) >= 14), key=lambda x: int(x[1:]))
    for f in wbf:
        if f not in st.fmust and f not in fpr_in:
            fpr_in.append(f)
    for r in sorted(gpr_nv | gpr_vw, key=lambda r: int(r[1:])):  # written back: the guest value where a path leaves it
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
        out.append(f'    u8 stk[{flags["stack"] + 16}] __attribute__((aligned(4)));  // the frame and the LR save word above it (dead after the call)')
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
    if gpr_vw:  # (dead by the ABI; a direct-mode caller stopped after the call compares them with guest code's)
        tail.append('    if (g_psNested) { ' + ' '.join(f'g->gpr[{r[1:]}] = {r};' for r in sorted(gpr_vw, key=lambda r: int(r[1:]))) + ' }')
    if flags['stack']:
        tail.append('#undef STK')
    out += body + wb + tail + ['    return n;', '}'] + undefs
    words = [w for _, w, _, _ in ins]
    return name, len(ins) * 4, words[0], fnv(words), out, flags['pipe'], bool(gpr_nv), flags['fpu']


def direct_out(path, name, ins, body, flags, st, gpr_used, gpr_nv, fpr_in, written_cr, cr_in, frest, all_fw, unsupported, fdecl=()):
    """Direct mode's C: every used GPR, CR field and XER[CA] taken from the guest at entry (so any of them can go back
    exactly wherever it stops); FPRs read before written must be singles in both lanes (fexact) and go back only when
    written (the mask fw); lfd's restores keep the saved double (d<k>, mask frs)."""
    num = lambda x: int(x[1:])
    regs = sorted(gpr_used, key=num)
    fset = sorted(all_fw | set(fpr_in) | set(fdecl) | set(frest), key=num)
    crs = sorted(written_cr | cr_in)
    uses_ca = any('ca' in s for s in body)
    out = [f'static u32 ps_{name}(Gekko* g) {{  // {len(ins)} instructions (direct: stops where an access is not in MEM1)']
    if flags['pipe']:
        out.append('    if (!g_pipeWrite) return 0;  // (no GP to take the bytes)')
    if regs:
        out.append('    u32 ' + ', '.join(f'{r} = g->gpr[{r[1:]}]' for r in regs) + ';')
    if flags['pipe']:
        out += ['    u8 pb[PIPE_MAX];', '    u32 pn = 0;']
    if flags['lr'] or flags['lr_w']:
        out.append('    u32 lr = g->lr;')
    if flags.get('ctr'):
        out.append('    u32 ctr = g->ctr;')
    if flags['xso']:
        out.append('    const u32 xso = g->xer >> 31;  // XER[SO] into each compare')
    if crs:
        out.append('    u32 ' + ', '.join(f'cr{n} = (g->cr >> {28 - 4 * n}) & 15' for n in crs) + ';')
    out.append('    u8* const xm = g_m;  // (locals: through the globals, every store would reload them)')
    out.append('    const u32 xsz = g_size;')
    out.append('    u32 ca = (g->xer >> 29) & 1, n = 0, spc = 0, fw = 0, fw0 = 0, frs = 0, fpin = 0;')
    for f in fset:
        out.append(f'    P {f} = (P){{ 0, 0 }};  /* {"an input: loaded at the FP gate" if f in fpr_in else ""} */')
    for f in sorted(frest, key=num):
        out.append(f'    u64 d{f[1:]} = 0;')
    out.append('    ' + ' '.join(f'(void){v};' for v in regs + [f'cr{n}' for n in crs] + ['ca', 'spc', 'fw', 'fw0', 'frs', 'fpin']))

    def fprs_back(which):
        w = []
        for f in fset:
            k = num(f)
            if which == 'end' and k != 1 and k < 14:
                continue
            w.append(f'    if (fw & (1u << {k})) {{ g->fpr[{k}].ps0 = (double){f}.p0; g->fpr[{k}].ps1 = (double){f}.p1; }} '
                     f'else if (fw0 & (1u << {k})) g->fpr[{k}].ps0 = (double){f}.p0;')
        for f in sorted(frest, key=num):
            k = num(f)
            w.append(f'    if (frs & (1u << {k})) g->fpr[{k}].ps0 = BITSD(d{k});')
        return w
    # Everything back in guest state (at the end, a stop and before a call): every GPR, the written FPRs, the CR fields,
    # XER[CA], LR and CTR, and the gather pipe's bytes so far. (At the end the volatile ones are dead by the ABI, but a
    # caller stopped inside its body, a region, compares them: a few stores keep them as guest code leaves them.)
    wball = (['    PIPE_OUT(); pn = 0;'] if flags['pipe'] else [])
    wball += [f'    g->gpr[{r[1:]}] = {r};' for r in regs if r not in ('r2', 'r13')]
    wball += fprs_back('stop')
    if crs:
        mask = 0
        for n_ in crs:
            mask |= 0xF << (28 - 4 * n_)
        wball.append(f'    g->cr = (g->cr & {(~mask) & 0xFFFFFFFF:#010x}u) | ' + ' | '.join(f'(cr{n_} << {28 - 4 * n_})' for n_ in crs) + ';')
    if uses_ca or flags.get('calls'):
        wball.append('    g->xer = (g->xer & ~XER_CA) | (ca ? XER_CA : 0);')
    if flags['lr_w']:
        wball.append('    g->lr = lr;')
    if flags.get('ctr'):
        wball.append('    g->ctr = ctr;')
    end = wball + ['    return n;']
    stop = ['stop:', '    n--;  // (the instruction that stopped did not run)', '    if (!n) return 0;'] + wball
    stop += ['    g->pc = spc;', '    g_psMid = true;', '    return n;']
    if not flags.get('stops'):
        stop = []
    # The FP gate, before the first FP instruction on each path: the FP state, then the FPR inputs (singles in both
    # lanes), loaded there.
    # (Once a call: inside a loop the gate is passed again, and the inputs may have changed since.)
    gate = ' && '.join(['FPOK()'] + [f'fexact(g, {f[1:]})' for f in fpr_in])
    gate = f'fpin || ({gate} && (' + ''.join(f'({f} = FIN({f[1:]})), ' for f in fpr_in) + 'fpin = 1))'
    # An FPR written whole (FWB) or in ps0 alone (FWB0); with doubles held (frs), that one is a single again.
    clr = (lambda m: f' frs &= ~(1u << ({m}));') if frest else (lambda m: '')
    pre = [f'#define FPGATE() ({gate})',
           f'#define FWB(k) do {{ fw |= 1u << (k);{clr("k")} }} while (0)',
           f'#define FWB0(k) do {{ fw0 |= 1u << (k);{clr("k")} }} while (0)']
    post = ['#undef FPGATE', '#undef FWB', '#undef FWB0']
    if flags.get('calls'):
        # A call: the FPRs written since the last call to guest state (FSYNC; the GPRs and CR fields are written at the
        # call, from what the translator knows was written since), XER[CA], CTR and the pipe's bytes; after it, the
        # volatile GPRs, f1 (the result), the volatile CR fields, XER[CA] and CTR come back. The non-volatile registers
        # are as the callee found them (the ABI); the volatile FPRs are dead.
        fs = [l.strip() for l in fprs_back('stop')] + ['fw = 0; fw0 = 0;']
        fs += (['PIPE_OUT(); pn = 0;'] if flags['pipe'] else [])
        fs.append('g->xer = (g->xer & ~XER_CA) | (ca ? XER_CA : 0);')
        if flags.get('ctr'):
            fs.append('g->ctr = ctr;')
        pre += ['#define FSYNC() do { ' + ' '.join(fs) + ' } while (0)']
        post += ['#undef FSYNC']
        vol = {'r0'} | {f'r{k}' for k in range(3, 13)}
        sin = [f'{r} = g->gpr[{r[1:]}];' for r in regs if r in vol]
        if 'f1' in fset:
            sin.append('f1 = FIN(1);')
        sin.append('fw &= ~0x3FFFu; fw0 &= ~0x3FFFu; frs &= ~0x3FFFu;')
        sin += [f'cr{n_} = (g->cr >> {28 - 4 * n_}) & 15;' for n_ in crs if n_ in (0, 1, 5, 6, 7)]
        sin.append('ca = (g->xer >> 29) & 1;')
        if flags.get('ctr'):
            sin.append('ctr = g->ctr;')
        bs = chr(92)  # (a line continuation in the macro)
        pre += ['#define SYNC_IN() do { ' + ' '.join(sin) + ' } while (0)']
        post += ['#undef SYNC_IN']
    if unsupported:
        out[0] += '  /* stops before: ' + ', '.join(unsupported[:12]) + (' ...' if len(unsupported) > 12 else '') + ' */'
        print(f'{name}: stops before {len(unsupported)} instructions: ' + ', '.join(unsupported), file=sys.stderr)
    out = pre + out + body + end + stop + ['}'] + post
    words = [w for _, w, _, _ in ins]
    return name, len(ins) * 4, words[0], fnv(words), out, flags['pipe'], bool(gpr_nv), flags['fpu'], True


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
    fns = [f if len(f) == 9 else f + (False,) for f in fns]
    seen = {}  # (methods of different classes demangle to one name: J3DModel's and J3DMtxBuffer's calcWeightEnvelopeMtx)
    for i, f in enumerate(fns):
        name = f[0]
        if name in seen:
            seen[name] += 1
            new = f'{name}_{seen[name]}'
            fns[i] = (new,) + f[1:4] + ([l.replace(f'ps_{name}(', f'ps_{new}(') for l in f[4]],) + f[5:]
        else:
            seen[name] = 1
    for name, size, first, h, out, pipe, gprs, fpu, direct in fns:
        print('\n'.join(out))
    print('static const PsDef g_psDefs[] = {')
    for name, size, first, h, out, pipe, gprs, fpu, direct in fns:
        b = lambda x: 'true' if x else 'false'
        print(f'    {{ "{name}", {size:#x}, {first:#010x}u, {h:#018x}ull, ps_{name}, {b(pipe)}, {b(gprs)}, {b(fpu)}, {b(direct)} }},')
    print('};')
