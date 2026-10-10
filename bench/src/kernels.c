#include "kernels.h"
#include "arm_emit.h"

int kern_linked(u32* buf, u32 capWords, u32 K, u32 slotWords) {
    Emit e = { buf, 0, capWords, 0 };
    const u32 base = 4;
    mov_reg(&e, 12, 0);
    dp_imm(&e, MOV, 0, 2, 0, 0);
    branch_to(&e, 0xE, base);
    for (u32 i = 0; i < K; i++) {
        while (e.n < base + i * slotWords) E(&e, 0xE1A00000u);  // mov r0, r0 (padding, never executed)
        for (int k = 0; k < 4; k++) dp_imm(&e, ADD, 0, 2, 2, 1);
        dp_imm(&e, SUB, 1, 12, 12, 1);
        branch_to(&e, COND_NE, base + kern_next(i, K) * slotWords);
        mov_reg(&e, 0, 2);
        bx_lr(&e);
    }
    return e.overflow ? -1 : (int)e.n;
}

int kern_dispatch(u32* buf, u32 capWords, u32 K, u32 slotWords, u32* blockWord) {
    Emit e = { buf, 0, capWords, 0 };
    for (u32 i = 0; i < K; i++) {
        while (e.n < i * slotWords) E(&e, 0xE1A00000u);
        blockWord[i] = e.n;
        for (int k = 0; k < 4; k++) dp_imm(&e, ADD, 0, 2, 2, 1);
        load_const(&e, 0, kern_block_pc(kern_next(i, K)));
        bx_lr(&e);
    }
    return e.overflow ? -1 : (int)e.n;
}

u32 kern_run_dispatch(const KernEntry* tab, u32 K, u32 steps) {
    u32 pc = kern_block_pc(0);
    for (u32 i = 0; i < steps; i++) {
        const KernEntry* x = &tab[(pc >> 4) & (K - 1)];
        if (x->pc != pc) return 0xDEAD;
        pc = x->fn();
    }
    return pc;
}

const char* const kern_mem_names[KMEM_COUNT] = { "fastmem", "range-check", "C call" };

static u8* g_kernRam;
void kern_set_ram(u8* ram) { g_kernRam = ram; }

// The slow path a JIT falls back to for memory-mapped hardware: a call into C per access.
static u32 kern_read32(u32 addr) {
    const u8* p = g_kernRam + (addr & 0x01FFFFFFu);
    return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3];
}

int kern_mem(u32* buf, u32 capWords, int variant) {
    Emit e = { buf, 0, capWords, 0 };
    u32 enc = 0, loop;
    if (variant == KMEM_CALL) {
        E(&e, 0xE92D41F0u);  // push {r4-r8, lr}
        mov_reg(&e, 4, 0);
        load_const(&e, 6, 0x80000000u);
        dp_imm(&e, MOV, 0, 7, 0, 0);
        load_const(&e, 8, (u32)kern_read32);
        loop = e.n;
        mov_reg(&e, 0, 6);
        blx_reg(&e, 8);
        dp_reg(&e, ADD, 0, 7, 7, 0, LSL, 0);
        dp_imm(&e, ADD, 0, 6, 6, 4);
        dp_imm(&e, SUB, 1, 4, 4, 1);
        branch_to(&e, COND_NE, loop);
        mov_reg(&e, 0, 7);
        E(&e, 0xE8BD81F0u);  // pop {r4-r8, pc}
        return e.overflow ? -1 : (int)e.n;
    }
    dp_imm(&e, MOV, 0, 2, 0, 0);
    load_const(&e, 3, 0x80000000u);
    loop = e.n;
    u32 slowFix = 0;
    if (variant == KMEM_FASTMEM) {
        arm_imm(0xFE000000u, &enc);
        dp_imm(&e, BIC, 0, 12, 3, enc);
    } else {
        arm_imm(0x80000000u, &enc);
        dp_imm(&e, SUB, 0, 12, 3, enc);
        arm_imm(0x01800000u, &enc);
        dp_imm(&e, CMP, 1, 0, 12, enc);
        slowFix = e.n;
        E(&e, 0x2A000000u);  // bhs slow (patched below)
    }
    ldr_reg(&e, 12, 1, 12);
    rev(&e, 12, 12);
    dp_reg(&e, ADD, 0, 2, 2, 12, LSL, 0);
    dp_imm(&e, ADD, 0, 3, 3, 4);
    dp_imm(&e, SUB, 1, 0, 0, 1);
    branch_to(&e, COND_NE, loop);
    mov_reg(&e, 0, 2);
    bx_lr(&e);
    if (variant == KMEM_CHECKED) {
        u32 slow = e.n;
        buf[slowFix] |= (u32)((s32)slow - (s32)(slowFix + 2)) & 0xFFFFFF;
        load_const(&e, 0, 0xDEADu);  // never taken in the benchmark; a real JIT would call the MMIO handler here
        bx_lr(&e);
    }
    return e.overflow ? -1 : (int)e.n;
}

const char* const kern_fp_names[KFP_COUNT] = {
    "f32 add", "f32 mul", "f32 mla", "f32 div", "f32 sqrt",
    "f64 add", "f64 mul", "f64 mla", "f64 div",
    "pair add", "pair mul", "pair mla",
    "cvt f32<-s32", "cvt s32<-f32",
    "vmov s<-r", "vmov r<-s", "vmov 2s<-2r", "vmov 2r<-2s", "vmov s<->r", "setend be/le",
};

int kern_fp(u32* buf, u32 capWords, int which, int dependent) {
    Emit e = { buf, 0, capWords, 0 };
    int isVec = which == KFP_VADD2 || which == KFP_VMUL2 || which == KFP_VMLA2;
    int isDbl = which >= KFP_ADD64 && which <= KFP_DIV64;
    E(&e, 0xED2D8A10u);  // vpush {s16-s31}
    vmrs_fpscr(&e, 3);
    if (isVec) {
        u32 enc = 0;
        arm_imm(0x370000u, &enc);
        dp_imm(&e, BIC, 0, 2, 3, enc);
        arm_imm(0x10000u, &enc);
        dp_imm(&e, ORR, 0, 2, 2, enc);
    }
    // Values chosen so every chain stays at 1.0 (or bounded), never denormal: s16-s31 = 1.0f, s0-s7 = 0,
    // vector zero pair s30/s31; doubles d8-d12 = 1.0, d0 = 0.
    load_const(&e, 12, 0x3F800000u);
    for (int s = 16; s < 32; s++) vmov_sr(&e, s, 12);
    dp_imm(&e, MOV, 0, 12, 0, 0);
    for (int s = 0; s < 8; s++) vmov_sr(&e, s, 12);
    vmov_sr(&e, 30, 12);
    vmov_sr(&e, 31, 12);
    if (isDbl) {
        load_const(&e, 1, 0x3FF00000u);
        for (int d = 8; d <= 12; d++) { vmov_sr(&e, 2 * d, 12); vmov_sr(&e, 2 * d + 1, 1); }
    }
    if (isVec) vmsr_fpscr(&e, 2);
    u32 loop = e.n;
    for (int j = 0; j < KERN_FP_UNROLL; j++) {
        int r32 = dependent ? 16 : 16 + (j & 7);
        int r64 = dependent ? 8 : 8 + (j & 3);
        int rv = dependent ? 16 : 16 + 2 * (j & 3);
        switch (which) {
        case KFP_ADD32: vfp3(&e, VADD, r32, r32, 0); break;
        case KFP_MUL32: vfp3(&e, VMUL, r32, r32, 24); break;
        case KFP_MLA32: vfp3(&e, VMLA, r32, 24, 0); break;
        case KFP_DIV32: vfp3(&e, VDIV, r32, r32, 24); break;
        case KFP_SQRT32: vfp2(&e, VSQRT, r32, dependent ? r32 : 24); break;
        case KFP_ADD64: vfp3d(&e, VADD, r64, r64, 0); break;
        case KFP_MUL64: vfp3d(&e, VMUL, r64, r64, 12); break;
        case KFP_MLA64: vfp3d(&e, VMLA, r64, 12, 0); break;
        case KFP_DIV64: vfp3d(&e, VDIV, r64, r64, 12); break;
        case KFP_VADD2: vfp3(&e, VADD, rv, rv, 30); break;
        case KFP_VMUL2: vfp3(&e, VMUL, rv, rv, 28); break;
        case KFP_VMLA2: vfp3(&e, VMLA, rv, 28, 30); break;
        case KFP_CVT_F_I: vfp2(&e, VCVT_F32_S32, r32, dependent ? r32 : 24); break;
        case KFP_CVT_I_F: vfp2(&e, VCVT_S32_F32, r32, 24); break;
        // Transfers: "latency" column = same register every time, "throughput" = rotating registers.
        case KFP_VMOV_SR: vmov_sr(&e, r32, 12); break;
        case KFP_VMOV_RS: vmov_rs(&e, dependent ? 1 : 1 + (j & 1), 16); break;
        case KFP_VMOV_SSRR: vmov_ssrr(&e, rv, 12, 12); break;
        case KFP_VMOV_RRSS: vmov_rrss(&e, 1, 2, rv); break;
        case KFP_VMOV_RT: if (j & 1) vmov_sr(&e, 16, 12); else vmov_rs(&e, 12, 16); break;
        case KFP_SETEND: if (j & 1) setend_le(&e); else setend_be(&e); break;  // unroll is even: ends little-endian
        }
    }
    dp_imm(&e, SUB, 1, 0, 0, 1);
    branch_to(&e, COND_NE, loop);
    vmsr_fpscr(&e, 3);
    E(&e, 0xECBD8A10u);  // vpop {s16-s31}
    bx_lr(&e);
    return e.overflow ? -1 : (int)e.n;
}
