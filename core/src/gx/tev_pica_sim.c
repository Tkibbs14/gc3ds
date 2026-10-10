// PICA200 combiner evaluation on the CPU, for checking the GX -> PICA translation in whole frames on the PC
// (soft.c --pica-tev): the combiner semantics follow core/tests/tev/tev_test.c (Citra/Azahar's model).
#include <math.h>
#include "tev_pica.h"
#include "tev_pica_sim.h"

typedef struct { float v[4]; } Col;  // r g b a, 0..1

static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

static Col unpack_konst(u32 k) {
    Col c = { { (float)(k & 255) / 255, (float)((k >> 8) & 255) / 255, (float)((k >> 16) & 255) / 255, (float)(k >> 24) / 255 } };
    return c;
}

static Col source(u8 src, const Col* unit, const Col* prim, u32 konst, const Col* prev, const Col* buf) {
    Col zero = { { 0, 0, 0, 0 } };
    switch (src) {
    case PSRC_PRIMARY: return *prim;
    case PSRC_TEX0: case PSRC_TEX1: case PSRC_TEX2: return unit[src - PSRC_TEX0];
    case PSRC_BUFFER: return *buf;
    case PSRC_CONSTANT: return unpack_konst(konst);
    case PSRC_PREVIOUS: return *prev;
    default: return zero;
    }
}

static void operand_rgb(u8 op, const Col* s, float o[3]) {
    float r = s->v[0], g = s->v[1], b = s->v[2], a = s->v[3];
    switch (op) {
    case POPC_COLOR: o[0] = r; o[1] = g; o[2] = b; return;
    case POPC_1_COLOR: o[0] = 1 - r; o[1] = 1 - g; o[2] = 1 - b; return;
    case POPC_ALPHA: o[0] = o[1] = o[2] = a; return;
    case POPC_1_ALPHA: o[0] = o[1] = o[2] = 1 - a; return;
    case POPC_R: o[0] = o[1] = o[2] = r; return;
    case POPC_1_R: o[0] = o[1] = o[2] = 1 - r; return;
    case POPC_G: o[0] = o[1] = o[2] = g; return;
    case POPC_1_G: o[0] = o[1] = o[2] = 1 - g; return;
    case POPC_B: o[0] = o[1] = o[2] = b; return;
    default: o[0] = o[1] = o[2] = 1 - b; return;  // POPC_1_B
    }
}

static float operand_a(u8 op, const Col* s) {
    switch (op) {
    case POPA_ALPHA: return s->v[3];
    case POPA_1_ALPHA: return 1 - s->v[3];
    case POPA_R: return s->v[0];
    case POPA_1_R: return 1 - s->v[0];
    case POPA_G: return s->v[1];
    case POPA_1_G: return 1 - s->v[1];
    case POPA_B: return s->v[2];
    default: return 1 - s->v[2];  // POPA_1_B
    }
}

static float fn(u8 f, float a, float b, float c) {
    switch (f) {
    case PFN_REPLACE: return a;
    case PFN_MODULATE: return a * b;
    case PFN_ADD: return a + b;
    case PFN_ADD_SIGNED: return a + b - 128.0f / 255;
    case PFN_INTERPOLATE: return a * c + b * (1 - c);
    case PFN_SUBTRACT: return a - b;
    case PFN_MULTIPLY_ADD: return a * b + c;
    case PFN_ADD_MULTIPLY: return fminf(a + b, 1) * c;
    default: return 0;
    }
}

static float scale_of(u8 s) { return s == PSCALE_1 ? 1.0f : s == PSCALE_2 ? 2.0f : 4.0f; }

void tev_pica_eval(const PicaTev* t, const float ras[2][4], const float unitTex[3][4], float out[4]) {
    Col unit[3], prim, prev = { { 0, 0, 0, 0 } }, buf = { { 0, 0, 0, 0 } }, next = unpack_konst(t->bufColor);
    for (int u = 0; u < 3; u++) for (int k = 0; k < 4; k++) unit[u].v[k] = unitTex[u][k] / 255.0f;
    for (int k = 0; k < 4; k++) prim.v[k] = clamp01(ras[t->rasChan & 1][k] / 255.0f);
    for (int i = 0; i < t->numStages; i++) {
        const PicaStage* s = &t->st[i];
        float in[3][3], ia[3], res[4];
        for (int k = 0; k < 3; k++) {
            Col c = source(s->srcRgb[k], unit, &prim, s->konst, &prev, &buf);
            operand_rgb(s->opRgb[k], &c, in[k]);
            c = source(s->srcA[k], unit, &prim, s->konst, &prev, &buf);
            ia[k] = operand_a(s->opA[k], &c);
        }
        if (s->fnRgb == PFN_DOT3_RGB || s->fnRgb == PFN_DOT3_RGBA) {
            float d = 4 * ((in[0][0] - .5f) * (in[1][0] - .5f) + (in[0][1] - .5f) * (in[1][1] - .5f) + (in[0][2] - .5f) * (in[1][2] - .5f));
            res[0] = res[1] = res[2] = clamp01(d * scale_of(s->scaleRgb));
        } else {
            for (int k = 0; k < 3; k++) res[k] = clamp01(fn(s->fnRgb, in[0][k], in[1][k], in[2][k]) * scale_of(s->scaleRgb));
        }
        res[3] = s->fnRgb == PFN_DOT3_RGBA ? res[0] : clamp01(fn(s->fnA, ia[0], ia[1], ia[2]) * scale_of(s->scaleA));
        for (int k = 0; k < 4; k++) res[k] = roundf(res[k] * 255) / 255;
        buf = next;
        if (i < 4 && ((t->bufRgb >> i) & 1)) for (int k = 0; k < 3; k++) next.v[k] = res[k];
        if (i < 4 && ((t->bufA >> i) & 1)) next.v[3] = res[3];
        for (int k = 0; k < 4; k++) prev.v[k] = res[k];
    }
    for (int k = 0; k < 4; k++) out[k] = prev.v[k] * 255;
}
