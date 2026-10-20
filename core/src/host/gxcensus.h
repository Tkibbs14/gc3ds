#pragma once
#include "../gx/gx.h"
#ifdef GX_CENSUS_PC
extern bool g_gxcensus;
#define GXCENSUS(call) do { if (__builtin_expect(g_gxcensus, 0)) { call; } } while (0)
void gxcensus_start(System* s, const char* path);
void gxcensus_finish(void);
void gxcensus_field(u32 field);
void gxcensus_reg(unsigned bank, u32 reg, u32 old, u32 value);
void gxcensus_list(const GXCore* gx, const u8* p, u32 size);
void gxcensus_list_end(void);
void gxcensus_draw(System* s, const GXCore* gx, u32 vat, const u8* p, u32 count, u32 stride);
void gxcensus_mem(u32 pc, u32 pa, u32 size, bool write);
#else
#define GXCENSUS(call) ((void)0)
#endif
