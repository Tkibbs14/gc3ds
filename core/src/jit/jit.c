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

// Host register roles inside compiled code (set once by the entry stub; blocks jump to each other without saving them).
#define RG R11   // Gekko*
#define RRAM R10 // host pointer to physical RAM 0
#define RDC R9   // guest instructions left in this run (each exit subtracts what its block executed)
#define RSTOP R5 // set to 1 by a slow memory access that requires leaving compiled code after the instruction

// Blocks are linked: an exit to a known guest address starts as a branch to a stub that returns to the dispatcher,
// which compiles the target and then patches that branch to jump straight to it. Exits to an address in a register
// (blr, bctr) look the address up in a small direct-mapped table first. Every block starts with a budget check, so a
// chain of linked blocks stops when RDC runs out.
#define FAST_SIZE 512
typedef struct { u32 pc; void* fn; } FastEntry;
// The shared stubs at the start of the code buffer (word offsets), kept across flushes.
enum { ST_ENTER, ST_EXIT, ST_LINK, ST_DYN, ST_BUDGET, ST_INDIRECT, ST_WORDS = 64 };

// Offsets into Gekko (ldr/str immediate range is 0..4095; everything used here is below that).
#define OFF_GPR(r) ((u32)offsetof(Gekko, gpr) + 4u * (u32)(r))
#define OFF_CR ((u32)offsetof(Gekko, cr))
#define OFF_XER ((u32)offsetof(Gekko, xer))
#define OFF_LR ((u32)offsetof(Gekko, lr))
#define OFF_CTR ((u32)offsetof(Gekko, ctr))
#define OFF_PC ((u32)offsetof(Gekko, pc))
#define OFF_MSR ((u32)offsetof(Gekko, msr))
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
    u32 stub[ST_INDIRECT + 1];  // word offsets of the shared stubs in `code`
    u32 stubWords;              // code words they use (compiled blocks start after them)
    FastEntry* fast;            // indirect-branch table, valid for blocks of key fastKey only
    u32 fastKey;
    u32 gen;                    // bumped by every flush: a pending link from before it is dropped
    bool linking;
    u32 insnsSinceFlush;        // guest instructions compiled into the code now in the buffer
    void (*gatherPut)(void* ctx, u32 v, u32 size);  // jit_set_gather
    void* gatherCtx;
    u32 gatherEa;
};

static Jit* g_jit;  // helpers called from generated code reach the JIT through this (one JIT per process)
u32 jit_debug_max_insns;
u32 jit_debug_plant;
void (*jit_debug_on_compile)(const u32* code, u32 words);
const u32* jit_debug_last_code;  // most recently compiled block and its length in words (tests dump it)
u32 jit_debug_last_words;

// ---- guest time inside compiled code ----
// gekko_run adds the instructions a JIT run executed to g->cycles only when the run returns, so during the run
// g->cycles is the time it started. Helpers that reach the rest of the machine (MMIO, the interpreter: timebase,
// decrementer, scheduled events) must see the exact current time, as the interpreter's would: compiled code stores
// its budget left at the instruction (RDC minus the block's earlier instructions) in g_jitLeft before calling them,
// and they run with g->cycles = start of the run + instructions executed.
static u64 g_runStart;    // g->cycles + instructions already run by this jit_entry, when the run started
static s32 g_runBudget;   // the run's budget
static s32 g_jitLeft;
typedef struct { Gekko* g; u64 saved; } GuestTime;
static GuestTime time_enter(Gekko* g) {
    GuestTime t = { g, g->cycles };
    g->cycles = g_runStart + (u64)(s64)(g_runBudget - g_jitLeft);
    return t;
}
static void time_leave(GuestTime t) { t.g->cycles = t.saved; }

// ---- helpers called from generated code ----
// Memory slow path: anything the inline fast path does not handle (MMIO, unaligned, other mappings). g_status:
// 1 = the access raised an exception (g->pc = handler; the instruction has no effect), 2 = it completed but
// interrupt-relevant state changed (g->pc = next instruction; finish the instruction, then leave compiled code).
static u32 g_status;

static u32 jit_load_at(Gekko* g, u32 ea, u32 size, u32 pc) {
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
static void jit_store_at(Gekko* g, u32 ea, u32 v, u32 sizePc) {
    g_jit->stats.slowStores++;
    GkPriv* p = GKP(g);
    u32 size = 1u << (sizePc & 3), pc = sizePc & ~3u;
    p->curPc = pc;
    p->npc = pc + 4;
    bool ok = size == 1 ? gk_wr8(g, ea, v) : size == 2 ? gk_wr16(g, ea, v) : gk_wr32(g, ea, v);
    if (!ok) { g->pc = p->npc; g_status = 1; return; }
    if (p->brk || p->stopReq) { g->pc = pc + 4; g_status = 2; }
}

// Store to the write-gather pipe (jit_set_gather). Returns 1, with g->pc = the next instruction, when compiled code
// must stop after this instruction (the store raised an interrupt or a stop request).
static u32 jit_gather_at(Gekko* g, u32 v, u32 sizePc) {
    g_jit->stats.gatherStores++;
    g_jit->gatherPut(g_jit->gatherCtx, v, 1u << (sizePc & 3));
    GkPriv* p = GKP(g);
    if (p->brk || p->stopReq) { g->pc = (sizePc & ~3u) + 4; return 1; }
    return 0;
}

// lfd / stfd slow paths (same status protocol): the helper moves the FPR itself.
static void jit_load64_at(Gekko* g, u32 ea, u32 pc, u32 frd) {
    g_jit->stats.slowLoads++;
    GkPriv* p = GKP(g);
    p->curPc = pc;
    p->npc = pc + 4;
    u64 v;
    if (!gk_rd64(g, ea, &v)) { g->pc = p->npc; g_status = 1; return; }
    fpr_set0(g, (int)frd, v);
    if (p->brk || p->stopReq) { g->pc = pc + 4; g_status = 2; }
}

static void jit_store64_at(Gekko* g, u32 ea, u32 pc, u32 frs) {
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

static u32 jit_interp_at(Gekko* g, u32 pc, u32 inst) {
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

// mtmsr from compiled code (OSDisableInterrupts / OSRestoreInterrupts run it constantly). Returns 1 when compiled code
// must stop after it: an MSR bit other than EE changed (address translation, FP and the block key depend on them),
// or EE was set while an interrupt is pending. Otherwise only EE changed, nothing can be due, and the block goes on.
static u32 jit_mtmsr(Gekko* g, u32 v) {
    u32 old = g->msr;
    if (((old ^ v) & ~MSR_EE) || ((v & ~old & MSR_EE) && (g->extIrq || GKP(g)->decPending))) {
        gk_set_msr(g, v);
        return 1;
    }
    g->msr = v;
    return 0;
}

// The helpers as compiled code calls them: each runs at the exact guest time (see g_jitLeft).
static u32 jit_load(Gekko* g, u32 ea, u32 size, u32 pc) {
    GuestTime t = time_enter(g);
    u32 v = jit_load_at(g, ea, size, pc);
    time_leave(t);
    return v;
}
static void jit_store(Gekko* g, u32 ea, u32 v, u32 sizePc) {
    GuestTime t = time_enter(g);
    jit_store_at(g, ea, v, sizePc);
    time_leave(t);
}
static u32 jit_gather(Gekko* g, u32 v, u32 sizePc) {
    GuestTime t = time_enter(g);
    u32 r = jit_gather_at(g, v, sizePc);
    time_leave(t);
    return r;
}
static void jit_load64(Gekko* g, u32 ea, u32 pc, u32 frd) {
    GuestTime t = time_enter(g);
    jit_load64_at(g, ea, pc, frd);
    time_leave(t);
}
static void jit_store64(Gekko* g, u32 ea, u32 pc, u32 frs) {
    GuestTime t = time_enter(g);
    jit_store64_at(g, ea, pc, frs);
    time_leave(t);
}
static u32 jit_interp(Gekko* g, u32 pc, u32 inst) {
    GuestTime t = time_enter(g);
    u32 r = jit_interp_at(g, pc, inst);
    time_leave(t);
    return r;
}

// ---- code generation ----
// Encoding of a constant that is known to fit a data-processing immediate (checked at startup by jit_create).
static u32 IMM(u32 v) { u32 enc = 0; arm_imm(v, &enc); return enc; }

// Register cache: guest GPRs live in host registers r4, r6, r7 and r8 (callee-saved, so C helpers keep them) from
// their first use in a block, and are written back (when changed: `dirty`) at every exit and before the interpreter
// runs an instruction. Instructions compute straight into and from these registers, which removes most of the loads
// and stores of guest state from compiled code.
#define NSLOT 4
static const int kSlot[NSLOT] = { R4, R6, R7, R8 };
typedef struct { s8 id[NSLOT]; bool dirty[NSLOT]; u32 use[NSLOT]; } Rc;  // id: guest GPR or -1

typedef struct {
    Emit e;
    u32 pc;          // guest address of the instruction being compiled
    u32 count;       // guest instructions compiled so far, including the current one
    bool fastmem;
    bool fp;            // MSR[FP] set and FPSCR[RN] = 0 (round to nearest): float instructions compile natively
    bool ps;            // ... and HID2[PSE]: paired-single instructions too
    bool lsq;           // HID2[LSQE]: psq_l / psq_st (D forms) are legal
    bool pendingStop;   // a slow memory path was emitted: after the instruction, leave if it reported status 2
    bool pr;            // MSR[PR]: supervisor-only instructions are left to the interpreter (it raises the exception)
    bool noLink;        // the block is a polling loop: its exits always return to the dispatcher, which skips the loop
    Jit* j;
    // ARM flags still holding the compare behind CR field flagsFld: set where update_crf_from_flags ends (flagsAt);
    // flagsLive when that was the end of the previous instruction.
    u32 flagsAt, flagsFld;
    bool flagsSigned, flagsLive;
    bool tailOn;        // set_tail
    int tailN, tailHost[2];
    u32 tailGpr[2];
    // Guest GPRs cached in host registers within the block (see "register cache" below).
    Rc rc;
    u32 pinned;         // slots used by the current instruction (never evicted during it)
    u32 tick;
    // Out-of-line exit stubs: each writes back the dirty cached GPRs of `snap`, subtracts `count` from RDC and leaves
    // to the dispatcher, after writing g->pc = pc when `setPc` (otherwise g->pc is already written). inCold: the branch
    // to the stub is in the cold code.
    struct { u32 at; u32 count; bool setPc, inCold; u32 pc; Rc snap; } exits[MAX_BLOCK_INSNS * 4];
    u32 nExits;
    // Cold code: slow paths are emitted into a second buffer (`alt` while hot code is current; the two Emit structs
    // swap places) and appended after the block, so hot code falls through its fast paths and packs into fewer cache
    // lines. Branches between the two are fixed up when the cold code is appended.
    Emit alt;
    bool inCold;
    struct { u32 at, target; bool fromCold, toCold; } fix[MAX_BLOCK_INSNS * 8];
    u32 nFix;
} Cg;

#define NO_BRANCH 0xFFFFFFFFu
static u32 g_coldBuf[BLOCK_MAX_WORDS];

static void swap_emit(Cg* c) { Emit t = c->e; c->e = c->alt; c->alt = t; c->inCold = !c->inCold; }
static void add_fix(Cg* c, bool fromCold, u32 at, bool toCold, u32 target) {
    if (c->nFix == sizeof c->fix / sizeof c->fix[0]) { c->e.overflow = true; return; }
    c->fix[c->nFix].at = at;
    c->fix[c->nFix].target = target;
    c->fix[c->nFix].fromCold = fromCold;
    c->fix[c->nFix].toCold = toCold;
    c->nFix++;
}
static void cold_begin(Cg* c) { swap_emit(c); }
// Called in cold code: the hot branch at hotAt goes to the current cold position.
static void fix_to_cold(Cg* c, u32 hotAt) { add_fix(c, false, hotAt, true, c->e.n); }
// Ends a cold fragment with a branch back to hot index hotResume and makes hot code current again.
static void cold_end(Cg* c, u32 hotResume) {
    u32 at = branch_c(&c->e, C_AL, 0);
    add_fix(c, true, at, false, hotResume);
    swap_emit(c);
    c->flagsAt = NO_BRANCH;
}

// ---- register cache ----
// Cold code reads the cache but never changes its state: the hot path continues with the state it had.
static int slot_of(const Rc* rc, u32 r) {
    for (int k = 0; k < NSLOT; k++) if (rc->id[k] == (s8)r) return k;
    return -1;
}

// Writes back the dirty slots of `rc` (does not change it).
static void rc_emit_flush(Cg* c, const Rc* rc) {
    bool skipped = false;
    for (int k = 0; k < NSLOT; k++) {
        if (rc->id[k] < 0 || !rc->dirty[k]) continue;
        if (jit_debug_plant == 4 && !skipped) { skipped = true; continue; }  // the fuzzer's self-test: lose a write
        str_imm(&c->e, kSlot[k], RG, OFF_GPR((u32)rc->id[k]));
    }
}

// Writes back every dirty slot; in hot code they are clean afterwards.
static void rc_flush(Cg* c) {
    rc_emit_flush(c, &c->rc);
    if (!c->inCold) for (int k = 0; k < NSLOT; k++) c->rc.dirty[k] = false;
}

// After the interpreter ran an instruction (it reads and writes guest state in memory): hot code forgets the cache;
// cold code, which must leave the hot path's state intact, reloads every cached GPR from memory instead.
static void rc_after_interp(Cg* c) {
    if (c->inCold) {
        for (int k = 0; k < NSLOT; k++)
            if (c->rc.id[k] >= 0) ldr_imm(&c->e, kSlot[k], RG, OFF_GPR((u32)c->rc.id[k]));
        return;
    }
    for (int k = 0; k < NSLOT; k++) { c->rc.id[k] = -1; c->rc.dirty[k] = false; }
}

// A slot for guest GPR r: an empty one, else the least recently used one not pinned by this instruction (written
// back first if dirty). -1 when all are pinned.
static int rc_alloc(Cg* c, u32 r) {
    int best = -1;
    for (int k = 0; k < NSLOT; k++) {
        if (c->pinned & (1u << k)) continue;
        if (c->rc.id[k] < 0) { best = k; break; }
        if (best < 0 || c->rc.use[k] < c->rc.use[best]) best = k;
    }
    if (best < 0) return -1;
    if (c->rc.id[best] >= 0 && c->rc.dirty[best]) str_imm(&c->e, kSlot[best], RG, OFF_GPR((u32)c->rc.id[best]));
    c->rc.id[best] = (s8)r;
    c->rc.dirty[best] = false;
    return best;
}

static void rc_touch(Cg* c, int k) { c->pinned |= 1u << k; c->rc.use[k] = ++c->tick; }

// A host register holding guest GPR r for reading: its slot (loaded on first use), or `tmp` loaded from memory.
static int src(Cg* c, u32 r, int tmp) {
    int k = slot_of(&c->rc, r);
    if (k >= 0) c->j->stats.forwards++;
    if (c->inCold) {
        if (k >= 0) return kSlot[k];
        ldr_imm(&c->e, tmp, RG, OFF_GPR(r));
        return tmp;
    }
    if (k < 0) {
        k = rc_alloc(c, r);
        if (k < 0) { ldr_imm(&c->e, tmp, RG, OFF_GPR(r)); return tmp; }
        ldr_imm(&c->e, kSlot[k], RG, OFF_GPR(r));
    }
    rc_touch(c, k);
    return kSlot[k];
}

// A host register to compute guest GPR r into (its slot, or `tmp`); dst_done commits the value afterwards. A slot
// is marked dirty only by dst_done, so an exit taken in between writes back the old value.
static int dst(Cg* c, u32 r, int tmp) {
    if (c->inCold) return tmp;
    int k = slot_of(&c->rc, r);
    if (k < 0) k = rc_alloc(c, r);
    if (k < 0) return tmp;
    rc_touch(c, k);
    return kSlot[k];
}
static void dst_done(Cg* c, u32 r, int reg) {
    int k = slot_of(&c->rc, r);
    if (k >= 0 && kSlot[k] == reg) { c->rc.dirty[k] = true; return; }
    str_imm(&c->e, reg, RG, OFF_GPR(r));
    if (k >= 0) mov_reg(&c->e, kSlot[k], reg);  // (cold code) keep the cached copy equal to memory
}

// Plain loads and stores of a GPR through a given host register, for code that does not use src / dst.
static void ld_gpr(Cg* c, int host, u32 r) { mov_reg(&c->e, host, src(c, r, host)); }
static void st_gpr(Cg* c, int host, u32 r) {
    int d = dst(c, r, host);
    mov_reg(&c->e, d, host);
    dst_done(c, r, d);
}
static const u32* stub(const Cg* c, int k) { return c->j->code + c->j->stub[k]; }

// Branch (if `cond`) to an exit stub that leaves with c->count instructions executed (g->pc already written),
// writing back the dirty slots of `snap` (the current cache when NULL).
static void exit_snap(Cg* c, int cond, bool setPc, u32 pc, const Rc* snap) {
    c->exits[c->nExits].at = branch_c(&c->e, cond, c->e.n);
    c->exits[c->nExits].count = c->count;
    c->exits[c->nExits].setPc = setPc;
    c->exits[c->nExits].inCold = c->inCold;
    c->exits[c->nExits].pc = pc;
    c->exits[c->nExits].snap = snap ? *snap : c->rc;
    c->nExits++;
}
static void exit_if(Cg* c, int cond) { exit_snap(c, cond, false, 0, NULL); }
// The same, writing g->pc = pc first; the dispatcher never links it (state the block depends on may have changed).
static void exit_pc_if(Cg* c, int cond, u32 pc) { exit_snap(c, cond, true, pc, NULL); }

// Unconditional exit to the guest address `target`, emitted in place: a link site (a branch the dispatcher patches
// to the target block once it is compiled; until then it falls through to the stub that asks for it). The cache must
// already be written back (rc_flush).
static void exit_const(Cg* c, u32 target) {
    Emit* e = &c->e;
    dp_imm(e, SUB, 0, RDC, RDC, c->count);
    if (c->noLink) {
        load_const(e, R1, target);
        str_imm(e, R1, RG, OFF_PC);
        branch_abs(e, C_AL, false, stub(c, ST_DYN));
        return;
    }
    branch_c(e, C_AL, e->n + 1);  // link site
    branch_abs(e, C_AL, true, stub(c, ST_LINK));
    word(e, target);
}

// Unconditional exit to the guest address in r2 (blr, bctr): through the indirect-branch table.
static void exit_indirect(Cg* c) {
    Emit* e = &c->e;
    str_imm(e, R2, RG, OFF_PC);
    dp_imm(e, SUB, 0, RDC, RDC, c->count);
    branch_abs(e, C_AL, false, stub(c, c->noLink ? ST_DYN : ST_INDIRECT));
}

// Call a C helper with up to 4 word arguments already in r0-r3.
static void call(Cg* c, const void* fn) {
    load_const(&c->e, R12, (u32)(uintptr_t)fn);
    blx_reg(&c->e, R12);
}

// Before a helper that runs at guest time: the budget left at this instruction to g_jitLeft (clobbers lr and r12).
static void emit_time(Cg* c) {
    dp_imm(&c->e, SUB, 0, LR, RDC, c->count - 1);
    load_const(&c->e, R12, (u32)(uintptr_t)&g_jitLeft);
    str_imm(&c->e, LR, R12, 0);
}

// CR field `fld` from the ARM flags of a compare already done (signed or unsigned), plus XER[SO]. The ARM flags are
// left as the compare set them, so a conditional branch right after it on that field tests them directly (see
// emit_branch_cond).
static void update_crf_from_flags(Cg* c, u32 fld, bool isSigned) {
    Emit* e = &c->e;
    u32 sh = 28 - 4 * fld;
    u32 enc = 0;
    ldr_imm(e, R1, RG, OFF_CR);
    ldr_imm(e, R3, RG, OFF_XER);
    load_const(e, R2, 0xFu << sh);  // MOV / ORR / BIC / AND without S leave the flags alone
    dp_reg(e, BIC, 0, R1, R1, R2);
    arm_imm(8u << sh, &enc); dp_imm_c(e, isSigned ? C_LT : C_CC, ORR, 0, R1, R1, enc);
    arm_imm(4u << sh, &enc); dp_imm_c(e, isSigned ? C_GT : C_HI, ORR, 0, R1, R1, enc);
    arm_imm(2u << sh, &enc); dp_imm_c(e, C_EQ, ORR, 0, R1, R1, enc);
    dp_imm(e, AND, 0, R3, R3, IMM(XER_SO));
    dp_reg_c(e, C_AL, ORR, 0, R1, R1, R3, LSR, (int)(31 - sh));  // SO (bit 31) to the field's bit 0
    str_imm(e, R1, RG, OFF_CR);
    c->flagsAt = c->inCold ? NO_BRANCH : e->n;
    c->flagsFld = fld;
    c->flagsSigned = isSigned;
}

// CR0 from a result in `r` (signed compare with 0) plus XER[SO].
static void update_cr0(Cg* c, int r) {
    dp_imm(&c->e, CMP, 1, 0, r, 0);
    update_crf_from_flags(c, 0, true);
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

// The GPR stores that finish an integer or float memory instruction after its access (set by the caller before
// emit_load / emit_store / emit_load64 / emit_store64). With a tail, a slow path that must leave compiled code after
// the access (status 2) finishes the instruction itself and leaves, so the hot path needs no RSTOP check.
static void set_tail(Cg* c, int n, int h0, u32 g0, int h1, u32 g1) {
    c->tailOn = true;
    c->tailN = n;
    c->tailHost[0] = h0; c->tailGpr[0] = g0;
    c->tailHost[1] = h1; c->tailGpr[1] = g1;
}

static void stop_tail(Cg* c) {
    if (!c->tailOn || !c->pendingStop) return;
    Emit* e = &c->e;
    dp_imm(e, CMP, 1, 0, RSTOP, 0);
    u32 skip = branch_c(e, C_EQ, 0);
    // Write back the cache as it stands, then the instruction's results straight to memory, and leave.
    rc_emit_flush(c, &c->rc);
    for (int k = 0; k < c->tailN; k++) str_imm(e, c->tailHost[k], RG, OFF_GPR(c->tailGpr[k]));
    Rc clean = c->rc;
    for (int k = 0; k < NSLOT; k++) clean.dirty[k] = false;
    exit_snap(c, C_AL, false, 0, &clean);  // g->pc is already the next instruction
    patch_branch(e, skip, e->n);
    c->pendingStop = false;
}

// Effective address into r0: (rA|0) + d, or rA + rB for indexed forms.
static void ea_disp(Cg* c, u32 ra, u32 d) {
    if (ra) add_const(&c->e, R0, src(c, ra, R0), d, R12);
    else load_const(&c->e, R0, d);
}
static void ea_index(Cg* c, u32 ra, u32 rb) {
    int b = src(c, rb, R1);
    if (ra) dp_reg(&c->e, ADD, 0, R0, src(c, ra, R0), b);
    else mov_reg(&c->e, R0, b);
}

// The inline MEM1 check of the fast paths: r1 = the physical offset of the guest address in r0 (0xC0000000 folded
// onto 0x80000000); branches to the slow path (returned in slow[], NO_BRANCH = none) when the address is not
// `align`-byte aligned or not inside MEM1 (an aligned access of `align` bytes then fits entirely).
static void mem1_check(Cg* c, u32 align, u32 slow[2]) {
    Emit* e = &c->e;
    slow[0] = slow[1] = NO_BRANCH;
    if (align > 1) { dp_imm(e, TST, 1, 0, R0, align - 1); slow[0] = branch_c(e, C_NE, 0); }
    dp_imm(e, BIC, 0, R1, R0, IMM(0x40000000u));
    dp_imm(e, SUB, 0, R1, R1, IMM(0x80000000u));
    dp_imm(e, CMP, 1, 0, R1, IMM(0x01800000u));
    slow[1] = branch_c(e, C_CS, 0);
}

// Starts the out-of-line part of a fast path: the hot branches in slow[] go to the cold code emitted from here on.
// Rule: between a branch into cold code and the cold code itself, hot code makes no register-cache allocation (src /
// dst of a GPR not yet cached): the cold path sees the cache state at its emission, and an eviction it skipped would
// lose a write-back. Instructions therefore take their operands and destination slots before their first check.
static void cold_start(Cg* c, const u32 slow[2]) {
    cold_begin(c);
    for (int k = 0; k < 2; k++) if (slow[k] != NO_BRANCH) fix_to_cold(c, slow[k]);
}

// Every memory access keeps the effective address in r0 on all its paths (update forms write it to rA afterwards):
// the slow paths save it around their helper call.
#define EA_SAVE ((1u << R0) | (1u << R1))  // two registers keep the stack 8-byte aligned

static void load_slow(Cg* c, u32 size, bool sign, int dreg) {
    Emit* e = &c->e;
    // r0 = ea; jit_load(g, ea, size, pc).
    push_regs(e, EA_SAVE);
    mov_reg(e, R1, R0);
    mov_reg(e, R0, RG);
    load_const(e, R2, size);
    load_const(e, R3, c->pc);
    emit_time(c);
    call(c, (const void*)jit_load);
    mov_reg(e, R2, R0);
    pop_regs(e, EA_SAVE);
    slow_status(c);  // a fault leaves before dreg (maybe a cached GPR) changes
    if (sign && size == 2) sxth(e, R2, R2);
    stop_tail(c);
    mov_reg(e, dreg, R2);
}

// Load `size` bytes from the guest address in r0 into dreg (zero- or sign-extended). The fast path covers aligned
// accesses to MEM1 through the standard cached/uncached mappings and falls through; everything else goes out of line
// to jit_load and comes back. With set_tail, the loaded value is r2 on the way out of a stop.
static void emit_load(Cg* c, u32 size, bool sign, int dreg) {
    Emit* e = &c->e;
    if (!c->fastmem) { load_slow(c, size, sign, dreg); return; }
    u32 slow[2];
    mem1_check(c, size, slow);
    if (size == 4) { ldr_reg(e, dreg, RRAM, R1); rev(e, dreg, dreg); }
    else if (size == 2) { ldrh_reg(e, dreg, RRAM, R1); if (sign) revsh(e, dreg, dreg); else rev16(e, dreg, dreg); }
    else ldrb_reg(e, dreg, RRAM, R1);
    u32 resume = e->n;
    cold_start(c, slow);
    load_slow(c, size, sign, dreg);
    cold_end(c, resume);
}

static void store_slow(Cg* c, u32 size, int vreg) {
    Emit* e = &c->e;
    u32 sizePc = c->pc | (size == 4 ? 2u : size == 2 ? 1u : 0u);  // pc is word aligned
    u32 pipeDone = NO_BRANCH;
    mov_reg(e, R2, vreg);
    if (c->fastmem && c->j->gatherPut) {
        // The write-gather pipe (the graphics command stream, written a few bytes at a time): straight to the GP.
        load_const(e, R1, c->j->gatherEa);
        dp_reg(e, CMP, 1, 0, R0, R1);
        u32 notPipe = branch_c(e, C_NE, 0);
        push_regs(e, EA_SAVE);
        mov_reg(e, R0, RG);
        mov_reg(e, R1, R2);
        load_const(e, R2, sizePc);
        emit_time(c);
        call(c, (const void*)jit_gather);
        mov_reg(e, R3, R0);
        pop_regs(e, EA_SAVE);
        dp_imm(e, CMP, 1, 0, R3, 0);
        dp_imm_c(e, C_NE, MOV, 0, RSTOP, 0, 1);  // leave once the instruction is done (as stop_flag_from_status)
        c->pendingStop = true;
        pipeDone = branch_c(e, C_AL, 0);
        patch_branch(e, notPipe, e->n);
    }
    // r0 = ea, r2 = value; jit_store(g, ea, value, pc | log2(size)).
    push_regs(e, EA_SAVE);
    mov_reg(e, R1, R0);
    mov_reg(e, R0, RG);
    load_const(e, R3, sizePc);
    emit_time(c);
    call(c, (const void*)jit_store);
    pop_regs(e, EA_SAVE);
    slow_status(c);
    if (pipeDone != NO_BRANCH) patch_branch(e, pipeDone, e->n);
    stop_tail(c);
}

// Store `size` bytes of vreg to the guest address in r0.
static void emit_store(Cg* c, u32 size, int vreg) {
    Emit* e = &c->e;
    if (!c->fastmem) { store_slow(c, size, vreg); return; }
    u32 slow[2];
    mem1_check(c, size, slow);
    if (size == 4) { rev(e, R3, vreg); str_reg(e, R3, RRAM, R1); }
    else if (size == 2) { rev16(e, R3, vreg); strh_reg(e, R3, RRAM, R1); }
    else strb_reg(e, vreg, RRAM, R1);
    u32 resume = e->n;
    cold_start(c, slow);
    store_slow(c, size, vreg);
    cold_end(c, resume);
}

static void emit_fallback(Cg* c, u32 inst) {
    Emit* e = &c->e;
    rc_flush(c);  // the interpreter works on guest state in memory
    Rc clean = c->rc;
    for (int k = 0; k < NSLOT; k++) clean.dirty[k] = false;
    mov_reg(e, R0, RG);
    load_const(e, R1, c->pc);
    load_const(e, R2, inst);
    emit_time(c);
    call(c, (const void*)jit_interp);
    dp_imm(e, CMP, 1, 0, R0, 0);
    exit_snap(c, C_NE, false, 0, &clean);
    rc_after_interp(c);
}

// ---- branches (end the block) ----
// Taken: to `target` (constant) or, when `indirect`, to the address in r2. Not taken: pc + 4. Uses r0, r3.
static void emit_branch_cond(Cg* c, u32 bo, u32 bi, bool indirect, u32 target) {
    Emit* e = &c->e;
    u32 notTaken[2];
    int nNot = 0;
    if (!(bo & 4)) {
        ldr_imm(e, R3, RG, OFF_CTR);
        dp_imm(e, SUB, 1, R3, R3, 1);
        str_imm(e, R3, RG, OFF_CTR);
        notTaken[nNot++] = branch_c(e, (bo & 2) ? C_NE : C_EQ, 0);  // branch if CTR == 0 (bo&2) / != 0
    }
    if (!(bo & 16)) {
        if ((bo & 4) && c->flagsLive && (bi >> 2) == c->flagsFld && (bi & 3) != 3) {
            // The previous instruction was the compare that set this CR field and left its ARM flags: branch on them.
            static const int kSigned[3] = { C_LT, C_GT, C_EQ }, kUnsigned[3] = { C_CC, C_HI, C_EQ };
            int cond = (c->flagsSigned ? kSigned : kUnsigned)[bi & 3];
            if (jit_debug_plant == 3) cond ^= 1;  // the fuzzer's self-test: a wrong fused branch must be caught
            notTaken[nNot++] = branch_c(e, (bo & 8) ? cond ^ 1 : cond, 0);  // ARM conditions invert by bit 0
            c->j->stats.fusedBranches++;
        } else {
            ldr_imm(e, R0, RG, OFF_CR);
            u32 bit = 1u << (31 - bi), enc;
            if (!arm_imm(bit, &enc)) enc = 0;
            dp_imm(e, TST, 1, 0, R0, enc);
            notTaken[nNot++] = branch_c(e, (bo & 8) ? C_EQ : C_NE, 0);  // want bit set: not taken if clear
        }
    }
    if (indirect) exit_indirect(c); else exit_const(c, target);
    if (!nNot) return;
    for (int i = 0; i < nNot; i++) patch_branch(e, notTaken[i], e->n);
    exit_const(c, c->pc + 4);
}

static bool compile_branch(Cg* c, u32 inst) {
    rc_flush(c);  // every path out of the block leaves guest state in memory (stores keep the ARM flags)
    Emit* e = &c->e;
    u32 op = inst >> 26, lk = inst & 1;
    if (op == 18) {  // b, bl, ba, bla
        s32 li = ((s32)(inst << 6)) >> 6 & ~3;
        u32 target = (inst & 2) ? (u32)li : c->pc + (u32)li;
        if (lk) { load_const(e, R1, c->pc + 4); str_imm(e, R1, RG, OFF_LR); }
        exit_const(c, target);
        return true;
    }
    if (op == 16) {  // bc
        u32 bo = (inst >> 21) & 31, bi = (inst >> 16) & 31;
        s32 bd = (s32)(s16)(inst & 0xFFFC);
        u32 target = (inst & 2) ? (u32)bd : c->pc + (u32)bd;
        if (lk) { load_const(e, R1, c->pc + 4); str_imm(e, R1, RG, OFF_LR); }
        emit_branch_cond(c, bo, bi, false, target);
        return true;
    }
    if (op == 19) {
        u32 xo = (inst >> 1) & 0x3FF, bo = (inst >> 21) & 31, bi = (inst >> 16) & 31;
        if (xo == 16 || xo == 528) {  // bclr / bcctr
            if (xo == 528 && !(bo & 4)) return false;  // invalid form: let the interpreter handle it
            ldr_imm(e, R2, RG, xo == 16 ? OFF_LR : OFF_CTR);   // read before LK overwrites LR
            dp_imm(e, BIC, 0, R2, R2, 3);
            if (lk) { load_const(e, R1, c->pc + 4); str_imm(e, R1, RG, OFF_LR); }
            emit_branch_cond(c, bo, bi, true, 0);
            return true;
        }
        if (xo == 150) {  // isync: nothing to do here, but it ends the block (code may have changed before it)
            exit_const(c, c->pc + 4);
            return true;
        }
    }
    return false;
}

static bool is_branch(u32 inst) {  // ...or isync, which also ends the block with a plain exit to pc + 4
    u32 op = inst >> 26, xo = (inst >> 1) & 0x3FF;
    return op == 18 || op == 16 || (op == 19 && (xo == 16 || xo == 528 || xo == 150));
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
// Operands come from src() and results go to dst() / dst_done(): cached GPRs are used in place, others through the
// given temporaries. A destination may be the same host register as a source; each case reads its sources before it
// writes its destination, or writes it with one ARM instruction.
static void op_imm(Cg* c, int opc, int d, int s, u32 imm) {
    u32 enc;
    if (arm_imm(imm, &enc)) dp_imm(&c->e, opc, 0, d, s, enc);
    else { load_const(&c->e, R12, imm); dp_reg(&c->e, opc, 0, d, s, R12); }
}

// Integer load of `size` bytes from the address in r0 into rD; with update, rA = the address.
static void int_load(Cg* c, u32 size, bool sign, u32 rd, u32 ra, bool update) {
    int d = dst(c, rd, R2);
    set_tail(c, update ? 2 : 1, R2, rd, R0, ra);
    emit_load(c, size, sign, d);
    dst_done(c, rd, d);
    if (update) st_gpr(c, R0, ra);
}

// Integer store of rS's low `size` bytes to the address in r0; with update, rA = the address.
static void int_store(Cg* c, u32 size, u32 rs, u32 ra, bool update) {
    int v = src(c, rs, R2);
    set_tail(c, update ? 1 : 0, R0, ra, 0, 0);
    emit_store(c, size, v);
    if (update) st_gpr(c, R0, ra);
}

static bool compile_int(Cg* c, u32 inst) {
    Emit* e = &c->e;
    u32 op = inst >> 26, rd = (inst >> 21) & 31, ra = (inst >> 16) & 31, rb = (inst >> 11) & 31;
    u32 uimm = inst & 0xFFFF, simm = (u32)(s32)(s16)uimm, rc = inst & 1;
    u32 enc;
    switch (op) {
    case 14: case 15: {  // addi, addis
        u32 v = op == 14 ? simm : uimm << 16;
        int a = ra ? src(c, ra, R0) : -1;
        int d = dst(c, rd, R0);
        if (ra) add_const(e, d, a, v, R12); else load_const(e, d, v);
        dst_done(c, rd, d);
        return true;
    }
    case 12: case 13: {  // addic, addic.
        int a = src(c, ra, R0);
        load_const(e, R1, simm);
        int d = dst(c, rd, R0);
        dp_reg(e, ADD, 1, d, a, R1);
        set_ca_from_c(c);
        dst_done(c, rd, d);
        if (op == 13) update_cr0(c, d);
        return true;
    }
    case 8: {  // subfic
        int a = src(c, ra, R0);
        load_const(e, R1, simm);
        int d = dst(c, rd, R0);
        dp_reg(e, SUB, 1, d, R1, a);  // imm - ra; C = imm >= ra (unsigned) = PowerPC CA
        set_ca_from_c(c);
        dst_done(c, rd, d);
        return true;
    }
    case 7: {  // mulli
        int a = src(c, ra, R0);
        load_const(e, R1, simm);
        int d = dst(c, rd, R2);
        mul(e, d, R1, a);
        dst_done(c, rd, d);
        return true;
    }
    case 10: case 11: {  // cmpli, cmpi
        if (inst & (1u << 21)) return false;  // L = 1 (64-bit) is invalid on a 32-bit core
        int a = src(c, ra, R0);
        u32 imm = op == 11 ? simm : uimm;
        if (arm_imm(imm, &enc)) dp_imm(e, CMP, 1, 0, a, enc);
        else if (arm_imm(0u - imm, &enc)) dp_imm(e, CMN, 1, 0, a, enc);
        else { load_const(e, R1, imm); dp_reg(e, CMP, 1, 0, a, R1); }
        update_crf_from_flags(c, rd >> 2, op == 11);
        return true;
    }
    case 24: case 25: case 26: case 27: case 28: case 29: {  // ori oris xori xoris andi. andis.
        u32 imm = (op & 1) ? uimm << 16 : uimm;
        int opc = op <= 25 ? ORR : op <= 27 ? EOR : AND;
        if (op <= 27 && imm == 0 && rd == ra) return true;  // ori r,r,0 (nop) and friends
        int s = src(c, rd, R0);  // rS
        int d = dst(c, ra, R0);
        op_imm(c, opc, d, s, imm);
        dst_done(c, ra, d);
        if (op >= 28) update_cr0(c, d);
        return true;
    }
    case 21: {  // rlwinm
        u32 sh = rb, mb = (inst >> 6) & 31, me = (inst >> 1) & 31;
        int s = src(c, rd, R0);
        int d = dst(c, ra, R0);
        rotate_mask(c, d, s, sh, ppc_mask(mb, me));
        dst_done(c, ra, d);
        if (rc) update_cr0(c, d);
        return true;
    }
    case 20: {  // rlwimi: ra = (rotl(rs, sh) & m) | (ra & ~m)
        u32 sh = rb, m = ppc_mask((inst >> 6) & 31, (inst >> 1) & 31);
        int s = src(c, rd, R0);
        int a = src(c, ra, R1);
        rotate_mask(c, R2, s, sh, m);
        if (arm_imm(m, &enc)) dp_imm(e, BIC, 0, R3, a, enc);
        else { load_const(e, R12, m); dp_reg(e, BIC, 0, R3, a, R12); }
        int d = dst(c, ra, R0);
        dp_reg(e, ORR, 0, d, R2, R3);
        dst_done(c, ra, d);
        if (rc) update_cr0(c, d);
        return true;
    }
    case 23: {  // rlwnm
        u32 m = ppc_mask((inst >> 6) & 31, (inst >> 1) & 31);
        int s = src(c, rd, R0);
        int b = src(c, rb, R1);
        dp_imm(e, AND, 0, R2, b, 31);
        dp_imm(e, RSB, 0, R2, R2, 32);  // rotate right by 32 - n (32 behaves as 0)
        int d = dst(c, ra, R0);
        dp_rsr(e, MOV, 0, d, 0, s, ROR, R2);
        if (m != 0xFFFFFFFFu) op_imm(c, AND, d, d, m);
        dst_done(c, ra, d);
        if (rc) update_cr0(c, d);
        return true;
    }
    case 32: case 33: case 34: case 35: case 40: case 41: case 42: case 43: {  // lwz lwzu lbz lbzu lhz lhzu lha lhau
        bool update = op & 1;
        if (update && (ra == 0 || ra == rd)) return false;  // invalid forms
        ea_disp(c, ra, simm);
        int_load(c, op <= 33 ? 4 : op <= 35 ? 1 : 2, op >= 42, rd, ra, update);
        return true;
    }
    case 36: case 37: case 38: case 39: case 44: case 45: {  // stw stwu stb stbu sth sthu
        bool update = op & 1;
        if (update && ra == 0) return false;
        ea_disp(c, ra, simm);
        int_store(c, op <= 37 ? 4 : op <= 39 ? 1 : 2, rd, ra, update);
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
    case 40: case 8: case 136: case 104: {  // subf, subfc, subfe, neg
        int a = src(c, ra, R0);
        int b = (xo != 202 && xo != 104) ? src(c, rb, R1) : -1;
        if (xo == 138 || xo == 202 || xo == 136) load_ca_to_c(c);
        int d = dst(c, rd, R0);
        switch (xo) {
        case 266: dp_reg(e, ADD, 0, d, a, b); break;
        case 10: dp_reg(e, ADD, 1, d, a, b); break;
        case 138: dp_reg(e, ADC, 1, d, a, b); break;
        case 202: dp_imm(e, ADC, 1, d, a, 0); break;
        case 40: dp_reg(e, SUB, 0, d, b, a); break;
        case 8: dp_reg(e, SUB, 1, d, b, a); break;
        case 136: dp_reg(e, SBC, 1, d, b, a); break;  // ~ra + rb + CA
        case 104: dp_imm(e, RSB, 0, d, a, 0); break;
        }
        if (xo == 10 || xo == 138 || xo == 202 || xo == 8 || xo == 136) set_ca_from_c(c);
        dst_done(c, rd, d);
        if (rc) update_cr0(c, d);
        return true;
    }
    case 28: case 60: case 444: case 412: case 316: case 124: case 476: case 284: {  // and andc or orc xor nor nand eqv
        int s = src(c, rd, R0);  // rS
        int b = src(c, rb, R1);
        if (xo == 412) { dp_reg(e, MVN, 0, R2, 0, b); b = R2; }  // orc: rs | ~rb
        int d = dst(c, ra, R0);
        switch (xo) {
        case 28: case 476: dp_reg(e, AND, 0, d, s, b); break;
        case 60: dp_reg(e, BIC, 0, d, s, b); break;
        case 444: case 412: case 124: dp_reg(e, ORR, 0, d, s, b); break;
        case 316: case 284: dp_reg(e, EOR, 0, d, s, b); break;
        }
        if (xo == 124 || xo == 476 || xo == 284) dp_reg(e, MVN, 0, d, 0, d);  // nor, nand, eqv
        dst_done(c, ra, d);
        if (rc) update_cr0(c, d);
        return true;
    }
    case 24: case 536: {  // slw, srw: shift amounts 32..63 give 0, as ARM's register shifts do
        int s = src(c, rd, R0);
        int b = src(c, rb, R1);
        dp_imm(e, AND, 0, R2, b, 0x3F);
        int d = dst(c, ra, R0);
        dp_rsr(e, MOV, 0, d, 0, s, xo == 24 ? LSL : LSR, R2);
        dst_done(c, ra, d);
        if (rc) update_cr0(c, d);
        return true;
    }
    case 824: {  // srawi: CA = negative source with 1-bits shifted out
        u32 sh = rb;
        int s = src(c, rd, R0);
        dp_imm(e, MOV, 0, R2, 0, 0);
        if (sh) {
            mov_shift(e, 1, R3, s, LSL, (int)(32 - sh));       // Z = no 1-bits shifted out
            dp_reg_c(e, C_NE, MOV, 0, R2, 0, s, LSR, 31);      // CA = sign when bits were lost
        }
        int d = dst(c, ra, R0);
        if (sh) mov_shift(e, 0, d, s, ASR, (int)sh); else mov_reg(e, d, s);
        ldr_imm(e, R3, RG, OFF_XER);
        dp_imm(e, BIC, 0, R3, R3, IMM(XER_CA));
        dp_reg_c(e, C_AL, ORR, 0, R3, R3, R2, LSL, 29);
        str_imm(e, R3, RG, OFF_XER);
        dst_done(c, ra, d);
        if (rc) update_cr0(c, d);
        return true;
    }
    case 26: case 954: case 922: {  // cntlzw extsb extsh
        int s = src(c, rd, R0);
        int d = dst(c, ra, R0);
        if (xo == 26) clz(e, d, s); else if (xo == 954) sxtb(e, d, s); else sxth(e, d, s);
        dst_done(c, ra, d);
        if (rc) update_cr0(c, d);
        return true;
    }
    case 0: case 32: {  // cmp, cmpl
        if (inst & (1u << 21)) return false;
        int a = src(c, ra, R0);
        int b = src(c, rb, R1);
        dp_reg(e, CMP, 1, 0, a, b);
        update_crf_from_flags(c, rd >> 2, xo == 0);
        return true;
    }
    case 235: case 75: case 11: {  // mullw, mulhw, mulhwu
        int a = src(c, ra, R0);
        int b = src(c, rb, R1);
        if (xo == 235) mul(e, R2, a, b);
        else if (xo == 75) smull(e, R3, R2, a, b);
        else umull(e, R3, R2, a, b);
        int d = dst(c, rd, R2);
        mov_reg(e, d, R2);
        dst_done(c, rd, d);
        if (rc) update_cr0(c, d);
        return true;
    }
    case 23: case 87: case 279: case 343: case 55: case 119: case 311: case 375: {  // lwzx lbzx lhzx lhax (+ux)
        bool update = xo == 55 || xo == 119 || xo == 311 || xo == 375;
        if (update && (ra == 0 || ra == rd)) return false;
        u32 base = update ? xo - 32 : xo;
        ea_index(c, ra, rb);
        int_load(c, base == 23 ? 4 : base == 87 ? 1 : 2, base == 343, rd, ra, update);
        return true;
    }
    case 151: case 215: case 407: case 183: case 247: case 439: {  // stwx stbx sthx (+ux)
        bool update = xo == 183 || xo == 247 || xo == 439;
        if (update && ra == 0) return false;
        u32 base = update ? xo - 32 : xo;
        ea_index(c, ra, rb);
        int_store(c, base == 151 ? 4 : base == 215 ? 1 : 2, rd, ra, update);
        return true;
    }
    case 339: case 467: {  // mfspr / mtspr for LR, CTR, XER and SPRs that are plain storage
        u32 spr = ((inst >> 16) & 31) | ((inst >> 6) & 0x3E0);
        u32 off = spr == 8 ? OFF_LR : spr == 9 ? OFF_CTR : spr == 1 ? OFF_XER : 0;
        int base = RG;
        if (!off) {
            bool plain = spr == SPR_SRR0 || spr == SPR_SRR1 || spr == SPR_DAR || spr == SPR_DSISR ||
                         (spr >= SPR_SPRG0 && spr < SPR_SPRG0 + 4) || (spr >= SPR_GQR0 && spr < SPR_GQR0 + 8);
            if (!plain || ((spr & 0x10) && c->pr)) return false;
            u32 so = (u32)offsetof(Gekko, spr) + 4u * spr;
            add_const(e, R1, RG, so & ~0xFFFu, R12);
            base = R1;
            off = so & 0xFFFu;
        }
        if (xo == 339) {
            int d = dst(c, rd, R0);
            ldr_imm(e, d, base, off);
            dst_done(c, rd, d);
        } else {
            int s = src(c, rd, R0);
            if (spr == 1) { load_const(e, R2, XER_MASK); dp_reg(e, AND, 0, R2, s, R2); s = R2; }
            str_imm(e, s, base, off);
        }
        return true;
    }
    case 19: {  // mfcr
        int d = dst(c, rd, R0);
        ldr_imm(e, d, RG, OFF_CR);
        dst_done(c, rd, d);
        return true;
    }
    case 83: {  // mfmsr
        if (c->pr) return false;
        int d = dst(c, rd, R0);
        ldr_imm(e, d, RG, OFF_MSR);
        dst_done(c, rd, d);
        return true;
    }
    case 146:  // mtmsr: the block goes on unless more than EE changed or an interrupt is now due
        if (c->pr) return false;
        ld_gpr(c, R1, rd);
        mov_reg(e, R0, RG);
        call(c, (const void*)jit_mtmsr);
        dp_imm(e, CMP, 1, 0, R0, 0);
        exit_pc_if(c, C_NE, c->pc + 4);
        return true;
    case 598: case 854: case 278: case 246:  // sync, eieio, dcbt, dcbtst: nothing to emulate
        return true;
    case 54: case 86: case 470: case 1014: {  // dcbst dcbf dcbi dcbz on MEM1 through the standard mappings
        if (!c->fastmem || (xo == 470 && c->pr)) return false;
        ea_index(c, ra, rb);
        dp_imm(e, BIC, 0, R1, R0, IMM(0x40000000u));
        dp_imm(e, SUB, 0, R1, R1, IMM(0x80000000u));
        dp_imm(e, CMP, 1, 0, R1, IMM(0x01800000u));
        u32 slow = branch_c(e, C_CS, 0);
        if (xo == 1014) {  // dcbz: zero the 32-byte line
            u32 zero = (1u << R2) | (1u << R3) | (1u << R12) | (1u << LR);
            dp_imm(e, BIC, 0, R1, R1, 31);
            dp_reg(e, ADD, 0, R1, R1, RRAM);
            dp_imm(e, MOV, 0, R2, 0, 0);
            dp_imm(e, MOV, 0, R3, 0, 0);
            dp_imm(e, MOV, 0, R12, 0, 0);
            dp_imm(e, MOV, 0, LR, 0, 0);
            stmia(e, R1, zero);
            dp_imm(e, ADD, 0, R1, R1, 16);
            stmia(e, R1, zero);
        }
        u32 resume = e->n;
        cold_begin(c);
        fix_to_cold(c, slow);
        emit_fallback(c, inst);  // other addresses: the interpreter translates (and may raise a DSI)
        cold_end(c, resume);
        return true;
    }
    case 144: {  // mtcrf
        u32 crm = (inst >> 12) & 0xFF, m = 0;
        for (int i = 0; i < 8; i++) if (crm & (0x80u >> i)) m |= 0xF0000000u >> (4 * i);
        int s = src(c, rd, R0);
        ldr_imm(e, R1, RG, OFF_CR);
        load_const(e, R2, m);
        dp_reg(e, AND, 0, R3, s, R2);
        dp_reg(e, BIC, 0, R1, R1, R2);
        dp_reg(e, ORR, 0, R1, R1, R3);
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

static void load64_slow(Cg* c, u32 frd) {
    Emit* e = &c->e;
    push_regs(e, EA_SAVE);
    mov_reg(e, R1, R0);
    mov_reg(e, R0, RG);
    load_const(e, R2, c->pc);
    load_const(e, R3, frd);
    emit_time(c);
    call(c, (const void*)jit_load64);
    pop_regs(e, EA_SAVE);
    slow_status(c);
    stop_tail(c);
}

// lfd: the 8-byte value at the guest address in r0 into FPR frd. The fast path needs 8-byte alignment inside MEM1.
static void emit_load64(Cg* c, u32 frd) {
    Emit* e = &c->e;
    if (!c->fastmem) { load64_slow(c, frd); return; }
    u32 slow[2];
    mem1_check(c, 8, slow);
    dp_reg(e, ADD, 0, R1, R1, RRAM);
    ldmia(e, R1, (1u << R2) | (1u << R3));
    rev(e, R2, R2);
    rev(e, R3, R3);
    str_imm(e, R3, RG, OFF_FPR(frd));
    str_imm(e, R2, RG, OFF_FPR(frd) + 4);
    u32 resume = e->n;
    cold_start(c, slow);
    load64_slow(c, frd);
    cold_end(c, resume);
}

static void store64_slow(Cg* c, u32 frs) {
    Emit* e = &c->e;
    push_regs(e, EA_SAVE);
    mov_reg(e, R1, R0);
    mov_reg(e, R0, RG);
    load_const(e, R2, c->pc);
    load_const(e, R3, frs);
    emit_time(c);
    call(c, (const void*)jit_store64);
    pop_regs(e, EA_SAVE);
    slow_status(c);
    stop_tail(c);
}

// stfd: FPR frs (ps0) to the guest address in r0.
static void emit_store64(Cg* c, u32 frs) {
    Emit* e = &c->e;
    if (!c->fastmem) { store64_slow(c, frs); return; }
    u32 slow[2];
    mem1_check(c, 8, slow);
    ldr_imm(e, R2, RG, OFF_FPR(frs) + 4);
    ldr_imm(e, R3, RG, OFF_FPR(frs));
    rev(e, R2, R2);
    rev(e, R3, R3);
    dp_reg(e, ADD, 0, R1, R1, RRAM);
    stmia(e, R1, (1u << R2) | (1u << R3));
    u32 resume = e->n;
    cold_start(c, slow);
    store64_slow(c, frs);
    cold_end(c, resume);
}

// lfs lfd stfs stfd and their update / indexed forms. kind: 0 lfs, 1 lfd, 2 stfs, 3 stfd.
static bool float_mem(Cg* c, u32 kind, u32 fr, u32 ra, u32 rb, u32 simm, bool indexed, bool update) {
    Emit* e = &c->e;
    if (update && ra == 0) return false;  // invalid form
    if (kind == 2) dbl_to_single_bits(c, R2, fr, 0, R12, LR);
    if (indexed) ea_index(c, ra, rb); else ea_disp(c, ra, simm);
    switch (kind) {
    case 0:
        emit_load(c, 4, false, R2);
        vmov_s_r(e, 0, R2);
        vcvt_f64_f32(e, 1, 0);
        st_fpr(c, 1, fr, 0);
        st_fpr(c, 1, fr, 1);
        break;
    case 1: set_tail(c, update ? 1 : 0, R0, ra, 0, 0); emit_load64(c, fr); break;
    case 2: set_tail(c, update ? 1 : 0, R0, ra, 0, 0); emit_store(c, 4, R2); break;
    default: set_tail(c, update ? 1 : 0, R0, ra, 0, 0); emit_store64(c, fr); break;
    }
    if (update) st_gpr(c, R0, ra);  // the access kept the effective address in r0
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
    // The address first: the register cache must not change after the first branch to the cold fallback (that path
    // would skip the write-back of an evicted register).
    if (indexed) ea_index(c, ra, rb); else ea_disp(c, ra, (u32)((s32)(inst << 20) >> 20));
    add_const(e, R1, RG, gqr & ~0xFFFu, R12);
    ldr_imm(e, R2, R1, gqr & 0xFFFu);
    dp_imm(e, AND, 1, R1, R2, IMM(store ? 0x3F00u : 0x3F000000u));        // scale
    dp_imm_c(e, C_EQ, AND, 1, R1, R2, IMM(store ? 0x7u : 0x70000u));      // type
    fb[nfb++] = branch_c(e, C_NE, 0);
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
        dbl_to_single_bits(c, R2, fr, 0, R12, LR);
        rev(e, R2, R2);
        if (w) {
            str_imm(e, R2, R1, 0);
        } else {
            dbl_to_single_bits(c, R3, fr, 1, R12, LR);
            rev(e, R3, R3);
            stmia(e, R1, (1u << R2) | (1u << R3));
        }
    }
    if (update) st_gpr(c, R0, ra);  // the access kept the effective address in r0
    u32 resume = e->n;
    cold_begin(c);  // other quantizations, unaligned or outside MEM1: the interpreter, out of line
    for (u32 k = 0; k < nfb; k++) fix_to_cold(c, fb[k]);
    emit_fallback(c, inst);
    cold_end(c, resume);
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

// Compiles the block starting at guest PC `pc`; NULL when the code is not in RAM or the cache is full. `noLink`: the
// block is a polling loop, so none of its exits is ever linked (the dispatcher must see every round to skip it).
static BlockFn compile_block(Jit* j, u32 pc, bool noLink) {
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
    c->alt = (Emit){ g_coldBuf, 0, BLOCK_MAX_WORDS, false };
    c->flagsAt = NO_BRANCH;
    for (int k = 0; k < NSLOT; k++) c->rc.id[k] = -1;
    c->j = j;
    c->noLink = noLink;
    c->fastmem = j->fastmem && (g->msr & MSR_DR);
    c->fp = (g->msr & MSR_FP) && !(g->fpscr & FPSCR_RN);
    c->ps = c->fp && (g->spr[SPR_HID2] & HID2_PSE);
    c->lsq = (g->spr[SPR_HID2] & HID2_LSQE) != 0;
    c->pr = (g->msr & MSR_PR) != 0;
    Emit* e = &c->e;
    // Budget check: a chain of linked blocks stops here once RDC is used up.
    dp_imm(e, CMP, 1, 0, RDC, 0);
    u32 noBudget = branch_c(e, C_LE, 0);
    u32 startPa = pa;
    bool ended = false;
    u32 maxInsns = jit_debug_max_insns ? jit_debug_max_insns : MAX_BLOCK_INSNS;
    for (u32 i = 0; i < maxInsns && !ended; i++) {
        c->pc = pc + 4 * i;
        u32 ipa = startPa + 4 * i;
        if ((((c->pc ^ pc) >> 12) != 0 && i > 0) || ipa + 4 > j->ramSize) break;  // one 4 KB page, inside RAM
        u32 inst = ((u32)j->ram[ipa] << 24) | ((u32)j->ram[ipa + 1] << 16) | ((u32)j->ram[ipa + 2] << 8) | j->ram[ipa + 3];
        c->count = i + 1;
        // The ARM flags the previous instruction left: every path through an instruction's code reaches its end, so
        // flags its last emitted instruction set are there on all of them.
        c->flagsLive = i > 0 && c->flagsAt == e->n;
        c->pinned = 0;
        c->tailOn = false;
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
            // The interpreter ran it and went on to pc + 4 (other targets left through emit_fallback's exit). Never
            // linked: these instructions change state that compiled code depends on (MSR, HID2, FPSCR[RN], BATs).
            exit_pc_if(c, C_AL, c->pc + 4);
            ended = true;
        }
    }
    if (!ended) { rc_flush(c); exit_const(c, pc + 4 * c->count); }  // ran out of instructions (or a page end)
    // Exit stubs: subtract what the block executed from RDC and leave to the dispatcher.
    for (u32 k = 0; k < c->nExits; k++) {
        if (c->exits[k].inCold) add_fix(c, true, c->exits[k].at, false, e->n);
        else patch_branch(e, c->exits[k].at, e->n);
        rc_emit_flush(c, &c->exits[k].snap);
        if (c->exits[k].setPc) { load_const(e, R1, c->exits[k].pc); str_imm(e, R1, RG, OFF_PC); }
        dp_imm(e, SUB, 0, RDC, RDC, c->exits[k].count);
        branch_abs(e, C_AL, false, stub(c, ST_DYN));
    }
    patch_branch(e, noBudget, e->n);
    branch_abs(e, C_AL, true, stub(c, ST_BUDGET));
    word(e, pc);
    // The cold code goes last; then every branch between hot and cold gets its offset.
    u32 base = e->n;
    if (c->alt.overflow || base + c->alt.n > e->cap) e->overflow = true;
    if (e->overflow) return NULL;
    memcpy(e->buf + base, c->alt.buf, 4 * c->alt.n);
    e->n += c->alt.n;
    for (u32 k = 0; k < c->nFix; k++)
        patch_branch(e, c->fix[k].fromCold ? base + c->fix[k].at : c->fix[k].at,
                     c->fix[k].toCold ? base + c->fix[k].target : c->fix[k].target);
    void* fn = j->code + j->codeUsed;
    jit_debug_last_code = fn;
    jit_debug_last_words = e->n;
    if (jit_debug_on_compile) jit_debug_on_compile(fn, e->n);
    jit_host_sync_icache(fn, e->n * 4);
    j->codeUsed += e->n;
    for (u32 a = startPa >> 12; a <= (startPa + 4 * MAX_BLOCK_INSNS) >> 12 && a < sizeof j->codePages; a++) j->codePages[a] = 1;
    j->stats.blocksCompiled++;
    j->insnsSinceFlush += c->count;
    return (BlockFn)fn;
}

static BlockFn compile(Jit* j, u32 pc, bool noLink) {
    u64 t0 = prof_begin();
    BlockFn fn = compile_block(j, pc, noLink);
    prof_end(PROF_JITC, t0);
    return fn;
}

// The shared stubs, at the start of the code buffer. Compiled code keeps RG, RRAM, RDC and RSTOP in registers
// from ST_ENTER until it leaves through ST_EXIT, which returns r0 to the dispatcher (a link site or 0) and stores
// the budget left in *left.
//   ST_ENTER (g, fn, budget, left): saves the C registers, sets up the block registers and jumps to fn.
//   ST_EXIT: r0 = link site or 0.
//   ST_LINK: reached with "bl" from a link stub whose next word is the target guest PC; returns the site to patch
//     (the branch two words before that literal) after writing g->pc.
//   ST_DYN: g->pc is already written; returns 0.
//   ST_BUDGET: as ST_LINK's entry (literal = the block's own PC), but returns 0.
//   ST_INDIRECT: r2 = target (g->pc already written): jumps to it through the table, or returns 0.
static void emit_stubs(Jit* j) {
    Emit em = { j->code, 0, ST_WORDS, false };
    Emit* e = &em;
    j->stub[ST_ENTER] = e->n;
    push_regs(e, 0x4FF8);  // {r3-r11, lr}: 10 registers keep the stack 8-byte aligned for C calls
    mov_reg(e, RG, R0);
    load_const(e, RRAM, (u32)(uintptr_t)j->ram);
    dp_imm(e, MOV, 0, RSTOP, 0, 0);
    mov_reg(e, RDC, R2);
    bx_reg_c(e, C_AL, R1);
    j->stub[ST_EXIT] = e->n;
    ldr_imm(e, R3, SP, 0);  // the saved r3: `left`
    str_imm(e, RDC, R3, 0);
    pop_regs(e, 0x8FF8);    // {r3-r11, pc}
    j->stub[ST_LINK] = e->n;
    ldr_imm(e, R1, LR, 0);
    str_imm(e, R1, RG, OFF_PC);
    dp_imm(e, SUB, 0, R0, LR, 8);
    branch_c(e, C_AL, j->stub[ST_EXIT]);
    j->stub[ST_DYN] = e->n;
    dp_imm(e, MOV, 0, R0, 0, 0);
    branch_c(e, C_AL, j->stub[ST_EXIT]);
    j->stub[ST_BUDGET] = e->n;
    ldr_imm(e, R1, LR, 0);
    str_imm(e, R1, RG, OFF_PC);
    dp_imm(e, MOV, 0, R0, 0, 0);
    branch_c(e, C_AL, j->stub[ST_EXIT]);
    j->stub[ST_INDIRECT] = e->n;
    mov_shift(e, 0, R1, R2, LSL, 21);  // (pc >> 2) & 511
    mov_shift(e, 0, R1, R1, LSR, 23);
    load_const(e, R12, (u32)(uintptr_t)j->fast);
    dp_reg_c(e, C_AL, ADD, 0, R12, R12, R1, LSL, 3);
    ldmia(e, R12, (1u << R0) | (1u << R1));
    dp_reg(e, CMP, 1, 0, R0, R2);
    bx_reg_c(e, C_EQ, R1);
    dp_imm(e, MOV, 0, R0, 0, 0);
    branch_c(e, C_AL, j->stub[ST_EXIT]);
    j->stubWords = e->n;
    jit_host_sync_icache(j->code, e->n * 4);
}
_Static_assert(FAST_SIZE == 512, "ST_INDIRECT hashes with 9 bits");

typedef u32 (*EnterFn)(Gekko* g, void* fn, s32 budget, s32* left);

// Runs compiled code from block `fn` with a budget of guest instructions; returns how many ran and the link site the
// code left through (or NULL).
static u32 run(Jit* j, BlockFn fn, s32 budget, u64 start, u32** site) {
    s32 left = budget;
    g_runStart = start;
    g_runBudget = budget;
    g_jitLeft = budget;
    *site = (u32*)(uintptr_t)((EnterFn)(void*)(j->code + j->stub[ST_ENTER]))(j->g, (void*)fn, budget, &left);
    return (u32)(budget - left);
}

// Patches the link site to jump straight to `fn`.
static void link_site(Jit* j, u32* site, BlockFn fn) {
    s32 off = (s32)((const u32*)(void*)fn - (site + 2));
    *site = 0xEA000000u | ((u32)off & 0xFFFFFF);
    jit_host_sync_icache(site, 4);
    j->stats.links++;
}

// ---- dispatcher ----
static u32 bucket_of(u32 pc, u32 msrBits) { return ((pc >> 2) ^ (pc >> 18) ^ (msrBits << 9)) & (DISPATCH_SIZE - 1); }

// Finds the block for (pc, msrBits), compiling it on first use; NULL when it cannot be compiled.
static Entry* lookup(Jit* j, u32 pc, u32 msrBits) {
    u32* head = &j->buckets[bucket_of(pc, msrBits)];
    for (u32 i = *head; i != NO_ENTRY; i = j->table[i].next)
        if (j->table[i].pc == pc && j->table[i].msrBits == msrBits) return &j->table[i];
    if (j->nEntries == MAX_ENTRIES) { jit_invalidate_all(j); j->stats.flushes++; }
    Gekko* g = j->g;
    u32 poll = gekko_poll_loop_len(g, pc);
    BlockFn fn = compile(j, pc, poll != 0);
    if (!fn) return NULL;
    // compile() may have flushed the whole cache (code space full), which also emptied the chains.
    head = &j->buckets[bucket_of(pc, msrBits)];
    u32 pa, idx = j->nEntries++;
    Entry* en = &j->table[idx];
    en->pc = pc; en->msrBits = msrBits; en->fn = fn;
    en->idle = gk_translate(g, pc, false, false, &pa, false) && pa + 4 <= j->ramSize &&
               ((u32)j->ram[pa] << 24 | (u32)j->ram[pa + 1] << 16 | (u32)j->ram[pa + 2] << 8 | j->ram[pa + 3]) == 0x48000000u;
    en->poll = (u8)poll;
    en->next = *head;
    *head = idx;
    return en;
}

static void fast_clear(Jit* j) {
    if (!j->fast) return;
    for (u32 i = 0; i < FAST_SIZE; i++) { j->fast[i].pc = 1; j->fast[i].fn = NULL; }  // 1: never a guest PC
}

static bool update_fastmem(Jit* j) {
    // The inline fast path assumes the standard mappings (0x80000000 and 0xC0000000 both at physical 0). Blocks
    // compiled under other mappings use the slow path for every access. (Blocks compiled before a BAT change keep
    // their choice; games set BATs once at boot.)
    Gekko* g = j->g;
    u32 a, b;
    j->fastmem = gk_translate(g, 0x80000000u, false, true, &a, false) && a == 0 &&
                 gk_translate(g, 0xC0000000u, false, true, &b, false) && b == 0 && j->ramSize >= 0x01800000u;
    return j->fastmem;
}

static u32 jit_entry(Gekko* g, s64 budget, void* ctx) {
    Jit* j = ctx;
    u32 total = 0;
    update_fastmem(j);
    u32* site = NULL;  // the link site the last run left through, patched once its target is known
    u32 siteGen = 0, siteKey = 0;
    while ((s64)total < budget) {
        u32 pc = g->pc, key = block_key(g);
        Entry* en = lookup(j, pc, key);
        if (!en) break;
        bool plain = !en->idle && !en->poll;  // idle and polling-loop blocks are only ever entered from here
        if (site) {
            if (plain && siteGen == j->gen && siteKey == key) link_site(j, site, en->fn);
            site = NULL;
        }
        // Idle loop ("b ." with interrupts on): nothing can change until the next event, so use up the budget.
        if (en->idle && (g->msr & MSR_EE)) {
            // Back to gekko_run without skipping, which takes the interrupt that may now be pending.
            if (gekko_idle_hook(g)) { GKP(g)->brk = true; total += 1; break; }
            total += (u32)(budget - (s64)total > 0 ? budget - (s64)total : 1);
            break;
        }
        // Compiled code only ever runs blocks of one key at a time: the indirect table holds that key's blocks.
        if (j->fastKey != key) { fast_clear(j); j->fastKey = key; }
        if (plain && j->linking) {
            FastEntry* f = &j->fast[(pc >> 2) & (FAST_SIZE - 1)];
            f->pc = pc;
            f->fn = (void*)en->fn;
        }
        s64 room = budget - (s64)total;
        u32 gen = j->gen, *s;
        u32 n = run(j, en->fn, room > 0x3FFFFFFF ? 0x3FFFFFFF : (s32)room, g->cycles + total, &s);
        j->stats.blocksRun++;
        if (!n) break;  // cannot happen for a well-formed block; never spin
        total += n;
        j->stats.instrsRun += n;
        if (s && j->linking && gen == j->gen) { site = s; siteGen = gen; siteKey = key; }
        // Polling loop (gekko_poll_loop_len) that went round again: nothing changes until the next event either.
        if (en->poll && g->pc == pc && (g->msr & MSR_EE) && gekko_poll_loop_ram(g, pc, en->poll, j->ramSize)) {
            if (gekko_idle_hook(g)) { GKP(g)->brk = true; break; }
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
    j->fast = malloc(FAST_SIZE * sizeof(FastEntry));
    if (!j->code || !j->table || !j->buckets || !j->fast) { free(j->table); free(j->buckets); free(j->fast); free(j); return NULL; }
    j->linking = true;
    emit_stubs(j);
    jit_invalidate_all(j);
    g_jit = j;
    return j;
}

void jit_destroy(Jit* j) {
    if (!j) return;
    gekko_set_jit(j->g, NULL, NULL);
    free(j->table);
    free(j->buckets);
    free(j->fast);
    free(j);  // the executable region is kept for the process lifetime
}

void jit_attach(Jit* j) { gekko_set_jit(j->g, jit_entry, j); }

void jit_set_linking(Jit* j, bool on) {
    if (j->linking == on) return;
    j->linking = on;
    jit_invalidate_all(j);  // drops every patched link
}

bool jit_linking(const Jit* j) { return j->linking; }

void jit_set_gather(Jit* j, void (*put)(void* ctx, u32 v, u32 size), void* ctx, u32 ea) {
    j->gatherPut = put;
    j->gatherCtx = ctx;
    j->gatherEa = ea;
    jit_invalidate_all(j);
}

u32 jit_run_one_block(Jit* j) {
    Gekko* g = j->g;
    GKP(g)->brk = false;
    u32 msrBits = block_key(g);
    update_fastmem(j);
    Entry* en = lookup(j, g->pc, msrBits);
    if (!en) return 0;
    if (j->fastKey != msrBits) { fast_clear(j); j->fastKey = msrBits; }
    u32* site;
    return run(j, en->fn, 0x3FFFFFFF, g->cycles, &site);
}

void jit_invalidate_all(Jit* j) {
    memset(j->buckets, 0xFF, DISPATCH_SIZE * sizeof(u32));
    j->nEntries = 0;
    memset(j->codePages, 0, sizeof j->codePages);
    j->codeUsed = j->stubWords;
    j->insnsSinceFlush = 0;
    fast_clear(j);
    j->gen++;
}

void jit_invalidate(Jit* j, u32 pa) {
    // Level 1 keeps no per-page block lists: any icbi that touches a page holding compiled code drops everything.
    j->stats.icbis++;
    if ((pa >> 12) < sizeof j->codePages && j->codePages[pa >> 12]) { j->stats.icbiFlushes++; jit_invalidate_all(j); }
}

const JitStats* jit_stats(const Jit* j) { return &j->stats; }

float jit_code_used(const Jit* j) { return j->codeWords ? (float)j->codeUsed / (float)j->codeWords : 0.0f; }

float jit_words_per_insn(const Jit* j) {
    return j->insnsSinceFlush ? (float)(j->codeUsed - j->stubWords) / (float)j->insnsSinceFlush : 0.0f;
}

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
    snprintf(line, sizeof line, "JIT: %llu blocks compiled, %llu dispatches, %llu links, %llu/%llu instructions native (%.1f%%), %llu flushes",
             (unsigned long long)s->blocksCompiled, (unsigned long long)s->blocksRun, (unsigned long long)s->links,
             (unsigned long long)s->instrsNative,
             (unsigned long long)s->instrsCompiled, s->instrsCompiled ? 100.0 * (double)s->instrsNative / (double)s->instrsCompiled : 0.0,
             (unsigned long long)s->flushes);
    print(line);
    snprintf(line, sizeof line, "JIT: %.1f host words per guest instruction compiled (%u KB of code); %llu fused compare-branches, %llu cached GPR reads, %llu pipe stores",
             s->instrsCompiled ? (double)(j->codeUsed - j->stubWords) / (double)s->instrsCompiled : 0.0,
             (j->codeUsed * 4) >> 10, (unsigned long long)s->fusedBranches, (unsigned long long)s->forwards,
             (unsigned long long)s->gatherStores);
    print(line);
    snprintf(line, sizeof line, "JIT: %llu instructions run, %llu interpreted (%.1f%%)", (unsigned long long)s->instrsRun,
             (unsigned long long)s->fallbacksRun, s->instrsRun ? 100.0 * (double)s->fallbacksRun / (double)s->instrsRun : 0.0);
    print(line);
    print_top(&s->fallback, "compiled-but-interpreted (static)", print);
    print_top(&s->fallbackRun, "interpreted at run time (dynamic)", print);
}
#endif
