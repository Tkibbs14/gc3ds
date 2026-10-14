#pragma once
// Ahead-of-time translated guest code (redesign candidate C12, docs/research/AOT_PLAN.md): a game's functions
// translated to C on the PC (core/tools/aot/ppc2c.py, from the user's own disc) and compiled into the emulator. The
// dispatchers (the JIT's and the interpreter's) call aot_run whenever the guest PC is an entry of a translated function
// whose guest bytes still match; everything else runs as before. Without generated code (core/src/aotgen empty, as in
// the repository and CI) nothing is translated and aot_run never runs anything.
#include "gekko.h"

void aot_set_enabled(bool on);
bool aot_enabled(void);
bool aot_available(void);  // generated code is linked in
void aot_set_memory(u8* mem1);
// The write-gather pipe (stores to `ea` go to put), as jit_set_gather.
void aot_set_gather(void (*put)(void* ctx, u32 v, u32 size), void* ctx, u32 ea);
// ... and its partial burst, which stores append to inline (as jit_set_gather_buffer).
void aot_set_gather_buffer(u8* buf, u32* len);
// Live switches, for finding which part of the translation a problem comes from without a new build: direct calls up
// to depthMax deep (0: none), the EABI's callee-saved registers trusted after a call (abi), pipe stores appended inline
// (pipe). Defaults: 40, on, on.
void aot_set_switches(u32 depthMax, bool abi, bool pipe);
void aot_get_switches(u32* depthMax, bool* abi, bool* pipe);
// Whether the machine state lets translated code run: standard address translation, FP and paired singles on,
// round to nearest (the state the translation assumes, as the JIT's block key).
bool aot_state_ok(const Gekko* g);
// Whether pc is the entry of a translated function (cheap; the code's bytes are checked when it first runs).
bool aot_is_entry(u32 pc);
// Runs translated functions from g->pc for up to about `budget` guest instructions (a function may run past it to its
// next loop head) and returns how many ran, with g->pc where guest code goes on; 0 when g->pc is not an entry.
// g->cycles must be the current guest time; it is unchanged on return (the caller adds what ran).
u32 aot_run(Gekko* g, s32 budget);
void aot_invalidate(u32 pa);   // code in the 4 KB page holding pa may have changed
void aot_invalidate_all(void);
// Counters: functions entered, guest instructions run, interpreter fallbacks, slow-path accesses.
typedef struct { u64 calls, insns, fallbacks, slow, runs, hashFail; } AotStats;
extern AotStats aot_stats;
