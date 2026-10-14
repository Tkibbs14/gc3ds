// Runtime for ahead-of-time translated guest code (aot.h, docs/research/AOT_PLAN.md).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#if defined(__x86_64__) || defined(__i386__)
#include <xmmintrin.h>
#endif
#include "aot.h"
#include "aot_rt.h"

// The generated code (core/src/aotgen, from ppc2c.py); absent in the repository, so referenced weakly.
extern const AotPart aot_parts[] __attribute__((weak));
extern const u32 aot_nparts __attribute__((weak));

uintptr_t aot_base;
u32 aot_stop;
AotStats aot_stats;
static bool g_on;
static bool g_checking;  // check mode (PC): slow paths and fallbacks leave before their instruction
static int g_noSlow = -1, g_noFb = -1;
#ifndef __3DS__
// Check mode: a slow path reached with an aligned MEM1 address is a translation bug (the fast path takes those).
static void chk_slow(u32 ea, u32 size, u32 pc) {
    static u32 n;
    if (g_checking && INMEM(ea, size) && !(ea & (size - 1)) && n++ < 20) fprintf(stderr, "AOT_CHECK slow path for MEM1 address %08x (size %u) at %08x\n", ea, size, pc);
}
#else
#define chk_slow(ea, size, pc) ((void)0)
#endif  // AOT_NOSLOW / AOT_NOFB (PC): slow paths / fallbacks leave before their instruction
static bool g_ni;        // FPSCR[NI] when the run started (estimates)
static void (*g_gatherPut)(void* ctx, u32 v, u32 size);
static void* g_gatherCtx;
static u32 g_gatherEa = 1;

typedef struct { const AotFunc* f; const u32* ents; s8 valid; } Func;  // valid: -1 not checked, 0 other code, 1 ok
typedef struct { u32 pc; u32 fi; u32 ei; } Slot;
static Func* g_funcs;
static u32 g_nfuncs;
static Slot* g_slots;
static u32 g_slotMask;

bool aot_available(void) {
    const volatile void* parts = aot_parts;  // weak: NULL without generated code
    return parts != NULL && &aot_nparts != NULL && aot_nparts > 0;
}

static u32 slot_hash(u32 pc) { return (pc >> 2) * 2654435761u; }

static int by_start(const void* a, const void* b) {
    u32 x = ((const Func*)a)->f->start, y = ((const Func*)b)->f->start;
    return x < y ? -1 : x > y;
}

static void build(void) {
    if (g_funcs || !aot_available()) return;
    u32 nf = 0, ne = 0;
    for (u32 p = 0; p < aot_nparts; p++) {
        nf += *aot_parts[p].n;
        for (u32 k = 0; k < *aot_parts[p].n; k++) ne += aot_parts[p].funcs[k].nent;
    }
    g_funcs = calloc(nf ? nf : 1, sizeof *g_funcs);
    u32 size = 1024;
    while (size < 2 * ne) size *= 2;
    g_slots = calloc(size, sizeof *g_slots);
    if (!g_funcs || !g_slots) { free(g_funcs); free(g_slots); g_funcs = NULL; g_slots = NULL; return; }
    g_slotMask = size - 1;
    for (u32 p = 0; p < aot_nparts; p++) {
        const u32* e = aot_parts[p].entries;
        for (u32 k = 0; k < *aot_parts[p].n; k++) {
            const AotFunc* f = &aot_parts[p].funcs[k];
            g_funcs[g_nfuncs++] = (Func){ f, e, -1 };
            e += f->nent;
        }
    }
#ifndef __3DS__
    if (getenv("AOT_RANGE")) {  // debugging: only functions starting in [lo, hi) (hex lo-hi)
        u32 lo = 0, hi = 0;
        sscanf(getenv("AOT_RANGE"), "%x-%x", &lo, &hi);
        u32 m = 0;
        for (u32 i = 0; i < g_nfuncs; i++)
            if (g_funcs[i].f->start >= lo && g_funcs[i].f->start < hi) g_funcs[m++] = g_funcs[i];
        g_nfuncs = m;
    }
#endif
    qsort(g_funcs, g_nfuncs, sizeof *g_funcs, by_start);
    for (u32 fi = 0; fi < g_nfuncs; fi++) {
        const Func* F = &g_funcs[fi];
        for (u32 ei = 0; ei < F->f->nent; ei++) {
            u32 pc = F->ents[ei], h = slot_hash(pc) & g_slotMask;
            while (g_slots[h].pc && g_slots[h].pc != pc) h = (h + 1) & g_slotMask;
            if (g_slots[h].pc) continue;  // an entry of two functions (overlapping symbols): the first one keeps it
            g_slots[h] = (Slot){ pc, fi, ei };
        }
    }
}

void aot_set_enabled(bool on) {
    g_on = on && aot_available();
    if (g_on) build();
    if (!g_funcs) g_on = false;
#if defined(__x86_64__) || defined(__i386__)
    // The 3DS runs translated code with flush-to-zero (jit_host_fpscr_init); do the same on the PC.
    if (g_on) _mm_setcsr(_mm_getcsr() | 0x8040);
#endif
}
bool aot_enabled(void) { return g_on; }

void aot_set_memory(u8* mem1) { aot_base = (uintptr_t)mem1 - 0x80000000u; aot_invalidate_all(); }

void aot_set_gather(void (*put)(void* ctx, u32 v, u32 size), void* ctx, u32 ea) {
    g_gatherPut = put;
    g_gatherCtx = ctx;
    g_gatherEa = ea;
}

bool aot_state_ok(const Gekko* g) {
    return (g->msr & (MSR_IR | MSR_DR | MSR_PR | MSR_FP)) == (MSR_IR | MSR_DR | MSR_FP) && !(g->fpscr & FPSCR_RN) &&
           (g->spr[SPR_HID2] & (HID2_PSE | HID2_LSQE)) == (HID2_PSE | HID2_LSQE);
}

static const Slot* find(u32 pc) {
    if (!g_slots) return NULL;
    for (u32 h = slot_hash(pc) & g_slotMask;; h = (h + 1) & g_slotMask) {
        if (g_slots[h].pc == pc) return &g_slots[h];
        if (!g_slots[h].pc) return NULL;
    }
}

bool aot_is_entry(u32 pc) { return g_on && find(pc) != NULL; }

static u64 fnv(const u8* p, u32 n) {
    u64 h = 0xCBF29CE484222325ull;
    for (u32 i = 0; i < n; i++) { h ^= p[i]; h *= 0x100000001B3ull; }
    return h;
}

static bool check_func(Func* F) {
    u32 a = F->f->start, n = F->f->size;
    if (!aot_base || !INMEM(a, n)) { F->valid = 0; return false; }
    F->valid = fnv(M_(a), n) == F->f->hash;
    if (!F->valid) aot_stats.hashFail++;
    return F->valid;
}

void aot_invalidate_all(void) {
    for (u32 i = 0; i < g_nfuncs; i++) g_funcs[i].valid = -1;
}

void aot_invalidate(u32 pa) {
    if (!g_nfuncs) return;
    u32 lo = 0x80000000u | (pa & ~0xFFFu), hi = lo + 0x1000u;
    // the first function ending after lo, then every one starting before hi
    u32 a = 0, b = g_nfuncs;
    while (a < b) { u32 m = (a + b) / 2; if (g_funcs[m].f->start + g_funcs[m].f->size <= lo) a = m + 1; else b = m; }
    for (u32 i = a > 0 ? a - 1 : 0; i < g_nfuncs && g_funcs[i].f->start < hi; i++)
        if (g_funcs[i].f->start + g_funcs[i].f->size > lo) g_funcs[i].valid = -1;
}

// ---- slow paths ----
static inline u64 time_set(Gekko* g, u64 t) { u64 s = g->cycles; g->cycles = t; return s; }

u32 aot_ld(Gekko* g, u32 ea, u32 size, u32* v, u32 pc, u64 t) {
    if (g_checking || g_noSlow == 1) { chk_slow(ea, size, pc); return 3; }
    if (getenv("AOT_NOSLOWLD")) return 3;
    aot_stats.slow++;
#ifndef __3DS__
    if (getenv("AOT_SLOWLOG") && aot_stats.slow < 40) fprintf(stderr, "aot slow load %u at %08x from %08x\n", size, ea, pc);
#endif
    GkPriv* p = GKP(g);
    p->curPc = pc;
    p->npc = pc + 4;
    u64 s = time_set(g, t);
    bool ok = size == 1 ? gk_rd8(g, ea, v) : size == 2 ? gk_rd16(g, ea, v) : gk_rd32(g, ea, v);
    g->cycles = s;
    if (!ok) { g->pc = p->npc; aot_stop = 1; return 1; }
    if (p->brk || p->stopReq) aot_stop = 1;
    return 0;
}

u32 aot_st(Gekko* g, u32 ea, u32 size, u32 v, u32 pc, u64 t) {
    if (g_checking || g_noSlow == 1) { chk_slow(ea, size, pc); return 3; }
    if (getenv("AOT_NOSLOWST")) return 3;
    aot_stats.slow++;
#ifndef __3DS__
    if (getenv("AOT_SLOWLOG") && aot_stats.slow < 40) fprintf(stderr, "aot slow store %u %08x at %08x from %08x\n", size, v, ea, pc);
#endif
    GkPriv* p = GKP(g);
    p->curPc = pc;
    p->npc = pc + 4;
    u64 s = time_set(g, t);
    bool ok = true;
    if (ea == g_gatherEa && g_gatherPut) g_gatherPut(g_gatherCtx, v, size);
    else ok = size == 1 ? gk_wr8(g, ea, v) : size == 2 ? gk_wr16(g, ea, v) : gk_wr32(g, ea, v);
    g->cycles = s;
    if (!ok) { g->pc = p->npc; aot_stop = 1; return 1; }
    if (p->brk || p->stopReq) aot_stop = 1;
    return 0;
}

u32 aot_ld64(Gekko* g, u32 ea, u64* v, u32 pc, u64 t) {
    if (g_checking || g_noSlow == 1) { chk_slow(ea, 4, pc); return 3; }
    aot_stats.slow++;
    GkPriv* p = GKP(g);
    p->curPc = pc;
    p->npc = pc + 4;
    u64 s = time_set(g, t);
    bool ok = gk_rd64(g, ea, v);
    g->cycles = s;
    if (!ok) { g->pc = p->npc; aot_stop = 1; return 1; }
    if (p->brk || p->stopReq) aot_stop = 1;
    return 0;
}

u32 aot_st64(Gekko* g, u32 ea, u64 v, u32 pc, u64 t) {
    if (g_checking || g_noSlow == 1) { chk_slow(ea, 4, pc); return 3; }
    aot_stats.slow++;
    GkPriv* p = GKP(g);
    p->curPc = pc;
    p->npc = pc + 4;
    u64 s = time_set(g, t);
    bool ok = gk_wr64(g, ea, v);
    g->cycles = s;
    if (!ok) { g->pc = p->npc; aot_stop = 1; return 1; }
    if (p->brk || p->stopReq) aot_stop = 1;
    return 0;
}

u32 aot_psq_l(Gekko* g, u32 ea, u32 w, u32 q, double* a, double* b, u32 pc, u64 t) {
    if (g_checking || g_noSlow == 1) return 3;
    aot_stats.slow++;
    GkPriv* p = GKP(g);
    p->curPc = pc;
    p->npc = pc + 4;
    u64 s = time_set(g, t), x0, x1;
    bool ok = gk_psq_load(g, ea, w, q, &x0, &x1);
    g->cycles = s;
    if (!ok) { g->pc = p->npc; aot_stop = 1; return 1; }
    *a = U2D(x0);
    *b = U2D(x1);
    if (p->brk || p->stopReq) aot_stop = 1;
    return 0;
}

u32 aot_psq_st(Gekko* g, u32 ea, u32 w, u32 q, double a, double b, u32 pc, u64 t) {
    if (g_checking || g_noSlow == 1) return 3;
    aot_stats.slow++;
    GkPriv* p = GKP(g);
    p->curPc = pc;
    p->npc = pc + 4;
    u64 s = time_set(g, t);
    bool ok = gk_psq_store(g, ea, w, q, D2U(a), D2U(b));
    g->cycles = s;
    if (!ok) { g->pc = p->npc; aot_stop = 1; return 1; }
    if (p->brk || p->stopReq) aot_stop = 1;
    return 0;
}

static u32 key_of(const Gekko* g) {
    return (g->msr & (MSR_IR | MSR_DR | MSR_PR | MSR_FP)) | (g->fpscr & FPSCR_RN) | (g->spr[SPR_HID2] & (HID2_PSE | HID2_LSQE));
}

u32 aot_interp(Gekko* g, u32 pc, u32 inst, u64 t) {
    if (g_checking || g_noFb == 1) { g->pc = pc; return 3; }
    aot_stats.fallbacks++;
    u32 key = key_of(g);
    u64 s = time_set(g, t);
    bool stop;
    u32 npc = gekko_interp_one(g, pc, inst, &stop);
    g->cycles = s;
    if (npc != pc + 4 || stop || key_of(g) != key) { g->pc = npc; aot_stop = 1; return 1; }
    return 0;
}

u32 aot_mtmsr(Gekko* g, u32 v) {
    u32 old = g->msr;
    if (((old ^ v) & ~MSR_EE) || ((v & ~old & MSR_EE) && (g->extIrq || GKP(g)->decPending))) {
        gk_set_msr(g, v);
        aot_stop = 1;
        return 1;
    }
    g->msr = v;
    return 0;
}

double aot_fres(double x) { SfEnv e = { 0, g_ni, 0 }; return U2D(sf_recip_estimate(&e, D2U(x))); }
double aot_frsqrte(double x) { SfEnv e = { 0, g_ni, 0 }; return U2D(sf_rsqrt_estimate(&e, D2U(x))); }
double aot_fctiw(Gekko* g, double x, u32 xo) {
    SfEnv e = { g->fpscr & FPSCR_RN, (g->fpscr & FPSCR_NI) != 0, 0 };
    bool negZero;
    u32 v = sf_to_int32(&e, D2U(x), xo == 15 ? 1 : (g->fpscr & FPSCR_RN), &negZero);
    return U2D(((0xFFF80000ull | (negZero ? 1u : 0u)) << 32) | v);
}
double aot_s2d_slow(u32 w) { return U2D(sf_single_to_double(w)); }

// ---- check mode (PC): every call compared with the interpreter ----
#ifndef __3DS__
u32 aot_logging;
typedef struct { u32 ea; u8 size; u8 old[8]; } LogEnt;
#define LOG_MAX 65536
static LogEnt* g_log;
static u32 g_logN;
static bool g_logFull;
void aot_log_store(u32 ea, u32 size) {
    if (g_logN == LOG_MAX) { g_logFull = true; return; }
    LogEnt* e = &g_log[g_logN++];
    e->ea = ea;
    e->size = (u8)size;
    memcpy(e->old, M_(ea), size);
}
extern void (*g_gkStoreHook)(u32 ea, u32 size);
static void interp_store_hook(u32 ea, u32 size) {
    if (INMEM(ea, size)) aot_log_store(ea, size);
}

static struct { u64 calls, bad, fbad, skipped, insns; } g_chk;
static void chk_report(void) {
    fprintf(stderr, "AOT_CHECK: %llu calls (%llu instructions) compared with the interpreter: %llu differed, %llu differed only in "
            "FPRs, %llu not compared (store log full)\n", (unsigned long long)g_chk.calls, (unsigned long long)g_chk.insns,
            (unsigned long long)g_chk.bad, (unsigned long long)g_chk.fbad, (unsigned long long)g_chk.skipped);
}

static u32 checked_call(Gekko* g, const Func* F, u32 ei, s32 budget) {
    static Gekko g0, gA;
    static u8* aNew;  // AOT's final bytes, per log entry
    static LogEnt* aLog;
    if (!g_log) { g_log = malloc(LOG_MAX * sizeof *g_log); aLog = malloc(LOG_MAX * sizeof *aLog); aNew = malloc(LOG_MAX * 8); atexit(chk_report); }
    g0 = *g;
    g_logN = 0; g_logFull = false;
    aot_logging = 1;
    g_checking = true;
    g_gkStoreHook = NULL;  // fallbacks are off in check mode; the hook only sees the interpreter below
    u32 n = F->f->fn(g, ei, budget);
    g_checking = false;
    aot_logging = 0;
    if (!n) return 0;
    gA = *g;
    u32 an = g_logN;
    bool full = g_logFull;
    memcpy(aLog, g_log, an * sizeof *g_log);
    for (u32 k = 0; k < an; k++) memcpy(aNew + 8 * k, M_(aLog[k].ea), aLog[k].size);
    for (u32 k = an; k-- > 0;) memcpy(M_(aLog[k].ea), aLog[k].old, aLog[k].size);  // undo
    *g = g0;
    g_logN = 0; g_logFull = false;
    aot_logging = 1;
    g_gkStoreHook = interp_store_hook;
    u32 steps = 0;
    while (steps < n) {
        u32 pc = g->pc, inst;
        if (!INMEM(pc, 4)) break;
        inst = LDU32(pc);
        bool stop;
        g->pc = gekko_interp_one(g, pc, inst, &stop);
        steps++;
    }
    g_gkStoreHook = NULL;
    aot_logging = 0;
    g_chk.calls++;
    g_chk.insns += n;
    if (full || g_logFull) { g_chk.skipped++; return steps; }
    bool same = steps == n && g->pc == gA.pc && !memcmp(g->gpr, gA.gpr, sizeof g->gpr) && g->cr == gA.cr &&
                g->xer == gA.xer && g->lr == gA.lr && g->ctr == gA.ctr && g->msr == gA.msr;  // FPSCR: only fcmp's FPCC is kept (fast float)
    bool memSame = true;
    u32 badEa = 0;
    // every byte the AOT wrote holds the interpreter's final value, and every byte the interpreter wrote that the AOT
    // did not write is unchanged
    for (u32 k = 0; k < an && memSame; k++)
        for (u32 b = 0; b < aLog[k].size; b++) {
            u32 ea = aLog[k].ea + b;
            u8 last = aNew[8 * k + b];
            for (u32 m = k + 1; m < an; m++)  // a later AOT store to the same byte decides
                if (ea - aLog[m].ea < aLog[m].size) last = aNew[8 * m + (ea - aLog[m].ea)];
            if (*M_(ea) != last) { memSame = false; badEa = ea; break; }
        }
    for (u32 k = 0; k < g_logN && memSame; k++)
        for (u32 b = 0; b < g_log[k].size; b++) {
            u32 ea = g_log[k].ea + b;
            bool aw = false;
            for (u32 m = 0; m < an && !aw; m++) aw = ea - aLog[m].ea < aLog[m].size;
            if (aw) continue;
            u8 orig = g_log[k].old[b];
            for (u32 m = 0; m < k; m++)  // the first interpreter store to the byte saw its original value
                if (ea - g_log[m].ea < g_log[m].size) { orig = g_log[m].old[ea - g_log[m].ea]; break; }
            if (*M_(ea) != orig) { memSame = false; badEa = ea; break; }
        }
    bool fsame = !memcmp(g->fpr, gA.fpr, sizeof g->fpr);
    if (same && memSame && fsame) return steps;
    if (same && memSame) {
        g_chk.fbad++;
        if (g_chk.fbad <= 10) {
            fprintf(stderr, "AOT_CHECK fpr-only difference in %08x (entry %u) from %08x, %u insns:\n", F->f->start, ei, g0.pc, n);
            for (int r = 0; r < 32; r++) {
                u64 a0 = D2U(gA.fpr[r].ps0), i0 = D2U(g->fpr[r].ps0), a1 = D2U(gA.fpr[r].ps1), i1 = D2U(g->fpr[r].ps1);
                if (a0 != i0 || a1 != i1)
                    fprintf(stderr, "  f%d aot %016llx %016llx interp %016llx %016llx\n", r, (unsigned long long)a0,
                            (unsigned long long)a1, (unsigned long long)i0, (unsigned long long)i1);
            }
        }
        return steps;
    }
    g_chk.bad++;
    if (g_chk.bad <= 30) {
        fprintf(stderr, "AOT_CHECK %08x (entry %u) from %08x: aot %u insns to %08x, interp %u to %08x; cr %08x/%08x xer %08x/%08x "
                "lr %08x/%08x ctr %08x/%08x%s\n", F->f->start, ei, g0.pc, n, gA.pc, steps, g->pc, gA.cr, g->cr, gA.xer, g->xer,
                gA.lr, g->lr, gA.ctr, g->ctr, memSame ? "" : " MEMORY");
        for (int r = 0; r < 32; r++)
            if (g->gpr[r] != gA.gpr[r]) fprintf(stderr, "  r%d aot %08x interp %08x\n", r, gA.gpr[r], g->gpr[r]);
        if (!memSame) fprintf(stderr, "  memory differs at %08x\n", badEa);
    }
    return steps;
}
#endif

u32 aot_run(Gekko* g, s32 budget) {
    if (!g_on) return 0;
    u32 total = 0;
    u64 t = g->cycles;
    aot_stop = 0;
    g_ni = (g->fpscr & FPSCR_NI) != 0;
    aot_stats.runs++;
#ifndef __3DS__
    static int chk = -1;
    if (chk < 0) { chk = getenv("AOT_CHECK") != NULL; g_noSlow = getenv("AOT_NOSLOW") != NULL; g_noFb = getenv("AOT_NOFB") != NULL; }
#endif
    while ((s32)total < budget) {
        const Slot* s = find(g->pc);
        if (!s) break;
        Func* F = &g_funcs[s->fi];
        if (F->valid != 1 && (F->valid == 0 || !check_func(F))) break;
        g->cycles = t + total;
        u32 n;
#ifndef __3DS__
        if (chk) n = checked_call(g, F, s->ei, budget - (s32)total);
        else
#endif
        n = F->f->fn(g, s->ei, budget - (s32)total);
        g->cycles = t;
        if (!n) break;
        total += n;
        aot_stats.calls++;
        aot_stats.insns += n;
        GkPriv* p = GKP(g);
        if (aot_stop || p->brk || p->stopReq) break;
    }
    return total;
}
