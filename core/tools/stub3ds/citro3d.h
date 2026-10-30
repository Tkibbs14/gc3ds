// Minimal stub of <citro3d.h> for a syntax-only compile of host3ds files on the PC. See 3ds.h here.
#pragma once
#include <3ds.h>
static inline float C3D_GetDrawingTime(void) { return 0.0f; }
static inline float C3D_GetProcessingTime(void) { return 0.0f; }
enum { GPU_RGBA8 = 0, GPU_RGB8, GPU_RGBA5551, GPU_RGB565, GPU_RGBA4, GPU_LA8, GPU_HILO8, GPU_L8, GPU_A8, GPU_LA4,
       GPU_L4, GPU_A4, GPU_ETC1, GPU_ETC1A4 };
enum { GPU_RGBA = 0, GPU_RGB, GPU_LA, GPU_L, GPU_A };
