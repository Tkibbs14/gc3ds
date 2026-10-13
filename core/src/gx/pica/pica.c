// GX -> PICA200 backend (3DS, citro3d). Plan and measurements: docs/GPU_BACKEND_PLAN.md, docs/FINDINGS.md.
//
// Frame model. citro3d queues every piece of GPU work (command lists, memory fills, transfers) and runs the queue
// only at C3D_FrameEnd. A frame here begins lazily at the first GX work after a submit and ends at present, or when
// the command buffer or the vertex arena fills. Transfers that must happen between draws (EFB clears and copies) are
// preceded by C3D_FrameSplit, so the queue holds them in stream order. C3D_FrameBegin waits for the previous submit,
// so the vertex arena and textures retired during a frame are reused only once the GPU is idle.
//
// EFB. A 512x256 RGBA8 texture with a D24S8 depth buffer in VRAM: the GameCube's 640x528 EFB scaled by 0.625 x 0.5,
// so a 640x480 frame lands 1:1 on the 400x240 top screen. Rows past 512 (PAL 528-line EFBs) are cut. Vertices are
// transformed on the CPU by gx/vfast.c (checked against the reference gx/xform.c) straight into PICA clip space, so
// the hardware clips them (GX viewport and depth range included); culling is done on the CPU with the reference
// renderer's rule. Consecutive draws with the same GPU state are batched into one C3D_DrawArrays.
//
// Copies. EFB -> XFB copies the render target into one of three XFB textures tagged with the destination address;
// present shows the one the VI is scanning out ("virtual XFB": the XFB in RAM is not written). EFB -> texture renders
// the source rectangle into a GPU texture registered under the destination address, used in place of decoding RAM.
//
// Not yet: fog, Z textures, indirect texturing, depth copies (white), partial clears (whole EFB cleared), mipmaps,
// textures in VRAM (they live in linear FCRAM, 2.5-3.6x slower to sample).
#include <3ds.h>
#define GC3DS_HOST_TYPES
#include <citro3d.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "../../sys/system.h"
#include "../tev_pica.h"
#include "../vfast.h"
#include "gxpass_shbin.h"
#include "pica.h"
#include "../../../host3ds/crashlog.h"

#define TW 512
#define TH 256
#define SX (400.0f / 640.0f)
#define SY (240.0f / 480.0f)
// Whether NDC y = +1 lands on texture row 0 of a render-target texture. Fixed by the Azahar check: with the wrong
// value every frame is upside down.
#define FLIP_Y 0
#define CMDBUF_BYTES (1u << 20)
#define ARENA_VERTS ((6u << 20) / sizeof(PVert))
#define IDX_MAX (512u << 10)  // u16 indices in the index arena (1 MB)
#ifndef VBASE_SPAN
#define VBASE_SPAN 0xFFFFu    // vertices a u16 index reaches from the vertex buffer's start
#endif
#define TEX_BUDGET (12u << 20)
#define MAX_TEX 384
#define MAX_GRAVE 128
#define MAX_COPIES 32
#define MAX_XFB 3
#define GXQUEUE_BUDGET 26  // of citro3d's 32 GX queue entries; C3D_FrameEnd adds its own
#define PAL_SLOTS 14       // position matrices in the shader's palette (pal[42] in gen_gxpass.sh)

#define DISPLAY_TRANSFER_FLAGS (GX_TRANSFER_FLIP_VERT(0) | GX_TRANSFER_OUT_TILED(0) | GX_TRANSFER_RAW_COPY(0) | \
    GX_TRANSFER_IN_FORMAT(GX_TRANSFER_FMT_RGBA8) | GX_TRANSFER_OUT_FORMAT(GX_TRANSFER_FMT_RGB8) | \
    GX_TRANSFER_SCALING(GX_TRANSFER_SCALE_NO))


typedef struct {         // a texture decoded from RAM
    u32 addr, fmt, w, h, tlutAddr, tlutFmt, dataHash, tlutHash, texVersion, tlutVersion, lastUse, bytes;
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
} g;

PicaStats g_picaStats;
static u8 g_morton[64];
static SVert* g_sv;
static PVert* g_pv;
static u32 g_vcap;
static u32* g_scratch;
static u32 g_scratchCap;

static volatile u32 g_vwHead, g_vwTail;  // the vertex worker's queue (below): jobs queued / done
// g_vwHead when the open batch (or its open sub-batch) began: the older jobs only feed draw calls already recorded,
// which is all an early split needs finished (split_early).
static u32 g_recJob;

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

static void draw_arrays(u32 first, u32 n) {
    set_vbase(0);
    C3D_DrawArrays(GPU_TRIANGLES, (int)first, (int)n);
    g_picaStats.drawCalls++;
}

static void draw_elements(u32 first, u32 n) {
    C3D_DrawElements(GPU_TRIANGLES, (int)n, C3D_UNSIGNED_SHORT, g.idx + first);
    g_picaStats.drawCalls++;
}

static void flush_batch(void) {
    vw_sync();  // where the batch ends may still be on the vertex worker (CPU-transformed draws it culls)
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

static void begin_frame(void) {
    if (g.frame) return;
    u64 t0 = prof_begin();
    C3D_FrameBegin(0);  // waits for the previous submit: the arena and retired textures are free again
    prof_end(PROF_GXSYNC, t0);
    g.frame = true;
    g.frameNo++;
    g.arenaUsed = g.arenaFlushed = g.queued = g.splitArena = g.idxUsed = g.idxFlushed = g.vbase = 0;
    g.batchOpen = false;
    for (u32 i = 0; i < g.nGrave; i++) C3D_TexDelete(&g.grave[i]);
    g.nGrave = 0;
    if (g.resetPending) { free_all_textures(); g.resetPending = false; }
    C3D_BindProgram(&g.prog);
    C3D_AttrInfo* ai = C3D_GetAttrInfo();
    AttrInfo_Init(ai);
    AttrInfo_AddLoader(ai, 0, GPU_FLOAT, 4);
    AttrInfo_AddLoader(ai, 1, GPU_UNSIGNED_BYTE, 4);
    AttrInfo_AddLoader(ai, 2, GPU_FLOAT, 2);
    AttrInfo_AddLoader(ai, 3, GPU_FLOAT, 2);
    AttrInfo_AddLoader(ai, 4, GPU_FLOAT, 2);
    AttrInfo_AddLoader(ai, 5, GPU_FLOAT, 3);  // eye-space position (shader lighting)
    AttrInfo_AddLoader(ai, 6, GPU_FLOAT, 3);  // eye-space normal
    C3D_BufInfo* bi = C3D_GetBufInfo();
    BufInfo_Init(bi);
    BufInfo_Add(bi, g.arena, sizeof(PVert), 7, 0x6543210);
    C3D_DepthMap(true, -1.0f, 0.0f);  // depth = -z/w; vertices carry z = -(GX screen z / 2^24) * w
    C3D_CullFace(GPU_CULL_NONE);
    C3D_FrameDrawOn(g.efb);
    g.stateValid = g.tevValid = false;
}

static void end_frame(void) {
    if (!g.frame) return;
    flush_batch();
    u64 t0 = prof_begin();
    flush_arena();
    C3D_FrameEnd(GX_CMDLIST_FLUSH);
    prof_end(PROF_GXEND, t0);
    g_picaStats.submits++;
    g.frame = false;
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

static void queue_run(void) { if (g_q && g_earlyRun) gxCmdQueueRun(g_q); }
void pica_set_early_queue(bool on) { g_earlyRun = on; }
bool pica_early_queue(void) { return g_earlyRun && g_q; }
// Draws by batch path: [0] nothing in the key changed (fast), [1] key rebuilt and equal, [2] new batch; [3..6] new
// batches with a changed BP / palette / light version or XFB copy since the open batch (written by the GX thread).
u64 g_picaDrawPaths[8];

u32 pica_queue_offset(void) { return g_q ? (u32)((u8*)g_q - __C3D_Context) : 0xFFFFFFFFu; }

// Submits what is queued so far so that the `transfers` GX transfers queued next run after it. The GX command queue
// holds 32 entries (citro3d) and runs only at C3D_FrameEnd; libctru panics when it is full, so a frame that has
// queued many splits and transfers (several emulated fields between presents) is submitted early.
static void split(u32 transfers) {
    begin_frame();
    flush_batch();
    if (g.queued + 1 + transfers > GXQUEUE_BUDGET) { end_frame(); begin_frame(); }
    flush_arena();
    verify_queue(true);
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
    verify_queue(false);
    g_picaStats.splits++;
    g.queued += 1 + transfers;
    g.splitArena = g.arenaUsed;
    if (!transfers) queue_run();  // with transfers, the caller runs the queue once it has added them
}

// Splits the frame every SPLIT_VERTS arena vertices so the GPU works on the frame while this thread records the
// rest: C3D_FrameBegin waits for everything after the last split, and without these the main scene came after the
// last EFB copy (2DS, 29a22f7: this thread waited for the GPU 7-14% of the time). Called between batches; keeps queue
// entries for the frame's copies and clears.
#define SPLIT_VERTS 8192
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
    if (verts > ARENA_VERTS) return false;
    begin_frame();
    vw_sync();  // this thread writes the arena itself, after the worker's variable-length draws
    if (g.arenaUsed + verts > ARENA_VERTS || C3D_GetCmdBufUsage() > 0.8f) { end_frame(); begin_frame(); }
    return true;
}

// Room for a draw the vertex worker will append, without waiting for it.
static bool need_work(u32 verts, u32 indices) {
    if (verts > ARENA_VERTS || indices > IDX_MAX) return false;
    begin_frame();
    if (g.arenaUsed + g_vwReserved + verts > ARENA_VERTS || g.idxUsed + indices > IDX_MAX || C3D_GetCmdBufUsage() > 0.8f) {
        end_frame();
        begin_frame();
    }
    return true;
}

static void retire(C3D_Tex* t, bool usedThisFrame) {
    if (!usedThisFrame) { C3D_TexDelete(t); return; }
    if (g.nGrave == MAX_GRAVE) { end_frame(); begin_frame(); }
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
static void upload(C3D_Tex* t, const u32* rgba, u32 w, u32 h) {
    u32 W = t->width, H = t->height;
    u32* dst = (u32*)t->data;
    for (u32 y = 0; y < H; y++) {
        const u32* row = rgba + ((H - 1 - y) * h / H) * w;
        u32* tiles = dst + (y >> 3) * (W >> 3) * 64;
        const u8* m = &g_morton[(y & 7) * 8];
        for (u32 x = 0; x < W; x++) tiles[(x >> 3) * 64 + m[x & 7]] = row[x * w / W];
    }
    svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)t->data, t->size);
}

static void evict_unused(u32 want) {
    while (g.texBytes + want > TEX_BUDGET) {
        PTex* old = NULL;
        for (u32 i = 0; i < MAX_TEX; i++) {
            PTex* p = &g.tex[i];
            if (p->live && p->lastUse != g.frameNo && (!old || p->lastUse < old->lastUse)) old = p;
        }
        if (!old) return;
        C3D_TexDelete(&old->tex);
        old->live = false;
        g.texBytes -= old->bytes;
    }
}

static bool decode(System* s, const GXCore* gx, PTex* e) {
    u32 W = pow2(e->w), H = pow2(e->h), bytes = W * H * 4;
    g_picaStats.texDecodes++;
    g_picaStats.texDecodeBytes += bytes;
    evict_unused(bytes);
    C3D_Tex nt;
    if (!C3D_TexInit(&nt, (u16)W, (u16)H, GPU_RGBA8)) return false;
    u32 n = e->w * e->h;
    if (n > g_scratchCap) {
        u32* p = realloc(g_scratch, n * 4);
        if (!p) { C3D_TexDelete(&nt); return false; }
        g_scratch = p;
        g_scratchCap = n;
    }
    gx_decode_texture(s->mem1 + e->addr, e->fmt, e->w, e->h, gx->tmem + (e->tlutAddr & 0xFFFFF), e->tlutFmt, g_scratch);
    upload(&nt, g_scratch, e->w, e->h);
    if (e->live) { retire(&e->tex, e->lastUse == g.frameNo); g.texBytes -= e->bytes; }
    e->tex = nt;
    e->bytes = bytes;
    e->live = true;
    g.texBytes += bytes;
    return true;
}

static C3D_Tex* cached_texture(System* s, const GXCore* gx, u32 map, u32 addr, u32 fmt, u32 w, u32 h, u32 tlutReg) {
    u32 bytes = gx_texture_size(fmt, w, h);
    if (!bytes || addr + bytes > PA_MEM1_END) return NULL;
    bool pal = fmt == 8 || fmt == 9 || fmt == 10;
    u32 tlutAddr = pal ? (tlutReg & 0x3FF) << 9 : 0, tlutFmt = pal ? (tlutReg >> 10) & 3 : 0;
    PTex* e = NULL;
    PTex* slot = NULL;
    PTex* last = g.lastHit[map];  // texture maps mostly keep their texture from one draw to the next
    if (last && last->live && last->addr == addr && last->fmt == fmt && last->w == w && last->h == h &&
        last->tlutAddr == tlutAddr && last->tlutFmt == tlutFmt)
        e = last;
    for (u32 i = 0; i < MAX_TEX && !e; i++) {
        PTex* p = &g.tex[i];
        if (!p->live) { if (!slot) slot = p; continue; }
        if (p->addr == addr && p->fmt == fmt && p->w == w && p->h == h && p->tlutAddr == tlutAddr && p->tlutFmt == tlutFmt) {
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
            C3D_TexDelete(&slot->tex);
            slot->live = false;
            g.texBytes -= slot->bytes;
        }
        e = slot;
        e->addr = addr; e->fmt = fmt; e->w = w; e->h = h; e->tlutAddr = tlutAddr; e->tlutFmt = tlutFmt;
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
static C3D_Tex* fetch_texture(System* s, const GXCore* gx, u32 map) {
    u32 hi = map >= 4 ? 0x20 : 0, m = map & 3;
    u32 img0 = gx->bp[0x88 + m + hi], img3 = gx->bp[0x94 + m + hi], tlut = gx->bp[0x98 + m + hi], mode = gx->bp[0x80 + m + hi];
    u32 w = (img0 & 0x3FF) + 1, h = ((img0 >> 10) & 0x3FF) + 1, fmt = (img0 >> 20) & 15, addr = (img3 & 0xFFFFFF) << 5;
    PCopy* c = find_copy(s, gx, addr, w, h);
    C3D_Tex* t = c ? &c->tex : cached_texture(s, gx, map, addr, fmt, w, h, tlut);
    if (!t) t = &g.white;
    static const GPU_TEXTURE_WRAP_PARAM wraps[4] = { GPU_CLAMP_TO_EDGE, GPU_REPEAT, GPU_MIRRORED_REPEAT, GPU_REPEAT };
    C3D_TexSetWrap(t, wraps[mode & 3], wraps[(mode >> 2) & 3]);
    C3D_TexSetFilter(t, (mode >> 4) & 1 ? GPU_LINEAR : GPU_NEAREST, ((mode >> 5) & 7) >= 4 ? GPU_LINEAR : GPU_NEAREST);
    return t;
}

// ---- state ----
static const GPU_TESTFUNC kFunc[8] = { GPU_NEVER, GPU_LESS, GPU_EQUAL, GPU_LEQUAL, GPU_GREATER, GPU_NOTEQUAL, GPU_GEQUAL, GPU_ALWAYS };

static void apply_tev(const PicaTev* t) {
    for (int i = 0; i < PICA_STAGES; i++) {
        C3D_TexEnv* e = C3D_GetTexEnv(i);
        C3D_TexEnvInit(e);
        if (i >= t->numStages) continue;
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
    C3D_TexEnvBufUpdate(C3D_RGB, t->bufRgb);
    C3D_TexEnvBufUpdate(C3D_Alpha, t->bufA);
    C3D_TexEnvBufColor(t->bufColor);
}

// The registers the TEV program depends on (65 words).
static u32 tev_key(const GXCore* gx, u32* k) {
    u32 n = 0;
    k[n++] = gx->bp[0x00];
    for (u32 r = 0x28; r <= 0x2F; r++) k[n++] = gx->bp[r];
    for (u32 r = 0xC0; r <= 0xDF; r++) k[n++] = gx->bp[r];
    for (u32 r = 0xF6; r <= 0xFD; r++) k[n++] = gx->bp[r];
    for (u32 i = 0; i < 4; i++) { k[n++] = gx->tevLo[i]; k[n++] = gx->tevHi[i]; k[n++] = gx->kLo[i]; k[n++] = gx->kHi[i]; }
    return n;
}

// Recompiles the TEV program only when a register it depends on changed.
static void update_tev(const GXCore* gx) {
    u32 k[72], n = tev_key(gx, k);
    if (g.tevValid && !memcmp(k, g.tevKey, n * 4)) return;
    memcpy(g.tevKey, k, n * 4);
    tev_pica_compile(gx, &g.tev);
    if (g.tev.approx) note_approx(g.tev.approx);
    apply_tev(&g.tev);
    g.sel = (VSel){ g.tev.rasChan, g.tev.numTex, { g.tev.texCoord[0], g.tev.texCoord[1], g.tev.texCoord[2] } };
    g.tevValid = true;
}

// Depth, alpha test, blending / logic op, write masks and scissor (BP 0x40, 0x41, 0x43, 0xF3, 0x20/0x21).
static void apply_frag(const GXCore* gx) {
    u32 k[6] = { gx->bp[0x40], gx->bp[0x41], gx->bp[0x43] & 7, gx->bp[0xF3], gx->bp[0x20], gx->bp[0x21] };
    if (g.stateValid && !memcmp(k, g.fragKey, sizeof k)) return;
    memcpy(g.fragKey, k, sizeof k);
    g.stateValid = true;
    u32 zmode = k[0], cmode = k[1];
    bool hasAlpha = k[2] == 1;  // RGBA6_Z24; the other formats have no destination alpha (kept at 255)
    int mask = 0;
    if (cmode & 8) mask |= GPU_WRITE_RED | GPU_WRITE_GREEN | GPU_WRITE_BLUE;
    if ((cmode & 16) && hasAlpha) mask |= GPU_WRITE_ALPHA;
    bool zen = zmode & 1;
    if (zen && (zmode & 16)) mask |= GPU_WRITE_DEPTH;
    C3D_DepthTest(zen, zen ? kFunc[(zmode >> 1) & 7] : GPU_ALWAYS, (GPU_WRITEMASK)mask);
    // Two GX alpha comparisons and a logic op reduced to the PICA's single comparison.
    u32 r = k[3], c0 = (r >> 16) & 7, c1 = (r >> 19) & 7, op = (r >> 22) & 3, r0 = r & 0xFF, r1 = (r >> 8) & 0xFF;
    if (op == 0 && c0 == 7 && c1 == 7) C3D_AlphaTest(false, GPU_ALWAYS, 0);
    else if ((op == 0 && c1 == 7) || (op == 1 && c1 == 0)) C3D_AlphaTest(true, kFunc[c0], (int)r0);
    else if ((op == 0 && c0 == 7) || (op == 1 && c0 == 0)) C3D_AlphaTest(true, kFunc[c1], (int)r1);
    else { note_approx(1u << 31); C3D_AlphaTest(true, kFunc[c0], (int)r0); }
    if (cmode & 1) {
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
    u32 w = (img0 & 0x3FF) + 1, h = ((img0 >> 10) & 0x3FF) + 1, fmt = (img0 >> 20) & 15, addr = (img3 & 0xFFFFFF) << 5;
    u32 key[5] = { img0, img3, tlut, mode, g.xfbSeq };
    if (memcmp(key, g_ztexKey, sizeof key)) {
        memcpy(g_ztexKey, key, sizeof key);
        u32 n = w * h, bytes = gx_texture_size(fmt, w, h);
        if (n > g_scratchCap) {
            u32* p = realloc(g_scratch, n * 4);
            if (!p) return false;
            g_scratch = p;
            g_scratchCap = n;
        }
        if (!bytes || addr + bytes > PA_MEM1_END) return false;
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
    if (g.batchGpuLight) C3D_FVUnifSet(GPU_VERTEX_SHADER, g.uOn, u->on[0], u->on[1], 1.0f, gpuPos ? 1.0f : 0.0f);
    else C3D_FVUnifSet(GPU_VERTEX_SHADER, g.uOn, 0, 0, 0, gpuPos ? 1.0f : 0.0f);
}

static void apply_light_uniforms(void) {
    C3D_CullFace(g.batchGpuPos ? g.batchCull : GPU_CULL_NONE);
    if (!g.batchGpuLight) { set_lon(g.batchGpuPos); return; }
    const VLightUniforms* u = &g.lu;
    for (int i = 0; i < 40; i++) C3D_FVUnifSet(GPU_VERTEX_SHADER, g.uLights + i, u->lights[i][0], u->lights[i][1], u->lights[i][2], u->lights[i][3]);
    for (int i = 0; i < 4; i++) C3D_FVUnifSet(GPU_VERTEX_SHADER, g.uSel + i, u->sel[i][0], u->sel[i][1], u->sel[i][2], u->sel[i][3]);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.uMat, u->mat[0], u->mat[1], u->mat[2], u->mat[3]);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.uAmb, u->amb[0], u->amb[1], u->amb[2], u->amb[3]);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.uVsel, u->vsel[0], u->vsel[1], u->vsel[2], u->vsel[3]);
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
#define VW_MEM (1024u << 10)
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
} VWork;
static VWork g_vw[VW_SLOTS];
static u8* g_vwMem;
static u32 g_vwMemHead;
static volatile u32 g_vwQuit, g_vwPos;
// The worker also gathers shader-lit draws from guest memory (vfast_prepare defer) when they are GPU-transformed:
// their batches never wait for the worker. For CPU-transformed draws it measured slower on the 2DS (6673eb2: 14.2%
// against 14.8%), because every batch flush then waits for the worker (vwait 18%).
static const bool kVwGather = false;  // CPU-transformed draws
static volatile u32 g_vwDeferEnd;     // the queue index after the last queued draw that reads guest memory
static Thread g_vwThread;
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

static void vw_main(void* arg) {
    (void)arg;
    crashlog_thread(CRASH_VWORKER);
    __asm__ volatile("vmsr fpscr, %0" : : "r"(0x03000000u));  // as the GX thread: round to nearest, flush to zero
    u32 idle = 0, jobs = 0;
    while (!g_vwQuit) {
        if (g_vwTail != g_vwHead) {
            vw_dmb();
            bool timed = !(++jobs & 15);  // timed on one job in 16 (a clock read costs 0.25 us)
            u64 t0 = timed ? prof_begin() : 0;
            g_vfPhaseTimed = timed;
            vw_do(&g_vw[g_vwTail % VW_SLOTS]);
            g_vfPhaseTimed = false;
            if (timed && g_profClock) { u64 d = g_profClock() - t0; g_hostProf[PROF_VWORK] += d * 16; g_vwPhaseAll += d; }
            vw_dmb();
            g_vwTail = g_vwTail + 1;
            idle = 0;
        } else if (++idle > 4000) {
            svcSleepThread(20000);
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
        while (g_vwTail != g_vwHead) {}
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
static void split_early(void) {
    if (!g_earlySplit || !g_q || !g.frame || g.arenaUsed - g.splitArena < SPLIT_VERTS || g.queued + 8 > GXQUEUE_BUDGET)
        return;
    if (!g_splitOpen || !g.batchOpen || g_vwReserved) {  // the whole batch: only when the worker is nearly idle
        if (vw_nearly_idle()) split(0);
        return;
    }
    s32 behind = (s32)(g_recJob - g_vwTail);
    if (behind > 1) return;  // more than the job in progress outstanding: the next job tries again
    if (behind > 0) {
        u64 t0 = prof_begin();
        while ((s32)(g_vwTail - g_recJob) < 0) {}
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
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
    verify_queue(false);
    g_picaStats.splits++;
    g.queued += 1;
    g.splitArena = g.arenaUsed;
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
    for (;;) {
        u32 t = g_vwTail;
        vw_dmb();
        if (t == g_vwHead) break;  // nothing in flight
        bool full = g_vwHead - t >= VW_SLOTS;
        // In flight: [os, head) going round the ring.
        u32 os = g_vw[t % VW_SLOTS].memStart, he = g_vwMemHead, end = start + bytes;
        bool overlap = os <= he ? (start < he && end > os) || (os == he) : (start < he || end > os);
        if (!full && !overlap) break;
        if (!t0) { t0 = prof_begin(); g_vwStats[3]++; g_vwStats[4] += g_vwHead - t; }
    }
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
    g_vwMem = malloc(VW_MEM);
    if (!g_vwMem) g_vwMem = linearAlloc(VW_MEM);
    if (!g_vwMem) return false;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    g_vwThread = threadCreate(vw_main, NULL, 32 * 1024, prio, 3, false);
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
    C3D_CullFace(GPU_CULL_NONE);
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
static void draw_job(System* s, u32 prim, u32 vat, const u8* raw, u32 count, u32 vsize, const u32* seg, u32 nSeg) {
    vw_apply();
    if (g_descWant && !g_descReady) describe_draw(&s->gx.core, prim, count);
    static u32 drawN;
    g_drawScale = 16;
    g_drawTimed = !(++drawN & 15);  // per-draw timers on one draw in 16
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
    bool gpuPos = g.gpuPos && !lines && !ztex, indexed = (g_indexed || nSeg) && !lines;
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
        flush_batch();
        split_early();
        update_tev(gx);
        kn = batch_key(gx, key);
        key[kn++] = cls;
        tex_key(gx, key, &kn);
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
        if (nSeg) for (u32 k = 0; k < nSeg; k++) workVerts += tri_verts(seg[k] >> 16, seg[k] & 0xFFFF);
        else workVerts = tri_verts(prim, count);
        if (!workVerts) { dprof_end(PROF_GXVTX, t0); return; }
        nIdx = indexed ? workVerts : 0;
        u64 tq = dprof_begin();
        if (!need_work(indexed ? count : workVerts, nIdx)) {
            emu_log(LOG_GX, "pica: draw of %u vertices too large, dropped\n", (unsigned)count);
            return;
        }
        bool defer = gpuPos || indexed || kVwGather, deferLit = defer && g.batchGpuLight;
        // Job memory only for what the draw uses: a deferred indexed job finishes straight into the arena (no vertex
        // copy), and GPU-transformed draws are culled by the PICA (no NDC).
        bool needPv = !(deferLit && indexed), needNd = !gpuPos && (cull == 1 || cull == 2);
        // The draw's own arrays first, then the job (its unused end is given back after vfast_prepare: vw_trim).
        u32 jb = (vfast_job_bytes(gx, count, vsize) + 7) & ~7u;
        u32 front = ((needPv ? count * (u32)sizeof(PVert) : 0) + (needNd ? count * 8 : 0) + nSeg * 4 + 7) & ~7u;
        u32 bytes = front + jb;
        u8* mem = work ? vw_alloc(bytes) : NULL;
        if (!mem) {  // the worker is off, or the draw is larger than its job ring: done here
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
        if (nSeg) {
            u32* sc = (u32*)at;
            memcpy(sc, seg, nSeg * 4);
            w.seg = sc;
            w.nSeg = nSeg;
        }
        w.deferred = deferLit;  // vfast_prepare defers shader-lit draws only
        u64 tp = dprof_begin();
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
        apply_frag(gx);
        // Only the units the combiners read are bound; the others keep whatever they had, which nothing samples.
        // - Never bind NULL: citro3d 1.7.1 reads the texture's type through it, a data abort on hardware. Azahar does
        //   not fault on reads near address 0, so it hid this.
        // - Binding a placeholder to unused units 1 and 2 crashed Azahar itself (2026-10-10).
        for (u32 i = 0; i < g.tev.numTex && i < PICA_TEXUNITS; i++) C3D_TexBind((int)i, tex[i] ? tex[i] : &g.white);
        apply_light_uniforms();
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
            bool palOk = pal_assign(w.job);
            dprof_end(PROF_VPAL, tpal);
            if (!palOk) {
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
            if (work) vw_queue(&w, 0);
            else vw_do(&w);
        } else if (work) {  // the worker appends the triangles it keeps; the batch's draw call waits for it (flush_batch)
            vw_queue(&w, workVerts);
        } else {  // too large for the worker's ring: culled and appended here, after the worker's queued draws
            vw_sync();
            g_vwPos = g.arenaUsed;
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
    u8 raw[MERGE_BYTES];
} g_mg;
static volatile bool g_mergeWant = true;  // pica_set_merge (the GX thread reads it at each draw)
u64 g_picaMerge[7];

static void merge_issue(System* s) {
    u32 n = g_mg.nSeg;
    g_mg.nSeg = 0;
    if (n) {
        g_picaMerge[2]++;
        draw_job(s, g_mg.seg[0] >> 16, g_mg.vat, g_mg.raw, g_mg.count, g_mg.vsize, n > 1 ? g_mg.seg : NULL, n > 1 ? n : 0);
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

static void pica_draw(void* ctx, System* s, u32 prim, u32 vat, const u8* raw, u32 count, u32 vsize) {
    (void)ctx;
    g_picaMerge[0]++;
    u32 bytes = count * vsize;
    bool can = g_mergeWant && g_indexed && s->gxAsync && prim < PRIM_LINES && count <= MERGE_VERTS && bytes <= MERGE_BYTES;
    if (g_gxMergeOpen) {
        if (can && vat == g_mg.vat && vsize == g_mg.vsize && g_mg.count + count <= MERGE_VERTS &&
            g_mg.bytes + bytes <= MERGE_BYTES && g_mg.nSeg < MERGE_SEGS) {
            memcpy(g_mg.raw + g_mg.bytes, raw, bytes);
            g_mg.seg[g_mg.nSeg++] = (prim << 16) | count;
            g_mg.count += count;
            g_mg.bytes += bytes;
            g_picaMerge[1]++;
            return;
        }
        g_picaMerge[4]++;
        merge_issue(s);
    }
    if (!can) { draw_job(s, prim, vat, raw, count, vsize, NULL, 0); return; }
    memcpy(g_mg.raw, raw, bytes);
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
    C3D_TexSetFilter(src, linear ? GPU_LINEAR : GPU_NEAREST, linear ? GPU_LINEAR : GPU_NEAREST);
    C3D_TexSetWrap(src, GPU_CLAMP_TO_EDGE, GPU_CLAMP_TO_EDGE);
    C3D_TexBind(0, src);
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

static void copy_to_xfb(u32 x0, u32 y0, u32 w, u32 h, u32 dest) {
    g_picaStats.xfbCopies++;
    PXfb* x = NULL;
    for (u32 i = 0; i < MAX_XFB; i++) if (g.xfb[i].seq && g.xfb[i].addr == dest) x = &g.xfb[i];
    if (!x) for (u32 i = 0; i < MAX_XFB; i++) if (!x || g.xfb[i].seq < x->seq) x = &g.xfb[i];
    split(1);  // the draws so far reach the EFB before the copy reads it
    GX_TextureCopy((u32*)g.efbTex.data, 0, (u32*)x->tex.data, 0, TW * TH * 4, GX_TRANSFER_RAW_COPY(1));
    queue_run();
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
    if (dest + c->ramBytes > PA_MEM1_END) c->ramBytes = 0;
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
    (void)ctx;
    GXCore* gx = &s->gx.core;
    begin_frame();
    if ((ctl >> 14) & 1) copy_to_xfb(x0, y0, w, h, dest);
    else copy_to_texture(s, gx, ctl, x0, y0, w, h, dest);
    if ((ctl >> 11) & 1) clear_efb(gx, x0, y0, w, h);
    return true;
}

static const GXBackend kPica = { pica_draw, pica_efb_copy, pica_flush };

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
#define CAP_FILE_BYTES (54u + 400u * 240u * 3u)
static Thread g_capThread;

static void cap_writer(void* arg) {
    (void)arg;
    for (;;) {
        if (g_capState == 3) {
            __asm__ volatile("mcr p15, 0, %0, c7, c10, 5" : : "r"(0) : "memory");
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

bool pica_present(System* s, u32 xfbAddr) {
    pica_merge_flush(s, false);
    if (g_capState == 2) cap_write();  // the copy was queued with the previous present
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
    }
    end_frame();  // queues the transfer to the top framebuffer and the swap
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
        g.dvlb = DVLB_ParseFile((u32*)gxpass_shbin, gxpass_shbin_size);
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
        g.arena = linearAlloc(ARENA_VERTS * sizeof(PVert));
        if (!g.arena) return false;
        g.idx = linearAlloc(IDX_MAX * sizeof(u16));
        if (!g.idx) g_indexed = false;
        if (!vw_start()) g_vwOn = false;  // no worker: the GX thread does all the vertex work
        if (!C3D_TexInitVRAM(&g.efbTex, TW, TH, GPU_RGBA8)) return false;
        g.efb = C3D_RenderTargetCreateFromTex(&g.efbTex, GPU_TEXFACE_2D, 0, GPU_RB_DEPTH24_STENCIL8);
        if (!g.efb) return false;
        g.efbDepthTex = g.efbTex;
        g.efbDepthTex.data = g.efb->frameBuf.depthBuf;
        for (u32 i = 0; i < MAX_XFB; i++)
            if (!C3D_TexInitVRAM(&g.xfb[i].tex, TW, TH, GPU_RGBA8) && !C3D_TexInit(&g.xfb[i].tex, TW, TH, GPU_RGBA8)) return false;
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
