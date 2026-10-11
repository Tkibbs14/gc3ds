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
// FPR r: ps0 at +0 and ps1 at +8, each a host double (low word first); vldr/vstr reach 1020 bytes.
#define OFF_FPR(r) ((u32)offsetof(Gekko, fpr) + 16u * (u32)(r))
#define OFF_FPSCR ((u32)offsetof(Gekko, fpscr))
_Static_assert(offsetof(Gekko, fpr) + 16 * 31 + 8 + 4 < 1020, "FPRs reachable with vldr/vstr");

typedef u32 (*BlockFn)(Gekko* g);
// Every compiled block stays reachable until the code cache is flushed: entries live in a pool, chained per hash
// bucket (a direct-mapped table evicted and recompiled colliding blocks, e.g. a real-mode exception handler at
// 0x00000500 and code at 0x80000500, hundreds of times per second).
typedef struct { u32 pc; u32 msrBits; BlockFn fn; bool idle; u8 poll; u32 next; } Entry;  // poll: polling-loop length or 0
#define MAX_ENTRIES (1u << 16)
#define NO_ENTRY 0xFFFFFFFFu

struct Jit {
    Gekko* g;
    u8* ram;
    u32 ramSize;
    u32* code;
    u32 codeWords, codeUsed;
    Entry* table;          // pool of MAX_ENTRIES
    u32* buckets;          // DISPATCH_SIZE chain heads
    u32 nEntries;
    bool fastmem;          // standard BATs: 0x80000000 and 0xC0000000 both map physical 0
    u8 codePages[(32u << 20) >> 12];  // physical 4 KB pages that hold compiled code (for icbi invalidation)
    JitStats stats;
};

static Jit* g_jit;  // helpers called from generated code reach the JIT through this (one JIT per process)
u32 jit_debug_max_insns;
u32 jit_debug_plant;
const u32* jit_debug_last_code;  // most recently compiled block and its length in words (tests dump it)
u32 jit_debug_last_words;

// ---- helpers called from generated code ----
// Memory slow path: anything the inline fast path does not handle (MMIO, unaligned, other mappings). g_status:
// 1 = the access raised an exception (g->pc = handler; the instruction has no effect), 2 = it completed but
// interrupt-relevant state changed (g->pc = next instruction; finish the instruction, then leave compiled code).
static u32 g_status;

static u32 jit_load(Gekko* g, u32 ea, u32 size, u32 pc) {
    g_jit->stats.slowLoads++;
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
    g_jit->stats.slowStores++;
    GkPriv* p = GKP(g);
    u32 size = 1u << (sizePc & 3), pc = sizePc & ~3u;
    p->curPc = pc;
    p->npc = pc + 4;
    bool ok = size == 1 ? gk_wr8(g, ea, v) : size == 2 ? gk_wr16(g, ea, v) : gk_wr32(g, ea, v);
    if (!ok) { g->pc = p->npc; g_status = 1; return; }
    if (p->brk || p->stopReq) { g->pc = pc + 4; g_status = 2; }
}

// lfd / stfd slow paths (same status protocol): the helper moves the FPR itself.
static void jit_load64(Gekko* g, u32 ea, u32 pc, u32 frd) {
    g_jit->stats.slowLoads++;
    GkPriv* p = GKP(g);
    p->curPc = pc;
    p->npc = pc + 4;
    u64 v;
    if (!gk_rd64(g, ea, &v)) { g->pc = p->npc; g_status = 1; return; }
    fpr_set0(g, (int)frd, v);
    if (p->brk || p->stopReq) { g->pc = pc + 4; g_status = 2; }
}

static void jit_store64(Gekko* g, u32 ea, u32 pc, u32 frs) {
    g_jit->stats.slowStores++;
    GkPriv* p = GKP(g);
    p->curPc = pc;
    p->npc = pc + 4;
    if (!gk_wr64(g, ea, fpr_get0(g, (int)frs))) { g->pc = p->npc; g_status = 1; return; }
    if (p->brk || p->stopReq) { g->pc = pc + 4; g_status = 2; }
}

// Interpreter fallback for one instruction. Returns non-zero (and sets g->pc) when the block must exit.
static void count_op(JitOpCounts* t, u32 inst) {
    u32 op = inst >> 26, xo = (inst >> 1) & 0x3FF, xo5 = xo & 31;
    // A-form float and paired-single instructions keep a register in the upper extended bits: count by xo5.
    if ((op == 63 && xo5 >= 18) || (op == 4 && ((xo5 >= 10 && xo5 <= 15) || xo5 == 18 || xo5 == 20 || xo5 == 21 || xo5 >= 23)))
        xo = xo5;
    t->primary[op]++;
    if (op == 31) t->x31[xo]++;
    else if (op == 19) t->x19[xo]++;
    else if (op == 63) t->x63[xo]++;
    else if (op == 59) t->x59[xo5]++;
    else if (op == 4) t->x4[xo]++;
}

static u32 jit_interp(Gekko* g, u32 pc, u32 inst) {
    u64 n = ++g_jit->stats.fallbacksRun;
    count_op(&g_jit->stats.fallbackRun, inst);
    bool stop;
    u32 npc;
    if (g_profClock && !(n & 63)) {  // sampled: a clock read per fallback would cost more than many fallbacks
        u32 op = inst >> 26;
        u64 t0 = g_profClock();
        npc = gekko_interp_one(g, pc, inst, &stop);
        g_hostProf[op == 4 || op >= 48 ? PROF_FBFP : PROF_FB] += (g_profClock() - t0) * 64;
    } else {
        npc = gekko_interp_one(g, pc, inst, &stop);
    }
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
    bool fp;            // MSR[FP] set and FPSCR[RN] = 0 (round to nearest): float instructions compile natively
    bool ps;            // ... and HID2[PSE]: paired-single instructions too
    bool lsq;           // HID2[LSQE]: psq_l / psq_st (D forms) are legal
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
    if (op == 63) return xo == 38 || xo == 70 || xo == 134 || xo == 711;  // mtfsb1 mtfsb0 mtfsfi mtfsf: rounding mode
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

// ---- floating point and paired singles ----
// "Fast float" accuracy model (as Dolphin's default): FPR results equal the interpreter's bit for bit for finite normal
// operands and results, except (a) the multiply-add forms round the product before the add (VFP11 has no fused
// multiply-add), so they may differ by 1 ulp of the result precision, and (b) denormals flush to zero and NaN payloads
// differ (host FPSCR = 0x03000000, see jit_host_fpscr_init). FPSCR is not maintained, except FPCC and the CR field by
// fcmpu / fcmpo. Single-precision results are computed in double and rounded to single (exact for single operands).
// Blocks are keyed on MSR[FP], HID2[PSE, LSQE] and FPSCR[RN] != 0, so compiled code never runs under other settings. FPSCR[NI]
// (flush denormals, which games set) matches the host and needs no guard.
// Record forms (Rc = 1), other rounding modes and everything not listed here run on the interpreter.
static void ld_fpr(Cg* c, int d, u32 fr, u32 lane) { vldr_d(&c->e, d, RG, OFF_FPR(fr) + 8u * lane); }
static void st_fpr(Cg* c, int d, u32 fr, u32 lane) { vstr_d(&c->e, d, RG, OFF_FPR(fr) + 8u * lane); }

// d = d rounded to single precision (still held as a double). Two registers at once so the conversions overlap.
static void round_pair(Cg* c, int d0, int d1) {
    Emit* e = &c->e;
    if (jit_debug_plant == 1) return;
    vcvt_f32_f64(e, 2 * d0, d0);
    if (d1 >= 0) vcvt_f32_f64(e, 2 * d1, d1);
    vcvt_f64_f32(e, d0, 2 * d0);
    if (d1 >= 0) vcvt_f64_f32(e, d1, 2 * d1);
}

// d = lane of frC with its significand cut to 25 bits, which is what Gekko's single-precision multiplier sees:
// (x + 2^27) & ~(2^28 - 1) on the 64-bit pattern (the interpreter's sf_round25; identity for exact singles). Uses r0, r1.
static void ld_c25(Cg* c, int d, u32 fr, u32 lane) {
    Emit* e = &c->e;
    ldr_imm(e, R0, RG, OFF_FPR(fr) + 8u * lane);
    ldr_imm(e, R1, RG, OFF_FPR(fr) + 8u * lane + 4);
    dp_imm(e, ADD, 1, R0, R0, IMM(0x08000000u));
    dp_imm(e, ADC, 0, R1, R1, 0);
    dp_imm(e, AND, 0, R0, R0, IMM(0xF0000000u));
    vmov_d_rr(e, d, R0, R1);
}

// dst = the word stfs / psq_st store for the double in an FPR lane: the exponent and mantissa bits are cut, not rounded
// (PEM D.7, as the interpreter does); anything below the normal single range, zero included, becomes a signed zero.
static void dbl_to_single_bits(Cg* c, int dst, u32 fr, u32 lane, int t1, int t2) {
    Emit* e = &c->e;
    u32 o = OFF_FPR(fr) + 8u * lane;
    ldr_imm(e, t1, RG, o);
    ldr_imm(e, t2, RG, o + 4);
    mov_shift(e, 0, dst, t1, LSR, 29);
    dp_reg_c(e, C_AL, ORR, 0, dst, dst, t2, LSL, 3);
    dp_imm(e, BIC, 0, dst, dst, IMM(0xC0000000u));
    dp_imm(e, AND, 0, t1, t2, IMM(0xC0000000u));
    dp_reg(e, ORR, 0, dst, dst, t1);
    dp_imm(e, BIC, 0, t1, t2, IMM(0x80000000u));          // |x| below 2^-126 (biased exponent <= 896): signed zero
    dp_imm(e, SUB, 1, t1, t1, IMM(0x38000000u));
    dp_imm_c(e, C_CS, CMP, 1, 0, t1, IMM(0x00100000u));
    dp_imm(e, AND, 0, t2, t2, IMM(0x80000000u));
    dp_reg_c(e, C_CC, MOV, 0, dst, 0, t2, LSL, 0);
}

// After a slow-path helper call: g_status 1 = the access raised an exception (leave; nothing was written), 2 = it
// completed but the block must stop after this instruction.
static void slow_status(Cg* c) {
    Emit* e = &c->e;
    load_const(e, R12, (u32)(uintptr_t)&g_status);
    ldr_imm(e, R3, R12, 0);
    dp_imm(e, CMP, 1, 0, R3, 1);
    u32 skip = branch_c(e, C_NE, 0);
    dp_imm(e, MOV, 0, R3, 0, 0);
    str_imm(e, R3, R12, 0);
    exit_if(c, C_AL);
    patch_branch(e, skip, e->n);
    stop_flag_from_status(c);
}

// lfd: the 8-byte value at the guest address in r0 into FPR frd. The fast path needs 8-byte alignment inside MEM1.
static void emit_load64(Cg* c, u32 frd) {
    Emit* e = &c->e;
    u32 slow = 0, slow2 = 0, done = 0;
    if (c->fastmem) {
        dp_imm(e, TST, 1, 0, R0, 7);
        slow = branch_c(e, C_NE, 0);
        dp_imm(e, BIC, 0, R1, R0, IMM(0x40000000u));
        dp_imm(e, SUB, 0, R1, R1, IMM(0x80000000u));
        dp_imm(e, CMP, 1, 0, R1, IMM(0x01800000u));
        slow2 = branch_c(e, C_CS, 0);
        dp_reg(e, ADD, 0, R1, R1, RRAM);
        ldmia(e, R1, (1u << R2) | (1u << R3));
        rev(e, R2, R2);
        rev(e, R3, R3);
        str_imm(e, R3, RG, OFF_FPR(frd));
        str_imm(e, R2, RG, OFF_FPR(frd) + 4);
        done = branch_c(e, C_AL, 0);
        patch_branch(e, slow, e->n);
        patch_branch(e, slow2, e->n);
    }
    mov_reg(e, R1, R0);
    mov_reg(e, R0, RG);
    load_const(e, R2, c->pc);
    load_const(e, R3, frd);
    call(c, (const void*)jit_load64);
    slow_status(c);
    if (c->fastmem) patch_branch(e, done, e->n);
}

// stfd: FPR frs (ps0) to the guest address in r0.
static void emit_store64(Cg* c, u32 frs) {
    Emit* e = &c->e;
    u32 slow = 0, slow2 = 0, done = 0;
    if (c->fastmem) {
        dp_imm(e, TST, 1, 0, R0, 7);
        slow = branch_c(e, C_NE, 0);
        dp_imm(e, BIC, 0, R1, R0, IMM(0x40000000u));
        dp_imm(e, SUB, 0, R1, R1, IMM(0x80000000u));
        dp_imm(e, CMP, 1, 0, R1, IMM(0x01800000u));
        slow2 = branch_c(e, C_CS, 0);
        ldr_imm(e, R2, RG, OFF_FPR(frs) + 4);
        ldr_imm(e, R3, RG, OFF_FPR(frs));
        rev(e, R2, R2);
        rev(e, R3, R3);
        dp_reg(e, ADD, 0, R1, R1, RRAM);
        stmia(e, R1, (1u << R2) | (1u << R3));
        done = branch_c(e, C_AL, 0);
        patch_branch(e, slow, e->n);
        patch_branch(e, slow2, e->n);
    }
    mov_reg(e, R1, R0);
    mov_reg(e, R0, RG);
    load_const(e, R2, c->pc);
    load_const(e, R3, frs);
    call(c, (const void*)jit_store64);
    slow_status(c);
    if (c->fastmem) patch_branch(e, done, e->n);
}

// lfs lfd stfs stfd and their update / indexed forms. kind: 0 lfs, 1 lfd, 2 stfs, 3 stfd.
static bool float_mem(Cg* c, u32 kind, u32 fr, u32 ra, u32 rb, u32 simm, bool indexed, bool update) {
    Emit* e = &c->e;
    if (update && ra == 0) return false;  // invalid form
    if (kind == 2) dbl_to_single_bits(c, R2, fr, 0, R6, R7);
    if (indexed) ea_index(c, ra, rb); else ea_disp(c, ra, simm);
    if (update) mov_reg(e, R4, R0);
    switch (kind) {
    case 0:
        emit_load(c, 4, false);
        vmov_s_r(e, 0, R2);
        vcvt_f64_f32(e, 1, 0);
        st_fpr(c, 1, fr, 0);
        st_fpr(c, 1, fr, 1);
        break;
    case 1: emit_load64(c, fr); break;
    case 2: emit_store(c, 4); break;
    default: emit_store64(c, fr); break;
    }
    if (update) st_gpr(c, R4, ra);
    return true;
}

// psq_l / psq_st and their update / indexed forms. Native only when the GQR says plain float for this direction and the
// access is aligned and inside MEM1; everything else runs the interpreter for this one instruction.
static bool compile_psq(Cg* c, u32 inst, bool store, bool indexed, bool update) {
    Emit* e = &c->e;
    if (!c->fastmem || !c->ps || (!indexed && !c->lsq)) return false;
    u32 fr = (inst >> 21) & 31, ra = (inst >> 16) & 31, rb = (inst >> 11) & 31;
    if (update && ra == 0) return false;
    u32 w = indexed ? (inst >> 10) & 1 : (inst >> 15) & 1, idx = indexed ? (inst >> 7) & 7 : (inst >> 12) & 7;
    u32 gqr = (u32)offsetof(Gekko, spr) + 4u * (SPR_GQR0 + idx);
    u32 fb[4], nfb = 0;
    add_const(e, R1, RG, gqr & ~0xFFFu, R12);
    ldr_imm(e, R0, R1, gqr & 0xFFFu);
    dp_imm(e, AND, 1, R1, R0, IMM(store ? 0x3F00u : 0x3F000000u));        // scale
    dp_imm_c(e, C_EQ, AND, 1, R1, R0, IMM(store ? 0x7u : 0x70000u));      // type
    fb[nfb++] = branch_c(e, C_NE, 0);
    if (indexed) ea_index(c, ra, rb); else ea_disp(c, ra, (u32)((s32)(inst << 20) >> 20));
    if (update) mov_reg(e, R4, R0);
    dp_imm(e, TST, 1, 0, R0, 3);
    fb[nfb++] = branch_c(e, C_NE, 0);
    dp_imm(e, BIC, 0, R1, R0, IMM(0x40000000u));
    dp_imm(e, SUB, 0, R1, R1, IMM(0x80000000u));
    dp_imm(e, CMP, 1, 0, R1, IMM(0x01800000u));
    fb[nfb++] = branch_c(e, C_CS, 0);
    if (!w) {  // the second word must be inside too
        dp_imm(e, ADD, 0, R3, R1, 4);
        dp_imm(e, CMP, 1, 0, R3, IMM(0x01800000u));
        fb[nfb++] = branch_c(e, C_CS, 0);
    }
    dp_reg(e, ADD, 0, R1, R1, RRAM);
    if (!store) {
        if (w) {
            ldr_imm(e, R2, R1, 0);
            rev(e, R2, R2);
            vmov_s_r(e, 0, R2);
            vcvt_f64_f32(e, 1, 0);
            st_fpr(c, 1, fr, 0);
            load_const(e, R2, 0x3FF00000u);  // ps1 = 1.0
            dp_imm(e, MOV, 0, R3, 0, 0);
            str_imm(e, R3, RG, OFF_FPR(fr) + 8);
            str_imm(e, R2, RG, OFF_FPR(fr) + 12);
        } else {
            ldmia(e, R1, (1u << R2) | (1u << R3));
            rev(e, R2, R2);
            rev(e, R3, R3);
            vmov_ss_rr(e, 0, R2, R3);
            vcvt_f64_f32(e, 1, 0);
            vcvt_f64_f32(e, 2, 1);
            st_fpr(c, 1, fr, 0);
            st_fpr(c, 2, fr, 1);
        }
    } else {
        dbl_to_single_bits(c, R2, fr, 0, R6, R7);
        rev(e, R2, R2);
        if (w) {
            str_imm(e, R2, R1, 0);
        } else {
            dbl_to_single_bits(c, R3, fr, 1, R6, R7);
            rev(e, R3, R3);
            stmia(e, R1, (1u << R2) | (1u << R3));
        }
    }
    if (update) st_gpr(c, R4, ra);
    u32 done = branch_c(e, C_AL, 0);
    for (u32 k = 0; k < nfb; k++) patch_branch(e, fb[k], e->n);
    emit_fallback(c, inst);
    patch_branch(e, done, e->n);
    return true;
}

// fcmpu / fcmpo: CR field and FPSCR[FPCC] = 8 less, 4 greater, 2 equal, 1 unordered (exception bits are not kept).
static void emit_fcmp(Cg* c, u32 crf, u32 fa, u32 fb) {
    Emit* e = &c->e;
    u32 sh = 28 - 4 * crf, enc = 0;
    ld_fpr(c, 0, fa, 0);
    ld_fpr(c, 1, fb, 0);
    vcmp_d(e, 0, 1);
    vmrs_apsr(e);
    dp_imm(e, MOV, 0, R0, 0, 1);
    dp_imm_c(e, C_MI, MOV, 0, R0, 0, 8);
    dp_imm_c(e, C_EQ, MOV, 0, R0, 0, 2);
    dp_imm_c(e, C_GT, MOV, 0, R0, 0, 4);
    ldr_imm(e, R1, RG, OFF_CR);
    arm_imm(0xFu << sh, &enc);
    dp_imm(e, BIC, 0, R1, R1, enc);
    dp_reg_c(e, C_AL, ORR, 0, R1, R1, R0, LSL, (int)sh);
    str_imm(e, R1, RG, OFF_CR);
    ldr_imm(e, R2, RG, OFF_FPSCR);
    dp_imm(e, BIC, 0, R2, R2, IMM(FPSCR_FPCC));
    dp_reg_c(e, C_AL, ORR, 0, R2, R2, R0, LSL, 12);
    str_imm(e, R2, RG, OFF_FPSCR);
}

// fctiw / fctiwz: low word = the int32 (saturating), high word 0xFFF80000, plus bit 32 for a zero result from a
// negative operand (the Gekko quirk the interpreter reproduces).
static void emit_fctiw(Cg* c, u32 frd, u32 frb, bool trunc) {
    Emit* e = &c->e;
    ld_fpr(c, 0, frb, 0);
    if (trunc) vcvt_s32_f64(e, 0, 0); else vcvtr_s32_f64(e, 0, 0);
    vmov_r_s(e, R0, 0);
    ldr_imm(e, R1, RG, OFF_FPR(frb) + 4);
    load_const(e, R2, 0xFFF80000u);
    dp_imm(e, CMP, 1, 0, R0, 0);
    dp_imm_c(e, C_NE, MOV, 0, R1, 0, 0);
    mov_shift(e, 0, R1, R1, LSR, 31);
    dp_reg(e, ORR, 0, R2, R2, R1);
    str_imm(e, R0, RG, OFF_FPR(frd));
    str_imm(e, R2, RG, OFF_FPR(frd) + 4);
}

// Paired-single arithmetic, per lane k: a_k op b_k, a_k * c', or b_k + a_k * c' (c' = frC lane rounded to 25 bits),
// each rounded to single. xo5: 10 sum0, 11 sum1, 12 muls0, 13 muls1, 14 madds0, 15 madds1, 18 div, 20 sub, 21 add,
// 25 mul, 28 msub, 29 madd, 30 nmsub, 31 nmadd.
static void ps_arith(Cg* c, u32 xo5, u32 fd, u32 fa, u32 fb, u32 fc) {
    Emit* e = &c->e;
    if (xo5 == 10 || xo5 == 11) {  // the sum goes to one lane, frC's other lane passes through (rounded)
        bool s0 = xo5 == 10;
        ld_fpr(c, 0, fa, 0);
        ld_fpr(c, 1, fb, 1);
        ld_fpr(c, 2, fc, s0 ? 1 : 0);
        vfp_d(e, VF_ADD, 0, 0, 1);
        round_pair(c, 0, 2);
        st_fpr(c, s0 ? 0 : 2, fd, 0);
        st_fpr(c, s0 ? 2 : 0, fd, 1);
        return;
    }
    bool plain = xo5 == 18 || xo5 == 20 || xo5 == 21;                  // div sub add: a op b
    bool madd = xo5 == 14 || xo5 == 15 || xo5 >= 28;                    // b + a * c'
    int res[2];
    for (u32 k = 0; k < 2; k++) {
        int d = (int)(4 * k);
        u32 cl = (xo5 == 12 || xo5 == 14) ? 0 : (xo5 == 13 || xo5 == 15) ? 1 : k;
        ld_fpr(c, d, fa, k);
        if (plain) ld_fpr(c, d + 1, fb, k); else ld_c25(c, d + 1, fc, cl);
        if (madd) ld_fpr(c, d + 2, fb, k);
        res[k] = madd ? d + 2 : d;
    }
    for (u32 k = 0; k < 2; k++) {
        int d = (int)(4 * k);
        if (plain) vfp_d(e, xo5 == 18 ? VF_DIV : xo5 == 20 ? VF_SUB : VF_ADD, d, d, d + 1);
        else if (!madd) vfp_d(e, VF_MUL, d, d, d + 1);
        else vfp_d(e, (xo5 == 28 || xo5 == 30) ? VF_NMLS : VF_MLA, d + 2, d, d + 1);
        if (xo5 >= 30) vneg_d(e, res[k], res[k]);
    }
    round_pair(c, res[0], res[1]);
    st_fpr(c, res[0], fd, 0);
    st_fpr(c, res[1], fd, 1);
}

static bool compile_float(Cg* c, u32 inst) {
    Emit* e = &c->e;
    u32 op = inst >> 26, frd = (inst >> 21) & 31, ra = (inst >> 16) & 31, rb = (inst >> 11) & 31, frc = (inst >> 6) & 31;
    u32 xo = (inst >> 1) & 0x3FF, xo5 = xo & 31, simm = (u32)(s32)(s16)(inst & 0xFFFF);
    if (!c->fp) return false;
    switch (op) {
    case 48: case 49: case 50: case 51: case 52: case 53: case 54: case 55:
        return float_mem(c, (op - 48) >> 1, frd, ra, 0, simm, false, op & 1);
    case 56: case 57: case 60: case 61:  // psq_l psq_lu psq_st psq_stu
        return compile_psq(c, inst, op >= 60, false, op & 1);
    case 31:  // lfsx lfsux lfdx lfdux stfsx stfsux stfdx stfdux
        if (xo < 535 || xo > 759 || (xo - 535) % 32) return false;
        return float_mem(c, (xo - 535) / 64, frd, ra, rb, 0, true, ((xo - 535) / 32) & 1);
    case 59: case 63: {
        bool single = op == 59;
        if (op == 63) {
            switch (xo) {
            case 0: case 32: emit_fcmp(c, (inst >> 23) & 7, ra, rb); return true;
            case 12: case 14: case 15: case 40: case 72: case 264:
                if (inst & 1) return false;
                break;
            default: goto arith;
            }
            switch (xo) {
            case 12: ld_fpr(c, 0, rb, 0); round_pair(c, 0, -1); st_fpr(c, 0, frd, 0); st_fpr(c, 0, frd, 1); break;  // frsp
            case 14: case 15: emit_fctiw(c, frd, rb, xo == 15); break;
            case 72: ld_fpr(c, 0, rb, 0); st_fpr(c, 0, frd, 0); break;                                              // fmr
            default:  // fneg, fabs: the sign bit only
                ldr_imm(e, R0, RG, OFF_FPR(rb));
                ldr_imm(e, R1, RG, OFF_FPR(rb) + 4);
                if (xo == 40) dp_imm(e, EOR, 0, R1, R1, IMM(0x80000000u)); else dp_imm(e, BIC, 0, R1, R1, IMM(0x80000000u));
                str_imm(e, R0, RG, OFF_FPR(frd));
                str_imm(e, R1, RG, OFF_FPR(frd) + 4);
                break;
            }
            return true;
        }
    arith:
        if (inst & 1) return false;
        switch (xo5) { case 18: case 20: case 21: case 25: case 28: case 29: case 30: case 31: break; default: return false; }
        int res = 0;
        ld_fpr(c, 0, ra, 0);
        if (xo5 == 18 || xo5 == 20 || xo5 == 21) {
            ld_fpr(c, 1, rb, 0);
            vfp_d(e, xo5 == 18 ? VF_DIV : xo5 == 20 ? VF_SUB : VF_ADD, 0, 0, 1);
        } else {
            if (single) ld_c25(c, 1, frc, 0); else ld_fpr(c, 1, frc, 0);
            if (xo5 == 25) {
                vfp_d(e, VF_MUL, 0, 0, 1);
            } else {  // fmsub fmadd fnmsub fnmadd: b + a*c, -b + a*c, negated for the n forms
                ld_fpr(c, 2, rb, 0);
                vfp_d(e, (xo5 == 28 || xo5 == 30) ? VF_NMLS : VF_MLA, 2, 0, 1);
                if (xo5 >= 30) vneg_d(e, 2, 2);
                res = 2;
            }
        }
        if (single) { round_pair(c, res, -1); st_fpr(c, res, frd, 0); st_fpr(c, res, frd, 1); }
        else st_fpr(c, res, frd, 0);
        return true;
    }
    case 4: {
        if (!c->ps) return false;
        switch (xo & 63) {  // psq_lx psq_stx psq_lux psq_stux
        case 6: return compile_psq(c, inst, false, true, false);
        case 7: return compile_psq(c, inst, true, true, false);
        case 38: return compile_psq(c, inst, false, true, true);
        case 39: return compile_psq(c, inst, true, true, true);
        }
        if (inst & 1) return false;
        switch (xo) {
        case 528: case 560: case 592: case 624:  // ps_merge00 / 01 / 10 / 11
            ld_fpr(c, 0, ra, (xo >> 6) & 1);
            ld_fpr(c, 1, rb, (xo >> 5) & 1);
            st_fpr(c, 0, frd, 0);
            st_fpr(c, 1, frd, 1);
            return true;
        case 72:  // ps_mr
            ld_fpr(c, 0, rb, 0);
            ld_fpr(c, 1, rb, 1);
            st_fpr(c, 0, frd, 0);
            st_fpr(c, 1, frd, 1);
            return true;
        case 40: case 136: case 264: {  // ps_neg ps_nabs ps_abs
            int opc = xo == 40 ? EOR : xo == 136 ? ORR : BIC;
            ldr_imm(e, R0, RG, OFF_FPR(rb));
            ldr_imm(e, R1, RG, OFF_FPR(rb) + 4);
            ldr_imm(e, R2, RG, OFF_FPR(rb) + 8);
            ldr_imm(e, R3, RG, OFF_FPR(rb) + 12);
            dp_imm(e, opc, 0, R1, R1, IMM(0x80000000u));
            dp_imm(e, opc, 0, R3, R3, IMM(0x80000000u));
            str_imm(e, R0, RG, OFF_FPR(frd));
            str_imm(e, R1, RG, OFF_FPR(frd) + 4);
            str_imm(e, R2, RG, OFF_FPR(frd) + 8);
            str_imm(e, R3, RG, OFF_FPR(frd) + 12);
            return true;
        }
        }
        switch (xo5) {
        case 10: case 11: case 12: case 13: case 14: case 15: case 18: case 20: case 21: case 25: case 28: case 29: case 30: case 31:
            ps_arith(c, xo5, frd, ra, rb, frc);
            return true;
        }
        return false;
    }
    }
    return false;
}

// Dispatch key of a block: the machine state compiled code depends on, beyond the guest PC.
static u32 block_key(const Gekko* g) {
    return (g->msr & (MSR_IR | MSR_DR | MSR_PR | MSR_FP)) | ((g->fpscr & FPSCR_RN) ? 1u << 30 : 0u) |
           (g->spr[SPR_HID2] & (HID2_PSE | HID2_LSQE));
}

// Compiles the block starting at guest PC `pc`; NULL when the code is not in RAM or the cache is full.
static BlockFn compile_block(Jit* j, u32 pc) {
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
    c->fp = (g->msr & MSR_FP) && !(g->fpscr & FPSCR_RN);
    c->ps = c->fp && (g->spr[SPR_HID2] & HID2_PSE);
    c->lsq = (g->spr[SPR_HID2] & HID2_LSQE) != 0;
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
        } else if (compile_int(c, inst) || compile_float(c, inst)) {
            j->stats.instrsNative++;
            if (c->pendingStop) {
                // g->pc was set to the next instruction by the helper; the exit counts this instruction.
                dp_imm(e, CMP, 1, 0, RSTOP, 0);
                exit_if(c, C_NE);
                c->pendingStop = false;
            }
            continue;
        }
        count_op(&j->stats.fallback, inst);
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

static BlockFn compile(Jit* j, u32 pc) {
    u64 t0 = prof_begin();
    BlockFn fn = compile_block(j, pc);
    prof_end(PROF_JITC, t0);
    return fn;
}

// ---- dispatcher ----
static u32 bucket_of(u32 pc, u32 msrBits) { return ((pc >> 2) ^ (pc >> 18) ^ (msrBits << 9)) & (DISPATCH_SIZE - 1); }

// Finds the block for (pc, msrBits), compiling it on first use; NULL when it cannot be compiled.
static Entry* lookup(Jit* j, u32 pc, u32 msrBits) {
    u32* head = &j->buckets[bucket_of(pc, msrBits)];
    for (u32 i = *head; i != NO_ENTRY; i = j->table[i].next)
        if (j->table[i].pc == pc && j->table[i].msrBits == msrBits) return &j->table[i];
    if (j->nEntries == MAX_ENTRIES) { jit_invalidate_all(j); j->stats.flushes++; head = &j->buckets[bucket_of(pc, msrBits)]; }
    BlockFn fn = compile(j, pc);
    if (!fn) return NULL;
    // compile() may have flushed the whole cache (code space full), which also emptied the chains.
    head = &j->buckets[bucket_of(pc, msrBits)];
    Gekko* g = j->g;
    u32 pa, idx = j->nEntries++;
    Entry* en = &j->table[idx];
    en->pc = pc; en->msrBits = msrBits; en->fn = fn;
    en->idle = gk_translate(g, pc, false, false, &pa, false) && pa + 4 <= j->ramSize &&
               ((u32)j->ram[pa] << 24 | (u32)j->ram[pa + 1] << 16 | (u32)j->ram[pa + 2] << 8 | j->ram[pa + 3]) == 0x48000000u;
    en->poll = (u8)gekko_poll_loop_len(g, pc);
    en->next = *head;
    *head = idx;
    return en;
}
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
        u32 pc = g->pc, msrBits = block_key(g);
        Entry* en = lookup(j, pc, msrBits);
        if (!en) break;
        // Idle loop ("b ." with interrupts on): nothing can change until the next event, so use up the budget.
        if (en->idle && (g->msr & MSR_EE)) { total += (u32)(budget - (s64)total > 0 ? budget - (s64)total : 1); break; }
        u32 n = en->fn(g);
        j->stats.blocksRun++;
        if (!n) break;  // cannot happen for a well-formed block; never spin
        total += n;
        j->stats.instrsRun += n;
        // Polling loop (gekko_poll_loop_len) that went round again: nothing changes until the next event either.
        if (en->poll && g->pc == pc && (g->msr & MSR_EE) && gekko_poll_loop_ram(g, pc, en->poll, j->ramSize)) {
            j->stats.pollSkips++;
            if ((s64)total < budget) total = (u32)budget;
            break;
        }
        GkPriv* p = GKP(g);
        if (p->brk || p->stopReq) break;
    }
    return total;
}

Jit* jit_create(Gekko* g, u8* ram, u32 ramSize, u32 codeBytes) {
    Jit* j = calloc(1, sizeof *j);
    if (!j) return NULL;
    jit_host_fpscr_init();
    j->g = g;
    j->ram = ram;
    j->ramSize = ramSize;
    j->code = jit_host_alloc_exec(codeBytes);
    j->codeWords = codeBytes / 4;
    j->table = calloc(MAX_ENTRIES, sizeof(Entry));
    j->buckets = malloc(DISPATCH_SIZE * sizeof(u32));
    if (!j->code || !j->table || !j->buckets) { free(j->table); free(j->buckets); free(j); return NULL; }
    jit_invalidate_all(j);
    g_jit = j;
    return j;
}

void jit_destroy(Jit* j) {
    if (!j) return;
    gekko_set_jit(j->g, NULL, NULL);
    free(j->table);
    free(j->buckets);
    free(j);  // the executable region is kept for the process lifetime
}

void jit_attach(Jit* j) { gekko_set_jit(j->g, jit_entry, j); }

u32 jit_run_one_block(Jit* j) {
    Gekko* g = j->g;
    GKP(g)->brk = false;
    u32 msrBits = block_key(g);
    u32 a, b;
    j->fastmem = gk_translate(g, 0x80000000u, false, true, &a, false) && a == 0 &&
                 gk_translate(g, 0xC0000000u, false, true, &b, false) && b == 0 && j->ramSize >= 0x01800000u;
    Entry* en = lookup(j, g->pc, msrBits);
    return en ? en->fn(g) : 0;
}

void jit_invalidate_all(Jit* j) {
    memset(j->buckets, 0xFF, DISPATCH_SIZE * sizeof(u32));
    j->nEntries = 0;
    memset(j->codePages, 0, sizeof j->codePages);
    j->codeUsed = 0;
}

void jit_invalidate(Jit* j, u32 pa) {
    // Level 1 keeps no per-page block lists: any icbi that touches a page holding compiled code drops everything.
    j->stats.icbis++;
    if ((pa >> 12) < sizeof j->codePages && j->codePages[pa >> 12]) { j->stats.icbiFlushes++; jit_invalidate_all(j); }
}

const JitStats* jit_stats(const Jit* j) { return &j->stats; }

// The 16 most frequent opcodes in `t`, with a mnemonic from the disassembler.
static void print_top(const JitOpCounts* t, const char* title, void (*print)(const char* line)) {
    static JitOpCounts c;
    c = *t;
    char line[160], dis[64];
    snprintf(line, sizeof line, "  %s:", title);
    print(line);
    for (int k = 0; k < 16; k++) {
        u32 best = 0, which = 0, group = 0;
        for (u32 i = 0; i < 64; i++) if (i != 31 && i != 19 && i != 63 && i != 59 && i != 4 && c.primary[i] > best) { best = c.primary[i]; which = i; group = 0; }
        for (u32 i = 0; i < 1024; i++) {
            if (c.x31[i] > best) { best = c.x31[i]; which = i; group = 31; }
            if (c.x19[i] > best) { best = c.x19[i]; which = i; group = 19; }
            if (c.x63[i] > best) { best = c.x63[i]; which = i; group = 63; }
            if (c.x4[i] > best) { best = c.x4[i]; which = i; group = 4; }
        }
        for (u32 i = 0; i < 32; i++) if (c.x59[i] > best) { best = c.x59[i]; which = i; group = 59; }
        if (!best) break;
        u32 inst = group ? (group << 26) | (which << 1) : which << 26;
        gekko_disasm(0, inst, dis, sizeof dis);
        char* sp = strchr(dis, ' ');
        if (sp) *sp = 0;
        if (group) snprintf(line, sizeof line, "    op %u xo %u %-10s %u", group, which, dis, best);
        else snprintf(line, sizeof line, "    op %u %-10s %u", which, dis, best);
        print(line);
        if (!group) c.primary[which] = 0;
        else if (group == 31) c.x31[which] = 0;
        else if (group == 19) c.x19[which] = 0;
        else if (group == 63) c.x63[which] = 0;
        else if (group == 4) c.x4[which] = 0;
        else c.x59[which] = 0;
    }
}

void jit_print_stats(const Jit* j, void (*print)(const char* line)) {
    char line[160];
    const JitStats* s = &j->stats;
    snprintf(line, sizeof line, "JIT: icbi %llu (%llu dropped all code), %u blocks live", (unsigned long long)s->icbis,
             (unsigned long long)s->icbiFlushes, j->nEntries);
    print(line);
    snprintf(line, sizeof line, "JIT: %llu blocks compiled, %llu run, %llu/%llu instructions native (%.1f%%), %llu flushes",
             (unsigned long long)s->blocksCompiled, (unsigned long long)s->blocksRun, (unsigned long long)s->instrsNative,
             (unsigned long long)s->instrsCompiled, s->instrsCompiled ? 100.0 * (double)s->instrsNative / (double)s->instrsCompiled : 0.0,
             (unsigned long long)s->flushes);
    print(line);
    snprintf(line, sizeof line, "JIT: %llu instructions run, %llu interpreted (%.1f%%)", (unsigned long long)s->instrsRun,
             (unsigned long long)s->fallbacksRun, s->instrsRun ? 100.0 * (double)s->fallbacksRun / (double)s->instrsRun : 0.0);
    print(line);
    print_top(&s->fallback, "compiled-but-interpreted (static)", print);
    print_top(&s->fallbackRun, "interpreted at run time (dynamic)", print);
}
#endif
