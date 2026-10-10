#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "arm_emit.h"
#include "ppc.h"

// Benchmark JIT: Gekko -> ARMv6K + VFPv2. Covers only the benchmark's instructions, but uses the shapes a real
// recompiler would: guest registers held in host registers for the whole block, RAM base in a register with masked
// "fastmem" addressing, CR0 kept in the ARM flags between a record-form op and the branch that reads it, and GQRs
// specialised at compile time.
//
// Host registers: r0-r3, r12 scratch; r4-r9 = guest r3-r8; r10 = RAM base; r11 = PPCState*.
// VFP: guest f1-f7 = pairs (s16,s17)..(s28,s29) and f8-f11 = (s8,s9)..(s14,s15), ps0 in the even register;
// s30/s31 scratch pair (vector bank); s0/s1 scalar scratch (bank 0, never vectorised); s2-s7 quantisation scales.
//
// JIT_PS_VECTOR runs the block with FPSCR.LEN = 2, so one data-processing op on a register in s8-s31 works on the
// whole pair. Under that mode an op whose destination is in bank 0 (s0-s7) stays scalar, and an op with a vector
// destination and a bank-0 source broadcasts that source to both lanes; the scalar ops below rely on both rules.
// Loads, stores, register transfers and conversions are never vectorised.
//
// RAM_SWAPPED keeps guest RAM as native little-endian words: lwz/stw/lfs/stfs need no byte swap and a float pair is
// one 64-bit VLDR/VSTR, while 8/16-bit accesses XOR the address. Paired accesses that the fast path cannot take
// (unaligned) leave the block through a bail exit that sets pc = PC_BAILED; a real JIT would take a slow path there.

_Static_assert(offsetof(PPCState, ps0) == PPC_OFF_PS0, "ps0 offset");
_Static_assert(offsetof(PPCState, ps1) == PPC_OFF_PS1, "ps1 offset");
_Static_assert(offsetof(PPCState, hostFpscr) == PPC_OFF_HOSTFPSCR, "hostFpscr offset");

#define R12 12
#define RRAM 10
#define RSTATE 11
#define S_SCRATCH_PAIR 30
#define MAX_SCALES 6
#define NFPR 11
#define ASR 2

static int gmap(int r) { return r >= 3 && r <= 8 ? r + 1 : -1; }
static int fmap(int f) {
    if (f >= 1 && f <= 7) return 16 + 2 * (f - 1);
    if (f >= 8 && f <= NFPR) return 8 + 2 * (f - 8);
    return -1;
}

static int gqr_scale(u32 field6) { return field6 >= 32 ? (int)field6 - 64 : (int)field6; }
static u32 pow2_bits(int k) { return (u32)(127 + k) << 23; }
static int qsize(u32 type) { return type == GQR_FLOAT ? 4 : (type == GQR_U16 || type == GQR_S16) ? 2 : 1; }
static int q_signed(u32 type) { return type == GQR_S8 || type == GQR_S16; }

// Effective address (rA|0)+d into r12, folded into the 32 MB RAM window.
static void ea_to_r12(Emit* e, int a, s32 imm) {
    if (a == 0) load_const(e, R12, (u32)imm);
    else add_imm(e, R12, gmap(a), (u32)imm);
    u32 enc = 0;
    arm_imm(~GC_ADDR_MASK, &enc);
    dp_imm(e, BIC, 0, R12, R12, enc);
}

void jit_flags(PPCState* s, u32 cpsr) {
    u32 n = cpsr >> 31, z = (cpsr >> 30) & 1, c = (cpsr >> 29) & 1;
    u32 so = s->xer >> 31;
    u32 gt = !n && !z;
    s->cr = (s->cr & 0x0FFFFFFF) | (n << 31) | (gt << 30) | (z << 29) | (so << 28);
    s->xer = (s->xer & ~(1u << 29)) | (c << 29);
}

typedef struct {
    Emit e;
    int vec, lay, usesFlags, flagsLive;
    u32 bailFix[64];
    int nBail;
    int overflowBail;
} Jit;

// Exit sequence: restore the host FPSCR before any C code, write guest registers back, materialise CR0/CA.
static void emit_exit(Jit* j, int bail) {
    Emit* e = &j->e;
    if (j->lay == RAM_BE_SETEND) setend_le(e);  // before touching host memory again
    if (j->usesFlags) mrs_cpsr(e, 1);  // before anything can disturb the flags
    ldr_imm(e, 0, RSTATE, PPC_OFF_HOSTFPSCR);
    vmsr_fpscr(e, 0);  // C code must never run with LEN != 1
    if (bail) { dp_imm(e, MVN, 0, 0, 0, 0); str_imm(e, 0, RSTATE, offsetof(PPCState, pc)); }
    for (int r = 3; r <= 8; r++) str_imm(e, gmap(r), RSTATE, r * 4);
    for (int f = 1; f <= NFPR; f++) {
        vstr(e, fmap(f), RSTATE, PPC_OFF_PS0 + f * 4);
        vstr(e, fmap(f) + 1, RSTATE, PPC_OFF_PS1 + f * 4);
    }
    if (j->usesFlags) {
        mov_reg(e, 0, RSTATE);
        load_const(e, R12, (u32)jit_flags);
        blx_reg(e, R12);
    }
    E(e, 0xECBD8A10u);  // vpop {s16-s31}
    E(e, 0xE8BD8FF8u);  // pop {r3-r11, pc}
}

// Branch to the shared bail exit unless r12 is word aligned. Clobbers the flags.
static void bail_unless_aligned(Jit* j) {
    tst_imm(&j->e, R12, 3);
    if (j->nBail == 64) { j->overflowBail = 1; return; }
    j->bailFix[j->nBail++] = j->e.n;
    E(&j->e, (COND_NE << 28) | 0x0A000000u);
    j->flagsLive = 0;
}

// Single-precision scalar result: compute into s0 (callers emit it), then give both lanes the value.
static void scalar_result(Jit* j, int sd) {
    if (j->vec) vfp2(&j->e, VMOVF, sd, 0);  // vector destination, bank-0 source: broadcast
    else { vfp2(&j->e, VMOVF, sd, 0); vfp2(&j->e, VMOVF, sd + 1, 0); }
}

// Paired op on both lanes: one vector op, or one op per lane.
static void pair_op(Jit* j, u32 base, int d, int n, int m) {
    vfp3(&j->e, base, d, n, m);
    if (!j->vec) vfp3(&j->e, base, d + 1, n + 1, m + 1);
}

// Load one integer lane of `type` from guest address r12 (+ XOR for the swapped layout) into core register rt.
static void load_int_lane(Jit* j, u32 type, int rt) {
    Emit* e = &j->e;
    int addr = R12;
    if (j->lay == RAM_SWAPPED) { dp_imm(e, EOR, 0, 1, R12, qsize(type) == 2 ? 2 : 3); addr = 1; }
    switch (type) {
    case GQR_U8: ldrb_reg(e, rt, RRAM, addr); break;
    case GQR_S8: ldrsb_reg(e, rt, RRAM, addr); break;
    case GQR_U16: ldrh_reg(e, rt, RRAM, addr); if (j->lay == RAM_BE) rev16(e, rt, rt); break;
    default: if (j->lay == RAM_BE) { ldrh_reg(e, rt, RRAM, addr); revsh(e, rt, rt); } else ldrsh_reg(e, rt, RRAM, addr); break;
    }
}

static void emit_psq_l(Jit* j, const Decoded* x, u32 type, int scaleReg) {
    Emit* e = &j->e;
    int sd = fmap(x->d), lanes = x->sh ? 1 : 2, sz = qsize(type);
    ea_to_r12(e, x->a, x->imm);
    if (type == GQR_FLOAT) {
        if (j->lay != RAM_BE) {
            bail_unless_aligned(j);
            dp_reg(e, ADD, 0, 3, RRAM, R12, LSL, 0);
            if (lanes == 1) vldr(e, sd, 3, 0);
            else if (j->lay == RAM_SWAPPED) vldr_pair(e, sd, 3);
            else vldm2(e, sd, 3);
        } else {
            ldr_reg(e, 2, RRAM, R12); rev(e, 2, 2);
            if (lanes == 2) { dp_imm(e, ADD, 0, R12, R12, 4); ldr_reg(e, 3, RRAM, R12); rev(e, 3, 3); vmov_ssrr(e, sd, 2, 3); }
            else vmov_sr(e, sd, 2);
        }
    } else {
        if (j->lay != RAM_BE && sz == 2 && lanes == 2) {
            // Both halfwords are one native word: (ps0 << 16) | ps1.
            bail_unless_aligned(j);
            ldr_reg(e, 3, RRAM, R12);
            dp_reg(e, MOV, 0, 2, 0, 3, q_signed(type) ? ASR : LSR, 16);
            if (q_signed(type)) sxth(e, 3, 3); else uxth(e, 3, 3);
        } else {
            load_int_lane(j, type, 2);
            if (lanes == 2) { dp_imm(e, ADD, 0, R12, R12, (u32)sz); load_int_lane(j, type, 3); }
        }
        if (lanes == 2) vmov_ssrr(e, sd, 2, 3); else vmov_sr(e, sd, 2);
        u32 cvt = q_signed(type) ? VCVT_F32_S32 : VCVT_F32_U32;
        vfp2(e, cvt, sd, sd);
        if (lanes == 2) vfp2(e, cvt, sd + 1, sd + 1);
        if (scaleReg >= 0) {
            // Vector mode: pair times bank-0 scalar in one op. With W=1 this also scales ps1, which is set next.
            vfp3(e, VMUL, sd, sd, scaleReg);
            if (!j->vec && lanes == 2) vfp3(e, VMUL, sd + 1, sd + 1, scaleReg);
        }
    }
    if (lanes == 1) { load_const(e, 3, 0x3F800000u); vmov_sr(e, sd + 1, 3); }
}

static void emit_psq_st(Jit* j, const Decoded* x, u32 type, int scaleReg) {
    Emit* e = &j->e;
    int ss = fmap(x->d), lanes = x->sh ? 1 : 2, sz = qsize(type);
    ea_to_r12(e, x->a, x->imm);
    if (type == GQR_FLOAT) {
        if (j->lay != RAM_BE) {
            bail_unless_aligned(j);
            dp_reg(e, ADD, 0, 3, RRAM, R12, LSL, 0);
            if (lanes == 1) vstr(e, ss, 3, 0);
            else if (j->lay == RAM_SWAPPED) vstr_pair(e, ss, 3);
            else vstm2(e, ss, 3);
        } else if (lanes == 2) {
            vmov_rrss(e, 2, 3, ss);
            rev(e, 2, 2); rev(e, 3, 3);
            str_reg(e, 2, RRAM, R12);
            dp_imm(e, ADD, 0, R12, R12, 4);
            str_reg(e, 3, RRAM, R12);
        } else {
            vmov_rs(e, 2, ss); rev(e, 2, 2); str_reg(e, 2, RRAM, R12);
        }
        return;
    }
    // Scale and convert in s0/s1: bank-0 destinations keep these scalar in either mode.
    for (int l = 0; l < lanes; l++) {
        if (scaleReg >= 0) vfp3(e, VMUL, l, ss + l, scaleReg);
        else vfp2(e, VMOVF, l, ss + l);
        vfp2(e, VCVT_S32_F32, l, l);
    }
    if (lanes == 2) vmov_rrss(e, 2, 3, 0); else vmov_rs(e, 2, 0);
    for (int l = 0; l < lanes; l++) {
        int r = l ? 3 : 2;
        if (type == GQR_U8 || type == GQR_U16) usat(e, r, sz * 8, r); else ssat(e, r, sz * 8, r);
    }
    if (j->lay != RAM_BE && sz == 2 && lanes == 2) {
        bail_unless_aligned(j);
        pkhbt(e, 3, 3, 2, 16);
        str_reg(e, 3, RRAM, R12);
        return;
    }
    for (int l = 0; l < lanes; l++) {
        int r = l ? 3 : 2, addr = R12;
        if (l) dp_imm(e, ADD, 0, R12, R12, (u32)sz);
        if (j->lay == RAM_SWAPPED) { dp_imm(e, EOR, 0, 1, R12, sz == 2 ? 2 : 3); addr = 1; }
        if (sz == 1) strb_reg(e, r, RRAM, addr);
        else { if (j->lay == RAM_BE) rev16(e, r, r); strh_reg(e, r, RRAM, addr); }
    }
}

#define FAIL(...) do { snprintf(err, errlen, __VA_ARGS__); return -1; } while (0)
#define NEED_G(r) do { if (gmap(r) < 0) FAIL("jit: guest r%d not mapped (op %d)", (r), i); } while (0)
#define NEED_F(r) do { if (fmap(r) < 0) FAIL("jit: guest f%d not mapped (op %d)", (r), i); } while (0)

int jit_compile(const Decoded* p, int n, u32* buf, u32 capWords, int psMode, int layout, const u32* gqr, char* err, int errlen) {
    Jit j;
    memset(&j, 0, sizeof j);
    j.e = (Emit){ buf, 0, capWords, 0 };
    j.vec = psMode == JIT_PS_VECTOR;
    j.lay = layout;
    Emit* e = &j.e;
    u32 label[256];
    struct { u32 at; int target; } fix[64];
    int nfix = 0;
    if (n > 256) FAIL("jit: block too long");

    // Quantisation scales used by this block, kept in s2-s7 for its whole run.
    u32 scaleBits[MAX_SCALES];
    int nScales = 0;
    for (int i = 0; i < n; i++) {
        if (p[i].op != OP_PSQ_L && p[i].op != OP_PSQ_ST) continue;
        u32 gq = gqr[p[i].c];
        int load = p[i].op == OP_PSQ_L;
        u32 type = load ? (gq >> 16) & 7 : gq & 7;
        int sc = gqr_scale(load ? (gq >> 24) & 63 : (gq >> 8) & 63);
        if (type == GQR_FLOAT || sc == 0) continue;
        u32 bits = pow2_bits(load ? -sc : sc);
        int k = 0;
        while (k < nScales && scaleBits[k] != bits) k++;
        if (k == nScales) { if (nScales == MAX_SCALES) FAIL("jit: too many scales"); scaleBits[nScales++] = bits; }
    }

    E(e, 0xE92D4FF8u);  // push {r3-r11, lr}: 40 bytes, with the 64-byte vpush below sp stays 8-byte aligned
    E(e, 0xED2D8A10u);  // vpush {s16-s31}
    mov_reg(e, RSTATE, 0);
    mov_reg(e, RRAM, 1);
    vmrs_fpscr(e, 0);
    str_imm(e, 0, RSTATE, PPC_OFF_HOSTFPSCR);
    if (j.vec) {
        u32 enc = 0;
        arm_imm(0x370000u, &enc);
        dp_imm(e, BIC, 0, 0, 0, enc);  // clear LEN and STRIDE
        arm_imm(0x10000u, &enc);
        dp_imm(e, ORR, 0, 0, 0, enc);  // LEN = 2, stride 1
        vmsr_fpscr(e, 0);
    }
    for (int r = 3; r <= 8; r++) ldr_imm(e, gmap(r), RSTATE, r * 4);
    for (int f = 1; f <= NFPR; f++) {
        vldr(e, fmap(f), RSTATE, PPC_OFF_PS0 + f * 4);
        vldr(e, fmap(f) + 1, RSTATE, PPC_OFF_PS1 + f * 4);
    }
    for (int k = 0; k < nScales; k++) { load_const(e, 3, scaleBits[k]); vmov_sr(e, 2 + k, 3); }
    // From here until the exit sequence the block touches only guest RAM and registers, never host memory.
    if (j.lay == RAM_BE_SETEND) setend_be(e);

    for (int i = 0; i < n; i++) {
        const Decoded* x = &p[i];
        label[i] = e->n;
        switch (x->op) {
        case OP_HALT:
            emit_exit(&j, 0);
            break;
        case OP_ADDI:
        case OP_ADDIS:
            NEED_G(x->d);
            if (x->a == 0) load_const(e, gmap(x->d), (u32)x->imm);
            else { NEED_G(x->a); add_imm(e, gmap(x->d), gmap(x->a), (u32)x->imm); }
            break;
        case OP_ORI: {
            NEED_G(x->d); NEED_G(x->a);
            u32 enc;
            if (arm_imm((u32)x->imm, &enc)) dp_imm(e, ORR, 0, gmap(x->a), gmap(x->d), enc);
            else { load_const(e, R12, (u32)x->imm); dp_reg(e, ORR, 0, gmap(x->a), gmap(x->d), R12, LSL, 0); }
            break;
        }
        case OP_ADD:
            NEED_G(x->d); NEED_G(x->a); NEED_G(x->b);
            dp_reg(e, ADD, 0, gmap(x->d), gmap(x->a), gmap(x->b), LSL, 0);
            break;
        case OP_XOR:
            NEED_G(x->d); NEED_G(x->a); NEED_G(x->b);
            dp_reg(e, EOR, 0, gmap(x->a), gmap(x->d), gmap(x->b), LSL, 0);
            break;
        case OP_RLWINM: {
            NEED_G(x->d); NEED_G(x->a);
            int ra = gmap(x->a), rs = gmap(x->d), sh = x->sh;
            u32 m = x->mask;
            int lowMB = __builtin_clz(m);  // valid when the mask is a contiguous run ending at bit 31
            if (sh == 0 && m == 0xFFFFFFFFu) {
                mov_reg(e, ra, rs);
            } else if (sh == 0 && m == (0xFFFFFFFFu >> lowMB)) {  // clrlwi: two shifts, no constant
                dp_reg(e, MOV, 0, ra, 0, rs, LSL, lowMB);
                dp_reg(e, MOV, 0, ra, 0, ra, LSR, lowMB);
            } else if (sh != 0 && m == (0xFFFFFFFFu << sh)) {  // slwi
                dp_reg(e, MOV, 0, ra, 0, rs, LSL, sh);
            } else {
                if (sh) dp_reg(e, MOV, 0, ra, 0, rs, ROR, 32 - sh);
                else mov_reg(e, ra, rs);
                u32 enc;
                if (arm_imm(m, &enc)) dp_imm(e, AND, 0, ra, ra, enc);
                else if (arm_imm(~m, &enc)) dp_imm(e, BIC, 0, ra, ra, enc);
                else { load_const(e, R12, m); dp_reg(e, AND, 0, ra, ra, R12, LSL, 0); }
            }
            break;
        }
        case OP_LWZ:
            NEED_G(x->d); if (x->a) NEED_G(x->a);
            ea_to_r12(e, x->a, x->imm);
            ldr_reg(e, gmap(x->d), RRAM, R12);
            if (j.lay == RAM_BE) rev(e, gmap(x->d), gmap(x->d));
            break;
        case OP_STW:
            NEED_G(x->d); if (x->a) NEED_G(x->a);
            ea_to_r12(e, x->a, x->imm);
            if (j.lay == RAM_BE) { rev(e, 3, gmap(x->d)); str_reg(e, 3, RRAM, R12); }
            else str_reg(e, gmap(x->d), RRAM, R12);
            break;
        case OP_ADDIC_RC: {
            // ARM C after SUBS #k equals PowerPC CA for adding -k, so both immediate signs map directly.
            NEED_G(x->d); NEED_G(x->a);
            u32 v = (u32)x->imm, enc;
            if (arm_imm(v, &enc)) dp_imm(e, ADD, 1, gmap(x->d), gmap(x->a), enc);
            else if (arm_imm(-v, &enc)) dp_imm(e, SUB, 1, gmap(x->d), gmap(x->a), enc);
            else { load_const(e, R12, v); dp_reg(e, ADD, 1, gmap(x->d), gmap(x->a), R12, LSL, 0); }
            j.usesFlags = j.flagsLive = 1;
            break;
        }
        case OP_BC: {
            if (!j.flagsLive || x->bi != 2) FAIL("jit: branch needs CR0.EQ from a preceding record op (op %d)", i);
            u32 cond;
            if (x->bo == 4) cond = COND_NE;
            else if (x->bo == 12) cond = COND_EQ;
            else FAIL("jit: unsupported BO %d", x->bo);
            if (x->target <= i) {
                branch_to(e, cond, label[x->target]);
            } else {
                if (nfix == 64) FAIL("jit: too many forward branches");
                fix[nfix].at = e->n; fix[nfix].target = x->target; nfix++;
                E(e, (cond << 28) | 0x0A000000u);
            }
            break;
        }
        case OP_LFS:
            NEED_F(x->d); if (x->a) NEED_G(x->a);
            ea_to_r12(e, x->a, x->imm);
            if (j.lay != RAM_BE) {
                dp_reg(e, ADD, 0, 3, RRAM, R12, LSL, 0);
                if (j.vec) { vldr(e, 0, 3, 0); vfp2(e, VMOVF, fmap(x->d), 0); }
                else { vldr(e, fmap(x->d), 3, 0); vfp2(e, VMOVF, fmap(x->d) + 1, fmap(x->d)); }
            } else {
                ldr_reg(e, 3, RRAM, R12);
                rev(e, 3, 3);
                vmov_ssrr(e, fmap(x->d), 3, 3);
            }
            break;
        case OP_STFS:
            NEED_F(x->d); if (x->a) NEED_G(x->a);
            ea_to_r12(e, x->a, x->imm);
            if (j.lay != RAM_BE) {
                dp_reg(e, ADD, 0, 3, RRAM, R12, LSL, 0);
                vstr(e, fmap(x->d), 3, 0);
            } else {
                vmov_rs(e, 3, fmap(x->d));
                rev(e, 3, 3);
                str_reg(e, 3, RRAM, R12);
            }
            break;
        case OP_FADDS:
            NEED_F(x->d); NEED_F(x->a); NEED_F(x->b);
            vfp3(e, VADD, 0, fmap(x->a), fmap(x->b));
            scalar_result(&j, fmap(x->d));
            break;
        case OP_FMULS:
            NEED_F(x->d); NEED_F(x->a); NEED_F(x->c);
            vfp3(e, VMUL, 0, fmap(x->a), fmap(x->c));
            scalar_result(&j, fmap(x->d));
            break;
        case OP_FMADDS:
            // Round after the multiply, as the interpreter does; VFPv2 has no fused multiply-add.
            NEED_F(x->d); NEED_F(x->a); NEED_F(x->b); NEED_F(x->c);
            vfp3(e, VMUL, 0, fmap(x->a), fmap(x->c));
            vfp3(e, VADD, 0, 0, fmap(x->b));
            scalar_result(&j, fmap(x->d));
            break;
        case OP_PS_ADD:
            NEED_F(x->d); NEED_F(x->a); NEED_F(x->b);
            pair_op(&j, VADD, fmap(x->d), fmap(x->a), fmap(x->b));
            break;
        case OP_PS_SUB:
            NEED_F(x->d); NEED_F(x->a); NEED_F(x->b);
            pair_op(&j, VSUB, fmap(x->d), fmap(x->a), fmap(x->b));
            break;
        case OP_PS_MUL:
            NEED_F(x->d); NEED_F(x->a); NEED_F(x->c);
            pair_op(&j, VMUL, fmap(x->d), fmap(x->a), fmap(x->c));
            break;
        case OP_PS_MADD:
            NEED_F(x->d); NEED_F(x->a); NEED_F(x->b); NEED_F(x->c);
            pair_op(&j, VMUL, S_SCRATCH_PAIR, fmap(x->a), fmap(x->c));
            pair_op(&j, VADD, fmap(x->d), S_SCRATCH_PAIR, fmap(x->b));
            break;
        case OP_PSQ_L:
        case OP_PSQ_ST: {
            NEED_F(x->d); if (x->a) NEED_G(x->a);
            int load = x->op == OP_PSQ_L;
            u32 gq = gqr[x->c];
            u32 type = load ? (gq >> 16) & 7 : gq & 7;
            int sc = gqr_scale(load ? (gq >> 24) & 63 : (gq >> 8) & 63);
            int scaleReg = -1;
            if (type != GQR_FLOAT && sc != 0) {
                u32 bits = pow2_bits(load ? -sc : sc);
                int k = 0;
                while (scaleBits[k] != bits) k++;
                scaleReg = 2 + k;
            }
            if (load) emit_psq_l(&j, x, type, scaleReg); else emit_psq_st(&j, x, type, scaleReg);
            break;
        }
        default:
            FAIL("jit: op %d not supported", x->op);
        }
    }
    for (int k = 0; k < nfix; k++) {
        s32 off = (s32)label[fix[k].target] - (s32)(fix[k].at + 2);
        buf[fix[k].at] |= (u32)off & 0xFFFFFF;
    }
    if (j.nBail) {
        u32 bail = e->n;
        emit_exit(&j, 1);
        for (int k = 0; k < j.nBail; k++) buf[j.bailFix[k]] |= (u32)((s32)bail - (s32)(j.bailFix[k] + 2)) & 0xFFFFFF;
    }
    if (j.overflowBail) FAIL("jit: too many bail sites");
    if (e->overflow) FAIL("jit: code buffer full");
    return (int)e->n;
}
