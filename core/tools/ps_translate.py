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

    def acc(op, size):
        base, off = mem(op)
        if base in gpr_written and base not in gpr_const:
            raise SystemExit(f'{path}: memory through {base} after it changed')
        lo, hi = access.get(base, (off, off + size))
        access[base] = (min(lo, off), max(hi, off + size))
        return f'H{base[1:]}({off})'

    for off, word, op, a in ins[:-1]:
        s = None
        if op in ('psq_l', 'psq_lu', 'psq_st', 'psq_stu'):
            if a[3] != '0':
                raise SystemExit(f'{path}: {op} with GQR{a[3]}')
            if op.endswith('u'):
                raise SystemExit(f'{path}: {op} (an update form)')
            w1 = a[2] == '1'
            if op == 'psq_l':
                s = f'{a[0]} = {"LD1" if w1 else "LD2"}({acc(a[1], 4 if w1 else 8)});'
                fpr_written.add(a[0])
            else:
                s = f'{"ST1" if w1 else "ST2"}({acc(a[1], 4 if w1 else 8)}, {use_fpr(a[0])});'
        elif op == 'lfs':
            s = f'{a[0]} = LD1B({acc(a[1], 4)});'
            fpr_written.add(a[0])
        elif op == 'stfs':
            s = f'ST1({acc(a[1], 4)}, {use_fpr(a[0])});'
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
        elif op == 'stwu' and a[0] == 'r1':
            s = '/* stwu r1 (the frame: dead state) */'
        elif op == 'addi' and a[0] == 'r1' and a[1] == 'r1':
            s = '/* addi r1 */'
        elif op == 'stfd' and mem(a[1])[0] == 'r1':
            fpr_saved.add(a[0])
            s = f'/* stfd {a[0]} (saved) */'
        elif op == 'lfd' and mem(a[1])[0] == 'r1':
            if a[0] not in fpr_saved:
                raise SystemExit(f'{path}: lfd {a[0]} not saved first')
            fpr_restored.add(a[0])
            s = f'/* lfd {a[0]} (ps0 restored) */'
        elif op == 'lis':
            gpr_const[a[0]] = (int(a[1]) & 0xFFFF) << 16
            gpr_written.add(a[0])
            s = f'/* lis {a[0]} */'
        elif op == 'addi' and a[1] in gpr_const:
            gpr_const[a[0]] = (gpr_const[a[1]] + int(a[2])) & 0xFFFFFFFF
            gpr_written.add(a[0])
            s = f'/* addi {a[0]} = {gpr_const[a[0]]:#010x} */'
        else:
            raise SystemExit(f'{path}: unsupported at {off:x}: {op} {a}')
        body.append(f'    {s}')
    n = len(ins)
    # Upfront range checks (bases: argument registers, or constants formed with lis/addi).
    pre = []
    for base, (lo, hi) in sorted(access.items()):
        r = base[1:]
        ea = f'{gpr_const[base]:#010x}u' if base in gpr_const else f'g->gpr[{r}]'
        pre.append(f'    u8* const h{r} = region({ea} + (u32)({lo}), {hi - lo});')
        pre.append(f'    if (!h{r}) return 0;')
    defs = [f'#define H{base[1:]}(o) (h{base[1:]} + ((o) - ({lo})))' for base, (lo, hi) in sorted(access.items())]
    undefs = [f'#undef H{base[1:]}' for base in sorted(access)]
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
    for f in fpr_in:
        out.append(f'    if (!fexact(g, {f[1:]})) return 0;')
    out += pre
    for f in fpr_in:
        out.append(f'    P {f} = FIN({f[1:]});')
    if fl:
        out.append(f'    P {", ".join(fl)};')
    out += body + wb + [f'    return {n};', '}'] + undefs
    return name, n * 4, words[0], fnv(words), out


fns = [translate(p) for p in sys.argv[1:]]
print('// Generated by core/tools/ps_translate.py from the Dolphin SDK functions as compiled into a game\'s main.dol: one C')
print('// statement per guest instruction. Included by hle_ps.c; do not edit.')
for name, size, first, h, out in fns:
    print('\n'.join(out))
print('static const PsDef g_psDefs[] = {')
for name, size, first, h, out in fns:
    print(f'    {{ "{name}", {size:#x}, {first:#010x}u, {h:#018x}ull, ps_{name} }},')
print('};')
