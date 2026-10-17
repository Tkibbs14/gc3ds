#pragma once
// Guest memory byte order (MEM1 and the locked cache), behind one switch.
//
// GC_SWAPRAM 0: big-endian bytes, as the GameCube holds them (a 32-bit load is ldr + rev on the ARM host).
// GC_SWAPRAM 1: each aligned 32-bit word in host order (REDESIGN_CANDIDATES C18): an aligned word load or store is a
// plain ldr / str; a halfword sits at offset ^ 2 and a byte at offset ^ 3 (the trick N64 emulators use); a double is
// two words, the high one first.
//
// Every access to guest memory from C goes through these, with a base that is 4-byte aligned in host memory (MEM1,
// the locked cache, a translation block's host pointer) and the offset of the guest address from it. Byte streams
// that are big-endian outside guest memory (the disc, DOL files, ARAM, the gather pipe, save states) move in and out
// with ram_from_be / ram_to_be, or are converted in place after a raw copy (ram_fix_be).
#include "common.h"
#include <string.h>

#ifndef GC_SWAPRAM
#define GC_SWAPRAM 0
#endif

#if GC_SWAPRAM
#define RAM_X8 3u
#define RAM_X16 2u
#else
#define RAM_X8 0u
#define RAM_X16 0u
#endif

static inline u32 ram_rd8(const u8* b, u32 o) { return b[o ^ RAM_X8]; }
static inline void ram_wr8(u8* b, u32 o, u32 v) { b[o ^ RAM_X8] = (u8)v; }

static inline u32 ram_rd16(const u8* b, u32 o) {
#if GC_SWAPRAM
    if (__builtin_expect(o & 1, 0)) return (ram_rd8(b, o) << 8) | ram_rd8(b, o + 1);
    u16 v; memcpy(&v, b + (o ^ 2), 2); return v;
#else
    u16 v; memcpy(&v, b + o, 2); return bswap16(v);
#endif
}
static inline void ram_wr16(u8* b, u32 o, u32 v) {
#if GC_SWAPRAM
    if (__builtin_expect(o & 1, 0)) { ram_wr8(b, o, v >> 8); ram_wr8(b, o + 1, v); return; }
    u16 h = (u16)v; memcpy(b + (o ^ 2), &h, 2);
#else
    u16 h = bswap16((u16)v); memcpy(b + o, &h, 2);
#endif
}
static inline u32 ram_rd32(const u8* b, u32 o) {
#if GC_SWAPRAM
    if (__builtin_expect(o & 3, 0))
        return (ram_rd8(b, o) << 24) | (ram_rd8(b, o + 1) << 16) | (ram_rd8(b, o + 2) << 8) | ram_rd8(b, o + 3);
    u32 v; memcpy(&v, b + o, 4); return v;
#else
    u32 v; memcpy(&v, b + o, 4); return bswap32(v);
#endif
}
static inline void ram_wr32(u8* b, u32 o, u32 v) {
#if GC_SWAPRAM
    if (__builtin_expect(o & 3, 0)) { ram_wr8(b, o, v >> 24); ram_wr8(b, o + 1, v >> 16); ram_wr8(b, o + 2, v >> 8); ram_wr8(b, o + 3, v); return; }
    memcpy(b + o, &v, 4);
#else
    v = bswap32(v); memcpy(b + o, &v, 4);
#endif
}
static inline u64 ram_rd64(const u8* b, u32 o) { return ((u64)ram_rd32(b, o) << 32) | ram_rd32(b, o + 4); }
static inline void ram_wr64(u8* b, u32 o, u64 v) { ram_wr32(b, o, (u32)(v >> 32)); ram_wr32(b, o + 4, (u32)v); }

// The same through a host pointer into guest memory (MEM1 and the locked cache are 4-byte aligned in host memory, so
// the word a pointer is in starts at the pointer with its low two bits cleared).
#define RAMP_B(p) ((const u8*)((uintptr_t)(p) & ~(uintptr_t)3))
#define RAMP_O(p) ((u32)((uintptr_t)(p) & 3))
static inline u32 ramp_rd8(const u8* p) { return ram_rd8(RAMP_B(p), RAMP_O(p)); }
static inline u32 ramp_rd16(const u8* p) { return ram_rd16(RAMP_B(p), RAMP_O(p)); }
static inline u32 ramp_rd32(const u8* p) { return ram_rd32(RAMP_B(p), RAMP_O(p)); }
static inline void ramp_wr8(u8* p, u32 v) { ram_wr8((u8*)RAMP_B(p), RAMP_O(p), v); }
static inline void ramp_wr16(u8* p, u32 v) { ram_wr16((u8*)RAMP_B(p), RAMP_O(p), v); }
static inline void ramp_wr32(u8* p, u32 v) { ram_wr32((u8*)RAMP_B(p), RAMP_O(p), v); }

// A big-endian byte stream into guest memory at b + o, and back out.
static inline void ram_from_be(u8* b, u32 o, const u8* src, u32 n) {
#if GC_SWAPRAM
    u32 i = 0;
    for (; i < n && ((o + i) & 3); i++) ram_wr8(b, o + i, src[i]);
    for (; i + 4 <= n; i += 4) { u32 w; memcpy(&w, src + i, 4); w = bswap32(w); memcpy(b + o + i, &w, 4); }
    for (; i < n; i++) ram_wr8(b, o + i, src[i]);
#else
    memcpy(b + o, src, n);
#endif
}
static inline void ram_to_be(u8* dst, const u8* b, u32 o, u32 n) {
#if GC_SWAPRAM
    u32 i = 0;
    for (; i < n && ((o + i) & 3); i++) dst[i] = (u8)ram_rd8(b, o + i);
    for (; i + 4 <= n; i += 4) { u32 w; memcpy(&w, b + o + i, 4); w = bswap32(w); memcpy(dst + i, &w, 4); }
    for (; i < n; i++) dst[i] = (u8)ram_rd8(b, o + i);
#else
    memcpy(dst, b + o, n);
#endif
}
// Guest memory at b + o that a raw copy has just filled with big-endian bytes (a DMA straight from the disc, say):
// brought into the layout in place. A no-op in the big-endian layout.
static inline void ram_fix_be(u8* b, u32 o, u32 n) {
#if GC_SWAPRAM
    u32 s = o & ~3u, e = (o + n + 3) & ~3u;
    if (s != o || e != o + n) {  // partial words at the ends: through a bounce buffer
        u8 tmp[4];
        for (u32 w = s; w < e; w += 4) {
            if (w >= o && w + 4 <= o + n) { u32 x; memcpy(&x, b + w, 4); x = bswap32(x); memcpy(b + w, &x, 4); continue; }
            // Guest byte k of this word: raw at w + k inside [o, o + n) (just copied in big-endian), at w + (k ^ 3)
            // outside it (already in the layout).
            memcpy(tmp, b + w, 4);
            for (u32 k = 0; k < 4; k++) b[w + (k ^ 3)] = (w + k >= o && w + k < o + n) ? tmp[k] : tmp[k ^ 3];
        }
        return;
    }
    for (u32 w = s; w < e; w += 4) { u32 x; memcpy(&x, b + w, 4); x = bswap32(x); memcpy(b + w, &x, 4); }
#else
    (void)b; (void)o; (void)n;
#endif
}
