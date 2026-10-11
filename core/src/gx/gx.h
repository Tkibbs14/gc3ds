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
} GXCore;

// Draw a primitive: `verts` is the raw vertex data (big-endian, as streamed), `vtxSize` bytes each.
struct GXBackend {
    void (*draw)(void* ctx, System* s, u32 prim, u32 vat, const u8* verts, u32 count, u32 vtxSize);
    // Optional (NULL for the software renderer): performs an EFB copy (BP 0x52 control word, source rectangle in
    // EFB pixels, destination physical address) on the backend's own framebuffer, including the clear. Returns true
    // when handled; the software copy into RAM is then skipped.
    bool (*efb_copy)(void* ctx, System* s, u32 ctl, u32 x0, u32 y0, u32 w, u32 h, u32 dest);
};

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
