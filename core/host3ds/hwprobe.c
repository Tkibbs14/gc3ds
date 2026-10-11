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
#include "../src/jit/jit.h"
#include "pmu.h"

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
        vfast_run(s, &gx, 0, &fold, &sel, raw, cases[c].count, 24, out, false);  // warm up
        u64 t0 = svcGetSystemTick();
        for (u32 d = 0; d < cases[c].draws; d++) vfast_run(s, &gx, 0, &fold, &sel, raw, cases[c].count, 24, out, false);
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

// ---- the JIT on synthetic guest code ----
// Small PowerPC programs run through the JIT on core 0 for a fixed number of guest instructions: ns and host cycles
// per guest instruction, with block linking on and off, and from the ARM11 counters the host instructions, I-cache
// misses and D-cache read misses per guest instruction. The straight-line programs (n x addi, then a branch back)
// are sized so their compiled code fits the 16 KB L1 I-cache, overflows it, fits the 2 MB L2, and overflows that: the
// cost of fetching compiled code from each level.
#define BENCH_CODE 0x00100000u  // physical; executed at 0x80000000 | BENCH_CODE
#define BENCH_DATA 0x00300000u
static u32 g_at;
static void put(System* s, u32 w) { mem1_write32(s, BENCH_CODE + 4 * g_at++, w); }
static u32 here(void) { return 0x80000000u | (BENCH_CODE + 4 * g_at); }
static u32 i_addi(u32 d, u32 a, s32 v) { return (14u << 26) | (d << 21) | (a << 16) | ((u32)v & 0xFFFF); }
static u32 i_x(u32 xo, u32 d, u32 a, u32 b) { return (31u << 26) | (d << 21) | (a << 16) | (b << 11) | (xo << 1); }
static u32 i_b(u32 from, u32 to, bool link) { return (18u << 26) | ((to - from) & 0x03FFFFFCu) | (link ? 1u : 0u); }

static void build_alu(System* s, u32 n) {
    (void)n;
    u32 top = here();
    put(s, i_addi(3, 3, 1));
    put(s, i_addi(4, 4, 3));
    put(s, (31u << 26) | (3u << 21) | (5u << 16) | (4u << 11) | (316u << 1));  // xor r5, r3, r4
    put(s, i_x(266, 6, 5, 3));                                               // add r6, r5, r3
    put(s, i_b(here(), top, false));
}
static void build_mem(System* s, u32 n) {  // r7 = data; a read-modify-write walk over 4 KB
    (void)n;
    u32 top = here();
    put(s, (32u << 26) | (5u << 21) | (4u << 16));                      // lwz r5, 0(r4)
    put(s, i_addi(5, 5, 1));
    put(s, (36u << 26) | (5u << 21) | (4u << 16));                      // stw r5, 0(r4)
    put(s, i_addi(6, 6, 4));
    put(s, (21u << 26) | (6u << 21) | (6u << 16) | (20u << 6) | (29u << 1));  // rlwinm r6, r6, 0, 20, 29
    put(s, i_x(266, 4, 7, 6));                                          // add r4, r7, r6
    put(s, i_b(here(), top, false));
}
static void build_call(System* s, u32 n) {  // bl / blr: the return goes through the indirect-branch table
    (void)n;
    u32 top = here();
    u32 at = here();
    put(s, 0);  // bl f, patched below
    put(s, i_addi(3, 3, 1));
    put(s, i_b(here(), top, false));
    u32 f = here();
    put(s, i_addi(4, 4, 1));
    put(s, 0x4E800020u);  // blr
    mem1_write32(s, at & 0x01FFFFFFu, i_b(at, f, true));
}
static void build_cmp(System* s, u32 n) {  // compare + conditional branch (fused), never taken
    (void)n;
    u32 top = here();
    put(s, i_addi(3, 3, 1));
    put(s, (11u << 26) | (3u << 16) | 0x8000u);  // cmpwi r3, -32768
    u32 at = here();
    put(s, 0);  // beq top (never: r3 counts up from 0)
    put(s, i_addi(4, 4, 1));
    put(s, i_b(here(), top, false));
    mem1_write32(s, at & 0x01FFFFFFu, (16u << 26) | (12u << 21) | (2u << 16) | ((top - at) & 0xFFFC));
}
static void build_straight(System* s, u32 n) {
    u32 top = here();
    for (u32 i = 0; i < n; i++) put(s, i_addi(3, 3, 1));
    put(s, i_b(here(), top, false));
}

typedef struct { u64 ticks, cyc, inst, icm, dcm; } BenchOut;

// Runs `instrs` guest instructions from the bench code (after a warm-up pass that compiles it) and measures it twice,
// once per counter pair.
static void bench_run(System* s, Jit* j, void (*build)(System*, u32), u32 n, u32 instrs, BenchOut* o) {
    Gekko* g = &s->cpu;
    sys_reset(s);
    g_at = 0;
    build(s, n);
    gekko_set_spr(g, SPR_IBAT0U, 0x80001FFF); gekko_set_spr(g, SPR_IBAT0U + 1, 0x00000002);
    gekko_set_spr(g, SPR_DBAT0U, 0x80001FFF); gekko_set_spr(g, SPR_DBAT0U + 1, 0x00000002);
    gekko_set_spr(g, SPR_DBAT0U + 2, 0xC0001FFF); gekko_set_spr(g, SPR_DBAT0U + 3, 0x0000002A);
    gekko_set_spr(g, SPR_HID2, 0xA0000000u);
    gekko_set_msr(g, (1u << 13) | (1u << 5) | (1u << 4) | (1u << 1));  // FP, IR, DR, RI; EE off
    g->pc = 0x80000000u | BENCH_CODE;
    g->gpr[7] = 0x80000000u | BENCH_DATA;
    g->gpr[4] = g->gpr[7];
    jit_invalidate_all(j);
    gekko_run(g, n + 64);  // compiles everything
    unsigned long long cyc, a, b;
    pmu_select(0);
    u64 t0 = svcGetSystemTick();
    gekko_run(g, instrs);
    o->ticks = svcGetSystemTick() - t0;
    pmu_read(&cyc, &a, &b);
    o->cyc = cyc; o->inst = a;
    pmu_select(1);
    gekko_run(g, instrs);
    pmu_read(&cyc, &a, &b);
    o->icm = a; o->dcm = b;
    pmu_select(0);
}

static void probe_jit(FILE* f, System* s, Jit* j) {
    if (!j) { fprintf(f, "jit: not available\n"); return; }
    static const struct { const char* name; void (*build)(System*, u32); u32 n; } kB[] = {
        { "alu", build_alu, 0 }, { "mem", build_mem, 0 }, { "call", build_call, 0 }, { "cmp", build_cmp, 0 },
        { "line1k", build_straight, 1024 }, { "line8k", build_straight, 8192 }, { "line64k", build_straight, 65536 },
        { "line256k", build_straight, 262144 },
    };
    jit_attach(j);
    fprintf(f, "jit benchmarks (ns / host cycles / host instructions / I-cache misses / D-cache read misses, per guest "
               "instruction; linking on, then off):\n");
    for (u32 k = 0; k < sizeof kB / sizeof kB[0]; k++) {
        u32 instrs = 1000000;
        if (kB[k].n > instrs / 2) instrs = 2 * kB[k].n;
        BenchOut on, off = { 0, 0, 0, 0, 0 };
        jit_set_linking(j, true);
        bench_run(s, j, kB[k].build, kB[k].n, instrs, &on);
        if (kB[k].n <= 1024) {  // unlinked: the small programs only (each block compile syncs the I-cache)
            jit_set_linking(j, false);
            bench_run(s, j, kB[k].build, kB[k].n, instrs, &off);
        }
        double ns = 1e9 / SYSCLOCK_ARM11 / instrs;
        fprintf(f, "  %-9s on %6.1f ns %6.1f cyc %5.2f ins %6.3f ic %6.3f dc | off %6.1f ns %6.1f cyc %5.2f ins %6.3f ic\n",
                kB[k].name, on.ticks * ns, (double)on.cyc / instrs, (double)on.inst / instrs, (double)on.icm / instrs,
                (double)on.dcm / instrs, off.ticks * ns, (double)off.cyc / instrs, (double)off.inst / instrs,
                (double)off.icm / instrs);
        fflush(f);
    }
    jit_set_linking(j, true);
    jit_invalidate_all(j);
    gekko_set_jit(&s->cpu, NULL, NULL);
    sys_reset(s);
}

void hwprobe_run(System* s, Jit* j) {
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
    fflush(f);
    pmu_probe(f);
    fflush(f);
    probe_jit(f, s, j);
    fprintf(f, "hwprobe total %.0f ms\n", ns_since(t0) / 1e6);
    fclose(f);
}
