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
extern u64 g_hleCost[3];  // what the stand-ins cost core 0: calls, timed calls (1 in 16), g_profClock ticks in them
// Forget what was recognised (code in the page holding physical address pa may have changed).
void hle_invalidate(u32 pa);
// Stand-ins on (default) or off (comparison); compiled code holds hooks, so drop it after a change (jit_invalidate_all).
void hle_set_enabled(bool on);
// The ShdwDraw walk memo (hle_tp.c; --shdwmemo): repeated calls replay a recorded walk. g_shmStats: calls, calls
// replayed to the end, segments replayed, segments run by the walk (holes, budget), calls recorded, records dropped (DMA).
void hle_set_shdw_memo(bool on);
bool hle_shdw_memo(void);
extern u64 g_shmStats[6], g_shmMiss[8], g_shmDiff[24];
// The triangle path natively (hle_tp.c; --shdwtri): g_triStats: to the shadow callback, skipped, (unused), refused.
void hle_set_shdw_tri(bool on);
bool hle_shdw_tri(void);
extern u64 g_triStats[4];
// Its shadow callback natively too (--shdwcb): g_cbStats: set a triangle, returned before set, poly full, not taken.
void hle_set_shdw_cb(bool on);
bool hle_shdw_cb(void);
extern u64 g_cbStats[4];
// Guest frame skip (experiment): the function at g_gfskipAddr returns at once on every other call while g_gfskipOn.
extern u32 g_gfskipAddr;
extern volatile bool g_gfskipOn;
extern u64 g_gfskipStats[2];
extern u32 g_gfskipEvery;  // one call in this many runs (2: every other)
extern u32 g_gfskipGuard[4];  // guest bytes that must be zero for a skip (TP: the fade's state)
typedef struct { u32 ptr, off, val; } GfskipOk;  // the word at *(u32*)ptr + off must equal val for a skip
extern GfskipOk g_gfskipOk[4];
extern u64 g_gfskipGuarded;
bool hle_enabled(void);
// Counters for the frontends: stand-in runs and guest instructions stood in for (all stand-ins).
extern u64 g_hleRuns, g_hleInsns;
// --pmu-split hle (3DS): called around every ShdwDraw stand-in run, for performance counters split to it.
extern void (*hle_shdw_begin_hook)(void);
extern void (*hle_shdw_end_hook)(void);
extern void (*hle_shdw_fast_begin_hook)(void);
extern void (*hle_shdw_fast_end_hook)(void);
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
void hle_ps_disable(const char* prefixes);  // --psoff A,B: stand-ins named A* or B* stay off (set before code compiles)
void hle_ps_set_group(const char* prefixes, bool on);  // a group on or off at run time (drop compiled code after)
bool hle_ps_enabled(void);
u32 hle_ps_run(Gekko* g);
u32 hle_ps_count(void);            // functions in the table
const char* hle_ps_name(u32 k);
extern u64 g_psCalls[], g_psRefused;  // calls per function (index hle_ps_count(): all), refused calls
extern u64 g_psRefusedBy[], g_psStops[];
extern u64 g_psWhy[6];  // refusals by reason (hle_ps.c ps_call)  // per function: in-place calls refused, and pipe stops
// The JIT's in-place call (hle_ps.c): the function at target, or -1 (*gprs: it writes r14-r31); then the call itself
// (LR set by the caller).
int hle_ps_inline(u32 target, bool* gprs, u32* size);
u32 hle_ps_inline_call(Gekko* g, u32 d);  // instructions run (0 refused); bit 31: the GP asked the CPU to stop
// X3 explicit entry operands. The JIT fills only `inputs`; unrelated dirty cache entries never enter guest memory.
typedef struct { u32 gpr[32], cr, xer; } HleFastArgs;
typedef struct { u32 id, size, inputs, partialWrites, fpInputs, fpWrites, restored; } HleFastInfo;
enum { HLE_FAST_GPR = 1, HLE_FAST_GX = 2, HLE_FAST_XYZ = 4 };
bool hle_ps_fast_info(u32 target, u32 groups, HleFastInfo* info);
u32 hle_ps_fast_call(Gekko* g, u32 id, const HleFastArgs* args);
extern u64 g_psFastCalls[3], g_psFastRefused[3]; // GPR, GX, cXyz; successful/refused experimental ABI calls
// Where the stand-ins' gather-pipe bytes go (the system's gp_fifo_write).
void hle_ps_set_pipe(void (*write)(void* ctx, const u8* data, u32 len), void* ctx);
