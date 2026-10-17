#!/usr/bin/env python3
"""Translates small paired-single SDK leaf functions (the Dolphin SDK's PSMTX / PSVEC library and the like, as
compiled into a game's main.dol) into C for gc3ds's native stand-ins (core/src/cpu/hle_ps.c): one statement per guest
instruction, straight-line code only. Input: objdump's disassembly of each function's bytes (devkitPPC:
powerpc-eabi-objdump -D -b binary -m powerpc:750 -M gekko -EB --adjust-vma=ADDR fn.bin). Run:
    python3 ps_translate.py NAME.dis ... > ../src/cpu/hle_ps_gen.inc

Each function becomes `static u32 ps_NAME(Gekko* g)`: it checks every guest range it will touch first (refusing with 0
before any store), runs the instructions in order (so overlapping arguments behave as on the guest), writes back f1
(the return value) and each of f14-f31 it changed as the guest leaves it (one saved with stfd and restored with lfd
keeps ps0 and takes ps1; f0 and f2-f13 are volatile and dead after the call), and returns the instruction count. An
FPR read before it is written is an argument, taken only when both lanes are singles (checked at entry); branches,
loops and any instruction not listed here are refused at translation time."""
import re, struct, sys

FNV = 0xCBF29CE484222325


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


def translate(path):
    ins = parse(path)
    name = re.sub(r'__.*$', '', path.split('/')[-1][:-4])
    assert ins[-1][2] == 'blr', (path, 'does not end in blr')
    gpr_const, gpr_written, fpr_written, fpr_saved, fpr_restored = {}, set(), set(), set(), set()
    access = {}  # base register -> [lo, hi)
    body = []

    fpr_in = []  # FPRs read before written here: arguments, taken only when both lanes are singles (checked at entry)

    def use_fpr(f):
        if f in fpr_restored:
            raise SystemExit(f'{path}: {f} read after lfd restored it')
        if f not in fpr_written and f not in fpr_in:
            fpr_in.append(f)
        return f

    # Memory: each access is through a base the translator classifies. 'r3' etc.: an entry register not written before
    # (range checked at entry); 'cXXXXXXXX': a constant address formed with lis/addi; 'dN': a pointer the function loads
    # from one of those before any store to RAM (fetched and its range checked at entry too); the gather pipe
    # (0xCC008000) collects bytes in order and goes to the GP once at the end.
    derived = {}  # register -> (key, its source key, offset)
    dsrc = {}     # derived key -> (source key, offset)
    stored = [False]
    gpr_in, gpr_set = [], set()

    def rd_gpr(r):  # a GPR read
        if r not in gpr_set and r not in gpr_in:
            gpr_in.append(r)
        return r

    def wr_gpr(r, const=None):
        if r in ('r1', 'r2', 'r13') or int(r[1:]) >= 14:
            raise SystemExit(f'{path}: writes {r} (non-volatile or reserved)')
        gpr_set.add(r)
        gpr_written.add(r)
        derived.pop(r, None)
        if const is None:
            gpr_const.pop(r, None)
        else:
            gpr_const[r] = const & 0xFFFFFFFF

    def key_of(base):
        if base in gpr_const:
            return f'c{gpr_const[base]:08x}', gpr_const[base]
        if base in derived:
            return derived[base][0], None
        if base in gpr_written:
            raise SystemExit(f'{path}: memory through {base} after it changed')
        rd_gpr(base)
        return base, None

    def acc_key(base, off, size):
        if base == 'r0':
            raise SystemExit(f'{path}: d(0) addressing')
        k, cv = key_of(base)
        if cv is not None and (cv + off) & 0xFFFFFFFF == 0xCC008000:
            return 'PIPE'
        lo, hi = access.get(k, (off, off + size))
        access[k] = (min(lo, off), max(hi, off + size))
        return f'H{k}({off})'

    def acc(op, size, store=False):
        base, off = mem(op)
        h = acc_key(base, off, size)
        if store and h != 'PIPE':
            stored[0] = True
        return h

    def rotl_mask(mb, me):
        m = 0
        i = mb
        while True:
            m |= 1 << (31 - i)
            if i == me:
                break
            i = (i + 1) & 31
        return m

    pipe_used = [False]

    def integer(word, off):  # integer instructions, decoded from the word: C, or None
        op, rD, rA, rB = word >> 26, f'r{(word >> 21) & 31}', f'r{(word >> 16) & 31}', f'r{(word >> 11) & 31}'
        simm = word & 0xFFFF
        simm = simm - 0x10000 if simm & 0x8000 else simm
        uimm = word & 0xFFFF
        if op == 14 or op == 15:  # addi / addis (li / lis when rA = 0)
            v = simm << 16 if op == 15 else simm
            if rA == 'r0':
                wr_gpr(rD, v)
                return f'{rD} = {v & 0xFFFFFFFF:#x}u;'
            if rA == 'r1' and rD == 'r1':
                return '/* addi r1 (the frame) */'
            c = gpr_const.get(rA)
            src = rd_gpr(rA) if rA not in gpr_set else rA
            wr_gpr(rD, None if c is None else c + v)
            return f'{rD} = {src} + {v & 0xFFFFFFFF:#x}u;'
        if op == 7:
            e = f'{rd_gpr(rA)} * (u32){simm}'
            wr_gpr(rD)
            return f'{rD} = {e};'
        if op == 8:  # subfic (XER[CA] is volatile: dead)
            e = f'(u32){simm} - {rd_gpr(rA)}'
            wr_gpr(rD)
            return f'{rD} = {e};'
        if op in (24, 25, 28):  # ori, oris, andi. (CR0 volatile: dead)
            rS, rA2 = rD, rA
            v = uimm << 16 if op == 25 else uimm
            e = f'{rd_gpr(rS)} {"&" if op == 28 else "|"} {v:#x}u'
            wr_gpr(rA2)
            return f'{rA2} = {e};'
        if op == 21:  # rlwinm (rlwinm.: CR0 dead)
            rS, sh, mb, me = rD, (word >> 11) & 31, (word >> 6) & 31, (word >> 1) & 31
            e = f'ROTL({rd_gpr(rS)}, {sh}) & {rotl_mask(mb, me):#010x}u'
            wr_gpr(rA)
            return f'{rA} = {e};'
        if op == 31:
            xo = (word >> 1) & 0x3FF
            if xo == 26:
                e = f'CLZ({rd_gpr(rD)})'
                wr_gpr(rA)
                return f'{rA} = {e};'
            if xo in (316, 444, 28):
                o = {316: '^', 444: '|', 28: '&'}[xo]
                if xo == 316 and rD == rB:
                    wr_gpr(rA, 0)
                    return f'{rA} = 0;'
                e = f'{rd_gpr(rD)} {o} {rd_gpr(rB)}'
                wr_gpr(rA)
                return f'{rA} = {e};'
            if xo in (266, 40):  # add, subf
                e = f'{rd_gpr(rA)} + {rd_gpr(rB)}' if xo == 266 else f'{rd_gpr(rB)} - {rd_gpr(rA)}'
                wr_gpr(rD)
                return f'{rD} = {e};'
            return None
        if op in (32, 34, 40):  # lwz, lbz, lhz
            size = {32: 4, 34: 1, 40: 2}[op]
            h = acc_key(rA, simm, size)
            if h == 'PIPE':
                raise SystemExit(f'{path}: a load from the pipe')
            fn = {32: 'BE32', 34: '*', 40: 'BE16'}[op]
            e = f'*({h})' if size == 1 else f'{fn}({h})'
            k, cv = key_of(rA)
            wr_gpr(rD)
            if op == 32 and not stored[0]:  # a pointer it may go through: fetchable at entry
                dk = f'd{len(dsrc)}'
                dsrc[dk] = (k, simm)
                derived[rD] = (dk, k, simm)
            return f'{rD} = {e};'
        if op in (36, 37, 38, 44):  # stw, stwu, stb, sth
            size = {36: 4, 37: 4, 38: 1, 44: 2}[op]
            if op == 37 and rA == 'r1':
                return '/* stwu r1 (the frame: dead state) */'
            h = acc(f'{simm}({rA})', size, store=True)
            v = rd_gpr(rD) if rD not in gpr_set else rD
            if h == 'PIPE':
                pipe_used[0] = True
                s = f'PIPE{size * 8}({v});'
            else:
                s = {4: f'SBE32({h}, {v});', 2: f'SBE16({h}, {v});', 1: f'*({h}) = (u8){v};'}[size]
            if op == 37:  # the update: rA = the address
                c = gpr_const.get(rA)
                src = rA
                wr_gpr(rA, None if c is None else c + simm)
                s += f' {rA} = {src} + {simm & 0xFFFFFFFF:#x}u;'
            return s
        return None

    for off, word, op, a in ins[:-1]:
        s = None
        if not op.startswith('ps') and op not in ('lfs', 'stfs', 'frsp', 'stfd', 'lfd'):
            s = integer(word, off)
            if s is None:
                raise SystemExit(f'{path}: unsupported at {off:x}: {op} {a}')
            body.append(f'    {s}')
            continue
        if op in ('psq_l', 'psq_lu', 'psq_st', 'psq_stu'):
            if a[3] != '0':
                raise SystemExit(f'{path}: {op} with GQR{a[3]}')
            if op.endswith('u'):
                raise SystemExit(f'{path}: {op} (an update form)')
            w1 = a[2] == '1'
            if op == 'psq_l':
                h = acc(a[1], 4 if w1 else 8)
                if h == 'PIPE':
                    raise SystemExit(f'{path}: a load from the pipe')
                s = f'{a[0]} = {"LD1" if w1 else "LD2"}({h});'
                fpr_written.add(a[0])
            else:
                h = acc(a[1], 4 if w1 else 8, store=True)
                if h == 'PIPE':
                    pipe_used[0] = True
                    s = f'PIPEF{1 if w1 else 2}({use_fpr(a[0])});'
                else:
                    s = f'{"ST1" if w1 else "ST2"}({h}, {use_fpr(a[0])});'
        elif op == 'lfs':
            s = f'{a[0]} = LD1B({acc(a[1], 4)});'
            fpr_written.add(a[0])
        elif op == 'stfs':
            h = acc(a[1], 4, store=True)
            if h == 'PIPE':
                raise SystemExit(f'{path}: stfs to the pipe (its rounding of a double is not modelled)')
            s = f'ST1({h}, {use_fpr(a[0])});'
        elif op == 'frsp':
            src = a[1]  # an FPR argument (a double) rounded to single, into both lanes
            s = f'{a[0]} = PS({src}.p0);' if src in fpr_written or src in fpr_in else f'{a[0]} = PS((float)g->fpr[{src[1:]}].ps0);'
            fpr_written.add(a[0])
        elif op in ('ps_add', 'ps_sub'):
            s = f'{a[0]} = {"PADD" if op == "ps_add" else "PSUB"}({use_fpr(a[1])}, {use_fpr(a[2])});'
            fpr_written.add(a[0])
        elif op in ('ps_mul', 'ps_muls0', 'ps_muls1'):
            s = f'{a[0]} = P{op[3:].upper()}({use_fpr(a[1])}, {use_fpr(a[2])});'
            fpr_written.add(a[0])
        elif op in ('ps_madd', 'ps_msub', 'ps_nmsub', 'ps_madds0', 'ps_madds1'):
            s = f'{a[0]} = P{op[3:].upper()}({use_fpr(a[1])}, {use_fpr(a[2])}, {use_fpr(a[3])});'
            fpr_written.add(a[0])
        elif op in ('ps_sum0', 'ps_sum1'):
            s = f'{a[0]} = P{op[3:].upper()}({use_fpr(a[1])}, {use_fpr(a[2])}, {use_fpr(a[3])});'
            fpr_written.add(a[0])
        elif op.startswith('ps_merge'):
            s = f'{a[0]} = PMERGE{op[-2:]}({use_fpr(a[1])}, {use_fpr(a[2])});'
            fpr_written.add(a[0])
        elif op == 'ps_neg':
            s = f'{a[0]} = PNEG({use_fpr(a[1])});'
            fpr_written.add(a[0])
        elif op == 'ps_mr':
            s = f'{a[0]} = {use_fpr(a[1])};'
            fpr_written.add(a[0])
        elif op == 'stfd' and mem(a[1])[0] == 'r1':
            fpr_saved.add(a[0])
            s = f'/* stfd {a[0]} (saved) */'
        elif op == 'lfd' and mem(a[1])[0] == 'r1':
            if a[0] not in fpr_saved:
                raise SystemExit(f'{path}: lfd {a[0]} not saved first')
            fpr_restored.add(a[0])
            s = f'/* lfd {a[0]} (ps0 restored) */'
        else:
            raise SystemExit(f'{path}: unsupported at {off:x}: {op} {a}')
        body.append(f'    {s}')
    n = len(ins)
    # Upfront range checks: entry registers and constants first, then the pointers fetched through them.
    pre = []
    order = sorted((k for k in access if k[0] != 'd')) + sorted((k for k in access if k[0] == 'd'))
    for k in order:
        lo, hi = access[k]
        if k[0] == 'r':
            ea = f'g->gpr[{k[1:]}]'
        elif k[0] == 'c':
            ea = f'0x{k[1:]}u'
        else:
            src, o = dsrc[k]
            if src not in access:
                raise SystemExit(f'{path}: {k} from an unchecked base')
            ea = f'BE32(H{src}({o}))'
        pre.append(f'    u8* const h{k} = region({ea} + (u32)({lo}), {hi - lo});')
        pre.append(f'    if (!h{k}) return 0;')
    defs = [f'#define H{k}(o) (h{k} + ((o) - ({access[k][0]})))' for k in order]
    undefs = [f'#undef H{k}' for k in order]
    gdecl = [f'    u32 {r} = g->gpr[{r[1:]}];' for r in gpr_in]
    others = sorted(gpr_set - set(gpr_in), key=lambda r: int(r[1:]))
    if others:
        gdecl.append(f'    u32 {", ".join(others)};')
    used = [r for r in gpr_in] + others
    if used:
        gdecl.append('    ' + ' '.join(f'(void){r};' for r in used))
    if pipe_used[0]:
        gdecl.append('    u8 pb[PIPE_MAX];')
        gdecl.append('    u32 pn = 0;')
    wb = []
    for f in sorted(fpr_written, key=lambda x: int(x[1:])):
        k = f[1:]
        if int(k) != 1 and int(k) < 14:
            continue  # f0, f2-f13: volatile, dead once the function returns (the ABI; owner decision 2026-10-15)
        if f in fpr_restored:
            wb.append(f'    g->fpr[{k}].ps1 = (double){f}.p1;')
        else:
            wb.append(f'    g->fpr[{k}].ps0 = (double){f}.p0; g->fpr[{k}].ps1 = (double){f}.p1;')
    words = [w for _, w, _, _ in ins]
    fl = sorted(set(fpr_written) - set(fpr_in), key=lambda x: int(x[1:]))
    out = defs + [f'static u32 ps_{name}(Gekko* g) {{  // {n} instructions']
    if pipe_used[0]:
        out.append('    if (!g_pipeWrite) return 0;  // (no GP to take the bytes)')
    for f in fpr_in:
        out.append(f'    if (!fexact(g, {f[1:]})) return 0;')
    out += gdecl[:len(gpr_in)]  # the entry registers (the range checks read them)
    out += pre
    out += gdecl[len(gpr_in):]
    for f in fpr_in:
        out.append(f'    P {f} = FIN({f[1:]});')
    if fl:
        out.append(f'    P {", ".join(fl)};')
    tail = ['    PIPE_OUT();'] if pipe_used[0] else []
    out += body + wb + tail + [f'    return {n};', '}'] + undefs
    return name, n * 4, words[0], fnv(words), out, pipe_used[0]


fns = [translate(p) for p in sys.argv[1:]]
print('// Generated by core/tools/ps_translate.py from the Dolphin SDK functions as compiled into a game\'s main.dol: one C')
print('// statement per guest instruction. Included by hle_ps.c; do not edit.')
for name, size, first, h, out, pipe in fns:
    print('\n'.join(out))
print('static const PsDef g_psDefs[] = {')
for name, size, first, h, out, pipe in fns:
    print(f'    {{ "{name}", {size:#x}, {first:#010x}u, {h:#018x}ull, ps_{name}, {"true" if pipe else "false"} }},')
print('};')
