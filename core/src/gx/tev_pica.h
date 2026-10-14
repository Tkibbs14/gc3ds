#pragma once
// GX TEV -> PICA200 texture combiner translation (portable C, built and tested on the PC; applied with citro3d on
// the 3DS). GX has up to 16 stages, 4 colour registers (PREV, C0, C1, C2), konst colours, 8 texture maps and two
// rasterised colours; the PICA has 6 stages, PREVIOUS, one combiner buffer, one constant per stage, 3 texture units
// and one vertex colour. tev_pica_compile maps what fits exactly and approximates the rest (flagged).
#include "gx.h"

// Numeric values equal libctru's GPU_TEVSRC / GPU_TEVOP_RGB / GPU_TEVOP_A / GPU_COMBINEFUNC / GPU_TEVSCALE, so the
// 3DS backend passes them straight to C3D_TexEnv*.
enum { PSRC_PRIMARY = 0x00, PSRC_TEX0 = 0x03, PSRC_TEX1 = 0x04, PSRC_TEX2 = 0x05, PSRC_BUFFER = 0x0D,
       PSRC_CONSTANT = 0x0E, PSRC_PREVIOUS = 0x0F };
enum { POPC_COLOR = 0x0, POPC_1_COLOR = 0x1, POPC_ALPHA = 0x2, POPC_1_ALPHA = 0x3, POPC_R = 0x4, POPC_1_R = 0x5,
       POPC_G = 0x8, POPC_1_G = 0x9, POPC_B = 0xC, POPC_1_B = 0xD };
enum { POPA_ALPHA = 0x0, POPA_1_ALPHA = 0x1, POPA_R = 0x2, POPA_1_R = 0x3, POPA_G = 0x4, POPA_1_G = 0x5,
       POPA_B = 0x6, POPA_1_B = 0x7 };
enum { PFN_REPLACE = 0, PFN_MODULATE = 1, PFN_ADD = 2, PFN_ADD_SIGNED = 3, PFN_INTERPOLATE = 4, PFN_SUBTRACT = 5,
       PFN_DOT3_RGB = 6, PFN_DOT3_RGBA = 7, PFN_MULTIPLY_ADD = 8, PFN_ADD_MULTIPLY = 9 };
enum { PSCALE_1 = 0, PSCALE_2 = 1, PSCALE_4 = 2 };

#define PICA_STAGES 6
#define PICA_TEXUNITS 3

typedef struct {
    u8 srcRgb[3], srcA[3];  // PSRC_*
    u8 opRgb[3], opA[3];    // POPC_* / POPA_*
    u8 fnRgb, fnA;          // PFN_*
    u8 scaleRgb, scaleA;    // PSCALE_*
    u32 konst;              // this stage's constant colour, 0xAABBGGRR (C3D_TexEnvColor layout)
} PicaStage;

typedef struct {
    u8 numStages;           // 1..PICA_STAGES; stages past it are left as pass-through by the backend
    PicaStage st[PICA_STAGES];
    u8 bufRgb, bufA;        // combiner-buffer update masks (bit i: stage i's output enters the buffer), stages 0-3
    u32 bufColor;           // initial combiner buffer, 0xAABBGGRR
    u8 numTex;              // texture units used
    u8 texMap[PICA_TEXUNITS], texCoord[PICA_TEXUNITS];  // unit i samples GX texmap texMap[i] at coordinate texCoord[i]
    u8 rasChan;             // GX colour channel delivered as PSRC_PRIMARY (0 or 1)
    u32 approx;             // bit set per approximation made (TEVA_*); 0 = exact up to 8-bit rounding
} PicaTev;

enum { TEVA_STAGES = 1u << 0,    // more PICA stages needed than exist; trailing work dropped
       TEVA_TEXUNITS = 1u << 1,  // more than 3 distinct (texmap, texcoord) pairs
       TEVA_RASCHAN = 1u << 2,   // both colour channels used; only one delivered
       TEVA_SWAP = 1u << 3,      // a swap table that no operand can express
       TEVA_RANGE = 1u << 4,     // signed or > 1 intermediate values clamped (GX keeps 11-bit signed registers)
       TEVA_COMPARE = 1u << 5,   // compare-mode stage
       TEVA_REGS = 1u << 6,      // more live colour registers than PREVIOUS + buffer + constants can hold
       TEVA_BIAS = 1u << 7 };    // bias / subtract / scale combination approximated

// Compiles the current TEV state (BP 0x00 stage count, 0x28-0x2F TREF, 0xC0-0xDF stages, 0xE0-0xE7 registers and
// konst, 0xF6-0xFD konst selects and swap tables) into PICA combiner state.
void tev_pica_compile(const GXCore* gx, PicaTev* out);
// Everything tev_pica_compile reads (66 words), for caching its result: the TEV registers and the texture formats.
#define TEV_PICA_KEY_MAX 72
u32 tev_pica_key(const GXCore* gx, u32* k);
