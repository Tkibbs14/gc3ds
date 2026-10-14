#!/usr/bin/env python3
"""Ahead-of-time translation of a game's PowerPC code into C (redesign candidate C12, docs/research/AOT_PLAN.md).

    python3 core/tools/aot/ppc2c.py --dol main.dol --symbols symbols.txt --hist hist.bin --out core/src/aotgen [--files 16]

Every selected guest function becomes one C function, `u32 F_xxxxxxxx(Gekko* g, u32 entry, s32 budget)`, that runs the
function from one of its entries (its start, the instruction after each call, the head of each loop) until it leaves:
a call, a return, a branch out of the function, the budget used up at a loop's back edge, or something only the
interpreter may do. It returns the guest instructions it ran and leaves g->pc where guest code goes on (core/src/cpu/aot.c
runs the next function from there, or hands back to the JIT).

Guest registers live in C locals inside a function (loaded at entry, stored at every exit), so the host compiler
allocates them across the whole function. Instructions that are not translated run in the interpreter, one at a time,
with every register stored before and reloaded after, so the translation is exact wherever it does not translate.
Floating point follows the JIT's "fast float" model (jit.c): results equal the interpreter's for normal operands, the
multiply-add forms round the product first, and FPSCR keeps only what fcmpu / fcmpo set.

The output is derived from the user's own disc: it is never committed (core/src/aotgen is ignored).
"""
import argparse, array, os, re, struct, sys, zlib

MASK = 0xFFFFFFFF


def s16(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def hx(v):
    return '0x%08Xu' % (v & MASK)


# ---------------------------------------------------------------------------------------------------- input
def load_dol(path):
    d = open(path, 'rb').read()
    off = struct.unpack('>18I', d[0:72]); addr = struct.unpack('>18I', d[72:144]); size = struct.unpack('>18I', d[144:216])
    secs = []
    for i in range(18):
        if size[i]:
            secs.append((addr[i], size[i], d[off[i]:off[i] + size[i]], i < 7))
    return secs


class Mem:
    def __init__(self, secs):
        self.secs = secs

    def word(self, a):
        for base, size, data, text in self.secs:
            if base <= a < base + size and text:
                o = a - base
                return struct.unpack('>I', data[o:o + 4])[0]
        return None

    def in_text(self, a):
        return any(base <= a < base + size and text for base, size, data, text in self.secs)


def load_symbols(path):
    out = []
    pat = re.compile(r'^(\S+) = \.text:0x([0-9A-Fa-f]+); // type:function size:0x([0-9A-Fa-f]+)')
    for line in open(path, encoding='utf-8', errors='replace'):
        m = pat.match(line)
        if m:
            out.append((int(m.group(2), 16), int(m.group(3), 16), m.group(1)))
    out.sort()
    return out


# ---------------------------------------------------------------------------------------------------- decoding
def fields(i):
    return dict(op=i >> 26, rd=(i >> 21) & 31, ra=(i >> 16) & 31, rb=(i >> 11) & 31, rc=(i >> 6) & 31,
                xo=(i >> 1) & 0x3FF, xo5=(i >> 1) & 31, rcb=i & 1, oe=(i >> 10) & 1, simm=s16(i), uimm=i & 0xFFFF)


def branch_info(pc, i):
    """('b'|'bc'|'bclr'|'bcctr', target or None, lk, bo, bi) for branches, else None."""
    op = i >> 26
    if op == 18:
        li = ((i & 0x03FFFFFC) ^ 0x02000000) - 0x02000000
        t = li if i & 2 else pc + li
        return ('b', t & MASK, i & 1, 20, 0)
    if op == 16:
        bd = s16(i & 0xFFFC)
        t = bd if i & 2 else pc + bd
        return ('bc', t & MASK, i & 1, (i >> 21) & 31, (i >> 16) & 31)
    if op == 19:
        xo = (i >> 1) & 0x3FF
        if xo == 16:
            return ('bclr', None, i & 1, (i >> 21) & 31, (i >> 16) & 31)
        if xo == 528:
            return ('bcctr', None, i & 1, (i >> 21) & 31, (i >> 16) & 31)
    return None


def is_poll_load(op):
    return op in (32, 34, 40, 42)


def poll_loop(words, start_idx, n):
    """gekko_poll_loop_len over the loop words[start_idx:start_idx+n] + the closing branch (cpu/gekko.c)."""
    allw = 0
    rw = []
    for k in range(n):
        i = words[start_idx + k]
        o, rd, ra, rb = i >> 26, (i >> 21) & 31, (i >> 16) & 31, (i >> 11) & 31
        r = w = 0
        if is_poll_load(o):
            r = (1 << ra) if ra else 0; w = 1 << rd
        elif o in (10, 11):
            r = 1 << ra
        elif o == 31 and ((i >> 1) & 0x3FF) in (0, 32):
            r = (1 << ra) | (1 << rb)
        elif o in (28, 21, 24):
            r = 1 << rd; w = 1 << ra
        else:
            return False
        rw.append((r, w)); allw |= w
    seen = 0
    for r, w in rw:
        if r & allw & ~seen:
            return False
        seen |= w
    return True


# ---------------------------------------------------------------------------------------------------- translation
class Fn:
    def __init__(self, start, size, name, mem):
        self.start, self.size, self.name = start, size, name
        self.end = start + size
        self.words = [mem.word(a) for a in range(start, self.end, 4)]
        self.ok = all(w is not None for w in self.words)


class Translator:
    """One function to C."""

    def __init__(self, fn, stats, fnset, calltargets=()):
        self.calltargets = calltargets  # addresses inside this function that code anywhere calls (bl): entries
        self.f = fn
        self.st = stats
        self.fnset = fnset  # starts of every translated function (for nothing yet: calls always leave)
        self.gpr = set(); self.fpr = set(); self.crf = set()
        self.use_lr = self.use_ctr = self.use_xer = self.use_fpscr = False
        self.wgpr = set(); self.wfpr = set(); self.wcrf = set()
        self.w_lr = self.w_ctr = self.w_xer = self.w_fpscr = False
        self.fallbacks = []  # (k, pc, inst) interpreter fallbacks
        self.lines = []
        self.lp = ''          # label prefix (an inlined callee's labels are its call site's)
        self.ret_label = None  # inlined leaf: where its blr goes
        self.inl = {}         # start -> Fn of small leaf functions inlined at their call sites
        self.deps = []        # (start, size) of inlined callees: the translation is valid only while they match too
        self.ninl = 0
        self.gidx = 0         # this function's index (aot_valid, the profile)
        self.attr = ''
        self.abi = True       # direct calls trust the EABI's callee-saved registers (--abi 0: reload everything)
        self.fidx = {}        # start -> index of every translated function (direct calls)
        self.dcalls = []      # direct call sites: their resume labels
        # Register liveness (two passes, see main): pass 1 records what each instruction reads and writes; pass 2 loads
        # at entry only what is read before it is written (E), and keeps a "written" bit per other register (U) so that
        # exits store only what this function actually wrote.
        self.rec_root = self  # where reads / writes are recorded (an inlined callee records into its call site)
        self.cur_r, self.cur_w, self.cur_wmay, self.cur_kind = set(), set(), set(), None
        self.rw = {}          # pass 1: k -> (reads, writes, kind)
        self.wids = set()     # every id written
        self.ids = set()      # every id used
        self.E = None         # pass 2: ids loaded at entry (None in pass 1)
        self.all_w = set()    # pass 2: every id this function writes (pass 1's wids): the dirty bits
        self.all_ids = set()  # pass 2: every id this function uses (pass 1's ids)
        self.live_after = {}  # pass 2: call site k -> ids read after the call before written (reloaded there)
        self.ubit = {}        # pass 2: U id -> bit in the dm words

    # -- helpers -------------------------------------------------------------------------------------------
    def _rec(self, ident, w):
        self.ids.add(ident)
        if w:
            self.cur_w.add(ident)
            self.wids.add(ident)
        else:
            self.cur_r.add(ident)
        root = self.rec_root
        if root is not self:
            root.ids.add(ident)
            if w: root.cur_wmay.add(ident)
            else: root.cur_r.add(ident)

    def R(self, r, write=False):
        self.gpr.add(r)
        if write: self.wgpr.add(r)
        self._rec(('r', r), write)
        return 'r%d' % r

    def RA0(self, r):
        return '0u' if r == 0 else self.R(r)

    def F(self, r, lane=0, write=False):
        self.fpr.add(r)
        if write: self.wfpr.add(r)
        self._rec(('f' if lane == 0 else 'p', r), write)
        return ('f%d' if lane == 0 else 'p%d') % r

    def CR(self, f, write=False):
        self.crf.add(f)
        if write: self.wcrf.add(f)
        self._rec(('c', f), write)
        return 'c%d' % f

    def Q(self, i):
        """GQR i, read-only here (mtspr to a GQR runs in the interpreter, which reloads every register after)."""
        self._rec(('q', i), False)
        return 'q%d' % i

    def LR(self, write=False):
        self.use_lr = True
        if write: self.w_lr = True
        self._rec(('lr',), write)
        return 'lr'

    def CTR(self, write=False):
        self.use_ctr = True
        if write: self.w_ctr = True
        self._rec(('ctr',), write)
        return 'ctr'

    def XER(self, write=False):
        self.use_xer = True
        if write: self.w_xer = True
        self._rec(('xer',), write)
        return 'xer'

    def crbit(self, b):
        return '((%s >> %d) & 1u)' % (self.CR(b >> 2), 3 - (b & 3))

    def set_crbit(self, b, expr):
        f = b >> 2; sh = 3 - (b & 3)
        self.CR(f)  # read as well: the other bits stay
        c = self.CR(f, True)
        return '%s = (%s & ~%du) | ((%s) << %d);' % (c, c, 1 << sh, expr, sh)

    def cr0(self, d):
        return '%s = CR_OF(%s, %s);' % (self.CR(0, True), d, self.XER())

    def single(self, r, lane):
        return (r, lane) in self.known

    # -- analysis ------------------------------------------------------------------------------------------
    def analyse(self):
        f = self.f
        n = len(f.words)
        leaders = {0}
        self.post_call = set()
        self.backedge = {}  # branch index -> target index (backward, inside)
        self.poll_heads = set()
        self.bctr_switch = set()
        for k, i in enumerate(f.words):
            pc = f.start + 4 * k
            bi = branch_info(pc, i)
            if bi:
                kind, t, lk, bo, b = bi
                if k + 1 < n: leaders.add(k + 1)
                if lk and k + 1 < n: self.post_call.add(k + 1)
                if t is not None and f.start <= t < f.end and not lk:
                    tk = (t - f.start) // 4
                    leaders.add(tk)
                    if tk <= k:
                        self.backedge[k] = tk
                        if (bo & 4) and not (i & 3) and 0 < k - tk < 8 and poll_loop(f.words, tk, k - tk):
                            self.poll_heads.add(tk)
            elif i in (0x4C00012C, 0x4C000064) or (i >> 26) == 17:  # isync is a no-op; rfi / sc leave anyway
                if k + 1 < n: leaders.add(k + 1)
        for t in self.calltargets:
            if f.start < t < f.end: leaders.add((t - f.start) // 4)
        self.leaders = sorted(leaders)
        # Entries: the start, after each call, and loop heads (not polling loops: the JIT dispatcher skips those).
        ent = {0} | self.post_call | set(self.backedge.values()) | {(t - f.start) // 4 for t in self.calltargets if f.start < t < f.end}
        ent -= self.poll_heads
        ent -= {k for k in range(n) if f.words[k] == 0x48000000}
        if 0 in self.poll_heads: ent.discard(0)
        self.entries = sorted(e for e in ent if e < n)
        # Basic blocks: from each leader to the next leader (or the end).
        self.blocks = []
        for idx, s in enumerate(self.leaders):
            e = self.leaders[idx + 1] if idx + 1 < len(self.leaders) else n
            self.blocks.append((s, e))
        self.block_of = {}
        for s, e in self.blocks:
            for k in range(s, e): self.block_of[k] = (s, e)
        self.single_analysis()

    def single_analysis(self):
        """Lanes (fpr, lane) known to hold an exact single at each instruction (forward, meet = intersection)."""
        f = self.f
        n = len(f.words)
        ALL = None  # top
        state_in = {s: ALL for s, e in self.blocks}
        ent = set(self.entries)
        for s in ent: state_in[s] = frozenset()
        state_in[0] = frozenset()
        succ = {}
        self.succ = succ
        for s, e in self.blocks:
            last = e - 1
            i = f.words[last]
            pc = f.start + 4 * last
            bi = branch_info(pc, i)
            out = []
            if bi:
                kind, t, lk, bo, b = bi
                if kind in ('b', 'bc') and t is not None and f.start <= t < f.end and not lk:
                    out.append((t - f.start) // 4)
                cond = not (kind == 'b' or (bo & 0x14) == 0x14)
                if (cond or lk) and e < n: out.append(e)
                if kind == 'bcctr' and not lk:
                    out.extend(x for x, y in self.blocks)  # a jump table may go anywhere in the function
            elif e < n:
                out.append(e)
            succ[s] = out
        changed = True
        self.known_at = {}
        while changed:
            changed = False
            for s, e in self.blocks:
                st = state_in[s]
                if st is ALL: continue
                cur = set(st)
                for k in range(s, e):
                    self.known_at[k] = frozenset(cur)
                    cur = self.single_step(f.words[k], cur)
                for t in succ[s]:
                    old = state_in[t]
                    new = frozenset(cur) if old is ALL else old & frozenset(cur)
                    if t in ent: new = frozenset()
                    if new != old:
                        state_in[t] = new; changed = True
        for k in range(n):
            self.known_at.setdefault(k, frozenset())

    @staticmethod
    def single_step(i, cur):
        op = i >> 26; rd = (i >> 21) & 31; ra = (i >> 16) & 31; rb = (i >> 11) & 31; rc = (i >> 6) & 31
        xo = (i >> 1) & 0x3FF; xo5 = (i >> 1) & 31
        def both(r): cur.add((r, 0)); cur.add((r, 1))
        def kill0(r): cur.discard((r, 0))
        def kill(r): cur.discard((r, 0)); cur.discard((r, 1))
        def cp(d, s, ls=0, ld=0):
            if (s, ls) in cur: cur.add((d, ld))
            else: cur.discard((d, ld))
        if op in (48, 49): both(rd)
        elif op == 31 and xo in (535, 567): both(rd)
        elif op in (50, 51) or (op == 31 and xo in (599, 631)): kill0(rd)
        elif op in (56, 57) or (op == 4 and (i >> 1) & 63 in (6, 38)): both(rd)
        elif op == 59: both(rd)
        elif op == 63:
            if xo in (0, 32, 38, 70, 64, 134, 711): pass
            elif xo == 12: both(rd)
            elif xo in (72, 40, 264, 136): cp(rd, rb)
            elif xo5 == 23:
                if (rb, 0) in cur and (rc, 0) in cur: cur.add((rd, 0))
                else: kill0(rd)
            else: kill0(rd)
        elif op == 4:
            x6 = (i >> 1) & 63
            if xo in (0, 32, 64, 96) or x6 in (7, 39) or xo == 1014: pass
            elif xo in (72, 40, 264, 136):
                a0, a1 = (rb, 0) in cur, (rb, 1) in cur
                kill(rd)
                if a0: cur.add((rd, 0))
                if a1: cur.add((rd, 1))
            elif xo in (528, 560, 592, 624):
                l0 = (ra, 0 if xo in (528, 560) else 1) in cur
                l1 = (rb, 0 if xo in (528, 592) else 1) in cur
                kill(rd)
                if l0: cur.add((rd, 0))
                if l1: cur.add((rd, 1))
            elif xo5 == 23:
                s0 = (rb, 0) in cur and (rc, 0) in cur; s1 = (rb, 1) in cur and (rc, 1) in cur
                kill(rd)
                if s0: cur.add((rd, 0))
                if s1: cur.add((rd, 1))
            else: both(rd)
        elif op in (52, 53, 54, 55, 60, 61) or op < 48:
            pass
        else:
            cur.clear()
        return cur

    # -- emission ------------------------------------------------------------------------------------------
    def emit(self):
        f = self.f
        self.analyse()
        body = []
        n = len(f.words)
        self.fb_sites = []  # (label id) for fallback resumption
        for s, e in self.blocks:
            body.append('%sL%d:' % (self.lp, s))
            body.append('    n += %d; AOT_PROF_ADD(%d, %d);' % (e - s, self.gidx, e - s))
            for k in range(s, e):
                self.k = k; self.bs, self.be = s, e
                self.known = self.known_at[k]
                pc = f.start + 4 * k
                self.cur_r, self.cur_w, self.cur_wmay, self.cur_kind = set(), set(), set(), None
                code = self.insn(pc, f.words[k])
                self.rw[k] = (frozenset(self.cur_r), frozenset(self.cur_w), self.cur_kind)
                code = code + self.dm_or(self.cur_w)
                for c in code:
                    body.append('    ' + c)
            last = f.words[e - 1]
            bi = branch_info(f.start + 4 * (e - 1), last)
            falls = not bi or not (bi[0] == 'b' or (bi[0] != 'b' and (bi[3] & 0x14) == 0x14 and not bi[2]))
            if falls and e >= n:
                body.append('    npc = %s; goto out;' % hx(f.start + 4 * n))
        return body

    def exit_after(self, npc_expr):
        """Leave after the current instruction completed (it counted)."""
        adj = self.be - 1 - self.k
        # what this instruction wrote before leaving (LR of a bl, CTR of a bdnz) is dirty before the exit's flush
        dirty = ' '.join(self.dm_or(self.cur_w))
        return '%snpc = %s;%s goto out;' % (dirty + ' ' if dirty else '', npc_expr, (' n -= %d;' % adj) if adj else '')

    def exit_before(self, pc):
        adj = self.be - self.k
        return 'npc = %s; n -= %d; goto out;' % (hx(pc), adj)

    def adj_after(self):
        return self.be - 1 - self.k

    def fallback(self, pc, i):
        k = len(self.fallbacks)
        self.fallbacks.append((k, pc, i))
        key = fields(i)
        self.st['fb'][(key['op'], key['xo'] if key['op'] in (4, 19, 31, 59, 63) else 0)] = \
            self.st['fb'].get((key['op'], key['xo'] if key['op'] in (4, 19, 31, 59, 63) else 0), 0) + 1
        self.cur_kind = 'sync'
        return ['fb_k = %d; fb_pc = %s; fb_inst = %s; fb_adj = %d; goto fallback;' % (k, hx(pc), hx(i), self.adj_after()),
                'FB%d:;' % k]

    # memory --------------------------------------------------------------------------------------------------
    def load(self, size, dst, ea, pc, i, sign=False, rev=False):
        fn = {1: 'LD8', 2: 'LD16', 4: 'LD32'}[size]
        conv = ''
        if size == 2 and sign: conv = '(u32)(s32)(s16)'
        if rev: fn = {2: 'LD16R', 4: 'LD32R'}[size]
        fix = {(2, True): ' v_ = BSWAP16((u16)v_);', (4, True): ' v_ = BSWAP32(v_);'}.get((size, rev), '')
        return ['{ u32 ea_ = %s, v_; if (__builtin_expect(!%s(ea_, &v_), 0)) { SLOW_LD(%d, ea_, &v_, %s, %d, %d);%s } %s = %s v_; }'
                % (ea, fn, size, hx(pc), self.adj_after(), self.be - self.k, fix, dst, conv)]

    def store(self, size, val, ea, pc, rev=False):
        fn = {1: 'ST8', 2: 'ST16', 4: 'ST32'}[size]
        if rev: fn = {2: 'ST16R', 4: 'ST32R'}[size]
        sv = {(2, True): '(u32)BSWAP16((u16)v_)', (4, True): 'BSWAP32(v_)'}.get((size, rev), 'v_')
        return ['{ u32 ea_ = %s, v_ = %s; if (__builtin_expect(!%s(ea_, v_), 0)) { SLOW_ST(%d, ea_, %s, %s, %d, %d); } }'
                % (ea, val, fn, size, sv, hx(pc), self.adj_after(), self.be - self.k)]

    def ea_d(self, d):
        return '%s + %du' % (self.RA0(d['ra']), d['simm'] & MASK) if d['simm'] else self.RA0(d['ra'])

    def ea_du(self, d):
        return '%s + %du' % (self.R(d['ra']), d['simm'] & MASK) if d['simm'] else self.R(d['ra'])

    def ea_x(self, d):
        return '%s + %s' % (self.RA0(d['ra']), self.R(d['rb']))

    def ea_xu(self, d):
        return '%s + %s' % (self.R(d['ra']), self.R(d['rb']))

    # instructions ---------------------------------------------------------------------------------------------
    def insn(self, pc, i):
        d = fields(i)
        op = d['op']
        try:
            r = self.insn_(pc, i, d, op)
        except NotImplementedError:
            r = None
        if r is None:
            return self.fallback(pc, i)
        self.st['native'] += 1
        return r

    def insn_(self, pc, i, d, op):
        rd, ra, rb, rc = d['rd'], d['ra'], d['rb'], d['rc']
        R = self.R
        if op == 14: return ['%s = %s;' % (R(rd, 1), '%s + %du' % (self.RA0(ra), d['simm'] & MASK) if ra else hx(d['simm']))]
        if op == 15: return ['%s = %s;' % (R(rd, 1), '%s + %s' % (self.RA0(ra), hx(d['uimm'] << 16)) if ra else hx(d['uimm'] << 16))]
        if op == 24: return ['%s = %s | %du;' % (R(ra, 1), R(rd), d['uimm'])]
        if op == 25: return ['%s = %s | %s;' % (R(ra, 1), R(rd), hx(d['uimm'] << 16))]
        if op == 26: return ['%s = %s ^ %du;' % (R(ra, 1), R(rd), d['uimm'])]
        if op == 27: return ['%s = %s ^ %s;' % (R(ra, 1), R(rd), hx(d['uimm'] << 16))]
        if op == 28: return ['%s = %s & %du;' % (R(ra, 1), R(rd), d['uimm']), self.cr0(R(ra))]
        if op == 29: return ['%s = %s & %s;' % (R(ra, 1), R(rd), hx(d['uimm'] << 16)), self.cr0(R(ra))]
        # multiplies in unsigned arithmetic: the same low 32 bits, and no signed-overflow undefined behaviour (which GCC
        # for ARM optimised differently from GCC for x86 in the audio mixer's fixed-point code)
        if op == 7: return ['%s = %s * %s;' % (R(rd, 1), R(ra), hx(d['simm']))]
        if op in (12, 13):
            out = ['{ u64 s_ = (u64)%s + %s; %s = (%s & ~XER_CA) | ((u32)(s_ >> 32) << 29); %s = (u32)s_; }'
                   % (R(ra), hx(d['simm']), self.XER(1), self.XER(), R(rd, 1))]
            if op == 13: out.append(self.cr0(R(rd)))
            return out
        if op == 8:
            return ['{ u64 s_ = (u64)(u32)~%s + %s + 1u; %s = (%s & ~XER_CA) | ((u32)(s_ >> 32) << 29); %s = (u32)s_; }'
                    % (R(ra), hx(d['simm']), self.XER(1), self.XER(), R(rd, 1))]
        if op == 10:
            return ['%s = CMPU(%s, %du, %s);' % (self.CR(rd >> 2, 1), R(ra), d['uimm'], self.XER())]
        if op == 11:
            return ['%s = CMPS(%s, %d, %s);' % (self.CR(rd >> 2, 1), R(ra), d['simm'], self.XER())]
        if op in (20, 21, 23):
            mb, me, sh = (i >> 6) & 31, (i >> 1) & 31, rb
            m = rlw_mask(mb, me)
            if op == 23: rot = 'ROTL(%s, %s & 31u)' % (R(rd), R(rb))
            elif sh: rot = 'ROTL(%s, %d)' % (R(rd), sh)
            else: rot = R(rd)
            if op == 20:
                out = ['%s = (%s & %s) | (%s & %s);' % (R(ra, 1), rot, hx(m), R(ra), hx(~m))]
            else:
                out = ['%s = %s & %s;' % (R(ra, 1), rot, hx(m))]
            if d['rcb']: out.append(self.cr0(R(ra)))
            return out
        if op in (16, 18, 19):
            return self.branch(pc, i, d, op)
        if op in (32, 33, 34, 35, 40, 41, 42, 43):
            size = {32: 4, 33: 4, 34: 1, 35: 1, 40: 2, 41: 2, 42: 2, 43: 2}[op]
            upd = op & 1
            if upd and (ra == 0 or ra == rd): return None
            ea = self.ea_du(d) if upd else self.ea_d(d)
            out = self.load(size, R(rd, 1), ea, pc, i, sign=op in (42, 43))
            if upd: out = ['{ u32 ua_ = %s;' % ea] + self.load(size, R(rd, 1), 'ua_', pc, i, sign=op in (42, 43)) + ['%s = ua_; }' % R(ra, 1)]
            return out
        if op in (36, 37, 38, 39, 44, 45):
            size = {36: 4, 37: 4, 38: 1, 39: 1, 44: 2, 45: 2}[op]
            upd = op & 1
            if upd and ra == 0: return None
            if upd:
                return ['{ u32 ua_ = %s;' % self.ea_du(d)] + self.store(size, R(rd), 'ua_', pc) + ['%s = ua_; }' % R(ra, 1)]
            return self.store(size, R(rd), self.ea_d(d), pc)
        if op == 46:  # lmw
            if ra >= rd and ra != 0: return None
            out = ['{ u32 lm_ = %s;' % self.ea_d(d)]
            for r in range(rd, 32):
                out += self.load(4, R(r, 1), 'lm_ + %du' % (4 * (r - rd)), pc, i)
            out.append('}')
            return out
        if op == 47:  # stmw
            out = ['{ u32 lm_ = %s;' % self.ea_d(d)]
            for r in range(rd, 32):
                out += self.store(4, R(r), 'lm_ + %du' % (4 * (r - rd)), pc)
            out.append('}')
            return out
        if op == 31: return self.op31(pc, i, d)
        if op in (48, 49, 50, 51, 52, 53, 54, 55, 56, 57, 60, 61): return self.fmem(pc, i, d, op)
        if op == 59: return self.op59(pc, i, d)
        if op == 63: return self.op63(pc, i, d)
        if op == 4: return self.op4(pc, i, d)
        return None

    def arith(self, d, x, y, cin, setca):
        R = self.R
        out = ['{ u64 s_ = (u64)(%s) + (u64)(%s) + (u64)(%s);' % (x, y, cin)]
        if setca: out.append('%s = (%s & ~XER_CA) | ((u32)(s_ >> 32) << 29);' % (self.XER(1), self.XER()))
        out.append('%s = (u32)s_; }' % R(d['rd'], 1))
        if d['rcb']: out.append(self.cr0(R(d['rd'])))
        return out

    def op31(self, pc, i, d):
        R = self.R
        xo, rd, ra, rb = d['xo'], d['rd'], d['ra'], d['rb']
        cain = '((%s >> 29) & 1u)' % self.XER() if xo & 0x1FF in (136, 138, 200, 202, 232, 234) else '0'
        if xo & 0x200 and (xo & 0x1FF) in (8, 10, 40, 104, 136, 138, 200, 202, 232, 234, 235, 266, 459, 491):
            return None  # OE = 1: overflow forms run in the interpreter
        a, b = (R(ra) if ra or True else '0'), R(rb)
        if xo == 266: return self.arith(d, R(ra), R(rb), '0u', 0)
        if xo == 10: return self.arith(d, R(ra), R(rb), '0u', 1)
        if xo == 138: return self.arith(d, R(ra), R(rb), cain, 1)
        if xo == 202: return self.arith(d, R(ra), '0u', cain, 1)
        if xo == 234: return self.arith(d, R(ra), '0xFFFFFFFFu', cain, 1)
        if xo == 40: return self.arith(d, '(u32)~%s' % R(ra), R(rb), '1u', 0)
        if xo == 8: return self.arith(d, '(u32)~%s' % R(ra), R(rb), '1u', 1)
        if xo == 136: return self.arith(d, '(u32)~%s' % R(ra), R(rb), cain, 1)
        if xo == 200: return self.arith(d, '(u32)~%s' % R(ra), '0u', cain, 1)
        if xo == 232: return self.arith(d, '(u32)~%s' % R(ra), '0xFFFFFFFFu', cain, 1)
        if xo == 104: return self.arith(d, '(u32)~%s' % R(ra), '0u', '1u', 0)
        if xo == 235:
            out = ['%s = %s * %s;' % (R(rd, 1), R(ra), R(rb))]
            if d['rcb']: out.append(self.cr0(R(rd)))
            return out
        if xo == 75:
            out = ['%s = (u32)((u64)((s64)(s32)%s * (s64)(s32)%s) >> 32);' % (R(rd, 1), R(ra), R(rb))]
            if d['rcb']: out.append(self.cr0(R(rd)))
            return out
        if xo == 11:
            out = ['%s = (u32)(((u64)%s * (u64)%s) >> 32);' % (R(rd, 1), R(ra), R(rb))]
            if d['rcb']: out.append(self.cr0(R(rd)))
            return out
        if xo == 491:
            out = ['%s = DIVW(%s, %s);' % (R(rd, 1), R(ra), R(rb))]
            if d['rcb']: out.append(self.cr0(R(rd)))
            return out
        if xo == 459:
            out = ['%s = %s ? %s / %s : 0u;' % (R(rd, 1), R(rb), R(ra), R(rb))]
            if d['rcb']: out.append(self.cr0(R(rd)))
            return out
        if xo == 0: return ['%s = CMPS(%s, (s32)%s, %s);' % (self.CR(rd >> 2, 1), R(ra), R(rb), self.XER())]
        if xo == 32: return ['%s = CMPU(%s, %s, %s);' % (self.CR(rd >> 2, 1), R(ra), R(rb), self.XER())]
        logic = {28: '%s & %s', 60: '%s & ~%s', 444: '%s | %s', 412: '%s | ~%s', 316: '%s ^ %s', 476: '~(%s & %s)',
                 124: '~(%s | %s)', 284: '~(%s ^ %s)'}
        if xo in logic:
            out = ['%s = %s;' % (R(ra, 1), logic[xo] % (R(rd), R(rb)))]
            if d['rcb']: out.append(self.cr0(R(ra)))
            return out
        un = {954: '(u32)(s32)(s8)%s', 922: '(u32)(s32)(s16)%s', 26: 'CLZ(%s)'}
        if xo in un:
            out = ['%s = %s;' % (R(ra, 1), un[xo] % R(rd))]
            if d['rcb']: out.append(self.cr0(R(ra)))
            return out
        if xo == 24:
            out = ['%s = (%s & 0x20u) ? 0u : %s << (%s & 31u);' % (R(ra, 1), R(rb), R(rd), R(rb))]
            if d['rcb']: out.append(self.cr0(R(ra)))
            return out
        if xo == 536:
            out = ['%s = (%s & 0x20u) ? 0u : %s >> (%s & 31u);' % (R(ra, 1), R(rb), R(rd), R(rb))]
            if d['rcb']: out.append(self.cr0(R(ra)))
            return out
        if xo == 824:
            sh = rb
            if sh == 0:
                out = ['%s = %s; %s &= ~XER_CA;' % (R(ra, 1), R(rd), self.XER(1))]
            else:
                out = ['{ s32 s_ = (s32)%s; %s = (%s & ~XER_CA) | ((s_ < 0 && ((u32)s_ << %d)) ? XER_CA : 0u); %s = (u32)(s_ >> %d); }'
                       % (R(rd), self.XER(1), self.XER(), 32 - sh, R(ra, 1), sh)]
            if d['rcb']: out.append(self.cr0(R(ra)))
            return out
        if xo == 792:
            out = ['{ s32 s_ = (s32)%s; u32 n_ = %s & 0x3Fu, d_, ca_; if (n_ & 0x20u) { d_ = s_ < 0 ? 0xFFFFFFFFu : 0u; ca_ = s_ < 0; } '
                   'else { d_ = (u32)(s_ >> n_); ca_ = s_ < 0 && n_ && ((u32)s_ << (32u - n_)) != 0u; } '
                   '%s = (%s & ~XER_CA) | (ca_ ? XER_CA : 0u); %s = d_; }' % (R(rd), R(rb), self.XER(1), self.XER(), R(ra, 1))]
            if d['rcb']: out.append(self.cr0(R(ra)))
            return out
        if xo == 19: return ['%s = %s;' % (R(rd, 1), self.cr_word())]
        if xo == 144:
            fxm = (i >> 12) & 0xFF
            out = []
            for f in range(8):
                if fxm & (0x80 >> f):
                    out.append('%s = (%s >> %d) & 15u;' % (self.CR(f, 1), R(rd), 28 - 4 * f))
            return out
        if xo == 512:
            return ['%s = %s >> 28; %s &= 0x0FFFFFFFu;' % (self.CR(rd >> 2, 1), self.XER(), self.XER(1))]
        if xo == 339:
            spr = ((i >> 16) & 31) | ((i >> 6) & 0x3E0)
            if spr == 8: return ['%s = %s;' % (R(rd, 1), self.LR())]
            if spr == 9: return ['%s = %s;' % (R(rd, 1), self.CTR())]
            if spr == 1: return ['%s = %s;' % (R(rd, 1), self.XER())]
            if 912 <= spr <= 919: return ['%s = %s;' % (R(rd, 1), self.Q(spr - 912))]
            return None
        if xo == 467:
            spr = ((i >> 16) & 31) | ((i >> 6) & 0x3E0)
            if spr == 8: return ['%s = %s;' % (self.LR(1), R(rd))]
            if spr == 9: return ['%s = %s;' % (self.CTR(1), R(rd))]
            if spr == 1: return ['%s = %s & XER_MASK;' % (self.XER(1), R(rd))]
            return None
        if xo == 83: return ['%s = g->msr;' % R(rd, 1)]
        if xo == 146:
            return ['if (aot_mtmsr(g, %s)) { %s }' % (R(rd), self.exit_after(hx(pc + 4)))]
        if xo in (278, 246, 854, 598): return []  # dcbt, dcbtst, eieio, sync: no-ops in the interpreter
        loads = {23: (4, 0, 0), 55: (4, 1, 0), 87: (1, 0, 0), 119: (1, 1, 0), 279: (2, 0, 0), 311: (2, 1, 0),
                 343: (2, 0, 1), 375: (2, 1, 1)}
        if xo in loads:
            size, upd, sign = loads[xo]
            if upd:
                if ra == 0 or ra == rd: return None
                return ['{ u32 ua_ = %s;' % self.ea_xu(d)] + self.load(size, R(rd, 1), 'ua_', pc, i, sign=sign) + ['%s = ua_; }' % R(ra, 1)]
            return self.load(size, R(rd, 1), self.ea_x(d), pc, i, sign=sign)
        if xo in (534, 790):
            return self.load(4 if xo == 534 else 2, R(rd, 1), self.ea_x(d), pc, i, rev=True)
        stores = {151: (4, 0), 183: (4, 1), 215: (1, 0), 247: (1, 1), 407: (2, 0), 439: (2, 1)}
        if xo in stores:
            size, upd = stores[xo]
            if upd:
                if ra == 0: return None
                return ['{ u32 ua_ = %s;' % self.ea_xu(d)] + self.store(size, R(rd), 'ua_', pc) + ['%s = ua_; }' % R(ra, 1)]
            return self.store(size, R(rd), self.ea_x(d), pc)
        if xo in (662, 918):
            return self.store(4 if xo == 662 else 2, R(rd), self.ea_x(d), pc, rev=True)
        if xo in (535, 567, 599, 631, 663, 695, 727, 759, 983):
            return self.fmem(pc, i, d, xo)
        return None

    def cr_word(self):
        return '(' + ' | '.join('(%s << %d)' % (self.CR(f), 28 - 4 * f) for f in range(8)) + ')'

    # branches --------------------------------------------------------------------------------------------------
    def cond(self, bo, bi):
        parts = []
        pre = []
        if not (bo & 4):
            self.CTR()
            pre.append('%s--;' % self.CTR(1))
            parts.append('(%s %s 0u)' % (self.CTR(), '==' if bo & 2 else '!='))
        if not (bo & 0x10):
            parts.append('(%s == %du)' % (self.crbit(bi), (bo >> 3) & 1))
        return pre, (' && '.join(parts) if parts else '1')

    def goto(self, pc, t):
        """Branch to guest address t: inside the function a goto (back edges check the budget), else leave."""
        f = self.f
        if f.start <= t < f.end:
            tk = (t - f.start) // 4
            if tk in self.poll_heads and tk <= self.k:
                return self.exit_after(hx(t))
            if f.words[tk] == 0x48000000 and tk == self.k:
                return self.exit_after(hx(t))
            if tk <= self.k:
                return 'if ((s32)n >= budget || aot_stop) { %s } goto %sL%d;' % (self.exit_after(hx(t)), self.lp, tk)
            return 'goto %sL%d;' % (self.lp, tk)
        return self.exit_after(hx(t))

    def inline_leaf(self, fn):
        """The body of a small leaf function at its call site, sharing this function's locals: its blr continues after
        the call (LR holds the return address, which a leaf never changes). Exits inside it leave with its own PCs."""
        self.ninl += 1
        sub = Translator(fn, self.st, None)
        sub.gpr, sub.fpr, sub.crf = self.gpr, self.fpr, self.crf
        sub.wgpr, sub.wfpr, sub.wcrf = self.wgpr, self.wfpr, self.wcrf
        sub.fallbacks = self.fallbacks
        sub.rec_root = self.rec_root
        sub.wids, sub.ubit, sub.E = self.wids, self.ubit, self.E
        sub.lp = 'I%d_' % self.ninl
        sub.ret_label = 'I%d_ret' % self.ninl
        sub.use_lr = True
        body = sub.emit()
        for a in ('use_lr', 'use_ctr', 'use_xer', 'use_fpscr', 'w_lr', 'w_ctr', 'w_xer', 'w_fpscr'):
            if getattr(sub, a): setattr(self, a, True)
        if (fn.start, fn.size) not in self.deps: self.deps.append((fn.start, fn.size))
        self.st['inlined'] = self.st.get('inlined', 0) + 1
        return ['{'] + body + ['%s:;' % sub.ret_label, '}']

    def dcall(self, target, pc, fn, gidx):
        """A call that stays in C: the shared dcall block stores the registers, calls the callee (fn, or the one
        looked up at `target`), and comes back here when it returned to pc + 4."""
        k = len(self.dcalls)
        self.dcalls.append(k)
        self.st['dcalls'] = self.st.get('dcalls', 0) + 1
        self.cur_kind = 'dcall'
        if self.E is None: return ['/* call */']  # pass 1: only the bookkeeping
        # The flush stores what was written since the last sync (dirty bits, constant along straight code, so most of
        # these tests fold away); after the call every local is clean again: the reloaded ones equal g, and the ones not
        # reloaded are not read again before they are written (liveness), so they must not be stored either.
        site = self.k
        live = self.live_after.get(site, set())
        o = ['{ u32 ei_ = 0; AotFn fn_ = %s;' % fn]
        o += self.dm_or(self.cur_w)  # LR, written just before
        o += self.flush_lines()
        o.append('%s' % ('dm0 = 0;' if self.ubit else ''))
        o += ['dm%d = 0;' % w for w in range(1, (len(self.ubit) + 31) // 32)]
        if fn == '0': o.append('fn_ = aot_lookup(%s, &ei_);' % target)
        else: o.append('if (aot_valid[%d] != 1 && !aot_check_idx(%d)) fn_ = 0;' % (gidx, gidx))
        o.append('if (!fn_ || aot_depth >= aot_depth_max || aot_stop) { g->pc = %s; return n; }' % target)
        o.append('g->cycles = t0 + n; aot_depth++;')
        o.append('u32 m_ = fn_(g, ei_, budget - (s32)n);')
        o.append('aot_depth--; g->cycles = t0;')
        o.append('if (!m_) { g->pc = %s; return n; }' % target)
        o.append('n += m_;')
        o.append('if (g->pc != %s || aot_stop) return n;' % hx(pc + 4))
        # every id of the function (pass 1's set: self.ids still lacks those first used after this site)
        for i in sorted(self.all_ids, key=self.idkey):
            if self.volatile(i) and i in live: o.append('%s = %s;' % (self.cname(i), self.gload(i)))
        o.append('}')
        self.cur_w = set()  # the dirty bits were set above (and cleared by the flush)
        return o

    def branch(self, pc, i, d, op):
        if op == 19:
            xo = d['xo']
            if xo in (16, 528):
                bo, bi = d['rd'], d['ra']
                lk = i & 1
                tgt = '(%s & ~3u)' % (self.LR() if xo == 16 else self.CTR())
                out = ['{ u32 t_ = %s;' % tgt]
                pre, c = self.cond(bo, bi) if xo == 16 else ([], self.cond(bo | 4, bi)[1])
                if xo == 528 and not (bo & 4): return None
                out += pre + self.dm_or(self.cur_w)  # CTR of a bdnz is dirty before the branch
                if lk: out.append('%s = %s;' % (self.LR(1), hx(pc + 4)))
                if xo == 528 and lk and c == '1' and not self.ret_label and not pre and self.fidx:
                    out.append('%s = %s;' % (self.LR(1), hx(pc + 4)))
                    out += self.dcall('t_', pc, '0', 0)
                    out.append('}')
                    return out
                if xo == 16 and self.ret_label and not lk:
                    if c == '1': out.append('goto %s;' % self.ret_label)
                    else: out.append('if (%s) goto %s;' % (c, self.ret_label))
                elif xo == 528 and not lk and c == '1':
                    # bctr: a switch statement's jump table lands on a label of this function, else leave
                    f = self.f
                    out.append('if ((s32)n >= budget || aot_stop) { %s }' % self.exit_after('t_'))
                    out.append('switch (t_) {')
                    for k in self.leaders:
                        out.append('case %s: goto %sL%d;' % (hx(f.start + 4 * k), self.lp, k))
                    out.append('default: %s' % self.exit_after('t_'))
                    out.append('}')
                elif c == '1': out.append(self.exit_after('t_'))
                else: out.append('if (%s) { %s }' % (c, self.exit_after('t_')))
                out.append('}')
                return out
            if xo in (257, 129, 289, 225, 33, 449, 417, 193):
                ops = {257: 'a_ & b_', 129: 'a_ & ~b_', 289: '~(a_ ^ b_)', 225: '~(a_ & b_)', 33: '~(a_ | b_)',
                       449: 'a_ | b_', 417: 'a_ | ~b_', 193: 'a_ ^ b_'}
                return ['{ u32 a_ = %s, b_ = %s; %s }' % (self.crbit(d['ra']), self.crbit(d['rb']),
                                                         self.set_crbit(d['rd'], '(%s) & 1u' % ops[xo]))]
            if xo == 0:
                return ['%s = %s;' % (self.CR(d['rd'] >> 2, 1), self.CR(d['ra'] >> 2))]
            if xo == 150: return []
            return None
        bk = branch_info(pc, i)
        kind, t, lk, bo, bi = bk
        out = []
        if lk:
            out.append('%s = %s;' % (self.LR(1), hx(pc + 4)))
            if kind == 'b' and t in self.inl and not self.ret_label and self.ninl < 64:
                return out + self.dm_or(self.cur_w) + self.inline_leaf(self.inl[t])
            if kind == 'b' and t in self.fidx and not self.ret_label:
                return out + self.dcall(hx(t), pc, 'F_%08X' % t, self.fidx[t])
            if kind == 'b':
                out.append(self.exit_after(hx(t)))
                return out
            pre, c = self.cond(bo, bi)
            out += pre + self.dm_or(self.cur_w)  # CTR of a bdnz is dirty before the branch
            out.append('if (%s) { %s }' % (c, self.exit_after(hx(t))))
            return out
        if kind == 'b':
            out.append(self.goto(pc, t))
            return out
        pre, c = self.cond(bo, bi)
        out += pre + self.dm_or(self.cur_w)  # CTR of a bdnz is dirty before the branch
        if c == '1': out.append(self.goto(pc, t))
        else: out.append('if (%s) { %s }' % (c, self.goto(pc, t)))
        return out

    # floating point --------------------------------------------------------------------------------------------
    def fp_ok(self):
        return True

    def fmem(self, pc, i, d, op):
        R, F = self.R, self.F
        rd, ra = d['rd'], d['ra']
        adj, bef = self.adj_after(), self.be - self.k
        if op in (48, 49, 535, 567):  # lfs
            upd = op in (49, 567)
            if upd and ra == 0: return None
            ea = {48: self.ea_d, 49: self.ea_du, 535: self.ea_x, 567: self.ea_xu}[op](d)
            return ['{ u32 ea_ = %s, w_; if (__builtin_expect(!LD32(ea_, &w_), 0)) { SLOW_LD(4, ea_, &w_, %s, %d, %d); } '
                    '%s = %s = S2D(w_);%s }' % (ea, hx(pc), adj, bef, F(rd, 0, 1), F(rd, 1, 1), (' %s = ea_;' % R(ra, 1)) if upd else '')]
        if op in (50, 51, 599, 631):  # lfd
            upd = op in (51, 631)
            if upd and ra == 0: return None
            ea = {50: self.ea_d, 51: self.ea_du, 599: self.ea_x, 631: self.ea_xu}[op](d)
            return ['{ u32 ea_ = %s; u64 w_; if (__builtin_expect(!LD64(ea_, &w_), 0)) { SLOW_LD64(ea_, &w_, %s, %d, %d); } '
                    '%s = U2D(w_);%s }' % (ea, hx(pc), adj, bef, F(rd, 0, 1), (' %s = ea_;' % R(ra, 1)) if upd else '')]
        if op in (52, 53, 663, 695):  # stfs
            upd = op in (53, 695)
            if upd and ra == 0: return None
            ea = {52: self.ea_d, 53: self.ea_du, 663: self.ea_x, 695: self.ea_xu}[op](d)
            conv = 'K2S(%s)' if self.single(rd, 0) else 'D2S(%s)'
            return ['{ u32 ea_ = %s, v_ = %s; if (__builtin_expect(!ST32(ea_, v_), 0)) { SLOW_ST(4, ea_, v_, %s, %d, %d); }%s }'
                    % (ea, conv % F(rd), hx(pc), adj, bef, (' %s = ea_;' % R(ra, 1)) if upd else '')]
        if op in (54, 55, 727, 759):  # stfd
            upd = op in (55, 759)
            if upd and ra == 0: return None
            ea = {54: self.ea_d, 55: self.ea_du, 727: self.ea_x, 759: self.ea_xu}[op](d)
            return ['{ u32 ea_ = %s; u64 v_ = D2U(%s); if (__builtin_expect(!ST64(ea_, v_), 0)) { SLOW_ST64(ea_, v_, %s, %d, %d); }%s }'
                    % (ea, F(rd), hx(pc), adj, bef, (' %s = ea_;' % R(ra, 1)) if upd else '')]
        if op == 983:  # stfiwx
            return ['{ u32 ea_ = %s, v_ = (u32)D2U(%s); if (__builtin_expect(!ST32(ea_, v_), 0)) { SLOW_ST(4, ea_, v_, %s, %d, %d); } }'
                    % (self.ea_x(d), F(rd), hx(pc), adj, bef)]
        if op in (56, 57, 60, 61):  # psq_l / psq_lu / psq_st / psq_stu
            upd = op in (57, 61)
            if upd and ra == 0: return None
            disp = ((i & 0xFFF) ^ 0x800) - 0x800
            base = self.R(ra) if upd else self.RA0(ra)
            ea = '%s + %du' % (base, disp & MASK) if disp else base
            w, q = (i >> 15) & 1, (i >> 12) & 7
            return self.psq(pc, op in (56, 57), rd, ra, ea, w, q, upd)
        return None

    def psq(self, pc, load, rd, ra, ea, w, q, upd):
        F, R = self.F, self.R
        adj, bef = self.adj_after(), self.be - self.k
        out = ['{ u32 ea_ = %s, q_ = %s;' % (ea, self.Q(q))]
        size = 4 if w else 8
        if load:
            fast = ('%s = S2D(LDU32(ea_)); %s = 1.0;' % (F(rd, 0, 1), F(rd, 1, 1)) if w else
                    '%s = S2D(LDU32(ea_)); %s = S2D(LDU32(ea_ + 4u));' % (F(rd, 0, 1), F(rd, 1, 1)))
            out.append('if (__builtin_expect(!(q_ & 0x00070000u) && OKN(ea_, %d), 1)) { %s } '
                       'else { double a_, b_; PSQ_SLOW_L(ea_, %d, %d, &a_, &b_, %s, %d, %d); %s = a_; %s = b_; }'
                       % (size, fast, w, q, hx(pc), adj, bef, F(rd, 0, 1), F(rd, 1, 1)))
        else:
            c0 = ('K2S(%s)' if self.single(rd, 0) else 'D2SF(%s)') % F(rd, 0)
            c1 = ('K2S(%s)' if self.single(rd, 1) else 'D2SF(%s)') % F(rd, 1)
            fast = 'STU32(ea_, %s);' % c0 if w else 'STU32(ea_, %s); STU32(ea_ + 4u, %s);' % (c0, c1)
            out.append('if (__builtin_expect(!(q_ & 7u) && OKN(ea_, %d), 1)) { %s } '
                       'else { PSQ_SLOW_S(ea_, %d, %d, %s, %s, %s, %d, %d); }'
                       % (size, fast, w, q, F(rd, 0), F(rd, 1), hx(pc), adj, bef))
        if upd: out.append('%s = ea_;' % R(ra, 1))
        out.append('}')
        return out

    def c25(self, r, lane):
        return self.F(r, lane) if self.single(r, lane) else 'C25(%s)' % self.F(r, lane)

    def op59(self, pc, i, d):
        if d['rcb']: return None
        F = self.F
        rd, ra, rb, rc, x = d['rd'], d['ra'], d['rb'], d['rc'], d['xo5']
        a, b = F(ra), F(rb)
        if x == 21: e = '%s + %s' % (a, b)
        elif x == 20: e = '%s - %s' % (a, b)
        elif x == 25: e = '%s * %s' % (a, self.c25(rc, 0))
        elif x == 18: e = '%s / %s' % (a, b)
        elif x in (28, 29, 30, 31):
            e = self.madd(x, a, self.c25(rc, 0), b)
        elif x == 24:
            return ['%s = %s = aot_fres(%s);' % (F(rd, 0, 1), F(rd, 1, 1), b)]
        else:
            return None
        return ['{ double r_ = RS(%s); %s = r_; %s = r_; }' % (e, F(rd, 0, 1), F(rd, 1, 1))]

    @staticmethod
    def madd(x, a, c, b):
        # 29 fmadd: a*c + b; 28 fmsub: a*c - b; 31 fnmadd: -(a*c + b); 30 fnmsub: -(a*c - b). A NaN result keeps its sign.
        core = '(%s * %s) %s %s' % (a, c, '+' if x in (29, 31) else '-', b)
        if x in (30, 31): return 'NEGNN(%s)' % core
        return core

    def op63(self, pc, i, d):
        if d['rcb']: return None
        F = self.F
        rd, ra, rb, rc, x, xo = d['rd'], d['ra'], d['rb'], d['rc'], d['xo5'], d['xo']
        if xo in (0, 32):
            return self.fcmp(rd >> 2, F(ra), F(rb), xo == 32, pc)
        if xo == 72: return ['%s = %s;' % (F(rd, 0, 1), F(rb))]
        if xo == 40: return ['%s = -%s;' % (F(rd, 0, 1), F(rb))]
        if xo == 264: return ['%s = __builtin_fabs(%s);' % (F(rd, 0, 1), F(rb))]
        if xo == 136: return ['%s = -__builtin_fabs(%s);' % (F(rd, 0, 1), F(rb))]
        if xo == 12: return ['{ double r_ = RS(%s); %s = r_; %s = r_; }' % (F(rb), F(rd, 0, 1), F(rd, 1, 1))]
        if xo in (14, 15): return ['%s = aot_fctiw(g, %s, %d);' % (F(rd, 0, 1), F(rb), xo)]
        if x == 23: return ['%s = %s >= 0.0 ? %s : %s;' % (F(rd, 0, 1), F(ra), F(rc), F(rb))]
        if x == 21: return ['%s = %s + %s;' % (F(rd, 0, 1), F(ra), F(rb))]
        if x == 20: return ['%s = %s - %s;' % (F(rd, 0, 1), F(ra), F(rb))]
        if x == 25: return ['%s = %s * %s;' % (F(rd, 0, 1), F(ra), F(rc))]
        if x == 18: return ['%s = %s / %s;' % (F(rd, 0, 1), F(ra), F(rb))]
        if x in (28, 29, 30, 31): return ['%s = %s;' % (F(rd, 0, 1), self.madd(x, F(ra), F(rc), F(rb)))]
        if x == 26: return ['%s = aot_frsqrte(%s);' % (F(rd, 0, 1), F(rb))]
        return None

    def fcmp(self, crf, a, b, ordered, pc):
        self.use_fpscr = self.w_fpscr = True
        self._rec(('fpscr',), False)
        self._rec(('fpscr',), True)
        out = ['{ u32 c_ = FCMP(%s, %s);' % (a, b)]
        if ordered:
            # fcmpo with a NaN operand sets VXVC (and maybe more): the interpreter does it exactly
            pass
        out.append('fpscr = (fpscr & ~0xF000u) | (c_ << 12); %s = c_; }' % self.CR(crf, 1))
        return out

    def op4(self, pc, i, d):
        F = self.F
        rd, ra, rb, rc, xo, x = d['rd'], d['ra'], d['rb'], d['rc'], d['xo'], d['xo5']
        x6 = (i >> 1) & 63
        if x6 in (6, 7, 38, 39):  # psq_lx / psq_stx / psq_lux / psq_stux
            upd = x6 >= 38
            if upd and ra == 0: return None
            w, q = (i >> 10) & 1, (i >> 7) & 7
            ea = self.ea_xu(d) if upd else self.ea_x(d)
            return self.psq(pc, x6 in (6, 38), rd, ra, ea, w, q, upd)
        if d['rcb']: return None
        if xo in (0, 32, 64, 96):
            l = 1 if xo >= 64 else 0
            return self.fcmp(rd >> 2, F(ra, l), F(rb, l), xo in (32, 96), pc)
        moves = {72: ('%s', '%s'), 40: ('-%s', '-%s'), 264: ('__builtin_fabs(%s)', '__builtin_fabs(%s)'),
                 136: ('-__builtin_fabs(%s)', '-__builtin_fabs(%s)')}
        if xo in moves:
            m0, m1 = moves[xo]
            return ['{ double a_ = %s, b_ = %s; %s = a_; %s = b_; }' % (m0 % F(rb, 0), m1 % F(rb, 1), F(rd, 0, 1), F(rd, 1, 1))]
        merges = {528: (0, 0), 560: (0, 1), 592: (1, 0), 624: (1, 1)}
        if xo in merges:
            la, lb = merges[xo]
            return ['{ double a_ = %s, b_ = %s; %s = a_; %s = b_; }' % (F(ra, la), F(rb, lb), F(rd, 0, 1), F(rd, 1, 1))]
        if xo == 1014: return None  # dcbz_l
        def lanes(e0, e1):
            return ['{ double a_ = RS(%s), b_ = RS(%s); %s = a_; %s = b_; }' % (e0, e1, F(rd, 0, 1), F(rd, 1, 1))]
        if x == 21: return lanes('%s + %s' % (F(ra, 0), F(rb, 0)), '%s + %s' % (F(ra, 1), F(rb, 1)))
        if x == 20: return lanes('%s - %s' % (F(ra, 0), F(rb, 0)), '%s - %s' % (F(ra, 1), F(rb, 1)))
        if x == 18: return lanes('%s / %s' % (F(ra, 0), F(rb, 0)), '%s / %s' % (F(ra, 1), F(rb, 1)))
        if x == 25: return lanes('%s * %s' % (F(ra, 0), self.c25(rc, 0)), '%s * %s' % (F(ra, 1), self.c25(rc, 1)))
        if x == 12: return lanes('%s * %s' % (F(ra, 0), self.c25(rc, 0)), '%s * %s' % (F(ra, 1), self.c25(rc, 0)))
        if x == 13: return lanes('%s * %s' % (F(ra, 0), self.c25(rc, 1)), '%s * %s' % (F(ra, 1), self.c25(rc, 1)))
        if x in (28, 29, 30, 31):
            return lanes(self.madd(x, F(ra, 0), self.c25(rc, 0), F(rb, 0)), self.madd(x, F(ra, 1), self.c25(rc, 1), F(rb, 1)))
        if x == 14: return lanes(self.madd(29, F(ra, 0), self.c25(rc, 0), F(rb, 0)), self.madd(29, F(ra, 1), self.c25(rc, 0), F(rb, 1)))
        if x == 15: return lanes(self.madd(29, F(ra, 0), self.c25(rc, 1), F(rb, 0)), self.madd(29, F(ra, 1), self.c25(rc, 1), F(rb, 1)))
        if x == 10: return lanes('%s + %s' % (F(ra, 0), F(rb, 1)), F(rc, 1))
        if x == 11: return lanes(F(rc, 0), '%s + %s' % (F(ra, 0), F(rb, 1)))
        if x == 23:
            return ['{ double a_ = %s >= 0.0 ? %s : %s, b_ = %s >= 0.0 ? %s : %s; %s = a_; %s = b_; }'
                    % (F(ra, 0), F(rc, 0), F(rb, 0), F(ra, 1), F(rc, 1), F(rb, 1), F(rd, 0, 1), F(rd, 1, 1))]
        if x == 24:
            return ['{ double a_ = aot_fres(%s), b_ = aot_fres(%s); %s = a_; %s = b_; }' % (F(rb, 0), F(rb, 1), F(rd, 0, 1), F(rd, 1, 1))]
        if x == 26:
            return ['{ double a_ = RS(aot_frsqrte(%s)), b_ = RS(aot_frsqrte(%s)); %s = a_; %s = b_; }'
                    % (F(rb, 0), F(rb, 1), F(rd, 0, 1), F(rd, 1, 1))]
        return None

    # -- liveness --------------------------------------------------------------------------------------------------
    @staticmethod
    def volatile(ident):
        """What the EABI lets a callee change (what a direct call reloads with aot_abi)."""
        k = ident[0]
        if k == 'r': return ident[1] == 0 or 3 <= ident[1] <= 12
        if k == 'f': return ident[1] <= 13
        if k == 'p': return True
        if k == 'c': return ident[1] not in (2, 3, 4)
        return True

    def liveness(self):
        """Pass 1's result: the ids read before they are written from some entry (loaded at entry)."""
        allids = set(self.ids)
        def defs_uses(k):
            r, w, kind = self.rw.get(k, (frozenset(), frozenset(), None))
            if kind == 'sync': return allids, r
            if kind == 'dcall': return set(w) | {x for x in allids if self.volatile(x)}, r
            return w, r
        live_in = {s: set() for s, e in self.blocks}
        changed = True
        while changed:
            changed = False
            for s, e in reversed(self.blocks):
                live = set()
                for t in self.succ.get(s, []):
                    live |= live_in.get(t, set())
                for k in range(e - 1, s - 1, -1):
                    d, u = defs_uses(k)
                    live = (live - set(d)) | set(u)
                if live != live_in[s]:
                    live_in[s] = live
                    changed = True
        self.live_after = {}
        for k, (r, w, kind) in self.rw.items():
            if kind == 'dcall': self.live_after[k] = set(live_in.get(k + 1, set()))
        E = set()
        for k in self.entries:
            E |= live_in.get(k, set())
        E |= {x for x in allids if x[0] in ('lr', 'ctr', 'fpscr')}  # written right before exits, or read-modify-written
        return E

    def dm_or(self, ws):
        if self.E is None: return []
        words = {}
        for i in ws:
            b = self.ubit.get(i)
            if b is not None: words[b // 32] = words.get(b // 32, 0) | (1 << (b % 32))
        return ['dm%d |= %s;' % (w, hx(m)) for w, m in sorted(words.items())]

    @staticmethod
    def cname(ident):
        k = ident[0]
        if k in ('r', 'f', 'p', 'c', 'q'): return '%s%d' % (k, ident[1])
        return k

    @staticmethod
    def gload(ident):
        k = ident[0]
        if k == 'r': return 'g->gpr[%d]' % ident[1]
        if k == 'f': return 'g->fpr[%d].ps0' % ident[1]
        if k == 'p': return 'g->fpr[%d].ps1' % ident[1]
        if k == 'c': return '((g->cr >> %d) & 15u)' % (28 - 4 * ident[1])
        if k == 'q': return 'g->spr[%d]' % (912 + ident[1])
        return 'g->%s' % k

    def gstore(self, ident):
        k = ident[0]
        v = self.cname(ident)
        if k == 'r': return 'g->gpr[%d] = %s;' % (ident[1], v)
        if k == 'f': return 'g->fpr[%d].ps0 = %s;' % (ident[1], v)
        if k == 'p': return 'g->fpr[%d].ps1 = %s;' % (ident[1], v)
        if k == 'c':
            sh = 28 - 4 * ident[1]
            return 'g->cr = (g->cr & %s) | (%s << %d);' % (hx(~(0xF << sh)), v, sh)
        return 'g->%s = %s;' % (k, v)

    @staticmethod
    def idkey(ident):
        order = {'r': 0, 'f': 1, 'p': 2, 'c': 3, 'lr': 4, 'ctr': 5, 'xer': 6, 'fpscr': 7, 'q': 8}
        return (order[ident[0]], ident[1] if len(ident) > 1 else 0)

    # -- the C function --------------------------------------------------------------------------------------------
    def c_function(self):
        f = self.f
        body = self.emit()
        name = 'F_%08X' % f.start
        o = ['// %s (%08x, %d instructions)' % (f.name[:100].replace('*/', ''), f.start, len(f.words)),
             '%su32 %s(Gekko* g, u32 entry, s32 budget) {' % (self.attr, name),
             '    u32 n = 0, npc = 0; u64 t0 = g->cycles; (void)t0; const uintptr_t mb_ = aot_base;',
             '    u32 fb_k = 0, fb_pc = 0, fb_inst = 0, fb_adj = 0; (void)fb_k; (void)fb_pc; (void)fb_inst; (void)fb_adj;',
             '    u32 dc_k = 0, dc_pc = 0, dc_rp = 0, dc_v = 0; AotFn dc_fn = 0; (void)dc_k; (void)dc_pc; (void)dc_rp; (void)dc_v; (void)dc_fn;',
             '    u32 lk_pc = 0, lk_adj = 0, lk_bef = 0; (void)lk_pc; (void)lk_adj; (void)lk_bef;']
        decl = []
        E = self.E if self.E is not None else set(self.ids)
        for i in sorted(self.ids, key=self.idkey):
            ty = 'double' if i[0] in ('f', 'p') else 'u32'
            if i in E: decl.append('%s %s = %s;' % (ty, self.cname(i), self.gload(i)))
            else: decl.append('%s %s = 0;  // written before it is read' % (ty, self.cname(i)))
        nw = (len(self.ubit) + 31) // 32
        if nw: decl.append('u32 ' + ', '.join('dm%d = 0' % w for w in range(nw)) + ';')
        o += ['    ' + x for x in decl]
        o.append('    switch (entry) {')
        for idx, k in enumerate(self.entries):
            o.append('    case %d: goto L%d;' % (idx, k))
        o.append('    default: return 0;')
        o.append('    }')
        o += body
        flush = self.flush_lines()
        reload_ = self.reload_lines()
        o.append('fallback:')
        if self.fallbacks:
            o += ['    ' + x for x in flush]
            o.append('    { u32 s_ = aot_interp(g, fb_pc, fb_inst, t0 + n - fb_adj - 1u); if (s_) { n -= fb_adj + (s_ == 3u); return n; } }')
            o += ['    ' + x for x in reload_]
            o.append('    switch (fb_k) {')
            for k, pc, i in self.fallbacks:
                o.append('    case %d: goto FB%d;' % (k, k))
            o.append('    }')
        o.append('    return n;')
        if False:
            o.append('dcall:')
            o += ['    ' + x for x in flush]
            o.append('    { u32 ei_ = 0; AotFn fn_ = dc_fn;')
            o.append('      if (!fn_) fn_ = aot_lookup(dc_pc, &ei_);')
            o.append('      else if (aot_valid[dc_v] != 1 && !aot_check_idx(dc_v)) fn_ = 0;')
            o.append('      if (!fn_ || aot_depth >= aot_depth_max || aot_stop) { g->pc = dc_pc; return n; }')
            o.append('      g->cycles = t0 + n; aot_depth++;')
            o.append('      u32 m_ = fn_(g, ei_, budget - (s32)n);')
            o.append('      aot_depth--; g->cycles = t0;')
            o.append('      if (!m_) { g->pc = dc_pc; return n; }')
            o.append('      n += m_;')
            o.append('      if (g->pc != dc_rp || aot_stop) return n; }')
            if self.abi:
                o.append('    if (aot_abi) {')
                o += ['        ' + x for x in self.reload_abi_lines()]
                o.append('    } else {')
                o += ['        ' + x for x in reload_]
                o.append('    }')
            else:
                o += ['    ' + x for x in reload_]
            o.append('    switch (dc_k) {')
            for k in self.dcalls:
                o.append('    case %d: goto DC%d;' % (k, k))
            o.append('    }')
            o.append('    return n;')
        o.append('leave:')
        o.append('    if (aot_ls == 3u) { npc = lk_pc; n -= lk_bef; } else { npc = g->pc; n -= lk_adj; }')
        o.append('out:')
        o += ['    ' + x for x in flush]
        o.append('    g->pc = npc;')
        o.append('    return n;')
        o.append('}')
        return name, o

    def flush_lines(self):
        """Every register written since the last sync (its dirty bit set)."""
        o = []
        for i in sorted(self.all_w or self.wids, key=self.idkey):
            b = self.ubit.get(i)
            if b is None: o.append(self.gstore(i))
            else: o.append('if (dm%d & %s) %s' % (b // 32, hx(1 << (b % 32)), self.gstore(i)))
        return o

    def dm_all(self):
        out = []
        n = len(self.ubit)
        for w in range((n + 31) // 32):
            bits = min(32, n - 32 * w)
            out.append('dm%d = %s;' % (w, hx((1 << bits) - 1)))
        return out

    def reload_abi_lines(self):
        o = [ '%s = %s;' % (self.cname(i), self.gload(i)) for i in sorted(self.ids, key=self.idkey) if self.volatile(i)]
        return o + self.dm_or([i for i in self.ids if self.volatile(i)])

    def reload_abi_lines_old(self):
        """After a call that returned normally: only what the PowerPC EABI lets a callee change (r0, r3-r12, f0-f13,
        cr0, cr1, cr5-cr7, LR, CTR, XER, FPSCR); r1, r2, r13, r14-r31, f14-f31 and cr2-cr4 come back as they were.
        AOT_CHECK compares every call chain with the interpreter, so code that breaks the convention shows up there."""
        o = []
        for r in sorted(self.gpr):
            if r == 0 or 3 <= r <= 12: o.append('r%d = g->gpr[%d];' % (r, r))
        for r in sorted(self.fpr):
            # f14-f31: the EABI saves and restores only ps0 (stfd / lfd), so a callee's lfs or paired-single code can
            # leave ps1 changed (TP: f31's ps1 after a call, found by AOT_CHECK on ARM)
            if r <= 13: o.append('f%d = g->fpr[%d].ps0; p%d = g->fpr[%d].ps1;' % (r, r, r, r))
            else: o.append('p%d = g->fpr[%d].ps1;' % (r, r))
        for c in sorted(self.crf):
            if c not in (2, 3, 4): o.append('c%d = (g->cr >> %d) & 15u;' % (c, 28 - 4 * c))
        if self.use_lr: o.append('lr = g->lr;')
        if self.use_ctr: o.append('ctr = g->ctr;')
        if self.use_xer: o.append('xer = g->xer;')
        if self.use_fpscr: o.append('fpscr = g->fpscr;')
        return o

    def reload_lines(self):
        return (['%s = %s;' % (self.cname(i), self.gload(i)) for i in sorted(self.ids, key=self.idkey)] +
                ['dm%d = 0;' % w for w in range((len(self.ubit) + 31) // 32)])

    def reload_lines_old(self):
        o = []
        for r in sorted(self.gpr): o.append('r%d = g->gpr[%d];' % (r, r))
        for r in sorted(self.fpr): o.append('f%d = g->fpr[%d].ps0; p%d = g->fpr[%d].ps1;' % (r, r, r, r))
        for c in sorted(self.crf): o.append('c%d = (g->cr >> %d) & 15u;' % (c, 28 - 4 * c))
        if self.use_lr: o.append('lr = g->lr;')
        if self.use_ctr: o.append('ctr = g->ctr;')
        if self.use_xer: o.append('xer = g->xer;')
        if self.use_fpscr: o.append('fpscr = g->fpscr;')
        return o


def is_leaf(fn):
    """A function that calls nothing, never leaves except by blr, and leaves LR alone: inlinable at a bl."""
    for k, i in enumerate(fn.words):
        pc = fn.start + 4 * k
        op = i >> 26
        if op == 17: return False
        if op == 19 and ((i >> 1) & 0x3FF) == 50: return False
        if op == 31 and ((i >> 1) & 0x3FF) == 467 and (((i >> 16) & 31) | ((i >> 6) & 0x3E0)) == 8: return False
        bi = branch_info(pc, i)
        if not bi: continue
        kind, t, lk, bo, b = bi
        if lk or kind == 'bcctr': return False
        if kind in ('b', 'bc') and not (fn.start <= t < fn.end): return False
    w = fn.words[-1]
    return w == 0x4E800020 or branch_info(fn.end - 4, w) is not None


def discover(secs, mem):
    """Functions without a symbol map: starts are the DOL's entry, every bl target, every word in a data section that
    points at code which begins like a function (vtables, callback tables), and code right after a blr or b that begins
    like one (a stack frame, mflr, or a leaf's first instruction after alignment padding). A function runs to the next
    start. Names are their addresses."""
    starts = set()
    text = [(b, z, d) for b, z, d, t in secs if t]
    def word(a):
        return mem.word(a)
    def looks_like_start(a):
        w = word(a)
        if w is None or w == 0: return False
        op = w >> 26
        if op == 37 and ((w >> 16) & 31) == 1 and ((w >> 21) & 31) == 1: return True  # stwu r1, -x(r1)
        if w == 0x7C0802A6: return True  # mflr r0
        return False
    for base, size, data in text:
        prev = None
        for o in range(0, size, 4):
            w = struct.unpack('>I', data[o:o + 4])[0]
            a = base + o
            if (w >> 26) == 18 and (w & 3) == 1:
                starts.add((a + (((w & 0x03FFFFFC) ^ 0x02000000) - 0x02000000)) & MASK)
            if prev is not None and (prev == 0x4E800020 or ((prev >> 26) == 18 and (prev & 3) == 0)):
                if looks_like_start(a): starts.add(a)
            prev = w
    for base, size, data, t in secs:
        if t: continue
        for o in range(0, size - 3, 4):
            w = struct.unpack('>I', data[o:o + 4])[0]
            if w & 3 or not mem.in_text(w): continue
            if looks_like_start(w): starts.add(w)
    starts = sorted(x for x in starts if mem.in_text(x))
    out = []
    ends = {}
    for base, size, data in text:
        ends[base] = base + size
    for k, a in enumerate(starts):
        sec_end = next(b + z for b, z, d in text if b <= a < b + z)
        e = starts[k + 1] if k + 1 < len(starts) else sec_end
        e = min(e, sec_end)
        out.append((a, e - a, 'fn_%08x' % a))
    print('discovered %d functions' % len(out))
    return out


def rlw_mask(mb, me):
    m = (MASK >> mb) ^ (0 if me >= 31 else (MASK >> (me + 1)))
    return (~m & MASK) if me < mb else m


# ---------------------------------------------------------------------------------------------------- driver
def fnv64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--dol', required=True)
    ap.add_argument('--symbols', help='a decomp symbol map; without one, functions are found in the code itself')
    ap.add_argument('--hist', help='executed-instruction histogram (pc_main PCHIST): only functions that ran')
    ap.add_argument('--top', type=int, default=0, help='only the N most executed functions')
    ap.add_argument('--cover', type=float, default=0, help='only the hottest functions covering this share of execution (e.g. 0.99)')
    ap.add_argument('--out', required=True)
    ap.add_argument('--files', type=int, default=16)
    ap.add_argument('--only', help='comma-separated function addresses (hex)')
    ap.add_argument('--inline', type=int, default=32, help='inline leaf functions up to this many instructions (0: none)')
    ap.add_argument('--dcalls', type=int, default=1, help='calls between translated functions stay in C (0: they leave)')
    ap.add_argument('--abi', type=int, default=1, help='after a direct call reload only what the EABI lets a callee change')
    ap.add_argument('--live', type=int, default=1, help='load at entry only registers read before written (0: all)')
    a = ap.parse_args()
    secs = load_dol(a.dol)
    mem = Mem(secs)
    syms = load_symbols(a.symbols) if a.symbols else discover(secs, mem)
    counts = None
    if a.hist:
        h = array.array('I'); h.frombytes(open(a.hist, 'rb').read())
        counts = h
    fns = []
    for start, size, name in syms:
        if size < 4 or not mem.in_text(start) or not mem.in_text(start + size - 4): continue
        c = 0
        if counts is not None:
            i0 = (start & 0x01FFFFFF) >> 2
            c = sum(counts[i0:i0 + size // 4])
            if not c: continue
        fns.append((c, start, size, name))
    fns.sort(key=lambda x: -x[0])
    if a.top: fns = fns[:a.top]
    if a.cover and counts is not None:  # only the hottest functions that together run this share of the instructions
        fns.sort(key=lambda x: -x[0])
        tot, acc, keep = sum(x[0] for x in fns) or 1, 0, []
        for x in fns:
            if acc >= a.cover * tot: break
            keep.append(x); acc += x[0]
        fns = keep
    if a.only:
        want = {int(x, 16) for x in a.only.split(',')}
        fns = [x for x in fns if x[1] in want]
    fns.sort(key=lambda x: (-x[0], x[1]))  # hottest first (counts from --hist; ties by address)
    tot_c = sum(x[0] for x in fns) or 1
    hot = set()
    acc = 0
    for c, st, sz, nm in fns:
        if acc < 0.97 * tot_c or not counts:
            hot.add(st)
        acc += c
    # Every bl target in the DOL's code: a callee entered part way in (__save_gpr / __restore_gpr) gets an entry there.
    calls = set()
    for base, size, data, text in secs:
        if not text: continue
        for o in range(0, size, 4):
            w = struct.unpack('>I', data[o:o + 4])[0]
            if (w >> 26) == 18 and (w & 3) == 1:
                calls.add((base + o + (((w & 0x03FFFFFC) ^ 0x02000000) - 0x02000000)) & MASK)
    stats = {'native': 0, 'fb': {}}
    leaves = {}
    if a.inline:
        for start, size, name in syms:
            if size < 4 or size // 4 > a.inline or not mem.in_text(start) or not mem.in_text(start + size - 4): continue
            fn = Fn(start, size, name, mem)
            if fn.ok and is_leaf(fn): leaves[start] = fn
    os.makedirs(a.out, exist_ok=True)
    for old in os.listdir(a.out):
        if old.startswith('aot_gen') and old.endswith('.c'): os.remove(os.path.join(a.out, old))
    nf = max(1, min(a.files, len(fns)))
    chunks = [[] for _ in range(nf)]
    table = []
    total_insns = 0
    fns = [x for x in fns if Fn(x[1], x[2], x[3], mem).ok]
    fidx = {x[1]: k for k, x in enumerate(fns)}
    with open(os.path.join(a.out, 'aot_gen.h'), 'w') as fo:
        fo.write('#pragma once\n#include "../cpu/aot_rt.h"\n')
        for x in fns:
            fo.write('u32 F_%08X(Gekko* g, u32 entry, s32 budget);\n' % x[1])
    for idx, (c, start, size, name) in enumerate(fns):
        fn = Fn(start, size, name, mem)
        if not fn.ok: continue
        def make(st):
            t = Translator(fn, st, None, [c for c in calls if start < c < start + size])
            t.gidx = fidx[start]
            t.inl = {k: v for k, v in leaves.items() if k != start}
            if a.dcalls: t.fidx = fidx
            t.abi = bool(a.abi)
            t.attr = '__attribute__((hot)) ' if start in hot else '__attribute__((cold)) '
            return t
        t1 = make({'native': 0, 'fb': {}})
        t1.emit()
        if os.environ.get("AOT_DEBUG_LIVE") and int(os.environ["AOT_DEBUG_LIVE"], 16) == start:
            E_ = t1.liveness(); print("E", sorted(E_, key=Translator.idkey)); [print("call", hex(start + 4 * k), sorted(v, key=Translator.idkey)) for k, v in sorted(t1.live_after.items())]
        E = t1.liveness() if a.live else set(t1.ids)

        t = make(stats)
        t.E = E
        t.all_w = set(t1.wids)
        t.all_ids = set(t1.ids)
        t.live_after = t1.live_after
        t.ubit = {i: b for b, i in enumerate(sorted(t1.wids, key=Translator.idkey))}
        cname, lines = t.c_function()
        data = b''.join(struct.pack('>I', w) for w in fn.words)
        ddata = b''.join(b''.join(struct.pack('>I', w) for w in leaves[ds].words) for ds, dz in t.deps)
        if t.deps:
            lines.append('static const u32 D_%08X[] = { %s, 0 };' % (start, ', '.join('%s, %du' % (hx(ds), dz) for ds, dz in t.deps)))
        part = min(nf - 1, idx * nf // max(1, len(fns)))
        table.append((start, size, fnv64(data), cname, [start + 4 * k for k in t.entries], part,
                      ('D_%08X' % start) if t.deps else 'NULL', fnv64(ddata) if t.deps else 0))
        chunks[part].append('\n'.join(lines))
        total_insns += len(fn.words)
    head = ('// Generated by core/tools/aot/ppc2c.py from the user\'s own disc: not to be committed or distributed.\n'
            '#include "aot_gen.h"\n'
            '#pragma GCC diagnostic ignored "-Wunused-label"\n'
            '#pragma GCC diagnostic ignored "-Wunused-variable"\n'
            '#pragma GCC diagnostic ignored "-Wunused-but-set-variable"\n'
            '#pragma GCC diagnostic ignored "-Wunused-parameter"\n'
            '#pragma GCC diagnostic ignored "-Wtype-limits"\n'
            '#pragma GCC diagnostic error "-Wshadow"\n'
            '#pragma GCC diagnostic error "-Winit-self"\n'
            '#pragma GCC diagnostic error "-Wuninitialized"\n')
    for k in range(nf):
        mine = [t for t in table if t[5] == k]
        with open(os.path.join(a.out, 'aot_gen%02d.c' % k), 'w') as fo:
            fo.write(head)
            fo.write('\n\n'.join(chunks[k]))
            fo.write('\n\nconst AotFunc aot_funcs_%02d[] = {\n' % k)
            for start, size, hsh, cname, ents, _, dname, dh in mine:
                fo.write('    { %s, %du, 0x%016XULL, %s, %du, %s, 0x%016XULL, %du },\n' % (hx(start), size, hsh, cname, len(ents), dname, dh, fidx[start]))
            fo.write('};\nconst u32 aot_nfuncs_%02d = %d;\n' % (k, len(mine)))
            fo.write('const u32 aot_entries_%02d[] = {\n' % k)
            for start, size, hsh, cname, ents, *_ in mine:
                fo.write('    ' + ', '.join(hx(e) for e in ents) + ',\n')
            fo.write('    0 };\n')
    with open(os.path.join(a.out, 'aot_gen_index.c'), 'w') as fo:
        fo.write(head)
        for k in range(nf):
            fo.write('extern const AotFunc aot_funcs_%02d[]; extern const u32 aot_nfuncs_%02d; extern const u32 aot_entries_%02d[];\n' % (k, k, k))
        fo.write('const AotPart aot_parts[] = {\n')
        for k in range(nf):
            fo.write('    { aot_funcs_%02d, &aot_nfuncs_%02d, aot_entries_%02d },\n' % (k, k, k))
        fo.write('};\nconst u32 aot_nparts = %d;\n' % nf)
        fo.write('s8 aot_valid[%d];\nconst u32 aot_nvalid = %d;\n' % (len(fns), len(fns)))
    fb = stats['fb']
    tot_fb = sum(fb.values())
    print('functions %d, guest instructions %d, translated %d, interpreter fallbacks %d, leaves %d, call sites inlined %d'
          % (len(table), total_insns, stats['native'], tot_fb, len(leaves), stats.get('inlined', 0)))
    print('direct call sites %d' % stats.get('dcalls', 0))
    for (op, xo), n in sorted(fb.items(), key=lambda x: -x[1])[:25]:
        print('  fallback op %d xo %d: %d' % (op, xo, n))


if __name__ == '__main__':
    main()
