#pragma once
// PICA200 combiner evaluation on the CPU (tev_pica_sim.c), for checking the GX -> PICA translation on the PC.
#include "tev_pica.h"

// ras: GX colour channels 0/1 (0-255); unitTex: the texel each PICA texture unit sampled (0-255). out: RGBA 0-255.
void tev_pica_eval(const PicaTev* t, const float ras[2][4], const float unitTex[3][4], float out[4]);
