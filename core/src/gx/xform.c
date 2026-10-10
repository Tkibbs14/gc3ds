// Vertex transformation shared by the GX backends: XF position/normal matrices, projection, per-vertex lighting,
// texture coordinate generation and the viewport. The software renderer (soft.c) is the reference; the PICA200
// backend calls the same code so both see identical vertices.
#include <math.h>
#include <string.h>
#include "xform.h"

static float xf_f(const GXCore* gx, u32 a) { float f; memcpy(&f, &gx->xf[a], 4); return f; }

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
void gx_transform(const GXCore* gx, const GXVertex* v, SVert* o) {
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
}

void gx_to_screen(const GXCore* gx, SVert* v) {
    float wd = xf_f(gx, 0x101A), ht = xf_f(gx, 0x101B), zr = xf_f(gx, 0x101C);
    float xo = xf_f(gx, 0x101D), yo = xf_f(gx, 0x101E), fz = xf_f(gx, 0x101F);
    float iw = v->w != 0 ? 1.0f / v->w : 1.0f;
    v->sx = v->x * iw * wd + xo - 342.0f;
    v->sy = v->y * iw * ht + yo - 342.0f;
    v->sz = v->z * iw * zr + fz;
}
