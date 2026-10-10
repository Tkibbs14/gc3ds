// Software reference renderer for GX draws (PC). Favours clarity over speed; the 3DS backend is checked against it.
// Covers: XF position/normal transform, projection, viewport, per-vertex lighting (diffuse, distance/angle
// attenuation), regular texgen, triangle rasterisation with perspective-correct attributes and a depth buffer,
// TEV colour/alpha combiners (all inputs, konst, swap tables, compare modes), texture sampling (nearest/bilinear,
// wrap modes, TLUTs), indirect texturing, alpha test, Z textures, fog, blending, lines and points (as quads). Not yet:
// range fog, mipmaps (level 0 only), clipping (triangles crossing the eye plane are dropped).
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
typedef struct { u32 addr, fmt, w, h, tlutAddr, tlutFmt, tlutVersion, texVersion; u32* pix; } TexCache;
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
    // Cached per texture map while nothing it depends on changed. Texture data rewritten in place is caught at the
    // next frame or EFB copy (texVersion), not within a frame.
    if (c->pix && c->addr == addr && c->fmt == fmt && c->w == tw && c->h == th && c->tlutAddr == tlutAddr &&
        c->tlutFmt == tlutFmt && c->tlutVersion == gx->tlutVersion && c->texVersion == gx->texVersion)
        return c->pix;
    u32 bytes = gx_texture_size(fmt, tw, th);
    if (addr + bytes > PA_MEM1_END) return NULL;
    free(c->pix);
    c->pix = malloc((size_t)tw * th * 4);
    if (!c->pix) return NULL;
    gx_decode_texture(s->mem1 + addr, fmt, tw, th, gx->tmem + (tlutAddr & 0xFFFFF), tlutFmt, c->pix);
    c->addr = addr; c->fmt = fmt; c->w = tw; c->h = th; c->tlutAddr = tlutAddr; c->tlutFmt = tlutFmt;
    c->tlutVersion = gx->tlutVersion;
    c->texVersion = gx->texVersion;
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

// Texel of the last stage that sampled a texture (after its swap table): the Z-texture unit reads it.
static float g_lastTex[4];

// ---- indirect texturing ----
// Coordinates here are texels in 1/128 units (S17.7), as the hardware's texture coordinate path keeps them; texels
// per normalised unit come from the setup unit's per-coordinate size (BP 0x30 + 2n: S size - 1, 0x31 + 2n: T size - 1),
// which GXLoadTexObj sets to the texture's size. BP 0x27: per indirect stage, map (3 bits) and coordinate (3 bits);
// 0x25/0x26: per indirect stage S/T shift (4 bits each); 0x06-0x0E: three 2x3 matrices, S0.10 elements, 6-bit scale
// split over the three registers; 0x10 + stage: the TEV stage's indirect command. Semantics as YAGCD BP 0x10/0x27 and
// Dolphin's software renderer (Tev.cpp, read for reference).
static float su_size(const GXCore* gx, u32 coord, int axis) { return (float)((gx->bp[0x30 + 2 * coord + axis] & 0xFFFF) + 1); }

static int ind_wrap(int v, u32 mode) {
    if (!mode || mode > 6) return v;
    if (mode == 6) return 0;
    return v & (((256 >> (mode - 1)) << 7) - 1);  // 256, 128, 64, 32, 16 texels
}

static void tex_dims(const GXCore* gx, u32 map, u32* w, u32* h) {
    u32 img0 = gx->bp[0x88 + (map & 3) + (map >= 4 ? 0x20 : 0)];
    *w = (img0 & 0x3FF) + 1;
    *h = ((img0 >> 10) & 0x3FF) + 1;
}

// Samples the indirect textures for this pixel: out[i] = texel of indirect stage i (RGBA, 0-255).
static void sample_indirect(System* s, const GXCore* gx, const float tc[8][2], u32 numInd, float out[4][4]) {
    u32 iref = gx->bp[0x27];
    for (u32 i = 0; i < numInd; i++) {
        u32 map = (iref >> (6 * i)) & 7, coord = (iref >> (6 * i + 3)) & 7;
        u32 sc = gx->bp[0x25 + i / 2] >> ((i & 1) * 8), ss = sc & 15, ts = (sc >> 4) & 15;
        u32 w, h;
        tex_dims(gx, map, &w, &h);
        float u = tc[coord][0] * su_size(gx, coord, 0) / (float)(1u << ss) / (float)w;
        float v = tc[coord][1] * su_size(gx, coord, 1) / (float)(1u << ts) / (float)h;
        sample(s, gx, map, u, v, out[i]);
    }
}

// One TEV stage's indirect command: turns the regular coordinate (normalised) into the coordinate it samples with,
// carrying the running coordinate for "add previous". Returns the bump alpha.
static u32 indirect_coord(const GXCore* gx, u32 cmd, u32 texcoord, const float tc[8][2], float ind[4][4], int prev[2],
                          float outUv[2]) {
    u32 bt = cmd & 3, fmt = (cmd >> 2) & 3, bias = (cmd >> 4) & 7, bs = (cmd >> 7) & 3, mid = (cmd >> 9) & 15;
    u32 sw = (cmd >> 13) & 7, tw = (cmd >> 16) & 7;
    bool addPrev = (cmd >> 20) & 1;
    float sS = su_size(gx, texcoord, 0), sT = su_size(gx, texcoord, 1);
    int S = (int)floorf(tc[texcoord][0] * sS * 128.0f), T = (int)floorf(tc[texcoord][1] * sT * 128.0f);
    // Texel components: S offset from alpha, T from blue, U from green; the bits below the offset field are bump alpha.
    int raw[3] = { (int)ind[bt][3], (int)ind[bt][2], (int)ind[bt][1] };
    static const u8 shifts[4] = { 0, 3, 4, 5 };
    u32 alphaBump = bs ? (u32)raw[bs - 1] : 0;
    alphaBump = fmt == 0 ? alphaBump & 0xF8 : (alphaBump << (8 - shifts[fmt])) & 0xFF;
    int biasValue = fmt == 0 ? -128 : 1, ic[3];
    for (int k = 0; k < 3; k++) ic[k] = (raw[k] >> shifts[fmt]) + ((bias >> k) & 1 ? biasValue : 0);
    int t0 = 0, t1 = 0;
    u32 index = mid & 3, type = mid >> 2;
    if (index) {
        u32 a = gx->bp[0x06 + 3 * (index - 1)], b = gx->bp[0x07 + 3 * (index - 1)], c = gx->bp[0x08 + 3 * (index - 1)];
        int scale = (int)(((a >> 22) & 3) | (((b >> 22) & 3) << 2) | (((c >> 22) & 3) << 4));
        if (type == 0) {
            t0 = (sext11(a) * ic[0] + sext11(b) * ic[1] + sext11(c) * ic[2]) >> 3;
            t1 = (sext11(a >> 11) * ic[0] + sext11(b >> 11) * ic[1] + sext11(c >> 11) * ic[2]) >> 3;
        } else {
            int k = type == 1 ? ic[0] : ic[1];
            t0 = S * k / 256;
            t1 = T * k / 256;
        }
        int shift = 17 - scale;
        if (shift >= 0) { t0 >>= shift; t1 >>= shift; } else { t0 *= 1 << -shift; t1 *= 1 << -shift; }
    }
    int ns = ind_wrap(S, sw) + t0, nt = ind_wrap(T, tw) + t1;
    if (addPrev) { prev[0] += ns; prev[1] += nt; } else { prev[0] = ns; prev[1] = nt; }
    outUv[0] = (float)prev[0] / 128.0f / sS;
    outUv[1] = (float)prev[1] / 128.0f / sT;
    return alphaBump;
}

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
    // Indirect texturing is evaluated only when some stage has an indirect command, so plain draws keep exact
    // floating-point coordinates.
    u32 numInd = (gen >> 16) & 7;
    bool anyInd = false;
    for (u32 st = 0; st < stages; st++) if (gx->bp[0x10 + st] & 0x1FFFFF) anyInd = true;
    float ind[4][4];
    int prevUv[2] = { 0, 0 };
    if (anyInd) {
        memset(ind, 0, sizeof ind);
        sample_indirect(s, gx, tc, numInd < 4 ? numInd : 4, ind);
    }
    for (u32 st = 0; st < stages; st++) {
        u32 tref = gx->bp[0x28 + st / 2] >> ((st & 1) * 12);
        u32 texmap = tref & 7, texcoord = (tref >> 3) & 7, chan = (tref >> 7) & 7;
        bool texEn = (tref >> 6) & 1;
        float tex[4] = { 255, 255, 255, 255 }, rasc[4] = { 0, 0, 0, 0 };
        float uv[2] = { tc[texcoord][0], tc[texcoord][1] };
        u32 bump = 0;
        if (anyInd) bump = indirect_coord(gx, gx->bp[0x10 + st] & 0x1FFFFF, texcoord, tc, ind, prevUv, uv);
        if (texEn) sample(s, gx, texmap, uv[0], uv[1], tex);
        if (chan < 2) memcpy(rasc, ras[chan], sizeof rasc);
        else if (chan == 5 || chan == 6) {  // bump alpha (6: normalised to 0-255)
            float b = (float)(chan == 5 ? bump : bump | (bump >> 5));
            rasc[0] = rasc[1] = rasc[2] = rasc[3] = b;
        }
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
        if (texEn) memcpy(g_lastTex, ts, sizeof ts);
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

// ---- fog ----
// BP 0xEE: A as a 20-bit float (sign 19, exponent 11-18, mantissa 0-10); 0xEF: B magnitude; 0xF0: B shift;
// 0xF1: C (same float layout), bit 20 orthographic, bits 21-23 function (2 linear, 4 exp, 5 exp2, 6 backward exp,
// 7 backward exp2); 0xF2: colour. Eye depth: A * 2^24 / (B - (Zs >> shift)) for perspective, A * Zs / 2^24 for ortho;
// fog = clamp(depth - C), shaped by the function, blended in 8-bit steps. Sources: YAGCD BP 0xEE-0xF2; Dolphin's
// pixel shader generator (formulae read for reference). Range adjustment (BP 0xE8-0xED) is not applied.
typedef struct { u32 fn; bool ortho; float a, c; u32 bmag, bshift; float color[3]; } Fog;

static float fog_float(u32 v) {
    u32 bits = ((v >> 19) & 1) << 31 | ((v >> 11) & 0xFF) << 23 | (v & 0x7FF) << 12;
    float f;
    memcpy(&f, &bits, 4);
    return f;
}

static void setup_fog(const GXCore* gx, Fog* f) {
    u32 p3 = gx->bp[0xF1];
    f->fn = (p3 >> 21) & 7;
    if (f->fn < 2 || f->fn == 3) { f->fn = 0; return; }
    f->ortho = (p3 >> 20) & 1;
    f->a = fog_float(gx->bp[0xEE]);
    f->c = fog_float(p3);
    f->bmag = gx->bp[0xEF] & 0xFFFFFF;
    f->bshift = gx->bp[0xF0] & 0x1F;
    u32 col = gx->bp[0xF2];
    f->color[0] = (float)((col >> 16) & 0xFF); f->color[1] = (float)((col >> 8) & 0xFF); f->color[2] = (float)(col & 0xFF);
}

static void apply_fog(const Fog* f, float z, float out[4]) {
    u32 zs = (u32)(z < 0 ? 0 : z > 16777215.0f ? 16777215.0f : z);
    float ze;
    if (f->ortho) ze = f->a * ((float)zs / 16777216.0f);
    else {
        float den = (float)f->bmag - (float)(zs >> f->bshift);
        ze = den != 0 ? f->a * 16777216.0f / den : 0;
    }
    float v = ze - f->c;
    v = v < 0 ? 0 : v > 1 ? 1 : v;
    switch (f->fn) {
    case 4: v = 1.0f - exp2f(-8.0f * v); break;
    case 5: v = 1.0f - exp2f(-8.0f * v * v); break;
    case 6: v = exp2f(-8.0f * (1.0f - v)); break;
    case 7: v = 1.0f - v; v = exp2f(-8.0f * v * v); break;
    default: break;
    }
    int k = (int)(v * 256.0f + 0.5f);
    for (int i = 0; i < 3; i++) out[i] = (out[i] * (float)(256 - k) + f->color[i] * (float)k) / 256.0f;
}

// ---- triangle rasterisation ----
static bool g_noCull;
static void raster_tri(System* s, GXCore* gx, const SVert* v0, const SVert* v1, const SVert* v2) {
    float area = (v1->sx - v0->sx) * (v2->sy - v0->sy) - (v2->sx - v0->sx) * (v1->sy - v0->sy);
    if (area == 0) return;
    u32 cull = g_noCull ? 0 : (gx->bp[0x00] >> 14) & 3;  // 0 none, 1 back, 2 front, 3 all
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
    Fog fog = { 0 };
    setup_fog(gx, &fog);
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
            u32 ztexMode = gx->bp[0xF5];
            if ((ztexMode >> 2) & 3) {
                // Z texture (BP 0xF4 bias, 0xF5 bits 0-1 texel format, 2-3 op): depth from the last stage's texel, as
                // U8 (alpha), U16 (alpha:red, big-endian like IA8 / Z16 textures) or U24 (red:green:blue), plus the
                // bias, added to (op 1) or replacing (op 2) the rasterised depth. Source: YAGCD BP 0xF4/0xF5, Dolphin.
                u32 r = (u32)g_lastTex[0], g = (u32)g_lastTex[1], b = (u32)g_lastTex[2], a = (u32)g_lastTex[3];
                u32 zt = gx->bp[0xF4] & 0xFFFFFF;
                switch (ztexMode & 3) {
                case 0: zt += a; break;
                case 1: zt += (a << 8) | r; break;
                default: zt += (r << 16) | (g << 8) | b; break;
                }
                if (((ztexMode >> 2) & 3) == 1) zt += (u32)(z < 0 ? 0 : z > 16777215.0f ? 16777215.0f : z);
                z = (float)(zt & 0xFFFFFF);
            }
            if (fog.fn) apply_fog(&fog, z, out);
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

// Lines and points are drawn as screen-space quads. BP 0x22 (SU_LPSIZE): bits 0-7 line width and 8-15 point size, in
// sixths of a pixel. A line is widened vertically when it is more horizontal than vertical, else horizontally (as the
// setup unit does). Texture-coordinate offsets across the quad (BP 0x22 bits 16-21) are not applied.
static void raster_quad(System* s, GXCore* gx, SVert q[4]) {
    g_noCull = true;  // lines and points are never culled
    raster_tri(s, gx, &q[0], &q[1], &q[2]);
    raster_tri(s, gx, &q[0], &q[2], &q[3]);
    g_noCull = false;
}

static void draw_line(System* s, GXCore* gx, const SVert* a, const SVert* b) {
    if (a->w <= 0 || b->w <= 0) return;
    float hw = (float)(gx->bp[0x22] & 0xFF) / 12.0f;
    bool horiz = fabsf(b->sx - a->sx) >= fabsf(b->sy - a->sy);
    SVert q[4] = { *a, *b, *b, *a };
    for (int i = 0; i < 4; i++) {
        float d = i < 2 ? -hw : hw;
        if (horiz) q[i].sy += d; else q[i].sx += d;
    }
    raster_quad(s, gx, q);
}

static void draw_point(System* s, GXCore* gx, const SVert* p) {
    if (p->w <= 0) return;
    float h = (float)((gx->bp[0x22] >> 8) & 0xFF) / 12.0f;
    SVert q[4] = { *p, *p, *p, *p };
    q[0].sx -= h; q[0].sy -= h; q[1].sx += h; q[1].sy -= h; q[2].sx += h; q[2].sy += h; q[3].sx -= h; q[3].sy += h;
    raster_quad(s, gx, q);
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
    if (gx->traceDraws)
        emu_log(LOG_GX, "draw prim %u x%u vat %u zmode %02x blend %04x tev %u gens %u chans %u v0 %.1f,%.1f,%.0f w %.3g "
                "v1 %.1f,%.1f,%.0f sc %06x %06x %06x vp %.0f %.0f %.0f %.0f | cenv %06x aenv %06x atest %06x tref %06x "
                "ksel %06x tex0 %06x %06x ind %06x %06x | z' %.9g zr %g fz %g proj %u %g %g\n",
                prim, count, vat, gx->bp[0x40] & 0x1F, gx->bp[0x41] & 0xFFFF, ((gx->bp[0x00] >> 10) & 15) + 1,
                gx->xf[0x103F] & 15, gx->xf[0x1009] & 3, v[0].sx, v[0].sy, v[0].sz, v[0].w,
                count > 1 ? v[1].sx : 0.f, count > 1 ? v[1].sy : 0.f, count > 1 ? v[1].sz : 0.f, gx->bp[0x20], gx->bp[0x21],
                gx->bp[0x59], xf_f(gx, 0x101A), xf_f(gx, 0x101B), xf_f(gx, 0x101D), xf_f(gx, 0x101E), gx->bp[0xC0],
                gx->bp[0xC1], gx->bp[0xF3], gx->bp[0x28], gx->bp[0xF6], gx->bp[0x88], gx->bp[0x80], gx->bp[0x10], gx->bp[0x27],
                v[0].z, xf_f(gx, 0x101C), xf_f(gx, 0x101F), gx->xf[0x1026], xf_f(gx, 0x1024), xf_f(gx, 0x1025));
    // Triangles with a vertex behind the eye are dropped (no clipping yet).
    #define TRI(a, b, c) do { if (v[a].w > 0 && v[b].w > 0 && v[c].w > 0) raster_tri(s, gx, &v[a], &v[b], &v[c]); } while (0)
    switch (prim) {
    case PRIM_QUADS: case PRIM_QUADS2:
        for (u32 i = 0; i + 3 < count; i += 4) { TRI(i, i + 1, i + 2); TRI(i, i + 2, i + 3); }
        break;
    case PRIM_TRIS: for (u32 i = 0; i + 2 < count; i += 3) TRI(i, i + 1, i + 2); break;
    case PRIM_STRIP: for (u32 i = 0; i + 2 < count; i++) { if (i & 1) TRI(i + 1, i, i + 2); else TRI(i, i + 1, i + 2); } break;
    case PRIM_FAN: for (u32 i = 1; i + 1 < count; i++) TRI(0, i, i + 1); break;
    case PRIM_LINES: for (u32 i = 0; i + 1 < count; i += 2) draw_line(s, gx, &v[i], &v[i + 1]); break;
    case PRIM_LSTRIP: for (u32 i = 0; i + 1 < count; i++) draw_line(s, gx, &v[i], &v[i + 1]); break;
    case PRIM_POINTS: for (u32 i = 0; i < count; i++) draw_point(s, gx, &v[i]); break;
    default: break;
    }
    #undef TRI
    free(v);
}

static const GXBackend kSoft = { soft_draw };

void gx_soft_attach(System* s) {
    s->gx.core.backend = &kSoft;
    s->gx.core.backendCtx = &g_soft;
}
