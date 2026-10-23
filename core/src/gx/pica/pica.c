// GX -> PICA200 backend (3DS, citro3d). Plan and measurements: docs/GPU_BACKEND_PLAN.md, docs/FINDINGS.md.
//
// Frame model. citro3d queues every piece of GPU work (command lists, memory fills, transfers) and runs the queue
// only at C3D_FrameEnd. A frame here begins lazily at the first GX work after a submit and ends at present, or when
// the command buffer or the vertex arena fills. Transfers that must happen between draws (EFB clears and copies) are
// preceded by C3D_FrameSplit, so the queue holds them in stream order. C3D_FrameBegin waits for the previous submit,
// so the vertex arena and textures retired during a frame are reused only once the GPU is idle.
//
// EFB. A 512x256 RGBA8 texture with a D24S8 depth buffer in VRAM: the GameCube's 640x528 EFB scaled by 0.625 x 0.5,
// so a 640x480 frame lands 1:1 on the 400x240 top screen. Rows past 512 (PAL 528-line EFBs) are cut. Vertices go
// through gx/vfast.c (checked against the reference gx/xform.c): positions mostly transformed by the vertex shader
// from a palette of the draw's matrices (GPU transform), else on the CPU straight into PICA clip space; lighting on the
// CPU (the vertex worker) by default. The hardware clips (GX viewport and depth range included); CPU-transformed draws
// are culled on the CPU with the reference renderer's rule. Consecutive draws with the same GPU state are batched, and
// draws are merged into one vertex job where nothing changes between them.
//
// Copies. EFB -> XFB copies the render target into one of three XFB textures tagged with the destination address;
// present shows the one the VI is scanning out ("virtual XFB": the XFB in RAM is not written). EFB -> texture renders
// the source rectangle into a GPU texture registered under the destination address, used in place of decoding RAM.
//
// Textures: decoded with the game's mip levels, in the smallest PICA format that holds them (CMPR / RGB5A3 rounded to
// 16 bits), into VRAM by DMA while it has room (decode).
//
// Not yet: fog, indirect texturing, depth copies (white), partial clears (whole EFB cleared); Z textures only as a
// uniform depth (ztex_value).
#include <3ds.h>
#define GC3DS_HOST_TYPES
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../sys/system.h"
#include "../tev_pica.h"
#include "../vfast.h"
#include "gxpass_native_shbin.h"  // the emulator's shader plus the converted-shape path (gen_gxpass.sh native)
#include "pica.h"
#include "../../../host3ds/crashlog.h"

#define TW 512
#define TH 256
#define SX (400.0f / 640.0f)
#define SY (240.0f / 480.0f)
// Whether NDC y = +1 lands on texture row 0 of a render-target texture. Fixed by the Azahar check: with the wrong
// value every frame is upside down.
#define FLIP_Y 0
// The GPU command buffer: a frame that fills it past g_cmdbufLimit is ended early, and the next C3D_FrameBegin waits
// for the GPU to finish everything. With 1 MB the heavy area did that ~6 times a second (job 448: ~2,000 draw calls of
// ~100 words a frame). 2 MB; --ab cmdbuf alternates the limit between 0.4 (as 1 MB) and 0.8.
#define CMDBUF_BYTES (2u << 20)
static volatile float g_cmdbufLimit = 0.8f;
void pica_set_cmdbuf_big(bool on) { g_cmdbufLimit = on ? 0.8f : 0.4f; }
bool pica_cmdbuf_big(void) { return g_cmdbufLimit > 0.5f; }
#define ARENA_VERTS ((6u << 20) / sizeof(PVert))
#define IDX_MAX (512u << 10)  // u16 indices in the index arena (1 MB)
// What a frame may use of the two arenas: all of them, or with overlapped frames (--dblframe, below) one half each.
static u32 g_arenaCap = ARENA_VERTS, g_idxCap = IDX_MAX;
#ifndef VBASE_SPAN
#define VBASE_SPAN 0xFFFFu    // vertices a u16 index reaches from the vertex buffer's start
#endif
#define TEX_BUDGET (12u << 20)
#define MAX_TEX 384
#define MAX_GRAVE 128
#define MAX_COPIES 32
#define MAX_XFB 3
// Entries a frame may queue (C3D_FrameEnd adds its own): of citro3d's 32, or with --qbig of the 128 the queue is given
// at a frame's start (citro3d sets maxEntries = 32 in C3D_Init and never reads it again; libctru's queue functions
// bound by it). A frame that runs out is ended and a new one begun, and C3D_FrameBegin then waits for the GPU to finish
// everything: the heavy area did that ~5 times a second (10.9 submits against 5.8 pictures, job 440).
#define GXQUEUE_BUDGET g_qBudget
#define QBIG_ENTRIES 128
static u32 g_qBudget = 26;
// Frames ended before their picture, by cause: [0] GX queue entries, [1] vertex arena, [2] command buffer, [3] index
// buffer, [4] texture graveyard, [5] VRAM staging buffers, [6] converted-shape graveyard, [7] converted shapes' commands.
u64 g_endWhy[8];
static volatile bool g_qBigWant;  // pica_set_qbig
static bool g_qBigDone;
void pica_set_qbig(bool on) { g_qBigWant = on; }
bool pica_qbig(void) { return g_qBigWant; }
static gxCmdQueue_s* g_q;  // (citro3d's GX queue, found at the first splits: below)
#define PAL_SLOTS 14       // position matrices in the shader's palette (pal[42] in gen_gxpass.sh)

#define DISPLAY_TRANSFER_FLAGS (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
    GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
    GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))


typedef struct {         // a texture decoded from RAM
    u32 addr, fmt, w, h, tlutAddr, tlutFmt, dataHash, tlutHash, texVersion, tlutVersion, lastUse, bytes;
    u32 levels;  // mip levels decoded (1: level 0 only)
    C3D_Tex tex;
    bool live;
} PTex;

typedef struct {         // a GPU-side EFB copy standing in for the texture the copy would have written to RAM
    u32 addr, w, h, ramHash, ramBytes, texVersion;
    C3D_Tex tex;
    C3D_RenderTarget* rt;
    bool live, used;
} PCopy;

typedef struct { u32 addr, x0, y0, w, h, seq; C3D_Tex tex; } PXfb;

static struct {
    bool ready, frame, resetPending, stateValid, tevValid;
    shaderProgram_s prog;
    DVLB_s* dvlb;
    C3D_Tex efbTex, white;
    // The EFB's D24S8 depth buffer read as an RGBA8 texture (same tiling): A = depth bits 0-7, B = 8-15, G = 16-23,
    // R = stencil. Depth copies sample it directly.
    C3D_Tex efbDepthTex;
    C3D_RenderTarget* efb;
    C3D_RenderTarget* screen;
    PVert* arena;
    // Indexed triangle draws (pica_set_indexed): each draw's vertices once at a fixed arena place and its triangles as
    // u16 indices, relative to vbase (the vertex buffer's start, set_vbase); culled triangles repeat one vertex.
    u16* idx;
    u32 idxUsed, idxFlushed, vbase, batchIdxFirst;
    bool batchIdx;
    u32 arenaUsed, arenaFlushed, splitArena;  // splitArena: arenaUsed at the last split (split_early)
    PTex tex[MAX_TEX];
    u32 texBytes, frameNo;
    C3D_Tex grave[MAX_GRAVE];
    u32 graveFrame[MAX_GRAVE];  // the frame each was retired in: freed once the GPU has finished it (free_done)
    u32 nGrave;
    PCopy copies[MAX_COPIES];
    PXfb xfb[MAX_XFB];
    u32 xfbSeq;
    u32 tevKey[72], fragKey[8];
    PicaTev tev;
    PTex* lastHit[8];  // per GX texture map: the entry its last lookup found
    u32 approxSeen, queued;  // queued: GX queue entries added this frame
    // Draw batching: GX draws whose state key matches the open batch append their triangles to it, and one
    // C3D_DrawArrays covers them all (Twilight Princess sends thousands of 4-6 vertex draws per field).
    bool batchOpen;
    u32 batchFirst, batchKeyLen, batchKey[96];
    u32 batchBpVer, batchTlutVer, batchLightVer, batchXfbSeq;  // what the key was made from (pica_draw's fast path)
    u32 batchCls;  // primitive class and transform of the open batch (pica_draw's cls)
    VSel sel;
    // Lighting in the vertex shader (gxpass.v.pica): on unless pica_set_shader_light(false); the open batch's
    // decision and uniforms; uniform locations.
    bool shaderLight, batchGpuLight;
    VLightUniforms lu;
    s8 uLights, uSel, uMat, uAmb, uVsel, uOn;
    // Position transform in the vertex shader (gxpass.v.pica, lon.w): on unless pica_set_gpu_transform(false). A batch
    // is all GPU-transformed or all CPU-transformed (batchGpuPos); GPU-transformed batches are culled by the PICA
    // (batchCull). The shader's projection and matrix palette as uploaded; a palette slot whose palUse is palSerial is
    // used by the open sub-batch (the batch's vertices since batchFirst, drawn by flush_sub) and is not replaced.
    bool gpuPos, batchGpuPos, projValid;
    GPU_CULLMODE batchCull;
    s8 uProj, uPal;
    float proj[16];
    float pal[PAL_SLOTS][12];
    u32 palUse[PAL_SLOTS], palSerial, palNext, palLast, palValid;
    // The last one-matrix job (pal_assign's fast path): its raw matrix and origin, and the slot it got while that slot
    // still holds it (palGen counts each slot's uploads).
    u32 palGen[PAL_SLOTS], lastSlot, lastGen;
    float lastRaw[12], lastOrigin[3];
    bool lastValid;
    // Converted shapes (below): the attribute layout is a converted shape's (attrs_regular restores the arena's); the
    // shader's nat and lcount uniforms; palette rows 0-38 as converted draws uploaded them (natRowValid: bit per row).
    bool natAttr;
    s8 uNat, uLcount;
    u64 natRowValid;
    float natRow[39][4];
    u32 natProjVer;  // xfRegVersion of the projection in g.proj (converted draws)
    bool natProjOk;
} g;

PicaStats g_picaStats;
static volatile u32 g_texDivWant = 1;  // --texdiv (measurement, decode below)
static u32 g_texDiv = 1;
// Mipmaps (--mips): textures whose GX filter samples mip levels get the levels the game stores after level 0 (decoded
// from RAM like level 0), so minified textures hit the PICA's texture cache. Without them the heavy Ordon area's GPU
// took ~100 ms a frame (job 386); textures at 1/4 size took 28 ms (+38% speed, job 387).
// --gpudiag (measurement only, wrong pictures): bits that take GPU work away to find what the GPU's time is spent on.
// 1 EFB -> texture copies not drawn (the texture keeps what it had); 2 blending off (logic op copy); 4 depth test off;
// 8 every game draw scissored to 16x16 pixels (what is left is vertex and setup work); 16 XFB copies not made;
// 32 EFB -> texture copy and present quads scissored to 16x16. (Bit 1 froze the 2DS within 7 s, job 403.)
static volatile int g_shlightWant = -1;  // pica_want_shader_light: applied by this thread at the next frame (-1: none)
void pica_want_shader_light(bool on) { g_shlightWant = on; }
void pica_set_shader_light(bool on);
static volatile bool g_deflitWant;  // pica_set_deflit: g_vfDeferCpu (vfast.c) from the next frame
void pica_set_deflit(bool on) { g_deflitWant = on; }
bool pica_deflit(void) { return g_deflitWant; }
static volatile u32 g_gpuDiagWant;
static u32 g_gpuDiag;
void pica_set_gpudiag(u32 bits) { g_gpuDiagWant = bits; }
u32 pica_gpudiag(void) { return g_gpuDiagWant; }
u64 g_copyPix[4];  // EFB -> texture copies: target texels, source EFB pixels (scaled); XFB copies; copies not drawn
// Textures in VRAM (--vramtex): the PICA samples linear FCRAM 2.5-3.6x slower, and its FCRAM texture reads double core
// 0's memory latency (job 192). Each new texture goes to VRAM while more than VRAM_RESERVE stays free (EFB copies
// allocate there too): tiled into a linear staging buffer, then DMA-copied in the GX queue ahead of the draws that use
// it (the CPU cannot write VRAM). Staging buffers are freed at the next frame, once the GPU has done the copy. With it on
// at start the XFBs go to FCRAM (1.5 MB of VRAM: sampled once per present).
#define VRAM_RESERVE (512u << 10)
#define MAX_STAGING 96
static volatile bool g_vramTexWant;
static bool g_vramTex;
static void* g_staging[MAX_STAGING];
static u32 g_stagingFrame[MAX_STAGING];  // the frame that queued each copy
static u32 g_nStaging;
u64 g_vramTexStats[3];  // textures put in VRAM, their bytes, textures left in FCRAM for want of VRAM
static volatile bool g_xfbFcramWant;  // pica_set_xfb_fcram: applied at the next frame (begin_frame)
static bool g_xfbFcram;
void pica_set_xfb_fcram(bool on) { g_xfbFcramWant = on; }
void pica_set_vramtex(bool on) { g_vramTexWant = on; }
bool pica_vramtex(void) { return g_vramTexWant; }
static volatile bool g_mipsWant;
static bool g_mips;
void pica_set_mips(bool on) { g_mipsWant = on; }
bool pica_mips(void) { return g_mipsWant; }
// Smaller texel formats (--smalltex, C17): each texture in the smallest PICA format that holds every texel of its level 0
// exactly (the decoder's 5- and 6-bit channels expand by bit replication, so the test is exact), else RGBA8 as before.
// A cache-missing texture is fetched at 27 Mpixel/s in RGB565 against 17 in RGBA8 (gpuprobe, job 180).
// Level 2 (lossy): CMPR and RGB5A3 sources (and palettes of RGB5A3) that are not exact in a small format go to RGB565
// when opaque, RGBA5551 with 1-bit alpha, else RGBA4 (CMPR's blended colours are never exactly 5-6-5: job 391 left 807 of
// 1,049 textures in RGBA8 at level 1); RGBA8 sources stay RGBA8.
static volatile u32 g_smallWant;
static u32 g_small;
void pica_set_smalltex(u32 level) { g_smallWant = level <= 2 ? level : 2; }
u32 pica_smalltex(void) { return g_smallWant; }
u64 g_texFmtCount[16];  // textures decoded per PICA format
u64 g_texRgba8By[16];   // of those left in RGBA8, by GX source format

static u8 g_morton[64];  // (defined below: the tile order)
static inline bool ex5(u32 v) { return (((v >> 3) << 3) | (v >> 5)) == v; }  // 8-bit value that a 5-bit one expands to
static inline bool ex6(u32 v) { return (((v >> 2) << 2) | (v >> 6)) == v; }
static inline bool ex4(u32 v) { return (v >> 4) * 17 == v; }

static GPU_TEXCOLOR small_format(const u32* p, u32 n, u32 gxFmt, u32 tlutFmt) {
    bool grey = true, opaque = true, a1 = true, c565 = true, c555 = true, c4 = true, l4 = true;
    for (u32 i = 0; i < n; i++) {
        u32 c = p[i], r = c >> 24, gg = (c >> 16) & 0xFF, b = (c >> 8) & 0xFF, a = c & 0xFF;
        grey &= r == gg && gg == b;
        opaque &= a == 255;
        a1 &= a == 0 || a == 255;
        c565 &= ex5(r) && ex6(gg) && ex5(b);
        c555 &= ex5(r) && ex5(gg) && ex5(b);
        c4 &= ex4(r) && ex4(gg) && ex4(b) && ex4(a);
        l4 &= ex4(r) && ex4(a);
    }
    if (grey) {
        if (opaque) return GPU_L8;
        return l4 ? GPU_LA4 : GPU_LA8;  // (alpha equal to grey, as GX intensity textures, or its own)
    }
    if (opaque && c565) return GPU_RGB565;
    if (a1 && c555) return GPU_RGBA5551;
    if (c4) return GPU_RGBA4;
    bool pal = gxFmt == 8 || gxFmt == 9 || gxFmt == 10;
    if (g_small >= 2 && (gxFmt == 14 || gxFmt == 5 || (pal && tlutFmt == 2)))  // CMPR, RGB5A3, an RGB5A3 palette
        return opaque ? GPU_RGB565 : a1 ? GPU_RGBA5551 : GPU_RGBA4;
    if (opaque) return GPU_RGB8;  // (24 bits, exact)
    return GPU_RGBA8;
}

// One level in a small format: rgba (w x h, 0xRRGGBBAA) into W x H tiles at dst (bytes per texel bpp).
// Nearest resampling's source column for each of W target columns, floor(x * w / W), found by stepping (the ARM11 has no
// divide instruction: a division per texel cost ~100 cycles, ~30 ms for one 512 x 512 movie frame). W <= 1024.
static u16 g_cols[1024];
static const u16* col_map(u32 W, u32 w) {
    u32 q = 0, r = 0;
    for (u32 x = 0; x < W && x < 1024; x++) {
        g_cols[x] = (u16)q;
        for (r += w; r >= W; r -= W) q++;
    }
    return g_cols;
}

#define TEXEL_LOOP(BPP, BODY) do { \
    for (u32 y = 0; y < H; y++) { \
        const u32* row = rgba + ((H - 1 - y) * h / H) * w; \
        u8* tiles = dst + (y >> 3) * (W >> 3) * 64 * (BPP); \
        const u8* m = &g_morton[(y & 7) * 8]; \
        for (u32 x = 0; x < W; x++) { \
            u32 c = row[cols[x]], r = c >> 24, gg = (c >> 16) & 0xFF, b = (c >> 8) & 0xFF, a = c & 0xFF; \
            u8* o = tiles + ((x >> 3) * 64 + m[x & 7]) * (BPP); \
            (void)r; (void)gg; (void)b; (void)a; \
            BODY; \
        } \
    } \
} while (0)

static void upload_level_fmt(u8* dst, u32 W, u32 H, const u32* rgba, u32 w, u32 h, GPU_TEXCOLOR f) {
    const u16* cols = col_map(W, w);
    switch (f) {
    case GPU_L8: TEXEL_LOOP(1, o[0] = (u8)r); break;
    case GPU_RGB8: TEXEL_LOOP(3, (o[0] = (u8)b, o[1] = (u8)gg, o[2] = (u8)r)); break;
    case GPU_LA4: TEXEL_LOOP(1, o[0] = (u8)(((r >> 4) << 4) | (a >> 4))); break;
    case GPU_LA8: TEXEL_LOOP(2, (o[0] = (u8)a, o[1] = (u8)r)); break;
    case GPU_RGB565: TEXEL_LOOP(2, { u16 v = (u16)(((r >> 3) << 11) | ((gg >> 2) << 5) | (b >> 3)); memcpy(o, &v, 2); }); break;
    case GPU_RGBA5551: TEXEL_LOOP(2, { u16 v = (u16)(((r >> 3) << 11) | ((gg >> 3) << 6) | ((b >> 3) << 1) | (a >> 7)); memcpy(o, &v, 2); }); break;
    case GPU_RGBA4: TEXEL_LOOP(2, { u16 v = (u16)(((r >> 4) << 12) | ((gg >> 4) << 8) | ((b >> 4) << 4) | (a >> 4)); memcpy(o, &v, 2); }); break;
    default: TEXEL_LOOP(4, memcpy(o, &c, 4)); break;
    }
}
#undef TEXEL_LOOP
static u8 g_morton[64];
static SVert* g_sv;
static PVert* g_pv;
static u32 g_vcap;
static u32* g_scratch;
static u32 g_scratchCap;
static bool g_scratchLinear;

// The decode scratch (RGBA8, up to 1024 x 1024: 4 MB): from malloc, else from linear memory. A build with translated
// code (C12) left malloc ~2 MB, and a failed realloc here skipped the texture: the THP movie drew white and large
// textures as flat colours on the 2DS (jobs 135-141), whether or not the translated code ran.
static bool scratch_reserve(u32 n) {
    if (n <= g_scratchCap) return true;
    u32* p = g_scratchLinear ? NULL : realloc(g_scratch, n * 4);
    if (!p) {
        p = linearAlloc(n * 4);
        if (!p) return false;
        if (g_scratchLinear) linearFree(g_scratch); else free(g_scratch);
        g_scratchLinear = true;
    }
    g_scratch = p;
    g_scratchCap = n;
    return true;
}

// The vertex worker's queue (below): jobs queued / done. Each counter written by one core has a 32-byte cache line of
// its own: side by side, the GX thread's head and the worker's tail moved one line between cores 2 and 3 on every job
// (ARM11 MPCore: 32-byte lines, the SCU migrates modified lines; REDESIGN_CANDIDATES C7).
typedef struct { volatile u32 v; } __attribute__((aligned(32))) VwLine;
static VwLine g_vwHeadL, g_vwTailL;
#define g_vwHead g_vwHeadL.v
#define g_vwTail g_vwTailL.v
// g_vwHead when the open batch (or its open sub-batch) began: the older jobs only feed draw calls already recorded,
// which is all an early split needs finished (split_early).
static u32 g_recJob;

// ---- lean state: citro3d calls that would upload what the GPU already holds ----
// citro3d uploads every block whose dirty bit is set at the next draw call (C3Di_UpdateContext), whether or not its
// values changed: every batch re-sent its cull mode (the whole effect block, 14 commands), its texture units, the
// shader-lighting uniforms (48 vectors) and, at each TEV change, all six combiner stages (job 360: ~8.5 us of the GX
// thread per batch in the draw call). With lean state on, a setter whose result equals citro3d's own copy of the state
// is skipped, or its dirty bit dropped again; citro3d's copy is what the GPU holds whenever that bit is clear.
// The context's leading fields, as citro3d 1.7.1 lays them out (source/internal.h); checked at start (lean_init).
typedef struct {
    u32 fragOpMode, fragOpShadow, zScale, zOffset;
    GPU_CULLMODE cullMode;
    bool zBuffer, earlyDepth;
    GPU_EARLYDEPTHFUNC earlyDepthFunc;
    u32 earlyDepthRef, alphaTest, stencilMode, stencilOp, depthTest, blendClr, alphaBlend;
    GPU_LOGICOP clrLogicOp;
} C3dEffect;
typedef struct {
    gxCmdQueue_s gxQueue;
    u32* cmdBuf;
    size_t cmdBufSize;
    float cmdBufUsage;
    u32 flags;
    shaderProgram_s* program;
    C3D_AttrInfo attrInfo;
    C3D_BufInfo bufInfo;
    C3dEffect effect;
    C3D_LightEnv* lightEnv;
    u32 texConfig, texShadow;
    C3D_Tex* tex[3];
    C3D_TexEnv texEnv[6];
    u32 texEnvBuf, texEnvBufClr, fogClr;
    void* fogLut;
    u16 gasAttn, gasAccMax;
    u32 gasLightXY, gasLightZ, gasLightZColor;
    u32 gasDeltaZ : 24;
    u32 gasFlags : 8;
    void* gasLut;
    void* procTex;
    void* procTexLut[3];
    void* procTexColorLut;
    C3D_FrameBuf fb;
    u32 viewport[5];
    u32 scissor[3];
    u16 fixedAttribDirty, fixedAttribEverDirty;
    C3D_FVec fixedAttribs[12];
} C3dCtx;
#define C3DF_EFFECT (1u << 4)
#define C3DF_TEXENVBUF (1u << 9)
#define C3DF_TEX(n) (1u << (23 + (n)))
#define C3DF_TEXENV(n) (1u << (26 + (n)))
extern u8 __C3D_Context[];
static C3dCtx* g_c3d;                // NULL until lean_init found the layout as above
static volatile u32 g_leanWant;  // pica_set_lean: LEAN_* bits (the GX thread reads it at each draw)
static u32 g_lean;
void pica_set_lean(u32 mask) { g_leanWant = mask & LEAN_ALL; }
u32 pica_lean(void) { return g_c3d ? g_leanWant : 0; }

// What a texture unit was last bound with (bind_tex): the registers citro3d uploads from the texture, and the texture
// epoch then. Anything that can change texture memory (a decode, an EFB copy, a clear, a new frame) starts a new epoch,
// and a bind in a new epoch always goes through: binding clears the PICA's texture cache, which must not keep texels
// of memory that changed under a texture still bound.
typedef struct { const C3D_Tex* t; void* data; u32 border, dim, param, lodParam, fmt, epoch; } TexBound;
static TexBound g_texBound[3];
static u32 g_texEpoch = 1;

static void lean_init(void) {
    C3dCtx* c = (C3dCtx*)(void*)__C3D_Context;
    u32 clr = c->texEnvBufClr;
    u16 fd = c->fixedAttribDirty, fe = c->fixedAttribEverDirty;
    C3D_TexEnvBufColor(0x5A3C1E0Fu);
    C3D_CullFace(GPU_CULL_BACK_CCW);
    C3D_SetScissor(GPU_SCISSOR_INVERT, 1, 2, 3, 4);
    bool ok = (c->flags & 1) && (void*)&c->attrInfo == (void*)C3D_GetAttrInfo() &&
              (void*)&c->bufInfo == (void*)C3D_GetBufInfo() && (void*)&c->texEnv[5] == (void*)C3D_GetTexEnv(5) &&
              c->texEnvBufClr == 0x5A3C1E0Fu && c->effect.cullMode == GPU_CULL_BACK_CCW &&
              (void*)&c->fb == (void*)C3D_GetFrameBuf() && c->scissor[0] == GPU_SCISSOR_INVERT &&
              (void*)&c->fixedAttribs[0] == (void*)C3D_FixedAttribGetWritePtr(0);
    c->fixedAttribDirty = fd;
    c->fixedAttribEverDirty = fe;
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    C3D_TexEnvBufColor(clr);
    C3D_CullFace(GPU_CULL_NONE);
    g_c3d = ok ? c : NULL;
    emu_log(LOG_GX, "pica: lean state %s (citro3d context layout %s)\n", ok ? "available" : "off", ok ? "as expected" : "differs");
}

static void cull_face(GPU_CULLMODE m) {
    if ((g_lean & LEAN_CULL) && g_c3d->effect.cullMode == m) return;
    C3D_CullFace(m);
}

static void bind_tex(int i, C3D_Tex* t) {
    TexBound* b = &g_texBound[i];
    TexBound now = { t, t->data, t->border, t->dim, t->param, t->lodParam, t->fmt, g_texEpoch };
    if ((g_lean & LEAN_TEX) && g_c3d->tex[i] == t && !memcmp(b, &now, sizeof now)) return;
    C3D_TexBind(i, t);
    *b = now;
}

static void unif_set(int id, float x, float y, float z, float w) {
    C3D_FVec v = FVec4_New(x, y, z, w);
    if ((g_lean & LEAN_UNIF) && !memcmp(&C3D_FVUnif[GPU_VERTEX_SHADER][id], &v, sizeof v)) return;
    *C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, id, 1) = v;
}

// n uniform vectors from rows (x, y, z, w each): compared as a block first (the lights rarely change between batches).
static void unif_rows(int id, const float (*rows)[4], int n) {
    if (g_lean & LEAN_UNIF) {  // bit for bit: -0 and +0, or two NaNs, are not the same upload
        const C3D_FVec* c = &C3D_FVUnif[GPU_VERTEX_SHADER][id];
        int i = 0;
        for (; i < n; i++) {
            C3D_FVec v = FVec4_New(rows[i][0], rows[i][1], rows[i][2], rows[i][3]);
            if (memcmp(&c[i], &v, sizeof v)) break;
        }
        if (i == n) return;
    }
    for (int i = 0; i < n; i++) unif_set(id + i, rows[i][0], rows[i][1], rows[i][2], rows[i][3]);
}

// Opens a group of effect setters (depth, alpha, blending) and closes it: the block's dirty bit is dropped again
// when it was clear before and the setters left every field as it was.
typedef struct { bool clean; C3dEffect e; } EffectGuard;
static void effect_guard_open(EffectGuard* gd) {
    gd->clean = (g_lean & LEAN_EFFECT) && !(g_c3d->flags & C3DF_EFFECT);
    if (gd->clean) memcpy(&gd->e, &g_c3d->effect, sizeof gd->e);
}
static void effect_guard_close(const EffectGuard* gd) {
    if (gd->clean && !memcmp(&gd->e, &g_c3d->effect, sizeof gd->e)) g_c3d->flags &= ~C3DF_EFFECT;
}

// The draw calls' uploads (perf.csv "# gx draw call 10s"): calls, command words they wrote, ticks in the call and
// calls timed (one in 8), effect blocks, texture units, combiner stages, combiner buffer blocks and other blocks
// (frame buffer, viewport, scissor, program, attributes, buffers) uploaded, float uniform vectors uploaded and their
// runs, ticks in flush_batch's wait for the vertex worker (timed calls).
u64 g_picaDc[12];
static u32 g_dcN, g_dcWords;
static u64 g_dcT0;
static bool g_dcTimed;

static void dc_begin(void) {
    u64* d = g_picaDc;
    d[0]++;
    if (g_c3d) {
        u32 f = g_c3d->flags;
        d[4] += (f & C3DF_EFFECT) != 0;
        d[5] += (u32)__builtin_popcount(f & (7u << 23));
        d[6] += (u32)__builtin_popcount(f & (0x3Fu << 26));
        d[7] += (f & C3DF_TEXENVBUF) != 0;
        d[8] += (f & 0x1E8u) != 0;  // AttrInfo, BufInfo, FrameBuf, Viewport, Scissor, Program
    }
    const bool* dirty = C3D_FVUnifDirty[GPU_VERTEX_SHADER];
    for (int i = 0; i < C3D_FVUNIF_COUNT; i++) {
        u32 w4;
        if (!(i & 3) && (memcpy(&w4, dirty + i, 4), !w4)) { i += 3; continue; }
        if (!dirty[i]) continue;
        d[10]++;
        while (i < C3D_FVUNIF_COUNT && dirty[i]) { d[9]++; i++; }
    }
    g_dcWords = gpuCmdBufOffset;
    g_dcTimed = !(++g_dcN & 7);
    if (g_dcTimed) g_dcT0 = svcGetSystemTick();
}

// Command buffer prefetch (--cmdpld): citro3d starts each frame at the buffer's start and the submit cleans and
// invalidates it for the GPU, so every 32-byte line a draw call writes is a miss to FCRAM (~14 per call, job 363-364:
// ~8.5 us a call whatever it uploads, raw or not). After each call the lines the next calls will write are prefetched,
// so they arrive during the draw's vertex work instead of inside the next call.
#define CMDPLD_AHEAD 1024u  // bytes past the write position (~2 calls)
static volatile bool g_cmdPldWant;
static u32* g_pldBuf;
static u32 g_pldTo;  // words of the buffer prefetched so far (this frame's buffer)
void pica_set_cmdpld(bool on) { g_cmdPldWant = on; }
bool pica_cmdpld(void) { return g_cmdPldWant; }

static void cmd_prefetch(void) {
    if (!g_cmdPldWant || !gpuCmdBuf) return;
    if (gpuCmdBuf != g_pldBuf || gpuCmdBufOffset + CMDPLD_AHEAD / 4 < g_pldTo) { g_pldBuf = gpuCmdBuf; g_pldTo = 0; }  // a split or a new frame
    u32 from = g_pldTo > gpuCmdBufOffset ? g_pldTo : gpuCmdBufOffset, to = gpuCmdBufOffset + CMDPLD_AHEAD / 4;
    if (to > gpuCmdBufSize) to = gpuCmdBufSize;
    for (u32 w = from & ~7u; w < to; w += 8) __builtin_prefetch(gpuCmdBuf + w, 1);
    if (to > g_pldTo) g_pldTo = to;
}

static void dc_end(void) {
    if (g_dcTimed) { g_picaDc[2] += (svcGetSystemTick() - g_dcT0) * 8; g_picaDc[3]++; }
    if (gpuCmdBufOffset >= g_dcWords) g_picaDc[1] += gpuCmdBufOffset - g_dcWords;
    cmd_prefetch();
}

// ---- frames ----
// Issues the open batch's triangles; call before anything changes GPU state or submits.
static void vw_sync(void);
static void vw_sync_all(void);

// The vertex buffer starts at arena vertex v: indices are u16, so indexed batches move the start along the arena.
static void set_vbase(u32 v) {
    if (v == g.vbase) return;
    C3D_BufInfo* bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, g.arena + v, sizeof(PVert), 7, 0x6543210);
    g.vbase = v;
}

// The arena's vertex layout (PVert: v0-v6), as begin_frame sets it.
static void attrs_arena(void) {
    C3D_AttrInfo* ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 4);
    AttrInfo_AddLoader(ai, 1, GPU_UNSIGNED_BYTE, 4);
    AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 2);
    AttrInfo_AddLoader(ai, 3, GPU_FLOAT, 2);
    AttrInfo_AddLoader(ai, 4, GPU_FLOAT, 2);
    AttrInfo_AddLoader(ai, 5, GPU_FLOAT, 3);  // eye-space position (shader lighting)
    AttrInfo_AddLoader(ai, 6, GPU_FLOAT, 3);  // eye-space normal
}

// After converted shapes (their own buffers and attribute layout, the shader's native path): the arena's again.
static void attrs_regular(void) {
    if (!g.natAttr) return;
    g.natAttr = false;
    attrs_arena();
    C3D_BufInfo* bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, g.arena + g.vbase, sizeof(PVert), 7, 0x6543210);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.uNat, 0, 0, 0, 0);
}

// ---- raw draw calls ----
// A batch's draw call without citro3d's fixed per-call work (gcbench: C3D_DrawArrays 3.25 us of CPU against 0.74 us
// for a raw register sequence, HANDOFF item 6): C3Di_UpdateContext runs only when citro3d has something to upload (it
// otherwise walks both shaders' 96 uniform flags for nothing), and the draw's own registers are one block copied into
// the command buffer instead of 11-15 GPUCMD calls. The blocks are citro3d 1.7.1's sequences word for word; the first
// draws and one in 4,096 after are also issued through citro3d and compared (g_rawStats).
void C3Di_UpdateContext(void);
#define CMDW(reg, mask, val) (u32)(val), GPUCMD_HEADER(0, (mask), (reg))
static const u32 kRawElements[30] = {  // C3D_DrawElements(GPU_TRIANGLES, n, C3D_UNSIGNED_SHORT, ...)
    CMDW(GPUREG_PRIMITIVE_CONFIG, 2, GPU_GEOMETRY_PRIM), CMDW(GPUREG_RESTART_PRIMITIVE, 0xF, 1),
    CMDW(GPUREG_INDEXBUFFER_CONFIG, 0xF, 0), CMDW(GPUREG_NUMVERTICES, 0xF, 0), CMDW(GPUREG_VERTEX_OFFSET, 0xF, 0),
    CMDW(GPUREG_GEOSTAGE_CONFIG, 2, 0x100), CMDW(GPUREG_GEOSTAGE_CONFIG2, 2, 0x100),
    CMDW(GPUREG_START_DRAW_FUNC0, 1, 0), CMDW(GPUREG_DRAWELEMENTS, 0xF, 1), CMDW(GPUREG_START_DRAW_FUNC0, 1, 1),
    CMDW(GPUREG_GEOSTAGE_CONFIG, 2, 0), CMDW(GPUREG_GEOSTAGE_CONFIG2, 2, 0), CMDW(GPUREG_VTX_FUNC, 0xF, 1),
    CMDW(GPUREG_PRIMITIVE_CONFIG, 8, 0), CMDW(GPUREG_PRIMITIVE_CONFIG, 8, 0),
};
static const u32 kRawArrays[22] = {  // C3D_DrawArrays(GPU_TRIANGLES, first, n)
    CMDW(GPUREG_PRIMITIVE_CONFIG, 2, GPU_TRIANGLES), CMDW(GPUREG_RESTART_PRIMITIVE, 0xF, 1),
    CMDW(GPUREG_INDEXBUFFER_CONFIG, 0xF, 0x80000000u), CMDW(GPUREG_NUMVERTICES, 0xF, 0), CMDW(GPUREG_VERTEX_OFFSET, 0xF, 0),
    CMDW(GPUREG_GEOSTAGE_CONFIG2, 1, 1), CMDW(GPUREG_START_DRAW_FUNC0, 1, 0), CMDW(GPUREG_DRAWARRAYS, 0xF, 1),
    CMDW(GPUREG_START_DRAW_FUNC0, 1, 1), CMDW(GPUREG_GEOSTAGE_CONFIG2, 1, 0), CMDW(GPUREG_VTX_FUNC, 0xF, 1),
};
static volatile bool g_rawWant;  // pica_set_rawdraw (off by default until measured)
static bool g_rawBad;            // a compared draw differed: raw draws stay off
static u32 g_idxPa, g_rawN;
u64 g_rawStats[3];  // draw calls issued raw, compared with citro3d's, compared and different
#define GW_ENTER(w) u8 gwSaved_ = g_gxWhere; g_gxWhere = (w)
#define GW_LEAVE() g_gxWhere = gwSaved_
void pica_set_rawdraw(bool on) { g_rawWant = on; }

void pica_watch(char* buf, u32 size) {
    snprintf(buf, size, "gx at %u (pica.h GW_), draw calls %llu, frame %d, vworker head %lu tail %lu, lean %d raw %d",
             (unsigned)g_gxWhere, (unsigned long long)g_picaStats.drawCalls, g.frame ? 1 : 0, (unsigned long)g_vwHead,
             (unsigned long)g_vwTail, (unsigned)g_lean, pica_rawdraw() ? 1 : 0);
}
bool pica_rawdraw(void) { return g_rawWant && g_c3d && !g_rawBad; }

// Whether C3Di_UpdateContext would upload anything: a dirty block (any flag but Active and DrawUsed), a fixed
// attribute, a light environment or gas (updated on every call), or a dirty uniform of either shader. Loading a
// program's shader constants always sets its bool uniforms dirty too (C3Di_LoadShaderUniforms).
static bool c3d_dirty(void) {
    const C3dCtx* c = g_c3d;
    if ((c->flags & ~3u) || c->fixedAttribDirty || c->lightEnv || (c->texEnvBuf & 7) == GPU_GAS) return true;
    u32 w[2 * C3D_FVUNIF_COUNT / 4], acc = 0;
    memcpy(w, C3D_FVUnifDirty, sizeof w);
    for (u32 i = 0; i < sizeof w / 4; i++) acc |= w[i];
    u32 iv[2 * C3D_IVUNIF_COUNT / 4];
    memcpy(iv, C3D_IVUnifDirty, sizeof iv);
    for (u32 i = 0; i < sizeof iv / 4; i++) acc |= iv[i];
    return acc || C3D_BoolUnifsDirty[0] || C3D_BoolUnifsDirty[1];
}

// Copies a draw block into the command buffer (as GPUCMD_Add: a full buffer is a panic, never an overrun).
static void raw_emit(const u32* w, u32 n) {
    if (!gpuCmdBuf || gpuCmdBufOffset + n > gpuCmdBufSize) svcBreak(USERBREAK_PANIC);
    memcpy(gpuCmdBuf + gpuCmdBufOffset, w, n * 4);
    gpuCmdBufOffset += n;
    g_c3d->flags |= 2;  // C3DiF_DrawUsed
}

// After citro3d issued the draw block itself: its last n words against ours.
static void raw_compare(u32 at, const u32* w, u32 n, const char* what) {
    g_rawStats[1]++;
    u32 got = gpuCmdBufOffset - at;
    if (got == n && !memcmp(gpuCmdBuf + at, w, n * 4)) return;
    g_rawStats[2]++;
    u32 i = 0;
    while (i < n && i < got && gpuCmdBuf[at + i] == w[i]) i++;
    if (!g_rawBad)
        emu_log(LOG_GX, "pica: raw %s draw differs from citro3d's (%u words written, %u expected; word %u: %08x, ours %08x): "
                "raw draws off\n", what, (unsigned)got, (unsigned)n, (unsigned)i, i < got ? (unsigned)gpuCmdBuf[at + i] : 0u,
                i < n ? (unsigned)w[i] : 0u);
    g_rawBad = true;
}

// ---- raw state ----
// The state blocks a batch changes, written by this file instead of C3Di_UpdateContext (--rawstate): the effect block,
// texture units, the combiner buffer, combiner stages and vertex-shader float uniforms, in citro3d 1.7.1's order and
// words (C3Di_UpdateContext, C3Di_EffectBind, C3Di_SetTex, C3Di_TexEnvBind, C3D_UpdateUniforms), with its dirty bits
// cleared as it clears them. A bare C3D_DrawArrays costs 3.25 us on an idle 2DS (gcbench 4) but ~8 us inside TP even
// raw (jobs 363-365): citro3d's update code is evicted from core 2's 16 KB I-cache between draws. Anything else dirty
// (frame buffer, viewport, program, attributes, fog, gas, lights, fixed attributes, int / bool / geometry-shader
// uniforms, a cube texture) goes through citro3d. The comparison draws (raw_draw) dry-run this and compare it too.
#define C3DF_TEXSTATUS (1u << 14)
#define RAWSTATE_FLAGS (C3DF_EFFECT | C3DF_TEXSTATUS | C3DF_TEXENVBUF | (7u << 23) | (0x3Fu << 26))
#define RAWSTATE_MAX 1024u  // words raw_state can write (uniforms: at most 96 vectors and their run headers)
static volatile bool g_rawStateWant;
u64 g_rawStateStats[3];  // draw calls whose state raw_state wrote, that went to citro3d, compared with it
void pica_set_rawstate(bool on) { g_rawStateWant = on; }
bool pica_rawstate(void) { return g_rawStateWant; }

// GPUCMD_Add's words for `n` parameters (n >= 1): parameter 0, the header with the count, the rest, an even pad;
// more than 256 parameters go as several commands (an incremental header moves on by the parameters sent).
static u32* cmd_add(u32* o, u32 header, const u32* p, u32 n) {
    while (n) {
        u32 r = n > 0x100 ? 0x100 : n, m = r - 1;
        o[0] = p[0];
        o[1] = header | ((m & 0xFF) << 20);
        memcpy(o + 2, p + 1, m * 4);
        o += m + 2;
        if (m & 1) *o++ = 0;
        p += r;
        n -= r;
        if (header & BIT(31)) header += r;
    }
    return o;
}

static inline u32* cmd_w(u32* o, u32 reg, u32 mask, u32 v) {
    o[0] = v;
    o[1] = GPUCMD_HEADER(0, mask, reg);
    return o + 2;
}

static inline u32* cmd_inc(u32* o, u32 reg, const u32* p, u32 n) { return cmd_add(o, GPUCMD_HEADER(1, 0xF, reg), p, n); }

// Whether raw_state can write everything citro3d holds dirty.
static bool raw_state_ok(void) {
    const C3dCtx* c = g_c3d;
    u32 f = c->flags & ~3u;
    if ((f & ~RAWSTATE_FLAGS) || c->fixedAttribDirty || c->lightEnv || (c->texEnvBuf & 7) == GPU_GAS) return false;
    if (C3D_BoolUnifsDirty[0] || C3D_BoolUnifsDirty[1]) return false;  // also stands for the shader constants
    u32 w[C3D_FVUNIF_COUNT / 4], acc = 0, iv[2 * C3D_IVUNIF_COUNT / 4];
    memcpy(w, C3D_FVUnifDirty[GPU_GEOMETRY_SHADER], sizeof w);
    for (u32 i = 0; i < sizeof w / 4; i++) acc |= w[i];
    memcpy(iv, C3D_IVUnifDirty, sizeof iv);
    for (u32 i = 0; i < sizeof iv / 4; i++) acc |= iv[i];
    if (acc) return false;
    for (int i = 0; i < 3; i++)
        if ((f & C3DF_TEX(i)) && c->tex[i] && C3D_TexGetType(c->tex[i]) != GPU_TEX_2D) return false;
    return true;
}

// Writes the dirty blocks from `o` as C3Di_UpdateContext would (raw_state_ok first); `apply` clears what it wrote as
// citro3d does, false is a dry run (the comparison draws).
static u32* raw_state(u32* o, bool apply) {
    C3dCtx* c = g_c3d;
    u32 f = c->flags;
    if (f & C3DF_EFFECT) {
        const C3dEffect* e = &c->effect;
        o = cmd_w(o, GPUREG_DEPTHMAP_ENABLE, 0xF, e->zBuffer ? 1 : 0);
        o = cmd_w(o, GPUREG_FACECULLING_CONFIG, 0xF, e->cullMode & 3);
        o = cmd_inc(o, GPUREG_DEPTHMAP_SCALE, &e->zScale, 2);
        o = cmd_inc(o, GPUREG_FRAGOP_ALPHA_TEST, &e->alphaTest, 4);
        o = cmd_w(o, GPUREG_GAS_DELTAZ_DEPTH, 0x8, (u32)GPU_MAKEGASDEPTHFUNC((e->depthTest >> 4) & 7) << 24);
        o = cmd_w(o, GPUREG_BLEND_COLOR, 0xF, e->blendClr);
        o = cmd_w(o, GPUREG_BLEND_FUNC, 0xF, e->alphaBlend);
        o = cmd_w(o, GPUREG_LOGIC_OP, 0xF, e->clrLogicOp);
        o = cmd_w(o, GPUREG_COLOR_OPERATION, 7, e->fragOpMode);
        o = cmd_w(o, GPUREG_FRAGOP_SHADOW, 0xF, e->fragOpShadow);
        o = cmd_w(o, GPUREG_EARLYDEPTH_TEST1, 1, e->earlyDepth ? 1 : 0);
        o = cmd_w(o, GPUREG_EARLYDEPTH_TEST2, 0xF, e->earlyDepth ? 1 : 0);
        o = cmd_w(o, GPUREG_EARLYDEPTH_FUNC, 1, e->earlyDepthFunc);
        o = cmd_w(o, GPUREG_EARLYDEPTH_DATA, 7, e->earlyDepthRef);
    }
    u32 texConfig = c->texConfig;
    bool status = (f & C3DF_TEXSTATUS) != 0;
    if (f & (7u << 23)) {
        static const u32 kBorder[3] = { GPUREG_TEXUNIT0_BORDER_COLOR, GPUREG_TEXUNIT1_BORDER_COLOR, GPUREG_TEXUNIT2_BORDER_COLOR };
        static const u32 kType[3] = { GPUREG_TEXUNIT0_TYPE, GPUREG_TEXUNIT1_TYPE, GPUREG_TEXUNIT2_TYPE };
        u32 units = 0;
        for (int i = 0; i < 3; i++) {
            const C3D_Tex* t = c->tex[i];
            if (!t) continue;
            units |= 1u << i;
            if (!(f & C3DF_TEX(i))) continue;
            u32 reg[5] = { t->border, t->dim, t->param, t->lodParam, osConvertVirtToPhys(t->data) >> 3 };
            o = cmd_inc(o, kBorder[i], reg, 5);
            o = cmd_w(o, kType[i], 0xF, t->fmt);
        }
        texConfig = (texConfig & ~7u) | units | BIT(16);
        status = true;
    }
    if (status) {
        o = cmd_w(o, GPUREG_TEXUNIT_CONFIG, 0xB, texConfig);
        if (texConfig & BIT(16)) {
            texConfig &= ~BIT(16);
            o = cmd_w(o, GPUREG_TEXUNIT_CONFIG, 0x4, BIT(16));
        }
        o = cmd_w(o, GPUREG_TEXUNIT0_SHADOW, 0xF, c->texShadow);
    }
    if (f & C3DF_TEXENVBUF) {
        o = cmd_w(o, GPUREG_TEXENV_UPDATE_BUFFER, 0x7, c->texEnvBuf);
        o = cmd_w(o, GPUREG_TEXENV_BUFFER_COLOR, 0xF, c->texEnvBufClr);
        o = cmd_w(o, GPUREG_FOG_COLOR, 0xF, c->fogClr);
    }
    if (f & (0x3Fu << 26))
        for (int i = 0; i < 6; i++)
            if (f & C3DF_TEXENV(i))
                o = cmd_inc(o, GPUREG_TEXENV0_SOURCE + (u32)(i >= 4 ? i + 2 : i) * 8, (const u32*)(const void*)&c->texEnv[i], 5);
    bool* d = C3D_FVUnifDirty[GPU_VERTEX_SHADER];
    for (int i = 0; i < C3D_FVUNIF_COUNT;) {
        u32 w4;
        if (!(i & 3) && (memcpy(&w4, d + i, 4), !w4)) { i += 4; continue; }
        if (!d[i]) { i++; continue; }
        int j = i;
        while (j < C3D_FVUNIF_COUNT && d[j]) j++;
        o = cmd_w(o, GPUREG_VSH_FLOATUNIFORM_CONFIG, 0xF, 0x80000000u | (u32)i);
        o = cmd_add(o, GPUCMD_HEADER(0, 0xF, GPUREG_VSH_FLOATUNIFORM_DATA), (const u32*)(const void*)&C3D_FVUnif[GPU_VERTEX_SHADER][i],
                    (u32)(j - i) * 4);
        if (apply) memset(d + i, 0, (size_t)(j - i));  // citro3d's "ever dirty" marks: every uniform, at the first frame
        i = j;
    }
    if (apply) {
        c->texConfig = texConfig;
        c->flags &= ~RAWSTATE_FLAGS;
    }
    return o;
}

// The draw block w (nw words) of a raw draw call, after its state; one call in 4,096 (and the first 256) is issued
// through citro3d instead and compared, state included when raw_state would have written it.
static u32 g_rawExp[RAWSTATE_MAX + 32];
static bool g_rawPlant;  // pica_set_rawplant: the comparison's self-test (one expected state word altered; must differ)
void pica_set_rawplant(bool on) { g_rawPlant = on; }
static void raw_draw(const u32* w, u32 nw, const char* what, bool elements, u32 first, u32 n) {
    bool dirty = c3d_dirty(), mine = dirty && g_rawStateWant && raw_state_ok();
    if (++g_rawN <= 256 || !(g_rawN & 4095)) {
        u32 k = 0;
        if (mine) k = (u32)(raw_state(g_rawExp, false) - g_rawExp);
        else if (dirty) C3Di_UpdateContext();
        if (g_rawPlant && k) { g_rawExp[0] ^= 1; g_rawPlant = false; emu_log(LOG_GX, "pica: raw state check: planted a difference\n"); }
        memcpy(g_rawExp + k, w, nw * 4);
        u32 at = gpuCmdBufOffset;
        if (elements) C3D_DrawElements(GPU_TRIANGLES, (int)n, C3D_UNSIGNED_SHORT, g.idx + first);
        else C3D_DrawArrays(GPU_TRIANGLES, (int)first, (int)n);
        raw_compare(at, g_rawExp, k + nw, what);
        if (mine) g_rawStateStats[2]++;
        return;
    }
    if (mine && gpuCmdBuf && gpuCmdBufOffset + RAWSTATE_MAX + nw <= gpuCmdBufSize) {
        gpuCmdBufOffset = (u32)(raw_state(gpuCmdBuf + gpuCmdBufOffset, true) - gpuCmdBuf);
        g_rawStateStats[0]++;
    } else if (dirty) {
        C3Di_UpdateContext();
        g_rawStateStats[1]++;
    }
    raw_emit(w, nw);
    g_rawStats[0]++;
}


static void draw_arrays(u32 first, u32 n) {
    attrs_regular();
    set_vbase(0);
    GW_ENTER(GW_DRAWCALL);
    dc_begin();
    if (g_rawWant && g_c3d && !g_rawBad) {
        u32 w[22];
        memcpy(w, kRawArrays, sizeof w);
        w[6] = n;
        w[8] = first;
        raw_draw(w, 22, "array", false, first, n);
    } else {
        C3D_DrawArrays(GPU_TRIANGLES, (int)first, (int)n);
    }
    dc_end();
    GW_LEAVE();
    g_picaStats.drawCalls++;
}

static void draw_elements(u32 first, u32 n) {
    attrs_regular();
    GW_ENTER(GW_DRAWCALL);
    dc_begin();
    if (g_rawWant && g_c3d && !g_rawBad) {
        u32 pa = g_idxPa + first * 2, base = g_c3d->bufInfo.base_paddr;
        if (pa < base) { dc_end(); GW_LEAVE(); return; }  // as citro3d: indices below the vertex buffer are not drawn
        u32 w[30];
        memcpy(w, kRawElements, sizeof w);
        w[4] = (pa - base) | 0x80000000u;  // C3D_UNSIGNED_SHORT << 31
        w[6] = n;
        raw_draw(w, 30, "indexed", true, first, n);
    } else {
        C3D_DrawElements(GPU_TRIANGLES, (int)n, C3D_UNSIGNED_SHORT, g.idx + first);
    }
    dc_end();
    GW_LEAVE();
    g_picaStats.drawCalls++;
}

// The GX state work of a draw, split (one draw in 16, scaled as PROF_GXSTATE): the previous batch's draw call
// (flush_batch), split_early, update_tev, the batch key (batch_key + tex_key, the full build), apply_frag, texture binds,
// light uniforms (perf.csv "# gx state split 10s").
u64 g_picaStateT[7];
#define SPROF_BEGIN() dprof_begin()
#define SPROF_END(k, t) do { if (g_drawTimed && g_profClock) g_picaStateT[k] += (g_profClock() - (t)) * g_drawScale; } while (0)

static void flush_batch(void) {
    static u32 n;
    bool timed = g.batchOpen && !(++n & 7);
    u64 t0 = timed ? svcGetSystemTick() : 0;
    vw_sync();  // where the batch ends may still be on the vertex worker (CPU-transformed draws it culls)
    if (timed) g_picaDc[11] += (svcGetSystemTick() - t0) * 8;
    if (g.batchOpen && g.batchIdx) {
        if (g.idxUsed > g.batchIdxFirst) draw_elements(g.batchIdxFirst, g.idxUsed - g.batchIdxFirst);
    } else if (g.batchOpen && g.arenaUsed > g.batchFirst) {
        draw_arrays(g.batchFirst, g.arenaUsed - g.batchFirst);
    }
    g.batchOpen = false;
}

static void flush_arena(void) {
    vw_sync_all();  // the vertex worker must have filled the arena before the GPU reads it
    // svcFlushProcessDataCache: 1.5-8x faster than GSPGPU_FlushDataCache (gcbench, GPU_GUIDANCE rule 14).
    if (g.arenaUsed > g.arenaFlushed)
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)(g.arena + g.arenaFlushed),
                                 (g.arenaUsed - g.arenaFlushed) * sizeof(PVert));
    if (g.idxUsed > g.idxFlushed)
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)(g.idx + g.idxFlushed), (g.idxUsed - g.idxFlushed) * 2);
    g.arenaFlushed = g.arenaUsed;
    g.idxFlushed = g.idxUsed;
}


static void free_all_textures(void) {
    for (u32 i = 0; i < MAX_TEX; i++)
        if (g.tex[i].live) { C3D_TexDelete(&g.tex[i].tex); g.tex[i].live = false; }
    g.texBytes = 0;
}

// ---- overlapped frames (--dblframe, C30) ----
// C3D_FrameBegin waits for the GPU to finish everything queued, the previous picture's tail included: 5-8% of the GX
// thread in the heavy area (job 512). With overlapped frames citro3d's frame is never ended after the first: a frame
// ends with a split and, at present, the screen transfer queued here; the vertex arena, the index arena and the
// command buffer alternate between two slots, so a frame waits only for the GPU to finish the frame before last (its
// slot's previous user). Retired textures, staging buffers and converted shapes are freed once the frame that retired
// them is known finished (free_done); textures drawn by a frame not yet finished are retired, not deleted. This thread
// swaps the screen once the queue has passed the transfer, and clears the queue only when it is idle (positions below
// are absolute: g_qBase + index). Option changes that free every texture wait for the GPU as before.
static volatile bool g_dblWant;
static bool g_dbl;           // active: citro3d stays in its frame
static u32 g_slot;           // the open frame's slot
static u64 g_slotFence[2];   // queue position after the last frame that used each slot
static u32 g_slotFrame[2];   // that frame's g.frameNo
static u32 g_doneFrame;      // frames up to this one are finished on the GPU (otherwise: the one before the open frame)
static u64 g_qBase;          // entries cleared from the queue so far
static u64 g_swapFence;      // the queued screen transfer's end: swap once passed (0: none pending)
static u64 g_capFence;       // the capture copy's end (cap_write reads it)
static u32* g_cmdBufs[2];
static PVert* g_arenaBase;
static u16* g_idxBase;
static u32 g_idxPaBase;
u64 g_dblStats[5];  // slot waits that blocked, queue clears, waits for queue room, swaps, waits for everything
void pica_set_dblframe(bool on) { g_dblWant = on; }
bool pica_dblframe(void) { return g_dblWant; }
static void queue_run(void);
static bool g_earlyRun;

static inline u64 q_end(void) { return g_qBase + g_q->numEntries; }
static inline bool q_passed(u64 pos) { return g_qBase + *(volatile u16*)&g_q->lastEntry >= pos; }
// Not running: everything queued has completed (an active queue runs what is added at once; Run restarts the rest).
static bool q_idle(void) { gxCmdQueueRun(g_q); return gxCmdQueueWait(g_q, 0); }

static void q_wait(u64 pos, u32 why) {
    if (q_passed(pos)) return;
    g_dblStats[why]++;
    u64 t0 = prof_begin();
    GW_ENTER(GW_FRAMEBEGIN);
    while (!q_passed(pos) && !q_idle()) gspWaitForAnyEvent();
    GW_LEAVE();
    prof_end(PROF_GXSYNC, t0);
}

static void q_clear_if_idle(void) {
    if (!g_q->numEntries || !q_idle()) return;
    g_qBase += g_q->numEntries;
    gxCmdQueueClear(g_q);
    g.queued = 0;
    g_dblStats[1]++;
}

// Room for n more entries: the queue is cleared, which needs it idle.
static void q_room(u32 n) {
    if (g_q->numEntries + n + 2 <= g_q->maxEntries) return;
    q_clear_if_idle();
    if (g_q->numEntries + n + 2 <= g_q->maxEntries) return;
    q_wait(q_end(), 2);
    q_clear_if_idle();
}

static void swap_check(bool wait) {
    if (!g_swapFence) return;
    if (wait) q_wait(g_swapFence, 0);
    else if (!q_passed(g_swapFence)) return;
    gfxScreenSwapBuffers(GFX_TOP, false);
    g_swapFence = 0;
    g_dblStats[3]++;
}

static void use_slot(u32 s) {
    g_slot = s;
    g.arena = g_arenaBase + s * (ARENA_VERTS / 2);
    if (g_idxBase) { g.idx = g_idxBase + s * (IDX_MAX / 2); g_idxPa = g_idxPaBase + s * IDX_MAX; }  // (IDX_MAX / 2 u16s)
    g_c3d->cmdBuf = g_cmdBufs[s];
    g_c3d->cmdBufUsage = 0;
    GPUCMD_SetBuffer(g_cmdBufs[s], g_c3d->cmdBufSize, 0);
}

// Right after C3D_FrameBegin (the queue idle, stopped and cleared): the open frame becomes slot 0's.
static bool dbl_enter(void) {
    if (!g_q || !g_earlyRun) return false;  // (the queue is found at the first splits: tried again at the next frame)
    if (!g_c3d) { g_dblWant = false; return false; }
    if (!g_cmdBufs[0]) {
        g_cmdBufs[0] = g_c3d->cmdBuf;
        g_arenaBase = g.arena;
        g_idxBase = g.idx;
        g_idxPaBase = g_idxPa;
    }
    if (!g_cmdBufs[1] && !(g_cmdBufs[1] = linearAlloc(g_c3d->cmdBufSize * 4))) { g_dblWant = false; return false; }
    if (g_q->maxEntries < QBIG_ENTRIES) {
        gxCmdEntry_s* e = malloc(QBIG_ENTRIES * sizeof *e);
        if (!e) { g_dblWant = false; return false; }
        free(g_q->entries);
        g_q->entries = e;
        g_q->maxEntries = QBIG_ENTRIES;
        g_qBigDone = true;
    }
    g_qBase = 0;
    g_slotFence[0] = g_slotFence[1] = 0;
    g_slotFrame[0] = g_slotFrame[1] = 0;
    g_swapFence = g_capFence = 0;
    g_arenaCap = ARENA_VERTS / 2;
    g_idxCap = IDX_MAX / 2;
    use_slot(0);
    g_dbl = true;
    return true;
}

// Between frames: everything finishes, the picture is swapped, and citro3d's frame ends (nothing is left to submit).
static void dbl_leave(void) {
    q_wait(q_end(), 4);
    swap_check(true);
    g_dbl = false;
    g_arenaCap = ARENA_VERTS;
    g_idxCap = IDX_MAX;
    g.arena = g_arenaBase;
    if (g_idxBase) { g.idx = g_idxBase; g_idxPa = g_idxPaBase; }
    C3D_FrameEnd(GX_CMDLIST_FLUSH);
}

// The next frame's slot, once its previous frame has finished; true when everything queued has (an option change).
static bool dbl_begin(void) {
    bool all = g.resetPending || g_texDiv != g_texDivWant || g_mips != g_mipsWant || g_small != g_smallWant ||
               g_vramTex != g_vramTexWant || g_xfbFcram != g_xfbFcramWant;
    u32 s = g_slot ^ 1;
    if (all) {
        q_wait(q_end(), 4);
        g_doneFrame = g.frameNo;
    } else {
        q_wait(g_slotFence[s], 0);
        if (g_slotFrame[s] > g_doneFrame) g_doneFrame = g_slotFrame[s];
    }
    swap_check(false);
    q_clear_if_idle();
    use_slot(s);
    return all;
}

static void dbl_end(bool present) {
    q_room(2);
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
    if (present) {
        swap_check(true);  // the previous picture shown before this transfer takes the back buffer
        C3D_FrameBufTransfer(&g.screen->frameBuf, g.screen->screen, g.screen->side, g.screen->transferFlags);
        g.screen->used = false;
        g_swapFence = q_end();
    }
    g_slotFence[g_slot] = q_end();
    g_slotFrame[g_slot] = g.frameNo;
    queue_run();
}

// Frees what was retired by frames the GPU has finished (all of it when C3D_FrameBegin waited).
static void ns_grave_free_done(void);
static void free_done(void) {
    u32 n = 0;
    for (u32 i = 0; i < g.nGrave; i++)
        if (g.graveFrame[i] <= g_doneFrame) C3D_TexDelete(&g.grave[i]);
        else { g.grave[n] = g.grave[i]; g.graveFrame[n++] = g.graveFrame[i]; }
    g.nGrave = n;
    n = 0;
    for (u32 i = 0; i < g_nStaging; i++)
        if (g_stagingFrame[i] <= g_doneFrame) linearFree(g_staging[i]);
        else { g_staging[n] = g_staging[i]; g_stagingFrame[n++] = g_stagingFrame[i]; }
    g_nStaging = n;
    ns_grave_free_done();
}

static void begin_frame(void) {
    if (g.frame) return;
    if (g_dbl && !g_dblWant) dbl_leave();
    bool idle = true;  // the GPU has finished everything queued
    if (g_dbl) {
        idle = dbl_begin();
    } else {
        u64 t0 = prof_begin();
        GW_ENTER(GW_FRAMEBEGIN);
        C3D_FrameBegin(0);  // waits for the previous submit: the arena and retired textures are free again
        GW_LEAVE();
        prof_end(PROF_GXSYNC, t0);
        if (g_qBigWant && !g_qBigDone && g_q && g_q->numEntries == 0 && g_q->maxEntries < QBIG_ENTRIES) {
            // (the queue is stopped and cleared here: C3D_FrameBegin waited for it)
            gxCmdEntry_s* e = malloc(QBIG_ENTRIES * sizeof *e);
            if (e) { free(g_q->entries); g_q->entries = e; g_q->maxEntries = QBIG_ENTRIES; g_qBigDone = true; }
        }
        if (g_dblWant) dbl_enter();
    }
    g_qBudget = (g_qBigWant || g_dbl) && g_qBigDone ? QBIG_ENTRIES - 8 : 26;
    g.frame = true;
    g.frameNo++;
    if (idle) g_doneFrame = g.frameNo - 1;
    g.arenaUsed = g.arenaFlushed = g.queued = g.splitArena = g.idxUsed = g.idxFlushed = g.vbase = 0;
    if (g_dbl) g.queued = g_q->numEntries;
    g.batchOpen = false;
    free_done();
    if (g_texDiv != g_texDivWant) { g_texDiv = g_texDivWant; g.resetPending = true; }  // (--texdiv: every texture again)
    if (g_mips != g_mipsWant) { g_mips = g_mipsWant; g.resetPending = true; }  // (--mips: every texture again)
    g_gpuDiag = g_gpuDiagWant;
    g_vfDeferCpu = g_deflitWant;  // this thread prepares the jobs (vfast_prepare reads it)
    if (g_shlightWant >= 0) { pica_set_shader_light(g_shlightWant != 0); g_shlightWant = -1; }
    if (g_small != g_smallWant) { g_small = g_smallWant; g.resetPending = true; }  // (--smalltex: every texture again)
    if (g_vramTex != g_vramTexWant) { g_vramTex = g_vramTexWant; g.resetPending = true; }  // (every texture again)
    if (g_xfbFcram != g_xfbFcramWant) {  // the XFBs moved (the GPU is done with them: C3D_FrameBegin waited); pictures lost
        g_xfbFcram = g_xfbFcramWant;
        for (u32 i = 0; i < MAX_XFB; i++) {
            C3D_TexDelete(&g.xfb[i].tex);
            if ((g_xfbFcram || !C3D_TexInitVRAM(&g.xfb[i].tex, TW, TH, GPU_RGBA8)) && !C3D_TexInit(&g.xfb[i].tex, TW, TH, GPU_RGBA8))
                emu_log(LOG_GX, "pica: XFB %u could not be allocated again\n", (unsigned)i);
            g.xfb[i].seq = 0;
        }
    }
    if (g.resetPending) { free_all_textures(); g.resetPending = false; }
    g.natAttr = false;
    if (g.uNat >= 0) C3D_FVUnifSet(GPU_VERTEX_SHADER, g.uNat, 0, 0, 0, 0);
    C3D_BindProgram(&g.prog);
    static bool unifWarm;  // raw_state cannot set citro3d's "ever dirty" marks: all uniforms once through citro3d
    if (!unifWarm) { (void)C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, 0, C3D_FVUNIF_COUNT); unifWarm = true; }
    attrs_arena();
    C3D_BufInfo* bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, g.arena, sizeof(PVert), 7, 0x6543210);
    C3D_DepthMap(true, -1.0f, 0.0f);  // depth = -z/w; vertices carry z = -(GX screen z / 2^24) * w
    C3D_CullFace(GPU_CULL_NONE);
    C3D_FrameDrawOn(g.efb);
    g.stateValid = g.tevValid = false;
    g_texEpoch++;
}

// present: the frame's picture is in g.screen (with overlapped frames the transfer and swap are queued here; citro3d
// does both for a linked target that was drawn on).
static void end_frame_at(bool present) {
    if (!g.frame) return;
    flush_batch();
    u64 t0 = prof_begin();
    flush_arena();
    GW_ENTER(GW_FRAMEEND);
    if (g_dbl) dbl_end(present);
    else C3D_FrameEnd(GX_CMDLIST_FLUSH);
    GW_LEAVE();
    prof_end(PROF_GXEND, t0);
    g_picaStats.submits++;
    g.frame = false;
}

static void end_frame(void) { end_frame_at(false); }

// A list of retired memory is full: everything queued must finish before the next frame frees it.
static void frame_drain(void) {
    end_frame();
    if (g_dbl) { q_wait(q_end(), 4); g_doneFrame = g.frameNo; }
    begin_frame();
}

// citro3d's GX command queue, found in its context at attach (citro3d keeps it private): citro3d starts it only at
// C3D_FrameEnd, so the GPU sat idle while this thread recorded a whole frame and the next C3D_FrameBegin then waited
// for all of it. Starting it at each split lets the GPU work on the frame's first part meanwhile. Entries run in
// order; citro3d's queue-finished callback only swaps screens after a real frame end.
static gxCmdQueue_s* g_q;
static bool g_earlyRun = true;  // pica_set_early_queue
extern u8 __C3D_Context[];

// Candidates: words of the context that look like a gxCmdQueue_s (heap entries, sane counts, a callback in code). The
// splits pick the one whose entry count grows by exactly one (verify_queue); until then nothing starts early.
static u32 g_qCand[16], g_qCandN, g_qCandCount[16], g_qTries;
static bool g_qChecked;

static void find_c3d_queue(void) {
    g_qCandN = 0;
    for (u32 off = 0; off + sizeof(gxCmdQueue_s) <= 0x2D0 && g_qCandN < 16; off += 4) {
        gxCmdQueue_s* q = (gxCmdQueue_s*)(__C3D_Context + off);
        u32 e = (u32)q->entries, cb = (u32)q->callback;
        if (e >= 0x08000000u && e < 0x40000000u && q->maxEntries >= 8 && q->maxEntries <= 1024 &&
            q->numEntries <= q->maxEntries && q->curEntry <= q->numEntries && cb >= 0x00100000u && cb < 0x08000000u)
            g_qCand[g_qCandN++] = off;
    }
}

// Around the first C3D_FrameSplit: before = true snapshots the candidates' entry counts, before = false keeps the
// one that grew by one.
static void verify_queue(bool before) {
    if (g_qChecked) return;
    for (u32 i = 0; i < g_qCandN; i++) {
        gxCmdQueue_s* q = (gxCmdQueue_s*)(__C3D_Context + g_qCand[i]);
        if (before) g_qCandCount[i] = q->numEntries;
        else if (q->numEntries == g_qCandCount[i] + 1 && !g_q) g_q = q;
    }
    if (!before && (g_q || ++g_qTries == 50)) {  // an empty split adds no entry: keep looking for a while
        g_qChecked = true;
        emu_log(LOG_GX, "pica: %u GX queue candidates in citro3d's context, %s\n", (unsigned)g_qCandN,
                g_q ? "found" : "none grew by one in 50 splits");
        for (u32 i = 0; i < g_qCandN; i++) {
            const gxCmdQueue_s* q = (const gxCmdQueue_s*)(__C3D_Context + g_qCand[i]);
            emu_log(LOG_GX, "  +%03lx entries %08lx max %u num %u (was %u) cur %u\n", (unsigned long)g_qCand[i],
                    (unsigned long)(u32)q->entries, q->maxEntries, q->numEntries, (unsigned)g_qCandCount[i], q->curEntry);
        }
        if (!g_qCandN) {  // nothing looked like a queue: dump the context's words for the next look
            const u32* w = (const u32*)__C3D_Context;
            for (u32 i = 0; i < 0x2D0 / 4; i += 8)
                emu_log(LOG_GX, "  ctx+%03lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx %08lx\n", (unsigned long)(i * 4),
                        (unsigned long)w[i], (unsigned long)w[i + 1], (unsigned long)w[i + 2], (unsigned long)w[i + 3],
                        (unsigned long)w[i + 4], (unsigned long)w[i + 5], (unsigned long)w[i + 6], (unsigned long)w[i + 7]);
        }
    }
}

static void queue_run(void) {
    if (g_q && g_earlyRun) gxCmdQueueRun(g_q);
    if (g_swapFence) swap_check(false);  // (overlapped frames: the picture is shown once its transfer is done)
}
void pica_set_early_queue(bool on) { g_earlyRun = on; }
bool pica_early_queue(void) { return g_earlyRun && g_q; }
// Draws by batch path: [0] nothing in the key changed (fast), [1] key rebuilt and equal, [2] new batch; [3..6] new
// batches with a changed BP / palette / light version or XFB copy since the open batch (written by the GX thread).
u64 g_picaDrawPaths[8];
u64 g_vwInline, g_vwInlineVerts;  // draws (and their vertices) too large for the worker's job ring, done on the GX thread

u32 pica_queue_offset(void) { return g_q ? (u32)((u8*)g_q - __C3D_Context) : 0xFFFFFFFFu; }

// Native shapes draw from their own buffers, not the arena: g_nsSince counts their indices since the last split, so
// that a frame of them is split as often (split_early; always 0 in the emulator).
static u32 g_nsSince;

// Submits what is queued so far so that the `transfers` GX transfers queued next run after it. The GX command queue
// holds 32 entries (citro3d) and runs only at C3D_FrameEnd; libctru panics when it is full, so a frame that has
// queued many splits and transfers (several emulated fields between presents) is submitted early.
static void split(u32 transfers) {
    begin_frame();
    flush_batch();
    if (g_dbl) { q_room(1 + transfers); g.queued = g_q->numEntries; }  // (the queue keeps earlier frames' entries)
    else if (g.queued + 1 + transfers > GXQUEUE_BUDGET) { g_endWhy[0]++; end_frame(); begin_frame(); }
    flush_arena();
    verify_queue(true);
    GW_ENTER(GW_SPLIT);
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
    GW_LEAVE();
    verify_queue(false);
    g_picaStats.splits++;
    g.queued += 1 + transfers;
    g.splitArena = g.arenaUsed;
    g_nsSince = 0;
    if (!transfers) queue_run();  // with transfers, the caller runs the queue once it has added them
}

// Splits the frame every SPLIT_VERTS arena vertices so the GPU works on the frame while this thread records the
// rest: C3D_FrameBegin waits for everything after the last split, and without these the main scene came after the
// last EFB copy (2DS, 29a22f7: this thread waited for the GPU 7-14% of the time). Called between batches; keeps queue
// entries for the frame's copies and clears.
#define SPLIT_VERTS g_splitVerts
static volatile u32 g_splitVerts = 8192;  // pica_set_split_verts (--splitverts N, --ab splitverts)
void pica_set_split_verts(u32 n) { g_splitVerts = n >= 1024 && n <= 65536 ? n : 8192; }
u32 pica_split_verts(void) { return g_splitVerts; }
static bool g_earlySplit = true;  // pica_set_early_split
static bool g_indexed = true;     // pica_set_indexed (indexed triangle draws, g.idx)
void pica_set_indexed(bool on) { g_indexed = on && g.idx; }
bool pica_indexed(void) { return g_indexed; }
void pica_set_early_split(bool on) { g_earlySplit = on; }
bool pica_early_split(void) { return g_earlySplit; }
static void split_early(void);  // below the vertex worker
static bool g_splitOpen = true;  // pica_set_split_open: early splits leave the open batch open
void pica_set_split_open(bool on) { g_splitOpen = on; }
bool pica_split_open(void) { return g_splitOpen; }

// Makes room for `verts` more vertices and keeps the command buffer from overflowing (libctru drops commands
// silently when it is full).
static u32 g_vwReserved;  // arena vertices the vertex worker's queued draws may write (vertex worker below)

static bool need(u32 verts) {
    if (verts > g_arenaCap) return false;
    begin_frame();
    vw_sync();  // this thread writes the arena itself, after the worker's variable-length draws
    if (g.arenaUsed + verts > g_arenaCap || C3D_GetCmdBufUsage() > g_cmdbufLimit) {
        g_endWhy[g.arenaUsed + verts > g_arenaCap ? 1 : 2]++;
        end_frame();
        begin_frame();
    }
    return true;
}

// Room for a draw the vertex worker will append, without waiting for it.
static bool need_work(u32 verts, u32 indices) {
    if (verts > g_arenaCap || indices > g_idxCap) return false;
    begin_frame();
    if (g.arenaUsed + g_vwReserved + verts > g_arenaCap || g.idxUsed + indices > g_idxCap || C3D_GetCmdBufUsage() > g_cmdbufLimit) {
        g_endWhy[g.arenaUsed + g_vwReserved + verts > g_arenaCap ? 1 : g.idxUsed + indices > g_idxCap ? 3 : 2]++;
        end_frame();
        begin_frame();
    }
    return true;
}

// lastUse: the frame that last drew with it. Drawn by a frame the GPU may not have finished (this one, or with
// overlapped frames the one before): kept until that is done.
static void retire(C3D_Tex* t, u32 lastUse) {
    if (lastUse <= g_doneFrame) { C3D_TexDelete(t); return; }
    if (g.nGrave == MAX_GRAVE) { g_endWhy[4]++; frame_drain(); }
    g.graveFrame[g.nGrave] = g.frameNo;
    g.grave[g.nGrave++] = *t;
}

static void note_approx(u32 bits) {
    u32 fresh = bits & ~g.approxSeen;
    if (!fresh) return;
    g.approxSeen |= fresh;
    emu_log(LOG_GX, "pica: TEV approximation flags %02x (first seen)\n", (unsigned)fresh);
}

// ---- textures ----
static u32 pow2(u32 v) { u32 p = 8; while (p < v && p < 1024) p <<= 1; return p; }

// FNV-1a over 32-bit words; above 16 KB every n-th word only (catches replaced textures, can miss small edits).
static u32 hash_words(const u8* p, u32 bytes) {
    const u32* w = (const u32*)p;
    u32 n = bytes / 4, step = n > 4096 ? n / 4096 : 1, h = 2166136261u;
    for (u32 i = 0; i < n; i += step) h = (h ^ w[i]) * 16777619u;
    return h ^ bytes;
}

static u32 tlut_bytes(u32 fmt) { return fmt == 8 ? 32 : fmt == 9 ? 512 : 32768; }

// Linear RGBA8 (0xRRGGBBAA, rows top-down) into the PICA's tiled layout: 8x8 tiles in Z-order, tile rows stored
// bottom-up (the PICA samples t = 0 from the last stored row; checked with texturetest in Azahar), so GX texture
// coordinates are used unchanged. Sizes that are not powers of two are resampled (nearest) up to one, which keeps
// every wrap mode correct.
// One level: rgba (w x h) into W x H tiles at dst (resampled nearest when the sizes differ).
static void upload_level(u32* dst, u32 W, u32 H, const u32* rgba, u32 w, u32 h) {
    const u16* cols = col_map(W, w);
    for (u32 y = 0; y < H; y++) {
        const u32* row = rgba + ((H - 1 - y) * h / H) * w;
        u32* tiles = dst + (y >> 3) * (W >> 3) * 64;
        const u8* m = &g_morton[(y & 7) * 8];
        for (u32 x = 0; x < W; x++) tiles[(x >> 3) * 64 + m[x & 7]] = row[cols[x]];
    }
}

static void evict_unused(u32 want) {
    while (g.texBytes + want > TEX_BUDGET) {
        PTex* old = NULL;
        for (u32 i = 0; i < MAX_TEX; i++) {
            PTex* p = &g.tex[i];
            if (p->live && p->lastUse != g.frameNo && (!old || p->lastUse < old->lastUse)) old = p;
        }
        if (!old) return;
        retire(&old->tex, old->lastUse);
        old->live = false;
        g.texBytes -= old->bytes;
    }
}

// Measurement only (--texdiv N, --ab texdiv): textures decoded at 1/N of their size (nearest), to test whether the
// GPU's time is texture fetch (job 386: ~100 ms of GPU a frame in the heavy area against 8 ms in Ordon's start).


void pica_set_texdiv(u32 n) { g_texDivWant = n >= 1 && n <= 16 ? n : 1; }
u32 pica_texdiv(void) { return g_texDivWant; }

static bool decode(System* s, const GXCore* gx, PTex* e) {
    u32 W = pow2(e->w), H = pow2(e->h);
    if (g_texDiv > 1) { W = W / g_texDiv < 8 ? 8 : W / g_texDiv; H = H / g_texDiv < 8 ? 8 : H / g_texDiv; }
    u32 bytes = W * H * 4;
    u32 lv = g_texDiv == 1 && e->levels > 1 ? e->levels : 1;
    u32 n = e->w * e->h;
    if (!scratch_reserve(n)) { g_picaStats.texFails++; return false; }
    gx_decode_texture(s->mem1 + e->addr, e->fmt, e->w, e->h, gx->tmem + (e->tlutAddr & 0xFFFFF), e->tlutFmt, g_scratch);
    GPU_TEXCOLOR tf = g_small && g_texDiv == 1 ? small_format(g_scratch, n, e->fmt, e->tlutFmt) : GPU_RGBA8;
    g_texFmtCount[tf & 15]++;
    if (tf == GPU_RGBA8) g_texRgba8By[e->fmt & 15]++;
    if (tf != GPU_RGBA8) bytes = W * H * (tf == GPU_RGB8 ? 3 : tf == GPU_L8 || tf == GPU_LA4 ? 1 : 2);
    if (lv > 1) bytes = C3D_TexCalcTotalSize(bytes, (int)lv - 1);
    g_picaStats.texDecodes++;
    g_picaStats.texDecodeBytes += bytes;
    evict_unused(bytes);
    C3D_Tex nt;
    bool vram = g_vramTex && vramSpaceFree() > bytes + VRAM_RESERVE;
    u8* staging = NULL;
    if (vram) {
        if (g_nStaging == MAX_STAGING) { g_endWhy[5]++; frame_drain(); }
        C3D_TexInitParams vp = { (u16)W, (u16)H, (u8)(lv - 1), tf, GPU_TEX_2D, true };
        staging = linearAlloc(bytes);
        if (!staging || !C3D_TexInitWithParams(&nt, NULL, vp)) {
            if (staging) linearFree(staging);
            staging = NULL;
            vram = false;
        }
    } else if (g_vramTex) {
        g_vramTexStats[2]++;
    }
    if (!vram) {
        C3D_TexInitParams ip = { (u16)W, (u16)H, (u8)(lv - 1), tf, GPU_TEX_2D, false };
        if (!C3D_TexInitWithParams(&nt, NULL, ip)) {
            evict_unused(TEX_BUDGET);  // linear memory short of the budget: every texture this frame does not use goes
            if (!C3D_TexInitWithParams(&nt, NULL, ip)) { g_picaStats.texFails++; return false; }
        }
    }
    // Where the texels are written: the texture itself, or (VRAM) the staging buffer laid out the same way.
    u8* base = staging ? staging : (u8*)nt.data;
#define LEVEL_PTR(L) (base + ((u8*)C3D_Tex2DGetImagePtr(&nt, (int)(L), NULL) - (u8*)nt.data))
    if (tf != GPU_RGBA8) {  // (a smaller format: level by level through the converter; lower levels quantized alike)
        u32 src = e->addr;
        for (u32 L = 0; L < lv; L++) {
            u32 wl = e->w >> L, hl = e->h >> L;
            if (L) gx_decode_texture(s->mem1 + src, e->fmt, wl, hl, gx->tmem + (e->tlutAddr & 0xFFFFF), e->tlutFmt, g_scratch);
            upload_level_fmt(LEVEL_PTR(L), W >> L, H >> L, g_scratch, L ? wl : e->w, L ? hl : e->h, tf);
            src += gx_texture_size(e->fmt, wl, hl);
        }
    } else if (lv == 1) {
        upload_level((u32*)(void*)base, W, H, g_scratch, e->w, e->h);
    } else {  // level L follows level L - 1 in RAM, each in the format's tiles (power-of-two sizes: no resampling)
        u32 src = e->addr;
        for (u32 L = 0; L < lv; L++) {
            u32 wl = e->w >> L, hl = e->h >> L;
            if (L) gx_decode_texture(s->mem1 + src, e->fmt, wl, hl, gx->tmem + (e->tlutAddr & 0xFFFFF), e->tlutFmt, g_scratch);
            upload_level((u32*)(void*)LEVEL_PTR(L), W >> L, H >> L, g_scratch, wl, hl);
            src += gx_texture_size(e->fmt, wl, hl);
        }
    }
#undef LEVEL_PTR
    svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)base, bytes);
    g_texEpoch++;
    if (staging) {  // to VRAM by DMA, queued ahead of the draws that will sample it
        split(1);
        GX_TextureCopy((u32*)(void*)staging, 0, (u32*)nt.data, 0, bytes, GX_TRANSFER_RAW_COPY(1));
        queue_run();
        g_stagingFrame[g_nStaging] = g.frameNo;
        g_staging[g_nStaging++] = staging;
        g_vramTexStats[0]++;
        g_vramTexStats[1] += bytes;
    }
    if (e->live) { retire(&e->tex, e->lastUse); g.texBytes -= e->bytes; }
    e->tex = nt;
    e->bytes = bytes;
    e->live = true;
    g.texBytes += bytes;
    return true;
}

static C3D_Tex* cached_texture(System* s, const GXCore* gx, u32 map, u32 addr, u32 fmt, u32 w, u32 h, u32 tlutReg,
                               u32 levels) {
    u32 bytes = 0;
    for (u32 L = 0; L < levels; L++) bytes += gx_texture_size(fmt, w >> L, h >> L);  // (the levels in RAM)
    if (!bytes || addr < PA_MEM1_BEGIN || addr + bytes > PA_MEM1_END) return NULL;
    bool pal = fmt == 8 || fmt == 9 || fmt == 10;
    u32 tlutAddr = pal ? (tlutReg & 0x3FF) << 9 : 0, tlutFmt = pal ? (tlutReg >> 10) & 3 : 0;
    PTex* e = NULL;
    PTex* slot = NULL;
    PTex* last = g.lastHit[map];  // texture maps mostly keep their texture from one draw to the next
    if (last && last->live && last->addr == addr && last->fmt == fmt && last->w == w && last->h == h &&
        last->tlutAddr == tlutAddr && last->tlutFmt == tlutFmt && last->levels == levels)
        e = last;
    for (u32 i = 0; i < MAX_TEX && !e; i++) {
        PTex* p = &g.tex[i];
        if (!p->live) { if (!slot) slot = p; continue; }
        if (p->addr == addr && p->fmt == fmt && p->w == w && p->h == h && p->tlutAddr == tlutAddr && p->tlutFmt == tlutFmt &&
            p->levels == levels) {
            e = p;
            break;
        }
    }
    if (e) {
        // RAM and palettes may have changed since the entry was checked: re-hash once per emulated frame (EFB copies
        // stay on the GPU here and never write RAM) and decode again only when the contents really differ.
        bool dirty = false;
        if (e->texVersion != g.xfbSeq) {
            u32 dh = hash_words(s->mem1 + addr, bytes);
            dirty |= dh != e->dataHash;
            e->dataHash = dh;
            e->texVersion = g.xfbSeq;
        }
        if (pal && e->tlutVersion != gx->tlutVersion) {
            u32 th = hash_words(gx->tmem + (tlutAddr & 0xFFFFF), tlut_bytes(fmt));
            dirty |= th != e->tlutHash;
            e->tlutHash = th;
            e->tlutVersion = gx->tlutVersion;
        }
        if (dirty && !decode(s, gx, e)) return NULL;
    } else {
        if (!slot) {  // table full: reuse the least recently used entry not drawn with this frame
            for (u32 i = 0; i < MAX_TEX; i++)
                if (g.tex[i].lastUse != g.frameNo && (!slot || g.tex[i].lastUse < slot->lastUse)) slot = &g.tex[i];
            if (!slot) return NULL;
            retire(&slot->tex, slot->lastUse);
            slot->live = false;
            g.texBytes -= slot->bytes;
        }
        e = slot;
        e->addr = addr; e->fmt = fmt; e->w = w; e->h = h; e->tlutAddr = tlutAddr; e->tlutFmt = tlutFmt; e->levels = levels;
        e->dataHash = hash_words(s->mem1 + addr, bytes);
        e->tlutHash = pal ? hash_words(gx->tmem + (tlutAddr & 0xFFFFF), tlut_bytes(fmt)) : 0;
        e->texVersion = g.xfbSeq;
        e->tlutVersion = gx->tlutVersion;
        if (!decode(s, gx, e)) return NULL;
    }
    e->lastUse = g.frameNo;
    g.lastHit[map] = e;
    return &e->tex;
}

static PCopy* find_copy(System* s, const GXCore* gx, u32 addr, u32 w, u32 h) {
    for (u32 i = 0; i < MAX_COPIES; i++) {
        PCopy* c = &g.copies[i];
        if (!c->used || c->addr != addr || c->w != w || c->h != h) continue;
        // The CPU writing the destination afterwards means RAM holds a newer texture than the copy.
        if (c->texVersion != g.xfbSeq) {
            if (hash_words(s->mem1 + addr, c->ramBytes) != c->ramHash) { c->used = false; return NULL; }
            c->texVersion = g.xfbSeq;
        }
        return c;
    }
    return NULL;
}

// The texture bound for GX texture map `map`, with its wrap and filter modes applied.
static C3D_Tex* fetch_texture_(System* s, const GXCore* gx, u32 map);
static C3D_Tex* fetch_texture(System* s, const GXCore* gx, u32 map) {
    GW_ENTER(GW_TEX);
    C3D_Tex* t = fetch_texture_(s, gx, map);
    GW_LEAVE();
    return t;
}
static C3D_Tex* fetch_texture_(System* s, const GXCore* gx, u32 map) {
    u32 hi = map >= 4 ? 0x20 : 0, m = map & 3;
    u32 img0 = gx->bp[0x88 + m + hi], img3 = gx->bp[0x94 + m + hi], tlut = gx->bp[0x98 + m + hi], mode = gx->bp[0x80 + m + hi];
    u32 w = (img0 & 0x3FF) + 1, h = ((img0 >> 10) & 0x3FF) + 1, fmt = (img0 >> 20) & 15, addr = gx_tex_addr(gx, map, img3);
    PCopy* c = find_copy(s, gx, addr, w, h);
    // Address 0: no data in RAM (native mode: a texture object Aurora gave no data, matched only by a copy to 0).
    // Mip levels: the min filter's mip mode (TX_SETMODE0 bits 5-6: 1 nearest, 2 linear) and TX_SETMODE1's max LOD (u4.4),
    // for power-of-two sizes from 16 (the PICA's smallest level is 8 x 8).
    u32 mip = (mode >> 5) & 3, levels = 1;
    if (g_mips && !c && (mip == 1 || mip == 2) && !(w & (w - 1)) && !(h & (h - 1)) && w >= 16 && h >= 16) {
        u32 maxLod = (gx->bp[0x84 + m + hi] >> 8) & 0xFF, gxLv = (maxLod + 15) / 16 + 1, pica = 1;
        while ((w >> pica) >= 8 && (h >> pica) >= 8) pica++;  // levels down to 8 x 8
        levels = gxLv < pica ? gxLv : pica;
    }
    C3D_Tex* t = c ? &c->tex : addr ? cached_texture(s, gx, map, addr, fmt, w, h, tlut, levels) : NULL;
    if (!t) t = &g.white;
    if (levels > 1 && t != &g.white) C3D_TexSetFilterMipmap(t, mip == 2 ? GPU_LINEAR : GPU_NEAREST);
    static const GPU_TEXTURE_WRAP_PARAM wraps[4] = { GPU_CLAMP_TO_EDGE, GPU_REPEAT, GPU_MIRRORED_REPEAT, GPU_REPEAT };
    C3D_TexSetWrap(t, wraps[mode & 3], wraps[(mode >> 2) & 3]);
    C3D_TexSetFilter(t, (mode >> 4) & 1 ? GPU_LINEAR : GPU_NEAREST, ((mode >> 5) & 7) >= 4 ? GPU_LINEAR : GPU_NEAREST);
    return t;
}

// ---- state ----
static const GPU_TESTFUNC kFunc[8] = { GPU_NEVER, GPU_LESS, GPU_EQUAL, GPU_LEQUAL, GPU_GREATER, GPU_NOTEQUAL, GPU_GEQUAL, GPU_ALWAYS };

// Each stage is built aside and stored only when it differs from the stage citro3d holds (lean state): a TEV change
// usually touches one or two of the six.
// A program's six combiner stages as citro3d holds them.
static void build_env(const PicaTev* t, C3D_TexEnv* env) {
    for (int i = 0; i < PICA_STAGES; i++) {
        C3D_TexEnv* e = &env[i];
        C3D_TexEnvInit(e);
        if (i < t->numStages) {
            const PicaStage* p = &t->st[i];
            C3D_TexEnvSrc(e, C3D_RGB, p->srcRgb[0], p->srcRgb[1], p->srcRgb[2]);
            C3D_TexEnvSrc(e, C3D_Alpha, p->srcA[0], p->srcA[1], p->srcA[2]);
            C3D_TexEnvOpRgb(e, p->opRgb[0], p->opRgb[1], p->opRgb[2]);
            C3D_TexEnvOpAlpha(e, p->opA[0], p->opA[1], p->opA[2]);
            C3D_TexEnvFunc(e, C3D_RGB, p->fnRgb);
            C3D_TexEnvFunc(e, C3D_Alpha, p->fnA);
            C3D_TexEnvScale(e, C3D_RGB, p->scaleRgb);
            C3D_TexEnvScale(e, C3D_Alpha, p->scaleA);
            C3D_TexEnvColor(e, p->konst);
        }
    }
}

// `env`: the program's stages built already (the combiner-plan cache keeps them: --tevenv), else built here.
static void apply_tev(const PicaTev* t, const C3D_TexEnv* env) {
    C3D_TexEnv built[PICA_STAGES];
    if (!env) { build_env(t, built); env = built; }
    for (int i = 0; i < PICA_STAGES; i++) {
        if ((g_lean & LEAN_STAGES) && !memcmp(&g_c3d->texEnv[i], &env[i], sizeof env[i])) continue;
        *C3D_GetTexEnv(i) = env[i];
    }
    // C3D_TexEnvBufUpdate's bits: RGB in 8-11, alpha in 12-15.
    if ((g_lean & LEAN_TEVBUF) && ((g_c3d->texEnvBuf >> 8) & 0xFF) == ((t->bufRgb & 0xFu) | (t->bufA & 0xFu) << 4) &&
        g_c3d->texEnvBufClr == t->bufColor)
        return;
    C3D_TexEnvBufUpdate(C3D_RGB, t->bufRgb);
    C3D_TexEnvBufUpdate(C3D_Alpha, t->bufA);
    C3D_TexEnvBufColor(t->bufColor);
}

#define tev_key tev_pica_key

// Combiner plans per material: a TEV program is compiled once and found again by its registers (a direct-mapped cache),
// as decomp ports build each material's combiner setup once (Mario Kart 64 on 3DS). Games alternate between materials
// from draw to draw, which the one-entry check alone recompiled every time.
#define TEVC_BITS 9
typedef struct { u32 n, key[72]; PicaTev tev; bool envReady; C3D_TexEnv env[PICA_STAGES]; } TevSlot;
static TevSlot* g_tevCache;
// --tevenv on (--ab tevenv): each cached program keeps its stages as citro3d holds them (build_env once), so a change
// of program copies them instead of building them through citro3d's setters again (7,500 changes a second in the heavy
// area, 48 ms/s in update_tev: job 501).
static volatile bool g_tevEnvWant;
void pica_set_tevenv(bool on) { g_tevEnvWant = on; }
bool pica_tevenv(void) { return g_tevEnvWant; }

// The TEV program of registers k (tev_key's n words): from the cache, else compiled into it (or into *tmp without one).
// *slotOut: its cache slot (NULL without one).
static const PicaTev* tev_lookup(const GXCore* gx, const u32* k, u32 n, PicaTev* tmp, TevSlot** slotOut) {
    if (!g_tevCache) g_tevCache = calloc(1u << TEVC_BITS, sizeof *g_tevCache);
    u32 h = 2166136261u;
    for (u32 i = 0; i < n; i++) h = (h ^ k[i]) * 16777619u;
    TevSlot* slot = g_tevCache ? &g_tevCache[(h ^ (h >> 15)) & ((1u << TEVC_BITS) - 1)] : NULL;
    if (slotOut) *slotOut = slot;
    if (slot && slot->n == n && !memcmp(slot->key, k, n * 4)) return &slot->tev;
    tev_pica_compile(gx, tmp);
    g_picaStats.tevCompiles++;
    if (tmp->approx) note_approx(tmp->approx);
    if (!slot) return tmp;
    slot->n = n;
    memcpy(slot->key, k, n * 4);
    slot->tev = *tmp;
    slot->envReady = false;
    return &slot->tev;
}

// Recompiles the TEV program only when a register it depends on changed, and only when the cache does not hold it.
extern u32 g_gxTevVersion;  // fifo.c bp_write
static volatile bool g_tevVerWant = true;  // pica_set_tevver (--tevver, --ab tevver)
void pica_set_tevver(bool on) { g_tevVerWant = on; }
bool pica_tevver(void) { return g_tevVerWant; }
static u32 g_tevVerSeen, g_tevFmtsSeen;

static void update_tev(const GXCore* gx) {
    // Nothing the key reads changed since it was built (no TEV register write that changed a value, the same texture
    // formats): the program stands. (The formats come from TX_SETIMAGE0, which changes with every texture.)
    u32 fmts = 0;
    for (u8 map = 0; map < 8; map++) fmts |= ((gx->bp[0x88 + (map & 3) + (map >= 4 ? 0x20 : 0)] >> 20) & 15u) << (4 * map);
    if (g_tevVerWant && g.tevValid && g_tevVerSeen == g_gxTevVersion && g_tevFmtsSeen == fmts) return;
    g_tevVerSeen = g_gxTevVersion;
    g_tevFmtsSeen = fmts;
    u32 k[72], n = tev_key(gx, k);
    if (g.tevValid && !memcmp(k, g.tevKey, n * 4)) return;
    memcpy(g.tevKey, k, n * 4);
    g_picaStats.tevChanges++;
    PicaTev tmp;
    TevSlot* slot = NULL;
    g.tev = *tev_lookup(gx, k, n, &tmp, &slot);
    const C3D_TexEnv* env = NULL;
    if (g_tevEnvWant && slot) {
        if (!slot->envReady) { build_env(&slot->tev, slot->env); slot->envReady = true; }
        env = slot->env;
    }
    apply_tev(&g.tev, env);
    g.sel = (VSel){ g.tev.rasChan, g.tev.numTex, { g.tev.texCoord[0], g.tev.texCoord[1], g.tev.texCoord[2] } };
    g.tevValid = true;
}

// Depth, alpha test, blending / logic op, write masks and scissor (BP 0x40, 0x41, 0x43, 0xF3, 0x20/0x21).
static void apply_frag(const GXCore* gx) {
    u32 k[6] = { gx->bp[0x40], gx->bp[0x41], gx->bp[0x43] & 7, gx->bp[0xF3], gx->bp[0x20], gx->bp[0x21] };
    if (g.stateValid && !memcmp(k, g.fragKey, sizeof k)) return;
    memcpy(g.fragKey, k, sizeof k);
    g.stateValid = true;
    EffectGuard eg;
    effect_guard_open(&eg);
    u32 zmode = k[0], cmode = k[1];
    bool hasAlpha = k[2] == 1;  // RGBA6_Z24; the other formats have no destination alpha (kept at 255)
    int mask = 0;
    if (cmode & 8) mask |= GPU_WRITE_RED | GPU_WRITE_GREEN | GPU_WRITE_BLUE;
    if ((cmode & 16) && hasAlpha) mask |= GPU_WRITE_ALPHA;
    bool zen = (zmode & 1) && !(g_gpuDiag & 4);
    if (zen && (zmode & 16)) mask |= GPU_WRITE_DEPTH;
    C3D_DepthTest(zen, zen ? kFunc[(zmode >> 1) & 7] : GPU_ALWAYS, (GPU_WRITEMASK)mask);
    // Two GX alpha comparisons and a logic op reduced to the PICA's single comparison.
    u32 r = k[3], c0 = (r >> 16) & 7, c1 = (r >> 19) & 7, op = (r >> 22) & 3, r0 = r & 0xFF, r1 = (r >> 8) & 0xFF;
    if (op == 0 && c0 == 7 && c1 == 7) C3D_AlphaTest(false, GPU_ALWAYS, 0);
    else if ((op == 0 && c1 == 7) || (op == 1 && c1 == 0)) C3D_AlphaTest(true, kFunc[c0], (int)r0);
    else if ((op == 0 && c0 == 7) || (op == 1 && c0 == 0)) C3D_AlphaTest(true, kFunc[c1], (int)r1);
    else { note_approx(1u << 31); C3D_AlphaTest(true, kFunc[c0], (int)r0); }
    if ((cmode & 1) && !(g_gpuDiag & 2)) {
        static const GPU_BLENDFACTOR srcF[8] = { GPU_ZERO, GPU_ONE, GPU_DST_COLOR, GPU_ONE_MINUS_DST_COLOR, GPU_SRC_ALPHA,
                                                 GPU_ONE_MINUS_SRC_ALPHA, GPU_DST_ALPHA, GPU_ONE_MINUS_DST_ALPHA };
        static const GPU_BLENDFACTOR dstF[8] = { GPU_ZERO, GPU_ONE, GPU_SRC_COLOR, GPU_ONE_MINUS_SRC_COLOR, GPU_SRC_ALPHA,
                                                 GPU_ONE_MINUS_SRC_ALPHA, GPU_DST_ALPHA, GPU_ONE_MINUS_DST_ALPHA };
        if (cmode & (1 << 11)) {  // subtract: dst - src
            C3D_AlphaBlend(GPU_BLEND_REVERSE_SUBTRACT, GPU_BLEND_REVERSE_SUBTRACT, GPU_ONE, GPU_ONE, GPU_ONE, GPU_ONE);
        } else {
            GPU_BLENDFACTOR sf = srcF[(cmode >> 8) & 7], df = dstF[(cmode >> 5) & 7];
            C3D_AlphaBlend(GPU_BLEND_ADD, GPU_BLEND_ADD, sf, df, sf, df);
        }
    } else if (cmode & 2) {
        static const GPU_LOGICOP ops[16] = { GPU_LOGICOP_CLEAR, GPU_LOGICOP_AND, GPU_LOGICOP_AND_REVERSE, GPU_LOGICOP_COPY,
            GPU_LOGICOP_AND_INVERTED, GPU_LOGICOP_NOOP, GPU_LOGICOP_XOR, GPU_LOGICOP_OR, GPU_LOGICOP_NOR, GPU_LOGICOP_EQUIV,
            GPU_LOGICOP_INVERT, GPU_LOGICOP_OR_REVERSE, GPU_LOGICOP_COPY_INVERTED, GPU_LOGICOP_OR_INVERTED, GPU_LOGICOP_NAND,
            GPU_LOGICOP_SET };
        C3D_ColorLogicOp(ops[(cmode >> 12) & 15]);
    } else {
        C3D_ColorLogicOp(GPU_LOGICOP_COPY);
    }
    effect_guard_close(&eg);
    // Scissor: BP 0x20 top-left, 0x21 bottom-right, inclusive, offset by 342.
    int sx0 = (int)((k[4] >> 12) & 0x7FF) - 342, sy0 = (int)(k[4] & 0x7FF) - 342;
    int sx1 = (int)((k[5] >> 12) & 0x7FF) - 342, sy1 = (int)(k[5] & 0x7FF) - 342;
    if (sx0 <= 0 && sy0 <= 0 && sx1 >= EFB_W - 1 && sy1 >= EFB_H - 1) {
        C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    } else {
        int l = (int)floorf((float)sx0 * SX), rr = (int)ceilf((float)(sx1 + 1) * SX);
        int t = (int)floorf((float)sy0 * SY), b = (int)ceilf((float)(sy1 + 1) * SY);
        if (FLIP_Y) { int tt = TH - b; b = TH - t; t = tt; }
        l = l < 0 ? 0 : l > TW ? TW : l; rr = rr < l ? l : rr > TW ? TW : rr;
        t = t < 0 ? 0 : t > TH ? TH : t; b = b < t ? t : b > TH ? TH : b;
        C3D_SetScissor(GPU_SCISSOR_NORMAL, (u32)l, (u32)t, (u32)rr, (u32)b);
    }
    if (g_gpuDiag & 8) C3D_SetScissor(GPU_SCISSOR_NORMAL, 0, 0, 16, 16);
}

// ---- vertices ----
// Z textures (BP 0xF4 bias, 0xF5 bits 0-1 texel format U8/U16/U24, bits 2-3 op: 1 add, 2 replace) give each pixel
// a depth taken from the last texture the TEV sampled. The PICA cannot write depth from a texture, but games use
// them mostly with uniform textures: Twilight Princess resets its depth buffer every frame with ~650 quads that
// replace depth from a 4x4 texture, and without this every later draw failed the depth test (only the sky showed).
// A uniform texture's value goes into the vertices' depth; a non-uniform one uses its first texel (approximation).
static u32 g_ztexKey[5], g_ztexValue;
static bool g_ztexUniform;

// The depth a draw's Z texture gives its vertices (op 1: add, 2: replace); false when it has none.
static bool ztex_value(System* s, const GXCore* gx, u32* opOut, float* zOut) {
    u32 mode = gx->bp[0xF5], op = (mode >> 2) & 3;
    if (!op || op == 3) return false;
    // The last enabled texture stage's map.
    u32 stages = ((gx->bp[0x00] >> 10) & 15) + 1, map = 8;
    for (u32 st = 0; st < stages; st++) {
        u32 tref = gx->bp[0x28 + st / 2] >> ((st & 1) * 12);
        if ((tref >> 6) & 1) map = tref & 7;
    }
    if (map == 8) return false;
    u32 hi = map >= 4 ? 0x20 : 0, m = map & 3;
    u32 img0 = gx->bp[0x88 + m + hi], img3 = gx->bp[0x94 + m + hi], tlut = gx->bp[0x98 + m + hi];
    u32 w = (img0 & 0x3FF) + 1, h = ((img0 >> 10) & 0x3FF) + 1, fmt = (img0 >> 20) & 15, addr = gx_tex_addr(gx, map, img3);
    u32 key[5] = { img0, img3, tlut, mode, g.xfbSeq };
    if (memcmp(key, g_ztexKey, sizeof key)) {
        memcpy(g_ztexKey, key, sizeof key);
        u32 n = w * h, bytes = gx_texture_size(fmt, w, h);
        if (!scratch_reserve(n)) { g_picaStats.texFails++; return false; }
        if (!bytes || !addr || addr < PA_MEM1_BEGIN || addr + bytes > PA_MEM1_END) return false;
        u32 tlutAddr = (tlut & 0x3FF) << 9;
        gx_decode_texture(s->mem1 + addr, fmt, w, h, gx->tmem + (tlutAddr & 0xFFFFF), (tlut >> 10) & 3, g_scratch);
        g_ztexUniform = true;
        for (u32 i = 1; i < n; i++) if (g_scratch[i] != g_scratch[0]) { g_ztexUniform = false; break; }
        u32 c = g_scratch[0], r = c >> 24, gg = (c >> 16) & 0xFF, b = (c >> 8) & 0xFF, a = c & 0xFF;
        g_ztexValue = (mode & 3) == 0 ? a : (mode & 3) == 1 ? (a << 8) | r : (r << 16) | (gg << 8) | b;
        if (!g_ztexUniform) note_approx(1u << 28);
    }
    u32 zt = g_ztexValue + (gx->bp[0xF4] & 0xFFFFFF);
    *zOut = (float)(op == 2 ? zt & 0xFFFFFF : zt) * (1.0f / 16777216.0f);  // replace wraps to 24 bits
    *opOut = op;
    return true;
}

static void ztex_apply(PVert* pv, u32 count, u32 op, float z) {
    for (u32 i = 0; i < count; i++) {
        PVert* o = &pv[i];
        if (op == 2) o->pos[2] = -z * o->pos[3];
        else o->pos[2] -= z * o->pos[3];
    }
}

// A vertex given in EFB screen space (lines and points, expanded on the CPU).
static void to_pica_screen(const VFold* f, const SVert* v, float sx, float sy, PVert* o) {
    o->pos[0] = sx * f->kx - 1.0f;
    o->pos[1] = FLIP_Y ? 1.0f - sy * f->ky : sy * f->ky - 1.0f;
    o->pos[2] = -v->sz * (1.0f / 16777216.0f);
    o->pos[3] = 1.0f;
    vfast_attribs(&g.sel, v, o);
}

static u32 g_cull;
static float (*g_nd)[2];  // NDC x, y per vertex for culling (w > 0 only)
static u8* g_jobMem;      // the GX thread's own vertex job (GPU-transformed draws without the worker)
static u32 g_jobCap;

// GX cull mode (BP 0x00 bits 14-15) -> the PICA's, for GPU-transformed batches. The CPU rule below rejects mode 1 at
// negative NDC area (clockwise with y up) and mode 2 at positive area (counter-clockwise); mode 3 draws nothing.
static const GPU_CULLMODE kCull[4] = { GPU_CULL_NONE, GPU_CULL_BACK_CCW, GPU_CULL_FRONT_CCW, GPU_CULL_NONE };

static void cull_ndc(u32 count) {
    if (g_cull != 1 && g_cull != 2) return;
    for (u32 i = 0; i < count; i++) {
        float w = g_pv[i].pos[3], iw = w > 0 ? 1.0f / w : 0;
        g_nd[i][0] = g_pv[i].pos[0] * iw; g_nd[i][1] = g_pv[i].pos[1] * iw;
    }
}

// Culling with the reference renderer's rule: only triangles entirely in front of the eye, by screen-space winding
// (NDC is EFB pixels scaled by positive factors, mirrored in y when FLIP_Y).
static void tri(u32 a, u32 b, u32 c) {
    if (g_cull == 3) return;
    const PVert *va = &g_pv[a], *vb = &g_pv[b], *vc = &g_pv[c];
    if (g_cull && va->pos[3] > 0 && vb->pos[3] > 0 && vc->pos[3] > 0) {
        const float *na = g_nd[a], *nb = g_nd[b], *nc = g_nd[c];
        float area = (nb[0] - na[0]) * (nc[1] - na[1]) - (nc[0] - na[0]) * (nb[1] - na[1]);
        if (FLIP_Y) area = -area;
        if (area == 0 || (g_cull == 1 && area < 0) || (g_cull == 2 && area > 0)) return;
    }
    PVert* o = g.arena + g.arenaUsed;
    o[0] = *va; o[1] = *vb; o[2] = *vc;
    g.arenaUsed += 3;
}

static void quad_screen(const VFold* f, const SVert* a, const SVert* b, const float xy[4][2]) {
    PVert q[4];
    for (int i = 0; i < 4; i++) to_pica_screen(f, i == 1 || i == 2 ? b : a, xy[i][0], xy[i][1], &q[i]);
    PVert* o = g.arena + g.arenaUsed;
    o[0] = q[0]; o[1] = q[1]; o[2] = q[2]; o[3] = q[0]; o[4] = q[2]; o[5] = q[3];
    g.arenaUsed += 6;
}

// Lines and points as screen-space quads, sized like the reference renderer (BP 0x22, sixths of a pixel).
static void line(const GXCore* gx, const VFold* f, const SVert* a, const SVert* b) {
    if (a->w <= 0 || b->w <= 0) return;
    float hw = (float)(gx->bp[0x22] & 0xFF) / 12.0f;
    bool horiz = fabsf(b->sx - a->sx) >= fabsf(b->sy - a->sy);
    float xy[4][2] = { { a->sx, a->sy }, { b->sx, b->sy }, { b->sx, b->sy }, { a->sx, a->sy } };
    for (int i = 0; i < 4; i++) { float d = i < 2 ? -hw : hw; if (horiz) xy[i][1] += d; else xy[i][0] += d; }
    quad_screen(f, a, b, xy);
}

static void point(const GXCore* gx, const VFold* f, const SVert* p) {
    if (p->w <= 0) return;
    float h = (float)((gx->bp[0x22] >> 8) & 0xFF) / 12.0f;
    float xy[4][2] = { { p->sx - h, p->sy - h }, { p->sx + h, p->sy - h }, { p->sx + h, p->sy + h }, { p->sx - h, p->sy + h } };
    quad_screen(f, p, p, xy);
}

enum { PRIM_QUADS = 0, PRIM_QUADS2 = 1, PRIM_TRIS = 2, PRIM_STRIP = 3, PRIM_FAN = 4, PRIM_LINES = 5, PRIM_LSTRIP = 6, PRIM_POINTS = 7 };

// Appends a draw's triangles (g_pv, culled by g_cull) or line / point quads (g_sv) to the arena.
static void assemble(const GXCore* gx, const VFold* f, u32 prim, u32 count) {
    switch (prim) {
    case PRIM_QUADS: case PRIM_QUADS2:
        for (u32 i = 0; i + 3 < count; i += 4) { tri(i, i + 1, i + 2); tri(i, i + 2, i + 3); }
        break;
    case PRIM_TRIS: for (u32 i = 0; i + 2 < count; i += 3) tri(i, i + 1, i + 2); break;
    case PRIM_STRIP: for (u32 i = 0; i + 2 < count; i++) { if (i & 1) tri(i + 1, i, i + 2); else tri(i, i + 1, i + 2); } break;
    case PRIM_FAN: for (u32 i = 1; i + 1 < count; i++) tri(0, i, i + 1); break;
    case PRIM_LINES: for (u32 i = 0; i + 1 < count; i += 2) line(gx, f, &g_sv[i], &g_sv[i + 1]); break;
    case PRIM_LSTRIP: for (u32 i = 0; i + 1 < count; i++) line(gx, f, &g_sv[i], &g_sv[i + 1]); break;
    case PRIM_POINTS: for (u32 i = 0; i < count; i++) point(gx, f, &g_sv[i]); break;
    default: break;
    }
}

// Everything a draw's GPU state depends on: TEV, fragment state, the used texture maps' registers, and what decides
// whether a texture is re-read from RAM. Draws with equal keys share a batch.
static u32 batch_key(const GXCore* gx, u32* k) {
    u32 n = tev_key(gx, k);
    k[n++] = gx->bp[0x40]; k[n++] = gx->bp[0x41]; k[n++] = gx->bp[0x43] & 7; k[n++] = gx->bp[0xF3];
    k[n++] = gx->bp[0x20]; k[n++] = gx->bp[0x21];
    k[n++] = g.xfbSeq; k[n++] = gx->tlutVersion;
    k[n++] = gx->xfLightVersion;  // the shader lighting uniforms
    return n;
}

// Shader lighting state for the batch being opened: whether the shader lights it (lon.z, tested with cmp in the
// shader; a bool uniform set with C3D_BoolUnifSet never reached it in Azahar), and its uniforms.
// lon: (colour half lit, alpha half lit, the shader lights, the shader transforms).
static void set_lon(bool gpuPos) {
    const VLightUniforms* u = &g.lu;
    if (g.batchGpuLight) unif_set(g.uOn, u->on[0], u->on[1], 1.0f, gpuPos ? 1.0f : 0.0f);
    else unif_set(g.uOn, 0, 0, 0, gpuPos ? 1.0f : 0.0f);
}

static void apply_light_uniforms(void) {
    cull_face(g.batchGpuPos ? g.batchCull : GPU_CULL_NONE);
    if (!g.batchGpuLight) { set_lon(g.batchGpuPos); return; }
    const VLightUniforms* u = &g.lu;
    unif_rows(g.uLights, u->lights, 40);
    unif_rows(g.uSel, u->sel, 4);
    unif_rows(g.uMat, &u->mat, 1);
    unif_rows(g.uAmb, &u->amb, 1);
    unif_rows(g.uVsel, &u->vsel, 1);
    set_lon(g.batchGpuPos);
}

// ---- position transform in the vertex shader ----
// GPU-transformed batches: draws the batch's vertices since batchFirst, so that the projection and palette slots can
// be replaced; the batch's other GPU state stays. Each draw's vertices have a fixed place in the arena (the GPU culls),
// so this never waits for the vertex worker.
static void flush_sub(void) {
    if (g.batchIdx && g.idxUsed > g.batchIdxFirst) {
        draw_elements(g.batchIdxFirst, g.idxUsed - g.batchIdxFirst);
        g_picaStats.subDraws++;
    } else if (!g.batchIdx && g.arenaUsed > g.batchFirst) {
        draw_arrays(g.batchFirst, g.arenaUsed - g.batchFirst);
        g_picaStats.subDraws++;
    }
    g.batchFirst = g.arenaUsed;
    g.batchIdxFirst = g.idxUsed;
    g_recJob = g_vwHead;
    g.palSerial++;
}

static void pal_upload(u32 k, const float* m) {
    g.natRowValid &= ~(7ull << (3 * k));  // rows a converted draw may have left there
    memcpy(g.pal[k], m, sizeof g.pal[k]);
    C3D_FVec* u = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, g.uPal + 3 * (int)k, 3);
    for (int r = 0; r < 3; r++) u[r] = FVec4_New(m[4 * r], m[4 * r + 1], m[4 * r + 2], m[4 * r + 3]);
    g.palValid |= 1u << k;
    g.palGen[k]++;
    g_picaStats.palUploads++;
}

static int pal_find(const float* m) {
    if (((g.palValid >> g.palLast) & 1) && !memcmp(g.pal[g.palLast], m, sizeof g.pal[0])) return (int)g.palLast;
    for (u32 k = 0; k < PAL_SLOTS; k++)
        if (((g.palValid >> k) & 1) && !memcmp(g.pal[k], m, sizeof g.pal[0])) return (int)k;
    return -1;
}

// Gives the job's position matrices palette slots (a slot holding the same matrix, else the next free one round the
// palette) and the shader the job's projection, drawing the open sub-batch first when a slot it uses or the projection
// must change. False when the job has more matrices than the palette.
static bool pal_assign(VJob* job) {
    const float (*raw)[21];
    u32 n = vfast_job_matrices(job, &raw);
    if (n > PAL_SLOTS) return false;
    const float* P = vfast_job_proj(job);
    if (!g.projValid || memcmp(P, g.proj, sizeof g.proj)) {
        flush_sub();
        memcpy(g.proj, P, sizeof g.proj);
        C3D_FVec* u = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, g.uProj, 4);
        for (int r = 0; r < 4; r++) u[r] = FVec4_New(P[4 * r], P[4 * r + 1], P[4 * r + 2], P[4 * r + 3]);
        g.projValid = true;
    }
    // Fast path (most of TP's draws): one matrix, the previous job's, same origin, in a slot that still holds it.
    const float* o = vfast_job_origin(job, NULL);
    if (n == 1 && g.lastValid && g.palGen[g.lastSlot] == g.lastGen && !memcmp(raw[0], g.lastRaw, sizeof g.lastRaw) &&
        !memcmp(o, g.lastOrigin, sizeof g.lastOrigin)) {
        u8 s1 = (u8)g.lastSlot;
        g.palUse[s1] = g.palSerial;
        vfast_job_gpu_pos(job, &s1);
        return true;
    }
    float pos[PAL_SLOTS][12];  // the matrices with the job's origin folded in: what the palette holds
    vfast_job_origin(job, pos);
    int slot[PAL_SLOTS];
    u32 misses = 0, busy = 0;
    for (u32 i = 0; i < n; i++) {
        slot[i] = pal_find(pos[i]);
        if (slot[i] < 0) misses++;
        else g.palUse[slot[i]] = g.palSerial;
    }
    if (misses) {
        for (u32 k = 0; k < PAL_SLOTS; k++) busy += g.palUse[k] == g.palSerial;
        if (misses > PAL_SLOTS - busy) {  // no room beside the open sub-batch's matrices: draw it, keep this job's
            flush_sub();
            for (u32 i = 0; i < n; i++) if (slot[i] >= 0) g.palUse[slot[i]] = g.palSerial;
        }
        for (u32 i = 0; i < n; i++) {
            if (slot[i] >= 0) continue;
            u32 k = g.palNext;
            while (g.palUse[k] == g.palSerial) k = (k + 1) % PAL_SLOTS;
            pal_upload(k, pos[i]);
            g.palUse[k] = g.palSerial;
            g.palNext = (k + 1) % PAL_SLOTS;
            slot[i] = (int)k;
        }
    }
    u8 slots8[PAL_SLOTS];
    for (u32 i = 0; i < n; i++) slots8[i] = (u8)slot[i];
    if (n) g.palLast = (u32)slot[n - 1];
    g.lastValid = n == 1;
    if (n == 1) {
        memcpy(g.lastRaw, raw[0], sizeof g.lastRaw);
        memcpy(g.lastOrigin, o, sizeof g.lastOrigin);
        g.lastSlot = (u32)slot[0];
        g.lastGen = g.palGen[slot[0]];
    }
    vfast_job_gpu_pos(job, slots8);
    return true;
}

static void tex_key(const GXCore* gx, u32* k, u32* n) {
    for (u32 i = 0; i < g.tev.numTex && i < PICA_TEXUNITS; i++) {
        u32 map = g.tev.texMap[i] & 7, hi = map >= 4 ? 0x20 : 0, m = map & 3;
        k[(*n)++] = gx->bp[0x80 + m + hi]; k[(*n)++] = gx->bp[0x88 + m + hi];
        k[(*n)++] = gx->bp[0x94 + m + hi]; k[(*n)++] = gx->bp[0x98 + m + hi];
    }
}

// ---- vertex worker (core 3) ----
// The second half of each triangle draw's vertex work runs on core 3 while this thread goes on parsing: vfast_finish,
// the Z-texture depth, culling and triangle assembly. The worker appends the triangles it keeps to the arena at
// g_vwPos, in queue order; this thread syncs with it (vw_sync: wait, then take g_vwPos as arenaUsed) before it records
// a batch's draw call, writes the arena itself, or hands the arena to the GPU. A batch holds ~90 of Twilight
// Princess's draws, so the worker keeps the overlap within a batch. (2DS, 7f2033e: writing culled triangles as
// degenerate ones instead, to fix each draw's place when queued, tripled the GPU's time per frame: the vertex shader
// still runs for every vertex.) Jobs live in a ring of memory the worker frees in order, small enough to stay in the
// L2 cache (a 4 MB ring made the GX thread's vertex time grow). With draw merging a job is ~28 draws (~140 vertices,
// ~30 KB), and 256 KB made the GX thread wait for ring space 17% of the time (2DS, f56d430) while core 3 was half idle:
// the ring's size is a run option (pica_set_job_ring, up to VW_MEM).
#define VW_SLOTS 1024
#ifndef VW_MEM  // native mode raises it: one merged model draw of Aurora's is ~2,000 vertices, several hundred KB of job
#define VW_MEM (1024u << 10)
#endif
static u32 g_vwLimit = 128u << 10;              // the ring's size in use: shallow keeps syncs short (2DS, relay 76-80)
static volatile u32 g_vwLimitWant = 128u << 10;  // pica_set_job_ring: applied when the ring is empty
typedef struct {
    VJob* job;
    PVert* pv;         // the draw's vertices (colours from vfast_prepare)
    float (*nd)[2];    // NDC x, y per vertex for culling
    u32 memStart, prim, count, cull, ztexOp;
    float ztexZ;
    bool fixed;        // GPU-transformed: every triangle is kept, written at arena index dst (the GPU culls)
    u32 dst;
    bool deferred;     // the worker gathers the attributes from guest memory (vfast_prepare defer)
    bool indexed;      // vertices at arena index dst, triangles as indices at ibase (vw_do_indexed); vrel: dst - vbase
    u32 ibase;
    u16 vrel;
    const u32* seg;    // a merged draw (draw merging below): its draws as prim << 16 | count, in job memory
    u32 nSeg;
    const u16* tri;    // an index-list draw (pica_draw_indexed): its triangles' vertex numbers, in job memory
    u32 nTri;
} VWork;
static VWork g_vw[VW_SLOTS];
static VCache *g_vcGx, *g_vcWorker;  // display-list vertex caches (vfast.c): one per gathering thread
static u8* g_vwMem;
static u32 g_vwMemHead;
static VwLine g_vwPosL;  // written by the worker (CPU-culled draws), read by the GX thread
#define g_vwPos g_vwPosL.v
static volatile u32 g_vwQuit;
// The worker also gathers shader-lit draws from guest memory (vfast_prepare defer) when they are GPU-transformed:
// their batches never wait for the worker. For CPU-transformed draws it measured slower on the 2DS (6673eb2: 14.2%
// against 14.8%), because every batch flush then waits for the worker (vwait 18%).
static const bool kVwGather = false;  // CPU-transformed draws
static VwLine g_vwDeferEndL;  // the queue index after the last queued draw that reads guest memory
#define g_vwDeferEnd g_vwDeferEndL.v
static Thread g_vwThread, g_vwHelper;
static bool g_vwOn = true;  // pica_set_vertex_worker
static inline void vw_dmb(void) { __asm__ volatile("mcr p15, 0, %0, c7, c10, 5" : : "r"(0) : "memory"); }

static u32 tri_verts(u32 prim, u32 count) {
    switch (prim) {
    case PRIM_QUADS: case PRIM_QUADS2: return count / 4 * 6;
    case PRIM_TRIS: return count / 3 * 3;
    case PRIM_STRIP: case PRIM_FAN: return count >= 3 ? (count - 2) * 3 : 0;
    default: return 0;
    }
}

// tri() for the worker: appends the triangle at *o unless it is culled.
static void vw_tri(const VWork* w, PVert** o, u32 a, u32 b, u32 c) {
    if (w->cull == 3) return;
    const PVert *va = &w->pv[a], *vb = &w->pv[b], *vc = &w->pv[c];
    if (w->cull && va->pos[3] > 0 && vb->pos[3] > 0 && vc->pos[3] > 0) {
        const float *na = w->nd[a], *nb = w->nd[b], *nc = w->nd[c];
        float area = (nb[0] - na[0]) * (nc[1] - na[1]) - (nc[0] - na[0]) * (nb[1] - na[1]);
        if (FLIP_Y) area = -area;
        if (area == 0 || (w->cull == 1 && area < 0) || (w->cull == 2 && area > 0)) return;
    }
    PVert* p = *o;
    p[0] = *va; p[1] = *vb; p[2] = *vc;
    *o = p + 3;
}

// An indexed draw (w->indexed): the vertices go to the arena at w->dst, the triangles to the index arena at w->ibase
// as indices relative to the vertex buffer's start (w->vrel is dst's). A culled triangle repeats its first vertex,
// which the GPU's vertex cache already holds, so it costs no vertex work and draws nothing; every draw's ranges are
// known when it is queued, so no batch waits for the worker.
static inline void vw_tri_idx(const VWork* w, const PVert* pv, u16** ix, u32 a, u32 b, u32 c) {
    u16* p = *ix;
    u16 base = w->vrel;
    if (w->cull) {
        const PVert *va = &pv[a], *vb = &pv[b], *vc = &pv[c];
        if (va->pos[3] > 0 && vb->pos[3] > 0 && vc->pos[3] > 0) {
            const float *na = w->nd[a], *nb = w->nd[b], *nc = w->nd[c];
            float area = (nb[0] - na[0]) * (nc[1] - na[1]) - (nc[0] - na[0]) * (nb[1] - na[1]);
            if (FLIP_Y) area = -area;
            if (area == 0 || (w->cull == 1 && area < 0) || (w->cull == 2 && area > 0)) b = c = a;
        }
    }
    p[0] = (u16)(base + a); p[1] = (u16)(base + b); p[2] = (u16)(base + c);
    *ix = p + 3;
}

// The triangles of one draw whose vertices start at job vertex `at`.
static void vw_assemble_idx(const VWork* w, const PVert* pv, u16** ix, u32 prim, u32 at, u32 count) {
    switch (prim) {
    case PRIM_QUADS: case PRIM_QUADS2:
        for (u32 i = at; i + 3 < at + count; i += 4) { vw_tri_idx(w, pv, ix, i, i + 1, i + 2); vw_tri_idx(w, pv, ix, i, i + 2, i + 3); }
        break;
    case PRIM_TRIS: for (u32 i = at; i + 2 < at + count; i += 3) vw_tri_idx(w, pv, ix, i, i + 1, i + 2); break;
    case PRIM_STRIP:
        for (u32 i = 0; i + 2 < count; i++) {
            if (i & 1) vw_tri_idx(w, pv, ix, at + i + 1, at + i, at + i + 2);
            else vw_tri_idx(w, pv, ix, at + i, at + i + 1, at + i + 2);
        }
        break;
    case PRIM_FAN: for (u32 i = 1; i + 1 < count; i++) vw_tri_idx(w, pv, ix, at, at + i, at + i + 1); break;
    default: break;
    }
}

static void vw_do_indexed(const VWork* w) {
    u32 count = w->count;
    PVert* pv = g.arena + w->dst;
    if (w->deferred) {
        vfast_finish(w->job, pv);
    } else {  // vfast_prepare put the colours in the job's own vertices
        vfast_finish(w->job, w->pv);
        memcpy(pv, w->pv, count * sizeof *pv);
    }
    if (w->ztexOp) ztex_apply(pv, count, w->ztexOp, w->ztexZ);
    if (w->cull == 1 || w->cull == 2)
        for (u32 i = 0; i < count; i++) {
            float wv = pv[i].pos[3], iw = wv > 0 ? 1.0f / wv : 0;
            w->nd[i][0] = pv[i].pos[0] * iw; w->nd[i][1] = pv[i].pos[1] * iw;
        }
    u16* ix = g.idx + w->ibase;
    if (w->tri) {  // every triangle written (a bad index as a degenerate one): the batch counted 3 per triangle
        for (u32 t = 0; t < w->nTri; t++) {
            u32 a = w->tri[3 * t], b = w->tri[3 * t + 1], c = w->tri[3 * t + 2];
            if (a >= count || b >= count || c >= count) a = b = c = 0;
            vw_tri_idx(w, pv, &ix, a, b, c);
        }
        return;
    }
    if (!w->nSeg) { vw_assemble_idx(w, pv, &ix, w->prim, 0, count); return; }
    for (u32 k = 0, at = 0; k < w->nSeg; k++) {
        u32 c = w->seg[k] & 0xFFFF;
        vw_assemble_idx(w, pv, &ix, w->seg[k] >> 16, at, c);
        at += c;
    }
}

static void vw_do(const VWork* w) {
    if (w->indexed) { vw_do_indexed(w); return; }
    u32 count = w->count;
    vfast_finish(w->job, w->pv);
    if (w->ztexOp) ztex_apply(w->pv, count, w->ztexOp, w->ztexZ);
    if (w->cull == 1 || w->cull == 2)
        for (u32 i = 0; i < count; i++) {
            float wv = w->pv[i].pos[3], iw = wv > 0 ? 1.0f / wv : 0;
            w->nd[i][0] = w->pv[i].pos[0] * iw; w->nd[i][1] = w->pv[i].pos[1] * iw;
        }
    PVert* o = g.arena + (w->fixed ? w->dst : g_vwPos);
    switch (w->prim) {
    case PRIM_QUADS: case PRIM_QUADS2:
        for (u32 i = 0; i + 3 < count; i += 4) { vw_tri(w, &o, i, i + 1, i + 2); vw_tri(w, &o, i, i + 2, i + 3); }
        break;
    case PRIM_TRIS: for (u32 i = 0; i + 2 < count; i += 3) vw_tri(w, &o, i, i + 1, i + 2); break;
    case PRIM_STRIP:
        for (u32 i = 0; i + 2 < count; i++) { if (i & 1) vw_tri(w, &o, i + 1, i, i + 2); else vw_tri(w, &o, i, i + 1, i + 2); }
        break;
    case PRIM_FAN: for (u32 i = 1; i + 1 < count; i++) vw_tri(w, &o, 0, i, i + 1); break;
    default: break;
    }
    if (!w->fixed) g_vwPos = (u32)(o - g.arena);
}

// Jobs are taken in queue order (g_vwClaim) and may finish out of it with two workers: each marks itself done, and
// g_vwTail (what every wait and the ring's memory follow) moves over the done jobs in order.
static VwLine g_vwClaimL;
#define g_vwClaim g_vwClaimL.v
static volatile u32 g_vwDone[VW_SLOTS];  // job index + 1 once that job is done
static volatile bool g_vw2;              // the second worker takes jobs
u64 g_vw2Jobs;
void pica_set_worker2(bool on) { g_vw2 = on; }
bool pica_worker2(void) { return g_vw2; }
static bool vw_claim(u32* out, bool helper) {
    for (;;) {
        u32 c = g_vwClaim;
        if (c == g_vwHead) return false;
        vw_dmb();
        if (helper) {
            const VWork* w = &g_vw[c % VW_SLOTS];
            if (!w->indexed && !w->fixed) return false;  // CPU-culled: the first worker's, in order
        }
        if (__sync_bool_compare_and_swap(&g_vwClaim, c, c + 1)) { *out = c; vw_dmb(); return true; }
    }
}
static void vw_done(u32 c) {
    vw_dmb();
    g_vwDone[c % VW_SLOTS] = c + 1;
    vw_dmb();
    for (;;) {
        u32 t = g_vwTail;
        if (t == g_vwHead || g_vwDone[t % VW_SLOTS] != t + 1) return;
        __sync_bool_compare_and_swap(&g_vwTail, t, t + 1);
    }
}

static volatile bool g_vwSteal;
u64 g_vwStolen, g_vwStealEmpty, g_vwStealKind;  // jobs run; attempts that found no unclaimed job / a CPU-culled one
void pica_set_steal(bool on) { g_vwSteal = on; }
bool pica_steal(void) { return g_vwSteal; }
// The GX thread, about to wait for the worker: runs the next queued job if it may finish out of order. false: none.
static bool vw_steal(void) {
    u32 c;
    if (!g_vwSteal) return false;
    if (!vw_claim(&c, true)) {
        if (g_vwClaim == g_vwHead) g_vwStealEmpty++; else g_vwStealKind++;
        return false;
    }
    if (g_vw[c % VW_SLOTS].job) vfast_job_set_cache(g_vw[c % VW_SLOTS].job, NULL);  // (the worker's cache is its own)
    vw_do(&g_vw[c % VW_SLOTS]);
    g_vwStolen++;
    vw_done(c);
    return true;
}

volatile u8 g_vwBusyNow;  // the worker is running a job (jitsample.c's joint states)
static void vw_main(void* arg) {
    (void)arg;
    crashlog_thread(CRASH_VWORKER);
    __asm__ volatile("vmsr fpscr, %0" : : "r"(0x03000000u));  // as the GX thread: round to nearest, flush to zero
    u32 idle = 0, jobs = 0, c;
    while (!g_vwQuit) {
        if (vw_claim(&c, false)) {
            bool timed = !(++jobs & 63);  // timed on one job in 64 (a clock read costs 0.25 us)
            u64 t0 = timed && g_profClock ? g_profClock() : 0;
            g_vfPhaseTimed = timed;
            if (g_vw[c % VW_SLOTS].job) vfast_job_set_cache(g_vw[c % VW_SLOTS].job, g_vcWorker);  // gathers here use the worker's cache
            g_vwBusyNow = 1;
            vw_do(&g_vw[c % VW_SLOTS]);
            g_vwBusyNow = 0;
            g_vfPhaseTimed = false;
            if (timed && g_profClock) { u64 d = g_profClock() - t0; g_hostProf[PROF_VWORK] += d * 64; g_vwPhaseAll += d; }
            vw_done(c);
            idle = 0;
        } else if (++idle > 4000) {
            svcSleepThread(20000);
        }
    }
}

// The second vertex worker (pica_set_worker2, --vw2): on core 1, which the app gets at 80% (APT_SetAppCpuTimeLimit)
// and which only runs the log writer and samplers. It takes indexed and fixed-place jobs only, whose arena ranges were
// reserved when queued, so it can finish them out of order; CPU-culled jobs append at g_vwPos and stay with the first
// worker, in order. Without the display-list cache (its gathers read guest RAM directly).
static void vw_helper(void* arg) {
    (void)arg;
    crashlog_thread(CRASH_VWORKER);
    __asm__ volatile("vmsr fpscr, %0" : : "r"(0x03000000u));
    u32 c;
    while (!g_vwQuit) {
        if (g_vw2 && vw_claim(&c, true)) {
            if (g_vw[c % VW_SLOTS].job) vfast_job_set_cache(g_vw[c % VW_SLOTS].job, NULL);
            vw_do(&g_vw[c % VW_SLOTS]);
            g_vw2Jobs++;
            vw_done(c);
        } else {
            // No spinning: polling the queue's counters, which the GX thread keeps writing, pulled their cache lines
            // between cores on every read, and core 0 ran 3-7% slower with the helper on (job 125).
            svcSleepThread(g_vw2 ? 30000 : 1000000);
        }
    }
}

// True when the worker has no queued draw (it gathers shader-lit draws from guest memory, so the CPU thread must not
// run on past a PE signal or a GX sync while one is queued).
bool pica_vertex_worker_idle(void) {
    bool idle = (s32)(g_vwTail - g_vwDeferEnd) >= 0;  // past the last draw that reads guest memory
    vw_dmb();
    return idle;
}

static bool g_vwFixed;  // fixed-place (GPU-transformed) draws queued since the last vw_sync_all

static void vw_wait(void) {
    if (g_vwTail != g_vwHead) {
        u64 t0 = prof_begin();
        GW_ENTER(GW_VWWAIT);
        while (g_vwTail != g_vwHead) vw_steal();
        GW_LEAVE();
        prof_end(PROF_VWAIT, t0);
    }
    vw_dmb();
}

// The worker has at most 4 draws queued (a split now waits for a few microseconds at most).
static bool vw_nearly_idle(void) {
    bool idle = g_vwHead - g_vwTail <= 4;
    vw_dmb();
    return idle;
}

// Waits for the worker's variable-length (CPU-culled) draws, and takes over the arena after them. Fixed-place draws
// need no wait here: their arena ranges were reserved when queued.
static void vw_sync(void) {
    if (!g_vwReserved) return;
    vw_wait();
    g.arenaUsed = g_vwPos;
    g_vwReserved = 0;
}

// Waits until the worker has done every queued draw (before the GPU reads the arena).
static void vw_sync_all(void) {
    vw_sync();
    if (g_vwFixed) { vw_wait(); g_vwFixed = false; }
}

// Splits the frame (see SPLIT_VERTS) at the draw calls recorded so far: the open batch stays open, and only the vertex
// worker's jobs for recorded draws must be done, which are older than the open batch's and nearly always finished.
// (Splitting at a closed batch waited for every queued job, so it was only done with <= 4 jobs queued; with draw
// merging a job is ~28 draws, the gate rarely opened, and splits fell from 67 to 49-55/s while the GX thread's wait for
// the GPU rose from 1.7% to 6-7%: 2DS, 9388639.)
#define NS_SPLIT (3u * SPLIT_VERTS)
static void split_early(void) {
    if (g_dbl && g.queued + 8 > GXQUEUE_BUDGET && g.frame) q_clear_if_idle();
    if (!g_earlySplit || !g_q || !g.frame || (g.arenaUsed - g.splitArena < SPLIT_VERTS && g_nsSince < NS_SPLIT) ||
        g.queued + 8 > GXQUEUE_BUDGET)
        return;
    if (!g_splitOpen || !g.batchOpen || g_vwReserved) {  // the whole batch: only when the worker is nearly idle
        if (vw_nearly_idle()) split(0);
        return;
    }
    s32 behind = (s32)(g_recJob - g_vwTail);
    if (behind > 1) return;  // more than the job in progress outstanding: the next job tries again
    if (behind > 0) {
        u64 t0 = prof_begin();
        GW_ENTER(GW_SPLITWAIT);
        while ((s32)(g_vwTail - g_recJob) < 0) {}
        GW_LEAVE();
        prof_end(PROF_VWAIT, t0);
    }
    vw_dmb();
    u32 arenaEnd = g.batchFirst, idxEnd = g.batchIdxFirst;
    if (arenaEnd > g.arenaFlushed)
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)(g.arena + g.arenaFlushed), (arenaEnd - g.arenaFlushed) * sizeof(PVert));
    if (idxEnd > g.idxFlushed)
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)(g.idx + g.idxFlushed), (idxEnd - g.idxFlushed) * 2);
    if (arenaEnd > g.arenaFlushed) g.arenaFlushed = arenaEnd;
    if (idxEnd > g.idxFlushed) g.idxFlushed = idxEnd;
    verify_queue(true);
    GW_ENTER(GW_SPLIT);
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
    GW_LEAVE();
    verify_queue(false);
    g_picaStats.splits++;
    g.queued += 1;
    g.splitArena = g.arenaUsed;
    g_nsSince = 0;
    queue_run();
}

u64 g_vwStats[5];
u64 g_vwPhaseAll;  // the worker's timed jobs in all (g_vfPhase splits vfast_finish's part of it)
static volatile bool g_vwTrim = true;
void pica_set_vw_trim(bool on) { g_vwTrim = on; }
bool pica_vw_trim(void) { return g_vwTrim; }

// `bytes` of job memory, after waiting for the worker to free them if the ring is short.
static u8* vw_alloc(u32 bytes) {
    if (g_vwLimitWant != g_vwLimit && g_vwTail == g_vwHead) { g_vwLimit = g_vwLimitWant; g_vwMemHead = 0; }
    if (bytes > g_vwLimit) return NULL;
    u32 start = g_vwMemHead;
    if (start + bytes > g_vwLimit) start = 0;
    u64 t0 = 0;
    bool waited = false;
    GW_ENTER(GW_VWALLOC);
    for (;;) {
        u32 t = g_vwTail;
        vw_dmb();
        if (t == g_vwHead) break;  // nothing in flight
        bool full = g_vwHead - t >= VW_SLOTS;
        // In flight: [os, head) going round the ring.
        u32 os = g_vw[t % VW_SLOTS].memStart, he = g_vwMemHead, end = start + bytes;
        bool overlap = os <= he ? (start < he && end > os) || (os == he) : (start < he || end > os);
        if (!full && !overlap) break;
        if (!waited) { waited = true; t0 = prof_begin(); g_vwStats[3]++; g_vwStats[4] += g_vwHead - t; }
        vw_steal();
    }
    GW_LEAVE();
    if (t0) { prof_end(PROF_VWAIT, t0); prof_end(PROF_VALLOC, t0); }
    g_vwMemHead = start + bytes;
    g_vwStats[0]++;
    g_vwStats[1] += bytes;
    g_vwStats[2] += bytes;
    return g_vwMem + start;
}

// The job at `mem` (vw_alloc's last allocation, `bytes` long) uses only `used` bytes: the ring takes the rest back.
static void vw_trim(const u8* mem, u32 used, u32 bytes) {
    u32 start = (u32)(mem - g_vwMem);
    used = (used + 7) & ~7u;
    if (!g_vwTrim || used >= bytes || g_vwMemHead != start + bytes) return;
    g_vwMemHead = start + used;
    g_vwStats[2] -= bytes - used;
}

// Queues a prepared draw that may write up to `verts` arena vertices.
static void vw_queue(const VWork* w, u32 verts) {
    if (w->fixed) {
        g_vwFixed = true;
    } else {
        if (!g_vwReserved) g_vwPos = g.arenaUsed;  // synced: the worker starts where this thread is
        g_vwReserved += verts;
    }
    g_vw[g_vwHead % VW_SLOTS] = *w;
    if (w->deferred) g_vwDeferEnd = g_vwHead + 1;
    vw_dmb();
    g_vwHead = g_vwHead + 1;
}

static bool vw_start(void) {
    if (g_vwThread) return true;
#ifndef GC3DS_NATIVE  // native mode: no display-list vertex cache (its pages are guest MEM1's); the game needs the heap
    if (!g_vcGx) g_vcGx = vcache_create(1536u << 10, 4096);  // ~1.5 MB of records + 0.3 MB of slots
    // The worker gathers ~40k segments/s of TP (job 111): ~6 MB of compact records hold several frames.
    if (!g_vcWorker) g_vcWorker = vcache_create(6u << 20, 32768);  // job 112: 16,384 slots evicted 16k live entries/s
#endif
    g_vwMem = malloc(VW_MEM);
    if (!g_vwMem) g_vwMem = linearAlloc(VW_MEM);
    if (!g_vwMem) return false;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    g_vwThread = threadCreate(vw_main, NULL, 32 * 1024, prio, 3, false);
    if (g_vwThread && !g_vwHelper) g_vwHelper = threadCreate(vw_helper, NULL, 32 * 1024, prio + 1 > 0x3F ? 0x3F : prio + 1, 1, false);
    return g_vwThread != NULL;
}

static volatile bool g_vwWant = true;  // the GX thread applies it at its next draw (vw_apply)
void pica_set_vertex_worker(bool on) { g_vwWant = on; }
static void vw_apply(void) {
    if (g_vwWant == g_vwOn || !g_vwThread) return;
    vw_sync_all();
    g_vwOn = g_vwWant;
}
bool pica_vertex_worker(void) { return g_vwOn && g_vwThread; }
void pica_set_job_ring(u32 bytes) { g_vwLimitWant = bytes < (64u << 10) ? (64u << 10) : bytes > VW_MEM ? VW_MEM : bytes; }
u32 pica_job_ring(void) { return g_vwLimitWant; }
bool pica_vertex_worker_wanted(void) { return g_vwWant; }

// A draw of a GPU-transformed batch with more matrices than the palette: transformed and culled on the CPU and drawn
// on its own (counted in g_picaDrawPaths[7]). It closes the batch, so the next draw applies its state again. Room was
// made for the whole draw.
static void draw_cpu_alone(VWork* w, const GXCore* gx, const VFold* f, u32 prim, u32 count, u32 nIdx) {
    g_picaDrawPaths[7]++;
    if (!w->nd) w->nd = g_nd;  // the job had none (the PICA was to cull it); draw_job sized g_nd for the draw
    flush_batch();
    vfast_job_gpu_pos(w->job, NULL);
    set_lon(false);
    cull_face(GPU_CULL_NONE);
    w->cull = (gx->bp[0x00] >> 14) & 3;
    if (w->indexed) {
        if (g.arenaUsed + count - g.vbase > VBASE_SPAN) set_vbase(g.arenaUsed);
        w->dst = g.arenaUsed;
        g.arenaUsed += count;
        w->ibase = g.idxUsed;
        g.idxUsed += nIdx;
        w->vrel = (u16)(w->dst - g.vbase);
        vw_do_indexed(w);
        draw_elements(w->ibase, nIdx);
        return;
    }
    if (!w->deferred) memcpy(g_pv, w->pv, count * sizeof *g_pv);  // vfast_prepare's colours
    vfast_finish(w->job, g_pv);
    g_cull = w->cull;
    cull_ndc(count);
    u32 first = g.arenaUsed;
    assemble(gx, f, prim, count);
    if (g.arenaUsed > first) draw_arrays(first, g.arenaUsed - first);
}

// Draw descriptions on request (pica_describe, the live "tev" command): the next draws' TEV registers (tev_key),
// texture maps (format, size, address) and what the combiner compiler made of them, as text for perf.csv. Written by
// this (GX) thread, taken by the CPU thread once complete.
static char g_desc[6144];
static u32 g_descLen;
static volatile u32 g_descWant, g_descReady;
void pica_describe(u32 draws) { if (!g_descReady && !g_descWant) { g_descLen = 0; g_descWant = draws > 16 ? 16 : draws; } }
const char* pica_take_description(void) { return g_descReady ? g_desc : NULL; }
void pica_description_taken(void) { g_descReady = 0; }
static void describe_draw(const GXCore* gx, u32 prim, u32 count) {
    u32 k[72], n = tev_key(gx, k);
    PicaTev t;
    tev_pica_compile(gx, &t);
    int w = snprintf(g_desc + g_descLen, sizeof g_desc - g_descLen, "# tev draw prim %u n %u: pica stages %u approx %02x tex",
                     (unsigned)prim, (unsigned)count, (unsigned)t.numStages, (unsigned)t.approx);
    for (u32 i = 0; i < t.numTex && w > 0 && g_descLen + (u32)w < sizeof g_desc; i++) {
        u32 map = t.texMap[i] & 7, hi = map >= 4 ? 0x20 : 0, m = map & 3, img0 = gx->bp[0x88 + m + hi], img3 = gx->bp[0x94 + m + hi];
        w += snprintf(g_desc + g_descLen + w, sizeof g_desc - g_descLen - w, " [map %u fmt %u %ux%u @%06x]", (unsigned)map,
                      (unsigned)((img0 >> 20) & 15), (unsigned)((img0 & 0x3FF) + 1), (unsigned)(((img0 >> 10) & 0x3FF) + 1),
                      (unsigned)((img3 & 0xFFFFFF) << 5));
    }
    for (u32 i = 0; i < n && w > 0 && g_descLen + (u32)w + 12 < sizeof g_desc; i++)
        w += snprintf(g_desc + g_descLen + w, sizeof g_desc - g_descLen - w, "%s%08lx", i ? " " : "; key ", (unsigned long)k[i]);
    if (w > 0 && g_descLen + (u32)w + 2 < sizeof g_desc) { g_descLen += (u32)w; g_desc[g_descLen++] = '\n'; g_desc[g_descLen] = 0; }
    if (--g_descWant == 0) { __sync_synchronize(); g_descReady = 1; }
}

// One draw, or a merged draw (nSeg > 0: seg[] lists its draws as prim << 16 | count, vertices back to back in raw;
// always triangles, always indexed).
// The display-list vertex cache (vfast.c): one for gathers on this (GX) thread, one for the vertex worker's.
const VCacheStats* pica_vcache_stats(int worker) { return vcache_stats(worker ? g_vcWorker : g_vcGx); }

// segPa: where the stream (or each merged draw's) lies in guest RAM, VC_NO_PA when it was not read from there.
// idx / ni: an explicit triangle list over the draw's `count` vertices (pica_draw_indexed), host-order u16 indices.
static void draw_job(System* s, u32 prim, u32 vat, const u8* raw, u32 count, u32 vsize, const u32* seg, u32 nSeg,
                     const u32* segPa, const u8* idx, u32 ni) {
    vw_apply();
    g_gxWhere = GW_DRAW;
    g_lean = g_c3d ? g_leanWant : 0;
    if (g_descWant && !g_descReady) describe_draw(&s->gx.core, prim, count);
    static u32 drawN;
    g_drawScale = 64;
    g_drawTimed = !(++drawN & 63);  // per-draw timers on one draw in 64 (job 436: at 1 in 16 they cost ~2%)
    GXCore* gx = &s->gx.core;
    if (count > g_vcap) {
        SVert* sv = realloc(g_sv, count * sizeof *sv);
        if (sv) g_sv = sv;
        PVert* pv = realloc(g_pv, count * sizeof *pv);
        if (pv) g_pv = pv;
        float (*nd)[2] = realloc(g_nd, count * sizeof *nd);
        if (nd) g_nd = nd;
        if (!sv || !pv || !nd) return;
        g_vcap = count;
    }
    begin_frame();
    u64 tState = dprof_begin();
    u32 frameBefore = g.frameNo;  // looking up textures can also start a frame (texture graveyard full)
    bool lines = prim >= PRIM_LINES;
    u32 cull = (gx->bp[0x00] >> 14) & 3;
    if (!lines && cull == 3) { dprof_end(PROF_GXSTATE, tState); return; }  // every triangle culled
    // Z-texture depth goes into the vertices on the CPU, so those draws keep the CPU transform.
    u32 zop = 0;
    float zz = 0;
    bool ztex = !lines && ztex_value(s, gx, &zop, &zz);
    bool gpuPos = g.gpuPos && !lines && !ztex, indexed = ((g_indexed || nSeg) && !lines) || idx;
    // Lines and points are lit and placed on the CPU, and a batch is transformed all on the GPU or all on the CPU, and
    // indexed or not: draws of different classes never share a batch.
    u32 cls = (lines ? 1u : 0u) | (gpuPos ? 2u : 0u) | (indexed ? 4u : 0u);
    // The open batch's key holds the TEV words first, so the used texture maps (from the compiled TEV) are only
    // compared once the TEV is known to be the same.
    // Fast path: nothing the key is made of has changed since the open batch's draw (no BP write that changed a
    // value, same palettes, lights, XFB copies and class), so it is the same batch without building the key.
    u32 key[96], kn = 0;
    bool same = g.batchOpen && gx->bpVersion == g.batchBpVer && gx->tlutVersion == g.batchTlutVer &&
                gx->xfLightVersion == g.batchLightVer && g.xfbSeq == g.batchXfbSeq && cls == g.batchCls;
    if (same) {
        kn = 0;  // the open batch keeps its key (g.batchKey): not copied
        g_picaDrawPaths[0]++;
    } else {
        kn = batch_key(gx, key);
        key[kn++] = cls;
        same = g.batchOpen && kn <= g.batchKeyLen && !memcmp(key, g.batchKey, kn * 4);
        if (same) { tex_key(gx, key, &kn); same = kn == g.batchKeyLen && !memcmp(key, g.batchKey, kn * 4); }
        g_picaDrawPaths[same ? 1 : 2]++;
        if (!same) {  // what made the batch key differ: BP, palettes, lights, XFB copy, primitive class
            if (gx->bpVersion != g.batchBpVer) g_picaDrawPaths[3]++;
            if (gx->tlutVersion != g.batchTlutVer) g_picaDrawPaths[4]++;
            if (gx->xfLightVersion != g.batchLightVer) g_picaDrawPaths[5]++;
            if (g.xfbSeq != g.batchXfbSeq) g_picaDrawPaths[6]++;
        }
    }
    // (recorded below once the batch is known to be this state)
    C3D_Tex* tex[PICA_TEXUNITS] = { NULL, NULL, NULL };
    if (!same) {
        u64 ts = SPROF_BEGIN();
        g_gxWhere = GW_FLUSH;
        flush_batch();
        SPROF_END(0, ts); ts = SPROF_BEGIN();
        split_early();
        SPROF_END(1, ts); ts = SPROF_BEGIN();
        g_gxWhere = GW_TEVUPD;
        update_tev(gx);
        SPROF_END(2, ts); ts = SPROF_BEGIN();
        g_gxWhere = GW_BATCHKEY;
        kn = batch_key(gx, key);
        key[kn++] = cls;
        tex_key(gx, key, &kn);
        SPROF_END(3, ts);
        u64 t0 = dprof_begin();
        for (u32 i = 0; i < g.tev.numTex && i < PICA_TEXUNITS; i++) tex[i] = fetch_texture(s, gx, g.tev.texMap[i] & 7);
        dprof_end(PROF_GXTEX, t0);
        g.batchGpuLight = g.shaderLight && prim < PRIM_LINES && vfast_gpu_light(gx, &g.sel, &g.lu);
        g.batchGpuPos = gpuPos;
        g.batchCull = kCull[cull];  // the cull mode is in the key (BP 0x00)
    }
    dprof_end(PROF_GXSTATE, tState);
    u64 t0 = dprof_begin();
    VFold f;
    vfold_init(gx, SX * 2.0f / TW, SY * 2.0f / TH, FLIP_Y, &f);
    // Triangles go through a vertex job: prepared here, finished on the vertex worker (core 3) or right here when the
    // worker is off. Each draw's arena range is fixed when queued (indexed draws, or the GPU transform, which culls),
    // except CPU-transformed draws without indices, which the worker culls and appends (their batches wait for it).
    bool work = !lines && pica_vertex_worker();
    bool jobPath = !lines && (work || indexed || gpuPos);
    VWork w;
    memset(&w, 0, sizeof w);
    u32 workVerts = 0, nIdx = 0;
    if (jobPath) {
        if (idx) workVerts = ni / 3 * 3;
        else if (nSeg) for (u32 k = 0; k < nSeg; k++) workVerts += tri_verts(seg[k] >> 16, seg[k] & 0xFFFF);
        else workVerts = tri_verts(prim, count);
        if (!workVerts) { dprof_end(PROF_GXVTX, t0); return; }
        nIdx = indexed ? workVerts : 0;
        u64 tq = dprof_begin();
        g_gxWhere = GW_VQUEUE;
        if (!need_work(indexed ? count : workVerts, nIdx)) {
            emu_log(LOG_GX, "pica: draw of %u vertices too large, dropped\n", (unsigned)count);
            return;
        }
        bool defer = gpuPos || indexed || kVwGather, deferLit = defer && (g.batchGpuLight || g_vfDeferCpu);
        // Job memory only for what the draw uses: a deferred indexed job finishes straight into the arena (no vertex
        // copy), and GPU-transformed draws are culled by the PICA (no NDC).
        bool needPv = !(deferLit && indexed), needNd = !gpuPos && (cull == 1 || cull == 2);
        // The draw's own arrays first, then the job (its unused end is given back after vfast_prepare: vw_trim).
        u32 jb = (vfast_job_bytes(gx, count, vsize) + 7) & ~7u;
        u32 front = ((needPv ? count * (u32)sizeof(PVert) : 0) + (needNd ? count * 8 : 0) + nSeg * 8 +
                     (idx ? workVerts * 2 : 0) + 7) & ~7u;
        u32 bytes = front + jb;
        u8* mem = work ? vw_alloc(bytes) : NULL;
        if (!mem) {  // the worker is off, or the draw is larger than its job ring: done here
            if (work) { g_vwInline++; g_vwInlineVerts += count; }
            work = false;
            if (bytes > g_jobCap) {
                u8* m = realloc(g_jobMem, bytes);
                if (!m) { dprof_end(PROF_GXVTX, t0); return; }
                g_jobMem = m;
                g_jobCap = bytes;
            }
            mem = g_jobMem;
        }
        dprof_end(PROF_VQUEUE, tq);
        u8* at = mem;
        if (needPv) { w.pv = (PVert*)at; at += count * sizeof(PVert); }
        if (needNd) { w.nd = (float (*)[2])at; at += count * 8; }
        const u32* pac = segPa;
        if (nSeg) {
            u32* sc = (u32*)at;
            memcpy(sc, seg, nSeg * 4);
            w.seg = sc;
            w.nSeg = nSeg;
            u32* pc = sc + nSeg;  // the segments' addresses live with the job (the worker gathers later)
            memcpy(pc, segPa, nSeg * 4);
            pac = pc;
            at = (u8*)(pc + nSeg);
        }
        if (idx) {  // the indices live with the job (the stream they came in is reused); 2-byte aligned here
            memcpy(at, idx, workVerts * 2);
            w.tri = (const u16*)(const void*)at;
            w.nTri = workVerts / 3;
            at += workVerts * 2;
        }
        w.deferred = deferLit;  // vfast_prepare defers shader-lit draws (and CPU-lit ones with g_vfDeferCpu)
        if (g_vcGx) vfast_set_sources(g_vcGx, pac, w.seg, nSeg ? nSeg : 1);
        u64 tp = dprof_begin();
        g_gxWhere = GW_VPREP;
        w.job = vfast_prepare(s, gx, vat, &f, &g.sel, raw, count, vsize, g.batchGpuLight, defer, mem + front, w.pv);
        dprof_end(PROF_VPREP, tp);
        if (!w.job) { dprof_end(PROF_GXVTX, t0); return; }
        if (work) vw_trim(mem, front + vfast_job_used(w.job), bytes);
        w.memStart = work ? (u32)(mem - g_vwMem) : 0;
        w.prim = prim;
        w.count = count;
        w.cull = gpuPos ? 0 : cull;  // GPU-transformed: the PICA culls
        w.ztexOp = ztex ? zop : 0;
        w.ztexZ = zz;
        w.indexed = indexed;
        w.fixed = gpuPos || indexed;
    } else if (lines) {
        for (u32 i = 0; i < count; i++) {
            GXVertex dv;
            gx_decode_vertex(s, gx, vat, raw + i * vsize, &dv);
            gx_transform(gx, &dv, &g_sv[i]);
            gx_to_screen(gx, &g_sv[i]);
        }
    } else {  // the worker is off: CPU transform and culling here, triangles appended below
        vfast_run(s, gx, vat, &f, &g.sel, raw, count, vsize, g_pv, g.batchGpuLight);
        if (ztex) ztex_apply(g_pv, count, zop, zz);
        g_cull = cull;
        cull_ndc(count);
    }
    dprof_end(PROF_GXVTX, t0);
    u32 maxVerts = lines ? count * 6 : count * 3;
    if (!jobPath && !need(maxVerts)) { emu_log(LOG_GX, "pica: draw of %u vertices too large, dropped\n", (unsigned)count); return; }
    tState = dprof_begin();
    g_gxWhere = GW_STATE2;
    if (g.frameNo != frameBefore || !same) {
        if (g.frameNo != frameBefore) {  // a new frame resets citro3d's state tracking; textures stay live
            update_tev(gx);
            if (same) {  // textures were not looked up for this draw: do it now
                for (u32 i = 0; i < g.tev.numTex && i < PICA_TEXUNITS; i++) tex[i] = fetch_texture(s, gx, g.tev.texMap[i] & 7);
            } else {
                for (u32 i = 0; i < g.tev.numTex && i < PICA_TEXUNITS; i++) {
                    PTex* p = NULL;
                    for (u32 j = 0; j < MAX_TEX; j++) if (&g.tex[j].tex == tex[i]) p = &g.tex[j];
                    if (p) p->lastUse = g.frameNo;
                }
            }
        }
        u64 ts = SPROF_BEGIN();
        apply_frag(gx);
        SPROF_END(4, ts);
        // Only the units the combiners read are bound; the others keep whatever they had, which nothing samples.
        // - Never bind NULL: citro3d 1.7.1 reads the texture's type through it, a data abort on hardware. Azahar does
        //   not fault on reads near address 0, so it hid this.
        // - Binding a placeholder to unused units 1 and 2 crashed Azahar itself (2026-10-10).
        ts = SPROF_BEGIN();
        for (u32 i = 0; i < g.tev.numTex && i < PICA_TEXUNITS; i++) bind_tex((int)i, tex[i] ? tex[i] : &g.white);
        SPROF_END(5, ts); ts = SPROF_BEGIN();
        apply_light_uniforms();
        SPROF_END(6, ts);
        g.batchOpen = true;
        g.batchFirst = g.arenaUsed;
        g.batchIdxFirst = g.idxUsed;
        g_recJob = g_vwHead;
        g.batchIdx = indexed;
        g.palSerial++;  // the previous batch is drawn: its palette slots may be replaced
        if (!same) {  // the same batch (a new frame only) already holds this key
            memcpy(g.batchKey, key, kn * 4);
            g.batchKeyLen = kn;
        }
    }
    g_gxWhere = GW_DRAWTAIL;
    g.batchBpVer = gx->bpVersion; g.batchTlutVer = gx->tlutVersion; g.batchLightVer = gx->xfLightVersion;
    g.batchXfbSeq = g.xfbSeq; g.batchCls = cls;
    dprof_end(PROF_GXSTATE, tState);
    if (jobPath) {
        split_early();  // at the draw calls recorded so far (the open batch's jobs need not be done)
        t0 = dprof_begin();
        // u16 indices: start the vertex buffer further along when this draw's vertices would be out of their reach
        // (before any palette slot is given, since that draws the open part of the batch).
        if (indexed && g.arenaUsed + count - g.vbase > VBASE_SPAN) {
            flush_sub();
            set_vbase(g.arenaUsed);
        }
        if (gpuPos) {
            u64 tpal = dprof_begin();
            g_gxWhere = GW_PALASSIGN;
            bool palOk = pal_assign(w.job);
            dprof_end(PROF_VPAL, tpal);
            if (!palOk) {
                g_gxWhere = GW_CPUALONE;
                draw_cpu_alone(&w, gx, &f, prim, count, nIdx);
                dprof_end(PROF_GXVTX, t0);
                return;
            }
        }
        if (w.fixed) {  // the draw's ranges are known now: no draw call waits for the worker
            w.dst = g.arenaUsed;
            g.arenaUsed += indexed ? count : workVerts;
            if (indexed) {
                w.ibase = g.idxUsed;
                g.idxUsed += nIdx;
                w.vrel = (u16)(w.dst - g.vbase);
            }
            g_gxWhere = work ? GW_VWQUEUE : GW_VWDO;
            if (work) vw_queue(&w, 0);
            else vw_do(&w);
        } else if (work) {  // the worker appends the triangles it keeps; the batch's draw call waits for it (flush_batch)
            g_gxWhere = GW_VWQUEUE;
            vw_queue(&w, workVerts);
        } else {  // too large for the worker's ring: culled and appended here, after the worker's queued draws
            g_gxWhere = GW_VWWAIT;
            vw_sync();
            g_vwPos = g.arenaUsed;
            g_gxWhere = GW_VWDO;
            vw_do(&w);
            g.arenaUsed = g_vwPos;
        }
        dprof_end(PROF_GXVTX, t0);
        return;
    }
    assemble(gx, &f, prim, count);
}

// ---- draw merging ----
// Twilight Princess sends ~50,000 draws a second of ~5 vertices, and nearly all of the GX thread's time is fixed work per
// draw (batch state, vertex setup, palette, the job for the vertex worker: ~16 us of core 2 per draw on the 2DS,
// 3a1696f). Consecutive triangle draws with the same vertex format and no state change between them are joined into
// one vertex job: the open draw keeps a copy of each draw's vertex stream and its primitive and count, and is issued
// as one indexed draw whose triangles are assembled draw by draw (vw_assemble_idx). It is issued before anything
// changes state it reads (fifo.c calls the backend's flush: register values, palette loads, PE signals, EFB copies),
// before a draw that cannot join, before present, and when the GX thread finds its ring empty (pica_merge_flush), so
// a sync from the CPU thread never sees it pending (gxthread.c's wait_drained also checks g_gxMergeOpen). Only with the
// GX thread: inline, the CPU thread would run on with the draw unissued. MERGE_CHECK on the PC models it (soft.c).
#define MERGE_VERTS 256
#define MERGE_SEGS 64
#define MERGE_BYTES (16u << 10)
static struct {
    u32 vat, vsize, count, bytes, nSeg;
    u32 seg[MERGE_SEGS];  // prim << 16 | count
    u32 pa[MERGE_SEGS];   // where each draw's stream lies in guest RAM (VC_NO_PA: not from RAM)
    u8 raw[MERGE_BYTES];
} g_mg;
static volatile bool g_mergeWant = true;  // pica_set_merge (the GX thread reads it at each draw)
u64 g_picaMerge[7];

static void merge_issue(System* s) {
    u32 n = g_mg.nSeg;
    g_mg.nSeg = 0;
    if (n) {
        g_picaMerge[2]++;
        draw_job(s, g_mg.seg[0] >> 16, g_mg.vat, g_mg.raw, g_mg.count, g_mg.vsize, n > 1 ? g_mg.seg : NULL, n > 1 ? n : 0, g_mg.pa,
                 NULL, 0);
    }
    vw_dmb();  // the draw is queued on the vertex worker before a sync on the CPU thread can see it issued
    g_gxMergeOpen = false;
}

static void pica_flush(void* ctx, System* s) {
    (void)ctx;
    g_picaMerge[3]++;
    merge_issue(s);
}

void pica_merge_flush(System* s, bool idle) {
    if (!g_gxMergeOpen) return;
    g_picaMerge[idle ? 5 : 6]++;
    merge_issue(s);
}

void pica_set_merge(bool on) { g_mergeWant = on; }
bool pica_merge(void) { return g_mergeWant; }

// Frame skip (REDESIGN_CANDIDATES C9; --fskip on, --ab fskip, live "set fskip"): every other rendered frame (frames
// end at the EFB copy to the XFB) draws nothing and makes no EFB copies; the game's GX stream still runs, and the
// screen keeps the previous frame. An experiment first: how much speed the vertex work holds (copies to textures in
// skipped frames are not made, so render-to-texture effects lag a frame).
static volatile bool g_fskipWant;
static bool g_fskipNow;
u64 g_fskipDraws, g_fskipFrames;
void pica_set_fskip(bool on) { g_fskipWant = on; }
bool pica_fskip(void) { return g_fskipWant; }

static void pica_draw(void* ctx, System* s, u32 prim, u32 vat, const u8* raw, u32 count, u32 vsize) {
    (void)ctx;
    g_gxWhere = GW_MERGE;
    if (g_fskipNow) { g_fskipDraws++; return; }
    g_picaMerge[0]++;
    u32 bytes = count * vsize;
    // A draw inside a display list streams straight from guest RAM: its address keys the vertex cache.
    u32 pa = raw >= s->mem1 && raw + bytes <= s->mem1 + PA_MEM1_END ? (u32)(raw - s->mem1) : VC_NO_PA;
    bool can = g_mergeWant && g_indexed && s->gxAsync && prim < PRIM_LINES && count <= MERGE_VERTS && bytes <= MERGE_BYTES;
    if (g_gxMergeOpen) {
        if (can && vat == g_mg.vat && vsize == g_mg.vsize && g_mg.count + count <= MERGE_VERTS &&
            g_mg.bytes + bytes <= MERGE_BYTES && g_mg.nSeg < MERGE_SEGS) {
            ram_copy(g_mg.raw, g_mg.bytes, raw, bytes);  // (in the layout, ramlayout.h)
            g_mg.pa[g_mg.nSeg] = pa;
            g_mg.seg[g_mg.nSeg++] = (prim << 16) | count;
            g_mg.count += count;
            g_mg.bytes += bytes;
            g_picaMerge[1]++;
            return;
        }
        g_picaMerge[4]++;
        merge_issue(s);
    }
    if (!can) { draw_job(s, prim, vat, raw, count, vsize, NULL, 0, &pa, NULL, 0); return; }
    ram_copy(g_mg.raw, 0, raw, bytes);
    g_mg.pa[0] = pa;
    g_mg.seg[0] = (prim << 16) | count;
    g_mg.nSeg = 1;
    g_mg.vat = vat;
    g_mg.vsize = vsize;
    g_mg.count = count;
    g_mg.bytes = bytes;
    g_gxMergeOpen = true;
}

// ---- EFB copies and clears ----
// Fixed-function state for a full-target textured quad (copies and present).
static void quad_state(C3D_Tex* src, bool linear) {
    flush_batch();
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.uOn, 0, 0, 0, 0);  // copy and present quads carry their colour
    C3D_CullFace(GPU_CULL_NONE);
    C3D_DepthTest(false, GPU_ALWAYS, GPU_WRITE_COLOR);
    C3D_AlphaTest(false, GPU_ALWAYS, 0);
    C3D_ColorLogicOp(GPU_LOGICOP_COPY);
    C3D_SetScissor(GPU_SCISSOR_DISABLE, 0, 0, 0, 0);
    if (g_gpuDiag & 32) C3D_SetScissor(GPU_SCISSOR_NORMAL, 0, 0, 16, 16);  // (diagnostic: copy and present quads tiny)
    C3D_TexSetFilter(src, linear ? GPU_LINEAR : GPU_NEAREST, linear ? GPU_LINEAR : GPU_NEAREST);
    C3D_TexSetWrap(src, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    g_texEpoch++;  // copies and present read and write textures: every unit binds again afterwards
    bind_tex(0, src);
    for (int i = 1; i < PICA_STAGES; i++) C3D_TexEnvInit(C3D_GetTexEnv(i));
    C3D_TexEnvBufUpdate(C3D_Both, 0);
    g.stateValid = g.tevValid = false;
}

// Two triangles: corner i at clip (x, y), sampling (s, t). The caller has made room (need(6)) before binding the
// target, since making room can start a new frame.
static void quad(const float pos[4][2], const float st[4][2]) {
    PVert v[4];
    memset(v, 0, sizeof v);
    for (int i = 0; i < 4; i++) {
        v[i].pos[0] = pos[i][0]; v[i].pos[1] = pos[i][1]; v[i].pos[2] = -0.5f; v[i].pos[3] = 1.0f;
        v[i].col[0] = v[i].col[1] = v[i].col[2] = v[i].col[3] = 255;
        v[i].tc[0][0] = st[i][0]; v[i].tc[0][1] = st[i][1];
    }
    PVert* o = g.arena + g.arenaUsed;
    o[0] = v[0]; o[1] = v[1]; o[2] = v[2]; o[3] = v[0]; o[4] = v[2]; o[5] = v[3];
    draw_arrays(g.arenaUsed, 6);
    g.arenaUsed += 6;
}

// Texture coordinate of EFB row y / column x in the EFB texture.
static float efb_s(float x) { return x * SX / TW; }
static float efb_t(float y) { return y * SY / TH; }
static float ndc_y(float t) { return FLIP_Y ? 1.0f - 2.0f * t : 2.0f * t - 1.0f; }

// New frames' pacing (perf.csv '# new frames 10s'): the gaps between EFB -> XFB copies in real time, the moment a new
// picture exists (presents run every field whatever the picture): count, sum, sum of squares (ms), largest.
double g_xfbGap[4];
static u64 g_xfbLast;
// The gaps' distributions: real time in quarter-VBlank bins (1/240 s; the last bin holds the rest), and VI fields
// emulated between copies (g_viField, written by core 0 at each field; the last bin 7 and more).
u32 g_xfbHistRt[64], g_xfbHistF[8];
volatile u32 g_viField;
static u32 g_xfbLastField;

static void copy_to_xfb(u32 x0, u32 y0, u32 w, u32 h, u32 dest) {
    g_picaStats.xfbCopies++;
    {
        u64 now = svcGetSystemTick();
        if (g_xfbLast) {
            double ms = (double)(now - g_xfbLast) * 1000.0 / SYSCLOCK_ARM11;
            g_xfbGap[0] += 1; g_xfbGap[1] += ms; g_xfbGap[2] += ms * ms;
            if (ms > g_xfbGap[3]) g_xfbGap[3] = ms;
            u32 b = (u32)(ms * 0.24);
            g_xfbHistRt[b < 63 ? b : 63]++;
            u32 f = g_viField - g_xfbLastField;
            g_xfbHistF[f < 7 ? f : 7]++;
        }
        g_xfbLast = now;
        g_xfbLastField = g_viField;
    }
    g_copyPix[2]++;
    PXfb* x = NULL;
    for (u32 i = 0; i < MAX_XFB; i++) if (g.xfb[i].seq && g.xfb[i].addr == dest) x = &g.xfb[i];
    if (!x) for (u32 i = 0; i < MAX_XFB; i++) if (!x || g.xfb[i].seq < x->seq) x = &g.xfb[i];
    if (!(g_gpuDiag & 16)) {
        split(1);  // the draws so far reach the EFB before the copy reads it
        GX_TextureCopy((u32*)g.efbTex.data, 0, (u32*)x->tex.data, 0, TW * TH * 4, GX_TRANSFER_RAW_COPY(1));
        queue_run();
    }
    x->addr = dest; x->x0 = x0; x->y0 = y0; x->w = w; x->h = h; x->seq = ++g.xfbSeq;
}

// Copy-unit formats (fifo.c): R4, R8 alias, RA4, RA8, RGB565, RGB5A3, RGBA8, A8, R8, G8, B8, RG8, GB8.
static void copy_to_texture(System* s, GXCore* gx, u32 ctl, u32 x0, u32 y0, u32 w, u32 h, u32 dest) {
    g_picaStats.efbCopies++;
    u32 field = (ctl >> 3) & 15, fmt = (field >> 1) | ((field & 1) << 3);
    bool half = (ctl >> 9) & 1, intensity = (ctl >> 15) & 1, depth = (gx->bp[0x43] & 7) == 3, hasAlpha = (gx->bp[0x43] & 7) == 1;
    u32 ow = half ? (w + 1) / 2 : w, oh = half ? (h + 1) / 2 : h;
    u32 W = pow2((u32)ceilf((float)w * SX)), H = pow2((u32)ceilf((float)h * SY));
    PCopy* c = NULL;
    for (u32 i = 0; i < MAX_COPIES && !c; i++)
        if (g.copies[i].live && g.copies[i].used && g.copies[i].addr == dest && g.copies[i].tex.width == W && g.copies[i].tex.height == H) c = &g.copies[i];
    for (u32 i = 0; i < MAX_COPIES && !c; i++)
        if (g.copies[i].live && !g.copies[i].used && g.copies[i].tex.width == W && g.copies[i].tex.height == H) c = &g.copies[i];
    for (u32 i = 0; i < MAX_COPIES && !c; i++) {
        PCopy* p = &g.copies[i];
        if (p->live) continue;
        if (!C3D_TexInitVRAM(&p->tex, (u16)W, (u16)H, GPU_RGBA8) && !C3D_TexInit(&p->tex, (u16)W, (u16)H, GPU_RGBA8)) break;
        p->rt = C3D_RenderTargetCreateFromTex(&p->tex, GPU_TEXFACE_2D, 0, -1);
        if (!p->rt) { C3D_TexDelete(&p->tex); break; }
        p->live = true;
        c = p;
    }
    if (!c) { emu_log(LOG_GX, "pica: no free EFB copy texture for %08x %ux%u\n", (unsigned)dest, (unsigned)ow, (unsigned)oh); return; }
    g_copyPix[0] += W * H;
    g_copyPix[1] += (u64)(w * SX * h * SY);
    if (g_gpuDiag & 1) {
        g_copyPix[3]++;
        c->addr = dest; c->w = ow; c->h = oh; c->used = true;
        u32 bpp = fmt == 0 ? 4 : fmt == 6 ? 32 : (fmt == 3 || fmt == 4 || fmt == 5 || fmt >= 11) ? 16 : 8;
        c->ramBytes = ow * oh * bpp / 8;
        if (!dest || dest < PA_MEM1_BEGIN || dest + c->ramBytes > PA_MEM1_END) c->ramBytes = 0;
        c->ramHash = hash_words(s->mem1 + dest, c->ramBytes);
        c->texVersion = g.xfbSeq;
        return;
    }
    split(0);
    need(6);
    C3D_FrameDrawOn(c->rt);
    quad_state(depth ? &g.efbDepthTex : &g.efbTex, !depth && (half || W < w || H < h));
    C3D_TexEnv* e = C3D_GetTexEnv(0);
    C3D_TexEnvInit(e);
    C3D_TexEnvSrc(e, C3D_Both, GPU_TEXTURE0, GPU_CONSTANT, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(e, C3D_Both, GPU_REPLACE);
    if (depth) {
        // Z copies: the copy unit treats depth bits 16-23 / 8-15 / 0-7 as R / G / B (fifo.c). The texture then holds
        // what the matching intensity decoder returns: one byte as I (I4/I8 reads) or two as I and A (IA8 reads,
        // which store the second-named channel first, so RG8 gives I = R, A = G). From efbDepthTex: hi = G,
        // mid = B, lo = A. RGBA8 / RGB565 / RGB5A3 Z copies show the high byte only (approximation).
        static const u8 rgbOp[13] = { GPU_TEVOP_RGB_SRC_G, GPU_TEVOP_RGB_SRC_G, GPU_TEVOP_RGB_SRC_G, GPU_TEVOP_RGB_SRC_G,
                                      GPU_TEVOP_RGB_SRC_G, GPU_TEVOP_RGB_SRC_G, GPU_TEVOP_RGB_SRC_G, GPU_TEVOP_RGB_SRC_G,
                                      GPU_TEVOP_RGB_SRC_G, GPU_TEVOP_RGB_SRC_B, GPU_TEVOP_RGB_SRC_ALPHA,
                                      GPU_TEVOP_RGB_SRC_G, GPU_TEVOP_RGB_SRC_B };
        static const u8 aOp[13] = { GPU_TEVOP_A_SRC_G, GPU_TEVOP_A_SRC_G, GPU_TEVOP_A_SRC_G, GPU_TEVOP_A_SRC_B,
                                    GPU_TEVOP_A_SRC_G, GPU_TEVOP_A_SRC_G, GPU_TEVOP_A_SRC_G, GPU_TEVOP_A_SRC_G,
                                    GPU_TEVOP_A_SRC_G, GPU_TEVOP_A_SRC_B, GPU_TEVOP_A_SRC_ALPHA,
                                    GPU_TEVOP_A_SRC_B, GPU_TEVOP_A_SRC_ALPHA };
        u32 f = fmt < 13 ? fmt : 8;
        C3D_TexEnvOpRgb(e, rgbOp[f], GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
        C3D_TexEnvOpAlpha(e, aOp[f], GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
        if (f == 2 || f == 3) {  // RA formats: the copy unit writes alpha 255 for depth
            C3D_TexEnvSrc(e, C3D_Alpha, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
            C3D_TexEnvColor(e, 0xFFFFFFFF);
        }
        if (f == 4 || f == 5 || f == 6) note_approx(1u << 29);
    } else if (intensity && fmt <= 3) {
        // Studio-range luma: Y = 0.257 R + 0.504 G + 0.098 B + 16/255 as DOT3 (4 * sum((a - .5)(b - .5))) against
        // b = 0.5 + k / 4, then + (0.5 * sum(k) + 16/255) in stage 1. IA formats keep the EFB alpha.
        C3D_TexEnvFunc(e, C3D_Both, GPU_DOT3_RGBA);
        C3D_TexEnvColor(e, 0xFF000000u | (134u << 16) | (160u << 8) | 144u);  // 0.5 + k/4 for B, G, R
        C3D_TexEnv* e1 = C3D_GetTexEnv(1);
        C3D_TexEnvInit(e1);
        C3D_TexEnvSrc(e1, C3D_Both, GPU_PREVIOUS, GPU_CONSTANT, GPU_PRIMARY_COLOR);
        C3D_TexEnvFunc(e1, C3D_Both, GPU_ADD);
        C3D_TexEnvColor(e1, 0x7D7D7D7Du);  // 0.430 + 0.063
        if (fmt == 2 || fmt == 3) {
            C3D_TexEnvSrc(e1, C3D_Alpha, GPU_TEXTURE0, GPU_CONSTANT, GPU_PRIMARY_COLOR);
            C3D_TexEnvFunc(e1, C3D_Alpha, GPU_REPLACE);
            if (!hasAlpha) { C3D_TexEnvSrc(e1, C3D_Alpha, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT); C3D_TexEnvColor(e1, 0xFF7D7D7Du); }
        }
    } else if (fmt == 0 || fmt == 1 || fmt == 8 || fmt == 9 || fmt == 10 || fmt == 7) {
        // One channel replicated, as the matching I4/I8 decoders return it.
        static const u8 opc[11] = { GPU_TEVOP_RGB_SRC_R, GPU_TEVOP_RGB_SRC_R, 0, 0, 0, 0, 0, GPU_TEVOP_RGB_SRC_ALPHA,
                                    GPU_TEVOP_RGB_SRC_R, GPU_TEVOP_RGB_SRC_G, GPU_TEVOP_RGB_SRC_B };
        static const u8 opa[11] = { GPU_TEVOP_A_SRC_R, GPU_TEVOP_A_SRC_R, 0, 0, 0, 0, 0, GPU_TEVOP_A_SRC_ALPHA,
                                    GPU_TEVOP_A_SRC_R, GPU_TEVOP_A_SRC_G, GPU_TEVOP_A_SRC_B };
        C3D_TexEnvOpRgb(e, opc[fmt], GPU_TEVOP_RGB_SRC_COLOR, GPU_TEVOP_RGB_SRC_COLOR);
        C3D_TexEnvOpAlpha(e, opa[fmt], GPU_TEVOP_A_SRC_ALPHA, GPU_TEVOP_A_SRC_ALPHA);
        if (fmt == 7 && !hasAlpha) { C3D_TexEnvSrc(e, C3D_Both, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT); C3D_TexEnvColor(e, 0xFFFFFFFF); }
    } else if (fmt == 4 || !hasAlpha) {  // RGB565, or an EFB without alpha: opaque
        C3D_TexEnvSrc(e, C3D_Alpha, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
        C3D_TexEnvColor(e, 0xFFFFFFFF);
    }
    float s0 = efb_s((float)x0), s1 = efb_s((float)(x0 + w)), t0 = efb_t((float)y0), t1 = efb_t((float)(y0 + h));
    float pos[4][2] = { { -1, ndc_y(0) }, { 1, ndc_y(0) }, { 1, ndc_y(1) }, { -1, ndc_y(1) } };
    float st[4][2] = { { s0, t0 }, { s1, t0 }, { s1, t1 }, { s0, t1 } };
    quad(pos, st);
    C3D_FrameDrawOn(g.efb);
    c->addr = dest; c->w = ow; c->h = oh; c->used = true;
    u32 bpp = fmt == 0 ? 4 : fmt == 6 ? 32 : (fmt == 3 || fmt == 4 || fmt == 5 || fmt >= 11) ? 16 : 8;
    c->ramBytes = ow * oh * bpp / 8;
    // No RAM behind it: out of range, or (native mode) a copy Aurora's stream gave no destination (the 2DS read
    // address 0 here, job 221).
    if (!dest || dest < PA_MEM1_BEGIN || dest + c->ramBytes > PA_MEM1_END) c->ramBytes = 0;
    c->ramHash = hash_words(s->mem1 + dest, c->ramBytes);
    c->texVersion = g.xfbSeq;
}

static void clear_efb(const GXCore* gx, u32 x0, u32 y0, u32 w, u32 h) {
    g_picaStats.clears++;
    u32 ar = gx->bp[0x4F], gb = gx->bp[0x50], cmode = gx->bp[0x41];
    u32 a = (gx->bp[0x43] & 7) == 1 ? (ar >> 8) & 0xFF : 0xFF;
    u32 rgba = ((ar & 0xFF) << 24) | (((gb >> 8) & 0xFF) << 16) | ((gb & 0xFF) << 8) | a;
    u32 bits = ((cmode & 0x18) ? C3D_CLEAR_COLOR : 0) | ((gx->bp[0x40] & 0x10) ? C3D_CLEAR_DEPTH : 0);
    if (!bits) return;
    g_texEpoch++;  // the EFB's colour and depth are also textures (copies)
    if (x0 > 8 || y0 > 8 || w * h < EFB_W * 400) note_approx(1u << 30);  // partial clear widened to the whole EFB
    split(1);
    // One memory fill with both slots naming a real buffer: C3D_RenderTargetClear leaves the second slot NULL for
    // a colour-only or depth-only clear, which Azahar turns into physical address 0 and crashes on.
    u32 n = TW * TH, z = gx->bp[0x51] & 0xFFFFFF;
    u32* cb = (u32*)g.efb->frameBuf.colorBuf;
    u32* db = (u32*)g.efb->frameBuf.depthBuf;
    bool color = bits & C3D_CLEAR_COLOR, depth = bits & C3D_CLEAR_DEPTH;
    u32* b0 = color ? cb : db;
    u32* b1 = depth ? db : cb;
    u16 ctl = GX_FILL_TRIGGER | GX_FILL_32BIT_DEPTH;
    GX_MemoryFill(b0, color ? rgba : z, b0 + n, ctl, b1, depth ? z : rgba, b1 + n, ctl);
}

static bool pica_efb_copy(void* ctx, System* s, u32 ctl, u32 x0, u32 y0, u32 w, u32 h, u32 dest) {
    g_gxWhere = GW_COPY;
    (void)ctx;
    GXCore* gx = &s->gx.core;
    if (g_fskipNow) {  // a skipped frame: no copies; its end starts a drawn one
        if ((ctl >> 14) & 1) { g_fskipNow = false; g_fskipFrames++; }
        return true;
    }
    if (((ctl >> 14) & 1) && g_fskipWant) g_fskipNow = true;  // after this drawn frame's copy, the next is skipped
    begin_frame();
    if ((ctl >> 14) & 1) copy_to_xfb(x0, y0, w, h, dest);
    else copy_to_texture(s, gx, ctl, x0, y0, w, h, dest);
    if ((ctl >> 11) & 1) clear_efb(gx, x0, y0, w, h);
    return true;
}

// An index-list draw (native mode's GX_AURORA_DRAW_INDEXED, what Aurora's display-list optimizer makes of a model): each
// of the `nv` vertices is gathered and transformed once, the triangles go to the index arena as they are. The open
// merged draw goes first, as for any draw that cannot join it.
static bool pica_draw_indexed(void* ctx, System* s, u32 vat, const u8* verts, u32 nv, u32 vsize, const u8* idx, u32 ni) {
    (void)ctx;
    if (!g.idx || nv > 0xFFFF || ni < 3) return false;
    if (g_fskipNow) { g_fskipDraws++; return true; }
    if (g_gxMergeOpen) merge_issue(s);
    u32 pa = VC_NO_PA;
    draw_job(s, PRIM_TRIS, vat, verts, nv, vsize, NULL, 0, &pa, idx, ni);
    return true;
}

// ---- converted shapes (docs/research/NATIVE_PORT_PLAN.md, "Converted shapes") ----
// Dusklight's models reach the stream as display lists called by reference, each shape's geometry one
// GX_AURORA_DRAW_INDEXED (Aurora's optimizer: unique vertices as GameCube attribute indices, u16 triangles). The vertex
// path gathered and transformed ~73,000 such vertices a frame on cores 2-3 (title demo, 5-7 fps). Here each command is
// converted once: its vertices decoded into PICA vertex buffers in linear memory (position + matrix row, normal,
// colours, texture coordinates), its indices copied; each frame it is drawn with the matrices, texgen, lighting and
// TEV state of the moment, all transformed in the vertex shader's native path (gxpass_native.v.pica).
//   Kept by the command's address (stable while its display list lives) and a key of the vertex format and the arrays
// it reads. A shape is made again when memory it was made from changes: Dusklight keeps the GameCube's cache flushes
// (DCStoreRange after CPU skinning, vertex colour animation, display-list rewrites), which Aurora's OSCache passes
// here (pica_native_store); a shape seen changing three times stays on the old path (CPU-deformed geometry). A few
// shapes a frame are checked against a hash of their sources, which counts any change the flushes missed.
//   The emulator converts the same way runs of draws inside the game's own display lists (fifo.c run_list: consecutive
// draws of one vertex format, pica_draw_list), keyed by the run's address in guest memory. It watches the memory a shape
// was made from with the page stamps of the guest's cache flushes (dcbf / dcbst / dcbz, gx.h MemStamps; the GPU reads
// memory only after them, the hardware's rule): a page stamped since the shape was made makes it again.
#define NS_SLOTS 4096                // power of two
#define NS_PROBE 16
#define NS_BUDGET (8u << 20)         // linear memory for converted shapes
#define NS_KEY 32
#define NS_RANGES 13                 // the display list, and the array elements of up to 12 indexed attributes
#define NS_GRAVE 1024
enum { NO_POS, NO_NRM, NO_COL0, NO_COL1, NO_TEX0, NO_IDX = 12, NO_N };
typedef struct {
    const u8* cmd;                   // the command in its display list; NULL: a free slot
    u32 len, nv, ni, bytes, lastFrame, hash, present, rows, live;
    u32 key[NS_KEY];
    u8* mem;                         // one linear block: the streams at off[] (~0u: absent)
    u32 off[NO_N];
    float origin[3];                 // subtracted from every position; folded into the matrices' translations
    bool pnDirect;
    u8 nr;
    u32 r0[NS_RANGES], r1[NS_RANGES], lo, hi;  // the bytes it was made from, [r0, r1), and their hull (emulator: guest
                                               // physical addresses)
    u32 stamp;                       // emulator: g_memStamps.now when it was made
} NShape;
static NShape* g_ns;
static u32* g_nsLive;   // slots in use, packed (NShape.live: the position here)
static u32 g_nsLiveN, g_nsBytes, g_nsVerify;
static u8* g_nsGrave[NS_GRAVE];  // retired buffers: the GPU may still read them until the frame's submit is done
static u32 g_nsGraveFrame[NS_GRAVE];  // the frame each was retired in
static u32 g_nsGraveN;
static volatile bool g_nsWant = true;
// Counters (totals): [0] draws drawn converted, [1] conversions, [2] their vertices; old path for want of [3] state
// support, [4] the shape's layout, [5] memory or slots; [6] shapes dropped for a flush on their memory, [7] hash checks,
// [8] of them changed, [9] evictions, [10] draws of shapes left on the old path as changing; ticks of [11] conversions,
// [12] native draws (conversions included); [13] flushed ranges looked at, [14] ring overflows.
u64 g_nsStats[16];
void pica_set_native_shapes(bool on) { g_nsWant = on; }
bool pica_native_shapes(void) { return g_nsWant; }
u32 pica_native_bytes(u32* live) { if (live) *live = g_nsLiveN; return g_nsBytes; }

#ifdef GC3DS_NATIVE
// Which memory converted shapes were made from: a bit per 256 bytes of the heap window (the game's heaps are there; a
// shape with anything outside is not converted). Flushes on marked memory go into a ring the GX thread reads.
#define NW_BASE 0x08000000u
#define NW_SPAN 0x08000000u
#define NW_SHIFT 8
static volatile u32 g_nwBits[(NW_SPAN >> NW_SHIFT) / 32];
#define ND_RING 1024
static struct { u32 lo, hi; } g_ndRing[ND_RING];
static volatile u32 g_ndHead, g_ndTail, g_ndOverflow, g_ndLock;

// Any thread (the game's, through Aurora's DCFlushRange / DCStoreRange / DCInvalidateRange).
void pica_native_store(const void* p, u32 len) {
    u32 a = (u32)(uintptr_t)p;
    if (!len || a - NW_BASE >= NW_SPAN) return;
    u32 end = a + len;
    if (end < a || end - NW_BASE > NW_SPAN) end = NW_BASE + NW_SPAN;
    u32 first = (a - NW_BASE) >> NW_SHIFT, last = (end - 1 - NW_BASE) >> NW_SHIFT;
    bool marked = false;
    for (u32 i = first; i <= last && !marked; i++) {
        u32 w = g_nwBits[i >> 5];
        if (!w) { i |= 31; continue; }
        marked = (w >> (i & 31)) & 1;
    }
    if (!marked) return;
    while (__sync_lock_test_and_set(&g_ndLock, 1)) {}
    u32 h = g_ndHead;
    if (h - g_ndTail >= ND_RING) {
        g_ndOverflow = 1;
    } else {
        g_ndRing[h % ND_RING].lo = a;
        g_ndRing[h % ND_RING].hi = end;
        vw_dmb();
        g_ndHead = h + 1;
    }
    __sync_lock_release(&g_ndLock);
}
static inline const u8* ns_host(u32 a) { return (const u8*)(uintptr_t)a; }  // ranges are host addresses
#else
static const u8* g_nsMem;  // MEM1's host copy: ranges are guest physical addresses
static inline const u8* ns_host(u32 a) { return g_nsMem + a; }
// A page the shape was made from stamped since it was made.
static bool ns_stale(const NShape* e) {
    for (u32 r = 0; r < e->nr; r++)
        for (u32 pg = e->r0[r] >> 12; pg <= (e->r1[r] - 1) >> 12; pg++)
            if ((s32)(g_memStamps.page[pg] - e->stamp) > 0) return true;
    return false;
}
#endif

static inline u32 ns_hash_ptr(const void* p) { u32 h = (u32)(uintptr_t)p * 2654435761u; return h ^ (h >> 15); }

// Shapes seen made again, by command: three times and it stays on the old path (CPU-deformed geometry).
#define NS_DYN 1024
static struct { const u8* cmd; u32 kills; } g_nsDyn[NS_DYN];
static void ns_dyn_note(const u8* cmd) {
    u32 i = ns_hash_ptr(cmd) & (NS_DYN - 1);
    if (g_nsDyn[i].cmd != cmd) { g_nsDyn[i].cmd = cmd; g_nsDyn[i].kills = 0; }
    g_nsDyn[i].kills++;
}
static bool ns_dynamic(const u8* cmd) {
    u32 i = ns_hash_ptr(cmd) & (NS_DYN - 1);
    return g_nsDyn[i].cmd == cmd && g_nsDyn[i].kills >= 3;
}
// A shape that cannot be made (its layout, a matrix row past 27, memory outside the window): left to the old path
// without decoding it again at every draw (the emulator drew ~440 such runs a second, job 275).
// g_nsWhy: [0] texture matrix indices in the vertices or no position, [1] a malformed run or a primitive not taken,
// [2] vertex / index limits, [3] a matrix row past 27, [4] memory outside the window or too many ranges.
u64 g_nsWhy[5];
static void ns_never(const u8* cmd, u32 why) {
    u32 i = ns_hash_ptr(cmd) & (NS_DYN - 1);
    g_nsDyn[i].cmd = cmd;
    g_nsDyn[i].kills = 3;
    g_nsWhy[why]++;
}

static void ns_grave_free_done(void) {
    u32 n = 0;
    for (u32 i = 0; i < g_nsGraveN; i++)
        if (g_nsGraveFrame[i] <= g_doneFrame) linearFree(g_nsGrave[i]);
        else { g_nsGrave[n] = g_nsGrave[i]; g_nsGraveFrame[n++] = g_nsGraveFrame[i]; }
    g_nsGraveN = n;
}

static void ns_kill(NShape* e) {
    if (g_nsGraveN == NS_GRAVE) { g_endWhy[6]++; frame_drain(); }  // begin_frame frees the grave
    g_nsGraveFrame[g_nsGraveN] = g.frameNo;
    g_nsGrave[g_nsGraveN++] = e->mem;
    g_nsBytes -= e->bytes;
    u32 pos = e->live, last = g_nsLive[--g_nsLiveN];
    g_nsLive[pos] = last;
    g_ns[last].live = pos;
    e->cmd = NULL;
    e->mem = NULL;
}

#ifdef GC3DS_NATIVE
// Flushed memory since the last look: the shapes made from it go.
static void ns_dirty(void) {
    u32 h = g_ndHead;
    vw_dmb();
    if (g_ndOverflow) {
        g_ndOverflow = 0;
        while (g_nsLiveN) ns_kill(&g_ns[g_nsLive[0]]);
        g_ndTail = h;
        g_nsStats[14]++;
        return;
    }
    while (g_ndTail != h) {
        u32 lo = g_ndRing[g_ndTail % ND_RING].lo, hi = g_ndRing[g_ndTail % ND_RING].hi;
        vw_dmb();
        g_ndTail = g_ndTail + 1;
        g_nsStats[13]++;
        for (u32 j = 0; j < g_nsLiveN;) {
            NShape* e = &g_ns[g_nsLive[j]];
            bool hit = false;
            if (lo < e->hi && hi > e->lo)
                for (u32 r = 0; r < e->nr && !hit; r++) hit = lo < e->r1[r] && hi > e->r0[r];
            if (!hit) { j++; continue; }
            ns_dyn_note(e->cmd);
            ns_kill(e);  // position j now holds another shape
            g_nsStats[6]++;
        }
    }
}
#endif

static u32 ns_sources_hash(const NShape* e) {
    u32 h = 2166136261u;
    for (u32 r = 0; r < e->nr; r++) {
        u32 lo = e->r0[r] & ~3u;
        h = (h ^ hash_words(ns_host(lo), e->r1[r] - lo)) * 16777619u;
    }
    return h;
}

// Two shapes a present: their sources hashed again (what the flushes would have missed).
static void ns_verify(void) {
    for (int k = 0; k < 2 && g_nsLiveN; k++) {
        NShape* e = &g_ns[g_nsLive[g_nsVerify++ % g_nsLiveN]];
        g_nsStats[7]++;
        if (ns_sources_hash(e) == e->hash) continue;
        g_nsStats[8]++;
        ns_dyn_note(e->cmd);
        ns_kill(e);
    }
}

// The key: vertex format, descriptor and, for indexed attributes, the arrays and their byte order.
static void ns_key(const GXCore* gx, u32 vat, u32* k) {
    u32 lo = gx->cp[0x50], hi = gx->cp[0x60], n = 0, used = 0;
    k[n++] = vat; k[n++] = lo; k[n++] = hi;
    k[n++] = gx->cp[0x70 + vat]; k[n++] = gx->cp[0x80 + vat]; k[n++] = gx->cp[0x90 + vat];
    u32 modes = ((lo >> 9) & 0xFF) | (hi << 8);  // two bits per attribute: position, normal, colours, coordinates
    for (u32 i = 0; i < 12; i++)
        if (((modes >> (2 * i)) & 3) >= 2) { used |= 1u << i; k[n++] = gx->cp[0xA0 + i]; k[n++] = gx->cp[0xB0 + i]; }
    k[n++] = GX_ARRAY_LE(gx) & used;
    while (n < NS_KEY) k[n++] = 0;
}

// Linear memory for a conversion: evicts shapes not drawn in this or the previous frame, oldest first.
static u8* ns_alloc(u32 bytes) {
    for (int tries = 0; tries < 64; tries++) {
        if (g_nsBytes + bytes <= NS_BUDGET) {
            u8* m = linearAlloc(bytes);
            if (m) return m;
        }
        NShape* old = NULL;
        for (u32 j = 0; j < g_nsLiveN; j++) {
            NShape* e = &g_ns[g_nsLive[j]];
            if (e->lastFrame + 1 < g.frameNo && (!old || e->lastFrame < old->lastFrame)) old = e;
        }
        if (!old) return NULL;
        ns_kill(old);
        g_nsStats[9]++;
    }
    return NULL;
}

static bool ns_range(NShape* e, u32 lo, u32 hi) {
    if (lo >= hi) return true;
#ifdef GC3DS_NATIVE
    if (lo - NW_BASE >= NW_SPAN || hi - NW_BASE > NW_SPAN || e->nr == NS_RANGES) return false;
#else
    if (hi > 0x01800000u || e->nr == NS_RANGES) return false;
#endif
    e->r0[e->nr] = lo; e->r1[e->nr] = hi; e->nr++;
    if (lo < e->lo) e->lo = lo;
    if (hi > e->hi) e->hi = hi;
    return true;
}

// What a converted draw is made from: native mode's indexed command (verts, nv unique vertices and ni u16 indices), or
// in the emulator a run of draws inside a display list (list: [cmd, cmd + len) holds draws of format vat and NOPs).
typedef struct { u32 vat; const u8* cmd; u32 len; const u8* verts; u32 nv, vsize; const u8* idx; u32 ni; bool list; } NsReq;

// A run's vertices and indices; false for a primitive the conversion does not take (lines, points) or a malformed run.
// (A run is read in guest memory's layout, ramlayout.h.)
static bool ns_list_count(const NsReq* q, u32* nv, u32* ni) {
    u32 v = 0, t = 0;
    for (u32 off = 0; off < q->len;) {
        u8 op = (u8)ramp_rd8(q->cmd + off);
        if (op == 0x00) { off++; continue; }
        if (op < 0x80 || op >= 0xC0 || (op & 7) != q->vat || off + 3 > q->len) return false;
        u32 prim = (op >> 3) & 7, n = ramp_rd16(q->cmd + off + 1);
        if (prim > PRIM_FAN) return false;
        v += n;
        t += tri_verts(prim, n);
        off += 3 + n * q->vsize;
        if (off > q->len) return false;
    }
    *nv = v;
    *ni = t;
    return true;
}

// Decodes a run's draws one after another into the shape's streams and builds its triangles (the regular path's
// order and winding, vw_assemble_idx); merges the array ranges they read into inf.
static void ns_list_fill(const GXCore* gx, const NsReq* q, VNatInfo* inf, float (*pos)[4], float (*nrm)[3], u8 (*col[2])[4],
                         float (*tex[8])[2], u16* ix) {
    u32 at = 0, k = 0;
    for (u32 r = 0; r < 12; r++) inf->lo[r] = inf->hi[r] = 0;
    for (u32 off = 0; off < q->len;) {
        const u8* c = q->cmd + off;
        u32 op = ramp_rd8(c);
        if (op == 0x00) { off++; continue; }
        u32 prim = (op >> 3) & 7, n = ramp_rd16(c + 1);
        VNatInfo d = *inf;
        u8 (*c2[2])[4] = { col[0] ? col[0] + at : NULL, col[1] ? col[1] + at : NULL };
        float (*t8[8])[2];
        for (u32 i = 0; i < 8; i++) t8[i] = tex[i] ? tex[i] + at : NULL;
        vfast_native_decode(gx, q->vat, c + 3, n, q->vsize, &d, pos + at, nrm ? nrm + at : NULL, c2, t8);
        for (u32 r = 0; r < 12; r++) {
            if (d.lo[r] == d.hi[r]) continue;
            if (inf->lo[r] == inf->hi[r]) { inf->lo[r] = d.lo[r]; inf->hi[r] = d.hi[r]; continue; }
            if (d.lo[r] < inf->lo[r]) inf->lo[r] = d.lo[r];
            if (d.hi[r] > inf->hi[r]) inf->hi[r] = d.hi[r];
        }
        switch (prim) {
        case PRIM_QUADS: case PRIM_QUADS2:
            for (u32 i = 0; i + 3 < n; i += 4) {
                u16 a = (u16)(at + i);
                ix[k++] = a; ix[k++] = a + 1; ix[k++] = a + 2;
                ix[k++] = a; ix[k++] = a + 2; ix[k++] = a + 3;
            }
            break;
        case PRIM_TRIS: for (u32 i = 0; i + 2 < n; i += 3) { ix[k++] = (u16)(at + i); ix[k++] = (u16)(at + i + 1); ix[k++] = (u16)(at + i + 2); } break;
        case PRIM_STRIP:
            for (u32 i = 0; i + 2 < n; i++) {
                if (i & 1) { ix[k++] = (u16)(at + i + 1); ix[k++] = (u16)(at + i); }
                else { ix[k++] = (u16)(at + i); ix[k++] = (u16)(at + i + 1); }
                ix[k++] = (u16)(at + i + 2);
            }
            break;
        case PRIM_FAN: for (u32 i = 1; i + 1 < n; i++) { ix[k++] = (u16)at; ix[k++] = (u16)(at + i); ix[k++] = (u16)(at + i + 1); } break;
        default: break;
        }
        at += n;
        off += 3 + n * q->vsize;
    }
}

static NShape* ns_build(const GXCore* gx, const NsReq* q, const u32* key, NShape* e) {
    VNatInfo inf;
    u32 vat = q->vat, nv = q->nv, ni = q->ni;
    const u8* cmd = q->cmd;
    u32 len = q->len;
    if (!vfast_native_info(gx, vat, &inf)) { g_nsStats[4]++; ns_never(cmd, 0); return NULL; }
    if (q->list && !ns_list_count(q, &nv, &ni)) { g_nsStats[4]++; ns_never(cmd, 1); return NULL; }
    if (!nv || nv > 0xFFFF || ni < 3) { g_nsStats[4]++; ns_never(cmd, 2); return NULL; }
    u32 off[NO_N], at = 0;
    static const u8 kSize[NO_IDX] = { 16, 12, 4, 4, 8, 8, 8, 8, 8, 8, 8, 8 };
    for (u32 k = 0; k < NO_IDX; k++) {
        bool has = k == NO_POS || (inf.present & (k == NO_NRM ? VN_NRM : k == NO_COL0 ? VN_COL0 : k == NO_COL1 ? VN_COL1
                                                 : VN_TEX0 << (k - NO_TEX0)));
        off[k] = has ? at : ~0u;
        if (has) at = (at + kSize[k] * nv + 15) & ~15u;
    }
    off[NO_IDX] = at;
    at += ni * 2;
    u32 bytes = (at + 15) & ~15u;
    u8* mem = ns_alloc(bytes);
    if (!mem) { g_nsStats[5]++; return NULL; }
    u64 t0 = g_profClock ? g_profClock() : 0;
#ifndef GC3DS_NATIVE
    u32 stamp = g_memStamps.now;  // read first: a change while decoding makes the shape stale at once
#endif
    float (*pos)[4] = (float (*)[4])(void*)(mem + off[NO_POS]);
    float (*nrm)[3] = off[NO_NRM] != ~0u ? (float (*)[3])(void*)(mem + off[NO_NRM]) : NULL;
    u8 (*col[2])[4] = { off[NO_COL0] != ~0u ? (u8 (*)[4])(mem + off[NO_COL0]) : NULL,
                        off[NO_COL1] != ~0u ? (u8 (*)[4])(mem + off[NO_COL1]) : NULL };
    float (*tex[8])[2];
    for (u32 i = 0; i < 8; i++) tex[i] = off[NO_TEX0 + i] != ~0u ? (float (*)[2])(void*)(mem + off[NO_TEX0 + i]) : NULL;
    u16* ix = (u16*)(void*)(mem + off[NO_IDX]);
    if (q->list) ns_list_fill(gx, q, &inf, pos, nrm, col, tex, ix);
    else { vfast_native_decode(gx, vat, q->verts, nv, q->vsize, &inf, pos, nrm, col, tex); memcpy(ix, q->idx, ni * 2); }
    // Matrix rows (each vertex's own, or the draw's default: row 0 here, loaded per draw) and the origin.
    u32 rows = 0;
    float mn[3] = { 1e30f, 1e30f, 1e30f }, mx[3] = { -1e30f, -1e30f, -1e30f };
    bool ok = true, rowsOk = true;
    for (u32 i = 0; i < nv; i++) {
        float* p = pos[i];
        if (inf.pnDirect) {
            u32 pn = (u32)p[3];
            if (pn > 27) ok = rowsOk = false;
            else rows |= 1u << pn;
        } else {
            p[3] = 0;
        }
        for (int c = 0; c < 3; c++) { if (p[c] < mn[c]) mn[c] = p[c]; if (p[c] > mx[c]) mx[c] = p[c]; }
    }
    float o[3];
    for (int c = 0; c < 3; c++) o[c] = nv ? 0.5f * (mn[c] + mx[c]) : 0;
    for (u32 i = 0; i < nv; i++) for (int c = 0; c < 3; c++) pos[i][c] -= o[c];
    for (u32 t = 0; t + 2 < ni; t += 3)  // a bad index makes its triangle degenerate (as vw_do_indexed)
        if (ix[t] >= nv || ix[t + 1] >= nv || ix[t + 2] >= nv) ix[t] = ix[t + 1] = ix[t + 2] = 0;
    e->nr = 0;
    e->lo = 0xFFFFFFFFu;
    e->hi = 0;
#ifdef GC3DS_NATIVE
    u32 c0 = (u32)(uintptr_t)cmd;
#else
    u32 c0 = (u32)(cmd - g_nsMem);
#endif
    ok = ok && ns_range(e, c0, c0 + len);
    for (u32 k = 0; k < 12 && ok; k++) ok = ns_range(e, inf.lo[k], inf.hi[k]);
    if (!ok) {
        linearFree(mem);  // never handed to the GPU
        g_nsStats[4]++;
        ns_never(cmd, rowsOk ? 4 : 3);
        return NULL;
    }
    svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)mem, bytes);
    e->cmd = cmd;
    e->len = len; e->nv = nv; e->ni = ni / 3 * 3; e->bytes = bytes; e->present = inf.present; e->rows = rows;
    e->pnDirect = inf.pnDirect;
    memcpy(e->key, key, sizeof e->key);
    e->mem = mem;
    memcpy(e->off, off, sizeof off);
    memcpy(e->origin, o, sizeof o);
    e->hash = ns_sources_hash(e);
#ifdef GC3DS_NATIVE
    for (u32 r = 0; r < e->nr; r++)
        for (u32 i = (e->r0[r] - NW_BASE) >> NW_SHIFT; i <= (e->r1[r] - 1 - NW_BASE) >> NW_SHIFT; i++)
            g_nwBits[i >> 5] |= 1u << (i & 31);
#else
    e->stamp = stamp;
#endif
    e->live = g_nsLiveN;
    g_nsLive[g_nsLiveN++] = (u32)(e - g_ns);
    g_nsBytes += bytes;
    g_nsStats[1]++;
    g_nsStats[2] += nv;
    if (t0) g_nsStats[11] += g_profClock() - t0;
    return e;
}

// The shape for this command and key, converted if it is not yet; NULL: the old path draws it.
static NShape* ns_get(const GXCore* gx, const NsReq* q) {
    u32 key[NS_KEY];
    const u8* cmd = q->cmd;
    u32 len = q->len;
    ns_key(gx, q->vat, key);
    u32 h = ns_hash_ptr(cmd);
    NShape* freeSlot = NULL;
    NShape* oldest = NULL;
    for (u32 p = 0; p < NS_PROBE; p++) {
        NShape* e = &g_ns[(h + p) & (NS_SLOTS - 1)];
        if (!e->cmd) { if (!freeSlot) freeSlot = e; continue; }
        if (e->cmd == cmd && e->len == len && !memcmp(e->key, key, sizeof key)) {
#ifndef GC3DS_NATIVE
            if (ns_stale(e)) {  // made from memory the guest has changed since: made again (or left to the old path)
                ns_dyn_note(cmd);
                ns_kill(e);
                g_nsStats[6]++;
                if (ns_dynamic(cmd)) { g_nsStats[10]++; return NULL; }
                return ns_build(gx, q, key, e);
            }
#endif
            return e;
        }
        if (!oldest || e->lastFrame < oldest->lastFrame) oldest = e;
    }
    if (!freeSlot) {  // the probe window is full: its least recently drawn shape goes, if not drawn this frame or the last
        if (!oldest || oldest->lastFrame + 1 >= g.frameNo) { g_nsStats[5]++; return NULL; }
        ns_kill(oldest);
        g_nsStats[9]++;
        freeSlot = oldest;
    }
    return ns_build(gx, q, key, freeSlot);
}

// Palette rows dst..dst+2 = XF rows src..src+2 (a 3x4 matrix) with `o` folded into the translation; uploaded only when
// they differ from what the rows hold.
static void ns_rows(const GXCore* gx, u32 src, u32 dst, const float* o, bool fold) {
    float m[12];
    memcpy(m, &gx->xf[(src & 63) * 4], sizeof m);
    if (fold) for (int r = 0; r < 3; r++) m[4 * r + 3] += m[4 * r] * o[0] + m[4 * r + 1] * o[1] + m[4 * r + 2] * o[2];
    if (((g.natRowValid >> dst) & 7) == 7 && !memcmp(g.natRow[dst], m, sizeof m)) return;
    memcpy(g.natRow[dst], m, sizeof m);
    g.natRowValid |= 7ull << dst;
    C3D_FVec* u = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, g.uPal + (int)dst, 3);
    for (int r = 0; r < 3; r++) u[r] = FVec4_New(m[4 * r], m[4 * r + 1], m[4 * r + 2], m[4 * r + 3]);
    g_picaStats.palUploads++;
}

// Texture unit u's texgen matrix (palette rows 30 + 3u ..), m as the shader applies it to (x, y, z, 1).
static void ns_texrows(u32 u, const float* m) {
    u32 dst = 30 + 3 * u;
    if (((g.natRowValid >> dst) & 7) == 7 && !memcmp(g.natRow[dst], m, 12 * sizeof(float))) return;
    memcpy(g.natRow[dst], m, 12 * sizeof(float));
    g.natRowValid |= 7ull << dst;
    C3D_FVec* v = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, g.uPal + (int)dst, 3);
    for (int r = 0; r < 3; r++) v[r] = FVec4_New(m[4 * r], m[4 * r + 1], m[4 * r + 2], m[4 * r + 3]);
}

// The native batch's colour: lighting uniforms in the native path's layout (lights packed, lcount over them).
static void ns_light_uniforms(const VLightUniforms* u, u32 n, bool lit) {
    if (lit) {
        C3D_FVec* L = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, g.uLights, (int)(5 * n));
        for (u32 i = 0; i < 5 * n; i++) L[i] = FVec4_New(u->lights[i][0], u->lights[i][1], u->lights[i][2], u->lights[i][3]);
        unif_rows(g.uSel, u->sel, 2);
        unif_rows(g.uAmb, &u->amb, 1);
        C3D_IVUnifSet(GPU_VERTEX_SHADER, g.uLcount, (int)n - 1, 0, 5, 0);
    }
    unif_rows(g.uMat, &u->mat, 1);
    unif_rows(g.uVsel, &u->vsel, 1);
    unif_set(g.uOn, u->on[0], u->on[1], lit ? 1.0f : 0.0f, 0);
}

static struct { VLightUniforms lu; u32 n; bool lit; } g_nl;  // the open native batch's colour

// Options (gxopt letters, for bisection on the 2DS): NSO_NOFIXED inputs a shape lacks come from constant buffers
// instead of the PICA's fixed attributes; NSO_NOLIGHT lit draws stay on the old path.
enum { NSO_NOFIXED = 1, NSO_NOLIGHT = 2 };
static volatile u32 g_nsOpt;
void pica_set_native_opts(u32 opts) { g_nsOpt = opts; }
static u8 *g_nsZero, *g_nsWhite;  // 64K vertices of bytes 0 / 255 (NSO_NOFIXED)

// An input the shape does not have, in register reg: white (colour, bytes 255) or zero.
static void ns_const(C3D_AttrInfo* ai, C3D_BufInfo* bi, int reg, bool white) {
    if ((g_nsOpt & NSO_NOFIXED) && g_nsZero && g_nsWhite) {
        int id = AttrInfo_AddLoader(ai, reg, GPU_UNSIGNED_BYTE, 4);
        BufInfo_Add(bi, white ? g_nsWhite : g_nsZero, 4, 1, (u64)id);
        return;
    }
    int id = AttrInfo_AddFixed(ai, reg);
    float v = white ? 255.0f : 0.0f;
    C3D_FixedAttribSet(id, v, v, v, v);
}

// What the native path supports of the current state (t: its TEV program): texgen of the regular kind only, and a
// colour channel vfast_native_light takes.
static bool ns_state_ok(const GXCore* gx, const PicaTev* t, bool fill) {
    VSel sel = { t->rasChan, t->numTex, { t->texCoord[0], t->texCoord[1], t->texCoord[2] } };
    u32 numGens = gx->xf[0x103F] & 15;
    for (u32 u = 0; u < sel.numTex && u < 3; u++) {
        u32 gi = sel.texCoord[u] & 7;
        if (gi < numGens && ((gx->xf[0x1040 + gi] >> 4) & 7) != 0) return false;
    }
    VLightUniforms lu;
    u32 n, colSrc;
    bool lit;
    if (!vfast_native_light(gx, &sel, 0, fill ? &g_nl.lu : &lu, fill ? &g_nl.n : &n, &colSrc, fill ? &g_nl.lit : &lit)) return false;
    if ((g_nsOpt & NSO_NOLIGHT) && (fill ? g_nl.lit : lit)) return false;
    return true;
}

static bool ns_draw(System* s, const NsReq* q) {
    if (!g_nsWant || !g_ns || g.uNat < 0 || g.uLcount < 0) return false;
    if (!q->list && (q->nv > 0xFFFF || q->nv == 0 || q->ni < 3)) return false;
    if (g_fskipNow) { g_fskipDraws++; return true; }
    if (ns_dynamic(q->cmd)) { g_nsStats[10]++; return false; }
    u64 t0 = g_profClock ? g_profClock() : 0;
    GXCore* gx = &s->gx.core;
    if (g_gxMergeOpen) merge_issue(s);
    vw_apply();
    begin_frame();
    if (C3D_GetCmdBufUsage() > g_cmdbufLimit) { g_endWhy[7]++; end_frame(); begin_frame(); }
#ifdef GC3DS_NATIVE
    ns_dirty();
#endif
    u32 cull = (gx->bp[0x00] >> 14) & 3;
    if (cull == 3) { g_nsStats[0]++; return true; }  // every triangle culled
    u32 zop;
    float zz;
    if (ztex_value(s, gx, &zop, &zz)) { g_nsStats[3]++; return false; }  // Z-texture depth: the CPU's
    const u32 cls = 8;
    bool same = g.batchOpen && gx->bpVersion == g.batchBpVer && gx->tlutVersion == g.batchTlutVer &&
                gx->xfLightVersion == g.batchLightVer && g.xfbSeq == g.batchXfbSeq && cls == g.batchCls;
    u32 key[96], kn = 0;
    if (!same) {
        kn = batch_key(gx, key);
        key[kn++] = cls;
        same = g.batchOpen && kn <= g.batchKeyLen && !memcmp(key, g.batchKey, kn * 4);
        if (same) { tex_key(gx, key, &kn); same = kn == g.batchKeyLen && !memcmp(key, g.batchKey, kn * 4); }
    }
    if (!same) {  // a native batch opens: its state must be one the native path supports
        u32 tk[72], tn = tev_key(gx, tk);
        PicaTev tmp;
        const PicaTev* t = g.tevValid && !memcmp(tk, g.tevKey, tn * 4) ? &g.tev : tev_lookup(gx, tk, tn, &tmp, NULL);
        if (!ns_state_ok(gx, t, false)) { g_nsStats[3]++; return false; }
    }
    u32 frameBefore = g.frameNo;
    NShape* e = ns_get(gx, q);
    if (!e) return false;
    if (g.frameNo != frameBefore) same = false;  // a retired shape ended the frame: state is applied again
    for (int attempt = 0; !same && attempt < 3; attempt++) {
        frameBefore = g.frameNo;
        flush_batch();
        split_early();
        update_tev(gx);
        kn = batch_key(gx, key);
        key[kn++] = cls;
        tex_key(gx, key, &kn);
        C3D_Tex* tex[PICA_TEXUNITS] = { NULL, NULL, NULL };
        for (u32 i = 0; i < g.tev.numTex && i < PICA_TEXUNITS; i++) tex[i] = fetch_texture(s, gx, g.tev.texMap[i] & 7);
        if (g.frameNo != frameBefore) continue;  // a texture ended the frame: once more in the new one
        ns_state_ok(gx, &g.tev, true);
        apply_frag(gx);
        for (u32 i = 0; i < g.tev.numTex && i < PICA_TEXUNITS; i++) bind_tex((int)i, tex[i] ? tex[i] : &g.white);
        ns_light_uniforms(&g_nl.lu, g_nl.n, g_nl.lit);
        cull_face(kCull[cull]);
        g.batchGpuLight = false;
        g.batchGpuPos = true;
        g.batchCull = kCull[cull];
        g.batchOpen = true;
        g.batchFirst = g.arenaUsed;
        g.batchIdxFirst = g.idxUsed;
        g_recJob = g_vwHead;
        g.batchIdx = true;
        g.palSerial++;
        memcpy(g.batchKey, key, kn * 4);
        g.batchKeyLen = kn;
        same = true;
    }
    if (!same) return false;
    g.batchBpVer = gx->bpVersion; g.batchTlutVer = gx->tlutVersion; g.batchLightVer = gx->xfLightVersion;
    g.batchXfbSeq = g.xfbSeq; g.batchCls = cls;
    split_early();
    e->lastFrame = g.frameNo;
    // Projection (as the GPU transform's), when the XF registers changed.
    if (!g.natProjOk || g.natProjVer != gx->xfRegVersion || !g.projValid) {
        VFold f;
        vfold_init(gx, SX * 2.0f / TW, SY * 2.0f / TH, FLIP_Y, &f);
        float P[16];
        vfast_proj(gx, &f, P);
        if (!g.projValid || memcmp(P, g.proj, sizeof P)) {
            memcpy(g.proj, P, sizeof P);
            C3D_FVec* u = C3D_FVUnifWritePtr(GPU_VERTEX_SHADER, g.uProj, 4);
            for (int r = 0; r < 4; r++) u[r] = FVec4_New(P[4 * r], P[4 * r + 1], P[4 * r + 2], P[4 * r + 3]);
            g.projValid = true;
        }
        g.natProjVer = gx->xfRegVersion;
        g.natProjOk = true;
    }
    // Position matrices: the rows the vertices name, or the draw's default matrix in rows 0-2.
    if (e->pnDirect) {
        for (u32 r = e->rows; r; r &= r - 1) { u32 row = (u32)__builtin_ctz(r); ns_rows(gx, row, row, e->origin, true); }
    } else {
        ns_rows(gx, gx->cp[0x30] & 63, 0, e->origin, true);
    }
    g.palValid = 0;  // the GPU transform's palette slots are overwritten
    g.lastValid = false;
    // Vertex layout: this shape's buffers; texture units' inputs by their texgen source.
    C3D_AttrInfo* ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    C3D_BufInfo* bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    u8* m = e->mem;
    int id = AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 4);
    BufInfo_Add(bi, m + e->off[NO_POS], 16, 1, (u64)id);
    {
        u32 ch = g.tev.rasChan & 1, c = ch;
        if (ch == 1 && !(e->present & VN_COL1) && (e->present & VN_COL0)) c = 0;
        if (e->present & (VN_COL0 << c)) {
            id = AttrInfo_AddLoader(ai, 1, GPU_UNSIGNED_BYTE, 4);
            BufInfo_Add(bi, m + e->off[NO_COL0 + c], 4, 1, (u64)id);
        } else {
            ns_const(ai, bi, 1, true);
        }
    }
    u32 ma = gx->cp[0x30], mb = gx->cp[0x40], numGens = gx->xf[0x103F] & 15;
    // Only the inputs the draw uses: every attribute past three costs ~45% of the vertex rate (GPU_GUIDANCE rule 2); the
    // shader's reads of the others feed outputs nothing samples.
    for (u32 u = 0; u < g.tev.numTex && u < 3; u++) {
        float tm[12] = { 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 1 };
        u32 gi = g.tev.texCoord[u] & 7;
        int src = -1;  // -1 none (0, 0, 1), 0 position, 1 normal, 5.. coordinate
        if (u < g.tev.numTex && gi < numGens) {
            u32 cfg = gx->xf[0x1040 + gi], sr = (cfg >> 7) & 31, tmi = gi < 4 ? (ma >> (6 + 6 * gi)) & 63 : (mb >> (6 * (gi - 4))) & 63;
            memcpy(tm, &gx->xf[tmi * 4], 12 * sizeof(float));
            if (!((cfg >> 1) & 1)) { tm[8] = tm[9] = tm[10] = 0; tm[11] = 1; }  // s, t only: q = 1
            src = sr <= 1 ? (int)sr : sr >= 5 && sr <= 12 ? (int)sr : -1;
            if (src == 1 && !(e->present & VN_NRM)) src = -2;  // no normal: (0, 0, 0)
            if (src >= 5 && !(e->present & (VN_TEX0 << (src - 5)))) src = -3;  // no coordinate: (0, 0)
        }
        if (src == 0) {  // position, relative to the origin: fold it in
            for (int r = 0; r < 3; r++) tm[4 * r + 3] += tm[4 * r] * e->origin[0] + tm[4 * r + 1] * e->origin[1] + tm[4 * r + 2] * e->origin[2];
            // All four floats of the 16-byte position (the shader reads only xyz): a loader narrower than its buffer's
            // stride hung the GPU (probe job 177), the suspect in native mode's hang at frame 241 (jobs 242, 244).
            id = AttrInfo_AddLoader(ai, 2 + (int)u, GPU_FLOAT, 4);
            BufInfo_Add(bi, m + e->off[NO_POS], 16, 1, (u64)id);
        } else if (src == 1) {
            id = AttrInfo_AddLoader(ai, 2 + (int)u, GPU_FLOAT, 3);
            BufInfo_Add(bi, m + e->off[NO_NRM], 12, 1, (u64)id);
        } else {
            // A coordinate is loaded as (s, t, 0) and GX's input is (s, t, 1); no input (or none present) is (0, 0, 1):
            // the z column joins the translation and the input's z is 0. No normal present: (0, 0, 0).
            if (src != -2) for (int r = 0; r < 3; r++) { tm[4 * r + 3] += tm[4 * r + 2]; tm[4 * r + 2] = 0; }
            if (src >= 5) {
                id = AttrInfo_AddLoader(ai, 2 + (int)u, GPU_FLOAT, 2);
                BufInfo_Add(bi, m + e->off[NO_TEX0 + (src - 5)], 8, 1, (u64)id);
            } else {
                ns_const(ai, bi, 2 + (int)u, false);
            }
        }
        ns_texrows(u, tm);
    }
    if (g_nl.lit && (e->present & VN_NRM)) {
        id = AttrInfo_AddLoader(ai, 6, GPU_FLOAT, 3);
        BufInfo_Add(bi, m + e->off[NO_NRM], 12, 1, (u64)id);
    } else if (g_nl.lit) {
        ns_const(ai, bi, 6, false);
    }
    if (!g.natAttr) { C3D_FVUnifSet(GPU_VERTEX_SHADER, g.uNat, 1, 0, 0, 0); g.natAttr = true; }
    g_gxWhere = GW_SHAPES;
    C3D_DrawElements(GPU_TRIANGLES, (int)e->ni, C3D_UNSIGNED_SHORT, m + e->off[NO_IDX]);
    g_picaStats.drawCalls++;
    g_nsSince += e->ni;
    g_nsStats[0]++;
    if (t0) g_nsStats[12] += g_profClock() - t0;
    return true;
}

#ifdef GC3DS_NATIVE
static bool pica_draw_native(void* ctx, System* s, u32 vat, const u8* cmd, u32 len, const u8* verts, u32 nv, u32 vsize,
                             const u8* idx, u32 ni) {
    (void)ctx;
    NsReq q = { vat, cmd, len, verts, nv, vsize, idx, ni, false };
    return ns_draw(s, &q);
}
#else
// The emulator's display-list runs (GXBackend.draw_list).
static bool pica_draw_list(void* ctx, System* s, u32 vat, const u8* cmd, u32 len) {
    (void)ctx;
    g_gxWhere = GW_DLIST;
    g_nsMem = s->mem1;
    vfast_native_mem(s->mem1);
    NsReq q = { vat, cmd, len, NULL, 0, gx_vertex_size(&s->gx.core, vat), NULL, 0, true };
    return ns_draw(s, &q);
}
#endif

#ifdef GC3DS_NATIVE
static const GXBackend kPica = { pica_draw, pica_efb_copy, pica_flush, pica_draw_indexed, pica_draw_native, NULL };
#else
// The emulator's entry points, each marking the GX thread back in the decoder when it returns (the sampled position,
// '# gx thread where 10s', would otherwise credit the decoding that follows to the backend's last position).
static void pica_draw_m(void* c, System* s, u32 prim, u32 vat, const u8* raw, u32 count, u32 vsize) {
    pica_draw(c, s, prim, vat, raw, count, vsize);
    g_gxWhere = GW_FIFO;
}
static bool pica_efb_copy_m(void* c, System* s, u32 ctl, u32 x0, u32 y0, u32 w, u32 h, u32 dest) {
    bool r = pica_efb_copy(c, s, ctl, x0, y0, w, h, dest);
    g_gxWhere = GW_FIFO;
    return r;
}
static void pica_flush_m(void* c, System* s) {
    pica_flush(c, s);
    g_gxWhere = GW_FIFO;
}
static bool pica_draw_indexed_m(void* c, System* s, u32 vat, const u8* verts, u32 nv, u32 vsize, const u8* idx, u32 ni) {
    bool r = pica_draw_indexed(c, s, vat, verts, nv, vsize, idx, ni);
    g_gxWhere = GW_FIFO;
    return r;
}
static bool pica_draw_list_m(void* c, System* s, u32 vat, const u8* cmd, u32 len) {
    bool r = pica_draw_list(c, s, vat, cmd, len);
    g_gxWhere = GW_FIFO;
    return r;
}
static const GXBackend kPica = { pica_draw_m, pica_efb_copy_m, pica_flush_m, pica_draw_indexed_m, NULL, pica_draw_list_m };
#endif

// ---- present ----
// ---- screenshots of what the top screen shows (sdmc:/gc3ds/shots, key points only) ----
// The CPU cannot read VRAM: present copies the finished top-screen render target (240 x 400 RGBA8, tiled) into a
// linear buffer with the GPU, and the next present, once that frame is complete, writes it as a 400 x 240 BMP.
#define CAP_W 240
#define CAP_H 400
static u32* g_capBuf;
static volatile int g_capState;  // 0 idle, 1 requested (CPU thread), 2 copy queued (GX thread), 3 writing (core 1)
static char g_capPath[96];

void pica_capture(const char* path) {
    if (g_capState != 0) return;
    snprintf(g_capPath, sizeof g_capPath, "%s", path);
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 5" : : "r"(0) : "memory");  // the path before the request
    g_capState = 1;
}

// The BMP is built in memory on the GX thread (a few ms) and written to the card by a low-priority thread on core 1:
// writing it on the GX thread, row by row, stalled graphics for 2.8 s on the 2DS (2026-10-11).
static u8* g_capFile;  // 24-bit BMP, rows bottom-up, 400 x 240
void (*pica_capture_done)(const void* bmp, u32 bytes);  // called on the writer thread with the finished BMP
#define CAP_FILE_BYTES (54u + 400u * 240u * 3u)
static Thread g_capThread;

static void cap_writer(void* arg) {
    (void)arg;
    for (;;) {
        if (g_capState == 3) {
            __asm__ volatile("mcr p15, 0, %0, c7, c10, 5" : : "r"(0) : "memory");
            if (pica_capture_done) pica_capture_done(g_capFile, CAP_FILE_BYTES);  // (from memory: no read back from the card)
            FILE* f = fopen(g_capPath, "wb");
            if (f) { fwrite(g_capFile, 1, CAP_FILE_BYTES, f); fclose(f); }
            g_capState = 0;
        }
        svcSleepThread(50000000LL);
    }
}

static void cap_write(void) {
    if (!g_capFile && !(g_capFile = malloc(CAP_FILE_BYTES))) { g_capState = 0; return; }
    if (!g_capThread) {
        s32 prio = 0x30;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        g_capThread = threadCreate(cap_writer, NULL, 16 * 1024, prio + 4 > 0x3F ? 0x3F : prio + 4, 1, true);
        if (!g_capThread) { g_capState = 0; return; }
    }
    GSPGPU_InvalidateDataCache(g_capBuf, CAP_W * CAP_H * 4);
    u8* hdr = g_capFile;
    memset(hdr, 0, 54);
    hdr[0] = 'B'; hdr[1] = 'M';
    u32 img = 400 * 240 * 3, v;
    v = 54 + img; memcpy(hdr + 2, &v, 4);
    v = 54; memcpy(hdr + 10, &v, 4);
    v = 40; memcpy(hdr + 14, &v, 4);
    v = 400; memcpy(hdr + 18, &v, 4);
    v = 240; memcpy(hdr + 22, &v, 4);
    hdr[26] = 1; hdr[28] = 24;
    v = img; memcpy(hdr + 34, &v, 4);
    u8* row = g_capFile + 54;
    for (int y = 239; y >= 0; y--, row += 400 * 3) {  // screen row y from the top
        for (int x = 0; x < 400; x++) {
            // The top screen's render target is rotated: target column tx = 239 - y, target row ty = x (checked
            // against Azahar's own screenshot of graphics_gx_texturetest: text reads the right way round).
            u32 tx = 239 - (u32)y, ty = (u32)x;
            u32 p = g_capBuf[((ty >> 3) * (CAP_W >> 3) + (tx >> 3)) * 64 + g_morton[(ty & 7) * 8 + (tx & 7)]];
            row[3 * x] = (u8)(p >> 8); row[3 * x + 1] = (u8)(p >> 16); row[3 * x + 2] = (u8)(p >> 24);  // B G R
        }
    }
    __asm__ volatile("mcr p15, 0, %0, c7, c10, 5" : : "r"(0) : "memory");
    g_capState = 3;  // the writer thread takes it from here
}

static void ns_verify(void);
bool pica_present(System* s, u32 xfbAddr) {
    pica_merge_flush(s, false);
    if (g.frame) ns_verify();
    if (g_capState == 2) {  // the copy was queued with the previous present
        if (g_dbl) q_wait(g_capFence, 4);
        cap_write();
    }
    PXfb* show = NULL;
    for (u32 i = 0; i < MAX_XFB; i++) {
        PXfb* x = &g.xfb[i];
        if (!x->seq) continue;
        if (xfbAddr >= x->addr && xfbAddr <= x->addr + 2 * 1280) { show = x; break; }
        if (!show || x->seq > show->seq) show = x;
    }
    if (!show) return false;
    if (!need(6)) return true;
    flush_batch();
    C3D_FrameDrawOn(g.screen);
    quad_state(&show->tex, true);
    C3D_TexEnv* e = C3D_GetTexEnv(0);
    C3D_TexEnvInit(e);
    C3D_TexEnvSrc(e, C3D_Both, GPU_TEXTURE0, GPU_PRIMARY_COLOR, GPU_PRIMARY_COLOR);
    C3D_TexEnvFunc(e, C3D_RGB, GPU_REPLACE);
    C3D_TexEnvSrc(e, C3D_Alpha, GPU_CONSTANT, GPU_CONSTANT, GPU_CONSTANT);
    C3D_TexEnvColor(e, 0xFFFFFFFF);
    // Screen pixels (x right, y up) -> the rotated top-screen target, as citro3d's own examples do.
    C3D_Mtx proj;
    Mtx_OrthoTilt(&proj, 0.0f, 400.0f, 0.0f, 240.0f, 0.0f, 1.0f, true);
    static const float corner[4][2] = { { 0, 240 }, { 400, 240 }, { 400, 0 }, { 0, 0 } };  // top-left first, clockwise
    float s0 = efb_s((float)show->x0), s1 = efb_s((float)(show->x0 + show->w));
    float t0 = efb_t((float)show->y0), t1 = efb_t((float)(show->y0 + show->h));
    float st[4][2] = { { s0, t0 }, { s1, t0 }, { s1, t1 }, { s0, t1 } };
    PVert* o = g.arena + g.arenaUsed;
    static const int order[6] = { 0, 1, 2, 0, 2, 3 };
    for (int k = 0; k < 6; k++) {
        int i = order[k];
        C3D_FVec p = Mtx_MultiplyFVec4(&proj, FVec4_New(corner[i][0], corner[i][1], 0.5f, 1.0f));
        PVert* v = &o[k];
        memset(v, 0, sizeof *v);
        v->pos[0] = p.x; v->pos[1] = p.y; v->pos[2] = p.z; v->pos[3] = p.w;
        v->col[0] = v->col[1] = v->col[2] = v->col[3] = 255;
        v->tc[0][0] = st[i][0]; v->tc[0][1] = st[i][1];
    }
    draw_arrays(g.arenaUsed, 6);
    g.arenaUsed += 6;
    if (g_capState == 1 && (g_capBuf || (g_capBuf = linearAlloc(CAP_W * CAP_H * 4)))) {
        split(1);  // the screen quad is drawn before the copy reads the target
        GX_TextureCopy((u32*)g.screen->frameBuf.colorBuf, 0, g_capBuf, 0, CAP_W * CAP_H * 4, GX_TRANSFER_RAW_COPY(1));
        queue_run();
        g_capState = 2;
        if (g_dbl) g_capFence = q_end();
    }
    end_frame_at(true);  // queues the transfer to the top framebuffer and the swap
    return true;
}

void pica_set_shader_light(bool on) {
    bool ok = g.uLights >= 0 && g.uOn >= 0 && g.dvlb;
    g.shaderLight = on && ok;
    g.batchOpen = false;
}

bool pica_shader_light(void) { return g.shaderLight; }

// Called from the CPU thread: the GX thread applies it at its next draw (a batch never mixes the two transforms).
void pica_set_gpu_transform(bool on) { g.gpuPos = on && g.uProj >= 0 && g.uPal >= 0 && g.uOn >= 0; }
bool pica_gpu_transform(void) { return g.gpuPos; }

void pica_reset(void) {
    g_mg.nSeg = 0;  // the program is going: its open draw too
    g_gxMergeOpen = false;
    vw_sync_all();
    vfast_reset_cache();
    g.palValid = 0;
    g.projValid = false;
    g.lastValid = false;
    // Copy and XFB records are only bookkeeping and go now; textures are freed once the GPU is idle.
    for (u32 i = 0; i < MAX_COPIES; i++) g.copies[i].used = false;
    for (u32 i = 0; i < MAX_XFB; i++) g.xfb[i].seq = 0;
    g.resetPending = true;
    g.tevValid = g.stateValid = false;
}

bool pica_attach(System* s) {
    if (!g.ready) {
        for (u32 y = 0; y < 8; y++)
            for (u32 x = 0; x < 8; x++)
                g_morton[y * 8 + x] = (u8)((x & 1) | ((y & 1) << 1) | ((x & 2) << 1) | ((y & 2) << 2) | ((x & 4) << 2) | ((y & 4) << 3));
        if (!C3D_Init(CMDBUF_BYTES)) return false;
        find_c3d_queue();
        lean_init();
        g.dvlb = DVLB_ParseFile((u32*)gxpass_native_shbin, gxpass_native_shbin_size);
        if (!g.dvlb) return false;
        shaderProgramInit(&g.prog);
        shaderProgramSetVsh(&g.prog, &g.dvlb->DVLE[0]);
        shaderInstance_s* vs = g.prog.vertexShader;
        g.uLights = shaderInstanceGetUniformLocation(vs, "lights");
        g.uSel = shaderInstanceGetUniformLocation(vs, "lsel");
        g.uMat = shaderInstanceGetUniformLocation(vs, "lmat");
        g.uAmb = shaderInstanceGetUniformLocation(vs, "lamb");
        g.uVsel = shaderInstanceGetUniformLocation(vs, "lvsel");
        g.uOn = shaderInstanceGetUniformLocation(vs, "lon");
        g.shaderLight = g.uLights >= 0 && g.uSel >= 0 && g.uMat >= 0 && g.uAmb >= 0 && g.uVsel >= 0 && g.uOn >= 0;
        emu_log(LOG_GX, "pica: shader lighting %s (uniforms %d %d %d %d %d %d)\n", g.shaderLight ? "on" : "unavailable",
                g.uLights, g.uSel, g.uMat, g.uAmb, g.uVsel, g.uOn);
        g.uProj = shaderInstanceGetUniformLocation(vs, "proj");
        g.uPal = shaderInstanceGetUniformLocation(vs, "pal");
        g.gpuPos = g.uProj >= 0 && g.uPal >= 0 && g.uOn >= 0;
        g.palSerial = 1;
        emu_log(LOG_GX, "pica: position transform %s (uniforms %d %d)\n", g.gpuPos ? "in the shader" : "on the CPU only",
                g.uProj, g.uPal);
        g.uNat = shaderInstanceGetUniformLocation(vs, "nat");
        g.uLcount = shaderInstanceGetUniformLocation(vs, "lcount");
        g_ns = calloc(NS_SLOTS, sizeof *g_ns);
        g_nsLive = calloc(NS_SLOTS, sizeof *g_nsLive);
        if (!g_ns || !g_nsLive) { free(g_ns); free(g_nsLive); g_ns = NULL; g_nsLive = NULL; }
        g_nsZero = linearAlloc(65536 * 4);
        g_nsWhite = linearAlloc(65536 * 4);
        if (g_nsZero) { memset(g_nsZero, 0, 65536 * 4); svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)g_nsZero, 65536 * 4); }
        if (g_nsWhite) { memset(g_nsWhite, 0xFF, 65536 * 4); svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)g_nsWhite, 65536 * 4); }
        emu_log(LOG_GX, "pica: converted shapes %s (uniforms %d %d)\n", g_ns && g.uNat >= 0 && g.uLcount >= 0 ? "on" : "unavailable",
                g.uNat, g.uLcount);
        g.arena = linearAlloc(ARENA_VERTS * sizeof(PVert));
        if (!g.arena) return false;
        g.idx = linearAlloc(IDX_MAX * sizeof(u16));
        if (!g.idx) g_indexed = false;
        else g_idxPa = osConvertVirtToPhys(g.idx);  // linear memory: physically contiguous
        if (!vw_start()) g_vwOn = false;  // no worker: the GX thread does all the vertex work
        if (!C3D_TexInitVRAM(&g.efbTex, TW, TH, GPU_RGBA8)) return false;
        g.efb = C3D_RenderTargetCreateFromTex(&g.efbTex, GPU_TEXFACE_2D, 0, GPU_RB_DEPTH24_STENCIL8);
        if (!g.efb) return false;
        g.efbDepthTex = g.efbTex;
        g.efbDepthTex.data = g.efb->frameBuf.depthBuf;
        for (u32 i = 0; i < MAX_XFB; i++)  // (with --vramtex in FCRAM: their VRAM goes to textures)
            if ((g_xfbFcram || !C3D_TexInitVRAM(&g.xfb[i].tex, TW, TH, GPU_RGBA8)) && !C3D_TexInit(&g.xfb[i].tex, TW, TH, GPU_RGBA8)) return false;
        g.screen = C3D_RenderTargetCreate(240, 400, GPU_RB_RGBA8, GPU_RB_DEPTH24_STENCIL8);  // a depth buffer even though unused: a NULL one reads as physical address 0
        if (!g.screen) return false;
        C3D_RenderTargetSetOutput(g.screen, GFX_TOP, GFX_LEFT, DISPLAY_TRANSFER_FLAGS);
        if (!C3D_TexInit(&g.white, 8, 8, GPU_RGBA8)) return false;
        memset(g.white.data, 0xFF, g.white.size);
        GSPGPU_FlushDataCache(g.white.data, g.white.size);
        emu_log(LOG_GX, "pica: PA arena %08x efb %08x depth %08x xfb %08x %08x %08x screen %08x white %08x\n",
                (unsigned)osConvertVirtToPhys(g.arena), (unsigned)osConvertVirtToPhys(g.efbTex.data),
                (unsigned)osConvertVirtToPhys(g.efb->frameBuf.depthBuf), (unsigned)osConvertVirtToPhys(g.xfb[0].tex.data),
                (unsigned)osConvertVirtToPhys(g.xfb[1].tex.data), (unsigned)osConvertVirtToPhys(g.xfb[2].tex.data),
                (unsigned)osConvertVirtToPhys(g.screen->frameBuf.colorBuf), (unsigned)osConvertVirtToPhys(g.white.data));
        g.ready = true;
    }
    s->gx.core.backend = &kPica;
    s->gx.core.backendCtx = NULL;
    return true;
}
