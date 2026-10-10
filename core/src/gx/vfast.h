#pragma once
// GX vertices straight to PICA200 vertex-shader inputs (vfast.c). Portable C: the 3DS backend uses it and the PC
// checks it against the reference transform (gx/xform.c) with VFAST_CHECK.
#include "xform.h"

typedef struct { float pos[4]; u8 col[4]; float tc[3][2]; } PVert;  // gxpass.v.pica inputs v0-v4

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

// Fast path: decodes, transforms and lights `count` raw vertices (stride vsize) into `out`. Equal to vfast_ref of
// the reference transform up to float rounding (matrices are composed per draw), for the coordinates the draw
// delivers; texture coordinates past the texgen count are 0 (the reference leaves them undefined).
void vfast_run(System* s, const GXCore* gx, u32 vat, const VFold* f, const VSel* sel, const u8* raw, u32 count,
               u32 vsize, PVert* out);
