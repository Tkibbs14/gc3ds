#pragma once
// Software IEEE-754 arithmetic with PowerPC semantics, used by the Gekko FPU and paired-single instructions.
//
// Everything is integer arithmetic on raw double bit patterns, so results, NaN payloads and FPSCR status are identical
// on every host (x86 SSE, ARM VFP in RunFast mode, ...) and independent of the host rounding mode. The rules follow the
// PowerPC Programming Environments Manual (PEM) chapter 3 and appendix D:
//  * results are correctly rounded in the FPSCR[RN] mode; "single" operations round once, from the exact result, to
//    single precision and single exponent range (the result is still held as a double);
//  * tininess is detected before rounding; with UE = 0, UX is set when the result is tiny and inexact;
//  * NaN operands: VXSNAN if any operand is a signalling NaN; the result is the first NaN in the order frA, frB, frC,
//    made quiet (and, for single results, truncated to single precision); invalid operations give the default QNaN
//    0x7FF8000000000000;
//  * FI = result inexact, FR = fraction was incremented (|rounded| > |exact|); both cleared for invalid/NaN/ZX cases.
// NI (non-IEEE mode) is modelled as "tiny results become a signed zero" (operands are not flushed).
#include "../common.h"

#define SF_SIGN 0x8000000000000000ull
#define SF_EXP 0x7FF0000000000000ull
#define SF_FRAC 0x000FFFFFFFFFFFFFull
#define SF_QBIT 0x0008000000000000ull
#define SF_DNAN 0x7FF8000000000000ull
#define SF_ONE 0x3FF0000000000000ull

typedef struct SfEnv {
    u32 rn;   // FPSCR[RN]: 0 nearest-even, 1 toward zero, 2 toward +inf, 3 toward -inf
    bool ni;  // FPSCR[NI]
    u32 fl;   // out: FPSCR exception bits raised (OX UX ZX VX*), plus FI and FR for this result
} SfEnv;

static inline bool sf_isnan(u64 x) { return (x & ~SF_SIGN) > SF_EXP; }
static inline bool sf_issnan(u64 x) { return sf_isnan(x) && !(x & SF_QBIT); }
static inline bool sf_isinf(u64 x) { return (x & ~SF_SIGN) == SF_EXP; }
static inline bool sf_iszero(u64 x) { return (x & ~SF_SIGN) == 0; }

u64 sf_add(SfEnv* e, u64 a, u64 b, bool single);           // a + b
u64 sf_sub(SfEnv* e, u64 a, u64 b, bool single);           // a - b
u64 sf_mul(SfEnv* e, u64 a, u64 c, bool single);           // a * c
u64 sf_div(SfEnv* e, u64 a, u64 b, bool single);           // a / b
u64 sf_madd(SfEnv* e, u64 a, u64 c, u64 b, bool single);   // a * c + b, rounded once
u64 sf_round_single(SfEnv* e, u64 b);                      // frsp
u32 sf_to_int32(SfEnv* e, u64 b, u32 rn, bool* negZeroIn); // fctiw / fctiwz (rn = 1)
u32 sf_compare(u64 a, u64 b);                              // 8 LT, 4 GT, 2 EQ, 1 unordered
u32 sf_class(u64 x, bool single);                          // 5-bit FPRF code
u64 sf_recip_estimate(SfEnv* e, u64 b);                    // fres (single result)
u64 sf_rsqrt_estimate(SfEnv* e, u64 b);                    // frsqrte (double result)
u64 sf_single_to_double(u32 w);                            // lfs: exact, preserves signalling NaNs
u32 sf_double_to_single(u64 x, bool tinyToZero);          // stfs (PEM D.7 store-single algorithm)
u32 sf_double_to_single_ftz(u64 x);                       // psq_st float: single-denormal range stored as 0
u64 sf_round25(u64 c);                                     // Gekko single-precision multiplier input rounding
