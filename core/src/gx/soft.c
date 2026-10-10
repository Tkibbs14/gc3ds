// Software reference renderer for GX draws (PC). Favours clarity over speed; the 3DS backend is checked against it.
// Covers: XF position/normal transform, projection, viewport, per-vertex lighting (diffuse, distance/angle
// attenuation), regular texgen, triangle rasterisation with perspective-correct attributes and a depth buffer,
// TEV colour/alpha combiners (all inputs, konst, swap tables, compare modes), texture sampling (nearest/bilinear,
// wrap modes, TLUTs), alpha test and blending. Not yet: fog, indirect textures, lines/points, Z textures.
#include <math.h>
#include <stdlib.h>
#include "../sys/system.h"
#include "gx.h"

static float xf_f(const GXCore* gx, u32 a) { float f; memcpy(&f, &gx->xf[a], 4); return f; }

enum { PRIM_QUADS = 0, PRIM_QUADS2 = 1, PRIM_TRIS = 2, PRIM_STRIP = 3, PRIM_FAN = 4, PRIM_LINES = 5, PRIM_LSTRIP = 6, PRIM_POINTS = 7 };

typedef struct {
    float x, y, z, w;      // clip space
    float sx, sy, sz;      // screen (after divide and viewport)
    float col[2][4];       // RGBA 0..255
    float tex[8][3];       // s, t, q
} SVert;

typedef struct { int unused; } SoftCtx;
static SoftCtx g_soft;

// ---- per-vertex lighting (one channel = colour or alpha) ----
static void unpack(u32 c, float o[4]) { o[0] = (float)(c >> 24); o[1] = (float)((c >> 16) & 255); o[2] = (float)((c >> 8) & 255); o[3] = (float)(c & 255); }

static void light_channel(const GXCore* gx, u32 ctrl, const float vtxColor[4], const float eyePos[3], const float n[3],
                          u32 ambReg, u32 matReg, float out[4], int comps0, int comps1) {
    float mat[4], amb[4];
    unpack(matReg, mat);
    unpack(ambReg, amb);
    if (ctrl & 1) memcpy(mat, vtxColor, sizeof mat);
    if (ctrl & (1 << 6)) memcpy(amb, vtxColor, sizeof amb);
    float lit[4];
    if (!(ctrl & 2)) {  // lighting off: material passes through
        for (int k = comps0; k < comps1; k++) out[k] = mat[k];
        return;
    }
    for (int k = 0; k < 4; k++) lit[k] = amb[k];
    u32 mask = ((ctrl >> 2) & 15) | (((ctrl >> 11) & 15) << 4);
    u32 diffFn = (ctrl >> 7) & 3;
    bool atten = (ctrl >> 9) & 1, spot = (ctrl >> 10) & 1;
    for (int i = 0; i < 8; i++) {
        if (!(mask & (1u << i))) continue;
        u32 base = 0x600 + 16 * i;
        float lc[4];
        unpack(gx->xf[base + 3], lc);
        float lp[3] = { xf_f(gx, base + 10), xf_f(gx, base + 11), xf_f(gx, base + 12) };
        float ld[3] = { xf_f(gx, base + 13), xf_f(gx, base + 14), xf_f(gx, base + 15) };
        float L[3] = { lp[0] - eyePos[0], lp[1] - eyePos[1], lp[2] - eyePos[2] };
        float dist2 = L[0] * L[0] + L[1] * L[1] + L[2] * L[2], dist = sqrtf(dist2);
        if (dist > 0) for (int k = 0; k < 3; k++) L[k] /= dist;
        float dot = n[0] * L[0] + n[1] * L[1] + n[2] * L[2];
        float diff = diffFn == 0 ? 1.0f : diffFn == 1 ? dot : (dot < 0 ? 0 : dot);
        float att = 1.0f;
        if (atten) {
            float a0 = xf_f(gx, base + 4), a1 = xf_f(gx, base + 5), a2 = xf_f(gx, base + 6);
            float k0 = xf_f(gx, base + 7), k1 = xf_f(gx, base + 8), k2 = xf_f(gx, base + 9);
            if (spot) {
                float cs = -(L[0] * ld[0] + L[1] * ld[1] + L[2] * ld[2]);
                float aa = a0 + a1 * cs + a2 * cs * cs;
                float dd = k0 + k1 * dist + k2 * dist2;
                att = (aa < 0 ? 0 : aa) / (dd != 0 ? dd : 1);
            } else {  // specular: angle from the half vector in the direction field
                float h = n[0] * ld[0] + n[1] * ld[1] + n[2] * ld[2];
                h = h < 0 ? 0 : h;
                float aa = a0 + a1 * h + a2 * h * h, dd = k0 + k1 * h + k2 * h * h;
                att = (aa < 0 ? 0 : aa) / (dd != 0 ? dd : 1);
            }
        }
        for (int k = 0; k < 4; k++) lit[k] += lc[k] * diff * att;
    }
    for (int k = comps0; k < comps1; k++) {
        float v = lit[k] < 0 ? 0 : lit[k] > 255 ? 255 : lit[k];
        out[k] = mat[k] * v / 255.0f;
    }
}

// ---- transform one vertex ----
static void transform(System* s, const GXCore* gx, const GXVertex* v, SVert* o) {
    u32 pm = (u32)v->pnmtx * 4;
    float p[3];
    for (int r = 0; r < 3; r++)
        p[r] = xf_f(gx, pm + 4 * r) * v->pos[0] + xf_f(gx, pm + 4 * r + 1) * v->pos[1] + xf_f(gx, pm + 4 * r + 2) * v->pos[2] +
               xf_f(gx, pm + 4 * r + 3);
    // Normal matrix: 3x3 at 0x400 + (index & 31) * 3.
    u32 nm = 0x400 + ((u32)v->pnmtx & 31) * 3;
    float n[3];
    for (int r = 0; r < 3; r++) n[r] = xf_f(gx, nm + 3 * r) * v->nrm[0][0] + xf_f(gx, nm + 3 * r + 1) * v->nrm[0][1] + xf_f(gx, nm + 3 * r + 2) * v->nrm[0][2];
    float nl = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
    if (nl > 0) for (int r = 0; r < 3; r++) n[r] /= nl;

    // Projection: XF 0x1020..0x1025 parameters, 0x1026 type (0 perspective, 1 orthographic).
    float a = xf_f(gx, 0x1020), b = xf_f(gx, 0x1021), c = xf_f(gx, 0x1022), d = xf_f(gx, 0x1023), e = xf_f(gx, 0x1024), f = xf_f(gx, 0x1025);
    if (gx->xf[0x1026] == 0) {
        o->x = a * p[0] + b * p[2]; o->y = c * p[1] + d * p[2]; o->z = e * p[2] + f; o->w = -p[2];
    } else {
        o->x = a * p[0] + b; o->y = c * p[1] + d; o->z = e * p[2] + f; o->w = 1.0f;
    }

    // Lighting: two colour channels (XF 0x100E/0x100F colour, 0x1010/0x1011 alpha).
    u32 numChans = gx->xf[0x1009] & 3;
    for (int ch = 0; ch < 2; ch++) {
        float vc[4];
        unpack(v->color[ch], vc);
        if (ch == 1 && !(v->hasColor & 2) && (v->hasColor & 1)) unpack(v->color[0], vc);
        if ((u32)ch >= numChans) { memcpy(o->col[ch], vc, sizeof vc); continue; }
        light_channel(gx, gx->xf[0x100E + ch], vc, p, n, gx->xf[0x100A + ch], gx->xf[0x100C + ch], o->col[ch], 0, 3);
        light_channel(gx, gx->xf[0x1010 + ch], vc, p, n, gx->xf[0x100A + ch], gx->xf[0x100C + ch], o->col[ch], 3, 4);
    }

    // Texture coordinate generation (XF 0x1040 + i).
    u32 numGens = gx->xf[0x103F] & 15;
    for (u32 i = 0; i < numGens && i < 8; i++) {
        u32 cfg = gx->xf[0x1040 + i];
        bool stq = (cfg >> 1) & 1;
        u32 type = (cfg >> 4) & 7, src = (cfg >> 7) & 31;
        float in[4] = { 0, 0, 1, 1 };
        if (src == 0) { in[0] = v->pos[0]; in[1] = v->pos[1]; in[2] = v->pos[2]; }
        else if (src == 1) { in[0] = v->nrm[0][0]; in[1] = v->nrm[0][1]; in[2] = v->nrm[0][2]; }
        else if (src >= 5 && src <= 12) { in[0] = v->tex[src - 5][0]; in[1] = v->tex[src - 5][1]; }
        if (type >= 2) {  // colour texgen: s,t from colour channel 0
            o->tex[i][0] = o->col[0][0] / 255.0f; o->tex[i][1] = o->col[0][1] / 255.0f; o->tex[i][2] = 1;
            continue;
        }
        u32 tm = (u32)v->texmtx[i] * 4;
        for (int r = 0; r < (stq ? 3 : 2); r++)
            o->tex[i][r] = xf_f(gx, tm + 4 * r) * in[0] + xf_f(gx, tm + 4 * r + 1) * in[1] + xf_f(gx, tm + 4 * r + 2) * in[2] + xf_f(gx, tm + 4 * r + 3) * in[3];
        if (!stq) o->tex[i][2] = 1.0f;
    }
    (void)s;
}

// ---- texture sampling ----
typedef struct { u32 addr, fmt, w, h, tlutAddr, tlutFmt, tlutVersion; u32* pix; } TexCache;
static TexCache g_tex[8];

static const u32* fetch_texture(System* s, const GXCore* gx, u32 map, u32* w, u32* h, u32* mode) {
    u32 hi = map >= 4 ? 0x20 : 0, m = map & 3;
    u32 img0 = gx->bp[0x88 + m + hi], img3 = gx->bp[0x94 + m + hi], tlut = gx->bp[0x98 + m + hi];
    *mode = gx->bp[0x80 + m + hi];
    u32 tw = (img0 & 0x3FF) + 1, th = ((img0 >> 10) & 0x3FF) + 1, fmt = (img0 >> 20) & 15;
    u32 addr = (img3 & 0xFFFFFF) << 5;
    *w = tw; *h = th;
    TexCache* c = &g_tex[map];
    u32 tlutAddr = (tlut & 0x3FF) << 9, tlutFmt = (tlut >> 10) & 3;
    // Cached per texture map while nothing it depends on changed. (Texture data rewritten in place is not detected
    // yet; the reference renderer favours correctness on simple programs over that case.)
    if (c->pix && c->addr == addr && c->fmt == fmt && c->w == tw && c->h == th && c->tlutAddr == tlutAddr &&
        c->tlutFmt == tlutFmt && c->tlutVersion == gx->tlutVersion)
        return c->pix;
    u32 bytes = gx_texture_size(fmt, tw, th);
    if (addr + bytes > PA_MEM1_END) return NULL;
    free(c->pix);
    c->pix = malloc((size_t)tw * th * 4);
    if (!c->pix) return NULL;
    gx_decode_texture(s->mem1 + addr, fmt, tw, th, gx->tmem + (tlutAddr & 0xFFFFF), tlutFmt, c->pix);
    c->addr = addr; c->fmt = fmt; c->w = tw; c->h = th; c->tlutAddr = tlutAddr; c->tlutFmt = tlutFmt;
    c->tlutVersion = gx->tlutVersion;
    return c->pix;
}

static int wrap_coord(int v, int size, u32 mode) {
    if (mode == 0) return v < 0 ? 0 : v >= size ? size - 1 : v;            // clamp
    if (mode == 2) { int p = size * 2; v %= p; if (v < 0) v += p; return v < size ? v : p - 1 - v; }  // mirror
    v %= size; return v < 0 ? v + size : v;                                  // repeat
}

static void sample(System* s, const GXCore* gx, u32 map, float u, float v, float out[4]) {
    u32 w, h, mode;
    const u32* pix = fetch_texture(s, gx, map, &w, &h, &mode);
    if (!pix) { out[0] = out[1] = out[2] = out[3] = 255; return; }
    u32 ws = mode & 3, wt = (mode >> 2) & 3;
    bool linear = (mode >> 4) & 1;
    float fx = u * (float)w - (linear ? 0.5f : 0), fy = v * (float)h - (linear ? 0.5f : 0);
    int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
    if (!linear) {
        u32 c = pix[wrap_coord(y0, (int)h, wt) * w + wrap_coord(x0, (int)w, ws)];
        unpack(c, out);
        return;
    }
    float ax = fx - (float)x0, ay = fy - (float)y0;
    float acc[4] = { 0, 0, 0, 0 };
    for (int j = 0; j < 2; j++)
        for (int i = 0; i < 2; i++) {
            float wgt = (i ? ax : 1 - ax) * (j ? ay : 1 - ay);
            float c[4];
            unpack(pix[wrap_coord(y0 + j, (int)h, wt) * w + wrap_coord(x0 + i, (int)w, ws)], c);
            for (int k = 0; k < 4; k++) acc[k] += c[k] * wgt;
        }
    memcpy(out, acc, sizeof acc);
}

// ---- TEV ----
static float konst_value(const GXCore* gx, u32 sel, int comp, const float konst[4][4]) {
    if (sel <= 7) return (8 - sel) * 255.0f / 8.0f;              // 1, 7/8, ... 1/8
    if (sel >= 12 && sel <= 15) return konst[sel - 12][comp];    // Kn rgb (or a)
    if (sel >= 16 && sel <= 31) return konst[(sel - 16) & 3][(sel - 16) >> 2];  // Kn.r / .g / .b / .a replicated
    (void)gx;
    return 255.0f;
}

static float tev_clamp(float v, bool clamp) { return clamp ? (v < 0 ? 0 : v > 255 ? 255 : v) : (v < -1024 ? -1024 : v > 1023 ? 1023 : v); }

static float lerp_tev(float a, float b, float c) {
    float cc = c < 0 ? 0 : c > 255 ? 255 : c;
    return a * (256 - (cc + (cc >= 128 ? 1 : 0))) / 256.0f + b * (cc + (cc >= 128 ? 1 : 0)) / 256.0f;
}

// Compare modes (bias = 3): the scale field picks what is compared (R8, GR16, BGR24, or each channel / A8) and
// the op bit picks greater-than (0) or equal (1).
static bool tev_stage_compare(u32 mode, bool eq, const float a[4], const float b[4], int k, bool alpha) {
    long va, vb;
    switch (mode) {
    case 0: va = (long)a[0]; vb = (long)b[0]; break;
    case 1: va = (long)a[1] * 256 + (long)a[0]; vb = (long)b[1] * 256 + (long)b[0]; break;
    case 2: va = (long)a[2] * 65536 + (long)a[1] * 256 + (long)a[0]; vb = (long)b[2] * 65536 + (long)b[1] * 256 + (long)b[0]; break;
    default: va = (long)(alpha ? a[3] : a[k]); vb = (long)(alpha ? b[3] : b[k]); break;
    }
    return eq ? va == vb : va > vb;
}

static s32 sext11(u32 v) { return ((s32)(v << 21)) >> 21; }

static void run_tev(System* s, const GXCore* gx, const float ras[2][4], const float tc[8][2], float out[4]) {
    u32 gen = gx->bp[0x00];
    u32 stages = ((gen >> 10) & 15) + 1;
    // Registers PREV, C0, C1, C2 (BP 0xE0.. in that order): low word R (0-10) and A (12-22), high word B and G,
    // 11-bit signed. Konst colours use the same layout, 8-bit.
    float reg[4][4], konst[4][4];
    for (int r = 0; r < 4; r++) {
        u32 lo = gx->tevLo[r], hi = gx->tevHi[r];
        reg[r][0] = (float)sext11(lo); reg[r][3] = (float)sext11(lo >> 12);
        reg[r][2] = (float)sext11(hi); reg[r][1] = (float)sext11(hi >> 12);
        lo = gx->kLo[r]; hi = gx->kHi[r];
        konst[r][0] = (float)(lo & 0xFF); konst[r][3] = (float)((lo >> 12) & 0xFF);
        konst[r][2] = (float)(hi & 0xFF); konst[r][1] = (float)((hi >> 12) & 0xFF);
    }
    for (u32 st = 0; st < stages; st++) {
        u32 tref = gx->bp[0x28 + st / 2] >> ((st & 1) * 12);
        u32 texmap = tref & 7, texcoord = (tref >> 3) & 7, chan = (tref >> 7) & 7;
        bool texEn = (tref >> 6) & 1;
        float tex[4] = { 255, 255, 255, 255 }, rasc[4] = { 0, 0, 0, 0 };
        if (texEn) sample(s, gx, texmap, tc[texcoord][0], tc[texcoord][1], tex);
        if (chan < 2) memcpy(rasc, ras[chan], sizeof rasc);
        u32 ksel = gx->bp[0xF6 + st / 2];
        u32 kc = (ksel >> (4 + (st & 1) * 10)) & 31, ka = (ksel >> (9 + (st & 1) * 10)) & 31;
        u32 cenv = gx->bp[0xC0 + 2 * st], aenv = gx->bp[0xC1 + 2 * st];
        // Swap tables: selected per stage for the rasterised colour and the texture.
        u32 rswap = aenv & 3, tswap = (aenv >> 2) & 3;
        float rs[4], ts[4];
        for (int c = 0; c < 4; c++) {
            u32 t0 = gx->bp[0xF6 + rswap * 2], t1 = gx->bp[0xF7 + rswap * 2];
            u32 sel = c < 2 ? (t0 >> (2 * c)) & 3 : (t1 >> (2 * (c - 2))) & 3;
            rs[c] = rasc[sel];
            t0 = gx->bp[0xF6 + tswap * 2]; t1 = gx->bp[0xF7 + tswap * 2];
            sel = c < 2 ? (t0 >> (2 * c)) & 3 : (t1 >> (2 * (c - 2))) & 3;
            ts[c] = tex[sel];
        }
        // Colour combiner inputs.
        float cin[4][3];
        for (int i = 0; i < 4; i++) {
            u32 sel = (cenv >> (12 - 4 * i)) & 15;  // a, b, c, d
            for (int k = 0; k < 3; k++) {
                float v;
                switch (sel) {
                case 0: v = reg[0][k]; break; case 1: v = reg[0][3]; break;
                case 2: v = reg[1][k]; break; case 3: v = reg[1][3]; break;
                case 4: v = reg[2][k]; break; case 5: v = reg[2][3]; break;
                case 6: v = reg[3][k]; break; case 7: v = reg[3][3]; break;
                case 8: v = ts[k]; break; case 9: v = ts[3]; break;
                case 10: v = rs[k]; break; case 11: v = rs[3]; break;
                case 12: v = 255; break; case 13: v = 128; break;
                case 14: v = konst_value(gx, kc, k, konst); break;
                default: v = 0; break;
                }
                cin[i][k] = v;
            }
        }
        float ain[4];
        for (int i = 0; i < 4; i++) {
            u32 sel = (aenv >> (13 - 3 * i)) & 7;
            switch (sel) {
            case 0: ain[i] = reg[0][3]; break; case 1: ain[i] = reg[1][3]; break;
            case 2: ain[i] = reg[2][3]; break; case 3: ain[i] = reg[3][3]; break;
            case 4: ain[i] = ts[3]; break; case 5: ain[i] = rs[3]; break;
            case 6: ain[i] = konst_value(gx, ka, 3, konst); break;
            default: ain[i] = 0; break;
            }
        }
        static const float scales[4] = { 1, 2, 4, 0.5f };
        static const float biases[3] = { 0, 128, -128 };
        u32 cb = (cenv >> 16) & 3, cop = (cenv >> 18) & 1, cclamp = (cenv >> 19) & 1, cscale = (cenv >> 20) & 3, cdest = (cenv >> 22) & 3;
        float res[4];
        for (int k = 0; k < 3; k++) {
            if (cb == 3) {
                float A[4] = { cin[0][0], cin[0][1], cin[0][2], 0 }, B[4] = { cin[1][0], cin[1][1], cin[1][2], 0 };
                bool cmp = tev_stage_compare(cscale, cop, A, B, k, false);
                res[k] = cin[3][k] + (cmp ? cin[2][k] : 0);
            } else {
                float v = lerp_tev(cin[0][k], cin[1][k], cin[2][k]);
                res[k] = (cin[3][k] + (cop ? -v : v) + biases[cb]) * scales[cscale];
            }
            res[k] = tev_clamp(res[k], cclamp);
        }
        u32 ab = (aenv >> 16) & 3, aop = (aenv >> 18) & 1, aclamp = (aenv >> 19) & 1, ascale = (aenv >> 20) & 3, adest = (aenv >> 22) & 3;
        if (ab == 3) {
            float A[4] = { 0, 0, 0, ain[0] }, B[4] = { 0, 0, 0, ain[1] };
            bool cmp = tev_stage_compare(ascale, aop, A, B, 3, true);
            res[3] = ain[3] + (cmp ? ain[2] : 0);
        } else {
            float v = lerp_tev(ain[0], ain[1], ain[2]);
            res[3] = (ain[3] + (aop ? -v : v) + biases[ab]) * scales[ascale];
        }
        res[3] = tev_clamp(res[3], aclamp);
        for (int k = 0; k < 3; k++) reg[cdest][k] = res[k];
        reg[adest][3] = res[3];
    }
    for (int k = 0; k < 4; k++) out[k] = reg[0][k] < 0 ? 0 : reg[0][k] > 255 ? 255 : reg[0][k];
}

// ---- per-pixel tests and blending ----
static bool compare(u32 fn, float a, float b) {
    switch (fn) {
    case 0: return false; case 1: return a < b; case 2: return a == b; case 3: return a <= b;
    case 4: return a > b; case 5: return a != b; case 6: return a >= b; default: return true;
    }
}

static bool alpha_test(const GXCore* gx, float a) {
    u32 r = gx->bp[0xF3];
    bool p0 = compare((r >> 16) & 7, a, (float)(r & 0xFF)), p1 = compare((r >> 19) & 7, a, (float)((r >> 8) & 0xFF));
    switch ((r >> 22) & 3) {
    case 0: return p0 && p1; case 1: return p0 || p1; case 2: return p0 != p1; default: return p0 == p1;
    }
}

static float blend_factor(u32 f, bool isSrc, const float src[4], const float dst[4], int k) {
    switch (f) {
    case 0: return 0;
    case 1: return 1;
    case 2: return (isSrc ? dst[k] : src[k]) / 255.0f;
    case 3: return 1 - (isSrc ? dst[k] : src[k]) / 255.0f;
    case 4: return src[3] / 255.0f;
    case 5: return 1 - src[3] / 255.0f;
    case 6: return dst[3] / 255.0f;
    default: return 1 - dst[3] / 255.0f;
    }
}

static void write_pixel(GXCore* gx, int x, int y, float z, const float c[4]) {
    u32 zmode = gx->bp[0x40], cmode = gx->bp[0x41];
    u32 zi = (u32)(z < 0 ? 0 : z > 16777215.0f ? 16777215.0f : z);
    u32* zb = &gx->efbZ[y * EFB_W + x];
    if ((zmode & 1) && !compare((zmode >> 1) & 7, (float)zi, (float)*zb)) return;
    if (!alpha_test(gx, c[3])) return;
    if ((zmode & 1) && (zmode & 0x10)) *zb = zi;
    u32* px = &gx->efb[y * EFB_W + x];
    float dst[4];
    unpack(*px, dst);
    float out[4];
    memcpy(out, c, sizeof out);
    if (cmode & 1) {
        u32 df = (cmode >> 5) & 7, sf = (cmode >> 8) & 7;
        bool sub = (cmode >> 11) & 1;
        for (int k = 0; k < 4; k++) {
            float v = sub ? dst[k] - c[k] : c[k] * blend_factor(sf, true, c, dst, k) + dst[k] * blend_factor(df, false, c, dst, k);
            out[k] = v < 0 ? 0 : v > 255 ? 255 : v;
        }
    }
    u32 r = (u32)out[0], g = (u32)out[1], b = (u32)out[2], a = (u32)out[3];
    u32 old = *px;
    u32 rgb = (cmode >> 3) & 1 ? ((r << 24) | (g << 16) | (b << 8)) : (old & 0xFFFFFF00u);
    u32 al = (cmode >> 4) & 1 ? a : (old & 0xFF);
    *px = rgb | al;
}

// ---- triangle rasterisation ----
static void raster_tri(System* s, GXCore* gx, const SVert* v0, const SVert* v1, const SVert* v2) {
    float area = (v1->sx - v0->sx) * (v2->sy - v0->sy) - (v2->sx - v0->sx) * (v1->sy - v0->sy);
    if (area == 0) return;
    u32 cull = (gx->bp[0x00] >> 14) & 3;  // 0 none, 1 back, 2 front, 3 all
    if (cull == 3 || (cull == 1 && area < 0) || (cull == 2 && area > 0)) return;
    // Scissor (BP 0x20 top-left, 0x21 bottom-right; both offset by 342).
    int sx0 = (int)((gx->bp[0x20] >> 12) & 0x7FF) - 342, sy0 = (int)(gx->bp[0x20] & 0x7FF) - 342;
    int sx1 = (int)((gx->bp[0x21] >> 12) & 0x7FF) - 342, sy1 = (int)(gx->bp[0x21] & 0x7FF) - 342;
    float minx = fminf(v0->sx, fminf(v1->sx, v2->sx)), maxx = fmaxf(v0->sx, fmaxf(v1->sx, v2->sx));
    float miny = fminf(v0->sy, fminf(v1->sy, v2->sy)), maxy = fmaxf(v0->sy, fmaxf(v1->sy, v2->sy));
    int x0 = (int)floorf(minx), x1 = (int)ceilf(maxx), y0 = (int)floorf(miny), y1 = (int)ceilf(maxy);
    x0 = x0 < sx0 ? sx0 : x0;
    y0 = y0 < sy0 ? sy0 : y0;
    x1 = x1 > sx1 ? sx1 : x1;
    y1 = y1 > sy1 ? sy1 : y1;
    x0 = x0 < 0 ? 0 : x0;
    y0 = y0 < 0 ? 0 : y0;
    x1 = x1 >= EFB_W ? EFB_W - 1 : x1;
    y1 = y1 >= EFB_H ? EFB_H - 1 : y1;
    u32 numGens = gx->xf[0x103F] & 15;
    float iw0 = 1.0f / v0->w, iw1 = 1.0f / v1->w, iw2 = 1.0f / v2->w;
    for (int y = y0; y <= y1; y++) {
        for (int x = x0; x <= x1; x++) {
            float px = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = ((v1->sx - px) * (v2->sy - py) - (v2->sx - px) * (v1->sy - py)) / area;
            float w1 = ((v2->sx - px) * (v0->sy - py) - (v0->sx - px) * (v2->sy - py)) / area;
            float w2 = 1 - w0 - w1;
            if (w0 < 0 || w1 < 0 || w2 < 0) continue;
            float z = w0 * v0->sz + w1 * v1->sz + w2 * v2->sz;
            float pw = w0 * iw0 + w1 * iw1 + w2 * iw2;
            float p0 = w0 * iw0 / pw, p1 = w1 * iw1 / pw, p2 = w2 * iw2 / pw;  // perspective-correct weights
            float ras[2][4], tc[8][2];
            for (int c = 0; c < 2; c++) for (int k = 0; k < 4; k++) ras[c][k] = p0 * v0->col[c][k] + p1 * v1->col[c][k] + p2 * v2->col[c][k];
            for (u32 t = 0; t < numGens && t < 8; t++) {
                float q = p0 * v0->tex[t][2] + p1 * v1->tex[t][2] + p2 * v2->tex[t][2];
                if (q == 0) q = 1;
                tc[t][0] = (p0 * v0->tex[t][0] + p1 * v1->tex[t][0] + p2 * v2->tex[t][0]) / q;
                tc[t][1] = (p0 * v0->tex[t][1] + p1 * v1->tex[t][1] + p2 * v2->tex[t][1]) / q;
            }
            float out[4];
            run_tev(s, gx, ras, tc, out);
            write_pixel(gx, x, y, z, out);
        }
    }
}

static void to_screen(const GXCore* gx, SVert* v) {
    float wd = xf_f(gx, 0x101A), ht = xf_f(gx, 0x101B), zr = xf_f(gx, 0x101C);
    float xo = xf_f(gx, 0x101D), yo = xf_f(gx, 0x101E), fz = xf_f(gx, 0x101F);
    float iw = v->w != 0 ? 1.0f / v->w : 1.0f;
    v->sx = v->x * iw * wd + xo - 342.0f;
    v->sy = v->y * iw * ht + yo - 342.0f;
    v->sz = v->z * iw * zr + fz;
}

static void soft_draw(void* ctx, System* s, u32 prim, u32 vat, const u8* raw, u32 count, u32 vsize) {
    (void)ctx;
    GXCore* gx = &s->gx.core;
    SVert* v = malloc(sizeof(SVert) * count);
    if (!v) return;
    for (u32 i = 0; i < count; i++) {
        GXVertex dv;
        gx_decode_vertex(s, gx, vat, raw + i * vsize, &dv);
        transform(s, gx, &dv, &v[i]);
        to_screen(gx, &v[i]);
    }
    // Triangles with a vertex behind the eye are dropped (no clipping yet).
    #define TRI(a, b, c) do { if (v[a].w > 0 && v[b].w > 0 && v[c].w > 0) raster_tri(s, gx, &v[a], &v[b], &v[c]); } while (0)
    switch (prim) {
    case PRIM_QUADS: case PRIM_QUADS2:
        for (u32 i = 0; i + 3 < count; i += 4) { TRI(i, i + 1, i + 2); TRI(i, i + 2, i + 3); }
        break;
    case PRIM_TRIS: for (u32 i = 0; i + 2 < count; i += 3) TRI(i, i + 1, i + 2); break;
    case PRIM_STRIP: for (u32 i = 0; i + 2 < count; i++) { if (i & 1) TRI(i + 1, i, i + 2); else TRI(i, i + 1, i + 2); } break;
    case PRIM_FAN: for (u32 i = 1; i + 1 < count; i++) TRI(0, i, i + 1); break;
    default: break;  // lines and points: not yet
    }
    #undef TRI
    free(v);
}

static const GXBackend kSoft = { soft_draw };

void gx_soft_attach(System* s) {
    s->gx.core.backend = &kSoft;
    s->gx.core.backendCtx = &g_soft;
}
