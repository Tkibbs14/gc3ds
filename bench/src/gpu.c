// gcbench GPU module: what can the PICA200 do for a GameCube/Wii renderer drawing at 400x240?
//
// HOW EVERY GPU NUMBER IS TIMED (unless its own comment says otherwise)
//  * The work for one frame is recorded into a citro3d frame, then timed from the CPU tick taken just before
//    C3D_FrameEnd() (which hands the command list / transfer to GSP) until citro3d's GX command queue reports
//    idle. Idle is polled with C3D_FrameBegin(C3D_FRAME_NONBLOCK) every POLL_NS, never with a blocking wait, and
//    every wait gives up after GPU_TIMEOUT_S. So a number covers: GSP submission + GPU execution + the completion
//    interrupt reaching this process + up to ~POLL_NS of polling slack. Building the command list on the CPU is
//    NOT included (it happens before the clock starts). The fixed part is measured once and printed as
//    "frame overhead"; it is included in every per-frame number and not subtracted.
//  * Vsync is NOT included. Frames never use C3D_FRAME_SYNCDRAW, and measured frames render into an off-screen
//    target that is not linked to a screen, so no display transfer or buffer swap is inside any timing. The
//    target is linked to the top screen only for the untimed show() frame after each test. The LCD keeps
//    scanning out of FCRAM during the tests: that bandwidth load is real and the emulator will see it too.
//  * C3D_FrameEnd(GX_CMDLIST_FLUSH) flushes only the command list from the CPU cache. The default (0) would
//    flush the whole linear heap every frame; instead, every buffer the CPU writes is flushed explicitly.
//  * Each measurement runs one untimed warm-up frame (shader upload, state, clear, cold caches), then enough
//    frames for about MEASURE_S of GPU time (3 to 200 frames), and reports the mean ms per frame.
//  * After drawing, the centre pixel of the target is read back; if it still holds the clear value the GPU drew
//    nothing (e.g. geometry clipped) and the line is marked INVALID instead of reporting a fake-fast number.
//
// FAIL-SAFE RULES: no unbounded GPU wait is ever reached. If a GPU wait times out, g_dead is set, every later
// GPU test is skipped, and cleanup leaks GPU-visible memory and leaves citro3d initialised rather than calling
// C3D_RenderTargetDelete/C3D_Fini, which wait for the GPU without a timeout.
#include <3ds.h>
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bench.h"
#include "gpu_simple_shbin.h"
#include "gpu_light_shbin.h"

#define GPU_TIMEOUT_S   2.0
#define POLL_NS         20000      // 20 us between idle polls
#define MEASURE_S       0.35       // target timed GPU time per measurement
#define MEASURE_MAX_S   3.0        // hard cap per measurement
#define MODULE_BUDGET_S 55.0       // skip remaining tests after this

#define SCR_W 400
#define SCR_H 240
#define SCR_PX (SCR_W * SCR_H)

// Geometry: a 100x100-cell grid over a 300x180 px region in the middle of the screen. 2 triangles per cell,
// so 20000 triangles of 2.7 px^2 each: small enough that the vertex stage, not fill, is the limit.
#define GRID_W 100
#define GRID_H 100
#define GRID_X0 50.0f
#define GRID_Y0 30.0f
#define GRID_CW 3.0f
#define GRID_CH 1.8f
#define GRID_TRIS (GRID_W * GRID_H * 2)            // 20000
#define GRID_VERTS ((GRID_W + 1) * (GRID_H + 1))   // 10201 shared vertices
#define TRI_VERTS (GRID_TRIS * 3)                  // 60000 de-indexed vertices (fits u16 indices)

#define TEX_N 256
#define TEX_BYTES (TEX_N * TEX_N * 4)
#define RB_BYTES (SCR_PX * 4)                      // readback buffer: whole 240x400 RGBA8 target
#define FILL_K 24                                  // full-screen quads per frame in the fill tests
#define CONV_N 100000                              // vertices for the CPU conversion test
#define GC_STRIDE 14                               // s16 xyz + u8x4 color + s16 uv, big-endian
#define UPLOAD_ITERS 16
#define SENTINEL 0x00000000u                       // clear colour: still there after a draw = nothing drawn

// Offsets into the quad VBO.
enum { Q_FULL = 0, Q_MIN = 6, Q_BLEND = 12, Q_SHOW = 18, Q_TINY = 24, Q_COUNT = 27 };

typedef struct { float x, y, z; u8 r, g, b, a; float u, v; } Vtx;                       // GC-like vertex
typedef struct { float x, y, z; u8 r, g, b, a; float u, v; float nx, ny, nz; } VtxN;    // + normal
typedef struct { s16 x, y, z, pad; u8 r, g, b, a; s16 u, v; } VtxS;                      // byteswapped GC
_Static_assert(sizeof(Vtx) == 24, "Vtx layout");
_Static_assert(sizeof(VtxN) == 36, "VtxN layout");
_Static_assert(sizeof(VtxS) == 16, "VtxS layout");

// Display transfer flags.
#define XFER_TO_LINEAR (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
    GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) | \
    GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))
#define XFER_TO_TEXTURE (GX_TRANSFER_FLIP_VERT(1) | GX_TRANSFER_OUT_TILED(1) | GX_TRANSFER_RAW_COPY(0) | \
    GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) | \
    GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))
#define COPY_RAW GX_TRANSFER_RAW_COPY(1)

typedef void (*DrawFn)(int arg);

static double g_t0;
static bool g_dead;                      // a GPU wait timed out: issue no more GPU work
static bool g_c3d;                       // C3D_Init succeeded

static DVLB_s *g_dvlbS, *g_dvlbL;
static shaderProgram_s g_progS, g_progL;
static bool g_progSOk, g_progLOk;
static int g_uProjS, g_uProjL, g_uMv, g_uNrm, g_uLDir, g_uLClr, g_uAmb;

static C3D_Mtx g_projTop;                // 400x240 pixel space onto the rotated top-screen target
static C3D_Mtx g_projTex;                // same pixel space squeezed into a 256x256 texture target (no tilt)
static C3D_RenderTarget* g_tgt;          // 240x400 RGBA8 + D24S8 in VRAM, off-screen except in show()
static u32 g_dispFlags;
static bool g_canShow;

static Vtx* g_tri;                       // 20000 separate triangles (the grid, de-indexed)
static VtxN* g_triN;                     // same triangles with normals
static Vtx* g_grid;                      // shared grid vertices
static u16* g_idxGrid;                   // 60000 indices into g_grid
static u16* g_idxIdent;                  // 0..59999 into g_tri
static Vtx* g_quad;
static u32* g_rb;                        // linear readback buffer
static u32* g_stage;                     // linear staging buffer for uploads
static u32* g_img;                       // decoded 256x256 RGBA8 source image, ordinary heap
static C3D_Tex g_texV, g_texL;           // same image: VRAM and linear FCRAM
static bool g_texVOk, g_texLOk;
static int g_quadFirst;                  // which quad draw_quads() draws
static C3D_RenderTarget* g_rtA;          // test 6
static C3D_Tex g_texA, g_texB;
static bool g_texAOk, g_texBOk;
static u8 g_morton[64];

// ---- summary --------------------------------------------------------------------------------------------------
typedef struct { const char* name; double v; const char* unit; } Sum;
static Sum g_sum[40];
static int g_nsum;

static void sum_add(const char* name, double v, const char* unit)
{
    if (g_nsum < (int)(sizeof g_sum / sizeof g_sum[0])) {
        g_sum[g_nsum].name = name;
        g_sum[g_nsum].v = v;
        g_sum[g_nsum].unit = unit;
        g_nsum++;
    }
}

// ---- bounded GPU synchronisation -------------------------------------------------------------------------------
static void gpu_timeout(void)
{
    g_dead = true;
    P("GPU TIMEOUT (>%.0f s): GPU tests stopped\n", GPU_TIMEOUT_S);
}

// Opens a frame once the GX queue is idle. Never blocks longer than GPU_TIMEOUT_S.
static bool frame_begin(void)
{
    if (g_dead) return false;
    double t0 = now_s();
    while (!C3D_FrameBegin(C3D_FRAME_NONBLOCK)) {
        if (now_s() - t0 > GPU_TIMEOUT_S) { gpu_timeout(); return false; }
        svcSleepThread(POLL_NS);
    }
    return true;
}

// Submits the open frame and returns seconds from submission until the GX queue is idle again, or -1 on
// timeout. Leaves no frame open: idle is detected by opening a frame, which is closed again empty (an empty
// frame queues nothing: no draws, and no linked target is marked used).
static double frame_end_timed(void)
{
    double t0 = now_s();
    C3D_FrameEnd(GX_CMDLIST_FLUSH);
    for (;;) {
        if (C3D_FrameBegin(C3D_FRAME_NONBLOCK)) {
            double t = now_s() - t0;
            C3D_FrameEnd(GX_CMDLIST_FLUSH);
            return t;
        }
        if (now_s() - t0 > GPU_TIMEOUT_S) { gpu_timeout(); return -1; }
        svcSleepThread(POLL_NS);
    }
}

static bool gpu_ok(const char* test)
{
    if (g_dead) { P("%s SKIPPED: GPU timed out earlier\n", test); return false; }
    if (now_s() - g_t0 > MODULE_BUDGET_S) { P("%s SKIPPED: module time budget used\n", test); return false; }
    return true;
}

// Copies a render target's colour buffer to g_rb (linear RGBA8) with the transfer engine. Untimed helper.
static bool read_target(C3D_RenderTarget* t)
{
    u32 w = t->frameBuf.width, h = t->frameBuf.height;
    if (!g_rb || w * h * 4 > RB_BYTES) return false;
    GSPGPU_FlushDataCache(g_rb, w * h * 4);     // no dirty CPU line may later overwrite what the GPU writes
    if (!frame_begin()) return false;
    GX_DisplayTransfer((u32*)t->frameBuf.colorBuf, GX_BUFFER_DIM(w, h), g_rb, GX_BUFFER_DIM(w, h), XFER_TO_LINEAR);
    if (frame_end_timed() < 0) return false;
    GSPGPU_InvalidateDataCache(g_rb, w * h * 4);
    return true;
}

static bool center_drawn(C3D_RenderTarget* t)
{
    if (!read_target(t)) return false;
    u32 w = t->frameBuf.width, h = t->frameBuf.height;
    return g_rb[(h / 2) * w + w / 2] != SENTINEL;
}

// Times frames of draw(arg) into g_tgt (see the note at the top). Returns mean ms per frame or -1.
static double measure(DrawFn draw, int arg, bool verify, bool* drawn)
{
    double total = 0;
    int frames = 0, want = 3;
    *drawn = true;
    for (int i = 0;; i++) {
        if (!frame_begin()) return -1;
        C3D_FrameDrawOn(g_tgt);
        if (i == 0) C3D_RenderTargetClear(g_tgt, C3D_CLEAR_ALL, SENTINEL, 0);
        draw(arg);
        double t = frame_end_timed();
        if (t < 0) return -1;
        if (i == 0) continue;
        total += t;
        frames++;
        if (frames == 1) {
            want = t > 1e-6 ? (int)(MEASURE_S / t) : 200;
            if (want < 3) want = 3;
            if (want > 200) want = 200;
        }
        if (frames >= want || total > MEASURE_MAX_S) break;
    }
    if (verify) *drawn = center_drawn(g_tgt);
    return total * 1000.0 / frames;
}

// Shows the off-screen target on the top screen (untimed).
static void show(void)
{
    if (!g_canShow || g_dead || !g_tgt) return;
    C3D_RenderTargetSetOutput(g_tgt, GFX_TOP, GFX_LEFT, g_dispFlags);
    if (frame_begin()) {
        C3D_FrameDrawOn(g_tgt);
        frame_end_timed();
    }
    // Detaching waits for the queue without a timeout; it is idle here unless the GPU hung.
    if (!g_dead) C3D_RenderTargetDetachOutput(g_tgt);
}

// ---- state helpers ---------------------------------------------------------------------------------------------
static void attr_vtx(void)
{
    C3D_AttrInfo* ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);
    AttrInfo_AddLoader(ai, 1, GPU_UNSIGNED_BYTE, 4);
    AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 2);
}

static void attr_vtxn(void)
{
    attr_vtx();
    AttrInfo_AddLoader(C3D_GetAttrInfo(), 3, GPU_FLOAT, 3);
}

static bool buf_set(const void* p, int stride, int attribs, u64 perm)
{
    C3D_BufInfo* bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    return BufInfo_Add(bi, p, stride, attribs, perm) >= 0;
}

static bool buf_vtx(const void* p) { return buf_set(p, sizeof(Vtx), 3, 0x210); }
static bool buf_vtxn(const void* p) { return buf_set(p, sizeof(VtxN), 4, 0x3210); }

static void use_simple(const C3D_Mtx* proj)
{
    C3D_BindProgram(&g_progS);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_uProjS, proj);
}

static void use_light(void)
{
    C3D_Mtx id;
    Mtx_Identity(&id);
    C3D_BindProgram(&g_progL);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_uProjL, &g_projTop);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_uMv, &id);
    C3D_FVUnifMtx3x4(GPU_VERTEX_SHADER, g_uNrm, &id);
    static const float dir[4][3] = { { 0.0f, 0.0f, 1.0f }, { 0.577f, 0.577f, 0.577f },
                                     { -0.707f, 0.0f, 0.707f }, { 0.0f, -0.707f, 0.707f } };
    static const float clr[4][3] = { { 0.6f, 0.6f, 0.6f }, { 0.4f, 0.2f, 0.1f },
                                     { 0.1f, 0.3f, 0.2f }, { 0.1f, 0.1f, 0.4f } };
    for (int i = 0; i < 4; i++) {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, g_uLDir + i, dir[i][0], dir[i][1], dir[i][2], 0.0f);
        C3D_FVUnifSet(GPU_VERTEX_SHADER, g_uLClr + i, clr[i][0], clr[i][1], clr[i][2], 0.0f);
    }
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g_uAmb, 0.2f, 0.2f, 0.2f, 1.0f);
}

static void env_passthrough_from(int first)
{
    for (int i = first; i < 6; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));
}

// Flat combiner: vertex colour only, texture units off.
static void env_color(void)
{
    C3D_TexBind(0, NULL);
    C3D_TexEnv* e = C3D_GetTexEnv(0);
    C3D_TexEnvInit(e);
    C3D_TexEnvSrc(e, C3D_Both, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(e, C3D_Both, GPU_REPLACE);
    env_passthrough_from(1);
}

// Stage 0: texture * vertex colour. Stages 1..n-1: lerp(previous, constant, texture) on RGB. Rest: passthrough.
static void env_tex(C3D_Tex* t, int stages)
{
    static const u32 stageClr[6] = { 0, 0xFF3060C0, 0xFFC06030, 0xFF30C060, 0xFF808080, 0xFF6030C0 };
    C3D_TexBind(0, t);
    C3D_TexEnv* e = C3D_GetTexEnv(0);
    C3D_TexEnvInit(e);
    C3D_TexEnvSrc(e, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(e, C3D_Both, GPU_MODULATE);
    for (int i = 1; i < stages && i < 6; i++) {
        e = C3D_GetTexEnv(i);
        C3D_TexEnvInit(e);
        C3D_TexEnvSrc(e, C3D_RGB, GPU_PREVIOUS, GPU_CONSTANT, GPU_TEXTURE0);
        C3D_TexEnvFunc(e, C3D_RGB, GPU_INTERPOLATE);
        C3D_TexEnvColor(e, stageClr[i]);
    }
    env_passthrough_from(stages < 1 ? 1 : stages);
}

static void blend_off(void) { C3D_ColorLogicOp(GPU_LOGICOP_COPY); }
static void blend_on(void)
{
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA,
                   GPU_ONE_MINUS_SRC_ALPHA);
}
static void depth_off(void) { C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR); }
static void depth_on(void) { C3D_DepthTest(true, GPU_GEQUAL, GPU_WRITE_ALL); }

// ---- draw functions --------------------------------------------------------------------------------------------
static void draw_tri_arrays(int reps)
{
    for (int r = 0; r < reps; r++) C3D_DrawArrays(GPU_TRIANGLES, 0, TRI_VERTS);
}
static void draw_tri_ident(int reps)
{
    for (int r = 0; r < reps; r++) C3D_DrawElements(GPU_TRIANGLES, TRI_VERTS, C3D_UNSIGNED_SHORT, g_idxIdent);
}
static void draw_grid_elems(int reps)
{
    for (int r = 0; r < reps; r++) C3D_DrawElements(GPU_TRIANGLES, GRID_TRIS * 3, C3D_UNSIGNED_SHORT, g_idxGrid);
}
static void draw_quads(int k)
{
    for (int i = 0; i < k; i++) C3D_DrawArrays(GPU_TRIANGLES, g_quadFirst, 6);
}
static void draw_tiny(int unused)
{
    (void)unused;
    C3D_DrawArrays(GPU_TRIANGLES, Q_TINY, 3);
}

// ---- data ------------------------------------------------------------------------------------------------------
static void set_vtx(Vtx* v, int i, int j)
{
    v->x = GRID_X0 + i * GRID_CW;
    v->y = GRID_Y0 + j * GRID_CH;
    v->z = 0.5f;
    v->r = (u8)(i * 255 / GRID_W);
    v->g = (u8)(j * 255 / GRID_H);
    v->b = (u8)(255 - (i + j) * 255 / (GRID_W + GRID_H));
    v->a = 255;
    v->u = (float)i / GRID_W;
    v->v = (float)j / GRID_H;
}

static void build_geometry(void)
{
    static const int corner[6][2] = { { 0, 0 }, { 1, 0 }, { 1, 1 }, { 0, 0 }, { 1, 1 }, { 0, 1 } };
    if (g_grid) {
        for (int j = 0; j <= GRID_H; j++)
            for (int i = 0; i <= GRID_W; i++) set_vtx(&g_grid[j * (GRID_W + 1) + i], i, j);
        GSPGPU_FlushDataCache(g_grid, GRID_VERTS * sizeof(Vtx));
    }
    int k = 0;
    for (int j = 0; j < GRID_H; j++)
        for (int i = 0; i < GRID_W; i++)
            for (int c = 0; c < 6; c++, k++) {
                int ci = i + corner[c][0], cj = j + corner[c][1];
                if (g_idxGrid) g_idxGrid[k] = (u16)(cj * (GRID_W + 1) + ci);
                if (g_idxIdent) g_idxIdent[k] = (u16)k;
                Vtx v;
                set_vtx(&v, ci, cj);
                if (g_tri) g_tri[k] = v;
                if (g_triN) {
                    VtxN* n = &g_triN[k];
                    memcpy(n, &v, sizeof v);
                    n->nx = sinf(ci * 0.13f);
                    n->ny = cosf(cj * 0.11f);
                    n->nz = 0.8f;
                }
            }
    if (g_idxGrid) GSPGPU_FlushDataCache(g_idxGrid, TRI_VERTS * sizeof(u16));
    if (g_idxIdent) GSPGPU_FlushDataCache(g_idxIdent, TRI_VERTS * sizeof(u16));
    if (g_tri) GSPGPU_FlushDataCache(g_tri, TRI_VERTS * sizeof(Vtx));
    if (g_triN) GSPGPU_FlushDataCache(g_triN, TRI_VERTS * sizeof(VtxN));

    if (g_quad) {
        static const float px[6][2] = { { 0, 0 }, { SCR_W, 0 }, { SCR_W, SCR_H }, { 0, 0 }, { SCR_W, SCR_H }, { 0, SCR_H } };
        static const u8 col[6][3] = { { 255, 90, 90 }, { 90, 255, 90 }, { 90, 90, 255 },
                                      { 255, 90, 90 }, { 90, 90, 255 }, { 255, 255, 90 } };
        for (int q = 0; q < 4; q++)
            for (int c = 0; c < 6; c++) {
                Vtx* v = &g_quad[q * 6 + c];
                v->x = px[c][0];
                v->y = px[c][1];
                v->z = 0.5f;
                bool white = q == 3;
                v->r = white ? 255 : col[c][0];
                v->g = white ? 255 : col[c][1];
                v->b = white ? 255 : col[c][2];
                v->a = q == 2 ? 128 : 255;
                // Q_FULL/Q_BLEND: 1 texel per pixel. Q_MIN: 2x2 texels per pixel. Q_SHOW: whole texture.
                float s = q == 1 ? 2.0f : 1.0f;
                v->u = q == 3 ? px[c][0] / SCR_W : s * px[c][0] / TEX_N;
                v->v = q == 3 ? px[c][1] / SCR_H : s * px[c][1] / TEX_N;
            }
        Vtx* t = &g_quad[Q_TINY];
        memset(t, 0, 3 * sizeof(Vtx));
        t[1].x = 1.0f;
        t[2].y = 1.0f;
        for (int c = 0; c < 3; c++) { t[c].z = 0.5f; t[c].a = 255; }
        GSPGPU_FlushDataCache(g_quad, Q_COUNT * sizeof(Vtx));
    }
}

static void morton_init(void)
{
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++)
            g_morton[y * 8 + x] = (u8)((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) |
                                       ((x & 4) << 2) | ((y & 4) << 3));
}

// Linear top-down 256x256 RGBA8 -> PICA tiled layout: 8x8 tiles in Z-order, tile rows from the bottom of the
// image up (the layout GX_DisplayTransfer produces with FLIP_VERT(1)|OUT_TILED(1); checked at run time).
static void swizzle256(u32* dst, const u32* src)
{
    for (int ty = 0; ty < TEX_N / 8; ty++)
        for (int tx = 0; tx < TEX_N / 8; tx++) {
            u32* tile = dst + (ty * (TEX_N / 8) + tx) * 64;
            for (int r = 0; r < 8; r++) {
                const u32* s = src + (TEX_N - 1 - (ty * 8 + r)) * TEX_N + tx * 8;
                const u8* m = &g_morton[r * 8];
                tile[m[0]] = s[0]; tile[m[1]] = s[1]; tile[m[2]] = s[2]; tile[m[3]] = s[3];
                tile[m[4]] = s[4]; tile[m[5]] = s[5]; tile[m[6]] = s[6]; tile[m[7]] = s[7];
            }
        }
}

static void build_image(void)
{
    for (int y = 0; y < TEX_N; y++)
        for (int x = 0; x < TEX_N; x++) {
            u32 c = ((x ^ y) & 32) ? 0xF0 : 0x50;
            u32 r = (u32)x, g = (u32)y, b = c;
            g_img[y * TEX_N + x] = (r << 24) | (g << 16) | (b << 8) | 0xFF;   // PICA RGBA8: 0xRRGGBBAA
        }
}

// DisplayTransfer linear -> tiled into the VRAM texture. Returns seconds of GPU time (submit -> idle) or -1.
static double upload_vram_transfer(void)
{
    if (!frame_begin()) return -1;
    GX_DisplayTransfer(g_stage, GX_BUFFER_DIM(TEX_N, TEX_N), (u32*)g_texV.data, GX_BUFFER_DIM(TEX_N, TEX_N),
                       XFER_TO_TEXTURE);
    return frame_end_timed();
}

// Raw DMA copy (TextureCopy) of already-tiled data into the VRAM texture.
static double upload_vram_copy(void)
{
    if (!frame_begin()) return -1;
    GX_TextureCopy(g_stage, 0, (u32*)g_texV.data, 0, TEX_BYTES, COPY_RAW);
    return frame_end_timed();
}

// ---- reporting -------------------------------------------------------------------------------------------------
static double report_tri(const char* name, int tris, double ms, bool drawn)
{
    if (ms < 0) { P("%-14s FAILED\n", name); sum_add(name, -1, "Mtri/s"); return -1; }
    double m = tris / (ms * 1000.0);
    P("%-14s %6.2f ms/f %6.2f Mtri/s%s\n", name, ms, m, drawn ? "" : " INVALID");
    sum_add(name, drawn ? m : -1, "Mtri/s");
    return drawn ? m : -1;
}

static double report_fill(const char* name, double ms, bool drawn)
{
    if (ms < 0) { P("%-14s FAILED\n", name); sum_add(name, -1, "Mpx/s"); return -1; }
    double m = (double)FILL_K * SCR_PX / (ms * 1000.0);
    P("%-14s %6.2f ms/f %6.1f Mpx/s%s\n", name, ms, m, drawn ? "" : " INVALID");
    sum_add(name, drawn ? m : -1, "Mpx/s");
    return drawn ? m : -1;
}

// ---- tests -----------------------------------------------------------------------------------------------------
static void test_overhead(void)
{
    step("gpu frame overhead");
    if (!gpu_ok("frame overhead") || !g_quad) return;
    use_simple(&g_projTop);
    attr_vtx();
    if (!buf_vtx(g_quad)) { P("frame overhead SKIPPED: BufInfo\n"); return; }
    env_color();
    bool drawn;
    double ms = measure(draw_tiny, 0, false, &drawn);
    if (ms >= 0) P("frame overhead %.3f ms (submit->idle, 1 tri)\n", ms);
    sum_add("frame overhead", ms, "ms");
}

// 1. Triangle throughput, GC-like vertex from a linear-heap VBO.
static void test_tris(void)
{
    static const char* names[2][2] = { { "tri flat 20k", "tri flat 60k" }, { "tri tex 20k", "tri tex 60k" } };
    static const int reps[2] = { 1, 3 };
    P("-- 1 triangles (24B vtx, ~2.7px tris) --\n");
    if (!g_tri) { P("1 SKIPPED: VBO alloc failed\n"); return; }
    use_simple(&g_projTop);
    attr_vtx();
    if (!buf_vtx(g_tri)) { P("1 SKIPPED: BufInfo\n"); return; }
    double tex60 = -1;
    for (int tex = 0; tex < 2; tex++) {
        if (tex && !g_texVOk) { P("tri tex SKIPPED: no VRAM texture\n"); continue; }
        if (tex) env_tex(&g_texV, 1); else env_color();
        for (int r = 0; r < 2; r++) {
            step(names[tex][r]);
            if (!gpu_ok(names[tex][r])) return;
            bool drawn;
            double ms = measure(draw_tri_arrays, reps[r], true, &drawn);
            double m = report_tri(names[tex][r], reps[r] * GRID_TRIS, ms, drawn);
            if (tex && r == 1) tex60 = m;
        }
    }
    show();
    P("budget: GC 30-60k tri/f @30fps = 0.9-1.8 Mtri/s\n");
    P("  (GC load is an assumption, not measured)\n");
    if (tex60 > 0) P("  60k tex tri here: %.1f ms of a 33.3 ms frame\n", 60000.0 / (tex60 * 1000.0));
}

// 2. Vertex shader cost: minimal (1 matrix) vs GC-like T&L (MV+P, normal, 4 diffuse lights), same geometry.
static void test_vshader(void)
{
    P("-- 2 vertex shader (36B vtx w/ normal) --\n");
    if (!g_triN) { P("2 SKIPPED: VBO alloc failed\n"); return; }
    double ms[2] = { -1, -1 };
    for (int s = 0; s < 2; s++) {
        const char* name = s ? "vs light 60k" : "vs simple 60k";
        step(name);
        if (!gpu_ok(name)) return;
        if (s && !g_progLOk) { P("vs light SKIPPED: shader/uniforms missing\n"); continue; }
        if (s) use_light(); else use_simple(&g_projTop);
        attr_vtxn();
        if (!buf_vtxn(g_triN)) { P("2 SKIPPED: BufInfo\n"); return; }
        env_color();
        bool drawn;
        ms[s] = measure(draw_tri_arrays, 3, true, &drawn);
        report_tri(name, 3 * GRID_TRIS, ms[s], drawn);
        if (!drawn) ms[s] = -1;
    }
    show();
    if (ms[0] > 0 && ms[1] > 0) {
        P("light/simple time: %.2fx\n", ms[1] / ms[0]);
        P("budget: 60k lit tri = %.1f ms of 33.3 ms (30fps)\n", ms[1]);
    }
}

// 3. Fill rate: FILL_K full-screen quads per frame.
typedef struct { const char* name; int quad; int tex; bool blend; bool depth; } FillCase;

static void test_fill(void)
{
    static const FillCase cases[] = {
        { "fill opaque", Q_FULL, 0, false, false },
        { "fill blend", Q_BLEND, 0, true, false },
        { "fill texV 1:1", Q_FULL, 1, false, false },
        { "fill texL 1:1", Q_FULL, 2, false, false },
        { "fill texV 2:1", Q_MIN, 1, false, false },
        { "fill texL 2:1", Q_MIN, 2, false, false },
        { "fill depth", Q_FULL, 0, false, true },
    };
    P("-- 3 fill (%d x 400x240 quads/frame) --\n", FILL_K);
    P("texV=VRAM texL=FCRAM linear, bilinear, no mips\n");
    P("1:1 = 1 texel/px, 2:1 = 2x2 texels/px\n");
    if (!g_quad) { P("3 SKIPPED: quad VBO alloc failed\n"); return; }
    use_simple(&g_projTop);
    attr_vtx();
    if (!buf_vtx(g_quad)) { P("3 SKIPPED: BufInfo\n"); return; }
    double texV = -1, texL = -1, texV2 = -1, texL2 = -1;
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        const FillCase* fc = &cases[c];
        step(fc->name);
        if (!gpu_ok(fc->name)) break;
        if (fc->tex == 1 && !g_texVOk) { P("%s SKIPPED: no VRAM texture\n", fc->name); continue; }
        if (fc->tex == 2 && !g_texLOk) { P("%s SKIPPED: no FCRAM texture\n", fc->name); continue; }
        if (fc->tex) env_tex(fc->tex == 1 ? &g_texV : &g_texL, 1); else env_color();
        if (fc->blend) blend_on(); else blend_off();
        if (fc->depth) depth_on(); else depth_off();
        g_quadFirst = fc->quad;
        bool drawn;
        double ms = measure(draw_quads, FILL_K, true, &drawn);
        double m = report_fill(fc->name, ms, drawn);
        if (c == 2) texV = m;
        if (c == 3) texL = m;
        if (c == 4) texV2 = m;
        if (c == 5) texL2 = m;
    }
    blend_off();
    depth_off();
    show();
    if (texV > 0 && texL > 0) P("FCRAM/VRAM tex speed 1:1: %.2f\n", texL / texV);
    if (texV2 > 0 && texL2 > 0) P("FCRAM/VRAM tex speed 2:1: %.2f\n", texL2 / texV2);
    P("budget: 400x240 x4 overdraw @30fps = 11.5 Mpx/s\n");
    P("  (overdraw 4 is an assumption)\n");
    if (texV > 0) P("  that load textured VRAM: %.1f ms/frame\n", 4.0 * SCR_PX / (texV * 1000.0));
}

// 4. Combiner stages: textured (VRAM) fill with 1, 3 and 6 active TexEnv stages.
static void test_tev(void)
{
    static const char* names[3] = { "tev 1 stage", "tev 3 stages", "tev 6 stages" };
    static const int stages[3] = { 1, 3, 6 };
    P("-- 4 combiner stages (tex fill, %d quads) --\n", FILL_K);
    if (!g_quad || !g_texVOk) { P("4 SKIPPED: quad VBO or VRAM texture missing\n"); return; }
    use_simple(&g_projTop);
    attr_vtx();
    if (!buf_vtx(g_quad)) { P("4 SKIPPED: BufInfo\n"); return; }
    blend_off();
    depth_off();
    g_quadFirst = Q_FULL;
    double m[3] = { -1, -1, -1 };
    for (int i = 0; i < 3; i++) {
        step(names[i]);
        if (!gpu_ok(names[i])) break;
        env_tex(&g_texV, stages[i]);
        bool drawn;
        double ms = measure(draw_quads, FILL_K, true, &drawn);
        m[i] = report_fill(names[i], ms, drawn);
    }
    show();
    env_tex(&g_texV, 1);
    if (m[0] > 0 && m[2] > 0) P("6 vs 1 stage speed: %.2f (1.00 = free)\n", m[2] / m[0]);
}

// 5. Texture upload, 256x256 RGBA8, from a decoded image in ordinary heap.
static void test_upload(void)
{
    P("-- 5 texture upload 256x256 RGBA8 --\n");
    if (!g_stage || !g_img || !g_texVOk || !g_texLOk) { P("5 SKIPPED: buffers/textures missing\n"); return; }
    const double mb = TEX_BYTES / 1e6;

    // (a) CPU writes linear data (memcpy), flush, DisplayTransfer linear->tiled into VRAM.
    step("upload a: transfer");
    if (!gpu_ok("upload a")) return;
    double cpu = 0, fl = 0, gpu = 0;
    int n = 0;
    for (int i = 0; i < UPLOAD_ITERS; i++, n++) {
        double t0 = now_s();
        memcpy(g_stage, g_img, TEX_BYTES);
        double t1 = now_s();
        GSPGPU_FlushDataCache(g_stage, TEX_BYTES);
        double t2 = now_s();
        double g = upload_vram_transfer();
        if (g < 0) return;
        cpu += t1 - t0; fl += t2 - t1; gpu += g;
    }
    double a = (cpu + fl + gpu) * 1000.0 / n;
    P("a linear+xfer %.2f ms %5.0f MB/s\n", a, mb / (a / 1000.0));
    P("  memcpy %.2f flush %.2f xfer %.2f ms\n", cpu * 1000.0 / n, fl * 1000.0 / n, gpu * 1000.0 / n);
    sum_add("upload a xfer", a, "ms");

    // Check: does the CPU swizzle produce exactly what the transfer engine produced?
    if (g_rb && frame_begin()) {
        GSPGPU_FlushDataCache(g_rb, TEX_BYTES);
        GX_TextureCopy((u32*)g_texV.data, 0, g_rb, 0, TEX_BYTES, COPY_RAW);
        if (frame_end_timed() >= 0) {
            GSPGPU_InvalidateDataCache(g_rb, TEX_BYTES);
            swizzle256((u32*)g_texL.data, g_img);          // g_texL is linear FCRAM (checked at init)
            GSPGPU_FlushDataCache(g_texL.data, TEX_BYTES);
            P("swizzle == GPU tiling: %s\n", memcmp(g_rb, g_texL.data, TEX_BYTES) ? "NO (layout unverified)" : "yes");
        }
    }

    // (b1) CPU swizzles straight into the linear-FCRAM texture; the GPU samples it there. No DMA.
    step("upload b1: swizzle fcram");
    if (!gpu_ok("upload b1")) return;
    cpu = fl = 0;
    for (int i = 0; i < UPLOAD_ITERS; i++) {
        double t0 = now_s();
        swizzle256((u32*)g_texL.data, g_img);
        double t1 = now_s();
        GSPGPU_FlushDataCache(g_texL.data, TEX_BYTES);
        cpu += t1 - t0; fl += now_s() - t1;
    }
    double b1 = (cpu + fl) * 1000.0 / UPLOAD_ITERS;
    P("b1 swz->FCRAM %.2f ms %5.0f MB/s\n", b1, mb / (b1 / 1000.0));
    P("  swizzle %.2f flush %.2f ms\n", cpu * 1000.0 / UPLOAD_ITERS, fl * 1000.0 / UPLOAD_ITERS);
    sum_add("upload b1 fcram", b1, "ms");

    // (b2) CPU swizzles into staging, flush, raw DMA (TextureCopy) into VRAM.
    step("upload b2: swizzle+dma");
    if (!gpu_ok("upload b2")) return;
    cpu = fl = gpu = 0;
    for (int i = 0; i < UPLOAD_ITERS; i++) {
        double t0 = now_s();
        swizzle256(g_stage, g_img);
        double t1 = now_s();
        GSPGPU_FlushDataCache(g_stage, TEX_BYTES);
        double t2 = now_s();
        double g = upload_vram_copy();
        if (g < 0) return;
        cpu += t1 - t0; fl += t2 - t1; gpu += g;
    }
    double b2 = (cpu + fl + gpu) * 1000.0 / UPLOAD_ITERS;
    P("b2 swz+DMA->VRAM %.2f ms %5.0f MB/s\n", b2, mb / (b2 / 1000.0));
    P("  swizzle %.2f flush %.2f dma %.2f ms\n", cpu * 1000.0 / UPLOAD_ITERS, fl * 1000.0 / UPLOAD_ITERS,
      gpu * 1000.0 / UPLOAD_ITERS);
    sum_add("upload b2 dma", b2, "ms");
    P("budget: 1 MB new tex/frame (assumed) = %.1f ms\n", (b1 < a ? b1 : a) / mb);

    // Show the uploaded texture.
    if (g_quad && gpu_ok("upload show")) {
        use_simple(&g_projTop);
        attr_vtx();
        if (buf_vtx(g_quad)) {
            env_tex(&g_texV, 1);
            g_quadFirst = Q_SHOW;
            bool drawn;
            measure(draw_quads, 1, false, &drawn);
            show();
        }
    }
}

// 6. Render to texture (the EFB-copy path).
//   mode 0 baseline: small scene + full-screen quad sampling a static VRAM texture, all into the screen target
//   mode 1 RTT:      scene into a 256x256 texture target, then the quad samples that texture
//   mode 2 RTT+copy: as 1, plus a raw TextureCopy of the 256x256 target into a second texture that is sampled
static void draw_rtt(int mode)
{
    if (mode) C3D_FrameDrawOn(g_rtA);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_uProjS, mode ? &g_projTex : &g_projTop);
    buf_vtx(g_tri);
    env_color();
    C3D_DrawArrays(GPU_TRIANGLES, 0, 6000);           // 2000 triangles
    if (mode == 2) {
        C3D_FrameSplit(GX_CMDLIST_FLUSH);            // finish the scene (citro3d adds the framebuffer flush)
        GX_TextureCopy((u32*)g_texA.data, 0, (u32*)g_texB.data, 0, TEX_BYTES, COPY_RAW);
    }
    if (mode) C3D_FrameDrawOn(g_tgt);
    C3D_FVUnifMtx4x4(GPU_VERTEX_SHADER, g_uProjS, &g_projTop);
    buf_vtx(g_quad);
    env_tex(mode == 0 ? &g_texV : mode == 1 ? &g_texA : &g_texB, 1);
    C3D_DrawArrays(GPU_TRIANGLES, Q_SHOW, 6);
}

static void test_rtt(void)
{
    static const char* names[3] = { "rtt baseline", "rtt direct", "rtt +copy" };
    P("-- 6 render to texture 256x256 RGBA8 --\n");
    if (!g_tri || !g_quad || !g_texVOk) { P("6 SKIPPED: VBO or texture missing\n"); return; }
    g_texAOk = C3D_TexInitVRAM(&g_texA, TEX_N, TEX_N, GPU_RGBA8);
    g_texBOk = C3D_TexInitVRAM(&g_texB, TEX_N, TEX_N, GPU_RGBA8);
    if (g_texAOk) g_rtA = C3D_RenderTargetCreateFromTex(&g_texA, GPU_TEXFACE_2D, 0, -1);
    if (!g_texAOk || !g_texBOk || !g_rtA) { P("6 SKIPPED: VRAM texture/target alloc failed\n"); return; }
    C3D_TexSetFilter(&g_texA, GPU_LINEAR, GPU_LINEAR);
    C3D_TexSetFilter(&g_texB, GPU_LINEAR, GPU_LINEAR);

    // Clear both textures to the sentinel so the content check below means something.
    if (!frame_begin()) return;
    C3D_RenderTargetClear(g_rtA, C3D_CLEAR_COLOR, SENTINEL, 0);
    C3D_FrameBuf fbB = g_rtA->frameBuf;
    fbB.colorBuf = g_texB.data;
    C3D_FrameBufClear(&fbB, C3D_CLEAR_COLOR, SENTINEL, 0);
    if (frame_end_timed() < 0) return;

    use_simple(&g_projTop);
    attr_vtx();
    blend_off();
    depth_off();
    double ms[3] = { -1, -1, -1 };
    for (int m = 0; m < 3; m++) {
        step(names[m]);
        if (!gpu_ok(names[m])) break;
        bool drawn;
        ms[m] = measure(draw_rtt, m, false, &drawn);
        if (ms[m] >= 0) P("%-14s %6.3f ms/cycle\n", names[m], ms[m]);
    }
    show();
    sum_add("rtt direct", ms[1], "ms");
    sum_add("rtt +copy", ms[2], "ms");
    if (ms[0] >= 0 && ms[1] >= 0) P("RTT switch cost: %+.3f ms vs baseline\n", ms[1] - ms[0]);
    if (ms[1] >= 0 && ms[2] >= 0) P("256x256 copy cost: %+.3f ms\n", ms[2] - ms[1]);

    // Readback: DisplayTransfer tiled VRAM target -> linear FCRAM, invalidate, CPU reads every pixel.
    step("rtt readback");
    if (!gpu_ok("rtt readback") || !g_rb) return;
    GSPGPU_FlushDataCache(g_rb, TEX_BYTES);
    double gpu = 0, inv = 0, rd = 0;
    u32 drawnPx = 0;
    int n = 0;
    for (; n < UPLOAD_ITERS; n++) {
        if (!frame_begin()) return;
        GX_DisplayTransfer((u32*)g_texA.data, GX_BUFFER_DIM(TEX_N, TEX_N), g_rb, GX_BUFFER_DIM(TEX_N, TEX_N),
                           XFER_TO_LINEAR);
        double g = frame_end_timed();
        if (g < 0) return;
        double t1 = now_s();
        GSPGPU_InvalidateDataCache(g_rb, TEX_BYTES);
        double t2 = now_s();
        u32 cnt = 0;
        for (int i = 0; i < TEX_N * TEX_N; i++) cnt += g_rb[i] != SENTINEL;
        double t3 = now_s();
        drawnPx = cnt;
        gpu += g; inv += t2 - t1; rd += t3 - t2;
    }
    double tot = (gpu + inv + rd) * 1000.0 / n;
    P("readback %.2f ms (%.0f MB/s)\n", tot, TEX_BYTES / 1e6 / (tot / 1000.0));
    P("  xfer %.2f inval %.2f cpu read %.2f ms\n", gpu * 1000.0 / n, inv * 1000.0 / n, rd * 1000.0 / n);
    P("RTT content: %lu%% of pixels drawn%s\n", (unsigned long)(drawnPx * 100u / (TEX_N * TEX_N)),
      drawnPx ? "" : " -> RTT FAILED, numbers INVALID");
    sum_add("readback 256", tot, "ms");
    if (ms[2] >= 0) P("budget: 4 EFB copies/f (assumed) = %.1f ms\n", 4.0 * (ms[2] - (ms[0] >= 0 ? ms[0] : 0)));
}

// 7. CPU vertex conversion, GC FIFO format -> PICA VBO in linear memory. No GPU involved.
static void conv_to_float(const u8* in, Vtx* out, int n)
{
    const float ps = 1.0f / 256.0f, ts = 1.0f / 1024.0f;
    for (int i = 0; i < n; i++, in += GC_STRIDE, out++) {
        out->x = (float)(s16)((in[0] << 8) | in[1]) * ps;
        out->y = (float)(s16)((in[2] << 8) | in[3]) * ps;
        out->z = (float)(s16)((in[4] << 8) | in[5]) * ps;
        out->r = in[6]; out->g = in[7]; out->b = in[8]; out->a = in[9];
        out->u = (float)(s16)((in[10] << 8) | in[11]) * ts;
        out->v = (float)(s16)((in[12] << 8) | in[13]) * ts;
    }
}

// Byteswap only: s16 stays s16 (the PICA can load GPU_SHORT attributes and scale them in the shader).
static void conv_to_s16(const u8* in, VtxS* out, int n)
{
    for (int i = 0; i < n; i++, in += GC_STRIDE, out++) {
        out->x = (s16)((in[0] << 8) | in[1]);
        out->y = (s16)((in[2] << 8) | in[3]);
        out->z = (s16)((in[4] << 8) | in[5]);
        out->pad = 0;
        out->r = in[6]; out->g = in[7]; out->b = in[8]; out->a = in[9];
        out->u = (s16)((in[10] << 8) | in[11]);
        out->v = (s16)((in[12] << 8) | in[13]);
    }
}

static s16 gen16(int i, int k) { return (s16)(u16)((u32)i * (u32)(2 * k + 3) * 37u + (u32)k * 1000u); }

static void test_convert(void)
{
    P("-- 7 CPU vertex convert (100k GC verts) --\n");
    step("cpu vertex convert");
    u8* in = malloc(CONV_N * GC_STRIDE);
    void* out = linearAlloc(CONV_N * sizeof(Vtx));
    if (!in || !out) {
        P("7 SKIPPED: alloc failed\n");
        free(in);
        if (out) linearFree(out);
        return;
    }
    for (int i = 0; i < CONV_N; i++) {
        u8* p = in + i * GC_STRIDE;
        for (int k = 0; k < 3; k++) { u16 v = (u16)gen16(i, k); p[2 * k] = (u8)(v >> 8); p[2 * k + 1] = (u8)v; }
        p[6] = (u8)i; p[7] = (u8)(i >> 8); p[8] = 0x80; p[9] = 0xFF;
        for (int k = 0; k < 2; k++) { u16 v = (u16)gen16(i, 3 + k); p[10 + 2 * k] = (u8)(v >> 8); p[11 + 2 * k] = (u8)v; }
    }
    for (int variant = 0; variant < 2; variant++) {
        double best = 1e9, sum = 0, fl = 0;
        const int passes = 4;
        for (int p = 0; p <= passes; p++) {        // pass 0 is a warm-up
            double t0 = now_s();
            if (variant == 0) conv_to_float(in, out, CONV_N); else conv_to_s16(in, out, CONV_N);
            double t1 = now_s();
            GSPGPU_FlushDataCache(out, CONV_N * (variant ? sizeof(VtxS) : sizeof(Vtx)));
            double t2 = now_s();
            if (p == 0) continue;
            sum += t1 - t0; fl += t2 - t1;
            if (t1 - t0 < best) best = t1 - t0;
        }
        // Correctness: spot-check against the generator (power-of-two scales are exact in float).
        bool ok = true;
        for (int i = 0; i < CONV_N; i += 9973) {
            if (variant == 0) {
                const Vtx* v = (const Vtx*)out + i;
                ok &= v->x == gen16(i, 0) / 256.0f && v->z == gen16(i, 2) / 256.0f && v->v == gen16(i, 4) / 1024.0f;
                ok &= v->r == (u8)i && v->a == 0xFF;
            } else {
                const VtxS* v = (const VtxS*)out + i;
                ok &= v->x == gen16(i, 0) && v->z == gen16(i, 2) && v->v == gen16(i, 4) && v->g == (u8)(i >> 8);
            }
        }
        double mean = sum / passes;
        double mv = CONV_N / mean / 1e6;
        const char* name = variant ? "conv ->s16 16B" : "conv ->float 24B";
        P("%-16s %5.2f Mvert/s %5.2f ms%s\n", name, mv, mean * 1000.0, ok ? "" : " WRONG OUTPUT");
        P("  best %.2f ms, flush %.2f ms\n", best * 1000.0, fl * 1000.0 / passes);
        sum_add(name, ok ? mv : -1, "Mvert/s");
        if (variant == 0) {
            P("budget: 100k verts/f @30fps = %.1f ms CPU\n", mean * 1000.0 + fl * 1000.0 / passes);
            P("  = %.0f%% of a 33.3 ms frame (GC load assumed)\n", (mean + fl / passes) * 1000.0 / 33.333 * 100.0);
        }
    }
    free(in);
    linearFree(out);
}

// 8. Index fetch: the same 60k triangles drawn three ways.
static void test_index(void)
{
    static const char* names[3] = { "idx arrays", "idx elems 1:1", "idx elems grid" };
    P("-- 8 index fetch (same 60k tris/frame) --\n");
    P("arrays=DrawArrays, 1:1=u16 identity, grid=shared\n");
    if (!g_tri || !g_idxIdent || !g_grid || !g_idxGrid) { P("8 SKIPPED: buffers missing\n"); return; }
    use_simple(&g_projTop);
    attr_vtx();
    env_color();
    blend_off();
    depth_off();
    double m[3] = { -1, -1, -1 };
    for (int i = 0; i < 3; i++) {
        step(names[i]);
        if (!gpu_ok(names[i])) break;
        if (!buf_vtx(i == 2 ? (const void*)g_grid : (const void*)g_tri)) { P("%s SKIPPED: BufInfo\n", names[i]); continue; }
        bool drawn;
        double ms = measure(i == 0 ? draw_tri_arrays : i == 1 ? draw_tri_ident : draw_grid_elems, 3, true, &drawn);
        m[i] = report_tri(names[i], 3 * GRID_TRIS, ms, drawn);
    }
    show();
    if (m[0] > 0 && m[1] > 0) P("index fetch cost: 1:1 elems %.2fx arrays\n", m[1] / m[0]);
    if (m[0] > 0 && m[2] > 0) P("vertex reuse (grid): %.2fx arrays\n", m[2] / m[0]);
}

// ---- setup / teardown ------------------------------------------------------------------------------------------
// VRAM is physical 0x18000000-0x185FFFFF (6 MB).
static bool is_vram(const void* p)
{
    u32 pa = osConvertVirtToPhys(p);
    return pa >= 0x18000000u && pa < 0x18600000u;
}

static bool load_shader(DVLB_s** dvlb, shaderProgram_s* prog, const u8* bin, u32 size)
{
    *dvlb = DVLB_ParseFile((u32*)bin, size);
    if (!*dvlb) return false;
    if (R_FAILED(shaderProgramInit(prog))) return false;
    if (R_FAILED(shaderProgramSetVsh(prog, &(*dvlb)->DVLE[0]))) { shaderProgramFree(prog); return false; }
    return true;
}

static void free_all(void)
{
    if (g_dead) {
        P("GPU hung: citro3d left initialised, GPU memory leaked\n");
        free(g_img);
        g_img = NULL;
        return;
    }
    // Every frame_end_timed() returned only after the queue went idle, so the waits inside these calls return
    // immediately.
    if (g_rtA) C3D_RenderTargetDelete(g_rtA);
    if (g_tgt) C3D_RenderTargetDelete(g_tgt);
    if (g_texAOk) C3D_TexDelete(&g_texA);
    if (g_texBOk) C3D_TexDelete(&g_texB);
    if (g_texVOk) C3D_TexDelete(&g_texV);
    if (g_texLOk) C3D_TexDelete(&g_texL);
    void* lin[] = { g_tri, g_triN, g_grid, g_idxGrid, g_idxIdent, g_quad, g_rb, g_stage };
    for (unsigned i = 0; i < sizeof lin / sizeof lin[0]; i++)
        if (lin[i]) linearFree(lin[i]);
    free(g_img);
    if (g_progSOk) shaderProgramFree(&g_progS);
    if (g_progLOk) shaderProgramFree(&g_progL);
    if (g_dvlbS) DVLB_Free(g_dvlbS);
    if (g_dvlbL) DVLB_Free(g_dvlbL);
    if (g_c3d) C3D_Fini();
}

static void reset_globals(void)
{
    g_dead = g_c3d = g_progSOk = g_progLOk = g_texVOk = g_texLOk = g_texAOk = g_texBOk = g_canShow = false;
    g_dvlbS = g_dvlbL = NULL;
    g_tgt = g_rtA = NULL;
    g_tri = NULL; g_triN = NULL; g_grid = NULL; g_quad = NULL;
    g_idxGrid = g_idxIdent = NULL;
    g_rb = g_stage = g_img = NULL;
    g_nsum = 0;
}

static void print_summary(void)
{
    P("-- GPU summary (-1 = failed/skipped) --\n");
    for (int i = 0; i < g_nsum; i++) {
        if (g_sum[i].v < 0) P("%-16s      -1 %s\n", g_sum[i].name, g_sum[i].unit);
        else P("%-16s %7.2f %s\n", g_sum[i].name, g_sum[i].v, g_sum[i].unit);
    }
    P("GPU module %.1f s%s\n", now_s() - g_t0, g_dead ? " (GPU TIMED OUT)" : "");
}

void bench_gpu(void)
{
    reset_globals();
    g_t0 = now_s();
    P("\n== GPU (PICA200 via citro3d) ==\n");
    P("times: submit->GPU idle, no vsync, offscreen\n");

    step("gpu init");
    g_c3d = C3D_Init(C3D_DEFAULT_CMDBUF_SIZE);
    if (!g_c3d) {
        P("GPU tests SKIPPED: C3D_Init failed\n");
        test_convert();
        print_summary();
        return;
    }
    g_progSOk = load_shader(&g_dvlbS, &g_progS, gpu_simple_shbin, gpu_simple_shbin_size);
    g_progLOk = load_shader(&g_dvlbL, &g_progL, gpu_light_shbin, gpu_light_shbin_size);
    if (g_progSOk) g_uProjS = shaderInstanceGetUniformLocation(g_progS.vertexShader, "projection");
    if (g_progLOk) {
        shaderInstance_s* vs = g_progL.vertexShader;
        g_uProjL = shaderInstanceGetUniformLocation(vs, "projection");
        g_uMv = shaderInstanceGetUniformLocation(vs, "modelView");
        g_uNrm = shaderInstanceGetUniformLocation(vs, "nrmMtx");
        g_uLDir = shaderInstanceGetUniformLocation(vs, "lightDir");
        g_uLClr = shaderInstanceGetUniformLocation(vs, "lightClr");
        g_uAmb = shaderInstanceGetUniformLocation(vs, "ambient");
        if (g_uProjL < 0 || g_uMv < 0 || g_uNrm < 0 || g_uLDir < 0 || g_uLClr < 0 || g_uAmb < 0) {
            P("light shader: uniform missing, test 2b off\n");
            g_progLOk = false;
            shaderProgramFree(&g_progL);
        }
    }
    if (!g_progSOk || g_uProjS < 0) {
        P("GPU tests SKIPPED: simple shader failed to load\n");
        test_convert();
        free_all();
        print_summary();
        return;
    }

    g_tgt = C3D_RenderTargetCreate(SCR_H, SCR_W, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    if (!g_tgt) {
        P("GPU tests SKIPPED: render target alloc failed\n");
        test_convert();
        free_all();
        print_summary();
        return;
    }
    u16 fbw = 0, fbh = 0;
    gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fbw, &fbh);
    GSPGPU_FramebufferFormat topFmt = gfxGetScreenFormat(GFX_TOP);
    g_canShow = fbw == SCR_H && fbh == SCR_W && (u32)topFmt <= GSP_RGBA4_OES;
    g_dispFlags = GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) |
                  GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT((u32)topFmt) |
                  GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO);
    if (!g_canShow) P("top screen is %ux%u fmt %d: not shown\n", fbw, fbh, (int)topFmt);

    Mtx_OrthoTilt(&g_projTop, 0.0f, SCR_W, 0.0f, SCR_H, 0.0f, 1.0f, true);
    Mtx_Ortho(&g_projTex, 0.0f, SCR_W, 0.0f, SCR_H, 0.0f, 1.0f, true);
    C3D_CullFace(GPU_CULL_NONE);
    depth_off();
    blend_off();

    g_tri = linearAlloc(TRI_VERTS * sizeof(Vtx));
    g_triN = linearAlloc(TRI_VERTS * sizeof(VtxN));
    g_grid = linearAlloc(GRID_VERTS * sizeof(Vtx));
    g_idxGrid = linearAlloc(TRI_VERTS * sizeof(u16));
    g_idxIdent = linearAlloc(TRI_VERTS * sizeof(u16));
    g_quad = linearAlloc(Q_COUNT * sizeof(Vtx));
    g_rb = linearAlloc(RB_BYTES);
    g_stage = linearAlloc(TEX_BYTES);
    g_img = malloc(TEX_BYTES);
    if (!g_tri || !g_triN || !g_grid || !g_idxGrid || !g_idxIdent || !g_quad || !g_rb || !g_stage || !g_img)
        P("note: some buffers failed to alloc (tests skip)\n");
    build_geometry();
    morton_init();

    // Textures: VRAM (filled by DisplayTransfer) and linear FCRAM (filled by the CPU swizzle).
    g_texVOk = C3D_TexInitVRAM(&g_texV, TEX_N, TEX_N, GPU_RGBA8);
    g_texLOk = C3D_TexInit(&g_texL, TEX_N, TEX_N, GPU_RGBA8);
    // The CPU writes g_texL directly, so it must be linear FCRAM. VRAM is never touched by the CPU in this module
    // (a CPU write to VRAM crashed the app on the device): VRAM is filled only by GX transfers / memory fills and
    // read only by transferring it to linear memory first.
    if (g_texLOk && is_vram(g_texL.data)) { C3D_TexDelete(&g_texL); g_texLOk = false; }
    if (g_img) build_image();
    if (g_texVOk && (!g_img || !g_stage)) { C3D_TexDelete(&g_texV); g_texVOk = false; }
    if (g_texLOk && !g_img) { C3D_TexDelete(&g_texL); g_texLOk = false; }
    if (g_texVOk) {
        memcpy(g_stage, g_img, TEX_BYTES);
        GSPGPU_FlushDataCache(g_stage, TEX_BYTES);
        if (upload_vram_transfer() < 0) g_texVOk = false;
        C3D_TexSetFilter(&g_texV, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&g_texV, GPU_REPEAT, GPU_REPEAT);
    }
    if (g_texLOk) {
        swizzle256((u32*)g_texL.data, g_img);
        GSPGPU_FlushDataCache(g_texL.data, TEX_BYTES);
        C3D_TexSetFilter(&g_texL, GPU_LINEAR, GPU_LINEAR);
        C3D_TexSetWrap(&g_texL, GPU_REPEAT, GPU_REPEAT);
    }
    if (!g_texVOk || !g_texLOk) P("note: texture alloc failed: V=%d L=%d\n", g_texVOk, g_texLOk);

    test_overhead();
    test_tris();
    test_vshader();
    test_fill();
    test_tev();
    test_upload();
    test_rtt();
    test_convert();
    test_index();

    step("gpu cleanup");
    free_all();
    print_summary();
}
