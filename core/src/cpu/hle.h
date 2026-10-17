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
// cannot start there (nothing changed). `budget`: guest instructions the caller has left in its slice; the stand-in
// stops at its next loop head once it has used that many (as compiled blocks do), so time and events stay on schedule.
u32 hle_run(Gekko* g, u32 budget);
// Forget what was recognised (code in the page holding physical address pa may have changed).
void hle_invalidate(u32 pa);
// Stand-ins on (default) or off (comparison); compiled code holds hooks, so drop it after a change (jit_invalidate_all).
void hle_set_enabled(bool on);
bool hle_enabled(void);
// Counters for the frontends: stand-in runs and guest instructions stood in for (all stand-ins).
extern u64 g_hleRuns, g_hleInsns;
// --pmu-split hle (3DS): called around every ShdwDraw stand-in run, for performance counters split to it.
extern void (*hle_shdw_begin_hook)(void);
extern void (*hle_shdw_end_hook)(void);
extern u64 g_shdwStats[4];
extern u64 g_shdwWalk[10];  // the cell walk's shape (hle_tp.c)  // ShdwDraw stand-in (3DS): calls, triangles skipped fast, other instructions, ticks inside
// J3D's display-list writers (hle_gd.c): called by the functions above; their own counts are also in g_hleGd*.
void hle_gd_set_memory(u8* mem1, u32 size);
void hle_gd_invalidate(u32 pa);
bool hle_gd_is_hook(u32 pc);
// The J3DGD stand-ins alone on (default) or off (--ab hlegd; drop compiled code after a change).
void hle_gd_set_enabled(bool on);
bool hle_gd_enabled(void);
u32 hle_gd_run(Gekko* g);
extern u64 g_hleGdRuns, g_hleGdInsns;
// The THP movie decoder's inverse DCTs (hle_thp.c), recognised wherever its module is loaded; called by the functions
// above. lc: the locked cache's 16 KB (its output and coefficients live there).
void hle_thp_set_memory(u8* mem1, u32 size, u8* lc);
void hle_thp_invalidate(u32 pa);
bool hle_thp_is_hook(u32 pc);
// On (default) or off (--ab thp; drop compiled code after a change).
void hle_thp_set_enabled(bool on);
bool hle_thp_enabled(void);
u32 hle_thp_run(Gekko* g);
extern u64 g_thpStats[3];  // calls, guest instructions stood in for, calls refused (an address outside MEM1 / the cache)
// The SDK's paired-single matrix / vector library (hle_ps.c), recognised at any address by its bytes.
void hle_ps_set_memory(u8* mem1, u32 size, u8* lc);
void hle_ps_invalidate(u32 pa);
bool hle_ps_is_hook(u32 pc);
void hle_ps_set_enabled(bool on);  // --ab ps / --ps off; drop compiled code after a change
bool hle_ps_enabled(void);
u32 hle_ps_run(Gekko* g);
u32 hle_ps_count(void);            // functions in the table
const char* hle_ps_name(u32 k);
extern u64 g_psCalls[], g_psRefused;  // calls per function (index hle_ps_count(): all), refused calls
// The JIT's in-place call (hle_ps.c): the function at target and its instruction count, or -1; then the call itself
// (LR set by the caller): 1 when the stand-in ran, 0 when it refused.
int hle_ps_inline(u32 target, u32* n);
u32 hle_ps_inline_call(Gekko* g, u32 d);  // also 2: it ran and the GP asked the CPU to stop (leave after it)
// Where the stand-ins' gather-pipe bytes go (the system's gp_fifo_write).
void hle_ps_set_pipe(void (*write)(void* ctx, const u8* data, u32 len), void* ctx);
