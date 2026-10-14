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
static u8* g_mem;
static u32 g_memSize;
static int g_known = -1;  // -1 not checked yet, 0 other code, 1 recognised
static bool g_on = true;   // hle_set_enabled

void hle_set_enabled(bool on) { g_on = on; }
bool hle_enabled(void) { return g_on; }

void hle_set_memory(u8* mem1, u32 size) { g_mem = mem1; g_memSize = size; g_known = -1; }
void hle_invalidate(u32 pa) { if (pa >> 12 == (PC_ENTRY & 0x0FFFFFFFu) >> 12 || pa >> 12 == (PC_EXIT & 0x0FFFFFFFu) >> 12) g_known = -1; }

static u64 code_hash(void) {
    u32 pa = PC_ENTRY & 0x0FFFFFFFu, n = PC_EXIT - PC_ENTRY;
    if (!g_mem || pa + n > g_memSize) return 0;
    u64 h = 0xCBF29CE484222325ull;
    for (u32 i = 0; i < n; i++) { h ^= g_mem[pa + i]; h *= 0x100000001B3ull; }
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
    if (pc != PC_ENTRY && pc != PC_NEXT) return false;
    if (!g_on) return false;
    return known();
}

// Guest memory through the standard BAT mappings (0x80000000 cached, 0xC0000000 uncached, both physical 0).
static inline bool rd32(u32 a, u32* v) {
    u32 top = a >> 28, pa = a & 0x0FFFFFFFu;
    if ((top != 0x8 && top != 0xC) || (a & 3) || pa + 4 > g_memSize) return false;
    u32 w;
    memcpy(&w, g_mem + pa, 4);
    *v = __builtin_bswap32(w);
    return true;
}
static inline bool rd16(u32 a, u32* v) {
    u32 top = a >> 28, pa = a & 0x0FFFFFFFu;
    if ((top != 0x8 && top != 0xC) || (a & 1) || pa + 2 > g_memSize) return false;
    u16 h;
    memcpy(&h, g_mem + pa, 2);
    *v = __builtin_bswap16(h);
    return true;
}
static inline bool wr32(u32 a, u32 v) {
    u32 top = a >> 28, pa = a & 0x0FFFFFFFu;
    if ((top != 0x8 && top != 0xC) || (a & 3) || pa + 4 > g_memSize) return false;
    v = __builtin_bswap32(v);
    memcpy(g_mem + pa, &v, 4);
    return true;
}
static inline u32 shr(u32 v, u32 s) { return (s & 0x20) ? 0 : v >> (s & 31); }  // srw
static inline u32 shl(u32 v, u32 s) { return (s & 0x20) ? 0 : v << (s & 31); }  // slw
static inline u32 rotl(u32 v, u32 n) { return n ? (v << n) | (v >> (32 - n)) : v; }
static inline u32 cmps(u32 a, u32 b) { return (s32)a < (s32)b ? 8u : (s32)a > (s32)b ? 4u : 2u; }  // CR field LT GT EQ
static inline u32 cmpu(u32 a, u32 b) { return a < b ? 8u : a > b ? 4u : 2u; }

static u32 scan(Gekko* g, u32 budget) {
    u32 r[32], pc = g->pc, n = 0, cr0 = 0, t;
    bool crSet = false, caClear = false;
    memcpy(r, g->gpr, sizeof r);
// A guest access that fails stops the stand-in before that instruction, for guest code to run it.
#define RD32(dst, addr, at) do { if (!rd32((addr), &t)) { pc = (at); goto out; } (dst) = t; } while (0)
#define RD16(dst, addr, at) do { if (!rd16((addr), &t)) { pc = (at); goto out; } (dst) = t; } while (0)
#define WR32(addr, v, at) do { if (!wr32((addr), (v))) { pc = (at); goto out; } } while (0)
#define CR(v) do { cr0 = (v); crSet = true; } while (0)
    if (pc == PC_NEXT) goto L_fea4;
    if (pc != PC_ENTRY) return 0;
L_fb48:
    r[3] = 0x000F0000u; n++;                         // addis r3,0,15
    r[14] = r[3] + 16960; n++;                       // addi r14,r3,16960
    r[18] = r[28]; n++;                              // or r18,r28,r28
L_fb54:
    r[3] = 0x000F0000u; n++;                         // addis r3,0,15
    r[0] = r[3] + 16960; n++;                        // addi r0,r3,16960
    WR32(r[1] + 2224, r[0], 0x8007FB5Cu); n++;       // stw r0,2224(r1)
    r[25] = 0; n++;                                  // addi r25,0,0
    r[24] = 0; n++;                                  // addi r24,0,0
    r[23] = 0; n++;                                  // addi r23,0,0
    r[17] = r[30]; n++;                              // or r17,r30,r30
L_fb70:
    if (n >= budget) { pc = 0x8007FB70u; goto out; }  // slice used up: guest code goes on from the loop head
    RD32(r[8], r[15] + 24, 0x8007FB70u); n++;        // lwz r8,24(r15)
    RD32(r[3], r[8] + 12, 0x8007FB74u); n++;         // lwz r3,12(r8)
    RD32(r[4], r[8] + 44, 0x8007FB78u); n++;         // lwz r4,44(r8)
    r[7] = shr(r[17], r[4]); n++;                    // srw r7,r17,r4
    r[5] = shr(r[19], r[4]); n++;                    // srw r5,r19,r4
    RD32(r[0], r[8] + 52, 0x8007FB84u); n++;         // lwz r0,52(r8)
    r[6] = shl(r[5], r[0]); n++;                     // slw r6,r5,r0
    r[5] = shr(r[18], r[4]); n++;                    // srw r5,r18,r4
    RD32(r[0], r[8] + 48, 0x8007FB90u); n++;         // lwz r0,48(r8)
    r[0] = shl(r[5], r[0]); n++;                     // slw r0,r5,r0
    r[0] = r[6] | r[0]; n++;                         // or r0,r6,r0
    r[0] = r[7] | r[0]; n++;                         // or r0,r7,r0
    r[5] = r[0] << 2; n++;                           // rlwinm r5,r0,2,0,29
    n++;                                             // b 0x8007fbcc
    goto L_fbcc;
L_fba8:
    r[3] = r[0] + r[5]; n++;                         // add r3,r0,r5
    r[4] = r[4] - 1; n++;                            // addi r4,r4,-1
    r[6] = shr(r[17], r[4]); n++;                    // srw r6,r17,r4
    r[5] = shr(r[19], r[4]); n++;                    // srw r5,r19,r4
    r[0] = shr(r[18], r[4]); n++;                    // srw r0,r18,r4
    r[0] = rotl(r[0], 1) & 2u; n++;                  // rlwinm r0,r0,1,30,30
    r[0] = (rotl(r[5], 2) & 4u) | (r[0] & ~4u); n++; // rlwimi r0,r5,2,29,29
    r[0] = (r[6] & 1u) | (r[0] & ~1u); n++;          // rlwimi r0,r6,0,31,31
    r[5] = r[0] << 2; n++;                           // rlwinm r5,r0,2,0,29
L_fbcc:
    r[0] = r[3]; n++;                                // or r0,r3,r3
    RD32(r[5], r[3] + r[5], 0x8007FBD0u); n++;       // lwzx r5,r3,r5
    CR(cmps(r[5], 0)); n++;                          // cmpi cr0,0,r5,0
    n++;                                             // bc 4,lt,0x8007fba8
    if ((s32)r[5] >= 0) goto L_fba8;
    r[0] = r[5] & 0x7FFFFFFFu; n++;                  // rlwinm r0,r5,0,1,31
    r[16] = r[3] + r[0]; n++;                        // add r16,r3,r0
    r[0] = 1; n++;                                   // addi r0,0,1
    r[3] = shl(r[0], r[4]); n++;                     // slw r3,r0,r4
    r[5] = r[3] - 1; n++;                            // addi r5,r3,-1
    r[0] = r[17] & r[5]; n++;                        // and r0,r17,r5
    r[0] = r[3] - r[0]; n++;                         // subf r0,r0,r3
    WR32(r[1] + 2228, r[0], 0x8007FBF8u); n++;       // stw r0,2228(r1)
    r[0] = r[18] & r[5]; n++;                        // and r0,r18,r5
    r[4] = r[3] - r[0]; n++;                         // subf r4,r0,r3
    r[0] = r[19] & r[5]; n++;                        // and r0,r19,r5
    r[0] = r[3] - r[0]; n++;                         // subf r0,r0,r3
    CR(cmps(r[0], r[14])); n++;                      // cmp cr0,0,r0,r14
    n++;                                             // bc 4,lt,0x8007fc18
    if ((s32)r[0] < (s32)r[14]) { r[14] = r[0]; n++; }  // or r14,r0,r0
    RD32(r[0], r[1] + 2224, 0x8007FC18u); n++;       // lwz r0,2224(r1)
    CR(cmps(r[4], r[0])); n++;                       // cmp cr0,0,r4,r0
    n++;                                             // bc 4,lt,0x8007fc28
    if ((s32)r[4] < (s32)r[0]) { WR32(r[1] + 2224, r[4], 0x8007FC24u); n++; }  // stw r4,2224(r1)
    RD16(r[0], r[16] + 2, 0x8007FC28u); n++;         // lhz r0,2(r16)
    CR(cmpu(r[0], 0)); n++;                          // cmpli cr0,0,r0,0
    n++;                                             // bc 12,eq,0x8007fc84
    if (r[0] == 0) goto L_fc84;
    CR(cmps(r[4], r[23])); n++;                      // cmp cr0,0,r4,r23
    n++;                                             // bc 4,gt,0x8007fc84
    if ((s32)r[4] <= (s32)r[23]) goto L_fc84;
    CR(cmps(r[4], r[24])); n++;                      // cmp cr0,0,r4,r24
    n++;                                             // bc 4,gt,0x8007fc7c
    if ((s32)r[4] <= (s32)r[24]) goto L_fc7c;
    CR(cmps(r[4], r[25])); n++;                      // cmp cr0,0,r4,r25
    n++;                                             // bc 4,gt,0x8007fc68
    if ((s32)r[4] <= (s32)r[25]) goto L_fc68;
    r[23] = r[24]; n++;                              // or r23,r24,r24
    r[24] = r[25]; n++;                              // or r24,r25,r25
    r[25] = r[4]; n++;                               // or r25,r4,r4
    r[20] = r[21]; n++;                              // or r20,r21,r21
    r[21] = r[22]; n++;                              // or r21,r22,r22
    r[22] = r[16]; n++;                              // or r22,r16,r16
    n++;                                             // b 0x8007fc84
    goto L_fc84;
L_fc68:
    r[23] = r[24]; n++;                              // or r23,r24,r24
    r[24] = r[4]; n++;                               // or r24,r4,r4
    r[20] = r[21]; n++;                              // or r20,r21,r21
    r[21] = r[16]; n++;                              // or r21,r16,r16
    n++;                                             // b 0x8007fc84
    goto L_fc84;
L_fc7c:
    r[23] = r[4]; n++;                               // or r23,r4,r4
    r[20] = r[16]; n++;                              // or r20,r16,r16
L_fc84:
    RD32(r[0], r[1] + 2220, 0x8007FC84u); n++;       // lwz r0,2220(r1)
    CR(cmpu(r[16], r[0])); n++;                      // cmpl cr0,0,r16,r0
    n++;                                             // bc 12,eq,0x8007feb0
    if (r[16] == r[0]) goto L_feb0;
    RD32(r[0], r[1] + 2216, 0x8007FC90u); n++;       // lwz r0,2216(r1)
    CR(cmpu(r[16], r[0])); n++;                      // cmpl cr0,0,r16,r0
    n++;                                             // bc 12,eq,0x8007feb0
    if (r[16] == r[0]) goto L_feb0;
    RD32(r[0], r[1] + 2212, 0x8007FC9Cu); n++;       // lwz r0,2212(r1)
    CR(cmpu(r[16], r[0])); n++;                      // cmpl cr0,0,r16,r0
    n++;                                             // bc 4,eq,0x8007fea4
    if (r[16] != r[0]) goto L_fea4;
    n++;                                             // b 0x8007feb0
    goto L_feb0;
L_fea4:
    {
        // Triangles the visited bitset already holds, in a tight loop: each is 13 guest instructions (lhzu, cmpli, bc,
        // then 0x8007fcb0..0x8007fcd0 back here) that leave only r16 and the count changed and r0 / r3 / r4 / r5 / CR0 /
        // XER[CA] as the last one set them, which is rebuilt below. The list's end, an unvisited triangle and any address
        // the loop cannot read go through the instruction-by-instruction code from where it stopped.
        u32 p = r[16], skipped = 0, lastT = 0, lastW = 0, vis = r[1] + 156;
        for (;;) {
            u32 a = p + 2, tt, wa, w;
            if (!rd16(a, &tt) || !tt) break;
            wa = vis + ((tt >> 5) << 2);
            if (!rd32(wa, &w) || !(w & (1u << (tt & 31)))) break;
            skipped++; lastT = tt; lastW = w; p = a;
        }
        if (skipped) {
            r[16] = p;
            n += 13 * skipped;
            r[3] = lastW;
            r[4] = (lastT >> 5) << 2;
            r[5] = 1u << (lastT & 31);
            r[0] = r[3] & r[5];
            CR(cmps(r[0], 0));
            caClear = true;
        }
    }
    RD16(r[0], r[16] + 2, 0x8007FEA4u);              // lhzu r0,2(r16)
    r[16] += 2; n++;
    CR(cmpu(r[0], 0)); n++;                          // cmpli cr0,0,r0,0
    n++;                                             // bc 4,eq,0x8007fcb0
    if (r[0] != 0) goto L_fcb0;
L_feb0:
    RD32(r[0], r[1] + 2228, 0x8007FEB0u); n++;       // lwz r0,2228(r1)
    r[17] = r[17] + r[0]; n++;                       // add r17,r17,r0
    CR(cmpu(r[17], r[29])); n++;                     // cmpl cr0,0,r17,r29
    n++;                                             // bc 4,gt,0x8007fb70
    if (r[17] <= r[29]) goto L_fb70;
    WR32(r[1] + 2220, r[22], 0x8007FEC0u); n++;      // stw r22,2220(r1)
    WR32(r[1] + 2216, r[21], 0x8007FEC4u); n++;      // stw r21,2216(r1)
    WR32(r[1] + 2212, r[20], 0x8007FEC8u); n++;      // stw r20,2212(r1)
    RD32(r[0], r[1] + 2224, 0x8007FECCu); n++;       // lwz r0,2224(r1)
    r[18] = r[18] + r[0]; n++;                       // add r18,r18,r0
    CR(cmpu(r[18], r[27])); n++;                     // cmpl cr0,0,r18,r27
    n++;                                             // bc 4,gt,0x8007fb54
    if (r[18] <= r[27]) goto L_fb54;
    r[19] = r[19] + r[14]; n++;                      // add r19,r19,r14
    CR(cmpu(r[19], r[26])); n++;                     // cmpl cr0,0,r19,r26
    n++;                                             // bc 4,gt,0x8007fb48
    if (r[19] <= r[26]) goto L_fb48;
    pc = PC_EXIT;
    goto out;
L_fcb0:
    r[3] = 1; n++;                                   // addi r3,0,1
    RD16(r[4], r[16], 0x8007FCB4u); n++;             // lhz r4,0(r16)
    r[0] = r[4] & 31u; n++;                          // rlwinm r0,r4,0,27,31
    r[5] = shl(r[3], r[0]); n++;                     // slw r5,r3,r0
    r[0] = (u32)((s32)r[4] >> 5); caClear = true; n++;  // srawi r0,r4,5 (r4 is a halfword: CA = 0)
    r[4] = r[0] << 2; n++;                           // rlwinm r4,r0,2,0,29
    r[3] = r[1] + 156; n++;                          // addi r3,r1,156
    RD32(r[3], r[3] + r[4], 0x8007FCCCu); n++;       // lwzx r3,r3,r4
    r[0] = r[3] & r[5]; CR(cmps(r[0], 0)); n++;      // and. r0,r3,r5
    n++;                                             // bc 4,eq,0x8007fea4
    if (r[0] != 0) goto L_fea4;
    r[0] = r[3] | r[5]; n++;                         // or r0,r3,r5
    r[3] = r[1] + 156; n++;                          // addi r3,r1,156
    WR32(r[3] + r[4], r[0], 0x8007FCE0u); n++;       // stwx r0,r3,r4
    pc = PC_TRI;
out:
#undef RD32
#undef RD16
#undef WR32
#undef CR
    if (!n) return 0;
    memcpy(g->gpr, r, sizeof r);
    if (crSet) g->cr = (g->cr & 0x0FFFFFFFu) | ((cr0 | ((g->xer & XER_SO) ? 1u : 0u)) << 28);
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
        inst = ((u32)g_mem[pa] << 24) | ((u32)g_mem[pa + 1] << 16) | ((u32)g_mem[pa + 2] << 8) | g_mem[pa + 3];
        bool stop;
        g->pc = gekko_interp_one(g, g->pc, inst, &stop);
        steps++;
    }
    if (!g_chk.calls++) atexit(chk_report);
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
    if (!g_on || !known()) return 0;
#ifndef __3DS__
    static int chk = -1;
    if (chk < 0) chk = getenv("HLE_CHECK") != NULL;
    u32 n = chk ? checked(g, budget) : scan(g, budget);
#else
    u32 n = scan(g, budget);
#endif
    if (n) { g_hleRuns++; g_hleInsns += n; }
    return n;
}
