#!/usr/bin/env python3
"""Translates the THP decoder's two inverse DCTs, as compiled into Twilight Princess's d_a_movie_player.rel, into C for
gc3ds's native stand-in (core/src/cpu/hle_thp.c): one statement per guest instruction, from the module's own code (the
decompilation's asm blocks are not what the module holds: their loads and pointer steps are scheduled differently, so
some paths count other instruction totals). Input: objdump's disassembly of each function's bytes, taken from the disc's
REL (devkitPPC: powerpc-eabi-objdump -D -b binary -m powerpc:750 -M gekko -EB fn.bin). Run:
    python3 thp_translate.py y8.dis noy.dis > ../src/cpu/hle_thp_idct.inc
Each function starts after its .rodata / .bss bases are formed (+0x44: r5 = .bss, r6 = .rodata) and ends at its
epilogue (the first FPR restore from r1); the 17 instructions before and the 14 after are counted as constants."""
import re, sys

HEAD = 17  # stwu, 6 x (stfd + psq_st), lis/addi x 2


def parse(path):
    out = []
    for line in open(path):
        m = re.match(r'\s*([0-9a-f]+):\t([0-9a-f ]+)\t(\S+)\s*(.*)$', line)
        if m:
            out.append((int(m.group(1), 16), m.group(3), [a.strip() for a in m.group(4).split(',')] if m.group(4) else []))
    return out


def mem(op):  # "OFF(rN)" -> ("rN", off)
    m = re.match(r'(-?\d+)\((r\d+)\)$', op)
    return m.group(2), int(m.group(1))


def ea(op):
    base, off = mem(op)
    return f'{base} + {off}' if off else base


def translate(path, fname, prefix):
    ins = parse(path)
    start = next(i for i, x in enumerate(ins) if x[0] == 0x44)
    end = next(i for i, x in enumerate(ins) if x[1] == 'psq_l' and mem(x[2][1])[0] == 'r1')
    tail = len(ins) - end
    assert ins[-1][1] == 'blr' and ins[start - 1][1] == 'addi' and ins[start - 2][1] == 'lis', (ins[start - 2:start])
    targets = set()
    for off, op, a in ins[start:end]:
        if op in ('bne-', 'bdnz+', 'b'):
            targets.add(int(a[0], 16))
    gprs, fprs, body = set(), set(), []
    for off, op, a in ins[start:end]:
        if off in targets:
            body.append(f'{prefix}{off:x}:;')
        for x in a:
            for r in re.findall(r'\b(r\d+)\b', x): gprs.add(r)
            for f in re.findall(r'\b(f\d+)\b', x): fprs.add(f)
        if op == 'psq_l':
            assert a[2] == '0' and a[3] in ('0', '5'), (off, op, a)
            s = f'{a[0]} = {"LDF2" if a[3] == "0" else "LDS16X2"}({ea(a[1])});'
        elif op == 'psq_st':
            assert a[2] == '0' and a[3] in ('0', '6'), (off, op, a)
            s = f'{"STF2" if a[3] == "0" else "STU8X2"}({ea(a[1])}, {a[0]});'
        elif op == 'psq_stu':
            assert a[2] == '0' and a[3] == '0', (off, op, a)
            base, d = mem(a[1])
            s = f'{base} += {d}; STF2({base}, {a[0]});'
        elif op == 'lfs':
            s = f'{a[0]} = PS(ldf({ea(a[1])}));'
        elif op in ('ps_add', 'ps_sub'):
            s = f'{a[0]} = {"PADD" if op == "ps_add" else "PSUB"}({a[1]}, {a[2]});'
        elif op == 'ps_mul':
            s = f'{a[0]} = PMUL({a[1]}, {a[2]});'
        elif op in ('ps_madd', 'ps_msub', 'ps_nmsub'):
            s = f'{a[0]} = P{op[3:].upper()}({a[1]}, {a[2]}, {a[3]});'
        elif op.startswith('ps_merge'):
            s = f'{a[0]} = PMERGE{op[-2:]}({a[1]}, {a[2]});'
        elif op == 'lwz':
            s = f'{a[0]} = LD32({ea(a[1])});'
        elif op == 'lhz':
            s = f'{a[0]} = LD16({ea(a[1])});'
        elif op == 'or':
            s = f'{a[0]} = {a[1]} | {a[2]};'
        elif op == 'or.':
            s = f'{a[0]} = {a[1]} | {a[2]}; cmpv = {a[0]};'
        elif op == 'cmpwi':
            assert a[1] == '0', (off, op, a)
            s = f'cmpv = {a[0]};'
        elif op == 'bne-':
            s = f'if (cmpv) goto {prefix}{int(a[0], 16):x};'
        elif op == 'addi':
            s = f'{a[0]} = {a[1]} + {int(a[2])};' if a[1] != 'r0' else f'{a[0]} = {int(a[2])};'
        elif op == 'li':
            s = f'{a[0]} = {int(a[1])};'
        elif op == 'add':
            s = f'{a[0]} = {a[1]} + {a[2]};'
        elif op == 'slwi':
            s = f'{a[0]} = {a[1]} << {int(a[2])};'
        elif op == 'mtctr':
            s = f'ctr = {a[0]};'
        elif op == 'bdnz+':
            s = f'if (--ctr) goto {prefix}{int(a[0], 16):x};'
        elif op == 'b':
            s = f'goto {prefix}{int(a[0], 16):x};'
        else:
            raise SystemExit(f'{path}: unknown instruction at {off:x}: {op} {a}')
        body.append(f'    n++; {s}' if 'goto' in s else f'    {s} n++;')
    assert ins[end][0] not in targets or True
    gprs -= {'r1'}
    params = ['r3', 'r4', 'r5', 'r6']
    locs = sorted(gprs - set(params), key=lambda r: int(r[1:]))
    fl = sorted(fprs, key=lambda f: int(f[1:]))
    out = [f'static u32 {fname}(u32 r3, u32 r4, u32 r5, u32 r6) {{']
    out.append(f'    P {", ".join(fl)};')
    out.append(f'    u32 {", ".join(locs)}, ctr, cmpv, n = {HEAD};')
    out += body
    out.append(f'    (void)cmpv; (void)ctr;')
    out.append(f'    return n + {tail};  // the epilogue: 12 FPR restores, addi r1, blr')
    out.append('}')
    return out


print('// Generated by core/tools/thp_translate.py from the THP decoder\'s inverse DCTs as compiled into Twilight Princess\'s')
print('// d_a_movie_player.rel: one C statement per guest instruction. Included by hle_thp.c; do not edit. Arguments: the')
print('// coefficients (r3), xPos (r4), the module\'s .bss (r5) and .rodata (r6) as the code forms them at +0x44.')
print('\n'.join(translate(sys.argv[1], 'idct_y8', 'Y_')))
print()
print('\n'.join(translate(sys.argv[2], 'idct_noy', 'N_')))
