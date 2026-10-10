#pragma once
// ARMv6K + VFPv2 instruction encoder shared by the JIT and the micro-kernels. Condition is always AL unless named.
#include <3ds/types.h>

#define AL 0xE0000000u
#define COND_EQ 0x0u
#define COND_NE 0x1u

enum { AND = 0, EOR = 1, SUB = 2, ADD = 4, CMP = 10, ORR = 12, MOV = 13, BIC = 14, MVN = 15 };
enum { LSL = 0, LSR = 1, ROR = 3 };

typedef struct { u32* buf; u32 n, cap; int overflow; } Emit;

static inline void E(Emit* e, u32 w) {
    if (e->n < e->cap) e->buf[e->n++] = w;
    else e->overflow = 1;
}

// ARM data-processing immediate: an 8-bit value rotated right by an even amount.
static inline int arm_imm(u32 v, u32* enc) {
    for (u32 r = 0; r < 16; r++) {
        u32 x = r ? (v << (2 * r)) | (v >> (32 - 2 * r)) : v;
        if (x <= 0xFF) { *enc = (r << 8) | x; return 1; }
    }
    return 0;
}

static inline void dp_imm(Emit* e, int opc, int s, int rd, int rn, u32 enc) {
    E(e, AL | 0x02000000u | (opc << 21) | (s << 20) | (rn << 16) | (rd << 12) | enc);
}
static inline void dp_reg(Emit* e, int opc, int s, int rd, int rn, int rm, int shtype, int shamt) {
    E(e, AL | (opc << 21) | (s << 20) | (rn << 16) | (rd << 12) | (shamt << 7) | (shtype << 5) | rm);
}
static inline void mov_reg(Emit* e, int rd, int rm) { dp_reg(e, MOV, 0, rd, 0, rm, LSL, 0); }

// ARMv6K has no MOVW/MOVT, so constants are built from rotated bytes.
static inline void load_const(Emit* e, int rd, u32 v) {
    u32 enc;
    if (arm_imm(v, &enc)) { dp_imm(e, MOV, 0, rd, 0, enc); return; }
    if (arm_imm(~v, &enc)) { dp_imm(e, MVN, 0, rd, 0, enc); return; }
    static const u32 rot[4] = { 0, 12, 8, 4 };
    dp_imm(e, MOV, 0, rd, 0, v & 0xFF);
    for (int b = 1; b < 4; b++) {
        u32 byte = (v >> (8 * b)) & 0xFF;
        if (byte) dp_imm(e, ORR, 0, rd, rd, (rot[b] << 8) | byte);
    }
}

// rd = rn + v, using r12 only when the constant cannot be encoded (so rd/rn must not be r12 in that case).
static inline void add_imm(Emit* e, int rd, int rn, u32 v) {
    u32 enc;
    if (v == 0) { if (rd != rn) mov_reg(e, rd, rn); return; }
    if (arm_imm(v, &enc)) { dp_imm(e, ADD, 0, rd, rn, enc); return; }
    if (arm_imm(-v, &enc)) { dp_imm(e, SUB, 0, rd, rn, enc); return; }
    load_const(e, 12, v);
    dp_reg(e, ADD, 0, rd, rn, 12, LSL, 0);
}

static inline void ldr_imm(Emit* e, int rt, int rn, u32 off) { E(e, AL | 0x05900000u | (rn << 16) | (rt << 12) | off); }
static inline void str_imm(Emit* e, int rt, int rn, u32 off) { E(e, AL | 0x05800000u | (rn << 16) | (rt << 12) | off); }
static inline void ldr_reg(Emit* e, int rt, int rn, int rm) { E(e, AL | 0x07900000u | (rn << 16) | (rt << 12) | rm); }
static inline void str_reg(Emit* e, int rt, int rn, int rm) { E(e, AL | 0x07800000u | (rn << 16) | (rt << 12) | rm); }
static inline void ldrb_reg(Emit* e, int rt, int rn, int rm) { E(e, AL | 0x07D00000u | (rn << 16) | (rt << 12) | rm); }
static inline void strb_reg(Emit* e, int rt, int rn, int rm) { E(e, AL | 0x07C00000u | (rn << 16) | (rt << 12) | rm); }
static inline void ldrh_reg(Emit* e, int rt, int rn, int rm) { E(e, AL | 0x019000B0u | (rn << 16) | (rt << 12) | rm); }
static inline void strh_reg(Emit* e, int rt, int rn, int rm) { E(e, AL | 0x018000B0u | (rn << 16) | (rt << 12) | rm); }
static inline void ldrsb_reg(Emit* e, int rt, int rn, int rm) { E(e, AL | 0x019000D0u | (rn << 16) | (rt << 12) | rm); }
static inline void rev(Emit* e, int rd, int rm) { E(e, AL | 0x06BF0F30u | (rd << 12) | rm); }
static inline void rev16(Emit* e, int rd, int rm) { E(e, AL | 0x06BF0FB0u | (rd << 12) | rm); }
static inline void revsh(Emit* e, int rd, int rm) { E(e, AL | 0x06FF0FB0u | (rd << 12) | rm); }
static inline void ssat(Emit* e, int rd, int bits, int rn) { E(e, AL | 0x06A00010u | ((bits - 1) << 16) | (rd << 12) | rn); }
static inline void usat(Emit* e, int rd, int bits, int rn) { E(e, AL | 0x06E00010u | (bits << 16) | (rd << 12) | rn); }
static inline void ldrsh_reg(Emit* e, int rt, int rn, int rm) { E(e, AL | 0x019000F0u | (rn << 16) | (rt << 12) | rm); }
static inline void sxth(Emit* e, int rd, int rm) { E(e, AL | 0x06BF0070u | (rd << 12) | rm); }
static inline void uxth(Emit* e, int rd, int rm) { E(e, AL | 0x06FF0070u | (rd << 12) | rm); }
// rd = (rn & 0xFFFF) | (rm << sh), sh != 0
static inline void pkhbt(Emit* e, int rd, int rn, int rm, int sh) { E(e, AL | 0x06800010u | (rn << 16) | (rd << 12) | (sh << 7) | rm); }
static inline void tst_imm(Emit* e, int rn, u32 enc) { E(e, AL | 0x03100000u | (rn << 16) | enc); }
static inline void setend_be(Emit* e) { E(e, 0xF1010200u); }
static inline void setend_le(Emit* e) { E(e, 0xF1010000u); }
// Two single registers sm, sm+1 from/to consecutive words, each a separate 32-bit access (correct in either data
// endianness, unlike a 64-bit VLDR, which a big-endian CPU treats as one doubleword).
static inline void vldm2(Emit* e, int sm, int rn) { E(e, AL | 0x0C900A02u | ((sm & 1) << 22) | (rn << 16) | ((sm >> 1) << 12)); }
static inline void vstm2(Emit* e, int sm, int rn) { E(e, AL | 0x0C800A02u | ((sm & 1) << 22) | (rn << 16) | ((sm >> 1) << 12)); }
static inline void blx_reg(Emit* e, int rm) { E(e, AL | 0x012FFF30u | rm); }
static inline void bx_lr(Emit* e) { E(e, 0xE12FFF1Eu); }
static inline void mrs_cpsr(Emit* e, int rd) { E(e, AL | 0x010F0000u | (rd << 12)); }
static inline void vmrs_fpscr(Emit* e, int rt) { E(e, AL | 0x0EF10A10u | (rt << 12)); }
static inline void vmsr_fpscr(Emit* e, int rt) { E(e, AL | 0x0EE10A10u | (rt << 12)); }

// Branch to word index `target` in the same buffer (cond in the top nibble).
static inline void branch_to(Emit* e, u32 cond, u32 target) {
    s32 off = (s32)target - (s32)(e->n + 2);
    E(e, (cond << 28) | 0x0A000000u | ((u32)off & 0xFFFFFF));
}

// ---- VFP, single precision registers s0-s31 ----
static inline void vldr(Emit* e, int sd, int rn, u32 off) { E(e, AL | 0x0D900A00u | ((sd & 1) << 22) | (rn << 16) | ((sd >> 1) << 12) | (off / 4)); }
static inline void vstr(Emit* e, int sd, int rn, u32 off) { E(e, AL | 0x0D800A00u | ((sd & 1) << 22) | (rn << 16) | ((sd >> 1) << 12) | (off / 4)); }
static inline void vmov_sr(Emit* e, int sn, int rt) { E(e, AL | 0x0E000A10u | ((sn >> 1) << 16) | (rt << 12) | ((sn & 1) << 7)); }
static inline void vmov_rs(Emit* e, int rt, int sn) { E(e, AL | 0x0E100A10u | ((sn >> 1) << 16) | (rt << 12) | ((sn & 1) << 7)); }

// Two core registers <-> two consecutive single registers (sm, sm+1) in one transfer.
static inline void vmov_ssrr(Emit* e, int sm, int rt, int rt2) { E(e, AL | 0x0C400A10u | (rt2 << 16) | (rt << 12) | ((sm & 1) << 5) | (sm >> 1)); }
static inline void vmov_rrss(Emit* e, int rt, int rt2, int sm) { E(e, AL | 0x0C500A10u | (rt2 << 16) | (rt << 12) | ((sm & 1) << 5) | (sm >> 1)); }
// 64-bit load/store of the pair (s2k, s2k+1) = d k; address in rn, word aligned.
static inline void vldr_pair(Emit* e, int sEven, int rn) { int d = sEven >> 1; E(e, AL | 0x0D900B00u | ((d >> 4) << 22) | (rn << 16) | ((d & 15) << 12)); }
static inline void vstr_pair(Emit* e, int sEven, int rn) { int d = sEven >> 1; E(e, AL | 0x0D800B00u | ((d >> 4) << 22) | (rn << 16) | ((d & 15) << 12)); }

static inline void vfp3(Emit* e, u32 base, int d, int n, int m) {
    E(e, AL | base | ((d & 1) << 22) | ((n >> 1) << 16) | ((d >> 1) << 12) | ((n & 1) << 7) | ((m & 1) << 5) | (m >> 1));
}
static inline void vfp2(Emit* e, u32 base, int d, int m) {
    E(e, AL | base | ((d & 1) << 22) | ((d >> 1) << 12) | ((m & 1) << 5) | (m >> 1));
}
#define VADD 0x0E300A00u
#define VSUB 0x0E300A40u
#define VMUL 0x0E200A00u
#define VMLA 0x0E000A00u
#define VDIV 0x0E800A00u
#define VMOVF 0x0EB00A40u      // vmov.f32 sd, sm (a data-processing op: vectorised under FPSCR.LEN)
#define VSQRT 0x0EB10AC0u
#define VCVT_F32_S32 0x0EB80AC0u
#define VCVT_F32_U32 0x0EB80A40u
#define VCVT_S32_F32 0x0EBD0AC0u  // round towards zero

// ---- VFP, double precision registers d0-d15 (sz bit set) ----
static inline void vfp3d(Emit* e, u32 base, int d, int n, int m) {
    E(e, AL | base | 0x100u | ((d >> 4) << 22) | ((n & 15) << 16) | ((d & 15) << 12) | ((n >> 4) << 7) | ((m >> 4) << 5) | (m & 15));
}
static inline void vfp2d(Emit* e, u32 base, int d, int m) {
    E(e, AL | base | 0x100u | ((d >> 4) << 22) | ((d & 15) << 12) | ((m >> 4) << 5) | (m & 15));
}
