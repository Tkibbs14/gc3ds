// GX vertices straight to PICA200 vertex-shader inputs, for the 3DS backend. The reference path (gx_decode_vertex,
// gx_transform, gx_to_screen, then the PICA fold) cost about 7.5 us per vertex on the ARM11, and Twilight Princess's
// title scene sends about 22,000 vertices per field. This path does the same work with what is fixed for a draw
// worked out once: the decode layout, position matrix x projection x viewport x PICA fold composed into one 4x4 per
// matrix index, and the light parameters. Per vertex it reads only the attributes the draw uses and lights only the
// colour channel the PICA receives. Checked against the reference on the PC with VFAST_CHECK (soft.c).
#include <math.h>
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
    }
}

// Components k0..k1-1 of a lit channel, as xform.c's light_channel.
static void half_run(const VHalf* h, const float vc[4], const float eye[3], const float n[3], int k0, int k1, float out[4]) {
    const float* mat = h->matVtx ? vc : h->mat;
    if (!h->lit) { for (int k = k0; k < k1; k++) out[k] = mat[k]; return; }
    const float* amb = h->ambVtx ? vc : h->amb;
    float lit[4];
    for (int k = k0; k < k1; k++) lit[k] = amb[k];
    for (u32 i = 0; i < h->n; i++) {
        const VLight* l = &h->l[i];
        float L[3] = { l->lp[0] - eye[0], l->lp[1] - eye[1], l->lp[2] - eye[2] };
        float dist2 = L[0] * L[0] + L[1] * L[1] + L[2] * L[2], dist = sqrtf(dist2);
        if (dist > 0) { L[0] /= dist; L[1] /= dist; L[2] /= dist; }
        float dot = n[0] * L[0] + n[1] * L[1] + n[2] * L[2];
        float diff = h->diffFn == 0 ? 1.0f : h->diffFn == 1 ? dot : (dot < 0 ? 0 : dot);
        float att = 1.0f;
        if (h->atten) {
            if (h->spot) {
                float cs = -(L[0] * l->ld[0] + L[1] * l->ld[1] + L[2] * l->ld[2]);
                float aa = l->a[0] + l->a[1] * cs + l->a[2] * cs * cs;
                float dd = l->k[0] + l->k[1] * dist + l->k[2] * dist2;
                att = (aa < 0 ? 0 : aa) / (dd != 0 ? dd : 1);
            } else {
                float hh = n[0] * l->ld[0] + n[1] * l->ld[1] + n[2] * l->ld[2];
                hh = hh < 0 ? 0 : hh;
                float aa = l->a[0] + l->a[1] * hh + l->a[2] * hh * hh, dd = l->k[0] + l->k[1] * hh + l->k[2] * hh * hh;
                att = (aa < 0 ? 0 : aa) / (dd != 0 ? dd : 1);
            }
        }
        float da = diff * att;
        for (int k = k0; k < k1; k++) lit[k] += l->lc[k] * da;
    }
    for (int k = k0; k < k1; k++) {
        float v = lit[k] < 0 ? 0 : lit[k] > 255 ? 255 : lit[k];
        out[k] = mat[k] * v / 255.0f;
    }
}

// ---- position matrices ----
// Position matrix `pnmtx` x projection x viewport x PICA fold, as one 4x4 (row-major, over model x, y, z, 1).
// Composed in double; per-vertex work is then 16 multiply-adds.
static void compose(const GXCore* gx, const VFold* f, u32 pnmtx, float out[16]) {
    double M[3][4];
    u32 pm = pnmtx * 4;
    for (int r = 0; r < 3; r++) for (int k = 0; k < 4; k++) M[r][k] = xf_f(gx, pm + 4 * r + k);
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
        for (int k = 0; k < 4; k++)
            out[4 * r + k] = (float)(P[r][0] * M[0][k] + P[r][1] * M[1][k] + P[r][2] * M[2][k] + (k == 3 ? P[r][3] : 0.0));
}

// ---- the draw ----
VFastStats g_vfastStats;
static float g_clip[256][16];
static u32 g_clipGen[256], g_gen;

void vfast_run(System* s, const GXCore* gx, u32 vat, const VFold* f, const VSel* sel, const u8* raw, u32 count,
               u32 vsize, PVert* out) {
    const u8* mem1 = s->mem1;
    u32 lo = gx->cp[CP_VCD_LO], hi = gx->cp[CP_VCD_HI];
    u32 a = gx->cp[CP_VAT_A + vat], b = gx->cp[CP_VAT_B + vat], c = gx->cp[CP_VAT_C + vat];
    u32 ma = gx->cp[CP_MATINDEX_A], mb = gx->cp[CP_MATINDEX_B];
    if (++g_gen == 0) { memset(g_clipGen, 0, sizeof g_clipGen); g_gen = 1; }

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

    for (u32 vi = 0; vi < count; vi++) {
        const u8* p = raw + vi * vsize;
        PVert* o = &out[vi];
        u32 pn = pnDefault;
        if (pnDirect) pn = *p++;
        u8 tm[8];
        memcpy(tm, tmDefault, 8);
        for (u32 i = 0; i < 8; i++) if ((tmDirect >> i) & 1) tm[i] = *p++;

        float vp[3] = { 0, 0, 0 };
        if (pos.mode) {
            const u8* src = fetch(mem1, &pos, &p, pos.n * pos.cs);
            for (u32 k = 0; k < pos.n; k++) vp[k] = read_comp(src + k * pos.cs, pos.fmt, pos.scale);
        }
        float vn[3] = { 0, 0, 0 };
        if (nrm.mode) {
            const u8* src;
            if (nrm.mode != 1 && nbt && index3) {
                src = fetch(mem1, &nrm, &p, 0);
                fetch(mem1, &nrm, &p, 0);
                fetch(mem1, &nrm, &p, 0);
            } else {
                src = fetch(mem1, &nrm, &p, nrm.n * nrm.cs);
            }
            if (nrmUsed) for (u32 k = 0; k < 3; k++) vn[k] = read_comp(src + k * nrm.cs, nrm.fmt, nrm.scale);
        }
        u32 vcol[2] = { 0xFFFFFFFFu, 0xFFFFFFFFu };
        u32 hasColor = 0;
        for (u32 i = 0; i < 2; i++) {
            if (!col[i].mode) continue;
            const u8* src = fetch(mem1, &col[i], &p, col[i].cs);
            vcol[i] = read_color(src, col[i].fmt);
            hasColor |= 1u << i;
        }
        float vt[8][2];
        for (u32 i = 0; i < 8; i++) {
            if (!tex[i].mode) continue;
            const u8* src = fetch(mem1, &tex[i], &p, tex[i].n * tex[i].cs);
            if (!((texUsed >> i) & 1)) continue;
            vt[i][0] = read_comp(src, tex[i].fmt, tex[i].scale);
            vt[i][1] = tex[i].n == 2 ? read_comp(src + tex[i].cs, tex[i].fmt, tex[i].scale) : 0.0f;
        }

        // Position.
        float* C = g_clip[pn & 255];
        if (g_clipGen[pn & 255] != g_gen) { compose(gx, f, pn & 255, C); g_clipGen[pn & 255] = g_gen; g_vfastStats.composes++; }
        for (int r = 0; r < 4; r++) o->pos[r] = C[4 * r] * vp[0] + C[4 * r + 1] * vp[1] + C[4 * r + 2] * vp[2] + C[4 * r + 3];

        // Colour channels the outputs need.
        float eye[3] = { 0, 0, 0 }, n[3] = { 0, 0, 0 };
        if (eyeUsed) {
            u32 pm = pn * 4;
            for (int r = 0; r < 3; r++)
                eye[r] = xf_f(gx, pm + 4 * r) * vp[0] + xf_f(gx, pm + 4 * r + 1) * vp[1] + xf_f(gx, pm + 4 * r + 2) * vp[2] + xf_f(gx, pm + 4 * r + 3);
            u32 nm = 0x400 + (pn & 31) * 3;
            for (int r = 0; r < 3; r++) n[r] = xf_f(gx, nm + 3 * r) * vn[0] + xf_f(gx, nm + 3 * r + 1) * vn[1] + xf_f(gx, nm + 3 * r + 2) * vn[2];
            float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (nl > 0) { n[0] /= nl; n[1] /= nl; n[2] /= nl; }
        }
        float lc[2][4];
        for (u32 ch = 0; ch < 2; ch++) {
            if (!(chans & (1u << ch))) continue;
            float vc[4];
            unpack(ch == 1 && !(hasColor & 2) && (hasColor & 1) ? vcol[0] : vcol[ch], vc);
            if (ch >= numChans) { memcpy(lc[ch], vc, sizeof vc); continue; }
            half_run(&half[ch][0], vc, eye, n, 0, 3, lc[ch]);
            half_run(&half[ch][1], vc, eye, n, 3, 4, lc[ch]);
        }
        const float* oc = lc[sel->rasChan & 1];
        o->col[0] = c8(oc[0]); o->col[1] = c8(oc[1]); o->col[2] = c8(oc[2]); o->col[3] = c8(oc[3]);

        // Texture coordinates of the units the combiners read.
        for (u32 u = 0; u < 3; u++) {
            o->tc[u][0] = o->tc[u][1] = 0;
            if (u >= numTex) continue;
            u32 gi = sel->texCoord[u] & 7;
            if (!((genUsed >> gi) & 1)) continue;
            u32 cfg = gx->xf[0x1040 + gi];
            bool stq = (cfg >> 1) & 1;
            u32 type = (cfg >> 4) & 7, src = (cfg >> 7) & 31;
            if (type >= 2) { o->tc[u][0] = lc[0][0] / 255.0f; o->tc[u][1] = lc[0][1] / 255.0f; continue; }
            float in[4] = { 0, 0, 1, 1 };
            if (src == 0) { in[0] = vp[0]; in[1] = vp[1]; in[2] = vp[2]; }
            else if (src == 1) { in[0] = vn[0]; in[1] = vn[1]; in[2] = vn[2]; }
            else if (src >= 5 && src <= 12) { in[0] = vt[src - 5][0]; in[1] = vt[src - 5][1]; }
            u32 t = (u32)tm[gi] * 4;
            float st[3];
            for (int r = 0; r < (stq ? 3 : 2); r++)
                st[r] = xf_f(gx, t + 4 * r) * in[0] + xf_f(gx, t + 4 * r + 1) * in[1] + xf_f(gx, t + 4 * r + 2) * in[2] + xf_f(gx, t + 4 * r + 3) * in[3];
            float iq = stq && st[2] != 0 ? 1.0f / st[2] : 1.0f;
            o->tc[u][0] = st[0] * iq;
            o->tc[u][1] = st[1] * iq;
        }
    }
}
