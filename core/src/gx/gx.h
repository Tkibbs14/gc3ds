#pragma once
// GameCube GPU (Flipper) command processing: the FIFO command stream, CP/XF/BP register files, draws and EFB copies.
// Rendering itself is behind the GXBackend interface (software reference on the PC, PICA200 on the 3DS later).
#include "../common.h"

typedef struct System System;

#define EFB_W 640
#define EFB_H 528

// Vertex attribute indices in the order the hardware streams them.
enum { VA_PNMTX, VA_TEX0MTX, VA_POS = 9, VA_NRM, VA_COL0, VA_COL1, VA_TEX0, VA_COUNT = VA_TEX0 + 8 };

typedef struct GXBackend GXBackend;

typedef struct {
    u32 cp[256];         // CP registers (matrix index, VCD, VAT, array base/stride)
    u32 bp[256];         // BP registers (rasteriser, TEV, PE, texture, EFB copy)
    u32 bpMask;          // write mask for the next BP write (BP 0xFE), 0xFFFFFF when unset
    u32 xf[0x1058];      // XF memory: matrices at 0x0000.., normal matrices 0x0400.., post 0x0500.., lights 0x0600..,
                         // registers at 0x1000..
    u8* pending;         // bytes received but not yet forming a complete command
    u32 pendingLen, pendingCap;
    u64 commands, draws, vertices;
    // TEV colour registers (PREV, C0, C1, C2) and konst colours share BP 0xE0-0xE7; bit 23 of the RA/BG write
    // selects konst. Stored as written: lo = (A << 12) | R, hi = (G << 12) | B, 11-bit signed for registers.
    u32 tevLo[4], tevHi[4], kLo[4], kHi[4];
    u8* tmem;            // 1 MB texture memory (palettes are loaded here)
    u32 tlutVersion;     // incremented on every palette load (texture caches compare it)
    u32* efb;            // RGBA8 colour, EFB_W x EFB_H (software backend and EFB copies)
    u32* efbZ;           // 24-bit depth
    const GXBackend* backend;
    void* backendCtx;
    u32 texVersion;      // incremented when texture memory in RAM may have changed (EFB copies, each frame);
                         // texture caches re-read RAM when it moves
    u64 efbCopies;       // EFB -> texture copies executed
    u64 primCount[8];    // draws per primitive type (quads, quads2, tris, strip, fan, lines, line strip, points)
    u32 bpNonZero[8];    // bit per BP register ever written with a non-zero value (feature-use diagnostics)
    bool traceDraws;     // debug: backends log every draw (LOG_GX)
    u32 xfLightVersion;  // incremented when XF lights (0x600-0x67F), channel controls (0x1009-0x1011) or texgen
                         // settings (0x103F-0x1047) change: shader lighting re-reads them then
    // Incremented when a register of the kind changes value (BP, CP, XF registers 0x1000-): backends skip work that
    // depends only on registers that have not changed since they last did it.
    u32 bpVersion, cpVersion, xfRegVersion;
#ifdef GC3DS_NATIVE
    // Native mode only (core/native, Aurora's GX stream): addresses too wide for the BP registers. Not in the
    // emulator's GXCore, whose layout save states depend on (tp850 failed to load with them, relay job 245).
    u32 texAddr[8];      // texture map data (GX_AURORA_LOAD_TEXOBJ)
    u32 copyDest;        // EFB -> texture copy destination (GX_AURORA_LOAD_COPY_DEST)
    u32 copyCtl;         // BP 0x52 format, intensity and box-filter bits from GX_AURORA_LOAD_COPY_DST
    u8 tlutFmt[32];      // palette format per Aurora TLUT slot (GX_AURORA_LOAD_TLUT)
    u32 arrayLE;         // bit per CP array the stream declared little-endian (not decoded yet: counted)
    u64 aurora[8];       // Aurora commands seen: arrays, LE arrays, textures, palettes, copies, sized / indexed draws,
                         // ignored (viewport / scissor render, projection, offscreen, polygon offset)
#endif
} GXCore;

#ifdef GC3DS_NATIVE
#define GX_ARRAY_LE(gx) ((gx)->arrayLE)
static inline u32 gx_tex_addr(const GXCore* gx, u32 map, u32 img3) {
    return gx->texAddr[map & 7] ? gx->texAddr[map & 7] : (img3 & 0xFFFFFF) << 5;
}
static inline u32 gx_copy_dest(const GXCore* gx) { return gx->copyDest ? gx->copyDest : (gx->bp[0x4B] & 0xFFFFFF) << 5; }
#else
#define GX_ARRAY_LE(gx) 0u  // the emulator's arrays are the GameCube's: big-endian
static inline u32 gx_tex_addr(const GXCore* gx, u32 map, u32 img3) { (void)gx; (void)map; return (img3 & 0xFFFFFF) << 5; }
static inline u32 gx_copy_dest(const GXCore* gx) { return (gx->bp[0x4B] & 0xFFFFFF) << 5; }
#endif

// Draw a primitive: `verts` is the raw vertex data (big-endian, as streamed), `vtxSize` bytes each.
struct GXBackend {
    void (*draw)(void* ctx, System* s, u32 prim, u32 vat, const u8* verts, u32 count, u32 vtxSize);
    // Optional (NULL for the software renderer): performs an EFB copy (BP 0x52 control word, source rectangle in
    // EFB pixels, destination physical address) on the backend's own framebuffer, including the clear. Returns true
    // when handled; the software copy into RAM is then skipped.
    bool (*efb_copy)(void* ctx, System* s, u32 ctl, u32 x0, u32 y0, u32 w, u32 h, u32 dest);
    // Optional: issues the draw the backend holds open for merging (g_gxMergeOpen). fifo.c calls it before any command
    // that changes state the open draw depends on.
    void (*flush)(void* ctx, System* s);
    // Optional: triangles from `nv` vertices and `ni` u16 indices in host byte order (native mode, Aurora's
    // GX_AURORA_DRAW_INDEXED), each vertex transformed once. Returns false when not taken; fifo.c then expands it.
    bool (*draw_indexed)(void* ctx, System* s, u32 vat, const u8* verts, u32 nv, u32 vsize, const u8* idx, u32 ni);
    // Optional (native mode): the same draw read in place from a display list called by reference, `cmd` its command's
    // address there (stable while the list lives), so the backend may keep the draw converted. False: not taken.
    bool (*draw_native)(void* ctx, System* s, u32 vat, const u8* cmd, u32 len, const u8* verts, u32 nv, u32 vsize,
                        const u8* idx, u32 ni);
};
// True while the backend holds a draw open to append the next draws to (set and cleared by the backend).
extern bool g_gxMergeOpen;
extern u64 g_dlStats[7];  // display-list statistics (fifo.c dl_note)
extern u64 g_dlRun[11];   // runs of draws inside display lists (fifo.c run_list)

// Guest RAM pages (4 KB) changed behind the CPU's cache: by the game's cache flushes (dcbf, dcbst, dcbz) and by DMA
// (disc, ARAM, EXI, locked cache). The GPU of a GameCube reads vertex arrays and display lists
// from RAM, so a game that changes them with the CPU must flush them, or the real GPU would read stale data: these
// stamps are what the display-list vertex cache (vfast.c) checks (docs/DL_CACHE_PLAN.md). `now` grows by one per
// change; a page holds the `now` of its last change. Plain CPU stores are not tracked (the hardware's rule).
#define MEM_PAGES (0x01800000u >> 12)
typedef struct { volatile u32 now; volatile u32 page[MEM_PAGES]; } MemStamps;
extern MemStamps g_memStamps;
static inline void mem_dirty(u32 pa, u32 len) {
    if (!len || pa >= 0x01800000u) return;
    u32 last = pa + len - 1 >= 0x01800000u ? MEM_PAGES - 1 : (pa + len - 1) >> 12;
    u32 now = g_memStamps.now + 1;
    g_memStamps.now = now;
    for (u32 p = pa >> 12; p <= last; p++) g_memStamps.page[p] = now;
}

// One decoded vertex, before transformation (model space, dequantised).
typedef struct {
    float pos[3];
    float nrm[3][3];   // normal, binormal, tangent
    u32 color[2];      // RGBA8 (0xRRGGBBAA); 0xFFFFFFFF when absent
    float tex[8][2];
    u8 pnmtx, texmtx[8];
    u8 hasColor, hasTex;  // bit per colour / texture coordinate set present in the stream
} GXVertex;

void gx_decode_vertex(System* s, const GXCore* gx, u32 vat, const u8* raw, GXVertex* out);

// Textures: GameCube tiled formats to linear RGBA8 (0xRRGGBBAA). `tlut` (big-endian 16-bit entries) is used by
// the paletted formats C4/C8/C14X2 with tlutFmt 0 = IA8, 1 = RGB565, 2 = RGB5A3.
void gx_decode_texture(const u8* src, u32 fmt, u32 w, u32 h, const u8* tlut, u32 tlutFmt, u32* out);
u32 gx_texture_size(u32 fmt, u32 w, u32 h);  // bytes the texture occupies in RAM
void gx_texture_block_size(u32 fmt, u32* bw, u32* bh);

// Software reference renderer (PC): draws go into GXCore.efb / efbZ.
void gx_soft_attach(System* s);
void gx_soft_set_pica_tev(bool on);  // TEV through the PICA translation (see soft.c)
// One pixel of the current TEV state with fixed rasterised colours and texels per texture map (translation tests).
void gx_soft_eval_tev(const GXCore* gx, const float ras[2][4], const float texel[8][4], float out[4]);

void gxcore_init(GXCore* gx);
void gxcore_free(GXCore* gx);
// Feeds command bytes (console byte order) and executes every complete command.
void gxcore_feed(System* s, GXCore* gx, const u8* data, u32 len);
// Size in bytes of one vertex for the given vertex attribute table, from the current VCD/VAT.
u32 gx_vertex_size(const GXCore* gx, u32 vat);
