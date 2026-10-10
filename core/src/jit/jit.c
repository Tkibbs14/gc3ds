// Gekko -> ARMv6K JIT, level 1 (see jit.h). Compiled only for 32-bit ARM hosts.
#if defined(__arm__)
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include "../cpu/gekko_internal.h"
#include "arm_emit.h"
#include "jit.h"

#define MAX_BLOCK_INSNS 32
#define DISPATCH_BITS 16
#define DISPATCH_SIZE (1u << DISPATCH_BITS)
#define BLOCK_MAX_WORDS 4096  // worst-case host words for one block (checked against remaining space)

// Host register roles inside a block.
#define RG R11   // Gekko*
#define RRAM R10 // host pointer to physical RAM 0
#define RSTOP R5 // set to 1 by a slow memory access that requires leaving compiled code after the instruction

// Offsets into Gekko (ldr/str immediate range is 0..4095; everything used here is below that).
#define OFF_GPR(r) ((u32)offsetof(Gekko, gpr) + 4u * (u32)(r))
#define OFF_CR ((u32)offsetof(Gekko, cr))
#define OFF_XER ((u32)offsetof(Gekko, xer))
#define OFF_LR ((u32)offsetof(Gekko, lr))
#define OFF_CTR ((u32)offsetof(Gekko, ctr))
#define OFF_PC ((u32)offsetof(Gekko, pc))
_Static_assert(offsetof(Gekko, pc) < 4096, "Gekko.pc reachable with ldr immediate");

typedef u32 (*BlockFn)(Gekko* g);
typedef struct { u32 pc; u32 msrBits; BlockFn fn; bool idle; } Entry;

struct Jit {
    Gekko* g;
    u8* ram;
    u32 ramSize;
    u32* code;
    u32 codeWords, codeUsed;
    Entry* table;
    bool fastmem;          // standard BATs: 0x80000000 and 0xC0000000 both map physical 0
    u8 codePages[(32u << 20) >> 12];  // physical 4 KB pages that hold compiled code (for icbi invalidation)
    JitStats stats;
};

static Jit* g_jit;  // helpers called from generated code reach the JIT through this (one JIT per process)
u32 jit_debug_max_insns;
const u32* jit_debug_last_code;  // most recently compiled block and its length in words (tests dump it)
u32 jit_debug_last_words;

// ---- helpers called from generated code ----
// Memory slow path: anything the inline fast path does not handle (MMIO, unaligned, other mappings). g_status:
// 1 = the access raised an exception (g->pc = handler; the instruction has no effect), 2 = it completed but
// interrupt-relevant state changed (g->pc = next instruction; finish the instruction, then leave compiled code).
static u32 g_status;

static u32 jit_load(Gekko* g, u32 ea, u32 size, u32 pc) {
    GkPriv* p = GKP(g);
    p->curPc = pc;
    p->npc = pc + 4;
    u32 v = 0;
    bool ok = size == 1 ? gk_rd8(g, ea, &v) : size == 2 ? gk_rd16(g, ea, &v) : gk_rd32(g, ea, &v);
    if (!ok) { g->pc = p->npc; g_status = 1; return 0; }
    if (p->brk || p->stopReq) { g->pc = pc + 4; g_status = 2; }
    return v;
}

// sizePc = pc | log2(size): instructions are word aligned, so the two low bits are free.
static void jit_store(Gekko* g, u32 ea, u32 v, u32 sizePc) {
    GkPriv* p = GKP(g);
    u32 size = 1u << (sizePc & 3), pc = sizePc & ~3u;
    p->curPc = pc;
    p->npc = pc + 4;
    bool ok = size == 1 ? gk_wr8(g, ea, v) : size == 2 ? gk_wr16(g, ea, v) : gk_wr32(g, ea, v);
    if (!ok) { g->pc = p->npc; g_status = 1; return; }
    if (p->brk || p->stopReq) { g->pc = pc + 4; g_status = 2; }
}

// Interpreter fallback for one instruction. Returns non-zero (and sets g->pc) when the block must exit.
static u32 jit_interp(Gekko* g, u32 pc, u32 inst) {
    bool stop;
    u32 npc = gekko_interp_one(g, pc, inst, &stop);
    if (npc != pc + 4 || stop) { g->pc = npc; return 1; }
    return 0;
}

// ---- code generation ----
// Encoding of a constant that is known to fit a data-processing immediate (checked at startup by jit_create).
static u32 IMM(u32 v) { u32 enc = 0; arm_imm(v, &enc); return enc; }
typedef struct {
    Emit e;
    u32 pc;          // guest address of the instruction being compiled
    u32 count;       // guest instructions compiled so far, including the current one
    bool fastmem;
    bool pendingStop;   // a slow memory path was emitted: after the instruction, leave if it reported status 2
    // Out-of-line exit stubs: each returns `count` with g->pc already written.
    struct { u32 at; u32 count; } exits[MAX_BLOCK_INSNS * 4];
    u32 nExits;
} Cg;

static void ld_gpr(Cg* c, int host, u32 r) { ldr_imm(&c->e, host, RG, OFF_GPR(r)); }
static void st_gpr(Cg* c, int host, u32 r) { str_imm(&c->e, host, RG, OFF_GPR(r)); }

// Branch (if `cond`) to an exit stub that returns c->count instructions executed.
static void exit_if(Cg* c, int cond) {
    c->exits[c->nExits].at = branch_c(&c->e, cond, c->e.n);
    c->exits[c->nExits].count = c->count;
    c->nExits++;
}

// Call a C helper with up to 4 word arguments already in r0-r3.
static void call(Cg* c, const void* fn) {
    load_const(&c->e, R12, (u32)(uintptr_t)fn);
    blx_reg(&c->e, R12);
}

// CR0 from a result in `r` (signed compare with 0) plus XER[SO].
static void update_cr0(Cg* c, int r) {
    Emit* e = &c->e;
    dp_imm(e, CMP, 1, 0, r, 0);
    ldr_imm(e, R1, RG, OFF_CR);
    dp_imm(e, BIC, 0, R1, R1, IMM(0xF0000000u));
    dp_imm_c(e, C_LT, ORR, 0, R1, R1, IMM(0x80000000u));
    dp_imm_c(e, C_GT, ORR, 0, R1, R1, IMM(0x40000000u));
    dp_imm_c(e, C_EQ, ORR, 0, R1, R1, IMM(0x20000000u));
    ldr_imm(e, R2, RG, OFF_XER);
    dp_imm(e, TST, 1, 0, R2, IMM(XER_SO));
    dp_imm_c(e, C_NE, ORR, 0, R1, R1, IMM(0x10000000u));
    str_imm(e, R1, RG, OFF_CR);
}

// CR field `fld` from the ARM flags of a compare already done (signed or unsigned), plus XER[SO].
static void update_crf_from_flags(Cg* c, u32 fld, bool isSigned) {
    Emit* e = &c->e;
    u32 sh = 28 - 4 * fld;
    u32 enc = 0;
    ldr_imm(e, R1, RG, OFF_CR);
    load_const(e, R2, 0xFu << sh);  // flags are not touched by MOV/ORR without S
    dp_reg(e, BIC, 0, R1, R1, R2);
    arm_imm(8u << sh, &enc); dp_imm_c(e, isSigned ? C_LT : C_CC, ORR, 0, R1, R1, enc);
    arm_imm(4u << sh, &enc); dp_imm_c(e, isSigned ? C_GT : C_HI, ORR, 0, R1, R1, enc);
    arm_imm(2u << sh, &enc); dp_imm_c(e, C_EQ, ORR, 0, R1, R1, enc);
    ldr_imm(e, R2, RG, OFF_XER);
    dp_imm(e, TST, 1, 0, R2, IMM(XER_SO));
    arm_imm(1u << sh, &enc); dp_imm_c(e, C_NE, ORR, 0, R1, R1, enc);
    str_imm(e, R1, RG, OFF_CR);
}

// XER[CA] from the ARM carry flag.
static void set_ca_from_c(Cg* c) {
    Emit* e = &c->e;
    ldr_imm(e, R3, RG, OFF_XER);
    dp_imm(e, BIC, 0, R3, R3, IMM(XER_CA));
    dp_imm_c(e, C_CS, ORR, 0, R3, R3, IMM(XER_CA));
    str_imm(e, R3, RG, OFF_XER);
}

// ARM carry flag from XER[CA] (bit 29 shifted out by LSL #3). Clobbers r3.
static void load_ca_to_c(Cg* c) {
    ldr_imm(&c->e, R3, RG, OFF_XER);
    mov_shift(&c->e, 1, R3, R3, LSL, 3);
}

// rd = rotl(rs, sh) & mask (rlwinm), using r12 for masks that do not encode.
static void rotate_mask(Cg* c, int rd, int rs, u32 sh, u32 mask) {
    Emit* e = &c->e;
    u32 enc;
    if (sh) mov_shift(e, 0, rd, rs, ROR, (int)(32 - sh)); else mov_reg(e, rd, rs);
    if (mask == 0xFFFFFFFFu) return;
    if (arm_imm(mask, &enc)) dp_imm(e, AND, 0, rd, rd, enc);
    else if (arm_imm(~mask, &enc)) dp_imm(e, BIC, 0, rd, rd, enc);
    else { load_const(e, R12, mask); dp_reg(e, AND, 0, rd, rd, R12); }
}

static u32 ppc_mask(u32 mb, u32 me) {
    u32 hi = 0xFFFFFFFFu >> mb, lo = me == 31 ? 0 : 0xFFFFFFFFu >> (me + 1);
    return mb <= me ? hi & ~lo : hi | ~lo;
}

// After a slow memory call whose status was not 1 (r3 = status, r12 = &g_status): status 2 sets RSTOP so the
// instruction leaves compiled code once it has finished; the status is cleared for the next call.
static void stop_flag_from_status(Cg* c) {
    Emit* e = &c->e;
    dp_imm(e, CMP, 1, 0, R3, 2);
    dp_imm_c(e, C_EQ, MOV, 0, RSTOP, 0, 1);
    dp_imm(e, MOV, 0, R3, 0, 0);
    str_imm(e, R3, R12, 0);
    c->pendingStop = true;
}

// Effective address into r0: (rA|0) + d, or rA + rB for indexed forms.
static void ea_disp(Cg* c, u32 ra, u32 d) {
    if (ra) { ld_gpr(c, R0, ra); add_const(&c->e, R0, R0, d, R12); }
    else load_const(&c->e, R0, d);
}
static void ea_index(Cg* c, u32 ra, u32 rb) {
    ld_gpr(c, R1, rb);
    if (ra) { ld_gpr(c, R0, ra); dp_reg(&c->e, ADD, 0, R0, R0, R1); }
    else mov_reg(&c->e, R0, R1);
}

// Load `size` bytes from the guest address in r0 into r2 (zero- or sign-extended). The fast path covers aligned
// accesses to MEM1 through the standard cached/uncached mappings; everything else calls jit_load.
static void emit_load(Cg* c, u32 size, bool sign) {
    Emit* e = &c->e;
    u32 slow = 0, done = 0;
    if (c->fastmem) {
        u32 enc = 0;
        if (size > 1) dp_imm(e, TST, 1, 0, R0, size - 1);
        if (size > 1) slow = branch_c(e, C_NE, 0);
        dp_imm(e, BIC, 0, R1, R0, IMM(0x40000000u));  // fold 0xC0000000 onto 0x80000000
        dp_imm(e, SUB, 0, R1, R1, IMM(0x80000000u));
        arm_imm(0x01800000u, &enc);
        dp_imm(e, CMP, 1, 0, R1, enc);
        u32 slow2 = branch_c(e, C_CS, 0);
        if (size == 4) { ldr_reg(e, R2, RRAM, R1); rev(e, R2, R2); }
        else if (size == 2) { ldrh_reg(e, R2, RRAM, R1); if (sign) revsh(e, R2, R2); else rev16(e, R2, R2); }
        else ldrb_reg(e, R2, RRAM, R1);
        done = branch_c(e, C_AL, 0);
        if (size > 1) patch_branch(e, slow, e->n);
        patch_branch(e, slow2, e->n);
    }
    // Slow path: r0 = ea already; r1 = size, r2 = pc; r0 = g.
    mov_reg(e, R1, R0);
    mov_reg(e, R0, RG);
    load_const(e, R2, size);
    load_const(e, R3, c->pc);
    call(c, (const void*)jit_load);
    mov_reg(e, R2, R0);
    load_const(e, R12, (u32)(uintptr_t)&g_status);
    ldr_imm(e, R3, R12, 0);
    dp_imm(e, CMP, 1, 0, R3, 1);
    {
        u32 skip = branch_c(e, C_NE, 0);  // fault: leave without writing anything (the exit counts this instruction)
        dp_imm(e, MOV, 0, R3, 0, 0);
        str_imm(e, R3, R12, 0);
        exit_if(c, C_AL);
        patch_branch(e, skip, e->n);
    }
    stop_flag_from_status(c);
    if (sign && size == 2) sxth(e, R2, R2);
    if (c->fastmem) patch_branch(e, done, e->n);
}

// Store `size` bytes of r2 to the guest address in r0.
static void emit_store(Cg* c, u32 size) {
    Emit* e = &c->e;
    u32 slow = 0, done = 0;
    if (c->fastmem) {
        u32 enc = 0;
        if (size > 1) { dp_imm(e, TST, 1, 0, R0, size - 1); slow = branch_c(e, C_NE, 0); }
        dp_imm(e, BIC, 0, R1, R0, IMM(0x40000000u));
        dp_imm(e, SUB, 0, R1, R1, IMM(0x80000000u));
        arm_imm(0x01800000u, &enc);
        dp_imm(e, CMP, 1, 0, R1, enc);
        u32 slow2 = branch_c(e, C_CS, 0);
        if (size == 4) { rev(e, R3, R2); str_reg(e, R3, RRAM, R1); }
        else if (size == 2) { rev16(e, R3, R2); strh_reg(e, R3, RRAM, R1); }
        else strb_reg(e, R2, RRAM, R1);
        done = branch_c(e, C_AL, 0);
        if (size > 1) patch_branch(e, slow, e->n);
        patch_branch(e, slow2, e->n);
    }
    mov_reg(e, R1, R0);
    mov_reg(e, R0, RG);
    // r2 already holds the value; r3 = pc | log2(size) (pc is word aligned).
    load_const(e, R3, c->pc | (size == 4 ? 2u : size == 2 ? 1u : 0u));
    call(c, (const void*)jit_store);
    load_const(e, R12, (u32)(uintptr_t)&g_status);
    ldr_imm(e, R3, R12, 0);
    dp_imm(e, CMP, 1, 0, R3, 1);
    u32 skip = branch_c(e, C_NE, 0);
    dp_imm(e, MOV, 0, R3, 0, 0);
    str_imm(e, R3, R12, 0);
    exit_if(c, C_AL);
    patch_branch(e, skip, e->n);
    stop_flag_from_status(c);
    if (c->fastmem) patch_branch(e, done, e->n);
}

static void emit_fallback(Cg* c, u32 inst) {
    Emit* e = &c->e;
    mov_reg(e, R0, RG);
    load_const(e, R1, c->pc);
    load_const(e, R2, inst);
    call(c, (const void*)jit_interp);
    dp_imm(e, CMP, 1, 0, R0, 0);
    exit_if(c, C_NE);
}

// Writes g->pc = r_target and exits with the current count.
static void exit_to_reg(Cg* c, int r) {
    str_imm(&c->e, r, RG, OFF_PC);
    exit_if(c, C_AL);
}

// ---- branches (end the block) ----
static void emit_branch_cond(Cg* c, u32 bo, u32 bi, int targetReg) {
    // targetReg holds the taken target; fallthrough is pc + 4. r0 = CR, r3 = CTR.
    Emit* e = &c->e;
    u32 fall = c->pc + 4;
    u32 notTaken[2];
    int nNot = 0;
    if (!(bo & 4)) {
        ldr_imm(e, R3, RG, OFF_CTR);
        dp_imm(e, SUB, 1, R3, R3, 1);
        str_imm(e, R3, RG, OFF_CTR);
        notTaken[nNot++] = branch_c(e, (bo & 2) ? C_NE : C_EQ, 0);  // branch if CTR == 0 (bo&2) / != 0
    }
    if (!(bo & 16)) {
        ldr_imm(e, R0, RG, OFF_CR);
        u32 bit = 1u << (31 - bi), enc;
        if (!arm_imm(bit, &enc)) enc = 0;
        dp_imm(e, TST, 1, 0, R0, enc);
        notTaken[nNot++] = branch_c(e, (bo & 8) ? C_EQ : C_NE, 0);  // want bit set: not taken if clear
    }
    exit_to_reg(c, targetReg);
    for (int i = 0; i < nNot; i++) patch_branch(e, notTaken[i], e->n);
    load_const(e, R1, fall);
    exit_to_reg(c, R1);
}

static bool compile_branch(Cg* c, u32 inst) {
    Emit* e = &c->e;
    u32 op = inst >> 26, lk = inst & 1;
    if (op == 18) {  // b, bl, ba, bla
        s32 li = ((s32)(inst << 6)) >> 6 & ~3;
        u32 target = (inst & 2) ? (u32)li : c->pc + (u32)li;
        if (lk) { load_const(e, R1, c->pc + 4); str_imm(e, R1, RG, OFF_LR); }
        load_const(e, R2, target);
        exit_to_reg(c, R2);
        return true;
    }
    if (op == 16) {  // bc
        u32 bo = (inst >> 21) & 31, bi = (inst >> 16) & 31;
        s32 bd = (s32)(s16)(inst & 0xFFFC);
        u32 target = (inst & 2) ? (u32)bd : c->pc + (u32)bd;
        if (lk) { load_const(e, R1, c->pc + 4); str_imm(e, R1, RG, OFF_LR); }
        load_const(e, R2, target);
        emit_branch_cond(c, bo, bi, R2);
        return true;
    }
    if (op == 19) {
        u32 xo = (inst >> 1) & 0x3FF, bo = (inst >> 21) & 31, bi = (inst >> 16) & 31;
        if (xo == 16 || xo == 528) {  // bclr / bcctr
            if (xo == 528 && !(bo & 4)) return false;  // invalid form: let the interpreter handle it
            ldr_imm(e, R2, RG, xo == 16 ? OFF_LR : OFF_CTR);   // read before LK overwrites LR
            dp_imm(e, BIC, 0, R2, R2, 3);
            if (lk) { load_const(e, R1, c->pc + 4); str_imm(e, R1, RG, OFF_LR); }
            emit_branch_cond(c, bo, bi, R2);
            return true;
        }
    }
    return false;
}

static bool is_branch(u32 inst) {
    u32 op = inst >> 26, xo = (inst >> 1) & 0x3FF;
    return op == 18 || op == 16 || (op == 19 && (xo == 16 || xo == 528));
}

// Instructions after which compiled code must stop, because they change state the block was compiled against
// (MSR, BATs, segment registers, caches) or are rare control transfers.
static bool ends_block(u32 inst) {
    u32 op = inst >> 26, xo = (inst >> 1) & 0x3FF;
    if (op == 17) return true;                                     // sc
    if (op == 19) return xo == 50 || xo == 150;                     // rfi, isync
    if (op == 31) return xo == 146 || xo == 210 || xo == 242 || xo == 467 || xo == 982 || xo == 4 || xo == 598;
    if (op == 3) return true;                                       // twi
    return false;
}

// ---- integer instructions ----
static bool compile_int(Cg* c, u32 inst) {
    Emit* e = &c->e;
    u32 op = inst >> 26, rd = (inst >> 21) & 31, ra = (inst >> 16) & 31, rb = (inst >> 11) & 31;
    u32 uimm = inst & 0xFFFF, simm = (u32)(s32)(s16)uimm, rc = inst & 1;
    u32 enc;
    switch (op) {
    case 14:  // addi
        if (ra) { ld_gpr(c, R0, ra); add_const(e, R0, R0, simm, R12); } else load_const(e, R0, simm);
        st_gpr(c, R0, rd);
        return true;
    case 15:  // addis
        if (ra) { ld_gpr(c, R0, ra); add_const(e, R0, R0, uimm << 16, R12); } else load_const(e, R0, uimm << 16);
        st_gpr(c, R0, rd);
        return true;
    case 12: case 13:  // addic, addic.
        ld_gpr(c, R0, ra);
        load_const(e, R1, simm);
        dp_reg(e, ADD, 1, R0, R0, R1);
        set_ca_from_c(c);
        st_gpr(c, R0, rd);
        if (op == 13) update_cr0(c, R0);
        return true;
    case 8:  // subfic
        ld_gpr(c, R0, ra);
        load_const(e, R1, simm);
        dp_reg(e, SUB, 1, R0, R1, R0);  // imm - ra; C = imm >= ra (unsigned) = PowerPC CA
        set_ca_from_c(c);
        st_gpr(c, R0, rd);
        return true;
    case 7:  // mulli
        ld_gpr(c, R0, ra);
        load_const(e, R1, simm);
        mul(e, R2, R0, R1);
        st_gpr(c, R2, rd);
        return true;
    case 10: case 11: {  // cmpli, cmpi
        if (inst & (1u << 21)) return false;  // L = 1 (64-bit) is invalid on a 32-bit core
        ld_gpr(c, R0, ra);
        u32 imm = op == 11 ? simm : uimm;
        if (arm_imm(imm, &enc)) dp_imm(e, CMP, 1, 0, R0, enc);
        else if (arm_imm(0u - imm, &enc)) dp_imm(e, CMN, 1, 0, R0, enc);
        else { load_const(e, R1, imm); dp_reg(e, CMP, 1, 0, R0, R1); }
        update_crf_from_flags(c, rd >> 2, op == 11);
        return true;
    }
    case 24: case 25: case 26: case 27: case 28: case 29: {  // ori oris xori xoris andi. andis.
        u32 imm = (op & 1) ? uimm << 16 : uimm;
        int opc = op <= 25 ? ORR : op <= 27 ? EOR : AND;
        ld_gpr(c, R0, rd);  // rS
        if (arm_imm(imm, &enc)) dp_imm(e, opc, 0, R0, R0, enc);
        else { load_const(e, R1, imm); dp_reg(e, opc, 0, R0, R0, R1); }
        st_gpr(c, R0, ra);
        if (op >= 28) update_cr0(c, R0);
        return true;
    }
    case 21: {  // rlwinm
        u32 sh = rb, mb = (inst >> 6) & 31, me = (inst >> 1) & 31;
        ld_gpr(c, R0, rd);
        rotate_mask(c, R0, R0, sh, ppc_mask(mb, me));
        st_gpr(c, R0, ra);
        if (rc) update_cr0(c, R0);
        return true;
    }
    case 20: {  // rlwimi: ra = (rotl(rs, sh) & m) | (ra & ~m)
        u32 sh = rb, m = ppc_mask((inst >> 6) & 31, (inst >> 1) & 31);
        ld_gpr(c, R0, rd);
        rotate_mask(c, R0, R0, sh, m);
        ld_gpr(c, R1, ra);
        if (arm_imm(m, &enc)) dp_imm(e, BIC, 0, R1, R1, enc);
        else { load_const(e, R12, m); dp_reg(e, BIC, 0, R1, R1, R12); }
        dp_reg(e, ORR, 0, R0, R0, R1);
        st_gpr(c, R0, ra);
        if (rc) update_cr0(c, R0);
        return true;
    }
    case 23: {  // rlwnm
        u32 m = ppc_mask((inst >> 6) & 31, (inst >> 1) & 31);
        ld_gpr(c, R0, rd);
        ld_gpr(c, R1, rb);
        dp_imm(e, AND, 0, R1, R1, 31);
        dp_imm(e, RSB, 0, R1, R1, 32);  // rotate right by 32 - n (32 behaves as 0)
        dp_rsr(e, MOV, 0, R0, 0, R0, ROR, R1);
        if (m != 0xFFFFFFFFu) {
            if (arm_imm(m, &enc)) dp_imm(e, AND, 0, R0, R0, enc);
            else { load_const(e, R12, m); dp_reg(e, AND, 0, R0, R0, R12); }
        }
        st_gpr(c, R0, ra);
        if (rc) update_cr0(c, R0);
        return true;
    }
    case 32: case 33: case 34: case 35: case 40: case 41: case 42: case 43: {  // lwz lwzu lbz lbzu lhz lhzu lha lhau
        bool update = op & 1;
        if (update && (ra == 0 || ra == rd)) return false;  // invalid forms
        u32 size = op <= 33 ? 4 : op <= 35 ? 1 : 2;
        ea_disp(c, ra, simm);
        if (update) mov_reg(e, R4, R0);
        emit_load(c, size, op >= 42);
        st_gpr(c, R2, rd);
        if (update) st_gpr(c, R4, ra);
        return true;
    }
    case 36: case 37: case 38: case 39: case 44: case 45: {  // stw stwu stb stbu sth sthu
        bool update = op & 1;
        if (update && ra == 0) return false;
        u32 size = op <= 37 ? 4 : op <= 39 ? 1 : 2;
        ea_disp(c, ra, simm);
        if (update) mov_reg(e, R4, R0);
        ld_gpr(c, R2, rd);
        emit_store(c, size);
        if (update) st_gpr(c, R4, ra);
        return true;
    }
    case 31:
        break;
    default:
        return false;
    }

    // Group 31.
    u32 xo = (inst >> 1) & 0x3FF;
    if ((inst >> 10) & 1) {
        // OE set: only the XO-form arithmetic ops have an OE bit, and overflow tracking is left to the interpreter.
        switch (xo & 0x1FF) { case 266: case 10: case 138: case 40: case 8: case 136: case 104: case 235: case 202: return false; }
    }
    switch (xo) {
    case 266: case 10: case 138: case 202:  // add, addc, adde, addze
    case 40: case 8: case 136: case 104:    // subf, subfc, subfe, neg
    case 522: case 650: case 714: case 552: case 520: case 648: case 616:  // OE forms are rejected above
        if (xo >= 512) return false;
        ld_gpr(c, R0, ra);
        if (xo != 202 && xo != 104) ld_gpr(c, R1, rb);
        switch (xo) {
        case 266: dp_reg(e, ADD, 0, R0, R0, R1); break;
        case 10: dp_reg(e, ADD, 1, R0, R0, R1); set_ca_from_c(c); break;
        case 138: load_ca_to_c(c); dp_reg(e, ADC, 1, R0, R0, R1); set_ca_from_c(c); break;
        case 202: load_ca_to_c(c); dp_imm(e, ADC, 1, R0, R0, 0); set_ca_from_c(c); break;
        case 40: dp_reg(e, SUB, 0, R0, R1, R0); break;
        case 8: dp_reg(e, SUB, 1, R0, R1, R0); set_ca_from_c(c); break;
        case 136: load_ca_to_c(c); dp_reg(e, SBC, 1, R0, R1, R0); set_ca_from_c(c); break;  // ~ra + rb + CA
        case 104: dp_imm(e, RSB, 0, R0, R0, 0); break;
        }
        st_gpr(c, R0, rd);
        if (rc) update_cr0(c, R0);
        return true;
    case 28: case 60: case 444: case 412: case 316: case 124: case 476: case 284: {  // and andc or orc xor nor nand eqv
        ld_gpr(c, R0, rd);  // rS
        ld_gpr(c, R1, rb);
        switch (xo) {
        case 28: dp_reg(e, AND, 0, R0, R0, R1); break;
        case 60: dp_reg(e, BIC, 0, R0, R0, R1); break;
        case 444: dp_reg(e, ORR, 0, R0, R0, R1); break;
        case 412: dp_reg(e, MVN, 0, R1, 0, R1); dp_reg(e, ORR, 0, R0, R0, R1); break;
        case 316: dp_reg(e, EOR, 0, R0, R0, R1); break;
        case 124: dp_reg(e, ORR, 0, R0, R0, R1); dp_reg(e, MVN, 0, R0, 0, R0); break;
        case 476: dp_reg(e, AND, 0, R0, R0, R1); dp_reg(e, MVN, 0, R0, 0, R0); break;
        case 284: dp_reg(e, EOR, 0, R0, R0, R1); dp_reg(e, MVN, 0, R0, 0, R0); break;
        }
        st_gpr(c, R0, ra);
        if (rc) update_cr0(c, R0);
        return true;
    }
    case 24: case 536: {  // slw, srw: shift amounts 32..63 give 0, as ARM's register shifts do
        ld_gpr(c, R0, rd);
        ld_gpr(c, R1, rb);
        dp_imm(e, AND, 0, R1, R1, 0x3F);
        dp_rsr(e, MOV, 0, R0, 0, R0, xo == 24 ? LSL : LSR, R1);
        st_gpr(c, R0, ra);
        if (rc) update_cr0(c, R0);
        return true;
    }
    case 824: {  // srawi: CA = negative source with 1-bits shifted out
        u32 sh = rb;
        ld_gpr(c, R0, rd);
        dp_imm(e, MOV, 0, R2, 0, 0);
        if (sh) {
            mov_shift(e, 1, R3, R0, LSL, (int)(32 - sh));       // Z = no 1-bits shifted out
            dp_reg_c(e, C_NE, MOV, 0, R2, 0, R0, LSR, 31);      // CA = sign when bits were lost
            mov_shift(e, 0, R0, R0, ASR, (int)sh);
        }
        ldr_imm(e, R3, RG, OFF_XER);
        dp_imm(e, BIC, 0, R3, R3, IMM(XER_CA));
        dp_reg_c(e, C_AL, ORR, 0, R3, R3, R2, LSL, 29);
        str_imm(e, R3, RG, OFF_XER);
        st_gpr(c, R0, ra);
        if (rc) update_cr0(c, R0);
        return true;
    }
    case 26: case 954: case 922: {  // cntlzw extsb extsh
        ld_gpr(c, R0, rd);
        if (xo == 26) clz(e, R0, R0); else if (xo == 954) sxtb(e, R0, R0); else sxth(e, R0, R0);
        st_gpr(c, R0, ra);
        if (rc) update_cr0(c, R0);
        return true;
    }
    case 0: case 32:  // cmp, cmpl
        if (inst & (1u << 21)) return false;
        ld_gpr(c, R0, ra);
        ld_gpr(c, R1, rb);
        dp_reg(e, CMP, 1, 0, R0, R1);
        update_crf_from_flags(c, rd >> 2, xo == 0);
        return true;
    case 235: case 75: case 11:  // mullw, mulhw, mulhwu
        ld_gpr(c, R0, ra);
        ld_gpr(c, R1, rb);
        if (xo == 235) mul(e, R2, R0, R1);
        else if (xo == 75) smull(e, R3, R2, R0, R1);
        else umull(e, R3, R2, R0, R1);
        st_gpr(c, R2, rd);
        if (rc) update_cr0(c, R2);
        return true;
    case 23: case 87: case 279: case 343: case 55: case 119: case 311: case 375: {  // lwzx lbzx lhzx lhax (+ux)
        bool update = xo == 55 || xo == 119 || xo == 311 || xo == 375;
        if (update && (ra == 0 || ra == rd)) return false;
        u32 base = update ? xo - 32 : xo;
        u32 size = base == 23 ? 4 : base == 87 ? 1 : 2;
        ea_index(c, ra, rb);
        if (update) mov_reg(e, R4, R0);
        emit_load(c, size, base == 343);
        st_gpr(c, R2, rd);
        if (update) st_gpr(c, R4, ra);
        return true;
    }
    case 151: case 215: case 407: case 183: case 247: case 439: {  // stwx stbx sthx (+ux)
        bool update = xo == 183 || xo == 247 || xo == 439;
        if (update && ra == 0) return false;
        u32 base = update ? xo - 32 : xo;
        u32 size = base == 151 ? 4 : base == 215 ? 1 : 2;
        ea_index(c, ra, rb);
        if (update) mov_reg(e, R4, R0);
        ld_gpr(c, R2, rd);
        emit_store(c, size);
        if (update) st_gpr(c, R4, ra);
        return true;
    }
    case 339: case 467: {  // mfspr / mtspr for LR, CTR, XER only
        u32 spr = ((inst >> 16) & 31) | ((inst >> 6) & 0x3E0);
        u32 off = spr == 8 ? OFF_LR : spr == 9 ? OFF_CTR : spr == 1 ? OFF_XER : 0;
        if (!off) return false;
        if (xo == 339) { ldr_imm(e, R0, RG, off); st_gpr(c, R0, rd); }
        else {
            ld_gpr(c, R0, rd);
            if (spr == 1) { load_const(e, R1, XER_MASK); dp_reg(e, AND, 0, R0, R0, R1); }
            str_imm(e, R0, RG, off);
        }
        return true;
    }
    case 19:  // mfcr
        ldr_imm(e, R0, RG, OFF_CR);
        st_gpr(c, R0, rd);
        return true;
    case 144: {  // mtcrf
        u32 crm = (inst >> 12) & 0xFF, m = 0;
        for (int i = 0; i < 8; i++) if (crm & (0x80u >> i)) m |= 0xF0000000u >> (4 * i);
        ld_gpr(c, R0, rd);
        ldr_imm(e, R1, RG, OFF_CR);
        load_const(e, R2, m);
        dp_reg(e, AND, 0, R0, R0, R2);
        dp_reg(e, BIC, 0, R1, R1, R2);
        dp_reg(e, ORR, 0, R1, R1, R0);
        str_imm(e, R1, RG, OFF_CR);
        return true;
    }
    }
    return false;
}

static void count_fallback(Jit* j, u32 inst) {
    u32 op = inst >> 26;
    j->stats.fallbackPrimary[op]++;
    if (op == 31) j->stats.fallback31[(inst >> 1) & 0x3FF]++;
    else if (op == 19) j->stats.fallback19[(inst >> 1) & 0x3FF]++;
    else if (op == 63) j->stats.fallback63[(inst >> 1) & 0x3FF]++;
    else if (op == 59) j->stats.fallback59[(inst >> 1) & 0x1F]++;
    else if (op == 4) j->stats.fallback4[(inst >> 1) & 0x3FF]++;
}

// Compiles the block starting at guest PC `pc`; NULL when the code is not in RAM or the cache is full.
static BlockFn compile(Jit* j, u32 pc) {
    Gekko* g = j->g;
    u32 pa;
    if (!gk_translate(g, pc, false, false, &pa, false) || pa >= j->ramSize) return NULL;
    if (j->codeWords - j->codeUsed < BLOCK_MAX_WORDS) {
        jit_invalidate_all(j);
        j->stats.flushes++;
    }
    static Cg cg;
    Cg* c = &cg;
    memset(c, 0, sizeof *c);
    c->e = (Emit){ j->code + j->codeUsed, 0, BLOCK_MAX_WORDS, false };
    c->fastmem = j->fastmem && (g->msr & MSR_DR);
    Emit* e = &c->e;
    push_regs(e, 0x4FF8);  // {r3-r11, lr}: 10 registers keep the stack 8-byte aligned for C calls
    mov_reg(e, RG, R0);
    load_const(e, RRAM, (u32)(uintptr_t)j->ram);
    dp_imm(e, MOV, 0, RSTOP, 0, 0);
    u32 startPa = pa;
    bool ended = false;
    u32 maxInsns = jit_debug_max_insns ? jit_debug_max_insns : MAX_BLOCK_INSNS;
    for (u32 i = 0; i < maxInsns && !ended; i++) {
        c->pc = pc + 4 * i;
        u32 ipa = startPa + 4 * i;
        if ((((c->pc ^ pc) >> 12) != 0 && i > 0) || ipa + 4 > j->ramSize) break;  // one 4 KB page, inside RAM
        u32 inst = ((u32)j->ram[ipa] << 24) | ((u32)j->ram[ipa + 1] << 16) | ((u32)j->ram[ipa + 2] << 8) | j->ram[ipa + 3];
        c->count = i + 1;
        j->stats.instrsCompiled++;
        if (is_branch(inst)) {
            if (compile_branch(c, inst)) { j->stats.instrsNative++; ended = true; continue; }
        } else if (compile_int(c, inst)) {
            j->stats.instrsNative++;
            if (c->pendingStop) {
                // g->pc was set to the next instruction by the helper; the exit counts this instruction.
                dp_imm(e, CMP, 1, 0, RSTOP, 0);
                exit_if(c, C_NE);
                c->pendingStop = false;
            }
            continue;
        }
        count_fallback(j, inst);
        emit_fallback(c, inst);
        if (is_branch(inst) || ends_block(inst)) {
            // The interpreter ran it; continue at whatever PC it left.
            load_const(e, R1, c->pc + 4);
            exit_to_reg(c, R1);
            ended = true;
        }
    }
    if (!ended) {  // ran out of instructions (or reached a page end): continue at the first uncompiled one
        load_const(e, R1, pc + 4 * c->count);
        exit_to_reg(c, R1);
    }
    // Exit stubs: return the number of instructions executed.
    for (u32 k = 0; k < c->nExits; k++) {
        patch_branch(e, c->exits[k].at, e->n);
        load_const(e, R0, c->exits[k].count);
        pop_regs(e, 0x8FF8);  // {r3-r11, pc}
    }
    if (e->overflow) return NULL;
    void* fn = j->code + j->codeUsed;
    jit_debug_last_code = fn;
    jit_debug_last_words = e->n;
    jit_host_sync_icache(fn, e->n * 4);
    j->codeUsed += e->n;
    for (u32 a = startPa >> 12; a <= (startPa + 4 * MAX_BLOCK_INSNS) >> 12 && a < sizeof j->codePages; a++) j->codePages[a] = 1;
    j->stats.blocksCompiled++;
    return (BlockFn)fn;
}

// ---- dispatcher ----
static u32 jit_entry(Gekko* g, s64 budget, void* ctx) {
    Jit* j = ctx;
    u32 total = 0;
    // The inline fast path assumes the standard mappings (0x80000000 and 0xC0000000 both at physical 0). Blocks
    // compiled under other mappings use the slow path for every access. (Blocks compiled before a BAT change keep
    // their choice; games set BATs once at boot.)
    u32 a, b;
    j->fastmem = gk_translate(g, 0x80000000u, false, true, &a, false) && a == 0 &&
                 gk_translate(g, 0xC0000000u, false, true, &b, false) && b == 0 && j->ramSize >= 0x01800000u;
    while ((s64)total < budget) {
        u32 pc = g->pc, msrBits = g->msr & (MSR_IR | MSR_DR | MSR_PR);
        Entry* en = &j->table[(pc >> 2) & (DISPATCH_SIZE - 1)];
        if (!en->fn || en->pc != pc || en->msrBits != msrBits) {
            BlockFn fn = compile(j, pc);
            if (!fn) break;
            u32 pa;
            en->pc = pc; en->msrBits = msrBits; en->fn = fn;
            en->idle = gk_translate(g, pc, false, false, &pa, false) && pa + 4 <= j->ramSize &&
                       ((u32)j->ram[pa] << 24 | (u32)j->ram[pa + 1] << 16 | (u32)j->ram[pa + 2] << 8 | j->ram[pa + 3]) == 0x48000000u;
        }
        // Idle loop ("b ." with interrupts on): nothing can change until the next event, so use up the budget.
        if (en->idle && (g->msr & MSR_EE)) { total += (u32)(budget - (s64)total > 0 ? budget - (s64)total : 1); break; }
        u32 n = en->fn(g);
        j->stats.blocksRun++;
        if (!n) break;  // cannot happen for a well-formed block; never spin
        total += n;
        GkPriv* p = GKP(g);
        if (p->brk || p->stopReq) break;
    }
    return total;
}

Jit* jit_create(Gekko* g, u8* ram, u32 ramSize, u32 codeBytes) {
    Jit* j = calloc(1, sizeof *j);
    if (!j) return NULL;
    j->g = g;
    j->ram = ram;
    j->ramSize = ramSize;
    j->code = jit_host_alloc_exec(codeBytes);
    j->codeWords = codeBytes / 4;
    j->table = calloc(DISPATCH_SIZE, sizeof(Entry));
    if (!j->code || !j->table) { free(j->table); free(j); return NULL; }
    g_jit = j;
    return j;
}

void jit_destroy(Jit* j) {
    if (!j) return;
    gekko_set_jit(j->g, NULL, NULL);
    free(j->table);
    free(j);  // the executable region is kept for the process lifetime
}

void jit_attach(Jit* j) { gekko_set_jit(j->g, jit_entry, j); }

u32 jit_run_one_block(Jit* j) {
    Gekko* g = j->g;
    GKP(g)->brk = false;
    u32 msrBits = g->msr & (MSR_IR | MSR_DR | MSR_PR);
    u32 a, b;
    j->fastmem = gk_translate(g, 0x80000000u, false, true, &a, false) && a == 0 &&
                 gk_translate(g, 0xC0000000u, false, true, &b, false) && b == 0 && j->ramSize >= 0x01800000u;
    Entry* en = &j->table[(g->pc >> 2) & (DISPATCH_SIZE - 1)];
    if (!en->fn || en->pc != g->pc || en->msrBits != msrBits) {
        BlockFn fn = compile(j, g->pc);
        if (!fn) return 0;
        en->pc = g->pc; en->msrBits = msrBits; en->fn = fn;
    }
    return en->fn(g);
}

void jit_invalidate_all(Jit* j) {
    memset(j->table, 0, DISPATCH_SIZE * sizeof(Entry));
    memset(j->codePages, 0, sizeof j->codePages);
    j->codeUsed = 0;
}

void jit_invalidate(Jit* j, u32 pa) {
    // Level 1 keeps no per-page block lists: any icbi that touches a page holding compiled code drops everything.
    if ((pa >> 12) < sizeof j->codePages && j->codePages[pa >> 12]) jit_invalidate_all(j);
}

const JitStats* jit_stats(const Jit* j) { return &j->stats; }

void jit_print_stats(const Jit* j, void (*print)(const char* line)) {
    char line[160];
    const JitStats* s = &j->stats;
    snprintf(line, sizeof line, "JIT: %llu blocks compiled, %llu run, %llu/%llu instructions native (%.1f%%), %llu flushes",
             (unsigned long long)s->blocksCompiled, (unsigned long long)s->blocksRun, (unsigned long long)s->instrsNative,
             (unsigned long long)s->instrsCompiled, s->instrsCompiled ? 100.0 * (double)s->instrsNative / (double)s->instrsCompiled : 0.0,
             (unsigned long long)s->flushes);
    print(line);
    // The most frequent compiled-but-interpreted instructions (static counts: one per compiled occurrence).
    for (int k = 0; k < 12; k++) {
        u32 best = 0, which = 0, group = 0;
        for (u32 i = 0; i < 64; i++) if (i != 31 && i != 19 && i != 63 && i != 59 && i != 4 && s->fallbackPrimary[i] > best) { best = s->fallbackPrimary[i]; which = i; group = 0; }
        for (u32 i = 0; i < 1024; i++) {
            if (s->fallback31[i] > best) { best = s->fallback31[i]; which = i; group = 31; }
            if (s->fallback19[i] > best) { best = s->fallback19[i]; which = i; group = 19; }
            if (s->fallback63[i] > best) { best = s->fallback63[i]; which = i; group = 63; }
            if (s->fallback4[i] > best) { best = s->fallback4[i]; which = i; group = 4; }
        }
        for (u32 i = 0; i < 32; i++) if (s->fallback59[i] > best) { best = s->fallback59[i]; which = i; group = 59; }
        if (!best) break;
        snprintf(line, sizeof line, "  fallback %s%u%s%u: %u", group ? "op " : "op ", group ? group : which, group ? " xo " : "", group ? which : 0, best);
        print(line);
        JitStats* m = (JitStats*)s;  // clear the printed entry to find the next one
        if (!group) m->fallbackPrimary[which] = 0;
        else if (group == 31) m->fallback31[which] = 0;
        else if (group == 19) m->fallback19[which] = 0;
        else if (group == 63) m->fallback63[which] = 0;
        else if (group == 4) m->fallback4[which] = 0;
        else m->fallback59[which] = 0;
    }
}
#endif
