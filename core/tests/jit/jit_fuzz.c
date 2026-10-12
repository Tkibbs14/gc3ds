// JIT differential fuzzer: random single instructions of every kind the JIT translates, executed by the JIT on one
// machine and by the interpreter on another from identical random state; registers and memory must match exactly.
// Float and paired-single instructions follow the JIT's "fast float" model (src/jit/jit.c): FPR lanes must match bit
// for bit except fused multiply-add forms (1 ulp), NaN payloads, and denormal results or inputs that the JIT flushes.
// Runs as an ARM Linux program under qemu-arm (tests/jit/run_jit_fuzz.sh), and on the 3DS inside gc3ds as
// jit_fuzz_main (a relay job with --fuzz: host3ds/main.c), where it frees what it made before gc3ds starts.
//   jit_fuzz [cases] [seed]        JIT_FUZZ_PLANT=1 corrupts one JIT result, =2 makes the JIT skip its rounding to
//                                  single, to prove the comparison can fail.
#ifdef __3DS__
#include <3ds.h>
#define GC3DS_HOST_TYPES
#define main jit_fuzz_main
#endif
#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include "../../src/jit/jit.h"
#include "../../src/sys/system.h"

static System A, B;
static u64 g_in[32][2];  // the FPRs the case started from
static Jit* jit;
static u64 rng = 0x9E3779B97F4A7C15ull;
static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (u32)(rng >> 16); }
static u32 rbits(u32 n) { return rnd() & ((1u << n) - 1); }

// The instruction is placed at a random word in this page, so PC-derived encodings (bit 2 and up) vary.
static u32 CODE_PA = 0x3000u;
#define CODE_EA (0x80000000u | CODE_PA)
#define WIN_PA 0x100000u
#define WIN_SIZE 0x10000u
#define TOP_SIZE 256u  // the end of MEM1 is compared and randomized too

#define MSR_BASE (MSR_IR | MSR_DR | MSR_RI)
#define HID2_BASE (0x80000000u | 0x20000000u)  // LSQE | PSE

// JIT_FUZZ_TRACE: every compiled block goes to /tmp/jit_last.bin before it runs (disassemble it after a crash).
static void dump_block(const u32* code, u32 words) {
    FILE* d = fopen("/tmp/jit_last.bin", "wb");
    if (d) { fwrite(code, 4, words, d); fclose(d); }
}

static u32 interesting(void) {
    static const u32 v[] = { 0, 1, 2, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFF, 0x8000, 0x7FFF, 0x10000, 31, 32, 33, 63 };
    switch (rnd() % 4) {
    case 0: return v[rnd() % (sizeof v / sizeof v[0])];
    case 1: return rnd() & 0xFF;
    case 2: return (u32)-(s32)(rnd() & 0xFF);
    default: return rnd() << 16 | rnd();
    }
}

static void boot_like(System* s) {
    Gekko* g = &s->cpu;
    gekko_set_spr(g, SPR_IBAT0U, 0x80001FFF); gekko_set_spr(g, SPR_IBAT0U + 1, 0x00000002);
    gekko_set_spr(g, SPR_DBAT0U, 0x80001FFF); gekko_set_spr(g, SPR_DBAT0U + 1, 0x00000002);
    gekko_set_spr(g, SPR_DBAT0U + 2, 0xC0001FFF); gekko_set_spr(g, SPR_DBAT0U + 3, 0x0000002A);
    gekko_set_spr(g, SPR_HID2, HID2_BASE);
    gekko_set_msr(g, MSR_FP | MSR_BASE);
}

// Random instruction from the translated set; may set up registers it needs (base addresses) in the state.
typedef struct {
    u32 gpr[32], cr, xer, lr, ctr;
    u64 fpr[32][2];
    u32 gqr[8], fpscr;
    bool fpOff;   // MSR[FP] = 0 (float instructions must raise the FP-unavailable exception)
    bool fmem;    // fill the memory window with normal single-precision floats
    bool irq;     // an external interrupt is pending (mtmsr setting EE must leave compiled code)
    bool user;    // MSR[PR] = 1 (supervisor instructions must raise the program exception)
} St;

static u32 load_base(St* st, u32 ra, u32 size) {
    u32 off = rnd() % (WIN_SIZE - 0x400) + 0x200;
    if (rnd() % 8) off &= ~(size - 1);       // mostly aligned (fast path), sometimes not (slow path)
    st->gpr[ra] = ((rnd() & 1) ? 0x80000000u : 0xC0000000u) | (WIN_PA + off);
    return st->gpr[ra];
}

// Base for float accesses: mostly the window, sometimes the last bytes of MEM1 (range checks) or a wild address (DSI).
static u32 float_base(St* st, u32 ra, u32 size) {
    u32 v = load_base(st, ra, size), k = rnd() % 16;
    if (k == 0) st->gpr[ra] = ((rnd() & 1) ? 0x80000000u : 0xC0000000u) | (MEM1_SIZE - 16 + 4 * (rnd() % 4));
    else if (k == 1) st->gpr[ra] = interesting();
    return v;
}

static u32 gen(St* st) {
    u32 rd = rbits(5), ra = rbits(5), rb = rbits(5), rc = rbits(1);
    u32 imm = rnd() & 0xFFFF;
    for (;;) {
        switch (rnd() % 17) {
        case 0: {  // D-form arithmetic / logic / compare
            static const u32 ops[] = { 14, 15, 12, 13, 8, 7, 24, 25, 26, 27, 28, 29, 10, 11 };
            u32 op = ops[rnd() % 14];
            if (op == 10 || op == 11) rd = rbits(3) << 2;  // crfD, L = 0
            return (op << 26) | (rd << 21) | (ra << 16) | imm;
        }
        case 1: case 2: {  // XO / X-form group 31
            static const u32 xos[] = { 266, 10, 138, 202, 40, 8, 136, 104, 28, 60, 444, 412, 316, 124, 476, 284, 24, 536,
                                       824, 26, 954, 922, 0, 32, 235, 75, 11, 19, 144 };
            u32 xo = xos[rnd() % (sizeof xos / sizeof xos[0])];
            if (xo == 0 || xo == 32) { rd = rbits(3) << 2; rc = 0; }
            if (xo == 19) { ra = rb = 0; rc = 0; }
            if (xo == 144) return (31u << 26) | (rd << 21) | (rbits(8) << 12) | (144u << 1);
            if (xo == 104 || xo == 202 || xo == 26 || xo == 954 || xo == 922) rb = 0;
            return (31u << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (xo << 1) | rc;
        }
        case 3: {  // rotates
            static const u32 ops[] = { 21, 20, 23 };
            u32 op = ops[rnd() % 3];
            return (op << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (rbits(5) << 6) | (rbits(5) << 1) | rc;
        }
        case 4: case 5: {  // D-form loads / stores
            static const u32 ops[] = { 32, 33, 34, 35, 40, 41, 42, 43, 36, 37, 38, 39, 44, 45 };
            u32 op = ops[rnd() % 14];
            u32 size = op <= 33 || op == 36 || op == 37 ? 4 : (op <= 35 || op == 38 || op == 39) ? 1 : 2;
            ra = 1 + rnd() % 31;
            if ((op & 1) && rd == ra) rd = (ra + 1) & 31;
            load_base(st, ra, size);
            u32 d = (u32)(s32)((s32)(rnd() % 512) - 256);
            if (rnd() % 8) d &= ~(size - 1);
            return (op << 26) | (rd << 21) | (ra << 16) | (d & 0xFFFF);
        }
        case 6: {  // indexed loads / stores
            static const u32 xos[] = { 23, 87, 279, 343, 55, 119, 311, 375, 151, 215, 407, 183, 247, 439 };
            u32 xo = xos[rnd() % 14];
            ra = 1 + rnd() % 31;
            rb = 1 + rnd() % 31;
            if (rb == ra) rb = (ra % 31) + 1;
            bool upd = xo == 55 || xo == 119 || xo == 311 || xo == 375 || xo == 183 || xo == 247 || xo == 439;
            if (upd && rd == ra) rd = (ra + 2) & 31;
            load_base(st, ra, 4);
            st->gpr[rb] = rnd() % 256;
            if (rnd() % 8) st->gpr[rb] &= ~3u;
            return (31u << 26) | (rd << 21) | (ra << 16) | (rb << 11) | (xo << 1);
        }
        case 7: {  // mfspr / mtspr LR, CTR, XER
            static const u32 sprs[] = { 1, 8, 9 };
            u32 spr = sprs[rnd() % 3];
            u32 xo = (rnd() & 1) ? 339 : 467;
            return (31u << 26) | (rd << 21) | ((spr & 31) << 16) | ((spr >> 5) << 11) | (xo << 1);
        }
        case 8: case 9: {  // conditional branch (bc) with every BO class
            static const u32 bos[] = { 0, 2, 4, 6, 8, 10, 12, 14, 16, 18, 20 };
            u32 bo = bos[rnd() % 11], bi = rbits(5);
            if (rnd() % 3 == 0) st->ctr = rnd() % 3;
            s32 bd = ((s32)(rnd() % 64) - 32) * 4;
            return (16u << 26) | (bo << 21) | (bi << 16) | ((u32)bd & 0xFFFC) | (rnd() & 1);
        }
        case 10: {  // b / bl / ba
            s32 li = ((s32)(rnd() % 4096) - 2048) * 4;
            return (18u << 26) | ((u32)li & 0x03FFFFFC) | (rnd() & 1) | ((rnd() % 8 == 0) ? 2u : 0u);
        }
        case 11: {  // bclr / bcctr
            static const u32 bos[] = { 4, 12, 20, 0, 2, 8, 16 };
            bool toCtr = rnd() & 1;
            u32 bo = bos[rnd() % (toCtr ? 3 : 7)], bi = rbits(5);
            if (rnd() % 3 == 0) st->ctr = rnd() % 3;
            return (19u << 26) | (bo << 21) | (bi << 16) | ((toCtr ? 528u : 16u) << 1) | (rnd() & 1);
        }
        case 12: {  // mfmsr, mtmsr (EE toggles, sometimes other bits, sometimes with an interrupt pending), sync, isync
            switch (rnd() % 5) {
            case 0: return (31u << 26) | (rd << 21) | (83u << 1);
            case 1: case 2: {
                u32 msr = MSR_BASE | (st->fpOff ? 0 : MSR_FP) | (st->user ? MSR_PR : 0);
                u32 v = msr ^ ((rnd() & 1) ? MSR_EE : 0) ^ ((rnd() % 4 == 0) ? MSR_FP : 0) ^ ((rnd() % 8 == 0) ? MSR_RI : 0);
                st->gpr[rd] = v;
                st->irq = rnd() % 3 == 0;
                return (31u << 26) | (rd << 21) | (146u << 1);
            }
            case 3: return (31u << 26) | (((rnd() & 1) ? 598u : 854u) << 1);
            default: return (19u << 26) | (150u << 1);
            }
        }
        case 13: {  // dcbst dcbf dcbi dcbz dcbt: window, end of MEM1 or a wild address
            static const u32 xos[] = { 54, 86, 470, 1014, 278 };
            u32 xo = xos[rnd() % 5];
            ra = 1 + rnd() % 31;
            rb = 1 + rnd() % 31;
            if (rb == ra) rb = (ra % 31) + 1;
            float_base(st, ra, 4);
            st->gpr[rb] = rnd() % 256;
            return (31u << 26) | (ra << 16) | (rb << 11) | (xo << 1);
        }
        case 15: {  // stores to the write-gather pipe (the JIT's direct path) and next to it (the MMIO path)
            static const u32 ops[] = { 36, 38, 44, 37, 39, 45 };
            u32 op = ops[rnd() % 6];
            ra = 1 + rnd() % 31;
            u32 d = (u32)(s32)((s32)(rnd() % 64) - 32) & ~3u;
            st->gpr[ra] = GP_WGP_EA - d + ((rnd() % 4 == 0) ? 4u : 0u);
            return (op << 26) | (rd << 21) | (ra << 16) | (d & 0xFFFF);
        }
        case 16: {  // CR logic and mcrf (bits and fields 0-31 / 0-7, including the last ones)
            static const u32 xos[] = { 257, 129, 289, 225, 33, 449, 417, 193, 0 };
            u32 xo = xos[rnd() % 9];
            if (xo == 0) return (19u << 26) | (rbits(3) << 23) | (rbits(3) << 18);
            u32 d = (rnd() % 4) ? rbits(5) : 31, a = (rnd() % 4) ? rbits(5) : 31, b = (rnd() % 4) ? rbits(5) : 31;
            return (19u << 26) | (d << 21) | (a << 16) | (b << 11) | (xo << 1);
        }
        case 14: {  // mfspr / mtspr of plain-storage SPRs
            static const u32 sprs[] = { 26, 27, 18, 19, 272, 273, 274, 275, 912, 913, 914, 915, 916, 917, 918, 919 };
            u32 spr = sprs[rnd() % 16];
            u32 xo = (rnd() & 1) ? 339 : 467;
            return (31u << 26) | (rd << 21) | ((spr & 31) << 16) | ((spr >> 5) << 11) | (xo << 1);
        }
        default:
            continue;
        }
    }
}

// ---- two-instruction blocks ----
// The JIT carries state from one instruction to the next (a GPR it just stored is reused; a compare's ARM flags drive
// the following bc), so some cases are pairs. These help the pair touch that state.
static bool is_branch_op(u32 in) {
    u32 op = in >> 26, xo = (in >> 1) & 0x3FF;
    return op == 16 || op == 18 || (op == 19 && (xo == 16 || xo == 528 || xo == 150));
}
// GPR written by an integer instruction (-1 when none or unknown).
static int dest_gpr(u32 in) {
    u32 op = in >> 26, rd = (in >> 21) & 31, ra = (in >> 16) & 31;
    if (op == 7 || op == 8 || (op >= 12 && op <= 15) || (op >= 32 && op <= 35) || (op >= 40 && op <= 43)) return (int)rd;
    if (op == 20 || op == 21 || op == 23 || (op >= 24 && op <= 29)) return (int)ra;
    if (op == 31) {
        u32 xo = (in >> 1) & 0x3FF;
        if (xo == 28 || xo == 60 || xo == 444 || xo == 412 || xo == 316 || xo == 124 || xo == 476 || xo == 284 ||
            xo == 24 || xo == 536 || xo == 824 || xo == 26 || xo == 954 || xo == 922) return (int)ra;
        if (xo == 266 || xo == 10 || xo == 138 || xo == 202 || xo == 40 || xo == 8 || xo == 136 || xo == 104 ||
            xo == 235 || xo == 75 || xo == 11 || xo == 19 || xo == 339 || xo == 83) return (int)rd;
    }
    return -1;
}
// Makes the second instruction read GPR r where its first source operand is (arithmetic and logic only).
static u32 read_gpr(u32 in, u32 r) {
    u32 op = in >> 26;
    if (op == 7 || op == 8 || (op >= 10 && op <= 15)) return (in & ~(31u << 16)) | (r << 16);
    if (op == 20 || op == 21 || op == 23 || (op >= 24 && op <= 29)) return (in & ~(31u << 21)) | (r << 21);
    if (op == 31) {
        u32 xo = (in >> 1) & 0x3FF;
        if (xo == 28 || xo == 60 || xo == 444 || xo == 316 || xo == 24 || xo == 536 || xo == 824 || xo == 26) return (in & ~(31u << 21)) | (r << 21);
        if (xo == 266 || xo == 10 || xo == 40 || xo == 8 || xo == 235 || xo == 0 || xo == 32) return (in & ~(31u << 16)) | (r << 16);
    }
    return in;
}
// CR field a compare or record form sets (-1 when none).
static int crf_set(u32 in) {
    u32 op = in >> 26, xo = (in >> 1) & 0x3FF;
    if (op == 10 || op == 11) return (int)((in >> 23) & 7);
    if (op == 31 && (xo == 0 || xo == 32)) return (int)((in >> 23) & 7);
    if (op == 28 || op == 29 || op == 13) return 0;
    if ((op == 20 || op == 21 || op == 23 || op == 31) && (in & 1)) return 0;
    return -1;
}

// ---- float instructions ----
static u64 mk(u32 sign, int e, u64 mant52) { return ((u64)sign << 63) | ((u64)(e + 1023) << 52) | mant52; }
static u64 rnd64(void) { return (u64)rnd() << 32 | rnd(); }
static const u64 FRAC = (1ull << 52) - 1;

// A finite normal double: exact singles and full doubles, moderate and wide exponents, zeros, small integers.
static u64 fval(void) {
    u32 s = rnd() & 1;
    u64 m52 = rnd64() & FRAC, m23 = m52 & (0x7FFFFFull << 29);
    if (rnd() % 64 == 0) return mk(s, 1024, 0);  // infinity
    switch (rnd() % 11) {
    case 0: return (u64)s << 63;
    case 1: { double d = (double)((int)(rnd() % 65) - 32); u64 b; memcpy(&b, &d, 8); return b; }
    case 2: case 3: return mk(s, (int)(rnd() % 41) - 20, m23);
    case 4: return mk(s, (int)(rnd() % 250) - 125, m23);
    case 5: case 6: return mk(s, (int)(rnd() % 41) - 20, m52);
    case 7: return mk(s, (int)(rnd() % 601) - 300, m52);
    case 8: return mk(s, (int)(rnd() % 21) - 10, m23 & ~(0x3FFFFull << 29));  // few mantissa bits: ties and cancellation
    default: return mk(s, (int)(rnd() % 6) - 3, m23);
    }
}
// A value stfs / psq_st can store without the tiny-value cases the JIT flushes (biased exponent >= 897 or zero).
static u64 fval_st(void) {
    for (;;) {
        u64 v = fval();
        u32 e = (u32)((v >> 52) & 0x7FF);
        if (e >= 897 || (v << 1) == 0) return v;
    }
}

static u32 gqr_rand(void) {
    u32 q = rnd() << 16 | rnd();
    if (rnd() & 1) q &= ~0x3F070000u;  // load direction: plain float
    if (rnd() & 1) q &= ~0x00003F07u;  // store direction: plain float
    if (rnd() % 4 == 0) q = 0;
    return q;
}

static u32 genf(St* st) {
    u32 frd = rbits(5), fra = rbits(5), frb = rbits(5), frc = rbits(5), rc = rnd() % 12 == 0;
    for (;;) {
        switch (rnd() % 14) {
        case 0: case 1: {  // lfs lfsu lfd lfdu stfs stfsu stfd stfdu
            u32 op = 48 + rnd() % 8;
            bool dbl = op == 50 || op == 51 || op == 54 || op == 55, store = op >= 52;
            u32 size = dbl ? 8 : 4, ra = 1 + rnd() % 31;
            float_base(st, ra, size);
            u32 d = (u32)(s32)((s32)(rnd() % 512) - 256);
            if (rnd() % 8) d &= ~(size - 1);
            if (store && !dbl) { st->fpr[frd][0] = fval_st(); }
            st->fmem = true;
            return (op << 26) | (frd << 21) | (ra << 16) | (d & 0xFFFF);
        }
        case 2: {  // lfsx lfsux lfdx lfdux stfsx stfsux stfdx stfdux
            u32 n = rnd() % 8, xo = 535 + 32 * n, ra = 1 + rnd() % 31, rb = 1 + rnd() % 31;
            bool dbl = (n >> 1) & 1, store = n >= 4;
            if (rb == ra) rb = (ra % 31) + 1;
            float_base(st, ra, dbl ? 8 : 4);
            st->gpr[rb] = rnd() % 256;
            if (rnd() % 8) st->gpr[rb] &= dbl ? ~7u : ~3u;
            if (store && !dbl) st->fpr[frd][0] = fval_st();
            st->fmem = true;
            return (31u << 26) | (frd << 21) | (ra << 16) | (rb << 11) | (xo << 1);
        }
        case 3: case 4: {  // psq_l psq_lu psq_st psq_stu
            static const u32 ops[] = { 56, 57, 60, 61 };
            u32 op = ops[rnd() % 4], ra = 1 + rnd() % 31, w = rnd() & 1, i = rbits(3);
            float_base(st, ra, 4);
            u32 d = (u32)(s32)((s32)(rnd() % 256) - 128);
            if (rnd() % 8) d &= ~3u;
            if (op >= 60) { st->fpr[frd][0] = fval(); st->fpr[frd][1] = fval(); }
            st->fmem = true;
            return (op << 26) | (frd << 21) | (ra << 16) | (w << 15) | (i << 12) | (d & 0xFFF);
        }
        case 5: {  // psq_lx psq_stx psq_lux psq_stux
            static const u32 xos[] = { 6, 7, 38, 39 };
            u32 xo = xos[rnd() % 4], ra = 1 + rnd() % 31, rb = 1 + rnd() % 31, w = rnd() & 1, i = rbits(3);
            if (rb == ra) rb = (ra % 31) + 1;
            float_base(st, ra, 4);
            st->gpr[rb] = rnd() % 256;
            if (rnd() % 8) st->gpr[rb] &= ~3u;
            if (xo & 1) { st->fpr[frd][0] = fval(); st->fpr[frd][1] = fval(); }
            st->fmem = true;
            return (4u << 26) | (frd << 21) | (ra << 16) | (rb << 11) | (w << 10) | (i << 7) | (xo << 1);
        }
        case 6: case 7: case 8: {  // single and double arithmetic, including forms left to the interpreter
            static const u32 x59[] = { 18, 20, 21, 25, 28, 29, 30, 31, 24 };
            static const u32 x63[] = { 18, 20, 21, 25, 28, 29, 30, 31, 23, 26 };
            bool single = rnd() & 1;
            u32 xo5 = single ? x59[rnd() % 9] : x63[rnd() % 10];
            return ((single ? 59u : 63u) << 26) | (frd << 21) | (fra << 16) | (frb << 11) | (frc << 6) | (xo5 << 1) | rc;
        }
        case 9: {  // fcmpu fcmpo frsp fctiw fctiwz fneg fmr fabs fnabs mffs
            static const u32 xos[] = { 0, 32, 12, 14, 15, 40, 72, 264, 136, 583 };
            u32 xo = xos[rnd() % 10];
            if (xo == 0 || xo == 32) {  // equal, signed-zero and unordered operands
                u32 k = rnd() % 8;
                if (k == 0) { st->fpr[frb][0] = st->fpr[fra][0]; }
                else if (k == 1) { st->fpr[fra][0] = 1ull << 63; st->fpr[frb][0] = 0; }
                else if (k == 2) { st->fpr[(rnd() & 1) ? fra : frb][0] = (rnd() & 1) ? 0x7FF8000000000000ull : 0x7FF4000000000000ull; }
                return (63u << 26) | (rbits(3) << 23) | (fra << 16) | (frb << 11) | (xo << 1);
            }
            if (xo == 12) st->fpr[frb][0] = fval_st();  // frsp of a tiny double: the interpreter keeps a single denormal
            return (63u << 26) | (frd << 21) | (frb << 11) | (xo << 1) | rc;
        }
        case 10: case 11: case 12: case 13: {  // paired singles
            static const u32 x5[] = { 10, 11, 12, 13, 14, 15, 18, 20, 21, 23, 24, 25, 26, 28, 29, 30, 31 };
            static const u32 xs[] = { 0, 32, 64, 96, 40, 72, 136, 264, 528, 560, 592, 624 };
            if (rnd() % 3 == 0) {
                u32 xo = xs[rnd() % 12];
                if (xo <= 96) return (4u << 26) | (rbits(3) << 23) | (fra << 16) | (frb << 11) | (xo << 1);
                return (4u << 26) | (frd << 21) | (fra << 16) | (frb << 11) | (xo << 1) | rc;
            }
            u32 xo5 = x5[rnd() % 17];
            return (4u << 26) | (frd << 21) | (fra << 16) | (frb << 11) | (frc << 6) | (xo5 << 1) | rc;
        }
        default:
            continue;
        }
    }
}

// ---- comparison ----
enum { T_SINGLE_FTZ = 1, T_DOUBLE_FTZ = 2, T_FUSED_S = 4, T_FUSED_D = 8 };

// What the JIT is allowed to get wrong for this instruction (see the file header).
static int tolerance(u32 inst) {
    u32 op = inst >> 26, xo = (inst >> 1) & 0x3FF, xo5 = xo & 31;
    int t = 0;
    if (op == 48 || op == 49 || op == 56 || op == 57 || op == 4) t |= T_SINGLE_FTZ;  // lfs, psq_l, paired singles
    if (op == 31 && (xo == 535 || xo == 567)) t |= T_SINGLE_FTZ;
    if (op == 59) t |= T_SINGLE_FTZ;
    if (op == 63) t |= xo == 12 ? T_SINGLE_FTZ : T_DOUBLE_FTZ;
    if ((op == 59 || op == 63 || op == 4) && xo5 >= 28)
        t |= op == 63 ? T_FUSED_D : T_FUSED_S;
    if (op == 4 && (xo5 == 14 || xo5 == 15)) t |= T_FUSED_S;
    return t;
}

static bool is_nan64(u64 v) { return (v & 0x7FF0000000000000ull) == 0x7FF0000000000000ull && (v & FRAC); }
static u32 sbits(double d) { float f = (float)d; u32 b; memcpy(&b, &f, 4); return b; }
static bool lane_ok(u64 jitv, u64 ref, int t, int prodExp) {
    if (jitv == ref) return true;
    if (is_nan64(jitv) && is_nan64(ref)) return true;
    u64 aj = jitv & ~(1ull << 63), ar = ref & ~(1ull << 63);
    if ((t & T_SINGLE_FTZ) && ar != 0 && ar < 0x3810000000000000ull && aj == 0) return true;  // below 2^-126 -> zero
    if ((t & T_DOUBLE_FTZ) && ar != 0 && ar < 0x0010000000000000ull && aj == 0) return true;
    if ((t & T_FUSED_D) && prodExp > -4000) {  // the product is rounded before the add: error up to an ulp of the product plus an ulp of the result
        double dj, dr;
        memcpy(&dj, &jitv, 8); memcpy(&dr, &ref, 8);
        int er = (int)((ar >> 52) & 0x7FF) - 1023;
        if (er > -1000 && er < 1000 && fabs(dj - dr) <= ldexp(1.0, prodExp - 51) + ldexp(1.0, er - 51)) return true;
    }
    if (t & T_FUSED_S) {
        double dj, dr;
        memcpy(&dj, &jitv, 8); memcpy(&dr, &ref, 8);
        u32 bj = sbits(dj), br = sbits(dr);
        if ((bj >> 31) == (br >> 31) && ((bj & 0x7FFFFFFFu) > (br & 0x7FFFFFFFu) ? (bj & 0x7FFFFFFFu) - (br & 0x7FFFFFFFu)
                                                                                  : (br & 0x7FFFFFFFu) - (bj & 0x7FFFFFFFu)) <= 1)
            return true;
    }
    return false;
}

static bool compare(u32 inst, u32 inst2, int n, int expect) {
    Gekko* a = &A.cpu;
    Gekko* b = &B.cpu;
    bool ok = true;
    char dis[96];
    gekko_disasm(CODE_EA, inst, dis, sizeof dis);
    for (int r = 0; r < 32; r++)
        if (a->gpr[r] != b->gpr[r]) { if (ok) printf("MISMATCH %08x  %s\n", inst, dis); printf("  r%d jit %08x interp %08x\n", r, a->gpr[r], b->gpr[r]); ok = false; }
#define CHK(f) if (a->f != b->f) { if (ok) printf("MISMATCH %08x  %s\n", inst, dis); printf("  " #f " jit %08x interp %08x\n", a->f, b->f); ok = false; }
    CHK(cr) CHK(xer) CHK(lr) CHK(ctr) CHK(pc) CHK(msr) CHK(spr[SPR_SRR0]) CHK(spr[SPR_SRR1]) CHK(spr[SPR_DAR]) CHK(spr[SPR_DSISR])
    CHK(spr[SPR_SPRG0]) CHK(spr[SPR_SPRG0 + 3]) CHK(spr[SPR_GQR0]) CHK(spr[SPR_GQR0 + 5]) CHK(spr[SPR_GQR0 + 7])
#undef CHK
    int t = tolerance(inst), prodExp = -9999;
    if (t & T_FUSED_D) {  // exponent of a * c for the double multiply-add forms
        u64 ia = g_in[(inst >> 16) & 31][0], ic = g_in[(inst >> 6) & 31][0];
        prodExp = (int)((ia >> 52) & 0x7FF) + (int)((ic >> 52) & 0x7FF) - 2046 + 1;
    }
    for (int r = 0; r < 32; r++)
        for (int l = 0; l < 2; l++) {
            u64 vj, vr;
            memcpy(&vj, l ? &a->fpr[r].ps1 : &a->fpr[r].ps0, 8);
            memcpy(&vr, l ? &b->fpr[r].ps1 : &b->fpr[r].ps0, 8);
            if (!lane_ok(vj, vr, t, prodExp)) {
                if (ok) printf("MISMATCH %08x  %s\n", inst, dis);
                printf("  f%d.ps%d jit %016llx interp %016llx\n", r, l, (unsigned long long)vj, (unsigned long long)vr);
                ok = false;
            }
        }
    u32 op = inst >> 26, xo = (inst >> 1) & 0x3FF;
    if ((op == 63 && (xo == 0 || xo == 32)) || (op == 4 && (xo == 0 || xo == 32 || xo == 64 || xo == 96)))
        if ((a->fpscr ^ b->fpscr) & 0xF000u) {  // FPSCR[FPCC]
            if (ok) printf("MISMATCH %08x  %s\n", inst, dis);
            printf("  fpscr.FPCC jit %x interp %x\n", (a->fpscr >> 12) & 15, (b->fpscr >> 12) & 15);
            ok = false;
        }
    if (memcmp(A.mem1 + MEM1_SIZE - TOP_SIZE, B.mem1 + MEM1_SIZE - TOP_SIZE, TOP_SIZE)) {
        if (ok) printf("MISMATCH %08x  %s\n", inst, dis);
        printf("  memory at the end of MEM1 differs\n");
        ok = false;
    }
    if (memcmp(A.mem1 + WIN_PA, B.mem1 + WIN_PA, WIN_SIZE)) {
        if (ok) printf("MISMATCH %08x  %s\n", inst, dis);
        for (u32 i = 0; i < WIN_SIZE; i++) if (A.mem1[WIN_PA + i] != B.mem1[WIN_PA + i]) { printf("  mem %08x jit %02x interp %02x\n", WIN_PA + i, A.mem1[WIN_PA + i], B.mem1[WIN_PA + i]); break; }
        ok = false;
    }
    if (A.gx.wgpLen != B.gx.wgpLen || memcmp(A.gx.wgp, B.gx.wgp, sizeof A.gx.wgp) || A.gx.bytesReceived != B.gx.bytesReceived) {
        if (ok) printf("MISMATCH %08x  %s\n", inst, dis);
        printf("  write-gather pipe differs (jit %u bytes pending, interp %u)\n", A.gx.wgpLen, B.gx.wgpLen);
        ok = false;
    }
    if (n != expect) {
        if (ok) printf("MISMATCH %08x  %s\n", inst, dis);
        printf("  JIT executed %d instructions, expected %d (second %08x)\n", n, expect, inst2);
        ok = false;
    }
    return ok;
}

// Memory window words that are normal single-precision floats (a quarter with any exponent).
static void fill_floats(System* s) {
    for (u32 i = 0; i < WIN_SIZE; i += 4) {
        u32 e = (rnd() % 4) ? 100 + rnd() % 50 : 1 + rnd() % 254;
        u32 w = (rnd() & 1) << 31 | e << 23 | (rnd() & 0x7FFFFF);
        mem1_write32(s, WIN_PA + i, w);
    }
}

// ---- traces ----
// The JIT follows an unconditional b and compiles a conditional branch as a side exit, so a branch can sit in the middle
// of a block. A program is a few instructions in which some are b / bc / bclr / bcctr that jump forward inside it (or
// not at all); the interpreter is run along the same path for as many instructions as the JIT ran.
static u32 gen_trace_branch(u32 i, u32 len, u32 prev) {
    static const u32 kBo[] = { 0, 2, 4, 8, 10, 12, 16, 18 };  // none of them "branch always"
    u32 kind = rnd() % 5;
    u32 target = i + 2 + rnd() % 4;
    if (target > len) target = len;
    u32 delta = ((target - i) * 4) & 0x03FFFFFCu;
    if (kind == 0) return (18u << 26) | delta;                         // b
    u32 bo = kBo[rnd() % 8], bi = rnd() % 32;
    int f = crf_set(prev);
    if (f >= 0 && rnd() % 3) { bo = (rnd() & 1) ? 4 : 12; bi = (u32)f * 4 + rnd() % 3; }  // branch on the compare just before
    if (kind <= 2) return (16u << 26) | (bo << 21) | (bi << 16) | (delta & 0xFFFCu);       // bc
    if (kind == 3) return (19u << 26) | (bo << 21) | (bi << 16) | (16u << 1);            // bclr
    return (19u << 26) | ((rnd() & 1 ? 4u : 12u) << 21) | (bi << 16) | (528u << 1);       // bcctr (no CTR decrement)
}

int main(int argc, char** argv) {
    u32 cases = argc > 1 ? (u32)atoi(argv[1]) : 20000;
    if (argc > 2) rng ^= strtoull(argv[2], NULL, 0) * 0x2545F4914F6CDD1Dull;
    g_logMask = 0;  // unmapped-access and reserved-GQR messages from the deliberately wild addresses
    const char* pe = getenv("JIT_FUZZ_PLANT");
    int plant = pe ? atoi(pe) : 0;
    if (!sys_init(&A) || !sys_init(&B)) { printf("init failed\n"); return 2; }
    boot_like(&A);
    boot_like(&B);
    jit = jit_create(&A.cpu, A.mem1, MEM1_SIZE, 4u << 20);
    if (!jit) { printf("jit_create failed\n"); return 2; }
    jit_debug_max_insns = 1;
    jit_set_gather(jit, gp_wgp_put, &A, GP_WGP_EA);
    if (getenv("JIT_FUZZ_TRACE")) jit_debug_on_compile = dump_block;
    if (plant == 2) jit_debug_plant = 1;
    if (plant >= 3) jit_debug_plant = (u32)plant;
    u32 fails = 0, floats = 0;
    for (u32 k = 0; k < cases && fails < 10; k++) {
        St st;
        memset(&st, 0, sizeof st);
        for (int r = 0; r < 32; r++) st.gpr[r] = interesting();
        st.cr = rnd() << 16 | rnd();
        st.xer = (rnd() << 16 | rnd()) & 0xE000007Fu;
        st.lr = (rnd() << 16 | rnd()) & ~3u;
        st.ctr = rnd() << 16 | rnd();
        for (int r = 0; r < 32; r++) { st.fpr[r][0] = fval(); st.fpr[r][1] = fval(); }
        for (int i = 0; i < 8; i++) st.gqr[i] = gqr_rand();
        st.fpscr = rnd() % 8 == 0 ? rnd() & 7 : 0;  // RN != 0: the JIT must leave every float instruction to the interpreter; NI: flush, as the host does
        st.fpOff = rnd() % 40 == 0;
        st.user = rnd() % 30 == 0;
        bool isFloat = rnd() % 100 < 55;
        // Integer cases are often blocks of 2-4 instructions: each later one often reads a GPR an earlier one wrote,
        // or branches on its compare (the JIT's register cache, forwarding of flags and write-back at exits).
        u32 seq[16] = { isFloat ? genf(&st) : gen(&st), 0 }, len = 1;
        bool traceCase = !isFloat && rnd() % 100 < 30 && !is_branch_op(seq[0]);
        if (traceCase) {
            len = 4 + rnd() % 8;
            for (u32 i = 1; i < len; i++) {
                if (rnd() % 100 < 28) { seq[i] = gen_trace_branch(i, len, seq[i - 1]); continue; }
                u32 next = gen(&st);
                while (is_branch_op(next)) next = gen(&st);
                int d = dest_gpr(seq[i - 1 - (rnd() % i)]);
                if (d >= 0 && (rnd() % 3)) next = read_gpr(next, (u32)d);
                seq[i] = next;
            }
        } else if (!isFloat && rnd() % 2 == 0 && !is_branch_op(seq[0])) {
            u32 want = 2 + rnd() % 3;
            while (len < want && !is_branch_op(seq[len - 1])) {
                u32 next = gen(&st);
                int d = dest_gpr(seq[len - 1 - (rnd() % len)]), f = crf_set(seq[len - 1]);
                if (d >= 0 && (rnd() % 3)) next = read_gpr(next, (u32)d);
                if (f >= 0 && (next >> 26) == 16 && rnd() % 4) next = (next & ~(31u << 16)) | (((u32)f * 4 + rnd() % 4) << 16);
                seq[len++] = next;
            }
        }
        u32 inst = seq[0], inst2 = seq[1];
        floats += isFloat;
        CODE_PA = 0x3000u + 4 * (rnd() % (1024 - len + 1));
        for (u32 i = 0; i < WIN_SIZE; i++) A.mem1[WIN_PA + i] = B.mem1[WIN_PA + i] = (u8)rnd();
        for (u32 i = 0; i < TOP_SIZE; i++) A.mem1[MEM1_SIZE - TOP_SIZE + i] = B.mem1[MEM1_SIZE - TOP_SIZE + i] = (u8)rnd();
        if (st.fmem && rnd() % 8) { fill_floats(&A); memcpy(B.mem1 + WIN_PA, A.mem1 + WIN_PA, WIN_SIZE); }
        for (u32 i = 0; i < 16; i++) { mem1_write32(&A, CODE_PA + 4 * i, seq[i]); mem1_write32(&B, CODE_PA + 4 * i, seq[i]); }
        memcpy(g_in, st.fpr, sizeof g_in);
        System* sys[2] = { &A, &B };
        for (int m = 0; m < 2; m++) {
            Gekko* g = &sys[m]->cpu;
            memcpy(g->gpr, st.gpr, sizeof st.gpr);
            g->cr = st.cr; g->xer = st.xer; g->lr = st.lr; g->ctr = st.ctr; g->pc = CODE_EA;
            for (int r = 0; r < 32; r++) { memcpy(&g->fpr[r].ps0, &st.fpr[r][0], 8); memcpy(&g->fpr[r].ps1, &st.fpr[r][1], 8); }
            for (int i = 0; i < 8; i++) g->spr[SPR_GQR0 + i] = st.gqr[i];
            g->fpscr = st.fpscr;
            u32 msr = MSR_BASE | (st.fpOff ? 0 : MSR_FP) | (st.user ? MSR_PR : 0);
            if (g->msr != msr) gekko_set_msr(g, msr);
            g->spr[SPR_SRR0] = g->spr[SPR_SRR1] = g->spr[SPR_DAR] = g->spr[SPR_DSISR] = 0;
            for (int i = 0; i < 4; i++) g->spr[SPR_SPRG0 + i] = 0;
            gekko_set_external_irq(g, st.irq);
        }
        jit_invalidate_all(jit);
        jit_debug_max_insns = len;
        if (getenv("JIT_FUZZ_TRACE")) { printf("case %u: %08x %08x %08x %08x\n", k, seq[0], seq[1], seq[2], seq[3]); fflush(stdout); }
        int n = (int)jit_run_one_block(jit);
        if (plant == 1 && k == cases / 2) A.cpu.gpr[3] ^= 1;
        // The interpreter runs the sequence while each instruction goes on to the next one, except that compiled code
        // must stop after an mtmsr that changed more than EE or set EE with an interrupt pending (other stops are the
        // JIT's choice: the interpreter goes on only as far as the JIT did).
        int expect = 0;
        if (traceCase) {  // follow the interpreter along the same path, for as many instructions as the JIT ran
            for (int step = 0; step < n; step++) {
                bool stop;
                u32 w = mem1_read32(&B, (B.cpu.pc & 0x01FFFFFFu));
                B.cpu.pc = gekko_interp_one(&B.cpu, B.cpu.pc, w, &stop);
                expect++;
            }
        }
        for (u32 i = 0; !traceCase && i < len; i++) {
            bool stop;
            u32 msrBefore = B.cpu.msr;
            B.cpu.pc = gekko_interp_one(&B.cpu, CODE_EA + 4 * i, seq[i], &stop);
            expect++;
            bool mtmsr = (seq[i] >> 26) == 31 && ((seq[i] >> 1) & 0x3FF) == 146;
            bool mustStop = stop && mtmsr && (((msrBefore ^ B.cpu.msr) & ~MSR_EE) || (st.irq && (B.cpu.msr & ~msrBefore & MSR_EE)));
            if (B.cpu.pc != CODE_EA + 4 * (i + 1) || mustStop || (stop && n <= expect)) break;
        }
        if (!compare(inst, inst2, n, expect)) {
            if (!fails && !plant) {  // keep the first failing block for disassembly
                FILE* d = fopen("/tmp/jit_fail.bin", "wb");
                if (d) { fwrite(jit_debug_last_code, 4, jit_debug_last_words, d); fclose(d); }
            }
            fails++;
        }
    }
    if (getenv("JIT_FUZZ_STATS")) {  // what the JIT left to the interpreter: static = at compile time, dynamic = at run time
        const JitStats* s = jit_stats(jit);
        printf("native %llu of %llu compiled; run-time fallbacks %llu\n", (unsigned long long)s->instrsNative,
               (unsigned long long)s->instrsCompiled, (unsigned long long)s->fallbacksRun);
        for (int pass = 0; pass < 2; pass++) {
            const JitOpCounts* t = pass ? &s->fallbackRun : &s->fallback;
            printf("%s:", pass ? "dynamic" : "static");
            for (int i = 48; i < 62; i++) if (t->primary[i]) printf(" op%d=%u", i, t->primary[i]);
            for (int i = 0; i < 32; i++) if (t->x59[i]) printf(" 59.%d=%u", i, t->x59[i]);
            for (int i = 0; i < 1024; i++) if (t->x63[i]) printf(" 63.%d=%u", i, t->x63[i]);
            for (int i = 0; i < 1024; i++) if (t->x4[i]) printf(" 4.%d=%u", i, t->x4[i]);
            printf("\n");
        }
    }
    {
        const JitStats* s = jit_stats(jit);
        printf("trace cases: %llu unconditional branches followed, %llu conditional branches as side exits, %llu fused compare-branches\n",
               (unsigned long long)s->traceFollowed, (unsigned long long)s->traceSideExits, (unsigned long long)s->fusedBranches);
    }
    printf("JIT FUZZ cases=%u (float %u) fails=%u%s\n", cases, floats, fails, plant ? " (planted)" : "");
    // Leave the process as it was (gc3ds goes on to start normally after an on-device run).
    jit_destroy(jit);
    jit = NULL;
    sys_free(&A);
    sys_free(&B);
    jit_debug_max_insns = 0;
    jit_debug_plant = 0;
    jit_debug_on_compile = NULL;
    return fails ? 1 : 0;
}
