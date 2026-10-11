// 2DS hardware self-test, run once at startup (well under a second) and written to sdmc:/gc3ds/hw.txt, so design
// questions about the hardware are answered by measurement on the device rather than by asking the owner.
// - cost of reading the system tick (what profiling costs);
// - cores 1-3: whether our threads run there, their speed and clock, and the round trip of a core 0 <-> core 2
//   handoff through a spin flag and through LightEvent;
// - the vertex path (gx/vfast.c) on a synthetic lit draw: ns per vertex lit and unlit, and per draw;
// - random-access memory latency in 256 KB, 1 MB and 16 MB buffers.
#include <3ds.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#define GC3DS_HOST_TYPES
#include "../src/gx/vfast.h"
#include "../src/sys/system.h"

static double ns_since(u64 t0) { return (double)(svcGetSystemTick() - t0) * 1e9 / SYSCLOCK_ARM11; }

// Same loop as main.c's clock estimate: ~18 cycles per iteration.
static double mhz(void) {
    u32 n = 200000, x = 0;
    u64 t0 = svcGetSystemTick();
    __asm__ volatile(
        "1:\n"
        "add %1, %1, #1\n add %1, %1, #1\n add %1, %1, #1\n add %1, %1, #1\n"
        "add %1, %1, #1\n add %1, %1, #1\n add %1, %1, #1\n add %1, %1, #1\n"
        "add %1, %1, #1\n add %1, %1, #1\n add %1, %1, #1\n add %1, %1, #1\n"
        "add %1, %1, #1\n add %1, %1, #1\n add %1, %1, #1\n add %1, %1, #1\n"
        "subs %0, %0, #1\n bne 1b\n"
        : "+r"(n), "+r"(x) : : "cc");
    return 200000.0 * 18 / (ns_since(t0) / 1e9) / 1e6;
}

// ---- cores ----
static volatile u32 g_flag, g_quit;
static double g_coreMhz[4];
static LightEvent g_req, g_resp;

static void clock_job(void* arg) { g_coreMhz[(u32)arg] = mhz(); }

static void spin_partner(void* arg) {
    (void)arg;
    while (!g_quit) { if (g_flag == 1) g_flag = 2; }
}

static void event_partner(void* arg) {
    (void)arg;
    for (;;) {
        if (!LightEvent_WaitTimeout(&g_req, 1000000000LL)) return;  // 1 s without a request: give up
        if (g_quit) return;
        LightEvent_Signal(&g_resp);
    }
}

static void probe_cores(FILE* f, s32 prio) {
    g_coreMhz[0] = mhz();
    for (u32 c = 1; c < 4; c++) {
        g_coreMhz[c] = 0;
        Thread t = threadCreate(clock_job, (void*)c, 4096, prio, (int)c, false);
        if (!t) { fprintf(f, "core %lu: thread not created\n", (unsigned long)c); continue; }
        bool done = R_SUCCEEDED(threadJoin(t, 1000000000ULL));
        fprintf(f, "core %lu: %.0f MHz%s\n", (unsigned long)c, g_coreMhz[c], done ? "" : " (did not finish in 1 s)");
        if (done) threadFree(t);
    }
    fprintf(f, "core 0: %.0f MHz\n", g_coreMhz[0]);

    // Spin handoff core 0 <-> core 2.
    g_flag = 0; g_quit = 0;
    Thread t = threadCreate(spin_partner, NULL, 4096, prio, 2, false);
    if (t) {
        // At most 20000 rounds or 200 ms: a partner that never runs must not hang startup.
        const u32 rounds = 20000;
        u64 t0 = svcGetSystemTick(), deadline = t0 + SYSCLOCK_ARM11 / 5;
        u32 ok = 0, i = 0;
        for (; i < rounds && svcGetSystemTick() < deadline; i++) {
            g_flag = 1;
            while (g_flag != 2 && svcGetSystemTick() < deadline) {}
            if (g_flag == 2) ok++;
            g_flag = 0;
        }
        double ns = ns_since(t0) / (ok ? ok : 1);
        g_quit = 1;
        threadJoin(t, 1000000000ULL);
        threadFree(t);
        fprintf(f, "handoff core0<->core2 spin: %.0f ns per round trip (%lu of %lu answered)\n", ns, (unsigned long)ok,
                (unsigned long)i);
    }
    // LightEvent handoff core 0 <-> core 2.
    LightEvent_Init(&g_req, RESET_ONESHOT);
    LightEvent_Init(&g_resp, RESET_ONESHOT);
    g_quit = 0;
    t = threadCreate(event_partner, NULL, 4096, prio - 1, 2, false);
    if (t) {
        const u32 rounds = 2000;
        u32 ok = 0;
        u64 t0 = svcGetSystemTick(), deadline = t0 + SYSCLOCK_ARM11 / 4;
        for (u32 i = 0; i < rounds && svcGetSystemTick() < deadline; i++) {
            LightEvent_Signal(&g_req);
            if (LightEvent_WaitTimeout(&g_resp, 100000000LL)) ok++;
            else break;
        }
        double ns = ns_since(t0) / (ok ? ok : 1);
        g_quit = 1;
        LightEvent_Signal(&g_req);
        threadJoin(t, 2000000000ULL);
        threadFree(t);
        fprintf(f, "handoff core0<->core2 LightEvent: %.0f ns per round trip (%lu/%lu answered)\n", ns, (unsigned long)ok,
                (unsigned long)rounds);
    }
}

// ---- vertex path on a synthetic draw ----
static u32 fbits(float v) { u32 u; memcpy(&u, &v, 4); return u; }
static void put_be(u8* p, float v) { u32 u = fbits(v); p[0] = (u8)(u >> 24); p[1] = (u8)(u >> 16); p[2] = (u8)(u >> 8); p[3] = (u8)u; }

static void probe_vertices(FILE* f, System* s) {
    static GXCore gx;
    memset(&gx, 0, sizeof gx);
    gx.cp[0x50] = (1u << 9) | (1u << 11);           // position and normal direct
    gx.cp[0x70] = 1u | (4u << 1) | (4u << 10);      // xyz float position, float normal
    for (int r = 0; r < 3; r++) { gx.xf[r * 4 + r] = fbits(1); gx.xf[0x400 + r * 3 + r] = fbits(1); }
    gx.xf[0x1020] = fbits(1); gx.xf[0x1022] = fbits(1); gx.xf[0x1024] = fbits(-1); gx.xf[0x1025] = fbits(-1);
    gx.xf[0x101A] = fbits(320); gx.xf[0x101B] = fbits(-240); gx.xf[0x101C] = fbits(16777215);
    gx.xf[0x101D] = fbits(662); gx.xf[0x101E] = fbits(582); gx.xf[0x101F] = fbits(16777215);
    gx.xf[0x1009] = 1;
    gx.xf[0x100A] = 0x202020FF; gx.xf[0x100C] = 0xFFFFFFFF;
    for (int i = 0; i < 8; i++) {
        u32 b = 0x600 + 16 * i;
        gx.xf[b + 3] = 0xFFFFFFFF;
        gx.xf[b + 4] = fbits(1); gx.xf[b + 7] = fbits(1); gx.xf[b + 8] = fbits(0.01f); gx.xf[b + 9] = fbits(0.001f);
        gx.xf[b + 10] = fbits((float)i); gx.xf[b + 11] = fbits(10); gx.xf[b + 12] = fbits(5);
        gx.xf[b + 14] = fbits(-1);
    }
    enum { N = 64 };
    static u8 raw[N * 24];
    for (int i = 0; i < N; i++) {
        float v[6] = { (float)(i % 8), (float)(i / 8), -10.0f - i, 0, 0.6f, 0.8f };
        for (int k = 0; k < 6; k++) put_be(raw + i * 24 + 4 * k, v[k]);
    }
    static PVert out[N];
    VFold fold;
    vfold_init(&gx, (400.0f / 640.0f) * 2.0f / 512.0f, (240.0f / 480.0f) * 2.0f / 256.0f, false, &fold);
    VSel sel = { 0, 0, { 0, 0, 0 } };
    // Seven lights, clamped diffuse, spot attenuation (the title scene's common case), then unlit.
    u32 lit7 = 2u | (0xFu << 2) | (0x7u << 11) | (2u << 7) | (1u << 9) | (1u << 10);
    struct { const char* name; u32 ctrl, count, draws; } cases[] = {
        { "unlit, 64-vertex draws", 0, 64, 400 }, { "7 lights, 64-vertex draws", lit7, 64, 400 },
        { "unlit, 5-vertex draws", 0, 5, 4000 }, { "7 lights, 5-vertex draws", lit7, 5, 4000 },
        { "1 light, 64-vertex draws", 2u | (1u << 2) | (2u << 7) | (1u << 9) | (1u << 10), 64, 400 },
    };
    for (u32 c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        gx.xf[0x100E] = cases[c].ctrl;
        vfast_run(s, &gx, 0, &fold, &sel, raw, cases[c].count, 24, out);  // warm up
        u64 t0 = svcGetSystemTick();
        for (u32 d = 0; d < cases[c].draws; d++) vfast_run(s, &gx, 0, &fold, &sel, raw, cases[c].count, 24, out);
        double ns = ns_since(t0);
        fprintf(f, "vertices, %s: %.0f ns per vertex, %.0f ns per draw\n", cases[c].name,
                ns / (cases[c].draws * cases[c].count), ns / cases[c].draws);
    }
    fprintf(f, "  (check: vertex 0 colour %u,%u,%u,%u pos %.3f,%.3f,%.3f,%.3f)\n", out[0].col[0], out[0].col[1],
            out[0].col[2], out[0].col[3], out[0].pos[0], out[0].pos[1], out[0].pos[2], out[0].pos[3]);
}

// ---- memory latency ----
static void probe_memory(FILE* f) {
    static const u32 sizes[] = { 256u << 10, 1u << 20, 16u << 20 };
    for (u32 si = 0; si < 3; si++) {
        u32 bytes = sizes[si], n = bytes / 64;
        u32* buf = malloc(bytes);
        if (!buf) { fprintf(f, "memory %lu KB: no buffer\n", (unsigned long)(bytes >> 10)); continue; }
        // A random cycle through one word per 64-byte line.
        u32* order = malloc(n * 4);
        if (!order) { free(buf); continue; }
        for (u32 i = 0; i < n; i++) order[i] = i;
        u32 seed = 12345;
        for (u32 i = n - 1; i > 0; i--) { seed = seed * 1103515245u + 12345u; u32 j = (seed >> 8) % (i + 1); u32 t = order[i]; order[i] = order[j]; order[j] = t; }
        for (u32 i = 0; i < n; i++) buf[order[i] * 16] = order[(i + 1) % n] * 16;
        free(order);
        volatile u32 sink;
        u32 p = 0, steps = 200000;
        u64 t0 = svcGetSystemTick();
        for (u32 i = 0; i < steps; i++) p = buf[p];
        sink = p;
        (void)sink;
        fprintf(f, "memory %5lu KB random reads: %.1f ns each\n", (unsigned long)(bytes >> 10), ns_since(t0) / steps);
        free(buf);
    }
}

void hwprobe_run(System* s) {
    mkdir("sdmc:/gc3ds", 0777);
    FILE* f = fopen("sdmc:/gc3ds/hw.txt", "w");
    if (!f) return;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    u64 t0 = svcGetSystemTick();
    u64 sum = 0;
    for (u32 i = 0; i < 100000; i++) sum += svcGetSystemTick();
    __asm__ volatile("" : : "r"((u32)sum));
    fprintf(f, "svcGetSystemTick: %.0f ns per call\n", ns_since(t0) / 100000);
    u32 fpscr;
    __asm__ volatile("vmrs %0, fpscr" : "=r"(fpscr));
    fprintf(f, "FPSCR %08lx\n", (unsigned long)fpscr);
    fflush(f);
    probe_cores(f, prio);
    fflush(f);
    probe_vertices(f, s);
    fflush(f);
    probe_memory(f);
    fprintf(f, "hwprobe total %.0f ms\n", ns_since(t0) / 1e6);
    fclose(f);
}
