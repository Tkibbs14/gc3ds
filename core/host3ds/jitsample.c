// Sampling profiler for core 0: a low-priority thread on core 1 reads, every ~100 us, whether core 0 is in compiled
// code and which block it last entered (Gekko.hostPc, one store per block). Every 10 s the samples are mapped to guest
// PCs and the hottest blocks go to perf.csv, so the guest code that costs most can be found and its compiled form
// improved. Outside compiled code (C dispatcher, events, waits) is counted as one bucket.
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define GC3DS_HOST_TYPES
#include "../src/common.h"
#include "../src/jit/jit.h"
#include "jitsample.h"

#define MAX_SAMPLES 200000
static u32* g_samples;
static volatile u32 g_n, g_outside, g_whereN[WH_N];
static volatile bool g_quit;
static Thread g_thread;
static const Gekko* g_g;

static void sampler(void* arg) {
    (void)arg;
    while (!g_quit) {
        if (jit_in_run) {
            u32 n = g_n;
            if (n < MAX_SAMPLES) { g_samples[n] = g_g->hostPc; g_n = n + 1; }
        } else {
            g_outside = g_outside + 1;
            unsigned char w = g_where;
            g_whereN[w < WH_N ? w : 0]++;
        }
        svcSleepThread(100000);
    }
}

bool jitsample_start(const Gekko* g) {
    if (g_thread) return true;
    g_g = g;
    g_samples = malloc(MAX_SAMPLES * sizeof *g_samples);
    if (!g_samples) return false;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    g_thread = threadCreate(sampler, NULL, 8 * 1024, prio + 2 > 0x3F ? 0x3F : prio + 2, 1, true);
    return g_thread != NULL;
}

static int by_count(const void* a, const void* b) {
    const u32* x = a; const u32* y = b;
    return x[1] < y[1] ? 1 : x[1] > y[1] ? -1 : 0;
}
static int by_value(const void* a, const void* b) {
    u32 x = *(const u32*)a, y = *(const u32*)b;
    return x < y ? -1 : x > y;
}

u32 jitsample_hot(const Jit* j, u32* pcs, u32 max) {
    if (!g_thread || !g_samples) return 0;
    return jit_hot_blocks(j, g_samples, g_n, pcs, max);
}

void jitsample_report(FILE* f, const Jit* j) {
    if (!g_thread || !f) return;
    u32 n = g_n, outside = g_outside;
    jit_map_hosts(j, g_samples, n);
    qsort(g_samples, n, sizeof *g_samples, by_value);
    // (pc, count) pairs, then the most frequent.
    static u32 pairs[2 * 4096];
    u32 np = 0;
    for (u32 i = 0; i < n && np < 4096;) {
        u32 k = i;
        while (k < n && g_samples[k] == g_samples[i]) k++;
        pairs[2 * np] = g_samples[i];
        pairs[2 * np + 1] = k - i;
        np++;
        i = k;
    }
    qsort(pairs, np, 8, by_count);
    double tot = (double)(n + outside);
    fprintf(f, "# hot guest code 10s (%u samples): outside compiled code %.1f%%;", (unsigned)(n + outside),
            tot > 0 ? 100.0 * outside / tot : 0.0);
    for (u32 i = 0; i < np && i < 200; i++)  // enough to group by function offline (run with --profile on)
        fprintf(f, " %08lx %.1f%%", (unsigned long)pairs[2 * i], tot > 0 ? 100.0 * pairs[2 * i + 1] / tot : 0.0);
    fprintf(f, "\n");
    {  // where core 0 was when it was not in compiled code (common.h: WH_*), as a share of all samples
        static const char* const kWh[WH_N] = { "none", "gekko_run", "dispatch", "compile", "sys_run", "events", "gx_poll", "hle",
                                               "interp", "frontend", "present", "vblank_wait", "perf_log",
                                               "perf_csv", "perf_reports" };
        fprintf(f, "# core0 outside compiled code 10s:");
        for (int k = 0; k < WH_N; k++) {
            if (g_whereN[k]) fprintf(f, " %s %.1f%%", kWh[k], tot > 0 ? 100.0 * g_whereN[k] / tot : 0.0);
            g_whereN[k] = 0;
        }
        fprintf(f, "\n");
    }
    g_n = 0;
    g_outside = 0;
}
