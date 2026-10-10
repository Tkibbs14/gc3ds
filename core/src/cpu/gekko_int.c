// Integer, branch, condition-register, load/store, cache and system instructions.
// Semantics: PowerPC Programming Environments Manual (PEM) chapter 8 instruction descriptions, with the 750CL-specific
// choices noted inline.
#include "gekko_internal.h"

#define GPR(r) (g->gpr[(r)])
#define RA0(i) (I_RA(i) ? GPR(I_RA(i)) : 0u)
#define EA_D(i) (RA0(i) + I_SIMM(i))
#define EA_X(i) (RA0(i) + GPR(I_RB(i)))
#define EA_DU(i) (GPR(I_RA(i)) + I_SIMM(i))
#define EA_XU(i) (GPR(I_RA(i)) + GPR(I_RB(i)))

static inline bool supervisor_check(Gekko* g) {
    if (g->msr & MSR_PR) {
        gk_program_exception(g, SRR1_PRIV);
        return false;
    }
    return true;
}

static inline void set_crf(Gekko* g, u32 f, u32 v) {
    u32 sh = 28 - 4 * f;
    g->cr = (g->cr & ~(0xFu << sh)) | ((v & 0xF) << sh);
}

static inline void set_cr0(Gekko* g, u32 v) {
    u32 f = ((s32)v < 0) ? 8u : (v ? 4u : 2u);
    g->cr = (g->cr & 0x0FFFFFFFu) | ((f | (g->xer >> 31)) << 28);
}

static inline void set_ov(Gekko* g, u32 ov) {
    if (ov) g->xer |= XER_OV | XER_SO;
    else g->xer &= ~XER_OV;
}

static inline void set_ca(Gekko* g, u32 ca) {
    if (ca) g->xer |= XER_CA;
    else g->xer &= ~XER_CA;
}

static inline u32 rotl32(u32 v, u32 n) { n &= 31; return n ? (v << n) | (v >> (32 - n)) : v; }

static inline u32 rlw_mask(u32 mb, u32 me) {
    u32 m = (0xFFFFFFFFu >> mb) ^ (me >= 31 ? 0u : (0xFFFFFFFFu >> (me + 1)));
    return me < mb ? ~m : m;
}

// x + y + cin with carry out and signed overflow (used by every add/subtract form: subtract is ~a + b + 1).
static inline u32 add3(u32 x, u32 y, u32 cin, u32* ca, u32* ov) {
    u64 s = (u64)x + y + cin;
    u32 d = (u32)s;
    *ca = (u32)(s >> 32);
    *ov = ((x ^ d) & (y ^ d)) >> 31;
    return d;
}

// ---- Integer arithmetic ----
#define ARITH(name, X, Y, CIN, SETCA)                                           \
    static void name(Gekko* g, u32 i) {                                         \
        u32 a = GPR(I_RA(i)), b = GPR(I_RB(i));                                 \
        (void)a; (void)b;                                                       \
        u32 ca, ov, d = add3((X), (Y), (CIN), &ca, &ov);                        \
        if (SETCA) set_ca(g, ca);                                               \
        if (I_OE(i)) set_ov(g, ov);                                             \
        GPR(I_RD(i)) = d;                                                       \
        if (I_RCB(i)) set_cr0(g, d);                                            \
    }
#define CAIN ((g->xer >> 29) & 1)
ARITH(op_add, a, b, 0, 0)
ARITH(op_addc, a, b, 0, 1)
ARITH(op_adde, a, b, CAIN, 1)
ARITH(op_addze, a, 0, CAIN, 1)
ARITH(op_addme, a, 0xFFFFFFFFu, CAIN, 1)
ARITH(op_subf, ~a, b, 1, 0)
ARITH(op_subfc, ~a, b, 1, 1)
ARITH(op_subfe, ~a, b, CAIN, 1)
ARITH(op_subfze, ~a, 0, CAIN, 1)
ARITH(op_subfme, ~a, 0xFFFFFFFFu, CAIN, 1)
ARITH(op_neg, ~a, 0, 1, 0)

static void op_addi(Gekko* g, u32 i) { GPR(I_RD(i)) = RA0(i) + I_SIMM(i); }
static void op_addis(Gekko* g, u32 i) { GPR(I_RD(i)) = RA0(i) + (I_UIMM(i) << 16); }
static void op_addic(Gekko* g, u32 i) {
    u32 ca, ov, d = add3(GPR(I_RA(i)), I_SIMM(i), 0, &ca, &ov);
    set_ca(g, ca);
    GPR(I_RD(i)) = d;
}
static void op_addic_rc(Gekko* g, u32 i) {
    u32 ca, ov, d = add3(GPR(I_RA(i)), I_SIMM(i), 0, &ca, &ov);
    set_ca(g, ca);
    GPR(I_RD(i)) = d;
    set_cr0(g, d);
}
static void op_subfic(Gekko* g, u32 i) {
    u32 ca, ov, d = add3(~GPR(I_RA(i)), I_SIMM(i), 1, &ca, &ov);
    set_ca(g, ca);
    GPR(I_RD(i)) = d;
}
static void op_mulli(Gekko* g, u32 i) { GPR(I_RD(i)) = (u32)((s32)GPR(I_RA(i)) * (s64)(s32)I_SIMM(i)); }

static void op_mullw(Gekko* g, u32 i) {
    s64 p = (s64)(s32)GPR(I_RA(i)) * (s32)GPR(I_RB(i));
    u32 d = (u32)p;
    if (I_OE(i)) set_ov(g, p != (s64)(s32)d);
    GPR(I_RD(i)) = d;
    if (I_RCB(i)) set_cr0(g, d);
}
static void op_mulhw(Gekko* g, u32 i) {
    u32 d = (u32)((u64)((s64)(s32)GPR(I_RA(i)) * (s32)GPR(I_RB(i))) >> 32);
    GPR(I_RD(i)) = d;
    if (I_RCB(i)) set_cr0(g, d);
}
static void op_mulhwu(Gekko* g, u32 i) {
    u32 d = (u32)(((u64)GPR(I_RA(i)) * GPR(I_RB(i))) >> 32);
    GPR(I_RD(i)) = d;
    if (I_RCB(i)) set_cr0(g, d);
}
// Undefined quotients (PEM: "the contents of rD are undefined"): divide by zero and 0x80000000 / -1 give
// rD = (rA < 0) ? 0xFFFFFFFF : 0 for divw and 0 for divwu, the values hardware tests on Gekko report (as modelled by
// Dolphin).
static void op_divw(Gekko* g, u32 i) {
    s32 a = (s32)GPR(I_RA(i)), b = (s32)GPR(I_RB(i));
    u32 d, ov = 0;
    if (b == 0 || ((u32)a == 0x80000000u && b == -1)) {
        d = a < 0 ? 0xFFFFFFFFu : 0;
        if (GKP(g)->compat & GK_COMPAT_DIV_QEMU) d = (u32)a;
        ov = 1;
    } else {
        d = (u32)(a / b);
    }
    if (I_OE(i)) set_ov(g, ov);
    GPR(I_RD(i)) = d;
    if (I_RCB(i)) set_cr0(g, d);
}
static void op_divwu(Gekko* g, u32 i) {
    u32 a = GPR(I_RA(i)), b = GPR(I_RB(i));
    u32 d = b ? a / b : ((GKP(g)->compat & GK_COMPAT_DIV_QEMU) ? a : 0);
    if (I_OE(i)) set_ov(g, b == 0);
    GPR(I_RD(i)) = d;
    if (I_RCB(i)) set_cr0(g, d);
}

// ---- Compare ----
static inline u32 cmp_s(Gekko* g, s32 a, s32 b) { return (a < b ? 8u : a > b ? 4u : 2u) | (g->xer >> 31); }
static inline u32 cmp_u(Gekko* g, u32 a, u32 b) { return (a < b ? 8u : a > b ? 4u : 2u) | (g->xer >> 31); }
static void op_cmp(Gekko* g, u32 i) { set_crf(g, I_CRFD(i), cmp_s(g, (s32)GPR(I_RA(i)), (s32)GPR(I_RB(i)))); }
static void op_cmpl(Gekko* g, u32 i) { set_crf(g, I_CRFD(i), cmp_u(g, GPR(I_RA(i)), GPR(I_RB(i)))); }
static void op_cmpi(Gekko* g, u32 i) { set_crf(g, I_CRFD(i), cmp_s(g, (s32)GPR(I_RA(i)), (s32)I_SIMM(i))); }
static void op_cmpli(Gekko* g, u32 i) { set_crf(g, I_CRFD(i), cmp_u(g, GPR(I_RA(i)), I_UIMM(i))); }

// ---- Logical / rotate / shift (rA = f(rS, rB)) ----
#define LOGIC(name, expr)                          \
    static void name(Gekko* g, u32 i) {            \
        u32 s = GPR(I_RS(i)), b = GPR(I_RB(i));    \
        (void)b;                                   \
        u32 d = (expr);                            \
        GPR(I_RA(i)) = d;                          \
        if (I_RCB(i)) set_cr0(g, d);               \
    }
LOGIC(op_and, s & b)
LOGIC(op_andc, s & ~b)
LOGIC(op_or, s | b)
LOGIC(op_orc, s | ~b)
LOGIC(op_xor, s ^ b)
LOGIC(op_nand, ~(s & b))
LOGIC(op_nor, ~(s | b))
LOGIC(op_eqv, ~(s ^ b))
LOGIC(op_extsb, (u32)(s32)(s8)s)
LOGIC(op_extsh, (u32)(s32)(s16)s)
LOGIC(op_cntlzw, s ? (u32)__builtin_clz(s) : 32u)
LOGIC(op_slw, (b & 0x20) ? 0u : s << (b & 31))
LOGIC(op_srw, (b & 0x20) ? 0u : s >> (b & 31))

static void op_ori(Gekko* g, u32 i) { GPR(I_RA(i)) = GPR(I_RS(i)) | I_UIMM(i); }
static void op_oris(Gekko* g, u32 i) { GPR(I_RA(i)) = GPR(I_RS(i)) | (I_UIMM(i) << 16); }
static void op_xori(Gekko* g, u32 i) { GPR(I_RA(i)) = GPR(I_RS(i)) ^ I_UIMM(i); }
static void op_xoris(Gekko* g, u32 i) { GPR(I_RA(i)) = GPR(I_RS(i)) ^ (I_UIMM(i) << 16); }
static void op_andi_rc(Gekko* g, u32 i) { u32 d = GPR(I_RS(i)) & I_UIMM(i); GPR(I_RA(i)) = d; set_cr0(g, d); }
static void op_andis_rc(Gekko* g, u32 i) { u32 d = GPR(I_RS(i)) & (I_UIMM(i) << 16); GPR(I_RA(i)) = d; set_cr0(g, d); }

static void op_sraw(Gekko* g, u32 i) {
    s32 s = (s32)GPR(I_RS(i));
    u32 n = GPR(I_RB(i)) & 0x3F, d, ca;
    if (n & 0x20) {
        d = s < 0 ? 0xFFFFFFFFu : 0;
        ca = s < 0;
    } else {
        d = (u32)(s >> n);
        ca = s < 0 && n && ((u32)s << (32 - n)) != 0;
    }
    set_ca(g, ca);
    GPR(I_RA(i)) = d;
    if (I_RCB(i)) set_cr0(g, d);
}
static void op_srawi(Gekko* g, u32 i) {
    s32 s = (s32)GPR(I_RS(i));
    u32 n = I_RB(i);
    u32 d = (u32)(s >> n);
    set_ca(g, s < 0 && n && ((u32)s << (32 - n)) != 0);
    GPR(I_RA(i)) = d;
    if (I_RCB(i)) set_cr0(g, d);
}
static void op_rlwimi(Gekko* g, u32 i) {
    u32 m = rlw_mask((i >> 6) & 31, (i >> 1) & 31);
    u32 d = (rotl32(GPR(I_RS(i)), I_RB(i)) & m) | (GPR(I_RA(i)) & ~m);
#if defined(GEKKO_PLANT_BUG) && GEKKO_PLANT_BUG == 1
    d ^= 0x100;  // deliberate bug, only in the harness self-test build (core/tests/cpu/run_tests.sh)
#endif
    GPR(I_RA(i)) = d;
    if (I_RCB(i)) set_cr0(g, d);
}
static void op_rlwinm(Gekko* g, u32 i) {
    u32 d = rotl32(GPR(I_RS(i)), I_RB(i)) & rlw_mask((i >> 6) & 31, (i >> 1) & 31);
    GPR(I_RA(i)) = d;
    if (I_RCB(i)) set_cr0(g, d);
}
static void op_rlwnm(Gekko* g, u32 i) {
    u32 d = rotl32(GPR(I_RS(i)), GPR(I_RB(i)) & 31) & rlw_mask((i >> 6) & 31, (i >> 1) & 31);
    GPR(I_RA(i)) = d;
    if (I_RCB(i)) set_cr0(g, d);
}

// ---- Branches (PEM 4.2.4.1: BO bit 0 = ignore condition, 1 = condition value, 2 = don't decrement CTR,
// 3 = branch if CTR == 0, 4 = static prediction hint) ----
static inline bool bo_ok(Gekko* g, u32 bo, u32 bi) {
    bool ctrOk = true, condOk = true;
    if (!(bo & 4)) {
        g->ctr--;
        ctrOk = ((g->ctr != 0) ^ ((bo >> 1) & 1)) != 0;
    }
    if (!(bo & 0x10)) condOk = (((g->cr >> (31 - bi)) & 1) == ((bo >> 3) & 1));
    return ctrOk && condOk;
}
static void op_b(Gekko* g, u32 i) {
    GkPriv* p = GKP(g);
    u32 li = (u32)(((s32)(i << 6)) >> 6) & ~3u;
    u32 pc = p->curPc;
    p->npc = (i & 2) ? li : pc + li;
    if (i & 1) g->lr = pc + 4;
}
static void op_bc(Gekko* g, u32 i) {
    GkPriv* p = GKP(g);
    u32 pc = p->curPc;
    if (bo_ok(g, I_RD(i), I_RA(i))) {
        u32 bd = (u32)(s32)(s16)(i & 0xFFFC);
        p->npc = (i & 2) ? bd : pc + bd;
    }
    if (i & 1) g->lr = pc + 4;
}
static void op_bclr(Gekko* g, u32 i) {
    GkPriv* p = GKP(g);
    u32 target = g->lr & ~3u;
    if (bo_ok(g, I_RD(i), I_RA(i))) p->npc = target;
    if (i & 1) g->lr = p->curPc + 4;
}
// bcctr with BO[2] = 0 is an invalid form (PEM); this implementation decrements CTR and branches to the old CTR.
static void op_bcctr(Gekko* g, u32 i) {
    GkPriv* p = GKP(g);
    u32 target = g->ctr & ~3u;
    if (bo_ok(g, I_RD(i), I_RA(i))) p->npc = target;
    if (i & 1) g->lr = p->curPc + 4;
}

// ---- Condition register ----
#define CRBIT(n) ((g->cr >> (31 - (n))) & 1)
#define CRLOGIC(name, expr)                                          \
    static void name(Gekko* g, u32 i) {                              \
        u32 a = CRBIT(I_RA(i)), b = CRBIT(I_RB(i)), d = (expr) & 1; \
        u32 sh = 31 - I_RD(i);                                       \
        g->cr = (g->cr & ~(1u << sh)) | (d << sh);                   \
    }
CRLOGIC(op_crand, a & b)
CRLOGIC(op_crandc, a & ~b)
CRLOGIC(op_creqv, ~(a ^ b))
CRLOGIC(op_crnand, ~(a & b))
CRLOGIC(op_crnor, ~(a | b))
CRLOGIC(op_cror, a | b)
CRLOGIC(op_crorc, a | ~b)
CRLOGIC(op_crxor, a ^ b)
static void op_mcrf(Gekko* g, u32 i) { set_crf(g, I_CRFD(i), g->cr >> (28 - 4 * I_CRFS(i))); }
static void op_mcrxr(Gekko* g, u32 i) {
    set_crf(g, I_CRFD(i), g->xer >> 28);
    g->xer &= 0x0FFFFFFFu;
}
static void op_mfcr(Gekko* g, u32 i) { GPR(I_RD(i)) = g->cr; }
static void op_mtcrf(Gekko* g, u32 i) {
    u32 fxm = (i >> 12) & 0xFF, m = 0;
    for (int f = 0; f < 8; f++)
        if (fxm & (0x80u >> f)) m |= 0xF0000000u >> (4 * f);
    g->cr = (g->cr & ~m) | (GPR(I_RS(i)) & m);
}

// ---- Loads and stores ----
#define LOADI(name, EA, RD, CONV, UPD)                    \
    static void name(Gekko* g, u32 i) {                   \
        u32 ea = (EA), v;                                 \
        if (!RD(g, ea, &v)) return;                       \
        GPR(I_RD(i)) = (CONV);                            \
        if (UPD) GPR(I_RA(i)) = ea;                       \
    }
LOADI(op_lwz, EA_D(i), gk_rd32, v, 0)
LOADI(op_lwzu, EA_DU(i), gk_rd32, v, 1)
LOADI(op_lwzx, EA_X(i), gk_rd32, v, 0)
LOADI(op_lwzux, EA_XU(i), gk_rd32, v, 1)
LOADI(op_lbz, EA_D(i), gk_rd8, v, 0)
LOADI(op_lbzu, EA_DU(i), gk_rd8, v, 1)
LOADI(op_lbzx, EA_X(i), gk_rd8, v, 0)
LOADI(op_lbzux, EA_XU(i), gk_rd8, v, 1)
LOADI(op_lhz, EA_D(i), gk_rd16, v, 0)
LOADI(op_lhzu, EA_DU(i), gk_rd16, v, 1)
LOADI(op_lhzx, EA_X(i), gk_rd16, v, 0)
LOADI(op_lhzux, EA_XU(i), gk_rd16, v, 1)
LOADI(op_lha, EA_D(i), gk_rd16, (u32)(s32)(s16)v, 0)
LOADI(op_lhau, EA_DU(i), gk_rd16, (u32)(s32)(s16)v, 1)
LOADI(op_lhax, EA_X(i), gk_rd16, (u32)(s32)(s16)v, 0)
LOADI(op_lhaux, EA_XU(i), gk_rd16, (u32)(s32)(s16)v, 1)
LOADI(op_lwbrx, EA_X(i), gk_rd32, bswap32(v), 0)
LOADI(op_lhbrx, EA_X(i), gk_rd16, bswap16((u16)v), 0)

#define STOREI(name, EA, WR, VAL, UPD)                    \
    static void name(Gekko* g, u32 i) {                   \
        u32 ea = (EA);                                    \
        if (!WR(g, ea, (VAL))) return;                    \
        if (UPD) GPR(I_RA(i)) = ea;                       \
    }
STOREI(op_stw, EA_D(i), gk_wr32, GPR(I_RS(i)), 0)
STOREI(op_stwu, EA_DU(i), gk_wr32, GPR(I_RS(i)), 1)
STOREI(op_stwx, EA_X(i), gk_wr32, GPR(I_RS(i)), 0)
STOREI(op_stwux, EA_XU(i), gk_wr32, GPR(I_RS(i)), 1)
STOREI(op_stb, EA_D(i), gk_wr8, GPR(I_RS(i)) & 0xFF, 0)
STOREI(op_stbu, EA_DU(i), gk_wr8, GPR(I_RS(i)) & 0xFF, 1)
STOREI(op_stbx, EA_X(i), gk_wr8, GPR(I_RS(i)) & 0xFF, 0)
STOREI(op_stbux, EA_XU(i), gk_wr8, GPR(I_RS(i)) & 0xFF, 1)
STOREI(op_sth, EA_D(i), gk_wr16, GPR(I_RS(i)) & 0xFFFF, 0)
STOREI(op_sthu, EA_DU(i), gk_wr16, GPR(I_RS(i)) & 0xFFFF, 1)
STOREI(op_sthx, EA_X(i), gk_wr16, GPR(I_RS(i)) & 0xFFFF, 0)
STOREI(op_sthux, EA_XU(i), gk_wr16, GPR(I_RS(i)) & 0xFFFF, 1)
STOREI(op_stwbrx, EA_X(i), gk_wr32, bswap32(GPR(I_RS(i))), 0)
STOREI(op_sthbrx, EA_X(i), gk_wr16, bswap16((u16)GPR(I_RS(i))), 0)

static void op_lmw(Gekko* g, u32 i) {
    u32 ea = EA_D(i);
    for (u32 r = I_RD(i); r < 32; r++, ea += 4) {
        u32 v;
        if (!gk_rd32(g, ea, &v)) return;
        GPR(r) = v;
    }
}
static void op_stmw(Gekko* g, u32 i) {
    u32 ea = EA_D(i);
    for (u32 r = I_RS(i); r < 32; r++, ea += 4)
        if (!gk_wr32(g, ea, GPR(r))) return;
}

// Load/store string (PEM lswi/lswx): bytes fill registers from the high-order byte, wrapping r31 -> r0; the last
// register's unfilled low bytes are zeroed. A byte count of 0 for lswx/stswx transfers nothing.
static void lsw(Gekko* g, u32 ea, u32 n, u32 rd) {
    u32 buf[32];
    u8 bytes[128];
    for (u32 k = 0; k < n; k++) {
        u32 b;
        if (!gk_rd8(g, ea + k, &b)) return;
        bytes[k] = (u8)b;
    }
    u32 nr = (n + 3) / 4;
    for (u32 r = 0; r < nr; r++) buf[r] = 0;
    for (u32 k = 0; k < n; k++) buf[k / 4] |= (u32)bytes[k] << (24 - 8 * (k & 3));
    for (u32 r = 0; r < nr; r++) GPR((rd + r) & 31) = buf[r];
}
static void stsw(Gekko* g, u32 ea, u32 n, u32 rs) {
    for (u32 k = 0; k < n; k++)
        if (!gk_wr8(g, ea + k, (GPR((rs + k / 4) & 31) >> (24 - 8 * (k & 3))) & 0xFF)) return;
}
static void op_lswi(Gekko* g, u32 i) { u32 n = I_RB(i); lsw(g, RA0(i), n ? n : 32, I_RD(i)); }
static void op_lswx(Gekko* g, u32 i) { lsw(g, EA_X(i), g->xer & 0x7F, I_RD(i)); }
static void op_stswi(Gekko* g, u32 i) { u32 n = I_RB(i); stsw(g, RA0(i), n ? n : 32, I_RS(i)); }
static void op_stswx(Gekko* g, u32 i) { stsw(g, EA_X(i), g->xer & 0x7F, I_RS(i)); }

// lwarx/stwcx.: a single reservation; stwcx. succeeds only if the reservation is held for the same EA (the PEM
// leaves a mismatched address undefined; matching exactly is what qemu does and what the fuzzer can check).
static void op_lwarx(Gekko* g, u32 i) {
    u32 ea = EA_X(i), v;
    if (!gk_rd32(g, ea, &v)) return;
    GPR(I_RD(i)) = v;
    GKP(g)->resv = true;
    GKP(g)->resvAddr = ea;
}
static void op_stwcx(Gekko* g, u32 i) {
    GkPriv* p = GKP(g);
    u32 ea = EA_X(i);
    u32 ok = 0;
    if (p->resv && p->resvAddr == ea) {
        if (!gk_wr32(g, ea, GPR(I_RS(i)))) return;
        ok = 1;
    }
    p->resv = false;
    set_crf(g, 0, (ok << 1) | (g->xer >> 31));
}

// ---- Cache control ----
static void cache_op(Gekko* g, u32 i, int op, bool write) {
    u32 ea = EA_X(i) & ~31u, pa;
    if (!gk_translate(g, ea, write, true, &pa, true)) return;
    if (g->bus->cache_op) g->bus->cache_op(g->bus->ctx, op, pa);
}
static void op_dcbf(Gekko* g, u32 i) { cache_op(g, i, CACHE_DCBF, false); }
static void op_dcbst(Gekko* g, u32 i) { cache_op(g, i, CACHE_DCBST, false); }
static void op_dcbi(Gekko* g, u32 i) {
    if (supervisor_check(g)) cache_op(g, i, CACHE_DCBI, true);
}
static void op_icbi(Gekko* g, u32 i) {
    u32 pa;
    if (gk_translate(g, EA_X(i) & ~31u, false, true, &pa, false) && g->bus->cache_op)
        g->bus->cache_op(g->bus->ctx, CACHE_ICBI, pa);
}
static void op_nop(Gekko* g, u32 i) { (void)g; (void)i; }
static void zero_line(Gekko* g, u32 ea) {
    u32 pa;
    ea &= ~31u;
    if (!gk_translate(g, ea, true, true, &pa, true)) return;
    if (g->bus->dcbz) {
        g->bus->dcbz(g->bus->ctx, pa);
    } else {
        for (u32 k = 0; k < 32; k += 8)
            if (!gk_wr64(g, ea + k, 0)) return;
    }
}
static void op_dcbz(Gekko* g, u32 i) { zero_line(g, EA_X(i)); }
// dcbz_l (750CL): allocate and zero a line in the locked half of the L1 data cache. Illegal unless HID2[LCE].
// The line address is translated like a store and handed to bus->dcbz; the system maps the locked-cache range.
static void op_dcbz_l(Gekko* g, u32 i) {
    if (!(g->spr[SPR_HID2] & HID2_LCE)) { gk_illegal(g, i); return; }
    zero_line(g, EA_X(i));
}

// ---- External control (eciwx/ecowx): require EAR[E], else DSI with DSISR[11] ----
static bool ear_check(Gekko* g, u32 ea, bool write) {
    if (g->spr[282] & 0x80000000u) return true;
    g->spr[SPR_DAR] = ea;
    g->spr[SPR_DSISR] = 0x00100000u | (write ? 0x02000000u : 0);
    gk_exception(g, EXC_DSI, GKP(g)->curPc, 0);
    return false;
}
static void op_eciwx(Gekko* g, u32 i) {
    u32 ea = EA_X(i), v;
    if (!ear_check(g, ea, false) || !gk_rd32(g, ea, &v)) return;
    GPR(I_RD(i)) = v;
}
static void op_ecowx(Gekko* g, u32 i) {
    u32 ea = EA_X(i);
    if (ear_check(g, ea, true)) gk_wr32(g, ea, GPR(I_RS(i)));
}

// ---- Traps and system call ----
static bool trap_cond(u32 to, u32 a, u32 b) {
    return ((to & 0x10) && (s32)a < (s32)b) || ((to & 0x08) && (s32)a > (s32)b) || ((to & 0x04) && a == b) ||
           ((to & 0x02) && a < b) || ((to & 0x01) && a > b);
}
static void op_tw(Gekko* g, u32 i) {
    if (trap_cond(I_RD(i), GPR(I_RA(i)), GPR(I_RB(i)))) gk_program_exception(g, SRR1_TRAP);
}
static void op_twi(Gekko* g, u32 i) {
    if (trap_cond(I_RD(i), GPR(I_RA(i)), I_SIMM(i))) gk_program_exception(g, SRR1_TRAP);
}
static void op_sc(Gekko* g, u32 i) {
    (void)i;
    gk_exception(g, EXC_SYSCALL, GKP(g)->curPc + 4, 0);
}

// ---- Supervisor / SPR ----
static void op_mfmsr(Gekko* g, u32 i) {
    if (supervisor_check(g)) GPR(I_RD(i)) = g->msr;
}
static void op_mtmsr(Gekko* g, u32 i) {
    if (supervisor_check(g)) gk_set_msr(g, GPR(I_RS(i)));
}
// rfi (750CL): MSR[16-31] <- SRR1[16-31] (POW cleared), NIA <- SRR0[0-29] || 00.
static void op_rfi(Gekko* g, u32 i) {
    (void)i;
    if (!supervisor_check(g)) return;
    u32 msr = (g->msr & 0xFFFF0000u & ~MSR_POW) | (g->spr[SPR_SRR1] & 0x0000FFFFu);
    gk_set_msr(g, msr);
    GKP(g)->npc = g->spr[SPR_SRR0] & ~3u;
}
static void op_mfsr(Gekko* g, u32 i) {
    if (supervisor_check(g)) GPR(I_RD(i)) = g->sr[(i >> 16) & 15];
}
static void op_mtsr(Gekko* g, u32 i) {
    if (supervisor_check(g)) g->sr[(i >> 16) & 15] = GPR(I_RS(i));
}
static void op_mfsrin(Gekko* g, u32 i) {
    if (supervisor_check(g)) GPR(I_RD(i)) = g->sr[GPR(I_RB(i)) >> 28];
}
static void op_mtsrin(Gekko* g, u32 i) {
    if (supervisor_check(g)) g->sr[GPR(I_RB(i)) >> 28] = GPR(I_RS(i));
}
static void op_tlbie(Gekko* g, u32 i) { (void)i; supervisor_check(g); }  // no TLB is modelled
static void op_mfspr(Gekko* g, u32 i) {
    u32 spr = I_SPR(i);
    if ((spr & 0x10) && !supervisor_check(g)) return;  // SPR numbers with bit 4 set are supervisor-only
    GPR(I_RD(i)) = gk_mfspr(g, spr);
}
static void op_mtspr(Gekko* g, u32 i) {
    u32 spr = I_SPR(i);
    if ((spr & 0x10) && !supervisor_check(g)) return;
    gk_mtspr(g, spr, GPR(I_RS(i)));
}
static void op_mftb(Gekko* g, u32 i) {
    u32 tbr = I_SPR(i);
    u64 tb = gk_timebase(g);
    if (tbr == SPR_TBL_R) GPR(I_RD(i)) = (u32)tb;
    else if (tbr == SPR_TBU_R) GPR(I_RD(i)) = (u32)(tb >> 32);
    else gk_illegal(g, i);
}

void gk_register_int(void) {
    GkOp* P = gk_prim;
    P[3] = op_twi; P[7] = op_mulli; P[8] = op_subfic; P[10] = op_cmpli; P[11] = op_cmpi;
    P[12] = op_addic; P[13] = op_addic_rc; P[14] = op_addi; P[15] = op_addis; P[16] = op_bc; P[17] = op_sc;
    P[18] = op_b; P[20] = op_rlwimi; P[21] = op_rlwinm; P[23] = op_rlwnm; P[24] = op_ori; P[25] = op_oris;
    P[26] = op_xori; P[27] = op_xoris; P[28] = op_andi_rc; P[29] = op_andis_rc;
    P[32] = op_lwz; P[33] = op_lwzu; P[34] = op_lbz; P[35] = op_lbzu; P[36] = op_stw; P[37] = op_stwu;
    P[38] = op_stb; P[39] = op_stbu; P[40] = op_lhz; P[41] = op_lhzu; P[42] = op_lha; P[43] = op_lhau;
    P[44] = op_sth; P[45] = op_sthu; P[46] = op_lmw; P[47] = op_stmw;

    GkOp* T = gk_op19;
    T[0] = op_mcrf; T[16] = op_bclr; T[33] = op_crnor; T[50] = op_rfi; T[129] = op_crandc; T[150] = op_nop /* isync */;
    T[193] = op_crxor; T[225] = op_crnand; T[257] = op_crand; T[289] = op_creqv; T[417] = op_crorc; T[449] = op_cror;
    T[528] = op_bcctr;

    T = gk_op31;
    // XO-form arithmetic: register both OE = 0 and OE = 1 encodings.
    struct { u32 xo; GkOp f; } oe[] = {
        {8, op_subfc}, {10, op_addc}, {40, op_subf}, {104, op_neg}, {136, op_subfe}, {138, op_adde},
        {200, op_subfze}, {202, op_addze}, {232, op_subfme}, {234, op_addme}, {235, op_mullw}, {266, op_add},
        {459, op_divwu}, {491, op_divw},
    };
    for (size_t k = 0; k < sizeof oe / sizeof oe[0]; k++) T[oe[k].xo] = T[oe[k].xo | 512] = oe[k].f;
    T[0] = op_cmp; T[4] = op_tw; T[11] = op_mulhwu; T[19] = op_mfcr; T[20] = op_lwarx; T[23] = op_lwzx;
    T[24] = op_slw; T[26] = op_cntlzw; T[28] = op_and; T[32] = op_cmpl; T[54] = op_dcbst; T[55] = op_lwzux;
    T[60] = op_andc; T[75] = op_mulhw; T[83] = op_mfmsr; T[86] = op_dcbf; T[87] = op_lbzx; T[119] = op_lbzux;
    T[124] = op_nor; T[144] = op_mtcrf; T[146] = op_mtmsr; T[150] = op_stwcx; T[151] = op_stwx; T[183] = op_stwux;
    T[210] = op_mtsr; T[215] = op_stbx; T[242] = op_mtsrin; T[246] = op_nop /* dcbtst */; T[247] = op_stbux;
    T[278] = op_nop /* dcbt */; T[279] = op_lhzx; T[284] = op_eqv; T[306] = op_tlbie; T[310] = op_eciwx;
    T[311] = op_lhzux; T[316] = op_xor; T[339] = op_mfspr; T[343] = op_lhax; T[371] = op_mftb; T[375] = op_lhaux;
    T[407] = op_sthx; T[412] = op_orc; T[438] = op_ecowx; T[439] = op_sthux; T[444] = op_or; T[467] = op_mtspr;
    T[470] = op_dcbi; T[476] = op_nand; T[512] = op_mcrxr; T[533] = op_lswx; T[534] = op_lwbrx; T[536] = op_srw;
    T[566] = op_tlbie /* tlbsync: supervisor no-op */; T[595] = op_mfsr; T[597] = op_lswi; T[598] = op_nop /* sync */;
    T[659] = op_mfsrin; T[661] = op_stswx; T[662] = op_stwbrx; T[725] = op_stswi; T[790] = op_lhbrx;
    T[792] = op_sraw; T[824] = op_srawi; T[854] = op_nop /* eieio */; T[918] = op_sthbrx; T[922] = op_extsh;
    T[954] = op_extsb; T[982] = op_icbi; T[1014] = op_dcbz;

    gk_op4[1014] = op_dcbz_l;
}
