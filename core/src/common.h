#pragma once
// Shared types and helpers for the emulator core. Portable C11: builds for the PC (development, reference) and for
// the 3DS (devkitARM).
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef GC3DS_HOST_TYPES  // a 3DS frontend file that also includes <3ds.h> uses libctru's (ABI-identical) types
typedef uint8_t u8;
typedef uint16_t u16;
// Plain int, not uint32_t: newlib (3DS) makes uint32_t `unsigned long`, glibc `unsigned int`. With int on both,
// printf's %x / %d / %u are correct for u32/s32 everywhere (do not use PRIx32 with these types).
typedef unsigned int u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int s32;
typedef int64_t s64;
#endif
_Static_assert(sizeof(u32) == 4 && sizeof(s32) == 4, "32-bit int required");

static inline u16 bswap16(u16 v) { return __builtin_bswap16(v); }
static inline u32 bswap32(u32 v) { return __builtin_bswap32(v); }
static inline u64 bswap64(u64 v) { return __builtin_bswap64(v); }

// Logging: categories can be filtered at runtime. LOG_ERROR always prints.
typedef enum { LOG_CPU, LOG_MEM, LOG_HW, LOG_BOOT, LOG_GX, LOG_DSP, LOG_EXI, LOG_SI, LOG_DI, LOG_VI, LOG_OS, LOG_NCAT } LogCat;
void emu_log(LogCat cat, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void emu_error(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
extern u32 g_logMask;  // bit per LogCat

// Wall-clock profiling of a few host-side areas (the 3DS frontend reads and clears it once a second). Off while
// g_profClock is NULL, which costs one test per call site.
// GXTEX, GXVTX, GXSYNC: parts of GX; VDEC (attribute gathering) and VLIGHT (lighting): parts of GXVTX.
// CPU: all of gekko_run (guest code and whatever it triggers, GX included). FBFP / FB: interpreter fallbacks from
// compiled code (float and paired-single / other), timed on 1 in 64 and scaled.
// GXTHREAD: busy time of the GX thread when GX runs on another core; GXWAIT: the CPU thread waiting for it (ring
// full, or a sync before a PE read or an idle skip). VWORK: busy time of the vertex worker (core 3); VWAIT: the GX
// thread waiting for it before the GPU reads the vertex arena. GXPRES: the GX thread presenting a field; GXSTATE: the
// draw path deciding and applying GPU state (batch key, TEV, fragment state, textures bound, uniforms). GXEND: ending a
// frame (the arena's cache flush and C3D_FrameEnd).
// VPREP (vfast_prepare, VDEC and VLIGHT included), VPAL (palette slots for GPU-transformed draws) and VQUEUE (room in
// the arena and the worker's job ring, queueing): parts of GXVTX.
enum { PROF_GX, PROF_DISC, PROF_LOG, PROF_JITC, PROF_PRESENT, PROF_GXTEX, PROF_GXVTX, PROF_GXSYNC, PROF_VDEC, PROF_VLIGHT,
       PROF_CPU, PROF_FBFP, PROF_FB, PROF_GXTHREAD, PROF_GXWAIT, PROF_VWORK, PROF_VWAIT, PROF_GXPRES, PROF_GXSTATE, PROF_GXEND,
       PROF_VPREP, PROF_VPAL, PROF_VQUEUE, PROF_N };
extern u64 g_hostProf[PROF_N];
// Where core 0 is when it is not inside compiled code (host3ds/jitsample.c reports how often the sampler finds it at each): the
// code that runs there sets this one byte on entry. WH_NONE is compiled code and its helpers (the sampler uses jit_in_run for those).
enum { WH_NONE, WH_GEKKO, WH_DISPATCH, WH_COMPILE, WH_SYSRUN, WH_EVENTS, WH_GXPOLL, WH_HLE, WH_INTERP, WH_FRONT, WH_PRESENT, WH_WAIT,
       WH_PERF, WH_N };
extern volatile unsigned char g_where;
extern u64 (*g_profClock)(void);
static inline u64 prof_begin(void) { return g_profClock ? g_profClock() : 0; }
static inline void prof_end(int area, u64 t0) { if (g_profClock) g_hostProf[area] += g_profClock() - t0; }
// Per-draw areas (GX thread): the 3DS backend times one draw in 16 and scales (g_drawScale = 16), because a clock
// read costs 0.25 us on the 2DS and a draw is ~5 vertices; it sets g_drawTimed for each draw.
extern bool g_drawTimed;
extern u32 g_drawScale;
static inline u64 dprof_begin(void) { return g_drawTimed ? prof_begin() : 0; }
static inline void dprof_end(int area, u64 t0) { if (g_drawTimed && g_profClock) g_hostProf[area] += (g_profClock() - t0) * g_drawScale; }
