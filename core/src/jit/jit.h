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
    u64 blocksNoCheck;    // ... of them without the budget branch (OPT_LATCH)
    u32 keySeen[8], keyCount[8];  // block keys (MSR / FPSCR / HID2 bits) blocks were compiled under, and how often
    u64 blocksRun;  // entries from the C dispatcher (linked blocks run without coming back)
    u64 links;      // exits patched to jump straight to their target block
    u64 fusedBranches, forwards;  // compile time: bc on a compare's ARM flags; GPR reads served by the register cache
    u64 traceFollowed, traceSideExits, traceCalls;  // ...; traceCalls: bl followed into the callee (OPT_CALLS)  // compile time: unconditional branches followed, conditional branches compiled as side exits
    u64 compileFails[3];  // blocks not compiled: hot code or exits overflowed (or a slow path without its tail), cold code overflowed, fix-ups overflowed
    u64 wordsHot, wordsExit, wordsCold, insnsKept;  // compile time, kept blocks: host words in the hot path, exit stubs, cold slow paths
    u64 dispLink, dispIndirect, dispBudget, dispOther;  // why compiled code returned to the dispatcher
    u64 linkNotPlain, linkGen, linkKey, linkNoSite, linkAlready;  // link requests not linked: target a poll / idle block, code dropped since, other key, no site (the run left elsewhere), site already linked
    u64 missPoll, missFirst, missTaken, missEmpty, missKey;
    u64 faults;      // fault-mode sites patched to their slow path (jit_fault)
    u64 deopts;      // blocks recompiled with per-access checks after a failed window check (OPT_HOIST)
    u64 slowMirror;  // slow-path loads / stores whose address is in MEM1's uncached mirror (0xC0000000)  // ... / slot held another target / slot empty / other key  // indirect-table misses whose target is a polling-loop or idle block / was never dispatched
    u64 fastSwitches;  // indirect tables reassigned to a new block key (each clears 16 KB)
    u64 icbis, icbiFlushes, dispatchMisses, pollSkips;
    u64 instrsRun, fallbacksRun;  // guest instructions executed by blocks, and how many of them were interpreted
    u64 slowLoads, slowStores;    // loads / stores through the C helpers (MMIO, write-gather pipe, non-standard BATs)
    u64 gatherStores;             // stores to the write-gather pipe through jit_set_gather's direct path
    u64 pipeKnown;                // pipe stores compiled to the append stub (an address known when compiling, OPT_PIPEK)
    JitOpCounts fallback;      // static: one per compiled occurrence
    JitOpCounts fallbackRun;   // dynamic: one per execution
    JitOpCounts exitOther;     // dispatcher returns for "other" reasons, by the guest instruction before g->pc
    u32 exitVector[32];        // ... or by exception vector (g->pc / 0x100) when the return went to one
    u32 sprRun[2][1024];       // dynamic: interpreted mfspr [0] / mtspr [1] by SPR number
    // Interpreted psq_st / psq_stu / psq_stx / psq_stux by where they store: [0] the locked cache, [1] MEM1 with every
    // element aligned (so the compiled code's GQR check failed: the GQR changed after compiling), [2] MEM1 misaligned,
    // [3] anywhere else; and by the GQR's store type at run time (0 float, 4 u8, 5 u16, 6 s8, 7 s16).
    u64 psqStFb[4], psqStFbType[8];
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
// Quantized psq_l / psq_st to the locked cache (16 KB at 0xE0000000) use this host copy directly instead of the
// interpreter; NULL (the default) leaves them to the interpreter.
void jit_set_locked_cache(Jit* j, u8* lc);
bool jit_lc_direct(const Jit* j);  // set, and the current BATs map 0xE0000000 to the locked cache
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
// Fault mode's accesses clear this bit of the guest address before adding MEM1's base: uncached MEM1 (0xC0000000) then
// reads MEM1 itself, and the other regions (EFB 0xC8, MMIO 0xCC, locked cache 0xE0) land 0x08 / 0x0C / 0x20 x 2^24
// bytes past MEM1, in user space, where a fault is delivered safely (above 0x40000000 it crashed the 2DS kernel).
#define JIT_FAULT_BIC 0x40000000u
void jit_set_fault_mem(Jit* j, bool on);
bool jit_fault_mem(const Jit* j);
// The code buffer past the shared stubs, emptied, as scratch for host measurements (call jit_invalidate_all after).
u32* jit_scratch(Jit* j, u32* words);
bool jit_fault(u32 hostPc, u32* resume);
extern bool jit_fault_fixed_data;  // 3DS (host3ds/jit_3ds.c): the kernel writes fault data to a fixed buffer (--faultdata fixed)
extern bool jit_fault_nosync;  // fault patches get no cache maintenance (a stale copy just faults again)
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
// Only in blocks with at least jit_fc_min float instructions before their first branch (jit.c fc_gate).
extern int jit_fc_min;
// OPT_CALLS: traces follow bl into the callee (LR written as usual) and a blr while LR still holds that return address
// back to it, so small functions (TP's PSVEC*, J3DFifoLoad*) cost no block exit and no indirect-table lookup.
// OPT_RETSTUB: blr / bctr (and bctrl, conditional forms too) branch to shared stubs that read LR / CTR and look the target up,
// one hot word per site instead of four; g->pc is written only when the lookup misses.
// OPT_SYSCALL: sc runs with its handler in one interpreter call inside the block (when the handler at 0xC00 is a short
// run of SPR moves, ori, isync / sync ending in rfi, as the SDK's PPCSync is), instead of ending the block and costing
// several dispatcher round trips; the block counts the handler's instructions and goes on at pc + 4.
// OPT_HLELINK: a native stand-in's block (hle.h) goes on through the indirect table to where the stand-in stopped,
// instead of returning to the dispatcher after every call.
// OPT_BEFLOAT: the float load / store stubs (lfs, lfd, psq_l, stfd, and stfs / psq_st of known singles) move the guest
// value straight between RAM and a VFP register in big-endian data mode (SETEND BE) instead of ldr -> rev -> vmov:
// the core <-> VFP moves cost ~3.5-4 cycles each and do not overlap (docs/FINDINGS.md run 3).
// OPT_LATCH (owner decision 2026-10-15: event timing may drift): only blocks that may branch back to their own address or
// below keep the budget branch (every loop of linked blocks passes one; ST_INDIRECT and the dispatcher check too), so a
// run may go past its budget by a chain of forward blocks. Every block still counts its instructions exactly. Modelled
// at 0.70 hot words per block run (2.5%; TP 850-880, JIT_REGSTAT).
// OPT_ICBI: icbi no longer ends its block: the interpreter runs it in place and compiled code leaves only when it
// dropped compiled code (a page that held some). TP ran ~62,000 icbi exits a second, each a dispatcher round trip,
// against ~10 flushes a second (job 184).
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
       OPT_CRSTUB = 128, OPT_ROTMASK = 256, OPT_NATIVE2 = 512, OPT_IND2 = 1024, OPT_BEFLOAT = 2048, OPT_LATCH = 4096,
       OPT_ICBI = 8192, OPT_INLMEM = 16384, OPT_FASTFLOAT = 32768, OPT_PIPEK = 65536, OPT_PIPEPS = 131072, OPT_ALL = 262143 };
// OPT_PIPEPS: the shared psq_st stubs (plain float and known singles) append to the write-gather pipe's buffer themselves
// when the address is the pipe's and the burst has room (GXLoadPosMtxImm and its kin), instead of failing to the site's
// cold path and a jit_gather call per element. Sites are unchanged (a per-site version lost 2.3%: job 371).
// OPT_PIPEK: a D-form store (stw sth stb stfs) whose address the block knows (a constant from lis / li / addi / ori)
// and lies in the write-gather pipe appends to the pipe's burst through a shared stub instead of the RAM check and the
// slow path: the SDK's GX functions write the graphics commands this way.
// OPT_FASTFLOAT (owner-approved experiment, 2026-10-17): single-precision and paired-single results are not rounded to
// single, frC is not cut to 25 bits, and stores convert with the hardware's rounding: results may differ from Gekko's
// in the last bits (exact by default).
// OPT_INLMEM: checked integer loads and stores check and access inline (6 words, the fast path takes no branch) instead
// of calling the shared memory stub (3 words, but a call and a return: two taken branches, ~3 cycles each on the 2DS).
// Taken branches cost ~11-13% of core 0 (MEASUREMENTS.md, jobs 247 and 251); the stubs were adopted for footprint.
void jit_set_opt(Jit* j, u32 mask);
// dcbst / dcbf / dcbz on MEM1 stamp their 4 KB page in `stamps` ({ u32 now; u32 page[]; }, gx.h MemStamps), for the
// display-list vertex cache. NULL (the default): not tracked. Drops all compiled code.
void jit_set_mem_stamps(Jit* j, void* stamps);
u32 jit_opt(const Jit* j);
// Measurement only: functions at these addresses return at once while jit_skip_on (drop compiled code after a change).
extern u32 jit_skip_fn[4];
extern volatile bool jit_skip_on;
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
extern u32 jit_debug_plant;  // 1 = compiled single-precision results skip the rounding to single (the fuzzer's float self-test), 3 = fused compare-branches test the opposite condition, 4 = the register cache loses a write-back, 9 = a failed window check (OPT_HOIST) counts its instruction as run, 10 = a deferred byte swap is lost at a write-back, 11 = sc's handler (OPT_SYSCALL) counts one instruction short, 12 = quantized psq accesses to the locked cache land 4 bytes off
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
// Hot-block relayout (--relayout, --ab relayout): the blocks the sampler saw, most sampled first (only blocks of the
// current block key, no idle or polling blocks), as guest PCs; then, after a flush, jit_precompile compiles them in
// that order, so the code that runs every frame lies together at the start of the buffer instead of among blocks that
// ran once (I-fetch stalls were 21% of core 0, job 184). Needs blocks to store their start (jit_set_profile).
u32 jit_hot_blocks(const Jit* j, const u32* hostAddrs, u32 n, u32* pcs, u32 max);
u32 jit_precompile(Jit* j, const u32* pcs, u32 n);  // how many were compiled (stops if the buffer fills)
// The sampler's work arrays, allocated while the heap is still whole (jitsample_start); false: no memory.
bool jit_sample_prepare(void);
bool jit_map_hosts(const Jit* j, u32* addrs, u32 n);  // false: no memory for its work arrays (addrs unchanged)
// The sampled code footprint (see jit.c): out[k] = {blocks, host bytes} covering 50, 80, 90, 99, 100% of the samples.
void jit_footprint(const Jit* j, const u32* hostAddrs, u32 n, u32 out[5][2]);
