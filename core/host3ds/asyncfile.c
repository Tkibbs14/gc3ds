// A file written without blocking the emulation thread: writes are copied into a memory ring, and a thread on core 1
// drains it to the card. perf.csv used to be flushed to the SD card on core 0 once a second, and the once-a-second
// logging was 4.5-4.7% of core 0 (2DS, 3605dfd: the `perf_log` share of "# core0 outside compiled code"). One file at a
// time (perf.csv).
#include <3ds.h>
#define GC3DS_HOST_TYPES
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "asyncfile.h"

#define RING (256u << 10)

static u8* g_ring;
static volatile u32 g_head, g_tail;  // bytes written by the caller / to the card, free-running
static volatile bool g_closing;
static FILE* g_file;
static Thread g_thread;

static inline void dmb(void) { __asm__ volatile("mcr p15, 0, %0, c7, c10, 5" : : "r"(0) : "memory"); }

static void writer(void* arg) {
    (void)arg;
    u32 sinceFlush = 0;
    for (;;) {
        u32 head = g_head, tail = g_tail;
        dmb();
        if (head == tail) {
            if (sinceFlush) { fflush(g_file); sinceFlush = 0; }
            if (g_closing) break;
            svcSleepThread(50000000LL);  // 50 ms
            continue;
        }
        u32 at = tail % RING, n = head - tail;
        if (n > RING - at) n = RING - at;  // up to the ring's end this round
        fwrite(g_ring + at, 1, n, g_file);
        sinceFlush += n;
        if (sinceFlush >= (32u << 10)) { fflush(g_file); sinceFlush = 0; }
        dmb();
        g_tail = tail + n;
    }
}

static int af_write(void* cookie, const char* buf, _READ_WRITE_BUFSIZE_TYPE len) {
    (void)cookie;
    for (int done = 0; done < (int)len;) {
        u32 room = RING - (g_head - g_tail);
        if (!room) { svcSleepThread(1000000LL); continue; }  // the card is behind: wait (rare)
        u32 at = g_head % RING, n = (u32)(len - done);
        if (n > room) n = room;
        if (n > RING - at) n = RING - at;
        memcpy(g_ring + at, buf + done, n);
        dmb();
        g_head = g_head + n;
        done += (int)n;
    }
    return (int)len;
}

static int af_close(void* cookie) {
    (void)cookie;
    dmb();
    g_closing = true;
    threadJoin(g_thread, U64_MAX);
    threadFree(g_thread);
    g_thread = NULL;
    int r = fclose(g_file);
    g_file = NULL;
    return r;
}

FILE* asyncfile_open(const char* path, const char* mode) {
    if (g_file) return NULL;  // one at a time
    if (!g_ring && !(g_ring = malloc(RING))) return fopen(path, mode);
    g_file = fopen(path, mode);
    if (!g_file) return NULL;
    g_head = g_tail = 0;
    g_closing = false;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    g_thread = threadCreate(writer, NULL, 16 * 1024, prio + 2 > 0x3F ? 0x3F : prio + 2, 1, false);
    if (!g_thread) { FILE* f = g_file; g_file = NULL; return f; }  // no thread: write directly
    FILE* f = funopen(NULL, NULL, af_write, NULL, af_close);
    if (!f) {  // no stream: stop the writer and write directly
        g_closing = true;
        threadJoin(g_thread, U64_MAX);
        threadFree(g_thread);
        g_thread = NULL;
        f = g_file;
        g_file = NULL;
        return f;
    }
    setvbuf(f, NULL, _IONBF, 0);  // writes go straight to the ring (the caller's own FILE buffers them)
    return f;
}
