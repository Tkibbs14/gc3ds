#pragma once
// Vertex transformation shared by the GX backends (xform.c).
#include "gx.h"

typedef struct {
    float x, y, z, w;      // clip space
    float sx, sy, sz;      // screen (after divide and viewport): EFB pixels, 24-bit depth units
    float col[2][4];       // RGBA 0..255
    float tex[8][3];       // s, t, q
} SVert;

// Model space -> clip space, both lit colour channels and every generated texture coordinate.
void gx_transform(const GXCore* gx, const GXVertex* v, SVert* o);
// Perspective divide and viewport: fills sx, sy, sz.
void gx_to_screen(const GXCore* gx, SVert* v);
