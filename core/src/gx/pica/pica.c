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

#define TW 512
#define TH 256
#define SX (400.0f / 640.0f)
#define SY (240.0f / 480.0f)
// Whether NDC y = +1 lands on texture row 0 of a render-target texture. Fixed by the Azahar check: with the wrong
// value every frame is upside down.
#define FLIP_Y 0
#define CMDBUF_BYTES (1u << 20)
#define ARENA_VERTS ((6u << 20) / sizeof(PVert))
#define TEX_BUDGET (12u << 20)
#define MAX_TEX 384
#define MAX_GRAVE 128
#define MAX_COPIES 32
#define MAX_XFB 3
#define GXQUEUE_BUDGET 26  // of citro3d's 32 GX queue entries; C3D_FrameEnd adds its own

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
    u32 arenaUsed, arenaFlushed;
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
    VSel sel;
    // Lighting in the vertex shader (gxpass.v.pica): on unless pica_set_shader_light(false); the open batch's
    // decision and uniforms; uniform locations.
    bool shaderLight, batchGpuLight;
    VLightUniforms lu;
    s8 uLights, uSel, uMat, uAmb, uVsel, uOn;
} g;

PicaStats g_picaStats;
static u8 g_morton[64];
static SVert* g_sv;
static PVert* g_pv;
static u32 g_vcap;
static u32* g_scratch;
static u32 g_scratchCap;

// ---- frames ----
// Issues the open batch's triangles; call before anything changes GPU state or submits.
static void vw_sync(void);
static void flush_batch(void) {
    vw_sync();  // the batch's last triangles may still be on the vertex worker
    if (g.batchOpen && g.arenaUsed > g.batchFirst) {
        C3D_DrawArrays(GPU_TRIANGLES, (int)g.batchFirst, (int)(g.arenaUsed - g.batchFirst));
        g_picaStats.drawCalls++;
    }
    g.batchOpen = false;
}

static void vw_sync(void);
static void flush_arena(void) {
    vw_sync();  // the vertex worker must have filled the arena before the GPU reads it
    if (g.arenaUsed > g.arenaFlushed)
        GSPGPU_FlushDataCache(g.arena + g.arenaFlushed, (g.arenaUsed - g.arenaFlushed) * sizeof(PVert));
    g.arenaFlushed = g.arenaUsed;
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
    g.arenaUsed = g.arenaFlushed = g.queued = 0;
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
    flush_arena();
    C3D_FrameEnd(GX_CMDLIST_FLUSH);
    g_picaStats.submits++;
    g.frame = false;
}

// Submits what is queued so far so that the `transfers` GX transfers queued next run after it. The GX command queue
// holds 32 entries (citro3d) and runs only at C3D_FrameEnd; libctru panics when it is full, so a frame that has
// queued many splits and transfers (several emulated fields between presents) is submitted early.
static void split(u32 transfers) {
    begin_frame();
    flush_batch();
    if (g.queued + 1 + transfers > GXQUEUE_BUDGET) { end_frame(); begin_frame(); }
    flush_arena();
    C3D_FrameSplit(GX_CMDLIST_FLUSH);
    g_picaStats.splits++;
    g.queued += 1 + transfers;
}

// Makes room for `verts` more vertices and keeps the command buffer from overflowing (libctru drops commands
// silently when it is full).
static u32 g_vwReserved;  // arena vertices the vertex worker's queued draws may write (vertex worker below)

static bool need(u32 verts) {
    if (verts > ARENA_VERTS) return false;
    begin_frame();
    vw_sync();  // this thread writes the arena itself
    if (g.arenaUsed + verts > ARENA_VERTS || C3D_GetCmdBufUsage() > 0.8f) { end_frame(); begin_frame(); }
    return true;
}

// Room for a draw the vertex worker will append, without waiting for it.
static bool need_work(u32 verts) {
    if (verts > ARENA_VERTS) return false;
    begin_frame();
    if (g.arenaUsed + g_vwReserved + verts > ARENA_VERTS || C3D_GetCmdBufUsage() > 0.8f) { end_frame(); begin_frame(); }
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
    GSPGPU_FlushDataCache(t->data, t->size);
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

static void apply_ztex(System* s, const GXCore* gx, u32 count) {
    u32 op;
    float z;
    if (ztex_value(s, gx, &op, &z)) ztex_apply(g_pv, count, op, z);
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
static void apply_light_uniforms(void) {
    if (!g.batchGpuLight) { C3D_FVUnifSet(GPU_VERTEX_SHADER, g.uOn, 0, 0, 0, 0); return; }
    const VLightUniforms* u = &g.lu;
    for (int i = 0; i < 40; i++) C3D_FVUnifSet(GPU_VERTEX_SHADER, g.uLights + i, u->lights[i][0], u->lights[i][1], u->lights[i][2], u->lights[i][3]);
    for (int i = 0; i < 4; i++) C3D_FVUnifSet(GPU_VERTEX_SHADER, g.uSel + i, u->sel[i][0], u->sel[i][1], u->sel[i][2], u->sel[i][3]);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.uMat, u->mat[0], u->mat[1], u->mat[2], u->mat[3]);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.uAmb, u->amb[0], u->amb[1], u->amb[2], u->amb[3]);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.uVsel, u->vsel[0], u->vsel[1], u->vsel[2], u->vsel[3]);
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.uOn, u->on[0], u->on[1], 1.0f, 0);
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
// L2 cache (a 4 MB ring made the GX thread's vertex time grow).
#define VW_SLOTS 1024
#define VW_MEM (256u << 10)
typedef struct {
    VJob* job;
    PVert* pv;         // the draw's vertices (colours from vfast_prepare)
    float (*nd)[2];    // NDC x, y per vertex for culling
    u32 memStart, prim, count, cull, ztexOp;
    float ztexZ;
} VWork;
static VWork g_vw[VW_SLOTS];
static u8* g_vwMem;
static u32 g_vwMemHead;
static volatile u32 g_vwHead, g_vwTail, g_vwQuit, g_vwPos;
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

static void vw_do(const VWork* w) {
    u32 count = w->count;
    vfast_finish(w->job, w->pv);
    if (w->ztexOp) ztex_apply(w->pv, count, w->ztexOp, w->ztexZ);
    if (w->cull == 1 || w->cull == 2)
        for (u32 i = 0; i < count; i++) {
            float wv = w->pv[i].pos[3], iw = wv > 0 ? 1.0f / wv : 0;
            w->nd[i][0] = w->pv[i].pos[0] * iw; w->nd[i][1] = w->pv[i].pos[1] * iw;
        }
    PVert* o = g.arena + g_vwPos;
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
    g_vwPos = (u32)(o - g.arena);
}

static void vw_main(void* arg) {
    (void)arg;
    __asm__ volatile("vmsr fpscr, %0" : : "r"(0x03000000u));  // as the GX thread: round to nearest, flush to zero
    u32 idle = 0;
    while (!g_vwQuit) {
        if (g_vwTail != g_vwHead) {
            vw_dmb();
            u64 t0 = prof_begin();
            vw_do(&g_vw[g_vwTail % VW_SLOTS]);
            prof_end(PROF_VWORK, t0);
            vw_dmb();
            g_vwTail = g_vwTail + 1;
            idle = 0;
        } else if (++idle > 4000) {
            svcSleepThread(20000);
        }
    }
}

// Waits until the worker has done every queued draw, and takes over the arena again.
static void vw_sync(void) {
    if (!g_vwReserved) return;
    if (g_vwTail != g_vwHead) {
        u64 t0 = prof_begin();
        while (g_vwTail != g_vwHead) {}
        prof_end(PROF_VWAIT, t0);
    }
    vw_dmb();
    g.arenaUsed = g_vwPos;
    g_vwReserved = 0;
}

// `bytes` of job memory, after waiting for the worker to free them if the ring is short.
static u8* vw_alloc(u32 bytes) {
    if (bytes > VW_MEM) return NULL;
    u32 start = g_vwMemHead;
    if (start + bytes > VW_MEM) start = 0;
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
        if (!t0) t0 = prof_begin();
    }
    if (t0) prof_end(PROF_VWAIT, t0);
    g_vwMemHead = start + bytes;
    return g_vwMem + start;
}

// Queues a prepared draw that may write up to `verts` arena vertices.
static void vw_queue(const VWork* w, u32 verts) {
    if (!g_vwReserved) g_vwPos = g.arenaUsed;  // synced: the worker starts where this thread is
    g_vwReserved += verts;
    g_vw[g_vwHead % VW_SLOTS] = *w;
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
    vw_sync();
    g_vwOn = g_vwWant;
}
bool pica_vertex_worker(void) { return g_vwOn && g_vwThread; }
bool pica_vertex_worker_wanted(void) { return g_vwWant; }

static void pica_draw(void* ctx, System* s, u32 prim, u32 vat, const u8* raw, u32 count, u32 vsize) {
    (void)ctx;
    vw_apply();
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
    u32 frameBefore = g.frameNo;  // looking up textures can also start a frame (texture graveyard full)
    // The open batch's key holds the TEV words first, so the used texture maps (from the compiled TEV) are only
    // compared once the TEV is known to be the same.
    // Lines and points are lit on the CPU (shader lighting is for triangles): they never share a batch with triangles.
    u32 key[96], kn = batch_key(gx, key);
    key[kn++] = prim >= PRIM_LINES;
    bool same = g.batchOpen && kn <= g.batchKeyLen && !memcmp(key, g.batchKey, kn * 4);
    if (same) { tex_key(gx, key, &kn); same = kn == g.batchKeyLen && !memcmp(key, g.batchKey, kn * 4); }
    C3D_Tex* tex[PICA_TEXUNITS] = { NULL, NULL, NULL };
    if (!same) {
        flush_batch();
        update_tev(gx);
        kn = batch_key(gx, key);
        key[kn++] = prim >= PRIM_LINES;
        tex_key(gx, key, &kn);
        u64 t0 = prof_begin();
        for (u32 i = 0; i < g.tev.numTex && i < PICA_TEXUNITS; i++) tex[i] = fetch_texture(s, gx, g.tev.texMap[i] & 7);
        prof_end(PROF_GXTEX, t0);
        g.batchGpuLight = g.shaderLight && prim < PRIM_LINES && vfast_gpu_light(gx, &g.sel, &g.lu);
    }
    u64 t0 = prof_begin();
    VFold f;
    vfold_init(gx, SX * 2.0f / TW, SY * 2.0f / TH, FLIP_Y, &f);
    bool lines = prim >= PRIM_LINES;
    // Triangles: the first half of the vertex work here, the second on the vertex worker (core 3).
    bool work = !lines && pica_vertex_worker();
    VWork w;
    u32 workVerts = 0;
    if (work) {
        workVerts = tri_verts(prim, count);
        if (!workVerts) { prof_end(PROF_GXVTX, t0); return; }
        if (!need_work(workVerts)) { emu_log(LOG_GX, "pica: draw of %u vertices too large, dropped\n", (unsigned)count); return; }
        u32 jb = (vfast_job_bytes(gx, count) + 7) & ~7u;
        u8* mem = vw_alloc(jb + count * (u32)sizeof(PVert) + count * 8);
        if (!mem) work = false;
        else {
            w.pv = (PVert*)(mem + jb);
            w.nd = (float (*)[2])(w.pv + count);
            w.job = vfast_prepare(s, gx, vat, &f, &g.sel, raw, count, vsize, g.batchGpuLight, mem, w.pv);
            if (!w.job) { prof_end(PROF_GXVTX, t0); return; }
            w.memStart = (u32)(mem - g_vwMem);
            w.prim = prim;
            w.count = count;
            w.cull = (gx->bp[0x00] >> 14) & 3;
            u32 op = 0;
            float z = 0;
            w.ztexOp = ztex_value(s, gx, &op, &z) ? op : 0;
            w.ztexZ = z;
        }
    }
    if (work) {
    } else if (lines) {
        for (u32 i = 0; i < count; i++) {
            GXVertex dv;
            gx_decode_vertex(s, gx, vat, raw + i * vsize, &dv);
            gx_transform(gx, &dv, &g_sv[i]);
            gx_to_screen(gx, &g_sv[i]);
        }
    } else {
        vfast_run(s, gx, vat, &f, &g.sel, raw, count, vsize, g_pv, g.batchGpuLight);
        apply_ztex(s, gx, count);
        g_cull = (gx->bp[0x00] >> 14) & 3;
        if (g_cull == 1 || g_cull == 2)
            for (u32 i = 0; i < count; i++) {
                float w = g_pv[i].pos[3], iw = w > 0 ? 1.0f / w : 0;
                g_nd[i][0] = g_pv[i].pos[0] * iw; g_nd[i][1] = g_pv[i].pos[1] * iw;
            }
    }
    prof_end(PROF_GXVTX, t0);
    u32 maxVerts = lines ? count * 6 : count * 3;
    if (!work && !need(maxVerts)) { emu_log(LOG_GX, "pica: draw of %u vertices too large, dropped\n", (unsigned)count); return; }
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
        memcpy(g.batchKey, key, kn * 4);
        g.batchKeyLen = kn;
    }
    if (work) {  // the worker appends its triangles to the arena; the batch's draw call waits for it (flush_batch)
        vw_queue(&w, workVerts);
        return;
    }
    switch (prim) {
    case PRIM_QUADS: case PRIM_QUADS2:
        for (u32 i = 0; i + 3 < count; i += 4) { tri(i, i + 1, i + 2); tri(i, i + 2, i + 3); }
        break;
    case PRIM_TRIS: for (u32 i = 0; i + 2 < count; i += 3) tri(i, i + 1, i + 2); break;
    case PRIM_STRIP: for (u32 i = 0; i + 2 < count; i++) { if (i & 1) tri(i + 1, i, i + 2); else tri(i, i + 1, i + 2); } break;
    case PRIM_FAN: for (u32 i = 1; i + 1 < count; i++) tri(0, i, i + 1); break;
    case PRIM_LINES: for (u32 i = 0; i + 1 < count; i += 2) line(gx, &f, &g_sv[i], &g_sv[i + 1]); break;
    case PRIM_LSTRIP: for (u32 i = 0; i + 1 < count; i++) line(gx, &f, &g_sv[i], &g_sv[i + 1]); break;
    case PRIM_POINTS: for (u32 i = 0; i < count; i++) point(gx, &f, &g_sv[i]); break;
    default: break;
    }
}

// ---- EFB copies and clears ----
// Fixed-function state for a full-target textured quad (copies and present).
static void quad_state(C3D_Tex* src, bool linear) {
    flush_batch();
    C3D_FVUnifSet(GPU_VERTEX_SHADER, g.uOn, 0, 0, 0, 0);  // copy and present quads carry their colour
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
    C3D_DrawArrays(GPU_TRIANGLES, (int)g.arenaUsed, 6);
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

static const GXBackend kPica = { pica_draw, pica_efb_copy };

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
    (void)s;
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
    C3D_DrawArrays(GPU_TRIANGLES, (int)g.arenaUsed, 6);
    g.arenaUsed += 6;
    if (g_capState == 1 && (g_capBuf || (g_capBuf = linearAlloc(CAP_W * CAP_H * 4)))) {
        split(1);  // the screen quad is drawn before the copy reads the target
        GX_TextureCopy((u32*)g.screen->frameBuf.colorBuf, 0, g_capBuf, 0, CAP_W * CAP_H * 4, GX_TRANSFER_RAW_COPY(1));
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

void pica_reset(void) {
    vw_sync();
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
        g.arena = linearAlloc(ARENA_VERTS * sizeof(PVert));
        if (!g.arena) return false;
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
