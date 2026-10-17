#pragma once
// Paired-single operations for the native stand-ins (hle_thp.c, hle_ps.c), computed as gc3ds's JIT computes them,
// which its fuzzer holds equal to the interpreter: add, sub and mul in single precision (for single inputs the same as
// double then single), the multiply-adds in double then rounded to single, the sums' pass-through lane as it is (a
// single already). Every value is a single (loads of floats and s16, results of ps operations, FPR arguments taken only
// when both lanes are singles), so the multiply's 25-bit frC cut never changes one.
typedef struct { float p0, p1; } P;  // ps0, ps1
static inline P PS(float v) { P p = { v, v }; return p; }
#define PADD(x, y) ((P){ (x).p0 + (y).p0, (x).p1 + (y).p1 })
#define PSUB(x, y) ((P){ (x).p0 - (y).p0, (x).p1 - (y).p1 })
#define PMUL(x, c) ((P){ (x).p0 * (c).p0, (x).p1 * (c).p1 })
#define PMULS0(x, c) ((P){ (x).p0 * (c).p0, (x).p1 * (c).p0 })
#define PMULS1(x, c) ((P){ (x).p0 * (c).p1, (x).p1 * (c).p1 })
#if defined(__3DS__) && !defined(HLE_EXACTFLOAT)
#define HLE_FASTFLOAT
#endif
#ifdef HLE_FASTFLOAT
// Relaxed (owner-approved, 2026-10-17; on the 3DS by default: 2DS job 348, cutscene +0.8%, later +1.2%, movie +2.4%):
// the multiply-adds in single precision, two roundings instead of one; results may differ from Gekko's in the last
// bit. The PC build stays exact, so its HLE_CHECK still compares the stand-ins' logic bit for bit.
static inline float madd1(float a, float c, float b) { return a * c + b; }
static inline float msub1(float a, float c, float b) { return a * c - b; }
#else
static inline float madd1(float a, float c, float b) { return (float)((double)a * (double)c + (double)b); }
static inline float msub1(float a, float c, float b) { return (float)((double)a * (double)c - (double)b); }
#endif
#define PMADD(a, c, b) ((P){ madd1((a).p0, (c).p0, (b).p0), madd1((a).p1, (c).p1, (b).p1) })
#define PMADDS0(a, c, b) ((P){ madd1((a).p0, (c).p0, (b).p0), madd1((a).p1, (c).p0, (b).p1) })
#define PMADDS1(a, c, b) ((P){ madd1((a).p0, (c).p1, (b).p0), madd1((a).p1, (c).p1, (b).p1) })
#define PMSUB(a, c, b) ((P){ msub1((a).p0, (c).p0, (b).p0), msub1((a).p1, (c).p1, (b).p1) })
#define PNMSUB(a, c, b) ((P){ -msub1((a).p0, (c).p0, (b).p0), -msub1((a).p1, (c).p1, (b).p1) })
#define PSUM0(a, c, b) ((P){ (a).p0 + (b).p1, (c).p1 })
#define PSUM1(a, c, b) ((P){ (c).p0, (a).p0 + (b).p1 })
#define PNEG(x) ((P){ -(x).p0, -(x).p1 })
#define PMERGE00(x, y) ((P){ (x).p0, (y).p0 })
#define PMERGE01(x, y) ((P){ (x).p0, (y).p1 })
#define PMERGE10(x, y) ((P){ (x).p1, (y).p0 })
#define PMERGE11(x, y) ((P){ (x).p1, (y).p1 })
