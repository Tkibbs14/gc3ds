// GPU probe (run with --timing-probe only, after the CPU probe): micro-benchmarks of the New 2DS XL's PICA200 for the
// timing model (docs/research/TIMING_MODEL_PLAN.md, "GPU"). GPU time is citro3d's C3D_GetDrawingTime (the GPU's own
// frame time, from its completion interrupts); each case is drawn in several frames and the fastest is kept. Only in
// probe-only jobs: it rebinds citro3d state that gc3ds's renderer (gx/pica) assumes it owns, and no game runs after.
//   vertex FMT      60,000 vertices of zero-area triangles (nothing filled): ns per vertex by attribute format
//   fill CASE       full-screen layers at 400x240: ns per pixel (and Mpixel/s) plain, blended, depth-tested, textured
//                   by format and size (a large texture sampled across the screen misses the texture cache), and by
//                   the number of combiner stages
//   draws CASE      1,000 one-triangle draws: GPU and CPU microseconds per draw, with and without a state change
//   xfer CASE       display transfer, memory fill, texture copy: MB/s
#include <3ds.h>
#include <citro3d.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "gpuprobe_shbin.h"

static char g_buf[8 << 10];
static size_t g_len;
extern FILE* probe_live;  // timingprobe.c: lines also go out as they are measured
static void out(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    size_t at = g_len;
    int n = vsnprintf(g_buf + g_len, sizeof g_buf - g_len, fmt, ap);
    va_end(ap);
    if (n > 0) g_len += (size_t)n < sizeof g_buf - g_len ? (size_t)n : sizeof g_buf - g_len - 1;
    if (probe_live && g_len > at) {
        fputs(g_buf + at, probe_live);
        fflush(probe_live);
    }
}

typedef struct { float x, y, z; u8 r, g, b, a; float u, v; } Vert;  // 24 bytes
typedef struct { s16 x, y, z, pad; u8 r, g, b, a; s16 u, v; } VertS;  // 16 bytes

static C3D_RenderTarget* g_rt;
static DVLB_s* g_dvlb;
static shaderProgram_s g_prog;

// Draws `draw` in six frames on the probe's target and returns the fastest GPU time (ms) of the last five, and the
// CPU's processing time of that frame.
typedef void (*DrawFn)(void* ctx);
static float frames(DrawFn draw, void* ctx, float* cpuMs) {
    float best = 1e9f, cpu = 0;
    for (int i = 0; i < 6; i++) {
        C3D_FrameBegin(C3D_FRAME_SYNCDRAW);  // waits for the previous frame: its drawing time is now known
        if (i > 0) {
            float t = C3D_GetDrawingTime();
            if (t > 0 && t < best) {
                best = t;
                cpu = C3D_GetProcessingTime();
            }
        }
        C3D_FrameDrawOn(g_rt);
        if (i < 5) draw(ctx);
        C3D_FrameEnd(0);
    }
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);  // the last frame finished before anything else uses the GPU
    C3D_FrameEnd(0);
    if (cpuMs) *cpuMs = cpu;
    return best;
}

static void env_color_only(void) {
    C3D_TexEnv* e = C3D_GetTexEnv(0);
    C3D_TexEnvInit(e);
    C3D_TexEnvSrc(e, C3D_Both, GPU_PRIMARY_COLOR, 0, 0);
    C3D_TexEnvFunc(e, C3D_Both, GPU_REPLACE);
    for (int i = 1; i < 6; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));
}
static void env_textured(void) {
    C3D_TexEnv* e = C3D_GetTexEnv(0);
    C3D_TexEnvInit(e);
    C3D_TexEnvSrc(e, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, 0);
    C3D_TexEnvFunc(e, C3D_Both, GPU_MODULATE);
    for (int i = 1; i < 6; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));
}
// n stages: stage 0 the vertex colour, each further stage multiplies by a constant.
static void env_stages(int n) {
    env_color_only();
    for (int i = 1; i < n; i++) {
        C3D_TexEnv* e = C3D_GetTexEnv(i);
        C3D_TexEnvInit(e);
        C3D_TexEnvSrc(e, C3D_Both, GPU_PREVIOUS, GPU_CONSTANT, 0);
        C3D_TexEnvFunc(e, C3D_Both, GPU_MODULATE);
        C3D_TexEnvColor(e, 0xFFF0F0F0);
    }
}

static void attrs_float(int n) {  // position, then colour, then texture coordinate: the first n of them
    C3D_AttrInfo* ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 3);
    if (n > 1) AttrInfo_AddLoader(ai, 1, GPU_UNSIGNED_BYTE, 4);
    if (n > 2) AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 2);
}
static void bufs(void* data, int stride, int nattr) {
    C3D_BufInfo* bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, data, stride, nattr, nattr == 1 ? 0x0 : nattr == 2 ? 0x10 : 0x210);
}

// ---- vertex throughput ----
typedef struct { int count; } VCtx;
static void draw_tris(void* c) { C3D_DrawArrays(GPU_TRIANGLES, 0, ((VCtx*)c)->count); }

// ---- fill ----
typedef struct { int layers; } FCtx;
static void draw_layers(void* c) {
    for (int i = 0; i < ((FCtx*)c)->layers; i++) C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
}

// ---- draw calls ----
typedef struct { int n; bool change; } DCtx;
static void draw_many(void* c) {
    DCtx* d = c;
    for (int i = 0; i < d->n; i++) {
        if (d->change) C3D_TexEnvColor(C3D_GetTexEnv(0), 0xFF000000u | (u32)i);
        C3D_DrawArrays(GPU_TRIANGLES, 0, 3);
    }
}

const char* gpu_probe(void) {
    g_len = 0;
    g_buf[0] = 0;
    u64 t0 = svcGetSystemTick();
    out("# gpu: probe start (PICA200; GPU ms = C3D_GetDrawingTime, fastest of five frames)\n");
    g_rt = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);
    g_dvlb = DVLB_ParseFile((u32*)gpuprobe_shbin, gpuprobe_shbin_size);
    if (!g_rt || !g_dvlb) {
        out("# gpu: cannot create the target or the shader\n");
        return g_buf;
    }
    shaderProgramInit(&g_prog);
    shaderProgramSetVsh(&g_prog, &g_dvlb->DVLE[0]);
    C3D_BindProgram(&g_prog);
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);  // no blending
    env_color_only();

    // Vertex throughput: zero-area triangles (all three corners equal), so nothing reaches the rasterizer's fill.
    out("# gpu: section vertex\n");
    const int kVerts = 60000;
    Vert* vf = linearAlloc(sizeof(Vert) * kVerts);
    VertS* vs = linearAlloc(sizeof(VertS) * kVerts);
    if (vf && vs) {
        for (int i = 0; i < kVerts; i++) {
            float x = (float)((i / 3) % 200) / 100.0f - 1.0f;
            vf[i] = (Vert){x, 0.0f, 0.5f, 255, 255, 255, 255, 0.0f, 0.0f};
            vs[i] = (VertS){(s16)(x * 100), 0, 50, 0, 255, 255, 255, 255, 0, 0};
        }
        GSPGPU_FlushDataCache(vf, sizeof(Vert) * kVerts);
        GSPGPU_FlushDataCache(vs, sizeof(VertS) * kVerts);
        VCtx vc = {kVerts};
        static const struct { const char* name; int nattr; } kF[] = {{"pos float3", 1}, {"pos float3 + color u8x4", 2},
                                                                    {"pos float3 + color u8x4 + tc float2", 3}};
        for (int k = 0; k < 3; k++) {
            attrs_float(kF[k].nattr);
            bufs(vf, sizeof(Vert), kF[k].nattr);
            float ms = frames(draw_tris, &vc, NULL);
            out("# gpu vertex %s (24-byte stride): %.2f ns per vertex (%.2f ms for %d)\n", kF[k].name, ms * 1e6f / kVerts, ms, kVerts);
        }
        C3D_AttrInfo* ai = C3D_GetAttrInfo();
        AttrInfo_Init(ai);
        AttrInfo_AddLoader(ai, 0, GPU_SHORT, 4);
        AttrInfo_AddLoader(ai, 1, GPU_UNSIGNED_BYTE, 4);
        AttrInfo_AddLoader(ai, 2, GPU_SHORT, 2);
        bufs(vs, sizeof(VertS), 3);
        float ms = frames(draw_tris, &vc, NULL);
        out("# gpu vertex pos s16x4 + color u8x4 + tc s16x2 (16-byte stride): %.2f ns per vertex\n", ms * 1e6f / kVerts);
    }

    // Fill: a full-screen quad (two triangles), several layers a frame.
    Vert* quad = linearAlloc(sizeof(Vert) * 6);
    C3D_Tex small, big, big565, bigEtc;
    bool texOk = quad && C3D_TexInit(&small, 64, 64, GPU_RGBA8) && C3D_TexInit(&big, 1024, 1024, GPU_RGBA8) &&
                 C3D_TexInit(&big565, 1024, 1024, GPU_RGB565) && C3D_TexInit(&bigEtc, 1024, 1024, GPU_ETC1);
    if (quad && texOk) {
        // u, v over 0..1 on the screen: the 64x64 texture is magnified (few texels, cache-friendly); the 1024x1024 ones
        // are minified ~2.5x without mipmaps (texels far apart: the texture cache misses).
        static const float kQ[6][4] = {{-1, -1, 0, 0}, {1, -1, 1, 0}, {1, 1, 1, 1}, {-1, -1, 0, 0}, {1, 1, 1, 1}, {-1, 1, 0, 1}};
        for (int i = 0; i < 6; i++) quad[i] = (Vert){kQ[i][0], kQ[i][1], 0.5f, 200, 100, 50, 128, kQ[i][2], kQ[i][3]};
        GSPGPU_FlushDataCache(quad, sizeof(Vert) * 6);
        memset(small.data, 0x7F, small.size);
        memset(big.data, 0x5A, big.size);
        memset(big565.data, 0x3C, big565.size);
        memset(bigEtc.data, 0x21, bigEtc.size);
        C3D_TexFlush(&small);
        C3D_TexFlush(&big);
        C3D_TexFlush(&big565);
        C3D_TexFlush(&bigEtc);
        attrs_float(3);
        bufs(quad, sizeof(Vert), 3);
        const int kLayers = 16;
        FCtx fc = {kLayers};
        const float pixels = 400.0f * 240.0f * kLayers;
        struct { const char* name; int mode; C3D_Tex* tex; int stages; } kC[] = {
            {"plain", 0, NULL, 1},           {"alpha blend", 1, NULL, 1},      {"depth test + write", 2, NULL, 1},
            {"tex rgba8 64x64", 0, &small, 1}, {"tex rgba8 1024x1024 minified", 0, &big, 1},
            {"tex rgb565 1024x1024 minified", 0, &big565, 1}, {"tex etc1 1024x1024 minified", 0, &bigEtc, 1},
            {"combiner 3 stages", 0, NULL, 3}, {"combiner 6 stages", 0, NULL, 6},
        };
        for (unsigned k = 0; k < sizeof kC / sizeof kC[0]; k++) {
            out("# gpu: section fill %s\n", kC[k].name);
            C3D_DepthTest(kC[k].mode == 2, GPU_LEQUAL, kC[k].mode == 2 ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
            if (kC[k].mode == 1)
                C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
            else
                C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
            if (kC[k].tex) {
                C3D_TexBind(0, kC[k].tex);
                env_textured();
            } else
                env_stages(kC[k].stages);
            float ms = frames(draw_layers, &fc, NULL);
            out("# gpu fill %s: %.3f ns per pixel (%.0f Mpixel/s; %.2f ms for %d layers of 400x240)\n", kC[k].name,
                ms * 1e6f / pixels, pixels / (ms * 1e3f), ms, kLayers);
        }
        C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        env_color_only();

        // Draw calls: one small triangle each.
        out("# gpu: section draws\n");
        for (int change = 0; change < 2; change++) {
            DCtx dc = {1000, change != 0};
            float cpu = 0, ms = frames(draw_many, &dc, &cpu);
            out("# gpu draws 1000 one-triangle draws%s: GPU %.2f us per draw, CPU %.2f us per draw\n",
                change ? ", a combiner colour change before each" : "", ms * 1e3f / 1000, cpu * 1e3f / 1000);
        }
        C3D_TexDelete(&small);
        C3D_TexDelete(&big);
        C3D_TexDelete(&big565);
        C3D_TexDelete(&bigEtc);
    } else
        out("# gpu fill: cannot allocate the quad or textures\n");

    // Transfers: through citro3d's synchronous calls, which queue on its GX command queue and wait for completion (raw
    // GX_* calls outside a frame fill that queue until libctru panics: found in Azahar, 10-15). Timed with the tick.
    out("# gpu: section xfer\n");
    const u32 kBytes = 1u << 20;
    void* lin = linearAlloc(kBytes);
    void* vram = vramAlloc(kBytes);
    if (lin && vram) {
        memset(lin, 0x11, kBytes);
        GSPGPU_FlushDataCache(lin, kBytes);
        u64 best = ~0ull;
        for (int i = 0; i < 4; i++) {
            u64 t = svcGetSystemTick();
            C3D_SyncMemoryFill((u32*)vram, 0x12345678, (u32*)((u8*)vram + kBytes), BIT(0) | GX_FILL_32BIT_DEPTH, NULL, 0, NULL, 0);
            t = svcGetSystemTick() - t;
            if (t < best) best = t;
        }
        out("# gpu xfer memory fill 1 MB VRAM: %.0f MB/s\n", 1.0 / ((double)best / SYSCLOCK_ARM11));
        static const struct { const char* name; bool toVram; } kX[] = {{"texture copy 1 MB linear -> VRAM", true}, {"texture copy 1 MB VRAM -> linear", false}};
        for (int k = 0; k < 2; k++) {
            best = ~0ull;
            for (int i = 0; i < 4; i++) {
                // A raw copy in four 256 KB lines: the line width is 16 bits of 16-byte units (16,384 here), gap 0.
                const u32 kPart = 256u << 10;
                u64 t = svcGetSystemTick();
                for (u32 off = 0; off < kBytes; off += kPart) {
                    u8* src = (u8*)(kX[k].toVram ? lin : vram) + off;
                    u8* dst = (u8*)(kX[k].toVram ? vram : lin) + off;
                    C3D_SyncTextureCopy((u32*)src, GX_BUFFER_DIM(kPart >> 4, 0), (u32*)dst, GX_BUFFER_DIM(kPart >> 4, 0), kPart,
                                        GX_TRANSFER_RAW_COPY(1));
                }
                t = svcGetSystemTick() - t;
                if (t < best) best = t;
            }
            out("# gpu xfer %s: %.0f MB/s\n", kX[k].name, 1.0 / ((double)best / SYSCLOCK_ARM11));
        }
        // Display transfer: the probe's 400x240 RGBA8 target to an RGB8 linear framebuffer (what a present does).
        best = ~0ull;
        for (int i = 0; i < 4; i++) {
            u64 t = svcGetSystemTick();
            C3D_SyncDisplayTransfer((u32*)g_rt->frameBuf.colorBuf, GX_BUFFER_DIM(240, 400), (u32*)lin, GX_BUFFER_DIM(240, 400),
                                    GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8));
            t = svcGetSystemTick() - t;
            if (t < best) best = t;
        }
        out("# gpu xfer display transfer 400x240 rgba8 -> rgb8: %.3f ms\n", (double)best * 1000.0 / SYSCLOCK_ARM11);
    } else
        out("# gpu xfer: cannot allocate 1 MB of linear and VRAM\n");
    if (lin) linearFree(lin);
    if (vram) vramFree(vram);
    if (vf) linearFree(vf);
    if (vs) linearFree(vs);
    if (quad) linearFree(quad);
    shaderProgramFree(&g_prog);
    DVLB_Free(g_dvlb);
    C3D_RenderTargetDelete(g_rt);
    out("# gpu: probe end after %.1f s\n", (double)(svcGetSystemTick() - t0) / SYSCLOCK_ARM11);
    return g_buf;
}
