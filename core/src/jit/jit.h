#pragma once
// Gekko -> ARMv6K JIT (32-bit ARM hosts only: the 3DS, and ARM Linux under qemu for testing).
//
// Level 1: blocks of guest code end at the first branch. Common integer, load/store and branch instructions are
// translated; everything else runs through the interpreter's handler for that one instruction, so any program runs
// correctly from the start and speed grows with coverage. Guest registers stay in the Gekko struct (no register
// allocation yet); blocks return to the dispatcher (no linking yet).
#include "../cpu/gekko.h"

typedef struct Jit Jit;

typedef struct {
    u64 blocksCompiled, blocksRun, instrsCompiled, instrsNative, flushes;
    u32 fallbackPrimary[64];   // compiled instructions left to the interpreter, by primary opcode
    u32 fallback31[1024];      // ... and by extended opcode for groups 31 / 19 / 63 / 59 / 4
    u32 fallback19[1024], fallback63[1024], fallback59[32], fallback4[1024];
} JitStats;

// ram/ramSize: host view of physical RAM starting at physical 0 (for the fast memory path).
Jit* jit_create(Gekko* g, u8* ram, u32 ramSize, u32 codeBytes);
void jit_destroy(Jit* j);
void jit_attach(Jit* j);                 // installs the JIT into the CPU's run loop (gekko_set_jit)
void jit_invalidate(Jit* j, u32 pa);     // the 32-byte line at physical pa may hold changed code (icbi)
void jit_invalidate_all(Jit* j);
const JitStats* jit_stats(const Jit* j);
void jit_print_stats(const Jit* j, void (*print)(const char* line));

// Testing: limit blocks to this many instructions (0 = default) and run one block from g->pc, returning the
// number of instructions it executed.
extern u32 jit_debug_max_insns;
extern const u32* jit_debug_last_code;
extern u32 jit_debug_last_words;
u32 jit_run_one_block(Jit* j);

// Host services each frontend provides: executable memory and instruction-cache maintenance.
void* jit_host_alloc_exec(u32 bytes);
void jit_host_sync_icache(void* start, u32 bytes);
