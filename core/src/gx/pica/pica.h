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
typedef struct { u64 drawCalls, submits, splits, texDecodes, texDecodeBytes, efbCopies, xfbCopies, clears, palUploads, subDraws; } PicaStats;
extern PicaStats g_picaStats;
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
void pica_describe(u32 draws);
const char* pica_take_description(void);
void pica_description_taken(void);
u32 pica_queue_offset(void);
extern u64 g_picaDrawPaths[8];
bool pica_vertex_worker_idle(void);  // no draw queued on the vertex worker (callable from any thread)  // draws by batch path (pica.c)
