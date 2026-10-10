// Floating-point, FPSCR and paired-single instructions, including quantized loads and stores.
//
// Lane behaviour on Gekko (750CL manual chapter on paired singles; confirmed against Dolphin's hardware-test-derived
// interpreter):
//  * double-precision arithmetic (fadd, fmul, fmadd family, fdiv, fsub), fsel, fmr, fneg, fabs, fnabs, fctiw[z],
//    frsqrte, mffs and lfd write ps0 only;
//  * single-precision arithmetic (fadds, fsubs, fmuls, fdivs, fmadds family), fres, frsp and lfs write the result to
//    both ps0 and ps1;
//  * paired-single instructions operate per lane and round each lane to single precision.
#include "gekko_internal.h"
#include "gekko_softfp.h"

#define FA0 fpr_get0(g, I_RA(i))
#define FB0 fpr_get0(g, I_RB(i))
#define FC0 fpr_get0(g, I_RC(i))
#define FA1 fpr_get1(g, I_RA(i))
#define FB1 fpr_get1(g, I_RB(i))
#define FC1 fpr_get1(g, I_RC(i))
#define FD I_RD(i)
#define ENV {g->fpscr & FPSCR_RN, (g->fpscr & FPSCR_NI) != 0, 0}

static inline bool fp_avail(Gekko* g) {
    if (g->msr & MSR_FP) return true;
    gk_exception(g, EXC_FP_UNAVAILABLE, GKP(g)->curPc, 0);
    return false;
}
#define FP_CHECK if (!fp_avail(g)) return

static inline bool ps_avail(Gekko* g, u32 i) {
    if (!(g->spr[SPR_HID2] & HID2_PSE)) {
        gk_illegal(g, i);
        return false;
    }
    return fp_avail(g);
}
#define PS_CHECK if (!ps_avail(g, i)) return

static inline u64 mulc(Gekko* g, u64 c) { return (GKP(g)->compat & GK_COMPAT_EXACT_SINGLE_MUL) ? c : sf_round25(c); }

// FEX and VX are summaries (PEM 3.3.6): VX = OR of the VX* bits, FEX = OR of the enabled exception bits.
static u32 fpscr_summary(u32 f) {
    f &= ~(FPSCR_VX | FPSCR_FEX);
    if (f & FPSCR_VX_ALL) f |= FPSCR_VX;
    if (((f >> 25) & 0x1F) & ((f >> 3) & 0x1F)) f |= FPSCR_FEX;
    return f;
}

// Merge an operation's status into FPSCR: sticky exception bits (FX on any 0 -> 1 transition, XX from FI), and
// optionally replace FR/FI.
static void fpscr_commit(Gekko* g, u32 fl, bool setFIFR) {
    u32 f = g->fpscr;
    u32 exc = fl & FPSCR_EXC_ALL;
    if (fl & FPSCR_FI) exc |= FPSCR_XX;
    if (exc & ~f) f |= FPSCR_FX;
    if (exc && (GKP(g)->compat & GK_COMPAT_FX_ALWAYS)) f |= FPSCR_FX;  // qemu: FX on every signalled exception
    f |= exc;
    if (setFIFR) {
        if (GKP(g)->compat & GK_COMPAT_FR_QEMU) {
            // qemu never sets FR; it clears it only on non-SNaN invalid-operation and zero-divide exceptions.
            if (fl & ((FPSCR_VX_ALL & ~FPSCR_VXSNAN) | FPSCR_ZX)) f &= ~FPSCR_FR;
            f = (f & ~FPSCR_FI) | (fl & FPSCR_FI);
        } else {
            f = (f & ~(FPSCR_FR | FPSCR_FI)) | (fl & (FPSCR_FR | FPSCR_FI));
        }
    }
    g->fpscr = fpscr_summary(f);
}

static inline void set_fprf(Gekko* g, u32 cls) { g->fpscr = (g->fpscr & ~FPSCR_FPRF) | (cls << 12); }
static inline u32 res_class(Gekko* g, u64 r, bool single) {
    return sf_class(r, single && !(GKP(g)->compat & GK_COMPAT_FPRF_DOUBLE));
}
static inline void cr1(Gekko* g, u32 i) {
    if (I_RCB(i)) g->cr = (g->cr & 0xF0FFFFFFu) | ((g->fpscr >> 28) << 24);
}

// Enabled invalid-operation or zero-divide exceptions leave the target unchanged (PEM 3.3.6.1/3.3.6.2).
static bool suppressed(Gekko* g, u32 fl) {
    return ((fl & FPSCR_VX_ALL) && (g->fpscr & FPSCR_VE)) || ((fl & FPSCR_ZX) && (g->fpscr & FPSCR_ZE));
}

enum { W_PS0, W_BOTH };
static void fp_result(Gekko* g, u32 i, const SfEnv* e, u64 r, int lanes, bool single) {
    bool sup = suppressed(g, e->fl);
    fpscr_commit(g, e->fl, true);
    if (!sup) {
        fpr_set0(g, FD, r);
        if (lanes == W_BOTH) fpr_set1(g, FD, r);
        set_fprf(g, res_class(g, r, single));
    }
    cr1(g, i);
}

// ---- Arithmetic ----
static void op_fadd(Gekko* g, u32 i) { FP_CHECK; SfEnv e = ENV; u64 r = sf_add(&e, FA0, FB0, false); fp_result(g, i, &e, r, W_PS0, false); }
static void op_fsub(Gekko* g, u32 i) { FP_CHECK; SfEnv e = ENV; u64 r = sf_sub(&e, FA0, FB0, false); fp_result(g, i, &e, r, W_PS0, false); }
static void op_fmul(Gekko* g, u32 i) { FP_CHECK; SfEnv e = ENV; u64 r = sf_mul(&e, FA0, FC0, false); fp_result(g, i, &e, r, W_PS0, false); }
static void op_fdiv(Gekko* g, u32 i) { FP_CHECK; SfEnv e = ENV; u64 r = sf_div(&e, FA0, FB0, false); fp_result(g, i, &e, r, W_PS0, false); }
static void op_fadds(Gekko* g, u32 i) {
    FP_CHECK;
    SfEnv e = ENV;
    u64 r = sf_add(&e, FA0, FB0, true);
#if defined(GEKKO_PLANT_BUG) && GEKKO_PLANT_BUG == 2
    if (!sf_isnan(r) && !sf_isinf(r) && !sf_iszero(r)) r ^= 1ull << 29;  // deliberate bug: flips the single LSB
#endif
    fp_result(g, i, &e, r, W_BOTH, true);
}
static void op_fsubs(Gekko* g, u32 i) { FP_CHECK; SfEnv e = ENV; u64 r = sf_sub(&e, FA0, FB0, true); fp_result(g, i, &e, r, W_BOTH, true); }
static void op_fmuls(Gekko* g, u32 i) { FP_CHECK; SfEnv e = ENV; u64 r = sf_mul(&e, FA0, mulc(g, FC0), true); fp_result(g, i, &e, r, W_BOTH, true); }
static void op_fdivs(Gekko* g, u32 i) { FP_CHECK; SfEnv e = ENV; u64 r = sf_div(&e, FA0, FB0, true); fp_result(g, i, &e, r, W_BOTH, true); }

// Multiply-add: fmsub = a*c - b, fnmadd = -(a*c + b), fnmsub = -(a*c - b). A NaN result is not negated (PEM fnmadd).

static u64 madd_kind(u32 compat, SfEnv* e, u64 a, u64 c, u64 b, bool sub, bool neg, bool single) {
    if (sub && !sf_isnan(b)) b ^= SF_SIGN;
    if (neg && (compat & GK_COMPAT_NEG_THEN_ROUND) && e->rn >= 2) {
        // qemu rounds the negated exact value: equivalent to rounding in the mirrored directed mode. An exact zero
        // keeps the sign rule of the original mode.
        SfEnv m = *e;
        m.rn ^= 1;
        u64 r = sf_madd(&m, a, c, b, single);
        if (!sf_iszero(r) || (m.fl & FPSCR_FI)) {
            *e = m;
            e->rn ^= 1;
            return sf_isnan(r) ? r : r ^ SF_SIGN;
        }
    }
    u64 r = sf_madd(e, a, c, b, single);
    if (neg && !sf_isnan(r)) r ^= SF_SIGN;
    return r;
}
#define MADD(name, SUB, NEG, SINGLE)                                                            \
    static void name(Gekko* g, u32 i) {                                                         \
        FP_CHECK;                                                                               \
        SfEnv e = ENV;                                                                          \
        u64 c = SINGLE ? mulc(g, FC0) : FC0;                                                    \
        u64 r = madd_kind(GKP(g)->compat, &e, FA0, c, FB0, SUB, NEG, SINGLE);                                   \
        fp_result(g, i, &e, r, SINGLE ? W_BOTH : W_PS0, SINGLE);                                \
    }
MADD(op_fmadd, false, false, false)
MADD(op_fmsub, true, false, false)
MADD(op_fnmadd, false, true, false)
MADD(op_fnmsub, true, true, false)
MADD(op_fmadds, false, false, true)
MADD(op_fmsubs, true, false, true)
MADD(op_fnmadds, false, true, true)
MADD(op_fnmsubs, true, true, true)

static void op_frsp(Gekko* g, u32 i) { FP_CHECK; SfEnv e = ENV; u64 r = sf_round_single(&e, FB0); fp_result(g, i, &e, r, W_BOTH, true); }

static void op_fres(Gekko* g, u32 i) {
    FP_CHECK;
    SfEnv e = ENV;
    u64 r = sf_recip_estimate(&e, FB0);
    fp_result(g, i, &e, r, W_BOTH, true);
}
static void op_frsqrte(Gekko* g, u32 i) {
    FP_CHECK;
    SfEnv e = ENV;
    u64 r = sf_rsqrt_estimate(&e, FB0);
    fp_result(g, i, &e, r, W_PS0, false);
}

// fctiw/fctiwz (ps0 only). The low word is the integer; the high word is 0xFFF80000 on Gekko, with bit 32 also set
// when the result is zero and the operand was negative (hardware tests as reported by Dolphin). FPRF is left
// unchanged (the PEM leaves it undefined).
static void fctiw_common(Gekko* g, u32 i, u32 rn) {
    FP_CHECK;
    SfEnv e = ENV;
    bool negZero;
    u64 b = FB0;
    u32 v = sf_to_int32(&e, b, rn, &negZero);
    bool sup = suppressed(g, e.fl);
    fpscr_commit(g, e.fl, true);
    if ((GKP(g)->compat & GK_COMPAT_FCTIW_QEMU) && (e.fl & FPSCR_VXCVI)) set_fprf(g, 0x11);  // qemu: FPRF = QNaN
    if (!sup) {
        u64 hi;
        if (GKP(g)->compat & GK_COMPAT_FCTIW_QEMU) hi = ((v & 0x80000000u) && !sf_isnan(b)) ? 0xFFFFFFFFull : 0;
        else hi = 0xFFF80000ull | (negZero ? 1u : 0u);
        fpr_set0(g, FD, (hi << 32) | v);
    }
    cr1(g, i);
}
static void op_fctiw(Gekko* g, u32 i) { fctiw_common(g, i, g->fpscr & FPSCR_RN); }
static void op_fctiwz(Gekko* g, u32 i) { fctiw_common(g, i, 1); }

// ---- Moves and select (ps0 only, no FPSCR change) ----
static void op_fmr(Gekko* g, u32 i) { FP_CHECK; fpr_set0(g, FD, FB0); cr1(g, i); }
static void op_fneg(Gekko* g, u32 i) { FP_CHECK; fpr_set0(g, FD, FB0 ^ SF_SIGN); cr1(g, i); }
static void op_fabs(Gekko* g, u32 i) { FP_CHECK; fpr_set0(g, FD, FB0 & ~SF_SIGN); cr1(g, i); }
static void op_fnabs(Gekko* g, u32 i) { FP_CHECK; fpr_set0(g, FD, FB0 | SF_SIGN); cr1(g, i); }
static inline bool ge_zero(u64 a) { return !sf_isnan(a) && (sf_iszero(a) || !(a >> 63)); }
static void op_fsel(Gekko* g, u32 i) {
    FP_CHECK;
    fpr_set0(g, FD, ge_zero(FA0) ? FC0 : FB0);
    cr1(g, i);
}

// ---- Compare ----
static void fcmp(Gekko* g, u32 crf, u64 a, u64 b, bool ordered) {
    u32 c = sf_compare(a, b), fl = 0;
    bool snan = sf_issnan(a) || sf_issnan(b);
    if (snan) fl |= FPSCR_VXSNAN;
    if (ordered && c == 1) {
        // PEM fcmpo: a QNaN operand sets VXVC; an SNaN sets VXSNAN and, if VE = 0, also VXVC.
        if (!snan || !(g->fpscr & FPSCR_VE)) fl |= FPSCR_VXVC;
    }
    g->fpscr = (g->fpscr & ~FPSCR_FPCC) | (c << 12);
    fpscr_commit(g, fl, false);
    if (GKP(g)->compat & GK_COMPAT_FCMP_QEMU) {
        // qemu clears FI on every compare and sets FPRF[C] when fcmpo signals VXVC.
        g->fpscr &= ~FPSCR_FI;
        if (fl & FPSCR_VXVC) g->fpscr |= 0x10000u;
    }
    u32 sh = 28 - 4 * crf;
    g->cr = (g->cr & ~(0xFu << sh)) | (c << sh);
}
static void op_fcmpu(Gekko* g, u32 i) { FP_CHECK; fcmp(g, I_CRFD(i), FA0, FB0, false); }
static void op_fcmpo(Gekko* g, u32 i) { FP_CHECK; fcmp(g, I_CRFD(i), FA0, FB0, true); }

// ---- FPSCR ----
static void op_mffs(Gekko* g, u32 i) {
    FP_CHECK;
    u64 hi = (GKP(g)->compat & GK_COMPAT_MFFS_UPPER_ZERO) ? 0 : 0xFFF80000ull;
    fpr_set0(g, FD, (hi << 32) | g->fpscr);
    cr1(g, i);
}
// mtfsf/mtfsfi/mtfsb: FEX and VX cannot be written explicitly (they are recomputed); FX can. Bit 20 is reserved and
// always reads 0.
static void op_mtfsf(Gekko* g, u32 i) {
    FP_CHECK;
    u32 fm = (i >> 17) & 0xFF, m = 0;
    for (int f = 0; f < 8; f++)
        if (fm & (0x80u >> f)) m |= 0xF0000000u >> (4 * f);
    u32 v = (u32)FB0;
    g->fpscr = fpscr_summary((g->fpscr & ~m) | (v & m & ~FPSCR_RES20));
    cr1(g, i);
}
static void op_mtfsfi(Gekko* g, u32 i) {
    FP_CHECK;
    u32 sh = 28 - 4 * I_CRFD(i);
    u32 imm = (i >> 12) & 0xF;
    g->fpscr = fpscr_summary((g->fpscr & ~(0xFu << sh)) | ((imm << sh) & ~FPSCR_RES20));
    cr1(g, i);
}
static void op_mtfsb0(Gekko* g, u32 i) {
    FP_CHECK;
    u32 bit = I_RD(i);
    if (bit != 1 && bit != 2) g->fpscr = fpscr_summary(g->fpscr & ~(0x80000000u >> bit));
    cr1(g, i);
}
static void op_mtfsb1(Gekko* g, u32 i) {
    FP_CHECK;
    u32 bit = I_RD(i), m = 0x80000000u >> bit;
    if (bit != 1 && bit != 2 && bit != 20) {
        u32 f = g->fpscr;
        // PEM: every instruction except mtfsf/mtfsfi sets FX when an exception bit goes 0 -> 1.
        if ((m & FPSCR_EXC_ALL) && !(f & m) && !(GKP(g)->compat & GK_COMPAT_MTFSB1_QEMU)) f |= FPSCR_FX;
        g->fpscr = fpscr_summary(f | m);
    }
    cr1(g, i);
}
static void op_mcrfs(Gekko* g, u32 i) {
    FP_CHECK;
    u32 sh = 28 - 4 * I_CRFS(i);
    u32 v = (g->fpscr >> sh) & 0xF;
    u32 dsh = 28 - 4 * I_CRFD(i);
    g->cr = (g->cr & ~(0xFu << dsh)) | (v << dsh);
    u32 clear = (0xFu << sh) & (FPSCR_FX | FPSCR_EXC_ALL);
    g->fpscr = fpscr_summary(g->fpscr & ~clear);
}

// ---- FP loads and stores ----
#define RA0(i) (I_RA(i) ? g->gpr[I_RA(i)] : 0u)
#define EA_D(i) (RA0(i) + I_SIMM(i))
#define EA_X(i) (RA0(i) + g->gpr[I_RB(i)])
#define EA_DU(i) (g->gpr[I_RA(i)] + I_SIMM(i))
#define EA_XU(i) (g->gpr[I_RA(i)] + g->gpr[I_RB(i)])

#define LFS(name, EA, UPD)                                       \
    static void name(Gekko* g, u32 i) {                          \
        FP_CHECK;                                                \
        u32 ea = (EA), w;                                        \
        if (!gk_rd32(g, ea, &w)) return;                         \
        u64 d = sf_single_to_double(w);                          \
        fpr_set0(g, FD, d);                                      \
        fpr_set1(g, FD, d);                                      \
        if (UPD) g->gpr[I_RA(i)] = ea;                           \
    }
LFS(op_lfs, EA_D(i), 0)
LFS(op_lfsu, EA_DU(i), 1)
LFS(op_lfsx, EA_X(i), 0)
LFS(op_lfsux, EA_XU(i), 1)

#define LFD(name, EA, UPD)                                       \
    static void name(Gekko* g, u32 i) {                          \
        FP_CHECK;                                                \
        u32 ea = (EA);                                           \
        u64 d;                                                   \
        if (!gk_rd64(g, ea, &d)) return;                         \
        fpr_set0(g, FD, d);                                      \
        if (UPD) g->gpr[I_RA(i)] = ea;                           \
    }
LFD(op_lfd, EA_D(i), 0)
LFD(op_lfdu, EA_DU(i), 1)
LFD(op_lfdx, EA_X(i), 0)
LFD(op_lfdux, EA_XU(i), 1)

#define STFS(name, EA, UPD)                                                                         \
    static void name(Gekko* g, u32 i) {                                                             \
        FP_CHECK;                                                                                   \
        u32 ea = (EA);                                                                              \
        u32 w = sf_double_to_single(fpr_get0(g, I_RS(i)), (GKP(g)->compat & GK_COMPAT_STFS_TINY_ZERO) != 0); \
        if (!gk_wr32(g, ea, w)) return;                                                             \
        if (UPD) g->gpr[I_RA(i)] = ea;                                                              \
    }
STFS(op_stfs, EA_D(i), 0)
STFS(op_stfsu, EA_DU(i), 1)
STFS(op_stfsx, EA_X(i), 0)
STFS(op_stfsux, EA_XU(i), 1)

#define STFD(name, EA, UPD)                                      \
    static void name(Gekko* g, u32 i) {                          \
        FP_CHECK;                                                \
        u32 ea = (EA);                                           \
        if (!gk_wr64(g, ea, fpr_get0(g, I_RS(i)))) return;       \
        if (UPD) g->gpr[I_RA(i)] = ea;                           \
    }
STFD(op_stfd, EA_D(i), 0)
STFD(op_stfdu, EA_DU(i), 1)
STFD(op_stfdx, EA_X(i), 0)
STFD(op_stfdux, EA_XU(i), 1)

static void op_stfiwx(Gekko* g, u32 i) {
    FP_CHECK;
    gk_wr32(g, EA_X(i), (u32)fpr_get0(g, I_RS(i)));
}

// ---- Quantized loads and stores (750CL "Paired-single load and store with quantization") ----
// GQRn: LD_SCALE bits 2-7, LD_TYPE bits 13-15, ST_SCALE bits 18-23, ST_TYPE bits 29-31. Types: 0 float, 4 u8,
// 5 u16, 6 s8, 7 s16 (1-3 reserved: treated as float and logged). Scale is a 6-bit two's-complement value;
// load gives int * 2^-scale, store converts value * 2^scale to the integer type with saturation, truncating toward
// zero after first rounding the operand to single precision (the float-precision pipeline Dolphin models from
// hardware tests). A NaN stores as 0.
static inline int qscale(u32 s) { return (s & 0x20) ? (int)s - 64 : (int)s; }

static u64 int_to_double_scaled(s32 v, int scale) {
    if (v == 0) return 0;
    u64 S = v < 0 ? SF_SIGN : 0;
    u32 mag = v < 0 ? (u32)(-v) : (u32)v;
    int L = 31 - __builtin_clz(mag);
    int e = L - scale;
    return S | ((u64)(e + 1023) << 52) | (((u64)mag << (52 - L)) & SF_FRAC);
}

static u32 quantize(u64 x, u32 type, int scale) {
    s32 lo, hi;
    switch (type) {
    case 4: lo = 0; hi = 255; break;
    case 5: lo = 0; hi = 65535; break;
    case 6: lo = -128; hi = 127; break;
    default: lo = -32768; hi = 32767; break;
    }
    if (sf_isnan(x)) return 0;
    SfEnv t = {0, false, 0};
    x = sf_round_single(&t, x);
    s32 v;
    if (sf_isinf(x) || sf_iszero(x)) {
        v = sf_iszero(x) ? 0 : ((x >> 63) ? lo : hi);
    } else {
        int e = (int)((x >> 52) & 0x7FF) - 1023 + scale;  // exponent after scaling (x is a normal double here)
        u64 m = (x & SF_FRAC) | (1ull << 52);
        s64 mag;
        if (e < 0) mag = 0;
        else if (e >= 31) mag = 0x7FFFFFFF;
        else mag = (s64)(m >> (52 - e));
        s64 sv = (x >> 63) ? -mag : mag;
        v = sv < lo ? lo : sv > hi ? hi : (s32)sv;
    }
    return (u32)v & (type & 1 ? 0xFFFFu : 0xFFu);
}

static bool qload(Gekko* g, u32 ea, u32 type, int scale, u64* out) {
    u32 v;
    switch (type) {
    case 4: if (!gk_rd8(g, ea, &v)) return false; *out = int_to_double_scaled((s32)v, scale); return true;
    case 5: if (!gk_rd16(g, ea, &v)) return false; *out = int_to_double_scaled((s32)v, scale); return true;
    case 6: if (!gk_rd8(g, ea, &v)) return false; *out = int_to_double_scaled((s8)v, scale); return true;
    case 7: if (!gk_rd16(g, ea, &v)) return false; *out = int_to_double_scaled((s16)v, scale); return true;
    default:
        if (!gk_rd32(g, ea, &v)) return false;
        *out = sf_single_to_double(v);
        return true;
    }
}

static bool qstore(Gekko* g, u32 ea, u32 type, int scale, u64 x) {
    switch (type) {
    case 4: case 6: return gk_wr8(g, ea, quantize(x, type, scale));
    case 5: case 7: return gk_wr16(g, ea, quantize(x, type, scale));
    default: return gk_wr32(g, ea, sf_double_to_single_ftz(x));
    }
}

static inline u32 qsize(u32 type) { return type == 4 || type == 6 ? 1 : type == 5 || type == 7 ? 2 : 4; }

static void psq_load(Gekko* g, u32 i, u32 ea, u32 w, u32 idx, bool update) {
    u32 q = g->spr[SPR_GQR0 + idx];
    u32 type = (q >> 16) & 7;
    int scale = qscale((q >> 24) & 0x3F);
    if (type >= 1 && type <= 3) emu_log(LOG_CPU, "psq_l with reserved GQR load type %u at %08x\n", type, GKP(g)->curPc);
    u64 p0, p1 = SF_ONE;
    if (!qload(g, ea, type, scale, &p0)) return;
    if (!w && !qload(g, ea + qsize(type), type, scale, &p1)) return;
    fpr_set0(g, FD, p0);
    fpr_set1(g, FD, p1);
    if (update) g->gpr[I_RA(i)] = ea;
}

static void psq_store(Gekko* g, u32 i, u32 ea, u32 w, u32 idx, bool update) {
    u32 q = g->spr[SPR_GQR0 + idx];
    u32 type = q & 7;
    int scale = qscale((q >> 8) & 0x3F);
    if (type >= 1 && type <= 3) emu_log(LOG_CPU, "psq_st with reserved GQR store type %u at %08x\n", type, GKP(g)->curPc);
    if (!w) {
        // Check the second element's translation first so a fault does not leave a half-written pair.
        u32 pa;
        if (!gk_translate(g, ea + qsize(type), true, true, &pa, true)) return;
    }
    if (!qstore(g, ea, type, scale, fpr_get0(g, I_RS(i)))) return;
    if (!w && !qstore(g, ea + qsize(type), type, scale, fpr_get1(g, I_RS(i)))) return;
    if (update) g->gpr[I_RA(i)] = ea;
}

static inline u32 d12(u32 i) { return (u32)((s32)(i << 20) >> 20); }
static inline bool lsqe(Gekko* g, u32 i) {
    if (!(g->spr[SPR_HID2] & HID2_LSQE)) {
        gk_illegal(g, i);
        return false;
    }
    return true;
}
static void op_psq_l(Gekko* g, u32 i) { PS_CHECK; if (!lsqe(g, i)) return; psq_load(g, i, RA0(i) + d12(i), (i >> 15) & 1, (i >> 12) & 7, false); }
static void op_psq_lu(Gekko* g, u32 i) { PS_CHECK; if (!lsqe(g, i)) return; psq_load(g, i, g->gpr[I_RA(i)] + d12(i), (i >> 15) & 1, (i >> 12) & 7, true); }
static void op_psq_st(Gekko* g, u32 i) { PS_CHECK; if (!lsqe(g, i)) return; psq_store(g, i, RA0(i) + d12(i), (i >> 15) & 1, (i >> 12) & 7, false); }
static void op_psq_stu(Gekko* g, u32 i) { PS_CHECK; if (!lsqe(g, i)) return; psq_store(g, i, g->gpr[I_RA(i)] + d12(i), (i >> 15) & 1, (i >> 12) & 7, true); }
static void op_psq_lx(Gekko* g, u32 i) { PS_CHECK; psq_load(g, i, EA_X(i), (i >> 10) & 1, (i >> 7) & 7, false); }
static void op_psq_lux(Gekko* g, u32 i) { PS_CHECK; psq_load(g, i, EA_XU(i), (i >> 10) & 1, (i >> 7) & 7, true); }
static void op_psq_stx(Gekko* g, u32 i) { PS_CHECK; psq_store(g, i, EA_X(i), (i >> 10) & 1, (i >> 7) & 7, false); }
static void op_psq_stux(Gekko* g, u32 i) { PS_CHECK; psq_store(g, i, EA_XU(i), (i >> 10) & 1, (i >> 7) & 7, true); }

// ---- Paired-single arithmetic ----
// Status from both lanes is merged: sticky bits OR'd, FI/FR OR'd; FPRF describes the lane named by fprfLane. If an
// enabled invalid/zero-divide exception occurs in either lane the target is left unchanged.
static void ps_result(Gekko* g, u32 i, const SfEnv* e0, const SfEnv* e1, u64 r0, u64 r1, int fprfLane) {
    u32 fl = e0->fl | e1->fl;
    bool sup = suppressed(g, fl);
    fpscr_commit(g, fl, true);
    if (!sup) {
        fpr_set0(g, FD, r0);
        fpr_set1(g, FD, r1);
        set_fprf(g, res_class(g, fprfLane ? r1 : r0, true));
    }
    cr1(g, i);
}

#define PS2(name, F, A0, B0, A1, B1)                                         \
    static void name(Gekko* g, u32 i) {                                      \
        PS_CHECK;                                                            \
        SfEnv e0 = ENV, e1 = ENV;                                            \
        u64 r0 = F(&e0, A0, B0, true), r1 = F(&e1, A1, B1, true);            \
        ps_result(g, i, &e0, &e1, r0, r1, 0);                                \
    }
PS2(op_ps_add, sf_add, FA0, FB0, FA1, FB1)
PS2(op_ps_sub, sf_sub, FA0, FB0, FA1, FB1)
PS2(op_ps_div, sf_div, FA0, FB0, FA1, FB1)
PS2(op_ps_mul, sf_mul, FA0, mulc(g, FC0), FA1, mulc(g, FC1))
PS2(op_ps_muls0, sf_mul, FA0, mulc(g, FC0), FA1, mulc(g, FC0))
PS2(op_ps_muls1, sf_mul, FA0, mulc(g, FC1), FA1, mulc(g, FC1))

#define PSMADD(name, C0, C1, SUB, NEG)                                                    \
    static void name(Gekko* g, u32 i) {                                                   \
        PS_CHECK;                                                                         \
        SfEnv e0 = ENV, e1 = ENV;                                                         \
        u64 r0 = madd_kind(GKP(g)->compat, &e0, FA0, mulc(g, C0), FB0, SUB, NEG, true);                   \
        u64 r1 = madd_kind(GKP(g)->compat, &e1, FA1, mulc(g, C1), FB1, SUB, NEG, true);                   \
        ps_result(g, i, &e0, &e1, r0, r1, 0);                                             \
    }
PSMADD(op_ps_madd, FC0, FC1, false, false)
PSMADD(op_ps_msub, FC0, FC1, true, false)
PSMADD(op_ps_nmadd, FC0, FC1, false, true)
PSMADD(op_ps_nmsub, FC0, FC1, true, true)
PSMADD(op_ps_madds0, FC0, FC0, false, false)
PSMADD(op_ps_madds1, FC1, FC1, false, false)

// ps_sum0: ps0 = a0 + b1, ps1 = c1; ps_sum1: ps0 = c0, ps1 = a0 + b1. The pass-through lane is rounded to single
// without affecting FPSCR; FPRF describes the sum.
static u64 round_quiet(u64 x) {
    SfEnv t = {0, false, 0};
    return sf_round_single(&t, x);
}
static void op_ps_sum0(Gekko* g, u32 i) {
    PS_CHECK;
    SfEnv e0 = ENV, e1 = ENV;
    u64 r0 = sf_add(&e0, FA0, FB1, true);
    u64 r1 = round_quiet(FC1);
    ps_result(g, i, &e0, &e1, r0, r1, 0);
}
static void op_ps_sum1(Gekko* g, u32 i) {
    PS_CHECK;
    SfEnv e0 = ENV, e1 = ENV;
    u64 r0 = round_quiet(FC0);
    u64 r1 = sf_add(&e1, FA0, FB1, true);
    ps_result(g, i, &e0, &e1, r0, r1, 1);
}
static void op_ps_res(Gekko* g, u32 i) {
    PS_CHECK;
    SfEnv e0 = ENV, e1 = ENV;
    u64 r0 = sf_recip_estimate(&e0, FB0), r1 = sf_recip_estimate(&e1, FB1);
    ps_result(g, i, &e0, &e1, r0, r1, 0);
}
static void op_ps_rsqrte(Gekko* g, u32 i) {
    PS_CHECK;
    SfEnv e0 = ENV, e1 = ENV;
    u64 r0 = round_quiet(sf_rsqrt_estimate(&e0, FB0)), r1 = round_quiet(sf_rsqrt_estimate(&e1, FB1));
    ps_result(g, i, &e0, &e1, r0, r1, 0);
}
static void op_ps_sel(Gekko* g, u32 i) {
    PS_CHECK;
    u64 r0 = ge_zero(FA0) ? FC0 : FB0, r1 = ge_zero(FA1) ? FC1 : FB1;
    fpr_set0(g, FD, r0);
    fpr_set1(g, FD, r1);
    cr1(g, i);
}
#define PSMOVE(name, X0, X1)                                   \
    static void name(Gekko* g, u32 i) {                        \
        PS_CHECK;                                              \
        u64 r0 = (X0), r1 = (X1);                              \
        fpr_set0(g, FD, r0);                                   \
        fpr_set1(g, FD, r1);                                   \
        cr1(g, i);                                             \
    }
PSMOVE(op_ps_mr, FB0, FB1)
PSMOVE(op_ps_neg, FB0 ^ SF_SIGN, FB1 ^ SF_SIGN)
PSMOVE(op_ps_abs, FB0 & ~SF_SIGN, FB1 & ~SF_SIGN)
PSMOVE(op_ps_nabs, FB0 | SF_SIGN, FB1 | SF_SIGN)
PSMOVE(op_ps_merge00, FA0, FB0)
PSMOVE(op_ps_merge01, FA0, FB1)
PSMOVE(op_ps_merge10, FA1, FB0)
PSMOVE(op_ps_merge11, FA1, FB1)

static void op_ps_cmpu0(Gekko* g, u32 i) { PS_CHECK; fcmp(g, I_CRFD(i), FA0, FB0, false); }
static void op_ps_cmpo0(Gekko* g, u32 i) { PS_CHECK; fcmp(g, I_CRFD(i), FA0, FB0, true); }
static void op_ps_cmpu1(Gekko* g, u32 i) { PS_CHECK; fcmp(g, I_CRFD(i), FA1, FB1, false); }
static void op_ps_cmpo1(Gekko* g, u32 i) { PS_CHECK; fcmp(g, I_CRFD(i), FA1, FB1, true); }

static void fill5(GkOp* t, u32 xo, GkOp f) {
    for (u32 k = 0; k < 1024; k++)
        if ((k & 31) == xo) t[k] = f;
}
static void fill6(GkOp* t, u32 xo, GkOp f) {
    for (u32 k = 0; k < 1024; k++)
        if ((k & 63) == xo) t[k] = f;
}

void gk_register_fpu(void) {
    GkOp* P = gk_prim;
    P[48] = op_lfs; P[49] = op_lfsu; P[50] = op_lfd; P[51] = op_lfdu;
    P[52] = op_stfs; P[53] = op_stfsu; P[54] = op_stfd; P[55] = op_stfdu;
    P[56] = op_psq_l; P[57] = op_psq_lu; P[60] = op_psq_st; P[61] = op_psq_stu;

    GkOp* T = gk_op31;
    T[535] = op_lfsx; T[567] = op_lfsux; T[599] = op_lfdx; T[631] = op_lfdux;
    T[663] = op_stfsx; T[695] = op_stfsux; T[727] = op_stfdx; T[759] = op_stfdux; T[983] = op_stfiwx;

    T = gk_op59;
    T[18] = op_fdivs; T[20] = op_fsubs; T[21] = op_fadds; T[24] = op_fres; T[25] = op_fmuls;
    T[28] = op_fmsubs; T[29] = op_fmadds; T[30] = op_fnmsubs; T[31] = op_fnmadds;

    T = gk_op63;
    fill5(T, 18, op_fdiv); fill5(T, 20, op_fsub); fill5(T, 21, op_fadd); fill5(T, 23, op_fsel);
    fill5(T, 25, op_fmul); fill5(T, 26, op_frsqrte); fill5(T, 28, op_fmsub); fill5(T, 29, op_fmadd);
    fill5(T, 30, op_fnmsub); fill5(T, 31, op_fnmadd);
    T[0] = op_fcmpu; T[12] = op_frsp; T[14] = op_fctiw; T[15] = op_fctiwz; T[32] = op_fcmpo; T[38] = op_mtfsb1;
    T[40] = op_fneg; T[64] = op_mcrfs; T[70] = op_mtfsb0; T[72] = op_fmr; T[134] = op_mtfsfi; T[136] = op_fnabs;
    T[264] = op_fabs; T[583] = op_mffs; T[711] = op_mtfsf;

    T = gk_op4;
    fill5(T, 10, op_ps_sum0); fill5(T, 11, op_ps_sum1); fill5(T, 12, op_ps_muls0); fill5(T, 13, op_ps_muls1);
    fill5(T, 14, op_ps_madds0); fill5(T, 15, op_ps_madds1); fill5(T, 18, op_ps_div); fill5(T, 20, op_ps_sub);
    fill5(T, 21, op_ps_add); fill5(T, 23, op_ps_sel); fill5(T, 24, op_ps_res); fill5(T, 25, op_ps_mul);
    fill5(T, 26, op_ps_rsqrte); fill5(T, 28, op_ps_msub); fill5(T, 29, op_ps_madd); fill5(T, 30, op_ps_nmsub);
    fill5(T, 31, op_ps_nmadd);
    fill6(T, 6, op_psq_lx); fill6(T, 7, op_psq_stx); fill6(T, 38, op_psq_lux); fill6(T, 39, op_psq_stux);
    T[0] = op_ps_cmpu0; T[32] = op_ps_cmpo0; T[40] = op_ps_neg; T[64] = op_ps_cmpu1; T[72] = op_ps_mr;
    T[96] = op_ps_cmpo1; T[136] = op_ps_nabs; T[264] = op_ps_abs; T[528] = op_ps_merge00; T[560] = op_ps_merge01;
    T[592] = op_ps_merge10; T[624] = op_ps_merge11;
}
