// FIFO command processing. Commands arrive through the write-gather pipe (and display lists in RAM) and are executed
// as soon as they are complete, which keeps PE interrupts and EFB copies in step with the CPU without a GPU thread.
#include <stdio.h>
#include <stdlib.h>
#include "../sys/system.h"
#include "gx.h"

static u32 rd16(const u8* p) { return ((u32)p[0] << 8) | p[1]; }
static u32 rd32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

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
static void xf_write(GXCore* gx, u32 addr, u32 v) {
    if (addr >= sizeof gx->xf / 4) return;
    // Lights, channel controls and texgen settings: what shader lighting depends on (GXCore.xfLightVersion).
    if (((addr >= 0x600 && addr < 0x680) || (addr >= 0x1009 && addr <= 0x1011) || (addr >= 0x103F && addr <= 0x1047)) &&
        gx->xf[addr] != v)
        gx->xfLightVersion++;
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
            if (at + bytes > PA_MEM1_END) return;
            u8* o = s->mem1 + at;
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
        }
    }
}

// EFB -> XFB copy (YUYV) or EFB -> texture, optionally clearing the source rectangle afterwards.
static void efb_copy(System* s, GXCore* gx, u32 ctl) {
    u32 tl = gx->bp[BP_EFB_TL], wh = gx->bp[BP_EFB_WH];
    u32 x0 = tl & 0x3FF, y0 = (tl >> 10) & 0x3FF, w = (wh & 0x3FF) + 1, h = ((wh >> 10) & 0x3FF) + 1;
    u32 dest = (gx->bp[BP_COPY_DEST] & 0xFFFFFF) << 5, stride = (gx->bp[BP_COPY_STRIDE] & 0x3FF) << 5;
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
            if (row + w * 2 > PA_MEM1_END) break;
            for (u32 x = 0; x + 1 < w; x += 2) {
                u32 c[2] = { gx->efb[(y0 + y) * EFB_W + x0 + x], gx->efb[(y0 + y) * EFB_W + x0 + x + 1] };
                int yy[2], u = 0, v = 0;
                for (int k = 0; k < 2; k++) {
                    int r = (c[k] >> 24) & 0xFF, g = (c[k] >> 16) & 0xFF, b = (c[k] >> 8) & 0xFF;
                    yy[k] = (66 * r + 129 * g + 25 * b + 128) / 256 + 16;
                    u += (-38 * r - 74 * g + 112 * b + 128) / 256 + 128;
                    v += (112 * r - 94 * g - 18 * b + 128) / 256 + 128;
                }
                u8* o = s->mem1 + row + x * 2;
                o[0] = clamp8(yy[0]); o[1] = clamp8(u / 2); o[2] = clamp8(yy[1]); o[3] = clamp8(v / 2);
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

static void bp_write(System* s, GXCore* gx, u32 reg, u32 val) {
    u32 mask = gx->bpMask;
    if (reg != BP_MASK) gx->bpMask = 0xFFFFFF;
    gx->bp[reg] = (gx->bp[reg] & ~mask) | (val & mask);
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
        if (ram + bytes <= PA_MEM1_END && tmemAddr + bytes <= (1u << 20)) memcpy(gx->tmem + tmemAddr, s->mem1 + ram, bytes);
        gx->tlutVersion++;
        break;
    }
    case BP_PE_DONE: pe_finish(s); break;
    case BP_PE_TOKEN: pe_token(s, val & 0xFFFF, false); break;
    case BP_PE_TOKEN_INT: pe_token(s, val & 0xFFFF, true); break;
    case BP_COPY_EXEC:
        if (s->on_efb_copy) s->on_efb_copy(s, s->user, val);
        efb_copy(s, gx, val);
        break;
    }
}

// LOAD_INDX_A..D: copy (size) words from an indexed array element in RAM into XF memory.
static void load_indexed(System* s, GXCore* gx, u32 which, u32 v) {
    u32 index = v >> 16, size = ((v >> 12) & 0xF) + 1, addr = v & 0xFFF;
    u32 base = gx->cp[CP_ARRAY_BASE + 12 + which] & 0x03FFFFFF, stride = gx->cp[CP_ARRAY_STRIDE + 12 + which] & 0xFFF;
    u32 pa = base + index * stride;
    for (u32 i = 0; i < size && pa + 4 * i + 4 <= PA_MEM1_END; i++) xf_write(gx, addr + i, mem1_read32(s, pa + 4 * i));
}

static void run_list(System* s, GXCore* gx, const u8* p, u32 len, bool inList);

// Executes one command at p; returns its length, or 0 if more bytes are needed.
static u32 exec_one(System* s, GXCore* gx, const u8* p, u32 avail, bool inList) {
    u8 op = p[0];
    if (op >= 0x80 && op < 0xC0) {
        if (avail < 3) return 0;
        u32 count = rd16(p + 1), vat = op & 7, vsize = gx_vertex_size(gx, vat);
        u32 total = 3 + count * vsize;
        if (avail < total) return 0;
        gx->draws++;
        gx->vertices += count;
        gx->primCount[(op >> 3) & 7]++;
        if (gx->backend && count) gx->backend->draw(gx->backendCtx, s, (op >> 3) & 7, vat, p + 3, count, vsize);
        return total;
    }
    switch (op) {
    case 0x00: return 1;              // NOP
    case 0x48: return 1;              // invalidate vertex cache
    case 0x44: return 1;              // unknown metrics command, no payload
    case 0x08:
        if (avail < 6) return 0;
        gx->cp[p[1]] = rd32(p + 2);
        return 6;
    case 0x10: {
        if (avail < 5) return 0;
        u32 h = rd32(p + 1), n = (h >> 16) + 1, addr = h & 0xFFFF;
        if (avail < 5 + 4 * n) return 0;
        for (u32 i = 0; i < n; i++) xf_write(gx, addr + i, rd32(p + 5 + 4 * i));
        return 5 + 4 * n;
    }
    case 0x20: case 0x28: case 0x30: case 0x38:
        if (avail < 5) return 0;
        load_indexed(s, gx, (op - 0x20) >> 3, rd32(p + 1));
        return 5;
    case 0x40: {
        if (avail < 9) return 0;
        u32 addr = rd32(p + 1) & 0x03FFFFE0, size = rd32(p + 5);
        if (inList) emu_log(LOG_GX, "nested display list call ignored\n");
        else if (addr + size <= PA_MEM1_END) run_list(s, gx, s->mem1 + addr, size, true);
        else emu_log(LOG_GX, "display list %08x+%x out of range\n", addr, size);
        return 9;
    }
    case 0x61:
        if (avail < 5) return 0;
        bp_write(s, gx, p[1], rd32(p + 1) & 0xFFFFFF);
        return 5;
    }
    static u32 reported;
    if (reported++ < 20) emu_log(LOG_GX, "unknown GX opcode %02x (pc=%08x)\n", op, s->cpu.pc);
    return 1;
}

static void run_list(System* s, GXCore* gx, const u8* p, u32 len, bool inList) {
    u32 off = 0;
    while (off < len) {
        u32 n = exec_one(s, gx, p + off, len - off, inList);
        if (!n) { emu_log(LOG_GX, "display list ends mid-command\n"); return; }
        gx->commands++;
        off += n;
    }
}

static void feed(System* s, GXCore* gx, const u8* data, u32 len) {
    if (gx->pendingLen + len > gx->pendingCap) {
        u32 cap = gx->pendingCap ? gx->pendingCap : 4096;
        while (cap < gx->pendingLen + len) cap *= 2;
        u8* p = realloc(gx->pending, cap);
        if (!p) { emu_error("GX FIFO buffer allocation failed\n"); return; }
        gx->pending = p;
        gx->pendingCap = cap;
    }
    memcpy(gx->pending + gx->pendingLen, data, len);
    gx->pendingLen += len;
    u32 off = 0;
    for (;;) {
        u32 n = off < gx->pendingLen ? exec_one(s, gx, gx->pending + off, gx->pendingLen - off, false) : 0;
        if (!n) break;
        gx->commands++;
        off += n;
    }
    if (off) {
        memmove(gx->pending, gx->pending + off, gx->pendingLen - off);
        gx->pendingLen -= off;
    }
}

void gxcore_feed(System* s, GXCore* gx, const u8* data, u32 len) {
    u64 t0 = prof_begin();
    feed(s, gx, data, len);
    prof_end(PROF_GX, t0);
}
