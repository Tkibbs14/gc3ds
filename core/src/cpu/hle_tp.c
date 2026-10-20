// Native stand-in for the hottest loop of Twilight Princess (GameCube USA, GZ2E01) on the 3DS: the cell and triangle
// scan of dBgWKCol::ShdwDraw (0x8007f9a4; name from the zeldaret/tp decompilation's symbol map), which projects
// real-time shadows onto the terrain collision. On the title scene the scan was ~33% of core 0's samples (2DS,
// 2026-10-11). It walks the collision grid cells inside the shadow's box (z, y, x), descends an octree to each cell's
// leaf, skips leaves the previous row already took, and runs through the leaf's triangle list against a visited bitset
// on the stack. Only an unvisited triangle needs the expensive part (polygon code, plane, vector maths, the shadow
// callback), which stays guest code.
//
// The stand-in is written from the guest instructions (0x8007fb48..0x8007fee8), one statement per instruction, and
// does exactly what they do to the GPRs, CR0, XER[CA] and the stack frame, counting each instruction it stands in for.
// It starts at the loop nest's entry (0x8007fb48) or at the next-triangle step (0x8007fea4, where guest code returns
// after a triangle), and stops where guest code takes over: a new triangle (0x8007fce4) or the end (0x8007fee8). A
// guest address it cannot read stops it at that instruction, for guest code to run. The code is recognised by a hash
// of those bytes (none are kept here). HLE_CHECK=1 (PC) runs every call natively and in the interpreter and compares.
#include "../ramlayout.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gekko_internal.h"
#include "hle.h"
#include "port.h"
#include "memo.h"
#include "gekko_softfp.h"
#include "hle_psops.h"
extern volatile unsigned int g_dmaGen;  // (gx/gx.h: DMA into guest RAM since start, and its last 64 destinations)
#define DMA_LOG_N 64  // (gx/gx.h DMA_LOG)
extern unsigned int g_dmaLog[DMA_LOG_N][2];
// The walk memo's recording (below): the collision data the walk read, as [g_shLo, g_shHi) (guest addresses; the
// octree's offsets only go forward, so a cell reads between the root and its deepest node or its list's end).
static bool g_shTrack;
static u32 g_shLo, g_shHi;
#define SH_TRACK(lo, hi) do { if (__builtin_expect(g_shTrack, 0)) { if ((lo) < g_shLo) g_shLo = (lo); if ((hi) > g_shHi) g_shHi = (hi); } } while (0)

#define PC_ENTRY 0x8007FB48u
#define PC_NEXT 0x8007FEA4u
#define PC_TRI 0x8007FCE4u
#define PC_EXIT 0x8007FEE8u
// The loop head, where the stand-in stops when the slice's budget is used: also a stand-in entry, so the walk goes on
// natively there in the next slice (compiled guest code walked the cells until the next triangle before).
#define PC_LOOP 0x8007FB70u
#define CODE_HASH 0x195639DFDBEA5654ull  // FNV-1a 64 of the guest bytes [PC_ENTRY, PC_EXIT); see code_hash

u64 g_hleRuns, g_hleInsns;
// Where the ShdwDraw stand-in is (jitsample.c counts it with every core-0 sample; '# shdw where 10s'): 0 not in it,
// 1 the walk replayed, 2 the walk run, 3 the triangle path's reads and copies, 4 its vector maths, 5 GetTriPla and the
// call, 6 the callback, 7 hle_run's own work. A byte store each: measurement that costs nothing measurable.
volatile u8 g_shWhere;
// ShdwDraw stand-in costs (device profile job 266: ~34% of core 0 in the later stretch sits in it): calls, triangles the
// fast loop skipped, guest instructions counted outside that loop, and ticks inside scan (g_profClock; 0 when off).
u64 g_shdwStats[4];
// The cell walk's shape: cells entered, tree levels descended, fast cells, cells with a new leaf (its triangle list
// scanned), empty leaves, triangles checked one by one (0x8007fcb0); fast cells rewound for an unvisited triangle, for
// the row's end, for anything else (an address), and to give back the scratch registers at a slice's end.
u64 g_shdwWalk[10];
void (*hle_shdw_begin_hook)(void);
void (*hle_shdw_end_hook)(void);
void (*hle_shdw_fast_begin_hook)(void);  // around the fast cells alone (--pmu-split fast / empty)
void (*hle_shdw_fast_end_hook)(void);
void (*hle_shdw_tri_begin_hook)(void), (*hle_shdw_tri_end_hook)(void);  // around each triangle path run (--pmu-split path)
void (*hle_shdw_cb_begin_hook)(void), (*hle_shdw_cb_end_hook)(void);    // around each native callback (--pmu-split cb)
static bool g_shCrK, g_shCa;  // the last scan: CR0 written, XER[CA] cleared (the walk memo below)
static u8* g_mem;
static u32 g_memSize;
static int g_known = -1;  // -1 not checked yet, 0 other code, 1 recognised
static bool g_on = true;   // hle_set_enabled

void hle_set_enabled(bool on) { g_on = on; }
bool hle_enabled(void) { return g_on; }

void hle_set_memory(u8* mem1, u32 size) { g_mem = mem1; g_memSize = size; g_known = -1; hle_gd_set_memory(mem1, size); }
static void tri_invalidate(u32 pa);
static void cb_invalidate(u32 pa);
void hle_invalidate(u32 pa) {
    if (pa >> 12 == (PC_ENTRY & 0x0FFFFFFFu) >> 12 || pa >> 12 == (PC_EXIT & 0x0FFFFFFFu) >> 12) g_known = -1;
    tri_invalidate(pa);
    cb_invalidate(pa);
    hle_gd_invalidate(pa);
    hle_thp_invalidate(pa);
    hle_ps_invalidate(pa);
}

static u64 code_hash(void) {
    u32 pa = PC_ENTRY & 0x0FFFFFFFu, n = PC_EXIT - PC_ENTRY;
    if (!g_mem || pa + n > g_memSize) return 0;
    u64 h = 0xCBF29CE484222325ull;
    for (u32 i = 0; i < n; i++) { h ^= ram_rd8(g_mem, pa + i); h *= 0x100000001B3ull; }  // (the guest's bytes, in order)
    return h;
}

static bool known(void) {
    if (g_known < 0) {
        u64 h = code_hash();
#ifndef __3DS__
        if (getenv("HLE_PRINT_HASH")) fprintf(stderr, "HLE hash of %08x..%08x: %016llx\n", PC_ENTRY, PC_EXIT, (unsigned long long)h);
#endif
        g_known = h != 0 && h == CODE_HASH;
    }
    return g_known == 1;
}

// Guest frame skip (experiment, --gfskip ADDR): the game's renderer (TP: mDoGph_Painter) returns at once on every other
// call while g_gfskipOn, so its logic runs every frame and the frame it draws, every second one: core 0's rendering
// work (56% of it in Ordon, job 374) and the GX thread's both halve. The skipped call does nothing at all; the screen
// keeps the previous frame. Not exact by design (owner, 2026-10-17: whatever makes the game run better).
u32 g_gfskipAddr;
volatile bool g_gfskipOn;
u64 g_gfskipStats[2];  // calls run, calls skipped
static u32 g_gfskipN;
u32 g_gfskipEvery = 2;  // --gfskip-n: the renderer runs one call in this many
// --gfskip-guard: guest bytes that must all be zero for a call to be skipped. TP's fade advances inside the renderer
// (mDoGph_gInf_c::calcFade): skipping calls during a fade left an area transition black for good (James, job 379), so
// with mFade (0x80450BE6) and the fade colour's alpha (0x80450597) as guards the renderer runs every frame then.
u32 g_gfskipGuard[4];
u64 g_gfskipGuarded;  // calls run because a guard byte was set
// --gfskip-ok: words reached through a pointer that must hold a value for a skip. TP's JUTFader (mDoGph_gInf_c::mFader,
// a pointer at 0x80450BC8) advances once per rendered frame while the scene change counts game frames: skipping left a
// fade half done at the area change and finished it late (James, job 382). Only with its mStatus (+4) 1 (faded in,
// idle) and mEStatus (+0x20) -1 (nothing pending) may a call be skipped.
GfskipOk g_gfskipOk[4];

static bool gfskip_guarded(void) {
    if (!g_mem) return true;
    for (u32 k = 0; k < 4; k++) {
        u32 a = g_gfskipGuard[k];
        if (a && a - 0x80000000u < 0x01800000u && ram_rd8(g_mem, a - 0x80000000u)) return true;
        const GfskipOk* o = &g_gfskipOk[k];
        if (!o->ptr) continue;
        if (o->ptr - 0x80000000u >= 0x01800000u - 4) return true;
        u32 p = ram_rd32(g_mem, o->ptr - 0x80000000u) + o->off;
        if (p - 0x80000000u >= 0x01800000u - 4 || (p & 3) || ram_rd32(g_mem, p - 0x80000000u) != o->val) return true;
    }
    return false;
}

// --panestat on (measurement): J2DPane::draw 0x802F67E0 (TP) is an entry hook that counts each call's pane class (the
// vtable at 0(r3)) and lets the guest code run: which pane types a native J2D walk would have to draw.
bool g_paneStat;
#define PANE_VT 32
u32 g_paneVt[PANE_VT], g_paneN[PANE_VT], g_paneOther;
static void pane_note(const Gekko* g) {
    u32 a = g->gpr[3] - 0x80000000u, vt = a < g_memSize - 4 && !(a & 3) ? ramp_rd32a(g_mem + a) : 0;
    for (u32 i = 0; i < PANE_VT; i++) {
        if (g_paneVt[i] == vt) { g_paneN[i]++; return; }
        if (!g_paneVt[i]) { g_paneVt[i] = vt; g_paneN[i] = 1; return; }
    }
    g_paneOther++;
}
bool hle_is_hook(Gekko* g, u32 pc) {
    (void)g;
    if (g_gfskipAddr && pc == g_gfskipAddr) return true;
    if (g_memoAddr && pc == g_memoAddr) return true;
    if (g_paneStat && pc == 0x802F67E0u) return true;
    if (port_is_hook(pc)) return true;
    if (!g_on) return false;
#ifndef __3DS__
    { static int bs = -1; if (bs < 0) bs = getenv("SHDW_BODYSTAT") != NULL;
      if (bs && (pc == 0x8007FCE4u || pc == 0x8007FE94u)) return true; }
#endif
    if (pc != PC_ENTRY && pc != PC_NEXT && pc != PC_LOOP) return hle_ps_is_hook(pc) || hle_gd_is_hook(pc) || hle_thp_is_hook(pc);
    return known();
}

#ifdef __3DS__
// ARM shifts by a register use its low byte and give 0 from 32 up: srw/slw (shift amount mod 64) without a branch.
static inline u32 shr(u32 v, u32 s) { u32 r; __asm__("lsr %0, %1, %2" : "=r"(r) : "r"(v), "r"(s & 63)); return r; }
static inline u32 shl(u32 v, u32 s) { u32 r; __asm__("lsl %0, %1, %2" : "=r"(r) : "r"(v), "r"(s & 63)); return r; }
#else
static inline u32 shr(u32 v, u32 s) { return (s & 0x20) ? 0 : v >> (s & 31); }  // srw
static inline u32 shl(u32 v, u32 s) { return (s & 0x20) ? 0 : v << (s & 31); }  // slw
#endif
// Guest memory in its layout (ramlayout.h); every caller has checked the address's alignment.
static inline u32 ld32(const u8* h) { return ramp_rd32a(h); }
static inline u32 ld16(const u8* h) { return ramp_rd16a(h); }
static inline u32 rotl(u32 v, u32 n) { return n ? (v << n) | (v >> (32 - n)) : v; }
static inline u32 cmps(u32 a, u32 b) { return (s32)a < (s32)b ? 8u : (s32)a > (s32)b ? 4u : 2u; }  // CR field LT GT EQ
static inline u32 cmpu(u32 a, u32 b) { return a < b ? 8u : a > b ? 4u : 2u; }

// Fast cells (at 0x8007fb70), in a function of their own: inside scan, with ~25 guest registers live as locals, the
// compiler spilled around every cell (~450 ARM instructions a cell, 2DS job 328). Here only what carries from one cell
// to the next is live (ShdwRun, loaded once and stored once a run of cells), and the grid's constants are in ShdwGrid.
// A cell computes into temporaries and commits only when it went through whole, so the cell a run stops at is left for
// the exact code from its head with nothing changed.
typedef struct { u32 r14, r16, r17, r20, r21, r22, r23, r24, r25, f2224, f2228, n, k, cells, levels, empty, fresh; } ShdwRun;
// Leaves whose triangles were all found visited, this ShdwDraw call: the visited bitset only gains bits within a call
// (the guest clears it in the prologue, before 0x8007fb48), so such a leaf stays so, and the walk reaches the same leaf
// again from other rows and slices. Tag: the call's generation << 12 | the leaf's triangle count (k < 4096).
typedef struct { u32 leaf, tag; } ShMemo;
#define SHMEMO 256
static ShMemo g_shMemo[SHMEMO];
static u32 g_shGen, g_shFrame;
typedef struct { const u8* m; const u8* vb; ShMemo* memo; u32 gen, lim4, lim2, vWords, r18, r19, r29, budget, cur, lev, s52, s48, f2212, f2216, f2220; } ShdwGrid;
enum { SHDW_BUDGET = 1, SHDW_TRI, SHDW_ROW, SHDW_ADDR };

static __attribute__((noinline)) u32 shdw_fast(ShdwRun* s, const ShdwGrid* gr) {
    // Live across cells: only the position (r17), the instruction count and the counters. The rest of what carries
    // (r14, r16, r20-r25, the frame's step words) stays in *s and is read and written at a cell's commit alone; the
    // grid's constants are read from *gr where they are needed. (Held in locals, GCC spilled ~70 loads a cell: job 331.)
    const u8* const m = gr->m;
    const u32 lim4 = gr->lim4, r18 = gr->r18, r19 = gr->r19, hLev = gr->lev;
    // The first level's offset ORs r17's bits with r18's and r19's, which do not change within a run.
    const u32 yz0 = shl(shr(r19, hLev), gr->s52) | shl(shr(r18, hLev), gr->s48);
    u32 r17 = s->r17, n = s->n, cells = 0, kAll = 0, why;
#ifndef __3DS__
    u32 levels = 0, empty = 0, fresh = 0;
#endif
    for (;;) {
        if (n >= gr->budget) { why = SHDW_BUDGET; break; }
        u32 a, w, lev = hLev, cur = gr->cur, lv = 0;
        u32 off = (shr(r17, lev) | yz0) << 2;
        for (;; lv++) {  // the descent (or, lwzx, cmpi, bc at 0x8007fbcc; 0x8007fba8 .. 0x8007fbc8 for each level down)
            a = cur + off - 0x80000000u;
            if (__builtin_expect((a > lim4) | (a & 3), 0)) { why = SHDW_ADDR; goto stop; }
            w = ld32(m + a);
            if ((s32)w < 0) break;
            cur += w;
            lev -= 1;
            off = ((shr(r17, lev) & 1u) | ((shr(r18, lev) & 1u) << 1) | ((shr(r19, lev) & 1u) << 2)) << 2;
        }
        const u32 aNode = a;  // (the deepest node read: the walk memo's range)
        const u32 leaf = cur + (w & 0x7FFFFFFFu), sz = shl(1, lev), mask = sz - 1, step = sz - (r17 & mask);
        // (Any reason to stop takes the cell to the exact code from its head, so the checks' order is free: the row's
        // end first, before its triangles are scanned.)
        if (r17 + step > gr->r29) { why = SHDW_ROW; goto stop; }  // the row's end: the exact code writes the row words
        a = leaf + 2 - 0x80000000u;
        if (__builtin_expect((a > gr->lim2) | (a & 1), 0)) { why = SHDW_ADDR; goto stop; }
        const u32 cnt = ld16(m + a);
        // 0x8007fb70 .. the descent, 0x8007fbd8 .. 0x8007fc28, the first of the previous row's leaves, 0x8007feb0's 4.
        u32 nn = n + 45 + 13 * lv, p = leaf, k = 0;
        if (leaf == gr->f2220) {
        } else if (leaf == gr->f2216) {
            nn += 3;
        } else if (leaf == gr->f2212) {
            nn += 7;
        } else {  // its triangles: all visited (13 each) up to the list's end (3), else the exact code takes the cell
            ShMemo* const me = &gr->memo[((leaf >> 1) ^ (leaf >> 9)) & (SHMEMO - 1)];
            if (me->leaf == leaf && (me->tag >> 12) == gr->gen) {  // found all visited before, this call
                k = me->tag & 0xFFFu;
                p = leaf + 2 * k + 2;
            } else {
                const u8* const vb = gr->vb;
                const u32 vWords = gr->vWords, lim2 = gr->lim2;
                for (;;) {
                    a = p + 2 - 0x80000000u;
                    if (__builtin_expect((a > lim2) | (a & 1), 0)) { why = SHDW_ADDR; goto stop; }
                    u32 tt = ld16(m + a);
                    p += 2;
                    if (!tt) break;
                    if ((tt >> 5) >= vWords || !(ram_rd8(vb, (tt >> 3) ^ 3) & (1u << (tt & 7)))) { why = SHDW_TRI; goto stop; }
                    k++;
                }
                if (k < 4096) { me->leaf = leaf; me->tag = gr->gen << 12 | k; }
            }
            nn += 6 + 13 * k + 3;
#ifndef __3DS__
            fresh++;
#endif
        }
        // The whole cell went through: commit it (the guest's order: r14, 2228, 2224, the leaf history, r16).
        const u32 r4p = sz - (r18 & mask), t = sz - (r19 & mask);
        if ((s32)t < (s32)s->r14) { s->r14 = t; nn++; }
        s->f2228 = step;
        if ((s32)r4p < (s32)s->f2224) { s->f2224 = r4p; nn++; }
        if (cnt) {
            nn += 2;
            if ((s32)r4p > (s32)s->r23) {
                nn += 2;
                if ((s32)r4p <= (s32)s->r24) { s->r23 = r4p; s->r20 = leaf; nn += 2; }
                else if (nn += 2, (s32)r4p <= (s32)s->r25) { s->r23 = s->r24; s->r24 = r4p; s->r20 = s->r21; s->r21 = leaf; nn += 5; }
                else { s->r23 = s->r24; s->r24 = s->r25; s->r25 = r4p; s->r20 = s->r21; s->r21 = s->r22; s->r22 = leaf; nn += 7; }
            }
        }
#ifndef __3DS__
        else empty++;
        levels += lv;
#endif
        s->r16 = p;
        SH_TRACK(gr->cur, (aNode + 0x80000000u > p ? aNode + 0x80000000u : p) + 4);
        r17 += step;
        n = nn;
        kAll += k; cells++;
    }
stop:
    s->r17 = r17; s->n = n; s->k = kAll; s->cells = cells;
#ifndef __3DS__
    s->levels = levels; s->empty = empty; s->fresh = fresh;
#else
    s->levels = s->empty = s->fresh = 0;  // (not counted here: three more live values a cell)
#endif
    return why;
}

static u32 scan(Gekko* g, u32 budget) {
    // The guest registers as locals (not an array): the compiler keeps the hot ones in ARM registers; the array
    // version spent ~7 cycles per guest instruction (2DS job 267: 38% of core 0 in the later stretch).
    u32 r0 = g->gpr[0], r1 = g->gpr[1], r3 = g->gpr[3], r4 = g->gpr[4], r5 = g->gpr[5], r6 = g->gpr[6], r7 = g->gpr[7], r8 = g->gpr[8], r14 = g->gpr[14], r15 = g->gpr[15], r16 = g->gpr[16], r17 = g->gpr[17], r18 = g->gpr[18], r19 = g->gpr[19], r20 = g->gpr[20], r21 = g->gpr[21], r22 = g->gpr[22], r23 = g->gpr[23], r24 = g->gpr[24], r25 = g->gpr[25], r26 = g->gpr[26], r27 = g->gpr[27], r28 = g->gpr[28], r29 = g->gpr[29], r30 = g->gpr[30];
    u32 pc = g->pc, n = 0, crA = 0, crB = 0, crK = 0, wCell = 0, wLevel = 0, wFast = 0, wNew = 0, wEmpty = 0, wTri = 0, wFTri = 0, wFRow = 0, wFail = 0, wBud = 0;  // CR0: the last compare's operands, kind 1 signed / 2 unsigned
    bool caClear = false;
    // Guest memory: only MEM1 through the cached mapping (0x80000000) is read here; any other address (including the
    // uncached mirror) stops the stand-in before that instruction, for guest code to run it. The base and limits are
    // locals: through the globals, every store into guest memory made the compiler reload them.
    u8* const m = g_mem;
    const u32 size = g_memSize, lim4 = size - 4, lim2 = size - 2;
#define RD32(dst, addr, at) do { u32 a_ = (addr) - 0x80000000u; if (__builtin_expect((a_ > lim4) | (a_ & 3), 0)) { pc = (at); goto out; } (dst) = ld32(m + a_); } while (0)
#define RD16(dst, addr, at) do { u32 a_ = (addr) - 0x80000000u; if (__builtin_expect((a_ > lim2) | (a_ & 1), 0)) { pc = (at); goto out; } (dst) = ld16(m + a_); } while (0)
#define WR32(addr, v, at) do { u32 a_ = (addr) - 0x80000000u; if (__builtin_expect((a_ > lim4) | (a_ & 3), 0)) { pc = (at); goto out; } ramp_wr32a(m + a_, (v)); } while (0)
#define CRS(a, b) do { crA = (a); crB = (b); crK = 1; } while (0)
#define CRU(a, b) do { crA = (a); crB = (b); crK = 2; } while (0)
    // Fast cells (at 0x8007fb70, shdw_fast): the frame (words 2212-2228 and the visited bitset at +156) through one host
    // pointer. The grid's header (r15's node: root, top level, shifts) is read once a call (nothing here writes it).
    const u32 fa = r1 - 0x80000000u;
    const bool fastOk = size >= 2232 && fa <= size - 2232 && !(fa & 3);
    u8* const fr = m + (fastOk ? fa : 0);
    bool hOk = false;
    // A new ShdwDraw call (or another frame): a new generation for the leaf memo.
    if (pc == PC_ENTRY || r1 != g_shFrame) {
        g_shFrame = r1;
        if (++g_shGen >= (1u << 20)) { g_shGen = 1; memset(g_shMemo, 0, sizeof g_shMemo); }
    }
    ShdwGrid gr = { m, fr + 156, g_shMemo, g_shGen, lim4, lim2, fastOk ? (size - fa - 156) >> 2 : 0, 0, 0, 0, budget, 0, 0, 0, 0, 0, 0, 0 };
#define FBAD4(a) (__builtin_expect(((a) > lim4) | ((a) & 3), 0))
#define FWR32(off, v) ramp_wr32a(fr + (off), (v))
    if (pc == PC_NEXT) goto L_fea4;
    if (pc == PC_LOOP) goto L_fb70;  // (r0, r3-r8, CR0 and XER[CA] are written there before they are read)
    if (pc != PC_ENTRY) return 0;
L_fb48:
    r3 = 0x000F0000u; n++;                         // addis r3,0,15
    r14 = r3 + 16960; n++;                       // addi r14,r3,16960
    r18 = r28; n++;                              // or r18,r28,r28
L_fb54:
    r3 = 0x000F0000u; n++;                         // addis r3,0,15
    r0 = r3 + 16960; n++;                        // addi r0,r3,16960
    WR32(r1 + 2224, r0, 0x8007FB5Cu); n++;       // stw r0,2224(r1)
    r25 = 0; n++;                                  // addi r25,0,0
    r24 = 0; n++;                                  // addi r24,0,0
    r23 = 0; n++;                                  // addi r23,0,0
    r17 = r30; n++;                              // or r17,r30,r30
L_fb70:
    if (n >= budget) {
        // Slice used up: guest code goes on from the loop head, where the scratch registers (r0, r3-r8), CR0 and XER[CA]
        // are all written before they are read, so the fast cells' leaving them as they were is dead state.
        pc = 0x8007FB70u;
        goto out;
    }
    if (fastOk) {
        if (!hOk) {
            u32 a, node;
            a = r15 + 24 - 0x80000000u; if (FBAD4(a)) goto L_ffail; node = ld32(m + a);
            a = node + 12 - 0x80000000u; if (FBAD4(a)) goto L_ffail; gr.cur = ld32(m + a);
            a = node + 44 - 0x80000000u; if (FBAD4(a)) goto L_ffail; gr.lev = ld32(m + a);
            a = node + 52 - 0x80000000u; if (FBAD4(a)) goto L_ffail; gr.s52 = ld32(m + a);
            a = node + 48 - 0x80000000u; if (FBAD4(a)) goto L_ffail; gr.s48 = ld32(m + a);
            hOk = true;
        }
        // The previous row's leaves and r18 / r19 change only in the exact code (at a row's end).
        gr.r18 = r18; gr.r19 = r19; gr.r29 = r29;
        gr.f2212 = ld32(fr + 2212); gr.f2216 = ld32(fr + 2216); gr.f2220 = ld32(fr + 2220);
        ShdwRun s = { r14, r16, r17, r20, r21, r22, r23, r24, r25, ld32(fr + 2224), ld32(fr + 2228), n, 0, 0, 0, 0, 0 };
        if (hle_shdw_fast_begin_hook) hle_shdw_fast_begin_hook();
        u32 why = shdw_fast(&s, &gr);
        if (hle_shdw_fast_end_hook) hle_shdw_fast_end_hook();
        if (s.cells) {
            r14 = s.r14; r16 = s.r16; r17 = s.r17; r20 = s.r20; r21 = s.r21; r22 = s.r22; r23 = s.r23; r24 = s.r24; r25 = s.r25;
            FWR32(2224, s.f2224); FWR32(2228, s.f2228);
            n = s.n;
            if (s.k) { caClear = true; g_shdwStats[1] += s.k; }
            wCell += s.cells; wFast += s.cells; wLevel += s.levels; wEmpty += s.empty; wNew += s.fresh;
        }
        if (why == SHDW_BUDGET) { pc = 0x8007FB70u; goto out; }  // (as above)
        if (why == SHDW_TRI) wFTri++;
        else if (why == SHDW_ROW) wFRow++;
        goto L_ffail;
    }
    goto L_fslow;
L_ffail:
    wFail++;
L_fslow:
    wCell++;
    RD32(r8, r15 + 24, 0x8007FB70u); n++;        // lwz r8,24(r15)
    RD32(r3, r8 + 12, 0x8007FB74u); n++;         // lwz r3,12(r8)
    RD32(r4, r8 + 44, 0x8007FB78u); n++;         // lwz r4,44(r8)
    r7 = shr(r17, r4); n++;                    // srw r7,r17,r4
    r5 = shr(r19, r4); n++;                    // srw r5,r19,r4
    RD32(r0, r8 + 52, 0x8007FB84u); n++;         // lwz r0,52(r8)
    r6 = shl(r5, r0); n++;                     // slw r6,r5,r0
    r5 = shr(r18, r4); n++;                    // srw r5,r18,r4
    RD32(r0, r8 + 48, 0x8007FB90u); n++;         // lwz r0,48(r8)
    r0 = shl(r5, r0); n++;                     // slw r0,r5,r0
    r0 = r6 | r0; n++;                         // or r0,r6,r0
    r0 = r7 | r0; n++;                         // or r0,r7,r0
    r5 = r0 << 2; n++;                           // rlwinm r5,r0,2,0,29
    n++;                                             // b 0x8007fbcc
    goto L_fbcc;
L_fba8:
    wLevel++;
    r3 = r0 + r5; n++;                         // add r3,r0,r5
    r4 = r4 - 1; n++;                            // addi r4,r4,-1
    r6 = shr(r17, r4); n++;                    // srw r6,r17,r4
    r5 = shr(r19, r4); n++;                    // srw r5,r19,r4
    r0 = shr(r18, r4); n++;                    // srw r0,r18,r4
    r0 = rotl(r0, 1) & 2u; n++;                  // rlwinm r0,r0,1,30,30
    r0 = (rotl(r5, 2) & 4u) | (r0 & ~4u); n++; // rlwimi r0,r5,2,29,29
    r0 = (r6 & 1u) | (r0 & ~1u); n++;          // rlwimi r0,r6,0,31,31
    r5 = r0 << 2; n++;                           // rlwinm r5,r0,2,0,29
L_fbcc:
    r0 = r3; n++;                                // or r0,r3,r3
    RD32(r5, r3 + r5, 0x8007FBD0u); n++;       // lwzx r5,r3,r5
    SH_TRACK(r3, r3 + r5 + 4);
    CRS(r5, 0); n++;                          // cmpi cr0,0,r5,0
    n++;                                             // bc 4,lt,0x8007fba8
    if ((s32)r5 >= 0) goto L_fba8;
    r0 = r5 & 0x7FFFFFFFu; n++;                  // rlwinm r0,r5,0,1,31
    r16 = r3 + r0; n++;                        // add r16,r3,r0
    r0 = 1; n++;                                   // addi r0,0,1
    r3 = shl(r0, r4); n++;                     // slw r3,r0,r4
    r5 = r3 - 1; n++;                            // addi r5,r3,-1
    r0 = r17 & r5; n++;                        // and r0,r17,r5
    r0 = r3 - r0; n++;                         // subf r0,r0,r3
    WR32(r1 + 2228, r0, 0x8007FBF8u); n++;       // stw r0,2228(r1)
    r0 = r18 & r5; n++;                        // and r0,r18,r5
    r4 = r3 - r0; n++;                         // subf r4,r0,r3
    r0 = r19 & r5; n++;                        // and r0,r19,r5
    r0 = r3 - r0; n++;                         // subf r0,r0,r3
    CRS(r0, r14); n++;                      // cmp cr0,0,r0,r14
    n++;                                             // bc 4,lt,0x8007fc18
    if ((s32)r0 < (s32)r14) { r14 = r0; n++; }  // or r14,r0,r0
    RD32(r0, r1 + 2224, 0x8007FC18u); n++;       // lwz r0,2224(r1)
    CRS(r4, r0); n++;                       // cmp cr0,0,r4,r0
    n++;                                             // bc 4,lt,0x8007fc28
    if ((s32)r4 < (s32)r0) { WR32(r1 + 2224, r4, 0x8007FC24u); n++; }  // stw r4,2224(r1)
    RD16(r0, r16 + 2, 0x8007FC28u); n++;         // lhz r0,2(r16)
    SH_TRACK(r16, r16 + 4);
    CRU(r0, 0); n++;                          // cmpli cr0,0,r0,0
    n++;                                             // bc 12,eq,0x8007fc84
    if (r0 == 0) { wEmpty++; goto L_fc84; }
    CRS(r4, r23); n++;                      // cmp cr0,0,r4,r23
    n++;                                             // bc 4,gt,0x8007fc84
    if ((s32)r4 <= (s32)r23) goto L_fc84;
    CRS(r4, r24); n++;                      // cmp cr0,0,r4,r24
    n++;                                             // bc 4,gt,0x8007fc7c
    if ((s32)r4 <= (s32)r24) goto L_fc7c;
    CRS(r4, r25); n++;                      // cmp cr0,0,r4,r25
    n++;                                             // bc 4,gt,0x8007fc68
    if ((s32)r4 <= (s32)r25) goto L_fc68;
    r23 = r24; n++;                              // or r23,r24,r24
    r24 = r25; n++;                              // or r24,r25,r25
    r25 = r4; n++;                               // or r25,r4,r4
    r20 = r21; n++;                              // or r20,r21,r21
    r21 = r22; n++;                              // or r21,r22,r22
    r22 = r16; n++;                              // or r22,r16,r16
    n++;                                             // b 0x8007fc84
    goto L_fc84;
L_fc68:
    r23 = r24; n++;                              // or r23,r24,r24
    r24 = r4; n++;                               // or r24,r4,r4
    r20 = r21; n++;                              // or r20,r21,r21
    r21 = r16; n++;                              // or r21,r16,r16
    n++;                                             // b 0x8007fc84
    goto L_fc84;
L_fc7c:
    r23 = r4; n++;                               // or r23,r4,r4
    r20 = r16; n++;                              // or r20,r16,r16
L_fc84:
    RD32(r0, r1 + 2220, 0x8007FC84u); n++;       // lwz r0,2220(r1)
    CRU(r16, r0); n++;                      // cmpl cr0,0,r16,r0
    n++;                                             // bc 12,eq,0x8007feb0
    if (r16 == r0) goto L_feb0;
    RD32(r0, r1 + 2216, 0x8007FC90u); n++;       // lwz r0,2216(r1)
    CRU(r16, r0); n++;                      // cmpl cr0,0,r16,r0
    n++;                                             // bc 12,eq,0x8007feb0
    if (r16 == r0) goto L_feb0;
    RD32(r0, r1 + 2212, 0x8007FC9Cu); n++;       // lwz r0,2212(r1)
    CRU(r16, r0); n++;                      // cmpl cr0,0,r16,r0
    n++;                                             // bc 4,eq,0x8007fea4
    if (r16 != r0) { wNew++; goto L_fea4; }
    n++;                                             // b 0x8007feb0
    goto L_feb0;
L_fea4:
    {
        // Triangles the visited bitset already holds, in a tight loop: each is 13 guest instructions (lhzu, cmpli, bc,
        // then 0x8007fcb0..0x8007fcd0 back here) that leave only r16 and the count changed and r0 / r3 / r4 / r5 / CR0 /
        // XER[CA] as the last one set them, which is rebuilt below. The list's end, an unvisited triangle and any address
        // the loop cannot read go through the instruction-by-instruction code from where it stopped.
        // The visited bit of triangle tt is bit tt&31 of the big-endian word tt>>5: byte (tt>>3)^3, bit tt&7.
        u32 p = r16, skipped = 0, va = r1 + 156 - 0x80000000u;
        if (va <= lim4 && !(va & 3)) {
            const u8* vb = m + va;
            const u32 vWords = (size - va) >> 2;  // bitset words inside MEM1
            for (;;) {
                u32 a = p + 2 - 0x80000000u, tt;
                if ((a > lim2) | (a & 1)) break;
                tt = ld16(m + a);
                if (!tt || (tt >> 5) >= vWords || !(ram_rd8(vb, (tt >> 3) ^ 3) & (1u << (tt & 7)))) break;
                skipped++; p += 2;
            }
            if (skipped) {
                u32 lastT = ld16(m + (p - 0x80000000u));
                g_shdwStats[1] += skipped;
                r16 = p;
                n += 13 * skipped;
                r3 = ramp_rd32(vb + ((lastT >> 5) << 2));  // (vb's alignment is the guest's)
                r4 = (lastT >> 5) << 2;
                r5 = 1u << (lastT & 31);
                r0 = r3 & r5;
                CRS(r0, 0);
                caClear = true;
            }
        }
    }
    RD16(r0, r16 + 2, 0x8007FEA4u);              // lhzu r0,2(r16)
    SH_TRACK(r16, r16 + 4);
    r16 += 2; n++;
    CRU(r0, 0); n++;                          // cmpli cr0,0,r0,0
    n++;                                             // bc 4,eq,0x8007fcb0
    if (r0 != 0) goto L_fcb0;
L_feb0:
    RD32(r0, r1 + 2228, 0x8007FEB0u); n++;       // lwz r0,2228(r1)
    r17 = r17 + r0; n++;                       // add r17,r17,r0
    CRU(r17, r29); n++;                     // cmpl cr0,0,r17,r29
    n++;                                             // bc 4,gt,0x8007fb70
    if (r17 <= r29) goto L_fb70;
    WR32(r1 + 2220, r22, 0x8007FEC0u); n++;      // stw r22,2220(r1)
    WR32(r1 + 2216, r21, 0x8007FEC4u); n++;      // stw r21,2216(r1)
    WR32(r1 + 2212, r20, 0x8007FEC8u); n++;      // stw r20,2212(r1)
    RD32(r0, r1 + 2224, 0x8007FECCu); n++;       // lwz r0,2224(r1)
    r18 = r18 + r0; n++;                       // add r18,r18,r0
    CRU(r18, r27); n++;                     // cmpl cr0,0,r18,r27
    n++;                                             // bc 4,gt,0x8007fb54
    if (r18 <= r27) goto L_fb54;
    r19 = r19 + r14; n++;                      // add r19,r19,r14
    CRU(r19, r26); n++;                     // cmpl cr0,0,r19,r26
    n++;                                             // bc 4,gt,0x8007fb48
    if (r19 <= r26) goto L_fb48;
    pc = PC_EXIT;
    goto out;
L_fcb0:
    wTri++;
    r3 = 1; n++;                                   // addi r3,0,1
    RD16(r4, r16, 0x8007FCB4u); n++;             // lhz r4,0(r16)
    SH_TRACK(r16, r16 + 4);
    r0 = r4 & 31u; n++;                          // rlwinm r0,r4,0,27,31
    r5 = shl(r3, r0); n++;                     // slw r5,r3,r0
    r0 = (u32)((s32)r4 >> 5); caClear = true; n++;  // srawi r0,r4,5 (r4 is a halfword: CA = 0)
    r4 = r0 << 2; n++;                           // rlwinm r4,r0,2,0,29
    r3 = r1 + 156; n++;                          // addi r3,r1,156
    RD32(r3, r3 + r4, 0x8007FCCCu); n++;       // lwzx r3,r3,r4
    r0 = r3 & r5; CRS(r0, 0); n++;      // and. r0,r3,r5
    n++;                                             // bc 4,eq,0x8007fea4
    if (r0 != 0) goto L_fea4;
    r0 = r3 | r5; n++;                         // or r0,r3,r5
    r3 = r1 + 156; n++;                          // addi r3,r1,156
    WR32(r3 + r4, r0, 0x8007FCE0u); n++;       // stwx r0,r3,r4
    pc = PC_TRI;
out:
    g_shdwWalk[0] += wCell; g_shdwWalk[1] += wLevel; g_shdwWalk[2] += wFast; g_shdwWalk[3] += wNew; g_shdwWalk[4] += wEmpty;
    g_shdwWalk[5] += wTri; g_shdwWalk[6] += wFTri; g_shdwWalk[7] += wFRow; g_shdwWalk[8] += wFail - wFTri - wFRow; g_shdwWalk[9] += wBud;
#undef RD32
#undef RD16
#undef WR32
#undef CRS
#undef CRU
#undef FBAD4
#undef FWR32
    g_shCrK = crK != 0; g_shCa = caClear;
    if (!n) return 0;
    g->gpr[0] = r0; g->gpr[1] = r1; g->gpr[3] = r3; g->gpr[4] = r4; g->gpr[5] = r5; g->gpr[6] = r6; g->gpr[7] = r7; g->gpr[8] = r8; g->gpr[14] = r14; g->gpr[15] = r15; g->gpr[16] = r16; g->gpr[17] = r17; g->gpr[18] = r18; g->gpr[19] = r19; g->gpr[20] = r20; g->gpr[21] = r21; g->gpr[22] = r22; g->gpr[23] = r23; g->gpr[24] = r24; g->gpr[25] = r25; g->gpr[26] = r26; g->gpr[27] = r27; g->gpr[28] = r28; g->gpr[29] = r29; g->gpr[30] = r30;
    if (crK) g->cr = (g->cr & 0x0FFFFFFFu) | (((crK == 1 ? cmps(crA, crB) : cmpu(crA, crB)) | ((g->xer & XER_SO) ? 1u : 0u)) << 28);
    if (caClear) g->xer &= ~XER_CA;
    g->pc = pc;
    return n;
}

#ifndef __3DS__
// HLE_CHECK=1: every call also runs in the interpreter from the same state, to the stand-in's stop, and the two are
// compared (GPRs, CR, XER, PC, instruction count, the 2304-byte stack frame); the interpreter's result stands.
static struct { u64 calls, bad; } g_chk;
static void chk_report(void) {
    fprintf(stderr, "HLE_CHECK: %llu stand-in calls compared with the interpreter, %llu differed\n",
            (unsigned long long)g_chk.calls, (unsigned long long)g_chk.bad);
}
static u32 checked(Gekko* g, u32 budget) {
    enum { FRAME = 2304 };
    u32 fpa = g->gpr[1] & 0x0FFFFFFFu;
    if (fpa + FRAME > g_memSize) return scan(g, budget);
    static u8 frame0[FRAME], frameN[FRAME];
    Gekko g0 = *g;
    memcpy(frame0, g_mem + fpa, FRAME);
    u32 n = scan(g, budget);
    if (!n) return 0;
    Gekko gn = *g;
    memcpy(frameN, g_mem + fpa, FRAME);
    *g = g0;
    memcpy(g_mem + fpa, frame0, FRAME);
    u32 steps = 0;
    while (steps < n) {  // exactly as many instructions as the stand-in counted, then compare where they ended
        u32 pa = g->pc & 0x0FFFFFFFu, inst;
        if (pa + 4 > g_memSize) break;
        inst = ram_rd32(g_mem, pa);
        bool stop;
        g->pc = gekko_interp_one(g, g->pc, inst, &stop);
        steps++;
    }
    if (!g_chk.calls++) atexit(chk_report);
    if (gn.pc == 0x8007FB70u && g->pc == gn.pc) {
        // A stop at the loop head after a fast cell: r0, r3-r8, CR0 and XER[CA] are written there before they are read
        // (dead state, as the fast cell leaves them): compared as equal.
        static const int dead[] = { 0, 3, 4, 5, 6, 7, 8 };
        for (u32 k = 0; k < sizeof dead / sizeof dead[0]; k++) gn.gpr[dead[k]] = g->gpr[dead[k]];
        gn.cr = (gn.cr & 0x0FFFFFFFu) | (g->cr & 0xF0000000u);
        gn.xer = (gn.xer & ~XER_CA) | (g->xer & XER_CA);
    }
    bool same = steps == n && g->pc == gn.pc && !memcmp(g->gpr, gn.gpr, sizeof g->gpr) && g->cr == gn.cr &&
                g->xer == gn.xer && !memcmp(g_mem + fpa, frameN, FRAME);
    if (!same && g_chk.bad++ < 20) {
        fprintf(stderr, "HLE_CHECK call %llu from %08x: native %u insns to %08x, interpreter %u to %08x; cr %08x/%08x xer %08x/%08x\n",
                (unsigned long long)g_chk.calls, g0.pc, n, gn.pc, steps, g->pc, gn.cr, g->cr, gn.xer, g->xer);
        for (int k = 0; k < 32; k++)
            if (g->gpr[k] != gn.gpr[k]) fprintf(stderr, "  r%d native %08x interpreter %08x\n", k, gn.gpr[k], g->gpr[k]);
        for (u32 k = 0; k < FRAME; k++)
            if (g_mem[fpa + k] != frameN[k]) { fprintf(stderr, "  frame +%u differs\n", k); break; }
    }
    return steps;
}
#endif

#ifndef __3DS__
// SHDW_KEYSTAT=1 (PC, temporary measurement): how often a ShdwDraw walk repeats one seen before. Key: the GPRs the
// walk reads at its entry; outcome: the triangles it hands to guest code (r0 index, r16 list pointer) and its end.
#define SKN (1u << 18)
static struct { u64 key, out; } *g_skT;
static struct { int on; bool live; u64 key, out, calls, done, hits, same, diff; } g_sk = { -1 };
static u64 sk_mix(u64 h, u32 v) { return (h ^ v) * 0x100000001B3ull; }
static u64 g_skEntries;
static void sk_report(void) {
    fprintf(stderr, "SHDW_KEYSTAT walk: cells %llu fast %llu new leaf %llu triangles %llu, stand-in entries %llu\n",
            (unsigned long long)g_shdwWalk[0], (unsigned long long)g_shdwWalk[2], (unsigned long long)g_shdwWalk[3],
            (unsigned long long)g_shdwWalk[5], (unsigned long long)g_skEntries);
    fprintf(stderr, "SHDW_KEYSTAT: calls %llu finished %llu; key seen before %llu (%.1f%%): same outcome %llu, different %llu\n",
            (unsigned long long)g_sk.calls, (unsigned long long)g_sk.done, (unsigned long long)g_sk.hits,
            g_sk.done ? 100.0 * g_sk.hits / g_sk.done : 0.0, (unsigned long long)g_sk.same, (unsigned long long)g_sk.diff);
}
static void sk_before(const Gekko* g) {
    if (g_sk.on < 0) { g_sk.on = getenv("SHDW_KEYSTAT") != NULL; if (g_sk.on) { g_skT = calloc(SKN, sizeof *g_skT); atexit(sk_report); } }
    if (!g_sk.on) return;
    g_skEntries++;
    if (g->pc != PC_ENTRY) return;
    static const u8 kRegs[] = { 2, 13, 15, 16, 19, 20, 21, 22, 26, 27, 28, 29, 30, 31 };
    u64 h = 0xCBF29CE484222325ull;
    for (u32 i = 0; i < sizeof kRegs; i++) h = sk_mix(h, g->gpr[kRegs[i]]);
    g_sk.key = h | 1;
    g_sk.out = 0xCBF29CE484222325ull;
    g_sk.live = true;
    g_sk.calls++;
}
static void sk_after(const Gekko* g) {
    if (!g_sk.on || !g_sk.live) return;
    if (g->pc == PC_TRI) { g_sk.out = sk_mix(sk_mix(g_sk.out, g->gpr[0]), g->gpr[16]); return; }
    if (g->pc != PC_EXIT) return;
    g_sk.live = false;
    g_sk.done++;
    u32 s = (u32)(g_sk.key >> 40) & (SKN - 1);
    for (u32 k = 0; k < 64; k++, s = (s + 1) & (SKN - 1)) {
        if (g_skT[s].key == g_sk.key) { g_sk.hits++; if (g_skT[s].out == g_sk.out) g_sk.same++; else g_sk.diff++; g_skT[s].out = g_sk.out; return; }
        if (!g_skT[s].key) { g_skT[s].key = g_sk.key; g_skT[s].out = g_sk.out; return; }
    }
}
#endif


// ---- The walk memo (--shdwmemo, on by default on the 3DS; GC3DS_SHDWMEMO=1 on the PC) ----
// In the title's later stretch the game makes ~1.3 ShdwDraw calls a frame (four distinct ones in turn; the long one
// ~6,500 segments, ~30,000 cells), and 98% of calls repeat a previous call's walk exactly (PC, SHDW_KEYSTAT: 369 of 377
// calls with a key seen before, all with the same outcome). The walk is a function of the registers it reads, its frame
// words (2212-2228, the visited bitset at +156, which guest code zeroes before the call) and the collision data. A call
// is recorded segment by segment (from the call's start, a triangle's return or the loop head to the next triangle, the
// loop head or the end): the
// segment's inputs (the PC, r1, r14-r30, the frame words 2212-2228) and everything the stand-in leaves (the registers it
// writes, CR0, XER[CA], the PC, the frame words and the one bitset word it set, the instruction count). A later call
// whose start has the same inputs replays the record while each segment's inputs are equal, and runs the walk from
// wherever they stop matching. A record is dropped when a DMA into guest RAM since it was made overlaps the collision
// data its walk read (shm_valid). r0, r3-r8 and CR0 are written before they are read: guest code leaves them differing.
// A stop at the slice's budget (the loop head, itself a stand-in entry) ends a segment as a triangle does. Replay runs a
// segment whole even past the slice's budget, by at most SHM_OVER guest instructions (the stops land at different places
// each frame: replay would otherwise end at the first; on the 2DS that ended most replays of the long call, job 615), so
// an event can come that much late: timing drift, owner-approved 2026-10-15 where the 2DS gains. Every segment's result
// is the walk's (SHDW_MEMOCHECK). A stop at an address the stand-in cannot read is a hole: guest code runs there.
// 2DS (jobs 617-618): the later stretch 19.09 -> 21.81%, 24% more pictures; Ordon and the heavy area even.
#define SHM_RECS 5
#define SHM_WORDS (192u << 10)  // a record's stream (the long call in the later stretch: ~8,700 segments, ~100k words)
#define SHM_IN 24               // inputs: the PC, r1, r14-r30, the frame words 2212-2228
// A replayed segment may pass the slice's budget by this many guest instructions (the event then comes that much late;
// VI fields are scheduled from the last one, so unbounded overruns stretched every field: 1.7G cycles over 400, PC).
#define SHM_OVER 32768u
// A record is a stream of segments, each:
//   w0: the written registers that changed (24 bits, kShOut order) | crK << 24 | CR0 << 25 (4 bits) | ca << 29 |
//       hole << 30 | inputs stored << 31
//   w1: the instruction count; w2: the PC it stops at; w3: the frame words that changed (5 bits) | the bitset offset << 16
//   then the inputs (24 words) when stored, the changed registers, the changed frame words, the bitset word (at a
//   triangle). Inputs are stored only where they differ from what the previous segments left (after a hole, or when
//   guest code changed them): otherwise they are the running state's. A hole has no outputs.
typedef struct { u64 key; u32 gen, nSeg, nw, use, lo, hi, hdr, node; bool done, own; u32* w; u32 in0[SHM_IN]; } ShRec;
// Memory: a call is recorded into one scratch stream (SHM_WORDS) and copied out at its exact size when it finishes (the
// long call in the later stretch ~80k words, the others a few thousand): five full-size records held 3.75 MB of linear
// memory and left 1.6 MB free (job 618).
static u32* g_shmScratch;
static ShRec g_shm[SHM_RECS];
static ShRec* g_shmCur;   // replaying: the next segment at g_shmPos
static ShRec* g_shmRec;   // recording
static u32 g_shmPos, g_shmSeg, g_shmUse;
static u32 g_shmOut[24], g_shmFrm[5];  // the running state: the registers and frame words as the last segment left them
static u32 g_shmLastPc;              // and where the last segment stopped (the next one starts after a triangle or at the loop head)
static bool g_shmOn;
u64 g_shmStats[6];  // calls, calls replayed to the end, segments replayed, segments run (holes, budget), calls recorded, records dropped
// Calls not replayed, by reason: [0] no record with the key, [1] the key's record not finished, [2] its first inputs
// differ, [3] dropped by a DMA, [4] the bitset not clear; [5] records given up at the word cap; [6] the longest record
// (segments), [7] replays that stopped on other inputs.
u64 g_shmMiss[8];
u64 g_shmDiff[24];  // replays stopped on other inputs, by the first input that differed (the PC, r1, r14-r30, frame words)
void hle_set_shdw_memo(bool on) { g_shmOn = on; }
bool hle_shdw_memo(void) { return g_shmOn; }
static const u8 kShOut[24] = { 0, 3, 4, 5, 6, 7, 8, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30 };

static void shm_in(const Gekko* g, u32* in) {
    in[0] = g->pc;
    in[1] = g->gpr[1];
    memcpy(in + 2, &g->gpr[14], 17 * 4);
    u32 fa = g->gpr[1] + 2212 - 0x80000000u;
    for (int k = 0; k < 5; k++) in[19 + k] = ld32(g_mem + fa + 4 * k);
    // At the call's start r14, r16-r18, r23-r25 and the frame words 2224 / 2228 are written before they are read (they
    // hold whatever the previous call left): not inputs there.
    if (g->pc == PC_ENTRY) { in[2] = in[4] = in[5] = in[6] = in[11] = in[12] = in[13] = in[22] = in[23] = 0; }
}
// The key a call's record is found by (its first inputs then compared whole).
static u64 shm_key(const u32* in) {
    u32 a = 0x9E3779B9u, b = 0x85EBCA6Bu;
    for (int k = 0; k < SHM_IN; k++) { a = (a ^ in[k]) * 0x01000193u; b = (b + in[k]) * 0xC2B2AE35u; }
    return ((u64)a << 32 | b) | 1;
}
// The inputs the running state gives for the next segment (a triangle's return to the walk).
static void shm_expect(u32 r1, u32* in) {
    in[0] = g_shmLastPc == PC_LOOP ? PC_LOOP : PC_NEXT;  // (after the budget's stop: the loop head)
    in[1] = r1;
    memcpy(in + 2, g_shmOut + 7, 17 * 4);
    memcpy(in + 19, g_shmFrm, 5 * 4);
}
static void shm_start(const u32* in) {  // the running state at a call's start
    g_shmLastPc = PC_TRI;
    for (int k = 0; k < 7; k++) g_shmOut[k] = 0;
    memcpy(g_shmOut + 7, in + 2, 17 * 4);
    memcpy(g_shmFrm, in + 19, 5 * 4);
}

static bool shm_bits_clear(const Gekko* g) {
    const u8* b = g_mem + (g->gpr[1] + 156 - 0x80000000u);
    for (u32 i = 0; i < 2048; i += 4) if (ld32(b + i)) return false;
    return true;
}

// The segment at *pos: its expected inputs into `want`; returns its header word and leaves *pos at its outputs.
static u32 shm_head(const ShRec* r, u32* pos, u32 r1, u32* want) {
    const u32* w = r->w + *pos;
    u32 h = w[0];
    if (h >> 31) { memcpy(want, w + 4, SHM_IN * 4); *pos += 4 + SHM_IN; }
    else { if (g_shmSeg == 0) memcpy(want, r->in0, SHM_IN * 4); else shm_expect(r1, want); *pos += 4; }
    return h;
}
// Its outputs into the running state (and, with g, into the guest).
static void shm_out(const ShRec* r, u32* pos, const u32* head, Gekko* g) {
    u32 h = head[0], fm = head[3] & 31, bitOff = head[3] >> 16, pc = head[2];
    const u32* w = r->w + *pos;
    for (int k = 0; k < 24; k++) if ((h >> k) & 1) g_shmOut[k] = *w++;
    for (int k = 0; k < 5; k++) if ((fm >> k) & 1) g_shmFrm[k] = *w++;
    u32 bitVal = pc == PC_TRI ? *w++ : 0;
    *pos = (u32)(w - r->w);
    g_shmLastPc = pc;
    if (!g) return;
    for (int k = 0; k < 24; k++) g->gpr[kShOut[k]] = g_shmOut[k];
    if ((h >> 24) & 1) g->cr = (g->cr & 0x0FFFFFFFu) | (((h >> 25) & 0xE) << 28) | (((g->xer & XER_SO) ? 1u : 0u) << 28);
    if ((h >> 29) & 1) g->xer &= ~XER_CA;
    u32 fa = g->gpr[1] + 2212 - 0x80000000u;
    for (int k = 0; k < 5; k++) ramp_wr32a(g_mem + fa + 4 * k, g_shmFrm[k]);
    if (pc == PC_TRI) ramp_wr32a(g_mem + (g->gpr[1] + 156 - 0x80000000u) + bitOff, bitVal);
    g->pc = pc;
}

static void shm_seal(ShRec* r);
static void shm_note(const Gekko* g, const u32* in, u32 n) {
    ShRec* r = g_shmRec;
    if (!r || !n) return;
    if (r->nw + 4 + SHM_IN + 24 + 5 + 1 > SHM_WORDS) { g_shmRec = NULL; r->key = 0; r->w = NULL; g_shTrack = false; g_shmMiss[5]++; return; }
    u32 want[SHM_IN];
    if (r->nSeg == 0) memcpy(want, r->in0, sizeof want); else shm_expect(g->gpr[1], want);
    bool store = memcmp(want, in, sizeof want) != 0;
    bool hole = g->pc != PC_TRI && g->pc != PC_EXIT && g->pc != PC_LOOP;  // a stop at an address: guest code runs there
    u32* w = r->w + r->nw;
    u32* head = w;
    w += 4;
    if (store) { memcpy(w, in, SHM_IN * 4); w += SHM_IN; }
    u32 regs = 0, fm = 0;
    if (!hole) {
        for (int k = 0; k < 24; k++) { u32 v = g->gpr[kShOut[k]]; if (v != g_shmOut[k]) { regs |= 1u << k; *w++ = v; g_shmOut[k] = v; } }
        u32 fa = g->gpr[1] + 2212 - 0x80000000u;
        for (int k = 0; k < 5; k++) { u32 v = ld32(g_mem + fa + 4 * k); if (v != g_shmFrm[k]) { fm |= 1u << k; *w++ = v; g_shmFrm[k] = v; } }
        if (g->pc == PC_TRI) *w++ = g->gpr[0];  // (stwx r0,r3,r4 with r3 = r1 + 156: the bitset word)
    }
    head[0] = regs | (u32)g_shCrK << 24 | ((g->cr >> 28) & 0xFu) << 25 | (u32)g_shCa << 29 | (u32)hole << 30 | (u32)store << 31;
    head[1] = n;
    head[2] = g->pc;
    head[3] = fm | (g->pc == PC_TRI ? g->gpr[4] << 16 : 0);
    r->nw = (u32)(w - r->w);
    r->nSeg++;
    if (!hole) g_shmLastPc = g->pc;
    if (g->pc == PC_EXIT) { r->done = true; r->lo = g_shLo; r->hi = g_shHi; g_shmRec = NULL; g_shTrack = false; g_shmStats[4]++; shm_seal(r); }
    if (r->nSeg > g_shmMiss[6]) g_shmMiss[6] = r->nSeg;
}

static u32 scan_live(Gekko* g, u32 budget);
#ifndef __3DS__
static void shm_report(void) {
    fprintf(stderr, "SHDW_MEMO: calls %llu, replayed to the end %llu, segments replayed %llu, run %llu, recorded %llu, dropped %llu\n",
            (unsigned long long)g_shmStats[0], (unsigned long long)g_shmStats[1], (unsigned long long)g_shmStats[2],
            (unsigned long long)g_shmStats[3], (unsigned long long)g_shmStats[4], (unsigned long long)g_shmStats[5]);
}
#endif
#ifdef __3DS__
void* linearAlloc(size_t size);  // (libctru: the records, more than the heap has free)
void linearFree(void* mem);
static void* shm_alloc(size_t n) { return linearAlloc(n); }
static void shm_free(void* p) { linearFree(p); }
#else
static void* shm_alloc(size_t n) { return malloc(n); }
static void shm_free(void* p) { free(p); }
// SHDW_MEMOCHECK=1: each replayed segment also runs the walk from the same state (to its natural stop) and the two are
// compared (GPRs, CR, XER, PC, count, the frame); the walk's result stands.
static int g_shmChk = -1;
static u64 g_shmChkN, g_shmChkBad;
static void shm_chk_report(void) {
    fprintf(stderr, "SHDW_MEMOCHECK: %llu replayed segments compared with the walk, %llu differed\n",
            (unsigned long long)g_shmChkN, (unsigned long long)g_shmChkBad);
}
#endif

// A finished record leaves the scratch stream for a block of its own size (none to be had: the record is dropped).
static void shm_seal(ShRec* r) {
    u32* b = r->nw ? shm_alloc(r->nw * 4) : NULL;
    if (!b) { r->key = 0; r->w = NULL; r->own = false; r->done = false; return; }
    memcpy(b, r->w, r->nw * 4);
    r->w = b;
    r->own = true;
}

// A record stays valid while no DMA since it was made (or last checked) wrote over the collision data its walk read or
// the grid header (r15's 64 bytes, and the node they point to). More DMAs than the log holds: dropped.
static bool shm_valid(ShRec* r) {
    u32 now = g_dmaGen;
    if (now - r->gen > DMA_LOG_N) { r->key = 0; g_shmStats[5]++; return false; }
    for (u32 q = r->gen; q != now; q++) {
        u32 pa = g_dmaLog[q % DMA_LOG_N][0] | 0x80000000u, len = g_dmaLog[q % DMA_LOG_N][1];
        bool hit = (pa < r->hi && pa + len > r->lo) || (pa < r->hdr + 64 && pa + len > r->hdr) ||
                   (pa < r->node + 64 && pa + len > r->node);
        if (hit) { r->key = 0; g_shmStats[5]++; return false; }
    }
    r->gen = now;
    return true;
}

static u32 shm_run(Gekko* g, u32 budget) {
    u32 fa = g->gpr[1] + 156 - 0x80000000u;
    if (fa > g_memSize - 2232 || (fa & 3)) { g_shmCur = g_shmRec = NULL; g_shTrack = false; return scan_live(g, budget); }
    // The common case first: replaying, a segment whose inputs are what the last one left (not stored, not a hole),
    // compared register by register against the running state (no input arrays built: ~2.8 us a segment before, 2DS
    // job 618).
#ifndef __3DS__
    if (g_shmChk == 0)
#endif
    if (g_shmCur && g->pc != PC_ENTRY && g_shmSeg && g_shmPos < g_shmCur->nw) {
        ShRec* r = g_shmCur;
        const u32* head = r->w + g_shmPos;
        u32 h = head[0];
        if (!(h >> 30) && head[1] <= budget + SHM_OVER && g->pc == (g_shmLastPc == PC_LOOP ? PC_LOOP : PC_NEXT) &&
            g->gpr[1] == r->in0[1] && !memcmp(&g->gpr[14], g_shmOut + 7, 17 * 4)) {
            const u8* f = g_mem + (g->gpr[1] + 2212 - 0x80000000u);
            if (ld32(f) == g_shmFrm[0] && ld32(f + 4) == g_shmFrm[1] && ld32(f + 8) == g_shmFrm[2] &&
                ld32(f + 12) == g_shmFrm[3] && ld32(f + 16) == g_shmFrm[4]) {
                u32 pos = g_shmPos + 4, hd[4];
                memcpy(hd, head, sizeof hd);
                g_shmSeg++;
                shm_out(r, &pos, hd, g);
                g_shmPos = pos;
                g_shmStats[2]++;
                if (hd[2] == PC_EXIT) { g_shmCur = NULL; g_shmStats[1]++; }
                return hd[1];
            }
        }
    }
    u32 in[SHM_IN];
    if (g->pc == PC_ENTRY || g_shmCur || g_shmRec) shm_in(g, in);
    if (g->pc == PC_ENTRY) {
        g_shmStats[0]++;
        // A new call: a new generation for the stand-in's leaf memo, as scan does at the call's start (a replayed start
        // never reaches scan, and a later segment the walk runs must not take the previous call's leaves as visited).
        g_shFrame = g->gpr[1];
        if (++g_shGen >= (1u << 20)) { g_shGen = 1; memset(g_shMemo, 0, sizeof g_shMemo); }
        u64 key = shm_key(in);
        if (g_shmRec) {  // the previous call's walk ended in guest code (after a hole): its record is a usable prefix
            ShRec* q = g_shmRec;
            if (q->nSeg) { q->done = true; q->lo = g_shLo; q->hi = g_shHi; g_shmStats[4]++; shm_seal(q); } else { q->key = 0; q->w = NULL; }
            g_shmRec = NULL; g_shTrack = false;
        }
        g_shmCur = NULL;
        u32 why = 0;
        for (int i = 0; i < SHM_RECS; i++) {
            if (g_shm[i].key != key) continue;
            if (!g_shm[i].done) { why = 1; continue; }
            if (memcmp(g_shm[i].in0, in, sizeof in)) { why = 2; continue; }
            if (!shm_valid(&g_shm[i])) { why = 3; continue; }
            if (!shm_bits_clear(g)) { why = 4; continue; }
            g_shmCur = &g_shm[i]; g_shmPos = 0; g_shmSeg = 0; g_shm[i].use = ++g_shmUse;
            break;
        }
        if (!g_shmCur) g_shmMiss[why]++;
        if (!g_shmCur) {  // record this call (into the least recently used record)
            ShRec* v = &g_shm[0];
            for (int i = 1; i < SHM_RECS; i++) if (g_shm[i].use < v->use) v = &g_shm[i];
            if (!g_shmScratch) g_shmScratch = shm_alloc(SHM_WORDS * 4);
            if (g_shmScratch && shm_bits_clear(g)) {
                if (v->own) shm_free(v->w);
                v->own = false;
                v->w = g_shmScratch;
                v->key = key; v->nSeg = v->nw = 0; v->done = false; v->use = ++g_shmUse; v->gen = g_dmaGen; v->hdr = g->gpr[15];
                u32 ha = g->gpr[15] + 24 - 0x80000000u;
                v->node = ha <= g_memSize - 4 && !(ha & 3) ? ld32(g_mem + ha) : 0;
                memcpy(v->in0, in, sizeof in);
                g_shmRec = v;
                g_shTrack = true; g_shLo = 0xFFFFFFFFu; g_shHi = 0;
            }
        }
        shm_start(in);
    }
    if (g_shmCur) {
        ShRec* r = g_shmCur;
        u32 want[SHM_IN];
        if (g_shmPos < r->nw) {
            u32 pos = g_shmPos;
            u32 h = shm_head(r, &pos, g->gpr[1], want);
            if (!memcmp(want, in, sizeof want)) {
                u32 head[4];
                memcpy(head, r->w + g_shmPos, sizeof head);
                g_shmSeg++;
                if ((h >> 30) & 1) { g_shmPos = pos; g_shmStats[3]++; return scan_live(g, budget); }  // a hole: the walk
                if (head[1] > budget + SHM_OVER) {  // far past the slice: the walk (which stops in time); replay then ends
                    g_shmCur = NULL; g_shmStats[3]++; return scan_live(g, budget);
                }
#ifndef __3DS__
                if (g_shmChk < 0) { g_shmChk = getenv("SHDW_MEMOCHECK") != NULL; if (g_shmChk) atexit(shm_chk_report); }
                if (g_shmChk) {
                    enum { FRAME = 2304 };
                    u32 fpa = g->gpr[1] - 0x80000000u;
                    static u8 f0[FRAME], fr[FRAME];
                    Gekko g0 = *g;
                    memcpy(f0, g_mem + fpa, FRAME);
                    shm_out(r, &pos, head, g);
                    Gekko gr = *g;
                    memcpy(fr, g_mem + fpa, FRAME);
                    *g = g0;
                    memcpy(g_mem + fpa, f0, FRAME);
                    u32 n2 = scan(g, head[2] == PC_LOOP ? head[1] : 0xFFFFFFFFu);  // (a budget stop: the same budget)
                    g_shmChkN++;
                    if ((n2 != head[1] || g->pc != gr.pc || memcmp(g->gpr, gr.gpr, sizeof g->gpr) || g->cr != gr.cr ||
                         g->xer != gr.xer || memcmp(g_mem + fpa, fr, FRAME)) && g_shmChkBad++ < 10)
                        fprintf(stderr, "SHDW_MEMOCHECK segment from %08x: walk %u insns to %08x, record %u to %08x\n",
                                g0.pc, n2, g->pc, head[1], gr.pc);
                } else
#endif
                shm_out(r, &pos, head, g);
                g_shmPos = pos;
                g_shmStats[2]++;
                if (head[2] == PC_EXIT) { g_shmCur = NULL; g_shmStats[1]++; }
                return head[1];
            }
        }
        g_shmCur = NULL;  // the inputs went another way (or the record ended): the walk runs from here
        g_shmMiss[7]++;
        if (g_shmPos < r->nw) { for (int k = 0; k < SHM_IN; k++) if (want[k] != in[k]) { g_shmDiff[k]++; break; } }
    }
    if (!g_shmRec) return scan_live(g, budget);
    u32 n = scan_live(g, budget);
    shm_note(g, in, n);
    return n;
}

// The walk itself: on the PC through HLE_CHECK when that is set (every replayed segment is then compared too, see
// shm_run's caller); on the 3DS the stand-in.
static u32 scan_live(Gekko* g, u32 budget) {
#ifndef __3DS__
    static int chk = -1;
    if (chk < 0) chk = getenv("HLE_CHECK") != NULL;
    return chk ? checked(g, budget) : scan(g, budget);
#else
    return scan(g, budget);
#endif
}


#ifndef __3DS__
// SHDW_BODYSTAT=1 (PC, measurement): the triangle path (0x8007FCE4 .. the callback's bctrl at 0x8007FE94, or a skip back
// to 0x8007FEA4): which registers it changes, its outcomes, and how many distinct (r15, triangle, r1) keys there are.
#define PC_BODY 0x8007FCE4u
#define PC_CBK 0x8007FE94u
static int g_bsOn = -1;
static bool g_bsPend;
static Gekko g_bs0;
static u32 g_bsGpr, g_bsFpr0, g_bsFpr1, g_bsMisc;  // changed GPRs / FPR ps0 / ps1 (bits), CR XER LR CTR FPSCR (bits 0-4)
static u64 g_bsN[3], g_bsKeys;  // to the callback, skipped, keys seen
// Memory: the frame window [r1 - 1024, r1 + 2240) snapshotted and diffed (stand-ins' stores included), and the
// interpreter's recorded stores outside it.
#define BS_LO 1024u
#define BS_HI 2240u
static u8 g_bsF0[BS_LO + BS_HI];
static u8 g_bsFw[(BS_LO + BS_HI) / 4];  // frame words ever changed
static u64 g_bsOut;  // stores outside the window
static u32 g_bsOutA[8];
#define BSN (1u << 20)
static u64* g_bsSet;
static void bs_report(void) {
    fprintf(stderr, "SHDW_BODYSTAT frame words changed (offsets from r1):");
    for (u32 i = 0; i < (BS_LO + BS_HI) / 4; i++) if (g_bsFw[i]) fprintf(stderr, " %d", (int)(i * 4) - (int)BS_LO);
    fprintf(stderr, "; stores outside the window %llu:", (unsigned long long)g_bsOut);
    for (u32 i = 0; i < 8 && i < g_bsOut; i++) fprintf(stderr, " %08x", g_bsOutA[i]);
    fprintf(stderr, "\n");
    fprintf(stderr, "SHDW_BODYSTAT: %llu to the callback, %llu skipped, %llu distinct keys; changed GPRs %08x FPR ps0 %08x ps1 %08x misc %x" "\n",
            (unsigned long long)g_bsN[0], (unsigned long long)g_bsN[1], (unsigned long long)g_bsKeys, g_bsGpr, g_bsFpr0, g_bsFpr1, g_bsMisc);
}
static bool bs_on(void) {
    if (g_bsOn < 0) { g_bsOn = getenv("SHDW_BODYSTAT") != NULL; if (g_bsOn) { g_bsSet = calloc(BSN, 8); atexit(bs_report); } }
    return g_bsOn;
}
static void bs_end(const Gekko* g, int kind) {
    if (!g_bsPend) return;
    g_bsPend = false;
    g_memoRec = NULL;
    u32 base = g_bs0.gpr[1] - BS_LO - 0x80000000u;
    for (u32 i = 0; i < BS_LO + BS_HI; i += 4) if (memcmp(g_mem + base + i, g_bsF0 + i, 4)) g_bsFw[i / 4] = 1;
    for (u32 i = 0; i < g_chkN; i++) {
        u32 a = g_chkW[i][0], lo = g_bs0.gpr[1] - BS_LO;
        if (a - lo >= BS_LO + BS_HI) { if (g_bsOut < 8) g_bsOutA[g_bsOut] = a; g_bsOut++; }
    }
    g_bsN[kind]++;
    for (int k = 0; k < 32; k++) if (g->gpr[k] != g_bs0.gpr[k]) g_bsGpr |= 1u << k;
    for (int k = 0; k < 32; k++) {
        if (memcmp(&g->fpr[k].ps0, &g_bs0.fpr[k].ps0, sizeof g->fpr[k].ps0)) g_bsFpr0 |= 1u << k;
        if (memcmp(&g->fpr[k].ps1, &g_bs0.fpr[k].ps1, sizeof g->fpr[k].ps1)) g_bsFpr1 |= 1u << k;
    }
    if (g->cr != g_bs0.cr) g_bsMisc |= 1;
    if (g->xer != g_bs0.xer) g_bsMisc |= 2;
    if (g->lr != g_bs0.lr) g_bsMisc |= 4;
    if (g->ctr != g_bs0.ctr) g_bsMisc |= 8;
    if (g->fpscr != g_bs0.fpscr) g_bsMisc |= 16;
}
static void bs_hook(const Gekko* g) {
    if (!bs_on()) return;
    if (g->pc == PC_BODY) {
        g_bs0 = *g;
        g_bsPend = true;
        memcpy(g_bsF0, g_mem + (g->gpr[1] - BS_LO - 0x80000000u), BS_LO + BS_HI);
        g_chkN = 0;
        g_memoRec = (void*)2;
        u32 a = g->gpr[16] - 0x80000000u, t = a < g_memSize - 2 ? ld16(g_mem + a) : 0;
        u64 k = ((u64)g->gpr[15] * 0x9E3779B97F4A7C15ull) ^ ((u64)t << 32) ^ g->gpr[1];
        k |= 1;
        u32 h = (u32)(k >> 40) & (BSN - 1);
        while (g_bsSet[h] && g_bsSet[h] != k) h = (h + 1) & (BSN - 1);
        if (!g_bsSet[h]) { g_bsSet[h] = k; g_bsKeys++; }
    } else if (g->pc == PC_CBK) bs_end(g, 0);
    else if (g->pc == PC_NEXT) bs_end(g, 1);
}
#endif


// ---- The triangle path (--shdwtri; REDESIGN C32b) ----
// With the walk replayed, what is left of the shadows in the title's later stretch is the path an unvisited triangle
// takes from 0x8007fce4: getPolyCode, ChkShdwDrawThrough, the prism's three vertices from two cross products, GetTriPla,
// then the shadow callback (bctrl at 0x8007fe94), or a skip back to 0x8007fea4 (a through polygon, a near-zero dot).
// ShdwDraw's own blocks were 22.4% of core 0 there, GetTriPla 3.1%, getPolyCode 1.5% (job 620). This runs the path
// natively to the callback's entry or the skip, one statement per guest instruction, and counts them: the integer work
// and moves here, the PSVEC calls through their stand-ins (hle_ps.c, the interpreter when one refuses), and every float
// operation (fabs, frsp, fcmpo, fdivs, fneg) through the interpreter's own handler, so FPRF and CR0 are its. Its
// callees are recognised by a hash of their bytes. It reads everything before it writes anything, so a guest address
// it cannot use (outside MEM1, misaligned, inside the frame it writes) leaves the path to guest code unchanged.
// TRI_CHECK=1 (PC) compares every run with the interpreter.
static const struct { u32 pc, n; } kTriFns[] = {
    { 0x8007EB70u, 0x54 },  // dBgWKCol::getPolyCode
    { 0x800740C4u, 0x18 },  // (its table lookup)
    { 0x80074048u, 0x2C },  // (its 20-byte copy)
    { 0x8007FF00u, 0x1C },  // dBgWKCol::ChkShdwDrawThrough
    { 0x8007E8C0u, 0xC0 },  // dBgWKCol::GetTriPla
    { 0x8007E850u, 0x1C },  // (its normal lookup)
    { 0x8026F52Cu, 0x50 },  // cM3dGPla::cM3dGPla(normal, point)
    { 0x803621DCu, 0x10 },  // _savegpr_29
    { 0x80362228u, 0x10 },  // _restgpr_29
};
#define TRI_HASH 0x4F72DC97525B15A2ull  // FNV-1a 64 of kTriFns' bytes in order (HLE_PRINT_HASH=1 prints it)
#define TRI_RET 0x8007FE98u
static int g_triKnown = -1;
static bool g_triOn;
static int g_triPs[4];  // hle_ps.c's PSVECCrossProduct, PSVECDotProduct, PSVECScale, PSVECAdd (-1: the interpreter)
static const u32 kTriPsPc[4] = { 0x803471B4u, 0x80347194u, 0x803470D8u, 0x80347090u };
u64 g_triWhy[8];  // refusals: the CPU state, the frame or list, the header, the prism, the code, the rest
u64 g_triStats[4];  // to the callback, skipped (through, first dot, second dot), refused
void hle_set_shdw_tri(bool on) { g_triOn = on; }
bool hle_shdw_tri(void) { return g_triOn; }
static void tri_invalidate(u32 pa) {
    for (u32 i = 0; i < sizeof kTriFns / sizeof kTriFns[0]; i++)
        if (pa >> 12 == (kTriFns[i].pc & 0x0FFFFFFFu) >> 12 || pa >> 12 == ((kTriFns[i].pc + kTriFns[i].n - 1) & 0x0FFFFFFFu) >> 12) g_triKnown = -1;
}
static bool tri_known(void) {
    if (g_triKnown < 0) {
        u64 h = 0xCBF29CE484222325ull;
        bool in = g_mem != NULL;
        for (u32 i = 0; in && i < sizeof kTriFns / sizeof kTriFns[0]; i++) {
            u32 pa = kTriFns[i].pc & 0x0FFFFFFFu;
            if (pa + kTriFns[i].n > g_memSize) { in = false; break; }
            for (u32 k = 0; k < kTriFns[i].n; k++) { h ^= ram_rd8(g_mem, pa + k); h *= 0x100000001B3ull; }
        }
#ifndef __3DS__
        if (getenv("HLE_PRINT_HASH")) fprintf(stderr, "HLE hash of the triangle path's callees: %016llx\n", (unsigned long long)h);
#endif
        g_triKnown = in && h == TRI_HASH;
        for (int k = 0; k < 4; k++) { bool gp; u32 sz; g_triPs[k] = hle_ps_inline(kTriPsPc[k], &gp, &sz); }
    }
    return g_triKnown == 1;
}
// A guest address in MEM1 (cached mapping), aligned to a: its host pointer, or NULL.
static inline u8* tri_p(u32 ea, u32 len, u32 a) {
    u32 pa = ea - 0x80000000u;
    if ((ea & (a - 1)) || pa >= g_memSize || len > g_memSize - pa) return NULL;
    return g_mem + pa;
}
// [ea, ea + len) clear of the frame window the path writes, [r1 - 64, r1 + 2212).
static inline bool tri_clear(u32 ea, u32 len, u32 r1) { return ea + len <= r1 - 64u || ea >= r1 + 2212u; }
// One float instruction (its word from guest memory, the code hashed) through the interpreter.
static inline void tri_fop(Gekko* g, u32 pc) {
    GkPriv* p = GKP(g);
    u32 c0 = p->curPc, n0 = p->npc;
    bool b0 = p->brk, st;
    gekko_interp_one(g, pc, ram_rd32(g_mem, pc & 0x0FFFFFFFu), &st);
    p->curPc = c0; p->npc = n0; p->brk = b0;
}
// The float operations natively, as the JIT computes them (jit.c "Fast float"): for finite, normal (or zero) operands
// and results the value is the interpreter's bit for bit (a single result computed in double then rounded: double
// rounding is harmless for one operation on singles), FPSCR is left alone but for FPCC on a compare; anything else
// (a denormal, an infinity, a NaN, an overflow) goes to the interpreter's handler. First version through the handlers
// for every operation: later stretch 21.94 -> 19.80% (2DS job 625; soft-float division and rounding).
static inline bool tri_nrm(double d) {  // finite and not a denormal (zero allowed)
    u64 b; memcpy(&b, &d, 8);
    u32 e = (u32)(b >> 52) & 0x7FFu;
    return e != 0x7FFu && (e != 0 || (b << 1) == 0);
}
static inline bool tri_nrmf(float f) {
    u32 b; memcpy(&b, &f, 4);
    u32 e = b & 0x7F800000u;
    return e != 0x7F800000u && (e != 0 || (b << 1) == 0);
}
static inline void tri_fabs(Gekko* g, int d, int b, u32 pc) { (void)pc; fpr_set0(g, d, fpr_get0(g, b) & ~0x8000000000000000ull); }
static inline void tri_fneg(Gekko* g, int d, int b, u32 pc) { (void)pc; fpr_set0(g, d, fpr_get0(g, b) ^ 0x8000000000000000ull); }
static inline void tri_frsp(Gekko* g, int d, int b, u32 pc) {
    double x = g->fpr[b].ps0;
    float f = (float)x;
    if (!tri_nrm(x) || !tri_nrmf(f) || (f == 0.0f && x != 0.0)) { tri_fop(g, pc); return; }
    g->fpr[d].ps0 = g->fpr[d].ps1 = (double)f;
}
static inline void tri_fdivs(Gekko* g, int d, int a, int b, u32 pc) {
    double x = g->fpr[a].ps0, y = g->fpr[b].ps0;
    if (!tri_nrm(x) || !tri_nrm(y) || y == 0.0) { tri_fop(g, pc); return; }
    float q = (float)(x / y);
    if (!tri_nrmf(q) || (q == 0.0f && x != 0.0)) { tri_fop(g, pc); return; }
    g->fpr[d].ps0 = g->fpr[d].ps1 = (double)q;
}
static inline void tri_fcmpo0(Gekko* g, int a, int b, u32 pc) {
    double x = g->fpr[a].ps0, y = g->fpr[b].ps0;
    if (x != x || y != y) { tri_fop(g, pc); return; }
    u32 c = x < y ? 8u : x > y ? 4u : 2u;
    g->cr = (g->cr & 0x0FFFFFFFu) | (c << 28);
    g->fpscr = (g->fpscr & ~FPSCR_FPCC) | (c << 12);
}
// lfs into f0 (both lanes, as the interpreter loads a single).
static inline void tri_lfs0(Gekko* g, u32 w) {
    u32 e = w & 0x7F800000u;
    if ((e && e != 0x7F800000u) || !(w << 1)) { float f; memcpy(&f, &w, 4); g->fpr[0].ps0 = g->fpr[0].ps1 = (double)f; return; }
    u64 d = sf_single_to_double(w); fpr_set0(g, 0, d); fpr_set1(g, 0, d);
}
// stfs of f0, as the interpreter rounds it.
static inline u32 tri_stfs0(const Gekko* g) {
    double x = g->fpr[0].ps0;
    float f = (float)x;
    u32 w; memcpy(&w, &f, 4);
    u32 e = w & 0x7F800000u;
    if ((e && e != 0x7F800000u && (double)f == x) || x == 0.0) return w;
    return sf_double_to_single(fpr_get0(g, 0), (GKP(g)->compat & GK_COMPAT_STFS_TINY_ZERO) != 0);
}
// lfs f0 then stfs f0 of one word: the word itself unless it is a denormal, an infinity or a NaN.
static inline u32 tri_mov(Gekko* g, u32 w) {
    u32 e = w & 0x7F800000u;
    if ((e && e != 0x7F800000u) || !(w << 1)) return w;
    tri_lfs0(g, w);
    return tri_stfs0(g);
}
// A PSVEC call: its stand-in, or the guest function in the interpreter when it refuses.
static u32 tri_call(Gekko* g, int k, u32 ret) {
    g->lr = ret;
    u32 n = g_triPs[k] >= 0 ? hle_ps_inline_call(g, (u32)g_triPs[k]) : 0;
    if (n) return n & 0x3FFFFFFFu;
    GkPriv* p = GKP(g);
    u32 c0 = p->curPc, n0 = p->npc, pc = kTriPsPc[k];
    bool b0 = p->brk, st;
    while (pc != ret && n < 256) { pc = gekko_interp_one(g, pc, ram_rd32(g_mem, pc & 0x0FFFFFFFu), &st); n++; }
    p->curPc = c0; p->npc = n0; p->brk = b0;
    return n;
}
// The four PSVEC calls inline, with their stand-ins' arithmetic (hle_ps_gen.inc: the same operations in the same order,
// so the same results on each platform) and counts; the stand-in call itself, its checks and three address lookups a
// call, was most of their cost here. Their addresses were checked before the first store. Off (hle_ps.c's stand-ins
// off or not recognised, or Scale's f1 not two singles): tri_call.
static inline float tri_ldf(const u8* p) { u32 w = ld32(p); float f; memcpy(&f, &w, 4); return f; }
static inline void tri_stf(u8* p, float f) { u32 w; memcpy(&w, &f, 4); if (!(w & 0x7F800000u)) w &= 0x80000000u; ramp_wr32a(p, w); }  // (PSTF)
static u32 tri_ps(Gekko* g, int k, u32 ret) {
    if (g_triPs[k] < 0 || !hle_ps_enabled()) return tri_call(g, k, ret);
    if (k == 2 && ((double)(float)g->fpr[1].ps0 != g->fpr[1].ps0 || (double)(float)g->fpr[1].ps1 != g->fpr[1].ps1)) return tri_call(g, k, ret);
    g->lr = ret;
    u8* a = g_mem + (g->gpr[3] - 0x80000000u);
    u8* b = g_mem + (g->gpr[4] - 0x80000000u);
    u8* o = g_mem + (g->gpr[5] - 0x80000000u);
    if (k == 0) {  // PSVECCrossProduct(a, b, o)
        P f1 = { tri_ldf(b), tri_ldf(b + 4) }, f2 = PS(tri_ldf(a + 8)), f0 = { tri_ldf(a), tri_ldf(a + 4) };
        P f6 = PMERGE10(f1, f1), f3 = PS(tri_ldf(b + 8));
        P f4 = PMUL(f1, f2), f7 = PMULS0(f1, f0);
        P f5 = PMSUB(f0, f3, f4), f8 = PMSUB(f0, f6, f7);
        P f9 = PMERGE11(f5, f5), f10 = PMERGE01(f5, f8);
        tri_stf(o, f9.p0);
        f10 = PNEG(f10);
        tri_stf(o + 4, f10.p0); tri_stf(o + 8, f10.p1);
        g->fpr[1].ps0 = (double)f1.p0; g->fpr[1].ps1 = (double)f1.p1;
        return 15;
    }
    if (k == 1) {  // PSVECDotProduct(a, b) -> f1
        P f2 = { tri_ldf(a + 4), tri_ldf(a + 8) }, f3 = { tri_ldf(b + 4), tri_ldf(b + 8) };
        f2 = PMUL(f2, f3);
        P f5 = { tri_ldf(a), tri_ldf(a + 4) }, f4 = { tri_ldf(b), tri_ldf(b + 4) };
        f3 = PMADD(f5, f4, f2);
        P f1 = PSUM0(f3, f2, f2);
        g->fpr[1].ps0 = (double)f1.p0; g->fpr[1].ps1 = (double)f1.p1;
        return 8;
    }
    if (k == 2) {  // PSVECScale(a, b, f1)
        P f1 = { (float)g->fpr[1].ps0, (float)g->fpr[1].ps1 };
        P f0 = { tri_ldf(a), tri_ldf(a + 4) }, f2 = { tri_ldf(a + 8), 1.0f };
        f0 = PMULS0(f0, f1);
        tri_stf(b, f0.p0); tri_stf(b + 4, f0.p1);
        f0 = PMULS0(f2, f1);
        tri_stf(b + 8, f0.p0);
        return 7;
    }
    {  // PSVECAdd(a, b, o)
        P f2 = { tri_ldf(a), tri_ldf(a + 4) }, f4 = { tri_ldf(b), tri_ldf(b + 4) };
        P f6 = PADD(f2, f4);
        tri_stf(o, f6.p0); tri_stf(o + 4, f6.p1);
        P f3 = { tri_ldf(a + 8), 1.0f }, f5 = { tri_ldf(b + 8), 1.0f };
        P f7 = PADD(f3, f5);
        tri_stf(o + 8, f7.p0);
        return 9;
    }
}
static inline void tri_cr0(Gekko* g, u32 v) { g->cr = (g->cr & 0x0FFFFFFFu) | ((cmps(v, 0) | (g->xer >> 31)) << 28); }
#define TW(p, v) ramp_wr32a((p), (v))

static u8 g_triStop;  // tri_bench: stop the path at this point (0: never)
// The fast form's words and singles (tri_run v4).
bool g_triFast = true;  // --trifast off: the PSVEC-at-a-time form only (comparison)
u64 g_triFastN;         // triangles the fast form took
static inline float U2Ft(u32 w) { float f; memcpy(&f, &w, 4); return f; }
static inline u32 bitsft(float f) { u32 w; memcpy(&w, &f, 4); return w; }
static inline u32 PSTFt(float f) { u32 w = bitsft(f); return (w & 0x7F800000u) ? w : w & 0x80000000u; }  // (psq_st's flush)
static inline bool tri_plw(u32 w) { u32 e = w & 0x7F800000u; return (e && e != 0x7F800000u) || !(w << 1); }  // zero or normal
#define CMPF(a, b) ((a) < (b) ? 8u : (a) > (b) ? 4u : 2u)  // fcmpo's CR field for two numbers
static bool g_tchkOn;  // TRI_CHECK (PC): the callback keeps what it overwrote
static bool g_cbRan;   // the last tri_run went through the callback natively

// ---- The shadow callback natively (--shdwcb; with --shdwtri) ----
// d_drawlist.cpp's psdRealCallBack 0x80054688 (the real-time shadow's callback: light direction, plane distance and
// box tests, then dDlst_shadowPoly_c::set 0x80054288, which stores the triangle pushed off the plane), so a triangle
// the walk reaches never leaves native code: the path, the callback and ShdwDraw's three instructions after it
// (0x8007fe98..0x8007fea0) to the next-triangle step. Run only when the callback is this one (recognised by a hash of
// it, set and the GPR helpers) and the shadow poly's two virtual getters (getTriMax, getTri) are a load-immediate and
// an add-immediate (decoded from their words). Everything it reads is checked before its first store (not taken:
// guest code runs the callback, with the path's results in place). The callee frames below r1 are dead state (the
// owner decision of 2026-10-15 for stand-ins), as are the volatile GPRs other than r0, r3 and r11.
#define PC_CB 0x80054688u
static const struct { u32 pc, n; } kCbFns[] = {
    { 0x80054688u, 0x1CC },  // psdRealCallBack
    { 0x80054288u, 0x12C },  // dDlst_shadowPoly_c::set
    { 0x803621CCu, 0x20 },   // _savegpr_25 .. 31
    { 0x80362218u, 0x20 },   // _restgpr_25 .. 31
};
#define CB_HASH 0xB085E3771247559Dull  // FNV-1a 64 of kCbFns' bytes in order (HLE_PRINT_HASH=1 prints it)
static int g_cbKnown = -1;
static bool g_cbOn;
u64 g_cbStats[4];  // set a triangle, returned before set (a test), the shadow poly full, not taken
void hle_set_shdw_cb(bool on) { g_cbOn = on; }
bool hle_shdw_cb(void) { return g_cbOn; }
static void cb_invalidate(u32 pa) {
    for (u32 i = 0; i < sizeof kCbFns / sizeof kCbFns[0]; i++)
        if (pa >> 12 == (kCbFns[i].pc & 0x0FFFFFFFu) >> 12 || pa >> 12 == ((kCbFns[i].pc + kCbFns[i].n - 1) & 0x0FFFFFFFu) >> 12) g_cbKnown = -1;
}
static bool cb_known(void) {
    if (g_cbKnown < 0) {
        u64 h = 0xCBF29CE484222325ull;
        bool in = g_mem != NULL;
        for (u32 i = 0; in && i < sizeof kCbFns / sizeof kCbFns[0]; i++) {
            u32 pa = kCbFns[i].pc & 0x0FFFFFFFu;
            if (pa + kCbFns[i].n > g_memSize) { in = false; break; }
            for (u32 k = 0; k < kCbFns[i].n; k++) { h ^= ram_rd8(g_mem, pa + k); h *= 0x100000001B3ull; }
        }
#ifndef __3DS__
        if (getenv("HLE_PRINT_HASH")) fprintf(stderr, "HLE hash of the shadow callback: %016llx\n", (unsigned long long)h);
#endif
        g_cbKnown = in && h == CB_HASH;
    }
    return g_cbKnown == 1;
}
// lfs / stfs of FPR k (the triangle path's f0 versions, for any register).
static inline void cb_lfs(Gekko* g, int k, u32 w) {
    u32 e = w & 0x7F800000u;
    if ((e && e != 0x7F800000u) || !(w << 1)) { float f; memcpy(&f, &w, 4); g->fpr[k].ps0 = g->fpr[k].ps1 = (double)f; return; }
    u64 d = sf_single_to_double(w); fpr_set0(g, k, d); fpr_set1(g, k, d);
}
static inline u32 cb_stfs(const Gekko* g, int k) {
    double x = g->fpr[k].ps0;
    float f = (float)x;
    u32 w; memcpy(&w, &f, 4);
    u32 e = w & 0x7F800000u;
    if ((e && e != 0x7F800000u && (double)f == x) || x == 0.0) return w;
    return sf_double_to_single(fpr_get0(g, k), (GKP(g)->compat & GK_COMPAT_STFS_TINY_ZERO) != 0);
}
// fmuls / fadds / fsubs (op 0 / 1 / 2) of two singles: one rounding, as the JIT; anything else through the handler.
static inline void cb_fs(Gekko* g, int op, int d, int a, int b, u32 pc) {
    double x = g->fpr[a].ps0, y = g->fpr[b].ps0;
    if (!tri_nrm(x) || !tri_nrm(y) || (double)(float)x != x || (double)(float)y != y) { tri_fop(g, pc); return; }
    float r = op == 0 ? (float)(x * y) : op == 1 ? (float)(x + y) : (float)(x - y);
    bool zero = op == 0 ? (x == 0.0 || y == 0.0) : op == 1 ? x == -y : x == y;
    if (!tri_nrmf(r) || (r == 0.0f && !zero)) { tri_fop(g, pc); return; }
    g->fpr[d].ps0 = g->fpr[d].ps1 = (double)r;
}
#define CB_LT (g->cr & 0x80000000u)
#define CB_GT (g->cr & 0x40000000u)
// Undo for TRI_CHECK (PC): the shadow poly bytes the callback wrote, as they were.
static u32 g_cbPoly, g_cbDst;
static u8 g_cbOld[2 + 36];
static bool g_cbSet;

static u32 cb_run(Gekko* g) {
    // Stage by stage: each test reads only what it needs (~77% of calls return at the light test; reading and checking
    // everything first cost ~1,300 ARM instructions and ~43 data micro-TLB misses a call, job 694). Nothing is written
    // until the outcome is known, so any stage may still leave the callback to guest code. f0-f13 are dead when it
    // returns (volatile): the values are C floats; each float read must be zero or normal and each result on the way
    // too, so every operation is one rounding as the interpreter's (and the PSVEC stand-ins' arithmetic for the calls).
    const u32 r1 = g->gpr[1], r2 = g->gpr[2];
    const u32 cbo = g->gpr[3], vtx = g->gpr[4], i1 = g->gpr[5], i2 = g->gpr[6], i3 = g->gpr[7], pla = g->gpr[8];
    g_cbSet = false;
    // ---- the light test: PSVECDotProduct(light, normal) against -0.2 (r2 - 31152) ----
    u8 *fr = tri_p(r1, 160, 8), *pO = tri_p(cbo, 64, 4), *pK = tri_p(r2 - 31192u, 48, 4), *pQ = tri_p(pla, 16, 4);
    if (!fr || !pO || !pK || !pQ || i1 > 2 || i2 > 2 || i3 > 2 || vtx != r1 + 120u || !tri_clear(cbo, 64, r1)) { g_cbStats[3]++; return 0; }
    const u32 lv = ld32(pO + 56);
    u8* pL = tri_p(lv, 12, 4);
    if (!pL || !tri_clear(lv, 12, r1)) { g_cbStats[3]++; return 0; }
    u32 bad = 0;
    float L[3], N[4];
    for (int k = 0; k < 3; k++) { u32 w = ld32(pL + 4 * k); bad |= !tri_plw(w); L[k] = U2Ft(w); }
    for (int k = 0; k < 4; k++) { u32 w = ld32(pQ + 4 * k); bad |= !tri_plw(w); N[k] = U2Ft(w); }
    const u32 kw40 = ld32(pK + 40);
    bad |= !tri_plw(kw40);
    const float l1 = L[1] * N[1], tl = L[2] * N[2], dl = madd1(L[0], N[0], l1) + tl;
    bad |= !tri_nrmf(l1) | !tri_nrmf(tl) | !tri_nrmf(dl);
    if (bad) { g_cbStats[3]++; return 0; }
    u32 n = 21 + 8 + 3;  // ..0x800546bc with _savegpr_26 (7), PSVECDotProduct (8), 0x800546c0..c8
    u32 fc = CMPF(dl, U2Ft(kw40)), cr0 = 0, ctr = g->ctr;
    int out = 0;  // 0 returned (a test), 1 rejected (the box), 2 set, 3 the poly full
    u8 *pP = NULL, *pD = NULL;
    u32 poly = 0, dst = 0, cnt = 0, outw[9];
    if (fc != 8) goto done;
    {   // ---- the plane distance (fmuls / fadds in its order) against -90 (r2 - 31148) ----
        const u32 ce = ld32(pO + 52), kw44 = ld32(pK + 44);
        u8* pC = tri_p(ce, 12, 4);
        if (!pC || !tri_clear(ce, 12, r1) || !tri_plw(kw44)) { g_cbStats[3]++; return 0; }
        float C[3];
        for (int k = 0; k < 3; k++) { u32 w = ld32(pC + 4 * k); bad |= !tri_plw(w); C[k] = U2Ft(w); }
        const float pz = N[2] * C[2], px = N[0] * C[0], py = N[1] * C[1], pxy = px + py, pzxy = pz + pxy, pd = pzxy + N[3];
        bad |= !tri_nrmf(pz) | !tri_nrmf(px) | !tri_nrmf(py) | !tri_nrmf(pxy) | !tri_nrmf(pzxy) | !tri_nrmf(pd);
        if (bad) { g_cbStats[3]++; return 0; }
        n += 17;
        fc = CMPF(pd, U2Ft(kw44));
        if (fc != 4) goto done;
    }
    {   // ---- the box tests: y, x, z; each axis all three below the minimum (r26 + 20) or above the maximum (+ 32) ----
        float MN[3], MX[3], V[3][3];
        for (int k = 0; k < 3; k++) {
            u32 w = ld32(pO + 20 + 4 * k); bad |= !tri_plw(w); MN[k] = U2Ft(w);
            w = ld32(pO + 32 + 4 * k); bad |= !tri_plw(w); MX[k] = U2Ft(w);
            for (int v = 0; v < 3; v++) { w = ld32(fr + 120 + 12 * v + 4 * k); bad |= !tri_plw(w); V[v][k] = U2Ft(w); }
        }
        if (bad) { g_cbStats[3]++; return 0; }
        const float *A = V[i1], *B = V[i2], *Cv = V[i3];
        n += 7;  // 0x80054710..28
        static const u8 ax[3] = { 1, 0, 2 };
        out = -1;
        for (int k = 0; k < 3 && out < 0; k++) {
            const int o = ax[k];
            fc = CMPF(A[o], MN[o]); n += 4;
            if (fc == 8) {
                fc = CMPF(B[o], MN[o]); n += 3;
                if (fc == 8) { fc = CMPF(Cv[o], MN[o]); n += 3; if (fc == 8) { out = 1; break; } }
            }
            fc = CMPF(A[o], MX[o]); n += 3;
            if (fc == 4) {
                fc = CMPF(B[o], MX[o]); n += 3;
                if (fc == 4) { fc = CMPF(Cv[o], MX[o]); n += 3; if (fc == 4) { out = 1; break; } }
            }
        }
        if (out >= 0) goto done;
        // ---- set(poly, vtx, i1, i2, i3, plane): the triangle pushed off the plane into the poly's next slot ----
        poly = ld32(pO + 60);
        pP = tri_p(poly, 8, 4);
        if (!pP) { g_cbStats[3]++; return 0; }
        u8* pV = tri_p(ld32(pP), 16, 4);
        if (!pV) { g_cbStats[3]++; return 0; }
        const u32 fmax = ld32(pV + 12), ftri = ld32(pV + 8);
        u8 *cmax = tri_p(fmax, 8, 4), *ctri = tri_p(ftri, 8, 4);
        if (!cmax || !ctri) { g_cbStats[3]++; return 0; }
        const u32 wm0 = ld32(cmax), wm1 = ld32(cmax + 4), wt0 = ld32(ctri), wt1 = ld32(ctri + 4);
        if ((wm0 >> 16) != 0x3860u || wm1 != 0x4E800020u || (wt0 >> 16) != 0x3863u || wt1 != 0x4E800020u) { g_cbStats[3]++; return 0; }
        const u32 tmax = (u32)(s32)(s16)(wm0 & 0xFFFF), tri0 = poly + (u32)(s32)(s16)(wt0 & 0xFFFF);
        cnt = ld16(pP + 4);
        n += 7 + 5 + 8 + 10 + 2 + 3;  // 0x80054818..30; prologue, _savegpr_25, ..bctrl, getTriMax, the count test
        ctr = fmax;
        cr0 = cmps(cnt, tmax) | (g->xer >> 31);
        if ((s32)cnt >= (s32)tmax) { out = 3; n += 2 + 14 + 1; goto done; }
        dst = tri0 + cnt * 36u;
        pD = tri_p(dst, 36, 4);
        const u32 kw0 = ld32(pK), kw32 = ld32(pK + 32);
        if (!pD || !tri_clear(dst, 36, r1) || !(dst >= poly + 6u || dst + 36u <= poly + 4u) || !tri_plw(kw0) || !tri_plw(kw32))
            { g_cbStats[3]++; return 0; }
        const float K1 = U2Ft(kw0), K2 = U2Ft(kw32);
        const float ay = __builtin_fabsf(N[1]), f3a = K1 - ay, f3 = f3a * f3a, bx = N[0] * f3, bz = N[2] * f3;
        bad |= !tri_nrmf(f3a) | !tri_nrmf(f3) | !tri_nrmf(bx) | !tri_nrmf(bz);
        // PSVECScale(b, b, 2) as stored (psq_st), then PSVECAdd(v_k, b, dst + 12k)
        const u32 sx = PSTFt(bx * K2), sy = PSTFt(N[1] * K2), sz = PSTFt(bz * K2);
        bad |= !tri_plw(sx) | !tri_plw(sy) | !tri_plw(sz);
        const float SX = U2Ft(sx), SY = U2Ft(sy), SZ = U2Ft(sz);
        const u32 idx[3] = { i1, i2, i3 };
        for (int k = 0; k < 3; k++) {
            const float* v = V[idx[k]];
            outw[3 * k] = PSTFt(v[0] + SX); outw[3 * k + 1] = PSTFt(v[1] + SY); outw[3 * k + 2] = PSTFt(v[2] + SZ);
            bad |= !tri_plw(outw[3 * k]) | !tri_plw(outw[3 * k + 1]) | !tri_plw(outw[3 * k + 2]);
        }
        if (bad) { g_cbStats[3]++; return 0; }
        n += 5 + 2 + 22 + 7 + 3 * (6 + 9) + 4 + 14 + 1;  // ..bctrl, getTri, 0x800542ec..340, Scale, the adds, ..398, epilogue, 0x80054834
        ctr = ftri;
        out = 2;
    }
done:
    // ---- the outcome: writes, registers ----
    if (out == 2) {
        if (g_tchkOn) { g_cbPoly = poly; g_cbDst = dst; memcpy(g_cbOld, pP + 4, 2); memcpy(g_cbOld + 2, pD, 36); g_cbSet = true; }
        for (int k = 0; k < 9; k++) TW(pD + 4 * k, outw[k]);
        ramp_wr16a(pP + 4, cnt + 1);  // sth r0,4(r31)
    }
    if (out == 1) n += 2;       // 0x80054810: li r3,1; b
    else if (out == 0) n += 1;  // 0x80054838: li r3,1
    n += 13;                    // 0x8005483c..: _restgpr_26 (7), the epilogue
    TW(fr + 4, TRI_RET);        // stw r0,36(r1) (the frames below r1: dead)
    g_cbStats[out == 2 ? 0 : out == 3 ? 2 : 1]++;
    // CR0: the last compare (set's cmpw, else the last fcmpo); FPSCR[FPCC]: the last fcmpo
    g->cr = (g->cr & 0x0FFFFFFFu) | ((out >= 2 ? cr0 : fc) << 28);
    g->fpscr = (g->fpscr & ~FPSCR_FPCC) | (fc << 12);
    g->ctr = ctr;
    g->gpr[11] = r1;
    g->lr = TRI_RET;
    // ---- ShdwDraw 0x8007fe98: lis r3,-32710; addi r0,r3,14272; stw r0,96(r1) ----
    g->gpr[3] = 0x803A0000u; g->gpr[0] = 0x803A37C0u;
    TW(fr + 96, 0x803A37C0u);
    n += 3;
    g->pc = PC_NEXT;
    return n;
}


static u32 tri_run(Gekko* g) {
    g_cbRan = false;
    g_shWhere = 3;
    // The state the PSVEC stand-ins and the float handlers assume (hle_ps.c ps_call), and no enabled FP exceptions.
    if (!(g->msr & MSR_DR) || !(g->msr & MSR_FP) || (g->msr & (MSR_FE0 | MSR_FE1)) || (g->fpscr & FPSCR_RN) ||
        (g->spr[SPR_HID2] & (HID2_PSE | HID2_LSQE)) != (HID2_PSE | HID2_LSQE) || (g->spr[SPR_GQR0] & 0x00070007u))
        { g_triStats[3]++; g_triWhy[0]++; return 0; }
    const u32 r1 = g->gpr[1], r15 = g->gpr[15], r16 = g->gpr[16];
    u8 *fr = tri_p(r1 - 64u, 64 + 2212, 8), *pt = tri_p(r16, 2, 2), *ph = tri_p(r15 + 24u, 8, 4);
    if (!fr || !pt || !ph) { g_triStats[3]++; g_triWhy[1]++; return 0; }
    fr += 64;  // fr[o]: r1 + o
    // ---- reads ----
    const u32 tri = ld16(pt), head = ld32(ph), ctb = ld32(ph + 4);  // lhz 0(r16); lwz 24(r15); lwz 28(r15)
    u8* pH = tri_p(head, 12, 4);
    if (!pH) { g_triStats[3]++; g_triWhy[2]++; return 0; }
    const u32 posb = ld32(pH), nrmb = ld32(pH + 4), pb = ld32(pH + 8);
    const u32 prism = pb + (tri << 4);
    u8* pP = tri_p(prism, 16, 4);
    if (!pP || !tri_clear(prism, 16, r1)) { g_triStats[3]++; g_triWhy[3]++; return 0; }
    const u32 ai = ld16(pP + 14);
    const u32 cp = ctb + 8u + ai * 20u;  // 800740c4
    u8* pc5 = tri_p(cp, 20, 4);
    if (!pc5 || !tri_clear(cp, 20, r1)) { g_triStats[3]++; g_triWhy[4]++; return 0; }
    u32 cw[5];
    for (int k = 0; k < 5; k++) cw[k] = ld32(pc5 + 4 * k);
    const u32 through = (cw[0] & 0x00400000u) ? 1u : (cw[0] >> 17) & 1u;
    u32 posi = 0, fni = 0, e1 = 0, e2 = 0, e3 = 0, pp = 0, n1 = 0, n2 = 0, n3 = 0, n4 = 0, hgt = 0, eps = 0, pw[3] = { 0 }, nw[3] = { 0 },
        cbo = 0, cbf = 0;
    if (!through) {
        posi = ld16(pP + 4); fni = ld16(pP + 6); e1 = ld16(pP + 8); e2 = ld16(pP + 10); e3 = ld16(pP + 12); hgt = ld32(pP);
        pp = posb + posi * 12u; n1 = nrmb + fni * 12u; n2 = nrmb + e1 * 12u; n3 = nrmb + e3 * 12u; n4 = nrmb + e2 * 12u;
        u8 *a = tri_p(pp, 12, 4), *b = tri_p(n1, 12, 4), *e = tri_p(g->gpr[13] - 29696u, 4, 4), *o = tri_p(ld32(fr + 8), 52, 4);
        if (!a || !b || !e || !o || !tri_p(n2, 12, 4) || !tri_p(n3, 12, 4) || !tri_p(n4, 12, 4) || !tri_clear(pp, 12, r1) ||
            !tri_clear(n1, 12, r1) || !tri_clear(n2, 12, r1) || !tri_clear(n3, 12, r1) || !tri_clear(n4, 12, r1) ||
            !tri_clear(g->gpr[13] - 29696u, 4, r1) || !tri_clear(ld32(fr + 8), 52, r1)) { g_triStats[3]++; g_triWhy[5]++; return 0; }
        for (int k = 0; k < 3; k++) { pw[k] = ld32(a + 4 * k); nw[k] = ld32(b + 4 * k); }
        eps = ld32(e); cbo = ld32(fr + 8); cbf = ld32(o + 48);
    }
    // ---- the fast form (v4): every input loaded first, both cross / dot / scale / add chains computed side by side in
    // registers (the VFP's latency overlapped: one chain after another, through memory, cost ~4,200 cycles a triangle,
    // jobs 681-682), then each exit's final frame words written once. Same operations in the same order per value as the
    // PSVEC stand-ins (hle_psops.h), so the same bits; taken only when every input and result on the path is zero or
    // normal (PSTF's flush, the dot tests, the divides), else the form below, one PSVEC call at a time, runs.
    if (!through && g_triFast) {
        const u8 *qa = g_mem + (n1 - 0x80000000u), *qb = g_mem + (n2 - 0x80000000u), *qc = g_mem + (n3 - 0x80000000u),
                 *qd = g_mem + (n4 - 0x80000000u);
        u32 wa[3], wb[3], wc[3], wd[3];
        u32 bad = !tri_plw(hgt) | !tri_plw(eps);
        for (int k = 0; k < 3; k++) {
            wa[k] = ld32(qa + 4 * k); wb[k] = ld32(qb + 4 * k); wc[k] = ld32(qc + 4 * k); wd[k] = ld32(qd + 4 * k);
            bad |= !tri_plw(wa[k]) | !tri_plw(wb[k]) | !tri_plw(wc[k]) | !tri_plw(wd[k]) | !tri_plw(pw[k]);
        }
        if (!bad) {
            const float a0 = U2Ft(wa[0]), a1 = U2Ft(wa[1]), a2 = U2Ft(wa[2]);  // n1
            const float b0 = U2Ft(wb[0]), b1 = U2Ft(wb[1]), b2 = U2Ft(wb[2]);  // n2
            const float c0 = U2Ft(wc[0]), c1 = U2Ft(wc[1]), c2 = U2Ft(wc[2]);  // n3
            const float d0 = U2Ft(wd[0]), d1 = U2Ft(wd[1]), d2 = U2Ft(wd[2]);  // n4
            const float p0 = U2Ft(pw[0]), p1 = U2Ft(pw[1]), p2 = U2Ft(pw[2]), h = U2Ft(hgt), ep = U2Ft(eps);
            // PSVECCrossProduct(n1, n2) and (n4, n1), as stored (PSTF)
            const u32 x1 = PSTFt(msub1(a1, b2, b1 * a2)), y1 = PSTFt(-msub1(a0, b2, b0 * a2)), z1 = PSTFt(-msub1(a1, b0, b1 * a0));
            const u32 x2 = PSTFt(msub1(d1, a2, a1 * d2)), y2 = PSTFt(-msub1(d0, a2, a0 * d2)), z2 = PSTFt(-msub1(d1, a0, a1 * d0));
            const float X1 = U2Ft(x1), Y1 = U2Ft(y1), Z1 = U2Ft(z1), X2 = U2Ft(x2), Y2 = U2Ft(y2), Z2 = U2Ft(z2);
            // PSVECDotProduct(cross, n3): f1 = { madd(x, c0, y * c1) + z * c2, z * c2 }
            const float t1 = Z1 * c2, t2 = Z2 * c2;
            const float dot1 = madd1(X1, c0, Y1 * c1) + t1, dot2 = madd1(X2, c0, Y2 * c1) + t2;
            const float ad1 = __builtin_fabsf(dot1), ad2 = __builtin_fabsf(dot2);
            // GetTriPla's plane: n1 copied, d = -PSVECDotProduct(n1, pos)
            const float tN = a2 * p2, dN = madd1(a0, p0, a1 * p1) + tN;
            bad = !tri_nrmf(dot1) | !tri_nrmf(t1) | !tri_plw(x1) | !tri_plw(y1) | !tri_plw(z1);
            u32 ex = 0;  // 1: skip at the first test, 2: at the second, 3: to the callback
            u32 s1[3] = { 0 }, v2[3] = { 0 }, s2[3] = { 0 }, v1[3] = { 0 };
            if (!bad) {
                if (ad1 < ep) ex = 1;
                else {
                    const float q1 = h / dot1;
                    const u32 sx = PSTFt(X1 * q1), sy = PSTFt(Y1 * q1), sz = PSTFt(Z1 * q1);
                    s1[0] = sx; s1[1] = sy; s1[2] = sz;
                    v2[0] = PSTFt(U2Ft(sx) + p0); v2[1] = PSTFt(U2Ft(sy) + p1); v2[2] = PSTFt(U2Ft(sz) + p2);
                    bad |= !tri_nrmf(q1) | !tri_plw(sx) | !tri_plw(sy) | !tri_plw(sz) | !tri_plw(v2[0]) | !tri_plw(v2[1]) | !tri_plw(v2[2]);
                    bad |= !tri_nrmf(dot2) | !tri_nrmf(t2) | !tri_plw(x2) | !tri_plw(y2) | !tri_plw(z2);
                    if (ad2 < ep) ex = 2;
                    else {
                        const float q2 = h / dot2;
                        const u32 ux = PSTFt(X2 * q2), uy = PSTFt(Y2 * q2), uz = PSTFt(Z2 * q2);
                        s2[0] = ux; s2[1] = uy; s2[2] = uz;
                        v1[0] = PSTFt(U2Ft(ux) + p0); v1[1] = PSTFt(U2Ft(uy) + p1); v1[2] = PSTFt(U2Ft(uz) + p2);
                        bad |= !tri_nrmf(q2) | !tri_plw(ux) | !tri_plw(uy) | !tri_plw(uz) | !tri_plw(v1[0]) | !tri_plw(v1[1]) | !tri_plw(v1[2]);
                        bad |= !tri_nrmf(dN) | !tri_nrmf(tN);
                        ex = 3;
                    }
                }
            }
            if (!bad) {
                g_triFastN++;
                const u32 r31 = g->gpr[31];
                // getPolyCode, ChkShdwDrawThrough (the words the call leaves: see the form below)
                TW(fr - 32, r1); TW(fr + 4, 0x8007FCF4u); TW(fr - 4, r31); TW(fr - 24, cp);
                for (int k = 0; k < 5; k++) TW(fr + 100 + 4 * k, cw[k]);
                for (int k = 0; k < 3; k++) TW(fr + 120 + 4 * k, pw[k]);
                TW(fr + 2208, n1); TW(fr + 2204, n3);
                g->gpr[31] = prism;
                u32 n = 52 + 31 + 15 + 3 + 8 + 5;
                g->gpr[3] = r1 + 24u; g->gpr[4] = n3; g->gpr[5] = r1 + 24u; g->gpr[0] = e1 * 12u;
                g->lr = 0x8007FD90u;
                const float dt = ex == 1 ? dot1 : dot2, tt = ex == 1 ? t1 : t2;
                if (ex <= 2) {
                    if (ex == 1) { TW(fr + 24, x1); TW(fr + 28, y1); TW(fr + 32, z1); }
                    else {
                        TW(fr + 24, s1[0]); TW(fr + 28, s1[1]); TW(fr + 32, s1[2]);
                        TW(fr + 144, v2[0]); TW(fr + 148, v2[1]); TW(fr + 152, v2[2]);
                        TW(fr + 12, x2); TW(fr + 16, y2); TW(fr + 20, z2);
                        n += 5 + 7 + 4 + 9 + 8 + 15 + 3 + 8 + 5;
                        g->gpr[0] = e2 * 12u; g->gpr[3] = r1 + 12u; g->gpr[5] = r1 + 12u; g->lr = 0x8007FDF4u;
                    }
                    // fabs f0,f1; frsp f2,f0; lfs f0,eps; fcmpo cr0,f2,f0 (less: the skip)
                    g->fpr[1].ps0 = (double)dt; g->fpr[1].ps1 = (double)tt;
                    g->fpr[2].ps0 = g->fpr[2].ps1 = (double)__builtin_fabsf(dt);
                    g->fpr[0].ps0 = g->fpr[0].ps1 = (double)ep;
                    g->cr = (g->cr & 0x0FFFFFFFu) | (8u << 28);
                    g->fpscr = (g->fpscr & ~FPSCR_FPCC) | (8u << 12);
                    g->pc = PC_NEXT;
                    g_triStats[1]++;
                    return n;
                }
                // to the callback: both halves, GetTriPla, the plane
                TW(fr + 24, s1[0]); TW(fr + 28, s1[1]); TW(fr + 32, s1[2]);
                TW(fr + 144, v2[0]); TW(fr + 148, v2[1]); TW(fr + 152, v2[2]);
                TW(fr + 12, s2[0]); TW(fr + 16, s2[1]); TW(fr + 20, s2[2]);
                TW(fr + 132, v1[0]); TW(fr + 136, v1[1]); TW(fr + 140, v1[2]);
                n += 5 + 7 + 4 + 9 + 8 + 15 + 3 + 8 + 5 + 5 + 7 + 4 + 9;
                const u32 vt = 0x803A37C0u;
                const u32 dw = bitsft(-dN);  // (stfs of the negated dot, a single: its bits)
                TW(fr + 96, vt); TW(fr - 48, r1); TW(fr + 4, 0x8007FE48u);
                TW(fr - 12, g->gpr[29]); TW(fr - 8, g->gpr[30]); TW(fr - 4, prism);
                TW(fr - 40, n1); TW(fr - 20, vt); TW(fr - 64, r1 - 48u); TW(fr - 44, 0x8007E938u); TW(fr - 52, pp);
                TW(fr - 36, wa[0]); TW(fr - 32, wa[1]); TW(fr - 28, wa[2]); TW(fr - 24, dw);
                TW(fr + 76, vt);
                TW(fr + 60, wa[0]); TW(fr + 64, wa[1]); TW(fr + 68, wa[2]); TW(fr + 72, dw);
                TW(fr + 80, wa[0]); TW(fr + 84, wa[1]); TW(fr + 88, wa[2]); TW(fr + 92, dw);
                n += 7 + 99 + 20;
                g->fpr[1].ps0 = (double)dN; g->fpr[1].ps1 = (double)tN;
                g->fpr[0].ps0 = g->fpr[0].ps1 = (double)(-dN);
                g->fpr[2].ps0 = g->fpr[2].ps1 = (double)ad2;
                g->cr = (g->cr & 0x0FFFFFFFu) | ((ad2 > ep ? 4u : 2u) << 28);
                g->fpscr = (g->fpscr & ~FPSCR_FPCC) | ((ad2 > ep ? 4u : 2u) << 12);
                g->gpr[0] = vt; g->gpr[3] = cbo; g->gpr[4] = r1 + 120u; g->gpr[5] = 0; g->gpr[6] = 1; g->gpr[7] = 2; g->gpr[8] = r1 + 80u;
                g->gpr[11] = r1; g->gpr[12] = cbf;
                g->ctr = cbf; g->lr = TRI_RET; g->pc = cbf & ~3u;
                g_triStats[0]++;
                if (g_cbOn && g->pc == PC_CB && cb_known()) { g_shWhere = 6; if (hle_shdw_cb_begin_hook) hle_shdw_cb_begin_hook(); u32 m = cb_run(g); if (hle_shdw_cb_end_hook) hle_shdw_cb_end_hook(); if (m) { n += m; g_cbRan = true; } }
                return n;
            }
        }
    }
    if (__builtin_expect(g_triStop == 1, 0)) return 1;  // (tri_bench: the cost up to here)
    // ---- 0x8007fce4: getPolyCode(r15, tri, r1 + 100) ----
    const u32 r31 = g->gpr[31];
    TW(fr - 32, r1);            // stwu r1,-32(r1)
    TW(fr + 4, 0x8007FCF4u);    // stw r0,36(r1)
    TW(fr - 4, r31);            // stw r31,28(r1)
    TW(fr - 24, cp);            // stw r3,0(r5) (800740d4)
    for (int k = 0; k < 5; k++) TW(fr + 100 + 4 * k, cw[k]);  // 80074048
    // ---- 0x8007fcf4: ChkShdwDrawThrough(r15, r1 + 100); 0x8007fd00: clrlwi. r0,r3,24; bne 0x8007fea4 ----
    u32 n = 52;
    g->gpr[0] = through; g->gpr[3] = through; g->gpr[4] = r1 + 100u; g->gpr[5] = r1 - 24u; g->lr = 0x8007FD00u;
    tri_cr0(g, through);
    if (through) { g->pc = PC_NEXT; g_triStats[1]++; return n; }
    if (__builtin_expect(g_triStop == 2, 0)) return n;  // (tri_bench: the cost up to here)
    // ---- 0x8007fd08: the vertex, the normals; PSVECCrossProduct(n1, n2, r1 + 24); PSVECDotProduct(r1 + 24, n3) ----
    for (int k = 0; k < 2; k++) TW(fr + 120 + 4 * k, tri_mov(g, pw[k]));
    tri_lfs0(g, pw[2]); TW(fr + 128, tri_stfs0(g));
    TW(fr + 2208, n1); TW(fr + 2204, n3);
    g->gpr[31] = prism;
    g->gpr[0] = e1 * 12u; g->gpr[3] = n1; g->gpr[4] = n2; g->gpr[5] = r1 + 24u;
    g_shWhere = 4;
    n += 31 + tri_ps(g, 0, 0x8007FD84u);
    g->gpr[3] = r1 + 24u; g->gpr[4] = n3;
    n += 3 + tri_ps(g, 1, 0x8007FD90u);
    tri_fabs(g, 0, 1, 0x8007FD90u); tri_frsp(g, 2, 0, 0x8007FD94u);  // fabs f0,f1; frsp f2,f0
    tri_lfs0(g, eps); tri_fcmpo0(g, 2, 0, 0x8007FD9Cu);              // lfs f0,-29696(r13); fcmpo cr0,f2,f0
    n += 5;
    if (g->cr & 0x80000000u) { g->pc = PC_NEXT; g_triStats[1]++; return n; }
    if (__builtin_expect(g_triStop == 3, 0)) return n;  // (tri_bench: the cost up to here)
    // ---- 0x8007fda4: PSVECScale(r1 + 24, r1 + 24, height / dot); PSVECAdd(r1 + 24, r1 + 120, r1 + 144) ----
    g->gpr[3] = r1 + 24u; g->gpr[4] = r1 + 24u;
    tri_lfs0(g, hgt); tri_fdivs(g, 1, 0, 1, 0x8007FDB0u);  // lfs f0,0(r31); fdivs f1,f0,f1
    n += 5 + tri_ps(g, 2, 0x8007FDB8u);
    g->gpr[3] = r1 + 24u; g->gpr[4] = r1 + 120u; g->gpr[5] = r1 + 144u;
    n += 4 + tri_ps(g, 3, 0x8007FDC8u);
    // ---- 0x8007fdc8: PSVECCrossProduct(n4, n1, r1 + 12); PSVECDotProduct(r1 + 12, n3) ----
    g->gpr[0] = e2 * 12u; g->gpr[3] = n4; g->gpr[4] = n1; g->gpr[5] = r1 + 12u;
    n += 8 + tri_ps(g, 0, 0x8007FDE8u);
    g->gpr[3] = r1 + 12u; g->gpr[4] = n3;
    n += 3 + tri_ps(g, 1, 0x8007FDF4u);
    tri_fabs(g, 0, 1, 0x8007FDF4u); tri_frsp(g, 2, 0, 0x8007FDF8u);
    tri_lfs0(g, eps); tri_fcmpo0(g, 2, 0, 0x8007FE00u);
    n += 5;
    if (g->cr & 0x80000000u) { g->pc = PC_NEXT; g_triStats[1]++; return n; }
    if (__builtin_expect(g_triStop == 4, 0)) return n;  // (tri_bench: the cost up to here)
    // ---- 0x8007fe08: PSVECScale(r1 + 12, r1 + 12, height / dot); PSVECAdd(r1 + 12, r1 + 120, r1 + 132) ----
    g->gpr[3] = r1 + 12u; g->gpr[4] = r1 + 12u;
    tri_lfs0(g, hgt); tri_fdivs(g, 1, 0, 1, 0x8007FE14u);
    n += 5 + tri_ps(g, 2, 0x8007FE1Cu);
    g->gpr[3] = r1 + 12u; g->gpr[4] = r1 + 120u; g->gpr[5] = r1 + 132u;
    n += 4 + tri_ps(g, 3, 0x8007FE2Cu);
    g_shWhere = 5;
    // ---- 0x8007fe2c: GetTriPla(r1 + 60, r15, tri) ----
    const u32 vt = 0x803A37C0u;  // (lis r3,-32710; addi r0,r3,14272)
    TW(fr + 96, vt);
    TW(fr - 48, r1);              // stwu r1,-48(r1)
    TW(fr + 4, 0x8007FE48u);      // stw r0,52(r1)
    TW(fr - 12, g->gpr[29]); TW(fr - 8, g->gpr[30]); TW(fr - 4, prism);  // _savegpr_29
    TW(fr - 40, n1);              // 8007e864: stw r0,0(r5)
    // 8007e918: PSVECDotProduct(n1, pp): only f1, which the next one overwrites; counted
    TW(fr - 20, vt);              // 8007e924: stw r0,28(r1)
    // 8026f52c: cM3dGPla(r1 - 36, n1, pp)
    TW(fr - 64, r1 - 48u);        // stwu r1,-16(r1)
    TW(fr - 44, 0x8007E938u);     // stw r0,20(r1)
    TW(fr - 52, pp);              // stw r31,12(r1)
    for (int k = 0; k < 2; k++) TW(fr - 36 + 4 * k, tri_mov(g, nw[k]));
    tri_lfs0(g, nw[2]); TW(fr - 28, tri_stfs0(g));
    g->gpr[3] = r1 - 36u; g->gpr[4] = pp;
    u32 nd = tri_ps(g, 1, 0x8026F560u);
    tri_fneg(g, 0, 1, 0x8026F560u);  // fneg f0,f1
    TW(fr - 24, tri_stfs0(g));    // stfs f0,12(r31)
    TW(fr + 76, vt);              // 8007e940: stw r0,16(r29)
    for (int k = 0; k < 3; k++) TW(fr + 60 + 4 * k, tri_mov(g, ld32(fr - 36 + 4 * k)));
    tri_lfs0(g, ld32(fr - 24)); TW(fr + 72, tri_stfs0(g));
    TW(fr - 20, vt);              // 8007e964: stw r0,28(r1)
    n += 7 + 99 - 8 + nd;  // fe2c..fe44, and GetTriPla's 99 with the second dot product as it ran (the first, 8, counted as run)
    if (__builtin_expect(g_triStop == 5, 0)) return n;  // (tri_bench: the cost up to here)
    // ---- 0x8007fe48: the plane to r1 + 80; the callback(r1[8], r1 + 120, 0, 1, 2, r1 + 80) ----
    for (int k = 0; k < 3; k++) TW(fr + 80 + 4 * k, tri_mov(g, ld32(fr + 60 + 4 * k)));
    tri_lfs0(g, ld32(fr + 72)); TW(fr + 92, tri_stfs0(g));
    TW(fr + 76, vt);
    g->gpr[0] = vt; g->gpr[3] = cbo; g->gpr[4] = r1 + 120u; g->gpr[5] = 0; g->gpr[6] = 1; g->gpr[7] = 2; g->gpr[8] = r1 + 80u;
    g->gpr[11] = r1; g->gpr[12] = cbf; g->gpr[31] = prism;
    g->ctr = cbf; g->lr = TRI_RET; g->pc = cbf & ~3u;
    n += 20;
    g_triStats[0]++;
    if (g_cbOn && g->pc == PC_CB && cb_known()) { g_shWhere = 6; if (hle_shdw_cb_begin_hook) hle_shdw_cb_begin_hook(); u32 m = cb_run(g); if (hle_shdw_cb_end_hook) hle_shdw_cb_end_hook(); if (m) { n += m; g_cbRan = true; } }
    return n;
}
#undef TW

#ifndef __3DS__
// TRI_CHECK=1: every run also runs in the interpreter from the same state for the same instruction count; compared: PC,
// GPRs, CR, XER, LR, CTR, f0, f1, f14-f31 (both lanes; f2-f13 are volatile, dead: the PSVEC stand-ins leave them), and the
// frame window [r1 - 64, r1 + 2240). FPSCR differences are counted apart (the PSVEC stand-ins leave FPRF as it was). The
// interpreter's result stands.
static struct { u64 calls, bad, fpscr, cb; } g_tchk;
static void tchk_report(void) {
    fprintf(stderr, "TRI_CHECK refusals: %llu %llu %llu %llu %llu %llu\n", (unsigned long long)g_triWhy[0], (unsigned long long)g_triWhy[1], (unsigned long long)g_triWhy[2], (unsigned long long)g_triWhy[3], (unsigned long long)g_triWhy[4], (unsigned long long)g_triWhy[5]);
    fprintf(stderr, "TRI_CHECK callback: %llu runs through it; set %llu, returned %llu, full %llu, not taken %llu\n", (unsigned long long)g_tchk.cb, (unsigned long long)g_cbStats[0], (unsigned long long)g_cbStats[1], (unsigned long long)g_cbStats[2], (unsigned long long)g_cbStats[3]);
    fprintf(stderr, "TRI_CHECK fast form: %llu runs\n", (unsigned long long)g_triFastN);
    fprintf(stderr, "TRI_CHECK: %llu runs compared with the interpreter, %llu differed, %llu with another FPSCR; to the callback %llu, skipped %llu, refused %llu\n",
            (unsigned long long)g_tchk.calls, (unsigned long long)g_tchk.bad, (unsigned long long)g_tchk.fpscr,
            (unsigned long long)g_triStats[0], (unsigned long long)g_triStats[1], (unsigned long long)g_triStats[3]);
}
static u32 tri_checked(Gekko* g) {
    enum { LO = 128, W = 128 + 2240 };
    u32 r1 = g->gpr[1];
    u8* w = tri_p(r1 - LO, W, 4);
    if (!w) return tri_run(g);
    static u8 w0[W], wn[W];
    Gekko g0 = *g;
    memcpy(w0, w, W);
    u32 n = tri_run(g);
    if (!n) return 0;
    Gekko gn = *g;
    memcpy(wn, w, W);
    bool cbm = g_cbRan;
    u8 pn[38], *pc = NULL, *pd = NULL;
    if (cbm && g_cbSet) {  // the poly's count and triangle as the native run left them, then as they were
        pc = g_mem + (g_cbPoly + 4u - 0x80000000u); pd = g_mem + (g_cbDst - 0x80000000u);
        memcpy(pn, pc, 2); memcpy(pn + 2, pd, 36);
        memcpy(pc, g_cbOld, 2); memcpy(pd, g_cbOld + 2, 36);
    }
    *g = g0;
    memcpy(w, w0, W);
    u32 steps = 0;
    while (steps < n) {
        bool stop;
        g->pc = gekko_interp_one(g, g->pc, ram_rd32(g_mem, g->pc & 0x0FFFFFFFu), &stop);
        steps++;
    }
    g_tchk.calls++;
    if (cbm) g_tchk.cb++;
    bool fsame = true;
    static const int fk[] = { 0, 1, 14, 15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, 26, 27, 28, 29, 30, 31 };
    for (u32 i = cbm ? 2 : 0; i < sizeof fk / sizeof fk[0]; i++)
        if (fpr_get0(g, fk[i]) != fpr_get0(&gn, fk[i]) || fpr_get1(g, fk[i]) != fpr_get1(&gn, fk[i])) fsame = false;
    bool gsame = true;
    for (int k = 0; k < 32; k++) {
        bool dead = cbm && ((k >= 4 && k <= 10) || k == 12);
        if (!dead && g->gpr[k] != gn.gpr[k]) gsame = false;
    }
    u32 wlo = cbm ? LO : LO - 64;  // (with the callback: from r1 up; the path alone: its callees' frames too)
    bool same = g->pc == gn.pc && gsame && g->cr == gn.cr && g->xer == gn.xer && g->lr == gn.lr && g->ctr == gn.ctr &&
                fsame && !memcmp(w + wlo, wn + wlo, W - wlo) && (!pc || (!memcmp(pc, pn, 2) && !memcmp(pd, pn + 2, 36)));
    if (g->fpscr != gn.fpscr) g_tchk.fpscr++;
    if (!same && g_tchk.bad++ < 20) {
        fprintf(stderr, "TRI_CHECK run %llu (r15 %08x, callback %d): native %u insns to %08x, interpreter to %08x; cr %08x/%08x lr %08x/%08x ctr %08x/%08x\n",
                (unsigned long long)g_tchk.calls, g0.gpr[15], cbm, n, gn.pc, g->pc, gn.cr, g->cr, gn.lr, g->lr, gn.ctr, g->ctr);
        for (int k = 0; k < 32; k++)
            if (g->gpr[k] != gn.gpr[k]) fprintf(stderr, "  r%d native %08x interpreter %08x\n", k, gn.gpr[k], g->gpr[k]);
        for (u32 i = cbm ? 2 : 0; i < sizeof fk / sizeof fk[0]; i++)
            if (fpr_get0(g, fk[i]) != fpr_get0(&gn, fk[i]) || fpr_get1(g, fk[i]) != fpr_get1(&gn, fk[i]))
                fprintf(stderr, "  f%d native %016llx/%016llx interpreter %016llx/%016llx\n", fk[i], (unsigned long long)fpr_get0(&gn, fk[i]),
                        (unsigned long long)fpr_get1(&gn, fk[i]), (unsigned long long)fpr_get0(g, fk[i]), (unsigned long long)fpr_get1(g, fk[i]));
        for (u32 k = wlo; k < W; k += 4)
            if (memcmp(w + k, wn + k, 4)) fprintf(stderr, "  r1%+d native %08x interpreter %08x\n", (int)k - LO, ld32(wn + k), ld32(w + k));
        if (pc && (memcmp(pc, pn, 2) || memcmp(pd, pn + 2, 36))) fprintf(stderr, "  the shadow poly's bytes differ\n");
    }
    return steps;
}
#endif
// The path at g->pc == PC_TRI: instructions stood in for (0: guest code runs it).
static u32 tri_go(Gekko* g) {
#ifndef __3DS__
    static int chk = -1;
    if (chk < 0) { chk = getenv("TRI_CHECK") != NULL; if (chk) atexit(tchk_report); g_tchkOn = chk; }
    if (chk) return tri_checked(g);
#endif
    return tri_run(g);
}


// --tribench N (measurement): at the Nth triangle path, its pieces are timed on that triangle's state, each N_TB times
// (state and frame restored before each): the restore alone, the whole path, the eight PSVEC operations, the float
// helpers, the moves (lfs / stfs pairs), and the callback. g_triBenchT: clock ticks for each loop; the frame and the
// registers are put back afterwards and the walk goes on as if nothing ran.
u32 g_triBench, g_triBenchN;
u64 (*hle_bench_clock)(void);
u64 g_triBenchT[16];
bool g_triBenchDone;
#define N_TB 4000u
static __attribute__((unused)) void tri_bench(Gekko* g) {
    enum { LO = 128, W = 128 + 2240 };
    const u32 r1 = g->gpr[1];
    u8* w = tri_p(r1 - LO, W, 8);
    if (!w || !hle_bench_clock || !tri_known()) return;
    static u8 w0[W], w1[W];
    static Gekko g0, g1;
    g0 = *g;
    memcpy(w0, w, W);
    bool cb0 = g_cbOn;
    g_cbOn = false;
    u64 t = hle_bench_clock();
    for (u32 i = 0; i < N_TB; i++) { *g = g0; memcpy(w, w0, W); }
    g_triBenchT[0] = hle_bench_clock() - t;
    t = hle_bench_clock();
    for (u32 i = 0; i < N_TB; i++) { *g = g0; memcpy(w, w0, W); tri_run(g); }
    g_triBenchT[1] = hle_bench_clock() - t;
    g1 = *g;  // (after a run: the callback's entry state, if it got there)
    memcpy(w1, w, W);
    u32 n1 = ld32(w + LO + 2208), n3 = ld32(w + LO + 2204);
    if (tri_p(n1, 12, 4) && tri_p(n3, 12, 4)) {
        t = hle_bench_clock();
        for (u32 i = 0; i < N_TB; i++) {
            for (int h = 0; h < 2; h++) {
                u32 o = h ? 12u : 24u;
                g->gpr[3] = n1; g->gpr[4] = n3; g->gpr[5] = r1 + o; tri_ps(g, 0, 0);
                g->gpr[3] = r1 + o; g->gpr[4] = n3; tri_ps(g, 1, 0);
                g->fpr[1].ps0 = g->fpr[1].ps1 = 0.5; g->gpr[3] = r1 + o; g->gpr[4] = r1 + o; tri_ps(g, 2, 0);
                g->gpr[3] = r1 + o; g->gpr[4] = r1 + 120u; g->gpr[5] = r1 + (h ? 132u : 144u); tri_ps(g, 3, 0);
            }
        }
        g_triBenchT[2] = hle_bench_clock() - t;
    }
    t = hle_bench_clock();
    for (u32 i = 0; i < N_TB; i++) {
        for (int h = 0; h < 2; h++) {
            g->fpr[1].ps0 = g->fpr[1].ps1 = 0.75;
            tri_fabs(g, 0, 1, 0x8007FD90u); tri_frsp(g, 2, 0, 0x8007FD94u);
            tri_lfs0(g, 0x358637BDu); tri_fcmpo0(g, 2, 0, 0x8007FD9Cu);
            tri_lfs0(g, 0x41200000u); tri_fdivs(g, 1, 0, 1, 0x8007FDB0u);
        }
        tri_fneg(g, 0, 1, 0x8026F560u);
    }
    g_triBenchT[3] = hle_bench_clock() - t;
    t = hle_bench_clock();
    for (u32 i = 0; i < N_TB; i++) {
        u8* fr = w + LO;
        for (int k = 0; k < 13; k++) ramp_wr32a(fr + 120 + 4 * (k % 9), tri_mov(g, ld32(fr + 60 + 4 * (k % 4))));
        for (int k = 0; k < 4; k++) { tri_lfs0(g, ld32(fr + 72)); ramp_wr32a(fr + 92, tri_stfs0(g)); }
    }
    g_triBenchT[4] = hle_bench_clock() - t;
    if (g1.pc == PC_CB && cb_known()) {
        t = hle_bench_clock();
        bool tk = g_tchkOn;
        g_tchkOn = true;  // (cb_run keeps the poly bytes it overwrites: put back after each run)
        for (u32 i = 0; i < N_TB; i++) {
            *g = g1; memcpy(w, w1, W); cb_run(g);
            if (g_cbSet) { memcpy(g_mem + (g_cbPoly + 4u - 0x80000000u), g_cbOld, 2); memcpy(g_mem + (g_cbDst - 0x80000000u), g_cbOld + 2, 36); }
        }
        g_triBenchT[5] = hle_bench_clock() - t;
        g_tchkOn = tk;
    }
    for (u8 k = 1; k <= 5; k++) {  // the path to each stop point
        g_triStop = k;
        t = hle_bench_clock();
        for (u32 i = 0; i < N_TB; i++) { *g = g0; memcpy(w, w0, W); tri_run(g); }
        g_triBenchT[5 + k] = hle_bench_clock() - t;
    }
    g_triStop = 0;
    g_cbOn = cb0;
    *g = g0;
    memcpy(w, w0, W);
    g_triBenchDone = true;
}

u32 hle_run(Gekko* g, u32 budget) {
#ifndef __3DS__
    bs_hook(g);
    if (g_bsOn > 0 && (g->pc == PC_BODY || g->pc == PC_CBK)) return 0;  // (observed only: guest code runs)
#endif
    if (g_gfskipAddr && g->pc == g_gfskipAddr) {
        if (g_gfskipOn && gfskip_guarded()) { g_gfskipGuarded++; g_gfskipStats[0]++; return 0; }  // a fade: draw every frame
        if (g_gfskipOn && (++g_gfskipN % g_gfskipEvery) != 0) { g->pc = g->lr; g_gfskipStats[1]++; return 1; }  // as a blr
        g_gfskipStats[0]++;
        return 0;  // declined: the renderer runs
    }
    if (g_memoAddr && g->pc == g_memoAddr) return memo_probe(g);
    if (g_paneStat && g->pc == 0x802F67E0u) { pane_note(g); return 0; }  // (counted; the guest code runs)
    if (port_is_hook(g->pc)) { u32 n = port_run_hook(g); if (n) return n; }  // (not taken: the guest code runs)
    if (!g_on) return 0;
    if (g->pc != PC_ENTRY && g->pc != PC_NEXT && g->pc != PC_LOOP) {
        u32 n = hle_ps_run(g);  // (first: on the PC it checks the generated GD writers before hle_gd.c's own)
        if (!n) n = hle_gd_run(g);
        if (!n) n = hle_thp_run(g);
        if (n) { g_hleRuns++; g_hleInsns += n; }
        return n;
    }
    if (!known()) return 0;
#ifndef __3DS__
    static int env = -1;
    if (env < 0) { env = 1; if (getenv("GC3DS_SHDWMEMO")) { g_shmOn = true; atexit(shm_report); } }
    { static int te = -1; if (te < 0) { te = 1; if (getenv("GC3DS_SHDWTRI")) g_triOn = true; if (getenv("GC3DS_SHDWCB")) g_cbOn = true; if (getenv("GC3DS_TRIFAST0")) g_triFast = false; } }
    sk_before(g);
    u32 n = g_shmOn ? shm_run(g, budget) : scan_live(g, budget);
    sk_after(g);
    while (n && g_triOn && g_bsOn <= 0 && g->pc == PC_TRI && tri_known()) {
        u32 t = tri_go(g);
        if (!t) break;
        n += t;
        if (g->pc != PC_NEXT || n >= budget) break;
        sk_before(g);
        u32 m = g_shmOn ? shm_run(g, budget - n) : scan_live(g, budget - n);
        sk_after(g);
        if (!m) break;
        n += m;
    }
#else
    // Timed one call in 16 (a clock read is a system call, ~200 cycles: two a call were ~1.3% of core 0 here).
    u64 t0 = g_profClock && !(g_shdwStats[0] & 15) ? g_profClock() : 0;
    if (hle_shdw_begin_hook) hle_shdw_begin_hook();
    g_shWhere = g_shmOn ? 1 : 2;
    u32 n = g_shmOn ? shm_run(g, budget) : scan(g, budget);
    // The triangle path natively while the walk stops there, then the walk again from the skip (--shdwtri).
    while (n && g_triOn && g->pc == PC_TRI && tri_known()) {
        if (g_triBench && ++g_triBenchN == g_triBench) tri_bench(g);
        if (hle_shdw_tri_begin_hook) hle_shdw_tri_begin_hook();
        u32 t = tri_run(g);
        if (hle_shdw_tri_end_hook) hle_shdw_tri_end_hook();
        if (!t) break;
        n += t;
        if (g->pc != PC_NEXT || n >= budget) break;
        g_shWhere = g_shmOn ? 1 : 2;
        u32 m = g_shmOn ? shm_run(g, budget - n) : scan(g, budget - n);
        if (!m) break;
        n += m;
    }
    g_shWhere = 7;
    if (hle_shdw_end_hook) hle_shdw_end_hook();
    g_shWhere = 0;
    if (t0) g_shdwStats[3] += (g_profClock() - t0) * 16;
    g_shdwStats[0]++;
    g_shdwStats[2] += n;
#endif
    if (n) { g_hleRuns++; g_hleInsns += n; }
    return n;
}
