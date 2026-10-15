// gc3ds native mode: gc3ds's GX pipeline behind Aurora's GX API. Aurora's FIFO consumer (platform/aurora/aurora_3ds.cpp)
// hands the GX stream here, and gc3ds's own pipeline draws it: the FIFO decoder (core/src/gx/fifo.c, which knows
// Aurora's extension commands when built with GC3DS_NATIVE), the vertex work (vfast.c) and the PICA200 backend
// (gx/pica/pica.c), on the GX thread (host3ds/gxthread.c, core 2) and the vertex worker (core 3), as in the emulator.
//
// Built with -DGC3DS_NATIVE -DPA_MEM1_END=0xFFFFFFF0u: MEM1's base is zero and its end the top of the address space, so
// the host pointers in Aurora's stream (arrays, display lists, textures, palettes, copy targets) are addresses.
#include <3ds.h>
#define GC3DS_HOST_TYPES
#include <malloc.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include "sys/system.h"
#include "gx/pica/pica.h"
#include "gx/vfast.h"
#include "gxthread.h"

void native_trace_c(const char* text);  // native_3ds.cpp: a line in sdmc:/gc3ds/native/trace.txt

// ---- gc3ds core's hooks, with no emulator behind them ----
u32 g_logMask;
u64 g_hostProf[PROF_N];
volatile unsigned char g_where;
u64 (*g_profClock)(void);
bool g_drawTimed;
u32 g_drawScale = 16;

// Only the decoder's "unknown ..." reports (each capped at 20), the backend's set-up lines and errors reach the trace.
void emu_log(LogCat cat, const char* fmt, ...) {
    (void)cat;
    if (strncmp(fmt, "unknown", 7) != 0 && strncmp(fmt, "pica:", 5) != 0) return;
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    native_trace_c(line);
}
void emu_error(const char* fmt, ...) {
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    native_trace_c(line);
}
// PE interrupts: Aurora handles draw-done itself (process() finds the marker); tokens and finish raise nothing here.
void pi_raise(System* s, u32 cause) { (void)s; (void)cause; }
void gekko_set_idle_hook(Gekko* g, bool (*hook)(void* ctx), void* ctx) { (void)g; (void)hook; (void)ctx; }

// ---- native mode's GX ----
static System* g_s;
static bool g_on;
static bool g_inline;
static bool g_dumpWant;  // gxopt 'D' (gc3ds_gx_feed)  // gxopt 'g': GX on the game thread (no GX thread, no merging): bisection only
// Where the end-of-frame display copy goes: the copy stays on the GPU (pica.c's virtual XFB) and present finds it by
// this address; real memory, so nothing can fault if a path ever touches it.
static u8* g_xfb;
static u32 g_frames;

static void present(System* s, const GXPresent* p) { pica_present(s, p->top); }
static u64 prof_clock(void) { return svcGetSystemTick(); }

// gxopt 'd': the backend with its draws dropped (bisection: is a draw what fails?).
static GXBackend g_noDraw;
static void no_draw(void* ctx, System* s, u32 prim, u32 vat, const u8* verts, u32 count, u32 vsize) {
    (void)ctx; (void)s; (void)prim; (void)vat; (void)verts; (void)count; (void)vsize;
}

bool gc3ds_gx_init(void) {
    g_s = calloc(1, sizeof(System));
    if (!g_s) return false;
    g_s->mem1 = NULL;  // guest addresses are host pointers
    gxcore_init(&g_s->gx.core);
    // Core 1 (the backend's screenshot writer and helper threads) needs a time limit set before threads start there,
    // as gc3ds's main.c does.
    APT_SetAppCpuTimeLimit(30);
    g_xfb = memalign(32, 640 * 480 * 2);
    if (!g_xfb || !pica_attach(g_s)) { native_trace_c("gx: PICA backend not attached"); return false; }
    // The emulator's defaults (host3ds/main.c), except the display-list vertex cache: it keys on guest pages below
    // 24 MB (MemStamps), which native addresses are not.
    g_vcacheOn = false;
    g_profClock = prof_clock;  // the pipeline's own profile (gc3ds_gx_stats: ms per second by area)
    pica_set_vertex_worker(true);
    pica_set_early_queue(true);
    pica_set_gpu_transform(true);
    pica_set_early_split(true);
    pica_set_indexed(true);
    pica_set_merge(true);
    // The whole job ring (VW_MEM, 4 MB here): Aurora's merged model draws need hundreds of KB of job each, and one that
    // does not fit runs on the GX thread instead of the worker (2DS job 238: the GX thread saturated, core 3 at ~30%).
    pica_set_job_ring(4096u << 10);
    // sdmc:/gc3ds/native/gxopt.txt turns paths off for bisection: q early queue, m merging, w vertex worker,
    // t shader transform, i indexed draws, s early splits; and on: 2 a second vertex worker on core 1, S the GX thread
    // running queued jobs while it would wait for the worker, D the stream dump.
    // --gxopt (native_3ds.cpp) first, else the file.
    extern char g_gxopt[32];
    char o[32] = "";
    snprintf(o, sizeof o, "%s", g_gxopt);
    FILE* of = o[0] ? NULL : fopen("sdmc:/gc3ds/native/gxopt.txt", "r");
    if (of && !fgets(o, sizeof o, of)) o[0] = 0;
    if (of) fclose(of);
    {
        if (o[0]) {
            if (strchr(o, 'q')) pica_set_early_queue(false);
            if (strchr(o, 'm')) pica_set_merge(false);
            if (strchr(o, 'w')) pica_set_vertex_worker(false);
            if (strchr(o, 't')) pica_set_gpu_transform(false);
            if (strchr(o, 'i')) pica_set_indexed(false);
            if (strchr(o, 's')) pica_set_early_split(false);
            if (strchr(o, 'g')) g_inline = true;
            if (strchr(o, '2')) pica_set_worker2(true);
            if (strchr(o, 'S')) pica_set_steal(true);
            if (strchr(o, 'D')) g_dumpWant = true;
            if (strchr(o, 'd')) {
                g_noDraw = *g_s->gx.core.backend;
                g_noDraw.draw = no_draw;
                g_noDraw.draw_indexed = NULL;  // expanded by fifo.c, then dropped
                g_s->gx.core.backend = &g_noDraw;
            }
            char line[64];
            snprintf(line, sizeof line, "gx: options: %s", o);
            native_trace_c(line);
        }
    }
    if (!g_inline && !gxthread_start(g_s, present)) { native_trace_c("gx: GX thread not started"); return false; }
    g_on = true;
    native_trace_c("gx: gc3ds GX pipeline on cores 2 and 3");
    return true;
}

bool gc3ds_gx_on(void) { return g_on; }

// Queues GX stream bytes for the GX thread (in pieces the ring always has room for; the decoder joins commands that
// straddle them).
void gc3ds_gx_feed(const u8* data, u32 len) {
    if (!g_on) return;
    // gxopt 'D': frames 236-241 of the stream as Aurora wrote it (sdmc:/gc3ds/native/gx_dump.bin; tools/gx_dump.py).
    // Debug only: writing the SD card on every feed made those frames take 5-10 s each (2DS job 229).
    if (g_dumpWant && g_frames >= 236 && g_frames < 242) {
        static FILE* dump;
        if (!dump) dump = fopen("sdmc:/gc3ds/native/gx_dump.bin", "wb");
        if (dump) fwrite(data, 1, len, dump);
    }
    while (len) {
        u32 n = len < (128u << 10) ? len : 128u << 10;
        if (g_inline) gxcore_feed(g_s, &g_s->gx.core, data, n);
        else g_s->gxAsync->feed(g_s, data, n);
        data += n;
        len -= n;
    }
}

// Waits until everything fed so far has been drawn and the vertex worker has read the game's arrays (before the game
// is told a draw is done and may change them).
void gc3ds_gx_sync(void) {
    if (g_on && !g_inline) gxthread_drain();
}

static u8* bp(u8* o, u32 reg, u32 v) {
    o[0] = 0x61; o[1] = (u8)reg; o[2] = (u8)(v >> 16); o[3] = (u8)(v >> 8); o[4] = (u8)v;
    return o + 5;
}

// End of a game frame: the EFB to the screen and cleared, as GXCopyDisp(xfb, GX_TRUE) does on the GameCube (Aurora's
// GXCopyDisp does nothing: its renderer presents the frame itself), then a present after it.
void gc3ds_gx_frame(u32 w, u32 h) {
    if (!g_on) return;
    u8 cmd[64], *o = cmd;
    u32 xfb = (u32)(uintptr_t)g_xfb;
    *o++ = 0x50; *o++ = 0x00; *o++ = 0x37;  // GX_AURORA_LOAD_COPY_DEST: u64 address
    memset(o, 0, 4); o += 4;
    *o++ = (u8)(xfb >> 24); *o++ = (u8)(xfb >> 16); *o++ = (u8)(xfb >> 8); *o++ = (u8)xfb;
    o = bp(o, 0x49, 0);                                          // source: top left
    o = bp(o, 0x4A, ((w - 1) & 0x3FF) | (((h - 1) & 0x3FF) << 10));  // and size
    o = bp(o, 0x4D, (w * 2) >> 5);                               // XFB stride in 32-byte units
    o = bp(o, 0x52, (1u << 14) | (1u << 11));                    // to the XFB, clear
    gc3ds_gx_feed(cmd, (u32)(o - cmd));
    GXPresent p = { xfb, xfb, w, h, w * 2, false };
    if (g_inline) present(g_s, &p);
    else gxthread_present(&p);
    g_frames++;
    // The top screen as the GPU drew it, at a few points of a run (sdmc:/gc3ds/native/shot_<frame>.bmp).
    if (g_frames == 120 || g_frames == 300 || g_frames == 600 || g_frames == 1200 || g_frames == 2400) {
        static char path[64];
        snprintf(path, sizeof path, "sdmc:/gc3ds/native/shot_%lu.bmp", (unsigned long)g_frames);
        pica_capture(path);
    }
}

// Per-second line for perf.csv: draws and vertices decoded, and Aurora's command counts (fifo.c GXCore.aurora).
void gc3ds_gx_stats(char* out, u32 size) {
    if (!g_on) { snprintf(out, size, "off"); return; }
    const GXCore* gx = &g_s->gx.core;
    const PicaStats* ps = &g_picaStats;
    int n = snprintf(out, size, "%llu %llu %llu %llu %llu %llu %llu %llu %llu %llu %lu %lu",
             (unsigned long long)gx->draws, (unsigned long long)gx->vertices, (unsigned long long)gx->aurora[0],
             (unsigned long long)gx->aurora[1], (unsigned long long)gx->aurora[2], (unsigned long long)gx->aurora[3],
             (unsigned long long)gx->aurora[4], (unsigned long long)gx->aurora[5], (unsigned long long)gx->aurora[6],
             (unsigned long long)gx->aurora[7], (unsigned long)g_picaStats.texFails, (unsigned long)g_gxPresentsSkipped);
    // The backend: GPU draw calls, submits, textures decoded, EFB -> texture and -> XFB copies, clears.
    if (n > 0 && (u32)n < size)
        n += snprintf(out + n, size - n, " | %llu %llu %llu %llu %llu %llu", (unsigned long long)ps->drawCalls,
                      (unsigned long long)ps->submits, (unsigned long long)ps->texDecodes, (unsigned long long)ps->efbCopies,
                      (unsigned long long)ps->xfbCopies, (unsigned long long)ps->clears);
    // Milliseconds since the last line (about a second): GX thread busy, waiting for the GPU (C3D_FrameBegin),
    // for the vertex worker, for its job ring; vertex worker busy; textures; draw state; frame ends; then the GX
    // thread's vertex work, its vfast_prepare and palette parts, and the decoder as a whole.
    static const u8 kAreas[] = { PROF_GXTHREAD, PROF_GXSYNC, PROF_VWAIT, PROF_VALLOC, PROF_VWORK, PROF_GXTEX, PROF_GXSTATE, PROF_GXEND,
                                 PROF_GXVTX, PROF_VPREP, PROF_VPAL, PROF_GX };
    if (n > 0 && (u32)n < size) n += snprintf(out + n, size - n, " |");
    for (u32 i = 0; i < sizeof kAreas && n > 0 && (u32)n < size; i++) {
        u64 t = g_hostProf[kAreas[i]];
        g_hostProf[kAreas[i]] = 0;
        n += snprintf(out + n, size - n, " %.0f", (double)t * 1000.0 / SYSCLOCK_ARM11);
    }
    // Where draws went (pica.c g_picaDrawPaths: same batch fast, same batch after its key, new batch; why new: BP,
    // palette, lights, XFB copy; drawn alone on the CPU for want of palette room) and what the vertex work was made of
    // (vfast.c: draws, vertices, setups, CPU light evaluations, texgens, compositions). Totals since start.
    extern u64 g_picaDrawPaths[8];
    extern u64 g_gxOpTicks[6], g_gxOpCount[6];
    extern u64 g_vwInline, g_vwInlineVerts;
    const VFastStats* v = &g_vfastStats;
    if (n > 0 && (u32)n < size)
        n += snprintf(out + n, size - n, " | %llu %llu %llu %llu %llu %llu %llu %llu | %llu %llu %llu %llu %llu %llu",
                      (unsigned long long)g_picaDrawPaths[0], (unsigned long long)g_picaDrawPaths[1],
                      (unsigned long long)g_picaDrawPaths[2], (unsigned long long)g_picaDrawPaths[3],
                      (unsigned long long)g_picaDrawPaths[4], (unsigned long long)g_picaDrawPaths[5],
                      (unsigned long long)g_picaDrawPaths[6], (unsigned long long)g_picaDrawPaths[7],
                      (unsigned long long)v->draws, (unsigned long long)v->verts, (unsigned long long)v->setups,
                      (unsigned long long)v->lightEvals, (unsigned long long)v->texgens, (unsigned long long)v->composes);
    // Draws done on the GX thread for want of job-ring room, and their vertices (totals).
    if (n > 0 && (u32)n < size)
        n += snprintf(out + n, size - n, " | %llu %llu", (unsigned long long)g_vwInline, (unsigned long long)g_vwInlineVerts);
    // The decoder by command class (fifo.c): ms since the last line and commands: draws, BP, XF, CP, Aurora, other.
    if (n > 0 && (u32)n < size) n += snprintf(out + n, size - n, " |");
    for (u32 i = 0; i < 6 && n > 0 && (u32)n < size; i++) {
        n += snprintf(out + n, size - n, " %.0f/%llu", (double)g_gxOpTicks[i] * 1000.0 / SYSCLOCK_ARM11,
                      (unsigned long long)g_gxOpCount[i]);
        g_gxOpTicks[i] = g_gxOpCount[i] = 0;
    }
}
