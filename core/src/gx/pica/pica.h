#pragma once
// GX -> PICA200 backend for the 3DS (citro3d). Replaces the software renderer: draws, EFB clears and EFB copies run
// on the GPU, and copies to the XFB stay on the GPU ("virtual XFB").
#include "../gx.h"

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
// Lighting in the vertex shader (on by default when the shader provides it); off lights on the CPU (comparison).
void pica_set_shader_light(bool on);
bool pica_shader_light(void);

// Work counters since start (the frontend logs them per second): C3D_DrawArrays calls (batches), command-list
// submits (C3D_FrameEnd) and splits, textures decoded from RAM and their size in VRAM/FCRAM, EFB -> texture and
// EFB -> XFB copies, EFB clears.
typedef struct { u64 drawCalls, submits, splits, texDecodes, texDecodeBytes, efbCopies, xfbCopies, clears; } PicaStats;
extern PicaStats g_picaStats;
// The vertex worker on core 3 (on by default when it starts): the second half of each triangle draw's vertex work
// runs there; off does it all on the GX thread (comparison).
void pica_set_vertex_worker(bool on);
bool pica_vertex_worker(void);
bool pica_vertex_worker_wanted(void);  // the setting (the GX thread applies it at its next draw)
// Starting citro3d's GX queue at each split (on by default when the queue was found), so the GPU overlaps the GX
// thread; off waits for C3D_FrameEnd as citro3d does (comparison). pica_queue_offset: where the queue was found in
// citro3d's context (0xFFFFFFFF: not found, never started early).
void pica_set_early_queue(bool on);
bool pica_early_queue(void);
u32 pica_queue_offset(void);
extern u64 g_picaDrawPaths[8];
bool pica_vertex_worker_idle(void);  // no draw queued on the vertex worker (callable from any thread)  // draws by batch path (pica.c)
