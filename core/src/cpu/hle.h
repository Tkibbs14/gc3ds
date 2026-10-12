#pragma once
// Native stand-ins for hot guest loops (hle_tp.c). A stand-in starts at a hooked guest PC, does exactly what the guest
// instructions from there would do to the registers and guest memory, and stops at a PC where guest code takes over
// again; it reports how many guest instructions it stood in for, so emulated time advances as it would have. The
// hooked code is recognised by a hash of its bytes, never by the game's identity alone.
#include "gekko.h"

void hle_set_memory(u8* mem1, u32 size);
// Whether pc starts a stand-in (the JIT ends blocks before it and compiles it as a call to hle_run).
bool hle_is_hook(Gekko* g, u32 pc);
// Runs the stand-in at g->pc: guest instructions stood in for (g->pc is then where guest code goes on), or 0 when it
// cannot start there (nothing changed).
u32 hle_run(Gekko* g);
// Forget what was recognised (code in the page holding physical address pa may have changed).
void hle_invalidate(u32 pa);
// Stand-ins on (default) or off (comparison); compiled code holds hooks, so drop it after a change (jit_invalidate_all).
void hle_set_enabled(bool on);
bool hle_enabled(void);
// Counters for the frontends: stand-in runs and guest instructions stood in for.
extern u64 g_hleRuns, g_hleInsns;
