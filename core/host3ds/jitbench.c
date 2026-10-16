// JIT microbenchmarks on the device (--jit-bench only): what one guest instruction of each class costs core 0 when the
// real JIT (the run's default options) compiles it and its code and data sit in the L1 caches. Each case is a loop of
// 32 copies of one instruction (registers rotated so that neighbours do not depend on each other unless the case says
// so) closed by addic. / bne, run for ~2 M guest instructions after a warm-up that compiles it; the PMU pairs give
// cycles, host instructions, stalls and misses per guest instruction. Read against TP's ~35 cycles per guest
// instruction in the cutscene (MEASUREMENTS.md), it separates what the instruction mix costs in cache from what the
// footprint and the data add. The machine is put back as it was afterwards (MEM1 is overwritten by the boot anyway).
#include <3ds.h>
#include <stdio.h>
#include <string.h>
#define GC3DS_HOST_TYPES
#include "../src/common.h"
#include "../src/cpu/gekko_internal.h"
#include "../src/jit/jit.h"
#include "../src/sys/system.h"
#include "pmu.h"

#define CODE_PA 0x00003000u
#define FUNC_PA 0x00006000u  // bl's target: a blr
#define DATA_PA 0x00100000u  // loads read and stores write 256 bytes here (L1-resident)
#define REPS 32

enum { K_NONE, K_GPR_DEP, K_LOAD, K_STORE, K_FLOAT, K_FLOAT_DEP, K_CMP, K_BL, K_B, K_BCNT };
typedef struct { const char* name; u32 kind; u32 base; } Case;

// Encodings (Gekko User's Manual): D-form op << 26 | rD << 21 | rA << 16 | d; X-form 31 << 26 | ... | xo << 1;
// A-form (float) op << 26 | fD << 21 | fA << 16 | fB << 11 | fC << 6 | xo << 1.
#define DF(op) ((u32)(op) << 26)
#define XF(xo) (DF(31) | (u32)(xo) << 1)
#define AF(op, xo) (DF(op) | (u32)(xo) << 1)
static const Case kCases[] = {
    {"addi", K_NONE, DF(14) | 1},                 // addi rD, rD, 1
    {"addi_dep", K_GPR_DEP, DF(14) | 1},          // addi r3, r3, 1 (each waits for the last)
    {"rlwinm", K_NONE, DF(21) | 3u << 11 | 0u << 6 | 28u << 1},  // rlwinm rA, rS, 3, 0, 28
    {"or", K_NONE, XF(444)},                      // or rA, rS, rB
    {"mullw", K_NONE, XF(235)},
    {"divw", K_NONE, XF(491)},
    {"cmpwi", K_CMP, DF(11)},                     // cmpwi crN, rA, 0 (cr1-cr7 rotated)
    {"lwz", K_LOAD, DF(32)},
    {"lbz", K_LOAD, DF(34)},
    {"lhz", K_LOAD, DF(40)},
    {"stw", K_STORE, DF(36)},
    {"stb", K_STORE, DF(38)},
    {"sth", K_STORE, DF(44)},
    {"lfs", K_LOAD, DF(48)},
    {"lfd", K_LOAD, DF(50)},
    {"stfs", K_STORE, DF(52)},
    {"stfd", K_STORE, DF(54)},
    {"psq_l_float", K_LOAD, DF(56) | 0u << 12},   // GQR0: plain floats
    {"psq_st_float", K_STORE, DF(60) | 0u << 12},
    {"psq_l_u8", K_LOAD, DF(56) | 1u << 12},      // GQR1: u8, scale 0
    {"psq_st_u8", K_STORE, DF(60) | 1u << 12},
    {"fadds", K_FLOAT, AF(59, 21)},
    {"fmuls", K_FLOAT, AF(59, 25)},
    {"fmadds", K_FLOAT, AF(59, 29)},
    {"fmadds_dep", K_FLOAT_DEP, AF(59, 29)},
    {"fadd", K_FLOAT, AF(63, 21)},
    {"fmadd", K_FLOAT, AF(63, 29)},
    {"frsp", K_FLOAT, AF(63, 12)},
    {"fctiwz", K_FLOAT, AF(63, 15)},
    {"ps_add", K_FLOAT, AF(4, 21)},
    {"ps_mul", K_FLOAT, AF(4, 25)},
    {"ps_madd", K_FLOAT, AF(4, 29)},
    {"bl_blr", K_BL, 0},                          // bl to a blr: 2 guest instructions per call
    {"b_next", K_B, 0},                           // b +4 (a taken unconditional branch)
    {"bc_not_taken", K_BCNT, 0},                  // beq crN, +8 with crN not equal
};

static u32 gen(const Case* c, u32* code, u32 k) {
    (void)code;
    u32 r = 3 + (k & 7), f = 1 + (k & 7);  // GPRs r3-r10, FPRs f1-f8 rotate
    u32 off = (k & 15) * 8;                // two words a slot: doubles and pairs stay aligned
    switch (c->kind) {
    case K_NONE:
        if (c->base == (DF(14) | 1)) return c->base | r << 21 | r << 16;
        if ((c->base >> 26) == 21) return c->base | r << 21 | r << 16;  // rlwinm rA=rS
        return c->base | r << 21 | (3 + ((k + 1) & 7)) << 16 | (3 + ((k + 2) & 7)) << 11;  // rD, rA, rB (X-form fields)
    case K_GPR_DEP: return c->base | 3u << 21 | 3u << 16;
    case K_CMP: return c->base | (1 + (k % 7)) << 23 | r << 16;
    case K_LOAD: return c->base | ((c->base >> 26) >= 48 ? f : r) << 21 | 20u << 16 | off;
    case K_STORE: return c->base | ((c->base >> 26) >= 48 ? f : r) << 21 | 21u << 16 | off;
    case K_FLOAT: {
        u32 xo = (c->base >> 1) & 31;
        if (xo == 12 || xo == 15) return c->base | f << 21 | f << 11;               // frsp / fctiwz fD, fB
        if (xo == 25) return c->base | f << 21 | f << 16 | 9u << 6;                  // fmuls fD, fA, f9
        if (xo == 29) return c->base | f << 21 | f << 16 | 10u << 11 | 9u << 6;      // fmadds fD, fA, f9, f10
        return c->base | f << 21 | f << 16 | 10u << 11;                              // fadds fD, fA, f10
    }
    case K_FLOAT_DEP: return c->base | 1u << 21 | 1u << 16 | 10u << 11 | 9u << 6;    // f1 = f1 * f9 + f10
    case K_BL: {
        u32 at = CODE_PA + 4 * k, disp = FUNC_PA - at;
        return DF(18) | (disp & 0x03FFFFFCu) | 1;
    }
    case K_B: return DF(18) | 4;
    case K_BCNT: return DF(16) | 12u << 21 | (4 * (1 + (k % 7)) + 2) << 16 | 8;      // beq crN, +8
    }
    return 0;
}

static System* g_s;
static Jit* g_j;

// Writes the case's loop at CODE_PA and sets the registers it reads.
static u32 setup(const Case* c) {
    Gekko* g = &g_s->cpu;
    u32 n = 0, reps = c->kind == K_BCNT ? REPS / 2 : REPS;
    for (u32 k = 0; k < reps; k++) {
        if (c->kind == K_BCNT) {  // beq crN, +8 then a nop the branch skips over when taken (it never is)
            mem1_write32(g_s, CODE_PA + 4 * n++, gen(c, NULL, k));
            mem1_write32(g_s, CODE_PA + 4 * n++, DF(24));  // ori r0, r0, 0
            continue;
        }
        mem1_write32(g_s, CODE_PA + 4 * n, gen(c, NULL, n));
        n++;
    }
    mem1_write32(g_s, CODE_PA + 4 * n++, DF(13) | 31u << 21 | 31u << 16 | 0xFFFFu);  // addic. r31, r31, -1
    mem1_write32(g_s, CODE_PA + 4 * n, DF(16) | 4u << 21 | 2u << 16 | ((0u - 4 * n) & 0xFFFCu));  // bne cr0, start
    mem1_write32(g_s, FUNC_PA, 0x4E800020u);  // blr
    for (u32 i = 0; i < 64; i++) mem1_write32(g_s, DATA_PA + 4 * i, 0x3F800000u);  // 1.0f (bytes 63, 128, 0, 0 for u8)
    for (u32 i = 0; i < 32; i++) g->gpr[i] = i;
    g->gpr[20] = 0x80000000u | DATA_PA;
    g->gpr[21] = 0x80000000u | (DATA_PA + 0x200);
    g->gpr[31] = 0x7FFFFFFFu;
    for (u32 i = 0; i < 32; i++) { g->fpr[i].ps0 = 1.0 + i / 64.0; g->fpr[i].ps1 = 1.0 - i / 64.0; }
    g->fpr[9].ps0 = g->fpr[9].ps1 = 1.0000001;
    g->fpr[10].ps0 = g->fpr[10].ps1 = 0.5;
    g->cr = 0x88888888u;  // every crN "less than", so beq never branches
    g->pc = 0x80000000u | CODE_PA;
    jit_invalidate_all(g_j);
    return n + 1;
}

static void run_case(FILE* f, const Case* c) {
    u32 words = setup(c);
    u32 perRound = words;  // guest instructions per round (bl_blr also runs a blr per call)
    if (c->kind == K_BL) perRound += REPS;
    Gekko* g = &g_s->cpu;
    // A run stops at once while an interrupt or stop request is pending (the loaded state leaves an external interrupt
    // raised): clear them, and keep calling until the instructions have run (calls: how many runs it took).
    #define QUIET() do { g->extIrq = false; GKP(g)->brk = false; GKP(g)->stopReq = false; g->msr &= ~MSR_EE; } while (0)
    QUIET();
    jit_run(g_j, 200000);  // warm-up: compiles the loop and fills the caches
    static const unsigned kPairs[4] = { 0, 2, 1, 5 };  // inst+brmiss, stall_inst+stall_data, icmiss+dcrmiss, brnopred+pcchg
    double cyc[4], ev[4][2];
    u32 calls = 0;
    for (int p = 0; p < 4; p++) {
        pmu_select(kPairs[p]);
        u32 ran = 0;
        calls = 0;
        while (ran < 2000000 && calls < 100000) { QUIET(); ran += jit_run(g_j, 2000000 - ran); calls++; }
        unsigned long long cy, e0, e1;
        pmu_read(&cy, &e0, &e1);
        cyc[p] = ran ? (double)cy / ran : 0;
        ev[p][0] = ran ? (double)e0 / ran : 0;
        ev[p][1] = ran ? (double)e1 / ran : 0;
    }
    double cmin = cyc[0];
    for (int p = 1; p < 4; p++) if (cyc[p] < cmin) cmin = cyc[p];
    // Per guest instruction of the loop (the addic. / bne pair is 2 of the round's guest instructions; reported raw).
    fprintf(f, "# jitbench %s: cyc %.2f inst %.2f brmiss %.4f stall_inst %.2f stall_data %.2f icmiss %.4f dcrmiss %.4f "
            "brnopred %.3f pcchg %.3f (round %lu guest instructions, %lu runs)\n", c->name, cmin, ev[0][0], ev[0][1], ev[1][0], ev[1][1],
            ev[2][0], ev[2][1], ev[3][0], ev[3][1], (unsigned long)perRound, (unsigned long)calls);
    fflush(f);
}

// Block boundaries and footprint: a ring of M blocks of K guest instructions each (K - 1 addi over r3-r6, then a bne
// cr7 taken to the next block, with a b to it on the not-taken path), each block 64 bytes apart in guest memory; run
// round and round until the budget. What a block boundary costs (registers written back and loaded again, the linked
// jump) and how the cost grows as the compiled code outgrows the caches.
#define RING_PA 0x00010000u
static void run_blocks(FILE* f, u32 k, u32 m) {
    Gekko* g = &g_s->cpu;
    for (u32 b = 0; b < m; b++) {
        u32 at = RING_PA + 64 * b, next = RING_PA + 64 * ((b + 1) % m);
        for (u32 i = 0; i + 1 < k; i++) {
            u32 r = 3 + (i & 3);
            mem1_write32(g_s, at + 4 * i, DF(14) | r << 21 | r << 16 | 1);  // addi rR, rR, 1
        }
        u32 bc = at + 4 * (k - 1);
        mem1_write32(g_s, bc, DF(16) | 4u << 21 | 30u << 16 | ((next - bc) & 0xFFFCu));  // bne cr7, next (cr7.eq clear)
        mem1_write32(g_s, bc + 4, DF(18) | ((next - bc - 4) & 0x03FFFFFCu));            // b next
    }
    for (u32 i = 0; i < 32; i++) g->gpr[i] = i;
    g->cr = 0x88888888u;
    g->pc = 0x80000000u | RING_PA;
    jit_invalidate_all(g_j);
    #define QUIET2() do { g->extIrq = false; GKP(g)->brk = false; GKP(g)->stopReq = false; g->msr &= ~MSR_EE; } while (0)
    QUIET2();
    jit_run(g_j, (s64)m * k * 4 + 100000);  // compiles every block of the ring
    static const unsigned kPairs[3] = { 0, 2, 1 };
    double cyc[3], ev[3][2];
    u32 calls = 0;
    for (int p = 0; p < 3; p++) {
        pmu_select(kPairs[p]);
        u32 ran = 0;
        calls = 0;
        while (ran < 2000000 && calls < 100000) { QUIET2(); ran += jit_run(g_j, 2000000 - ran); calls++; }
        unsigned long long cy, e0, e1;
        pmu_read(&cy, &e0, &e1);
        cyc[p] = ran ? (double)cy / ran : 0;
        ev[p][0] = ran ? (double)e0 / ran : 0;
        ev[p][1] = ran ? (double)e1 / ran : 0;
    }
    double cmin = cyc[0] < cyc[1] ? cyc[0] : cyc[1];
    if (cyc[2] < cmin) cmin = cyc[2];
    fprintf(f, "# jitbench blocks K %lu M %lu: cyc %.2f per guest instruction (%.1f per block) inst %.2f stall_inst %.2f "
            "stall_data %.2f icmiss %.4f dcrmiss %.4f (%lu runs; JIT code %.0f%% of its buffer)\n", (unsigned long)k,
            (unsigned long)m, cmin, cmin * k, ev[0][0], ev[1][0], ev[1][1], ev[2][0], ev[2][1], (unsigned long)calls,
            100.0 * jit_code_used(g_j));
    fflush(f);
}

void jit_bench(Jit* j, System* s, FILE* f) {
    if (!pmu_init()) { fprintf(f, "# jitbench: performance counters unavailable (%s)\n", pmu_status()); return; }
    g_s = s;
    g_j = j;
    static Gekko saved;
    memcpy(&saved, &s->cpu, sizeof saved);
    Gekko* g = &s->cpu;
    gekko_set_spr(g, SPR_IBAT0U, 0x80001FFF); gekko_set_spr(g, SPR_IBAT0U + 1, 0x00000002);
    gekko_set_spr(g, SPR_DBAT0U, 0x80001FFF); gekko_set_spr(g, SPR_DBAT0U + 1, 0x00000002);
    gekko_set_spr(g, SPR_DBAT0U + 2, 0xC0001FFF); gekko_set_spr(g, SPR_DBAT0U + 3, 0x0000002A);
    gekko_set_spr(g, SPR_HID2, HID2_LSQE | HID2_PSE);
    gekko_set_spr(g, SPR_GQR0, 0);
    gekko_set_spr(g, SPR_GQR0 + 1, 0x00040004u);  // u8 both ways, scale 0
    gekko_set_msr(g, MSR_FP | MSR_IR | MSR_DR | MSR_RI);
    fprintf(f, "# jitbench: per guest instruction, loops of %d in L1, the run's JIT options (jit opt %lx)\n", REPS,
            (unsigned long)jit_opt(j));
    for (u32 i = 0; i < sizeof kCases / sizeof kCases[0]; i++) run_case(f, &kCases[i]);
    static const u32 kK[3] = { 2, 6, 12 }, kM[4] = { 16, 256, 1024, 4096 };
    for (u32 a = 0; a < 3; a++)
        for (u32 b = 0; b < 4; b++) run_blocks(f, kK[a], kM[b]);
    memcpy(&s->cpu, &saved, sizeof saved);
    jit_invalidate_all(j);
    fprintf(f, "# jitbench: done\n");
    fflush(f);
}
