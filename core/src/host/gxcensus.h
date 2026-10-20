#pragma once
#include "../gx/gx.h"
#ifdef GX_CENSUS_PC
extern bool g_gxcensus;
extern bool g_gxcensusSkip;
void gxcensus_pause(void);
void gxcensus_resume(void);
#define GXCENSUS(call) do { if (__builtin_expect(g_gxcensus && !g_gxcensusSkip, 0)) { \
    gxcensus_pause(); call; gxcensus_resume(); } } while (0)
#ifdef GX_CENSUS_TIMING_PLANT
#define GXCENSUS_WORK(...) do { __VA_ARGS__; } while (0)
#else
#define GXCENSUS_WORK(...) do { if (g_gxcensus) gxcensus_pause(); \
    __VA_ARGS__; if (g_gxcensus) gxcensus_resume(); } while (0)
#endif
int gxcensus_prepare(System* s, const GXCore* gx, const u8* p, u32 avail, bool in_list);
void gxcensus_timer_enter(int tag, bool leaf);
void gxcensus_timer_leave(u32 consumed);
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
#define GXCENSUS_WORK(...) do { __VA_ARGS__; } while (0)
#endif
