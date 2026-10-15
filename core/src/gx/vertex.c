// Vertex decoding: raw streamed vertices (direct or indexed attributes, big-endian, fixed point or float) into
// GXVertex. Shared by every rendering backend.
#include "../sys/system.h"
#include "gx.h"

enum { CP_MATINDEX_A = 0x30, CP_MATINDEX_B = 0x40, CP_VCD_LO = 0x50, CP_VCD_HI = 0x60, CP_VAT_A = 0x70,
       CP_VAT_B = 0x80, CP_VAT_C = 0x90, CP_ARRAY_BASE = 0xA0, CP_ARRAY_STRIDE = 0xB0 };
enum { ARR_POS = 0, ARR_NRM = 1, ARR_COL0 = 2, ARR_TEX0 = 4 };

static u32 be16(const u8* p) { return ((u32)p[0] << 8) | p[1]; }
static u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

// Formats 8-15: the same components from a little-endian array (native mode: Dusklight stores the game's vertex
// arrays in host order and says so in GX_AURORA_LOAD_ARRAYBASE; fifo.c GXCore.arrayLE). Bytes are unchanged.
static inline u32 le16(const u8* p) { return p[0] | ((u32)p[1] << 8); }
static inline u32 le32(const u8* p) { return p[0] | ((u32)p[1] << 8) | ((u32)p[2] << 16) | ((u32)p[3] << 24); }
static inline float read_comp(const u8* p, u32 fmt, float scale) {
    switch (fmt) {
    case 8: return p[0] * scale;
    case 9: return (s8)p[0] * scale;
    case 10: return le16(p) * scale;
    case 11: return (s16)le16(p) * scale;
    case 12: case 13: case 14: case 15: { u32 v = le32(p); float f; memcpy(&f, &v, 4); return f; }
    case 0: return p[0] * scale;
    case 1: return (s8)p[0] * scale;
    case 2: return be16(p) * scale;
    case 3: return (s16)be16(p) * scale;
    default: { u32 v = be32(p); float f; memcpy(&f, &v, 4); return f; }
    }
}

static u32 comp_size(u32 fmt) { return fmt <= 1 ? 1 : fmt <= 3 ? 2 : 4; }

static u32 read_color(const u8* p, u32 fmt) {
    if (fmt & 8) {  // a little-endian array: the 16- and 24-bit formats are swapped, the byte formats are not
        fmt &= 7;
        if (fmt == 0 || fmt == 3) { u8 q[2] = { p[1], p[0] }; return read_color(q, fmt); }
        if (fmt == 4) { u8 q[3] = { p[2], p[1], p[0] }; return read_color(q, fmt); }
    }
    switch (fmt) {
    case 0: { u32 c = be16(p); u32 r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;  // RGB565
              return ((r * 255 / 31) << 24) | ((g * 255 / 63) << 16) | ((b * 255 / 31) << 8) | 0xFF; }
    case 1: case 2: return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | 0xFF;  // RGB888(x)
    case 3: { u32 c = be16(p);  // RGBA4444
              return (((c >> 12) & 15) * 17u << 24) | (((c >> 8) & 15) * 17u << 16) | (((c >> 4) & 15) * 17u << 8) | ((c & 15) * 17u); }
    case 4: { u32 c = ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2];  // RGBA6666
              u32 r = (c >> 18) & 63, g = (c >> 12) & 63, b = (c >> 6) & 63, a = c & 63;
              return ((r * 255 / 63) << 24) | ((g * 255 / 63) << 16) | ((b * 255 / 63) << 8) | (a * 255 / 63); }
    default: return be32(p);  // RGBA8888
    }
}

static u32 color_size(u32 fmt) { static const u8 s[8] = { 2, 3, 4, 2, 3, 4, 4, 4 }; return s[fmt & 7]; }

// Source of one attribute: the stream itself (direct) or an array element in RAM (indexed).
static const u8* attr_src(System* s, const GXCore* gx, u32 mode, u32 arr, const u8** stream, u32 directSize) {
    if (mode == 1) { const u8* p = *stream; *stream += directSize; return p; }
    u32 index = mode == 2 ? (*stream)[0] : be16(*stream);
    *stream += mode == 2 ? 1 : 2;
    u32 pa = (gx->cp[CP_ARRAY_BASE + arr] & GX_ARRAY_MASK) + index * (gx->cp[CP_ARRAY_STRIDE + arr] & 0xFF);
    static const u8 zero[64];
    return pa >= PA_MEM1_BEGIN && pa + 64 <= PA_MEM1_END ? s->mem1 + pa : zero;
}

void gx_decode_vertex(System* s, const GXCore* gx, u32 vat, const u8* p, GXVertex* v) {
    u32 lo = gx->cp[CP_VCD_LO], hi = gx->cp[CP_VCD_HI];
    u32 a = gx->cp[CP_VAT_A + vat], b = gx->cp[CP_VAT_B + vat], c = gx->cp[CP_VAT_C + vat];
    u32 ma = gx->cp[CP_MATINDEX_A], mb = gx->cp[CP_MATINDEX_B];
    memset(v, 0, sizeof *v);
    v->pnmtx = (u8)(ma & 63);
    for (int i = 0; i < 8; i++) v->texmtx[i] = (u8)(i < 4 ? (ma >> (6 + 6 * i)) & 63 : (mb >> (6 * (i - 4))) & 63);
    if (lo & 1) v->pnmtx = *p++;
    for (int i = 0; i < 8; i++) if ((lo >> (1 + i)) & 1) v->texmtx[i] = *p++;

    u32 pos = (lo >> 9) & 3;
    if (pos) {
        u32 fmt = (a >> 1) & 7, n = (a & 1) ? 3 : 2;
        float scale = fmt == 4 ? 1.0f : 1.0f / (float)(1u << ((a >> 4) & 31));
        const u8* src = attr_src(s, gx, pos, ARR_POS, &p, n * comp_size(fmt));
        u32 rf = fmt | (pos >= 2 && (gx->arrayLE >> ARR_POS) & 1 ? 8 : 0);  // read_comp: a little-endian array
        for (u32 k = 0; k < n; k++) v->pos[k] = read_comp(src + k * comp_size(fmt), rf, scale);
    }
    u32 nrm = (lo >> 11) & 3;
    if (nrm) {
        u32 fmt = (a >> 10) & 7;
        bool nbt = (a >> 9) & 1, index3 = (a >> 31) & 1;
        static const u8 shift[5] = { 7, 6, 15, 14, 0 };
        float scale = fmt >= 4 ? 1.0f : 1.0f / (float)(1u << shift[fmt]);
        u32 vecs = nbt ? 3 : 1, cs = comp_size(fmt), rf = fmt | (nrm >= 2 && (gx->arrayLE >> ARR_NRM) & 1 ? 8 : 0);
        if (nrm != 1 && nbt && index3) {
            // Three separate indices: normal, binormal and tangent, each into the normal array.
            for (u32 k = 0; k < 3; k++) {
                const u8* src = attr_src(s, gx, nrm, ARR_NRM, &p, 0);
                for (u32 j = 0; j < 3; j++) v->nrm[k][j] = read_comp(src + (3 * k + j) * cs, rf, scale);
            }
        } else {
            const u8* src = attr_src(s, gx, nrm, ARR_NRM, &p, vecs * 3 * cs);
            for (u32 j = 0; j < vecs * 3; j++) v->nrm[j / 3][j % 3] = read_comp(src + j * cs, rf, scale);
        }
    }
    for (int i = 0; i < 2; i++) {
        u32 mode = (lo >> (13 + 2 * i)) & 3;
        u32 fmt = (a >> (14 + 4 * i)) & 7;
        if (!mode) { v->color[i] = 0xFFFFFFFFu; continue; }
        const u8* src = attr_src(s, gx, mode, ARR_COL0 + i, &p, color_size(fmt));
        v->color[i] = read_color(src, fmt | (mode >= 2 && (gx->arrayLE >> (ARR_COL0 + i)) & 1 ? 8 : 0));
        v->hasColor |= 1u << i;
    }
    u32 tcnt[8] = { (a >> 21) & 1, b & 1, (b >> 9) & 1, (b >> 18) & 1, (b >> 27) & 1, (c >> 5) & 1, (c >> 14) & 1, (c >> 23) & 1 };
    u32 tfmt[8] = { (a >> 22) & 7, (b >> 1) & 7, (b >> 10) & 7, (b >> 19) & 7, (b >> 28) & 7, (c >> 6) & 7, (c >> 15) & 7, (c >> 24) & 7 };
    u32 tsh[8] = { (a >> 25) & 31, (b >> 4) & 31, (b >> 13) & 31, (b >> 22) & 31, c & 31, (c >> 9) & 31, (c >> 18) & 31, (c >> 27) & 31 };
    for (int i = 0; i < 8; i++) {
        u32 mode = (hi >> (2 * i)) & 3;
        if (!mode) continue;
        u32 n = tcnt[i] ? 2 : 1, fmt = tfmt[i];
        float scale = fmt == 4 ? 1.0f : 1.0f / (float)(1u << tsh[i]);
        const u8* src = attr_src(s, gx, mode, ARR_TEX0 + i, &p, n * comp_size(fmt));
        u32 rf = fmt | (mode >= 2 && (gx->arrayLE >> (ARR_TEX0 + i)) & 1 ? 8 : 0);
        for (u32 k = 0; k < n; k++) v->tex[i][k] = read_comp(src + k * comp_size(fmt), rf, scale);
        v->hasTex |= 1u << i;
    }
}
