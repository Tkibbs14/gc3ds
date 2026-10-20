// FIFO command processing. Commands arrive through the write-gather pipe (and display lists in RAM) and are executed
// as soon as they are complete, which keeps PE interrupts and EFB copies in step with the CPU without a GPU thread.
#include "../host/gxcensus.h"
#include <stdio.h>
#include <stdlib.h>
#include "../sys/system.h"
#include "gx.h"

// Commands are read in guest memory's layout (ramlayout.h): display lists in RAM, and the gather pipe's bytes, which
// feed() stores in the same layout so that one parser reads both. (Native mode's Aurora commands assume the
// big-endian layout, GC_SWAPRAM 0.)
static u32 rd8(const u8* p) { return ramp_rd8(p); }
static u32 rd16(const u8* p) { return ramp_rd16(p); }
static u32 rd32(const u8* p) { return ramp_rd32(p); }

// CP register numbers.
enum { CP_VCD_LO = 0x50, CP_VCD_HI = 0x60, CP_VAT_A = 0x70, CP_VAT_B = 0x80, CP_VAT_C = 0x90, CP_ARRAY_BASE = 0xA0,
       CP_ARRAY_STRIDE = 0xB0 };
// BP register numbers used here.
enum { BP_EFB_TL = 0x49, BP_EFB_WH = 0x4A, BP_COPY_DEST = 0x4B, BP_COPY_STRIDE = 0x4D, BP_CLEAR_AR = 0x4F,
       BP_CLEAR_GB = 0x50, BP_CLEAR_Z = 0x51, BP_COPY_EXEC = 0x52, BP_PE_DONE = 0x45, BP_PE_TOKEN = 0x47,
       BP_PE_TOKEN_INT = 0x48, BP_MASK = 0xFE };

void gxcore_init(GXCore* gx) {
    u8* pending = gx->pending;
    u32 cap = gx->pendingCap;
    u32* efb = gx->efb;
    u32* efbZ = gx->efbZ;
    u8* tmem = gx->tmem;
    const GXBackend* backend = gx->backend;
    void* backendCtx = gx->backendCtx;
    memset(gx, 0, sizeof *gx);
    gx->pending = pending;
    gx->pendingCap = cap;
    gx->backend = backend;
    gx->backendCtx = backendCtx;
    gx->tmem = tmem ? tmem : calloc(1, 1u << 20);
    gx->efb = efb ? efb : calloc(EFB_W * EFB_H, 4);
    gx->efbZ = efbZ ? efbZ : calloc(EFB_W * EFB_H, 4);
    gx->bpMask = 0xFFFFFF;
}

void gxcore_free(GXCore* gx) {
    free(gx->pending);
    free(gx->tmem);
    free(gx->efb);
    free(gx->efbZ);
}

// ---- vertex size from VCD (which attributes, direct or indexed) and VAT (component counts and formats) ----
static u32 comp_size(u32 fmt) { return fmt <= 1 ? 1 : fmt <= 3 ? 2 : 4; }
static u32 color_size(u32 fmt) { static const u8 s[8] = { 2, 3, 4, 2, 3, 4, 4, 4 }; return s[fmt & 7]; }

u32 gx_vertex_size(const GXCore* gx, u32 vat) {
    u32 lo = gx->cp[CP_VCD_LO], hi = gx->cp[CP_VCD_HI];
    u32 a = gx->cp[CP_VAT_A + vat], b = gx->cp[CP_VAT_B + vat], c = gx->cp[CP_VAT_C + vat];
    u32 size = 0;
    size += lo & 1;                                   // position/normal matrix index
    for (int i = 0; i < 8; i++) size += (lo >> (1 + i)) & 1;  // texture matrix indices
    u32 pos = (lo >> 9) & 3, nrm = (lo >> 11) & 3, col0 = (lo >> 13) & 3, col1 = (lo >> 15) & 3;
    if (pos == 1) size += ((a & 1) ? 3 : 2) * comp_size((a >> 1) & 7);
    else if (pos) size += pos - 1;
    if (nrm) {
        bool nbt = (a >> 9) & 1, index3 = (a >> 31) & 1;
        if (nrm == 1) size += (nbt ? 9 : 3) * comp_size((a >> 10) & 7);
        else size += (nrm - 1) * (nbt && index3 ? 3 : 1);
    }
    if (col0 == 1) size += color_size((a >> 14) & 7); else if (col0) size += col0 - 1;
    if (col1 == 1) size += color_size((a >> 18) & 7); else if (col1) size += col1 - 1;
    // Texture coordinates: count bit and format for each of the 8 sets, spread over VAT A/B/C.
    u32 tcnt[8] = { (a >> 21) & 1, b & 1, (b >> 9) & 1, (b >> 18) & 1, (b >> 27) & 1, (c >> 5) & 1, (c >> 14) & 1, (c >> 23) & 1 };
    u32 tfmt[8] = { (a >> 22) & 7, (b >> 1) & 7, (b >> 10) & 7, (b >> 19) & 7, (b >> 28) & 7, (c >> 6) & 7, (c >> 15) & 7, (c >> 24) & 7 };
    for (int i = 0; i < 8; i++) {
        u32 t = (hi >> (2 * i)) & 3;
        if (t == 1) size += (tcnt[i] ? 2 : 1) * comp_size(tfmt[i]);
        else if (t) size += t - 1;
    }
    return size;
}

// ---- register writes ----
// Draw merging: while the backend holds a draw open to append the next draws to (pica.c), it reads the draw's state
// only when it issues it. Every command that would change that state (any register value, palette loads, PE signals,
// EFB copies) first lets the backend issue the open draw.
bool g_gxMergeOpen;
volatile u8 g_gxWhere;
static inline void merge_break(System* s, GXCore* gx) {
    if (g_gxMergeOpen && gx->backend && gx->backend->flush) gx->backend->flush(gx->backendCtx, s);
}

static void xf_write(System* s, GXCore* gx, u32 addr, u32 v) {
    if (addr >= sizeof gx->xf / 4) return;
    GXCENSUS(gxcensus_reg(2, addr, gx->xf[addr], v));
    if (gx->xf[addr] == v) return;
    merge_break(s, gx);
    // Lights, channel controls and texgen settings: what shader lighting depends on (GXCore.xfLightVersion).
    if (((addr >= 0x600 && addr < 0x680) || (addr >= 0x1009 && addr <= 0x1011) || (addr >= 0x103F && addr <= 0x1047)) &&
        gx->xf[addr] != v)
        gx->xfLightVersion++;
    if (addr >= 0x1000 && addr != 0x1018 && addr != 0x1019 && gx->xf[addr] != v) gx->xfRegVersion++;  // not the matrix indices
    gx->xf[addr] = v;
}

void gx_pe_signal(System* s, u32 kind, u32 value) {
    u16* ctrl = &s->gx.peRegs[0x0A / 2];
    if (kind == GXEV_FINISH) {
        *ctrl |= 0x08;
        if (*ctrl & 0x02) pi_raise(s, PI_INT_PE_FINISH);
        return;
    }
    s->gx.peRegs[0x0E / 2] = (u16)value;
    if (kind != GXEV_TOKEN_IRQ) return;
    *ctrl |= 0x04;
    if (*ctrl & 0x01) pi_raise(s, PI_INT_PE_TOKEN);
}

// PE signals touch PI and PE state, which belong to the CPU thread: with asynchronous GX they are posted to it.
static void pe_signal(System* s, u32 kind, u32 value) {
    if (s->gxAsync) s->gxAsync->event(s, kind, value);
    else gx_pe_signal(s, kind, value);
}

static void pe_finish(System* s) { pe_signal(s, GXEV_FINISH, 0); }
static void pe_token(System* s, u32 token, bool irq) { pe_signal(s, irq ? GXEV_TOKEN_IRQ : GXEV_TOKEN, token); }

static u8 clamp8(int v) { return (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }

// ---- EFB -> texture copy ----
// BP 0x52 (copy control): bit 9 half-size box filter, bit 11 clear, bit 14 to XFB, bit 15 intensity (YUV) formats,
// bits 3-6 the target format, stored rotated: format = (field >> 1) | ((field & 1) << 3). BP 0x4D: destination stride
// in 32-byte units per row of tiles. Depth is copied instead of colour when the EFB format (BP 0x43 bits 0-2) is 3
// (Z24 copy mode). Sources: YAGCD 5.11 (BP registers), Dolphin's BPMemory.h field layout (read for reference).
enum { CF_R4 = 0, CF_R8_ALIAS = 1, CF_RA4 = 2, CF_RA8 = 3, CF_RGB565 = 4, CF_RGB5A3 = 5, CF_RGBA8 = 6, CF_A8 = 7,
       CF_R8 = 8, CF_G8 = 9, CF_B8 = 10, CF_RG8 = 11, CF_GB8 = 12 };

// Source texel (RGBA8 0xRRGGBBAA, or depth in the low 24 bits) at output position x, y.
static u32 efb_src(const GXCore* gx, u32 x0, u32 y0, u32 w, u32 h, u32 x, u32 y, bool half, bool depth, bool noAlpha) {
    const u32* buf = depth ? gx->efbZ : gx->efb;
    u32 sx = half ? x * 2 : x, sy = half ? y * 2 : y;
    if (sx >= w) sx = w - 1;
    if (sy >= h) sy = h - 1;
    u32 c;
    if (!half || depth) {
        c = buf[(y0 + sy) * EFB_W + x0 + sx];
    } else {  // 2x2 box filter
        u32 acc[4] = { 0, 0, 0, 0 };
        for (u32 j = 0; j < 2; j++)
            for (u32 i = 0; i < 2; i++) {
                u32 xx = sx + i < w ? sx + i : w - 1, yy = sy + j < h ? sy + j : h - 1;
                u32 p = buf[(y0 + yy) * EFB_W + x0 + xx];
                for (int k = 0; k < 4; k++) acc[k] += (p >> (24 - 8 * k)) & 0xFF;
            }
        c = ((acc[0] / 4) << 24) | ((acc[1] / 4) << 16) | ((acc[2] / 4) << 8) | (acc[3] / 4);
    }
    if (!depth && noAlpha) c |= 0xFF;
    return c;
}

static u32 lum(u32 c) {  // BT.601 studio-range luma, as the copy unit's intensity formats use
    u32 r = c >> 24, g = (c >> 16) & 0xFF, b = (c >> 8) & 0xFF;
    return (66 * r + 129 * g + 25 * b + 128) / 256 + 16;
}

static void efb_copy_texture(System* s, GXCore* gx, u32 ctl, u32 x0, u32 y0, u32 w, u32 h, u32 dest) {
    u32 field = (ctl >> 3) & 15, fmt = (field >> 1) | ((field & 1) << 3);
    bool half = (ctl >> 9) & 1, intensity = (ctl >> 15) & 1;
    u32 efbFmt = gx->bp[0x43] & 7;
    bool depth = efbFmt == 3, noAlpha = efbFmt != 1;  // RGBA6_Z24 (1) is the only colour format with alpha
    u32 ow = half ? (w + 1) / 2 : w, oh = half ? (h + 1) / 2 : h;
    u32 bw, bh, bytes;
    bool bpp4 = fmt == CF_R4, bpp8 = fmt == CF_R8_ALIAS || fmt == CF_RA4 || fmt == CF_A8 || (fmt >= CF_R8 && fmt <= CF_B8);
    if (bpp4) { bw = 8; bh = 8; bytes = 32; } else if (bpp8) { bw = 8; bh = 4; bytes = 32; }
    else { bw = 4; bh = 4; bytes = fmt == CF_RGBA8 ? 64 : 32; }
    u32 stride = (gx->bp[BP_COPY_STRIDE] & 0x3FF) << 5;
    u32 tilesX = (ow + bw - 1) / bw, tilesY = (oh + bh - 1) / bh;
    if (!stride) stride = tilesX * bytes;
    gx->efbCopies++;
    gx->texVersion++;
    emu_log(LOG_GX, "EFB copy to texture %08x fmt %u%s%s %ux%u -> %ux%u\n", dest, fmt, intensity ? " (I)" : "",
            depth ? " (Z)" : "", w, h, ow, oh);
    for (u32 ty = 0; ty < tilesY; ty++) {
        for (u32 tx = 0; tx < tilesX; tx++) {
            u32 at = dest + ty * stride + tx * bytes;
            if (at < PA_MEM1_BEGIN || at + bytes > PA_MEM1_END) return;
            u8 o[64];  // the tile, big-endian, then into RAM in its layout
            memset(o, 0, bytes);
            for (u32 py = 0; py < bh; py++) {
                for (u32 px = 0; px < bw; px++) {
                    u32 x = tx * bw + px, y = ty * bh + py, i = py * bw + px;
                    u32 c = efb_src(gx, x0, y0, w, h, x < ow ? x : ow - 1, y < oh ? y : oh - 1, half, depth, noAlpha);
                    u32 r, g, b, a;
                    if (depth) {  // Z24: high, middle and low bytes play R, G, B
                        r = (c >> 16) & 0xFF; g = (c >> 8) & 0xFF; b = c & 0xFF; a = 0xFF;
                    } else {
                        r = c >> 24; g = (c >> 16) & 0xFF; b = (c >> 8) & 0xFF; a = c & 0xFF;
                        if (intensity && fmt <= CF_RA8) r = g = b = lum(c);
                    }
                    switch (fmt) {
                    case CF_R4: o[i / 2] |= (u8)((r >> 4) << ((i & 1) ? 0 : 4)); break;
                    case CF_R8_ALIAS: case CF_R8: o[i] = (u8)r; break;
                    case CF_G8: o[i] = (u8)g; break;
                    case CF_B8: o[i] = (u8)b; break;
                    case CF_A8: o[i] = (u8)a; break;
                    case CF_RA4: o[i] = (u8)(((a >> 4) << 4) | (r >> 4)); break;
                    case CF_RA8: o[2 * i] = (u8)a; o[2 * i + 1] = (u8)r; break;
                    // Two-channel formats store their second-named channel first, like IA8 stores A before I
                    // (byte order as Dolphin's encoders produce it: RA8 = A,R; RG8 = G,R; GB8 = B,G).
                    case CF_RG8: o[2 * i] = (u8)g; o[2 * i + 1] = (u8)r; break;
                    case CF_GB8: o[2 * i] = (u8)b; o[2 * i + 1] = (u8)g; break;
                    case CF_RGB565: {
                        u32 v = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3);
                        o[2 * i] = (u8)(v >> 8); o[2 * i + 1] = (u8)v;
                        break;
                    }
                    case CF_RGB5A3: {
                        u32 v = a >= 0xE0 ? 0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)
                                          : ((a >> 5) << 12) | ((r >> 4) << 8) | ((g >> 4) << 4) | (b >> 4);
                        o[2 * i] = (u8)(v >> 8); o[2 * i + 1] = (u8)v;
                        break;
                    }
                    case CF_RGBA8: o[2 * i] = (u8)a; o[2 * i + 1] = (u8)r; o[32 + 2 * i] = (u8)g; o[32 + 2 * i + 1] = (u8)b; break;
                    default: break;
                    }
                }
            }
            ram_from_be(s->mem1, at, o, bytes);
        }
    }
}

// EFB -> XFB copy (YUYV) or EFB -> texture, optionally clearing the source rectangle afterwards.
static void efb_copy(System* s, GXCore* gx, u32 ctl) {
    u32 tl = gx->bp[BP_EFB_TL], wh = gx->bp[BP_EFB_WH];
    u32 x0 = tl & 0x3FF, y0 = (tl >> 10) & 0x3FF, w = (wh & 0x3FF) + 1, h = ((wh >> 10) & 0x3FF) + 1;
    u32 dest = gx_copy_dest(gx), stride = (gx->bp[BP_COPY_STRIDE] & 0x3FF) << 5;
    bool toXfb = (ctl >> 14) & 1, clear = (ctl >> 11) & 1;
    if (x0 + w > EFB_W) w = EFB_W - x0;
    if (y0 + h > EFB_H) h = EFB_H - y0;
    if (gx->backend && gx->backend->efb_copy && gx->backend->efb_copy(gx->backendCtx, s, ctl, x0, y0, w, h, dest)) {
        if (!toXfb) gx->efbCopies++;
        gx->texVersion++;
        return;
    }
    if (toXfb) {
        emu_log(LOG_GX, "EFB copy to XFB %08x %ux%u from %u,%u (%llu draws so far)\n", dest, w, h, x0, y0,
                (unsigned long long)gx->draws);
        for (u32 y = 0; y < h; y++) {
            u32 row = dest + y * stride;
            if (row < PA_MEM1_BEGIN || row + w * 2 > PA_MEM1_END) break;
            for (u32 x = 0; x + 1 < w; x += 2) {
                u32 c[2] = { gx->efb[(y0 + y) * EFB_W + x0 + x], gx->efb[(y0 + y) * EFB_W + x0 + x + 1] };
                int yy[2], u = 0, v = 0;
                for (int k = 0; k < 2; k++) {
                    int r = (c[k] >> 24) & 0xFF, g = (c[k] >> 16) & 0xFF, b = (c[k] >> 8) & 0xFF;
                    yy[k] = (66 * r + 129 * g + 25 * b + 128) / 256 + 16;
                    u += (-38 * r - 74 * g + 112 * b + 128) / 256 + 128;
                    v += (112 * r - 94 * g - 18 * b + 128) / 256 + 128;
                }
                const u8 o[4] = { clamp8(yy[0]), clamp8(u / 2), clamp8(yy[1]), clamp8(v / 2) };
                ram_from_be(s->mem1, row + x * 2, o, 4);
            }
        }
        gx->texVersion++;  // a new frame: textures in RAM may have been replaced since the last one
    } else {
        efb_copy_texture(s, gx, ctl, x0, y0, w, h, dest);
    }
    if (clear) {
        u32 ar = gx->bp[BP_CLEAR_AR], gb = gx->bp[BP_CLEAR_GB];
        u32 rgba = ((ar & 0xFF) << 24) | (((gb >> 8) & 0xFF) << 16) | ((gb & 0xFF) << 8) | ((ar >> 8) & 0xFF);
        u32 z = gx->bp[BP_CLEAR_Z] & 0xFFFFFF;
        for (u32 y = 0; y < h; y++)
            for (u32 x = 0; x < w; x++) { gx->efb[(y0 + y) * EFB_W + x0 + x] = rgba; gx->efbZ[(y0 + y) * EFB_W + x0 + x] = z; }
    }
}

// Bumped when a BP register the TEV key reads (tev_pica_key: 0x00, 0x28-0x2F, 0xC0-0xE7, 0xF6-0xFD) changes value, so
// the PICA backend can skip rebuilding and comparing the 72-word key when none did. Outside GXCore: a save state's
// layout stays (backends reset their cached TEV on a load, pica_reset).
u32 g_gxTevVersion;

static void bp_write(System* s, GXCore* gx, u32 reg, u32 val) {
    u32 mask = gx->bpMask;
    if (reg != BP_MASK) gx->bpMask = 0xFFFFFF;
    u32 nv = (gx->bp[reg] & ~mask) | (val & mask);
    GXCENSUS({
        u32 old = gx->bp[reg];
        // E0-E7 address two separate banks selected by bit 23.
        if (reg >= 0xe0 && reg <= 0xe7) {
            u32 i = (reg - 0xe0) / 2;
            old = nv & (1u << 23) ? (reg & 1 ? gx->kHi[i] : gx->kLo[i]) :
                                   (reg & 1 ? gx->tevHi[i] : gx->tevLo[i]);
        }
        gxcensus_reg(0, reg, old, nv);
    });
    if (nv != gx->bp[reg] || reg == BP_PE_DONE || reg == BP_PE_TOKEN || reg == BP_PE_TOKEN_INT || reg == BP_COPY_EXEC ||
        reg == 0x65)
        merge_break(s, gx);
    if (nv != gx->bp[reg]) {
        gx->bpVersion++;
        if (reg == 0 || (reg >= 0x28 && reg <= 0x2F) || (reg >= 0xC0 && reg <= 0xE7) || (reg >= 0xF6 && reg <= 0xFD))
            g_gxTevVersion++;
    }
    gx->bp[reg] = nv;
    if (gx->bp[reg]) gx->bpNonZero[reg >> 5] |= 1u << (reg & 31);
    if (reg >= 0xE0 && reg <= 0xE7) {
        u32 v = gx->bp[reg], i = (reg - 0xE0) / 2;
        bool konst = (v >> 23) & 1;
        if (reg & 1) { if (konst) gx->kHi[i] = v; else gx->tevHi[i] = v; }
        else { if (konst) gx->kLo[i] = v; else gx->tevLo[i] = v; }
    }
    switch (reg) {
    case BP_MASK: gx->bpMask = val & 0xFFFFFF; break;
    case 0x65: {  // LOADTLUT1: copy a palette from RAM (LOADTLUT0 = address >> 5) into texture memory
        u32 ram = (gx->bp[0x64] & 0x1FFFFF) << 5, tmemAddr = (val & 0x3FF) << 9, bytes = ((val >> 10) & 0x7FF) * 32;
        GXCENSUS_WORK(if (ram + bytes <= PA_MEM1_END && tmemAddr + bytes <= (1u << 20))
            ram_to_be(gx->tmem + tmemAddr, s->mem1, ram, bytes));
        gx->tlutVersion++;
        break;
    }
    case BP_PE_DONE: pe_finish(s); break;
    case BP_PE_TOKEN: pe_token(s, val & 0xFFFF, false); break;
    case BP_PE_TOKEN_INT: pe_token(s, val & 0xFFFF, true); break;
    case BP_COPY_EXEC:
        GXCENSUS_WORK({
        if (s->on_efb_copy) s->on_efb_copy(s, s->user, val);
        efb_copy(s, gx, val);
        {  // the copy wrote RAM (a bound: rows of h lines or tiles at the copy's stride, or 4 bytes a pixel unstrided)
            u32 wh = gx->bp[BP_EFB_WH], w = (wh & 0x3FF) + 1, h = ((wh >> 10) & 0x3FF) + 1;
            u32 stride = (gx->bp[BP_COPY_STRIDE] & 0x3FF) << 5, len = h * stride > w * h * 4 ? h * stride : w * h * 4;
            mem_dirty(gx_copy_dest(gx), len);
        }
        });
        break;
    }
}

// LOAD_INDX_A..D: copy (size) words from an indexed array element in RAM into XF memory.
static void load_indexed(System* s, GXCore* gx, u32 which, u32 v) {
    u32 index = v >> 16, size = ((v >> 12) & 0xF) + 1, addr = v & 0xFFF;
    u32 base = gx->cp[CP_ARRAY_BASE + 12 + which] & GX_ARRAY_MASK, stride = gx->cp[CP_ARRAY_STRIDE + 12 + which] & 0xFFF;
    u32 pa = base + index * stride;
    bool le = (GX_ARRAY_LE(gx) >> (12 + which)) & 1;  // native mode: a little-endian matrix array
    for (u32 i = 0; i < size && pa >= PA_MEM1_BEGIN && pa + 4 * i + 4 <= PA_MEM1_END; i++) {
        u32 v = mem1_read32(s, pa + 4 * i);
        xf_write(s, gx, addr + i, le ? bswap32(v) : v);
    }
}

static void run_list(System* s, GXCore* gx, const u8* p, u32 len, bool inList);

// Display-list statistics (docs/research/PERF_LEVERS.md D1: could the vertices a list gathers be cached across frames?).
// Per list address the last size, a hash of its bytes and of the array bases / strides it ran with; one call in four
// is checked against them. g_dlStats: calls, draws inside lists, draws outside, sampled calls, sampled with the same
// bytes, sampled with the same arrays too, new list addresses.
u64 g_dlStats[7];
MemStamps g_memStamps;
volatile u32 g_dmaGen;
u32 g_dmaLog[DMA_LOG][2];
#define DL_SLOTS 2048
static struct { u32 addr, size, bytes, arrays; } g_dl[DL_SLOTS];
static u32 dl_hash(const u8* p, u32 n, u32 h) { for (u32 i = 0; i < n; i++) h = (h ^ p[i]) * 16777619u; return h; }
// The decoder's measurement-only work (display-list hashes, the command-class profile): off unless profiling (main.c
// --profile on): dl_note's hashes alone were 3.9% of the GX thread (2DS job 432).
bool g_gxDiag;
static void dl_note(const GXCore* gx, u32 addr, const u8* p, u32 size) {
    g_dlStats[0]++;
    if (!g_gxDiag) return;
    u32 slot = (addr >> 5) & (DL_SLOTS - 1);
    if (g_dl[slot].addr != addr || g_dl[slot].size != size) {
        g_dl[slot].addr = addr; g_dl[slot].size = size; g_dl[slot].bytes = dl_hash(p, size, 2166136261u);
        g_dl[slot].arrays = dl_hash((const u8*)&gx->cp[CP_ARRAY_BASE], 32 * 4, 2166136261u);
        g_dlStats[6]++;
        return;
    }
    if (g_dlStats[0] & 3) return;
    g_dlStats[3]++;
    u32 b = dl_hash(p, size, 2166136261u), a = dl_hash((const u8*)&gx->cp[CP_ARRAY_BASE], 32 * 4, 2166136261u);
    if (b == g_dl[slot].bytes) { g_dlStats[4]++; if (a == g_dl[slot].arrays) g_dlStats[5]++; }
    g_dl[slot].bytes = b; g_dl[slot].arrays = a;
}

static void draw(System* s, GXCore* gx, u8 op, const u8* verts, u32 count, u32 vsize, bool inList) {
    GXCENSUS(gxcensus_draw(s, gx, op & 7, verts, count, vsize));
    gx->draws++;
    g_dlStats[inList ? 1 : 2]++;
    gx->vertices += count;
    gx->primCount[(op >> 3) & 7]++;
    GXCENSUS_WORK(if (gx->backend && count)
        gx->backend->draw(gx->backendCtx, s, (op >> 3) & 7, op & 7, verts, count, vsize));
}

#ifdef GC3DS_NATIVE
// Native mode: Aurora's extension commands, GX_AURORA (0x50) and a big-endian u16 subcommand, with the layouts of
// extern/aurora's include/dolphin/gx/GXAurora.h and lib/gx/command_processor.cpp. Host pointers come as u64; on the
// 3DS they fit the low word, and MEM1's base is zero, so they are used as addresses directly.
static u32 aurora_cmd(System* s, GXCore* gx, const u8* p, u32 avail, bool inList) {
    if (avail < 3) return 0;
    u32 sub = rd16(p + 1), have = avail - 3;
    const u8* a = p + 3;
#define NEED(n) do { if (have < (u32)(n)) return 0; } while (0)
    if (sub >= 0x0010 && sub <= 0x001F) {  // LOAD_ARRAYBASE | array: u64 data, u32 size, u8 little-endian
        NEED(13);
        u32 r = CP_ARRAY_BASE + (sub & 15), v = rd32(a + 4), bit = 1u << (sub & 15);
        if (gx->cp[r] != v) { merge_break(s, gx); gx->cp[r] = v; }
        if (rd8(a + 12)) { gx->arrayLE |= bit; gx->aurora[1]++; } else gx->arrayLE &= ~bit;
        gx->aurora[0]++;
        return 3 + 13;
    }
    switch (sub) {
    case 0x0001: NEED(24); gx->aurora[7]++; return 3 + 24;  // LOAD_VIEWPORT_RENDER: six f32
    case 0x0002: NEED(16); gx->aurora[7]++; return 3 + 16;  // LOAD_SCISSOR_RENDER: four u32
    case 0x0003: NEED(64); gx->aurora[7]++; return 3 + 64;  // LOAD_PROJECTION_FULL: sixteen f32
    case 0x0020: case 0x0022: { NEED(2); u32 n = rd16(a); NEED(2 + n); return 3 + 2 + n; }  // debug group, marker
    case 0x0021: return 3;
    case 0x0030: {  // LOAD_TEXOBJ: u8 map, u64 data, u32 width, height, format, tlut, u8 mips, u32 id, version
        NEED(34);
        u32 map = rd8(a) & 7, addr = rd32(a + 5);
        if (gx->texAddr[map] != addr) { merge_break(s, gx); gx->texAddr[map] = addr; gx->bpVersion++; }
        gx->aurora[2]++;
        return 3 + 34;
    }
    case 0x0031: {  // LOAD_TLUT: u8 slot, u64 data, u32 format, u16 entries, u32 id, version
        // The texture's BP TLUT register holds the slot (GXLoadTexObj), read as TMEM offset slot * 512: the palette
        // goes there, and its format is added to that register when it is written (bp_write).
        NEED(23);
        u32 slot = rd8(a) & 31, addr = rd32(a + 5), bytes = rd16(a + 13) * 2u, at = slot << 9;
        merge_break(s, gx);
        if (addr >= PA_MEM1_BEGIN && at + bytes <= (1u << 20)) ram_to_be(gx->tmem + at, s->mem1, addr, bytes);
        gx->tlutFmt[slot] = (u8)(rd32(a + 9) & 3);
        gx->tlutVersion++;
        gx->aurora[3]++;
        return 3 + 23;
    }
    case 0x0032: case 0x0033: NEED(4); return 3 + 4;  // DESTROY_TEXOBJ / TLUT: u32 id
    case 0x0034: NEED(8); return 3 + 8;               // DESTROY_COPY_TEX: u64 data
    case 0x0035: {  // LOAD_COPY_SRC: s32 left, top, width, height -> BP 0x49 / 0x4A
        NEED(16);
        u32 x = rd32(a), y = rd32(a + 4), w = rd32(a + 8), h = rd32(a + 12);
        bp_write(s, gx, BP_EFB_TL, (x & 0x3FF) | ((y & 0x3FF) << 10));
        bp_write(s, gx, BP_EFB_WH, ((w - 1) & 0x3FF) | (((h - 1) & 0x3FF) << 10));
        return 3 + 16;
    }
    case 0x0036: {  // LOAD_COPY_DST: u32 width, height, format, u8 mipmap -> the BP 0x52 bits GXSetTexCopyDst sets
        NEED(13);
        u32 fmt = rd32(a + 8), f = fmt & 15;
        gx->copyCtl = (((f & 7) << 1 | (f >> 3)) << 3) | (rd8(a + 12) ? 1u << 9 : 0) | (fmt < 4 ? 1u << 15 : 0);
        bp_write(s, gx, BP_COPY_STRIDE, 0);  // efb_copy_texture: a packed row of tiles
        gx->aurora[4]++;
        return 3 + 13;
    }
    case 0x0037: NEED(8); gx->copyDest = rd32(a + 4); return 3 + 8;  // LOAD_COPY_DEST: u64 data
    case 0x0038: return 3;                                           // REQUEST_DEPTH_SNAPSHOT
    case 0x0039: NEED(8); gx->aurora[7]++; return 3 + 8;             // BEGIN_OFFSCREEN: u32 width, height
    case 0x003A: return 3;                                           // END_OFFSCREEN
    case 0x0040: {  // DRAW_SIZED: u8 draw opcode, u32 bytes, vertex data
        NEED(5);
        u32 bytes = rd32(a + 1);
        NEED(5 + bytes);
        u32 vsize = gx_vertex_size(gx, rd8(a) & 7);
        if (bytes && vsize) draw(s, gx, (u8)rd8(a), a + 5, bytes / vsize, vsize, inList);
        gx->aurora[5]++;
        return 3 + 5 + bytes;
    }
    case 0x0041: {  // DRAW_INDEXED: u8 opcode (triangles), u16 vertices, u32 indices, u16 host-order indices, vertices
        NEED(7);
        u32 nv = rd16(a + 1), ni = rd32(a + 3), vsize = gx_vertex_size(gx, a[0] & 7);
        u32 len = 7 + ni * 2 + nv * vsize;
        NEED(len);
        const u8* idx = a + 7;
        const u8* verts = idx + ni * 2;
        if (inList && ni && vsize && gx->backend && gx->backend->draw_native &&
            gx->backend->draw_native(gx->backendCtx, s, a[0] & 7, p, 3 + len, verts, nv, vsize, idx, ni)) {
            gx->draws++;
            g_dlStats[1]++;
            gx->vertices += nv;
            gx->primCount[2]++;
            gx->aurora[6]++;
            return 3 + len;
        }
        if (ni && vsize && gx->backend && gx->backend->draw_indexed &&
            gx->backend->draw_indexed(gx->backendCtx, s, a[0] & 7, verts, nv, vsize, idx, ni)) {
            gx->draws++;
            g_dlStats[inList ? 1 : 2]++;
            gx->vertices += nv;
            gx->primCount[2]++;
            gx->aurora[6]++;
            return 3 + len;
        }
        static u8* buf;
        static u32 cap;
        if (ni * vsize > cap) {
            u8* nb = realloc(buf, ni * vsize);
            if (!nb) return 3 + len;
            buf = nb;
            cap = ni * vsize;
        }
        for (u32 i = 0; i < ni; i++) {
            u32 k = idx[2 * i] | (u32)idx[2 * i + 1] << 8;
            memcpy(buf + i * vsize, verts + (k < nv ? k : 0) * vsize, vsize);
        }
        if (ni && vsize) draw(s, gx, a[0], buf, ni, vsize, inList);
        gx->aurora[6]++;
        return 3 + len;
    }
    case 0x0060: {  // gc3ds: CALL_DL by reference (patch_dusk.sh, GXCallDisplayList): u64 data, u32 size
        NEED(12);
        u32 addr = rd32(a + 4), size = rd32(a + 8);
        if (addr >= PA_MEM1_BEGIN && size) run_list(s, gx, s->mem1 + addr, size, true);
        return 3 + 12;
    }
    case 0x1000: NEED(20); gx->aurora[7]++; return 3 + 20;  // GX2_SET_POLYGON_OFFSET: five f32
    }
#undef NEED
    static u32 reported;
    if (reported++ < 20) emu_log(LOG_GX, "unknown Aurora subcommand %04x\n", sub);
    return 3;
}
#endif

// Executes one command at p; returns its length, or 0 if more bytes are needed.
static u32 exec_one_(System* s, GXCore* gx, const u8* p, u32 avail, bool inList);
// The decoder's profile (one command in 16, scaled): ticks by command class: draws, BP, XF, CP, Aurora commands other
// than list calls, the rest; and how many commands of each class. List calls (0x40, Aurora 0x0060) are not timed
// themselves: their commands are. [6] / [7]: runs of draws in display lists handed to the backend whole (draw_list).
u64 g_gxOpTicks[8], g_gxOpCount[8];
static u32 exec_one(System* s, GXCore* gx, const u8* p, u32 avail, bool inList) {
    u8 op = (u8)rd8(p);
    u32 cls = op >= 0x80 && op < 0xC0 ? 0 : op == 0x61 ? 1 : op == 0x10 ? 2 : op == 0x08 ? 3 : op == 0x50 ? 4 : 5;
    bool call = op == 0x40 || (op == 0x50 && avail >= 3 && rd8(p + 1) == 0x00 && rd8(p + 2) == 0x60);
#ifdef GX_CENSUS_PC
    if (g_gxcensus) {
        gxcensus_pause(); // exclude child commands and probes from a list call's exclusive decoder time
        int tag = gxcensus_prepare(s, gx, p, avail, inList);
        gxcensus_timer_enter(tag, !call);
        u32 consumed = exec_one_(s, gx, p, avail, inList);
        gxcensus_timer_leave(consumed);
        gxcensus_resume();
        return consumed;
    }
#endif
    static u32 n;
    if (call || !g_gxDiag || !g_profClock || (++n & 15)) {
        if (!call) g_gxOpCount[cls]++;
        return exec_one_(s, gx, p, avail, inList);
    }
    g_gxOpCount[cls]++;
    u64 t0 = g_profClock();
    u32 r = exec_one_(s, gx, p, avail, inList);
    g_gxOpTicks[cls] += (g_profClock() - t0) * 16;
    return r;
}

static u32 exec_one_(System* s, GXCore* gx, const u8* p, u32 avail, bool inList) {
    u8 op = (u8)rd8(p);
    g_gxWhere = GW_DECOP;
    if (op >= 0x80 && op < 0xC0) {
        if (avail < 3) return 0;
        g_gxWhere = GW_DECDRAW;
        u32 count = rd16(p + 1), vat = op & 7, vsize = gx_vertex_size(gx, vat);
        u32 total = 3 + count * vsize;
        if (avail < total) return 0;
        draw(s, gx, op, p + 3, count, vsize, inList);
        return total;
    }
    switch (op) {
    case 0x00: return 1;              // NOP
    case 0x48: return 1;              // invalidate vertex cache
    case 0x44: return 1;              // unknown metrics command, no payload
    case 0x08:
        if (avail < 6) return 0;
        g_gxWhere = GW_DECCP;
        {  // matrix indices (0x30, 0x40) and array bases / strides (0xA0-0xBF) change with nearly every draw: not versioned
            u32 v = rd32(p + 2), r = rd8(p + 1);
            GXCENSUS(gxcensus_reg(1, r, gx->cp[r], v));
            if (gx->cp[r] != v) {
                merge_break(s, gx);
                if ((r & 0xF0) != 0x30 && (r & 0xF0) != 0x40 && (r < 0xA0 || r > 0xBF)) gx->cpVersion++;
            }
            gx->cp[r] = v;
        }
        g_gxWhere = GW_FIFO;
        return 6;
    case 0x10: {
        if (avail < 5) return 0;
        u32 h = rd32(p + 1), n = (h >> 16) + 1, addr = h & 0xFFFF;
        if (avail < 5 + 4 * n) return 0;
        g_gxWhere = GW_DECXF;
        for (u32 i = 0; i < n; i++) xf_write(s, gx, addr + i, rd32(p + 5 + 4 * i));
        g_gxWhere = GW_FIFO;
        return 5 + 4 * n;
    }
    case 0x20: case 0x28: case 0x30: case 0x38:
        if (avail < 5) return 0;
        g_gxWhere = GW_DECIDX;
        load_indexed(s, gx, (op - 0x20) >> 3, rd32(p + 1));
        g_gxWhere = GW_FIFO;
        return 5;
    case 0x40: {
        if (avail < 9) return 0;
        u32 addr = rd32(p + 1) & 0x03FFFFE0, size = rd32(p + 5);
        if (inList) emu_log(LOG_GX, "nested display list call ignored\n");
        else if (size <= PA_MEM1_END && addr <= PA_MEM1_END - size) {  // no wrap
            g_gxWhere = GW_DECNOTE;
            dl_note(gx, addr, s->mem1 + addr, size);
            g_gxWhere = GW_FIFO;
            GXCENSUS(gxcensus_list(gx, s->mem1 + addr, size));
            run_list(s, gx, s->mem1 + addr, size, true);
            GXCENSUS(gxcensus_list_end());
        }
        else emu_log(LOG_GX, "display list %08x+%x out of range\n", addr, size);
        return 9;
    }
    case 0x61:
        if (avail < 5) return 0;
#ifdef GC3DS_NATIVE
        {
            u32 reg = rd8(p + 1), v = rd32(p + 1) & 0xFFFFFF;
            // Aurora's GXCopyTex sends only the clear bit: the rest is GXSetTexCopyDst's (aurora_cmd). Display copies
            // (bit 14) are gc3ds's own, at the end of a frame (core/native/platform/gx_3ds.c).
            if (reg == BP_COPY_EXEC && !(v & (1u << 14))) v = (v & (1u << 11)) | gx->copyCtl;
            // Texture TLUT registers name an Aurora palette slot: its format comes from GX_AURORA_LOAD_TLUT.
            if ((reg >= 0x98 && reg <= 0x9B) || (reg >= 0xB8 && reg <= 0xBB)) v = (v & ~0xC00u) | (u32)gx->tlutFmt[v & 31] << 10;
            // Palette loads from RAM (BP 0x64 / 0x65): Aurora's address field is empty; GX_AURORA_LOAD_TLUT brings it.
            if (reg == 0x64 || reg == 0x65) return 5;
            bp_write(s, gx, reg, v);
        }
        return 5;
    case 0x50: return aurora_cmd(s, gx, p, avail, inList);
#else
        g_gxWhere = GW_DECBP;
        bp_write(s, gx, rd8(p + 1), rd32(p + 1) & 0xFFFFFF);
        g_gxWhere = GW_FIFO;
        return 5;
#endif
    }
    static u32 reported;
    if (reported++ < 20) emu_log(LOG_GX, "unknown GX opcode %02x (pc=%08x)\n", op, s->cpu.pc);
    return 1;
}

// Runs of draw commands inside display lists (could a list's draws be converted once into one GPU-resident shape?):
// [0] draws in lists, [1] runs (consecutive draws, NOPs between them allowed, one VAT), [2] runs of 2+ draws, [3]
// vertices in lists, [4] vertices in runs of 2+ draws, [5] draws of lines or points; what ended a run: [6] a CP write,
// [7] an XF write or indexed XF load, [8] a BP write, [9] another VAT, [10] anything else.
u64 g_dlRun[11];

static void run_list(System* s, GXCore* gx, const u8* p, u32 len, bool inList) {
    u32 off = 0, stop = 0;  // stop: the end of the run of draws being executed one by one
    while (off < len) {
        u8 op = (u8)rd8(p + off);
        if (op >= 0x80 && op < 0xC0 && len - off >= 3 && off >= stop) {
            g_gxWhere = GW_DECLIST;
            // A run: this draw and the draws (and NOPs) right after it in the same vertex format.
            u32 vat = op & 7, vsize = gx_vertex_size(gx, vat), end = off, draws = 0, verts = 0, cmds = 0;
            bool other = false;  // lines or points: not converted
            while (end < len) {
                u8 o = (u8)rd8(p + end);
                if (o == 0x00) { end++; cmds++; continue; }
                if (o < 0x80 || o >= 0xC0 || (o & 7) != vat || len - end < 3) break;
                u32 n = rd16(p + end + 1);
                if (3 + n * vsize > len - end) break;
                if (((o >> 3) & 7) >= 5) other = true;
                draws++; cmds++; verts += n;
                end += 3 + n * vsize;
            }
            g_dlRun[0] += draws; g_dlRun[1]++; g_dlRun[3] += verts;
            if (draws >= 2) { g_dlRun[2]++; g_dlRun[4] += verts; }
            if (other) g_dlRun[5]++;
            if (end < len) {
                u8 o = (u8)rd8(p + end);
                g_dlRun[o >= 0x80 && o < 0xC0 ? 9 : o == 0x08 ? 6 : o == 0x10 || (o >= 0x20 && o <= 0x38) ? 7 : o == 0x61 ? 8 : 10]++;
            }
            static u32 nl;
            u64 tl = draws && g_gxDiag && g_profClock && !(++nl & 15) ? g_profClock() : 0;
            bool listed = draws && inList && !other && gx->backend && gx->backend->draw_list &&
                          gx->backend->draw_list(gx->backendCtx, s, vat, p + off, end - off);
            g_gxWhere = GW_FIFO;
            if (listed) { g_gxOpCount[6]++; g_gxOpCount[7] += draws; if (tl) g_gxOpTicks[6] += (g_profClock() - tl) * 16; }
            if (listed) {
                for (u32 q = off; q < end;) {  // the statistics draw() keeps
                    if (rd8(p + q) == 0x00) { q++; continue; }
                    u32 n = rd16(p + q + 1);
                    gx->draws++; g_dlStats[1]++; gx->vertices += n; gx->primCount[(rd8(p + q) >> 3) & 7]++;
                    q += 3 + n * vsize;
                }
                gx->commands += cmds;
                off = end;
                continue;
            }
            stop = end;
        }
        g_gxWhere = GW_DECRUN;
        u32 n = exec_one(s, gx, p + off, len - off, inList);
        if (!n) { emu_log(LOG_GX, "display list ends mid-command\n"); return; }
        gx->commands++;
        off += n;
    }
}

// g_feedDirect (--feeddirect, --ab feeddirect): commands are parsed straight from the bytes fed (the GX thread's ring
// record) instead of from a copy of them, and only a command split across two feeds goes through the pending buffer,
// topped up FEED_STEP bytes at a time until it completes. The same bytes in the same order (the layout is the stream's
// own with GC_SWAPRAM off).
bool g_feedDirect;
#define FEED_STEP 128
static bool pend_append(GXCore* gx, const u8* data, u32 len) {
    if (gx->pendingLen + len > gx->pendingCap) {
        u32 cap = gx->pendingCap ? gx->pendingCap : 4096;
        while (cap < gx->pendingLen + len) cap *= 2;
        u8* p = realloc(gx->pending, cap);
        if (!p) { emu_error("GX FIFO buffer allocation failed\n"); return false; }
        gx->pending = p;
        gx->pendingCap = cap;
    }
    ram_from_be(gx->pending, gx->pendingLen, data, len);
    gx->pendingLen += len;
    return true;
}
static u32 run_cmds(System* s, GXCore* gx, const u8* p, u32 len) {
    u32 off = 0;
    for (;;) {
        u32 n = off < len ? exec_one(s, gx, p + off, len - off, false) : 0;
        if (!n) break;
        gx->commands++;
        off += n;
    }
    return off;
}
static void feed_direct(System* s, GXCore* gx, const u8* data, u32 len) {
    while (gx->pendingLen && len) {  // a command the last feed ended inside: completed from these bytes
        u32 take = len < FEED_STEP ? len : FEED_STEP;
        if (!pend_append(gx, data, take)) return;
        data += take;
        len -= take;
        u32 off = run_cmds(s, gx, gx->pending, gx->pendingLen);
        if (off) {
            memmove(gx->pending, gx->pending + off, gx->pendingLen - off);
            gx->pendingLen -= off;
        }
    }
    if (!len) return;
    u32 off = run_cmds(s, gx, data, len);
    if (off < len) pend_append(gx, data + off, len - off);
}

static void feed(System* s, GXCore* gx, const u8* data, u32 len) {
    if (g_feedDirect && !GC_SWAPRAM) { g_gxWhere = GW_FIFO; feed_direct(s, gx, data, len); return; }
    if (gx->pendingLen + len > gx->pendingCap) {
        u32 cap = gx->pendingCap ? gx->pendingCap : 4096;
        while (cap < gx->pendingLen + len) cap *= 2;
        u8* p = realloc(gx->pending, cap);
        if (!p) { emu_error("GX FIFO buffer allocation failed\n"); return; }
        gx->pending = p;
        gx->pendingCap = cap;
    }
    g_gxWhere = GW_DECFEED;
    ram_from_be(gx->pending, gx->pendingLen, data, len);  // (in guest memory's layout, as display lists are)
    g_gxWhere = GW_FIFO;
    gx->pendingLen += len;    u32 off = 0;
    for (;;) {
        u32 n = off < gx->pendingLen ? exec_one(s, gx, gx->pending + off, gx->pendingLen - off, false) : 0;
        if (!n) break;
        gx->commands++;
        off += n;
    }
    if (off) {
        if (GC_SWAPRAM && (off & 3))  // a shift by a part of a word: byte by byte, to stay in the layout
            for (u32 i = 0; i < gx->pendingLen - off; i++) ram_wr8(gx->pending, i, ram_rd8(gx->pending, off + i));
        else  // (whole words: the last one's bytes lie up to its end in the swapped layout; the capacity is whole words)
            memmove(gx->pending, gx->pending + off, GC_SWAPRAM ? (gx->pendingLen - off + 3) & ~3u : gx->pendingLen - off);
        gx->pendingLen -= off;
    }
}

void gxcore_feed(System* s, GXCore* gx, const u8* data, u32 len) {
    u64 t0 = prof_begin();
#ifdef GX_CENSUS_PC
    if (g_gxcensus) {
        gxcensus_pause();
        gxcensus_timer_enter(-2, false); // exclusive ingress: append/memmove/dispatch, not child commands
        feed(s, gx, data, len);
        gxcensus_timer_leave(len);
        gxcensus_resume();
    } else
#endif
    feed(s, gx, data, len);
    prof_end(PROF_GX, t0);
}
