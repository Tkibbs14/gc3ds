// GX vertices straight to PICA200 vertex-shader inputs, for the 3DS backend. The reference path (gx_decode_vertex,
// gx_transform, gx_to_screen, then the PICA fold) cost about 7.5 us per vertex on the ARM11, and Twilight Princess's
// title scene sends about 22,000 vertices per field. This path does the same work with what is fixed for a draw
// worked out once: the decode layout, projection x viewport x PICA fold composed into one 4x4, and the light
// parameters. Per vertex it reads only the attributes the draw uses and lights only the
// colour channel the PICA receives. Checked against the reference on the PC with VFAST_CHECK (soft.c).
// sqrtf as the bare instruction: with errno handling it carries a branch, which stops the compiler interleaving
// the lighting loop's independent vertices.
#pragma GCC optimize("no-math-errno")
#include <math.h>
#include <stdlib.h>
#include "../sys/system.h"
#include "vfast.h"

enum { CP_MATINDEX_A = 0x30, CP_MATINDEX_B = 0x40, CP_VCD_LO = 0x50, CP_VCD_HI = 0x60, CP_VAT_A = 0x70,
       CP_VAT_B = 0x80, CP_VAT_C = 0x90, CP_ARRAY_BASE = 0xA0, CP_ARRAY_STRIDE = 0xB0 };
enum { ARR_POS = 0, ARR_NRM = 1, ARR_COL0 = 2, ARR_TEX0 = 4 };

static float xf_f(const GXCore* gx, u32 a) { float f; memcpy(&f, &gx->xf[a], 4); return f; }
static u8 c8(float v) { return (u8)(v < 0 ? 0 : v > 255 ? 255 : v + 0.5f); }

void vfold_init(const GXCore* gx, float kx, float ky, bool flipY, VFold* f) {
    f->kx = kx; f->ky = ky; f->flipY = flipY;
    f->wd = xf_f(gx, 0x101A); f->ht = xf_f(gx, 0x101B); f->zr = xf_f(gx, 0x101C);
    f->xo = xf_f(gx, 0x101D) - 342.0f; f->yo = xf_f(gx, 0x101E) - 342.0f; f->fz = xf_f(gx, 0x101F);
}

void vfast_attribs(const VSel* sel, const SVert* v, PVert* o) {
    const float* c = v->col[sel->rasChan & 1];
    o->col[0] = c8(c[0]); o->col[1] = c8(c[1]); o->col[2] = c8(c[2]); o->col[3] = c8(c[3]);
    for (u32 i = 0; i < 3; i++) {
        const float* t = v->tex[sel->texCoord[i] & 7];
        float iq = i < sel->numTex && t[2] != 0 ? 1.0f / t[2] : 1.0f;
        o->tc[i][0] = t[0] * iq;
        o->tc[i][1] = t[1] * iq;
    }
}

// GX clip space -> PICA clip space. EFB position times w is linear in the clip coordinates, so the viewport, the
// EFB scale and the depth range fold in before the divide and the hardware clips correctly.
void vfast_ref(const VFold* f, const VSel* sel, const SVert* v, PVert* o) {
    float w = v->w, ex = v->x * f->wd + f->xo * w, ey = v->y * f->ht + f->yo * w;
    o->pos[0] = ex * f->kx - w;
    o->pos[1] = f->flipY ? w - ey * f->ky : ey * f->ky - w;
    o->pos[2] = -(v->z * f->zr + f->fz * w) * (1.0f / 16777216.0f);
    o->pos[3] = w;
    vfast_attribs(sel, v, o);
}

// ---- decoding ----
typedef struct { u8 mode, fmt, n, cs; float scale; u32 base, stride; } VAttr;  // mode: 0 absent, 1 direct, 2/3 index8/16

static const u8 kZero[64];

static u32 be16(const u8* p) { return ((u32)p[0] << 8) | p[1]; }
static u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static u32 comp_size(u32 fmt) { return fmt <= 1 ? 1 : fmt <= 3 ? 2 : 4; }
static u32 color_size(u32 fmt) { static const u8 s[8] = { 2, 3, 4, 2, 3, 4, 4, 4 }; return s[fmt & 7]; }

static inline float read_comp(const u8* p, u32 fmt, float scale) {
    switch (fmt) {
    case 0: return p[0] * scale;
    case 1: return (s8)p[0] * scale;
    case 2: return be16(p) * scale;
    case 3: return (s16)be16(p) * scale;
    default: { u32 v = be32(p); float f; memcpy(&f, &v, 4); return f; }
    }
}

static u32 read_color(const u8* p, u32 fmt) {
    switch (fmt) {
    case 0: { u32 c = be16(p); u32 r = (c >> 11) & 31, g = (c >> 5) & 63, b = c & 31;
              return ((r * 255 / 31) << 24) | ((g * 255 / 63) << 16) | ((b * 255 / 31) << 8) | 0xFF; }
    case 1: case 2: return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | 0xFF;
    case 3: { u32 c = be16(p);
              return (((c >> 12) & 15) * 17u << 24) | (((c >> 8) & 15) * 17u << 16) | (((c >> 4) & 15) * 17u << 8) | ((c & 15) * 17u); }
    case 4: { u32 c = ((u32)p[0] << 16) | ((u32)p[1] << 8) | p[2];
              u32 r = (c >> 18) & 63, g = (c >> 12) & 63, b = (c >> 6) & 63, a = c & 63;
              return ((r * 255 / 63) << 24) | ((g * 255 / 63) << 16) | ((b * 255 / 63) << 8) | (a * 255 / 63); }
    default: return be32(p);
    }
}

static void attr_init(const GXCore* gx, VAttr* a, u32 mode, u32 arr, u32 fmt, u32 n, u32 cs, float scale) {
    a->mode = (u8)mode; a->fmt = (u8)fmt; a->n = (u8)n; a->cs = (u8)cs; a->scale = scale;
    a->base = gx->cp[CP_ARRAY_BASE + arr] & 0x03FFFFFF;
    a->stride = gx->cp[CP_ARRAY_STRIDE + arr] & 0xFF;
}

// Source of one attribute (stream or array element); advances the stream past it. `direct`: bytes in the stream.
static inline const u8* fetch(const u8* mem1, const VAttr* a, const u8** p, u32 direct) {
    if (a->mode == 1) { const u8* q = *p; *p += direct; return q; }
    u32 idx = a->mode == 2 ? (*p)[0] : be16(*p);
    *p += a->mode == 2 ? 1 : 2;
    u32 pa = a->base + idx * a->stride;
    return pa + 64 <= PA_MEM1_END ? mem1 + pa : kZero;
}

// ---- lighting ----
typedef struct { float lc[4], lp[3], ld[3], a[3], k[3]; } VLight;
typedef struct {            // one half (colour or alpha) of one channel
    bool lit, matVtx, ambVtx, atten, spot;
    u8 diffFn, n;
    float mat[4], amb[4];
    VLight l[8];
} VHalf;

static void unpack(u32 c, float o[4]) { o[0] = (float)(c >> 24); o[1] = (float)((c >> 16) & 255); o[2] = (float)((c >> 8) & 255); o[3] = (float)(c & 255); }

static void half_init(const GXCore* gx, u32 ch, u32 ctrl, VHalf* h) {
    unpack(gx->xf[0x100C + ch], h->mat);
    unpack(gx->xf[0x100A + ch], h->amb);
    h->matVtx = ctrl & 1;
    h->ambVtx = (ctrl >> 6) & 1;
    h->lit = (ctrl >> 1) & 1;
    h->diffFn = (u8)((ctrl >> 7) & 3);
    h->atten = (ctrl >> 9) & 1;
    h->spot = (ctrl >> 10) & 1;
    h->n = 0;
    if (!h->lit) return;
    u32 mask = ((ctrl >> 2) & 15) | (((ctrl >> 11) & 15) << 4);
    for (u32 i = 0; i < 8; i++) {
        if (!(mask & (1u << i))) continue;
        u32 base = 0x600 + 16 * i;
        VLight* l = &h->l[h->n++];
        unpack(gx->xf[base + 3], l->lc);
        for (int k = 0; k < 3; k++) {
            l->a[k] = xf_f(gx, base + 4 + k); l->k[k] = xf_f(gx, base + 7 + k);
            l->lp[k] = xf_f(gx, base + 10 + k); l->ld[k] = xf_f(gx, base + 13 + k);
        }
        // The reference divides by 1 where the attenuation denominator is 0, which happens when all three distance
        // coefficients are 0; doing that here keeps the per-vertex loop free of a float compare (a pipeline stall).
        if (l->k[0] == 0 && l->k[1] == 0 && l->k[2] == 0) l->k[0] = 1;
    }
}

// Lighting runs light by light over all the draw's vertices, four vertices at a time with branch-free bodies
// specialised per light mode: the VFP11 needs 8 cycles before a result can be used (19 for divide and square root)
// but starts a new add or multiply every cycle, so one vertex's dependent chain mostly waits, and four independent
// chains fill the gaps. It follows xform.c's light_channel; reciprocals replace divides (within float rounding).
typedef struct { float eye[3], n[3], vc[2][4], lc[2][4], lit[4]; } VLit;  // per-vertex lighting state
static VLit* g_lit;
static u32 g_litCap;

#define VF_INLINE static inline __attribute__((always_inline))
VF_INLINE float pos0(float x) { return 0.5f * (x + fabsf(x)); }  // max(x, 0), exact and without a branch

// One light on one vertex, adding lc * diffuse * attenuation to lit[K0..K1). D: diffuse function (0 one, 1 signed,
// 2 clamped); A: attenuation (0 none, 1 spot: angle and distance, 2 specular).
VF_INLINE void light_one(const VLight* l, int D, int A, int K0, int K1, VLit* v) {
    float da = 1.0f;
    if (D || A) {
        float L0 = l->lp[0] - v->eye[0], L1 = l->lp[1] - v->eye[1], L2 = l->lp[2] - v->eye[2];
        float dist2 = L0 * L0 + L1 * L1 + L2 * L2, dist = sqrtf(dist2);
        float inv = 1.0f / (dist + 1e-30f);  // a light at the vertex leaves L at zero, as the reference does
        L0 *= inv; L1 *= inv; L2 *= inv;
        float diff = 1.0f;
        if (D) { float dot = v->n[0] * L0 + v->n[1] * L1 + v->n[2] * L2; diff = D == 1 ? dot : pos0(dot); }
        float att = 1.0f;
        if (A == 1) {
            float cs = -(L0 * l->ld[0] + L1 * l->ld[1] + L2 * l->ld[2]);
            float aa = l->a[0] + l->a[1] * cs + l->a[2] * cs * cs, dd = l->k[0] + l->k[1] * dist + l->k[2] * dist2;
            att = pos0(aa) / dd;
        } else if (A == 2) {
            float hh = pos0(v->n[0] * l->ld[0] + v->n[1] * l->ld[1] + v->n[2] * l->ld[2]);
            float aa = l->a[0] + l->a[1] * hh + l->a[2] * hh * hh, dd = l->k[0] + l->k[1] * hh + l->k[2] * hh * hh;
            att = pos0(aa) / dd;
        }
        da = diff * att;
    }
    // Unrolled so that the four vertices of LIGHT_LOOP stay one branch-free block the compiler can interleave.
#pragma GCC unroll 4
    for (int k = K0; k < K1; k++) v->lit[k] += l->lc[k] * da;
}

#define LIGHT_LOOP(D, A, K0, K1) do { \
    u32 i = 0; \
    for (; i + 4 <= count; i += 4) { \
        light_one(&l, D, A, K0, K1, &v[i]); light_one(&l, D, A, K0, K1, &v[i + 1]); \
        light_one(&l, D, A, K0, K1, &v[i + 2]); light_one(&l, D, A, K0, K1, &v[i + 3]); \
    } \
    for (; i < count; i++) light_one(&l, D, A, K0, K1, &v[i]); \
} while (0)

#define LIGHT_MODES(K0, K1) switch (mode) { \
    case 0: LIGHT_LOOP(0, 0, K0, K1); break; case 1: LIGHT_LOOP(0, 1, K0, K1); break; \
    case 2: LIGHT_LOOP(0, 2, K0, K1); break; case 3: LIGHT_LOOP(1, 0, K0, K1); break; \
    case 4: LIGHT_LOOP(1, 1, K0, K1); break; case 5: LIGHT_LOOP(1, 2, K0, K1); break; \
    case 6: LIGHT_LOOP(2, 0, K0, K1); break; case 7: LIGHT_LOOP(2, 1, K0, K1); break; \
    default: LIGHT_LOOP(2, 2, K0, K1); break; }

static void light_rgb(const VLight* lp, int mode, VLit* v, u32 count) { const VLight l = *lp; LIGHT_MODES(0, 3) }
static void light_alpha(const VLight* lp, int mode, VLit* v, u32 count) { const VLight l = *lp; LIGHT_MODES(3, 4) }

// One half (colour: components 0-2, or alpha: 3) of channel `ch` for every vertex, into v[i].lc[ch].
static void light_half(const VHalf* h, u32 ch, bool alpha, VLit* v, u32 count) {
    int k0 = alpha ? 3 : 0, k1 = alpha ? 4 : 3;
    for (u32 i = 0; i < count; i++) {
        const float* mat = h->matVtx ? v[i].vc[ch] : h->mat;
        const float* amb = h->ambVtx ? v[i].vc[ch] : h->amb;
        for (int k = k0; k < k1; k++) { if (h->lit) v[i].lit[k] = amb[k]; else v[i].lc[ch][k] = mat[k]; }
    }
    if (!h->lit) return;
    int mode = 3 * (h->diffFn >= 2 ? 2 : h->diffFn) + (!h->atten ? 0 : h->spot ? 1 : 2);
    for (u32 li = 0; li < h->n; li++) {
        if (alpha) light_alpha(&h->l[li], mode, v, count);
        else light_rgb(&h->l[li], mode, v, count);
    }
    for (u32 i = 0; i < count; i++) {
        const float* mat = h->matVtx ? v[i].vc[ch] : h->mat;
        for (int k = k0; k < k1; k++) {
            float x = v[i].lit[k];
            x = x < 0 ? 0 : x > 255 ? 255 : x;
            v[i].lc[ch][k] = mat[k] * x * (1.0f / 255.0f);
        }
    }
}

// ---- position matrices ----
// Projection x viewport x PICA fold as one 4x4 over eye space (x, y, z, 1), composed in double once per draw.
// Positions go model -> eye with the position matrix in float first, as the reference does: folding the position
// matrix in as well put a large translation into float coefficients and lost up to 8x more precision on Twilight
// Princess's far geometry.
static void proj_fold(const GXCore* gx, const VFold* f, float out[16]) {
    double a = xf_f(gx, 0x1020), b = xf_f(gx, 0x1021), c = xf_f(gx, 0x1022), d = xf_f(gx, 0x1023);
    double e = xf_f(gx, 0x1024), ff = xf_f(gx, 0x1025);
    double X[4] = { a, 0, 0, b }, Y[4] = { 0, c, 0, d }, Z[4] = { 0, 0, e, ff }, W[4] = { 0, 0, 0, 1 };
    if (gx->xf[0x1026] == 0) {  // perspective: x = a px + b pz, y = c py + d pz, w = -pz
        X[2] = b; X[3] = 0; Y[2] = d; Y[3] = 0; W[2] = -1; W[3] = 0;
    }
    double P[4][4];
    for (int k = 0; k < 4; k++) {
        double ey = (double)f->ht * Y[k] + (double)f->yo * W[k];
        P[0][k] = (double)f->kx * ((double)f->wd * X[k] + (double)f->xo * W[k]) - W[k];
        P[1][k] = f->flipY ? W[k] - (double)f->ky * ey : (double)f->ky * ey - W[k];
        P[2][k] = -(Z[k] * (double)f->zr + (double)f->fz * W[k]) / 16777216.0;
        P[3][k] = W[k];
    }
    for (int r = 0; r < 4; r++)
        for (int k = 0; k < 4; k++) out[4 * r + k] = (float)P[r][k];
}

// ---- the draw ----
VFastStats g_vfastStats;
static float g_clip[256][12];  // position matrices used by the current draw, by index (3x4, row-major)

// One gathered vertex: the attributes the draw uses, decoded, plus channel 0's lit colour for colour texgen.
typedef struct { float p[3], n[3], t[8][2], c0[2]; u32 col[2]; u8 pn, hasColor, tm[8]; } VIn;
static VIn* g_in;
static u32 g_inCap;

// One generated texture coordinate (xform.c's regular texgen): texture matrix in->tm[gi] times the input, divided by
// q when stq. kind: TG_XYZ three components at byte offset `off` of the vertex, TG_XY two, TG_NONE (0, 0, 1); w = 1.
enum { TG_XYZ, TG_XY, TG_NONE };
VF_INLINE void texgen_one(const GXCore* gx, const VIn* in, u32 gi, int kind, u32 off, bool stq, float tc[2]) {
    const float* a = (const float*)((const u8*)in + off);
    float x = 0, y = 0, z = 1;
    if (kind == TG_XYZ) { x = a[0]; y = a[1]; z = a[2]; }
    else if (kind == TG_XY) { x = a[0]; y = a[1]; }
    float m[12];
    memcpy(m, &gx->xf[(u32)in->tm[gi] * 4], (stq ? 12u : 8u) * 4);
    float s = m[0] * x + m[1] * y + m[2] * z + m[3], t = m[4] * x + m[5] * y + m[6] * z + m[7];
    if (stq) {
        float q = m[8] * x + m[9] * y + m[10] * z + m[11], iq = q != 0 ? 1.0f / q : 1.0f;
        s *= iq;
        t *= iq;
    }
    tc[0] = s;
    tc[1] = t;
}

#define TG_LOOP(KIND, STQ) do { \
    _Pragma("GCC unroll 4") \
    for (u32 vi = 0; vi < count; vi++) texgen_one(gx, &g_in[vi], gi, KIND, off, STQ, out[vi].tc[u]); \
} while (0)
static u32 g_clipGen[256], g_gen;

void vfast_run(System* s, const GXCore* gx, u32 vat, const VFold* f, const VSel* sel, const u8* raw, u32 count,
               u32 vsize, PVert* out) {
    const u8* mem1 = s->mem1;
    u32 lo = gx->cp[CP_VCD_LO], hi = gx->cp[CP_VCD_HI];
    u32 a = gx->cp[CP_VAT_A + vat], b = gx->cp[CP_VAT_B + vat], c = gx->cp[CP_VAT_C + vat];
    u32 ma = gx->cp[CP_MATINDEX_A], mb = gx->cp[CP_MATINDEX_B];
    if (++g_gen == 0) { memset(g_clipGen, 0, sizeof g_clipGen); g_gen = 1; }
    float P[16];
    proj_fold(gx, f, P);

    // What the delivered outputs need.
    u32 numChans = gx->xf[0x1009] & 3, numGens = gx->xf[0x103F] & 15;
    if (numGens > 8) numGens = 8;
    u32 numTex = sel->numTex > 3 ? 3 : sel->numTex;
    u32 chans = 1u << (sel->rasChan & 1);
    u32 genUsed = 0, texUsed = 0;
    bool nrmUsed = false, eyeUsed = false;
    for (u32 u = 0; u < numTex; u++) {
        u32 gi = sel->texCoord[u] & 7;
        if (gi >= numGens) continue;
        genUsed |= 1u << gi;
        u32 cfg = gx->xf[0x1040 + gi], type = (cfg >> 4) & 7, src = (cfg >> 7) & 31;
        if (type >= 2) chans |= 1;
        else if (src == 1) nrmUsed = true;
        else if (src >= 5 && src <= 12) texUsed |= 1u << (src - 5);
    }
    VHalf half[2][2];
    for (u32 ch = 0; ch < 2; ch++) {
        if (!(chans & (1u << ch)) || ch >= numChans) continue;
        half_init(gx, ch, gx->xf[0x100E + ch], &half[ch][0]);
        half_init(gx, ch, gx->xf[0x1010 + ch], &half[ch][1]);
        if (half[ch][0].n || half[ch][1].n) nrmUsed = eyeUsed = true;
    }

    // Decode layout.
    u8 tmDefault[8];
    for (int i = 0; i < 8; i++) tmDefault[i] = (u8)(i < 4 ? (ma >> (6 + 6 * i)) & 63 : (mb >> (6 * (i - 4))) & 63);
    u32 tmDirect = (lo >> 1) & 0xFF;
    VAttr pos = { 0 }, nrm = { 0 }, col[2] = { { 0 }, { 0 } }, tex[8];
    memset(tex, 0, sizeof tex);
    {
        u32 fmt = (a >> 1) & 7;
        attr_init(gx, &pos, (lo >> 9) & 3, ARR_POS, fmt, (a & 1) ? 3 : 2, comp_size(fmt),
                  fmt == 4 ? 1.0f : 1.0f / (float)(1u << ((a >> 4) & 31)));
    }
    bool nbt = (a >> 9) & 1, index3 = (a >> 31) & 1;
    {
        static const u8 shift[4] = { 7, 6, 15, 14 };
        u32 fmt = (a >> 10) & 7;
        attr_init(gx, &nrm, (lo >> 11) & 3, ARR_NRM, fmt, nbt ? 9 : 3, comp_size(fmt),
                  fmt >= 4 ? 1.0f : 1.0f / (float)(1u << shift[fmt]));
    }
    for (u32 i = 0; i < 2; i++) {
        u32 fmt = (a >> (14 + 4 * i)) & 7;
        attr_init(gx, &col[i], (lo >> (13 + 2 * i)) & 3, ARR_COL0 + i, fmt, 1, color_size(fmt), 1.0f);
    }
    {
        u32 tcnt[8] = { (a >> 21) & 1, b & 1, (b >> 9) & 1, (b >> 18) & 1, (b >> 27) & 1, (c >> 5) & 1, (c >> 14) & 1, (c >> 23) & 1 };
        u32 tfmt[8] = { (a >> 22) & 7, (b >> 1) & 7, (b >> 10) & 7, (b >> 19) & 7, (b >> 28) & 7, (c >> 6) & 7, (c >> 15) & 7, (c >> 24) & 7 };
        u32 tsh[8] = { (a >> 25) & 31, (b >> 4) & 31, (b >> 13) & 31, (b >> 22) & 31, c & 31, (c >> 9) & 31, (c >> 18) & 31, (c >> 27) & 31 };
        for (u32 i = 0; i < 8; i++)
            attr_init(gx, &tex[i], (hi >> (2 * i)) & 3, ARR_TEX0 + i, tfmt[i], tcnt[i] ? 2 : 1, comp_size(tfmt[i]),
                      tfmt[i] == 4 ? 1.0f : 1.0f / (float)(1u << tsh[i]));
    }
    u32 pnDefault = ma & 63;
    bool pnDirect = lo & 1;
    {
        VFastStats* st = &g_vfastStats;
        st->draws++;
        st->verts += count;
        u32 lights = 0, halves = 0, idx = 0, gens = 0;
        for (u32 ch = 0; ch < 2; ch++)
            if ((chans & (1u << ch)) && ch < numChans)
                for (u32 k = 0; k < 2; k++) { lights += half[ch][k].n; halves += half[ch][k].lit; }
        idx += pos.mode >= 2; idx += nrm.mode >= 2; idx += col[0].mode >= 2; idx += col[1].mode >= 2;
        for (u32 i = 0; i < 8; i++) idx += tex[i].mode >= 2;
        for (u32 i = 0; i < 8; i++) gens += (genUsed >> i) & 1;
        st->lightEvals += (u64)lights * count;
        st->litHalves += (u64)halves * count;
        st->indexed += (u64)idx * count;
        st->texgens += (u64)gens * count;
        st->nrmReads += nrmUsed ? count : 0;
    }

    // Three passes over the draw, timed separately on the 3DS profile: gather (all reads of guest memory), lighting
    // (colours), then position and texture coordinates.
    if (count > g_inCap) {
        VIn* n = realloc(g_in, count * sizeof *n);
        if (!n) return;
        g_in = n;
        g_inCap = count;
    }
    u64 t0 = prof_begin();
    for (u32 vi = 0; vi < count; vi++) {
        const u8* p = raw + vi * vsize;
        VIn* in = &g_in[vi];
        in->pn = (u8)pnDefault;
        if (pnDirect) in->pn = *p++;
        memcpy(in->tm, tmDefault, 8);
        for (u32 i = 0; i < 8; i++) if ((tmDirect >> i) & 1) in->tm[i] = *p++;
        in->p[0] = in->p[1] = in->p[2] = 0;
        if (pos.mode) {
            const u8* src = fetch(mem1, &pos, &p, pos.n * pos.cs);
            for (u32 k = 0; k < pos.n; k++) in->p[k] = read_comp(src + k * pos.cs, pos.fmt, pos.scale);
        }
        in->n[0] = in->n[1] = in->n[2] = 0;
        if (nrm.mode) {
            const u8* src;
            if (nrm.mode != 1 && nbt && index3) {
                src = fetch(mem1, &nrm, &p, 0);
                fetch(mem1, &nrm, &p, 0);
                fetch(mem1, &nrm, &p, 0);
            } else {
                src = fetch(mem1, &nrm, &p, nrm.n * nrm.cs);
            }
            if (nrmUsed) for (u32 k = 0; k < 3; k++) in->n[k] = read_comp(src + k * nrm.cs, nrm.fmt, nrm.scale);
        }
        in->col[0] = in->col[1] = 0xFFFFFFFFu;
        in->hasColor = 0;
        for (u32 i = 0; i < 2; i++) {
            if (!col[i].mode) continue;
            const u8* src = fetch(mem1, &col[i], &p, col[i].cs);
            in->col[i] = read_color(src, col[i].fmt);
            in->hasColor |= (u8)(1u << i);
        }
        for (u32 i = 0; i < 8; i++) {
            if (!tex[i].mode) continue;
            const u8* src = fetch(mem1, &tex[i], &p, tex[i].n * tex[i].cs);
            if (!((texUsed >> i) & 1)) continue;
            in->t[i][0] = read_comp(src, tex[i].fmt, tex[i].scale);
            in->t[i][1] = tex[i].n == 2 ? read_comp(src + tex[i].cs, tex[i].fmt, tex[i].scale) : 0.0f;
        }
    }
    prof_end(PROF_VDEC, t0);

    // Colour channels the outputs need (texgen from colour reads channel 0, kept in c0).
    t0 = prof_begin();
    if (count > g_litCap) {
        VLit* n = realloc(g_lit, count * sizeof *n);
        if (!n) return;
        g_lit = n;
        g_litCap = count;
    }
    VLit* lv = g_lit;
    for (u32 vi = 0; vi < count; vi++) {
        const VIn* in = &g_in[vi];
        VLit* v = &lv[vi];
        if (eyeUsed) {
            u32 pn = in->pn, pm = pn * 4, nm = 0x400 + (pn & 31) * 3;
            float* n = v->n;
            for (int r = 0; r < 3; r++)
                v->eye[r] = xf_f(gx, pm + 4 * r) * in->p[0] + xf_f(gx, pm + 4 * r + 1) * in->p[1] + xf_f(gx, pm + 4 * r + 2) * in->p[2] + xf_f(gx, pm + 4 * r + 3);
            for (int r = 0; r < 3; r++) n[r] = xf_f(gx, nm + 3 * r) * in->n[0] + xf_f(gx, nm + 3 * r + 1) * in->n[1] + xf_f(gx, nm + 3 * r + 2) * in->n[2];
            float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]), inl = nl > 0 ? 1.0f / nl : 0.0f;
            if (nl > 0) { n[0] *= inl; n[1] *= inl; n[2] *= inl; }
        }
        for (u32 ch = 0; ch < 2; ch++)
            if (chans & (1u << ch)) unpack(ch == 1 && !(in->hasColor & 2) && (in->hasColor & 1) ? in->col[0] : in->col[ch], v->vc[ch]);
    }
    for (u32 ch = 0; ch < 2; ch++) {
        if (!(chans & (1u << ch))) continue;
        if (ch >= numChans) { for (u32 vi = 0; vi < count; vi++) memcpy(lv[vi].lc[ch], lv[vi].vc[ch], sizeof lv[vi].vc[ch]); continue; }
        light_half(&half[ch][0], ch, false, lv, count);
        light_half(&half[ch][1], ch, true, lv, count);
    }
    for (u32 vi = 0; vi < count; vi++) {
        const float* oc = lv[vi].lc[sel->rasChan & 1];
        PVert* o = &out[vi];
        o->col[0] = c8(oc[0]); o->col[1] = c8(oc[1]); o->col[2] = c8(oc[2]); o->col[3] = c8(oc[3]);
        if (chans & 1) { g_in[vi].c0[0] = lv[vi].lc[0][0]; g_in[vi].c0[1] = lv[vi].lc[0][1]; }
    }
    prof_end(PROF_VLIGHT, t0);

    // Position: the matrices the draw uses are fetched first, so the vertex loop is branch-free and the compiler
    // unrolls and interleaves it (see the lighting notes).
    for (u32 vi = 0; vi < count; vi++) {
        u32 pn = g_in[vi].pn;
        if (g_clipGen[pn] != g_gen) { memcpy(g_clip[pn], &gx->xf[pn * 4], sizeof g_clip[0]); g_clipGen[pn] = g_gen; g_vfastStats.composes++; }
    }
#pragma GCC unroll 4
    for (u32 vi = 0; vi < count; vi++) {
        const VIn* in = &g_in[vi];
        const float* M = g_clip[in->pn];
        float x = in->p[0], y = in->p[1], z = in->p[2];
        float e0 = M[0] * x + M[1] * y + M[2] * z + M[3];
        float e1 = M[4] * x + M[5] * y + M[6] * z + M[7];
        float e2 = M[8] * x + M[9] * y + M[10] * z + M[11];
        float* o = out[vi].pos;
        for (int r = 0; r < 4; r++) o[r] = P[4 * r] * e0 + P[4 * r + 1] * e1 + P[4 * r + 2] * e2 + P[4 * r + 3];
    }

    // Texture coordinates, one unit at a time with its texgen settled outside the vertex loop.
    for (u32 u = 0; u < 3; u++) {
        u32 gi = sel->texCoord[u] & 7;
        if (u >= numTex || !((genUsed >> gi) & 1)) {
            for (u32 vi = 0; vi < count; vi++) out[vi].tc[u][0] = out[vi].tc[u][1] = 0;
            continue;
        }
        u32 cfg = gx->xf[0x1040 + gi], type = (cfg >> 4) & 7, src = (cfg >> 7) & 31;
        bool stq = (cfg >> 1) & 1;
        if (type >= 2) {  // colour texgen: s, t from channel 0
            for (u32 vi = 0; vi < count; vi++) { out[vi].tc[u][0] = g_in[vi].c0[0] / 255.0f; out[vi].tc[u][1] = g_in[vi].c0[1] / 255.0f; }
            continue;
        }
        // Input: three components at `off` (position, normal), two (a texture coordinate), or none (0, 0, 1, 1).
        int kind = src <= 1 ? TG_XYZ : (src >= 5 && src <= 12) ? TG_XY : TG_NONE;
        u32 off = src == 0 ? offsetof(VIn, p) : src == 1 ? offsetof(VIn, n) : kind == TG_XY ? offsetof(VIn, t) + 8 * (src - 5) : 0;
        if (stq) {
            switch (kind) {
            case TG_XYZ: TG_LOOP(TG_XYZ, true); break;
            case TG_XY: TG_LOOP(TG_XY, true); break;
            default: TG_LOOP(TG_NONE, true); break;
            }
        } else {
            switch (kind) {
            case TG_XYZ: TG_LOOP(TG_XYZ, false); break;
            case TG_XY: TG_LOOP(TG_XY, false); break;
            default: TG_LOOP(TG_NONE, false); break;
            }
        }
    }
}
