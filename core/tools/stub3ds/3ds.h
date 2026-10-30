// Minimal stub of <3ds.h> for a syntax-only compile of host3ds files on the PC.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;
typedef struct Thread_tag* Thread;
static inline Thread threadCreate(void (*f)(void*), void* a, size_t sz, int prio, int core, bool detach) {
    (void)f; (void)a; (void)sz; (void)prio; (void)core; (void)detach; return 0;
}
static inline int svcGetThreadPriority(int32_t* out, uint32_t h) { (void)h; if (out) *out = 0x30; return 0; }
static inline void svcSleepThread(int64_t ns) { (void)ns; }
static inline uint64_t svcGetSystemTick(void) { return 0; }
#define CUR_THREAD_HANDLE 0xFFFF8000

// Add what a file you want to check needs, and nothing more: this is a syntax checker, not an SDK.
