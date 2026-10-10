// Differential test of tev_pica_compile: the reference TEV (soft.c run_tev, through gx_soft_eval_tev with fixed texels)
// against a simulator of the PICA200 combiners running the compiled PicaTev, on the same rasterised colours and texels.
//   tev_test [fuzz-cases] [seed] [all|hand|game|regs|live|wild|bench]     TEV_PLANT=1 corrupts every compiled result to prove the comparison can fail.
// Cases the compiler reports as exact (approx == 0) must agree to within 2/255 for one stage at scale 1 and a tolerance
// that grows with scale gain and stage depth (see tolerance()); approximated cases are only reported.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include "../../src/gx/tev_pica.h"

#define TOL 2.0f

// ---- random numbers ----
static u64 rng = 0x9E3779B97F4A7C15ull;
static u32 rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (u32)(rng >> 16); }
static u32 rbits(u32 n) { return rnd() & ((1u << n) - 1); }
static bool chance(u32 pct) { return rnd() % 100 < pct; }

// ---- PICA200 combiner simulator ----
typedef struct { float v[4]; } Col;  // r g b a, 0..1

static float clamp01(float x) { return x < 0 ? 0 : x > 1 ? 1 : x; }

static Col unpack_konst(u32 k) {
    Col c = { { (float)(k & 255) / 255, (float)((k >> 8) & 255) / 255, (float)((k >> 16) & 255) / 255, (float)(k >> 24) / 255 } };
    return c;
}

static Col pica_source(u8 src, const PicaTev* t, const Col* tex, const Col* prim, u32 konst, const Col* prev, const Col* buf) {
    Col zero = { { 0, 0, 0, 0 } };
    switch (src) {
    case PSRC_PRIMARY: return *prim;
    case PSRC_TEX0: case PSRC_TEX1: case PSRC_TEX2: return tex[t->texMap[src - PSRC_TEX0]];
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

// One combiner function on one component (DOT3 is handled by the caller as it spans components).
static float pica_fn(u8 fn, float a, float b, float c) {
    switch (fn) {
    case PFN_REPLACE: return a;
    case PFN_MODULATE: return a * b;
    case PFN_ADD: return a + b;
    case PFN_ADD_SIGNED: return a + b - 128.0f / 255;  // the combiner works on 8-bit values: its half is 128/255, like GX bias 128
    case PFN_INTERPOLATE: return a * c + b * (1 - c);
    case PFN_SUBTRACT: return a - b;
    case PFN_MULTIPLY_ADD: return a * b + c;
    case PFN_ADD_MULTIPLY: return fminf(a + b, 1) * c;
    default: return 0;
    }
}

static float scale_of(u8 s) { return s == PSCALE_1 ? 1.0f : s == PSCALE_2 ? 2.0f : 4.0f; }

// ras: GX colours 0/1 (0-255), texel: per GX texmap (0-255). out: RGBA 0-255. quant rounds every stage output to 8 bits.
static void pica_eval(const PicaTev* t, const float ras[2][4], const float texel[8][4], bool quant, float out[4]) {
    Col tex[8], prim, prev = { { 0, 0, 0, 0 } }, buf = { { 0, 0, 0, 0 } }, next = unpack_konst(t->bufColor);
    for (int m = 0; m < 8; m++) for (int k = 0; k < 4; k++) tex[m].v[k] = texel[m][k] / 255.0f;
    for (int k = 0; k < 4; k++) prim.v[k] = ras[t->rasChan & 1][k] / 255.0f;
    for (int i = 0; i < t->numStages; i++) {
        const PicaStage* s = &t->st[i];
        float in[3][3], ia[3], res[4];
        for (int k = 0; k < 3; k++) {
            Col c = pica_source(s->srcRgb[k], t, tex, &prim, s->konst, &prev, &buf);
            operand_rgb(s->opRgb[k], &c, in[k]);
            c = pica_source(s->srcA[k], t, tex, &prim, s->konst, &prev, &buf);
            ia[k] = operand_a(s->opA[k], &c);
        }
        if (s->fnRgb == PFN_DOT3_RGB) {
            float d = 4 * ((in[0][0] - .5f) * (in[1][0] - .5f) + (in[0][1] - .5f) * (in[1][1] - .5f) + (in[0][2] - .5f) * (in[1][2] - .5f));
            res[0] = res[1] = res[2] = clamp01(d * scale_of(s->scaleRgb));
        } else {
            for (int k = 0; k < 3; k++) res[k] = clamp01(pica_fn(s->fnRgb, in[0][k], in[1][k], in[2][k]) * scale_of(s->scaleRgb));
        }
        res[3] = clamp01(pica_fn(s->fnA, ia[0], ia[1], ia[2]) * scale_of(s->scaleA));
        if (quant) for (int k = 0; k < 4; k++) res[k] = roundf(res[k] * 255) / 255;
        buf = next;
        if (i < 4 && ((t->bufRgb >> i) & 1)) for (int k = 0; k < 3; k++) next.v[k] = res[k];
        if (i < 4 && ((t->bufA >> i) & 1)) next.v[3] = res[3];
        for (int k = 0; k < 4; k++) prev.v[k] = res[k];
    }
    for (int k = 0; k < 4; k++) out[k] = prev.v[k] * 255;
}

// ---- GX state builder ----
enum { CC_CPREV, CC_APREV, CC_C0, CC_A0, CC_C1, CC_A1, CC_C2, CC_A2, CC_TEXC, CC_TEXA, CC_RASC, CC_RASA, CC_ONE, CC_HALF, CC_KONST, CC_ZERO };
enum { CA_APREV, CA_A0, CA_A1, CA_A2, CA_TEXA, CA_RASA, CA_KONST, CA_ZERO };
enum { KC_1 = 0, KC_K0 = 12, KC_K1, KC_K2, KC_K3, KC_K0_R = 16, KC_K0_G = 20, KC_K0_B = 24, KC_K0_A = 28 };
enum { DST_PREV, DST_C0, DST_C1, DST_C2 };

typedef struct {
    u8 texmap, texcoord, texEn, chan, kc, ka, rswap, tswap;
    u8 c[4], cbias, cop, cclamp, cscale, cdest;  // inputs a b c d
    u8 a[4], abias, aop, aclamp, ascale, adest;
} Stage;

static Stage stage_new(void) {
    Stage s;
    memset(&s, 0, sizeof s);
    for (int i = 0; i < 4; i++) { s.c[i] = CC_ZERO; s.a[i] = CA_ZERO; }
    s.cclamp = s.aclamp = 1;
    return s;
}


static void set_stage(GXCore* g, int st, const Stage* s) {
    u32 sh = (st & 1) * 12;
    u32 tref = s->texmap | (u32)s->texcoord << 3 | (u32)s->texEn << 6 | (u32)s->chan << 7;
    g->bp[0x28 + st / 2] = (g->bp[0x28 + st / 2] & ~(0xFFFu << sh)) | tref << sh;
    u32 ksh = 4 + (st & 1) * 10;
    g->bp[0xF6 + st / 2] = (g->bp[0xF6 + st / 2] & ~(0x3FFu << ksh)) | ((u32)s->kc << ksh) | ((u32)s->ka << (ksh + 5));
    g->bp[0xC0 + 2 * st] = s->c[3] | (u32)s->c[2] << 4 | (u32)s->c[1] << 8 | (u32)s->c[0] << 12 | (u32)s->cbias << 16 |
                           (u32)s->cop << 18 | (u32)s->cclamp << 19 | (u32)s->cscale << 20 | (u32)s->cdest << 22;
    g->bp[0xC1 + 2 * st] = s->rswap | (u32)s->tswap << 2 | (u32)s->a[3] << 4 | (u32)s->a[2] << 7 | (u32)s->a[1] << 10 |
                           (u32)s->a[0] << 13 | (u32)s->abias << 16 | (u32)s->aop << 18 | (u32)s->aclamp << 19 |
                           (u32)s->ascale << 20 | (u32)s->adest << 22;
}

static void set_count(GXCore* g, int n) { g->bp[0x00] = (g->bp[0x00] & ~(15u << 10)) | (u32)(n - 1) << 10; }

static void set_swap(GXCore* g, int tab, int r, int gg, int b, int a) {
    g->bp[0xF6 + 2 * tab] = (g->bp[0xF6 + 2 * tab] & ~15u) | (u32)r | (u32)gg << 2;
    g->bp[0xF7 + 2 * tab] = (g->bp[0xF7 + 2 * tab] & ~15u) | (u32)b | (u32)a << 2;
}

static void set_reg(GXCore* g, int r, int R, int G, int B, int A) {
    g->tevLo[r] = ((u32)R & 0x7FF) | (((u32)A & 0x7FF) << 12);
    g->tevHi[r] = ((u32)B & 0x7FF) | (((u32)G & 0x7FF) << 12);
}

static void set_konst(GXCore* g, int r, int R, int G, int B, int A) {
    g->kLo[r] = (u32)R | (u32)A << 12;
    g->kHi[r] = (u32)B | (u32)G << 12;
}

static void gx_reset(GXCore* g) { memset(g, 0, sizeof *g); }

// Convenience: stage with colour (a,b,c,d) and alpha (aa,ab,ac,ad), texture map m on coordinate m.
static Stage mk(u8 ca, u8 cb, u8 cc, u8 cd, u8 aa, u8 ab, u8 ac, u8 ad) {
    Stage s = stage_new();
    s.c[0] = ca; s.c[1] = cb; s.c[2] = cc; s.c[3] = cd;
    s.a[0] = aa; s.a[1] = ab; s.a[2] = ac; s.a[3] = ad;
    return s;
}

static Stage mk_tex(u8 map, Stage s) { s.texmap = map; s.texcoord = map; s.texEn = 1; return s; }

// ---- comparison ----
static GXCore g_gx;
static int g_plant;  // TEV_PLANT: corrupt each compiled result

static void plant(PicaTev* t) {
    PicaStage* s = &t->st[t->numStages - 1];
    s->opRgb[0] ^= 1;  // inverted operand: changes the result of every function
    s->opA[0] ^= 1;
}

// g_bin: all colours and texels are 0 or 255, so multiplies, lerps and sums are exact in both arithmetics and any
// difference left is a translation fault, whatever the depth of the stage chain.
static bool g_bin;

static void random_inputs(float ras[2][4], float texel[8][4]) {
    if (g_bin) {
        for (int i = 0; i < 2; i++) for (int k = 0; k < 4; k++) ras[i][k] = rnd() & 1 ? 255.0f : 0.0f;
        for (int i = 0; i < 8; i++) for (int k = 0; k < 4; k++) texel[i][k] = rnd() & 1 ? 255.0f : 0.0f;
        return;
    }
    static const float edge[] = { 0, 255, 128, 127, 1, 254 };
    for (int i = 0; i < 2; i++) for (int k = 0; k < 4; k++) ras[i][k] = chance(15) ? edge[rnd() % 6] : (float)rbits(8);
    for (int i = 0; i < 8; i++) for (int k = 0; k < 4; k++) texel[i][k] = chance(15) ? edge[rnd() % 6] : (float)rbits(8);
}

// Maximum absolute error (0-255 scale) over n random input sets; qerr gets the same with 8-bit rounding per stage.
static float max_error(const GXCore* g, const PicaTev* t, int n, float* qerr) {
    float worst = 0, qworst = 0;
    for (int i = 0; i < n; i++) {
        float ras[2][4], texel[8][4], ref[4], sim[4], simq[4];
        random_inputs(ras, texel);
        gx_soft_eval_tev(g, ras, texel, ref);
        pica_eval(t, ras, texel, false, sim);
        pica_eval(t, ras, texel, true, simq);
        for (int k = 0; k < 4; k++) {
            float e = fabsf(ref[k] - sim[k]), q = fabsf(ref[k] - simq[k]);
            if (e > worst) worst = e;
            if (q > qworst) qworst = q;
        }
    }
    if (qerr) *qerr = qworst;
    return worst;
}

static const char* const kFlag[] = { "STAGES", "TEXUNITS", "RASCHAN", "SWAP", "RANGE", "COMPARE", "REGS", "BIAS" };

static void flags_str(u32 f, char* out) {
    out[0] = 0;
    if (!f) { strcpy(out, "exact"); return; }
    for (int i = 0; i < 8; i++) if (f & (1u << i)) { if (out[0]) strcat(out, "|"); strcat(out, kFlag[i]); }
}

static void dump_pica(const PicaTev* t) {
    static const char* const fn[] = { "REPLACE", "MODULATE", "ADD", "ADD_SIGNED", "INTERPOLATE", "SUBTRACT", "DOT3_RGB", "DOT3_RGBA", "MULTIPLY_ADD", "ADD_MULTIPLY" };
    printf("    PICA: %d stages, tex %d [", t->numStages, t->numTex);
    for (int i = 0; i < t->numTex; i++) printf("%s(map%d,coord%d)", i ? " " : "", t->texMap[i], t->texCoord[i]);
    printf("], ras %d, bufRgb %x bufA %x\n", t->rasChan, t->bufRgb, t->bufA);
    for (int i = 0; i < t->numStages; i++) {
        const PicaStage* s = &t->st[i];
        printf("    %d: rgb %s x%d src %x/%x %x/%x %x/%x | a %s x%d src %x/%x %x/%x %x/%x | k %08x\n", i, fn[s->fnRgb], 1 << s->scaleRgb,
               s->srcRgb[0], s->opRgb[0], s->srcRgb[1], s->opRgb[1], s->srcRgb[2], s->opRgb[2], fn[s->fnA], 1 << s->scaleA,
               s->srcA[0], s->opA[0], s->srcA[1], s->opA[1], s->srcA[2], s->opA[2], s->konst);
    }
}

static void dump_gx(const GXCore* g) {
    static const char* const cin[16] = { "Cp", "Ap", "C0", "A0", "C1", "A1", "C2", "A2", "TexC", "TexA", "RasC", "RasA", "1", ".5", "K", "0" };
    static const char* const ain[8] = { "Ap", "A0", "A1", "A2", "TexA", "RasA", "K", "0" };
    static const char* const dst[4] = { "PREV", "C0", "C1", "C2" };
    u32 n = ((g->bp[0] >> 10) & 15) + 1;
    printf("    GX: %u stages\n", n);
    for (u32 s = 0; s < n; s++) {
        u32 tref = (g->bp[0x28 + s / 2] >> ((s & 1) * 12)) & 0xFFF, ks = g->bp[0xF6 + s / 2];
        u32 ce = g->bp[0xC0 + 2 * s], ae = g->bp[0xC1 + 2 * s];
        printf("      %u: tex%s map%u uv%u ras%u kc%u ka%u tsw%u rsw%u | rgb (%s,%s,%s,%s) bias%u op%u clamp%u scale%u -> %s | a (%s,%s,%s,%s) bias%u op%u clamp%u scale%u -> %s\n",
               s, (tref >> 6) & 1 ? "+" : "-", tref & 7, (tref >> 3) & 7, (tref >> 7) & 7, (ks >> (4 + (s & 1) * 10)) & 31, (ks >> (9 + (s & 1) * 10)) & 31,
               (ae >> 2) & 3, ae & 3, cin[(ce >> 12) & 15], cin[(ce >> 8) & 15], cin[(ce >> 4) & 15], cin[ce & 15], (ce >> 16) & 3, (ce >> 18) & 1,
               (ce >> 19) & 1, (ce >> 20) & 3, dst[(ce >> 22) & 3], ain[(ae >> 13) & 7], ain[(ae >> 10) & 7], ain[(ae >> 7) & 7], ain[(ae >> 4) & 7],
               (ae >> 16) & 3, (ae >> 18) & 1, (ae >> 19) & 1, (ae >> 20) & 3, dst[(ae >> 22) & 3]);
    }
    for (int r = 0; r < 4; r++) printf("      reg%d %08x %08x  konst %08x %08x\n", r, g->tevLo[r], g->tevHi[r], g->kLo[r], g->kHi[r]);
}

static int g_fails;

// Allowed error for a case reported as exact. Each GX stage can leave up to half a level of arithmetic mismatch (the GX
// lerp weight (c + (c >= 128)) / 256 against c / 255, konst fractions, constants folded to 8 bits), and every later
// stage's scale 2 or 4 multiplies it, so: 1.5 + 0.5 * sum over stages of the gain from that stage to the end. One
// stage at scale 1 gives the 2/255 of 8-bit rounding. Random inputs also let a product of two mismatched values (a
// stage multiplying a value by itself doubles it) compound along a chain, so that bound grows by half per extra stage;
// the 0/255 inputs of g_bin keep the mismatch out and use the base bound at any depth. Faults show as
// tens of levels either way.
static float tolerance(const GXCore* g) {
    float sum = 0, suffix = 1;
    for (int s = (int)((g->bp[0] >> 10) & 15); s >= 0; s--) {
        u32 cs = (g->bp[0xC0 + 2 * s] >> 20) & 3, as = (g->bp[0xC1 + 2 * s] >> 20) & 3;
        u32 m = (cs < 3 ? cs : 0) > (as < 3 ? as : 0) ? (cs < 3 ? cs : 0) : (as < 3 ? as : 0);
        suffix *= (float)(1u << m);
        sum += suffix;
    }
    float base = 1.5f + 0.5f * sum;
    return g_bin ? base : base * (1.0f + 0.5f * (float)((g->bp[0] >> 10) & 15));
}

typedef struct {
    int n, exact, exactFail, approx, exactBuf, exactTwoStage;
    float maxExact, maxApprox, maxQExact;
    int flagCount[8];
    int stagesHist[8];
} Stats;

static void stats_add(Stats* st, const PicaTev* t, float err, float qerr, float tol) {
    u32 approx = t->approx;
    int numStages = t->numStages;
    st->n++;
    st->stagesHist[numStages]++;
    if (!approx) {
        st->exact++;
        if (t->bufRgb || t->bufA) st->exactBuf++;
        if (numStages > 1) st->exactTwoStage++;
        if (err > st->maxExact) st->maxExact = err;
        if (qerr > st->maxQExact) st->maxQExact = qerr;
        if (err > tol) st->exactFail++;
    } else {
        st->approx++;
        if (err > st->maxApprox) st->maxApprox = err;
    }
    for (int i = 0; i < 8; i++) if (approx & (1u << i)) st->flagCount[i]++;
}

// Hand-written case: compile, compare on 64 random input sets, print one line.
static void run_case(const char* name, bool expectExact, int expectStages, const GXCore* g) {
    PicaTev t;
    tev_pica_compile(g, &t);
    if (g_plant) plant(&t);
    float q;
    float err = max_error(g, &t, 64, &q);
    char fl[128];
    flags_str(t.approx, fl);
    const char* verdict = "PASS";
    if (t.approx == 0 && err > tolerance(g)) verdict = "FAIL";
    else if (t.approx) verdict = "approx";
    if (expectExact && t.approx) verdict = "FAIL(unexpected approx)";
    if (expectStages && t.numStages != expectStages) verdict = "FAIL(stage count)";
    if (verdict[0] == 'F') g_fails++;
    printf("  %-8s %-34s stages=%d err=%5.2f qerr=%5.2f %s\n", verdict, name, t.numStages, err, q, fl);
    if (verdict[0] == 'F') dump_pica(&t);
}

// ---- hand-written cases ----
static void hand_cases(void) {
    GXCore* g = &g_gx;
    Stage s;

#define BEGIN(n) gx_reset(g); set_count(g, n); set_reg(g, 0, 0, 0, 0, 0); set_konst(g, 0, 200, 90, 40, 160); set_konst(g, 1, 20, 220, 130, 77); set_konst(g, 2, 255, 0, 128, 10); set_konst(g, 3, 5, 6, 7, 8)
    BEGIN(1); s = mk_tex(0, mk(CC_ZERO, CC_TEXC, CC_RASC, CC_ZERO, CA_ZERO, CA_TEXA, CA_RASA, CA_ZERO)); set_stage(g, 0, &s);
    run_case("modulate tex*ras", true, 1, g);

    BEGIN(1); s = mk_tex(0, mk(CC_ZERO, CC_ZERO, CC_ZERO, CC_TEXC, CA_ZERO, CA_ZERO, CA_ZERO, CA_TEXA)); set_stage(g, 0, &s);
    run_case("replace tex", true, 1, g);

    BEGIN(1); s = mk(CC_ZERO, CC_ZERO, CC_ZERO, CC_RASC, CA_ZERO, CA_ZERO, CA_ZERO, CA_RASA); set_stage(g, 0, &s);
    run_case("replace ras (pass colour)", true, 1, g);

    BEGIN(1); s = mk_tex(0, mk(CC_RASC, CC_TEXC, CC_TEXA, CC_ZERO, CA_ZERO, CA_ZERO, CA_ZERO, CA_RASA)); set_stage(g, 0, &s);
    run_case("decal (lerp ras,tex by tex a)", true, 1, g);

    BEGIN(1); s = mk_tex(0, mk(CC_RASC, CC_ONE, CC_TEXC, CC_ZERO, CA_ZERO, CA_TEXA, CA_RASA, CA_ZERO)); set_stage(g, 0, &s);
    run_case("blend (lerp ras,one by tex)", true, 1, g);

    BEGIN(1); s = mk_tex(0, mk(CC_ZERO, CC_TEXC, CC_ONE, CC_RASC, CA_ZERO, CA_TEXA, CA_KONST, CA_RASA)); s.ka = KC_1; set_stage(g, 0, &s);
    run_case("add tex+ras", true, 1, g);

    BEGIN(1); s = mk_tex(0, mk(CC_ZERO, CC_TEXC, CC_KONST, CC_ZERO, CA_ZERO, CA_TEXA, CA_KONST, CA_ZERO)); s.kc = KC_K0; s.ka = KC_K0_A; set_stage(g, 0, &s);
    run_case("konst modulate (K0 vector, K0.a)", true, 1, g);

    BEGIN(1); s = mk_tex(0, mk(CC_ZERO, CC_TEXC, CC_KONST, CC_ZERO, CA_ZERO, CA_TEXA, CA_KONST, CA_ZERO)); s.kc = KC_K0_G + 1; s.ka = KC_K0_B + 2; set_stage(g, 0, &s);
    run_case("konst scalars (K1.g, K2.b)", true, 1, g);

    BEGIN(1); s = mk_tex(0, mk(CC_ZERO, CC_TEXC, CC_KONST, CC_ZERO, CA_ZERO, CA_TEXA, CA_KONST, CA_ZERO)); s.kc = 3; s.ka = 5; set_stage(g, 0, &s);
    run_case("konst fractions (5/8, 3/8)", true, 1, g);

    BEGIN(2);
    s = mk_tex(0, mk(CC_ZERO, CC_TEXC, CC_RASC, CC_ZERO, CA_ZERO, CA_TEXA, CA_RASA, CA_ZERO)); set_stage(g, 0, &s);
    s = mk(CC_ZERO, CC_CPREV, CC_KONST, CC_ZERO, CA_ZERO, CA_APREV, CA_KONST, CA_ZERO); s.kc = KC_K1; s.ka = (KC_K0_A + 1); set_stage(g, 1, &s);
    run_case("two stages through PREV", true, 2, g);

    BEGIN(2);
    s = mk_tex(0, mk(CC_ZERO, CC_TEXC, CC_RASC, CC_ZERO, CA_ZERO, CA_TEXA, CA_RASA, CA_ZERO)); set_stage(g, 0, &s);
    s = mk(CC_ZERO, CC_CPREV, CC_KONST, CC_ZERO, CA_ZERO, CA_APREV, CA_KONST, CA_ZERO); s.kc = KC_K1; s.ka = KC_K0_A + 1; s.cscale = 1; set_stage(g, 1, &s);
    run_case("PREV chain, scale 2", true, 2, g);

    BEGIN(3);
    s = mk_tex(0, mk(CC_ZERO, CC_TEXC, CC_RASC, CC_ZERO, CA_ZERO, CA_TEXA, CA_RASA, CA_ZERO)); s.cdest = DST_C0; s.adest = DST_C0; set_stage(g, 0, &s);
    s = mk_tex(1, mk(CC_ZERO, CC_TEXC, CC_KONST, CC_ZERO, CA_ZERO, CA_TEXA, CA_KONST, CA_ZERO)); s.kc = KC_K1; s.ka = (KC_K0_A + 1); set_stage(g, 1, &s);
    s = mk(CC_ZERO, CC_C0, CC_ONE, CC_CPREV, CA_ZERO, CA_A0, CA_KONST, CA_APREV); s.ka = KC_1; set_stage(g, 2, &s);
    run_case("C0 written, read two stages later", true, 3, g);

    BEGIN(2);
    s = mk_tex(0, mk(CC_ZERO, CC_TEXC, CC_RASC, CC_ZERO, CA_ZERO, CA_TEXA, CA_RASA, CA_ZERO)); s.cdest = DST_C0; s.adest = DST_C0; set_stage(g, 0, &s);
    s = mk_tex(1, mk(CC_C0, CC_TEXC, CC_TEXA, CC_ZERO, CA_A0, CA_ZERO, CA_ZERO, CA_ZERO)); set_stage(g, 1, &s);
    run_case("C0 written, read next stage", true, 2, g);

    BEGIN(3);
    s = mk_tex(0, mk(CC_ZERO, CC_TEXC, CC_RASC, CC_ZERO, CA_ZERO, CA_TEXA, CA_RASA, CA_ZERO)); set_stage(g, 0, &s);
    s = mk_tex(1, mk(CC_ZERO, CC_TEXC, CC_RASC, CC_ZERO, CA_ZERO, CA_TEXA, CA_RASA, CA_ZERO)); s.cdest = DST_C1; s.adest = DST_C1; set_stage(g, 1, &s);
    s = mk(CC_ZERO, CC_CPREV, CC_C1, CC_ZERO, CA_ZERO, CA_APREV, CA_A1, CA_ZERO); set_stage(g, 2, &s);
    run_case("PREV displaced by C1, both read", true, 3, g);

    BEGIN(1); s = mk_tex(0, mk(CC_ZERO, CC_TEXC, CC_RASC, CC_ZERO, CA_ZERO, CA_TEXA, CA_RASA, CA_ZERO)); s.tswap = 1; set_stage(g, 0, &s);
    set_swap(g, 0, 0, 1, 2, 3); set_swap(g, 1, 3, 3, 3, 3);
    run_case("swap table: alpha replicate (IA8)", true, 1, g);

    BEGIN(1); s = mk_tex(0, mk(CC_ZERO, CC_TEXC, CC_RASC, CC_ZERO, CA_ZERO, CA_TEXA, CA_RASA, CA_ZERO)); s.tswap = 2; set_stage(g, 0, &s);
    set_swap(g, 0, 0, 1, 2, 3); set_swap(g, 2, 0, 0, 0, 3);
    run_case("swap table: red replicate, alpha kept", true, 1, g);

    BEGIN(1); s = mk_tex(0, mk(CC_ZERO, CC_TEXC, CC_RASC, CC_ZERO, CA_ZERO, CA_TEXA, CA_RASA, CA_ZERO)); s.tswap = 1; s.rswap = 1; set_stage(g, 0, &s);
    set_swap(g, 0, 0, 1, 2, 3); set_swap(g, 1, 1, 1, 1, 0);
    run_case("swap tables on tex and ras", true, 1, g);

    BEGIN(1); s = mk_tex(0, mk(CC_ZERO, CC_TEXC, CC_RASC, CC_ZERO, CA_ZERO, CA_TEXA, CA_RASA, CA_ZERO)); s.tswap = 1; set_stage(g, 0, &s);
    set_swap(g, 0, 0, 1, 2, 3); set_swap(g, 1, 2, 1, 0, 3);
    run_case("swap table: BGR (unrepresentable)", false, 1, g);

    BEGIN(3);  // a typical 3-stage game TEV: base * vertex colour, lightmap * 2, fog towards a constant colour
    set_reg(g, 1, 90, 130, 200, 0);
    s = mk_tex(0, mk(CC_ZERO, CC_TEXC, CC_RASC, CC_ZERO, CA_ZERO, CA_TEXA, CA_RASA, CA_ZERO)); set_stage(g, 0, &s);
    s = mk_tex(1, mk(CC_ZERO, CC_TEXC, CC_CPREV, CC_ZERO, CA_ZERO, CA_ZERO, CA_ZERO, CA_APREV)); s.cscale = 1; set_stage(g, 1, &s);
    s = mk(CC_CPREV, CC_C0, CC_KONST, CC_ZERO, CA_ZERO, CA_ZERO, CA_ZERO, CA_APREV); s.kc = (KC_K0_A + 1); set_stage(g, 2, &s);
    set_reg(g, 1, 90, 130, 200, 0);
    run_case("3-stage game TEV (base, lightmap, fog)", true, 3, g);

    BEGIN(1); s = mk_tex(0, mk(CC_ZERO, CC_TEXC, CC_ONE, CC_RASC, CA_ZERO, CA_TEXA, CA_KONST, CA_ZERO)); s.cbias = 2; s.cscale = 1; s.ka = KC_1; set_stage(g, 0, &s);
    run_case("add signed x2", true, 1, g);

    BEGIN(1); s = mk_tex(0, mk(CC_ZERO, CC_TEXC, CC_ONE, CC_RASC, CA_ZERO, CA_ZERO, CA_ZERO, CA_RASA)); s.cop = 1; set_stage(g, 0, &s);
    run_case("subtract ras - tex", true, 1, g);

    BEGIN(1); s = mk_tex(0, mk(CC_ZERO, CC_TEXC, CC_RASC, CC_ZERO, CA_ZERO, CA_TEXA, CA_RASA, CA_ZERO)); s.cscale = 2; s.ascale = 2; set_stage(g, 0, &s);
    run_case("modulate x4", true, 1, g);

    BEGIN(1); s = mk_tex(0, mk(CC_RASC, CC_ZERO, CC_TEXC, CC_ZERO, CA_RASA, CA_ZERO, CA_TEXA, CA_ZERO)); set_stage(g, 0, &s);
    run_case("ras * (1 - tex)", true, 1, g);

    BEGIN(1); s = mk_tex(0, mk(CC_ZERO, CC_ZERO, CC_ZERO, CC_RASC, CA_ZERO, CA_ZERO, CA_ZERO, CA_ZERO)); s.texEn = 0; set_stage(g, 0, &s);
    run_case("alpha ZERO, colour pass", true, 1, g);

    BEGIN(1); s = mk(CC_ZERO, CC_TEXC, CC_RASC, CC_ZERO, CA_ZERO, CA_TEXA, CA_RASA, CA_ZERO); set_stage(g, 0, &s);
    run_case("texture disabled reads white", true, 1, g);

    BEGIN(1); s = mk_tex(0, mk(CC_C0, CC_C1, CC_TEXA, CC_ZERO, CA_A0, CA_A1, CA_TEXA, CA_ZERO)); set_stage(g, 0, &s);
    set_reg(g, 1, 240, 20, 100, 30); set_reg(g, 2, 10, 200, 60, 250);
    run_case("lerp two register constants", true, 0, g);

    BEGIN(1); s = mk_tex(0, mk(CC_C0, CC_KONST, CC_TEXC, CC_ZERO, CA_A0, CA_KONST, CA_TEXA, CA_ZERO)); s.kc = KC_K0; s.ka = KC_K0_A; set_stage(g, 0, &s);
    set_reg(g, 1, 240, 20, 100, 30);
    run_case("lerp register and konst vector", true, 0, g);

    BEGIN(2);  // general lerp + d where d is the previous stage's output
    s = mk_tex(1, mk(CC_ZERO, CC_TEXC, CC_RASC, CC_ZERO, CA_ZERO, CA_TEXA, CA_RASA, CA_ZERO)); set_stage(g, 0, &s);
    s = mk_tex(0, mk(CC_RASC, CC_TEXC, CC_TEXA, CC_CPREV, CA_RASA, CA_TEXA, CA_KONST, CA_APREV)); s.ka = 4; set_stage(g, 1, &s);
    run_case("d + lerp(a,b,c), d = PREV", true, 0, g);

    BEGIN(1); s = mk_tex(0, mk(CC_RASC, CC_TEXC, CC_TEXA, CC_C0, CA_RASA, CA_TEXA, CA_KONST, CA_A0)); s.ka = 3; set_stage(g, 0, &s);
    set_reg(g, 1, 20, 40, 60, 80);
    run_case("d + lerp(a,b,c), d = register const", true, 1 + 1, g);

    BEGIN(2);  // lerp result read as alpha-of-colour
    s = mk_tex(0, mk(CC_ZERO, CC_TEXC, CC_RASC, CC_ZERO, CA_ZERO, CA_TEXA, CA_RASA, CA_ZERO)); set_stage(g, 0, &s);
    s = mk_tex(1, mk(CC_ZERO, CC_APREV, CC_TEXC, CC_ZERO, CA_APREV, CA_ZERO, CA_ZERO, CA_ZERO)); set_stage(g, 1, &s);
    run_case("previous alpha used as colour", true, 2, g);

    BEGIN(2);  // alpha-only second stage
    s = mk_tex(0, mk(CC_ZERO, CC_TEXC, CC_RASC, CC_ZERO, CA_ZERO, CA_TEXA, CA_RASA, CA_ZERO)); set_stage(g, 0, &s);
    s = mk(CC_ZERO, CC_ZERO, CC_ZERO, CC_CPREV, CA_ZERO, CA_APREV, CA_KONST, CA_ZERO); s.ka = KC_K0_A; set_stage(g, 1, &s);
    run_case("alpha-only change in stage 2", true, 2, g);

    BEGIN(2);  // stage 0 colour into PREV via alias (konst), then used
    s = mk(CC_ZERO, CC_ZERO, CC_ZERO, CC_KONST, CA_ZERO, CA_ZERO, CA_ZERO, CA_KONST); s.kc = KC_K0; s.ka = KC_K0_A; set_stage(g, 0, &s);
    s = mk_tex(0, mk(CC_ZERO, CC_TEXC, CC_CPREV, CC_ZERO, CA_ZERO, CA_TEXA, CA_APREV, CA_ZERO)); set_stage(g, 1, &s);
    run_case("konst into PREV, then modulate", true, 1, g);

    BEGIN(1); s = mk(CC_ZERO, CC_ZERO, CC_ZERO, CC_C1, CA_ZERO, CA_ZERO, CA_ZERO, CA_A1); set_stage(g, 0, &s);
    set_reg(g, 2, 33, 66, 99, 12);
    run_case("pass register constant", true, 1, g);

    BEGIN(2); s = mk_tex(0, mk(CC_ZERO, CC_TEXC, CC_RASC, CC_ZERO, CA_ZERO, CA_TEXA, CA_RASA, CA_ZERO)); s.cdest = DST_C2; s.adest = DST_C2; set_stage(g, 0, &s);
    s = mk(CC_ZERO, CC_ZERO, CC_ZERO, CC_C2, CA_ZERO, CA_ZERO, CA_ZERO, CA_A2); set_stage(g, 1, &s);
    run_case("final PREV copied from C2", true, 0, g);

    BEGIN(1); s = mk_tex(0, mk(CC_ZERO, CC_TEXC, CC_RASC, CC_ZERO, CA_ZERO, CA_TEXA, CA_RASA, CA_ZERO)); s.cdest = DST_C0; s.adest = DST_C0; set_stage(g, 0, &s);
    set_reg(g, 0, 70, 80, 90, 100);
    run_case("last stage writes C0: PREV is the preset", true, 0, g);

    printf(" -- approximations (reported, not failed)\n");
    BEGIN(2);
    s = mk_tex(0, mk(CC_ZERO, CC_TEXC, CC_RASC, CC_ZERO, CA_ZERO, CA_TEXA, CA_RASA, CA_ZERO)); s.chan = 0; set_stage(g, 0, &s);
    s = mk_tex(0, mk(CC_ZERO, CC_CPREV, CC_RASC, CC_ZERO, CA_ZERO, CA_APREV, CA_RASA, CA_ZERO)); s.chan = 1; set_stage(g, 1, &s);
    run_case("both colour channels", false, 2, g);

    BEGIN(1); s = mk_tex(0, mk(CC_ZERO, CC_TEXC, CC_RASC, CC_ONE, CA_ZERO, CA_TEXA, CA_RASA, CA_ZERO)); s.cbias = 1; set_stage(g, 0, &s);
    run_case("bias +0.5", false, 0, g);

    BEGIN(1); s = mk_tex(0, mk(CC_TEXC, CC_RASC, CC_TEXA, CC_ZERO, CA_ZERO, CA_TEXA, CA_RASA, CA_ZERO)); s.cscale = 3; set_stage(g, 0, &s);
    run_case("scale 0.5", false, 0, g);

    BEGIN(1); s = mk_tex(0, mk(CC_TEXC, CC_RASC, CC_TEXA, CC_ZERO, CA_ZERO, CA_TEXA, CA_RASA, CA_ZERO)); s.cbias = 3; s.cscale = 3; s.cop = 0; set_stage(g, 0, &s);
    run_case("compare (A > B ? C : 0)", false, 0, g);

    BEGIN(2);
    s = mk_tex(0, mk(CC_ZERO, CC_TEXC, CC_RASC, CC_ZERO, CA_ZERO, CA_TEXA, CA_RASA, CA_ZERO)); s.cclamp = 0; s.cop = 1; set_stage(g, 0, &s);
    s = mk(CC_ZERO, CC_CPREV, CC_ONE, CC_ZERO, CA_ZERO, CA_APREV, CA_KONST, CA_ZERO); s.ka = KC_1; set_stage(g, 1, &s);
    run_case("clamp off, consumed", false, 0, g);

    BEGIN(4);
    for (int i = 0; i < 4; i++) {
        s = mk_tex((u8)i, mk(CC_ZERO, CC_TEXC, i ? CC_CPREV : CC_RASC, CC_ZERO, CA_ZERO, CA_TEXA, i ? CA_APREV : CA_RASA, CA_ZERO));
        set_stage(g, i, &s);
    }
    run_case("4 texture units", false, 0, g);

    BEGIN(9);
    for (int i = 0; i < 9; i++) {
        s = mk_tex(0, mk(CC_ZERO, CC_TEXC, i ? CC_CPREV : CC_RASC, CC_ZERO, CA_ZERO, CA_TEXA, i ? CA_APREV : CA_RASA, CA_ZERO));
        s.cscale = 1; set_stage(g, i, &s);
    }
    run_case("9 stages (drops past 6)", false, 0, g);

    BEGIN(8);  // 8 PREV-chained modulates of konst: still fits after folding? no: each needs a stage
    for (int i = 0; i < 8; i++) {
        s = mk(CC_ZERO, i ? CC_CPREV : CC_RASC, CC_KONST, CC_ZERO, CA_ZERO, i ? CA_APREV : CA_RASA, CA_KONST, CA_ZERO);
        s.kc = KC_K0 + (u8)(i & 3); s.ka = KC_K0_A + (u8)(i & 3); set_stage(g, i, &s);
    }
    run_case("8 konst modulates", false, 0, g);
#undef BEGIN
}

// ---- fuzz ----
static void rand_regs(GXCore* g, bool wild) {
    if (g_bin) {
        for (int r = 0; r < 4; r++) {
            int v[8];
            for (int k = 0; k < 8; k++) v[k] = rnd() & 1 ? 255 : 0;
            set_reg(g, r, v[0], v[1], v[2], v[3]);
            set_konst(g, r, v[4], v[5], v[6], v[7]);
        }
        return;
    }
    for (int r = 0; r < 4; r++) {
        if (wild && chance(15)) set_reg(g, r, (int)rbits(11) - 1024, (int)rbits(11) - 1024, (int)rbits(11) - 1024, (int)rbits(11) - 1024);
        else if (chance(30)) set_reg(g, r, 0, 0, 0, 0);
        else set_reg(g, r, (int)rbits(8), (int)rbits(8), (int)rbits(8), (int)rbits(8));
        set_konst(g, r, (int)rbits(8), (int)rbits(8), (int)rbits(8), (int)rbits(8));
    }
}

// A random input pick biased to what games use.
static u8 pick_c(int st) {
    static const u8 common[] = { CC_CPREV, CC_APREV, CC_C0, CC_C1, CC_TEXC, CC_TEXC, CC_TEXA, CC_RASC, CC_RASC, CC_RASA, CC_ONE, CC_HALF, CC_KONST, CC_KONST, CC_ZERO };
    u8 v = common[rnd() % sizeof common];
    if (st == 0 && (v == CC_CPREV || v == CC_APREV) && chance(80)) v = CC_TEXC;
    return v;
}
static u8 pick_a(int st) {
    static const u8 common[] = { CA_APREV, CA_APREV, CA_A0, CA_TEXA, CA_TEXA, CA_RASA, CA_RASA, CA_KONST, CA_ZERO };
    u8 v = common[rnd() % sizeof common];
    if (st == 0 && v == CA_APREV && chance(80)) v = CA_TEXA;
    return v;
}

static void fuzz_game(GXCore* g) {
    gx_reset(g);
    int n = 1 + (int)(rnd() % 6);
    set_count(g, n);
    rand_regs(g, false);
    for (int t = 0; t < 4; t++) set_swap(g, t, 0, 1, 2, 3);
    if (chance(15)) set_swap(g, 1, 3, 3, 3, 3);
    if (chance(10)) set_swap(g, 2, 0, 0, 0, 3);
    for (int st = 0; st < n; st++) {
        Stage s = stage_new();
        s.texmap = (u8)(rnd() % 3); s.texcoord = s.texmap; s.texEn = chance(85);
        s.chan = chance(95) ? 0 : 1;
        s.kc = chance(50) ? (u8)(KC_K0 + rnd() % 4) : (u8)(rnd() % 32); s.ka = chance(50) ? (u8)(KC_K0_A + rnd() % 4) : (u8)(rnd() % 32);
        s.tswap = chance(90) ? 0 : (u8)(1 + rnd() % 2); s.rswap = 0;
        int tmpl = (int)(rnd() % 10);
        u8 x = pick_c(st), y = pick_c(st), z = pick_c(st);
        switch (tmpl) {
        case 0: case 1: case 2: s.c[1] = x; s.c[2] = y; break;                                   // modulate
        case 3: s.c[3] = x; break;                                                               // replace
        case 4: s.c[3] = x; s.c[1] = y; s.c[2] = CC_ONE; break;                                  // add
        case 5: s.c[0] = x; s.c[1] = y; s.c[2] = z; break;                                       // lerp
        case 6: s.c[1] = x; s.c[2] = y; s.c[3] = z; break;                                       // multiply-add
        case 7: s.c[0] = x; s.c[1] = y; s.c[2] = z; s.c[3] = pick_c(st); break;                  // general
        default: s.c[1] = x; s.c[2] = y; break;
        }
        int ta = (int)(rnd() % 6);
        u8 p = pick_a(st), q = pick_a(st), r = pick_a(st);
        switch (ta) {
        case 0: case 1: s.a[1] = p; s.a[2] = q; break;
        case 2: s.a[3] = p; break;
        case 3: s.a[3] = p; s.a[1] = q; s.a[2] = CA_KONST; s.ka = KC_1; break;
        case 4: s.a[0] = p; s.a[1] = q; s.a[2] = r; break;
        default: s.a[1] = p; s.a[2] = q; s.a[3] = r; break;
        }
        if (chance(15)) s.cdest = (u8)(1 + rnd() % 3);
        s.adest = chance(70) ? s.cdest : (chance(50) ? 0 : (u8)(rnd() % 4));
        if (chance(15)) s.cscale = (u8)(1 + rnd() % 2);
        if (chance(15)) s.ascale = (u8)(1 + rnd() % 2);
        if (chance(8)) s.cbias = 2;
        if (chance(8)) s.abias = 2;
        if (chance(5)) s.cop = 1;
        if (chance(5)) s.aop = 1;
        if (chance(5)) s.cclamp = 0;
        if (chance(5)) s.aclamp = 0;
        set_stage(g, st, &s);
    }
}

// Register-heavy programs: most stages write C0-C2 and later stages read them back, which exercises PREVIOUS, the
// combiner buffer and its write/read timing. Registers start at zero (so reads of unwritten ones fold away) or at constants.
static void fuzz_regs(GXCore* g) {
    gx_reset(g);
    int n = 2 + (int)(rnd() % 5);
    set_count(g, n);
    rand_regs(g, false);
    for (int t = 0; t < 4; t++) set_swap(g, t, 0, 1, 2, 3);
    static const u8 cpick[] = { CC_CPREV, CC_CPREV, CC_C0, CC_C0, CC_C1, CC_C2, CC_TEXC, CC_TEXC, CC_RASC, CC_APREV, CC_A0, CC_TEXA, CC_ONE, CC_ZERO };
    static const u8 apick[] = { CA_APREV, CA_APREV, CA_A0, CA_A0, CA_A1, CA_A2, CA_TEXA, CA_RASA, CA_ZERO };
    for (int st = 0; st < n; st++) {
        Stage s = stage_new();
        s.texmap = (u8)(rnd() % 3); s.texcoord = s.texmap; s.texEn = chance(80);
        s.kc = (u8)(KC_K0 + rnd() % 4); s.ka = (u8)(KC_K0_A + rnd() % 4);
        int tmpl = (int)(rnd() % 4);
        u8 x = cpick[rnd() % sizeof cpick], y = cpick[rnd() % sizeof cpick], z = cpick[rnd() % sizeof cpick];
        switch (tmpl) {
        case 0: s.c[1] = x; s.c[2] = y; break;
        case 1: s.c[3] = x; s.c[1] = y; s.c[2] = CC_ONE; break;
        case 2: s.c[0] = x; s.c[1] = y; s.c[2] = z; break;
        default: s.c[3] = x; break;
        }
        u8 p = apick[rnd() % sizeof apick], q = apick[rnd() % sizeof apick];
        switch (rnd() % 3) {
        case 0: s.a[1] = p; s.a[2] = q; break;
        case 1: s.a[3] = p; break;
        default: s.a[3] = p; s.a[1] = q; s.a[2] = CA_KONST; s.ka = KC_1; break;
        }
        s.cdest = chance(55) ? (u8)(1 + rnd() % 3) : 0;
        s.adest = chance(75) ? s.cdest : (u8)(rnd() % 4);
        if (chance(10)) s.cscale = 1;
        set_stage(g, st, &s);
    }
}

// Programs without dead code: every stage reads values written by earlier stages (any of PREV, C0-C2, both tracks), so
// values stay live across several stages and compete for PREVIOUS and the one combiner buffer.
static void fuzz_live(GXCore* g) {
    gx_reset(g);
    int n = 3 + (int)(rnd() % 4);
    set_count(g, n);
    rand_regs(g, false);
    for (int t = 0; t < 4; t++) set_swap(g, t, 0, 1, 2, 3);
    bool wr[4] = { false, false, false, false };  // registers written so far (the colour track decides; alpha mostly follows)
    for (int st = 0; st < n; st++) {
        Stage s = stage_new();
        s.texmap = (u8)(rnd() % 3); s.texcoord = s.texmap; s.texEn = chance(85);
        s.kc = (u8)(KC_K0 + rnd() % 4); s.ka = (u8)(KC_K0_A + rnd() % 4);
        u8 regC[4] = { CC_CPREV, CC_C0, CC_C1, CC_C2 }, regA[4] = { CC_APREV, CC_A0, CC_A1, CC_A2 };
        u8 pickC[24], pickA[12];
        int nc = 0, na = 0;
        for (int r = 0; r < 4; r++) if (wr[r]) { pickC[nc++] = regC[r]; pickC[nc++] = regC[r]; pickC[nc++] = regA[r]; pickA[na++] = (u8)r; pickA[na++] = (u8)r; }
        pickC[nc++] = CC_TEXC; pickC[nc++] = CC_RASC; pickC[nc++] = CC_ONE;
        pickA[na++] = CA_TEXA; pickA[na++] = CA_RASA;
        u8 x = pickC[rnd() % nc], y = pickC[rnd() % nc], z = pickC[rnd() % nc];
        switch (rnd() % 4) {
        case 0: s.c[1] = x; s.c[2] = y; break;
        case 1: s.c[3] = x; s.c[1] = y; s.c[2] = CC_ONE; break;
        case 2: s.c[0] = x; s.c[1] = y; s.c[2] = z; break;
        default: s.c[1] = x; s.c[2] = y; s.c[3] = z; break;
        }
        u8 p = pickA[rnd() % na], q = pickA[rnd() % na], r = pickA[rnd() % na];
        switch (rnd() % 3) {
        case 0: s.a[1] = p; s.a[2] = q; break;
        case 1: s.a[3] = p; s.a[1] = q; s.a[2] = CA_KONST; s.ka = KC_1; break;
        default: s.a[1] = p; s.a[2] = q; s.a[3] = r; break;
        }
        s.cdest = st == n - 1 ? 0 : (u8)(rnd() % 4);
        s.adest = chance(70) ? s.cdest : (u8)(rnd() % 4);
        wr[s.cdest] = true;
        if (s.adest != s.cdest) wr[s.adest] = true;
        if (chance(10)) s.cscale = 1;
        set_stage(g, st, &s);
    }
}

static void fuzz_wild(GXCore* g) {
    gx_reset(g);
    int n = 1 + (int)(rnd() % 16);
    set_count(g, n);
    rand_regs(g, true);
    for (int t = 0; t < 4; t++) set_swap(g, t, (int)(rnd() % 4), (int)(rnd() % 4), (int)(rnd() % 4), (int)(rnd() % 4));
    for (int st = 0; st < n; st++) {
        Stage s = stage_new();
        s.texmap = (u8)(rnd() % 8); s.texcoord = (u8)(rnd() % 8); s.texEn = chance(70);
        static const u8 chans[] = { 0, 0, 0, 1, 5, 6, 7 };
        s.chan = chans[rnd() % 7];
        s.kc = (u8)(rnd() % 32); s.ka = (u8)(rnd() % 32);
        s.rswap = (u8)(rnd() % 4); s.tswap = (u8)(rnd() % 4);
        for (int i = 0; i < 4; i++) { s.c[i] = (u8)(rnd() % 16); s.a[i] = (u8)(rnd() % 8); }
        s.cbias = (u8)(rnd() % 4); s.cop = (u8)(rnd() % 2); s.cclamp = (u8)chance(80); s.cscale = (u8)(rnd() % 4); s.cdest = (u8)(rnd() % 4);
        s.abias = (u8)(rnd() % 4); s.aop = (u8)(rnd() % 2); s.aclamp = (u8)chance(80); s.ascale = (u8)(rnd() % 4); s.adest = (u8)(rnd() % 4);
        set_stage(g, st, &s);
    }
}

static void print_stats(const char* title, const Stats* st) {
    printf(" == %s: %d cases, %d exact (%.1f%%), %d exact cases over their tolerance, max exact error %.2f (with per-stage 8-bit rounding %.2f), max approximated error %.1f\n",
           title, st->n, st->exact, st->n ? 100.0 * st->exact / st->n : 0.0, st->exactFail, st->maxExact, st->maxQExact, st->maxApprox);
    printf("    exact cases using the combiner buffer: %d, using more than one PICA stage: %d\n", st->exactBuf, st->exactTwoStage);
    printf("    approximation flags (cases):");
    for (int i = 0; i < 8; i++) printf(" %s=%d", kFlag[i], st->flagCount[i]);
    printf("\n    PICA stages:");
    for (int i = 1; i <= PICA_STAGES; i++) printf(" %d:%d", i, st->stagesHist[i]);
    printf("\n");
}

static Stats run_fuzz(const char* title, void (*gen)(GXCore*), int cases) {
    Stats st;
    memset(&st, 0, sizeof st);
    int shown = 0, shownFlag = 0;
    for (int i = 0; i < cases; i++) {
        g_bin = i & 1;
        gen(&g_gx);
        PicaTev t;
        tev_pica_compile(&g_gx, &t);
        if (g_plant) plant(&t);
        float q;
        float err = max_error(&g_gx, &t, 4, &q);
        stats_add(&st, &t, err, q, tolerance(&g_gx));
        const char* show = getenv("TEV_SHOW");  // TEV_SHOW=<flag number>: also list the first cases that set only that approximation flag
        if (show && t.approx == (1u << atoi(show)) && shownFlag++ < 4) {
            printf("  case %d (%s) approx %x: err %.2f\n", i, title, t.approx, err);
            dump_pica(&t);
            dump_gx(&g_gx);
        }
        if (!t.approx && err > tolerance(&g_gx)) {
            g_fails++;
            if (shown++ < 3) {
                printf("  FAIL %s fuzz case %d: err %.2f\n", title, i, err);
                dump_pica(&t);
                dump_gx(&g_gx);
            }
        }
    }
    g_bin = false;
    return st;
}

// The comparison must be able to fail: corrupt single stages of known-exact results and require detection.
static int self_test(void) {
    GXCore* g = &g_gx;
    int detected = 0, tried = 0;
    for (int kind = 0; kind < 4; kind++) {
        gx_reset(g);
        set_count(g, 1);
        Stage s = mk_tex(0, mk(CC_ZERO, CC_TEXC, CC_RASC, CC_ZERO, CA_ZERO, CA_TEXA, CA_RASA, CA_ZERO));
        set_stage(g, 0, &s);
        PicaTev t;
        tev_pica_compile(g, &t);
        if (t.approx || max_error(g, &t, 64, NULL) > TOL) { printf("  self-test: baseline not exact\n"); return 0; }
        PicaStage* p = &t.st[0];
        switch (kind) {
        case 0: p->opRgb[0] ^= 1; break;                  // inverted operand
        case 1: p->scaleA = PSCALE_2; break;              // wrong scale
        case 2: p->fnRgb = PFN_ADD; break;                // wrong function
        default: p->srcA[0] = PSRC_PRIMARY; break;        // wrong source
        }
        tried++;
        float pe = max_error(g, &t, 64, NULL);
        printf("  self-test: corruption %d gives error %.2f\n", kind, pe);
        if (pe > TOL) detected++;
    }
    printf("  self-test: %d of %d planted corruptions detected\n", detected, tried);
    return detected == tried;
}

// Compile speed: average time of tev_pica_compile over game-like programs (the 3DS backend caches the result by BP state).
static void bench(void) {
    enum { N = 2000, REP = 50 };
    static GXCore cfg[N];
    for (int i = 0; i < N; i++) { fuzz_game(&g_gx); cfg[i] = g_gx; }
    PicaTev t;
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (int r = 0; r < REP; r++) for (int i = 0; i < N; i++) tev_pica_compile(&cfg[i], &t);
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double us = ((t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3) / (double)(N * REP);
    printf("bench: %.2f us per tev_pica_compile on this machine (sizeof PicaTev %zu)\n", us, sizeof(PicaTev));
}

int main(int argc, char** argv) {
    int fuzzCases = argc > 1 ? atoi(argv[1]) : 40000;
    if (argc > 2) rng ^= (u64)atoll(argv[2]) * 0x2545F4914F6CDD1Dull;
    const char* only = argc > 3 ? argv[3] : "all";  // all | hand | game | regs | live | wild
    bool all = !strcmp(only, "all");
    g_plant = getenv("TEV_PLANT") != NULL;
    if (g_plant) printf("TEV_PLANT: every compiled result is corrupted; failures are expected\n");
    if (!strcmp(only, "bench")) { bench(); return 0; }
    if (all || !strcmp(only, "hand")) {
        printf("hand-written cases (64 random input sets each, exact cases must be within %.0f/255):\n", TOL);
        hand_cases();
    }
    if (all || !strcmp(only, "game")) {
        Stats game = run_fuzz("game-like", fuzz_game, fuzzCases);
        print_stats("game-like fuzz", &game);
    }
    if (all || !strcmp(only, "regs")) {
        Stats regs = run_fuzz("register-heavy", fuzz_regs, fuzzCases);
        print_stats("register-heavy fuzz", &regs);
    }
if (all || !strcmp(only, "live")) {        Stats live = run_fuzz("live-register", fuzz_live, fuzzCases);        print_stats("live-register fuzz", &live);    }
    if (all || !strcmp(only, "wild")) {
        Stats wild = run_fuzz("wild", fuzz_wild, fuzzCases);
        print_stats("wild fuzz", &wild);
    }
    if (!g_plant && all && !self_test()) { printf("SELF-TEST FAILED\n"); g_fails++; }
    printf("fails=%d\n", g_fails);
    return g_fails ? 1 : 0;
}
