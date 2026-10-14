#pragma once
// What the ahead-of-time translated functions (core/tools/aot/ppc2c.py, core/src/aotgen) are written against: guest
// memory through the standard mapping, the slow paths in aot.c, and the floating-point conversions of the JIT's "fast
// float" model. See aot.h for the runtime and docs/research/AOT_PLAN.md for the design.
#include "gekko_internal.h"
#include "gekko_softfp.h"

typedef u32 (*AotFn)(Gekko* g, u32 entry, s32 budget);
// deps: start, size pairs (0-terminated) of the functions inlined into this one, whose bytes hash to dhash.
// gidx: the function's index in aot_valid (direct calls check the callee there).
typedef struct { u32 start, size; u64 hash; AotFn fn; u32 nent; const u32* deps; u64 dhash; u32 gidx; } AotFunc;
typedef struct { const AotFunc* funcs; const u32* n; const u32* entries; } AotPart;

// MEM1's host address minus 0x80000000: guest ea in [0x80000000, 0x81800000) is the byte at aot_base + ea.
extern uintptr_t aot_base;
// Set by a slow path when guest code must leave soon (an interrupt became due, a stop was asked for): loops stop at
// their next back edge, and aot_run returns to the dispatcher.
extern u32 aot_stop;
// Per translated function: 1 its bytes (and its inlined callees') match, 0 they do not, -1 not checked yet. In the
// generated code (aot_gen_index.c).
extern s8 aot_valid[];
// Direct calls between translated functions nest C calls: at most AOT_DEPTH deep (the rest leave to aot_run).
#define AOT_DEPTH 40
extern u32 aot_depth;
// Live switches (aot.h): the direct-call depth limit (0: every call leaves to aot_run), and whether a direct call that
// returned reloads only the registers the EABI lets a callee change (1) or every register (0).
extern u32 aot_depth_max, aot_abi;
AotFn aot_lookup(u32 pc, u32* entry);  // the translated function with an entry at pc (checked), or NULL
bool aot_check_idx(u32 gidx);          // checks function gidx's bytes now; true when it may run
#ifdef AOT_PROF
// Test builds: guest instructions run per translated function (by its index), printed at exit (aot.c).
extern u64 aot_prof[];
#define AOT_PROF_ADD(i, k) (aot_prof[i] += (k))
#else
#define AOT_PROF_ADD(i, k) ((void)0)
#endif
#ifdef AOT_LOG
// Check mode (PC): every store through the fast path is logged first (aot.c compares with the interpreter).
extern u32 aot_logging;
void aot_log_store(u32 ea, u32 size);
#define AOT_LOGST(ea, size) do { if (aot_logging) aot_log_store(ea, size); } while (0)
#else
#define AOT_LOGST(ea, size) ((void)0)
#endif

// Slow paths (aot.c). Status: 0 done, 1 the access raised an exception (g->pc = its handler), 3 not done (check
// mode: leave before the instruction, which the interpreter then runs). `t` is the guest time at the instruction.
u32 aot_ld(Gekko* g, u32 ea, u32 size, u32* v, u32 pc, u64 t);
u32 aot_st(Gekko* g, u32 ea, u32 size, u32 v, u32 pc, u64 t);
u32 aot_ld64(Gekko* g, u32 ea, u64* v, u32 pc, u64 t);
u32 aot_st64(Gekko* g, u32 ea, u64 v, u32 pc, u64 t);
u32 aot_psq_l(Gekko* g, u32 ea, u32 w, u32 q, double* a, double* b, u32 pc, u64 t);
u32 aot_psq_st(Gekko* g, u32 ea, u32 w, u32 q, double a, double b, u32 pc, u64 t);
// One instruction in the interpreter (every register is in g): 0 go on, 1 leave (g->pc set), 3 not run (check mode).
u32 aot_interp(Gekko* g, u32 pc, u32 inst, u64 t);
u32 aot_mtmsr(Gekko* g, u32 v);  // 1: leave after it (other MSR bits changed, or an interrupt is now due)
double aot_fres(double x);
double aot_frsqrte(double x);
double aot_fctiw(Gekko* g, double x, u32 xo);
double aot_s2d_slow(u32 w);

// Guest memory through a base the caller holds in a local (`mb_`: each translated function copies aot_base once, so
// that GCC does not reload the global after every guest store, which may alias anything).
#define MB_(b, ea) ((u8*)((b) + (uintptr_t)(u32)(ea)))
#define M_(ea) MB_(mb_, ea)
#define INMEM(ea, size) ((u32)((ea) - 0x80000000u) <= 0x01800000u - (size))
// In MEM1 and aligned, in one compare: rotating the offset right by log2(size) moves misaligned low bits to the top.
#define ROR32(v, k) (((v) >> (k)) | ((v) << (32 - (k))))
#define OK32(ea) (ROR32((u32)((ea) - 0x80000000u), 2) < 0x00600000u)
#define OK16(ea) (ROR32((u32)((ea) - 0x80000000u), 1) < 0x00C00000u)
#define OK8(ea) ((u32)((ea) - 0x80000000u) < 0x01800000u)
#define OKN(ea, size) ((size) == 4 ? OK32(ea) : (INMEM(ea, size) && !((ea) & 3u)))
#define BSWAP16(v) __builtin_bswap16(v)
#define BSWAP32(v) __builtin_bswap32(v)

static inline u32 LDU32b(uintptr_t b, u32 ea) { u32 v; memcpy(&v, MB_(b, ea), 4); return BSWAP32(v); }
static inline void STU32b(uintptr_t b, u32 ea, u32 v) { AOT_LOGST(ea, 4); v = BSWAP32(v); memcpy(MB_(b, ea), &v, 4); }
static inline bool LD32b(uintptr_t b, u32 ea, u32* v) { if (!OKN(ea, 4)) return false; *v = LDU32b(b, ea); return true; }
static inline bool LD32Rb(uintptr_t b, u32 ea, u32* v) { if (!OKN(ea, 4)) return false; memcpy(v, MB_(b, ea), 4); return true; }
static inline bool LD16b(uintptr_t b, u32 ea, u32* v) {
    if (!OK16(ea)) return false;
    u16 h; memcpy(&h, MB_(b, ea), 2); *v = BSWAP16(h); return true;
}
static inline bool LD16Rb(uintptr_t b, u32 ea, u32* v) { if (!OK16(ea)) return false; u16 h; memcpy(&h, MB_(b, ea), 2); *v = h; return true; }
static inline bool LD8b(uintptr_t b, u32 ea, u32* v) { if (!OK8(ea)) return false; *v = *MB_(b, ea); return true; }
static inline bool ST32b(uintptr_t b, u32 ea, u32 v) { if (!OKN(ea, 4)) return false; STU32b(b, ea, v); return true; }
static inline bool ST32Rb(uintptr_t b, u32 ea, u32 v) { if (!OKN(ea, 4)) return false; AOT_LOGST(ea, 4); memcpy(MB_(b, ea), &v, 4); return true; }
static inline bool ST16b(uintptr_t b, u32 ea, u32 v) {
    if (!OK16(ea)) return false;
    AOT_LOGST(ea, 2); u16 h = BSWAP16((u16)v); memcpy(MB_(b, ea), &h, 2); return true;
}
static inline bool ST16Rb(uintptr_t b, u32 ea, u32 v) { if (!OK16(ea)) return false; AOT_LOGST(ea, 2); u16 h = (u16)v; memcpy(MB_(b, ea), &h, 2); return true; }
static inline bool ST8b(uintptr_t b, u32 ea, u32 v) { if (!OK8(ea)) return false; AOT_LOGST(ea, 1); *MB_(b, ea) = (u8)v; return true; }
static inline bool LD64b(uintptr_t b, u32 ea, u64* v) { if (!OKN(ea, 8)) return false; *v = ((u64)LDU32b(b, ea) << 32) | LDU32b(b, ea + 4); return true; }
static inline bool ST64b(uintptr_t b, u32 ea, u64 v) { if (!OKN(ea, 8)) return false; STU32b(b, ea, (u32)(v >> 32)); STU32b(b, ea + 4, (u32)v); return true; }
#define LDU32(ea) LDU32b(mb_, ea)
#define STU32(ea, v) STU32b(mb_, ea, v)
#define LD32(ea, pv) LD32b(mb_, ea, pv)
#define LD32R(ea, pv) LD32Rb(mb_, ea, pv)
#define LD16(ea, pv) LD16b(mb_, ea, pv)
#define LD16R(ea, pv) LD16Rb(mb_, ea, pv)
#define LD8(ea, pv) LD8b(mb_, ea, pv)
#define ST32(ea, v) ST32b(mb_, ea, v)
#define ST32R(ea, v) ST32Rb(mb_, ea, v)
#define ST16(ea, v) ST16b(mb_, ea, v)
#define ST16R(ea, v) ST16Rb(mb_, ea, v)
#define ST8(ea, v) ST8b(mb_, ea, v)
#define LD64(ea, pv) LD64b(mb_, ea, pv)
#define ST64(ea, v) ST64b(mb_, ea, v)

static inline double U2D(u64 v) { double d; memcpy(&d, &v, 8); return d; }
static inline u64 D2U(double d) { u64 v; memcpy(&v, &d, 8); return v; }
// lfs: exact (normal singles and zeros by the hardware conversion; denormals, infinities and NaNs by bits).
// The JIT's fast-float model: the hardware conversion, so single denormals flush to zero and NaN payloads are not kept.
static inline double S2D(u32 w) { float f; memcpy(&f, &w, 4); return (double)f; }
// stfs (PEM D.7, as the interpreter): bits extracted above the single denormal range.
static inline u32 D2S(double d) {
    u64 x = D2U(d);
    u32 e = (u32)((x >> 52) & 0x7FF);
    if (__builtin_expect(e > 896 || !(x << 1), 1)) return (u32)(((x >> 32) & 0xC0000000u) | ((x >> 29) & 0x3FFFFFFFu));
    return sf_double_to_single(x, false);
}
// psq_st with a float GQR: the single denormal range stores as a signed zero.
static inline u32 D2SF(double d) {
    u64 x = D2U(d);
    u32 e = (u32)((x >> 52) & 0x7FF);
    if (__builtin_expect(e > 896 || !(x << 1), 1)) return (u32)(((x >> 32) & 0xC0000000u) | ((x >> 29) & 0x3FFFFFFFu));
    return (u32)(x >> 63) << 31;
}
// A value known to be an exact single (the translator's analysis): the hardware conversion gives the same bits.
static inline u32 K2S(double d) { float f = (float)d; u32 w; memcpy(&w, &f, 4); return w; }
// Round to single precision (the result stays a double).
#define RS(x) ((double)(float)(x))
// Gekko's single-precision multiplier sees frC with a 25-bit significand (sf_round25); exact singles are unchanged.
// Branchless: for every finite normal value this is sf_round25's rounding; zeros and infinities come out unchanged;
// NaN payloads may change (the fast-float model) and denormals are flushed by the host anyway.
static inline double C25(double c) { return U2D((D2U(c) + 0x08000000ull) & ~0x0FFFFFFFull); }
static inline double NEGNN(double x) { return x == x ? -x : x; }
// fcmpu: 8 LT, 4 GT, 2 EQ, 1 unordered.
static inline u32 FCMP(double a, double b) { return a < b ? 8u : a > b ? 4u : a == b ? 2u : 1u; }

#define CR_OF(d, xer) (((s32)(d) < 0 ? 8u : (d) ? 4u : 2u) | ((xer) >> 31))
#define CMPS(a, b, xer) (((s32)(a) < (s32)(b) ? 8u : (s32)(a) > (s32)(b) ? 4u : 2u) | ((xer) >> 31))
#define CMPU(a, b, xer) (((u32)(a) < (u32)(b) ? 8u : (u32)(a) > (u32)(b) ? 4u : 2u) | ((xer) >> 31))
#define ROTL(v, n) (((v) << (n)) | ((v) >> ((32u - (n)) & 31u)))
#define CLZ(v) ((v) ? (u32)__builtin_clz(v) : 32u)
static inline u32 DIVW(u32 a, u32 b) {
    if (!b || (a == 0x80000000u && b == 0xFFFFFFFFu)) return (s32)a < 0 ? 0xFFFFFFFFu : 0u;
    return (u32)((s32)a / (s32)b);
}

// The write-gather pipe's partial 32-byte burst (aot_set_gather_buffer, as the JIT's): a store that does not complete
// the burst is appended here inline; the one that completes it goes through the slow path to the pipe.
extern u8* aot_gbuf;
extern u32* aot_glen;
static inline void aot_gappend(u32 v, u32 size) {
    u32 l = *aot_glen;
    if (size == 4) { u32 be = BSWAP32(v); memcpy(aot_gbuf + l, &be, 4); }
    else if (size == 2) { u16 be = BSWAP16((u16)v); memcpy(aot_gbuf + l, &be, 2); }
    else aot_gbuf[l] = (u8)v;
    *aot_glen = l + size;
}

// Slow paths as the generated code calls them (inside a translated function: g, n, npc, lk_pc, lk_adj, lk_bef and the
// labels leave and out exist). Each site is one call to a shared helper; when it reports that guest code must leave
// (aot_ls: 1 an exception was taken, 3 not done in check mode), the site records its PC and count adjustments and
// jumps to the function's one leave block. adj: instructions of the block after this one; bef: this one and after.
// The helpers take the guest time as instructions run in this function (g->cycles is the function's start meanwhile)
// and the PC with log2(size) in its two low bits.
extern u32 aot_ls;
u32 aot_ldx(Gekko* g, u32 ea, u32 nb, u32 pcs);
u32 aot_stx(Gekko* g, u32 ea, u32 v, u32 nb, u32 pcs);
u64 aot_ld64x(Gekko* g, u32 ea, u32 nb, u32 pc);
u32 aot_st64x(Gekko* g, u32 ea, u64 v, u32 nb, u32 pc);
u32 aot_psq_lx(Gekko* g, u32 ea, u32 wq, double* a, double* b, u32 nb, u32 pc);
u32 aot_psq_stx(Gekko* g, u32 ea, u32 wq, double a, double b, u32 nb, u32 pc);
#define SZ_LOG(size) ((size) == 4 ? 2u : (size) == 2 ? 1u : 0u)
#define SLOW_GO(PC_, ADJ_, BEF_) do { lk_pc = (PC_); lk_adj = (ADJ_); lk_bef = (BEF_); goto leave; } while (0)
#define SLOW_LD(size, ea, pv, PC_, ADJ_, BEF_) do { *(pv) = aot_ldx(g, ea, n - (BEF_), (PC_) | SZ_LOG(size)); if (__builtin_expect(aot_ls != 0, 0)) SLOW_GO(PC_, ADJ_, BEF_); } while (0)
#define SLOW_ST(size, ea, v, PC_, ADJ_, BEF_) do { if (__builtin_expect(aot_stx(g, ea, v, n - (BEF_), (PC_) | SZ_LOG(size)) != 0, 0)) SLOW_GO(PC_, ADJ_, BEF_); } while (0)
#define SLOW_LD64(ea, pv, PC_, ADJ_, BEF_) do { *(pv) = aot_ld64x(g, ea, n - (BEF_), PC_); if (__builtin_expect(aot_ls != 0, 0)) SLOW_GO(PC_, ADJ_, BEF_); } while (0)
#define SLOW_ST64(ea, v, PC_, ADJ_, BEF_) do { if (__builtin_expect(aot_st64x(g, ea, v, n - (BEF_), PC_) != 0, 0)) SLOW_GO(PC_, ADJ_, BEF_); } while (0)
#define PSQ_SLOW_L(ea, w, q, pa, pb, PC_, ADJ_, BEF_) do { if (__builtin_expect(aot_psq_lx(g, ea, ((w) << 3) | (q), pa, pb, n - (BEF_), PC_) != 0, 0)) SLOW_GO(PC_, ADJ_, BEF_); } while (0)
#define PSQ_SLOW_S(ea, w, q, a, b, PC_, ADJ_, BEF_) do { if (__builtin_expect(aot_psq_stx(g, ea, ((w) << 3) | (q), a, b, n - (BEF_), PC_) != 0, 0)) SLOW_GO(PC_, ADJ_, BEF_); } while (0)
