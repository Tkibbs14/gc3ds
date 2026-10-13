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
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
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

static void attr_array(const GXCore* gx, VAttr* a, u32 arr) {
    if (a->mode < 2) return;
    a->base = gx->cp[CP_ARRAY_BASE + arr] & 0x03FFFFFF;
    a->stride = gx->cp[CP_ARRAY_STRIDE + arr] & 0xFF;
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
// A draw is done in two halves so the second can run on another core (the 3DS backend's vertex worker):
// - vfast_prepare reads everything that belongs to the moment of the draw (the vertex stream and arrays, the CP and XF
//   registers), gathers the vertices, lights them, and copies the few matrices the vertices use into the job;
// - vfast_finish computes clip positions, shader-lighting inputs and texture coordinates from the job alone.
VFastStats g_vfastStats;

// One gathered vertex: the attributes the draw uses, decoded, plus channel 0's lit colour for colour texgen. pn and
// tm[] index the job's own matrix copies once vfast_prepare has finished.
typedef struct { float p[3], n[3], t[8][2], c0[2]; u32 col[2]; u8 pn, hasColor, tm[8]; } VIn;

// What a texture unit's coordinate comes from: zero, channel 0's colour, or texgen `gi` from `kind` input at `off`.
typedef struct { u8 mode, gi, kind; bool stq; u32 off; } VUnit;  // mode: 0 zero, 1 colour, 2 texgen
enum { VU_ZERO, VU_COLOUR, VU_GEN };

struct VJob {
    u32 count, nPos, nTex;
    bool gpuLight, gpuPos;
    float slot3[64];   // gpuPos: 3 * palette slot per local position matrix (the shader's pal[] row)
    float origin[3];   // gpuPos: subtracted from every position (vfast_job_origin)
    bool originDone;
    float P[16];
    VUnit unit[3];
    u8* end;           // the end of what the job uses (vfast_job_used)
    VCache* vc;        // the display-list vertex cache gathers use (vfast_set_sources / vfast_job_set_cache)
    u32 srcN;          // stream segments in guest RAM (0: not cacheable)
    const u32 *srcPa, *srcSeg;  // each segment's physical address (VC_NO_PA: not in RAM) and prim << 16 | count
    u32 onePa, oneSeg; // a single segment's, held in the job
    u32 made;          // g_memStamps.now before the job read guest RAM (the stream copy of a deferred gather is that old)
    VIn* in;
    float (*pos)[21];  // position matrix (3x4) followed by its normal matrix (3x3), per local index
    float (*tex)[12];  // texture matrices (3x4), per local index
    // The gather's inputs. A deferred job (vfast_prepare's `defer`) has only pn / tm read and a copy of the stream in
    // `raw`; vfast_finish gathers the attributes (reading guest memory) before it transforms.
    bool gathered, nrmUsed, nbt, index3, pnDirect;
    u8 rasChan, pnDefault, tmDefault[8];
    u32 texUsed, tmDirect, vsize;
    const u8* mem1;
    const u8* raw;
    VAttr pos_, nrm_, col_[2], tex_[8];
};

// One generated texture coordinate (xform.c's regular texgen): texture matrix m times the input, divided by q when
// stq. kind: TG_XYZ three components at byte offset `off` of the vertex, TG_XY two, TG_NONE (0, 0, 1); w = 1.
enum { TG_XYZ, TG_XY, TG_NONE };
VF_INLINE void texgen_one(const float* m, const VIn* in, int kind, u32 off, bool stq, float tc[2]) {
    const float* a = (const float*)((const u8*)in + off);
    float x = 0, y = 0, z = 1;
    if (kind == TG_XYZ) { x = a[0]; y = a[1]; z = a[2]; }
    else if (kind == TG_XY) { x = a[0]; y = a[1]; }
    float s = m[0] * x + m[1] * y + m[2] * z + m[3], t = m[4] * x + m[5] * y + m[6] * z + m[7];
    if (stq) {
        float q = m[8] * x + m[9] * y + m[10] * z + m[11], iq = q != 0 ? 1.0f / q : 1.0f;
        s *= iq;
        t *= iq;
    }
    tc[0] = s;
    tc[1] = t;
}

// Bounds for the job memory: one position matrix unless the vertices carry their own index, one texture matrix per
// unit unless they carry those.
static u32 pos_cap(const GXCore* gx, u32 count) { return (gx->cp[CP_VCD_LO] & 1) ? (count < 64 ? count : 64) : 1; }
static u32 tex_cap(const GXCore* gx, u32 count) {
    u32 tmDirect = (gx->cp[CP_VCD_LO] >> 1) & 0xFF;
    return tmDirect ? 3 * (count < 64 ? count : 64) : 3;
}

static u32 align8(u32 v) { return (v + 7) & ~7u; }

// Job memory: VJob, the vertices, the stream copy, then room for the most position and texture matrices the draw can
// use. vfast_prepare packs the matrices it used at the start of that room, so the job ends at vfast_job_used bytes and a
// ring allocator can take the rest back (the 3DS vertex worker's job ring: most draws use a few of the 64 matrices).
u32 vfast_job_bytes(const GXCore* gx, u32 count, u32 vsize) {
    return align8(sizeof(VJob)) + align8(count * (u32)sizeof(VIn)) + align8(count * vsize) +
           align8(pos_cap(gx, count) * 21 * 4) + align8(tex_cap(gx, count) * 12 * 4);
}
u32 vfast_job_used(const VJob* job) { return (u32)(job->end - (const u8*)job); }

// The vertices' matrix indices (pn, tm) from the stream or the defaults.
static void gather_indices(VJob* job) {
    for (u32 vi = 0; vi < job->count; vi++) {
        const u8* p = job->raw + vi * job->vsize;
        VIn* in = &job->in[vi];
        in->pn = job->pnDefault;
        if (job->pnDirect) in->pn = *p++;
        memcpy(in->tm, job->tmDefault, 8);
        for (u32 i = 0; i < 8; i++) if ((job->tmDirect >> i) & 1) in->tm[i] = *p++;
    }
}

// All reads of guest memory: the attributes from the stream and the arrays (the indices too unless `indices` is
// false, when gather_indices has already run and pn / tm may hold local matrix numbers).
// Prefetching the array elements of indexed attributes a few vertices ahead (vfast_set_prefetch). Twilight Princess's
// vertices read ~3.6 indexed attributes each from scattered places in guest RAM, and the 3DS vertex worker spent ~2.2 us
// (~1,750 cycles) per vertex (2DS, bf169a0): mostly waiting for memory.
bool g_vfastPrefetch = true;
void vfast_set_prefetch(bool on) { g_vfastPrefetch = on; }
#define PF_AHEAD 4
typedef struct { u32 off, base, stride; u8 wide; } VPf;  // an index's byte offset in the record; 16-bit index when wide

static u32 prefetch_table(const VJob* job, VPf* pf) {
    u32 n = 0, off = (job->pnDirect ? 1u : 0u) + (u32)__builtin_popcount(job->tmDirect);
    const VAttr* a[12] = { &job->pos_, &job->nrm_, &job->col_[0], &job->col_[1] };
    for (int i = 0; i < 8; i++) a[4 + i] = &job->tex_[i];
    for (int i = 0; i < 12; i++) {
        const VAttr* v = a[i];
        if (!v->mode) continue;
        u32 direct = i == 2 || i == 3 ? v->cs : (u32)v->n * v->cs;
        if (v->mode == 1) { off += direct; continue; }
        u32 isz = v->mode == 2 ? 1 : 2, count = i == 1 && job->nbt && job->index3 ? 3 : 1;
        if (n < 8) pf[n++] = (VPf){ off, v->base, v->stride, (u8)(isz == 2) };
        off += isz * count;
    }
    return n;
}

static void gather_range(VJob* job, bool indices, u32 from, u32 to) {
    const u8* mem1 = job->mem1;
    VAttr pos = job->pos_, nrm = job->nrm_, col[2] = { job->col_[0], job->col_[1] }, tex[8];
    memcpy(tex, job->tex_, sizeof tex);
    bool nrmUsed = job->nrmUsed, nbt = job->nbt, index3 = job->index3, pnDirect = job->pnDirect;
    u32 texUsed = job->texUsed, tmDirect = job->tmDirect, skip = (pnDirect ? 1u : 0u) + (u32)__builtin_popcount(tmDirect);
    VPf pf[8];
    u32 npf = g_vfastPrefetch ? prefetch_table(job, pf) : 0;
    for (u32 vi = from; vi < to; vi++) {
        const u8* p = job->raw + vi * job->vsize;
        if (npf && vi + PF_AHEAD < to) {
            const u8* q = job->raw + (vi + PF_AHEAD) * job->vsize;
            for (u32 k = 0; k < npf; k++) {
                u32 idx = pf[k].wide ? be16(q + pf[k].off) : q[pf[k].off];
                u32 pa = pf[k].base + idx * pf[k].stride;
                if (pa < PA_MEM1_END) __builtin_prefetch(mem1 + pa);
            }
        }
        VIn* in = &job->in[vi];
        if (indices) {
            in->pn = job->pnDefault;
            if (pnDirect) in->pn = *p++;
            memcpy(in->tm, job->tmDefault, 8);
            for (u32 i = 0; i < 8; i++) if ((tmDirect >> i) & 1) in->tm[i] = *p++;
        } else {
            p += skip;
        }
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
}

// Shader lighting: the lit channel's raw vertex colour (eye and normal come later).
static void gpu_colours(const VJob* job, PVert* out) {
    u32 ch = job->rasChan & 1;
    for (u32 vi = 0; vi < job->count; vi++) {
        const VIn* in = &job->in[vi];
        u32 cc = ch == 1 && !(in->hasColor & 2) && (in->hasColor & 1) ? in->col[0] : in->col[ch];
        u8* o = out[vi].col;
        o[0] = (u8)(cc >> 24); o[1] = (u8)(cc >> 16); o[2] = (u8)(cc >> 8); o[3] = (u8)cc;
    }
}

// A draw's setup: what vfast_prepare works out from registers before it reads any vertex.
typedef struct {
    u32 key[6];
    bool valid;
    float P[16];
    VUnit unit[3];
    u32 numChans, chans, genUsed, texUsed, tmDirect, pnDefault;
    bool nrmUsed, eyeUsed, nbt, index3, pnDirect;
    u8 tmDefault[8];
    VHalf half[2][2];
    VAttr pos, nrm, col[2], tex[8];
} VSetup;
static VSetup g_vs[2];  // by lighting mode (CPU, shader), which draws can alternate
static u32 g_vsGen;
void vfast_reset_cache(void) { g_vsGen++; }

static void setup_compute(VSetup* S, const GXCore* gx, u32 vat, const VFold* f, const VSel* sel, bool gpuLight) {
    u32 lo = gx->cp[CP_VCD_LO], hi = gx->cp[CP_VCD_HI];
    u32 a = gx->cp[CP_VAT_A + vat], b = gx->cp[CP_VAT_B + vat], c = gx->cp[CP_VAT_C + vat];
    u32 ma = gx->cp[CP_MATINDEX_A], mb = gx->cp[CP_MATINDEX_B];
    proj_fold(gx, f, S->P);

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
    for (u32 ch = 0; ch < 2; ch++)
        for (u32 k = 0; k < 2; k++) { S->half[ch][k].n = 0; S->half[ch][k].lit = 0; }
    // The shader lights from eye position and normal; it is only chosen when no colour is lit on the CPU (no
    // texgen from a lit colour), so the lights are not read here: parsing 7-8 of them per draw cost 2-4k cycles.
    if (gpuLight) nrmUsed = true;
    else
        for (u32 ch = 0; ch < 2; ch++) {
            if (!(chans & (1u << ch)) || ch >= numChans) continue;
            half_init(gx, ch, gx->xf[0x100E + ch], &S->half[ch][0]);
            half_init(gx, ch, gx->xf[0x1010 + ch], &S->half[ch][1]);
            if (S->half[ch][0].n || S->half[ch][1].n) nrmUsed = eyeUsed = true;
        }

    // Texture units.
    for (u32 u = 0; u < 3; u++) {
        VUnit* vu = &S->unit[u];
        u32 gi = sel->texCoord[u] & 7;
        memset(vu, 0, sizeof *vu);
        vu->mode = VU_ZERO;
        vu->gi = (u8)gi;
        if (u >= numTex || !((genUsed >> gi) & 1)) continue;
        u32 cfg = gx->xf[0x1040 + gi], type = (cfg >> 4) & 7, src = (cfg >> 7) & 31;
        if (type >= 2) { vu->mode = VU_COLOUR; continue; }  // colour texgen: s, t from channel 0
        vu->mode = VU_GEN;
        vu->stq = (cfg >> 1) & 1;
        // Input: three components at `off` (position, normal), two (a texture coordinate), or none (0, 0, 1, 1).
        vu->kind = (u8)(src <= 1 ? TG_XYZ : (src >= 5 && src <= 12) ? TG_XY : TG_NONE);
        vu->off = src == 0 ? offsetof(VIn, p) : src == 1 ? offsetof(VIn, n) : vu->kind == TG_XY ? offsetof(VIn, t) + 8 * (src - 5) : 0;
    }

    // Decode layout.
    for (int i = 0; i < 8; i++) S->tmDefault[i] = (u8)(i < 4 ? (ma >> (6 + 6 * i)) & 63 : (mb >> (6 * (i - 4))) & 63);
    S->tmDirect = (lo >> 1) & 0xFF;
    memset(&S->pos, 0, sizeof S->pos);
    memset(&S->nrm, 0, sizeof S->nrm);
    memset(S->col, 0, sizeof S->col);
    memset(S->tex, 0, sizeof S->tex);
    {
        u32 fmt = (a >> 1) & 7;
        attr_init(gx, &S->pos, (lo >> 9) & 3, ARR_POS, fmt, (a & 1) ? 3 : 2, comp_size(fmt),
                  fmt == 4 ? 1.0f : 1.0f / (float)(1u << ((a >> 4) & 31)));
    }
    S->nbt = (a >> 9) & 1;
    S->index3 = (a >> 31) & 1;
    {
        static const u8 shift[4] = { 7, 6, 15, 14 };
        u32 fmt = (a >> 10) & 7;
        attr_init(gx, &S->nrm, (lo >> 11) & 3, ARR_NRM, fmt, S->nbt ? 9 : 3, comp_size(fmt),
                  fmt >= 4 ? 1.0f : 1.0f / (float)(1u << shift[fmt]));
    }
    for (u32 i = 0; i < 2; i++) {
        u32 fmt = (a >> (14 + 4 * i)) & 7;
        attr_init(gx, &S->col[i], (lo >> (13 + 2 * i)) & 3, ARR_COL0 + i, fmt, 1, color_size(fmt), 1.0f);
    }
    {
        u32 tcnt[8] = { (a >> 21) & 1, b & 1, (b >> 9) & 1, (b >> 18) & 1, (b >> 27) & 1, (c >> 5) & 1, (c >> 14) & 1, (c >> 23) & 1 };
        u32 tfmt[8] = { (a >> 22) & 7, (b >> 1) & 7, (b >> 10) & 7, (b >> 19) & 7, (b >> 28) & 7, (c >> 6) & 7, (c >> 15) & 7, (c >> 24) & 7 };
        u32 tsh[8] = { (a >> 25) & 31, (b >> 4) & 31, (b >> 13) & 31, (b >> 22) & 31, c & 31, (c >> 9) & 31, (c >> 18) & 31, (c >> 27) & 31 };
        for (u32 i = 0; i < 8; i++)
            attr_init(gx, &S->tex[i], (hi >> (2 * i)) & 3, ARR_TEX0 + i, tfmt[i], tcnt[i] ? 2 : 1, comp_size(tfmt[i]),
                      tfmt[i] == 4 ? 1.0f : 1.0f / (float)(1u << tsh[i]));
    }
    S->pnDefault = ma & 63;
    S->pnDirect = lo & 1;
    S->numChans = numChans;
    S->chans = chans;
    S->genUsed = genUsed;
    S->texUsed = texUsed;
    S->nrmUsed = nrmUsed;
    S->eyeUsed = eyeUsed;
}

// ---- display-list vertex cache (docs/DL_CACHE_PLAN.md) ----
// 99.6% of Twilight Princess's draws come from display lists and ~85% of list calls replay the same bytes over the same
// arrays (2DS, job 107), while the vertex worker spent 58-62% of its time gathering attributes from guest RAM. A cache
// per thread keeps the gathered attributes (position, normal, texture coordinates, colours: not the matrix indices,
// which come from the registers of each draw) of each stream segment in RAM, keyed by its physical address, vertex
// count and size and a hash of the vertex setup (formats, scales, array bases and strides). An entry records the RAM
// pages its stream and array elements cover and g_memStamps.now before it gathered; it is used only while none of
// those pages has a later stamp (gx.h: what the game flushed or DMA wrote).
bool g_vcacheOn = true;
bool g_vcacheCheck;  // VCACHE_CHECK (PC): every hit is gathered again and compared (vcache_check_failures)
bool g_vcachePlant;  // the checker's self-test: page stamps ignored, so a changed array must show as a failure
u64 g_vcacheCheckFails, g_vcacheChecked;
u64 g_vfPhase[3];
bool g_vfPhaseTimed;
static inline u64 vf_clock(void) { return g_vfPhaseTimed && g_profClock ? g_profClock() : 0; }
// The display-list cache's share of the gather on marked calls: [0] lookups and copies of hits, [1] gathers of missed
// runs, [2] inserts (2DS job 113: 84% of lookups hit and the gather phase did not shrink).
u64 g_vcPhase[3];

#define VC_RANGES 8
typedef struct {
    u32 pa, count, vsize, made, key[2];
    u64 at;   // the data's position in the ring's running byte count
    u32 off;  // and in the ring (no 64-bit modulo on the 2DS: it is a library call)
    u8 nr;
    u16 r0[VC_RANGES], r1[VC_RANGES];
} VCEnt;
struct VCache {
    VCEnt* ent;
    u32 nEnt;  // a power of two; two-way: a segment's slots are the pair at its even index
    u8* ring;
    u32 size;
    u64 total;
    u32 pos;  // total's place in the ring
    VCacheStats st;
};

VCache* vcache_create(u32 bytes, u32 entries) {
    VCache* vc = calloc(1, sizeof *vc);
    if (!vc) return NULL;
    u32 n = 2;
    while (n < entries) n <<= 1;
    vc->ent = calloc(n, sizeof *vc->ent);
    vc->ring = malloc(bytes);
    if (!vc->ent || !vc->ring) { free(vc->ent); free(vc->ring); free(vc); return NULL; }
    vc->nEnt = n;
    vc->size = bytes;
    for (u32 i = 0; i < n; i++) vc->ent[i].pa = VC_NO_PA;
    return vc;
}
void vcache_reset(VCache* vc) { if (vc) for (u32 i = 0; i < vc->nEnt; i++) vc->ent[i].pa = VC_NO_PA; }
const VCacheStats* vcache_stats(const VCache* vc) { return vc ? &vc->st : NULL; }

static VCache* g_pendVc;
static const u32 *g_pendPa, *g_pendSeg;
static u32 g_pendN;
void vfast_set_sources(VCache* vc, const u32* pa, const u32* seg, u32 n) { g_pendVc = vc; g_pendPa = pa; g_pendSeg = seg; g_pendN = n; }
void vfast_job_set_cache(VJob* job, VCache* vc) { job->vc = vc; }

static void vc_key(const VJob* job, u32 key[2]) {
    u32 h0 = 2166136261u, h1 = 0x9747B28Cu;
    const VAttr* a[12] = { &job->pos_, &job->nrm_, &job->col_[0], &job->col_[1] };
    for (int i = 0; i < 8; i++) a[4 + i] = &job->tex_[i];
    for (int i = 0; i < 12; i++) {
        const u8* b = (const u8*)a[i];
        for (u32 k = 0; k < sizeof(VAttr); k++) { h0 = (h0 ^ b[k]) * 16777619u; h1 = (h1 ^ b[k]) * 0x5bd1e995u; h1 ^= h1 >> 15; }
    }
    u32 f[4] = { (u32)job->nrmUsed | (u32)job->nbt << 1 | (u32)job->index3 << 2 | (u32)job->pnDirect << 3,
                 job->texUsed, job->tmDirect, job->vsize };
    for (int i = 0; i < 4; i++) { h0 = (h0 ^ f[i]) * 16777619u; h1 = (h1 ^ f[i]) * 0x5bd1e995u; h1 ^= h1 >> 15; }
    key[0] = h0; key[1] = h1;
}

// The pair of slots a segment may live in.
static VCEnt* vc_pair(VCache* vc, u32 pa, u32 count, const u32 key[2]) {
    u32 h = (pa >> 5) * 2654435761u ^ count * 40503u ^ key[0];
    return &vc->ent[(h ^ (h >> 16)) & (vc->nEnt - 2)];
}
static bool vc_match(const VCEnt* e, const VJob* job, u32 pa, u32 cnt, const u32 key[2]) {
    return e->pa == pa && e->count == cnt && e->vsize == job->vsize && e->key[0] == key[0] && e->key[1] == key[1];
}
static bool vc_ring_ok(const VCache* vc, const VCEnt* e) { return vc->total - e->at <= vc->size; }  // not written over

static bool vc_fresh(const VCEnt* e) {
    if (g_vcachePlant) return true;
    for (u32 r = 0; r < e->nr; r++)
        for (u32 p = e->r0[r]; p <= e->r1[r]; p++)
            if ((s32)(g_memStamps.page[p] - e->made) > 0) return false;
    return true;
}
static u32 vc_pages(const VCEnt* e) { u32 n = 0; for (u32 r = 0; r < e->nr; r++) n += e->r1[r] - e->r0[r] + 1u; return n; }

// The pages [lo, hi] (bytes) into the entry's ranges, merging overlaps; false when they do not fit.
static bool vc_range(VCEnt* e, u32 lo, u32 hi) {
    if (lo >= PA_MEM1_END) return true;
    if (hi >= PA_MEM1_END) hi = PA_MEM1_END - 1;
    u16 a = (u16)(lo >> 12), b = (u16)(hi >> 12);
    for (u32 r = 0; r < e->nr; r++)
        if (a <= e->r1[r] + 1 && b + 1 >= e->r0[r]) {
            if (a < e->r0[r]) e->r0[r] = a;
            if (b > e->r1[r]) e->r1[r] = b;
            return true;
        }
    if (e->nr == VC_RANGES) return false;
    e->r0[e->nr] = a; e->r1[e->nr] = b; e->nr++;
    return true;
}

// Every index the vertex records hold (the prefetch table without its limit, and all three of an NBT normal's).
typedef struct { u32 off, base, stride; bool wide; } VIdx;
static u32 index_list(const VJob* job, VIdx* out) {
    u32 n = 0, off = (job->pnDirect ? 1u : 0u) + (u32)__builtin_popcount(job->tmDirect);
    const VAttr* a[12] = { &job->pos_, &job->nrm_, &job->col_[0], &job->col_[1] };
    for (int i = 0; i < 8; i++) a[4 + i] = &job->tex_[i];
    for (int i = 0; i < 12; i++) {
        const VAttr* v = a[i];
        if (!v->mode) continue;
        u32 direct = i == 2 || i == 3 ? v->cs : (u32)v->n * v->cs;
        if (v->mode == 1) { off += direct; continue; }
        u32 isz = v->mode == 2 ? 1 : 2, count = i == 1 && job->nbt && job->index3 ? 3 : 1;
        for (u32 c = 0; c < count; c++) out[n++] = (VIdx){ off + c * isz, v->base, v->stride, isz == 2 };
        off += isz * count;
    }
    return n;
}

// A cached vertex holds only what the draw uses (2DS job 111: whole 116-byte VIns filled a 2.5 MB ring in less than a
// frame and the worker's cache hit 0.7%): position, the normal when used, both colours and which are present, and the
// used texture coordinates. Not c0, pn or tm (per draw).
static u32 vc_rec(const VJob* job) { return 24u + (job->nrmUsed ? 12u : 0u) + 8u * (u32)__builtin_popcount(job->texUsed); }
static void vc_pack(u8* d, const VJob* job, const VIn* in, u32 n) {
    for (u32 i = 0; i < n; i++, in++) {
        memcpy(d, in->p, 12); d += 12;
        if (job->nrmUsed) { memcpy(d, in->n, 12); d += 12; }
        memcpy(d, in->col, 8); d += 8;
        u32 hc = in->hasColor;
        memcpy(d, &hc, 4); d += 4;
        for (u32 t = 0; t < 8; t++) if ((job->texUsed >> t) & 1) { memcpy(d, in->t[t], 8); d += 8; }
    }
}
static void vc_unpack(VIn* out, const VJob* job, const u8* d, u32 n) {
    for (u32 i = 0; i < n; i++, out++) {
        memcpy(out->p, d, 12); d += 12;
        if (job->nrmUsed) { memcpy(out->n, d, 12); d += 12; } else out->n[0] = out->n[1] = out->n[2] = 0;
        memcpy(out->col, d, 8); d += 8;
        u32 hc;
        memcpy(&hc, d, 4); d += 4;
        out->hasColor = (u8)hc;
        for (u32 t = 0; t < 8; t++) if ((job->texUsed >> t) & 1) { memcpy(out->t[t], d, 8); d += 8; }
    }
}

static void vc_put(VCache* vc, const VJob* job, u32 pa, u32 from, u32 cnt, const u32 key[2], u32 made) {
    u32 len = cnt * vc_rec(job);
    if (len > vc->size / 4) return;
    VCEnt e;
    memset(&e, 0, sizeof e);
    e.pa = pa; e.count = cnt; e.vsize = job->vsize; e.made = made; e.key[0] = key[0]; e.key[1] = key[1];
    bool ok = vc_range(&e, pa, pa + cnt * job->vsize - 1);  // the stream (and the display list around it)
    VIdx ix[16];
    u32 ni = index_list(job, ix);
    for (u32 k = 0; k < ni && ok; k++) {  // the array elements the segment's indices reach
        u32 lo = 0xFFFFFFFFu, hi = 0;
        for (u32 vi = from; vi < from + cnt; vi++) {
            const u8* p = job->raw + vi * job->vsize + ix[k].off;
            u32 idx = ix[k].wide ? ((u32)p[0] << 8 | p[1]) : p[0];
            if (idx < lo) lo = idx;
            if (idx > hi) hi = idx;
        }
        ok = vc_range(&e, ix[k].base + lo * ix[k].stride, ix[k].base + hi * ix[k].stride + 63);
    }
    if (!ok) { vc->st.uncacheable++; return; }
    if (vc->pos + len > vc->size) { vc->total += vc->size - vc->pos; vc->pos = 0; }
    vc_pack(vc->ring + vc->pos, job, job->in + from, cnt);
    e.at = vc->total;
    e.off = vc->pos;
    vc->total += len;
    vc->pos += len;
    // The slot of the pair that holds the same segment, else a free or written-over one, else the older one.
    VCEnt* s = vc_pair(vc, pa, cnt, key);
    VCEnt* d = vc_match(&s[1], job, pa, cnt, key) ? &s[1] : vc_match(&s[0], job, pa, cnt, key) ? &s[0]
             : s[0].pa == VC_NO_PA || !vc_ring_ok(vc, &s[0]) ? &s[0] : s[1].pa == VC_NO_PA || !vc_ring_ok(vc, &s[1]) ? &s[1]
             : s[0].at < s[1].at ? &s[0] : &s[1];
    if (d->pa != VC_NO_PA && !vc_match(d, job, pa, cnt, key) && vc_ring_ok(vc, d)) vc->st.evicted++;
    *d = e;
    vc->st.inserts++;
}

static bool vc_same(const VJob* job, const VIn* a, const VIn* b, u32 n) {
    for (u32 i = 0; i < n; i++) {
        if (memcmp(a[i].p, b[i].p, sizeof a[i].p) || memcmp(a[i].col, b[i].col, sizeof a[i].col) || a[i].hasColor != b[i].hasColor) return false;
        if (memcmp(a[i].n, b[i].n, sizeof a[i].n)) return false;
        for (u32 t = 0; t < 8; t++) if (((job->texUsed >> t) & 1) && memcmp(a[i].t[t], b[i].t[t], sizeof a[i].t[t])) return false;
    }
    return true;
}

static void gather_range(VJob* job, bool indices, u32 from, u32 to);
// A segment's vertex count, clipped to the job.
static u32 seg_count(const VJob* job, u32 k, u32 at) {
    u32 cnt = job->srcSeg[k] & 0xFFFF;
    return at + cnt > job->count ? job->count - at : cnt;
}

// Segments that missed are gathered in runs, not one by one: TP's segments average ~5 vertices, and per-segment calls
// lost the prefetch of indexed attributes 4 vertices ahead (2DS job 112: the cache hit 60% and was still slower).
static void gather_cached(VJob* job, bool indices) {
    VCache* vc = job->vc;
    if (!vc || !g_vcacheOn || !job->srcN) { gather_range(job, indices, 0, job->count); return; }
    if (indices) gather_indices(job);  // matrix indices: from the stream and this draw's registers, never cached
    u32 key[2];
    vc_key(job, key);
    u32 at = 0, runK = 0, runAt = 0;
    bool inRun = false;
    u64 tl = vf_clock(), tLook = 0, tGather = 0, tPut = 0;
    for (u32 k = 0; k <= job->srcN; k++) {
        bool end = k == job->srcN || at >= job->count;
        bool hit = false;
        u32 cnt = 0, pa = VC_NO_PA;
        if (!end) {
            cnt = seg_count(job, k, at);
            pa = job->srcPa[k];
            VCEnt* s = pa == VC_NO_PA ? NULL : vc_pair(vc, pa, cnt, key);
            VCEnt* e = !s ? NULL : vc_match(&s[0], job, pa, cnt, key) ? &s[0] : vc_match(&s[1], job, pa, cnt, key) ? &s[1] : NULL;
            bool ringOk = e && vc_ring_ok(vc, e);
            hit = ringOk && vc_fresh(e);
            if (ringOk) vc->st.pagesChecked += vc_pages(e);
            if (hit) {
                vc_unpack(job->in + at, job, vc->ring + e->off, cnt);
                vc->st.hits++;
                vc->st.hitVerts += cnt;
                if (g_vcacheCheck) {  // the checker: gather again and compare
                    static VIn fresh[1024], keep[1024];  // (PC only; static: the 3DS worker's stack is 32 KB)
                    if (cnt <= 1024) {
                        memcpy(keep, job->in + at, cnt * sizeof(VIn));
                        gather_range(job, false, at, at + cnt);
                        memcpy(fresh, job->in + at, cnt * sizeof(VIn));
                        memcpy(job->in + at, keep, cnt * sizeof(VIn));
                        g_vcacheChecked++;
                        if (!vc_same(job, fresh, keep, cnt)) g_vcacheCheckFails++;
                    }
                }
            } else if (pa == VC_NO_PA) {
                vc->st.uncacheable++;
            } else if (!e) {
                vc->st.misses++;
            } else if (!ringOk) {
                vc->st.staleRing++;
            } else {
                vc->st.stale++;
            }
        }
        if (!hit && !end && !inRun) { inRun = true; runK = k; runAt = at; }
        if ((hit || end) && inRun) {  // the run [runK, k): one gather, then its segments into the cache
            u64 t0 = vf_clock();
            tLook += t0 - tl;
            gather_range(job, false, runAt, at);
            u64 t1 = vf_clock();
            tGather += t1 - t0;
            for (u32 j = runK, a2 = runAt; j < k; j++) {
                u32 c = seg_count(job, j, a2);
                if (job->srcPa[j] != VC_NO_PA) vc_put(vc, job, job->srcPa[j], a2, c, key, job->made);  // as old as the stream
                a2 += c;
            }
            tl = vf_clock();
            tPut += tl - t1;
            inRun = false;
        }
        if (end) break;
        at += cnt;
    }
    tLook += vf_clock() - tl;
    g_vcPhase[0] += tLook; g_vcPhase[1] += tGather; g_vcPhase[2] += tPut;
    if (at < job->count) gather_range(job, false, at, job->count);
}

VJob* vfast_prepare(System* s, const GXCore* gx, u32 vat, const VFold* f, const VSel* sel, const u8* raw, u32 count,
                    u32 vsize, bool gpuLight, bool defer, void* mem, PVert* out) {
    VJob* job = mem;
    u8* m = (u8*)mem + align8(sizeof(VJob));
    job->in = (VIn*)m;
    m += align8(count * (u32)sizeof(VIn));
    u8* rawCopy = m;  // the stream, for a gather left to vfast_finish
    m += align8(count * vsize);
    job->pos = (float (*)[21])m;
    m += align8(pos_cap(gx, count) * 21 * 4);
    job->tex = (float (*)[12])m;
    job->count = count;
    job->gpuLight = gpuLight;
    job->gpuPos = false;
    job->vc = g_pendVc;  // the stream's place in RAM (vfast_set_sources), for the display-list vertex cache
    job->srcN = g_pendVc ? g_pendN : 0;
    if (g_pendVc && g_pendN == 1) { job->onePa = g_pendPa[0]; job->oneSeg = count; job->srcPa = &job->onePa; job->srcSeg = &job->oneSeg; }
    else { job->srcPa = g_pendPa; job->srcSeg = g_pendSeg; }
    g_pendVc = NULL; g_pendN = 0;
    job->made = g_memStamps.now;
    __sync_synchronize();  // the stamp before the reads it covers
    VIn* g_in = job->in;
    // The draw's setup (decode layout, outputs needed, texture units, lighting halves, projection) depends only on
    // registers, so it is kept until one of them changes value (GXCore versions) or the draw's format, the TEV's
    // selection or the lighting mode differ.
    VSetup* S = &g_vs[gpuLight ? 1 : 0];
    u32 key[6] = { gx->cpVersion, gx->xfRegVersion, gpuLight ? 0 : gx->xfLightVersion,  // lights: CPU lighting only
                   vat | (gpuLight ? 0x100u : 0) | ((u32)sel->rasChan << 16) | ((u32)sel->numTex << 24),
                   (u32)sel->texCoord[0] | ((u32)sel->texCoord[1] << 8) | ((u32)sel->texCoord[2] << 16), g_vsGen };
    if (!S->valid || memcmp(key, S->key, sizeof key)) {
        for (int i = 0; i < 6; i++) if (!S->valid || key[i] != S->key[i]) g_vfastStats.setupWhy[i]++;
        setup_compute(S, gx, vat, f, sel, gpuLight);
        memcpy(S->key, key, sizeof key);
        S->valid = true;
        g_vfastStats.setups++;
    }
    memcpy(job->P, S->P, sizeof job->P);
    memcpy(job->unit, S->unit, sizeof job->unit);
    u32 numChans = S->numChans, chans = S->chans, genUsed = S->genUsed, texUsed = S->texUsed;
    bool nrmUsed = S->nrmUsed, eyeUsed = S->eyeUsed, nbt = S->nbt, index3 = S->index3, pnDirect = S->pnDirect;
    const VHalf (*half)[2] = S->half;
    // The current matrix indices change with nearly every draw: read here, not cached.
    u32 ma = gx->cp[CP_MATINDEX_A], mb = gx->cp[CP_MATINDEX_B];
    u8 tmDefault[8];
    for (int i = 0; i < 8; i++) tmDefault[i] = (u8)(i < 4 ? (ma >> (6 + 6 * i)) & 63 : (mb >> (6 * (i - 4))) & 63);
    u32 tmDirect = S->tmDirect, pnDefault = ma & 63;
    VAttr pos = S->pos, nrm = S->nrm, col[2] = { S->col[0], S->col[1] }, tex[8];
    memcpy(tex, S->tex, sizeof tex);
    // Array bases and strides change with nearly every draw: read here, not cached.
    attr_array(gx, &pos, ARR_POS); attr_array(gx, &nrm, ARR_NRM);
    attr_array(gx, &col[0], ARR_COL0); attr_array(gx, &col[1], ARR_COL0 + 1);
    for (u32 i = 0; i < 8; i++) attr_array(gx, &tex[i], ARR_TEX0 + i);
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


    job->mem1 = s->mem1;
    job->raw = raw;
    job->vsize = vsize;
    job->pos_ = pos; job->nrm_ = nrm; job->col_[0] = col[0]; job->col_[1] = col[1];
    memcpy(job->tex_, tex, sizeof tex);
    job->nrmUsed = nrmUsed; job->nbt = nbt; job->index3 = index3; job->pnDirect = pnDirect;
    job->texUsed = texUsed; job->tmDirect = tmDirect; job->rasChan = sel->rasChan;
    job->pnDefault = (u8)pnDefault;
    memcpy(job->tmDefault, tmDefault, 8);
    // Shader-lit draws may leave the gather to vfast_finish (another core): only the matrix indices are read here,
    // and the stream is copied into the job (it does not outlive the draw).
    job->gathered = !(defer && gpuLight);
    u64 t0;
    if (!job->gathered) {
        gather_indices(job);
        memcpy(rawCopy, raw, count * vsize);
        job->raw = rawCopy;
        goto matrices;
    }
    t0 = dprof_begin();  // all reads of guest memory (timed as "dec" on the 3DS profile)
    gather_cached(job, true);
    dprof_end(PROF_VDEC, t0);

    // Colour channels the outputs need (texgen from colour reads channel 0, kept in c0).
    t0 = dprof_begin();
    if (gpuLight) {
        gpu_colours(job, out);
    } else {
        if (count > g_litCap) {
            VLit* n = realloc(g_lit, count * sizeof *n);
            if (!n) return NULL;
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
    }
    dprof_end(PROF_VLIGHT, t0);

matrices:
    // The matrices the vertices use, copied into the job: position (+ normal) matrices by pn, texture matrices by
    // the used units' tm. Vertices then index the copies.
    u8 posLocal[64], texLocal[64];
    memset(posLocal, 0xFF, sizeof posLocal);
    memset(texLocal, 0xFF, sizeof texLocal);
    job->nPos = job->nTex = 0;
    u32 usedGi = 0;
    for (u32 u = 0; u < 3; u++) if (job->unit[u].mode == VU_GEN) usedGi |= 1u << job->unit[u].gi;
    for (u32 vi = 0; vi < count; vi++) {
        VIn* in = &g_in[vi];
        u32 pn = in->pn & 63;
        if (posLocal[pn] == 0xFF) {
            float* d = job->pos[job->nPos];
            memcpy(d, &gx->xf[pn * 4], 12 * 4);
            memcpy(d + 12, &gx->xf[0x400 + (pn & 31) * 3], 9 * 4);  // its normal matrix
            posLocal[pn] = (u8)job->nPos++;
            g_vfastStats.composes++;
        }
        in->pn = posLocal[pn];
        for (u32 gi = 0; gi < 8; gi++) {
            if (!((usedGi >> gi) & 1)) continue;
            u32 tm = in->tm[gi] & 63;
            if (texLocal[tm] == 0xFF) {
                memcpy(job->tex[job->nTex], &gx->xf[tm * 4], 12 * 4);
                texLocal[tm] = (u8)job->nTex++;
            }
            in->tm[gi] = texLocal[tm];
        }
    }
    // The texture matrices right after the position matrices used: the job ends there (vfast_job_used).
    float (*packed)[12] = (float (*)[12])(void*)((u8*)job->pos + align8(job->nPos * 21 * 4));
    if (packed != job->tex && job->nTex) memmove(packed, job->tex, job->nTex * 12 * 4);
    job->tex = packed;
    job->end = (u8*)(job->tex + job->nTex);
    job->originDone = false;
    return job;
}

static void job_origin(VJob* job);

#define TG_LOOP(KIND, STQ) do { \
    _Pragma("GCC unroll 4") \
    for (u32 vi = 0; vi < count; vi++) texgen_one(job->tex[in[vi].tm[gi]], &in[vi], KIND, off, STQ, out[vi].tc[u]); \
} while (0)

// Per-phase time of vfast_finish on calls the caller marks (g_vfPhaseTimed: the 3DS vertex worker, one job in 16):
// [0] the deferred gather and colours, [1] positions and normals, [2] texture coordinates.

void vfast_finish(VJob* job, PVert* out) {
    u64 tp0 = vf_clock(), tp1, tp2;
    if (!job->gathered) {
        gather_cached(job, false);
        gpu_colours(job, out);
        job->gathered = true;
    }
    tp1 = vf_clock();
    u32 count = job->count;
    const VIn* in = job->in;
    const float* P = job->P;
    if (job->gpuPos && !job->originDone) job_origin(job);
    if (job->gpuPos) {  // the shader transforms: position from the origin and palette row; normals for shader lighting
        const float* slot3 = job->slot3;
        float ox = job->origin[0], oy = job->origin[1], oz = job->origin[2];
        for (u32 vi = 0; vi < count; vi++) {
            float* o = out[vi].pos;
            o[0] = in[vi].p[0] - ox; o[1] = in[vi].p[1] - oy; o[2] = in[vi].p[2] - oz; o[3] = slot3[in[vi].pn];
        }
        if (job->gpuLight) {
#pragma GCC unroll 4
            for (u32 vi = 0; vi < count; vi++) {
                const float* N = job->pos[in[vi].pn] + 12;
                float nx = in[vi].n[0], ny = in[vi].n[1], nz = in[vi].n[2];
                PVert* o = &out[vi];
                o->nrm[0] = N[0] * nx + N[1] * ny + N[2] * nz;
                o->nrm[1] = N[3] * nx + N[4] * ny + N[5] * nz;
                o->nrm[2] = N[6] * nx + N[7] * ny + N[8] * nz;
            }
        }
        goto texcoords;
    }
    // Position: branch-free, so the compiler unrolls and interleaves the loop (see the lighting notes).
#pragma GCC unroll 4
    for (u32 vi = 0; vi < count; vi++) {
        const float* M = job->pos[in[vi].pn];
        float x = in[vi].p[0], y = in[vi].p[1], z = in[vi].p[2];
        float e0 = M[0] * x + M[1] * y + M[2] * z + M[3];
        float e1 = M[4] * x + M[5] * y + M[6] * z + M[7];
        float e2 = M[8] * x + M[9] * y + M[10] * z + M[11];
        float* o = out[vi].pos;
        for (int r = 0; r < 4; r++) o[r] = P[4 * r] * e0 + P[4 * r + 1] * e1 + P[4 * r + 2] * e2 + P[4 * r + 3];
    }
    if (job->gpuLight) {
#pragma GCC unroll 4
        for (u32 vi = 0; vi < count; vi++) {
            const float* M = job->pos[in[vi].pn];
            const float* N = M + 12;
            float x = in[vi].p[0], y = in[vi].p[1], z = in[vi].p[2], nx = in[vi].n[0], ny = in[vi].n[1], nz = in[vi].n[2];
            PVert* o = &out[vi];
            o->eye[0] = M[0] * x + M[1] * y + M[2] * z + M[3];
            o->eye[1] = M[4] * x + M[5] * y + M[6] * z + M[7];
            o->eye[2] = M[8] * x + M[9] * y + M[10] * z + M[11];
            o->nrm[0] = N[0] * nx + N[1] * ny + N[2] * nz;
            o->nrm[1] = N[3] * nx + N[4] * ny + N[5] * nz;
            o->nrm[2] = N[6] * nx + N[7] * ny + N[8] * nz;
        }
    }
texcoords:
    tp2 = vf_clock();
    // Texture coordinates, one unit at a time with its texgen settled outside the vertex loop.
    for (u32 u = 0; u < 3; u++) {
        const VUnit* vu = &job->unit[u];
        if (vu->mode == VU_ZERO) {
            for (u32 vi = 0; vi < count; vi++) out[vi].tc[u][0] = out[vi].tc[u][1] = 0;
            continue;
        }
        if (vu->mode == VU_COLOUR) {
            for (u32 vi = 0; vi < count; vi++) { out[vi].tc[u][0] = in[vi].c0[0] / 255.0f; out[vi].tc[u][1] = in[vi].c0[1] / 255.0f; }
            continue;
        }
        u32 gi = vu->gi, off = vu->off;
        if (vu->stq) {
            switch (vu->kind) {
            case TG_XYZ: TG_LOOP(TG_XYZ, true); break;
            case TG_XY: TG_LOOP(TG_XY, true); break;
            default: TG_LOOP(TG_NONE, true); break;
            }
        } else {
            switch (vu->kind) {
            case TG_XYZ: TG_LOOP(TG_XYZ, false); break;
            case TG_XY: TG_LOOP(TG_XY, false); break;
            default: TG_LOOP(TG_NONE, false); break;
            }
        }
    }
    if (g_vfPhaseTimed && tp0) {
        u64 tp3 = vf_clock();
        g_vfPhase[0] += tp1 - tp0; g_vfPhase[1] += tp2 - tp1; g_vfPhase[2] += tp3 - tp2;
    }
}

u32 vfast_job_matrices(const VJob* job, const float (**pos)[21]) { *pos = job->pos; return job->nPos; }
const float* vfast_job_proj(const VJob* job) { return job->P; }

// GPU transform: positions go relative to an origin near the first vertex (vfast_job_origin), so the shader's 24-bit
// floats hold offsets that are small against the vertex's distance from the eye, not world coordinates (Twilight
// Princess's far geometry was off by up to a texel without it). The origin is the first vertex rounded to a
// power-of-two grid of cells 4-8 times its eye distance (in model space: over the matrix's scale), so that
// neighbouring draws with the same matrix share a palette entry, and geometry in small local coordinates keeps
// origin 0. TP to field 1000 (vfast_check.sh): draws using only the previous draw's palette entries 86% without an
// origin, 81% with these cells, 70% with cells of 1-2 eye distances, 40% at 1/16; largest on-screen error 1.04,
// 0.114, 0.035 and 0.028 EFB texels. A job not gathered yet reads only its first vertex's position (one guest memory
// read). Computed on first use: CPU-transformed draws never need it.
static void job_origin(VJob* job) {
    job->originDone = true;
    memset(job->origin, 0, sizeof job->origin);
    if (!job->count) return;
    float p[3] = { 0, 0, 0 };
    if (job->gathered) {
        memcpy(p, job->in[0].p, sizeof p);
    } else if (job->pos_.mode) {
        const u8* q = job->raw + (job->pnDirect ? 1u : 0u) + (u32)__builtin_popcount(job->tmDirect);
        const u8* src = fetch(job->mem1, &job->pos_, &q, job->pos_.n * job->pos_.cs);
        for (u32 k = 0; k < job->pos_.n; k++) p[k] = read_comp(src + k * job->pos_.cs, job->pos_.fmt, job->pos_.scale);
    }
    const float* M = job->pos[job->in[0].pn];
    float e = 0, sc = 0;  // eye distance (largest component), matrix scale (largest row sum)
    for (int r = 0; r < 3; r++) {
        float c = fabsf(M[4 * r] * p[0] + M[4 * r + 1] * p[1] + M[4 * r + 2] * p[2] + M[4 * r + 3]);
        float rs = fabsf(M[4 * r]) + fabsf(M[4 * r + 1]) + fabsf(M[4 * r + 2]);
        e = c > e ? c : e;
        sc = rs > sc ? rs : sc;
    }
    if (sc > 0) {
        int ex;
        frexpf(e / sc, &ex);
        float step = ldexpf(1.0f, ex + 2), inv = 1.0f / step;
        for (int k = 0; k < 3; k++) job->origin[k] = step * floorf(p[k] * inv + 0.5f);
    }
}

const float* vfast_job_origin(VJob* job, float m[][12]) {
    if (!job->originDone) job_origin(job);
    const float* o = job->origin;
    for (u32 i = 0; m && i < job->nPos; i++) {
        const float* M = job->pos[i];
        memcpy(m[i], M, 12 * sizeof(float));
        for (int r = 0; r < 3; r++) m[i][4 * r + 3] = M[4 * r + 3] + (M[4 * r] * o[0] + M[4 * r + 1] * o[1] + M[4 * r + 2] * o[2]);
    }
    return o;
}

void vfast_job_gpu_pos(VJob* job, const u8* slots) {
    job->gpuPos = slots != NULL;
    for (u32 i = 0; slots && i < job->nPos; i++) job->slot3[i] = 3.0f * slots[i];
}

float vfast_f24(float x) {
    u32 b;
    memcpy(&b, &x, 4);
    b = (b + 0x40u) & ~0x7Fu;
    memcpy(&x, &b, 4);
    return x;
}

// dp4 as the shader computes it: f24 rounds the inputs, each product and each partial sum.
static float dp4_model(const float* a, const float* b, bool f24) {
    if (!f24) return a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
    float s = vfast_f24(vfast_f24(a[0]) * vfast_f24(b[0]));
    for (int k = 1; k < 4; k++) s = vfast_f24(s + vfast_f24(vfast_f24(a[k]) * vfast_f24(b[k])));
    return s;
}

void vfast_gpu_pos_model(const float (*pal)[12], const float P[16], const PVert* v, bool f24, float clip[4], float eye[3]) {
    const float* M = pal[(int)v->pos[3] / 3];
    float p[4] = { v->pos[0], v->pos[1], v->pos[2], 1.0f }, e[4];
    for (int r = 0; r < 3; r++) e[r] = dp4_model(M + 4 * r, p, f24);
    e[3] = 1.0f;
    for (int r = 0; r < 4; r++) clip[r] = dp4_model(P + 4 * r, e, f24);
    for (int r = 0; r < 3; r++) eye[r] = e[r];
}

// Both halves at once (the PC checker and the backend's inline path).
static u8* g_jobMem;
static u32 g_jobCap;
void vfast_run(System* s, const GXCore* gx, u32 vat, const VFold* f, const VSel* sel, const u8* raw, u32 count,
               u32 vsize, PVert* out, bool gpuLight) {
    u32 bytes = vfast_job_bytes(gx, count, vsize);
    if (bytes > g_jobCap) {
        u8* n = realloc(g_jobMem, bytes);
        if (!n) return;
        g_jobMem = n;
        g_jobCap = bytes;
    }
    // Deferred like the 3DS vertex worker (finish right after), so the PC checks that path.
    VJob* job = vfast_prepare(s, gx, vat, f, sel, raw, count, vsize, gpuLight, true, g_jobMem, out);
    if (job) vfast_finish(job, out);
}

// ---- lighting in the vertex shader ----
bool vfast_gpu_light(const GXCore* gx, const VSel* sel, VLightUniforms* u) {
    u32 ch = sel->rasChan & 1, numChans = gx->xf[0x1009] & 3;
    if (ch >= numChans) return false;
    u32 ctrl[2] = { gx->xf[0x100E + ch], gx->xf[0x1010 + ch] };  // colour half, alpha half
    bool lit[2] = { (ctrl[0] >> 1) & 1, (ctrl[1] >> 1) & 1 };
    // What the shader implements (gen_gxpass.sh): the colour half lit with attenuation none or spot, and the alpha half
    // unlit or lit exactly as the colour half (same lights, diffuse function and attenuation: one factor per light
    // then lights all four components). That is every shader-lit draw of Twilight Princess's title scene and of the
    // NeHe lessons, at about two thirds of the instructions of the general form; the rest is lit on the CPU.
    if (!lit[0] || ((ctrl[0] >> 9) & 3) == 1) return false;  // colour unlit, or specular (atten on, not spot)
    if (lit[1] && ((ctrl[0] ^ ctrl[1]) & 0x7FBCu)) return false;  // light masks, diffuse function, attenuation
    // A texture coordinate generated from a lit colour needs the colour on the CPU.
    u32 numGens = gx->xf[0x103F] & 15;
    for (u32 i = 0; i < sel->numTex && i < 3; i++) {
        u32 gi = sel->texCoord[i] & 7;
        if (gi < numGens && ((gx->xf[0x1040 + gi] >> 4) & 7) >= 2) return false;
    }
    memset(u, 0, sizeof *u);
    u32 mask[2];
    for (int h = 0; h < 2; h++) {
        u32 c = ctrl[h];
        mask[h] = lit[h] ? ((c >> 2) & 15) | (((c >> 11) & 15) << 4) : 0;
        u32 diffFn = (c >> 7) & 3;
        u->sel[2 * h][diffFn == 0 ? 0 : diffFn == 1 ? 1 : 2] = 1;
        u->sel[2 * h + 1][!((c >> 9) & 1) ? 0 : ((c >> 10) & 1) ? 1 : 2] = 1;
        u->on[h] = lit[h];
        u->vsel[h] = c & 1;               // material from the vertex colour
        u->vsel[2 + h] = (c >> 6) & 1;    // ambient from the vertex colour
    }
    {
        u32 at[2];
        for (int h = 0; h < 2; h++) at[h] = !((ctrl[h] >> 9) & 1) ? 0 : ((ctrl[h] >> 10) & 1) ? 1 : 2;
        g_vfastStats.gpuLit[lit[0]][lit[1]][lit[0] ? at[0] : 0][lit[1] ? at[1] : 0]++;
    }
    float m[4], a[4];
    unpack(gx->xf[0x100C + ch], m);
    unpack(gx->xf[0x100A + ch], a);
    for (int k = 0; k < 4; k++) { u->mat[k] = m[k] / 255.0f; u->amb[k] = a[k] / 255.0f; }
    for (u32 i = 0; i < 8; i++) {
        u32 base = 0x600 + 16 * i;
        float (*L)[4] = &u->lights[5 * i];
        float c[4];
        unpack(gx->xf[base + 3], c);
        for (int k = 0; k < 3; k++) {
            L[0][k] = xf_f(gx, base + 10 + k);
            L[1][k] = xf_f(gx, base + 13 + k);
            L[3][k] = xf_f(gx, base + 4 + k);
            L[4][k] = xf_f(gx, base + 7 + k);
        }
        for (int k = 0; k < 4; k++) L[2][k] = c[k] / 255.0f;
        // As half_init: an all-zero distance polynomial divides by 1.
        if (L[4][0] == 0 && L[4][1] == 0 && L[4][2] == 0) L[4][0] = 1;
        L[3][3] = (float)((mask[0] >> i) & 1);
        L[4][3] = (float)((mask[1] >> i) & 1);
    }
    return true;
}

static float dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

// The shader clamps every reciprocal to +-1e18, so that a term its selector discards stays finite (the PICA
// flushes tiny values to 0 and 1/0 would be infinite, and 0 * infinity is not 0); NeHe lesson 8's unused
// attenuation coefficients are such tiny values.
#define VBIG 1.0e18f
static float rcp_c(float x) { float r = 1.0f / x; return r > VBIG ? VBIG : r < -VBIG ? -VBIG : r; }
static float rsq_c(float x) { float r = 1.0f / sqrtf(x); return r > VBIG ? VBIG : r; }

void vfast_gpu_light_model(const VLightUniforms* u, const PVert* v, float out[4]) {
    float vc[4] = { v->col[0] / 255.0f, v->col[1] / 255.0f, v->col[2] / 255.0f, v->col[3] / 255.0f };
    float n[3] = { v->nrm[0], v->nrm[1], v->nrm[2] }, nr = rsq_c(dot3(n, n));
    n[0] *= nr; n[1] *= nr; n[2] *= nr;
    float acc[4] = { 0, 0, 0, 0 };
    for (int i = 0; i < 8; i++) {
        const float (*L)[4] = &u->lights[5 * i];
        float l[3] = { L[0][0] - v->eye[0], L[0][1] - v->eye[1], L[0][2] - v->eye[2] };
        float d2 = dot3(l, l), inv = rsq_c(d2), dist = d2 * inv;
        l[0] *= inv; l[1] *= inv; l[2] *= inv;
        float dot = dot3(n, l), dotc = dot < 0 ? 0 : dot;
        float cs = -dot3(L[1], l), hh = dot3(L[1], n);
        hh = hh < 0 ? 0 : hh;
        float aa = L[3][0] + L[3][1] * cs + L[3][2] * cs * cs, dd = L[4][0] + L[4][1] * dist + L[4][2] * d2;
        float aa2 = L[3][0] + L[3][1] * hh + L[3][2] * hh * hh, dd2 = L[4][0] + L[4][1] * hh + L[4][2] * hh * hh;
        float attSpot = (aa < 0 ? 0 : aa) * rcp_c(dd), attSpec = (aa2 < 0 ? 0 : aa2) * rcp_c(dd2);
        float dv[3] = { 1, dot, dotc }, av[3] = { 1, attSpot, attSpec };
        float wc = dot3(u->sel[0], dv) * dot3(u->sel[1], av) * L[3][3];
        float wa = dot3(u->sel[2], dv) * dot3(u->sel[3], av) * L[4][3];
        for (int k = 0; k < 3; k++) acc[k] += L[2][k] * wc;
        acc[3] += L[2][3] * wa;
    }
    for (int k = 0; k < 4; k++) {
        int h = k < 3 ? 0 : 1;
        float mat = u->mat[k] + u->vsel[h] * (vc[k] - u->mat[k]);
        float amb = u->amb[k] + u->vsel[2 + h] * (vc[k] - u->amb[k]);
        float lit = amb + acc[k];
        lit = lit < 0 ? 0 : lit > 1 ? 1 : lit;
        out[k] = 255.0f * mat * (1.0f + u->on[h] * (lit - 1.0f));
    }
}
