#pragma once
// Gekko -> ARMv6K JIT (32-bit ARM hosts only: the 3DS, and ARM Linux under qemu for testing).
//
// Level 1: blocks of guest code end at the first branch. Common integer, load/store, branch, float and paired-single
// instructions are translated (floats on VFP in a "fast float" model, see jit.c); everything else runs through the
// interpreter's handler for that one instruction, so any program runs correctly from the start and speed grows with
// coverage. Guest registers stay in the Gekko struct (no register allocation yet). Blocks are linked: an exit to a
// constant address jumps straight to the next block once that is compiled, and blr / bctr go through a small table,
// so compiled code returns to the C dispatcher only when the budget is used up or for something it must handle.
#include "../cpu/gekko.h"

typedef struct Jit Jit;

// Instructions left to the interpreter, by primary opcode and by extended opcode for groups 31 / 19 / 63 / 59 / 4.
typedef struct {
    u32 primary[64], x31[1024], x19[1024], x63[1024], x59[32], x4[1024];
} JitOpCounts;

typedef struct {
    u64 blocksCompiled, instrsCompiled, instrsNative, flushes;
    u64 compileRepeats;   // blocks compiled at a PC already compiled since the last flush (a lookup missed it)
    u32 keySeen[8], keyCount[8];  // block keys (MSR / FPSCR / HID2 bits) blocks were compiled under, and how often
    u64 blocksRun;  // entries from the C dispatcher (linked blocks run without coming back)
    u64 links;      // exits patched to jump straight to their target block
    u64 fusedBranches, forwards;  // compile time: bc on a compare's ARM flags; GPR reads served by the register cache
    u64 traceFollowed, traceSideExits, traceCalls;  // ...; traceCalls: bl followed into the callee (OPT_CALLS)  // compile time: unconditional branches followed, conditional branches compiled as side exits
    u64 compileFails[3];  // blocks not compiled: hot code or exits overflowed (or a slow path without its tail), cold code overflowed, fix-ups overflowed
    u64 wordsHot, wordsExit, wordsCold, insnsKept;  // compile time, kept blocks: host words in the hot path, exit stubs, cold slow paths
    u64 dispLink, dispIndirect, dispBudget, dispOther;  // why compiled code returned to the dispatcher
    u64 missPoll, missFirst, missTaken, missEmpty, missKey;
    u64 faults;      // fault-mode sites patched to their slow path (jit_fault)
    u64 deopts;      // blocks recompiled with per-access checks after a failed window check (OPT_HOIST)
    u64 slowMirror;  // slow-path loads / stores whose address is in MEM1's uncached mirror (0xC0000000)  // ... / slot held another target / slot empty / other key  // indirect-table misses whose target is a polling-loop or idle block / was never dispatched
    u64 fastSwitches;  // indirect tables reassigned to a new block key (each clears 16 KB)
    u64 icbis, icbiFlushes, dispatchMisses, pollSkips;
    u64 instrsRun, fallbacksRun;  // guest instructions executed by blocks, and how many of them were interpreted
    u64 slowLoads, slowStores;    // loads / stores through the C helpers (MMIO, write-gather pipe, non-standard BATs)
    u64 gatherStores;             // stores to the write-gather pipe through jit_set_gather's direct path
    JitOpCounts fallback;      // static: one per compiled occurrence
    JitOpCounts fallbackRun;   // dynamic: one per execution
    JitOpCounts exitOther;     // dispatcher returns for "other" reasons, by the guest instruction before g->pc
    u32 exitVector[32];        // ... or by exception vector (g->pc / 0x100) when the return went to one
    u32 sprRun[2][1024];       // dynamic: interpreted mfspr [0] / mtspr [1] by SPR number
} JitStats;

// ram/ramSize: host view of physical RAM starting at physical 0 (for the fast memory path).
Jit* jit_create(Gekko* g, u8* ram, u32 ramSize, u32 codeBytes);
void jit_destroy(Jit* j);
void jit_attach(Jit* j);                 // installs the JIT into the CPU's run loop (gekko_set_jit)
// The dispatcher as gekko_run calls it: compiled code from g->pc for up to about `budget` instructions; returns how many
// ran (the fuzzer's multi-block cases).
u32 jit_run(Jit* j, s64 budget);
void jit_invalidate(Jit* j, u32 pa);     // the 32-byte line at physical pa may hold changed code (icbi)
void jit_invalidate_all(Jit* j);
const JitStats* jit_stats(const Jit* j);
float jit_code_used(const Jit* j);  // fraction of the code buffer in use (a full buffer drops all compiled code)
float jit_words_per_insn(const Jit* j);  // host code words per guest instruction compiled since the last flush
// Stores to exactly guest address `ea` (the write-gather pipe) skip translation and the bus: compiled code hands the
// value to put(ctx, value, size) directly. Only for blocks compiled with the standard mappings.
void jit_set_gather(Jit* j, void (*put)(void* ctx, u32 v, u32 size), void* ctx, u32 ea);
// The pipe's partial 32-byte burst (console byte order) and its length: compiled code appends a store that does not
// complete the burst itself, as `put` would, and calls `put` for the store that does.
void jit_set_gather_buffer(Jit* j, u8* buf, u32* len);
void jit_set_linking(Jit* j, bool on);  // off: every block returns to the dispatcher (for comparison); drops all code
bool jit_linking(const Jit* j);
// Loads and stores call shared stubs for their MEM1 fast path (on by default) instead of inlining it; off inlines it
// (comparison). Drops all code.
void jit_set_mem_stubs(Jit* j, bool on);
// Traces (on by default): blocks follow unconditional branches and continue past conditional ones, instead of ending at
// every branch. Off ends each block at its first branch (comparison). Drops all code.
void jit_set_traces(Jit* j, bool on);
bool jit_traces(const Jit* j);
// The two halves of traces, for bisecting on the device: follow unconditional b, and compile conditional bc / blr /
// bctr as side exits. Both on by default; each change drops all compiled code. maxInsns 0: MAX_BLOCK_INSNS.
// side is a mask of TRACE_SIDE_* (0: no side exits).
enum { TRACE_SIDE_CR = 1, TRACE_SIDE_CTR = 2, TRACE_SIDE_IND = 4, TRACE_SIDE_FUSED = 8, TRACE_SIDE_ALL = 15 };
void jit_set_trace_parts(Jit* j, bool follow, u32 side, u32 maxInsns);
u32 jit_entries(const Jit* j);  // blocks in the cache now
// Called around every run of compiled code when set (the 3DS's performance-counter split, pmu.c).
extern void (*jit_run_begin_hook)(void);
extern void (*jit_run_end_hook)(void);
// Fault mode: integer loads and stores to MEM1 run inline without checks; an access elsewhere faults on the host and the
// host's handler calls jit_fault, which patches that site into a branch to its slow path and returns where to resume
// (false: not a fault-mode site). Needs jit_host_fault_init (the host's handler, and nothing mapped where the guest's
// other regions land); off when the host cannot. Drops all compiled code.
void jit_set_fault_mem(Jit* j, bool on);
bool jit_fault_mem(const Jit* j);
bool jit_fault(u32 hostPc, u32* resume);
// Code scheduled against ARM11 interlocks (tools/arm11_sched.py), for A/B: SCHED_STUBS, memory stubs whose range check
// reads r0 and whose loads leave the byte swap to the site; SCHED_CONST, constants added in two immediates instead of
// loaded (a negative 16-bit displacement: 2 words instead of up to 5). Drops all compiled code.
enum { SCHED_STUBS = 1, SCHED_CONST = 2, SCHED_ALL = 3 };
void jit_set_sched(Jit* j, u32 mask);
u32 jit_sched(const Jit* j);
// Optimizations of the optimizing JIT (docs/OPT_JIT_PLAN.md), for A/B; each change drops all compiled code.
// OPT_HOIST: the first D-form load or store through base register rA in a block checks rA against a window inside MEM1;
// later accesses through the unchanged rA run unchecked. A failed check recompiles that block with per-access checks.
// Has no effect in fault mode (accesses are unchecked there already).
// OPT_KSTORE: stfs / psq_st of values the block knows are exact singles convert without the round-trip check.
// OPT_FCACHE: exact-single FPR lanes live in VFP registers as singles within a block (jit.c "float lanes").
// OPT_CALLS: traces follow bl into the callee (LR written as usual) and a blr while LR still holds that return address
// back to it, so small functions (TP's PSVEC*, J3DFifoLoad*) cost no block exit and no indirect-table lookup.
// OPT_RETSTUB: blr / bctr (and bctrl, conditional forms too) branch to shared stubs that read LR / CTR and look the target up,
// one hot word per site instead of four; g->pc is written only when the lookup misses.
// OPT_SYSCALL: sc runs with its handler in one interpreter call inside the block (when the handler at 0xC00 is a short
// run of SPR moves, ori, isync / sync ending in rfi, as the SDK's PPCSync is), instead of ending the block and costing
// several dispatcher round trips; the block counts the handler's instructions and goes on at pc + 4.
// OPT_HLELINK: a native stand-in's block (hle.h) goes on through the indirect table to where the stand-in stopped,
// instead of returning to the dispatcher after every call.
// OPT_IND2: the indirect-branch table is 2-way: a target whose slot holds another one goes to the slot next to it (same
// cache line), which ST_INDIRECT checks second. The 2DS's misses were ~3,400/s "slot taken" (job 157).
// OPT_NATIVE2: instructions the 2DS interpreted thousands of times a second compiled natively (each fallback is a C
// excursion, which costs I-cache misses as dispatcher round trips did): ps_cmpu0 / ps_cmpo0 / ps_cmpu1 / ps_cmpo1,
// lmw / stmw (a shared copy loop for aligned ranges inside MEM1), divw / divwu (a direct C call), sraw.
// OPT_ROTMASK: rlwinm / rlwimi whose mask does not wrap as one to three shifts instead of a rotate and a mask that may
// need a constant of up to four words.
// OPT_CRSTUB: a compare's CR-field update (BIC and three conditional ORRs) is a call to a shared stub, one word instead
// of four: the 2DS rewards less distinct hot code (traces lost 5.5% to their duplicated code, job 154).
enum { OPT_HOIST = 1, OPT_KSTORE = 2, OPT_FCACHE = 4, OPT_CALLS = 8, OPT_RETSTUB = 16, OPT_SYSCALL = 32, OPT_HLELINK = 64,
       OPT_CRSTUB = 128, OPT_ROTMASK = 256, OPT_NATIVE2 = 512, OPT_IND2 = 1024, OPT_ALL = 2047 };
void jit_set_opt(Jit* j, u32 mask);
// dcbst / dcbf / dcbz on MEM1 stamp their 4 KB page in `stamps` ({ u32 now; u32 page[]; }, gx.h MemStamps), for the
// display-list vertex cache. NULL (the default): not tracked. Drops all compiled code.
void jit_set_mem_stamps(Jit* j, void* stamps);
u32 jit_opt(const Jit* j);
// Memory slow paths: shared stubs (default) or the old per-site code (true), for A/B; drops all compiled code.
void jit_set_slow_inline(Jit* j, bool on);
// XER[SO] in CR updates: compares and record forms leave CR bit SO out while XER[SO] has never been seen set (games
// almost never use overflow instructions; Dolphin does not emulate SO in CR at all). The first time XER[SO] is set the
// JIT drops its code and compiles exact CR updates from then on. jit_set_so_exact(j, false) starts over (fuzzer).
void jit_set_so_exact(Jit* j, bool on);
// Each block stores its start in Gekko.hostPc for the sampling profiler (host3ds/jitsample.c): one store per block
// entered, so it is off unless asked for. Drops all compiled code.
void jit_set_profile(Jit* j, bool on);
bool jit_so_exact(const Jit* j);
bool jit_slow_inline(const Jit* j);
bool jit_mem_stubs(const Jit* j);
void jit_print_stats(const Jit* j, void (*print)(const char* line));

// Testing: limit blocks to this many instructions (0 = default) and run one block from g->pc, returning the
// number of instructions it executed.
extern u32 jit_debug_max_insns;
extern const u32* jit_debug_last_code;
extern u32 jit_debug_last_words;
extern void (*jit_debug_on_compile)(const u32* code, u32 words);  // called with every block compiled (tests)
extern u32 jit_debug_plant;  // 1 = compiled single-precision results skip the rounding to single (the fuzzer's float self-test), 3 = fused compare-branches test the opposite condition, 4 = the register cache loses a write-back, 9 = a failed window check (OPT_HOIST) counts its instruction as run, 10 = a deferred byte swap is lost at a write-back, 11 = sc's handler (OPT_SYSCALL) counts one instruction short
u32 jit_run_one_block(Jit* j);

// Compiled float code assumes the host FPSCR is 0x03000000: round to nearest (the 3DS kernel starts threads with
// round toward zero), flush-to-zero and default NaN (denormals and NaN payloads are not reproduced; flush-to-zero also
// keeps VFP11 from bouncing denormals to support code). Call it in the thread that runs compiled code.
#if defined(__arm__)
static inline void jit_host_fpscr_init(void) { __asm__ volatile("vmsr fpscr, %0" : : "r"(0x03000000u)); }
#else
static inline void jit_host_fpscr_init(void) {}
#endif

// Host services each frontend provides: executable memory and instruction-cache maintenance.
void* jit_host_alloc_exec(u32 bytes);
void jit_host_sync_icache(void* start, u32 bytes);
// Installs the host's fault handler (once; it calls jit_fault) and checks that the host addresses the guest's non-MEM1
// regions land on with base ram - 0x80000000 are not mapped (reserving them where it can). false: fault mode stays off.
bool jit_host_fault_init(const u8* ram);

// Sampling profiler support: jit_in_run is 1 while compiled code (or a helper it called) runs; each block stores its
// host start address in Gekko.hostPc. jit_map_hosts replaces host addresses with the guest PC of the block they are
// in (0 when none).
extern volatile u32 jit_in_run;
void jit_map_hosts(const Jit* j, u32* addrs, u32 n);
