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
    u64 blocksRun;  // entries from the C dispatcher (linked blocks run without coming back)
    u64 links;      // exits patched to jump straight to their target block
    u64 fusedBranches, forwards;  // compile time: bc on a compare's ARM flags; GPR reads served by the register cache
    u64 compileFails[3];  // blocks not compiled: hot code or exits overflowed (or a slow path without its tail), cold code overflowed, fix-ups overflowed
    u64 wordsHot, wordsExit, wordsCold, insnsKept;  // compile time, kept blocks: host words in the hot path, exit stubs, cold slow paths
    u64 dispLink, dispIndirect, dispBudget, dispOther;  // why compiled code returned to the dispatcher
    u64 fastSwitches;  // indirect tables reassigned to a new block key (each clears 16 KB)
    u64 icbis, icbiFlushes, dispatchMisses, pollSkips;
    u64 instrsRun, fallbacksRun;  // guest instructions executed by blocks, and how many of them were interpreted
    u64 slowLoads, slowStores;    // loads / stores through the C helpers (MMIO, write-gather pipe, non-standard BATs)
    u64 gatherStores;             // stores to the write-gather pipe through jit_set_gather's direct path
    JitOpCounts fallback;      // static: one per compiled occurrence
    JitOpCounts fallbackRun;   // dynamic: one per execution
    u32 sprRun[2][1024];       // dynamic: interpreted mfspr [0] / mtspr [1] by SPR number
} JitStats;

// ram/ramSize: host view of physical RAM starting at physical 0 (for the fast memory path).
Jit* jit_create(Gekko* g, u8* ram, u32 ramSize, u32 codeBytes);
void jit_destroy(Jit* j);
void jit_attach(Jit* j);                 // installs the JIT into the CPU's run loop (gekko_set_jit)
void jit_invalidate(Jit* j, u32 pa);     // the 32-byte line at physical pa may hold changed code (icbi)
void jit_invalidate_all(Jit* j);
const JitStats* jit_stats(const Jit* j);
float jit_code_used(const Jit* j);  // fraction of the code buffer in use (a full buffer drops all compiled code)
float jit_words_per_insn(const Jit* j);  // host code words per guest instruction compiled since the last flush
// Stores to exactly guest address `ea` (the write-gather pipe) skip translation and the bus: compiled code hands the
// value to put(ctx, value, size) directly. Only for blocks compiled with the standard mappings.
void jit_set_gather(Jit* j, void (*put)(void* ctx, u32 v, u32 size), void* ctx, u32 ea);
void jit_set_linking(Jit* j, bool on);  // off: every block returns to the dispatcher (for comparison); drops all code
bool jit_linking(const Jit* j);
// Loads and stores call shared stubs for their MEM1 fast path (on by default) instead of inlining it; off inlines it
// (comparison). Drops all code.
void jit_set_mem_stubs(Jit* j, bool on);
bool jit_mem_stubs(const Jit* j);
void jit_print_stats(const Jit* j, void (*print)(const char* line));

// Testing: limit blocks to this many instructions (0 = default) and run one block from g->pc, returning the
// number of instructions it executed.
extern u32 jit_debug_max_insns;
extern const u32* jit_debug_last_code;
extern u32 jit_debug_last_words;
extern void (*jit_debug_on_compile)(const u32* code, u32 words);  // called with every block compiled (tests)
extern u32 jit_debug_plant;  // 1 = compiled single-precision results skip the rounding to single (the fuzzer's float self-test), 3 = fused compare-branches test the opposite condition, 4 = the register cache loses a write-back
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

// Sampling profiler support: jit_in_run is 1 while compiled code (or a helper it called) runs; each block stores its
// host start address in Gekko.hostPc. jit_map_hosts replaces host addresses with the guest PC of the block they are
// in (0 when none).
extern volatile u32 jit_in_run;
void jit_map_hosts(const Jit* j, u32* addrs, u32 n);
