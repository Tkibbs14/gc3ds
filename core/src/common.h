#pragma once
// Shared types and helpers for the emulator core. Portable C11: builds for the PC (development, reference) and for
// the 3DS (devkitARM).
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#ifndef GC3DS_HOST_TYPES  // a 3DS frontend file that also includes <3ds.h> uses libctru's (ABI-identical) types
typedef uint8_t u8;
typedef uint16_t u16;
// Plain int, not uint32_t: newlib (3DS) makes uint32_t `unsigned long`, glibc `unsigned int`. With int on both,
// printf's %x / %d / %u are correct for u32/s32 everywhere (do not use PRIx32 with these types).
typedef unsigned int u32;
typedef uint64_t u64;
typedef int8_t s8;
typedef int16_t s16;
typedef int s32;
typedef int64_t s64;
#endif
_Static_assert(sizeof(u32) == 4 && sizeof(s32) == 4, "32-bit int required");

static inline u16 bswap16(u16 v) { return __builtin_bswap16(v); }
static inline u32 bswap32(u32 v) { return __builtin_bswap32(v); }
static inline u64 bswap64(u64 v) { return __builtin_bswap64(v); }

// Logging: categories can be filtered at runtime. LOG_ERROR always prints.
typedef enum { LOG_CPU, LOG_MEM, LOG_HW, LOG_BOOT, LOG_GX, LOG_DSP, LOG_EXI, LOG_SI, LOG_DI, LOG_VI, LOG_OS, LOG_NCAT } LogCat;
void emu_log(LogCat cat, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
void emu_error(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
extern u32 g_logMask;  // bit per LogCat
