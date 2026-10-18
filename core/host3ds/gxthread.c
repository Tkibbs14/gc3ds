// GX on core 2: the CPU thread (core 0) queues write-gather bursts and presents into a ring buffer, and a thread on
// core 2 executes them (FIFO parsing, vertices, citro3d). PE token / finish signals come back through an event queue
// the CPU thread applies (system.h, GXAsync). Measured on the New 2DS XL: core 2 runs our code at 95% of core 0's
// speed, and a spin-flag handoff between the cores takes about 0.9 us; LightEvent wake-ups across cores were not
// reliable (3 of 2000), so both sides poll.
#include <3ds.h>
#include "crashlog.h"
#define GC3DS_HOST_TYPES
#include <string.h>
#include "gxthread.h"
#include "../src/gx/pica/pica.h"

#define RING_BYTES (1u << 20)
#define EV_N 1024u

enum { REC_PAD = 0, REC_DATA = 1, REC_PRESENT = 2 };

static u8 g_ring[RING_BYTES] __attribute__((aligned(32)));
static volatile u32 g_head, g_tail;  // bytes queued (CPU thread) and executed (GX thread), free-running
static volatile u32 g_evHead, g_evTail;
static u32 g_evKind[EV_N], g_evVal[EV_N];
static volatile bool g_quit;
static bool g_on;
static System* g_s;
static Thread g_thread;
static void (*g_present)(System* s, const GXPresent* p);
static u64 g_lastPresent;
u32 g_gxPresentsSkipped;

// Orders memory accesses between the cores (CP15 data memory barrier, available to user mode on ARMv6).
static inline void dmb(void) { __asm__ volatile("mcr p15, 0, %0, c7, c10, 5" : : "r"(0) : "memory"); }

// ---- GX thread ----
static void ev_post(System* s, u32 kind, u32 value) {
    (void)s;
    // The vertex worker reads guest vertex arrays: the program must not see this signal (and change them) first.
    while (!pica_vertex_worker_idle()) {}
    while (g_evHead - g_evTail >= EV_N) svcSleepThread(10000);  // the CPU thread has not caught up: wait
    u32 i = g_evHead % EV_N;
    g_evKind[i] = kind;
    g_evVal[i] = value;
    dmb();
    g_evHead = g_evHead + 1;
}

static void gx_main(void* arg) {
    System* s = arg;
    crashlog_thread(CRASH_GX);
    __asm__ volatile("vmsr fpscr, %0" : : "r"(0x03000000u));  // round to nearest (threads start rounding to zero)
    u32 idle = 0;
    while (!g_quit) {
        u32 tail = g_tail;
        if (tail == g_head) {
            // The ring stayed empty for a moment: issue the draw held open for merging (pica.c), which a sync waits for.
            if (++idle == 32 && g_gxMergeOpen) {
                u64 t0 = prof_begin();
                pica_merge_flush(s, true);
                prof_end(PROF_GXTHREAD, t0);
            }
            // Spin briefly (a sync waits on us), then sleep so an idle GX thread costs nothing.
            if (idle < 20000) continue;
            svcSleepThread(100000);
            continue;
        }
        idle = 0;
        dmb();
        g_gxWhere = GW_FIFO;
        u64 t0 = prof_begin();
        u32 at = tail % RING_BYTES, hdr;
        memcpy(&hdr, g_ring + at, 4);
        u32 type = hdr >> 24, len = hdr & 0xFFFFFF;
        if (type == REC_PAD) {
            tail += RING_BYTES - at;  // to the start of the ring
        } else {
            if (type == REC_DATA) {
                gxcore_feed(s, &s->gx.core, g_ring + at + 4, len);
            } else if (type == REC_PRESENT) {
                GXPresent p;
                memcpy(&p, g_ring + at + 4, sizeof p);
                // Presenting more than once per 3DS refresh floods the GPU service (FINDINGS); skip instead.
                u64 now = svcGetSystemTick();
                if (now - g_lastPresent < SYSCLOCK_ARM11 / 62) g_gxPresentsSkipped++;
                else {
                    u64 tp = prof_begin();
                    g_present(s, &p);
                    prof_end(PROF_GXPRES, tp);
                    g_lastPresent = svcGetSystemTick();
                }
            }
            tail += 4 + ((len + 3) & ~3u);
        }
        prof_end(PROF_GXTHREAD, t0);
        g_gxWhere = GW_RING;
        dmb();
        g_tail = tail;
    }
}

// ---- CPU thread ----
// Space for a record of `bytes` (header included), contiguous in the ring; waits while the GX thread is behind.
static u8* ring_reserve(u32 bytes) {
    for (;;) {
        u32 at = g_head % RING_BYTES;
        if (at + bytes > RING_BYTES) {  // no room before the end: pad to it and start over at 0
            u32 pad = RING_BYTES - at;
            if (RING_BYTES - (g_head - g_tail) < pad) { u64 t0 = prof_begin(); while (RING_BYTES - (g_head - g_tail) < pad) {} prof_end(PROF_GXWAIT, t0); }
            u32 hdr = (u32)REC_PAD << 24;
            memcpy(g_ring + at, &hdr, 4);
            dmb();
            g_head = g_head + pad;
            continue;
        }
        if (RING_BYTES - (g_head - g_tail) < bytes) {
            u64 t0 = prof_begin();
            while (RING_BYTES - (g_head - g_tail) < bytes) {}
            prof_end(PROF_GXWAIT, t0);
        }
        return g_ring + at;
    }
}

static void ring_commit(u32 bytes) {
    dmb();
    g_head = g_head + bytes;
}

static void feed(System* s, const u8* data, u32 len) {
    (void)s;
    u32 bytes = 4 + ((len + 3) & ~3u);
    u8* p = ring_reserve(bytes);
    u32 hdr = ((u32)REC_DATA << 24) | len;
    memcpy(p, &hdr, 4);
    memcpy(p + 4, data, len);
    ring_commit(bytes);
}

// Applies the PE signals the GX thread posted; returns how many.
static u32 apply_events(System* s) {
    u32 n = 0;
    while (g_evTail != g_evHead) {
        dmb();
        u32 i = g_evTail % EV_N;
        gx_pe_signal(s, g_evKind[i], g_evVal[i]);
        g_evTail = g_evTail + 1;
        n++;
    }
    return n;
}

static void poll(System* s) { apply_events(s); }

// Everything fed so far executed, including the draw held open for merging (the GX thread issues it once the ring is
// empty, and sets g_gxMergeOpen before it publishes g_tail) and the vertex worker's reads of guest memory (its draws
// are queued before the GX thread publishes g_tail or clears g_gxMergeOpen, so the worker check after those sees them).
static bool drained(void) {
    if (g_tail != g_head) return false;
    dmb();
    if (*(volatile bool*)&g_gxMergeOpen) return false;
    dmb();
    return pica_vertex_worker_idle();
}

static void wait_drained(void) {
    if (drained()) return;
    u64 t0 = prof_begin();
    u64 limit = svcGetSystemTick() + 2 * SYSCLOCK_ARM11;
    while (!drained()) {
        if (svcGetSystemTick() > limit) {
            char w[160];
            pica_watch(w, sizeof w);
            emu_error("GX thread: no progress for 2 s (head %08lx tail %08lx; %s)\n", (unsigned long)g_head,
                      (unsigned long)g_tail, w);
            break;
        }
    }
    prof_end(PROF_GXWAIT, t0);
}

static bool sync(System* s) {
    wait_drained();
    dmb();
    return apply_events(s) != 0;
}

static const GXAsync kAsync = { feed, ev_post, poll, sync };

static bool idle_hook(void* ctx) { return sync(ctx); }

bool gxthread_start(System* s, void (*present)(System* s, const GXPresent* p)) {
    g_s = s;
    g_present = present;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    g_thread = threadCreate(gx_main, s, 64 * 1024, prio, 2, false);
    if (!g_thread) return false;
    gxthread_enable(s, true);
    return true;
}

void gxthread_present(const GXPresent* p) {
    u32 bytes = 4 + ((sizeof *p + 3) & ~3u);
    u8* r = ring_reserve(bytes);
    u32 hdr = ((u32)REC_PRESENT << 24) | sizeof *p;
    memcpy(r, &hdr, 4);
    memcpy(r + 4, p, sizeof *p);
    ring_commit(bytes);
}

void gxthread_drain(void) {
    if (!g_on) return;
    wait_drained();
    dmb();
    apply_events(g_s);
}

void gxthread_enable(System* s, bool on) {
    if (on && !g_thread) return;
    gxthread_drain();
    g_on = on;
    s->gxAsync = on ? &kAsync : NULL;
    gekko_set_idle_hook(&s->cpu, on ? idle_hook : NULL, s);
}

bool gxthread_enabled(void) { return g_on; }
