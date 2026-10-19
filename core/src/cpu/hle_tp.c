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
static bool g_shCrK, g_shCa;  // the last scan: CR0 written, XER[CA] cleared (the walk memo below)
static u8* g_mem;
static u32 g_memSize;
static int g_known = -1;  // -1 not checked yet, 0 other code, 1 recognised
static bool g_on = true;   // hle_set_enabled

void hle_set_enabled(bool on) { g_on = on; }
bool hle_enabled(void) { return g_on; }

void hle_set_memory(u8* mem1, u32 size) { g_mem = mem1; g_memSize = size; g_known = -1; hle_gd_set_memory(mem1, size); }
void hle_invalidate(u32 pa) {
    if (pa >> 12 == (PC_ENTRY & 0x0FFFFFFFu) >> 12 || pa >> 12 == (PC_EXIT & 0x0FFFFFFFu) >> 12) g_known = -1;
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

bool hle_is_hook(Gekko* g, u32 pc) {
    (void)g;
    if (g_gfskipAddr && pc == g_gfskipAddr) return true;
    if (g_memoAddr && pc == g_memoAddr) return true;
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
    sk_before(g);
    u32 n = g_shmOn ? shm_run(g, budget) : scan_live(g, budget);
    sk_after(g);
#else
    // Timed one call in 16 (a clock read is a system call, ~200 cycles: two a call were ~1.3% of core 0 here).
    u64 t0 = g_profClock && !(g_shdwStats[0] & 15) ? g_profClock() : 0;
    if (hle_shdw_begin_hook) hle_shdw_begin_hook();
    u32 n = g_shmOn ? shm_run(g, budget) : scan(g, budget);
    if (hle_shdw_end_hook) hle_shdw_end_hook();
    if (t0) g_shdwStats[3] += (g_profClock() - t0) * 16;
    g_shdwStats[0]++;
    g_shdwStats[2] += n;
#endif
    if (n) { g_hleRuns++; g_hleInsns += n; }
    return n;
}
