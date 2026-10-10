#define _POSIX_C_SOURCE 199309L
// Interpreter throughput on the PC: runs three guest loops from RAM (host_ptr fast path, real mode) and prints MIPS.
//   int:   ALU/rotate/compare/branch mix with loads and stores (typical integer game code)
//   alu:   register-only integer ops
//   fp:    paired-single / scalar FP mix
#include "tb.h"
#include <stdio.h>
#include <time.h>

#define OP(p) ((u32)(p) << 26)
static u32 D(u32 op, u32 d, u32 a, u32 imm) { return OP(op) | d << 21 | a << 16 | (imm & 0xFFFF); }
static u32 X(u32 op, u32 d, u32 a, u32 b, u32 xo, u32 rc) { return OP(op) | d << 21 | a << 16 | b << 11 | xo << 1 | rc; }
static u32 A(u32 op, u32 d, u32 a, u32 b, u32 c, u32 xo) { return OP(op) | d << 21 | a << 16 | b << 11 | c << 6 | xo << 1; }

static double now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
}

static Gekko g;
static TestBus tb;

static double run_loop(const u32* body, int n, const char* name) {
    u32 base = 0x00100000u, pc = base;
    gekko_reset(&g);
    gekko_set_msr(&g, MSR_FP);
    gekko_set_spr(&g, SPR_HID2, 0xA0000000u);
    tb_w32(&tb, pc, D(15, 9, 0, 0x0020)); pc += 4;  // lis r9,0x20 (data pointer 0x200000)
    u32 top = pc;
    for (int k = 0; k < n; k++, pc += 4) tb_w32(&tb, pc, body[k]);
    tb_w32(&tb, pc, OP(18) | ((top - pc) & 0x03FFFFFCu));  // b top (endless)
    g.pc = base;
    const u32 slice = 1000000;
    u64 total = 0;
    double t0 = now();
    while (now() - t0 < 1.0) total += gekko_run(&g, slice);
    double t = now() - t0;
    double mips = total / t / 1e6;
    printf("BENCH %-4s %8.1f MIPS (%llu instructions in %.2f s)\n", name, mips, (unsigned long long)total, t);
    return mips;
}

int main(void) {
    tb_init(&tb, &g);
    if (!gekko_init(&g, &tb.bus)) return 2;
    const u32 intMix[] = {
        D(32, 3, 9, 0),            // lwz r3,0(r9)
        X(31, 4, 3, 5, 266, 0),    // add r4,r3,r5
        OP(21) | 4 << 21 | 6 << 16 | 3 << 11 | 0 << 6 | 28 << 1,  // rlwinm r6,r4,3,0,28
        X(31, 6, 7, 3, 316, 0),    // xor r7,r6,r3
        D(11, 0, 7, 100),          // cmpwi r7,100
        D(16, 12, 0, 8),           // bc 12,lt,+8 (blt)
        D(14, 5, 5, 1),            // addi r5,r5,1
        D(36, 4, 9, 4),            // stw r4,4(r9)
        X(31, 8, 4, 6, 235, 0),    // mullw r8,r4,r6
        X(31, 6, 10, 8, 444, 1),   // or. r10,r6,r8
        D(40, 11, 9, 8),           // lhz r11,8(r9)
        X(31, 11, 12, 5, 40, 0),   // subf r12,r5,r11
    };
    const u32 aluMix[] = {
        X(31, 4, 3, 5, 266, 0), X(31, 5, 4, 3, 40, 0), OP(21) | 4 << 21 | 6 << 16 | 5 << 11 | 0 << 6 | 31 << 1,
        X(31, 6, 7, 3, 316, 0), D(24, 7, 8, 0x55), X(31, 8, 4, 6, 28, 1), D(14, 3, 3, 7), X(31, 4, 9, 0, 824, 0),
    };
    const u32 fpMix[] = {
        A(63, 1, 2, 3, 0, 21),     // fadd
        A(59, 4, 1, 5, 6, 29),     // fmadds
        A(4, 7, 8, 9, 10, 29),     // ps_madd
        A(4, 11, 7, 12, 0, 21),    // ps_add
        A(4, 13, 11, 0, 14, 25),   // ps_mul
        D(48, 15, 9, 16),          // lfs
        D(52, 13, 9, 24),          // stfs
        A(63, 16, 4, 1, 0, 18),    // fdiv
    };
    run_loop(intMix, sizeof intMix / 4, "int");
    run_loop(aluMix, sizeof aluMix / 4, "alu");
    run_loop(fpMix, sizeof fpMix / 4, "fp");
    gekko_free(&g);
    tb_free(&tb);
    return 0;
}
