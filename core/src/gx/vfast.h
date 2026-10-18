#pragma once
// GX vertices straight to PICA200 vertex-shader inputs (vfast.c). Portable C: the 3DS backend uses it and the PC
// checks it against the reference transform (gx/xform.c) with VFAST_CHECK.
#include "xform.h"

// gxpass.v.pica inputs v0-v6: clip position, colour, three texture coordinates, and for draws lit by the shader the
// eye-space position and (unnormalised) normal.
typedef struct { float pos[4]; u8 col[4]; float tc[3][2]; float eye[3], nrm[3]; } PVert;

// What a draw delivers: the PICA's one vertex colour (GX channel rasChan) and, per texture unit, a GX texture
// coordinate (texCoord[i] & 7) divided by its q.
typedef struct { u8 rasChan, numTex, texCoord[3]; } VSel;

// The GX viewport folded into PICA clip space: EFB pixel (x, y) lands at NDC (x * kx - 1, y * ky - 1), with y
// mirrored when flipY; depth is -(GX screen z) / 2^24.
typedef struct { float kx, ky, wd, ht, xo, yo, zr, fz; bool flipY; } VFold;
void vfold_init(const GXCore* gx, float kx, float ky, bool flipY, VFold* f);

// Reference: a vertex from gx_transform + gx_to_screen to PICA inputs.
void vfast_ref(const VFold* f, const VSel* sel, const SVert* v, PVert* o);
// Colour and texture coordinates only (vertices placed some other way, such as lines and points).
void vfast_attribs(const VSel* sel, const SVert* v, PVert* o);

// Work counters since start (cheap; printed by the PC checker): what the vertex cost is made of.
typedef struct {
    u64 draws, verts, composes, lightEvals, litHalves, indexed, texgens, nrmReads, setups, setupWhy[6];
    u64 gpuLit[2][2][3][3];  // shader-lit draws by colour half lit, alpha half lit, their attenuation (none, spot, specular)
    // Sampled (one draw in 32): draws with indexed attributes, their vertices, vertices whose indexed attributes all
    // carry one index, draws where every vertex does, vertices of draws with a single indexed attribute.
    u64 sharedSampled, idxDraws, idxVerts, sharedVerts, sharedDraws, oneIdxVerts;
} VFastStats;
extern VFastStats g_vfastStats;

// Fast path: decodes, transforms and lights `count` raw vertices (stride vsize) into `out`. Equal to vfast_ref of
// the reference transform up to float rounding (matrices are composed per draw), for the coordinates the draw
// delivers; texture coordinates past the texgen count are 0 (the reference leaves them undefined).
// gpuLight: the shader lights the colour (vfast_gpu_light returned true): `col` gets the channel's vertex colour and
// eye / nrm are filled instead of lighting on the CPU.
void vfast_run(System* s, const GXCore* gx, u32 vat, const VFold* f, const VSel* sel, const u8* raw, u32 count,
               u32 vsize, PVert* out, bool gpuLight);
// The same in two halves, so the second can run on another core: vfast_prepare reads everything that belongs to the
// moment of the draw (stream, arrays, CP and XF state), gathers and lights the vertices into `out` (colours) and into
// job memory `mem` of vfast_job_bytes, with copies of the matrices the vertices use; vfast_finish then fills in clip
// positions, the shader-lighting inputs and texture coordinates from the job alone. prepare returns NULL on failure.
typedef struct VJob VJob;
// defer (shader-lit draws only): leave the gather of attributes from guest memory to vfast_finish as well; the caller
// must then run vfast_finish before the guest can change the vertex arrays (before PE tokens / finish are signalled).
u32 vfast_job_bytes(const GXCore* gx, u32 count, u32 vsize);
// The display-list vertex cache (vfast.c, docs/DL_CACHE_PLAN.md): one per thread that gathers. Before vfast_prepare,
// vfast_set_sources names the cache and where the draw's vertex stream lies in guest RAM: n segments, pa[k] each
// (VC_NO_PA: not in RAM, not cached) and seg[k] = prim << 16 | count (seg unused for n = 1). Arrays for n > 1 must stay
// valid until vfast_finish. vfast_job_set_cache: the cache a vfast_finish on another thread uses instead.
#define VC_NO_PA 0xFFFFFFFFu
typedef struct VCache VCache;
// stale: a page it covers was stamped since; staleRing: the ring wrote over its data; evicted: a live entry replaced
typedef struct { u64 hits, hitVerts, misses, stale, staleRing, inserts, uncacheable, evicted, pagesChecked; } VCacheStats;
VCache* vcache_create(u32 bytes, u32 entries);
void vcache_reset(VCache* vc);
const VCacheStats* vcache_stats(const VCache* vc);
void vfast_set_sources(VCache* vc, const u32* pa, const u32* seg, u32 n);
void vfast_job_set_cache(VJob* job, VCache* vc);
extern bool g_vcacheOn;     // off: every draw gathers (A/B)
extern bool g_vfDeferCpu;    // CPU-lit draws' gather and lighting left to vfast_finish too (written by the preparing thread)
extern bool g_vfDeferCheck;  // PC: vfast_run compares both ways on CPU-lit draws (g_vfDeferChecked, g_vfDeferFails)
extern u64 g_vfDeferChecked, g_vfDeferFails, g_vfDeferLitHalves, g_vfDeferJobs;
extern int g_vfDeferPlant;  // 1: lit draws' colour flipped; 2: their lights left out
extern bool g_vinCompact;   // vertices in job memory sized to the texture coordinates the draw uses (--vin, --ab vin)
extern bool g_vcacheCheck;  // PC checker: hits are gathered again and compared
extern bool g_vcachePlant;  // the checker's self-test: page stamps ignored
extern u64 g_vcacheCheckFails, g_vcacheChecked;
extern u64 g_vcPhase[8];  // of g_vfPhase[0], the display-list cache: hits (lookups, copies), missed runs gathered, inserts
extern u64 g_vfPhase[3];  // vfast_finish's time by phase on marked calls (vfast.c)
extern bool g_vfPhaseTimed;
u32 vfast_job_used(const VJob* job);  // bytes from the job's start to the end of what it uses, once prepared
VJob* vfast_prepare(System* s, const GXCore* gx, u32 vat, const VFold* f, const VSel* sel, const u8* raw, u32 count,
                    u32 vsize, bool gpuLight, bool defer, void* mem, PVert* out);
void vfast_finish(VJob* job, PVert* out);
// Position transform in the vertex shader (gxpass.v.pica with lon.w = 1): the shader computes clip = P * (M * p) from
// a palette of position matrices in uniforms (pal[3 * slot + row]) and the projection P. Between prepare and finish
// the caller gives every matrix of the job a palette slot (vfast_job_gpu_pos); vfast_finish then writes positions
// relative to the job's origin with w = 3 * slot, and for shader-lit draws only the normal (the shader's eye position
// is M * p).
u32 vfast_job_matrices(const VJob* job, const float (**pos)[21]);  // position matrix (3x4) + normal matrix (3x3)
const float* vfast_job_proj(const VJob* job);                       // P: projection x viewport x fold, rows of 4
// The job's origin (its first vertex's position on a grid, deferred jobs included), which vfast_finish subtracts
// from every position, and in m[i] (unless m is NULL) each position matrix with the origin folded into its
// translation: the palette's contents. Returns the origin.
const float* vfast_job_origin(VJob* job, float m[][12]);
void vfast_job_gpu_pos(VJob* job, const u8* slots);                // slots[i]: palette slot of matrix i (< 42)
// The shader's position computation in C: clip and eye position of a vertex vfast_finish wrote with slots. f24:
// inputs and every product and sum rounded to the PICA's 24-bit floats (vfast_f24).
void vfast_gpu_pos_model(const float (*pal)[12], const float P[16], const PVert* v, bool f24, float clip[4], float eye[3]);
float vfast_f24(float x);  // rounded to the PICA's 24-bit float (16 mantissa bits, to nearest; exponent range ignored)
// Prefetch of indexed attributes' array elements a few vertices ahead in the gather (on by default; comparison off).
void vfast_set_prefetch(bool on);
extern bool g_vfastPrefetch;
// Forgets the cached draw setup (call when GX state is replaced, e.g. a reset or a loaded state).
void vfast_reset_cache(void);

// Lighting in the vertex shader. The shader lights the colour channel the PICA receives (both its colour and alpha
// halves) from the eight GX lights, with these uniforms (gxpass.v.pica; colours 0..1):
// - lights[5i + 0..4]: position, direction, colour, (a0 a1 a2 colour-half mask), (k0 k1 k2 alpha-half mask);
// - sel[0..3]: one-hot selectors (1, signed, clamped) diffuse and (none, spot, specular) attenuation, colour half
//   then alpha half;
// - mat, amb: the material and ambient registers; vsel: (material from vertex colour: rgb, a; ambient: rgb, a);
//   on: (colour half lit, alpha half lit). Unlit halves output the material.
typedef struct { float lights[40][4], sel[4][4], mat[4], amb[4], vsel[4], on[4]; } VLightUniforms;
// Whether the draw's delivered colour channel can be lit by the shader (it is lit, and no texture coordinate is
// generated from a lit colour, which the CPU would need); fills u when it can.
bool vfast_gpu_light(const GXCore* gx, const VSel* sel, VLightUniforms* u);
// The shader's colour computation in C (the PC checks it against the reference): RGBA 0..255.
void vfast_gpu_light_model(const VLightUniforms* u, const PVert* v, float out[4]);

// Converted shapes (pica.c): a model's geometry (native mode: its GX_AURORA_DRAW_INDEXED; the emulator: a run of draws
// in a display list) decoded once into vertex buffers the PICA reads, drawn each frame with only the matrices and state
// of the moment.
void vfast_proj(const GXCore* gx, const VFold* f, float P[16]);  // projection x viewport x fold (as the GPU transform)
enum { VN_POS = 1, VN_NRM = 2, VN_COL0 = 4, VN_COL1 = 8, VN_TEX0 = 16 };
typedef struct {
    u32 present;           // VN_* bits (VN_TEX0 << i for texture coordinate i)
    bool pnDirect;         // a position matrix index in each vertex
    u32 lo[12], hi[12];    // per attribute (position, normal, colours, texture coordinates): the bytes of array
                           // elements its indices reach, [lo, hi); 0, 0 when not indexed
} VNatInfo;
// The current vertex descriptor for format `vat`; false when native shapes do not take it (texture matrix indices in
// the vertices, no position).
bool vfast_native_info(const GXCore* gx, u32 vat, VNatInfo* ni);
// Every vertex's attributes, as gather_range decodes them: pos[i] = (x, y, z, position matrix row), normals, colours as
// bytes R, G, B, A, texture coordinates (NULL outputs are skipped); fills ni's ranges.
void vfast_native_decode(const GXCore* gx, u32 vat, const u8* raw, u32 count, u32 vsize, VNatInfo* ni, float (*pos)[4],
                         float (*nrm)[3], u8 (*col[2])[4], float (*tex[8])[2]);
// The delivered colour channel for the native shader path: lit (the channel's lights packed to the front of u->lights,
// *nLights of them, the shader loops over those) or not (u->vsel picks the material from the vertex colour or the
// register). colSrc: the vertex colour that feeds it (0, 1; 2: none present, white). False for what the shader does not
// do: specular attenuation, an alpha half lit unlike the colour half, texture coordinates from a colour.
bool vfast_native_light(const GXCore* gx, const VSel* sel, u32 present, VLightUniforms* u, u32* nLights, u32* colSrc, bool* lit);
// Where array elements are read from: the host address of guest physical 0 (NULL in native mode, MEM1's copy in the
// emulator); ranges in VNatInfo are guest physical (host in native mode).
void vfast_native_mem(const u8* mem1);
