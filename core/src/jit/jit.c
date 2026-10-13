// Gekko -> ARMv6K JIT, level 1 (see jit.h). Compiled only for 32-bit ARM hosts.
#if defined(__arm__)
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include "../cpu/gekko_internal.h"
#include "../cpu/hle.h"
#include "arm_emit.h"
#include "jit.h"

#define MAX_BLOCK_INSNS 64  // a trace (blocks follow unconditional branches and fall through conditional ones) is up to this long
#define DISPATCH_BITS 16
#define DISPATCH_SIZE (1u << DISPATCH_BITS)
#define BLOCK_MAX_WORDS 4096  // worst-case host words for one block (checked against remaining space)

// Host register roles inside compiled code (set once by the entry stub; blocks jump to each other without saving them).
#define RG R11   // Gekko*
#define RRAM R10 // host address of guest 0 through the cached mapping: ram - 0x80000000, so [RRAM, ea] is MEM1's byte ea
#define RDC R9   // guest instructions left in this run (each exit subtracts what its block executed)

// Blocks are linked: an exit to a known guest address starts as a branch to a stub that returns to the dispatcher,
// which compiles the target and then patches that branch to jump straight to it. Exits to an address in a register
// (blr, bctr) look the address up in a small direct-mapped table first. Every block starts with a budget check, so a
// chain of linked blocks stops when RDC runs out.
#ifndef FAST_BITS
#define FAST_BITS 16
#endif
#define FAST_SIZE (1u << FAST_BITS)
#define FAST_TABLES 4  // one table per block key seen recently (switching tables instead of clearing one)
typedef struct { u32 pc; void* fn; } FastEntry;
// The shared stubs at the start of the code buffer (word offsets), kept across flushes.
enum { ST_ENTER, ST_EXIT, ST_LINK, ST_DYN, ST_BUDGET, ST_INDIRECT, ST_INTERP, ST_LOADSLOW, ST_STORESLOW, ST_STOREPIPE, ST_N,
       ST_WORDS = 2048 };
enum { EXIT_DISPATCH, EXIT_LINK, EXIT_INDIRECT };
// The shared memory-access stubs (emit_mem_stubs): kinds load 32, 16 unsigned, 16 signed, 8; store 32, 16, 8, by the
// register that holds the loaded value or the value to store (r2 or a cached-GPR slot, r4-r8).
enum { MS_L32, MS_L16U, MS_L16S, MS_L8, MS_S32, MS_S16, MS_S8, MS_KINDS };
#define MS_REGS 6
static int ms_reg(int r) { return r == R2 ? 0 : r >= R4 && r <= R8 ? r - R4 + 1 : -1; }

// Offsets into Gekko (ldr/str immediate range is 0..4095; everything used here is below that).
#define OFF_GPR(r) ((u32)offsetof(Gekko, gpr) + 4u * (u32)(r))
#define OFF_CR ((u32)offsetof(Gekko, cr))
#define OFF_XER ((u32)offsetof(Gekko, xer))
#define OFF_LR ((u32)offsetof(Gekko, lr))
#define OFF_CTR ((u32)offsetof(Gekko, ctr))
#define OFF_PC ((u32)offsetof(Gekko, pc))
#define OFF_MSR ((u32)offsetof(Gekko, msr))
#define OFF_HOSTPC ((u32)offsetof(Gekko, hostPc))
#define OFF_FASTBASE ((u32)offsetof(Gekko, fastBase))
#define OFF_JITLEFT ((u32)offsetof(Gekko, jitLeft))
#define OFF_JITSTATUS ((u32)offsetof(Gekko, jitStatus))
#define OFF_JITFN(k) ((u32)offsetof(Gekko, jitFn) + 4u * (u32)(k))
_Static_assert(offsetof(Gekko, pc) < 4096, "Gekko.pc reachable with ldr immediate");
// FPR r: ps0 at +0 and ps1 at +8, each a host double (low word first); vldr/vstr reach 1020 bytes.
#define OFF_FPR(r) ((u32)offsetof(Gekko, fpr) + 16u * (u32)(r))
#define OFF_FPSCR ((u32)offsetof(Gekko, fpscr))
_Static_assert(offsetof(Gekko, fpr) + 16 * 31 + 8 + 4 < 1020, "FPRs reachable with vldr/vstr");

typedef u32 (*BlockFn)(Gekko* g);
// Every compiled block stays reachable until the code cache is flushed: entries live in a pool, chained per hash
// bucket (a direct-mapped table evicted and recompiled colliding blocks, e.g. a real-mode exception handler at
// 0x00000500 and code at 0x80000500, hundreds of times per second).
// poll: polling-loop length or 0; count: the guest instructions the block's entry takes off the budget
typedef struct { u32 pc; u32 msrBits; BlockFn fn; bool idle; u8 poll, count; u32 next; } Entry;
#define JIT_PROLOGUE 2  // words of each block's budget check (compile_block); the dispatcher enters after them
#define MAX_ENTRIES (1u << 16)
#define NO_ENTRY 0xFFFFFFFFu

struct Jit {
    Gekko* g;
    u8* ram;
    u32 ramSize;
    u32* code;
    u32 codeWords, codeUsed;    // the hot region: stubs, then each block's hot path and exit stubs, up to coldStart
    u32 coldStart, coldUsed;    // the cold region (to codeWords): each block's slow paths, kept apart so that hot code
                                // packs into fewer cache lines and pages
    Entry* table;          // pool of MAX_ENTRIES
    u32* buckets;          // DISPATCH_SIZE chain heads
    u32 nEntries;
    bool fastmem;          // standard BATs: 0x80000000 and 0xC0000000 both map physical 0
    bool fastmemUser;      // ... in problem state (MSR[PR]) too
    // Fault mode (jit_set_fault_mem): loads and stores reach MEM1 inline with no checks ([RRAM, ea]); any other
    // address lands on host memory that is not mapped, the access faults, and the host's fault handler calls jit_fault,
    // which patches the site into a branch to its slow path and resumes there. faultKey / faultVal: an open-addressed
    // table from a site's word index in `code` to its slow path's (empty: ~0), cleared with the code.
    bool faultMem;
    u32 *faultKey, *faultVal, faultN;
    u8 codePages[(32u << 20) >> 12];  // physical 4 KB pages that hold compiled code (for icbi invalidation)
    u8* compiledPc;             // one bit per guest word of MEM1: a block started there since the last flush (stats)
    JitStats stats;
    u32 stub[ST_N];             // word offsets of the shared stubs in `code`
    u32 stubWords;              // code words they use (compiled blocks start after them)
    u16 memStub[MS_KINDS][MS_REGS];  // word offsets of the memory-access stubs
    u16 memStub2[MS_KINDS][MS_REGS];  // ... the scheduled set (SCHED_STUBS)
    u32 sched;                  // jit_set_sched: SCHED_STUBS | SCHED_CONST
    u16 psqStub[2][8][2];       // emit_float_stubs: psq_l [0] / psq_st [1] with a plain-float GQR, by GQR and W
    u16 stfsStub, lfsStub, lfdStub, stfdStub;  // emit_float_stubs
    bool memStubs;              // jit_set_mem_stubs: loads and stores call them instead of checking inline
    bool traces;                // jit_set_traces: blocks follow branches (trace_branch) instead of ending at each one
    bool traceFollow;           // jit_set_trace_parts: follow unconditional b
    u32 traceSide;              // jit_set_trace_parts: which side exits (TRACE_SIDE_*)
    u32 traceMax;               // jit_set_trace_parts: block length limit (0: MAX_BLOCK_INSNS)
    FastEntry* fast;            // FAST_TABLES indirect-branch tables, table i for blocks of key fastKeys[i]
    u32 fastKeys[FAST_TABLES], fastNext;
    bool fastUsed[FAST_TABLES];
    u32 gen;                    // bumped by every flush: a pending link from before it is dropped
    bool linking;
    u32 insnsSinceFlush;        // guest instructions compiled into the code now in the buffer
    void (*gatherPut)(void* ctx, u32 v, u32 size);  // jit_set_gather
    void* gatherCtx;
    u32 gatherEa;
    u8* gatherBuf;              // jit_set_gather_buffer: the pipe's partial burst and its length, appended to inline
    u32* gatherLen;
    bool slowInline;            // jit_set_slow_inline: the old per-site slow paths instead of the shared stubs (A/B)
    bool soExact;               // jit_set_so_exact: CR updates include XER[SO] (set once XER[SO] has been seen set)
    bool profile;               // jit_set_profile: blocks store their start in Gekko.hostPc
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
#define g_jitLeft (g_jit->g->jitLeft)  // in the Gekko: compiled code reaches it RG-relative in one instruction
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
#define g_status (g_jit->g->jitStatus)

static u32 jit_load_at(Gekko* g, u32 ea, u32 size, u32 pc) {
    g_jit->stats.slowLoads++;
    if (ea - 0xC0000000u < 0x01800000u) g_jit->stats.slowMirror++;
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
    if (ea - 0xC0000000u < 0x01800000u) g_jit->stats.slowMirror++;
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

// A block that is a native stand-in (hle.h): runs it from `pc` and returns the guest instructions it stood in for, with
// g->pc where guest code goes on. If it cannot start, the one instruction there is interpreted instead. `pc` comes from
// the block: linked blocks jump to each other without writing g->pc, which is stale here (2DS, 9d7c239: the stand-in
// saw a stale PC, declined, and the instruction at that PC was interpreted, which froze Twilight Princess).
static u32 jit_hle(Gekko* g, u32 pc, s32 budget) {
    g->pc = pc;
    u32 n = hle_run(g, budget > 0 ? (u32)budget : 1u);
    if (n) return n;
    u32 pa;
    bool stop;
    if (!gk_translate(g, g->pc, false, false, &pa, false) || pa + 4 > g_jit->ramSize) return 0;
    const u8* w = g_jit->ram + pa;
    g->pc = gekko_interp_one(g, g->pc, ((u32)w[0] << 24) | ((u32)w[1] << 16) | ((u32)w[2] << 8) | w[3], &stop);
    return 1;
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
    u32 xo = (inst >> 1) & 0x3FF;
    if ((inst >> 26) == 31 && (xo == 339 || xo == 467))
        g_jit->stats.sprRun[xo == 467][((inst >> 16) & 31) | ((inst >> 6) & 0x3E0)]++;
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
    // XER[SO] newly set (an overflow-form instruction, mtxer): leave, so the dispatcher switches to exact CR updates
    // before any more code compiled without SO runs.
    if (!g_jit->soExact && (g->xer & XER_SO)) { g->pc = npc; return 1; }
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

// Register cache: guest GPRs, CR and XER live in host registers r4-r8 (callee-saved, so C helpers keep them) from
// their first use in a block, and are written back (when changed: `dirty`) at every exit and before the interpreter
// runs an instruction. Instructions compute straight into and from these registers, which removes most of the loads
// and stores of guest state from compiled code.
#define NSLOT 5
static const int kSlot[NSLOT] = { R4, R5, R6, R7, R8 };
enum { ID_CR = 32, ID_XER = 33 };  // cache ids beyond the GPRs
typedef struct { s8 id[NSLOT]; bool dirty[NSLOT]; u32 use[NSLOT]; } Rc;  // id: GPR 0-31, ID_CR, ID_XER, or -1
static u32 rc_off(u32 id) { return id < 32 ? OFF_GPR(id) : id == ID_CR ? OFF_CR : OFF_XER; }

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
    bool trace;         // blocks follow unconditional branches and continue past conditional ones (side exits), see trace_branch
    u32 gpc[MAX_BLOCK_INSNS], gpa[MAX_BLOCK_INSNS];  // guest address and physical address of each compiled instruction (a trace is not linear)
    Jit* j;
    // ARM flags still holding the compare behind CR field flagsFld: set where update_crf_from_flags ends (flagsAt);
    // flagsLive when that was the end of the previous instruction.
    u32 flagsAt, flagsFld;
    bool flagsSigned, flagsLive;
    int tailFpr;        // set_tail: lfs's FPR, or -1
    int tailN, tailHost[2];
    u32 tailGpr[2];
    // Guest GPRs cached in host registers within the block (see "register cache" below).
    Rc rc;
    u32 pinned;         // slots used by the current instruction (never evicted during it)
    u32 tick;
    // Out-of-line exit stubs: each writes back the dirty cached GPRs of `snap`, subtracts `count` from RDC and leaves
    // to the dispatcher, after writing g->pc = pc when `setPc` (otherwise g->pc is already written). inCold: the branch
    // to the stub is in the cold code.
    // kind: EXIT_DISPATCH (to the dispatcher, g->pc = pc when setPc), EXIT_LINK (a link site to guest address pc: a conditional
    // branch out of a trace), EXIT_INDIRECT (through the indirect table to the guest address in the Gekko word at offset pc: lr / ctr).
    struct { u32 at; u32 count; bool setPc, inCold; u32 pc; Rc snap; u8 kind; } exits[MAX_BLOCK_INSNS * 8];
    u32 nExits;
    // Cold code: slow paths are emitted into a second buffer (`alt` while hot code is current; the two Emit structs
    // swap places) and appended after the block, so hot code falls through its fast paths and packs into fewer cache
    // lines. Branches between the two are fixed up when the cold code is appended.
    Emit alt;
    bool inCold;
    struct { u32 at, target; bool fromCold, toCold; } fix[MAX_BLOCK_INSNS * 16];  // a pipe psq_st takes ~8
    u32 nFix;
    // Link sites (exit_const): a one-word branch in the hot path to its stub after the block, patched to the target
    // block once that is compiled (link_site).
    struct { u32* site; u32 target; } links[MAX_BLOCK_INSNS * 4];
    u32 nLinks;
    // Words that depend on the block's instruction count, known only at its end (see "Budget" in compile_block):
    // emit_time's ADD immediate and ST_INTERP's literal, in the hot or the cold buffer.
    struct { u32 at, count; bool cold, literal; } tfix[MAX_BLOCK_INSNS * 8];
    u32 nTfix;
    // FPR lanes (bit 2 * fr + lane) that hold an exact single-precision value at this point of the block: written by
    // lfs, psq_l or a rounded single-precision result since the block began. Gekko's single multiply cuts frC to 25
    // significant bits (ld_c25), which leaves such a value unchanged, so the cut is skipped for them.
    u64 single;
    // Fault-mode sites of this block (hot word index of the access, cold word index of its slow path), entered in the
    // Jit's table once the block is kept.
    bool faultMem;
    struct { u32 at, cold; } fsite[MAX_BLOCK_INSNS * 2];
    u32 nFsite;
} Cg;

#define NO_BRANCH 0xFFFFFFFFu

static void swap_emit(Cg* c) { Emit t = c->e; c->e = c->alt; c->alt = t; c->inCold = !c->inCold; }
// Points the branch at `site` to `dest` (both at their final addresses), keeping its condition and link bit.
static void patch_to(u32* site, const u32* dest) {
    s32 off = (s32)(dest - (site + 2));
    *site = (*site & 0xFF000000u) | ((u32)off & 0xFFFFFF);
}
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

// ---- register-cache statistics (PC build, JIT_REGSTAT=1): what pinning a guest register across blocks would save ----
// Per compiled block: how many times each cache id is loaded from memory into a slot and written back; compiled code
// counts the block's executions; the products are summed per id (folded when the cache is flushed, printed with the
// JIT statistics). The weights are dynamic, so a register loaded once in a hot loop body counts as much as it costs.
#ifndef __3DS__
#define RS_IDS 34
static bool g_regStat;
static u8 (*g_rsLd)[RS_IDS], (*g_rsSt)[RS_IDS];
static u32* g_rsExec;  // indexed by block entry (Entry index), incremented by the block itself
static u64 g_rsLdTot[RS_IDS], g_rsStTot[RS_IDS], g_rsExecTot;
static u32 g_rsCur;    // the block being compiled
// Dynamic opcode profile (same switch): each block's (opcode, hot words) list in a pool; folded with its execution count
// into executed guest instructions and executed hot ARM words per opcode.
#define DOP_POOL (4u << 20)
static u32* g_dopPool;   // key << 8 | hot words (capped at 255)
static u32 g_dopUsed, *g_dopStart, *g_dopLen;  // per block entry
static u64 g_dopN[65536], g_dopWords[65536];
// Simulated 8-lane FPR cache (what a float register cache would save): per block, ld_fpr hits (the lane was read or
// written earlier in the block, LRU of 8) and st_fpr stores overwritten by a later st_fpr before any ld_fpr of the lane.
#define FS_N 8
static u32 *g_fsLd, *g_fsHit, *g_fsSt, *g_fsDead;  // per block entry
static u64 g_fsLdTot, g_fsHitTot, g_fsStTot, g_fsDeadTot;
static int g_fsLane[FS_N], g_fsPending[64];  // lane ids (fr * 2 + lane), -1 empty; pending store per lane: 1 = stored, unread
static u32 g_fsUse[FS_N], g_fsTick;
static void fs_reset(void) {
    for (int k = 0; k < FS_N; k++) g_fsLane[k] = -1;
    for (int k = 0; k < 64; k++) g_fsPending[k] = 0;
}
static bool fs_touch(int lane) {  // true if the lane was in the simulated cache
    int best = 0;
    for (int k = 0; k < FS_N; k++) {
        if (g_fsLane[k] == lane) { g_fsUse[k] = ++g_fsTick; return true; }
        if (g_fsLane[k] < 0 || g_fsUse[k] < g_fsUse[best]) best = k;
    }
    if (g_fsLane[best] >= 0) g_fsPending[g_fsLane[best]] = 0;  // evicted: a write-back cache would store it then
    g_fsLane[best] = lane;
    g_fsUse[best] = ++g_fsTick;
    return false;
}
static void fs_ld(u32 fr, u32 lane) {
    if (!g_regStat) return;
    int l = (int)(fr * 2 + lane);
    g_fsLd[g_rsCur]++;
    if (fs_touch(l)) g_fsHit[g_rsCur]++;
    g_fsPending[l] = 0;
}
static void fs_st(u32 fr, u32 lane) {
    if (!g_regStat) return;
    int l = (int)(fr * 2 + lane);
    g_fsSt[g_rsCur]++;
    if (g_fsPending[l]) g_fsDead[g_rsCur]++;
    fs_touch(l);
    g_fsPending[l] = 1;
}

// ---- region-compilation model (JIT_REGSTAT; docs/REGION_JIT_PLAN.md) ----
// Why each load into a slot and each write-back happens, per block: loads at a register's first touch in the block
// (RL_FIRST), after an interpreter fallback forgot the cache (RL_INTERP) or after an eviction (RL_EVICT); write-backs at
// the block's end before a constant exit (RW_CONST) or blr / bctr (RW_IND), before an interpreter fallback (RW_INTERP),
// at an eviction (RW_EVICT), elsewhere (RW_OTHER).
enum { RL_FIRST, RL_INTERP, RL_EVICT, RL_N };
enum { RW_CONST, RW_IND, RW_INTERP, RW_EVICT, RW_OTHER, RW_N };
static u8 (*g_rwLd)[RL_N], (*g_rwSt)[RW_N];
static u64 g_rwLdTot[RL_N], g_rwStTot[RW_N];
static int g_rwWhy = RW_OTHER;          // the reason of the rc_flush being emitted
static u64 g_rsSeen, g_rsEvicted, g_rsWritten;  // ids touched / evicted / written so far in the block being compiled
// Each block's would-be entry state: the first NSLOT_STAT ids it touches (the slots they take in an empty cache, so a
// predecessor could hand them over in place), which of them it touches first by reading (a load today), and every id it
// writes (a dirty one handed over needs no write-back by the predecessor: the block's own exits write it back).
#define NSLOT_STAT 5
typedef struct { s8 id[NSLOT_STAT]; u8 readFirst; u64 written, touched; } RsEntry;
static RsEntry* g_rsE;
// Exits at block ends, with the cache as it stood before the flush and their executions (counted by compiled code).
enum { RX_CONST, RX_IND, RX_DISPATCH };
typedef struct { u32 block, target, from; u8 kind; s8 id[NSLOT_STAT]; u8 dirty; } RsExit;  // from: the branch's address
#define RX_POOL (1u << 18)
static RsExit* g_rx;
static u32* g_rxCount;
static u32 g_rxUsed;
static bool g_rxHave;   // g_rxState holds the cache of the block end being compiled (set before its flush)
static Rc g_rxState;
// Totals of the model (folded with the cache): edge executions and their write-backs and loads today, and with the
// target's entry state handed over (stores of what the target does not keep, loads of what it wants and the source
// lacks, moves of what sits in another slot).
static u64 g_rxN[3], g_rxTodaySt[3], g_rxTodayLd, g_rxHoSt, g_rxHoLd, g_rxHoMov, g_rxNoTarget;
// Variant 1 (the entry state holds the registers the block writes first too): entries not through a link must load
// those as well (g_rxV1Extra). Variant 2 (only the registers it reads first): g_rxV2*.
static u64 g_rxV1Extra, g_rxV2St, g_rxV2Ld, g_rxV2Mov, g_rxLinkIn[1u << 16];
// Concentration: block traffic (loads + write-backs per run x runs) and back edges.
#define RS_TOPN 4096
static u64 g_rsTrafTot, g_rsTrafTop[5], g_rsTrafLoop, g_rsBackN, g_rsBackSt;
// psq_l / psq_st executions by store, the GQR's type at compile time (0 float, 4-7 quantized) and W (one element).
static u32 g_psqRun[2][8][2];
// Memory instructions executed by how they form their address: (rA|0) + d with rA = r1, r2, r13, another GPR or 0;
// rA + rB.
static u32 g_eaRun[6];
// Loop regions: each block belongs to the innermost back edge's range holding its start. A loop whose blocks touch at
// most NSLOT_STAT ids could keep them all in slots: its traffic goes, and each entry loads them and each exit stores the
// written ones instead. g_rsLoopFit / g_rsLoopBig: traffic in loops that fit / do not; g_rsLoopEntryCost.
static u64 g_rsLoopFit, g_rsLoopBig, g_rsLoopEntryCost, g_rsLoops[2];
static void rs_emit_count(Emit* e, u32* counter) {  // ++*counter in compiled code (r12, lr; flags kept)
    load_const(e, R12, (u32)(uintptr_t)counter);
    ldr_imm(e, LR, R12, 0);
    dp_imm(e, ADD, 0, LR, LR, 1);
    str_imm(e, LR, R12, 0);
}
static void rx_add(Cg* c, int kind, u32 target, u32 from);

static void dop_add(u32 key, u32 hot) {
    if (!g_regStat || g_dopUsed >= DOP_POOL) return;
    g_dopPool[g_dopUsed++] = key << 8 | (hot > 255 ? 255 : hot);
    g_dopLen[g_rsCur]++;
}
static int u64_desc(const void* a, const void* b) {
    u64 x = *(const u64*)a, y = *(const u64*)b;
    return x < y ? 1 : x > y ? -1 : 0;
}

static Entry* rs_find(Jit* j, u32 pc, u32 msrBits);

// The region model over the blocks and exits of the cache being flushed (see the declarations above).
static void rx_fold(Jit* j, u32 nEntries) {
    static u8* loopMark;  // one bit per guest word of MEM1 inside a back edge's range
    if (!loopMark) loopMark = calloc(1, (24u << 20) / 32);
    for (u32 x = 0; x < g_rxUsed; x++) {
        const RsExit* r = &g_rx[x];
        u64 n = g_rxCount[x];
        if (!n) continue;
        u32 dirty = (u32)__builtin_popcount(r->dirty);
        g_rxN[r->kind] += n;
        g_rxTodaySt[r->kind] += n * dirty;
        if (r->kind != RX_CONST) continue;
        // A back edge (to an earlier address, within 4 KB): the blocks starting in its range are in a loop.
        u32 from = r->from, to = r->target, src = j->table[r->block].pc;
        if (to <= from && from - to < 4096 && loopMark) {
            g_rsBackN += n;
            g_rsBackSt += n * dirty;
            for (u32 a = to; a <= from; a += 4) {
                u32 w = (a & 0x01FFFFFFu) >> 2;
                if (w < (24u << 20) / 4) loopMark[w >> 3] |= (u8)(1u << (w & 7));
            }
        }
        (void)src;
        Entry* te = rs_find(j, r->target, j->table[r->block].msrBits);
        if (!te) { g_rxNoTarget += n; continue; }
        const RsEntry* E = &g_rsE[te - j->table];
        u32 st = 0, ld = 0, mov = 0;
        for (int k = 0; k < NSLOT_STAT; k++) {
            if (r->id[k] < 0 || !((r->dirty >> k) & 1)) continue;
            bool kept = false;
            for (int m = 0; m < NSLOT_STAT; m++) if (E->id[m] == r->id[k]) kept = (E->written >> (u32)r->id[k]) & 1;
            st += !kept;
        }
        for (int m = 0; m < NSLOT_STAT; m++) {
            if (E->id[m] < 0) continue;
            int at = -1;
            for (int k = 0; k < NSLOT_STAT; k++) if (r->id[k] == E->id[m]) at = k;
            if (at < 0) ld += (E->readFirst >> m) & 1;
            else mov += at != m;
        }
        g_rxTodayLd += n * (u64)__builtin_popcount(E->readFirst);
        g_rxHoSt += n * st;
        g_rxHoLd += n * ld;
        g_rxHoMov += n * mov;
        u32 ti = (u32)(te - j->table);
        if (ti < (1u << 16)) g_rxLinkIn[ti] += n;
        // Variant 2: the read-first registers only, in their slots.
        u32 st2 = 0, ld2 = 0, mov2 = 0;
        for (int k = 0; k < NSLOT_STAT; k++) {
            if (r->id[k] < 0 || !((r->dirty >> k) & 1)) continue;
            bool kept = false;
            for (int m = 0; m < NSLOT_STAT; m++)
                if (E->id[m] == r->id[k] && ((E->readFirst >> m) & 1)) kept = (E->written >> (u32)r->id[k]) & 1;
            st2 += !kept;
        }
        for (int m = 0; m < NSLOT_STAT; m++) {
            if (E->id[m] < 0 || !((E->readFirst >> m) & 1)) continue;
            int at = -1;
            for (int k = 0; k < NSLOT_STAT; k++) if (r->id[k] == E->id[m]) at = k;
            if (at < 0) ld2++;
            else mov2 += at != m;
        }
        g_rxV2St += n * st2;
        g_rxV2Ld += n * ld2;
        g_rxV2Mov += n * mov2;
    }
    {   // Loop regions (see g_rsLoopFit): back edges, innermost first.
        typedef struct { u32 to, from; u64 n; } Back;
        static Back back[8192];
        static u32 owner[1u << 16];  // block -> 1 + back edge index
        u32 nb = 0;
        for (u32 x = 0; x < g_rxUsed && nb < 8192; x++) {
            const RsExit* r = &g_rx[x];
            if (r->kind != RX_CONST || !g_rxCount[x] || r->target > r->from || r->from - r->target >= 4096) continue;
            back[nb++] = (Back){ r->target, r->from, g_rxCount[x] };
        }
        for (u32 a = 0; a < nb; a++)  // smallest range first
            for (u32 b = a + 1; b < nb; b++)
                if (back[b].from - back[b].to < back[a].from - back[a].to) { Back t = back[a]; back[a] = back[b]; back[b] = t; }
        u32 nbk = nEntries < (1u << 16) ? nEntries : (1u << 16);
        memset(owner, 0, nbk * sizeof owner[0]);
        for (u32 a = 0; a < nb; a++)
            for (u32 b = 0; b < nbk; b++)
                if (!owner[b] && g_rsExec[b] && j->table[b].pc >= back[a].to && j->table[b].pc <= back[a].from) owner[b] = a + 1;
        for (u32 a = 0; a < nb; a++) {
            u64 touched = 0, written = 0, traf = 0, headExec = 0;
            bool any = false;
            for (u32 b = 0; b < nbk; b++) {
                if (owner[b] != a + 1) continue;
                any = true;
                touched |= g_rsE[b].touched;
                written |= g_rsE[b].written;
                u64 t = 0;
                for (int i = 0; i < RS_IDS; i++) t += (u64)g_rsLd[b][i] + g_rsSt[b][i];
                traf += t * g_rsExec[b];
                if (j->table[b].pc == back[a].to) headExec += g_rsExec[b];
            }
            if (!any) continue;
            u32 nt = (u32)__builtin_popcountll(touched), nw = (u32)__builtin_popcountll(written);
            if (nt <= NSLOT_STAT) {
                u64 entries = headExec > back[a].n ? headExec - back[a].n : 0;
                g_rsLoopFit += traf;
                g_rsLoopEntryCost += entries * (nt + nw);
                g_rsLoops[0]++;
            } else {
                g_rsLoopBig += traf;
                g_rsLoops[1]++;
            }
        }
    }
    // Variant 1's extra loads: entries from the dispatcher or blr / bctr load the write-first registers too.
    for (u32 b = 0; b < nEntries && b < (1u << 16); b++) {
        u32 wf = 0;
        for (int m = 0; m < NSLOT_STAT; m++) wf += g_rsE[b].id[m] >= 0 && !((g_rsE[b].readFirst >> m) & 1);
        u64 other = g_rsExec[b] > g_rxLinkIn[b] ? g_rsExec[b] - g_rxLinkIn[b] : 0;
        g_rxV1Extra += other * wf;
        g_rxLinkIn[b] = 0;
    }
    // Block traffic: how concentrated it is, and how much of it is in blocks inside a back edge's range.
    static u64 traf[1u << 16];
    u32 nt = 0;
    for (u32 b = 0; b < nEntries && nt < (1u << 16); b++) {
        u64 t = 0;
        for (int i = 0; i < RS_IDS; i++) t += (u64)g_rsLd[b][i] + g_rsSt[b][i];
        t *= g_rsExec[b];
        if (!t) continue;
        traf[nt++] = t;
        g_rsTrafTot += t;
        u32 w = (j->table[b].pc & 0x01FFFFFFu) >> 2;
        if (loopMark && w < (24u << 20) / 4 && ((loopMark[w >> 3] >> (w & 7)) & 1)) g_rsTrafLoop += t;
    }
    qsort(traf, nt, sizeof traf[0], u64_desc);
    static const u32 kTop[5] = { 16, 64, 256, 1024, 4096 };
    u64 cum = 0;
    for (u32 i = 0, k = 0; i < nt && k < 5; i++) {
        cum += traf[i];
        while (k < 5 && i + 1 == kTop[k]) g_rsTrafTop[k++] += cum;
        if (i + 1 == nt) while (k < 5) g_rsTrafTop[k++] += cum;
    }
    if (loopMark) memset(loopMark, 0, (24u << 20) / 32);
    memset(g_rxCount, 0, g_rxUsed * sizeof(u32));
    g_rxUsed = 0;
    for (u32 b = 0; b < nEntries; b++) {
        for (int i = 0; i < RL_N; i++) g_rwLdTot[i] += (u64)g_rsExec[b] * g_rwLd[b][i];
        for (int i = 0; i < RW_N; i++) g_rwStTot[i] += (u64)g_rsExec[b] * g_rwSt[b][i];
        memset(g_rwLd[b], 0, RL_N);
        memset(g_rwSt[b], 0, RW_N);
    }
}

// JIT_SCHEDDUMP=path (with JIT_REGSTAT): every executed block's hot code goes to `path` when the statistics are folded,
// for an offline model of the ARM11 pipeline (core/tools/arm11_sched.py): per block the guest PC, the hot words,
// where its instructions start (after the budget check and the statistics counter), its (opcode, hot words) list and
// its execution count.
static FILE* g_schedOut;
static u32 *g_hotLen, *g_proLen;
static void rs_fold(Jit* j, u32 nEntries) {
    if (!g_regStat) return;
    rx_fold(j, nEntries);
    static bool stubsOut;
    if (g_schedOut && !stubsOut) {  // the shared stubs once (calls into them are followed): magic, base, words
        u32 hdr[3] = { 0x42555453u, (u32)(uintptr_t)j->code, j->stubWords };
        fwrite(hdr, 4, 3, g_schedOut);
        fwrite(j->code, 4, j->stubWords, g_schedOut);
        stubsOut = true;
    }
    for (u32 b = 0; g_schedOut && b < nEntries; b++) {
        u64 n = g_rsExec[b];
        if (!n || !g_hotLen[b] || !j->table[b].fn) continue;
        u32 hdr[6] = { 0x4B4C4222u, j->table[b].pc, g_hotLen[b], g_proLen[b], g_dopLen[b], (u32)(uintptr_t)j->table[b].fn };
        fwrite(hdr, 4, 6, g_schedOut);
        fwrite(&n, 8, 1, g_schedOut);
        fwrite((const void*)j->table[b].fn, 4, g_hotLen[b], g_schedOut);
        fwrite(&g_dopPool[g_dopStart[b]], 4, g_dopLen[b], g_schedOut);
    }
    if (g_schedOut) fflush(g_schedOut);
    g_dopUsed = 0;  // the pool restarts with the cache (every block's list is folded below)
    for (u32 b = 0; b < nEntries; b++) {
        u64 n = g_rsExec[b];
        g_rsExecTot += n;
        for (int i = 0; i < RS_IDS; i++) { g_rsLdTot[i] += n * g_rsLd[b][i]; g_rsStTot[i] += n * g_rsSt[b][i]; }
        for (u32 k = 0; k < g_dopLen[b]; k++) {
            u32 v = g_dopPool[g_dopStart[b] + k];
            g_dopN[v >> 8] += n;
            g_dopWords[v >> 8] += n * (v & 255);
        }
        g_dopLen[b] = 0;
        g_fsLdTot += n * g_fsLd[b]; g_fsHitTot += n * g_fsHit[b]; g_fsStTot += n * g_fsSt[b]; g_fsDeadTot += n * g_fsDead[b];
        g_fsLd[b] = g_fsHit[b] = g_fsSt[b] = g_fsDead[b] = 0;
        g_rsExec[b] = 0;
        memset(g_rsLd[b], 0, RS_IDS);
        memset(g_rsSt[b], 0, RS_IDS);
    }
}
static void rs_load(u32 id) {
    if (!g_regStat) return;
    if (g_rsLd[g_rsCur][id] < 255) g_rsLd[g_rsCur][id]++;
    int why = !((g_rsSeen >> id) & 1) ? RL_FIRST : ((g_rsEvicted >> id) & 1) ? RL_EVICT : RL_INTERP;
    if (g_rwLd[g_rsCur][why] < 255) g_rwLd[g_rsCur][why]++;
}
static void rs_store_why(u32 id, int why) {
    if (!g_regStat) return;
    if (g_rsSt[g_rsCur][id] < 255) g_rsSt[g_rsCur][id]++;
    if (g_rwSt[g_rsCur][why] < 255) g_rwSt[g_rsCur][why]++;
}
static void rs_store(u32 id) { rs_store_why(id, g_rwWhy); }
// A hot-code touch of cache id `id` (read: a load from memory, or write: computed into its slot): the block's entry
// state records its first NSLOT_STAT ids.
static void rs_touch(u32 id, bool read) {
    if (!g_regStat || ((g_rsSeen >> id) & 1)) return;
    g_rsSeen |= 1ull << id;
    RsEntry* E = &g_rsE[g_rsCur];
    for (int k = 0; k < NSLOT_STAT; k++)
        if (E->id[k] < 0) { E->id[k] = (s8)id; if (read) E->readFirst |= (u8)(1u << k); break; }
}
static void rs_written(u32 id) { if (g_regStat) g_rsWritten |= 1ull << id; }
static void rs_evicted(u32 id) { if (g_regStat) g_rsEvicted |= 1ull << id; }
#else
static void rs_load(u32 id) { (void)id; }
static void rs_store(u32 id) { (void)id; }
static void rs_store_why(u32 id, int why) { (void)id; (void)why; }
static void rs_touch(u32 id, bool read) { (void)id; (void)read; }
static void rs_written(u32 id) { (void)id; }
static void rs_evicted(u32 id) { (void)id; }
#define RW_EVICT 0
#endif

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
        str_imm(&c->e, kSlot[k], RG, rc_off((u32)rc->id[k]));
        if (!c->inCold && rc == &c->rc) rs_store((u32)rc->id[k]);  // hot flushes only: exit stubs would count once per stub, not per run
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
            if (c->rc.id[k] >= 0) ldr_imm(&c->e, kSlot[k], RG, rc_off((u32)c->rc.id[k]));
        return;
    }
    for (int k = 0; k < NSLOT; k++) { c->rc.id[k] = -1; c->rc.dirty[k] = false; }
}

#ifndef __3DS__
// A block-end exit for the region model: the cache before the block's final flush, and a counter compiled code bumps.
static void rx_add(Cg* c, int kind, u32 target, u32 from) {
    if (!g_regStat || !g_rxHave || c->inCold || g_rxUsed == RX_POOL) return;
    RsExit* x = &g_rx[g_rxUsed];
    x->block = g_rsCur;
    x->target = target;
    x->from = from;
    x->kind = (u8)kind;
    x->dirty = 0;
    for (int k = 0; k < NSLOT_STAT; k++) {
        x->id[k] = g_rxState.id[k];
        if (g_rxState.id[k] >= 0 && g_rxState.dirty[k]) x->dirty |= (u8)(1u << k);
    }
    rs_emit_count(&c->e, &g_rxCount[g_rxUsed]);
    g_rxUsed++;
}
#endif

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
    if (c->rc.id[best] >= 0 && c->rc.dirty[best]) { str_imm(&c->e, kSlot[best], RG, rc_off((u32)c->rc.id[best])); rs_store_why((u32)c->rc.id[best], RW_EVICT); }
    if (c->rc.id[best] >= 0) rs_evicted((u32)c->rc.id[best]);
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
        ldr_imm(&c->e, tmp, RG, rc_off(r));
        return tmp;
    }
    if (k < 0) {
        k = rc_alloc(c, r);
        if (k < 0) { ldr_imm(&c->e, tmp, RG, rc_off(r)); rs_load(r); rs_touch(r, true); return tmp; }
        ldr_imm(&c->e, kSlot[k], RG, rc_off(r));
        rs_load(r);
        rs_touch(r, true);
    }
    rc_touch(c, k);
    return kSlot[k];
}

// A host register to compute guest GPR r into (its slot, or `tmp`); dst_done commits the value afterwards. A slot
// is marked dirty only by dst_done, so an exit taken in between writes back the old value.
static int dst(Cg* c, u32 r, int tmp) {
    if (c->inCold) return tmp;
    rs_touch(r, false);
    int k = slot_of(&c->rc, r);
    if (k < 0) k = rc_alloc(c, r);
    if (k < 0) return tmp;
    rc_touch(c, k);
    return kSlot[k];
}
static void dst_done(Cg* c, u32 r, int reg) {
    if (!c->inCold) rs_written(r);
    int k = slot_of(&c->rc, r);
    if (k >= 0 && kSlot[k] == reg) { c->rc.dirty[k] = true; return; }
    str_imm(&c->e, reg, RG, rc_off(r));
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
    if (c->nExits == sizeof c->exits / sizeof c->exits[0]) { c->e.overflow = true; return; }  // refused, see compileFails
    c->exits[c->nExits].at = branch_c(&c->e, cond, c->e.n);
    c->exits[c->nExits].count = c->count;
    c->exits[c->nExits].setPc = setPc;
    c->exits[c->nExits].inCold = c->inCold;
    c->exits[c->nExits].pc = pc;
    c->exits[c->nExits].snap = snap ? *snap : c->rc;
    c->exits[c->nExits].kind = EXIT_DISPATCH;
    c->nExits++;
}
// A conditional exit from the middle of a trace (the hot path continues after the branch): to the stub, which writes back
// the cache as it stands here and leaves through a link site to guest address `pc` (EXIT_LINK) or through the indirect table
// to the address in the Gekko word at offset `pc` (EXIT_INDIRECT).
static void exit_kind_if(Cg* c, int cond, int kind, u32 pc) {
    if (c->nExits == sizeof c->exits / sizeof c->exits[0]) { c->e.overflow = true; return; }
    c->exits[c->nExits].at = branch_c(&c->e, cond, c->e.n);
    c->exits[c->nExits].count = c->count;
    c->exits[c->nExits].setPc = false;
    c->exits[c->nExits].inCold = c->inCold;
    c->exits[c->nExits].pc = pc;
    c->exits[c->nExits].snap = c->rc;
    c->exits[c->nExits].kind = (u8)kind;
    c->nExits++;
}
static void exit_if(Cg* c, int cond) { exit_snap(c, cond, false, 0, NULL); }
// The same, writing g->pc = pc first; the dispatcher never links it (state the block depends on may have changed).
static void exit_pc_if(Cg* c, int cond, u32 pc) { exit_snap(c, cond, true, pc, NULL); }

// Unconditional exit to the guest address `target`, emitted in place: a link site, one word in the hot path (a branch
// to its stub after the block, which asks the dispatcher for the target; link_site then points it at the target block).
// The cache must already be written back (rc_flush). The block's instructions were taken off the budget at its entry
// (compile_block), and the target block checks the budget at its own entry.
static void link_add(Cg* c, u32 at, u32 target) {
    if (c->nLinks == sizeof c->links / sizeof c->links[0]) { c->e.overflow = true; return; }  // refused
    c->links[c->nLinks].site = c->e.buf + at;  // hot and cold code are both emitted at their final addresses
    c->links[c->nLinks].target = target;
    c->nLinks++;
}
static void exit_const(Cg* c, u32 target) {
    Emit* e = &c->e;
#ifndef __3DS__
    rx_add(c, c->noLink ? RX_DISPATCH : RX_CONST, target, c->pc);
#endif
    if (c->noLink) {
        load_const(e, R1, target);
        str_imm(e, R1, RG, OFF_PC);
        branch_abs(e, C_AL, false, stub(c, ST_DYN));
        return;
    }
    link_add(c, branch_c(e, C_AL, 0), target);
}

// Unconditional exit to the guest address in r2 (blr, bctr): through the indirect-branch table.
static void exit_indirect(Cg* c) {
    Emit* e = &c->e;
#ifndef __3DS__
    rx_add(c, c->noLink ? RX_DISPATCH : RX_IND, 0, c->pc);
#endif
    str_imm(e, R2, RG, OFF_PC);
    branch_abs(e, C_AL, false, stub(c, c->noLink ? ST_DYN : ST_INDIRECT));
}

// A word that depends on the block's instruction count (see Cg.tfix), at the current position.
static void tfix_add(Cg* c, bool literal) {
    if (c->nTfix == sizeof c->tfix / sizeof c->tfix[0]) { c->e.overflow = true; return; }
    c->tfix[c->nTfix].at = c->e.n;
    c->tfix[c->nTfix].count = c->count;
    c->tfix[c->nTfix].cold = c->inCold;
    c->tfix[c->nTfix].literal = literal;
    c->nTfix++;
}

// Call a C helper with up to 4 word arguments already in r0-r3.
// Helpers compiled code calls, by their index in Gekko.jitFn (filled by jit_create): two words per call instead of
// a 32-bit constant (MOV + up to three ORRs) and BLX.
static const void* const kJitFns[] = { (const void*)jit_load, (const void*)jit_store, (const void*)jit_gather,
                                       (const void*)jit_interp, (const void*)jit_mtmsr, (const void*)jit_load64,
                                       (const void*)jit_store64, (const void*)jit_hle };
// Gekko.jitFn after the C helpers: the shared slow-path stubs, and the write-gather pipe's address, length and buffer.
enum { JF_LOADSLOW = 8, JF_STORESLOW, JF_STOREPIPE, JF_GATHER_EA, JF_GATHER_LEN, JF_GATHER_BUF, JF_N };
_Static_assert(sizeof kJitFns / sizeof kJitFns[0] <= JF_LOADSLOW, "kJitFns overlaps the stub slots");
_Static_assert(JF_N <= sizeof ((Gekko*)0)->jitFn / sizeof(u32), "Gekko.jitFn too small");
static void call(Cg* c, const void* fn) {
    for (u32 k = 0; k < sizeof kJitFns / sizeof kJitFns[0]; k++)
        if (kJitFns[k] == fn) { ldr_imm(&c->e, R12, RG, OFF_JITFN(k)); blx_reg(&c->e, R12); return; }
    load_const(&c->e, R12, (u32)(uintptr_t)fn);
    blx_reg(&c->e, R12);
}

// Before a helper that runs at guest time: the budget left at this instruction to g_jitLeft (clobbers lr and r12).
// RDC already has the whole block taken off: lr = RDC + (instructions in the block - instructions before this one),
// an immediate patched at the block's end (tfix).
static void emit_time(Cg* c) {
    tfix_add(c, false);
    dp_imm(&c->e, ADD, 0, LR, RDC, 0);
    str_imm(&c->e, LR, RG, OFF_JITLEFT);
}

// CR field `fld` from the ARM flags of a compare already done (signed or unsigned), plus XER[SO]. The ARM flags are
// left as the compare set them (cache loads and stores and MOV / ORR / BIC / AND without S keep them), so a
// conditional branch right after it on that field tests them directly (see emit_branch_cond).
static void update_crf_from_flags(Cg* c, u32 fld, bool isSigned) {
    Emit* e = &c->e;
    u32 sh = 28 - 4 * fld;
    u32 enc = 0;
    bool so = c->j->soExact;  // otherwise XER[SO] is known 0 (see jit_set_so_exact): bit 0 of the field stays clear
    int cr = src(c, ID_CR, R1);
    int xer = so ? src(c, ID_XER, R3) : -1;
    int d = dst(c, ID_CR, R1);
    arm_imm(0xFu << sh, &enc); dp_imm(e, BIC, 0, d, cr, enc);  // a nibble at a multiple of 4: always an ARM immediate
    arm_imm(8u << sh, &enc); dp_imm_c(e, isSigned ? C_LT : C_CC, ORR, 0, d, d, enc);
    arm_imm(4u << sh, &enc); dp_imm_c(e, isSigned ? C_GT : C_HI, ORR, 0, d, d, enc);
    arm_imm(2u << sh, &enc); dp_imm_c(e, C_EQ, ORR, 0, d, d, enc);
    if (so) {
        dp_imm(e, AND, 0, R3, xer, IMM(XER_SO));
        dp_reg_c(e, C_AL, ORR, 0, d, d, R3, LSR, (int)(31 - sh));  // SO (bit 31) to the field's bit 0
    }
    dst_done(c, ID_CR, d);
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
    int x = src(c, ID_XER, R3);
    int d = dst(c, ID_XER, R3);
    dp_imm(e, BIC, 0, d, x, IMM(XER_CA));
    dp_imm_c(e, C_CS, ORR, 0, d, d, IMM(XER_CA));
    dst_done(c, ID_XER, d);
}

// ARM carry flag from XER[CA] (bit 29 shifted out by LSL #3). Clobbers r3.
static void load_ca_to_c(Cg* c) {
    int x = src(c, ID_XER, R3);
    mov_shift(&c->e, 1, R3, x, LSL, 3);
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

// After a slow-path helper call: g_status 1 = the access raised an exception (leave; nothing was written), 2 = it
// completed but compiled code must stop after this instruction. Leaves the status in r3 (0 or 2) for stop_tail and
// clears g_status.
static void slow_status(Cg* c) {
    Emit* e = &c->e;
    ldr_imm(e, R3, RG, OFF_JITSTATUS);
    dp_imm(e, MOV, 0, R1, 0, 0);
    str_imm(e, R1, RG, OFF_JITSTATUS);
    dp_imm(e, CMP, 1, 0, R3, 1);
    exit_if(c, C_EQ);
    c->pendingStop = true;
}

// What finishes a memory instruction after its access (set by the caller before emit_load / emit_store /
// emit_load64 / emit_store64): GPR results in host registers and, for lfs, an FPR from the single in r2. A slow path
// that must leave compiled code after the access (r3 != 0) finishes the instruction with them and leaves.
static void set_tail(Cg* c, int n, int h0, u32 g0, int h1, u32 g1) {
    c->tailN = n;
    c->tailHost[0] = h0; c->tailGpr[0] = g0;
    c->tailHost[1] = h1; c->tailGpr[1] = g1;
}

static void stop_tail(Cg* c) {
    if (!c->pendingStop) return;
    Emit* e = &c->e;
    dp_imm(e, CMP, 1, 0, R3, 0);
    u32 skip = branch_c(e, C_EQ, 0);
    // Write back the cache as it stands, then the instruction's results straight to memory, and leave.
    rc_emit_flush(c, &c->rc);
    for (int k = 0; k < c->tailN; k++) str_imm(e, c->tailHost[k], RG, OFF_GPR(c->tailGpr[k]));
    if (c->tailFpr >= 0) {
        vmov_s_r(e, 0, R2);
        vcvt_f64_f32(e, 1, 0);
        vstr_d(e, 1, RG, OFF_FPR((u32)c->tailFpr));
        vstr_d(e, 1, RG, OFF_FPR((u32)c->tailFpr) + 8);
    }
    Rc clean = c->rc;
    for (int k = 0; k < NSLOT; k++) clean.dirty[k] = false;
    exit_snap(c, C_AL, false, 0, &clean);  // g->pc is already the next instruction
    patch_branch(e, skip, e->n);
    c->pendingStop = false;
}

// Effective address into r0: (rA|0) + d, or rA + rB for indexed forms.
static void ea_disp(Cg* c, u32 ra, u32 d) {
#ifndef __3DS__
    if (g_regStat && !c->inCold) rs_emit_count(&c->e, &g_eaRun[ra == 1 ? 0 : ra == 2 ? 1 : ra == 13 ? 2 : ra ? 3 : 4]);
#endif
    if (ra) add_const(&c->e, R0, src(c, ra, R0), d, R12);
    else load_const(&c->e, R0, d);
}
static void ea_index(Cg* c, u32 ra, u32 rb) {
#ifndef __3DS__
    if (g_regStat && !c->inCold) rs_emit_count(&c->e, &g_eaRun[5]);
#endif
    int b = src(c, rb, R1);
    if (ra) dp_reg(&c->e, ADD, 0, R0, src(c, ra, R0), b);
    else mov_reg(&c->e, R0, b);
}

// The inline MEM1 check of the fast paths: r1 = the physical offset of the guest address in r0 (0xC0000000 folded
// onto 0x80000000); branches to the slow path (returned in slow[], NO_BRANCH = none) when the address is not
// `align`-byte aligned or not inside MEM1 (an aligned access of `align` bytes then fits entirely).
// r1 = ea - 0x80000000; the alignment and the MEM1 range in one compare: rotated right by log2(align), a misaligned
// offset has bits at the top and fails the same unsigned bound as one past the end. The uncached mirror (0xC0000000)
// takes the slow path. Uses r12.
static void mem1_check(Cg* c, u32 align, u32 slow[2]) {
    Emit* e = &c->e;
    slow[0] = slow[1] = NO_BRANCH;
    u32 sh = align >= 8 ? 3 : align >= 4 ? 2 : align >= 2 ? 1 : 0;
    dp_imm(e, SUB, 0, R1, R0, IMM(0x80000000u));
    if (sh) {
        mov_shift(e, 0, R12, R1, ROR, (int)sh);
        dp_imm(e, CMP, 1, 0, R12, IMM(0x01800000u >> sh));
    } else {
        dp_imm(e, CMP, 1, 0, R1, IMM(0x01800000u));
    }
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

// Fault mode (see Jit.faultMem): a site may run its access unchecked while the block's sites and the Jit's table have
// room; fault_site, called at the start of the site's cold slow path, records where the handler sends it.
#define FAULT_BITS 18
#define FAULT_SLOTS (1u << FAULT_BITS)
#define FAULT_MAX (FAULT_SLOTS / 4 * 3)
static bool fault_ok(const Cg* c) {
    return c->faultMem && !c->inCold && c->nFsite < sizeof c->fsite / sizeof c->fsite[0] && c->j->faultN + c->nFsite < FAULT_MAX;
}
static void fault_site(Cg* c, u32 hotAt) {
    c->fsite[c->nFsite].at = hotAt;
    c->fsite[c->nFsite].cold = c->e.n;
    c->nFsite++;
}

// Every memory access keeps the effective address in r0 on all its paths (update forms write it to rA afterwards):
// the slow paths save it around their helper call.
#define EA_SAVE ((1u << R0) | (1u << R1))  // two registers keep the stack 8-byte aligned

// Calls a shared slow-path stub (ST_LOADSLOW / ST_STORESLOW / ST_STOREPIPE) with its literal after the call; the
// stub returns past it with the status in r3 (1 exception, 2 finish and leave), r0 (the address) preserved. Five words
// per site instead of about 25: no 32-bit constants (the PC is the literal) and the helper call is shared.
static void call_slow_stub(Cg* c, int jf, u32 literal) {
    Emit* e = &c->e;
    emit_time(c);
    ldr_imm(e, R12, RG, OFF_JITFN(jf));
    blx_reg(e, R12);
    word(e, literal);
}
static void stub_status(Cg* c) {
    dp_imm(&c->e, CMP, 1, 0, R3, 1);
    exit_if(c, C_EQ);
    c->pendingStop = true;
}

static void load_slow(Cg* c, u32 size, bool sign, int dreg) {
    Emit* e = &c->e;
    if (c->j->slowInline) {  // the old per-site slow path (A/B): r0 = ea; jit_load(g, ea, size, pc)
        push_regs(e, EA_SAVE);
        mov_reg(e, R1, R0);
        mov_reg(e, R0, RG);
        load_const(e, R2, size);
        load_const(e, R3, c->pc);
        emit_time(c);
        call(c, (const void*)jit_load);
        mov_reg(e, R2, R0);
        pop_regs(e, EA_SAVE);
        slow_status(c);
    } else {  // r0 = ea; the stub calls jit_load(g, ea, size, pc) and returns the value in r2
        call_slow_stub(c, JF_LOADSLOW, c->pc | (size == 4 ? 2u : size == 2 ? 1u : 0u));
        stub_status(c);  // a fault leaves before dreg (maybe a cached GPR) changes
    }
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
    if (fault_ok(c)) {  // the access alone; its slow path is reached only by the fault handler's patch
        u32 at = e->n;
        if (size == 4) { ldr_reg(e, dreg, RRAM, R0); rev(e, dreg, dreg); }
        else if (size == 2) { ldrh_reg(e, dreg, RRAM, R0); if (sign) revsh(e, dreg, dreg); else rev16(e, dreg, dreg); }
        else ldrb_reg(e, dreg, RRAM, R0);
        u32 resume = e->n;
        cold_begin(c);
        fault_site(c, at);
        load_slow(c, size, sign, dreg);
        cold_end(c, resume);
        return;
    }
    u32 slow[2];
    int ri = ms_reg(dreg);
    if (c->j->memStubs && ri >= 0) {  // the shared stub: two words here instead of the check and access inline
        int k = size == 4 ? MS_L32 : size == 2 ? (sign ? MS_L16S : MS_L16U) : MS_L8;
        bool v2 = (c->j->sched & SCHED_STUBS) != 0;
        branch_abs(e, C_AL, true, c->j->code + (v2 ? c->j->memStub2[k][ri] : c->j->memStub[k][ri]));
        slow[0] = branch_c(e, C_CS, 0);
        slow[1] = NO_BRANCH;
        if (v2 && size == 4) rev(e, dreg, dreg);  // the scheduled stubs leave the swap here (see emit_mem_stubs)
        else if (v2 && size == 2) { if (sign) revsh(e, dreg, dreg); else rev16(e, dreg, dreg); }
    } else {
        mem1_check(c, size, slow);
        if (size == 4) { ldr_reg(e, dreg, RRAM, R0); rev(e, dreg, dreg); }
        else if (size == 2) { ldrh_reg(e, dreg, RRAM, R0); if (sign) revsh(e, dreg, dreg); else rev16(e, dreg, dreg); }
        else ldrb_reg(e, dreg, RRAM, R0);
    }
    u32 resume = e->n;
    cold_start(c, slow);
    load_slow(c, size, sign, dreg);
    cold_end(c, resume);
}

static void store_slow(Cg* c, u32 size, int vreg) {
    Emit* e = &c->e;
    u32 sizePc = c->pc | (size == 4 ? 2u : size == 2 ? 1u : 0u);  // pc is word aligned
    mov_reg(e, R2, vreg);
    if (!c->j->slowInline) {  // the shared stubs (see call_slow_stub); the pipe one also appends to the write-gather pipe
        call_slow_stub(c, c->fastmem && c->j->gatherPut ? JF_STOREPIPE : JF_STORESLOW, sizePc);
        stub_status(c);
        stop_tail(c);
        return;
    }
    u32 pipeDone = NO_BRANCH;
    if (c->fastmem && c->j->gatherPut) {
        // The write-gather pipe (the graphics command stream, written a few bytes at a time): straight to the GP.
        load_const(e, R1, c->j->gatherEa);
        dp_reg(e, CMP, 1, 0, R0, R1);
        u32 notPipe = branch_c(e, C_NE, 0);
        u32 pipeFast = NO_BRANCH;
        if (c->j->gatherBuf) {
            // The common case inline (as gp_wgp_put's): the bytes fit before the 32-byte burst is complete, so they are
            // appended to the pipe's buffer and nothing else happens. A store that completes a burst calls the GP.
            load_const(e, R1, (u32)(uintptr_t)c->j->gatherLen);
            ldr_imm(e, R12, R1, 0);
            dp_imm(e, ADD, 0, R3, R12, size);
            dp_imm(e, CMP, 1, 0, R3, 32);
            u32 full = branch_c(e, C_CS, 0);
            str_imm(e, R3, R1, 0);
            load_const(e, R1, (u32)(uintptr_t)c->j->gatherBuf);
            dp_reg(e, ADD, 0, R1, R1, R12);
            for (u32 i = 0; i < size; i++) {  // console byte order
                u32 sh = 8 * (size - 1 - i);
                if (sh) { mov_shift(e, 0, R3, R2, LSR, (int)sh); strb_imm(e, R3, R1, i); }
                else strb_imm(e, R2, R1, i);
            }
            dp_imm(e, MOV, 0, R3, 0, 0);  // no stop
            pipeFast = branch_c(e, C_AL, 0);
            patch_branch(e, full, e->n);
        }
        push_regs(e, EA_SAVE);
        mov_reg(e, R0, RG);
        mov_reg(e, R1, R2);
        load_const(e, R2, sizePc);
        emit_time(c);
        call(c, (const void*)jit_gather);
        mov_reg(e, R3, R0);
        pop_regs(e, EA_SAVE);
        c->pendingStop = true;  // r3 != 0: leave once the instruction is done (stop_tail)
        if (pipeFast != NO_BRANCH) patch_branch(e, pipeFast, e->n);
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
    if (fault_ok(c) && vreg != R3) {  // (the slow path needs vreg unchanged: the byte swap goes to r3)
        u32 at;
        if (size == 4) { rev(e, R3, vreg); at = e->n; str_reg(e, R3, RRAM, R0); }
        else if (size == 2) { rev16(e, R3, vreg); at = e->n; strh_reg(e, R3, RRAM, R0); }
        else { at = e->n; strb_reg(e, vreg, RRAM, R0); }
        u32 resume = e->n;
        cold_begin(c);
        fault_site(c, at);
        store_slow(c, size, vreg);
        cold_end(c, resume);
        return;
    }
    u32 slow[2];
    int ri = ms_reg(vreg);
    if (c->j->memStubs && ri >= 0) {
        int k = size == 4 ? MS_S32 : size == 2 ? MS_S16 : MS_S8;
        branch_abs(e, C_AL, true, c->j->code + ((c->j->sched & SCHED_STUBS) ? c->j->memStub2[k][ri] : c->j->memStub[k][ri]));
        slow[0] = branch_c(e, C_CS, 0);
        slow[1] = NO_BRANCH;
    } else {
        mem1_check(c, size, slow);
        if (size == 4) { rev(e, R3, vreg); str_reg(e, R3, RRAM, R0); }
        else if (size == 2) { rev16(e, R3, vreg); strh_reg(e, R3, RRAM, R0); }
        else strb_reg(e, vreg, RRAM, R0);
    }
    u32 resume = e->n;
    cold_start(c, slow);
    store_slow(c, size, vreg);
    cold_end(c, resume);
}

static void emit_fallback(Cg* c, u32 inst) {
    Emit* e = &c->e;
    c->single = 0;  // the interpreter may write any FPR
#ifndef __3DS__
    g_rwWhy = RW_INTERP;
#endif
    rc_flush(c);  // the interpreter works on guest state in memory
#ifndef __3DS__
    g_rwWhy = RW_OTHER;
    if (!c->inCold && g_regStat) fs_reset();  // a float register cache would be forgotten here too
#endif
    Rc clean = c->rc;
    for (int k = 0; k < NSLOT; k++) clean.dirty[k] = false;
    if (c->inCold) {  // cold code moves when it is appended: no branches to absolute addresses
        mov_reg(e, R0, RG);
        load_const(e, R1, c->pc);
        load_const(e, R2, inst);
        emit_time(c);
        call(c, (const void*)jit_interp);
        dp_imm(e, CMP, 1, 0, R0, 0);
    } else {  // the shared stub: 4 words instead of about 20
        branch_abs(e, C_AL, true, stub(c, ST_INTERP));
        word(e, c->pc);
        word(e, inst);
        tfix_add(c, true);  // (instructions before this one) - (instructions in the block), patched at the block's end
        word(e, 0);
    }
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
            int cr = src(c, ID_CR, R0);  // after rc_flush: a slot loaded here is clean
            u32 bit = 1u << (31 - bi), enc;
            if (!arm_imm(bit, &enc)) enc = 0;
            dp_imm(e, TST, 1, 0, cr, enc);
            notTaken[nNot++] = branch_c(e, (bo & 8) ? C_EQ : C_NE, 0);  // want bit set: not taken if clear
        }
    }
    bool condSite = nNot == 1 && !indirect && !c->noLink;
#ifndef __3DS__
    if (g_regStat) condSite = false;  // the region model counts each exit with code in front of it
#endif
    if (condSite) {  // one condition: the branch itself becomes the taken side's link site (its condition inverted)
        e->buf[notTaken[0]] ^= 1u << 28;
        link_add(c, notTaken[0], target);
        exit_const(c, c->pc + 4);
        return;
    }
    if (indirect) exit_indirect(c); else exit_const(c, target);
    if (!nNot) return;
    for (int i = 0; i < nNot; i++) patch_branch(e, notTaken[i], e->n);
    exit_const(c, c->pc + 4);
}

// LR = the address after a branch with LK: a PC-relative load of a literal placed after the block's last branch (every
// branch ends the block with an unconditional one), one word where the constant took up to four (ARMv6 has no MOVW).
static u32 lr_set(Cg* c) {
    u32 at = c->e.n;
    ldr_imm(&c->e, R1, PC, 0);
    str_imm(&c->e, R1, RG, OFF_LR);
    return at;
}
static void lr_literal(Cg* c, u32 at) {
    Emit* e = &c->e;
    e->buf[at] |= (e->n - (at + 2)) * 4;  // PC reads 8 bytes ahead
    word(e, c->pc + 4);
}

static bool compile_branch(Cg* c, u32 inst) {
    u32 op = inst >> 26, lk = inst & 1;
#ifndef __3DS__
    if (g_regStat && !c->inCold) {  // the region model: the cache at the block's end, and why it is written back
        u32 xo = (inst >> 1) & 0x3FF;
        g_rxState = c->rc;
        g_rxHave = true;
        g_rwWhy = op == 19 && (xo == 16 || xo == 528) ? RW_IND : RW_CONST;
    }
#endif
    rc_flush(c);  // every path out of the block leaves guest state in memory (stores keep the ARM flags)
#ifndef __3DS__
    g_rwWhy = RW_OTHER;
#endif
    Emit* e = &c->e;
    if (op == 18) {  // b, bl, ba, bla
        s32 li = ((s32)(inst << 6)) >> 6 & ~3;
        u32 target = (inst & 2) ? (u32)li : c->pc + (u32)li;
        u32 lit = lk ? lr_set(c) : 0;
        exit_const(c, target);
        if (lk) lr_literal(c, lit);
        return true;
    }
    if (op == 16) {  // bc
        u32 bo = (inst >> 21) & 31, bi = (inst >> 16) & 31;
        s32 bd = (s32)(s16)(inst & 0xFFFC);
        u32 target = (inst & 2) ? (u32)bd : c->pc + (u32)bd;
        u32 lit = lk ? lr_set(c) : 0;
        emit_branch_cond(c, bo, bi, false, target);
        if (lk) lr_literal(c, lit);
        return true;
    }
    if (op == 19) {
        u32 xo = (inst >> 1) & 0x3FF, bo = (inst >> 21) & 31, bi = (inst >> 16) & 31;
        if (xo == 16 || xo == 528) {  // bclr / bcctr
            if (xo == 528 && !(bo & 4)) return false;  // invalid form: let the interpreter handle it
            ldr_imm(e, R2, RG, xo == 16 ? OFF_LR : OFF_CTR);   // read before LK overwrites LR
            dp_imm(e, BIC, 0, R2, R2, 3);
            u32 lit = lk ? lr_set(c) : 0;
            emit_branch_cond(c, bo, bi, true, 0);
            if (lk) lr_literal(c, lit);
            return true;
        }
        if (xo == 150) {  // isync: nothing to do here, but it ends the block (code may have changed before it)
            exit_const(c, c->pc + 4);
            return true;
        }
    }
    return false;
}

// ---- traces ----
// Level 2 blocks do not end at every branch. An unconditional b is followed: compilation goes on at its target, with
// the register cache and ARM flags intact and no exit at all. A conditional branch becomes a side exit: one host
// branch in the hot path to an out-of-line stub (which writes back the cache as it stood there and leaves through a link
// site, or the indirect table for blr / bctr), and compilation goes on with the instruction after it. Compiled code
// then spends its words on guest instructions instead of on a flush, two link sites and a reload per branch.
typedef struct {
    Jit* j;
    u32 nextPc, nextPa;   // where compilation goes on (the instruction after the last one, or a followed branch's target)
    u32 segPage;          // the 4 KB page of the current straight run: crossing it ends the trace
    u32 visited[24];      // the guest address of every run's first instruction (a trace never loops back into itself)
    u32 nVisited;
    u32 maxInsns;
} Trace;

// Conditional branch whose taken side leaves the trace: bc (target), bclr / bcctr (indirect through the Gekko word at
// offset indOff). The fall-through side continues in place. Not taken when the CTR / CR condition fails.
static void emit_branch_side(Cg* c, u32 bo, u32 bi, bool indirect, u32 target, u32 indOff) {
    Emit* e = &c->e;
    bool hasCtr = !(bo & 4), hasCr = !(bo & 16);
    int takenCond = C_AL, cr = -1;
    u32 skip = NO_BRANCH;
    bool fused = false;
    if (hasCr) {
        if (!hasCtr && (c->j->traceSide & TRACE_SIDE_FUSED) && c->flagsLive && (bi >> 2) == c->flagsFld && (bi & 3) != 3) {
            // The previous instruction was the compare that set this CR field and left its ARM flags: branch on them.
            static const int kSigned[3] = { C_LT, C_GT, C_EQ }, kUnsigned[3] = { C_CC, C_HI, C_EQ };
            int cond = (c->flagsSigned ? kSigned : kUnsigned)[bi & 3];
            if (jit_debug_plant == 3) cond ^= 1;  // the fuzzer's self-test: a wrong fused branch must be caught
            takenCond = (bo & 8) ? cond : cond ^ 1;  // ARM conditions invert by bit 0
            c->j->stats.fusedBranches++;
            fused = true;
        } else {
            cr = src(c, ID_CR, R0);  // before any branch over code: the cache state must not differ between the two paths
        }
    }
    if (hasCtr) {
        ldr_imm(e, R3, RG, OFF_CTR);
        dp_imm(e, SUB, 1, R3, R3, 1);
        str_imm(e, R3, RG, OFF_CTR);
        int ctrTaken = (bo & 2) ? C_EQ : C_NE;  // taken when CTR == 0 (bo & 2) / != 0
        if (hasCr) skip = branch_c(e, ctrTaken ^ 1, 0);  // the CTR condition fails: not taken, skip the CR test
        else takenCond = ctrTaken;
    }
    if (hasCr && !fused) {
        u32 bit = 1u << (31 - bi), enc;
        if (!arm_imm(bit, &enc)) enc = 0;
        dp_imm(e, TST, 1, 0, cr, enc);
        takenCond = (bo & 8) ? C_NE : C_EQ;  // wanted bit set: taken if it is set
    }
    exit_kind_if(c, takenCond, indirect ? EXIT_INDIRECT : EXIT_LINK, indirect ? indOff : target);
    if (skip != NO_BRANCH) patch_branch(e, skip, e->n);
    c->j->stats.traceSideExits++;
}

// Compiles a branch inside a trace if it can stay one; false when the branch ends the block (the caller then compiles
// it as before). Unconditional b to a place not in the trace yet is followed; bc and conditional bclr / bcctr (without
// LK, which would also write LR) become side exits.
static bool trace_branch(Cg* c, u32 inst, Trace* t) {
    Jit* j = t->j;
    Gekko* g = j->g;
    u32 op = inst >> 26, lk = inst & 1;
    if (lk || !c->trace || c->noLink) return false;
    if (op == 18) {  // b, ba
        if (!j->traceFollow) return false;
        s32 li = ((s32)(inst << 6)) >> 6 & ~3;
        u32 target = (inst & 2) ? (u32)li : c->pc + (u32)li;
        if (t->nVisited == sizeof t->visited / sizeof t->visited[0] || c->count + 1 >= t->maxInsns) return false;
        for (u32 k = 0; k < t->nVisited; k++) if (t->visited[k] == target) return false;
        u32 tpa;
        if (!gk_translate(g, target, false, false, &tpa, false) || tpa + 4 > j->ramSize || hle_is_hook(g, target)) return false;
        t->visited[t->nVisited++] = target;
        t->nextPc = target + (jit_debug_plant == 5 ? 4 : 0);  // plant 5 (the fuzzer self-test): a followed b skips an instruction
        t->nextPa = tpa + (jit_debug_plant == 5 ? 4 : 0);
        t->segPage = target >> 12;
        j->stats.traceFollowed++;
        return true;
    }
    u32 bo = (inst >> 21) & 31, bi = (inst >> 16) & 31;
    if (op == 16) {  // bc, bca
        if ((bo & 20) == 20) return false;  // branch always (no CTR, no CR test): ends the block like b
        if (!(j->traceSide & (bo & 4 ? TRACE_SIDE_CR : TRACE_SIDE_CTR))) return false;
        s32 bd = (s32)(s16)(inst & 0xFFFC);
        emit_branch_side(c, bo, bi, false, (inst & 2) ? (u32)bd : c->pc + (u32)bd, 0);
        return true;
    }
    if (op == 19) {
        u32 xo = (inst >> 1) & 0x3FF;
        if ((xo != 16 && xo != 528) || (bo & 20) == 20) return false;  // blr / bctr always: ends the block
        if (xo == 528 && !(bo & 4)) return false;                        // invalid form: the interpreter handles it
        if (!(j->traceSide & TRACE_SIDE_IND)) return false;
        emit_branch_side(c, bo, bi, true, 0, xo == 16 ? OFF_LR : OFF_CTR);
        return true;
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

// rd = rs >> n (logical); a plain move for n = 0 (LSR #0 encodes a shift by 32).
static void lsr_or_mov(Cg* c, int rd, int rs, u32 n) {
    if (n) mov_shift(&c->e, 0, rd, rs, LSR, (int)n); else mov_reg(&c->e, rd, rs);
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
    case 19: {  // CR logic (crand crandc creqv crnand crnor cror crorc crxor) and mcrf, on the cached CR
        u32 xo19 = (inst >> 1) & 0x3FF;
        if (xo19 == 0) {  // mcrf: field crfS to crfD
            u32 fd = rd >> 2, fs = ra >> 2;
            int cr = src(c, ID_CR, R1);
            if (fd == fs) return true;
            lsr_or_mov(c, R0, cr, 28 - 4 * fs);
            dp_imm(e, AND, 0, R0, R0, 15);
            int d = dst(c, ID_CR, R1);
            op_imm(c, BIC, d, cr, 0xFu << (28 - 4 * fd));
            if (fd < 7) dp_reg_c(e, C_AL, ORR, 0, d, d, R0, LSL, (int)(28 - 4 * fd));
            else dp_reg(e, ORR, 0, d, d, R0);
            dst_done(c, ID_CR, d);
            return true;
        }
        int opc, inv;  // the ARM op on bits a, b; inv: complement b first (1) or the result (2)
        switch (xo19) {
        case 257: opc = AND; inv = 0; break;  // crand
        case 129: opc = BIC; inv = 0; break;  // crandc: a & ~b
        case 289: opc = EOR; inv = 2; break;  // creqv
        case 225: opc = AND; inv = 2; break;  // crnand
        case 33: opc = ORR; inv = 2; break;   // crnor
        case 449: opc = ORR; inv = 0; break;  // cror
        case 417: opc = ORR; inv = 1; break;  // crorc: a | ~b
        case 193: opc = EOR; inv = 0; break;  // crxor
        default: return false;
        }
        int cr = src(c, ID_CR, R1);
        lsr_or_mov(c, R0, cr, 31 - ra);  // bit a in bit 0 (above it: other CR bits)
        lsr_or_mov(c, R2, cr, 31 - rb);
        if (inv == 1) dp_reg(e, MVN, 0, R2, 0, R2);
        dp_reg(e, opc, 0, R0, R0, R2);
        if (inv == 2) dp_reg(e, MVN, 0, R0, 0, R0);
        dp_imm(e, AND, 0, R0, R0, 1);
        int d = dst(c, ID_CR, R1);
        op_imm(c, BIC, d, cr, 1u << (31 - rd));
        if (rd < 31) dp_reg_c(e, C_AL, ORR, 0, d, d, R0, LSL, (int)(31 - rd));
        else dp_reg(e, ORR, 0, d, d, R0);
        dst_done(c, ID_CR, d);
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
        int x = src(c, ID_XER, R3);
        int xd = dst(c, ID_XER, R3);
        dp_imm(e, BIC, 0, xd, x, IMM(XER_CA));
        dp_reg_c(e, C_AL, ORR, 0, xd, xd, R2, LSL, 29);
        dst_done(c, ID_XER, xd);
        int d = dst(c, ra, R0);
        if (sh) mov_shift(e, 0, d, s, ASR, (int)sh); else mov_reg(e, d, s);
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
        if (spr == 1) {  // XER (cached)
            if (xo == 339) {
                int x = src(c, ID_XER, R1);
                int d = dst(c, rd, R0);
                mov_reg(e, d, x);
                dst_done(c, rd, d);
            } else {
                if (!c->j->soExact) return false;  // may set XER[SO]: the interpreter, which notices it (jit_interp_at)
                int s = src(c, rd, R0);
                load_const(e, R2, XER_MASK);
                int d = dst(c, ID_XER, R1);
                dp_reg(e, AND, 0, d, s, R2);
                dst_done(c, ID_XER, d);
            }
            return true;
        }
        u32 off = spr == 8 ? OFF_LR : spr == 9 ? OFF_CTR : 0;
        int base = RG;
        if (!off) {
            bool plain = spr == SPR_SRR0 || spr == SPR_SRR1 || spr == SPR_DAR || spr == SPR_DSISR || spr == SPR_HID0 ||
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
            str_imm(e, s, base, off);
        }
        return true;
    }
    case 19: {  // mfcr
        int cr = src(c, ID_CR, R1);
        int d = dst(c, rd, R0);
        mov_reg(e, d, cr);
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
        dp_imm(e, SUB, 0, R2, R1, IMM(0x80000000u));
        dp_imm(e, CMP, 1, 0, R2, IMM(0x01800000u));
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
        int cr = src(c, ID_CR, R1);
        load_const(e, R2, m);
        dp_reg(e, AND, 0, R3, s, R2);
        int d = dst(c, ID_CR, R1);
        dp_reg(e, BIC, 0, d, cr, R2);
        dp_reg(e, ORR, 0, d, d, R3);
        dst_done(c, ID_CR, d);
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
static void ld_fpr(Cg* c, int d, u32 fr, u32 lane) {
#ifndef __3DS__
    if (!c->inCold) fs_ld(fr, lane);
#endif
    vldr_d(&c->e, d, RG, OFF_FPR(fr) + 8u * lane);
}
static void st_fpr(Cg* c, int d, u32 fr, u32 lane) {
#ifndef __3DS__
    if (!c->inCold) fs_st(fr, lane);
#endif
    vstr_d(&c->e, d, RG, OFF_FPR(fr) + 8u * lane);
}

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
    if ((c->single >> (2 * fr + lane)) & 1) { ld_fpr(c, d, fr, lane); return; }  // an exact single: the cut changes nothing
    ldr_imm(e, R0, RG, OFF_FPR(fr) + 8u * lane);
    ldr_imm(e, R1, RG, OFF_FPR(fr) + 8u * lane + 4);
    dp_imm(e, ADD, 1, R0, R0, IMM(0x08000000u));
    dp_imm(e, ADC, 0, R1, R1, 0);
    dp_imm(e, AND, 0, R0, R0, IMM(0xF0000000u));
    vmov_d_rr(e, d, R0, R1);
}

// dst = the word stfs / psq_st store for the double in an FPR lane: the exponent and mantissa bits are cut, not rounded
// (PEM D.7, as the interpreter does); anything below the normal single range, zero included, becomes a signed zero.
static void cut_single_bits(Cg* c, int dst, u32 o, int t1, int t2) {
    Emit* e = &c->e;
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

// The same, fast for the usual case: a double that is exactly a single (zero and infinity included) converts to the
// same bits in hardware as by the cut. The round trip checks it; a NaN, extra mantissa bits or anything outside the
// normal single range (which the ARM11 also flushes to +0 where the cut keeps the sign) takes the cut, out of line.
// Uses d5-d7; clobbers the ARM flags.
static void dbl_to_single_bits(Cg* c, int dst, u32 fr, u32 lane, int t1, int t2) {
    Emit* e = &c->e;
    u32 o = OFF_FPR(fr) + 8u * lane;
    if (c->inCold) { cut_single_bits(c, dst, o, t1, t2); return; }  // no cold code inside cold code
    vldr_d(e, 7, RG, o);
    vcvt_f32_f64(e, 12, 7);  // s12 (in d6)
    vcvt_f64_f32(e, 5, 12);
    vcmp_d(e, 5, 7);
    vmrs_apsr(e);
    vmov_r_s(e, dst, 12);
    u32 slow = branch_c(e, C_NE, 0);
    u32 resume = e->n;
    cold_begin(c);
    fix_to_cold(c, slow);
    cut_single_bits(c, dst, o, t1, t2);
    cold_end(c, resume);
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
    dp_reg(e, ADD, 0, R1, R0, RRAM);
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
    dp_reg(e, ADD, 0, R1, R0, RRAM);
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
    if (kind == 2 && c->j->memStubs && c->fastmem && !c->inCold) {  // the shared stub (emit_float_stubs)
        ld_fpr(c, 1, fr, 0);
        if (indexed) ea_index(c, ra, rb); else ea_disp(c, ra, simm);
        set_tail(c, update ? 1 : 0, R0, ra, 0, 0);
        branch_abs(e, C_AL, true, c->j->code + c->j->stfsStub);
        u32 slow = branch_c(e, C_CS, 0);
        u32 resume = e->n;
        cold_begin(c);
        fix_to_cold(c, slow);
        store_slow(c, 4, R2);  // the stub left the value's bits in r2
        cold_end(c, resume);
        if (update) st_gpr(c, R0, ra);
        return true;
    }
    if (kind != 2 && c->j->memStubs && c->fastmem && !c->inCold) {  // lfs, lfd, stfd: the shared stubs
        if (kind == 3) ld_fpr(c, 1, fr, 0);
        if (indexed) ea_index(c, ra, rb); else ea_disp(c, ra, simm);
        set_tail(c, update ? 1 : 0, R0, ra, 0, 0);
        if (kind == 0) c->tailFpr = (int)fr;
        branch_abs(e, C_AL, true, c->j->code + (kind == 0 ? c->j->lfsStub : kind == 1 ? c->j->lfdStub : c->j->stfdStub));
        u32 slow = branch_c(e, C_CS, 0);
        u32 resume = e->n;
        if (kind == 0) { st_fpr(c, 1, fr, 0); st_fpr(c, 1, fr, 1); }
        else if (kind == 1) st_fpr(c, 1, fr, 0);
        cold_begin(c);
        fix_to_cold(c, slow);
        if (kind == 0) {  // the slow load's value in r2, then back to the stores
            load_slow(c, 4, false, R2);
            vmov_s_r(&c->e, 0, R2);
            vcvt_f64_f32(&c->e, 1, 0);
        } else if (kind == 1) {
            load64_slow(c, fr);  // the helper wrote the FPR: the stores below write it again
            ld_fpr(c, 1, fr, 0);
        } else {
            store64_slow(c, fr);
        }
        cold_end(c, resume);  // lfs / lfd: back to the FPR stores; stfd: past the call
        if (update) st_gpr(c, R0, ra);
        if (kind == 0) c->single |= 3ull << (2 * fr);
        return true;
    }
    if (kind == 2) dbl_to_single_bits(c, R2, fr, 0, R12, LR);
    if (indexed) ea_index(c, ra, rb); else ea_disp(c, ra, simm);
    switch (kind) {
    case 0:
        set_tail(c, update ? 1 : 0, R0, ra, 0, 0);
        c->tailFpr = (int)fr;
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
    if (kind == 0) c->single |= 3ull << (2 * fr);  // lfs: both lanes, exactly the loaded single
    return true;
}

// r2 = the value psq_st stores for an FPR lane under quantization qt / scale (host order, before the byte swap; the
// gather pipe takes the low sz bytes). Uses r12, lr, d0 and d2.
static void psq_store_value(Cg* c, u32 fr, u32 lane, u32 qt, int scale, bool sgn, u32 sz) {
    Emit* e = &c->e;
    if (qt == 0) { dbl_to_single_bits(c, R2, fr, lane, R12, LR); return; }
    ld_fpr(c, 0, fr, lane);
    vcvt_f32_f64(e, 0, 0);
    vcvt_f64_f32(e, 0, 0);
    if (scale != 0) {
        dp_imm(e, MOV, 0, R12, 0, 0);
        load_const(e, LR, (u32)(1023 + scale) << 20);
        vmov_d_rr(e, 2, R12, LR);
        vfp_d(e, VF_MUL, 0, 0, 2);
    }
    vcvt_s32_f64(e, 0, 0);
    vmov_r_s(e, R2, 0);
    if (sgn) ssat(e, R2, sz == 1 ? 8 : 16, R2); else usat(e, R2, sz == 1 ? 8 : 16, R2);
}

// psq_l / psq_st and their update / indexed forms, compiled for the GQR's quantization at compile time (plain float, or
// u8 / u16 / s8 / s16 with its scale); a run-time check of the GQR, misaligned elements or an address outside MEM1 run
// the interpreter for this one instruction. Quantized loads are exact (int, then times 2^-scale); quantized stores
// round to single, scale, truncate toward zero and saturate, as the interpreter's quantize() (NaN gives 0).
static bool compile_psq(Cg* c, u32 inst, bool store, bool indexed, bool update) {
    Emit* e = &c->e;
    if (!c->fastmem || !c->ps || (!indexed && !c->lsq)) return false;
    u32 fr = (inst >> 21) & 31, ra = (inst >> 16) & 31, rb = (inst >> 11) & 31;
    if (update && ra == 0) return false;
    u32 w = indexed ? (inst >> 10) & 1 : (inst >> 15) & 1, idx = indexed ? (inst >> 7) & 7 : (inst >> 12) & 7;
    u32 gqr = (u32)offsetof(Gekko, spr) + 4u * (SPR_GQR0 + idx);
    u32 q = c->j->g->spr[SPR_GQR0 + idx], half = (store ? q : q >> 16) & 0x3F07u;
    u32 qt = half & 7, qs = (half >> 8) & 0x3F;
    if (qt >= 1 && qt <= 3) qt = 0;  // reserved types: the guard below fails and the interpreter logs them
    int scale = (qs & 0x20) ? (int)qs - 64 : (int)qs;
    u32 sz = qt == 0 ? 4 : qt == 4 || qt == 6 ? 1 : 2;
    bool sgn = qt == 6 || qt == 7;
    u32 fb[4], nfb = 0;
#ifndef __3DS__
    if (g_regStat && !c->inCold) rs_emit_count(e, &g_psqRun[store][qt][w]);
#endif
    // The address first: the register cache must not change after the first branch to the cold fallback (that path
    // would skip the write-back of an evicted register).
    if (indexed) ea_index(c, ra, rb); else ea_disp(c, ra, (u32)((s32)(inst << 20) >> 20));
    if (qt == 0 && c->j->memStubs && !c->inCold) {  // plain float: the shared stub (emit_float_stubs)
        bool pipe = store && c->j->gatherPut;
        if (store) { ld_fpr(c, 1, fr, 0); if (!w) ld_fpr(c, 2, fr, 1); }
        branch_abs(e, C_AL, true, c->j->code + c->j->psqStub[store][idx][w]);
        u32 slow = branch_c(e, C_CS, 0);
        if (!store) { st_fpr(c, 1, fr, 0); st_fpr(c, 2, fr, 1); }
        if (update) st_gpr(c, R0, ra);  // the stub kept the effective address in r0
        u32 resume = e->n;
        cold_begin(c);
        fix_to_cold(c, slow);
        u32 toInterp[2], nInterp = 0;
        if (pipe) {  // outside MEM1 (r3 = 1) at the gather pipe's address: element by element through jit_gather
            dp_imm(e, CMP, 1, 0, R3, 1);
            toInterp[nInterp++] = branch_c(e, C_NE, 0);
            load_const(e, R1, c->j->gatherEa);
            dp_reg(e, CMP, 1, 0, R0, R1);
            toInterp[nInterp++] = branch_c(e, C_NE, 0);
            set_tail(c, update ? 1 : 0, R0, ra, 0, 0);
            u32 sizePc = c->pc | 2u;
            for (u32 lane = 0; lane < (w ? 1u : 2u); lane++) {
                psq_store_value(c, fr, lane, 0, 0, false, 4);
                push_regs(e, (1u << R0) | (1u << R3));
                mov_reg(e, R1, R2);
                mov_reg(e, R0, RG);
                load_const(e, R2, sizePc);
                emit_time(c);
                call(c, (const void*)jit_gather);
                mov_reg(e, R12, R0);
                pop_regs(e, (1u << R0) | (1u << R3));
                if (lane == 0) mov_reg(e, R3, R12); else dp_reg(e, ORR, 0, R3, R3, R12);
            }
            c->pendingStop = true;
            stop_tail(c);
            if (update) st_gpr(c, R0, ra);  // (cold: straight to memory)
            branch_c(e, C_AL, 0);
            add_fix(c, true, e->n - 1, false, resume);
        }
        for (u32 k = 0; k < nInterp; k++) patch_branch(e, toInterp[k], e->n);
        emit_fallback(c, inst);
        cold_end(c, resume);
        if (!store) c->single |= 3ull << (2 * fr);  // floats (or 1.0) loaded from singles
        return true;
    }
    add_const(e, R1, RG, gqr & ~0xFFFu, R12);
    ldr_imm(e, R2, R1, gqr & 0xFFFu);
    if (!store) mov_shift(e, 0, R2, R2, LSR, 16);
    if (qt == 0) {
        dp_imm(e, TST, 1, 0, R2, 7);  // plain float: the scale is not used
    } else {
        dp_imm(e, AND, 0, R1, R2, IMM(0x3F00u));
        dp_imm(e, CMP, 1, 0, R1, IMM(qs << 8));
        dp_imm_c(e, C_EQ, AND, 0, R1, R2, 7);
        dp_imm_c(e, C_EQ, CMP, 1, 0, R1, qt);
    }
    fb[nfb++] = branch_c(e, C_NE, 0);
    if (sz > 1) {
        dp_imm(e, TST, 1, 0, R0, sz - 1);
        fb[nfb++] = branch_c(e, C_NE, 0);
    }
    dp_imm(e, BIC, 0, R1, R0, IMM(0x40000000u));
    dp_imm(e, SUB, 0, R12, R1, IMM(0x80000000u));
    dp_imm(e, CMP, 1, 0, R12, IMM(0x01800000u));
    bool pipe = store && c->j->gatherPut;  // psq_st to the write-gather pipe: element by element, out of line
    u32 outside = branch_c(e, C_CS, 0);
    if (!pipe) fb[nfb++] = outside;
    if (!w) {  // the second element must be inside too
        dp_imm(e, ADD, 0, R3, R12, sz);
        dp_imm(e, CMP, 1, 0, R3, IMM(0x01800000u));
        fb[nfb++] = branch_c(e, C_CS, 0);
    }
    dp_reg(e, ADD, 0, R1, R1, RRAM);
    if (qt != 0 && scale != 0) {  // d2 = 2^-scale (load) or 2^scale (store)
        dp_imm(e, MOV, 0, R2, 0, 0);
        load_const(e, R3, (u32)(1023 + (store ? scale : -scale)) << 20);
        vmov_d_rr(e, 2, R2, R3);
    }
    if (!store && qt == 0) {
        if (w) {
            ldr_imm(e, R2, R1, 0);
            rev(e, R2, R2);
            vmov_s_r(e, 0, R2);
            vcvt_f64_f32(e, 1, 0);
            st_fpr(c, 1, fr, 0);
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
    } else if (!store) {
        for (u32 lane = 0; lane < (w ? 1u : 2u); lane++) {
            if (sz == 1) { ldrb_imm(e, R2, R1, lane); if (sgn) sxtb(e, R2, R2); }
            else { ldrh_imm(e, R2, R1, 2 * lane); if (sgn) revsh(e, R2, R2); else rev16(e, R2, R2); }
            vmov_s_r(e, 0, R2);
            vcvt_f64_s32(e, 1, 0);
            if (scale != 0) vfp_d(e, VF_MUL, 1, 1, 2);
            st_fpr(c, 1, fr, lane);
        }
    } else if (qt == 0) {
        dbl_to_single_bits(c, R2, fr, 0, R12, LR);
        rev(e, R2, R2);
        if (w) {
            str_imm(e, R2, R1, 0);
        } else {
            dbl_to_single_bits(c, R3, fr, 1, R12, LR);
            rev(e, R3, R3);
            stmia(e, R1, (1u << R2) | (1u << R3));
        }
    } else {
        u32 bits = sz == 1 ? 8 : 16;
        for (u32 lane = 0; lane < (w ? 1u : 2u); lane++) {
            ld_fpr(c, 0, fr, lane);
            vcvt_f32_f64(e, 0, 0);  // round to single first
            vcvt_f64_f32(e, 0, 0);
            if (scale != 0) vfp_d(e, VF_MUL, 0, 0, 2);
            vcvt_s32_f64(e, 0, 0);  // toward zero, saturating to int32; NaN gives 0
            vmov_r_s(e, R2, 0);
            if (sgn) ssat(e, R2, bits, R2); else usat(e, R2, bits, R2);
            if (sz == 1) strb_imm(e, R2, R1, lane);
            else { rev16(e, R2, R2); strh_imm(e, R2, R1, 2 * lane); }
        }
    }
    if (pipe) {
        // Outside MEM1 with the compiled quantization: the gather pipe's address takes each element through jit_gather
        // (r3 = its stop status, ORed over the elements); any other address joins the interpreter fallback below.
        u32 join = e->n;
        cold_begin(c);
        fix_to_cold(c, outside);
        load_const(e, R1, c->j->gatherEa);
        dp_reg(e, CMP, 1, 0, R0, R1);
        fb[nfb++] = branch_c(e, C_NE, 0);  // a cold-to-cold branch: fixed up with the fallback below
        set_tail(c, update ? 1 : 0, R0, ra, 0, 0);
        u32 sizePc = c->pc | (sz == 4 ? 2u : sz == 2 ? 1u : 0u);
        for (u32 lane = 0; lane < (w ? 1u : 2u); lane++) {
            psq_store_value(c, fr, lane, qt, scale, sgn, sz);
            push_regs(e, (1u << R0) | (1u << R3));
            mov_reg(e, R1, R2);
            mov_reg(e, R0, RG);
            load_const(e, R2, sizePc);
            emit_time(c);
            call(c, (const void*)jit_gather);
            mov_reg(e, R12, R0);
            pop_regs(e, (1u << R0) | (1u << R3));
            if (lane == 0) mov_reg(e, R3, R12); else dp_reg(e, ORR, 0, R3, R3, R12);
        }
        c->pendingStop = true;
        stop_tail(c);
        cold_end(c, join);
    }
    if (!store && w) {
        load_const(e, R2, 0x3FF00000u);  // ps1 = 1.0
        dp_imm(e, MOV, 0, R3, 0, 0);
        str_imm(e, R3, RG, OFF_FPR(fr) + 8);
        str_imm(e, R2, RG, OFF_FPR(fr) + 12);
    }
    if (update) st_gpr(c, R0, ra);  // the access kept the effective address in r0
    u32 resume = e->n;
    cold_begin(c);  // another quantization, misaligned or outside MEM1: the interpreter, out of line
    for (u32 k = 0; k < nfb; k++) {
        if (pipe && k == nfb - 1) add_fix(c, true, fb[k], true, c->e.n);  // from the pipe check, itself cold
        else fix_to_cold(c, fb[k]);
    }
    emit_fallback(c, inst);
    cold_end(c, resume);
    return true;
}

// d = 1 / lane of frB, or 1 / sqrt(it) when rsqrt: what the interpreter's fres / frsqrte estimates return (the
// correctly rounded reciprocal; 1 / sqrt in double, exact under power-of-two scaling, so equal for normal operands).
// Uses r0, r1 and d2 (d must not be d2).
static void emit_estimate(Cg* c, int d, u32 rb, u32 lane, bool rsqrt) {
    Emit* e = &c->e;
    ld_fpr(c, 2, rb, lane);
    if (rsqrt) vsqrt_d(e, 2, 2);
    dp_imm(e, MOV, 0, R0, 0, 0);
    load_const(e, R1, 0x3FF00000u);
    vmov_d_rr(e, d, R0, R1);
    vfp_d(e, VF_DIV, d, d, 2);
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
    int cr = src(c, ID_CR, R1);
    int d = dst(c, ID_CR, R1);
    arm_imm(0xFu << sh, &enc);
    dp_imm(e, BIC, 0, d, cr, enc);
    dp_reg_c(e, C_AL, ORR, 0, d, d, R0, LSL, (int)sh);
    dst_done(c, ID_CR, d);
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
    // muls0 / muls1 / madds0 / madds1 use one lane of frC for both lanes: its 25-bit cut is made once, into d7.
    bool sharedC = xo5 >= 12 && xo5 <= 15;
    if (sharedC) ld_c25(c, 7, fc, (xo5 == 12 || xo5 == 14) ? 0 : 1);
    int res[2], cr[2];
    for (u32 k = 0; k < 2; k++) {
        int d = (int)(4 * k);
        ld_fpr(c, d, fa, k);
        if (plain) ld_fpr(c, d + 1, fb, k);
        else if (!sharedC) ld_c25(c, d + 1, fc, k);
        cr[k] = sharedC ? 7 : d + 1;
        if (madd) ld_fpr(c, d + 2, fb, k);
        res[k] = madd ? d + 2 : d;
    }
    for (u32 k = 0; k < 2; k++) {
        int d = (int)(4 * k);
        if (plain) vfp_d(e, xo5 == 18 ? VF_DIV : xo5 == 20 ? VF_SUB : VF_ADD, d, d, cr[k]);
        else if (!madd) vfp_d(e, VF_MUL, d, d, cr[k]);
        else vfp_d(e, (xo5 == 28 || xo5 == 30) ? VF_NMLS : VF_MLA, d + 2, d, cr[k]);
        if (xo5 >= 30) vneg_d(e, res[k], res[k]);
    }
    round_pair(c, res[0], res[1]);
    st_fpr(c, res[0], fd, 0);
    st_fpr(c, res[1], fd, 1);
    c->single |= 3ull << (2 * fd);
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
        if ((single && xo5 == 24) || (!single && xo5 == 26)) {  // fres (single result in both lanes) / frsqrte (ps0)
            emit_estimate(c, 0, rb, 0, xo5 == 26);
            if (single) { round_pair(c, 0, -1); st_fpr(c, 0, frd, 0); st_fpr(c, 0, frd, 1); }
            else st_fpr(c, 0, frd, 0);
            return true;
        }
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
        if (single) { round_pair(c, res, -1); st_fpr(c, res, frd, 0); st_fpr(c, res, frd, 1); c->single |= 3ull << (2 * frd); }
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
        case 24: case 26:  // ps_res ps_rsqrte: each lane, rounded to single
            emit_estimate(c, 0, rb, 0, xo5 == 26);
            emit_estimate(c, 1, rb, 1, xo5 == 26);
            round_pair(c, 0, 1);
            st_fpr(c, 0, frd, 0);
            st_fpr(c, 1, frd, 1);
            return true;
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
#ifndef __3DS__
// Per guest opcode (gekko_op_key): instructions compiled and the host words they took in the hot path and in cold code,
// printed with the JIT statistics, so the opcodes that bloat compiled code can be found.
static u32 g_opN[65536], g_opHot[65536], g_opCold[65536];
static void op_words(u32 key, u32 hot, u32 cold) { g_opN[key]++; g_opHot[key] += hot; g_opCold[key] += cold; }
#endif

static u32 g_lastCount;  // the instruction count of the block compile_block made last (Entry.count)

static u32 fault_hash(u32 key) { return (key * 2654435761u) >> (32 - FAULT_BITS); }
static void fault_add(Jit* j, u32 key, u32 val) {
    u32 h = fault_hash(key);
    while (j->faultKey[h] != ~0u) h = (h + 1) & (FAULT_SLOTS - 1);
    j->faultKey[h] = key;
    j->faultVal[h] = val;
    j->faultN++;
}

static BlockFn compile_block(Jit* j, u32 pc, bool noLink) {
    Gekko* g = j->g;
    u32 pa;
    if (!gk_translate(g, pc, false, false, &pa, false) || pa >= j->ramSize) return NULL;
    if (j->coldStart - j->codeUsed < BLOCK_MAX_WORDS || j->codeWords - j->coldUsed < BLOCK_MAX_WORDS) {
        jit_invalidate_all(j);
        j->stats.flushes++;
    }
    static Cg cg;
    Cg* c = &cg;
    memset(c, 0, sizeof *c);
    c->e = (Emit){ j->code + j->codeUsed, 0, BLOCK_MAX_WORDS, false };
    c->alt = (Emit){ j->code + j->coldUsed, 0, BLOCK_MAX_WORDS, false };  // cold code is emitted where it stays
    c->flagsAt = NO_BRANCH;
    for (int k = 0; k < NSLOT; k++) c->rc.id[k] = -1;
    c->j = j;
    c->noLink = noLink;
    c->trace = j->traces && !noLink;
    c->fastmem = j->fastmem && (g->msr & MSR_DR) && (!(g->msr & MSR_PR) || j->fastmemUser);
    c->faultMem = c->fastmem && j->faultMem;
    c->fp = (g->msr & MSR_FP) && !(g->fpscr & FPSCR_RN);
    c->ps = c->fp && (g->spr[SPR_HID2] & HID2_PSE);
    c->lsq = (g->spr[SPR_HID2] & HID2_LSQE) != 0;
    c->pr = (g->msr & MSR_PR) != 0;
    Emit* e = &c->e;
    // Budget: the block's instructions come off RDC at its entry (the immediate is patched once the block's length is
    // known), and the block only runs while some budget is left; the dispatcher enters after these two words with the
    // count already taken off (JIT_PROLOGUE), so a run executes at most its budget plus one block. Exits are then plain
    // branches (exit_const); exits from the middle of the block give back the instructions it did not run.
    u32 budgetSub = e->n;
    dp_imm(e, SUB, 1, RDC, RDC, 0);
    u32 budgetBr = branch_c(e, C_LE, 0);
    // The sampling profiler's store, when asked for.
    if (j->profile) str_imm(e, PC, RG, OFF_HOSTPC);
#ifndef __3DS__
    if (g_regStat) {  // this block's execution counter (register-cache statistics)
        g_rsCur = j->nEntries;
        g_dopStart[g_rsCur] = g_dopUsed;
        g_dopLen[g_rsCur] = 0;
        fs_reset();
        g_rsSeen = g_rsEvicted = g_rsWritten = 0;
        g_rxHave = false;
        RsEntry* E = &g_rsE[g_rsCur];
        for (int k = 0; k < NSLOT_STAT; k++) E->id[k] = -1;
        E->readFirst = 0;
        E->written = 0;
        load_const(e, R12, (u32)(uintptr_t)&g_rsExec[g_rsCur]);
        ldr_imm(e, R0, R12, 0);
        dp_imm(e, ADD, 0, R0, R0, 1);
        str_imm(e, R0, R12, 0);
        if (g_proLen) { g_proLen[g_rsCur] = e->n; g_hotLen[g_rsCur] = 0; }
    }
#endif
    u32 startPa = pa;
    bool ended = false;
    if (hle_is_hook(g, pc)) {
        // A native stand-in starts here (hle.h): call it, take the guest instructions it stood in for off the budget,
        // and leave for the dispatcher at the PC it stopped at.
        mov_reg(e, R0, RG);
        load_const(e, R1, pc);
        mov_reg(e, R2, RDC);  // the budget left: the stand-in stops when it has used it
        call(c, (const void*)jit_hle);
        dp_reg(e, SUB, 0, RDC, RDC, R0);
        branch_abs(e, C_AL, false, stub(c, ST_DYN));
        ended = true;
    }
    u32 maxInsns = jit_debug_max_insns ? jit_debug_max_insns : j->traceMax ? j->traceMax : MAX_BLOCK_INSNS;
    if (maxInsns > MAX_BLOCK_INSNS) maxInsns = MAX_BLOCK_INSNS;
#ifndef __3DS__
    u32 prevInst = 0, prevHot = 0, prevCold = 0;
#endif
    Trace t = { j, pc, startPa, pc >> 12, { pc }, 1, maxInsns };
    for (u32 i = 0; i < maxInsns && !ended; i++) {
        c->pc = t.nextPc;
        u32 ipa = t.nextPa;
        if (i > 0 && hle_is_hook(g, c->pc)) break;  // a stand-in starts there: it gets a block of its own
        if ((c->pc >> 12) != t.segPage || ipa + 4 > j->ramSize) break;  // one 4 KB page per straight run, inside RAM
        u32 inst = ((u32)j->ram[ipa] << 24) | ((u32)j->ram[ipa + 1] << 16) | ((u32)j->ram[ipa + 2] << 8) | j->ram[ipa + 3];
        c->count = i + 1;
        c->gpc[i] = c->pc;
        c->gpa[i] = ipa;
        if ((ipa >> 12) < sizeof j->codePages) j->codePages[ipa >> 12] = 1;
        t.nextPc = c->pc + 4;  // a followed branch changes these
        t.nextPa = ipa + 4;
#ifndef __3DS__
        if (i > 0) { op_words(gekko_op_key(prevInst), e->n - prevHot, c->alt.n - prevCold); dop_add(gekko_op_key(prevInst), e->n - prevHot); }
        prevInst = inst; prevHot = e->n; prevCold = c->alt.n;
#endif
        // The ARM flags the previous instruction left: every path through an instruction's code reaches its end, so
        // flags its last emitted instruction set are there on all of them.
        c->flagsLive = i > 0 && c->flagsAt == e->n;
        {   // the destination FPR is not known single until its producer says so (conservative: any instruction of these
            // classes; op 31's FPR loads are among its other forms)
            u32 pop = inst >> 26;
            if (pop == 4 || pop == 31 || (pop >= 48 && pop <= 57) || pop == 59 || pop == 63) c->single &= ~(3ull << (2 * ((inst >> 21) & 31)));
        }
        c->pinned = 0;
        c->tailN = 0;
        c->tailFpr = -1;
        j->stats.instrsCompiled++;
        if (is_branch(inst)) {
            if (trace_branch(c, inst, &t)) { j->stats.instrsNative++; continue; }
            if (compile_branch(c, inst)) { j->stats.instrsNative++; ended = true; continue; }
        } else if (compile_int(c, inst) || compile_float(c, inst)) {
            j->stats.instrsNative++;
            if (c->pendingStop) e->overflow = true;  // a slow path without stop_tail: never compile it
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
#ifndef __3DS__
    if (c->count) { op_words(gekko_op_key(prevInst), e->n - prevHot, c->alt.n - prevCold); dop_add(gekko_op_key(prevInst), e->n - prevHot); }
#endif
    if (!ended) {  // ran out of instructions (or a page end)
#ifndef __3DS__
        if (g_regStat) { g_rxState = c->rc; g_rxHave = true; g_rwWhy = RW_CONST; }
#endif
        rc_flush(c);
#ifndef __3DS__
        g_rwWhy = RW_OTHER;
#endif
        exit_const(c, t.nextPc);
    }
#ifndef __3DS__
    if (g_regStat) { g_rsE[g_rsCur].written = g_rsWritten; g_rsE[g_rsCur].touched = g_rsSeen; g_rxHave = false; }
#endif
    u32 hotEnd = e->n;
#ifndef __3DS__
    if (g_regStat && g_hotLen) g_hotLen[g_rsCur] = hotEnd;
#endif
    u32 total = hle_is_hook(g, pc) ? 0 : c->count;  // a stand-in takes off what it ran itself
    e->buf[budgetSub] |= total;  // SUBS RDC, RDC, #total (at most MAX_BLOCK_INSNS: a plain immediate)
    for (u32 k = 0; k < c->nTfix; k++) {
        u32* w = c->tfix[k].cold ? &c->alt.buf[c->tfix[k].at] : &e->buf[c->tfix[k].at];
        if (c->tfix[k].literal) *w = (c->tfix[k].count - 1) - total;  // ST_INTERP: jitLeft = RDC - literal
        else *w |= total - (c->tfix[k].count - 1);  // the block's instructions not yet run at that instruction
    }
    // The block's stubs go to the cold region: they run only before a link is made, when the budget runs out, and on
    // the exits of slow paths, and in the hot region they spread every block's hot code over more cache lines.
    u32* hotBuf = e->buf;
    u32* coldBuf = c->alt.buf;
    u32 coldBefore = c->alt.n;
    cold_begin(c);
    e = &c->e;
    // Exit stubs: give back to RDC the instructions the block did not run, and leave.
    for (u32 k = 0; k < c->nExits; k++) {
        patch_to((c->exits[k].inCold ? coldBuf : hotBuf) + c->exits[k].at, e->buf + e->n);
        rc_emit_flush(c, &c->exits[k].snap);
        u32 back = total - c->exits[k].count + (c->exits[k].kind == EXIT_LINK && jit_debug_plant == 6 ? 1 : 0);  // plant 6: a side exit miscounts
        if (back) dp_imm(e, ADD, 0, RDC, RDC, back);
        if (c->exits[k].kind == EXIT_LINK) {  // a conditional branch out of a trace: a link site to the target block
            link_add(c, branch_c(e, C_AL, 0), c->exits[k].pc);
            continue;
        }
        if (c->exits[k].kind == EXIT_INDIRECT) {  // blr / bctr taken: through the indirect table to the address in LR / CTR
            ldr_imm(e, R2, RG, c->exits[k].pc);
            dp_imm(e, BIC, 0, R2, R2, 3);
            str_imm(e, R2, RG, OFF_PC);
            branch_abs(e, C_AL, false, stub(c, ST_INDIRECT));
            continue;
        }
        if (c->exits[k].setPc) { load_const(e, R1, c->exits[k].pc); str_imm(e, R1, RG, OFF_PC); }
        branch_abs(e, C_AL, false, stub(c, ST_DYN));
    }
    // Link stubs: ST_LINK with the target's guest address and the site to patch.
    for (u32 k = 0; k < c->nLinks; k++) {
        patch_to(c->links[k].site, e->buf + e->n);
        branch_abs(e, C_AL, true, stub(c, ST_LINK));
        word(e, c->links[k].target);
        word(e, (u32)(uintptr_t)c->links[k].site);
    }
    // No budget left at the entry: give the block's count back and leave with g->pc = the block.
    patch_to(hotBuf + budgetBr, e->buf + e->n);
    if (total) dp_imm(e, ADD, 0, RDC, RDC, total);
    branch_abs(e, C_AL, true, stub(c, ST_BUDGET));
    word(e, pc);
    u32 exitWords = e->n - coldBefore;
    swap_emit(c);
    e = &c->e;

    // The cold code goes to the cold region; then every branch between hot and cold gets its offset (base: the cold
    // code's word index relative to this block's hot code).
    if (c->alt.overflow || e->overflow) {
        j->stats.compileFails[c->alt.overflow ? 1 : c->nFix == sizeof c->fix / sizeof c->fix[0] ? 2 : 0]++;
        return NULL;
    }
    u32 base = j->coldUsed - j->codeUsed;
    u32* cold = j->code + j->coldUsed;
    for (u32 k = 0; k < c->nFix; k++)
        patch_branch(e, c->fix[k].fromCold ? base + c->fix[k].at : c->fix[k].at,
                     c->fix[k].toCold ? base + c->fix[k].target : c->fix[k].target);
    for (u32 k = 0; k < c->nFsite; k++) fault_add(j, j->codeUsed + c->fsite[k].at, j->coldUsed + c->fsite[k].cold);
    void* fn = j->code + j->codeUsed;
    jit_debug_last_code = fn;
    jit_debug_last_words = e->n;
    if (jit_debug_on_compile) jit_debug_on_compile(fn, e->n);
#ifndef __3DS__
    {  // JIT_DUMP=pc (hex, ARM Linux build): the block's hot words, its exit stubs and its cold words, for objdump
        static int want = -1, big;  // JIT_DUMP=big: the first 8 blocks of 16 or more guest instructions
        static u32 dumpPc;
        if (want < 0) {
            const char* s = getenv("JIT_DUMP");
            want = s != NULL;
            big = s && !strcmp(s, "big") ? 8 : 0;
            dumpPc = s && !big ? (u32)strtoul(s, NULL, 16) : 0;
        }
        if (want && (big ? c->count >= 16 && big-- > 0 : pc == dumpPc)) {
            fprintf(stderr, "JIT_DUMP pc %08x guest %u hot %u exit %u cold %u\n", pc, c->count, hotEnd, exitWords, c->alt.n);
            for (u32 k = 0; k < c->count && c->gpa[k] + 4 <= j->ramSize; k++) {
                const u8* w = j->ram + c->gpa[k];
                char dis[96];
                gekko_disasm(c->gpc[k], ((u32)w[0] << 24) | ((u32)w[1] << 16) | ((u32)w[2] << 8) | w[3], dis, sizeof dis);
                fprintf(stderr, "JIT_GUEST %08x %s\n", c->gpc[k], dis);
            }
            for (u32 k = 0; k < e->n; k++) fprintf(stderr, "JIT_HOT %08x\n", ((const u32*)fn)[k]);
            for (u32 k = 0; k < c->alt.n; k++) fprintf(stderr, "JIT_COLD %08x\n", cold[k]);
        }
    }
#endif
    jit_host_sync_icache(fn, e->n * 4);
    if (c->alt.n) jit_host_sync_icache(cold, c->alt.n * 4);
    j->codeUsed += e->n;
    j->coldUsed += c->alt.n;
    for (u32 a = startPa >> 12; a <= (startPa + 4 * MAX_BLOCK_INSNS) >> 12 && a < sizeof j->codePages; a++) j->codePages[a] = 1;
    j->stats.blocksCompiled++;
    j->insnsSinceFlush += c->count;
    j->stats.wordsHot += hotEnd;
    j->stats.wordsExit += exitWords;

    j->stats.wordsCold += c->alt.n;
    j->stats.insnsKept += c->count;
    g_lastCount = total;
    return (BlockFn)fn;
}

static BlockFn compile(Jit* j, u32 pc, bool noLink) {
    u64 t0 = prof_begin();
    BlockFn fn = compile_block(j, pc, noLink);
    prof_end(PROF_JITC, t0);
    return fn;
}

// The shared stubs, at the start of the code buffer. Compiled code keeps RG, RRAM and RDC in registers
// from ST_ENTER until it leaves through ST_EXIT, which returns r0 to the dispatcher (a link site or 0) and stores
// the budget left in *left.
//   ST_ENTER (g, fn, budget, left): saves the C registers, sets up the block registers and jumps to fn.
//   ST_EXIT: r0 = link site or 0.
//   ST_LINK: reached with "bl" from a link stub whose next words are the target guest PC and the address of the site
//     to patch (the hot path's branch to that stub); writes g->pc and returns the site.
//   ST_DYN: g->pc is already written; returns 0.
//   ST_BUDGET: as ST_LINK's entry (literal = the block's own PC), but returns 0.
//   ST_INDIRECT: r2 = target (g->pc already written): jumps to it through the table, or returns 0.
// The memory-access stubs, reached with "bl" from a load or store whose guest address is in r0: emit_load's and
// emit_store's fast path (aligned, inside MEM1 through 0x80000000 / 0xC0000000) shared instead of inlined at every
// site, which took ~9 words each. Core 0 spent 44% of its cycles waiting for instructions on Twilight Princess's title
// scene (2DS, 862fe63), about one instruction-cache miss per two guest instructions, so code size per guest
// instruction is what to cut; a stub costs a predicted call and return. They return with the carry flag clear after
// the access, or set (access not done) for the site's out-of-line slow path, with r0 kept; they clobber r1, r3 and lr.
static void cond_last(Emit* e, int cond) { e->buf[e->n - 1] = (e->buf[e->n - 1] & 0x0FFFFFFFu) | cc(cond); }
static void emit_mem_stubs(Jit* j, Emit* e) {
    for (int k = 0; k < MS_KINDS; k++) {
        for (int ri = 0; ri < MS_REGS; ri++) {
            int r = ri == 0 ? R2 : R4 + ri - 1;
            u32 size = k == MS_L32 || k == MS_S32 ? 4 : k == MS_L8 || k == MS_S8 ? 1 : 2;
            j->memStub[k][ri] = (u16)e->n;
            // Alignment and range in one compare (see mem1_check); a failure returns with the carry set.
            u32 sh = size == 4 ? 2 : size == 2 ? 1 : 0;
            dp_imm(e, SUB, 0, R1, R0, IMM(0x80000000u));
            if (sh) {
                mov_shift(e, 0, R12, R1, ROR, (int)sh);
                dp_imm(e, CMP, 1, 0, R12, IMM(0x01800000u >> sh));
            } else {
                dp_imm(e, CMP, 1, 0, R1, IMM(0x01800000u));
            }
            bx_reg_c(e, C_CS, LR);  // misaligned, outside MEM1 or the uncached mirror
            switch (k) {
            case MS_L32: ldr_reg(e, r, RRAM, R0); rev(e, r, r); break;
            case MS_L16U: ldrh_reg(e, r, RRAM, R0); rev16(e, r, r); break;
            case MS_L16S: ldrh_reg(e, r, RRAM, R0); revsh(e, r, r); break;
            case MS_L8: ldrb_reg(e, r, RRAM, R0); break;
            case MS_S32: rev(e, R3, r); str_reg(e, R3, RRAM, R0); break;
            case MS_S16: rev16(e, R3, r); strh_reg(e, R3, RRAM, R0); break;
            default: strb_reg(e, r, RRAM, R0); break;
            }
            bx_reg_c(e, C_AL, LR);  // carry clear from the range check
        }
    }
    // The scheduled set (SCHED_STUBS), from the ARM11 interlock model (tools/arm11_sched.py: loads were 63% of TP's
    // modelled interlock cycles, lwz alone 17.5%). The range check reads r0, which the call's address computation made
    // ready, instead of the SUB's fresh result as a shifted operand (one cycle): r0 - 0x80000000 is r0 with bit 31
    // flipped, and rotation distributes over EOR. A load leaves its byte swap to the site, after the site's branch to
    // its slow path, so the swap no longer waits on the load (two cycles). The access is conditional and there is one
    // return: carry clear, done; set, not done (r0 kept).
    for (int k = 0; k < MS_KINDS; k++) {
        for (int ri = 0; ri < MS_REGS; ri++) {
            int r = ri == 0 ? R2 : R4 + ri - 1;
            u32 size = k == MS_L32 || k == MS_S32 ? 4 : k == MS_L8 || k == MS_S8 ? 1 : 2;
            u32 sh = size == 4 ? 2 : size == 2 ? 1 : 0;
            j->memStub2[k][ri] = (u16)e->n;
            if (sh) {
                mov_shift(e, 0, R12, R0, ROR, (int)sh);
                dp_imm(e, EOR, 0, R12, R12, IMM(0x80000000u >> sh));
            } else {
                dp_imm(e, EOR, 0, R12, R0, IMM(0x80000000u));
            }
            dp_imm(e, CMP, 1, 0, R12, IMM(0x01800000u >> sh));
            switch (k) {
            case MS_L32: ldr_reg(e, r, RRAM, R0); break;
            case MS_L16U: case MS_L16S: ldrh_reg(e, r, RRAM, R0); break;
            case MS_L8: ldrb_reg(e, r, RRAM, R0); break;
            case MS_S32: rev(e, R3, r); cond_last(e, C_CC); str_reg(e, R3, RRAM, R0); break;
            case MS_S16: rev16(e, R3, r); cond_last(e, C_CC); strh_reg(e, R3, RRAM, R0); break;
            default: strb_reg(e, r, RRAM, R0); break;
            }
            cond_last(e, C_CC);
            bx_reg_c(e, C_AL, LR);
        }
    }
}

// The shared float stubs. Twilight Princess's float loads and stores took ~19% of executed hot code (qemu, JIT_REGSTAT:
// psq_l 24 words, psq_st 31, stfs 10 per site), most of it the same checks and conversions at every site; as with the
// memory-access stubs, a site now keeps a call and a branch to its slow path, and core 0's instruction fetch has less
// to fetch. All return with the carry clear when done, set when the site's slow path must run (r0 kept); they keep
// r4-r11 and clobber r1-r3, r12, lr, d0-d7 and the flags.
//   psq_l (plain-float GQR q, W): r0 = ea; checks the GQR's load type, alignment and MEM1, loads both elements (or one,
//     and 1.0) into d1, d2.
//   psq_st (plain-float GQR q, W): r0 = ea, d1 (and d2) the values; converts them as stfs does (dbl_to_single_bits),
//     checks the GQR's store type, alignment and MEM1, stores. On failure r3 = 1 when only the address was outside
//     MEM1 (the gather pipe may take it), else 0.
//   stfs: r0 = ea, d1 the value; converts it into r2 (also on failure, for the site's slow path), checks, stores.
//   lfs: r0 = ea; checks, loads the single into d1 as a double. lfd: r0 = ea; checks (8-byte alignment), loads d1.
//   stfd: r0 = ea, d1 the value; checks (8-byte alignment), stores.
// The cut for values that are not exactly singles is shared (cut_d1_r2 / cut_d2_r3): r1 and r12 are its temporaries.
static void fs_cut(Emit* e, int dst, int dsrc) {  // dst = cut_single_bits of d<dsrc>; uses r1, r12; returns with bx lr
    vmov_rr_d(e, R1, R12, dsrc);  // r1 = low word, r12 = high word
    mov_shift(e, 0, dst, R1, LSR, 29);
    dp_reg_c(e, C_AL, ORR, 0, dst, dst, R12, LSL, 3);
    dp_imm(e, BIC, 0, dst, dst, IMM(0xC0000000u));
    dp_imm(e, AND, 0, R1, R12, IMM(0xC0000000u));
    dp_reg(e, ORR, 0, dst, dst, R1);
    dp_imm(e, BIC, 0, R1, R12, IMM(0x80000000u));  // |x| below 2^-126: signed zero
    dp_imm(e, SUB, 1, R1, R1, IMM(0x38000000u));
    dp_imm_c(e, C_CS, CMP, 1, 0, R1, IMM(0x00100000u));
    dp_imm(e, AND, 0, R12, R12, IMM(0x80000000u));
    dp_reg_c(e, C_CC, MOV, 0, dst, 0, R12, LSL, 0);
    bx_reg_c(e, C_AL, LR);
}

// dst = d<dsrc> as single bits: in hardware when the double is exactly a single, else through the shared cut (cutAt).
// Uses s12 / d5 (dsrc 1) or s14 / d5 (dsrc 2).
static void fs_conv(Emit* e, int dst, int dsrc, u32 cutAt) {
    int sTmp = dsrc == 1 ? 12 : 14;
    vcvt_f32_f64(e, sTmp, dsrc);
    vcvt_f64_f32(e, 5, sTmp);
    vcmp_d(e, 5, dsrc);
    vmrs_apsr(e);
    vmov_r_s(e, dst, sTmp);
    u32 slow = branch_c(e, C_NE, 0);
    u32 back = e->n;
    u32 skip = branch_c(e, C_AL, 0);  // the slow call, out of the way of the fast path
    patch_branch(e, slow, e->n);
    push_regs(e, (1u << R1) | (1u << LR));
    branch_abs(e, C_AL, true, e->buf + cutAt);
    pop_regs(e, (1u << R1) | (1u << LR));
    branch_c(e, C_AL, back);  // back to the branch over this slow call
    patch_branch(e, skip, e->n);
}

static void fs_gqr(Jit* j, Emit* e, u32 q) {  // r1 = GQR q
    u32 off = (u32)offsetof(Gekko, spr) + 4u * (SPR_GQR0 + q);
    add_const(e, R1, RG, off & ~0xFFFu, R12);
    ldr_imm(e, R1, R1, off & 0xFFFu);
    (void)j;
}

static void emit_float_stubs(Jit* j, Emit* e) {
    u32 cut1 = e->n;
    fs_cut(e, R2, 1);
    u32 cut2 = e->n;
    fs_cut(e, R3, 2);
    u32 fail = e->n;  // carry set, r3 = 0
    dp_imm(e, MOV, 0, R3, 0, 0);
    dp_imm(e, CMP, 1, 0, R0, 0);
    bx_reg_c(e, C_AL, LR);
    u32 failOut = e->n;  // carry set, r3 = 1 (only outside MEM1)
    dp_imm(e, MOV, 0, R3, 0, 1);
    dp_imm(e, CMP, 1, 0, R0, 0);
    bx_reg_c(e, C_AL, LR);
    for (u32 q = 0; q < 8; q++)
        for (u32 w = 0; w < 2; w++) {
            // psq_l
            j->psqStub[0][q][w] = (u16)e->n;
            fs_gqr(j, e, q);
            dp_imm(e, TST, 1, 0, R1, IMM(0x70000u));  // the load half's type: plain float
            branch_c(e, C_NE, fail);
            dp_imm(e, TST, 1, 0, R0, 3);
            branch_c(e, C_NE, fail);
            dp_imm(e, BIC, 0, R1, R0, IMM(0x40000000u));
            dp_imm(e, SUB, 0, R12, R1, IMM(0x80000000u));
            dp_imm(e, CMP, 1, 0, R12, IMM(0x01800000u));
            branch_c(e, C_CS, fail);
            if (!w) {
                dp_imm(e, ADD, 0, R3, R12, 4);
                dp_imm(e, CMP, 1, 0, R3, IMM(0x01800000u));
                branch_c(e, C_CS, fail);
            }
            dp_reg(e, ADD, 0, R1, R1, RRAM);
            if (w) {
                ldr_imm(e, R2, R1, 0);
                rev(e, R2, R2);
                vmov_s_r(e, 0, R2);
                vcvt_f64_f32(e, 1, 0);
                dp_imm(e, MOV, 0, R2, 0, 0);
                load_const(e, R3, 0x3FF00000u);
                vmov_d_rr(e, 2, R2, R3);  // ps1 = 1.0
            } else {
                ldmia(e, R1, (1u << R2) | (1u << R3));
                rev(e, R2, R2);
                rev(e, R3, R3);
                vmov_ss_rr(e, 0, R2, R3);
                vcvt_f64_f32(e, 1, 0);
                vcvt_f64_f32(e, 2, 1);
            }
            bx_reg_c(e, C_AL, LR);  // carry clear from the range check
            // psq_st
            j->psqStub[1][q][w] = (u16)e->n;
            fs_gqr(j, e, q);
            dp_imm(e, TST, 1, 0, R1, 7);  // the store half's type: plain float
            branch_c(e, C_NE, fail);
            dp_imm(e, TST, 1, 0, R0, 3);
            branch_c(e, C_NE, fail);
            fs_conv(e, R2, 1, cut1);
            if (!w) fs_conv(e, R3, 2, cut2);
            dp_imm(e, BIC, 0, R1, R0, IMM(0x40000000u));
            dp_imm(e, SUB, 0, R12, R1, IMM(0x80000000u));
            dp_imm(e, CMP, 1, 0, R12, IMM(0x01800000u));
            branch_c(e, C_CS, failOut);
            if (!w) {
                dp_imm(e, ADD, 0, R12, R12, 4);
                dp_imm(e, CMP, 1, 0, R12, IMM(0x01800000u));
                branch_c(e, C_CS, failOut);
            }
            dp_reg(e, ADD, 0, R1, R1, RRAM);
            rev(e, R2, R2);
            if (w) str_imm(e, R2, R1, 0);
            else { rev(e, R3, R3); stmia(e, R1, (1u << R2) | (1u << R3)); }
            bx_reg_c(e, C_AL, LR);
        }
    // stfs
    j->stfsStub = (u16)e->n;
    fs_conv(e, R2, 1, cut1);
    dp_imm(e, TST, 1, 0, R0, 3);
    branch_c(e, C_NE, fail);  // (r3 is not part of stfs's protocol)
    dp_imm(e, BIC, 0, R1, R0, IMM(0x40000000u));
    dp_imm(e, SUB, 0, R12, R1, IMM(0x80000000u));
    dp_imm(e, CMP, 1, 0, R12, IMM(0x01800000u));
    branch_c(e, C_CS, fail);
    rev(e, R3, R2);
    str_reg(e, R3, RRAM, R1);
    bx_reg_c(e, C_AL, LR);
    // lfs
    j->lfsStub = (u16)e->n;
    dp_imm(e, TST, 1, 0, R0, 3);
    branch_c(e, C_NE, fail);
    dp_imm(e, BIC, 0, R1, R0, IMM(0x40000000u));
    dp_imm(e, SUB, 0, R12, R1, IMM(0x80000000u));
    dp_imm(e, CMP, 1, 0, R12, IMM(0x01800000u));
    branch_c(e, C_CS, fail);
    ldr_reg(e, R2, RRAM, R1);
    rev(e, R2, R2);
    vmov_s_r(e, 0, R2);
    vcvt_f64_f32(e, 1, 0);
    bx_reg_c(e, C_AL, LR);
    // lfd (FPR words: low word first)
    j->lfdStub = (u16)e->n;
    dp_imm(e, TST, 1, 0, R0, 7);
    branch_c(e, C_NE, fail);
    dp_imm(e, BIC, 0, R1, R0, IMM(0x40000000u));
    dp_imm(e, SUB, 0, R12, R1, IMM(0x80000000u));
    dp_imm(e, CMP, 1, 0, R12, IMM(0x01800000u));  // 8-byte aligned and below the end: all 8 bytes inside
    branch_c(e, C_CS, fail);
    dp_reg(e, ADD, 0, R1, R1, RRAM);
    ldmia(e, R1, (1u << R2) | (1u << R3));
    rev(e, R2, R2);
    rev(e, R3, R3);
    vmov_d_rr(e, 1, R3, R2);
    bx_reg_c(e, C_AL, LR);
    // stfd
    j->stfdStub = (u16)e->n;
    dp_imm(e, TST, 1, 0, R0, 7);
    branch_c(e, C_NE, fail);
    dp_imm(e, BIC, 0, R1, R0, IMM(0x40000000u));
    dp_imm(e, SUB, 0, R12, R1, IMM(0x80000000u));
    dp_imm(e, CMP, 1, 0, R12, IMM(0x01800000u));
    branch_c(e, C_CS, fail);
    vmov_rr_d(e, R3, R2, 1);  // r3 = low word, r2 = high word
    rev(e, R2, R2);
    rev(e, R3, R3);
    dp_reg(e, ADD, 0, R1, R1, RRAM);
    stmia(e, R1, (1u << R2) | (1u << R3));
    bx_reg_c(e, C_AL, LR);
}

static void emit_stubs(Jit* j) {
    Emit em = { j->code, 0, ST_WORDS, false };
    Emit* e = &em;
    j->stub[ST_ENTER] = e->n;
    push_regs(e, 0x4FF8);  // {r3-r11, lr}: 10 registers keep the stack 8-byte aligned for C calls
    mov_reg(e, RG, R0);
    load_const(e, RRAM, (u32)(uintptr_t)j->ram - 0x80000000u);
    mov_reg(e, RDC, R2);
    bx_reg_c(e, C_AL, R1);
    j->stub[ST_EXIT] = e->n;
    ldr_imm(e, R3, SP, 0);  // the saved r3: `left`
    str_imm(e, RDC, R3, 0);
    pop_regs(e, 0x8FF8);    // {r3-r11, pc}
    j->stub[ST_LINK] = e->n;
    ldr_imm(e, R1, LR, 0);
    str_imm(e, R1, RG, OFF_PC);
    ldr_imm(e, R0, LR, 4);
    branch_c(e, C_AL, j->stub[ST_EXIT]);
    j->stub[ST_DYN] = e->n;
    dp_imm(e, MOV, 0, R0, 0, 0);
    branch_c(e, C_AL, j->stub[ST_EXIT]);
    j->stub[ST_BUDGET] = e->n;
    ldr_imm(e, R1, LR, 0);
    str_imm(e, R1, RG, OFF_PC);
    dp_imm(e, MOV, 0, R0, 0, 0);
    branch_c(e, C_AL, j->stub[ST_EXIT]);
    j->stub[ST_INDIRECT] = e->n;  // (the target block checks the budget at its entry)
    mov_shift(e, 0, R1, R2, LSL, 30 - FAST_BITS);  // fast_index: (pc >> 2) & (FAST_SIZE - 1)
    mov_shift(e, 0, R1, R1, LSR, 32 - FAST_BITS);
    ldr_imm(e, R12, RG, OFF_FASTBASE);  // the running key's table (fast_select)
    dp_reg_c(e, C_AL, ADD, 0, R12, R12, R1, LSL, 3);
    ldmia(e, R12, (1u << R0) | (1u << R1));
    dp_reg(e, CMP, 1, 0, R0, R2);
    bx_reg_c(e, C_EQ, R1);
    dp_imm(e, MOV, 0, R0, 0, 1);  // 1: a miss in the table (the dispatcher counts it)
    branch_c(e, C_AL, j->stub[ST_EXIT]);
    // ST_INTERP: reached with "bl" followed by three literals: guest pc, instruction, and the block's instructions before it.
    // Runs the instruction in the interpreter at the exact guest time and returns past the literals with the flags of
    // "cmp r0, #0" (non-zero: leave compiled code; jit_interp has written g->pc).
    j->stub[ST_INTERP] = e->n;
    push_regs(e, (1u << R3) | (1u << LR));
    ldr_imm(e, R1, LR, 0);
    ldr_imm(e, R2, LR, 4);
    ldr_imm(e, R3, LR, 8);
    dp_reg(e, SUB, 0, R3, RDC, R3);
    str_imm(e, R3, RG, OFF_JITLEFT);
    mov_reg(e, R0, RG);
    load_const(e, R12, (u32)(uintptr_t)jit_interp);
    blx_reg(e, R12);
    pop_regs(e, (1u << R3) | (1u << LR));
    dp_imm(e, ADD, 0, LR, LR, 12);
    dp_imm(e, CMP, 1, 0, R0, 0);
    bx_reg_c(e, C_AL, LR);
    // ST_LOADSLOW: r0 = ea, literal = pc | log2(size). jit_load(g, ea, size, pc); returns past the literal with the value
    // in r2, the status in r3 (Gekko.jitStatus, cleared) and r0 kept. r1, r12 clobbered.
    j->stub[ST_LOADSLOW] = e->n;
    push_regs(e, (1u << R0) | (1u << R1) | (1u << R4) | (1u << LR));  // four registers: the stack stays 8-byte aligned
    ldr_imm(e, R3, LR, 0);
    mov_reg(e, R1, R0);
    mov_reg(e, R0, RG);
    dp_imm(e, AND, 0, R2, R3, 3);
    dp_imm(e, MOV, 0, R12, 0, 1);
    dp_rsr(e, MOV, 0, R2, 0, R12, LSL, R2);  // size = 1 << log2
    dp_imm(e, BIC, 0, R3, R3, 3);
    ldr_imm(e, R12, RG, OFF_JITFN(0));       // jit_load
    blx_reg(e, R12);
    mov_reg(e, R2, R0);
    u32 slowRet = e->n;  // shared tail: status to r3, restore, return past the literal
    ldr_imm(e, R3, RG, OFF_JITSTATUS);
    dp_imm(e, MOV, 0, R1, 0, 0);
    str_imm(e, R1, RG, OFF_JITSTATUS);
    u32 popRet = e->n;
    pop_regs(e, (1u << R0) | (1u << R1) | (1u << R4) | (1u << LR));
    dp_imm(e, ADD, 0, LR, LR, 4);
    bx_reg_c(e, C_AL, LR);
    // ST_STORESLOW: r0 = ea, r2 = value, literal = pc | log2(size). jit_store(g, ea, value, sizePc); returns as above.
    j->stub[ST_STORESLOW] = e->n;
    push_regs(e, (1u << R0) | (1u << R1) | (1u << R4) | (1u << LR));
    u32 storeBody = e->n;
    ldr_imm(e, R3, LR, 0);
    mov_reg(e, R1, R0);
    mov_reg(e, R0, RG);
    ldr_imm(e, R12, RG, OFF_JITFN(1));       // jit_store
    blx_reg(e, R12);
    branch_c(e, C_AL, slowRet);
    // ST_STOREPIPE: ST_STORESLOW, but a store to the write-gather pipe's address goes to the pipe: appended to its buffer
    // when the bytes fit before the 32-byte burst completes (most pipe stores), else through jit_gather (status 2 when
    // it asks compiled code to stop).
    j->stub[ST_STOREPIPE] = e->n;
    push_regs(e, (1u << R0) | (1u << R1) | (1u << R4) | (1u << LR));
    ldr_imm(e, R1, RG, OFF_JITFN(JF_GATHER_EA));
    dp_reg(e, CMP, 1, 0, R0, R1);
    branch_c(e, C_NE, storeBody);
    ldr_imm(e, R3, LR, 0);                   // sizePc
    ldr_imm(e, R1, RG, OFF_JITFN(JF_GATHER_LEN));
    dp_imm(e, CMP, 1, 0, R1, 0);
    u32 noBuf = branch_c(e, C_EQ, 0);
    dp_imm(e, AND, 0, R4, R3, 3);
    dp_imm(e, MOV, 0, R12, 0, 1);
    dp_rsr(e, MOV, 0, R4, 0, R12, LSL, R4);  // r4 = size
    ldr_imm(e, R12, R1, 0);                  // r12 = bytes already in the burst
    dp_reg(e, ADD, 0, LR, R12, R4);          // lr (saved on the stack) = the new length
    dp_imm(e, CMP, 1, 0, LR, 32);
    u32 full = branch_c(e, C_CS, 0);
    str_imm(e, LR, R1, 0);
    ldr_imm(e, R1, RG, OFF_JITFN(JF_GATHER_BUF));
    dp_reg(e, ADD, 0, R1, R1, R12);
    // Console byte order: size 1, 2 or 4.
    dp_imm(e, CMP, 1, 0, R4, 2);
    u32 not2 = branch_c(e, C_NE, 0);
    mov_shift(e, 0, R12, R2, LSR, 8); strb_imm(e, R12, R1, 0); strb_imm(e, R2, R1, 1);
    u32 done2 = branch_c(e, C_AL, 0);
    patch_branch(e, not2, e->n);
    dp_imm(e, CMP, 1, 0, R4, 4);
    u32 not4 = branch_c(e, C_NE, 0);
    mov_shift(e, 0, R12, R2, LSR, 24); strb_imm(e, R12, R1, 0);
    mov_shift(e, 0, R12, R2, LSR, 16); strb_imm(e, R12, R1, 1);
    mov_shift(e, 0, R12, R2, LSR, 8); strb_imm(e, R12, R1, 2);
    strb_imm(e, R2, R1, 3);
    u32 done4 = branch_c(e, C_AL, 0);
    patch_branch(e, not4, e->n);
    strb_imm(e, R2, R1, 0);
    patch_branch(e, done2, e->n);
    patch_branch(e, done4, e->n);
    dp_imm(e, MOV, 0, R3, 0, 0);             // appended: no stop
    branch_c(e, C_AL, popRet);
    patch_branch(e, noBuf, e->n);
    patch_branch(e, full, e->n);
    mov_reg(e, R1, R2);                      // jit_gather(g, value, sizePc)
    mov_reg(e, R2, R3);
    mov_reg(e, R0, RG);
    ldr_imm(e, R12, RG, OFF_JITFN(2));       // jit_gather
    blx_reg(e, R12);
    dp_imm(e, MOV, 0, R3, 0, 0);
    dp_imm(e, CMP, 1, 0, R0, 0);
    dp_imm_c(e, C_NE, MOV, 0, R3, 0, 2);     // the GP asked compiled code to stop: finish the instruction and leave
    branch_c(e, C_AL, popRet);
    emit_mem_stubs(j, e);
    emit_float_stubs(j, e);
    if (e->overflow) { fprintf(stderr, "JIT: shared stubs exceed ST_WORDS\n"); abort(); }
    j->stubWords = e->n;
    jit_host_sync_icache(j->code, e->n * 4);
}
_Static_assert(FAST_BITS >= 8 && FAST_BITS <= 16, "ST_INDIRECT's index shifts");

typedef u32 (*EnterFn)(Gekko* g, void* fn, s32 budget, s32* left);

// Runs compiled code from block `fn` with a budget of guest instructions; returns how many ran and the link site the
// code left through (or NULL).
void (*jit_run_begin_hook)(void);
void (*jit_run_end_hook)(void);

// `pre`: entering block en after its budget check (JIT_PROLOGUE) with its count already taken off; 0 at the check.
static u32 run(Jit* j, BlockFn fn, s32 budget, u64 start, u32** site, u32 pre) {
    s32 left = budget;
    g_runStart = start;
    g_runBudget = budget;
    g_jitLeft = budget;
    if (jit_run_begin_hook) jit_run_begin_hook();
    *site = (u32*)(uintptr_t)((EnterFn)(void*)(j->code + j->stub[ST_ENTER]))(j->g, (void*)fn, budget - (s32)pre, &left);
    if (jit_run_end_hook) jit_run_end_hook();
    return (u32)(budget - left);
}

// Patches the link site to jump straight to `fn`.
static void link_site(Jit* j, u32* site, BlockFn fn) {
    s32 off = (s32)((const u32*)(void*)fn - (site + 2));
    u32 insn = (*site & 0xF0000000u) | 0x0A000000u | ((u32)off & 0xFFFFFF);  // the site's condition kept
    if (*site == insn) return;  // linked already (no cache sync)
    *site = insn;
    jit_host_sync_icache(site, 4);
    j->stats.links++;
}

// ---- dispatcher ----
static u32 bucket_of(u32 pc, u32 msrBits) { return ((pc >> 2) ^ (pc >> 18) ^ (msrBits << 9)) & (DISPATCH_SIZE - 1); }

#ifndef __3DS__
static Entry* rs_find(Jit* j, u32 pc, u32 msrBits) {  // the region model: a compiled block, without compiling
    for (u32 i = j->buckets[bucket_of(pc, msrBits)]; i != NO_ENTRY; i = j->table[i].next)
        if (j->table[i].pc == pc && j->table[i].msrBits == msrBits) return &j->table[i];
    return NULL;
}
#endif

// Finds the block for (pc, msrBits), compiling it on first use; NULL when it cannot be compiled.
static Entry* lookup(Jit* j, u32 pc, u32 msrBits) {
    u32* head = &j->buckets[bucket_of(pc, msrBits)];
    for (u32 i = *head; i != NO_ENTRY; i = j->table[i].next)
        if (j->table[i].pc == pc && j->table[i].msrBits == msrBits) return &j->table[i];
    if (j->nEntries == MAX_ENTRIES) { jit_invalidate_all(j); j->stats.flushes++; }
    Gekko* g = j->g;
    {  // statistics: a block compiled again at a PC compiled since the last flush means a lookup missed it
        u32 w = (pc & 0x01FFFFFFu) >> 2;
        if (j->compiledPc && w < (24u << 20) / 4) {
            if (j->compiledPc[w >> 3] & (1u << (w & 7))) j->stats.compileRepeats++;
            j->compiledPc[w >> 3] |= (u8)(1u << (w & 7));
        }
    }
    for (u32 k = 0; k < 8; k++)
        if (j->stats.keySeen[k] == msrBits || !j->stats.keyCount[k]) { j->stats.keySeen[k] = msrBits; j->stats.keyCount[k]++; break; }
    u32 poll = gekko_poll_loop_len(g, pc);
    unsigned char wasWhere = g_where;
    g_where = WH_COMPILE;
    BlockFn fn = compile(j, pc, poll != 0);
    g_where = wasWhere;
    if (!fn) return NULL;
    // compile() may have flushed the whole cache (code space full), which also emptied the chains.
    head = &j->buckets[bucket_of(pc, msrBits)];
    u32 pa, idx = j->nEntries++;
    Entry* en = &j->table[idx];
    en->pc = pc; en->msrBits = msrBits; en->fn = fn;
    en->idle = gk_translate(g, pc, false, false, &pa, false) && pa + 4 <= j->ramSize &&
               ((u32)j->ram[pa] << 24 | (u32)j->ram[pa + 1] << 16 | (u32)j->ram[pa + 2] << 8 | j->ram[pa + 3]) == 0x48000000u;
    en->poll = (u8)poll;
    en->count = (u8)g_lastCount;
    en->next = *head;
    *head = idx;
    return en;
}

// The indirect table's slot for a guest address, as ST_INDIRECT computes it. Folding in the address bits from 4 KB up did
// not reduce TP's misses (qemu, 30 fields from tp_arm_850: 86,499 -> 91,663), so they are not code 16 KB apart sharing a
// slot; blr / bctr to polling-loop and idle blocks, which are never put in the table, are the likely rest.
static u32 fast_index(u32 pc) { return (pc >> 2) & (FAST_SIZE - 1); }

static void fast_clear_one(FastEntry* t) {
    for (u32 i = 0; i < FAST_SIZE; i++) { t[i].pc = 1; t[i].fn = NULL; }  // 1: never a guest PC
}

static void fast_clear(Jit* j) {
    if (!j->fast) return;
    for (u32 t = 0; t < FAST_TABLES; t++) { fast_clear_one(j->fast + t * FAST_SIZE); j->fastUsed[t] = false; }
    j->fastNext = 0;
    j->g->fastBase = (u32)(uintptr_t)j->fast;
}

// The indirect-branch table for blocks of key: compiled code only ever runs blocks of one key at a time, so the
// stub reads the running key's table from Gekko.fastBase. A key not seen recently reuses the oldest table.
static FastEntry* fast_select(Jit* j, u32 key) {
    FastEntry* t = (FastEntry*)(uintptr_t)j->g->fastBase;
    u32 cur = (u32)((uintptr_t)t - (uintptr_t)j->fast) / (FAST_SIZE * sizeof(FastEntry));  // a reset Gekko: out of range
    if (cur < FAST_TABLES && t == j->fast + cur * FAST_SIZE && j->fastUsed[cur] && j->fastKeys[cur] == key) return t;
    for (u32 i = 0; i < FAST_TABLES; i++)
        if (j->fastUsed[i] && j->fastKeys[i] == key) {
            t = j->fast + i * FAST_SIZE;
            j->g->fastBase = (u32)(uintptr_t)t;
            return t;
        }
    u32 i = j->fastNext;
    j->fastNext = (i + 1) % FAST_TABLES;
    t = j->fast + i * FAST_SIZE;
    fast_clear_one(t);
    j->fastUsed[i] = true;
    j->fastKeys[i] = key;
    j->stats.fastSwitches++;
    j->g->fastBase = (u32)(uintptr_t)t;
    return t;
}

static bool update_fastmem(Jit* j) {
    // The inline fast path assumes the standard mappings (0x80000000 and 0xC0000000 both at physical 0). Blocks
    // compiled under other mappings use the slow path for every access. (Blocks compiled before a BAT change keep
    // their choice; games set BATs once at boot.)
    Gekko* g = j->g;
    u32 a, b;
    j->fastmem = gk_translate(g, 0x80000000u, false, true, &a, false) && a == 0 &&
                 gk_translate(g, 0xC0000000u, false, true, &b, false) && b == 0 && j->ramSize >= 0x01800000u;
    // The same in problem state: a BAT valid only for supervisor accesses (Vp = 0) gives user code a DSI, which the
    // fast paths and the write-gather pipe's shortcut would skip. Games never run in problem state; the fuzzer does.
    u32 msr = g->msr;
    g->msr = msr | MSR_PR;
    j->fastmemUser = j->fastmem && gk_translate(g, 0x80000000u, false, true, &a, false) && a == 0 &&
                     gk_translate(g, 0xC0000000u, false, true, &b, false) && b == 0;
    g->msr = msr;
    return j->fastmem;
}

// Gekko.jitFn: helper and stub addresses and the pipe's pointers (compiled code reads them RG-relative). Written at every
// entry: a loaded save state brings another build's values, and the pipe may be configured after the stubs exist.
static void jit_fill_fns(Jit* j) {
    Gekko* g = j->g;
    for (u32 k = 0; k < sizeof kJitFns / sizeof kJitFns[0]; k++) g->jitFn[k] = (u32)(uintptr_t)kJitFns[k];
    g->jitFn[JF_LOADSLOW] = (u32)(uintptr_t)(j->code + j->stub[ST_LOADSLOW]);
    g->jitFn[JF_STORESLOW] = (u32)(uintptr_t)(j->code + j->stub[ST_STORESLOW]);
    g->jitFn[JF_STOREPIPE] = (u32)(uintptr_t)(j->code + j->stub[ST_STOREPIPE]);
    g->jitFn[JF_GATHER_EA] = j->gatherEa;
    g->jitFn[JF_GATHER_LEN] = (u32)(uintptr_t)j->gatherLen;
    g->jitFn[JF_GATHER_BUF] = (u32)(uintptr_t)j->gatherBuf;
}

static u32 jit_entry(Gekko* g, s64 budget, void* ctx) {
    Jit* j = ctx;
    u32 total = 0;
    g_where = WH_DISPATCH;
    // The helper table compiled code calls through: refilled here because a loaded save state overwrites the Gekko
    // with another build's host addresses.
    jit_fill_fns(j);
    update_fastmem(j);
    u32* site = NULL;  // the link site the last run left through, patched once its target is known
    bool lastMiss = false;  // the last run left through an indirect-table miss (statistics)
    u32 missKey = 0;
    u32 siteGen = 0, siteKey = 0;
    while ((s64)total < budget) {
        if (!j->soExact && (g->xer & XER_SO)) { j->soExact = true; jit_invalidate_all(j); }  // see jit_set_so_exact
        u32 pc = g->pc, key = block_key(g);
        u32 compiledBefore = j->nEntries;
        Entry* en = lookup(j, pc, key);
        if (en && lastMiss) {  // what the indirect-table miss that brought us here jumped to
            if (en->poll || en->idle) j->stats.missPoll++;
            else if (j->nEntries != compiledBefore) j->stats.missFirst++;
            else {
                const FastEntry* t = (const FastEntry*)(uintptr_t)g->fastBase + fast_index(pc);
                if (key != missKey) j->stats.missKey++;
                else if (t->pc == 1) j->stats.missEmpty++;
                else j->stats.missTaken++;
            }
        }
        lastMiss = false;
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
        FastEntry* ft = fast_select(j, key);
        if (plain && j->linking) {
            FastEntry* f = &ft[fast_index(pc)];
            f->pc = pc;
            f->fn = (void*)en->fn;
        }
        s64 room = budget - (s64)total;
        u32 gen = j->gen, *s;
        jit_in_run = 1;
        u32 n = run(j, (BlockFn)(void*)((u32*)(void*)en->fn + JIT_PROLOGUE), room > 0x3FFFFFFF ? 0x3FFFFFFF : (s32)room,
                    g->cycles + total, &s, en->count - (jit_debug_plant == 8 && en->count > 1 ? 1u : 0u));  // plant 8: a wrong count
        jit_in_run = 0;
        g_where = WH_DISPATCH;
        j->stats.blocksRun++;
        if (!n) break;  // cannot happen for a well-formed block; never spin
        total += n;
        j->stats.instrsRun += n;
        if (s == (u32*)1) { j->stats.dispIndirect++; lastMiss = true; missKey = block_key(g); }
        else if (s) j->stats.dispLink++;
        else if ((s64)n >= room) j->stats.dispBudget++;
        else j->stats.dispOther++;
        if (s > (u32*)1 && j->linking && gen == j->gen) { site = s; siteGen = gen; siteKey = key; }
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
    g_where = WH_GEKKO;
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
    j->coldStart = j->codeWords / 8 * 3;  // hot code: ~35% of TP's code since the block stubs and float stubs moved out (5.0 hot vs 9.3 cold words per instruction)
    j->table = calloc(MAX_ENTRIES, sizeof(Entry));
    j->compiledPc = calloc(1, (24u << 20) / 32);  // 768 KB; NULL just disables the statistic
#ifndef __3DS__
    if (getenv("JIT_REGSTAT")) {
        g_rsLd = calloc(MAX_ENTRIES, RS_IDS);
        g_rsSt = calloc(MAX_ENTRIES, RS_IDS);
        g_rsExec = calloc(MAX_ENTRIES, sizeof(u32));
        g_dopPool = calloc(DOP_POOL, sizeof(u32));
        g_dopStart = calloc(MAX_ENTRIES, sizeof(u32));
        g_dopLen = calloc(MAX_ENTRIES, sizeof(u32));
        g_fsLd = calloc(MAX_ENTRIES, sizeof(u32)); g_fsHit = calloc(MAX_ENTRIES, sizeof(u32));
        g_fsSt = calloc(MAX_ENTRIES, sizeof(u32)); g_fsDead = calloc(MAX_ENTRIES, sizeof(u32));
        g_rwLd = calloc(MAX_ENTRIES, RL_N);
        g_rwSt = calloc(MAX_ENTRIES, RW_N);
        g_rsE = calloc(MAX_ENTRIES, sizeof(RsEntry));
        g_rx = calloc(RX_POOL, sizeof(RsExit));
        g_rxCount = calloc(RX_POOL, sizeof(u32));
        if (getenv("JIT_SCHEDDUMP")) {
            g_schedOut = fopen(getenv("JIT_SCHEDDUMP"), "wb");
            g_hotLen = calloc(MAX_ENTRIES, sizeof(u32));
            g_proLen = calloc(MAX_ENTRIES, sizeof(u32));
            if (!g_hotLen || !g_proLen) { if (g_schedOut) fclose(g_schedOut); g_schedOut = NULL; }
        }
        g_regStat = g_rsLd && g_rsSt && g_rsExec && g_dopPool && g_dopStart && g_dopLen && g_fsLd && g_fsHit && g_fsSt && g_fsDead &&
                    g_rwLd && g_rwSt && g_rsE && g_rx && g_rxCount;
    }
#endif
    j->buckets = malloc(DISPATCH_SIZE * sizeof(u32));
    j->fast = malloc(FAST_TABLES * FAST_SIZE * sizeof(FastEntry));
    if (!j->code || !j->table || !j->buckets || !j->fast) { free(j->table); free(j->buckets); free(j->fast); free(j); return NULL; }
    j->linking = true;
    j->memStubs = true;
    j->traces = true;
    j->traceFollow = true;
    j->traceSide = TRACE_SIDE_ALL;
    emit_stubs(j);
    jit_fill_fns(j);  // compiled code may run without jit_entry (the fuzzer)
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
    free(j->compiledPc);
    free(j);  // the executable region is kept for the process lifetime
}

void jit_attach(Jit* j) { gekko_set_jit(j->g, jit_entry, j); }

void jit_set_linking(Jit* j, bool on) {
    if (j->linking == on) return;
    j->linking = on;
    jit_invalidate_all(j);  // drops every patched link
}

bool jit_linking(const Jit* j) { return j->linking; }

void jit_set_traces(Jit* j, bool on) {
    if (j->traces == on) return;
    j->traces = on;
    jit_invalidate_all(j);  // blocks are compiled one way or the other
}

bool jit_traces(const Jit* j) { return j->traces; }

void jit_set_trace_parts(Jit* j, bool follow, u32 side, u32 maxInsns) {
    if (j->traceFollow == follow && j->traceSide == side && j->traceMax == maxInsns) return;
    j->traceFollow = follow;
    j->traceSide = side;
    j->traceMax = maxInsns;
    jit_invalidate_all(j);
}

u32 jit_entries(const Jit* j) { return j->nEntries; }

void jit_set_profile(Jit* j, bool on) {
    if (j->profile == on) return;
    j->profile = on;
    jit_invalidate_all(j);
}

void jit_set_so_exact(Jit* j, bool on) {
    if (j->soExact == on) return;
    j->soExact = on;
    jit_invalidate_all(j);
}
bool jit_so_exact(const Jit* j) { return j->soExact; }

// The Jits in fault mode, for jit_fault (called from the host's fault handler with only the faulting PC).
static Jit* g_faultJits[4];

void jit_set_fault_mem(Jit* j, bool on) {
    if (on && !j->faultKey) {
        j->faultKey = malloc(FAULT_SLOTS * sizeof(u32));
        j->faultVal = malloc(FAULT_SLOTS * sizeof(u32));
        if (!j->faultKey || !j->faultVal) { free(j->faultKey); free(j->faultVal); j->faultKey = j->faultVal = NULL; return; }
        memset(j->faultKey, 0xFF, FAULT_SLOTS * sizeof(u32));
    }
    if (on && !jit_host_fault_init(j->ram)) on = false;
    if (on) {
        int k = 0;
        while (k < 4 && g_faultJits[k] && g_faultJits[k] != j) k++;
        if (k == 4) on = false;
        else g_faultJits[k] = j;
    }
    if (j->faultMem == on) return;
    j->faultMem = on;
    jit_invalidate_all(j);
}
bool jit_fault_mem(const Jit* j) { return j->faultMem; }

bool jit_fault(u32 hostPc, u32* resume) {
    for (int k = 0; k < 4 && g_faultJits[k]; k++) {
        Jit* j = g_faultJits[k];
        if (!j->faultMem || hostPc < (u32)(uintptr_t)j->code || hostPc >= (u32)(uintptr_t)(j->code + j->codeWords) || (hostPc & 3)) continue;
        u32 key = (hostPc - (u32)(uintptr_t)j->code) >> 2;
        for (u32 h = fault_hash(key); j->faultKey[h] != ~0u; h = (h + 1) & (FAULT_SLOTS - 1)) {
            if (j->faultKey[h] != key) continue;
            u32* site = j->code + key;
            u32* dest = j->code + j->faultVal[h];
            *site = 0xEA000000u | ((u32)(s32)(dest - (site + 2)) & 0xFFFFFFu);  // b dest
            jit_host_sync_icache(site, 4);
            j->stats.faults++;
            *resume = (u32)(uintptr_t)dest;
            return true;
        }
    }
    return false;
}

void jit_set_slow_inline(Jit* j, bool on) {
    if (j->slowInline == on) return;
    j->slowInline = on;
    jit_invalidate_all(j);
}
bool jit_slow_inline(const Jit* j) { return j->slowInline; }

void jit_set_sched(Jit* j, u32 mask) {
    g_addChunks = (mask & SCHED_CONST) ? 2 : 1;
    if (j->sched == mask) return;
    j->sched = mask;
    jit_invalidate_all(j);
}
u32 jit_sched(const Jit* j) { return j->sched; }

void jit_set_mem_stubs(Jit* j, bool on) {
    if (j->memStubs == on) return;
    j->memStubs = on;
    jit_invalidate_all(j);  // blocks are compiled one way or the other
}

bool jit_mem_stubs(const Jit* j) { return j->memStubs; }

static void jit_fill_fns(Jit* j);
void jit_set_gather(Jit* j, void (*put)(void* ctx, u32 v, u32 size), void* ctx, u32 ea) {
    j->gatherPut = put;
    j->gatherCtx = ctx;
    j->gatherEa = ea;
    jit_fill_fns(j);  // the pipe stub reads its address there (the fuzzer runs code without jit_entry, which refills it)
    jit_invalidate_all(j);
}

void jit_set_gather_buffer(Jit* j, u8* buf, u32* len) {
    j->gatherBuf = buf;
    j->gatherLen = len;
    jit_fill_fns(j);
    jit_invalidate_all(j);
}

u32 jit_run_one_block(Jit* j) {
    Gekko* g = j->g;
    GKP(g)->brk = false;
    u32 msrBits = block_key(g);
    update_fastmem(j);
    if (!j->soExact && (g->xer & XER_SO)) { j->soExact = true; jit_invalidate_all(j); }  // as the dispatcher does
    Entry* en = lookup(j, g->pc, msrBits);
    if (!en) return 0;
    fast_select(j, msrBits);
    u32* site;
    return run(j, en->fn, 0x3FFFFFFF, g->cycles, &site, 0);
}

u32 jit_run(Jit* j, s64 budget) { return jit_entry(j->g, budget, j); }

void jit_invalidate_all(Jit* j) {
#ifndef __3DS__
    rs_fold(j, j->nEntries);
#endif
    memset(j->buckets, 0xFF, DISPATCH_SIZE * sizeof(u32));
    j->nEntries = 0;
    memset(j->codePages, 0, sizeof j->codePages);
    j->codeUsed = j->stubWords;
    j->coldUsed = j->coldStart;
    j->insnsSinceFlush = 0;
    if (j->faultKey) { memset(j->faultKey, 0xFF, FAULT_SLOTS * sizeof(u32)); j->faultN = 0; }
    if (j->compiledPc) memset(j->compiledPc, 0, (24u << 20) / 32);
    fast_clear(j);
    j->gen++;
}

void jit_invalidate(Jit* j, u32 pa) {
    // Level 1 keeps no per-page block lists: any icbi that touches a page holding compiled code drops everything.
    j->stats.icbis++;
    if ((pa >> 12) < sizeof j->codePages && j->codePages[pa >> 12]) { j->stats.icbiFlushes++; jit_invalidate_all(j); }
}

volatile u32 jit_in_run;

static const Entry* g_sortEntries;
static int by_fn(const void* a, const void* b) {
    uintptr_t fa = (uintptr_t)g_sortEntries[*(const u32*)a].fn, fb = (uintptr_t)g_sortEntries[*(const u32*)b].fn;
    return fa < fb ? -1 : fa > fb;
}

void jit_map_hosts(const Jit* j, u32* addrs, u32 n) {
    static u32* order;
    if (!order && !(order = malloc(MAX_ENTRIES * sizeof *order))) return;
    u32 m = j->nEntries;
    for (u32 i = 0; i < m; i++) order[i] = i;
    g_sortEntries = j->table;
    qsort(order, m, sizeof *order, by_fn);
    for (u32 k = 0; k < n; k++) {
        u32 a = addrs[k], lo = 0, hi = m, best = NO_ENTRY;
        while (lo < hi) {  // the last block starting at or before a
            u32 mid = (lo + hi) / 2;
            if ((u32)(uintptr_t)j->table[order[mid]].fn <= a) { best = order[mid]; lo = mid + 1; } else hi = mid;
        }
        addrs[k] = best == NO_ENTRY ? 0 : j->table[best].pc;
    }
}

const JitStats* jit_stats(const Jit* j) { return &j->stats; }

// The fuller of the two regions.
float jit_code_used(const Jit* j) {
    if (!j->codeWords) return 0.0f;
    float hot = (float)j->codeUsed / (float)j->coldStart, cold = (float)(j->coldUsed - j->coldStart) / (float)(j->codeWords - j->coldStart);
    return hot > cold ? hot : cold;
}
static u32 code_words_used(const Jit* j) { return j->codeUsed - j->stubWords + j->coldUsed - j->coldStart; }

float jit_words_per_insn(const Jit* j) {
    return j->insnsSinceFlush ? (float)code_words_used(j) / (float)j->insnsSinceFlush : 0.0f;
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
    char line[400];
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
    snprintf(line, sizeof line, "JIT: slow-path loads %llu, stores %llu (of them in the uncached mirror %llu); fault mode %s: %lu sites, %llu faults",
             (unsigned long long)s->slowLoads, (unsigned long long)s->slowStores, (unsigned long long)s->slowMirror,
             j->faultMem ? "on" : "off", (unsigned long)j->faultN, (unsigned long long)s->faults);
    print(line);
    snprintf(line, sizeof line, "JIT: returns to the dispatcher: link requests %llu, indirect-table misses %llu (to polling / idle "
             "blocks %llu, to blocks compiled then %llu; else slot taken %llu, empty %llu, key changed %llu), budget ends "
             "%llu, other %llu", (unsigned long long)s->dispLink,
             (unsigned long long)s->dispIndirect, (unsigned long long)s->missPoll, (unsigned long long)s->missFirst,
             (unsigned long long)s->missTaken, (unsigned long long)s->missEmpty, (unsigned long long)s->missKey,
             (unsigned long long)s->dispBudget, (unsigned long long)s->dispOther);
    print(line);
    snprintf(line, sizeof line, "JIT: %.1f host words per guest instruction compiled (%u KB of code); %llu fused compare-branches, %llu cached GPR reads, %llu pipe stores",
             s->instrsCompiled ? (double)code_words_used(j) / (double)s->instrsCompiled : 0.0,
             (code_words_used(j) * 4) >> 10, (unsigned long long)s->fusedBranches, (unsigned long long)s->forwards,
             (unsigned long long)s->gatherStores);
    print(line);
    double kept = s->insnsKept ? (double)s->insnsKept : 1.0;
    snprintf(line, sizeof line, "JIT: kept blocks, host words per guest instruction: %.1f hot, %.1f exit stubs, %.1f cold; "
             "compiles refused: %llu hot, %llu cold, %llu fix-ups", (double)s->wordsHot / kept, (double)s->wordsExit / kept,
             (double)s->wordsCold / kept, (unsigned long long)s->compileFails[0], (unsigned long long)s->compileFails[1],
             (unsigned long long)s->compileFails[2]);
    print(line);
    snprintf(line, sizeof line, "JIT: traces %s: %llu unconditional branches followed, %llu conditional branches as side exits", j->traces ? "on" : "off",
             (unsigned long long)s->traceFollowed, (unsigned long long)s->traceSideExits);
    print(line);
    snprintf(line, sizeof line, "JIT: %llu instructions run, %llu interpreted (%.1f%%)", (unsigned long long)s->instrsRun,
             (unsigned long long)s->fallbacksRun, s->instrsRun ? 100.0 * (double)s->fallbacksRun / (double)s->instrsRun : 0.0);
    print(line);
    print_top(&s->fallback, "compiled-but-interpreted (static)", print);
    print_top(&s->fallbackRun, "interpreted at run time (dynamic)", print);
#ifndef __3DS__
    {  // the opcodes that take the most compiled code
        char l2[256];
        u64 totHot = 0, totCold = 0, totN = 0;
        for (u32 k = 0; k < 65536; k++) { totHot += g_opHot[k]; totCold += g_opCold[k]; totN += g_opN[k]; }
        snprintf(l2, sizeof l2, "  compiled code by opcode (hot words / cold words / count; of %llu hot, %llu cold words, %llu instructions):",
                 (unsigned long long)totHot, (unsigned long long)totCold, (unsigned long long)totN);
        print(l2);
        static bool done[65536];
        memset(done, 0, sizeof done);
        for (int rank = 0; rank < 30; rank++) {
            u32 best = 0, key = 0;
            for (u32 k = 0; k < 65536; k++) if (!done[k] && g_opHot[k] > best) { best = g_opHot[k]; key = k; }
            if (!best) break;
            done[key] = true;
            u32 op = key >> 10, xo = key & 1023;
            char dis[64];
            gekko_disasm(0, (op == 31 || op == 19 || op == 63 || op == 59 || op == 4) ? (op << 26) | (xo << 1) : op << 26, dis, sizeof dis);
            char* sp = strchr(dis, ' ');
            if (sp) *sp = 0;
            snprintf(l2, sizeof l2, "    %-9s op %2u xo %4u: %6.2f%% of hot words, %5.1f hot + %5.1f cold words each, %llu compiled",
                     dis, op, xo, 100.0 * (double)g_opHot[key] / (double)(totHot ? totHot : 1),
                     (double)g_opHot[key] / (double)g_opN[key], (double)g_opCold[key] / (double)g_opN[key],
                     (unsigned long long)g_opN[key]);
            print(l2);
        }
    }
    if (g_regStat) {
        rs_fold((Jit*)j, j->nEntries);  // folding the model resets counters only
        {  // executed: guest instructions and hot ARM words per opcode, weighted by block executions
            u64 totN = 0, totW = 0;
            for (u32 k = 0; k < 65536; k++) { totN += g_dopN[k]; totW += g_dopWords[k]; }
            snprintf(line, sizeof line, "  executed by opcode (JIT_REGSTAT): %llu guest instructions, %llu hot ARM words (%.2f per instruction; exit stubs and shared stubs not included):",
                     (unsigned long long)totN, (unsigned long long)totW, totN ? (double)totW / (double)totN : 0.0);
            print(line);
            static bool seen[65536];
            memset(seen, 0, sizeof seen);
            for (int rank = 0; rank < 30; rank++) {
                u64 best = 0; u32 key = 0;
                for (u32 k = 0; k < 65536; k++) if (!seen[k] && g_dopWords[k] > best) { best = g_dopWords[k]; key = k; }
                if (!best) break;
                seen[key] = true;
                u32 op = key >> 10, xo = key & 1023;
                char dis[64];
                gekko_disasm(0, (op == 31 || op == 19 || op == 63 || op == 59 || op == 4) ? (op << 26) | (xo << 1) : op << 26, dis, sizeof dis);
                char* sp = strchr(dis, ' ');
                if (sp) *sp = 0;
                snprintf(line, sizeof line, "    %-9s %5.2f%% of executed words, %5.2f%% of executed instructions, %5.1f words each",
                         dis, 100.0 * (double)g_dopWords[key] / (double)(totW ? totW : 1), 100.0 * (double)g_dopN[key] / (double)(totN ? totN : 1),
                         (double)g_dopWords[key] / (double)(g_dopN[key] ? g_dopN[key] : 1));
                print(line);
            }
        }
        snprintf(line, sizeof line, "  simulated FPR lane cache (8 lanes, per block): ld_fpr %llu executed, %.1f%% hits; st_fpr %llu executed, %.1f%% overwritten in the block before being read",
                 (unsigned long long)g_fsLdTot, g_fsLdTot ? 100.0 * (double)g_fsHitTot / (double)g_fsLdTot : 0.0,
                 (unsigned long long)g_fsStTot, g_fsStTot ? 100.0 * (double)g_fsDeadTot / (double)g_fsStTot : 0.0);
        print(line);
        print("  register cache by guest register (loads into a slot / write-backs, per block execution; JIT_REGSTAT):");
        snprintf(line, sizeof line, "    %llu block executions", (unsigned long long)g_rsExecTot);
        print(line);
        u64 order[RS_IDS];
        for (int i = 0; i < RS_IDS; i++) order[i] = ((g_rsLdTot[i] + g_rsStTot[i]) << 8) | (u64)i;
        for (int a = 0; a < RS_IDS; a++) for (int b = a + 1; b < RS_IDS; b++) if (order[b] > order[a]) { u64 t = order[a]; order[a] = order[b]; order[b] = t; }
        for (int n = 0; n < 16; n++) {
            int i = (int)(order[n] & 255);
            if (!(order[n] >> 8)) break;
            snprintf(line, sizeof line, "    %-4s loads %6.3f  stores %6.3f  per block run", i < 32 ? (char[8]){ 'r', (char)('0' + i / 10), (char)('0' + i % 10), 0 } : i == 32 ? "CR" : "XER",
                     g_rsExecTot ? (double)g_rsLdTot[i] / (double)g_rsExecTot : 0.0, g_rsExecTot ? (double)g_rsStTot[i] / (double)g_rsExecTot : 0.0);
            print(line);
        }
        // The region model (docs/REGION_JIT_PLAN.md): per block run unless stated.
        double runs = g_rsExecTot ? (double)g_rsExecTot : 1.0;
        u64 words = 0;
        for (u32 k = 0; k < 65536; k++) words += g_dopWords[k];
        snprintf(line, sizeof line, "  region model: loads first touch %.3f, after a fallback %.3f, after an eviction %.3f; write-backs at "
                 "constant exits %.3f, blr/bctr %.3f, fallbacks %.3f, evictions %.3f, other %.3f (per block run; %.1f hot words per run)",
                 (double)g_rwLdTot[RL_FIRST] / runs, (double)g_rwLdTot[RL_INTERP] / runs, (double)g_rwLdTot[RL_EVICT] / runs,
                 (double)g_rwStTot[RW_CONST] / runs, (double)g_rwStTot[RW_IND] / runs, (double)g_rwStTot[RW_INTERP] / runs,
                 (double)g_rwStTot[RW_EVICT] / runs, (double)g_rwStTot[RW_OTHER] / runs, (double)words / runs);
        print(line);
        u64 ex = g_rxN[0] + g_rxN[1] + g_rxN[2];
        snprintf(line, sizeof line, "  region model: block-end exits %llu: constant %.1f%%, blr/bctr %.1f%%, to the dispatcher %.1f%%; "
                 "constant exits whose target was not compiled %.1f%%; back edges %.1f%% of constant exits",
                 (unsigned long long)ex, ex ? 100.0 * (double)g_rxN[0] / (double)ex : 0.0, ex ? 100.0 * (double)g_rxN[1] / (double)ex : 0.0,
                 ex ? 100.0 * (double)g_rxN[2] / (double)ex : 0.0, g_rxN[0] ? 100.0 * (double)g_rxNoTarget / (double)g_rxN[0] : 0.0,
                 g_rxN[0] ? 100.0 * (double)g_rsBackN / (double)g_rxN[0] : 0.0);
        print(line);
        double today = (double)(g_rxTodaySt[RX_CONST] + g_rxTodayLd), ho = (double)(g_rxHoSt + g_rxHoLd);
        snprintf(line, sizeof line, "  region model, entry state handed over at constant exits: today %.3f write-backs + %.3f target loads, "
                 "handed over %.3f + %.3f + %.3f moves per block run: saves %.3f memory accesses per run (%.2f%% of hot words, moves not counted)",
                 (double)g_rxTodaySt[RX_CONST] / runs, (double)g_rxTodayLd / runs, (double)g_rxHoSt / runs, (double)g_rxHoLd / runs,
                 (double)g_rxHoMov / runs, (today - ho) / runs, words ? 100.0 * (today - ho) / (double)words : 0.0);
        print(line);
        snprintf(line, sizeof line, "  region model, variant 1 (above) also loads the write-first registers at entries not through a link: "
                 "%.3f per block run, net saving %.3f (%.2f%% of hot words); variant 2 (read-first registers only): handed over %.3f + "
                 "%.3f + %.3f moves, saves %.3f (%.2f%%)", (double)g_rxV1Extra / runs, (today - ho - (double)g_rxV1Extra) / runs,
                 words ? 100.0 * (today - ho - (double)g_rxV1Extra) / (double)words : 0.0, (double)g_rxV2St / runs,
                 (double)g_rxV2Ld / runs, (double)g_rxV2Mov / runs, (today - (double)(g_rxV2St + g_rxV2Ld)) / runs,
                 words ? 100.0 * (today - (double)(g_rxV2St + g_rxV2Ld)) / (double)words : 0.0);
        print(line);
        snprintf(line, sizeof line, "  region model, loop regions (innermost back edge per block): %llu loops touch at most %d ids, "
                 "their traffic %.3f per block run against %.3f to enter and leave them (saves %.3f, %.2f%% of hot words); %llu loops "
                 "touch more, traffic %.3f per run", (unsigned long long)g_rsLoops[0], NSLOT_STAT, (double)g_rsLoopFit / runs,
                 (double)g_rsLoopEntryCost / runs, ((double)g_rsLoopFit - (double)g_rsLoopEntryCost) / runs,
                 words ? 100.0 * ((double)g_rsLoopFit - (double)g_rsLoopEntryCost) / (double)words : 0.0,
                 (unsigned long long)g_rsLoops[1], (double)g_rsLoopBig / runs);
        print(line);
        double tt = g_rsTrafTot ? (double)g_rsTrafTot : 1.0;
        snprintf(line, sizeof line, "  memory instructions executed by address form: r1+d %u, r2+d %u, r13+d %u, other rA+d %u, 0+d %u, rA+rB %u",
                 g_eaRun[0], g_eaRun[1], g_eaRun[2], g_eaRun[3], g_eaRun[4], g_eaRun[5]);
        print(line);
        for (int st = 0; st < 2; st++) {
            int n = snprintf(line, sizeof line, "  %s executed by GQR type (W=0 / W=1):", st ? "psq_st" : "psq_l");
            for (int q = 0; q < 8; q++)
                if (g_psqRun[st][q][0] || g_psqRun[st][q][1])
                    n += snprintf(line + n, sizeof line - (size_t)n, " type %d %u / %u", q, g_psqRun[st][q][0], g_psqRun[st][q][1]);
            print(line);
        }
        snprintf(line, sizeof line, "  region model, register traffic by block: top 16 %.1f%%, 64 %.1f%%, 256 %.1f%%, 1024 %.1f%%, 4096 %.1f%%; "
                 "in blocks inside a back edge's range %.1f%%", 100.0 * (double)g_rsTrafTop[0] / tt, 100.0 * (double)g_rsTrafTop[1] / tt,
                 100.0 * (double)g_rsTrafTop[2] / tt, 100.0 * (double)g_rsTrafTop[3] / tt, 100.0 * (double)g_rsTrafTop[4] / tt,
                 100.0 * (double)g_rsTrafLoop / tt);
        print(line);
    }
#endif
}
#endif
