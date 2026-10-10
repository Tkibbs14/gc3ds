#!/usr/bin/env python3
"""Random instruction-sequence generator for differential testing of the Gekko interpreter against qemu-ppc.

Writes one assembly file containing NCASES independent test cases. Each case loads a random architectural state
(GPRs, FPRs, CR, XER, LR, CTR, FPSCR) and a random 512-byte scratch buffer from its own data block, runs a random
sequence, and stores the final state into the same block. At the end the program write()s the whole data section to
stdout and exits, so qemu-ppc and the interpreter (running the same ELF) can be compared byte for byte.

Data layout (per case, 1536 bytes, offsets from the case base; r31 = base + 768 while the body runs):
    0   GPR[32] in        128 FPR[32] in       384 CR  388 XER  392 LR  396 CTR  400 FPSCR (as a double)
    408 reservation-clearing dummy word
    512 scratch[512]      (r31 - 256 .. r31 + 255)
    1024 GPR[32] out      1152 FPR[32] out     1408 CR 1412 XER 1416 LR 1420 CTR 1424 FPSCR (mffs, as a double)
Header at the start of .data: magic 'GEKO', case count, then (body start, body end) code addresses per case.

Never generated (see run_fuzz.sh for reasons): paired singles and other Gekko-only instructions, privileged
instructions, traps, sc, mftb, fres/frsqrte (estimates), FPSCR enable/NI bits, invalid forms.
"""
import random
import struct
import sys

CASE_SIZE = 1536
R31_OFF = 768
OUT = 1024 - R31_OFF  # out area relative to r31


def f32_to_f64_bits(w):
    return struct.unpack('>Q', struct.pack('>d', struct.unpack('>f', struct.pack('>I', w))[0]))[0] \
        if (w >> 23) & 0xFF != 0xFF else ((w >> 31) << 63) | (0x7FF << 52) | ((w & 0x7FFFFF) << 29)


class Gen:
    def __init__(self, rnd, classes):
        self.r = rnd
        self.classes = classes
        self.lines = []
        self.label_n = 0

    # ---- random values ----
    def gpr_value(self):
        r = self.r
        k = r.random()
        if k < 0.40:
            return r.getrandbits(32)
        if k < 0.60:
            return r.randint(-16, 16) & 0xFFFFFFFF
        if k < 0.75:
            return r.choice([0, 1, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0x80000001, 0xFFFF, 0x8000, 0x10000,
                             0x7FFF, 0xFFFF8000, 2, 31, 32, 33, 63, 64])
        if k < 0.90:
            v = 1 << r.randint(0, 31)
            return (v + r.choice([-1, 0, 1])) & 0xFFFFFFFF
        return r.getrandbits(16)

    def fpr_value(self):
        r = self.r
        k = r.random()
        s = r.getrandbits(1) << 63
        if k < 0.22:  # moderate doubles
            return s | ((1023 + r.randint(-30, 30)) << 52) | r.getrandbits(52)
        if k < 0.44:  # single-representable values
            w = (r.getrandbits(1) << 31) | (r.randint(100, 160) << 23) | r.getrandbits(23)
            return f32_to_f64_bits(w)
        if k < 0.52:  # small exact values
            v = r.randint(-40, 40) / r.choice([1, 2, 4, 8, 3])
            return struct.unpack('>Q', struct.pack('>d', v))[0]
        if k < 0.67:
            return r.choice([
                0, 1 << 63, 0x7FF0000000000000, 0xFFF0000000000000,
                0x7FF8000000000000, 0xFFF8000000000000, 0x7FF8000000000123, 0x7FFC00000000ABCD,
                0x7FF0000000000001, 0x7FF4000000000000, 0xFFF0000012345678,  # SNaNs
                0x7FEFFFFFFFFFFFFF, 0x0010000000000000, 0x0000000000000001, 0x000FFFFFFFFFFFFF,
                0x47EFFFFFE0000000, 0x47EFFFFFF0000000, 0x47F0000000000000, 0x3810000000000000,
                0x3800000000000000, 0x36A0000000000000, 0x3690000000000000, 0x37F0000000000000,
                0x3FF0000000000000, 0x3FE0000000000000, 0x41E0000000000000, 0xC1E0000000000000,
                0x41DFFFFFFFC00000, 0xC1E0000000200000, 0x41E0000000100000, 0x4330000000000001,
            ]) ^ (s if r.random() < 0.3 else 0)
        if k < 0.75:  # single-rounding ties and near-ties
            w = (r.randint(110, 150) << 23) | r.getrandbits(23)
            base = f32_to_f64_bits(w)
            return s | base | (1 << 28) | (r.choice([0, 0, 1, (1 << 27)]) if r.random() < 0.5 else 0)
        if k < 0.85:  # any exponent
            return s | (r.randint(0, 2046) << 52) | r.getrandbits(52)
        if k < 0.92:  # tiny doubles around the single denormal range
            return s | (r.randint(860, 900) << 52) | r.getrandbits(52)
        return r.getrandbits(64)

    def label(self):
        self.label_n += 1
        return 'L%d' % self.label_n

    def tmp(self, avoid=()):
        while True:
            t = self.r.randint(1, 30)
            if t not in avoid:
                return t

    def reg(self):
        return self.r.randint(0, 30)

    def anyreg(self):
        return self.r.randint(0, 31)

    def fr(self):
        return self.r.randint(0, 31)

    def crf(self):
        return self.r.randint(0, 7)

    def emit(self, s):
        self.lines.append('\t' + s)

    # ---- instruction classes ----
    def g_arith(self):
        r = self.r
        op = r.choice(['add', 'addc', 'adde', 'subf', 'subfc', 'subfe', 'mullw', 'divw', 'divwu',
                       'addme', 'addze', 'subfme', 'subfze', 'neg', 'mulhw', 'mulhwu'])
        oe = 'o' if op not in ('mulhw', 'mulhwu') and r.random() < 0.4 else ''
        rc = '.' if r.random() < 0.5 else ''
        if op in ('addme', 'addze', 'subfme', 'subfze', 'neg'):
            self.emit('%s%s%s %d,%d' % (op, oe, rc, self.reg(), self.anyreg()))
        else:
            self.emit('%s%s%s %d,%d,%d' % (op, oe, rc, self.reg(), self.anyreg(), self.anyreg()))

    def g_imm(self):
        r = self.r
        op = r.choice(['addi', 'addis', 'addic', 'addic.', 'subfic', 'mulli', 'ori', 'oris', 'xori', 'xoris',
                       'andi.', 'andis.', 'cmpwi', 'cmplwi'])
        if op in ('addi', 'addis', 'addic', 'addic.', 'subfic', 'mulli'):
            a = self.anyreg() if op not in ('addi', 'addis') else r.randint(0, 31)
            self.emit('%s %d,%d,%d' % (op, self.reg(), a, r.randint(-32768, 32767)))
        elif op == 'cmpwi':
            self.emit('cmpwi %d,%d,%d' % (self.crf(), self.anyreg(), r.randint(-32768, 32767)))
        elif op == 'cmplwi':
            self.emit('cmplwi %d,%d,%d' % (self.crf(), self.anyreg(), r.randint(0, 65535)))
        else:
            self.emit('%s %d,%d,%d' % (op, self.reg(), self.anyreg(), r.randint(0, 65535)))

    def g_logic(self):
        r = self.r
        rc = '.' if r.random() < 0.5 else ''
        op = r.choice(['and', 'andc', 'or', 'orc', 'xor', 'nand', 'nor', 'eqv', 'slw', 'srw', 'sraw',
                       'srawi', 'extsb', 'extsh', 'cntlzw', 'rlwinm', 'rlwimi', 'rlwnm', 'cmpw', 'cmplw'])
        if op in ('extsb', 'extsh', 'cntlzw'):
            self.emit('%s%s %d,%d' % (op, rc, self.reg(), self.anyreg()))
        elif op == 'srawi':
            self.emit('srawi%s %d,%d,%d' % (rc, self.reg(), self.anyreg(), r.randint(0, 31)))
        elif op in ('rlwinm', 'rlwimi'):
            self.emit('%s%s %d,%d,%d,%d,%d' % (op, rc, self.reg(), self.anyreg(), r.randint(0, 31),
                                               r.randint(0, 31), r.randint(0, 31)))
        elif op == 'rlwnm':
            self.emit('rlwnm%s %d,%d,%d,%d,%d' % (rc, self.reg(), self.anyreg(), self.anyreg(),
                                                  r.randint(0, 31), r.randint(0, 31)))
        elif op in ('cmpw', 'cmplw'):
            self.emit('%s %d,%d,%d' % (op, self.crf(), self.anyreg(), self.anyreg()))
        else:
            self.emit('%s%s %d,%d,%d' % (op, rc, self.reg(), self.anyreg(), self.anyreg()))

    def g_cr(self):
        r = self.r
        op = r.choice(['crand', 'crandc', 'creqv', 'crnand', 'crnor', 'cror', 'crorc', 'crxor', 'mcrf', 'mcrxr',
                       'mfcr', 'mtcrf', 'mtxer', 'mfxer', 'mtlr', 'mflr', 'mfctr', 'mtctr', 'sync', 'isync',
                       'eieio'])
        if op.startswith('cr'):
            self.emit('%s %d,%d,%d' % (op, r.randint(0, 31), r.randint(0, 31), r.randint(0, 31)))
        elif op == 'mcrf':
            self.emit('mcrf %d,%d' % (self.crf(), self.crf()))
        elif op == 'mcrxr':
            self.emit('mcrxr %d' % self.crf())
        elif op == 'mfcr':
            self.emit('mfcr %d' % self.reg())
        elif op == 'mtcrf':
            self.emit('mtcrf %d,%d' % (r.randint(0, 255), self.anyreg()))
        elif op == 'mtxer':
            t = self.tmp()
            # Only SO/OV/CA and the byte count: the other XER bits are reserved on the 750 (see run_fuzz.sh).
            self.emit('lis %d,0xE000' % t)
            self.emit('ori %d,%d,0x7F' % (t, t))
            self.emit('and %d,%d,%d' % (t, t, self.anyreg()))
            self.emit('mtxer %d' % t)
        elif op in ('mfxer', 'mflr', 'mfctr'):
            self.emit('%s %d' % (op, self.reg()))
        elif op in ('mtlr', 'mtctr'):
            self.emit('%s %d' % (op, self.anyreg()))
        else:
            self.emit(op)

    def mem_off(self, size, align):
        o = self.r.randint(-256, 256 - size)
        return o - (o % align)

    def g_mem(self):
        r = self.r
        k = r.random()
        loads = [('lwz', 4), ('lbz', 1), ('lhz', 2), ('lha', 2)]
        stores = [('stw', 4), ('stb', 1), ('sth', 2)]
        if k < 0.45:
            op, size = r.choice(loads + stores)
            align = 1 if r.random() < 0.3 else size
            form = r.choice(['d', 'u', 'x', 'ux'])
            rd = self.reg()
            if form == 'd':
                self.emit('%s %d,%d(31)' % (op, rd, self.mem_off(size, align)))
            elif form == 'u':
                t = self.tmp(avoid=(rd,))
                base = r.randint(-128, 128)
                self.emit('addi %d,31,%d' % (t, base))
                d = r.randint(-128, 128 - size)
                d -= (base + d) % align
                self.emit('%su %d,%d(%d)' % (op, rd, d, t))
            elif form == 'x':
                t = self.tmp()
                self.emit('li %d,%d' % (t, self.mem_off(size, align)))
                self.emit('%sx %d,31,%d' % (op, rd, t))
            else:
                t = self.tmp(avoid=(rd,))
                t2 = self.tmp(avoid=(t,))
                base = r.randint(-128, 128)
                d = r.randint(-128, 128 - size)
                d -= (base + d) % align
                self.emit('addi %d,31,%d' % (t, base))
                self.emit('li %d,%d' % (t2, d))
                self.emit('%sux %d,%d,%d' % (op, rd, t, t2))
        elif k < 0.55:
            op, size = r.choice([('lwbrx', 4), ('lhbrx', 2), ('stwbrx', 4), ('sthbrx', 2)])
            t = self.tmp()
            self.emit('li %d,%d' % (t, self.mem_off(size, 1)))
            self.emit('%s %d,31,%d' % (op, self.reg(), t))
        elif k < 0.62:
            if r.random() < 0.5:
                rd = r.randint(18, 30)
                n = 32 - rd
                d = r.randint(-256, 256 - 4 * n) & ~3
                self.emit('stw 31,%d(31)' % (d + 4 * (31 - rd)))  # lmw reloads r31 with its own value
                t = r.randint(1, rd - 1)  # base register outside the loaded range (rA in range is invalid)
                self.emit('addi %d,31,%d' % (t, d))
                self.emit('lmw %d,0(%d)' % (rd, t))
            else:
                rs = r.randint(0, 30)
                n = 32 - rs
                d = r.randint(-256, 256 - 4 * n) & ~3
                self.emit('stmw %d,%d(31)' % (rs, d))
        elif k < 0.72:
            self.g_string()
        elif k < 0.82:
            t = self.tmp()
            t2 = self.tmp(avoid=(t,))
            off = self.mem_off(4, 4)
            self.emit('li %d,%d' % (t, off))
            self.emit('lwarx %d,31,%d' % (self.reg(), t))
            for _ in range(r.randint(0, 2)):
                self.g_logic()
            off2 = off if r.random() < 0.7 else self.mem_off(4, 4)
            self.emit('li %d,%d' % (t2, off2))
            self.emit('stwcx. %d,31,%d' % (self.reg(), t2))
        else:
            op = r.choice(['dcbz', 'dcbz', 'dcbt', 'dcbtst', 'dcbf', 'dcbst', 'icbi'])
            t = self.tmp()
            self.emit('li %d,%d' % (t, r.randint(-256, 255)))
            self.emit('%s 31,%d' % (op, t))

    def g_string(self):
        r = self.r
        op = r.choice(['lswi', 'lswx', 'stswi', 'stswx'])
        n = r.randint(1, 32) if op.endswith('i') else r.randint(0, 40)
        nregs = max((n + 3) // 4, 1)  # the assembler rejects rA/rB == rD even for a zero byte count
        if op.startswith('l'):
            rd = r.randint(1, 30 - nregs + 1)
            rng = set(range(rd, rd + nregs))
        else:
            rd = r.randint(0, 31)
            rng = set()
        base = self.tmp(avoid=rng)
        off = r.randint(-256, 256 - max(n, 1))
        if op.endswith('i'):
            self.emit('addi %d,31,%d' % (base, off))
            self.emit('%s %d,%d,%d' % (op, rd, base, n % 32))
        else:
            idx = self.tmp(avoid=rng | {base})
            x = self.tmp(avoid={base, idx})
            self.emit('mfxer %d' % x)
            self.emit('rlwinm %d,%d,0,0,24' % (x, x))
            self.emit('ori %d,%d,%d' % (x, x, n))
            self.emit('mtxer %d' % x)
            self.emit('addi %d,31,%d' % (base, off - 16))
            self.emit('li %d,16' % idx)
            self.emit('%s %d,%d,%d' % (op, rd, base, idx))

    def g_fp(self):
        r = self.r
        rc = '.' if r.random() < 0.3 else ''
        k = r.random()
        if k < 0.55:
            op = r.choice(['fadd', 'fsub', 'fmul', 'fdiv', 'fadds', 'fsubs', 'fmuls', 'fdivs',
                           'fmadd', 'fmsub', 'fnmadd', 'fnmsub', 'fmadds', 'fmsubs', 'fnmadds', 'fnmsubs', 'fsel'])
            if op in ('fmul', 'fmuls'):
                self.emit('%s%s %d,%d,%d' % (op, rc, self.fr(), self.fr(), self.fr()))
            elif op.startswith('f') and ('madd' in op or 'msub' in op or op == 'fsel'):
                self.emit('%s%s %d,%d,%d,%d' % (op, rc, self.fr(), self.fr(), self.fr(), self.fr()))
            else:
                self.emit('%s%s %d,%d,%d' % (op, rc, self.fr(), self.fr(), self.fr()))
        elif k < 0.75:
            op = r.choice(['fneg', 'fabs', 'fnabs', 'fmr', 'frsp', 'frsp', 'fctiw', 'fctiwz', 'fctiw'])
            self.emit('%s%s %d,%d' % (op, rc, self.fr(), self.fr()))
        elif k < 0.85:
            self.emit('%s %d,%d,%d' % (r.choice(['fcmpu', 'fcmpo']), self.crf(), self.fr(), self.fr()))
        else:
            op = r.choice(['mffs', 'mtfsf', 'mtfsfi', 'mtfsb0', 'mtfsb1', 'mcrfs'])
            if op == 'mffs':
                self.emit('mffs%s %d' % (rc, self.fr()))
            elif op == 'mtfsf':
                # Fields 0-5 only: fields 6-7 hold the enable bits and NI, kept clear (qemu does not model them).
                self.emit('mtfsf%s %d,%d' % (rc, r.randint(0, 255) & 0xFC, self.fr()))
            elif op == 'mtfsfi':
                f = r.randint(0, 7)
                imm = r.randint(0, 15)
                if f == 6:
                    imm = 0
                if f == 7:
                    imm &= 3
                self.emit('mtfsfi%s %d,%d' % (rc, f, imm))
            elif op in ('mtfsb0', 'mtfsb1'):
                b = r.randint(0, 31)
                if op == 'mtfsb1':
                    while 24 <= b <= 29:
                        b = r.randint(0, 31)
                self.emit('%s%s %d' % (op, rc, b))
            else:
                self.emit('mcrfs %d,%d' % (self.crf(), self.crf()))

    def g_fmem(self):
        r = self.r
        op, size = r.choice([('lfs', 4), ('lfd', 8), ('stfs', 4), ('stfd', 8), ('stfiwx', 4)])
        fd = self.fr()
        if op == 'stfiwx':
            t = self.tmp()
            self.emit('li %d,%d' % (t, self.mem_off(4, 4)))
            self.emit('stfiwx %d,31,%d' % (fd, t))
            return
        form = r.choice(['d', 'u', 'x', 'ux'])
        if form == 'd':
            self.emit('%s %d,%d(31)' % (op, fd, self.mem_off(size, 4)))
        elif form == 'u':
            t = self.tmp()
            base = r.randint(-128, 128) & ~3
            self.emit('addi %d,31,%d' % (t, base))
            self.emit('%su %d,%d(%d)' % (op, fd, r.randint(-128, 128 - size) & ~3, t))
        elif form == 'x':
            t = self.tmp()
            self.emit('li %d,%d' % (t, self.mem_off(size, 4)))
            self.emit('%sx %d,31,%d' % (op, fd, t))
        else:
            t = self.tmp()
            t2 = self.tmp(avoid=(t,))
            self.emit('addi %d,31,%d' % (t, r.randint(-128, 128) & ~3))
            self.emit('li %d,%d' % (t2, r.randint(-128, 128 - size) & ~3))
            self.emit('%sux %d,%d,%d' % (op, fd, t, t2))

    def simple(self):
        k = self.r.random()
        if k < 0.4:
            self.g_logic()
        elif k < 0.7:
            self.g_imm()
        else:
            self.g_arith()

    # ---- branches ----
    BO_COND = [4, 5, 12, 13, 20]
    BO_ALL = [0, 1, 2, 3, 4, 5, 8, 9, 10, 11, 12, 13, 16, 17, 18, 19, 20]

    def g_branch(self, fwd_labels):
        r = self.r
        tgt = r.choice(fwd_labels)
        k = r.random()
        if k < 0.15:
            self.emit('%s %s' % (r.choice(['b', 'bl']), tgt))
        elif k < 0.55:
            self.emit('%s %d,%d,%s' % (r.choice(['bc', 'bcl']), r.choice(self.BO_ALL), r.randint(0, 31), tgt))
        elif k < 0.75:
            t = self.tmp()
            self.emit('lis %d,%s@ha' % (t, tgt))
            self.emit('addi %d,%d,%s@l' % (t, t, tgt))
            self.emit('mtlr %d' % t)
            self.emit('%s %d,%d' % (r.choice(['bclr', 'bclrl']), r.choice(self.BO_ALL), r.randint(0, 31)))
        elif k < 0.9:
            t = self.tmp()
            self.emit('lis %d,%s@ha' % (t, tgt))
            self.emit('addi %d,%d,%s@l' % (t, t, tgt))
            self.emit('mtctr %d' % t)
            self.emit('%s %d,%d' % (r.choice(['bcctr', 'bcctrl']), r.choice(self.BO_COND), r.randint(0, 31)))
        else:
            # Counted loop: the body never touches CTR, so every decrementing form terminates.
            t = self.tmp()
            self.emit('li %d,%d' % (t, r.randint(1, 4)))
            self.emit('mtctr %d' % t)
            top = self.label()
            self.lines.append('%s:' % top)
            for _ in range(r.randint(1, 4)):
                self.simple()
            self.emit('bc %d,%d,%s' % (r.choice([0, 2, 8, 10, 16, 18]), r.randint(0, 31), top))

    def body(self, n):
        labels = [self.label() for _ in range(r_int(self.r, 1, 4))]
        end = self.label()
        positions = sorted(self.r.sample(range(n), min(len(labels), n)))
        lab_at = dict(zip(positions, labels[:len(positions)]))
        weights = [(c, w) for c, w in (('arith', 3), ('imm', 2), ('logic', 3), ('cr', 2), ('mem', 3),
                                       ('fp', 4), ('fmem', 2), ('branch', 1)) if c in self.classes]
        total = sum(w for _, w in weights)
        for k in range(n):
            if k in lab_at:
                self.lines.append('%s:' % lab_at[k])
            x = self.r.uniform(0, total)
            for c, w in weights:
                x -= w
                if x <= 0:
                    break
            later = [lab_at[p] for p in lab_at if p > k] + [end]
            getattr(self, 'g_' + c)(*([later] if c == 'branch' else []))
        self.lines.append('%s:' % end)


def r_int(r, a, b):
    return r.randint(a, b)


def main():
    seed = int(sys.argv[1])
    ncases = int(sys.argv[2])
    out = sys.argv[3]
    classes = sys.argv[4].split(',') if len(sys.argv) > 4 else ['arith', 'imm', 'logic', 'cr', 'mem', 'fp', 'fmem',
                                                                   'branch']
    r = random.Random(seed)
    g = Gen(r, classes)
    text = ['\t.text', '\t.globl _start', '_start:']
    data = ['\t.data', 'data_begin:', '\t.long 0x47454B4F, %d' % ncases]
    for c in range(ncases):
        data.append('\t.long body%d, bodyend%d' % (c, c))
    data.append('\t.balign 512')
    for c in range(ncases):
        g.lines = []
        cb = 'case%d' % c
        L = text.append
        L('\tlis 31,%s@ha' % cb)
        L('\taddi 31,31,%s@l' % cb)
        L('\taddi 0,31,408')
        L('\tstwcx. 0,0,0')  # drop any reservation left by the previous case
        L('\tlwz 0,384(31)')
        L('\tmtcrf 255,0')
        L('\tlwz 0,388(31)')
        L('\tmtxer 0')
        L('\tlwz 0,392(31)')
        L('\tmtlr 0')
        L('\tlwz 0,396(31)')
        L('\tmtctr 0')
        L('\tlfd 0,400(31)')
        L('\tmtfsf 255,0')
        for k in range(32):
            L('\tlfd %d,%d(31)' % (k, 128 + 8 * k))
        for k in range(31):
            L('\tlwz %d,%d(31)' % (k, 4 * k))
        L('\taddi 31,31,%d' % R31_OFF)
        L('body%d:' % c)
        g.body(r.randint(8, 40))
        text.extend(g.lines)
        L('bodyend%d:' % c)
        for k in range(31):
            L('\tstw %d,%d(31)' % (k, OUT + 4 * k))
        L('\tmfcr 0')
        L('\tstw 0,%d(31)' % (OUT + 384))
        L('\tmfxer 0')
        L('\tstw 0,%d(31)' % (OUT + 388))
        L('\tmflr 0')
        L('\tstw 0,%d(31)' % (OUT + 392))
        L('\tmfctr 0')
        L('\tstw 0,%d(31)' % (OUT + 396))
        for k in range(32):
            L('\tstfd %d,%d(31)' % (k, OUT + 128 + 8 * k))
        L('\tmffs 0')
        L('\tstfd 0,%d(31)' % (OUT + 400))
        # Case data.
        gprs = [g.gpr_value() for _ in range(32)]
        fprs = [g.fpr_value() for _ in range(32)]
        cr = r.getrandbits(32)
        xer = r.getrandbits(32) & 0xE000007F
        lr = r.getrandbits(32)
        ctr = r.getrandbits(32)
        fpscr = r.getrandbits(32) & 0x9FFFF703  # no FEX/VX (computed), reserved bit 20, enables or NI
        data.append('%s:' % cb)
        data.append('\t.long ' + ','.join('0x%08X' % v for v in gprs))
        data.append('\t.quad ' + ','.join('0x%016X' % v for v in fprs))
        data.append('\t.long 0x%08X,0x%08X,0x%08X,0x%08X' % (cr, xer, lr, ctr))
        data.append('\t.long 0,0x%08X' % fpscr)
        data.append('\t.long 0,0')
        data.append('\t.balign 512')
        data.append('\t.long ' + ','.join('0x%08X' % r.getrandbits(32) for _ in range(128)))
        data.append('\t.space 512')
    data.append('data_end:')
    size = ((8 + 8 * ncases + 511) // 512) * 512 + ncases * CASE_SIZE
    text += ['\tlis 4,data_begin@ha', '\taddi 4,4,data_begin@l', '\tlis 5,0x%X' % (size >> 16),
             '\tori 5,5,0x%X' % (size & 0xFFFF), '\tli 3,1', '\tli 0,4', '\tsc', '\tli 3,0', '\tli 0,1', '\tsc']
    with open(out, 'w') as f:
        f.write('\n'.join(text + data) + '\n')


if __name__ == '__main__':
    main()
