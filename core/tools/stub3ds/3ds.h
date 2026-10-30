// Minimal stub of <3ds.h> for a syntax-only compile of host3ds files on the PC.
//
// Why it exists: a cloud session with no devkitARM cannot build gc3ds.3dsx, so a C mistake in core/host3ds used to
// reach the machine that does build (2026-10-30: `double tot` declared inside an `if` block and used after it broke
// the build and cost a device session). `gcc -fsyntax-only -Icore/tools/stub3ds -Icore/src -Icore/host3ds FILE.c`
// catches that class of error here. It is a syntax checker, not an SDK: nothing below behaves like the real thing,
// and a clean check never replaces `bash core/host3ds/build.sh`.
#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include <math.h>
typedef uint8_t u8; typedef uint16_t u16; typedef uint32_t u32; typedef uint64_t u64;
typedef int8_t s8; typedef int16_t s16; typedef int32_t s32; typedef int64_t s64;
typedef int32_t Result;
typedef uint32_t Handle;
#define R_SUCCEEDED(r) ((r) >= 0)
#define R_FAILED(r) ((r) < 0)
#define SYSCLOCK_ARM11 268111856LL
#define CUR_THREAD_HANDLE 0xFFFF8000

// Threads
typedef struct Thread_tag* Thread;
static inline Thread threadCreate(void (*f)(void*), void* a, size_t sz, int prio, int core, bool detach) {
    (void)f; (void)a; (void)sz; (void)prio; (void)core; (void)detach; return 0;
}
static inline Result threadJoin(Thread t, u64 ns) { (void)t; (void)ns; return 0; }
static inline void threadFree(Thread t) { (void)t; }
static inline int svcGetThreadPriority(int32_t* out, uint32_t h) { (void)h; if (out) *out = 0x30; return 0; }
static inline void svcSleepThread(int64_t ns) { (void)ns; }
static inline uint64_t svcGetSystemTick(void) { return 0; }
static inline Result svcCloseHandle(Handle h) { (void)h; return 0; }
static inline Result svcConnectToPort(Handle* out, const char* name) { (void)name; if (out) *out = 0; return 0; }
static inline Result svcSendSyncRequest(Handle h) { (void)h; return 0; }
static inline u32* getThreadCommandBuffer(void) { static u32 b[64]; return b; }
#define IPC_MakeHeader(cmd, normal, translate) ((u32)(((cmd) << 16) | ((normal) << 6) | (translate)))
#define IPC_Desc_StaticBuffer(size, id) ((u32)(((size) << 14) | ((id) << 10) | 0x2))

// Graphics
typedef enum { GFX_TOP = 0, GFX_BOTTOM } gfxScreen_t;
typedef enum { GFX_LEFT = 0, GFX_RIGHT } gfx3dSide_t;
static inline void gfxInitDefault(void) {}
static inline void gfxExit(void) {}
static inline void gfxSwapBuffers(void) {}
static inline void gfxFlushBuffers(void) {}
static inline void gfxSetDoubleBuffering(gfxScreen_t s, bool on) { (void)s; (void)on; }
static inline u8* gfxGetFramebuffer(gfxScreen_t s, gfx3dSide_t e, u16* w, u16* h) {
    (void)s; (void)e; if (w) *w = 240; if (h) *h = 400; return 0;
}
static inline void gspWaitForVBlank(void) {}
static inline void consoleInit(gfxScreen_t s, void* c) { (void)s; (void)c; }
static inline void consoleClear(void) {}

// Input
enum {
    KEY_A = 1, KEY_B = 2, KEY_SELECT = 4, KEY_START = 8, KEY_DRIGHT = 16, KEY_DLEFT = 32, KEY_DUP = 64,
    KEY_DDOWN = 128, KEY_R = 256, KEY_L = 512, KEY_X = 1024, KEY_Y = 2048, KEY_ZL = 1 << 14, KEY_ZR = 1 << 15,
    KEY_TOUCH = 1 << 20, KEY_CSTICK_RIGHT = 1 << 24, KEY_CSTICK_LEFT = 1 << 25, KEY_CSTICK_UP = 1 << 26,
    KEY_CSTICK_DOWN = 1 << 27, KEY_CPAD_RIGHT = 1 << 28, KEY_CPAD_LEFT = 1 << 29, KEY_CPAD_UP = 1 << 30
};
typedef struct { s16 dx, dy; } circlePosition;
static inline void hidScanInput(void) {}
static inline u32 hidKeysDown(void) { return 0; }
static inline u32 hidKeysHeld(void) { return 0; }
static inline void hidCircleRead(circlePosition* p) { if (p) { p->dx = 0; p->dy = 0; } }
static inline void hidCstickRead(circlePosition* p) { if (p) { p->dx = 0; p->dy = 0; } }

// Applet, services, memory
static inline bool aptMainLoop(void) { return false; }
static inline void aptSetChainloaderToSelf(void) {}
static inline Result APT_SetAppCpuTimeLimit(u32 pct) { (void)pct; return 0; }
static inline Result ptmSysmInit(void) { return 0; }
static inline void ptmSysmExit(void) {}
static inline Result PTMSYSM_ConfigureNew3DSCPU(u8 v) { (void)v; return 0; }
static inline Result romfsInit(void) { return 0; }
static inline Result romfsExit(void) { return 0; }
static inline void osSetSpeedupEnable(bool on) { (void)on; }
static inline u64 osGetTime(void) { return 0; }
static inline void* linearAlloc(size_t n) { (void)n; return 0; }
static inline u32 linearSpaceFree(void) { return 0; }
static inline u32 vramSpaceFree(void) { return 0; }

// Exceptions (crashlog.h)
typedef struct { u32 type, isdata, fault_type, fsr, far, fpexc, fpinst, fpinst2; } ERRF_ExceptionInfo;
typedef struct { u32 r[13], sp, lr, pc, cpsr; } CpuRegisters;

// Events, and the newlib detail asyncfile.c's FILE hook needs
#define U64_MAX UINT64_MAX
typedef int _READ_WRITE_BUFSIZE_TYPE;
typedef enum { RESET_ONESHOT = 0, RESET_STICKY, RESET_PULSE } ResetType;
typedef struct { Handle handle; ResetType reset_type; } LightEvent;
static inline void LightEvent_Init(LightEvent* e, ResetType t) { if (e) { e->handle = 0; e->reset_type = t; } }
static inline void LightEvent_Signal(LightEvent* e) { (void)e; }
static inline void LightEvent_Clear(LightEvent* e) { (void)e; }
static inline void LightEvent_Wait(LightEvent* e) { (void)e; }
static inline bool LightEvent_TryWait(LightEvent* e) { (void)e; return false; }

// newlib has <sys/statvfs.h>; the host one is used for the syntax check.

// Add what a file you want to check needs, and nothing more.
