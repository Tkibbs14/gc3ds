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

#define PC_ENTRY 0x8007FB48u
#define PC_NEXT 0x8007FEA4u
#define PC_TRI 0x8007FCE4u
#define PC_EXIT 0x8007FEE8u
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

bool hle_is_hook(Gekko* g, u32 pc) {
    (void)g;
    if (!g_on) return false;
    if (pc != PC_ENTRY && pc != PC_NEXT) return hle_ps_is_hook(pc) || hle_gd_is_hook(pc) || hle_thp_is_hook(pc);
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

u32 hle_run(Gekko* g, u32 budget) {
    if (!g_on) return 0;
    if (g->pc != PC_ENTRY && g->pc != PC_NEXT) {
        u32 n = hle_ps_run(g);  // (first: on the PC it checks the generated GD writers before hle_gd.c's own)
        if (!n) n = hle_gd_run(g);
        if (!n) n = hle_thp_run(g);
        if (n) { g_hleRuns++; g_hleInsns += n; }
        return n;
    }
    if (!known()) return 0;
#ifndef __3DS__
    static int chk = -1;
    if (chk < 0) chk = getenv("HLE_CHECK") != NULL;
    u32 n = chk ? checked(g, budget) : scan(g, budget);
#else
    // Timed one call in 16 (a clock read is a system call, ~200 cycles: two a call were ~1.3% of core 0 here).
    u64 t0 = g_profClock && !(g_shdwStats[0] & 15) ? g_profClock() : 0;
    if (hle_shdw_begin_hook) hle_shdw_begin_hook();
    u32 n = scan(g, budget);
    if (hle_shdw_end_hook) hle_shdw_end_hook();
    if (t0) g_shdwStats[3] += (g_profClock() - t0) * 16;
    g_shdwStats[0]++;
    g_shdwStats[2] += n;
#endif
    if (n) { g_hleRuns++; g_hleInsns += n; }
    return n;
}
