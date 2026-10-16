// GPU probe (run with --timing-probe only, after the CPU probe): micro-benchmarks of the New 2DS XL's PICA200 for the
// timing model (docs/research/TIMING_MODEL_PLAN.md, "GPU"). GPU time is citro3d's C3D_GetDrawingTime (the GPU's own
// frame time, from its completion interrupts); each case is drawn in several frames and the fastest is kept. Only in
// probe-only jobs: it rebinds citro3d state that gc3ds's renderer (gx/pica) assumes it owns, and no game runs after.
//   vertex FMT      60,000 vertices of zero-area triangles (nothing filled): ns per vertex by attribute format
//   fill CASE       full-screen layers at 400x240: ns per pixel (and Mpixel/s) plain, blended, depth-tested, textured
//                   (each with the target's centre pixel read back: zero means nothing was drawn and the number is void)
//                   by format and size (a large texture sampled across the screen misses the texture cache), and by
//                   the number of combiner stages
//   shader          the vertex case through the same shader with 32 and 64 more instructions (per instruction)
//   setup TILE      the screen covered once by TILExTILE-pixel tiles (triangle setup by triangle size)
//   draws CASE      1,000 one-triangle draws: GPU and CPU microseconds per draw, with and without a state change
//   xfer CASE       display transfer, memory fill, texture copy: MB/s
#include <3ds.h>
#include <citro3d.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include "gpuprobe_shbin.h"
#include "gpuprobe_n32_shbin.h"
#include "gpuprobe_n64_shbin.h"
#include "gxpass_shbin.h"  // the emulator's own vertex shader (gxpass_case)

static char g_buf[16 << 10];
static size_t g_len;
extern FILE* probe_live;  // timingprobe.c: lines also go out as they are measured
void** probe_chase_prepare(u32 bytes);         // timingprobe.c: a random pointer cycle over its scratch
double probe_chase_run(void*** p, u32 loads);  // ns per load
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

// Whether `draw` puts pixels on the target: cleared to zero, drawn once, copied to linear memory, centre pixel read.
// Job 178's fill cases reported 15 Gpixel/s because every quad sat at clip z = +0.5, outside the PICA200's 0 >= z >= -w,
// and nothing was drawn; a number is only kept beside this check now.
static u32 centre_pixel(DrawFn draw, void* ctx) {
    u32* lin = linearAlloc(240 * 400 * 4);
    if (!lin) return 0xDEADBEEF;
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
    C3D_RenderTargetClear(g_rt, C3D_CLEAR_ALL, 0, 0);
    C3D_FrameDrawOn(g_rt);
    draw(ctx);
    C3D_FrameEnd(0);
    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);  // waits for that frame
    C3D_FrameEnd(0);
    memset(lin, 0, 240 * 400 * 4);
    GSPGPU_FlushDataCache(lin, 240 * 400 * 4);
    C3D_SyncDisplayTransfer((u32*)g_rt->frameBuf.colorBuf, GX_BUFFER_DIM(240, 400), lin, GX_BUFFER_DIM(240, 400),
                            GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8));
    GSPGPU_InvalidateDataCache(lin, 240 * 400 * 4);
    u32 px = lin[200 * 240 + 120];
    linearFree(lin);
    return px;
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

// The emulator's own vertex shader (src/gx/pica/gxpass.v.pica) with the backend's vertex (vfast.h PVert, 68 bytes,
// seven attributes as pica.c loads them), over zero-area triangles: what moving TP's vertex work onto the GPU would
// cost per vertex (MEASUREMENTS.md gap: the GX lighting shader at 44-79k vertices a frame). Modes by the lon uniform:
// clip position from the CPU; transform in the shader; transform plus the eight-light lighting path.
typedef struct { float pos[4]; u8 col[4]; float tc[3][2]; float eye[3], nrm[3]; } ProbePVert;
static void gxpass_case(VCtx* vc, int nverts) {
    DVLB_s* d = DVLB_ParseFile((u32*)gxpass_shbin, gxpass_shbin_size);
    ProbePVert* v = linearAlloc(sizeof(ProbePVert) * (size_t)nverts);
    if (!d || !v) { out("# gpu gxpass: cannot load the shader or the vertices\n"); if (d) DVLB_Free(d); if (v) linearFree(v); return; }
    for (int i = 0; i < nverts; i++) {
        float x = (float)((i / 3) % 200) / 100.0f - 1.0f;
        v[i] = (ProbePVert){{x, 0.0f, -0.5f, 0.0f}, {255, 255, 255, 255}, {{0, 0}, {0, 0}, {0, 0}}, {x, 0.0f, -0.5f}, {0.0f, 0.0f, 1.0f}};
    }
    GSPGPU_FlushDataCache(v, sizeof(ProbePVert) * (size_t)nverts);
    shaderProgram_s sp;
    shaderProgramInit(&sp);
    shaderProgramSetVsh(&sp, &d->DVLE[0]);
    C3D_BindProgram(&sp);
    shaderInstance_s* vs = sp.vertexShader;
    int uOn = shaderInstanceGetUniformLocation(vs, "lon"), uProj = shaderInstanceGetUniformLocation(vs, "proj");
    int uPal = shaderInstanceGetUniformLocation(vs, "pal"), uLights = shaderInstanceGetUniformLocation(vs, "lights");
    int uSel = shaderInstanceGetUniformLocation(vs, "lsel");
    C3D_AttrInfo* ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 4);
    AttrInfo_AddLoader(ai, 1, GPU_UNSIGNED_BYTE, 4);
    AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 2);
    AttrInfo_AddLoader(ai, 3, GPU_FLOAT, 2);
    AttrInfo_AddLoader(ai, 4, GPU_FLOAT, 2);
    AttrInfo_AddLoader(ai, 5, GPU_FLOAT, 3);
    AttrInfo_AddLoader(ai, 6, GPU_FLOAT, 3);
    C3D_BufInfo* bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, v, sizeof(ProbePVert), 7, 0x6543210);
    for (int r = 0; uProj >= 0 && r < 4; r++)  // identity projection and palette slot 0
        C3D_FVUnifSet(GPU_VERTEX_SHADER, uProj + r, r == 0, r == 1, r == 2, r == 3);
    for (int r = 0; uPal >= 0 && r < 3; r++) C3D_FVUnifSet(GPU_VERTEX_SHADER, uPal + r, r == 0, r == 1, r == 2, 0);
    for (int r = 0; uLights >= 0 && r < 40; r++) C3D_FVUnifSet(GPU_VERTEX_SHADER, uLights + r, 0.5f, 0.5f, 0.5f, 1.0f);
    for (int r = 0; uSel >= 0 && r < 4; r++) C3D_FVUnifSet(GPU_VERTEX_SHADER, uSel + r, 1, 1, 1, 1);  // every light selected
    static const struct { const char* name; float on[4]; } kM[] = {
        {"clip position from the CPU", {0, 0, 0, 0}}, {"transform in the shader", {0, 0, 0, 1}},
        {"transform + lighting, 8 lights selected", {1, 0, 1, 1}}};
    for (int k = 0; uOn >= 0 && k < 3; k++) {
        C3D_FVUnifSet(GPU_VERTEX_SHADER, uOn, kM[k].on[0], kM[k].on[1], kM[k].on[2], kM[k].on[3]);
        float ms = frames(draw_tris, vc, NULL);
        out("# gpu gxpass %s (68-byte PVert, 7 attributes): %.2f ns per vertex (%.2f ms for %d)\n", kM[k].name,
            ms * 1e6f / (float)nverts, ms, nverts);
    }
    C3D_BindProgram(&g_prog);  // frames() ended on a finished frame: the GPU no longer reads sp or v
    shaderProgramFree(&sp);
    DVLB_Free(d);
    linearFree(v);
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
            vf[i] = (Vert){x, 0.0f, -0.5f, 255, 255, 255, 255, 0.0f, 0.0f};
            vs[i] = (VertS){(s16)(x * 100), 0, -50, 100, 255, 255, 255, 255, 0, 0};
        }
        GSPGPU_FlushDataCache(vf, sizeof(Vert) * kVerts);
        GSPGPU_FlushDataCache(vs, sizeof(VertS) * kVerts);
        VCtx vc = {kVerts};
        // Each format packed at exactly its own size: on the device, a stride wider than the attributes loaded (24
        // bytes holding 16 bytes of attributes) hung the GPU (job 177; Azahar had accepted it).
        static const struct { const char* name; int nattr, stride; } kF[] = {
            {"pos float3", 1, 12}, {"pos float3 + color u8x4", 2, 16}, {"pos float3 + color u8x4 + tc float2", 3, 24}};
        u8* packed = linearAlloc(sizeof(Vert) * kVerts);
        for (int k = 0; packed && k < 3; k++) {
            for (int i = 0; i < kVerts; i++) memcpy(packed + (size_t)i * kF[k].stride, &vf[i], kF[k].stride);
            GSPGPU_FlushDataCache(packed, (u32)kF[k].stride * kVerts);
            attrs_float(kF[k].nattr);
            bufs(packed, kF[k].stride, kF[k].nattr);
            float ms = frames(draw_tris, &vc, NULL);
            out("# gpu vertex %s (%d-byte stride): %.2f ns per vertex (%.2f ms for %d)\n", kF[k].name, kF[k].stride,
                ms * 1e6f / kVerts, ms, kVerts);
        }
        if (packed) {
            // Shader length: the 12-byte format through the same shader with 32 and 64 more instructions.
            for (int i = 0; i < kVerts; i++) memcpy(packed + (size_t)i * 12, &vf[i], 12);
            GSPGPU_FlushDataCache(packed, 12u * kVerts);
            attrs_float(1);
            bufs(packed, 12, 1);
            const struct { const char* name; const u8* bin; u32 size; } kS[] = {
                {"+32 instructions", gpuprobe_n32_shbin, gpuprobe_n32_shbin_size},
                {"+64 instructions", gpuprobe_n64_shbin, gpuprobe_n64_shbin_size}};
            for (int k = 0; k < 2; k++) {
                DVLB_s* d = DVLB_ParseFile((u32*)kS[k].bin, kS[k].size);
                if (!d) continue;
                shaderProgram_s sp;
                shaderProgramInit(&sp);
                shaderProgramSetVsh(&sp, &d->DVLE[0]);
                C3D_BindProgram(&sp);
                float ms = frames(draw_tris, &vc, NULL);
                out("# gpu shader %s (pos float3, 12-byte stride): %.2f ns per vertex\n", kS[k].name, ms * 1e6f / kVerts);
                C3D_BindProgram(&g_prog);  // frames() ended on a finished frame: the GPU no longer reads sp
                shaderProgramFree(&sp);
                DVLB_Free(d);
            }
            linearFree(packed);
        }
        gxpass_case(&vc, kVerts);
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
    C3D_Tex small, mid, big, big565, bigEtc;
    bool texOk = quad && C3D_TexInit(&small, 64, 64, GPU_RGBA8) && C3D_TexInit(&mid, 256, 256, GPU_RGBA8) &&
                 C3D_TexInit(&big, 1024, 1024, GPU_RGBA8) &&
                 C3D_TexInit(&big565, 1024, 1024, GPU_RGB565) && C3D_TexInit(&bigEtc, 1024, 1024, GPU_ETC1);
    if (quad && texOk) {
        // u, v over 0..1 on the screen: the 64x64 texture is magnified (few texels, cache-friendly); the 1024x1024 ones
        // are minified ~2.5x without mipmaps (texels far apart: the texture cache misses).
        static const float kQ[6][4] = {{-1, -1, 0, 0}, {1, -1, 1, 0}, {1, 1, 1, 1}, {-1, -1, 0, 0}, {1, 1, 1, 1}, {-1, 1, 0, 1}};
        for (int i = 0; i < 6; i++) quad[i] = (Vert){kQ[i][0], kQ[i][1], -0.5f, 200, 100, 50, 128, kQ[i][2], kQ[i][3]};
        GSPGPU_FlushDataCache(quad, sizeof(Vert) * 6);
        memset(small.data, 0x7F, small.size);
        memset(mid.data, 0x66, mid.size);
        memset(big.data, 0x5A, big.size);
        memset(big565.data, 0x3C, big565.size);
        memset(bigEtc.data, 0x21, bigEtc.size);
        C3D_TexFlush(&small);
        C3D_TexFlush(&mid);
        C3D_TexFlush(&big);
        C3D_TexFlush(&big565);
        C3D_TexFlush(&bigEtc);
        attrs_float(3);
        bufs(quad, sizeof(Vert), 3);
        const int kLayers = 16;
        FCtx fc = {kLayers};
        const float pixels = 400.0f * 240.0f * kLayers;
        // span: the texture coordinates' range across the screen (1: the whole texture; 0.39: about one texel per
        // pixel for 1024 texels over 400 pixels), for the texture cache's behaviour by texels per pixel.
        struct { const char* name; int mode; C3D_Tex* tex; int stages; float span; } kC[] = {
            {"plain", 0, NULL, 1, 1},           {"alpha blend", 1, NULL, 1, 1},      {"depth test + write", 2, NULL, 1, 1},
            {"depth test, all rejected (GREATER on equal z)", 3, NULL, 1, 1},
            {"tex rgba8 64x64", 0, &small, 1, 1}, {"tex rgba8 256x256 (0.64 texel per pixel)", 0, &mid, 1, 1},
            {"tex rgba8 1024x1024 minified", 0, &big, 1, 1},
            {"tex rgb565 1024x1024 minified", 0, &big565, 1, 1}, {"tex etc1 1024x1024 minified", 0, &bigEtc, 1, 1},
            {"tex rgba8 1024 span 0.2 (0.5 texel per pixel)", 0, &big, 1, 0.2f},
            {"tex rgba8 1024 span 0.39 (1 texel per pixel)", 0, &big, 1, 0.39f},
            {"tex rgba8 1024 span 0.78 (2 texels per pixel)", 0, &big, 1, 0.78f},
            {"tex rgb565 1024 span 0.39 (1 texel per pixel)", 0, &big565, 1, 0.39f},
            {"tex etc1 1024 span 0.39 (1 texel per pixel)", 0, &bigEtc, 1, 0.39f},
            {"combiner 3 stages", 0, NULL, 3, 1}, {"combiner 6 stages", 0, NULL, 6, 1},
        };
        float span = 1;
        for (unsigned k = 0; k < sizeof kC / sizeof kC[0]; k++) {
            out("# gpu: section fill %s\n", kC[k].name);
            if (kC[k].span != span) {
                span = kC[k].span;
                for (int i = 0; i < 6; i++) {
                    quad[i].u = kQ[i][2] * span;
                    quad[i].v = kQ[i][3] * span;
                }
                GSPGPU_FlushDataCache(quad, sizeof(Vert) * 6);
            }
            // mode 3: the depth buffer holds the previous case's equal z, so GREATER rejects every fragment (early
            // depth rejection shows as a cost below the plain fill's).
            C3D_DepthTest(kC[k].mode >= 2, kC[k].mode == 3 ? GPU_GREATER : GPU_GEQUAL, kC[k].mode >= 2 ? GPU_WRITE_ALL : GPU_WRITE_COLOR);
            if (kC[k].mode == 1)
                C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA, GPU_SRC_ALPHA, GPU_ONE_MINUS_SRC_ALPHA);
            else
                C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
            if (kC[k].tex) {
                C3D_TexBind(0, kC[k].tex);
                env_textured();
            } else {
                // Unbound: a texture left bound keeps its unit fetching (job 179's combiner cases ran at exactly the
                // ETC1 case's 8.55 ns, the last texture bound).
                C3D_TexBind(0, NULL);
                env_stages(kC[k].stages);
            }
            float ms = frames(draw_layers, &fc, NULL);
            FCtx one = {1};
            u32 px = centre_pixel(draw_layers, &one);
            out("# gpu fill %s: %.3f ns per pixel (%.0f Mpixel/s; %.2f ms for %d layers of 400x240), centre pixel %08lx%s\n",
                kC[k].name, ms * 1e6f / pixels, pixels / (ms * 1e3f), ms, kLayers, (unsigned long)px,
                px ? "" : " NOT DRAWN: the number is void");
        }
        C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
        C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
        env_color_only();

        // Draw calls: one small triangle each.
        // Contention: core 0's pointer chase (timingprobe.c's scratch) while the GPU samples a large texture, the
        // texture in FCRAM (the GPU's reads share the bus with the CPU's) and then in VRAM. The draws (~270 ms) are
        // submitted without waiting, the chase runs meanwhile, then the frame is waited for.
        out("# gpu: section contention\n");
        {
            C3D_Tex bigV;
            bool vramOk = C3D_TexInitVRAM(&bigV, 1024, 1024, GPU_RGBA8);
            if (vramOk) {  // in 256 KB lines: a line's width is 16 bits of 16-byte units
                const u32 kPart = 256u << 10;
                for (u32 off = 0; off + kPart <= big.size; off += kPart)
                    C3D_SyncTextureCopy((u32*)((u8*)big.data + off), GX_BUFFER_DIM(kPart >> 4, 0), (u32*)((u8*)bigV.data + off),
                                        GX_BUFFER_DIM(kPart >> 4, 0), kPart, GX_TRANSFER_RAW_COPY(1));
            }
            for (int i = 0; i < 6; i++) {
                quad[i].u = kQ[i][2];
                quad[i].v = kQ[i][3];
            }
            GSPGPU_FlushDataCache(quad, sizeof(Vert) * 6);
            C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, GPU_ONE, GPU_ZERO, GPU_ONE, GPU_ZERO);
            env_textured();
            static const u32 kChase[] = {1u << 20, 8u << 20};
            for (int k = 0; k < 2; k++) {
                void** p = probe_chase_prepare(kChase[k]);
                if (!p) { out("# gpu contention: no chase scratch\n"); break; }
                double idle = probe_chase_run(&p, 200000);
                for (int where = 0; where < 2; where++) {
                    if (where == 1 && !vramOk) break;
                    C3D_TexBind(0, where ? &bigV : &big);
                    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);
                    C3D_FrameDrawOn(g_rt);
                    for (int l = 0; l < 48; l++) C3D_DrawArrays(GPU_TRIANGLES, 0, 6);
                    C3D_FrameEnd(0);
                    double busy = probe_chase_run(&p, 200000);
                    C3D_FrameBegin(C3D_FRAME_SYNCDRAW);  // the GPU's frame done
                    C3D_FrameEnd(0);
                    out("# gpu contention chase %luK: %.1f ns per load idle, %.1f ns while the GPU samples a 1024x1024 RGBA8 texture in %s\n",
                        (unsigned long)(kChase[k] >> 10), idle, busy, where ? "VRAM" : "FCRAM");
                }
            }
            if (vramOk) C3D_TexDelete(&bigV);
            C3D_TexBind(0, NULL);
            env_color_only();
        }

        out("# gpu: section setup\n");
        C3D_TexBind(0, NULL);
        Vert* grid = linearAlloc(sizeof(Vert) * (400 / 4) * (240 / 4) * 6);
        static const int kTile[] = {32, 16, 8, 4};
        for (int t = 0; grid && t < 4; t++) {
            int nx = 400 / kTile[t], ny = 240 / kTile[t], nv = 0;
            for (int y = 0; y < ny; y++)
                for (int x = 0; x < nx; x++) {
                    float x0 = 2.0f * x / nx - 1, x1 = 2.0f * (x + 1) / nx - 1, y0 = 2.0f * y / ny - 1, y1 = 2.0f * (y + 1) / ny - 1;
                    const float c[6][2] = {{x0, y0}, {x1, y0}, {x1, y1}, {x0, y0}, {x1, y1}, {x0, y1}};
                    for (int i = 0; i < 6; i++) grid[nv++] = (Vert){c[i][0], c[i][1], -0.5f, 200, 100, 50, 255, 0, 0};
                }
            GSPGPU_FlushDataCache(grid, sizeof(Vert) * nv);
            bufs(grid, sizeof(Vert), 3);
            VCtx gc = {nv};
            float ms = frames(draw_tris, &gc, NULL);
            out("# gpu setup %dx%d-pixel tiles: %d triangles of %d pixels covering the screen once, %.3f ms, %.1f ns per "
                "triangle, %.1f ns per vertex\n", kTile[t], kTile[t], nv / 3, kTile[t] * kTile[t] / 2, ms, ms * 1e6f / (nv / 3),
                ms * 1e6f / nv);
        }
        if (grid) linearFree(grid);
        bufs(quad, sizeof(Vert), 3);

        out("# gpu: section draws\n");
        // A triangle of ~32 pixels: the quad's first triangle, now actually drawn, would fill half the screen per draw.
        static const float kT[3][2] = {{0, 0}, {0.04f, 0}, {0, 0.04f}};
        for (int i = 0; i < 3; i++) quad[i] = (Vert){kT[i][0], kT[i][1], -0.5f, 200, 100, 50, 255, 0, 0};
        GSPGPU_FlushDataCache(quad, sizeof(Vert) * 3);
        for (int change = 0; change < 2; change++) {
            DCtx dc = {1000, change != 0};
            float cpu = 0, ms = frames(draw_many, &dc, &cpu);
            out("# gpu draws 1000 one-triangle draws%s: GPU %.2f us per draw, CPU %.2f us per draw\n",
                change ? ", a combiner colour change before each" : "", ms * 1e3f / 1000, cpu * 1e3f / 1000);
        }
        C3D_TexDelete(&small);
        C3D_TexDelete(&mid);
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
