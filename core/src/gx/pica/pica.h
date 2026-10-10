#pragma once
// GX -> PICA200 backend for the 3DS (citro3d). Replaces the software renderer: draws, EFB clears and EFB copies run
// on the GPU, and copies to the XFB stay on the GPU ("virtual XFB").
#include "../gx.h"

// Initialises citro3d and attaches the backend to s. Call after gfxInitDefault. Returns false if the GPU resources
// could not be allocated (the caller keeps the software renderer).
bool pica_attach(System* s);
// Shows the field the VI is scanning out (physical XFB address xfbAddr) on the top screen. Returns false when no GPU
// XFB copy has happened since the program started, so the XFB in RAM is the only image (programs that draw with the
// CPU, such as consoles); the caller then converts RAM.
bool pica_present(System* s, u32 xfbAddr);
// Forgets every GPU-side copy and cached texture; call before booting another program.
void pica_reset(void);
