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
#include "../src/ramlayout.h"
#include "jitsample.h"

#define MAX_SAMPLES 200000
static u32* g_samples;
static volatile u32 g_n, g_outside, g_whereN[WH_N];
extern volatile u8 g_gxWhere;  // the GX thread's position (pica.c GW_*), sampled with core 0
#define GXW_N 40
static volatile u32 g_gxwN[GXW_N];
static volatile bool g_quit;
static Thread g_thread;
static const Gekko* g_g;

// Guest call stacks (--profile on, jitsample_stacks): each sample in compiled code also walks the guest stack's back
// chain from r1 (EABI: the back chain at 0(sp), a caller's saved LR at 4 of the frame above) and counts every return
// address on it once, so a function's share includes everything it called (inclusive time: which subsystem's whole
// call tree is worth running natively, HANDOFF 2026-10-17). Read while core 0 runs: r1 in guest state may be a block
// behind, and a frame being built may give one wrong address; anything outside MEM1 ends the walk.
#define STK_BITS 13
#define STK_DEPTH 32
static const u8* g_mem1;
static u32 g_stkAddr[1u << STK_BITS], g_stkCount[1u << STK_BITS];
static volatile u32 g_stkWalks, g_stkLost;

static inline bool stk_ok(u32 a) { return a - 0x80000000u < 0x01800000u - 8 && !(a & 3); }

// Guest threads (with the stacks): each sample counted against the OS's current thread (__OSCurrentThread, 0x800000E4),
// with the outermost return address its stack walk reached (the thread's entry, mapped to a function offline), so a
// thread that keeps core 0 busy without doing the frame's work (polling instead of idling) shows apart.
#define THR_N 24
static u32 g_thrPtr[THR_N], g_thrCount[THR_N], g_thrRoot[THR_N], g_thrOther;
static void thr_note(u32 root) {
    u32 t = ram_rd32(g_mem1, 0xE4);
    for (u32 k = 0; k < THR_N; k++) {
        if (g_thrPtr[k] == t && g_thrCount[k]) { g_thrCount[k]++; if (root) g_thrRoot[k] = root; return; }
        if (!g_thrCount[k]) { g_thrPtr[k] = t; g_thrCount[k] = 1; g_thrRoot[k] = root; return; }
    }
    g_thrOther++;
}

static void stk_walk(void) {
    u32 seen[STK_DEPTH + 1], ns = 0, root = 0;
    u32 sp = g_g->gpr[1];
    for (u32 d = 0; d <= STK_DEPTH; d++) {
        u32 ra;
        if (d == 0) {
            ra = g_g->lr;  // a leaf function's caller is only in LR (it saves nothing)
        } else {
            if (!stk_ok(sp)) break;
            u32 up = ram_rd32(g_mem1, sp - 0x80000000u);
            if (!stk_ok(up) || up <= sp) break;
            ra = ram_rd32(g_mem1, up + 4 - 0x80000000u);
            sp = up;
        }
        if (!stk_ok(ra)) continue;
        root = ra;
        bool dup = false;
        for (u32 k = 0; k < ns; k++) if (seen[k] == ra) { dup = true; break; }
        if (dup) continue;
        seen[ns++] = ra;
        u32 h = (ra >> 2) * 2654435761u >> (32 - STK_BITS);
        for (u32 probe = 0; probe < 64; probe++, h = (h + 1) & ((1u << STK_BITS) - 1)) {
            if (g_stkAddr[h] == ra) { g_stkCount[h]++; break; }
            if (!g_stkAddr[h]) { g_stkAddr[h] = ra; g_stkCount[h] = 1; break; }
            if (probe == 63) g_stkLost++;
        }
    }
    g_stkWalks++;
    thr_note(root);
}

void jitsample_stacks(const u8* mem1) { g_mem1 = mem1; }

// Joint states (perf.csv '# joint 10s'): at each sample, core 0 (running guest code / waiting for ring space / waiting
// for the GX thread to drain / other), the GX thread (working / its ring empty / waiting for the vertex worker / waiting
// for the GPU) and the worker (working / idle), counted together: which resource the others wait on, sample by sample.
extern volatile u8 g_c0Wait, g_vwBusyNow;
extern volatile u8 g_shWhere;  // hle_tp.c: where the ShdwDraw stand-in is
static volatile u32 g_shwN[8];
static volatile u32 g_joint[4][4][2];
static inline u32 gx_state(u8 w) {
    if (w == 0) return 1;                     // idle/ring
    if (w == 3 || w == 4 || w == 5) return 2; // vwwait, vwalloc, splitwait: the worker
    if (w == 6) return 3;                     // framebegin: the GPU
    return 0;
}
static void sampler(void* arg) {
    (void)arg;
    while (!g_quit) {
        if (jit_in_run) {
            u32 n = g_n;
            if (n < MAX_SAMPLES) { g_samples[n] = g_g->hostPc; g_n = n + 1; }
            if (g_mem1) stk_walk();
        } else {
            g_outside = g_outside + 1;
            unsigned char w = g_where;
            g_whereN[w < WH_N ? w : 0]++;
        }
        { u8 gw = g_gxWhere; g_gxwN[gw < GXW_N ? gw : GXW_N - 1]++; }
        { u8 sw = g_shWhere; if (sw) g_shwN[sw & 7]++; }
        {
            u8 cw = g_c0Wait, gw = g_gxWhere, vb = g_vwBusyNow;
            u32 c0 = cw == 1 ? 1 : cw == 2 ? 2 : jit_in_run ? 0 : 3;
            g_joint[c0][gx_state(gw)][vb ? 0 : 1]++;
        }
        svcSleepThread(100000);
    }
}

bool jitsample_start(const Gekko* g) {
    if (g_thread) return true;
    g_g = g;
    g_samples = malloc(MAX_SAMPLES * sizeof *g_samples);
    if (!g_samples) return false;
    jit_sample_prepare();  // the report's work arrays now, before the run fragments the heap (job 282)
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
    // Every line below that comes from a sample's address needs blocks to store their start (jit_set_profile, i.e.
    // --profile on). Without it Gekko.hostPc never changes and each sample reads the same stale word, which read as
    // one guest address at 38% of core 0 and a footprint of "1 block, 16 KB" - plausible-looking nonsense (2026-10-30).
    // The counters that do not use an address (where core 0 was, the GX thread, the joint states) are fine either way.
    bool addrs = jit_profile(j);
    double tot = (double)(n + outside);  // every line's denominator: all samples, in compiled code or not. `n` is a
                                         // valid count either way - only the sampled *addresses* need `addrs`.
    if (!addrs) {
        fprintf(f, "# hot guest code 10s: not sampled (--profile off: blocks do not store their start)\n");
        g_n = 0;
    }
    if (addrs) {  // how much compiled code core 0's time is spread over (before the samples are mapped to guest PCs)
        u32 fp[5][3];
        jit_footprint(j, g_samples, n, fp);
        fprintf(f, "# code footprint 10s (blocks, host KB covering this share of time in compiled code): 50%% %u %.1f, "
                "80%% %u %.1f, 90%% %u %.1f, 99%% %u %.1f, all sampled %u %.1f\n", (unsigned)fp[0][0], fp[0][1] / 1024.0,
                (unsigned)fp[1][0], fp[1][1] / 1024.0, (unsigned)fp[2][0], fp[2][1] / 1024.0, (unsigned)fp[3][0],
                fp[3][1] / 1024.0, (unsigned)fp[4][0], fp[4][1] / 1024.0);
        fprintf(f, "# code pages 10s (4 KB host pages those blocks span): 50%% %u, 80%% %u, 90%% %u, 99%% %u, all sampled %u\n",
                (unsigned)fp[0][2], (unsigned)fp[1][2], (unsigned)fp[2][2], (unsigned)fp[3][2], (unsigned)fp[4][2]);
    }
    if (addrs && !jit_map_hosts(j, g_samples, n)) {
        fprintf(f, "# hot guest code 10s: not mapped (no memory for the work arrays)\n");
        g_n = 0;
        g_outside = 0;
        return;
    }
    if (addrs) {
    qsort(g_samples, n, sizeof *g_samples, by_value);
    // (pc, count) pairs, then the most frequent.
    // Room for every block a report samples (4,600-7,300 distinct in TP, job 309): with 4,096 the pairs, built in PC
    // order, dropped the highest addresses, runtime-loaded modules first (the movie's decoder read 0% instead of ~10%).
    enum { MAX_PAIRS = 16384 };
    static u32 pairs[2 * MAX_PAIRS];
    u32 np = 0, i = 0;
    while (i < n && np < MAX_PAIRS) {
        u32 k = i;
        while (k < n && g_samples[k] == g_samples[i]) k++;
        pairs[2 * np] = g_samples[i];
        pairs[2 * np + 1] = k - i;
        np++;
        i = k;
    }
    if (i < n) fprintf(f, "# hot guest code 10s: %u samples past %u distinct blocks not listed\n", (unsigned)(n - i), (unsigned)MAX_PAIRS);
    qsort(pairs, np, 8, by_count);
    fprintf(f, "# hot guest code 10s (%u samples): outside compiled code %.1f%%;", (unsigned)(n + outside),
            tot > 0 ? 100.0 * outside / tot : 0.0);
    for (u32 i = 0; i < np && i < 1500; i++)  // enough to group by function and library offline (--profile on; a flat profile needs many)
        fprintf(f, " %08lx %.3f%%", (unsigned long)pairs[2 * i], tot > 0 ? 100.0 * pairs[2 * i + 1] / tot : 0.0);
    fprintf(f, "\n");
    }
    {  // where core 0 was when it was not in compiled code (common.h: WH_*), as a share of all samples
        static const char* const kWh[WH_N] = { "none", "gekko_run", "dispatch", "compile", "sys_run", "events", "gx_poll", "hle",
                                               "interp", "frontend", "present", "vblank_wait", "perf_log",
                                               "perf_csv", "perf_reports" };
        static const char* const kGw[] = { "idle/ring", "draw", "drawcall", "vwwait", "vwalloc", "splitwait", "framebegin",
                                           "frameend", "split", "tex", "copy", "shapes", "fifo", "merge", "flush", "tev",
                                           "batchkey", "vprep", "vqueue", "state2", "drawtail", "dlist", "dec_bp",
                                           "dec_xf", "dec_cp", "dec_idx", "dec_list", "dec_feed", "dec_dlnote", "dec_draw", "dec_run", "dec_op",
                                           "palassign", "cpualone", "vwqueue", "vwdo", "g2" };
        u32 gt = 0;
        for (int k = 0; k < GXW_N; k++) gt += g_gxwN[k];
        fprintf(f, "# gx thread where 10s (%u samples):", (unsigned)gt);
        for (int k = 0; k < GXW_N; k++) {
            if (g_gxwN[k]) fprintf(f, " %s %.1f%%", k < (int)(sizeof kGw / sizeof kGw[0]) ? kGw[k] : "?",
                                   gt ? 100.0 * g_gxwN[k] / gt : 0.0);
            g_gxwN[k] = 0;
        }
        fprintf(f, "\n");
        {
            static const char* const c0n[4] = { "run", "ringwait", "gxsync", "other" };
            static const char* const gxn[4] = { "work", "idle", "waitvw", "waitgpu" };
            u32 jt = 0;
            for (int a = 0; a < 4; a++) for (int b = 0; b < 4; b++) for (int c = 0; c < 2; c++) jt += g_joint[a][b][c];
            fprintf(f, "# joint 10s (core0/gx/worker, share of samples):");
            for (int a = 0; a < 4; a++) for (int b = 0; b < 4; b++) for (int c = 0; c < 2; c++) {
                if (jt && g_joint[a][b][c] * 1000u >= jt) fprintf(f, " %s/%s/%s %.1f%%", c0n[a], gxn[b], c ? "idle" : "work", 100.0 * g_joint[a][b][c] / jt);
                g_joint[a][b][c] = 0;
            }
            fprintf(f, "\n");
        }
        {
            static const char* const shn[8] = { "", "replay", "walk", "path reads", "path maths", "GetTriPla", "callback", "hle_run" };
            u32 sh = 0;
            for (int k = 1; k < 8; k++) sh += g_shwN[k];
            if (sh) {
                fprintf(f, "# shdw where 10s (share of all core-0 samples):");
                for (int k = 1; k < 8; k++) fprintf(f, " %s %.1f%%", shn[k], tot > 0 ? 100.0 * g_shwN[k] / tot : 0.0);
                fprintf(f, "\n");
            }
            for (int k = 0; k < 8; k++) g_shwN[k] = 0;
        }
        fprintf(f, "# core0 outside compiled code 10s:");
        for (int k = 0; k < WH_N; k++) {
            if (g_whereN[k]) fprintf(f, " %s %.1f%%", kWh[k], tot > 0 ? 100.0 * g_whereN[k] / tot : 0.0);
            g_whereN[k] = 0;
        }
        fprintf(f, "\n");
    }
    if (g_mem1) {  // the return addresses on the sampled stacks, as a share of all samples (mapped to callers offline)
        enum { TOP = 600 };
        static u32 top[2 * TOP];
        u32 nt = 0;
        for (u32 h = 0; h < (1u << STK_BITS); h++) {
            if (!g_stkAddr[h]) continue;
            u32 c = g_stkCount[h], k = nt < TOP ? nt++ : TOP;
            if (k == TOP && c <= top[2 * (TOP - 1) + 1]) continue;
            if (k == TOP) k = TOP - 1;
            while (k > 0 && top[2 * (k - 1) + 1] < c) { top[2 * k] = top[2 * (k - 1)]; top[2 * k + 1] = top[2 * (k - 1) + 1]; k--; }
            top[2 * k] = g_stkAddr[h];
            top[2 * k + 1] = c;
        }
        fprintf(f, "# guest stacks 10s (%u walks, %u addresses lost):", (unsigned)g_stkWalks, (unsigned)g_stkLost);
        for (u32 i = 0; i < nt; i++)
            fprintf(f, " %08lx %.3f%%", (unsigned long)top[2 * i], tot > 0 ? 100.0 * top[2 * i + 1] / tot : 0.0);
        fprintf(f, "\n");
        memset(g_stkAddr, 0, sizeof g_stkAddr);
        g_stkWalks = g_stkLost = 0;
        fprintf(f, "# guest threads 10s (thread, share of all samples, outermost return address):");
        for (u32 k = 0; k < THR_N && g_thrCount[k]; k++)
            fprintf(f, " %08lx %.1f%% %08lx", (unsigned long)g_thrPtr[k], tot > 0 ? 100.0 * g_thrCount[k] / tot : 0.0,
                    (unsigned long)g_thrRoot[k]);
        if (g_thrOther) fprintf(f, " other %.1f%%", tot > 0 ? 100.0 * g_thrOther / tot : 0.0);
        fprintf(f, "\n");
        memset(g_thrCount, 0, sizeof g_thrCount);
        g_thrOther = 0;
        // Every guest OS thread on the active queue (OS global 0x800000DC; OSThread link at 0x2FC), with its state
        // (1 ready, 2 running, 4 waiting), saved PC, LR and up to six return addresses from its back chain: where a
        // thread that never runs is waiting (sampling only sees running ones).
        fprintf(f, "# guest os threads:");
        u32 t = ram_rd32(g_mem1, 0xDC);
        for (u32 k = 0; k < 16 && t >= 0x80000000u && t < 0x81800000u; k++) {
            u32 a = t - 0x80000000u, sp = ram_rd32(g_mem1, a + 4);
            fprintf(f, " [%08lx st %u pc %08lx lr %08lx", (unsigned long)t, (unsigned)(ram_rd32(g_mem1, a + 0x2C8) >> 16),
                    (unsigned long)ram_rd32(g_mem1, a + 0x198), (unsigned long)ram_rd32(g_mem1, a + 0x84));
            for (u32 d = 0; d < 6 && sp >= 0x80000000u && sp < 0x81800000u; d++) {
                sp = ram_rd32(g_mem1, sp - 0x80000000u);
                if (sp < 0x80000000u || sp >= 0x81800000u) break;
                fprintf(f, " %08lx", (unsigned long)ram_rd32(g_mem1, sp + 4 - 0x80000000u));
            }
            fprintf(f, "]");
            t = ram_rd32(g_mem1, a + 0x2FC);
        }
        fprintf(f, "\n");
    }
    g_n = 0;
    g_outside = 0;
}
