// Software floating point with PowerPC semantics. See gekko_softfp.h for the rules implemented.
#include "gekko_softfp.h"
#include "gekko_internal.h"
#include <math.h>

typedef struct { u64 hi, lo; } U128;

static inline int clz64(u64 x) { return x ? __builtin_clzll(x) : 64; }

static inline u64 shr_jam64(u64 x, int n) {
    if (n <= 0) return x;
    if (n >= 64) return x != 0;
    return (x >> n) | ((x << (64 - n)) != 0);
}

static U128 mul64(u64 a, u64 b) {
    u64 a0 = (u32)a, a1 = a >> 32, b0 = (u32)b, b1 = b >> 32;
    u64 p00 = a0 * b0, p01 = a0 * b1, p10 = a1 * b0, p11 = a1 * b1;
    u64 mid = (p00 >> 32) + (u32)p01 + (u32)p10;
    U128 r;
    r.lo = (mid << 32) | (u32)p00;
    r.hi = p11 + (p01 >> 32) + (p10 >> 32) + (mid >> 32);
    return r;
}

static U128 shl128(U128 x, int n) {
    U128 r;
    if (n == 0) return x;
    if (n >= 64) { r.hi = x.lo << (n - 64); r.lo = 0; return r; }
    r.hi = (x.hi << n) | (x.lo >> (64 - n));
    r.lo = x.lo << n;
    return r;
}

static U128 shr128_jam(U128 x, int n) {
    U128 r;
    if (n <= 0) return x;
    if (n >= 128) { r.hi = 0; r.lo = (x.hi | x.lo) != 0; return r; }
    if (n >= 64) {
        u64 lost = x.lo | (n > 64 ? (x.hi << (128 - n)) : 0);
        r.lo = (n == 64 ? x.hi : x.hi >> (n - 64)) | (lost != 0);
        r.hi = 0;
        return r;
    }
    u64 lost = x.lo << (64 - n);
    r.lo = (x.lo >> n) | (x.hi << (64 - n)) | (lost != 0);
    r.hi = x.hi >> n;
    return r;
}

static inline int clz128(U128 x) { return x.hi ? clz64(x.hi) : 64 + clz64(x.lo); }
static inline bool ge128(U128 a, U128 b) { return a.hi > b.hi || (a.hi == b.hi && a.lo >= b.lo); }
static inline U128 add128(U128 a, U128 b) {
    U128 r;
    r.lo = a.lo + b.lo;
    r.hi = a.hi + b.hi + (r.lo < a.lo);
    return r;
}
static inline U128 sub128(U128 a, U128 b) {
    U128 r;
    r.lo = a.lo - b.lo;
    r.hi = a.hi - b.hi - (a.lo < b.lo);
    return r;
}

// Finite nonzero x -> significand with its leading one at bit 52, and the unbiased exponent of that leading one.
static void unpack(u64 x, u64* sig, int* exp) {
    int e = (int)((x >> 52) & 0x7FF);
    u64 f = x & SF_FRAC;
    if (e) {
        *sig = f | (1ull << 52);
        *exp = e - 1023;
    } else {
        int s = clz64(f) - 11;
        *sig = f << s;
        *exp = -1022 - s;
    }
}

// q * 2^e2, exactly representable as a double (q != 0, q < 2^53).
static u64 pack_exact(u64 q, int e2) {
    int L = 63 - clz64(q);
    int dexp = e2 + L;
    if (dexp >= -1022) return ((u64)(dexp + 1023) << 52) | ((q << (52 - L)) & SF_FRAC);
    return q << (e2 + 1074);
}

// Round sig * 2^(exp - 62) (sig: leading one at bit 62, bit 0 sticky) to double or single precision.
static u64 round_pack(SfEnv* env, u32 sign, int exp, u64 sig, bool single) {
    const int emin = single ? -126 : -1022, emax = single ? 127 : 1023;
    const int shift = single ? 39 : 10, prec = single ? 24 : 53;
    const u64 S = (u64)sign << 63;
    bool tiny = exp < emin;
    if (tiny) {
        if (env->ni) {
            env->fl |= FPSCR_UX | FPSCR_FI;
            return S;
        }
        sig = shr_jam64(sig, emin - exp);
        exp = emin;
    }
    u64 rmask = (1ull << shift) - 1, half = 1ull << (shift - 1);
    u64 rb = sig & rmask, q = sig >> shift;
    bool inexact = rb != 0, inc = false;
    switch (env->rn) {
    case 0: inc = rb > half || (rb == half && (q & 1)); break;
    case 1: break;
    case 2: inc = !sign && inexact; break;
    default: inc = sign && inexact; break;
    }
    if (inc) {
        q++;
        if (q >> prec) {
            q >>= 1;
            exp++;
        }
    }
    if (exp > emax) {
        bool toInf = env->rn == 0 || (env->rn == 2 && !sign) || (env->rn == 3 && sign);
        env->fl |= FPSCR_OX | FPSCR_FI | (toInf ? FPSCR_FR : 0);
        if (toInf) return S | SF_EXP;
        return S | (single ? 0x47EFFFFFE0000000ull : 0x7FEFFFFFFFFFFFFFull);
    }
    if (tiny && inexact) env->fl |= FPSCR_UX;
    if (inexact) env->fl |= FPSCR_FI;
    if (inc) env->fl |= FPSCR_FR;
    if (q == 0) return S;
    return S | pack_exact(q, exp - (prec - 1));
}

static u64 round_value(SfEnv* env, u64 x, bool single) {
    u64 m;
    int e;
    unpack(x, &m, &e);
    return round_pack(env, (u32)(x >> 63), e, m << 10, single);
}

static u64 quiet(u64 x, bool single) {
    x |= SF_QBIT;
    if (single) x &= ~0x1FFFFFFFull;
    return x;
}

// NaN result for an operation whose operands (in PPC precedence order) include a NaN.
static u64 nan_result(SfEnv* env, u64 x, u64 y, u64 z, int n, bool single) {
    u64 ops[3] = {x, y, z};
    for (int k = 0; k < n; k++)
        if (sf_issnan(ops[k])) env->fl |= FPSCR_VXSNAN;
    for (int k = 0; k < n; k++)
        if (sf_isnan(ops[k])) return quiet(ops[k], single);
    return SF_DNAN;
}

static u64 invalid(SfEnv* env, u32 flag) {
    env->fl |= flag;
    return SF_DNAN;
}

static u64 zero_sum(SfEnv* env) { return env->rn == 3 ? SF_SIGN : 0; }

u64 sf_add(SfEnv* env, u64 a, u64 b, bool single) {
    if (sf_isnan(a) || sf_isnan(b)) return nan_result(env, a, b, 0, 2, single);
    u32 sa = (u32)(a >> 63), sb = (u32)(b >> 63);
    if (sf_isinf(a)) {
        if (sf_isinf(b) && sa != sb) return invalid(env, FPSCR_VXISI);
        return a;
    }
    if (sf_isinf(b)) return b;
    if (sf_iszero(a) && sf_iszero(b)) return sa == sb ? a : zero_sum(env);
    if (sf_iszero(b)) return round_value(env, a, single);
    if (sf_iszero(a)) return round_value(env, b, single);
    u64 ma, mb;
    int ea, eb;
    unpack(a, &ma, &ea);
    unpack(b, &mb, &eb);
    ma <<= 10;
    mb <<= 10;
    if (ea < eb || (ea == eb && ma < mb)) {
        u64 tm = ma; ma = mb; mb = tm;
        int te = ea; ea = eb; eb = te;
        u32 ts = sa; sa = sb; sb = ts;
    }
    mb = shr_jam64(mb, ea - eb);
    u64 s;
    if (sa == sb) {
        s = ma + mb;
        if (s >> 63) {
            s = shr_jam64(s, 1);
            ea++;
        }
    } else {
        s = ma - mb;
        if (s == 0) return zero_sum(env);
        int sh = clz64(s) - 1;
        s <<= sh;
        ea -= sh;
    }
    return round_pack(env, sa, ea, s, single);
}

u64 sf_sub(SfEnv* env, u64 a, u64 b, bool single) {
    if (!sf_isnan(b)) b ^= SF_SIGN;
    return sf_add(env, a, b, single);
}

static u64 mul_finite(SfEnv* env, u32 sign, u64 a, u64 c, bool single) {
    u64 ma, mc;
    int ea, ec;
    unpack(a, &ma, &ea);
    unpack(c, &mc, &ec);
    U128 p = mul64(ma, mc);
    int exp = ea + ec;
    u64 sig;
    if (p.hi >> 41) {  // product has its leading one at bit 105
        exp++;
        sig = shr128_jam(p, 43).lo;
    } else {
        sig = shr128_jam(p, 42).lo;
    }
    return round_pack(env, sign, exp, sig, single);
}

u64 sf_mul(SfEnv* env, u64 a, u64 c, bool single) {
    if (sf_isnan(a) || sf_isnan(c)) return nan_result(env, a, c, 0, 2, single);
    u32 sign = (u32)((a ^ c) >> 63);
    if (sf_isinf(a) || sf_isinf(c)) {
        if (sf_iszero(a) || sf_iszero(c)) return invalid(env, FPSCR_VXIMZ);
        return ((u64)sign << 63) | SF_EXP;
    }
    if (sf_iszero(a) || sf_iszero(c)) return (u64)sign << 63;
    return mul_finite(env, sign, a, c, single);
}

u64 sf_div(SfEnv* env, u64 a, u64 b, bool single) {
    if (sf_isnan(a) || sf_isnan(b)) return nan_result(env, a, b, 0, 2, single);
    u32 sign = (u32)((a ^ b) >> 63);
    u64 S = (u64)sign << 63;
    if (sf_isinf(a)) {
        if (sf_isinf(b)) return invalid(env, FPSCR_VXIDI);
        return S | SF_EXP;
    }
    if (sf_isinf(b)) return S;
    if (sf_iszero(b)) {
        if (sf_iszero(a)) return invalid(env, FPSCR_VXZDZ);
        env->fl |= FPSCR_ZX;
        return S | SF_EXP;
    }
    if (sf_iszero(a)) return S;
    u64 ma, mb;
    int ea, eb;
    unpack(a, &ma, &ea);
    unpack(b, &mb, &eb);
    int exp = ea - eb;
    if (ma < mb) {
        ma <<= 1;
        exp--;
    }
    // Restoring division: q = floor(ma * 2^62 / mb), leading one at bit 62; remainder becomes the sticky bit.
    u64 rem = ma, q = 0;
    for (int k = 0; k < 63; k++) {
        q <<= 1;
        if (rem >= mb) {
            rem -= mb;
            q |= 1;
        }
        rem <<= 1;
    }
    if (rem) q |= 1;
    return round_pack(env, sign, exp, q, single);
}

u64 sf_madd(SfEnv* env, u64 a, u64 c, u64 b, bool single) {
    bool infZero = (sf_isinf(a) && sf_iszero(c)) || (sf_iszero(a) && sf_isinf(c));
    if (sf_isnan(a) || sf_isnan(b) || sf_isnan(c)) {
        // 0 * inf with a NaN addend still signals VXIMZ (as qemu's softfloat muladd does).
        if (infZero) env->fl |= FPSCR_VXIMZ;
        return nan_result(env, a, b, c, 3, single);
    }
    u32 sp = (u32)((a ^ c) >> 63), sb = (u32)(b >> 63);
    if (sf_isinf(a) || sf_isinf(c)) {
        if (infZero) return invalid(env, FPSCR_VXIMZ);
        if (sf_isinf(b) && sb != sp) return invalid(env, FPSCR_VXISI);
        return ((u64)sp << 63) | SF_EXP;
    }
    if (sf_isinf(b)) return b;
    if (sf_iszero(a) || sf_iszero(c)) {
        if (sf_iszero(b)) return sp == sb ? b : zero_sum(env);
        return round_value(env, b, single);
    }
    if (sf_iszero(b)) return mul_finite(env, sp, a, c, single);

    u64 ma, mc, mb;
    int ea, ec, eb;
    unpack(a, &ma, &ea);
    unpack(c, &mc, &ec);
    unpack(b, &mb, &eb);
    // Exact product and addend, both with the leading one at bit 125: value = M * 2^(e - 125).
    U128 P = mul64(ma, mc);
    int ep = ea + ec;
    if (P.hi >> 41) {
        ep++;
        P = shl128(P, 20);
    } else {
        P = shl128(P, 21);
    }
    U128 B;
    B.hi = mb << 9;
    B.lo = 0;
    U128 big, small;
    int e;
    u32 sign, ssmall;
    if (ep > eb || (ep == eb && ge128(P, B))) {
        big = P; sign = sp; e = ep;
        small = shr128_jam(B, ep - eb); ssmall = sb;
    } else {
        big = B; sign = sb; e = eb;
        small = shr128_jam(P, eb - ep); ssmall = sp;
    }
    U128 s = (sign == ssmall) ? add128(big, small) : sub128(big, small);
    if (s.hi == 0 && s.lo == 0) return zero_sum(env);
    int L = 127 - clz128(s);
    u64 sig = (L >= 62) ? shr128_jam(s, L - 62).lo : (s.lo << (62 - L));
    return round_pack(env, sign, e - 125 + L, sig, single);
}

u64 sf_round_single(SfEnv* env, u64 b) {
    if (sf_isnan(b)) return nan_result(env, b, 0, 0, 1, true);
    if (sf_isinf(b) || sf_iszero(b)) return b;
    return round_value(env, b, true);
}

u32 sf_to_int32(SfEnv* env, u64 b, u32 rn, bool* negZeroIn) {
    u32 sign = (u32)(b >> 63);
    *negZeroIn = false;
    if (sf_isnan(b)) {
        if (sf_issnan(b)) env->fl |= FPSCR_VXSNAN;
        env->fl |= FPSCR_VXCVI;
        return 0x80000000u;
    }
    if (sf_isinf(b)) {
        env->fl |= FPSCR_VXCVI;
        return sign ? 0x80000000u : 0x7FFFFFFFu;
    }
    if (sf_iszero(b)) {
        *negZeroIn = sign != 0;
        return 0;
    }
    u64 m;
    int e;
    unpack(b, &m, &e);
    if (e > 62) {
        env->fl |= FPSCR_VXCVI;
        return sign ? 0x80000000u : 0x7FFFFFFFu;
    }
    u64 ip, fr, half;
    int fbits;
    m <<= 10;  // leading one at bit 62: value = m * 2^(e - 62)
    if (e >= 0) {
        fbits = 62 - e;
        ip = fbits ? m >> fbits : m;
        fr = fbits ? m & ((1ull << fbits) - 1) : 0;
        half = fbits ? 1ull << (fbits - 1) : 0;
    } else {
        ip = 0;
        fr = 1;  // nonzero fraction; only its relation to one half matters below
        half = 2;
        if (e == -1) fr = (m == (1ull << 62)) ? 2 : 3;  // exactly 0.5, or above it
    }
    bool inexact = fr != 0, inc = false;
    switch (rn) {
    case 0: inc = fr > half || (fr == half && (ip & 1)); break;
    case 1: break;
    case 2: inc = !sign && inexact; break;
    default: inc = sign && inexact; break;
    }
    if (inc) ip++;
    if ((!sign && ip > 0x7FFFFFFFull) || (sign && ip > 0x80000000ull)) {
        env->fl |= FPSCR_VXCVI;
        return sign ? 0x80000000u : 0x7FFFFFFFu;
    }
    if (inexact) env->fl |= FPSCR_FI;
    if (inc) env->fl |= FPSCR_FR;
    if (ip == 0) *negZeroIn = sign != 0;
    return sign ? (u32)(0 - (u32)ip) : (u32)ip;
}

u32 sf_compare(u64 a, u64 b) {
    if (sf_isnan(a) || sf_isnan(b)) return 1;
    s64 ka = (a >> 63) ? -(s64)(a & ~SF_SIGN) : (s64)a;
    s64 kb = (b >> 63) ? -(s64)(b & ~SF_SIGN) : (s64)b;
    return ka < kb ? 8u : ka > kb ? 4u : 2u;
}

u32 sf_class(u64 x, bool single) {
    u32 s = (u32)(x >> 63);
    if (sf_isnan(x)) return 0x11;
    if (sf_isinf(x)) return s ? 0x09 : 0x05;
    if (sf_iszero(x)) return s ? 0x12 : 0x02;
    u32 e = (u32)((x >> 52) & 0x7FF);
    bool denorm = single ? e < 897 : e == 0;  // 897 = biased exponent of 2^-126
    if (denorm) return s ? 0x18 : 0x14;
    return s ? 0x08 : 0x04;
}

// fres: the 750CL manual guarantees 1/4096 relative accuracy; the hardware's exact estimate table is not public
// (only Dolphin carries a hardware-derived one), so this returns the correctly rounded single reciprocal, which is
// within that bound. Overflow/underflow are reported; FI/FR are cleared.
u64 sf_recip_estimate(SfEnv* env, u64 b) {
    if (sf_isnan(b)) return nan_result(env, b, 0, 0, 1, true);
    u64 S = b & SF_SIGN;
    if (sf_iszero(b)) {
        env->fl |= FPSCR_ZX;
        return S | SF_EXP;
    }
    if (sf_isinf(b)) return S;
    SfEnv t = {0, env->ni, 0};
    u64 r = sf_div(&t, SF_ONE, b, true);
    env->fl |= t.fl & (FPSCR_OX | FPSCR_UX);
    return r;
}

// frsqrte: manual accuracy is 1/32; this returns 1/sqrt(x) to within an ulp of double precision, computed on a
// mantissa scaled into [1, 4) so the host never sees denormals or overflow (deterministic on any IEEE host).
u64 sf_rsqrt_estimate(SfEnv* env, u64 b) {
    if (sf_isnan(b)) return nan_result(env, b, 0, 0, 1, false);
    if (sf_iszero(b)) {
        env->fl |= FPSCR_ZX;
        return (b & SF_SIGN) | SF_EXP;
    }
    if (b >> 63) return invalid(env, FPSCR_VXSQRT);
    if (sf_isinf(b)) return 0;
    u64 m;
    int e;
    unpack(b, &m, &e);
    int odd = e & 1;
    int k = (e - odd) / 2;  // x = M * 2^(2k), M in [1, 4)
    double M = (double)m * (odd ? 0x1p-51 : 0x1p-52);
    double r = 1.0 / sqrt(M);  // in (0.5, 1]
    u64 rb;
    memcpy(&rb, &r, 8);
    int re = (int)((rb >> 52) & 0x7FF) - k;
    return ((u64)re << 52) | (rb & SF_FRAC);
}

u64 sf_single_to_double(u32 w) {
    u64 S = (u64)(w >> 31) << 63;
    u32 e = (w >> 23) & 0xFF, f = w & 0x7FFFFF;
    if (e == 255) return S | SF_EXP | ((u64)f << 29);
    if (e == 0) {
        if (!f) return S;
        int s = __builtin_clz(f) - 8;  // move the leading one to bit 23
        f <<= s;
        return S | ((u64)(-126 - s + 1023) << 52) | ((u64)(f & 0x7FFFFF) << 29);
    }
    return S | ((u64)(e - 127 + 1023) << 52) | ((u64)f << 29);
}

// PEM D.7: exponents above the single denormal range (or zero) are stored by bit extraction (FRS[0:1] ||
// FRS[5:34]); exponents 874..896 are denormalized by truncation. Below that the result is architecturally
// undefined; hardware tests (reported by Dolphin) show bit extraction again, qemu stores a signed zero.
u32 sf_double_to_single(u64 x, bool tinyToZero) {
    u32 e = (u32)((x >> 52) & 0x7FF);
    u32 extract = (u32)(((x >> 32) & 0xC0000000u) | ((x >> 29) & 0x3FFFFFFFu));
    if (e > 896 || (x & ~SF_SIGN) == 0) return extract;
    u32 sign = (u32)(x >> 63) << 31;
    if (e >= 874) return sign | (u32)(((1ull << 52) | (x & SF_FRAC)) >> (926 - e));
    return tinyToZero ? sign : extract;
}

u32 sf_double_to_single_ftz(u64 x) {
    u32 e = (u32)((x >> 52) & 0x7FF);
    if (e > 896 || (x & ~SF_SIGN) == 0) return (u32)(((x >> 32) & 0xC0000000u) | ((x >> 29) & 0x3FFFFFFFu));
    return (u32)(x >> 63) << 31;
}

// Gekko's single-precision multiplier uses frC rounded to a 25-bit significand (24 fraction bits), rounding half up
// on the first dropped bit and ignoring the rest. Behaviour from hardware tests as documented by Dolphin
// ("Force25Bit"); implemented here from that description.
u64 sf_round25(u64 c) {
    if (sf_isnan(c) || sf_isinf(c) || sf_iszero(c)) return c;
    u32 e = (u32)((c >> 52) & 0x7FF);
    if (e) return (c + 0x08000000ull) & ~0x0FFFFFFFull;
    int L = 63 - clz64(c & SF_FRAC);  // subnormal: keep 25 bits from the leading one
    if (L < 25) return c;
    return (c + (1ull << (L - 25))) & ~((1ull << (L - 24)) - 1);
}
