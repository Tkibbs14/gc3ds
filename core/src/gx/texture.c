// GameCube texture formats -> linear RGBA8 (0xRRGGBBAA per pixel, row-major). All formats are stored in tiles
// (block width x block height), tiles left-to-right then top-to-bottom; RGBA8 tiles hold AR pairs then GB pairs.
#include "gx.h"
#include "../ramlayout.h"

enum { TF_I4 = 0, TF_I8 = 1, TF_IA4 = 2, TF_IA8 = 3, TF_RGB565 = 4, TF_RGB5A3 = 5, TF_RGBA8 = 6, TF_C4 = 8, TF_C8 = 9,
       TF_C14X2 = 10, TF_CMPR = 14 };
enum { TLUT_IA8 = 0, TLUT_RGB565 = 1, TLUT_RGB5A3 = 2 };

static u32 rgba(u32 r, u32 g, u32 b, u32 a) { return (r << 24) | (g << 16) | (b << 8) | a; }
static u32 be16(const u8* p) { return ((u32)p[0] << 8) | p[1]; }

static u32 from565(u32 c) {
    u32 r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
    return rgba((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255);
}

static u32 from5a3(u32 c) {
    if (c & 0x8000) {
        u32 r = (c >> 10) & 31, g = (c >> 5) & 31, b = c & 31;
        return rgba((r << 3) | (r >> 2), (g << 3) | (g >> 2), (b << 3) | (b >> 2), 255);
    }
    u32 a = (c >> 12) & 7, r = (c >> 8) & 15, g = (c >> 4) & 15, b = c & 15;
    return rgba(r * 17, g * 17, b * 17, (a << 5) | (a << 2) | (a >> 1));
}

static u32 tlut_color(const u8* tlut, u32 fmt, u32 index) {
    u32 c = be16(tlut + 2 * index);
    if (fmt == TLUT_IA8) return rgba(c & 0xFF, c & 0xFF, c & 0xFF, c >> 8);
    if (fmt == TLUT_RGB565) return from565(c);
    return from5a3(c);
}

void gx_texture_block_size(u32 fmt, u32* bw, u32* bh) {
    switch (fmt) {
    case TF_I4: case TF_C4: case TF_CMPR: *bw = 8; *bh = 8; break;
    case TF_I8: case TF_IA4: case TF_C8: *bw = 8; *bh = 4; break;
    default: *bw = 4; *bh = 4; break;
    }
}

u32 gx_texture_size(u32 fmt, u32 w, u32 h) {
    u32 bw, bh;
    gx_texture_block_size(fmt, &bw, &bh);
    u32 tiles = ((w + bw - 1) / bw) * ((h + bh - 1) / bh);
    return tiles * (fmt == TF_RGBA8 ? 64 : 32);
}

static void put(u32* out, u32 w, u32 h, u32 x, u32 y, u32 c) { if (x < w && y < h) out[y * w + x] = c; }

// DXT1-style 4x4 sub-block of CMPR: two RGB565 endpoints (big-endian), then 2-bit indices, MSB first per row.
static void cmpr_sub(const u8* p, u32* out, u32 w, u32 h, u32 x0, u32 y0) {
    u32 c0 = be16(p), c1 = be16(p + 2);
    u32 pal[4];
    pal[0] = from565(c0);
    pal[1] = from565(c1);
    u32 r0 = pal[0] >> 24, g0 = (pal[0] >> 16) & 0xFF, b0 = (pal[0] >> 8) & 0xFF;
    u32 r1 = pal[1] >> 24, g1 = (pal[1] >> 16) & 0xFF, b1 = (pal[1] >> 8) & 0xFF;
    if (c0 > c1) {
        pal[2] = rgba((2 * r0 + r1) / 3, (2 * g0 + g1) / 3, (2 * b0 + b1) / 3, 255);
        pal[3] = rgba((r0 + 2 * r1) / 3, (g0 + 2 * g1) / 3, (b0 + 2 * b1) / 3, 255);
    } else {
        pal[2] = rgba((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 255);
        pal[3] = rgba((r0 + r1) / 2, (g0 + g1) / 2, (b0 + b1) / 2, 0);  // transparent
    }
    for (u32 y = 0; y < 4; y++) {
        u8 bits = p[4 + y];
        for (u32 x = 0; x < 4; x++) put(out, w, h, x0 + x, y0 + y, pal[(bits >> (6 - 2 * x)) & 3]);
    }
}

void gx_decode_texture(const u8* src, u32 fmt, u32 w, u32 h, const u8* tlut, u32 tlutFmt, u32* out) {
    u32 bw, bh;
    gx_texture_block_size(fmt, &bw, &bh);
    for (u32 ty = 0; ty < h; ty += bh) {
        for (u32 tx = 0; tx < w; tx += bw) {
            // The tile, in guest memory's layout (ramlayout.h): read through a big-endian copy when the layout is not.
            const u8* t = src;
#if GC_SWAPRAM
            u8 tb[64];
            ram_to_be(tb, src, 0, fmt == TF_RGBA8 ? 64 : 32);
            t = tb;
#endif
            switch (fmt) {
            case TF_I4: case TF_C4:
                for (u32 y = 0; y < 8; y++)
                    for (u32 x = 0; x < 8; x++) {
                        u32 v = (t[y * 4 + x / 2] >> ((x & 1) ? 0 : 4)) & 15;
                        put(out, w, h, tx + x, ty + y, fmt == TF_I4 ? rgba(v * 17, v * 17, v * 17, v * 17) : tlut_color(tlut, tlutFmt, v));
                    }
                src += 32;
                break;
            case TF_I8: case TF_C8:
                for (u32 y = 0; y < 4; y++)
                    for (u32 x = 0; x < 8; x++) {
                        u32 v = t[y * 8 + x];
                        put(out, w, h, tx + x, ty + y, fmt == TF_I8 ? rgba(v, v, v, v) : tlut_color(tlut, tlutFmt, v));
                    }
                src += 32;
                break;
            case TF_IA4:
                for (u32 y = 0; y < 4; y++)
                    for (u32 x = 0; x < 8; x++) {
                        u32 v = t[y * 8 + x], i = (v & 15) * 17, a = (v >> 4) * 17;
                        put(out, w, h, tx + x, ty + y, rgba(i, i, i, a));
                    }
                src += 32;
                break;
            case TF_IA8:
                for (u32 y = 0; y < 4; y++)
                    for (u32 x = 0; x < 4; x++) {
                        const u8* p = t + (y * 4 + x) * 2;
                        put(out, w, h, tx + x, ty + y, rgba(p[1], p[1], p[1], p[0]));
                    }
                src += 32;
                break;
            case TF_RGB565: case TF_RGB5A3:
                for (u32 y = 0; y < 4; y++)
                    for (u32 x = 0; x < 4; x++) {
                        u32 c = be16(t + (y * 4 + x) * 2);
                        put(out, w, h, tx + x, ty + y, fmt == TF_RGB565 ? from565(c) : from5a3(c));
                    }
                src += 32;
                break;
            case TF_RGBA8:
                for (u32 y = 0; y < 4; y++)
                    for (u32 x = 0; x < 4; x++) {
                        u32 i = y * 4 + x;
                        put(out, w, h, tx + x, ty + y, rgba(t[2 * i + 1], t[32 + 2 * i], t[32 + 2 * i + 1], t[2 * i]));
                    }
                src += 64;
                break;
            case TF_C14X2:
                for (u32 y = 0; y < 4; y++)
                    for (u32 x = 0; x < 4; x++)
                        put(out, w, h, tx + x, ty + y, tlut_color(tlut, tlutFmt, be16(t + (y * 4 + x) * 2) & 0x3FFF));
                src += 32;
                break;
            case TF_CMPR:
                for (u32 k = 0; k < 4; k++) cmpr_sub(t + 8 * k, out, w, h, tx + (k & 1) * 4, ty + (k >> 1) * 4);
                src += 32;
                break;
            default:
                for (u32 y = 0; y < bh; y++) for (u32 x = 0; x < bw; x++) put(out, w, h, tx + x, ty + y, rgba(255, 0, 255, 255));
                src += 32;
                break;
            }
        }
    }
}
