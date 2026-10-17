#pragma once
// GX -> PICA200 backend for the 3DS (citro3d). Replaces the software renderer: draws, EFB clears and EFB copies run
// on the GPU, and copies to the XFB stay on the GPU ("virtual XFB").
#include "../gx.h"
#include "../vfast.h"

// Initialises citro3d and attaches the backend to s. Call after gfxInitDefault. Returns false if the GPU resources
// could not be allocated (the caller keeps the software renderer).
bool pica_attach(System* s);
// Shows the field the VI is scanning out (physical XFB address xfbAddr) on the top screen. Returns false when no GPU
// XFB copy has happened since the program started, so the XFB in RAM is the only image (programs that draw with the
// CPU, such as consoles); the caller then converts RAM.
bool pica_present(System* s, u32 xfbAddr);
// Forgets every GPU-side copy and cached texture; call before booting another program.
void pica_reset(void);
// Saves the next presented top-screen image as a 400 x 240 BMP at `path` (ignored while one is in progress).
// Callable from the CPU thread; the GX thread does the work at its next two presents.
void pica_capture(const char* path);
// Called (on a background thread) with each finished capture (a 400x240 BMP in memory); NULL: none.
extern void (*pica_capture_done)(const void* bmp, u32 bytes);
// Lighting in the vertex shader (on by default when the shader provides it); off lights on the CPU (comparison).
void pica_set_shader_light(bool on);
bool pica_shader_light(void);
// Position transform in the vertex shader with a palette of matrices (on by default when the shader provides it;
// lines, points and Z-texture draws stay on the CPU); off transforms and culls every vertex on the CPU (comparison).
void pica_set_gpu_transform(bool on);
bool pica_gpu_transform(void);

// Work counters since start (the frontend logs them per second): C3D_DrawArrays calls (batches), command-list
// submits (C3D_FrameEnd) and splits, textures decoded from RAM and their size in VRAM/FCRAM, EFB -> texture and
// EFB -> XFB copies, EFB clears; palette matrices uploaded and draw calls made to change palette slots or the
// projection inside a batch (GPU-transformed batches).
// tevChanges: draws whose TEV registers differ from the previous draw's; tevCompiles: of those, compiled anew (the
// rest came from the combiner-plan cache).
typedef struct { u64 drawCalls, submits, splits, texDecodes, texDecodeBytes, efbCopies, xfbCopies, clears, palUploads, subDraws,
                 tevChanges, tevCompiles,
                 texFails; } PicaStats;  // texFails: textures not decoded for want of memory (drawn untextured)
extern PicaStats g_picaStats;
// Lean state (off by default until measured): citro3d setters whose result is the state citro3d already holds are
// skipped, so its draw calls upload only what changed. Off when citro3d's context layout is not the expected one.
// One bit per kind of skipped setter (bisection on the 2DS): cull mode, texture binds, combiner stages, the combiner
// buffer, the effect block's dirty bit after apply_frag, float uniforms.
enum { LEAN_CULL = 1, LEAN_TEX = 2, LEAN_STAGES = 4, LEAN_TEVBUF = 8, LEAN_EFFECT = 16, LEAN_UNIF = 32, LEAN_ALL = 63 };
void pica_set_lean(u32 mask);
u32 pica_lean(void);
// Raw draw calls (off by default until measured): a batch's draw call as citro3d's register words copied in one block,
// and C3Di_UpdateContext only when citro3d has state to upload. Off for good once a compared draw differed.
void pica_set_rawdraw(bool on);
bool pica_rawdraw(void);
// Command buffer prefetch (off by default until measured): after each batch's draw call, the next lines of citro3d's
// command buffer are prefetched so the following calls do not miss to FCRAM.
void pica_set_cmdpld(bool on);
bool pica_cmdpld(void);
// Raw state (off by default until measured; with raw draws): the blocks a batch changes (effect, texture units,
// combiner stages and buffer, vertex-shader float uniforms) written as citro3d would, without C3Di_UpdateContext.
void pica_set_rawstate(bool on);
bool pica_rawstate(void);
void pica_set_rawplant(bool on);  // the raw draw comparison's self-test: the next compared state differs
// Where the GX thread is (one byte it sets at each wait and in the draw call), for the watchdog: a stalled run says
// whether the GX thread waits for the GPU (frame begin / end / split), the vertex worker, or is in a draw.
enum { GW_RING, GW_DRAW, GW_DRAWCALL, GW_VWWAIT, GW_VWALLOC, GW_SPLITWAIT, GW_FRAMEBEGIN, GW_FRAMEEND, GW_SPLIT,
       GW_TEX, GW_COPY, GW_SHAPES, GW_N };
extern volatile u8 g_gxWhere;
// The GX thread's state as text for a watchdog line: its position, draw calls made, the vertex worker's queue.
void pica_watch(char* buf, u32 size);
// The vertex worker on core 3 (on by default when it starts): the second half of each triangle draw's vertex work
// runs there; off does it all on the GX thread (comparison).
void pica_set_vertex_worker(bool on);
bool pica_vertex_worker(void);
bool pica_vertex_worker_wanted(void);  // the setting (the GX thread applies it at its next draw)
// The vertex worker's job ring in bytes (64 KB to 1 MB, default 256 KB; applied when the ring is next empty).
void pica_set_job_ring(u32 bytes);
u32 pica_job_ring(void);
// Starting citro3d's GX queue at each split (on by default when the queue was found), so the GPU overlaps the GX
// thread; off waits for C3D_FrameEnd as citro3d does (comparison). pica_queue_offset: where the queue was found in
// citro3d's context (0xFFFFFFFF: not found, never started early).
void pica_set_early_queue(bool on);
// Splitting the frame every few thousand vertices so the GPU starts on it sooner (on by default); off splits only at
// EFB copies and clears (comparison).
void pica_set_early_split(bool on);
// Early splits at the draw calls recorded so far, leaving the open batch open (on by default); off splits only between
// batches and only with the vertex worker nearly idle (comparison).
void pica_set_split_open(bool on);
bool pica_split_open(void);
bool pica_early_split(void);
// Indexed triangle draws (on by default): each vertex written and shaded once, culled triangles as a repeated vertex,
// so no batch waits for the vertex worker; off draws three vertices per triangle (comparison).
void pica_set_indexed(bool on);
bool pica_indexed(void);
bool pica_early_queue(void);
// Draw merging (on by default; only with the GX thread): consecutive triangle draws with no state change between them
// become one vertex job; off issues every draw by itself (comparison). pica_merge_flush (GX thread) issues the draw held
// open, idle: because the ring is empty. g_picaMerge: draws, draws that joined an open draw, jobs issued, open draws
// issued for a state change, for a draw that could not join, for an empty ring, for present.
void pica_set_merge(bool on);
bool pica_merge(void);
void pica_merge_flush(System* s, bool idle);
extern u64 g_picaMerge[7];
// The vertex worker's job ring: jobs, bytes reserved, bytes kept (after the trim), waits for space, jobs in flight summed
// over those waits.
extern u64 g_vwStats[5];
extern u64 g_vwPhaseAll;  // the vertex worker's timed jobs, whole (g_vfPhase holds vfast_finish's phases of them)
void pica_set_vw_trim(bool on);  // give back the unused end of each job's reservation (default on; A/B)
bool pica_vw_trim(void);
// Describes the next `draws` draws (TEV registers, texture maps, the combiner compiler's result) for the log: the
// text is ready once pica_take_description returns it; pica_description_taken releases it.
// The display-list vertex cache's counters: the GX thread's (0) or the vertex worker's (1); NULL before it exists.
const VCacheStats* pica_vcache_stats(int worker);
void pica_set_fskip(bool on);  // frame skip (pica.c): every other rendered frame draws nothing
bool pica_fskip(void);
extern u64 g_fskipDraws, g_fskipFrames;
void pica_set_worker2(bool on);  // a second vertex worker on core 1 (indexed and fixed-place jobs)
bool pica_worker2(void);
extern u64 g_vw2Jobs;
// While the GX thread would wait for the vertex worker (job ring space, a drain), it runs queued indexed and fixed-place
// jobs itself (the ones that may finish out of order). The 2DS's GX thread waited ~23% of its time for ring space while
// the worker was 61% busy (job 163). g_vwStolen: jobs the GX thread ran.
void pica_set_steal(bool on);
bool pica_steal(void);
extern u64 g_vwStolen, g_vwStealEmpty, g_vwStealKind;
#ifdef GC3DS_NATIVE
// pica_native_store: memory the game flushed (Aurora's DCStoreRange and kin), callable from any thread.
void pica_native_store(const void* p, u32 len);
#endif
// Converted shapes (pica.c): models' draws converted once into PICA vertex buffers (native mode: indexed commands; the
// emulator: runs of draws in display lists). On by default; off: every draw on the vertex path, for comparison.
// g_nsStats: the counters described in pica.c. pica_native_bytes: linear memory in use.
void pica_set_native_shapes(bool on);
void pica_set_native_opts(u32 opts);  // bisection: 1 no PICA fixed attributes, 2 lit draws on the old path (pica.c NSO_*)
bool pica_native_shapes(void);
u32 pica_native_bytes(u32* live);
extern u64 g_nsStats[16];
extern u64 g_nsWhy[5];  // shapes left to the old path, by cause (pica.c ns_never)
void pica_describe(u32 draws);
const char* pica_take_description(void);
void pica_description_taken(void);
u32 pica_queue_offset(void);
extern u64 g_picaDrawPaths[8];
bool pica_vertex_worker_idle(void);  // no draw queued on the vertex worker (callable from any thread)  // draws by batch path (pica.c)
