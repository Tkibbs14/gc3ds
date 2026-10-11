#pragma once
// GX on core 2 (gxthread.c): implements System.gxAsync for the 3DS frontend.
#include "../src/sys/system.h"

// The video output for one present, read on the CPU thread (VI state belongs to it).
typedef struct { u32 top, bottom, w, h, stride; bool valid; } GXPresent;

// Starts the GX thread on core 2 and attaches it to s. `present` runs on the GX thread for each gxthread_present.
// Returns false (and leaves GX inline) if the thread cannot be created.
bool gxthread_start(System* s, void (*present)(System* s, const GXPresent* p));
// Queues a present after everything fed so far.
void gxthread_present(const GXPresent* p);
// Waits until the GX thread has executed everything queued (before resets and boots).
void gxthread_drain(void);
// Turns asynchronous GX off (inline on the CPU thread) or back on; drains first.
void gxthread_enable(System* s, bool on);
bool gxthread_enabled(void);
// Presents skipped on the GX thread because the previous one was less than a refresh ago.
extern u32 g_gxPresentsSkipped;
