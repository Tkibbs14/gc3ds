// GX TEV -> PICA200 combiner translation (see tev_pica.h). The reference semantics are soft.c's run_tev.
//
// Each GX stage computes, per track (rgb and alpha, which are independent), out = clamp((d +/- lerp(a,b,c) + bias) *
// scale) and writes it to one of four registers. The compiler walks the stages in order (skipping tracks whose result
// nothing reads) and keeps, for every (register, track), where its current value lives:
//   - an alias of a leaf (constant, texel, rasterised colour): no PICA stage is spent on it, consumers read the leaf;
//     stages whose inputs are all constants are folded into one;
//   - the output of PICA stage p. A later stage reads it as PREVIOUS while no operation of that track has replaced it
//     (stages without one pass the track through), else from the combiner buffer. The buffer is committed lazily: the
//     first late read sets stage p's buffer-write bit, after checking every recorded buffer read against Citra's rule
//     (a read at stage j sees the latest write among stages <= j-2).
// A track is turned into a plan of one or two PICA operations (REPLACE / MODULATE / MULTIPLY_ADD / INTERPOLATE /
// ADD / ADD_SIGNED / SUBTRACT). Every candidate plan is placed on a copy of the state and the cheapest feasible one wins;
// alpha may start a stage after rgb. Constants (konst, registers never written, 1, 0.5, 0) go into the byte slots of the
// PICA stage's single constant colour: a scalar takes one byte and is read through an R/G/B/A operand, a vector takes RGB.
// Whatever cannot be expressed is approximated and flagged in PicaTev.approx; with no flag the result matches run_tev to
// 8-bit rounding (the GX lerp weight (c + (c >= 128)) / 256 against the PICA's c / 255 is the one difference that stays).
#include <math.h>
#include "tev_pica.h"

#define BUFRD_MAX 24

enum { OK_CONST, OK_TEX, OK_RAS, OK_VAL, OK_TEMP };
enum { T_RGB = 0, T_ALPHA = 1 };

// One input of a combiner function, as a PICA source plus operand. Alpha-track operands replicate one component in sel.
typedef struct {
    u8 kind;
    u8 inv;        // yields 1 - x
    u8 unclamped;  // the GX value may lie outside 0-255 (clamp bit off, or a register preset out of range)
    u8 sel[3];     // raw component (0-3 = R, G, B, A) of the base colour that feeds output component k
    u8 a, b;       // OK_TEX: texmap, texcoord; OK_RAS: channel; OK_VAL: producing PICA stage, value track
    u8 cv[3];      // OK_CONST: value of each output component, 0-255
} Opnd;

typedef struct { u8 fn, scale; Opnd in[3]; } POp;
typedef struct { u8 n; POp op[2]; } Plan;  // n = 1 or 2 operations; the second reads the first through OK_TEMP

typedef struct {
    PicaTev o;
    Opnd loc[4][2];              // current value of GX register r, track t
    u8 kfill[PICA_STAGES];       // bit per byte of the stage's konst that holds a value
    u8 nst;                      // PICA stages emitted
    u8 rasSet, force, stop;
    u8 opMask[2];                // per track, bit per PICA stage: an operation was placed there (else the stage passes the track through)
    u8 nrd[2];                   // recorded combiner-buffer reads per track: stage j reading the output of stage p
    u8 rdJ[2][BUFRD_MAX], rdP[2][BUFRD_MAX];
} Ctx;

// ---- operands ----
static u8 q8(float v) { return v <= 0 ? 0 : v >= 255 ? 255 : (u8)(v + 0.5f); }

static Opnd k_vec(u8 r, u8 g, u8 b) { Opnd o; memset(&o, 0, sizeof o); o.kind = OK_CONST; o.cv[0] = r; o.cv[1] = g; o.cv[2] = b; return o; }
static Opnd k_scalar(u8 v) { return k_vec(v, v, v); }

static Opnd leaf(u8 kind, u8 a, u8 b, u8 s0, u8 s1, u8 s2) {
    Opnd o; memset(&o, 0, sizeof o);
    o.kind = kind; o.a = a; o.b = b; o.sel[0] = s0; o.sel[1] = s1; o.sel[2] = s2;
    return o;
}

static Opnd op_val(u8 stage, int t, bool unclamped) {
    Opnd o = t == T_RGB ? leaf(OK_VAL, stage, T_RGB, 0, 1, 2) : leaf(OK_VAL, stage, T_ALPHA, 3, 3, 3);
    o.unclamped = unclamped;
    return o;
}

static Opnd op_temp(int t) { return t == T_RGB ? leaf(OK_TEMP, 0, 0, 0, 1, 2) : leaf(OK_TEMP, 0, 0, 3, 3, 3); }

static Opnd op_inv(Opnd o) {
    if (o.kind == OK_CONST) { for (int k = 0; k < 3; k++) o.cv[k] = (u8)(255 - o.cv[k]); }
    else o.inv ^= 1;
    return o;
}

static bool is_k(const Opnd* o, u8 v) { return o->kind == OK_CONST && o->cv[0] == v && o->cv[1] == v && o->cv[2] == v; }

static bool op_eq(const Opnd* x, const Opnd* y) {
    return x->kind == y->kind && x->inv == y->inv && x->a == y->a && x->b == y->b && !memcmp(x->sel, y->sel, 3) && !memcmp(x->cv, y->cv, 3);
}

// Constant folding runs the GX arithmetic of soft.c (lerp_tev) on compile-time values, rounded to 8 bits.
static float gx_lerp(float a, float b, float c) {
    float cc = c < 0 ? 0 : c > 255 ? 255 : c;
    float w = cc + (cc >= 128 ? 1 : 0);
    return a * (256 - w) / 256.0f + b * w / 256.0f;
}

static Opnd k_lerp(const Opnd* a, const Opnd* b, const Opnd* c) {
    Opnd o = k_vec(0, 0, 0);
    for (int k = 0; k < 3; k++) o.cv[k] = q8(gx_lerp(a->cv[k], b->cv[k], c->cv[k]));
    return o;
}

// The constant stage result clamp((d + (sub ? -L : L) + bias) * scale) per component. Flagged unclamped when the raw
// value leaves 0-255, which only a clamping stage hides (commit_group clears the flag then).
static Opnd k_stage(const Opnd* d, const float L[3], int sub, float bias, float scale) {
    Opnd o = k_vec(0, 0, 0);
    for (int k = 0; k < 3; k++) {
        float r = ((float)d->cv[k] + (sub ? -L[k] : L[k]) + bias) * scale;
        if (r < 0 || r > 255) o.unclamped = 1;
        o.cv[k] = q8(r);
    }
    return o;
}

static s32 sext11(u32 v) { return ((s32)(v << 21)) >> 21; }

static int pop8(u8 v) { int n = 0; for (; v; v &= (u8)(v - 1)) n++; return n; }

// ---- stage state ----
static void stage_default(PicaStage* s) {
    for (int i = 0; i < 3; i++) { s->srcRgb[i] = s->srcA[i] = PSRC_PREVIOUS; s->opRgb[i] = POPC_COLOR; s->opA[i] = POPA_ALPHA; }
    s->fnRgb = s->fnA = PFN_REPLACE;
    s->scaleRgb = s->scaleA = PSCALE_1;
    s->konst = 0;
}

static void konst_put(Ctx* c, int j, int slot, u8 v) {
    c->o.st[j].konst = (c->o.st[j].konst & ~(0xFFu << (8 * slot))) | ((u32)v << (8 * slot));
    c->kfill[j] |= (u8)(1u << slot);
}

// A scalar constant needs one byte of the stage constant: an existing byte holding v (or 255 - v, read inverted) or a free one.
static bool konst_slot(Ctx* c, int j, u8 v, int* slot, int* inv) {
    u32 k = c->o.st[j].konst;
    for (int s = 0; s < 4; s++) {
        if (!((c->kfill[j] >> s) & 1)) continue;
        u8 x = (u8)(k >> (8 * s));
        if (x == v) { *slot = s; *inv = 0; return true; }
        if (x == 255 - v) { *slot = s; *inv = 1; return true; }
    }
    for (int s = 0; s < 4; s++)
        if (!((c->kfill[j] >> s) & 1)) { konst_put(c, j, s, v); *slot = s; *inv = 0; return true; }
    return false;
}

static bool konst_vec(Ctx* c, int j, const u8 cv[3]) {
    u32 k = c->o.st[j].konst;
    for (int s = 0; s < 3; s++)
        if (((c->kfill[j] >> s) & 1) && (u8)(k >> (8 * s)) != cv[s]) return false;
    for (int s = 0; s < 3; s++) konst_put(c, j, s, cv[s]);
    return true;
}

static int tex_unit(Ctx* c, u8 map, u8 coord) {
    PicaTev* o = &c->o;
    for (int i = 0; i < o->numTex; i++)
        if (o->texMap[i] == map && o->texCoord[i] == coord) return i;
    if (o->numTex < PICA_TEXUNITS) { o->texMap[o->numTex] = map; o->texCoord[o->numTex] = coord; return o->numTex++; }
    o->approx |= TEVA_TEXUNITS;
    for (int i = 0; i < o->numTex; i++)
        if (o->texMap[i] == map) return i;
    return 0;
}

// ---- combiner buffer ----
// Every recorded read must still find, under the current write mask, the stage that produced its value.
static bool buf_valid(const Ctx* c, int t) {
    u8 mask = t == T_RGB ? c->o.bufRgb : c->o.bufA;
    for (int n = 0; n < c->nrd[t]; n++) {
        int w = -1;
        for (int s = c->rdJ[t][n] - 2 < 3 ? c->rdJ[t][n] - 2 : 3; s >= 0; s--)
            if ((mask >> s) & 1) { w = s; break; }
        if (w != c->rdP[t][n]) return false;
    }
    return true;
}

// Reads the output of stage p from the buffer at stage j (j >= p + 2): sets p's write bit if that keeps all other reads valid.
static bool buf_read(Ctx* c, int t, int p, int j) {
    if (p > 3 || j < p + 2) return false;
    for (int n = 0; n < c->nrd[t]; n++)
        if (c->rdJ[t][n] == j && c->rdP[t][n] == p) return true;
    if (c->nrd[t] >= BUFRD_MAX) return false;
    u8* mask = t == T_RGB ? &c->o.bufRgb : &c->o.bufA;
    u8 old = *mask;
    *mask |= (u8)(1u << p);
    c->rdJ[t][c->nrd[t]] = (u8)j; c->rdP[t][c->nrd[t]] = (u8)p; c->nrd[t]++;
    if (!buf_valid(c, t)) { *mask = old; c->nrd[t]--; return false; }
    return true;
}

// Is the output of track t's stage p still PREVIOUS when stage j reads? Stages that place no operation for the track
// pass it through, so only an operation in between replaces it. Stages from limit on count as occupied.
static bool in_prev(const Ctx* c, int t, int p, int j, int limit) {
    if (j <= p) return false;  // not produced yet
    for (int s = p + 1; s < j; s++)
        if (s >= limit || ((c->opMask[t] >> s) & 1)) return false;
    return true;
}

// ---- operand -> PICA source ----
static u8 opcode(Ctx* c, int t, const u8 sel[3], int inv) {
    static const u8 rgbRep[4] = { POPC_R, POPC_G, POPC_B, POPC_ALPHA };
    static const u8 aRep[4] = { POPA_R, POPA_G, POPA_B, POPA_ALPHA };
    u8 base;
    if (t == T_RGB && sel[0] == 0 && sel[1] == 1 && sel[2] == 2) base = POPC_COLOR;
    else if (sel[0] == sel[1] && sel[1] == sel[2]) base = (t == T_RGB ? rgbRep : aRep)[sel[0]];
    else { c->o.approx |= TEVA_SWAP; base = t == T_RGB ? POPC_COLOR : POPA_ALPHA; }  // no operand permutes components
    return (u8)(base + inv);  // the inverted operand is always the next code
}

// Resolves operand o, read by track t of PICA stage j. False when the operand cannot be supplied there (unless c->force).
static bool resolve(Ctx* c, int j, int t, const Opnd* o, u8* src, u8* opc) {
    u8 sel[3] = { o->sel[0], o->sel[1], o->sel[2] };
    int inv = o->inv;
    switch (o->kind) {
    case OK_CONST:
        *src = PSRC_CONSTANT;
        if (o->cv[0] == o->cv[1] && o->cv[1] == o->cv[2]) {
            int slot = 0, ci = 0;
            if (!konst_slot(c, j, o->cv[0], &slot, &ci)) {
                if (!c->force) return false;
                c->o.approx |= TEVA_REGS;
            }
            sel[0] = sel[1] = sel[2] = (u8)slot;
            inv = ci;
        } else {
            if (!konst_vec(c, j, o->cv)) {
                if (!c->force) return false;
                c->o.approx |= TEVA_REGS;
            }
            sel[0] = 0; sel[1] = 1; sel[2] = 2;
        }
        break;
    case OK_TEX: *src = (u8)(PSRC_TEX0 + tex_unit(c, o->a, o->b)); break;
    case OK_RAS:
        if (!c->rasSet) { c->rasSet = 1; c->o.rasChan = o->a; }
        else if (c->o.rasChan != o->a) c->o.approx |= TEVA_RASCHAN;
        *src = PSRC_PRIMARY;
        break;
    case OK_VAL:
        // rgb is planned before alpha: a pass-through assumed on the alpha track must not depend on stages alpha has not placed yet.
        if (in_prev(c, o->b, o->a, j, o->b != t && t == T_RGB ? c->nst : 255)) *src = PSRC_PREVIOUS;
        else if (buf_read(c, o->b, o->a, j)) *src = PSRC_BUFFER;
        else if (c->force) { c->o.approx |= TEVA_REGS; *src = PSRC_PREVIOUS; }
        else return false;
        break;
    default: *src = PSRC_PREVIOUS; break;
    }
    *opc = opcode(c, t, sel, inv);
    return true;
}

static int fn_srcs(u8 fn) {
    switch (fn) {
    case PFN_REPLACE: return 1;
    case PFN_INTERPOLATE: case PFN_MULTIPLY_ADD: case PFN_ADD_MULTIPLY: return 3;
    default: return 2;
    }
}

static bool place_op(Ctx* c, int j, int t, const POp* op) {
    PicaStage* s = &c->o.st[j];
    u8 src[3], opc[3];
    int n = fn_srcs(op->fn);
    for (int i = 0; i < 3; i++) {
        if (i < n) { if (!resolve(c, j, t, &op->in[i], &src[i], &opc[i])) return false; }
        else { src[i] = PSRC_PREVIOUS; opc[i] = t == T_RGB ? POPC_COLOR : POPA_ALPHA; }
    }
    if (t == T_RGB) {
        memcpy(s->srcRgb, src, 3); memcpy(s->opRgb, opc, 3);
        s->fnRgb = op->fn; s->scaleRgb = op->scale;
    } else {
        memcpy(s->srcA, src, 3); memcpy(s->opA, opc, 3);
        s->fnA = op->fn; s->scaleA = op->scale;
    }
    c->opMask[t] |= (u8)(1u << j);
    return true;
}

static bool place_plan(Ctx* c, int t, int base, const Plan* p) {
    if (base + p->n > PICA_STAGES) return false;
    for (int k = 0; k < p->n; k++)
        if (!place_op(c, base + k, t, &p->op[k])) return false;
    return true;
}

// ---- plans ----
static void set_op(POp* p, u8 fn, u8 scale, const Opnd* i0, const Opnd* i1, const Opnd* i2) {
    memset(p, 0, sizeof *p);
    p->fn = fn; p->scale = scale;
    p->in[0] = *i0;
    if (i1) p->in[1] = *i1;
    if (i2) p->in[2] = *i2;
}

static void plan1(Plan* p, u8 fn, u8 scale, const Opnd* i0, const Opnd* i1, const Opnd* i2) {
    p->n = 1;
    set_op(&p->op[0], fn, scale, i0, i1, i2);
}

static void plan2(Plan* p, const POp* first, const POp* second) { p->n = 2; p->op[0] = *first; p->op[1] = *second; }

// Candidate plans for out = clamp((d + (sub ? -1 : 1) * lerp(a,b,c) + (bm ? -0.5 : 0)) * 2^sc), cheapest first.
// lerp collapses when an operand is a known constant: c = 0 or a == b gives a, c = 1 gives b, a = 0 gives b*c, b = 0 gives a*(1-c).
static int gen_plans(int t, const Opnd in[4], int sub, int bm, int sc, Plan out[4]) {
    const Opnd *a = &in[0], *b = &in[1], *c = &in[2], *d = &in[3];
    Opnd zero = k_scalar(0), zeroU = zero, tmp = op_temp(t), ic = op_inv(*c);
    zeroU.unclamped = 1;  // a result that is negative in GX and 0 here: only equal when the stage clamps
    bool zd = is_k(d, 0);
    enum { L0, LX, LMUL, LLERP } lk;
    Opnd x = *a, y = *a;
    bool ka = a->kind == OK_CONST, kb = b->kind == OK_CONST, kc = c->kind == OK_CONST;
    if (op_eq(a, b) || is_k(c, 0)) lk = LX;
    else if (is_k(c, 255)) { lk = LX; x = *b; }
    else if (ka && kb && kc) { lk = LX; x = k_lerp(a, b, c); }
    else if (is_k(a, 0)) {
        if (is_k(b, 0)) lk = L0;
        else if (is_k(b, 255)) { lk = LX; x = *c; }
        else if (kb && kc) { lk = LX; x = k_lerp(&zero, b, c); }
        else { lk = LMUL; x = *b; y = *c; }
    } else if (is_k(b, 0)) {
        if (is_k(a, 255)) { lk = LX; x = ic; }
        else if (ka && kc) { lk = LX; x = k_lerp(a, &zero, c); }
        else { lk = LMUL; x = *a; y = ic; }
    } else lk = LLERP;
    if (lk == LMUL && is_k(&x, 255)) { lk = LX; x = y; }
    else if (lk == LMUL && is_k(&y, 255)) lk = LX;
    if (lk == LX && is_k(&x, 0)) lk = L0;
    if ((lk == L0 || (lk == LX && x.kind == OK_CONST)) && d->kind == OK_CONST) {
        float L[3] = { 0, 0, 0 };
        if (lk == LX) for (int k = 0; k < 3; k++) L[k] = x.cv[k];
        Opnd v = k_stage(d, L, sub, bm ? -128.0f : 0, (float)(1u << sc));
        plan1(&out[0], PFN_REPLACE, PSCALE_1, &v, NULL, NULL);
        return 1;
    }
    POp lop;  // L alone, for the two-operation plans
    if (lk == LMUL) set_op(&lop, PFN_MODULATE, PSCALE_1, &x, &y, NULL);
    else set_op(&lop, PFN_INTERPOLATE, PSCALE_1, b, a, c);  // PICA a*c + b*(1-c) with its a = GX b
    const Opnd* dz = zd ? &zero : d;
    int n = 0;
    POp s;
    if (sub) {
        if (zd) plan1(&out[n++], PFN_REPLACE, PSCALE_1, lk == L0 ? &zero : &zeroU, NULL, NULL);  // 0 - L: negative, so 0 after the clamp
        else if (lk == L0) plan1(&out[n++], PFN_REPLACE, (u8)sc, d, NULL, NULL);
        else if (lk == LX) plan1(&out[n++], PFN_SUBTRACT, (u8)sc, d, &x, NULL);
        else { set_op(&s, PFN_SUBTRACT, (u8)sc, d, &tmp, NULL); plan2(&out[n++], &lop, &s); }
    } else if (bm) {
        if (lk == L0) {
            if (zd) plan1(&out[n++], PFN_REPLACE, PSCALE_1, &zeroU, NULL, NULL);
            else plan1(&out[n++], PFN_ADD_SIGNED, (u8)sc, d, &zero, NULL);
        } else if (lk == LX) plan1(&out[n++], PFN_ADD_SIGNED, (u8)sc, dz, &x, NULL);
        else { set_op(&s, PFN_ADD_SIGNED, (u8)sc, &tmp, dz, NULL); plan2(&out[n++], &lop, &s); }
    } else {
        if (lk == L0) plan1(&out[n++], PFN_REPLACE, zd ? (u8)PSCALE_1 : (u8)sc, zd ? &zero : d, NULL, NULL);
        else if (lk == LX) {
            if (zd) plan1(&out[n++], PFN_REPLACE, (u8)sc, &x, NULL, NULL);
            else plan1(&out[n++], PFN_ADD, (u8)sc, d, &x, NULL);
        } else if (lk == LMUL) {
            if (zd) plan1(&out[n++], PFN_MODULATE, (u8)sc, &x, &y, NULL);
            else plan1(&out[n++], PFN_MULTIPLY_ADD, (u8)sc, &x, &y, d);
        } else {
            if (zd) plan1(&out[n++], PFN_INTERPOLATE, (u8)sc, b, a, c);
            else { set_op(&s, PFN_ADD, (u8)sc, &tmp, d, NULL); plan2(&out[n++], &lop, &s); }
            // d + a(1-c) + b*c as two multiply-adds: the clamp after the first only bites when the final value clamps too.
            // It reads every GX input in the first operation, which keeps the previous stage's output in PREVIOUS.
            POp f, g;
            if (zd) set_op(&f, PFN_MODULATE, PSCALE_1, a, &ic, NULL); else set_op(&f, PFN_MULTIPLY_ADD, PSCALE_1, a, &ic, d);
            set_op(&g, PFN_MULTIPLY_ADD, (u8)sc, b, c, &tmp);
            plan2(&out[n++], &f, &g);
            if (zd) set_op(&f, PFN_MODULATE, PSCALE_1, b, c, NULL); else set_op(&f, PFN_MULTIPLY_ADD, PSCALE_1, b, c, d);
            set_op(&g, PFN_MULTIPLY_ADD, (u8)sc, a, &ic, &tmp);
            plan2(&out[n++], &f, &g);
            if (kc && (ka || kb)) {
                // A constant multiplier c with one constant end: L = P + K with P = b*c (a constant) or a*(1-c) (b constant) the
                // only non-constant term and K the constant one, folded; keeps one constant per operation.
                const Opnd *p1 = ka ? b : a, *p2 = ka ? c : &ic;
                Opnd kk = ka ? k_lerp(a, &zero, c) : k_lerp(&zero, b, c);
                if (d->kind == OK_CONST) {
                    for (int k = 0; k < 3; k++) kk.cv[k] = q8((float)kk.cv[k] + d->cv[k]);  // d + K, saturating
                    set_op(&f, PFN_MODULATE, PSCALE_1, p1, p2, NULL);
                } else {
                    set_op(&f, PFN_MULTIPLY_ADD, PSCALE_1, p1, p2, d);
                }
                set_op(&g, PFN_ADD, (u8)sc, &tmp, &kk, NULL);
                plan2(&out[n++], &f, &g);
            }
        }
    }
    return n;
}

typedef struct { u8 n, off, alias; Opnd aop; } TrackRes;  // n operations at stages base + off ..; alias = the value is just aop

// Places the cheapest feasible plan for one track from stage c->nst on. gHave is the group length the other track already
// needs. Alpha may start one stage later than rgb when the two collide on the stage constant. Sets c->stop when nothing fits.
static void plan_track(Ctx* c, int t, int gHave, const Opnd in[4], int sub, int bm, int sc, TrackRes* res) {
    Plan cand[4];
    int nc = gen_plans(t, in, sub, bm, sc, cand);
    int base = c->nst;
    memset(res, 0, sizeof *res);
    if (cand[0].n == 1 && cand[0].op[0].fn == PFN_REPLACE && cand[0].op[0].scale == PSCALE_1) {
        res->alias = 1;
        res->aop = cand[0].op[0].in[0];
        return;
    }
    Ctx best;
    int bestI = -1, bestOff = 0, bestCost = 1 << 30;
    for (int pass = 0; pass < 2 && bestI < 0; pass++) {
        int maxOff = pass == 0 && t == T_ALPHA ? 1 : 0;
        for (int off = 0; off <= maxOff; off++) {
            for (int i = 0; i < nc; i++) {
                if (base + off + cand[i].n > PICA_STAGES) continue;
                Ctx tmp = *c;
                tmp.force = (u8)pass;
                if (!place_plan(&tmp, t, base + off, &cand[i])) continue;
                int len = off + cand[i].n;
                int cost = (len > gHave ? len - gHave : 0) * 16 + cand[i].n + 2 * (pop8((u8)(tmp.o.bufRgb ^ c->o.bufRgb)) + pop8((u8)(tmp.o.bufA ^ c->o.bufA)));
                if (cost < bestCost) { bestCost = cost; bestI = i; bestOff = off; best = tmp; }
            }
        }
    }
    if (bestI < 0) { c->o.approx |= TEVA_STAGES; c->stop = 1; return; }
    *c = best;
    c->force = 0;
    res->n = cand[bestI].n;
    res->off = (u8)bestOff;
}

// ---- GX stage decode ----
static void swap_sel(const GXCore* gx, u32 tab, u8 sel[4]) {
    u32 t0 = gx->bp[0xF6 + tab * 2], t1 = gx->bp[0xF7 + tab * 2];
    sel[0] = (u8)(t0 & 3); sel[1] = (u8)((t0 >> 2) & 3); sel[2] = (u8)(t1 & 3); sel[3] = (u8)((t1 >> 2) & 3);
}

// Konst selection (soft.c konst_value): 1, 7/8 .. 1/8; Kn (rgb, or its alpha for the alpha track); Kn.r/g/b/a replicated.
static Opnd konst_op(const u8 kv[4][4], u32 sel, bool alpha) {
    if (sel <= 7) return k_scalar(q8((float)(8 - sel) * 255.0f / 8.0f));
    if (sel >= 12 && sel <= 15) {
        const u8* k = kv[sel - 12];
        return alpha ? k_scalar(k[3]) : k_vec(k[0], k[1], k[2]);
    }
    if (sel >= 16 && sel <= 31) return k_scalar(kv[(sel - 16) & 3][(sel - 16) >> 2]);
    return k_scalar(255);
}

typedef struct { Opnd texC, texA, rasC, rasA, kc, ka; } Leaves;

static Opnd cin_opnd(const Ctx* c, const Leaves* lv, u32 sel) {
    switch (sel) {
    case 0: case 2: case 4: case 6: return c->loc[sel >> 1][T_RGB];
    case 1: case 3: case 5: case 7: return c->loc[sel >> 1][T_ALPHA];  // a register's alpha used as a colour
    case 8: return lv->texC;
    case 9: return lv->texA;
    case 10: return lv->rasC;
    case 11: return lv->rasA;
    case 12: return k_scalar(255);
    case 13: return k_scalar(128);
    case 14: return lv->kc;
    default: return k_scalar(0);
    }
}

static Opnd ain_opnd(const Ctx* c, const Leaves* lv, u32 sel) {
    switch (sel) {
    case 0: case 1: case 2: case 3: return c->loc[sel][T_ALPHA];
    case 4: return lv->texA;
    case 5: return lv->rasA;
    case 6: return lv->ka;
    default: return k_scalar(0);
    }
}

// Backward liveness over the stages: a track whose destination value is never read (and is not the final PREV) is dead.
static void liveness(const GXCore* gx, u32 stages, u8 needC[16], u8 needA[16]) {
    u8 live[4][2] = { { 1, 1 }, { 0, 0 }, { 0, 0 }, { 0, 0 } };
    for (int st = (int)stages - 1; st >= 0; st--) {
        u32 cenv = gx->bp[0xC0 + 2 * st], aenv = gx->bp[0xC1 + 2 * st];
        u32 cd = (cenv >> 22) & 3, ad = (aenv >> 22) & 3;
        needC[st] = live[cd][T_RGB];
        needA[st] = live[ad][T_ALPHA];
        live[cd][T_RGB] = 0;
        live[ad][T_ALPHA] = 0;
        if (needC[st])
            for (int i = 0; i < 4; i++) {
                u32 sel = (cenv >> (12 - 4 * i)) & 15;
                if (sel < 8) live[sel >> 1][sel & 1] = 1;
            }
        if (needA[st])
            for (int i = 0; i < 4; i++) {
                u32 sel = (aenv >> (13 - 3 * i)) & 7;
                if (sel < 4) live[sel][T_ALPHA] = 1;
            }
    }
}

static void init_regs(Ctx* c, const GXCore* gx) {
    for (int r = 0; r < 4; r++) {
        s32 v[4] = { sext11(gx->tevLo[r]), sext11(gx->tevHi[r] >> 12), sext11(gx->tevHi[r]), sext11(gx->tevLo[r] >> 12) };  // R G B A
        u8 q[4];
        bool un[4];
        for (int k = 0; k < 4; k++) {
            un[k] = v[k] < 0 || v[k] > 255;
            q[k] = (u8)(v[k] < 0 ? 0 : v[k] > 255 ? 255 : v[k]);
        }
        c->loc[r][T_RGB] = k_vec(q[0], q[1], q[2]);
        c->loc[r][T_RGB].unclamped = un[0] || un[1] || un[2];
        c->loc[r][T_ALPHA] = k_scalar(q[3]);
        c->loc[r][T_ALPHA].unclamped = un[3];
    }
}

// After a group of PICA stages starting at base: records where each track's destination value lives (the stage of the
// track's last operation) and advances the stage count.
static void commit_group(Ctx* c, int base, const u8 dest[2], const u8 need[2], const u8 clamp[2], const TrackRes res[2]) {
    int g = 0;
    for (int t = 0; t < 2; t++) {
        if (!need[t]) continue;
        if (res[t].alias) {
            Opnd o = res[t].aop;
            if (clamp[t]) o.unclamped = 0;
            c->loc[dest[t]][t] = o;
        } else if (res[t].n) {
            c->loc[dest[t]][t] = op_val((u8)(base + res[t].off + res[t].n - 1), t, !clamp[t]);
            if (res[t].off + res[t].n > g) g = res[t].off + res[t].n;
        }
    }
    c->nst = (u8)(c->nst + g);
}

// ---- linear programs (out-of-range intermediates) ----
// The PICA clamps every stage's output to 0-1, GX keeps 11-bit signed registers. Twilight Princess's THP movie player
// (and any game using the SDK's THP player) converts YUV with a negative register preset added first (R = C0 + U*K0,
// then ... + Y): the PICA lost the negative part and the movie came out purple (2DS, 2026-10-13, the live "tev"
// command). When a GX program is linear in its texels and rasterised colour (lerp weights constant, or both lerp ends
// constant; a clamp only on a value that is then passed on whole), each output channel is k + sum v_i x_i with x_i the
// texel / colour components in 0-1. That is computed exactly in unsigned stages: v_i x_i with v_i < 0 becomes
// |v_i| (1 - x_i) - |v_i|, every term is added first (MODULATE, then MULTIPLY_ADD), the negative constant is subtracted
// last, and the whole sum runs at 1/s of its value with the last stage's scale s. While a partial sum clamps at 1, the
// true result is at least s - |k|/255 >= 1, so the clamp only cuts what the output clamp would cut anyway.
#define LV_N 16  // texture units 0-2 x raw channels R G B A, then the rasterised colour's R G B A
typedef struct { float k, v[LV_N]; bool clamped; } Lin;  // value = k + sum v[i] x_i, in 0-255 units

static Lin lin_k(float k) { Lin l; memset(&l, 0, sizeof l); l.k = k; return l; }
static bool lin_var(const Lin* l) { for (int i = 0; i < LV_N; i++) if (l->v[i] != 0) return true; return false; }
static void lin_axpy(Lin* r, float a, const Lin* x) { r->k += a * x->k; for (int i = 0; i < LV_N; i++) r->v[i] += a * x->v[i]; }

typedef struct {
    Lin reg[4][4];  // GX register r, component R G B A
    u8 unitMap[3], unitCoord[3], nUnits;
    int ras;        // the rasterised colour channel used (-1: none)
} LinCtx;

static int lin_unit(LinCtx* L, u8 map, u8 coord) {
    for (int u = 0; u < L->nUnits; u++) if (L->unitMap[u] == map && L->unitCoord[u] == coord) return u;
    if (L->nUnits == 3) return -1;
    L->unitMap[L->nUnits] = map; L->unitCoord[L->nUnits] = coord;
    return L->nUnits++;
}

typedef struct { Lin c[3], a; } LinIn;  // a stage input: colour components and alpha

// The raw channel a texel component really is: intensity formats (I4, I8) hold one value in all four, IA4 / IA8 one in
// R G B and alpha apart (the THP movie's Y, U and V are I8: as four separate inputs they needed more stages than exist).
static u8 tex_channel(const GXCore* gx, u8 map, u8 ch) {
    u32 hi = map >= 4 ? 0x20 : 0, fmt = (gx->bp[0x88 + (map & 3) + hi] >> 20) & 15;
    if (fmt <= 1) return 0;
    if (fmt <= 3) return ch == 3 ? 3 : 0;
    return ch;
}

static bool lin_leaf_tex(LinCtx* L, const GXCore* gx, u8 map, u8 coord, const u8 ts[4], LinIn* o) {
    int u = lin_unit(L, map, coord);
    if (u < 0) return false;
    for (int k = 0; k < 3; k++) { o->c[k] = lin_k(0); o->c[k].v[u * 4 + tex_channel(gx, map, ts[k])] = 255; }
    o->a = lin_k(0); o->a.v[u * 4 + tex_channel(gx, map, ts[3])] = 255;
    return true;
}

// GX stage for one component: (d + (sub ? -1 : 1) lerp(a, b, c) + bias) * scale, clamped or not. false: not linear.
static bool lin_stage(const Lin* a, const Lin* b, const Lin* cw, const Lin* d, int sub, float bias, float scale, bool clamp, Lin* out) {
    Lin r = lin_k(0);
    float s = sub ? -1.0f : 1.0f;
    const Lin* used[3];
    float coef[3];
    int n = 0;
    if (!lin_var(cw) && !cw->clamped) {  // constant weight: a (1 - w) + b w, w as GX rounds it
        float c = cw->k < 0 ? 0 : cw->k > 255 ? 255 : cw->k, w = (c + (c >= 128 ? 1 : 0)) / 256.0f;
        lin_axpy(&r, s * (1 - w) * scale, a); used[n] = a; coef[n++] = s * (1 - w) * scale;
        lin_axpy(&r, s * w * scale, b); used[n] = b; coef[n++] = s * w * scale;
    } else if (!lin_var(a) && !lin_var(b) && !cw->clamped) {  // constant ends: a + (b - a) c / 255
        r.k += s * a->k * scale;
        for (int i = 0; i < LV_N; i++) r.v[i] += s * (b->k - a->k) * cw->v[i] / 255.0f * scale;
        r.k += s * (b->k - a->k) * cw->k / 255.0f * scale;
    } else {
        return false;
    }
    lin_axpy(&r, scale, d); used[n] = d; coef[n++] = scale;
    r.k += bias * scale;
    // A clamped (no longer linear) input may only be passed on whole: weight 1, nothing else added.
    for (int j = 0; j < n; j++) {
        if (!used[j]->clamped || coef[j] == 0) continue;
        if (fabsf(coef[j] - 1) > 1e-6f || bias != 0) return false;
        for (int m = 0; m < n; m++) if (m != j && coef[m] != 0 && (lin_var(used[m]) || used[m]->k != 0)) return false;
        r.clamped = true;
    }
    float lo = r.k, hi = r.k;
    for (int i = 0; i < LV_N; i++) { if (r.v[i] < 0) lo += r.v[i]; else hi += r.v[i]; }
    if (lo < -1024 || hi > 1023) return false;  // would wrap the 11-bit register
    if (clamp) {
        if (!lin_var(&r)) { r.k = r.k < 0 ? 0 : r.k > 255 ? 255 : r.k; r.clamped = false; }
        else if (lo < 0 || hi > 255) r.clamped = true;
    }
    *out = r;
    return true;
}

static Lin lin_cin(const LinCtx* L, const LinIn* tex, const LinIn* ras, const Lin kc[3], u32 sel, int k) {
    switch (sel) {
    case 0: case 2: case 4: case 6: return L->reg[sel >> 1][k];
    case 1: case 3: case 5: case 7: return L->reg[sel >> 1][3];
    case 8: return tex->c[k];
    case 9: return tex->a;
    case 10: return ras->c[k];
    case 11: return ras->a;
    case 12: return lin_k(255);
    case 13: return lin_k(128);
    case 14: return kc[k];
    default: return lin_k(0);
    }
}

static Lin lin_ain(const LinCtx* L, const LinIn* tex, const LinIn* ras, const Lin* ka, u32 sel) {
    switch (sel) {
    case 0: case 1: case 2: case 3: return L->reg[sel][3];
    case 4: return tex->a;
    case 5: return ras->a;
    case 6: return *ka;
    default: return lin_k(0);
    }
}

// The source and operand that read variable i (neg: 1 - x) for the colour or alpha track.
static void lin_operand(int i, bool neg, bool alpha, u8* src, u8* op) {
    int ch = i % 4;
    static const u8 kc[4][2] = { { POPC_R, POPC_1_R }, { POPC_G, POPC_1_G }, { POPC_B, POPC_1_B }, { POPC_ALPHA, POPC_1_ALPHA } };
    static const u8 ka[4][2] = { { POPA_R, POPA_1_R }, { POPA_G, POPA_1_G }, { POPA_B, POPA_1_B }, { POPA_ALPHA, POPA_1_ALPHA } };
    *src = i < 12 ? (u8)(PSRC_TEX0 + i / 4) : (u8)PSRC_PRIMARY;
    *op = alpha ? ka[ch][neg] : kc[ch][neg];
}

// A track's plan: one stage per (variable, polarity) term with its constant per component, then the constant: added
// first when positive, subtracted last when negative (per component, so both can occur), all at 1/s.
typedef struct { int n, s; int var[8]; bool neg[8]; u8 cv[8][3]; bool posK, negK; u8 kPos[3], kNeg[3]; } LinPlan;

static bool lin_plan(const Lin* comp, int nc, LinPlan* p) {
    memset(p, 0, sizeof *p);
    float K[3] = { 0, 0, 0 }, maxTerm = 0;
    for (int c = 0; c < nc; c++) {
        K[c] = comp[c].k;
        for (int i = 0; i < LV_N; i++) {
            if (comp[c].v[i] < 0) K[c] += comp[c].v[i];
            if (fabsf(comp[c].v[i]) > maxTerm) maxTerm = fabsf(comp[c].v[i]);
        }
    }
    int s = 0;
    for (int cand = 1; cand <= 4 && !s; cand *= 2) {
        bool ok = maxTerm / (float)cand <= 255.5f;
        for (int c = 0; c < nc && ok; c++) {
            if (K[c] >= 0) ok = K[c] / (float)cand <= 255.5f;
            else ok = -K[c] <= (float)(cand - 1) * 255.0f + 0.5f && -K[c] / (float)cand <= 255.5f;
        }
        if (ok) s = cand;
    }
    if (!s) return false;
    p->s = s;
    for (int i = 0; i < LV_N; i++)
        for (int pol = 0; pol < 2; pol++) {
            bool any = false;
            u8 cv[3] = { 0, 0, 0 };
            for (int c = 0; c < nc; c++) {
                float v = comp[c].v[i];
                if (v != 0 && (v < 0) == (pol == 1)) { any = true; cv[c] = q8(fabsf(v) / (float)s); }
            }
            if (!any) continue;
            if (p->n == 8) return false;
            p->var[p->n] = i; p->neg[p->n] = pol == 1;
            memcpy(p->cv[p->n], cv, 3);
            p->n++;
        }
    for (int c = 0; c < nc; c++) {
        if (K[c] > 0.25f) { p->posK = true; p->kPos[c] = q8(K[c] / (float)s); }
        if (K[c] < -0.25f) { p->negK = true; p->kNeg[c] = q8(-K[c] / (float)s); }
    }
    return true;
}

static int lin_plan_len(const LinPlan* p) { int n = p->n + p->posK + p->negK; return n ? n : 1; }

// Writes a track's plan into stages 0.. (the colour or alpha half of each stage), then passes the value through.
static void lin_emit(const LinPlan* p, bool alpha, int nStages, PicaTev* o) {
    int j = 0;
    bool have = false;  // PREVIOUS holds the partial sum
    u8 opAll = alpha ? POPA_ALPHA : POPC_COLOR;
    for (int step = 0; step < p->n + 2; step++) {
        bool isPos = step == 0, isNeg = step == p->n + 1;
        if ((isPos && !p->posK) || (isNeg && !p->negK)) continue;
        PicaStage* st = &o->st[j];
        u8* src = alpha ? st->srcA : st->srcRgb;
        u8* op = alpha ? st->opA : st->opRgb;
        u8 fn;
        const u8* cv;
        if (isPos) {
            cv = p->kPos; fn = PFN_REPLACE;
            src[0] = PSRC_CONSTANT; op[0] = opAll;
        } else if (isNeg) {
            cv = p->kNeg; fn = PFN_SUBTRACT;
            src[0] = PSRC_PREVIOUS; op[0] = opAll;
            src[1] = PSRC_CONSTANT; op[1] = opAll;
        } else {
            int t = step - 1;
            cv = p->cv[t];
            lin_operand(p->var[t], p->neg[t], alpha, &src[0], &op[0]);
            src[1] = PSRC_CONSTANT; op[1] = opAll;
            if (have) { fn = PFN_MULTIPLY_ADD; src[2] = PSRC_PREVIOUS; op[2] = opAll; }
            else fn = PFN_MODULATE;
        }
        if (alpha) { st->fnA = fn; st->scaleA = PSCALE_1; st->konst = (st->konst & 0x00FFFFFFu) | (u32)cv[0] << 24; }
        else { st->fnRgb = fn; st->scaleRgb = PSCALE_1; st->konst = (st->konst & 0xFF000000u) | cv[0] | (u32)cv[1] << 8 | (u32)cv[2] << 16; }
        have = true;
        j++;
    }
    if (!j) {  // no term and no constant: zero
        PicaStage* st = &o->st[0];
        if (alpha) { st->fnA = PFN_REPLACE; st->srcA[0] = PSRC_CONSTANT; st->opA[0] = POPA_ALPHA; st->konst &= 0x00FFFFFFu; }
        else { st->fnRgb = PFN_REPLACE; st->srcRgb[0] = PSRC_CONSTANT; st->opRgb[0] = POPC_COLOR; st->konst &= 0xFF000000u; }
        j = 1;
    }
    u8 sc = p->s == 4 ? PSCALE_4 : p->s == 2 ? PSCALE_2 : PSCALE_1;
    if (alpha) o->st[j - 1].scaleA = sc; else o->st[j - 1].scaleRgb = sc;
    for (; j < nStages; j++) {  // the other track has more stages: pass this one through
        PicaStage* st = &o->st[j];
        if (alpha) { st->fnA = PFN_REPLACE; st->srcA[0] = PSRC_PREVIOUS; st->opA[0] = POPA_ALPHA; st->scaleA = PSCALE_1; }
        else { st->fnRgb = PFN_REPLACE; st->srcRgb[0] = PSRC_PREVIOUS; st->opRgb[0] = POPC_COLOR; st->scaleRgb = PSCALE_1; }
    }
}

static bool tev_linear(const GXCore* gx, PicaTev* out) {
    LinCtx L;
    memset(&L, 0, sizeof L);
    L.ras = -1;
    for (int r = 0; r < 4; r++) {
        s32 v[4] = { sext11(gx->tevLo[r]), sext11(gx->tevHi[r] >> 12), sext11(gx->tevHi[r]), sext11(gx->tevLo[r] >> 12) };  // R G B A
        for (int k = 0; k < 4; k++) L.reg[r][k] = lin_k((float)v[k]);
    }
    u8 kv[4][4];
    for (int r = 0; r < 4; r++) {
        u32 lo = gx->kLo[r], hi = gx->kHi[r];
        kv[r][0] = (u8)(lo & 0xFF); kv[r][3] = (u8)((lo >> 12) & 0xFF);
        kv[r][2] = (u8)(hi & 0xFF); kv[r][1] = (u8)((hi >> 12) & 0xFF);
    }
    static const float kBias[4] = { 0, 128, -128, 0 }, kScale[4] = { 1, 2, 4, 0.5f };
    u32 stages = ((gx->bp[0x00] >> 10) & 15) + 1;
    for (u32 st = 0; st < stages; st++) {
        u32 tref = gx->bp[0x28 + st / 2] >> ((st & 1) * 12);
        u32 texmap = tref & 7, texcoord = (tref >> 3) & 7, chan = (tref >> 7) & 7;
        bool texEn = (tref >> 6) & 1;
        u32 ksel = gx->bp[0xF6 + st / 2];
        u32 kcs = (ksel >> (4 + (st & 1) * 10)) & 31, kas = (ksel >> (9 + (st & 1) * 10)) & 31;
        u32 cenv = gx->bp[0xC0 + 2 * st], aenv = gx->bp[0xC1 + 2 * st];
        u32 cb = (cenv >> 16) & 3, ab = (aenv >> 16) & 3;
        if (cb == 3 || ab == 3) return false;  // compare modes are not linear
        u8 rs[4], ts[4];
        swap_sel(gx, aenv & 3, rs);
        swap_sel(gx, (aenv >> 2) & 3, ts);
        LinIn tex, ras;
        if (texEn) {
            if (!lin_leaf_tex(&L, gx, (u8)texmap, (u8)texcoord, ts, &tex)) return false;
        } else {
            for (int k = 0; k < 3; k++) tex.c[k] = lin_k(255);  // run_tev: a stage without a texture reads white
            tex.a = lin_k(255);
        }
        if (chan < 2) {
            if (L.ras >= 0 && L.ras != (int)chan) return false;
            L.ras = (int)chan;
            for (int k = 0; k < 3; k++) { ras.c[k] = lin_k(0); ras.c[k].v[12 + rs[k]] = 255; }
            ras.a = lin_k(0); ras.a.v[12 + rs[3]] = 255;
        } else {
            for (int k = 0; k < 3; k++) ras.c[k] = lin_k(0);
            ras.a = lin_k(0);
        }
        Opnd kco = konst_op(kv, kcs, false), kao = konst_op(kv, kas, true);
        Lin kc[3] = { lin_k(kco.cv[0]), lin_k(kco.cv[1]), lin_k(kco.cv[2]) }, ka = lin_k(kao.cv[0]);
        Lin outC[3], outA;
        for (int k = 0; k < 3; k++) {
            Lin in[4];
            for (int i = 0; i < 4; i++) in[i] = lin_cin(&L, &tex, &ras, kc, (cenv >> (12 - 4 * i)) & 15, k);
            if (!lin_stage(&in[0], &in[1], &in[2], &in[3], (int)((cenv >> 18) & 1), kBias[cb], kScale[(cenv >> 20) & 3],
                           (cenv >> 19) & 1, &outC[k])) return false;
        }
        Lin ain[4];
        for (int i = 0; i < 4; i++) ain[i] = lin_ain(&L, &tex, &ras, &ka, (aenv >> (13 - 3 * i)) & 7);
        if (!lin_stage(&ain[0], &ain[1], &ain[2], &ain[3], (int)((aenv >> 18) & 1), kBias[ab], kScale[(aenv >> 20) & 3],
                       (aenv >> 19) & 1, &outA)) return false;
        u32 cd = (cenv >> 22) & 3, ad = (aenv >> 22) & 3;
        for (int k = 0; k < 3; k++) L.reg[cd][k] = outC[k];
        L.reg[ad][3] = outA;
    }
    LinPlan pc, pa;  // the output is PREV
    if (!lin_plan(L.reg[0], 3, &pc) || !lin_plan(&L.reg[0][3], 1, &pa)) return false;
    int n = lin_plan_len(&pc) > lin_plan_len(&pa) ? lin_plan_len(&pc) : lin_plan_len(&pa);
    if (n > PICA_STAGES) return false;
    memset(out, 0, sizeof *out);
    for (int i = 0; i < PICA_STAGES; i++) stage_default(&out->st[i]);
    for (int i = 0; i < n; i++) out->st[i].konst = 0;
    lin_emit(&pc, false, n, out);
    lin_emit(&pa, true, n, out);
    out->numStages = (u8)n;
    out->numTex = L.nUnits;
    for (int u = 0; u < L.nUnits; u++) { out->texMap[u] = L.unitMap[u]; out->texCoord[u] = L.unitCoord[u]; }
    out->rasChan = (u8)(L.ras < 0 ? 0 : L.ras);
    out->approx = 0;
    return true;
}

void tev_pica_compile(const GXCore* gx, PicaTev* out) {
    Ctx c;
    memset(&c, 0, sizeof c);
    for (int i = 0; i < PICA_STAGES; i++) stage_default(&c.o.st[i]);
    init_regs(&c, gx);

    u32 stages = ((gx->bp[0x00] >> 10) & 15) + 1;
    u8 needC[16], needA[16];
    liveness(gx, stages, needC, needA);

    u8 kv[4][4];  // konst colours R G B A
    for (int r = 0; r < 4; r++) {
        u32 lo = gx->kLo[r], hi = gx->kHi[r];
        kv[r][0] = (u8)(lo & 0xFF); kv[r][3] = (u8)((lo >> 12) & 0xFF);
        kv[r][2] = (u8)(hi & 0xFF); kv[r][1] = (u8)((hi >> 12) & 0xFF);
    }

    for (u32 st = 0; st < stages && !c.stop; st++) {
        if (!needC[st] && !needA[st]) continue;
        Ctx snap = c;
        u32 tref = gx->bp[0x28 + st / 2] >> ((st & 1) * 12);
        u32 texmap = tref & 7, texcoord = (tref >> 3) & 7, chan = (tref >> 7) & 7;
        bool texEn = (tref >> 6) & 1;
        u32 ksel = gx->bp[0xF6 + st / 2];
        u32 kcs = (ksel >> (4 + (st & 1) * 10)) & 31, kas = (ksel >> (9 + (st & 1) * 10)) & 31;
        u32 cenv = gx->bp[0xC0 + 2 * st], aenv = gx->bp[0xC1 + 2 * st];
        u8 rs[4], ts[4];
        swap_sel(gx, aenv & 3, rs);
        swap_sel(gx, (aenv >> 2) & 3, ts);

        Leaves lv;
        if (texEn) {
            lv.texC = leaf(OK_TEX, (u8)texmap, (u8)texcoord, ts[0], ts[1], ts[2]);
            lv.texA = leaf(OK_TEX, (u8)texmap, (u8)texcoord, ts[3], ts[3], ts[3]);
        } else {
            lv.texC = lv.texA = k_scalar(255);  // run_tev: a stage without a texture reads white
        }
        if (chan < 2) {
            lv.rasC = leaf(OK_RAS, (u8)chan, 0, rs[0], rs[1], rs[2]);
            lv.rasA = leaf(OK_RAS, (u8)chan, 0, rs[3], rs[3], rs[3]);
        } else {
            lv.rasC = lv.rasA = k_scalar(0);  // bump alpha and the null channel read as 0 here (no indirect texturing)
        }
        lv.kc = konst_op(kv, kcs, false);
        lv.ka = konst_op(kv, kas, true);

        u8 dest[2] = { (u8)((cenv >> 22) & 3), (u8)((aenv >> 22) & 3) };
        u8 need[2] = { needC[st], needA[st] };
        u8 clamp[2] = { (u8)((cenv >> 19) & 1), (u8)((aenv >> 19) & 1) };
        TrackRes res[2];
        memset(res, 0, sizeof res);
        for (int t = 0; t < 2 && !c.stop; t++) {
            if (!need[t]) continue;
            u32 env = t == T_RGB ? cenv : aenv;
            Opnd in[4];
            for (int i = 0; i < 4; i++)
                in[i] = t == T_RGB ? cin_opnd(&c, &lv, (cenv >> (12 - 4 * i)) & 15) : ain_opnd(&c, &lv, (aenv >> (13 - 3 * i)) & 7);
            for (int i = 0; i < 4; i++)
                if (in[i].unclamped) c.o.approx |= TEVA_RANGE;  // out-of-range GX values are not representable (flagged even if folded away)
            u32 bias = (env >> 16) & 3, sub = (env >> 18) & 1, scale = (env >> 20) & 3;
            if (bias != 3 && in[0].kind == OK_CONST && in[1].kind == OK_CONST && in[2].kind == OK_CONST && in[3].kind == OK_CONST) {
                static const float kBias[3] = { 0, 128, -128 }, kScale[4] = { 1, 2, 4, 0.5f };
                float L[3];
                for (int k = 0; k < 3; k++) L[k] = gx_lerp(in[0].cv[k], in[1].cv[k], in[2].cv[k]);
                res[t].alias = 1;  // every input is known: the stage is a constant
                res[t].aop = k_stage(&in[3], L, (int)sub, kBias[bias], kScale[scale]);
                continue;
            }
            int sb = (int)sub, bm = bias == 2, sc = (int)scale;
            if (bias == 3) {
                // Compare: d + (A > B ? C : 0) (or == B). Approximated as always taking C.
                c.o.approx |= TEVA_COMPARE;
                in[0] = in[2]; in[1] = k_scalar(0); in[2] = k_scalar(0);
                sb = 0; bm = 0; sc = 0;
            } else {
                if (bias == 1 || (sub && bias == 2)) { c.o.approx |= TEVA_BIAS; bm = 0; }  // +0.5 has no PICA form; nor does subtract with a bias
                if (scale == 3) { c.o.approx |= TEVA_BIAS; sc = 0; }                        // scale 0.5 likewise
            }
            plan_track(&c, t, t == T_ALPHA ? res[0].off + res[0].n : 0, in, sb, bm, sc, &res[t]);
        }
        if (c.stop) { c = snap; c.o.approx |= TEVA_STAGES; c.stop = 1; break; }  // drop the group that did not fit
        commit_group(&c, c.nst, dest, need, clamp, res);
    }

    // The final PREV must be what the last PICA stage outputs. A track that only passes through in the last stage takes
    // its REPLACE there; otherwise one more stage is needed.
    bool fin[2];
    for (int t = 0; t < 2; t++) {
        const Opnd* v = &c.loc[0][t];
        // The value must be the previous stage's own track output, unmodified (not the other track's, not inverted).
        fin[t] = !(v->kind == OK_VAL && c.nst && v->b == t && !v->inv && v->sel[0] == (t ? 3 : 0) && in_prev(&c, t, v->a, c.nst, 255));
    }
    bool extra[2] = { false, false };
    for (int t = 0; t < 2; t++) {
        if (!fin[t]) continue;
        extra[t] = true;
        if (c.nst && !((c.opMask[t] >> (c.nst - 1)) & 1)) {
            POp op;
            set_op(&op, PFN_REPLACE, PSCALE_1, &c.loc[0][t], NULL, NULL);
            Ctx tmp = c;
            if (place_op(&tmp, c.nst - 1, t, &op)) { c = tmp; extra[t] = false; }
        }
    }
    if (extra[0] || extra[1]) {
        if (c.nst >= PICA_STAGES) c.o.approx |= TEVA_STAGES;
        else {
            for (int pass = 0; pass < 2; pass++) {
                Ctx tmp = c;
                tmp.force = (u8)pass;
                bool ok = true;
                for (int t = 0; t < 2 && ok; t++) {
                    if (!extra[t]) continue;
                    POp op;
                    set_op(&op, PFN_REPLACE, PSCALE_1, &c.loc[0][t], NULL, NULL);
                    ok = place_op(&tmp, c.nst, t, &op);
                }
                if (ok) { c = tmp; break; }
            }
            c.force = 0;
            c.nst++;
        }
    }

    c.o.numStages = c.nst ? c.nst : 1;
    // Out-of-range intermediates or too many stages, and nothing else: a linear program compiles exactly another way.
    if ((c.o.approx & (TEVA_RANGE | TEVA_STAGES)) && !(c.o.approx & ~(u32)(TEVA_RANGE | TEVA_STAGES)) && tev_linear(gx, out)) return;
    *out = c.o;
}
