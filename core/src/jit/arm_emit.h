#pragma once
// ARMv6K (+ VFPv2) instruction encoder for the JIT. Every function appends one instruction; `cond` variants take an
// ARM condition code, the rest are unconditional (AL).
#include "../common.h"

enum { C_EQ = 0, C_NE = 1, C_CS = 2, C_CC = 3, C_MI = 4, C_PL = 5, C_VS = 6, C_VC = 7, C_HI = 8, C_LS = 9, C_GE = 10,
       C_LT = 11, C_GT = 12, C_LE = 13, C_AL = 14 };
enum { AND = 0, EOR = 1, SUB = 2, RSB = 3, ADD = 4, ADC = 5, SBC = 6, RSC = 7, TST = 8, TEQ = 9, CMP = 10, CMN = 11,
       ORR = 12, MOV = 13, BIC = 14, MVN = 15 };
enum { LSL = 0, LSR = 1, ASR = 2, ROR = 3 };
enum { R0, R1, R2, R3, R4, R5, R6, R7, R8, R9, R10, R11, R12, SP = 13, LR = 14, PC = 15 };

typedef struct { u32* buf; u32 n, cap; bool overflow; } Emit;

static inline void E(Emit* e, u32 w) {
    if (e->n < e->cap) e->buf[e->n++] = w;
    else e->overflow = true;
}
static inline u32 cc(int cond) { return (u32)cond << 28; }

// Data-processing immediate: an 8-bit value rotated right by an even amount.
static inline bool arm_imm(u32 v, u32* enc) {
    for (u32 r = 0; r < 16; r++) {
        u32 x = r ? (v << (2 * r)) | (v >> (32 - 2 * r)) : v;
        if (x <= 0xFF) { *enc = (r << 8) | x; return true; }
    }
    return false;
}

static inline void dp_imm_c(Emit* e, int cond, int opc, int s, int rd, int rn, u32 enc) {
    E(e, cc(cond) | 0x02000000u | ((u32)opc << 21) | ((u32)s << 20) | ((u32)rn << 16) | ((u32)rd << 12) | enc);
}
static inline void dp_reg_c(Emit* e, int cond, int opc, int s, int rd, int rn, int rm, int sht, int sha) {
    E(e, cc(cond) | ((u32)opc << 21) | ((u32)s << 20) | ((u32)rn << 16) | ((u32)rd << 12) | ((u32)sha << 7) | ((u32)sht << 5) | (u32)rm);
}
// Register-shifted register: rd = rn OP (rm SHIFT rs).
static inline void dp_rsr(Emit* e, int opc, int s, int rd, int rn, int rm, int sht, int rs) {
    E(e, cc(C_AL) | ((u32)opc << 21) | ((u32)s << 20) | ((u32)rn << 16) | ((u32)rd << 12) | ((u32)rs << 8) | ((u32)sht << 5) | 0x10u | (u32)rm);
}
static inline void dp_imm(Emit* e, int opc, int s, int rd, int rn, u32 enc) { dp_imm_c(e, C_AL, opc, s, rd, rn, enc); }
static inline void dp_reg(Emit* e, int opc, int s, int rd, int rn, int rm) { dp_reg_c(e, C_AL, opc, s, rd, rn, rm, LSL, 0); }
static inline void mov_reg(Emit* e, int rd, int rm) { if (rd != rm) dp_reg(e, MOV, 0, rd, 0, rm); }
static inline void mov_shift(Emit* e, int s, int rd, int rm, int sht, int sha) { dp_reg_c(e, C_AL, MOV, s, rd, 0, rm, sht, sha); }

// ARMv6K has no MOVW/MOVT: constants are MOV/MVN of a rotated byte, or MOV + up to three ORRs.
static inline void load_const_c(Emit* e, int cond, int rd, u32 v) {
    u32 enc;
    if (arm_imm(v, &enc)) { dp_imm_c(e, cond, MOV, 0, rd, 0, enc); return; }
    if (arm_imm(~v, &enc)) { dp_imm_c(e, cond, MVN, 0, rd, 0, enc); return; }
    static const u32 rot[4] = { 0, 12, 8, 4 };
    dp_imm_c(e, cond, MOV, 0, rd, 0, v & 0xFF);
    for (int b = 1; b < 4; b++) {
        u32 byte = (v >> (8 * b)) & 0xFF;
        if (byte) dp_imm_c(e, cond, ORR, 0, rd, rd, (rot[b] << 8) | byte);
    }
}
static inline void load_const(Emit* e, int rd, u32 v) { load_const_c(e, C_AL, rd, v); }

// rd = rn (+/-) v, using `tmp` for constants that do not encode (tmp must differ from rd and rn).
static inline void add_const(Emit* e, int rd, int rn, u32 v, int tmp) {
    u32 enc;
    if (v == 0) { mov_reg(e, rd, rn); return; }
    if (arm_imm(v, &enc)) { dp_imm(e, ADD, 0, rd, rn, enc); return; }
    if (arm_imm(0u - v, &enc)) { dp_imm(e, SUB, 0, rd, rn, enc); return; }
    load_const(e, tmp, v);
    dp_reg(e, ADD, 0, rd, rn, tmp);
}

// Loads and stores. Immediate offsets are 0..4095 (positive).
static inline void ldr_imm(Emit* e, int rt, int rn, u32 off) { E(e, cc(C_AL) | 0x05900000u | ((u32)rn << 16) | ((u32)rt << 12) | off); }
static inline void str_imm(Emit* e, int rt, int rn, u32 off) { E(e, cc(C_AL) | 0x05800000u | ((u32)rn << 16) | ((u32)rt << 12) | off); }
static inline void ldr_reg(Emit* e, int rt, int rn, int rm) { E(e, cc(C_AL) | 0x07900000u | ((u32)rn << 16) | ((u32)rt << 12) | (u32)rm); }
static inline void str_reg(Emit* e, int rt, int rn, int rm) { E(e, cc(C_AL) | 0x07800000u | ((u32)rn << 16) | ((u32)rt << 12) | (u32)rm); }
static inline void ldrb_reg(Emit* e, int rt, int rn, int rm) { E(e, cc(C_AL) | 0x07D00000u | ((u32)rn << 16) | ((u32)rt << 12) | (u32)rm); }
static inline void strb_reg(Emit* e, int rt, int rn, int rm) { E(e, cc(C_AL) | 0x07C00000u | ((u32)rn << 16) | ((u32)rt << 12) | (u32)rm); }
static inline void ldrh_reg(Emit* e, int rt, int rn, int rm) { E(e, cc(C_AL) | 0x019000B0u | ((u32)rn << 16) | ((u32)rt << 12) | (u32)rm); }
static inline void strh_reg(Emit* e, int rt, int rn, int rm) { E(e, cc(C_AL) | 0x018000B0u | ((u32)rn << 16) | ((u32)rt << 12) | (u32)rm); }
static inline void rev(Emit* e, int rd, int rm) { E(e, cc(C_AL) | 0x06BF0F30u | ((u32)rd << 12) | (u32)rm); }
static inline void rev16(Emit* e, int rd, int rm) { E(e, cc(C_AL) | 0x06BF0FB0u | ((u32)rd << 12) | (u32)rm); }
static inline void revsh(Emit* e, int rd, int rm) { E(e, cc(C_AL) | 0x06FF0FB0u | ((u32)rd << 12) | (u32)rm); }
static inline void sxtb(Emit* e, int rd, int rm) { E(e, cc(C_AL) | 0x06AF0070u | ((u32)rd << 12) | (u32)rm); }
static inline void sxth(Emit* e, int rd, int rm) { E(e, cc(C_AL) | 0x06BF0070u | ((u32)rd << 12) | (u32)rm); }
static inline void uxth(Emit* e, int rd, int rm) { E(e, cc(C_AL) | 0x06FF0070u | ((u32)rd << 12) | (u32)rm); }
static inline void clz(Emit* e, int rd, int rm) { E(e, cc(C_AL) | 0x016F0F10u | ((u32)rd << 12) | (u32)rm); }
static inline void mul(Emit* e, int rd, int rm, int rs) { E(e, cc(C_AL) | 0x00000090u | ((u32)rd << 16) | ((u32)rs << 8) | (u32)rm); }
// {hi, lo} = rm * rs, signed / unsigned.
static inline void smull(Emit* e, int lo, int hi, int rm, int rs) { E(e, cc(C_AL) | 0x00C00090u | ((u32)hi << 16) | ((u32)lo << 12) | ((u32)rs << 8) | (u32)rm); }
static inline void umull(Emit* e, int lo, int hi, int rm, int rs) { E(e, cc(C_AL) | 0x00800090u | ((u32)hi << 16) | ((u32)lo << 12) | ((u32)rs << 8) | (u32)rm); }
static inline void mrs(Emit* e, int rd) { E(e, cc(C_AL) | 0x010F0000u | ((u32)rd << 12)); }
static inline void blx_reg(Emit* e, int rm) { E(e, cc(C_AL) | 0x012FFF30u | (u32)rm); }
static inline void ldmia(Emit* e, int rn, u32 mask) { E(e, cc(C_AL) | 0x08900000u | ((u32)rn << 16) | mask); }
static inline void stmia(Emit* e, int rn, u32 mask) { E(e, cc(C_AL) | 0x08800000u | ((u32)rn << 16) | mask); }
static inline void push_regs(Emit* e, u32 mask) { E(e, cc(C_AL) | 0x092D0000u | mask); }
static inline void pop_regs(Emit* e, u32 mask) { E(e, cc(C_AL) | 0x08BD0000u | mask); }

// Branch to word index `target` of the same buffer; returns the index of the branch (for later patching).
static inline u32 branch_c(Emit* e, int cond, u32 target) {
    u32 at = e->n;
    s32 off = (s32)target - (s32)(at + 2);
    E(e, cc(cond) | 0x0A000000u | ((u32)off & 0xFFFFFF));
    return at;
}
static inline void patch_branch(Emit* e, u32 at, u32 target) {
    s32 off = (s32)target - (s32)(at + 2);
    e->buf[at] = (e->buf[at] & 0xFF000000u) | ((u32)off & 0xFFFFFF);
}

// ---- VFPv2. Sn: single registers 0..31; Dn: double registers 0..15 (d0-d7 = s0-s15 are caller-saved). ----
// Multiply-accumulate group: Vd = (+/-)Vd (+/-) Vn * Vm; the product is rounded before the add (VFP11 has no fused op).
enum { VF_MLA = 0x00000000u, VF_MLS = 0x00000040u, VF_NMLS = 0x00100000u, VF_NMLA = 0x00100040u, VF_MUL = 0x00200000u,
       VF_ADD = 0x00300000u, VF_SUB = 0x00300040u, VF_DIV = 0x00800000u };
static inline u32 vf_sd(int s) { return ((u32)(s >> 1) << 12) | ((u32)(s & 1) << 22); }
static inline u32 vf_sn(int s) { return ((u32)(s >> 1) << 16) | ((u32)(s & 1) << 7); }
static inline u32 vf_sm(int s) { return (u32)(s >> 1) | ((u32)(s & 1) << 5); }
static inline void vfp_d(Emit* e, u32 op, int d, int n, int m) { E(e, 0xEE000B00u | op | ((u32)n << 16) | ((u32)d << 12) | (u32)m); }
static inline void vfp_s(Emit* e, u32 op, int d, int n, int m) { E(e, 0xEE000A00u | op | vf_sd(d) | vf_sn(n) | vf_sm(m)); }
static inline void vmov_d(Emit* e, int d, int m) { E(e, 0xEEB00B40u | ((u32)d << 12) | (u32)m); }
static inline void vneg_d(Emit* e, int d, int m) { E(e, 0xEEB10B40u | ((u32)d << 12) | (u32)m); }
static inline void vcvt_f32_f64(Emit* e, int sd, int dm) { E(e, 0xEEB70BC0u | vf_sd(sd) | (u32)dm); }
static inline void vcvt_f64_f32(Emit* e, int dd, int sm) { E(e, 0xEEB70AC0u | ((u32)dd << 12) | vf_sm(sm)); }
// double -> int32: vcvt truncates toward zero, vcvtr rounds as FPSCR says.
static inline void vcvt_s32_f64(Emit* e, int sd, int dm) { E(e, 0xEEBD0BC0u | vf_sd(sd) | (u32)dm); }
static inline void vcvtr_s32_f64(Emit* e, int sd, int dm) { E(e, 0xEEBD0B40u | vf_sd(sd) | (u32)dm); }
static inline void vcmp_d(Emit* e, int d, int m) { E(e, 0xEEB40B40u | ((u32)d << 12) | (u32)m); }
static inline void vmrs_apsr(Emit* e) { E(e, 0xEEF1FA10u); }  // vmrs APSR_nzcv, fpscr
static inline void vmsr_fpscr(Emit* e, int rt) { E(e, 0xEEE10A10u | ((u32)rt << 12)); }
// Loads and stores with a word-multiple offset 0..1020.
static inline void vldr_d(Emit* e, int d, int rn, u32 off) { E(e, 0xED900B00u | ((u32)rn << 16) | ((u32)d << 12) | (off >> 2)); }
static inline void vstr_d(Emit* e, int d, int rn, u32 off) { E(e, 0xED800B00u | ((u32)rn << 16) | ((u32)d << 12) | (off >> 2)); }
// Moves between core and VFP registers.
static inline void vmov_s_r(Emit* e, int sn, int rt) { E(e, 0xEE000A10u | vf_sn(sn) | ((u32)rt << 12)); }
static inline void vmov_r_s(Emit* e, int rt, int sn) { E(e, 0xEE100A10u | vf_sn(sn) | ((u32)rt << 12)); }
static inline void vmov_d_rr(Emit* e, int dm, int rt, int rt2) { E(e, 0xEC400B10u | ((u32)rt2 << 16) | ((u32)rt << 12) | (u32)dm); }
static inline void vmov_ss_rr(Emit* e, int sm, int rt, int rt2) { E(e, 0xEC400A10u | ((u32)rt2 << 16) | ((u32)rt << 12) | vf_sm(sm)); }  // sm, sm+1 = rt, rt2
