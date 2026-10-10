// Directed tests for Gekko behaviour qemu cannot check: paired singles, quantized loads/stores, exceptions,
// BAT translation, timebase/decrementer, reservations, string instructions, Gekko FPU specifics and SPR side effects.
// Expected values come from the manuals cited per test:
//   [750CL]  IBM PowerPC 750CL RISC Microprocessor User's Manual
//   [PEM]    PowerPC Microprocessor Family: The Programming Environments Manual for 32-bit Microprocessors
// Each CHECK prints on failure; the program ends with "DIRECTED pass=<n> fail=<m>".
#include "tb.h"
#include "../../src/cpu/gekko_internal.h"
#include "../../src/cpu/gekko_softfp.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static Gekko g;
static TestBus tb;
static int npass, nfail;
static const char* cur = "";

#define CHECK(cond, ...)                                                    \
    do {                                                                    \
        if (cond) npass++;                                                  \
        else {                                                              \
            nfail++;                                                        \
            printf("FAIL [%s] %s:%d: ", cur, __FILE__, __LINE__);           \
            printf(__VA_ARGS__);                                            \
            printf("\n");                                                   \
        }                                                                   \
    } while (0)
#define CHECK_EQ(a, b) CHECK((u64)(a) == (u64)(b), "%s = %llx, expected %llx", #a, (unsigned long long)(u64)(a), (unsigned long long)(u64)(b))

// ---- Encoders ----
#define OP(p) ((u32)(p) << 26)
static u32 D(u32 op, u32 d, u32 a, u32 imm) { return OP(op) | d << 21 | a << 16 | (imm & 0xFFFF); }
static u32 X(u32 op, u32 d, u32 a, u32 b, u32 xo, u32 rc) { return OP(op) | d << 21 | a << 16 | b << 11 | xo << 1 | rc; }
static u32 A(u32 op, u32 d, u32 a, u32 b, u32 c, u32 xo) { return OP(op) | d << 21 | a << 16 | b << 11 | c << 6 | xo << 1; }
static u32 MTSPR(u32 spr, u32 s) { return OP(31) | s << 21 | (spr & 31) << 16 | (spr >> 5) << 11 | 467 << 1; }
static u32 MFSPR(u32 d, u32 spr) { return OP(31) | d << 21 | (spr & 31) << 16 | (spr >> 5) << 11 | 339 << 1; }
static u32 PSQ(u32 op, u32 f, u32 a, u32 w, u32 i, s32 off) { return OP(op) | f << 21 | a << 16 | w << 15 | i << 12 | ((u32)off & 0xFFF); }
static u32 PSQX(u32 xo, u32 f, u32 a, u32 b, u32 w, u32 i) { return OP(4) | f << 21 | a << 16 | b << 11 | w << 10 | i << 7 | xo << 1; }
#define NOP 0x60000000u
#define LI(d, v) D(14, d, 0, (u32)(v))
#define SC 0x44000002u
#define MTMSR(s) X(31, s, 0, 0, 146, 0)
#define RFI X(19, 0, 0, 0, 50, 0)

static u64 d2u(double d) { u64 v; memcpy(&v, &d, 8); return v; }
static double u2d(u64 v) { double d; memcpy(&d, &v, 8); return d; }
static u64 F0(int r) { return fpr_get0(&g, r); }
static u64 F1(int r) { return fpr_get1(&g, r); }
static void setF(int r, double a, double b) { fpr_set0(&g, r, d2u(a)); fpr_set1(&g, r, d2u(b)); }

#define CODE 0x00100000u
static u32 here;

static void reset_cpu(void) {
    gekko_reset(&g);
    gekko_set_msr(&g, MSR_FP);
    gekko_set_spr(&g, SPR_HID2, HID2_LSQE | HID2_PSE);
    memset(tb.ram, 0, 0x00400000);
    for (u32 v = 0; v < 0x1000; v += 0x100) tb_w32(&tb, v, NOP);  // a nop at every vector
    here = CODE;
    g.pc = CODE;
    tb.fatalCount = 0;
    tb.noSyscallTrap = true;
}
static void emit(u32 inst) { tb_w32(&tb, here, inst); here += 4; }
// Executes exactly n instructions (interrupt checks happen between them as in a normal run).
static void step(u32 n) { gekko_run(&g, n); }

// ---- Paired-single arithmetic ([750CL] chapter on paired-single instructions; lane rules in gekko_fpu.c) ----
static void t_paired(void) {
    cur = "paired arithmetic";
    reset_cpu();
    setF(1, 1.5, -2.0);
    setF(2, 2.25, 0.5);
    setF(3, 4.0, 3.0);
    emit(A(4, 4, 1, 2, 0, 21));      // ps_add f4,f1,f2
    emit(A(4, 5, 1, 2, 0, 20));      // ps_sub f5,f1,f2
    emit(A(4, 6, 1, 0, 3, 25));      // ps_mul f6,f1,f3
    emit(A(4, 7, 1, 2, 0, 18));      // ps_div f7,f1,f2
    emit(A(4, 8, 1, 2, 3, 29));      // ps_madd f8,f1,f3,f2 = f1*f3+f2
    emit(A(4, 9, 1, 2, 3, 28));      // ps_msub
    emit(A(4, 10, 1, 2, 3, 31));     // ps_nmadd
    emit(A(4, 11, 1, 2, 3, 30));     // ps_nmsub
    emit(A(4, 12, 1, 2, 3, 10));     // ps_sum0: ps0 = a0 + b1, ps1 = c1
    emit(A(4, 13, 1, 2, 3, 11));     // ps_sum1: ps0 = c0, ps1 = a0 + b1
    emit(A(4, 14, 1, 0, 3, 12));     // ps_muls0: a * c0
    emit(A(4, 15, 1, 0, 3, 13));     // ps_muls1: a * c1
    emit(A(4, 16, 1, 2, 3, 14));     // ps_madds0: a * c0 + b
    emit(A(4, 17, 1, 2, 3, 15));     // ps_madds1: a * c1 + b
    emit(A(4, 18, 1, 2, 3, 23));     // ps_sel: a >= 0 ? c : b  (lane0 1.5 -> c0, lane1 -2 -> b1)
    step(15);
    CHECK(u2d(F0(4)) == 3.75 && u2d(F1(4)) == -1.5, "ps_add %g %g", u2d(F0(4)), u2d(F1(4)));
    CHECK(u2d(F0(5)) == -0.75 && u2d(F1(5)) == -2.5, "ps_sub");
    CHECK(u2d(F0(6)) == 6.0 && u2d(F1(6)) == -6.0, "ps_mul");
    CHECK(u2d(F0(7)) == (double)(float)(1.5 / 2.25) && u2d(F1(7)) == -4.0, "ps_div %.17g", u2d(F0(7)));
    CHECK(u2d(F0(8)) == 8.25 && u2d(F1(8)) == -5.5, "ps_madd");
    CHECK(u2d(F0(9)) == 3.75 && u2d(F1(9)) == -6.5, "ps_msub");
    CHECK(u2d(F0(10)) == -8.25 && u2d(F1(10)) == 5.5, "ps_nmadd");
    CHECK(u2d(F0(11)) == -3.75 && u2d(F1(11)) == 6.5, "ps_nmsub");
    CHECK(u2d(F0(12)) == 2.0 && u2d(F1(12)) == 3.0, "ps_sum0 %g %g", u2d(F0(12)), u2d(F1(12)));
    CHECK(u2d(F0(13)) == 4.0 && u2d(F1(13)) == 2.0, "ps_sum1");
    CHECK(u2d(F0(14)) == 6.0 && u2d(F1(14)) == -8.0, "ps_muls0");
    CHECK(u2d(F0(15)) == 4.5 && u2d(F1(15)) == -6.0, "ps_muls1");
    CHECK(u2d(F0(16)) == 8.25 && u2d(F1(16)) == -7.5, "ps_madds0");
    CHECK(u2d(F0(17)) == 6.75 && u2d(F1(17)) == -5.5, "ps_madds1");
    CHECK(u2d(F0(18)) == 4.0 && u2d(F1(18)) == 0.5, "ps_sel");
    CHECK(((g.fpscr >> 12) & 0x1F) == 0x04, "FPRF after ps_madds1 (+normal from ps0) = %x", (g.fpscr >> 12) & 0x1F);

    cur = "paired moves/merges/compares";
    reset_cpu();
    setF(1, 1.5, -2.0);
    setF(2, -0.0, 7.0);
    fpr_set0(&g, 3, 0x7FF0000000000001ull);  // SNaN must be copied raw by moves
    fpr_set1(&g, 3, 0xFFF8000000000000ull);
    emit(X(4, 4, 0, 1, 40, 0));       // ps_neg
    emit(X(4, 5, 0, 1, 264, 0));      // ps_abs
    emit(X(4, 6, 0, 1, 136, 0));      // ps_nabs
    emit(X(4, 7, 0, 3, 72, 0));       // ps_mr
    emit(X(4, 8, 1, 2, 528, 0));      // ps_merge00
    emit(X(4, 9, 1, 2, 560, 0));      // ps_merge01
    emit(X(4, 10, 1, 2, 592, 0));     // ps_merge10
    emit(X(4, 11, 1, 2, 624, 0));     // ps_merge11
    emit(X(4, 1 << 2, 1, 2, 0, 0));   // ps_cmpu0 cr1: 1.5 vs -0.0 -> GT
    emit(X(4, 2 << 2, 1, 2, 64, 0));  // ps_cmpu1 cr2: -2 vs 7 -> LT
    emit(X(4, 3 << 2, 3, 1, 32, 0));  // ps_cmpo0 cr3: SNaN -> unordered, VXSNAN, VXVC (VE = 0)
    step(11);
    CHECK(u2d(F0(4)) == -1.5 && u2d(F1(4)) == 2.0, "ps_neg");
    CHECK(u2d(F0(5)) == 1.5 && u2d(F1(5)) == 2.0, "ps_abs");
    CHECK(u2d(F0(6)) == -1.5 && u2d(F1(6)) == -2.0, "ps_nabs");
    CHECK_EQ(F0(7), 0x7FF0000000000001ull);
    CHECK_EQ(F1(7), 0xFFF8000000000000ull);
    CHECK(u2d(F0(8)) == 1.5 && F1(8) == 0x8000000000000000ull, "ps_merge00");
    CHECK(u2d(F0(9)) == 1.5 && u2d(F1(9)) == 7.0, "ps_merge01");
    CHECK(u2d(F0(10)) == -2.0 && F1(10) == 0x8000000000000000ull, "ps_merge10");
    CHECK(u2d(F0(11)) == -2.0 && u2d(F1(11)) == 7.0, "ps_merge11");
    CHECK_EQ((g.cr >> 24) & 0xF, 4);
    CHECK_EQ((g.cr >> 20) & 0xF, 8);
    CHECK_EQ((g.cr >> 16) & 0xF, 1);
    CHECK((g.fpscr & (FPSCR_VXSNAN | FPSCR_VXVC | FPSCR_FX | FPSCR_VX)) == (FPSCR_VXSNAN | FPSCR_VXVC | FPSCR_FX | FPSCR_VX),
          "fcmpo SNaN flags %08x", g.fpscr);

    cur = "paired res/rsqrte";
    reset_cpu();
    setF(1, 3.0, 0.0);
    setF(2, 4.0, -1.0);
    emit(X(4, 4, 0, 1, 24, 0));  // ps_res f4,f1  [750CL]: estimate accurate to 1/4096
    emit(X(4, 5, 0, 2, 26, 0));  // ps_rsqrte f5,f2: lane1 negative -> VXSQRT, default NaN
    step(2);
    CHECK(fabs(u2d(F0(4)) * 3.0 - 1.0) <= 1.0 / 4096, "ps_res lane0 %.9g", u2d(F0(4)));
    CHECK_EQ(F1(4), 0x7FF0000000000000ull);  // 1/+0 = +inf with ZX
    CHECK(g.fpscr & FPSCR_ZX, "ps_res 1/0 sets ZX");
    CHECK(fabs(u2d(F0(5)) - 0.5) <= 0.5 / 32, "ps_rsqrte lane0 %.9g", u2d(F0(5)));
    CHECK_EQ(F1(5), SF_DNAN);
    CHECK(g.fpscr & FPSCR_VXSQRT, "ps_rsqrte negative sets VXSQRT");

    cur = "paired single rounding";
    reset_cpu();
    // Each lane rounds to single: 1 + 2^-30 is not representable and rounds to 1.0 (round to nearest).
    setF(1, 1.0 + ldexp(1, -30), 1.0);
    setF(2, 0.0, ldexp(1, -30));
    emit(A(4, 3, 1, 2, 0, 21));  // ps_add
    step(1);
    CHECK(u2d(F0(3)) == 1.0 && u2d(F1(3)) == 1.0, "ps_add rounds both lanes to single");
    CHECK((g.fpscr & (FPSCR_FI | FPSCR_XX)) == (FPSCR_FI | FPSCR_XX), "inexact lane sets FI/XX");

    cur = "paired HID2 gating";
    reset_cpu();
    gekko_set_spr(&g, SPR_HID2, 0);  // PSE = 0: paired singles are illegal ([750CL] HID2[PSE])
    emit(A(4, 3, 1, 2, 0, 21));
    step(1);
    CHECK_EQ(g.pc, 0x700);
    CHECK_EQ(g.spr[SPR_SRR1] & 0x00080000u, 0x00080000u);
    CHECK(tb.fatalCount == 1, "illegal instruction reported through gekko_on_fatal");
}

// ---- Quantized load/store ([750CL] "Quantization": GQR fields, dequantize = int * 2^-LD_SCALE,
// quantize = value * 2^ST_SCALE saturated to the type range) ----
static u32 gqr(u32 ldType, u32 ldScale, u32 stType, u32 stScale) {
    return (ldScale & 0x3F) << 24 | (ldType & 7) << 16 | (stScale & 0x3F) << 8 | (stType & 7);
}
static void t_quant(void) {
    static const struct { u32 type, size; bool sign; } types[] = {{4, 1, false}, {5, 2, false}, {6, 1, true}, {7, 2, true}};
    static const int scales[] = {0, 1, 5, 31, -32, -1, -7};
    u32 data = 0x00200000u;
    cur = "psq_l integer types";
    for (u32 t = 0; t < 4; t++) {
        for (u32 s = 0; s < sizeof scales / sizeof scales[0]; s++) {
            for (u32 w = 0; w < 2; w++) {
                reset_cpu();
                int sc = scales[s];
                gekko_set_spr(&g, SPR_GQR0 + 3, gqr(types[t].type, (u32)sc, 0, 0));
                // element 0 = all-ones pattern (max for unsigned, -1 for signed); element 1 = 0x80 / 0x8000
                u32 sz = types[t].size;
                for (u32 k = 0; k < sz; k++) tb.ram[data + k] = 0xFF;
                tb.ram[data + sz] = 0x80;
                if (sz == 2) tb.ram[data + sz + 1] = 0x00;
                g.gpr[5] = data;
                setF(1, 99.0, 99.0);
                emit(PSQ(56, 1, 5, w, 3, 0));
                step(1);
                double e0 = types[t].sign ? -1.0 : (sz == 1 ? 255.0 : 65535.0);
                double e1 = types[t].sign ? (sz == 1 ? -128.0 : -32768.0) : (sz == 1 ? 128.0 : 32768.0);
                e0 = ldexp(e0, -sc);
                e1 = w ? 1.0 : ldexp(e1, -sc);
                CHECK(u2d(F0(1)) == e0 && u2d(F1(1)) == e1, "type %u scale %d W=%u: got %.17g %.17g want %.17g %.17g",
                      types[t].type, sc, w, u2d(F0(1)), u2d(F1(1)), e0, e1);
            }
        }
    }
    cur = "psq_l float / update / indexed";
    reset_cpu();
    gekko_set_spr(&g, SPR_GQR0 + 0, 0);
    tb_w32(&tb, data, 0x3FC00000u);      // 1.5f
    tb_w32(&tb, data + 4, 0x7F800001u);  // SNaN: loaded without quieting
    g.gpr[5] = data - 8;
    g.gpr[6] = 8;
    emit(PSQ(57, 1, 5, 0, 0, 8));         // psq_lu f1,8(r5),0,0 -> r5 = data
    emit(PSQX(38, 2, 5, 0, 1, 0) | 0);    // psq_lux f2,r5,r0... (rB = r0 = 0 -> EA = r5)
    step(2);
    CHECK(u2d(F0(1)) == 1.5, "psq_lu float ps0");
    CHECK_EQ(F1(1), 0x7FF0000020000000ull);
    CHECK_EQ(g.gpr[5], data);
    CHECK(u2d(F0(2)) == 1.5 && u2d(F1(2)) == 1.0, "psq_lux W=1 sets ps1 = 1.0");

    cur = "psq_st integer types";
    // Values: in range, fractional (truncated toward zero), above max, below min.
    static const double vals[] = {3.75, -3.75, 1e9, -1e9, 0.5, -0.0};
    for (u32 t = 0; t < 4; t++) {
        for (u32 s = 0; s < sizeof scales / sizeof scales[0]; s++) {
            for (u32 v = 0; v + 1 < sizeof vals / sizeof vals[0]; v += 2) {
                reset_cpu();
                int sc = scales[s];
                gekko_set_spr(&g, SPR_GQR0 + 5, gqr(0, 0, types[t].type, (u32)sc));
                setF(1, vals[v], vals[v + 1]);
                g.gpr[5] = data;
                emit(PSQ(60, 1, 5, 0, 5, 0));
                step(1);
                u32 sz = types[t].size;
                for (u32 lane = 0; lane < 2; lane++) {
                    double x = ldexp((double)(float)vals[v + lane], sc);
                    double lo = types[t].sign ? (sz == 1 ? -128 : -32768) : 0;
                    double hi = types[t].sign ? (sz == 1 ? 127 : 32767) : (sz == 1 ? 255 : 65535);
                    double c = x < lo ? lo : x > hi ? hi : trunc(x);
                    u32 want = (u32)(s32)c & (sz == 1 ? 0xFF : 0xFFFF);
                    u32 got = sz == 1 ? tb.ram[data + lane] : (u32)(tb.ram[data + 2 * lane] << 8 | tb.ram[data + 2 * lane + 1]);
                    CHECK(got == want, "type %u scale %d value %g: stored %x want %x", types[t].type, sc, vals[v + lane], got, want);
                }
            }
        }
    }
    cur = "psq_st W / float / stux";
    reset_cpu();
    gekko_set_spr(&g, SPR_GQR0 + 1, gqr(0, 0, 4, 0));
    setF(1, 7.0, 9.0);
    tb.ram[data] = 0xAA;
    tb.ram[data + 1] = 0xAA;
    g.gpr[5] = data;
    emit(PSQ(60, 1, 5, 1, 1, 0));  // W = 1: only ps0
    gekko_set_spr(&g, SPR_GQR0 + 2, 0);
    setF(2, 1.5, -2.0);
    g.gpr[6] = data + 0x100;
    g.gpr[7] = 0x10;
    emit(PSQX(39, 2, 6, 7, 0, 2));  // psq_stux f2,r6,r7,0,2 -> r6 += 0x10
    step(2);
    CHECK(tb.ram[data] == 7 && tb.ram[data + 1] == 0xAA, "psq_st W=1 writes one element");
    CHECK_EQ(tb_r32(&tb, data + 0x110), 0x3FC00000u);
    CHECK_EQ(tb_r32(&tb, data + 0x114), 0xC0000000u);
    CHECK_EQ(g.gpr[6], data + 0x110);

    cur = "psq LSQE gating";
    reset_cpu();
    gekko_set_spr(&g, SPR_HID2, HID2_PSE);  // LSQE = 0: non-indexed psq_l/psq_st illegal, indexed forms allowed
    g.gpr[5] = data;
    emit(PSQX(6, 1, 0, 5, 0, 0));  // psq_lx: allowed
    emit(PSQ(56, 1, 5, 0, 0, 0));  // psq_l: illegal
    step(2);
    CHECK_EQ(g.pc, 0x700);
    CHECK_EQ(g.spr[SPR_SRR0], CODE + 4);
}

// ---- Exceptions ([PEM] 6.4, [750CL] chapter 4) ----
static void t_exceptions(void) {
    cur = "system call";
    reset_cpu();
    gekko_set_msr(&g, MSR_FP | MSR_EE | MSR_DR | MSR_IR | MSR_ME | MSR_RI);
    // Identity BATs so translation stays valid.
    gekko_set_spr(&g, SPR_IBAT0U, 0x00001FFFu);  // EA 0, 256 MB, Vs Vp
    gekko_set_spr(&g, SPR_IBAT0U + 1, 0x00000002u);
    gekko_set_spr(&g, SPR_DBAT0U, 0x00001FFFu);
    gekko_set_spr(&g, SPR_DBAT0U + 1, 0x00000002u);
    emit(SC);
    step(1);
    CHECK_EQ(g.spr[SPR_SRR0], CODE + 4);  // [PEM] sc: SRR0 = address of the next instruction
    CHECK_EQ(g.spr[SPR_SRR1], MSR_FP | MSR_EE | MSR_DR | MSR_IR | MSR_ME | MSR_RI);
    CHECK_EQ(g.msr, MSR_ME);  // EE, PR, FP, IR, DR, RI cleared; ME kept; IP = 0
    CHECK_EQ(g.pc, 0xC00);
    // rfi returns: MSR <- SRR1, PC <- SRR0.
    tb_w32(&tb, 0xC00, RFI);
    step(1);
    CHECK_EQ(g.pc, CODE + 4);
    CHECK_EQ(g.msr, MSR_FP | MSR_EE | MSR_DR | MSR_IR | MSR_ME | MSR_RI);

    cur = "vector base with MSR[IP]";
    reset_cpu();
    gekko_set_msr(&g, MSR_FP | MSR_IP);
    emit(SC);
    step(1);
    CHECK_EQ(g.spr[SPR_SRR0], CODE + 4);
    CHECK_EQ(g.msr, MSR_IP);
    CHECK((g.pc & 0xFFFFF000u) == 0xFFF00000u, "vector at 0xFFF00C00 (pc=%08x)", g.pc);

    cur = "illegal / privileged / trap";
    reset_cpu();
    emit(0x00000000u);  // primary opcode 0: illegal
    step(1);
    CHECK_EQ(g.spr[SPR_SRR0], CODE);
    CHECK_EQ(g.spr[SPR_SRR1] & 0xFFFF0000u, SRR1_ILLEGAL);
    CHECK_EQ(g.pc, 0x700);
    reset_cpu();
    gekko_set_msr(&g, MSR_FP | MSR_PR);
    emit(X(31, 3, 0, 0, 83, 0));  // mfmsr in user mode
    step(1);
    CHECK_EQ(g.spr[SPR_SRR1] & 0xFFFF0000u, SRR1_PRIV);
    CHECK_EQ(g.spr[SPR_SRR1] & MSR_PR, MSR_PR);
    reset_cpu();
    gekko_set_msr(&g, MSR_FP | MSR_PR);
    emit(MFSPR(3, SPR_HID2));  // supervisor SPR (bit 4 of the SPR number set)
    step(1);
    CHECK_EQ(g.spr[SPR_SRR1] & 0xFFFF0000u, SRR1_PRIV);
    reset_cpu();
    g.gpr[3] = 5;
    emit(D(3, 0x10, 3, 4));               // twi lt,r3,4: 5 < 4 false -> no trap
    emit(X(31, 0x04, 3, 3, 4, 0));        // tw eq,r3,r3 -> trap
    step(2);
    CHECK_EQ(g.spr[SPR_SRR0], CODE + 4);
    CHECK_EQ(g.spr[SPR_SRR1] & 0xFFFF0000u, SRR1_TRAP);

    cur = "FP unavailable";
    reset_cpu();
    gekko_set_msr(&g, 0);
    emit(A(63, 1, 2, 3, 0, 21));  // fadd
    step(1);
    CHECK_EQ(g.spr[SPR_SRR0], CODE);
    CHECK_EQ(g.pc, 0x800);
    reset_cpu();
    gekko_set_msr(&g, 0);
    emit(D(48, 1, 0, 0x100));  // lfs also requires MSR[FP]
    step(1);
    CHECK_EQ(g.pc, 0x800);

    cur = "external interrupt";
    reset_cpu();
    for (int k = 0; k < 8; k++) emit(NOP);
    step(2);
    gekko_set_external_irq(&g, true);  // EE = 0: held off
    step(2);
    CHECK_EQ(g.pc, CODE + 16);
    gekko_set_msr(&g, MSR_FP | MSR_EE);
    step(1);
    CHECK_EQ(g.spr[SPR_SRR0], CODE + 16);  // [PEM] external: SRR0 = next instruction to execute
    CHECK_EQ(g.pc, 0x504);
    CHECK_EQ(g.msr & MSR_EE, 0);
    gekko_set_external_irq(&g, false);

    cur = "mtmsr enabling EE takes a pending interrupt";
    reset_cpu();
    g.gpr[4] = MSR_FP | MSR_EE;
    emit(MTMSR(4));
    emit(NOP);
    gekko_set_external_irq(&g, true);
    step(2);
    CHECK_EQ(g.spr[SPR_SRR0], CODE + 4);
    CHECK_EQ(g.pc, 0x504);
    gekko_set_external_irq(&g, false);
}

// ---- BAT translation ([PEM] 7.4, [750CL] 4 IBAT + 4 DBAT) ----
static void t_bats(void) {
    cur = "DBAT mapping";
    reset_cpu();
    tb_w32(&tb, 0x00012340u, 0xCAFEBABEu);
    tb_w32(&tb, 0x01012340u, 0x12345678u);
    // DBAT0: EA 0x80000000, 256 MB (BL = 0x7FF), Vs = 1, -> PA 0, PP = RW
    gekko_set_spr(&g, SPR_DBAT0U, 0x80001FFEu);
    gekko_set_spr(&g, SPR_DBAT0U + 1, 0x00000002u);
    // DBAT1: EA 0xC0000000, 128 KB (BL = 0), -> PA 0x01000000, PP = read-only
    gekko_set_spr(&g, SPR_DBAT0U + 2, 0xC0000002u);
    gekko_set_spr(&g, SPR_DBAT0U + 3, 0x01000001u);
    // IBAT0 identity for the code.
    gekko_set_spr(&g, SPR_IBAT0U, 0x00001FFEu);
    gekko_set_spr(&g, SPR_IBAT0U + 1, 0x00000002u);
    gekko_set_msr(&g, MSR_FP | MSR_IR | MSR_DR);
    g.gpr[3] = 0x80012340u;
    g.gpr[4] = 0xC0012340u - 0x00010000u;  // 0xC0002340 is within the 128 KB block
    emit(D(32, 5, 3, 0));                // lwz r5,0(r3)
    emit(D(32, 6, 4, 0x0000));           // lwz r6,0(r4) -> PA 0x01002340
    step(2);
    CHECK_EQ(g.gpr[5], 0xCAFEBABEu);
    u32 pa = 0;
    CHECK(gekko_translate_data(&g, 0xC0002340u, &pa) && pa == 0x01002340u, "translate 0xC0002340 -> %08x", pa);
    CHECK(!gekko_translate_data(&g, 0xC0020000u, &pa), "past a 128 KB BAT block is unmapped");
    CHECK(gekko_translate_data(&g, 0x8FFFFFFCu, &pa) && pa == 0x0FFFFFFCu, "end of 256 MB block");

    cur = "DSI on missing translation / protection";
    g.gpr[7] = 0x40000000u;  // no BAT
    emit(D(32, 8, 7, 0x10));   // lwz r8,16(r7)
    step(1);
    CHECK_EQ(g.spr[SPR_SRR0], CODE + 8);
    CHECK_EQ(g.spr[SPR_DAR], 0x40000010u);
    CHECK_EQ(g.spr[SPR_DSISR], 0x40000000u);
    CHECK_EQ(g.pc, 0x300);
    CHECK_EQ(g.msr & (MSR_IR | MSR_DR), 0);
    // Back to translated mode; store into the read-only block -> protection DSI with the store bit.
    gekko_set_msr(&g, MSR_FP | MSR_IR | MSR_DR);
    g.pc = here;
    emit(D(36, 8, 4, 0));  // stw r8,0(r4)
    step(1);
    CHECK_EQ(g.spr[SPR_DSISR], 0x0A000000u);
    CHECK_EQ(g.spr[SPR_DAR], 0xC0002340u);

    cur = "Vs/Vp";
    // User mode with a BAT valid only for supervisor (Vs = 1, Vp = 0) -> DSI.
    gekko_set_spr(&g, SPR_IBAT0U, 0x00001FFFu);  // IBAT valid for both
    gekko_set_msr(&g, MSR_FP | MSR_IR | MSR_DR | MSR_PR);
    g.pc = here;
    emit(D(32, 5, 3, 0));
    step(1);
    CHECK_EQ(g.pc, 0x300);
    CHECK_EQ(g.spr[SPR_DSISR], 0x40000000u);

    cur = "ISI";
    reset_cpu();
    gekko_set_spr(&g, SPR_IBAT0U, 0x00001FFEu);
    gekko_set_spr(&g, SPR_IBAT0U + 1, 0x00000002u);
    gekko_set_msr(&g, MSR_FP | MSR_IR);
    g.gpr[3] = 0x50000000u;
    emit(X(31, 3, 9, 0, 467, 0));  // mtctr r3
    emit(0x4E800420u);             // bctr
    step(3);
    CHECK_EQ(g.spr[SPR_SRR0], 0x50000000u);
    CHECK_EQ(g.spr[SPR_SRR1] & 0x40000000u, 0x40000000u);
    CHECK_EQ(g.pc, 0x400);

    cur = "IBAT execution at a translated address";
    reset_cpu();
    gekko_set_spr(&g, SPR_IBAT0U, 0x80001FFEu);  // EA 0x80000000 -> PA 0
    gekko_set_spr(&g, SPR_IBAT0U + 1, 0x00000002u);
    gekko_set_msr(&g, MSR_FP | MSR_IR);
    g.pc = 0x80000000u | CODE;
    emit(LI(3, 42));
    step(1);
    CHECK_EQ(g.gpr[3], 42);
    CHECK_EQ(g.pc, 0x80000000u | (CODE + 4));

    cur = "MMIO through a BAT uses the bus";
    reset_cpu();
    gekko_set_spr(&g, SPR_DBAT0U, 0xCC000002u);  // EA 0xCC000000 128 KB -> PA 0x0C000000, cache-inhibited
    gekko_set_spr(&g, SPR_DBAT0U + 1, 0x0C00002Au);
    gekko_set_msr(&g, MSR_FP | MSR_DR);
    tb.mmio[1] = 0x11223344u;
    tb.mmioReads = tb.mmioWrites = 0;
    g.gpr[3] = 0xCC000000u;
    g.gpr[4] = 0xDEADBEEFu;
    emit(D(32, 5, 3, 4));   // lwz r5,4(r3)
    emit(D(36, 4, 3, 8));   // stw r4,8(r3)
    emit(D(40, 6, 3, 6));   // lhz r6,6(r3)
    step(3);
    CHECK_EQ(g.gpr[5], 0x11223344u);
    CHECK_EQ(tb.mmio[2], 0xDEADBEEFu);
    CHECK_EQ(g.gpr[6], 0x3344u);
    CHECK(tb.mmioReads >= 2 && tb.mmioWrites == 1, "bus reads %u writes %u", tb.mmioReads, tb.mmioWrites);
}

// ---- Timebase and decrementer (gekko.h: one tick per 12 cycles; [PEM] 2.2.x, 6.4.9) ----
static void t_timing(void) {
    cur = "timebase";
    reset_cpu();
    for (int k = 0; k < 40; k++) emit(NOP);
    emit(X(31, 3, 12, 8, 371, 0));  // mftb r3 (TBL = 268)
    emit(X(31, 4, 13, 8, 371, 0));  // mftb r4 (TBU = 269)
    step(42);
    CHECK_EQ(g.gpr[3], 40 / 12);
    CHECK_EQ(g.gpr[4], 0);
    // Write TB = 0x00000001_FFFFFFF0 at cycle 42; read it back 26 cycles later.
    reset_cpu();
    g.gpr[5] = 0xFFFFFFF0u;
    g.gpr[6] = 1;
    emit(MTSPR(SPR_TBL_W, 5));
    emit(MTSPR(SPR_TBU_W, 6));
    for (int k = 0; k < 22; k++) emit(NOP);
    emit(MFSPR(7, SPR_TBL_R));
    emit(MFSPR(8, SPR_TBU_R));
    step(26);
    // TBL written at cycle 0, TBU at cycle 1 (both tick 0); read at cycle 24 = tick 2.
    CHECK_EQ(g.gpr[7], 0xFFFFFFF2u);
    CHECK_EQ(g.gpr[8], 1);
    CHECK_EQ(gekko_get_spr(&g, SPR_TBL_R), 0xFFFFFFF2u);

    cur = "decrementer value";
    reset_cpu();
    g.gpr[3] = 100;
    emit(MTSPR(SPR_DEC, 3));  // cycle 0, tick 0
    for (int k = 0; k < 48; k++) emit(NOP);
    emit(MFSPR(4, SPR_DEC));  // cycle 49, tick 4
    step(50);
    CHECK_EQ(g.gpr[4], 96);

    cur = "decrementer exception";
    reset_cpu();
    g.gpr[3] = 2;
    g.gpr[4] = MSR_FP | MSR_EE;
    emit(MTSPR(SPR_DEC, 3));  // cycle 0: DEC = 2 -> 0 at tick 2, -1 at tick 3 (cycle 36)
    emit(MTMSR(4));
    for (int k = 0; k < 60; k++) emit(NOP);
    step(36);
    CHECK_EQ(g.pc, CODE + 36 * 4);  // no exception yet
    step(1);
    CHECK_EQ(g.spr[SPR_SRR0], CODE + 36 * 4);
    CHECK_EQ(g.pc, 0x904);
    CHECK_EQ(gekko_get_spr(&g, SPR_DEC), 0xFFFFFFFFu);
    // Not re-signalled while DEC keeps counting down from -1.
    gekko_set_msr(&g, MSR_FP | MSR_EE);
    g.pc = CODE + 40 * 4;
    step(10);
    CHECK_EQ(g.pc, CODE + 50 * 4);

    cur = "decrementer held off by EE, software 0->1 transition";
    reset_cpu();
    g.gpr[3] = 0x80000005u;  // writing a negative value over a non-negative DEC signals an exception
    g.gpr[4] = 1000;
    emit(MTSPR(SPR_DEC, 4));
    emit(MTSPR(SPR_DEC, 3));
    emit(NOP);
    emit(NOP);
    step(3);
    CHECK_EQ(g.pc, CODE + 12);  // EE = 0: pending only
    gekko_set_msr(&g, MSR_FP | MSR_EE);
    step(1);
    CHECK_EQ(g.spr[SPR_SRR0], CODE + 12);
    CHECK_EQ(g.pc, 0x904);

    cur = "run slice length and gekko_stop";
    reset_cpu();
    for (int k = 0; k < 300; k++) emit(NOP);
    u32 n = gekko_run(&g, 123);
    CHECK_EQ(n, 123);
    CHECK_EQ(g.pc, CODE + 123 * 4);
    CHECK_EQ(g.cycles, 123);
}

// ---- lwarx / stwcx. ([PEM] 8 lwarx, stwcx.) ----
static void t_reservation(void) {
    cur = "lwarx/stwcx.";
    reset_cpu();
    u32 a = 0x00200000u;
    tb_w32(&tb, a, 5);
    g.gpr[3] = a;
    g.gpr[4] = 77;
    g.xer = XER_SO;
    emit(X(31, 5, 0, 3, 20, 0));   // lwarx r5,0,r3
    emit(X(31, 4, 0, 3, 150, 1));  // stwcx. r4,0,r3 -> success
    emit(X(31, 4, 0, 3, 150, 1));  // again: reservation gone -> fail
    step(2);
    CHECK_EQ(g.gpr[5], 5);
    CHECK_EQ(tb_r32(&tb, a), 77);
    CHECK_EQ(g.cr >> 28, 0x3);  // EQ | SO
    tb_w32(&tb, a, 1);
    step(1);
    CHECK_EQ(tb_r32(&tb, a), 1);
    CHECK_EQ(g.cr >> 28, 0x1);
    reset_cpu();
    g.gpr[3] = a;
    g.gpr[6] = a + 4;
    emit(X(31, 5, 0, 3, 20, 0));   // lwarx a
    emit(X(31, 4, 0, 6, 150, 1));  // stwcx. a+4 -> fails (different address), reservation cleared
    emit(X(31, 4, 0, 3, 150, 1));  // stwcx. a -> fails
    step(3);
    CHECK_EQ(g.cr >> 28, 0x0);
    CHECK_EQ(tb_r32(&tb, a + 4), 0);
}

// ---- String instructions ([PEM] lswi/lswx/stswi/stswx) ----
static void t_strings(void) {
    cur = "lswi/stswi";
    reset_cpu();
    u32 a = 0x00200000u;
    for (u32 k = 0; k < 32; k++) tb.ram[a + k] = (u8)(0x10 + k);
    g.gpr[10] = a;
    g.gpr[31] = 0x99999999u;
    emit(X(31, 29, 10, 7, 597, 0));  // lswi r29,r10,7 -> r29 = 10111213, r30 = 14151600
    emit(X(31, 30, 10, 0, 597, 0));  // lswi r30,r10,32 (NB = 0) -> r30, r31, r0 .. r5 (wraps)
    step(2);
    CHECK_EQ(g.gpr[29], 0x10111213u);
    CHECK_EQ(g.gpr[30], 0x10111213u);
    CHECK_EQ(g.gpr[31], 0x14151617u);
    CHECK_EQ(g.gpr[0], 0x18191A1Bu);
    CHECK_EQ(g.gpr[5], 0x2C2D2E2Fu);
    reset_cpu();
    g.gpr[3] = a;
    g.gpr[10] = 0xA1A2A3A4u;
    g.gpr[11] = 0xB1B2B3B4u;
    emit(X(31, 10, 3, 6, 725, 0));  // stswi r10,r3,6
    step(1);
    CHECK_EQ(tb_r32(&tb, a), 0xA1A2A3A4u);
    CHECK_EQ(tb_r32(&tb, a + 4), 0xB1B20000u);

    cur = "lswx/stswx";
    reset_cpu();
    for (u32 k = 0; k < 8; k++) tb.ram[a + k] = (u8)(0x40 + k);
    g.gpr[3] = a;
    g.gpr[4] = 0;
    g.gpr[8] = 0x55555555u;
    g.gpr[9] = 0x66666666u;
    g.xer = 5;
    emit(X(31, 8, 3, 4, 533, 0));  // lswx r8,r3,r4 with XER count 5
    step(1);
    CHECK_EQ(g.gpr[8], 0x40414243u);
    CHECK_EQ(g.gpr[9], 0x44000000u);
    g.xer = 0;
    g.gpr[8] = 1;
    emit(X(31, 8, 3, 4, 533, 0));  // count 0: no registers change
    step(1);
    CHECK_EQ(g.gpr[8], 1);
    g.xer = 3;
    g.gpr[12] = 0x01020304u;
    g.gpr[13] = a + 0x40;
    emit(X(31, 12, 13, 4, 661, 0));  // stswx r12,r13,r4 (3 bytes)
    step(1);
    CHECK_EQ(tb_r32(&tb, a + 0x40), 0x01020300u);
}

// ---- FPU specifics ----
static void t_fpu(void) {
    cur = "single ops write both lanes, double ops ps0 only";
    reset_cpu();
    setF(1, 1.0, 11.0);
    setF(2, 2.0, 12.0);
    setF(3, 0, 33.0);
    setF(4, 0, 44.0);
    setF(5, 0, 55.0);
    setF(6, 0, 66.0);
    emit(A(59, 3, 1, 2, 0, 21));  // fadds f3 -> both lanes 3.0
    emit(A(63, 4, 1, 2, 0, 21));  // fadd f4 -> ps1 untouched
    emit(X(63, 5, 0, 2, 12, 0));  // frsp f5 -> both lanes
    emit(X(63, 6, 0, 2, 72, 0));  // fmr f6 -> ps0 only
    step(4);
    CHECK(u2d(F0(3)) == 3.0 && u2d(F1(3)) == 3.0, "fadds lanes");
    CHECK(u2d(F0(4)) == 3.0 && u2d(F1(4)) == 44.0, "fadd lanes");
    CHECK(u2d(F0(5)) == 2.0 && u2d(F1(5)) == 2.0, "frsp lanes");
    CHECK(u2d(F0(6)) == 2.0 && u2d(F1(6)) == 66.0, "fmr lanes");

    cur = "lfs/lfd/stfs bit exactness";
    reset_cpu();
    u32 a = 0x00200000u;
    tb_w32(&tb, a, 0x7F800001u);           // single SNaN
    tb_w32(&tb, a + 4, 0x00000001u);       // smallest single denormal
    tb_w64(&tb, a + 8, 0x3810000000000000ull);  // 2^-126 as double -> stfs gives 0x00800000
    tb_w64(&tb, a + 16, 0x3690000000000000ull); // 2^-150: below single denormal range (PEM: undefined)
    setF(7, 5.0, 5.0);
    g.gpr[3] = a;
    emit(D(48, 1, 3, 0));       // lfs f1
    emit(D(48, 2, 3, 4));       // lfs f2
    emit(D(50, 7, 3, 8));       // lfd f7 -> ps0 only
    emit(D(52, 1, 3, 0x20));    // stfs f1
    emit(D(52, 2, 3, 0x24));    // stfs f2
    emit(D(52, 7, 3, 0x28));    // stfs f7
    emit(D(50, 8, 3, 16));      // lfd f8
    emit(D(52, 8, 3, 0x2C));    // stfs f8
    step(8);
    CHECK_EQ(F0(1), 0x7FF0000020000000ull);
    CHECK_EQ(F1(1), 0x7FF0000020000000ull);
    CHECK_EQ(F0(2), 0x36A0000000000000ull);
    CHECK_EQ(F1(7), d2u(5.0));
    CHECK_EQ(tb_r32(&tb, a + 0x20), 0x7F800001u);
    CHECK_EQ(tb_r32(&tb, a + 0x24), 0x00000001u);
    CHECK_EQ(tb_r32(&tb, a + 0x28), 0x00800000u);
    CHECK_EQ(tb_r32(&tb, a + 0x2C), 0x34800000u);  // bit extraction: FRS[0:1] || FRS[5:34] (hardware behaviour)

    cur = "fctiw upper word and -0 quirk; mffs upper word";
    reset_cpu();
    setF(1, -2.5, 0);
    setF(2, -0.25, 0);
    emit(X(63, 3, 0, 1, 14, 0));   // fctiw f3,f1 (RN = nearest-even) -> -2
    emit(X(63, 4, 0, 2, 15, 0));   // fctiwz f4,f2 -> 0 from a negative operand
    emit(X(63, 5, 0, 0, 583, 0));  // mffs f5
    step(3);
    CHECK_EQ(F0(3), 0xFFF80000FFFFFFFEull);
    CHECK_EQ(F0(4), 0xFFF8000100000000ull);
    CHECK_EQ(F0(5) >> 32, 0xFFF80000u);
    CHECK((g.fpscr & (FPSCR_FI | FPSCR_XX)) == (FPSCR_FI | FPSCR_XX), "inexact conversion sets FI/XX");

    cur = "single multiply rounds frC to 25 bits (Gekko)";
    reset_cpu();
    setF(1, 1.0 + ldexp(1, -23), 0);
    setF(2, 1.0 + ldexp(1, -25), 0);
    emit(A(59, 3, 1, 0, 2, 25));     // fmuls f3,f1,f2
    emit(A(63, 4, 1, 0, 2, 25));     // fmul f4,f1,f2 (double: exact operand)
    step(2);
    // frC -> 1 + 2^-24; product 1 + 2^-23 + 2^-24 + 2^-47 rounds up to 1 + 2^-22. Without the quirk: 1 + 2^-23.
    CHECK(u2d(F0(3)) == 1.0 + ldexp(1, -22), "fmuls %.17g", u2d(F0(3)));
    CHECK(u2d(F0(4)) == (1.0 + ldexp(1, -23)) * (1.0 + ldexp(1, -25)), "fmul exact");
    gekko_set_compat(&g, GK_COMPAT_EXACT_SINGLE_MUL);
    g.pc = CODE;
    step(1);
    CHECK(u2d(F0(3)) == 1.0 + ldexp(1, -23), "fmuls without the quirk");
    gekko_set_compat(&g, 0);

    cur = "fused multiply-add rounds once";
    reset_cpu();
    // a*c + b with a = c = 1 + 2^-27, b = -1: exact = 2^-26 + 2^-54; a non-fused version loses the 2^-54 term.
    setF(1, 1.0 + ldexp(1, -27), 0);
    setF(2, -1.0, 0);
    emit(A(63, 3, 1, 2, 1, 29));  // fmadd f3,f1,f1,f2
    step(1);
    CHECK(u2d(F0(3)) == ldexp(1, -26) + ldexp(1, -54), "fmadd fused %.17g", u2d(F0(3)));

    cur = "rounding modes";
    reset_cpu();
    setF(1, 1.0, 0);
    setF(2, ldexp(1, -60), 0);
    for (u32 rn = 0; rn < 4; rn++) {
        g.fpscr = rn;
        g.pc = CODE;
        tb_w32(&tb, CODE, A(63, 3, 1, 2, 0, 21));  // fadd 1 + 2^-60
        step(1);
        double want = rn == 2 ? 1.0 + ldexp(1, -52) : 1.0;
        CHECK(u2d(F0(3)) == want, "RN=%u fadd -> %.17g", rn, u2d(F0(3)));
        CHECK(((g.fpscr & FPSCR_FR) != 0) == (rn == 2), "RN=%u FR", rn);
    }

    cur = "NI flushes tiny results";
    reset_cpu();
    g.fpscr = FPSCR_NI;
    setF(1, ldexp(1, -1000), 0);
    setF(2, ldexp(1, -40), 0);
    emit(A(63, 3, 1, 0, 2, 25));  // fmul -> 2^-1040 (denormal double) -> +0 with NI
    emit(A(59, 4, 2, 0, 2, 25));  // fmuls -> 2^-80 fine
    emit(A(59, 5, 1, 0, 1, 25));  // fmuls -> tiny -> 0
    step(3);
    CHECK_EQ(F0(3), 0);
    CHECK(u2d(F0(4)) == ldexp(1, -80), "fmuls normal under NI");
    CHECK_EQ(F0(5), 0);
    g.fpscr = 0;
    g.pc = CODE;
    step(1);
    CHECK(u2d(F0(3)) == ldexp(1, -1040), "without NI the denormal is delivered");
    CHECK((g.fpscr & FPSCR_UX) == 0, "exact denormal: no UX");

    cur = "enabled invalid operation leaves the target";
    reset_cpu();
    g.fpscr = FPSCR_VE;
    setF(1, INFINITY, 0);
    setF(2, INFINITY, 0);
    setF(3, 123.0, 0);
    emit(A(63, 3, 1, 2, 0, 20));  // fsub inf - inf
    step(1);
    CHECK(u2d(F0(3)) == 123.0, "target unchanged");
    CHECK((g.fpscr & (FPSCR_VXISI | FPSCR_FEX | FPSCR_VX)) == (FPSCR_VXISI | FPSCR_FEX | FPSCR_VX), "VXISI/FEX %08x", g.fpscr);

    cur = "fres/frsqrte accuracy and specials";
    reset_cpu();
    int bad = 0;
    for (int k = 0; k < 2000; k++) {
        double x = ldexp(1.0 + k / 2000.0, (k % 40) - 20) * ((k & 1) ? -1 : 1);
        setF(1, x, 0);
        g.pc = CODE;
        tb_w32(&tb, CODE, X(59, 2, 0, 1, 24, 0));  // fres
        tb_w32(&tb, CODE + 4, X(63, 3, 0, 1, 26, 0));  // frsqrte
        step(2);
        if (fabs(u2d(F0(2)) * x - 1.0) > 1.0 / 4096) bad++;
        if (x > 0 && fabs(u2d(F0(3)) * sqrt(x) - 1.0) > 1.0 / 32) bad++;
    }
    CHECK(bad == 0, "%d estimates outside the 750CL accuracy bounds", bad);
    setF(1, -0.0, 0);
    g.pc = CODE;
    g.fpscr = 0;
    step(2);
    CHECK_EQ(F0(2), 0xFFF0000000000000ull);  // fres(-0) = -inf, ZX
    CHECK_EQ(F0(3), 0xFFF0000000000000ull);  // frsqrte(-0) = -inf, ZX
    CHECK(g.fpscr & FPSCR_ZX, "ZX");

    cur = "FPSCR instructions";
    reset_cpu();
    g.fpscr = FPSCR_OX | FPSCR_FX;
    emit(X(63, 4 << 2, 0, 0, 64, 0));      // mcrfs cr4,cr0 -> CR4 = FX FEX VX OX; clears FX and OX
    emit(X(63, 3, 0, 0, 38, 0));           // mtfsb1 3 (OX) -> sets OX and FX
    emit(X(63, 1, 0, 0, 38, 0));           // mtfsb1 1 (FEX) -> ignored
    emit(X(63, 7 << 2, 0, 3 << 1, 134, 0));  // mtfsfi 7,3 -> RN = 3
    step(4);
    CHECK_EQ((g.cr >> 12) & 0xF, 0x9);
    CHECK_EQ(g.fpscr, FPSCR_FX | FPSCR_OX | 3);
}

// ---- SPR side effects and cache operations ----
static void t_spr_cache(void) {
    cur = "locked cache DMA";
    reset_cpu();
    gekko_set_spr(&g, SPR_HID2, HID2_LSQE | HID2_PSE | HID2_LCE);
    g.gpr[3] = 0x00340000u | 0x02;   // DMA_U: MEM_ADDR 0x00340000, LEN_U = 2
    g.gpr[4] = 0xE0000000u | 0x10 | (1 << 2) | 2;  // DMA_L: LC_ADDR, LD = 1 (load), LEN_L = 1, T = 1
    emit(MTSPR(SPR_DMAU, 3));
    emit(MTSPR(SPR_DMAL, 4));
    emit(MFSPR(5, SPR_DMAL));
    step(3);
    CHECK_EQ(tb.dmaCount, 1);
    CHECK(tb.dmaToRam == false && tb.dmaRam == 0x00340000u && tb.dmaCache == 0xE0000000u && tb.dmaLen == 9 * 32,
          "dma toRam=%d ram=%08x cache=%08x len=%u", tb.dmaToRam, tb.dmaRam, tb.dmaCache, tb.dmaLen);
    CHECK_EQ(g.gpr[5] & 3, 0);  // trigger bit cleared on completion
    CHECK_EQ(gekko_get_spr(&g, SPR_HID2) & HID2_DMAQL, 0);

    cur = "dcbz / dcbz_l / dcbf / icbi";
    reset_cpu();
    u32 a = 0x00200040u;
    for (u32 k = 0; k < 96; k++) tb.ram[a - 32 + k] = 0xEE;
    g.gpr[3] = a + 5;
    emit(X(31, 0, 0, 3, 1014, 0));  // dcbz 0,r3 -> zero the aligned 32-byte line
    emit(X(31, 0, 0, 3, 86, 0));    // dcbf
    emit(X(31, 0, 0, 3, 982, 0));   // icbi
    step(3);
    CHECK(tb.ram[a - 1] == 0xEE && tb.ram[a] == 0 && tb.ram[a + 31] == 0 && tb.ram[a + 32] == 0xEE, "dcbz line");
    CHECK(tb.cacheOps[CACHE_DCBF] == 1 && tb.cacheOps[CACHE_ICBI] == 1 && tb.lastCacheOpPa == a, "cache ops pa %08x",
          tb.lastCacheOpPa);
    reset_cpu();
    gekko_set_spr(&g, SPR_HID2, HID2_PSE | HID2_LCE);
    memset(tb.lockedCache, 0xEE, 64);
    g.gpr[3] = 0xE0000020u;
    emit(X(4, 0, 0, 3, 1014, 0));  // dcbz_l 0,r3
    step(1);
    CHECK(tb.lockedCache[0x1F] == 0xEE && tb.lockedCache[0x20] == 0 && tb.lockedCache[0x3F] == 0, "dcbz_l");

    cur = "XER / SPR backing store";
    reset_cpu();
    g.gpr[3] = 0xFFFFFFFFu;
    emit(MTSPR(SPR_XER, 3));
    emit(MFSPR(4, SPR_XER));
    emit(MTSPR(SPR_GQR0 + 7, 3));
    emit(MFSPR(5, SPR_GQR0 + 7));
    emit(MTSPR(SPR_PVR, 3));  // read-only
    emit(MFSPR(6, SPR_PVR));
    step(6);
    CHECK_EQ(g.gpr[4], XER_MASK);
    CHECK_EQ(g.gpr[5], 0xFFFFFFFFu);
    CHECK_EQ(g.gpr[6], 0x00083214u);
}

// ---- Disassembler spot checks ----
static void t_disasm(void) {
    cur = "disassembler";
    struct { u32 pc, inst; const char* want; } v[] = {
        {0, 0x7C642A14u, "add     r3,r4,r5"},
        {0, 0x7C642E15u, "addo.   r3,r4,r5"},
        {0x100, 0x48000010u, "b       0x110"},
        {0x100, 0x4182FFF0u, "bc      12,eq,0xf0"},
        {0, 0x4E800020u, "bclr    20,lt"},
        {0, 0x5463103Au, "rlwinm  r3,r3,2,0,29"},
        {0, 0xFC22182Au, "fadd    f1,f2,f3"},
        {0, 0x102220FAu, "ps_madd f1,f2,f3,f4"},
        {0, 0xE0232008u, "psq_l   f1,8(r3),0,2"},
        {0, 0x7C7A02A6u, "mfspr   r3,26"},
        {0, 0x80640008u, "lwz     r3,8(r4)"},
        {0, 0x00000000u, ".long 0x00000000"},
    };
    for (size_t k = 0; k < sizeof v / sizeof v[0]; k++) {
        char buf[96];
        gekko_disasm(v[k].pc, v[k].inst, buf, sizeof buf);
        CHECK(!strcmp(buf, v[k].want), "%08x -> '%s', want '%s'", v[k].inst, buf, v[k].want);
    }
}

int main(void) {
    tb_init(&tb, &g);
    if (!gekko_init(&g, &tb.bus)) return 2;
    t_paired();
    t_quant();
    t_exceptions();
    t_bats();
    t_timing();
    t_reservation();
    t_strings();
    t_fpu();
    t_spr_cache();
    t_disasm();
    printf("DIRECTED pass=%d fail=%d\n", npass, nfail);
    gekko_free(&g);
    tb_free(&tb);
    return nfail ? 1 : 0;
}
