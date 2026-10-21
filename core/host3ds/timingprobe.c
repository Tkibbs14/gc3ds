// Timing probe (--timing-probe on): micro-benchmarks that each isolate one mechanism of the 2DS's speed, measured with
// core 0's own performance counters, for the timing model (docs/research/TIMING_MODEL_PLAN.md: calibrate a model of
// the 2DS against the device, mechanism by mechanism). Run at the start of a relay job, before the machine boots, when
// MEM1 (24 MB) and the JIT's code buffer (16 MB) are free scratch; the JIT buffer is emptied again afterwards and MEM1
// is overwritten by the boot. Results go to perf.csv as "# timing" lines, per operation:
//   cyc, and the ten counter events in pmu.c's five pairs (each benchmark runs once per pair, ~25 ms each).
// Benchmarks:
//   icode SIZE    straight-line independent adds, SIZE bytes of code in a loop (I-cache, L2, memory instruction fetch)
//   chase SIZE    dependent loads in a random cycle of SIZE bytes, one per 32-byte line (L1 / L2 / memory latency)
//   tlb PAGES     dependent loads, one per 4 KB page in random order, lines spread over the sets (main TLB reach)
//   dep / ind     dependent and independent chains of one instruction kind (latency and throughput)
//   branch        a conditional branch on a pseudo-random bit vs a predictable one (mispredict penalty)
//   stream        sequential stores over 4 MB (write-miss / write-buffer cost)
//   svc           system calls: the tick, and the JIT's cache sync (flush + svc 0x94)
//   xcore         chase 1 MB / 8 MB on core 0 while core 2 streams stores over 4 MB (shared L2 / bus)
// Added from docs/research/BENCHMARK_TOOLING_SURVEY.md (2026-10-15):
//   indirect      calls through a table from one site: 1 target, 2 / 4 / 16 round-robin, 16 random
//   calls DEPTH   chains of distinct functions (return stack depth)
//   btac N        N taken branches 8 bytes apart in generated code (branch target cache capacity)
//   install SIZE  write + clean + whole I-cache invalidate + first run of SIZE bytes of code (the JIT's install)
//   op forms      ldrh / ldrb then use, ldrd / strd / ldm / stm, and ldr + rev as a dependent chain (guest BE loads)
//   assoc S xK    K lines S bytes apart in a random cycle (L1 / L2 ways)
//   seqchase      sequential dependent loads by stride over 1 MB and 8 MB (line size, hardware prefetch)
//   mlp 8M        1-6 independent chases through memory at once (misses in flight)
//   store / load  one per 32-byte line over 8 KB / 512 KB / 8 MB (write-allocate); store then load; store throughput
#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pmu.h"
#include "l2lock.h"

bool g_l2lockUnsafe;  // refuse subsequent jobs after a failed restoration; only a restart clears this

void jit_host_sync_icache(void* start, u32 bytes);

static char* g_out;
static size_t g_outLen, g_outCap;
// probe_live: every line also goes here as it is measured (a relay job's log), so a crash or a hang mid-probe still
// leaves what came before it, and the last "section" line names where it stopped.
FILE* probe_live;
static void out(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    size_t at = g_outLen;
    int n = vsnprintf(g_out + g_outLen, g_outCap - g_outLen, fmt, ap);
    va_end(ap);
    if (n > 0) g_outLen += (size_t)n < g_outCap - g_outLen ? (size_t)n : g_outCap - g_outLen - 1;
    if (probe_live && g_outLen > at) {
        fputs(g_out + at, probe_live);
        fflush(probe_live);
    }
}
#define SECTION(name) out("# timing: section %s\n", name)

typedef void (*Kernel)(void* ctx, u32 n);
static volatile u32 g_sink;

// Without the performance counters (an emulator such as Azahar refuses them), cycles come from the system tick at the
// ARM11's clock and the events read zero: every benchmark still runs, so the probe itself is tested off the device.
static bool g_pmuOk, g_disagreed;
static unsigned g_badRun;
static u64 g_tick0;
static void sel(unsigned p) {
    if (g_pmuOk) pmu_select(p);
    g_tick0 = svcGetSystemTick();
}
static void rd(unsigned long long* cyc, unsigned long long* e0, unsigned long long* e1) {
    const unsigned long long byTick = (svcGetSystemTick() - g_tick0) * 804 / 268;  // ticks at 268 MHz -> cycles at 804 MHz
    // The counters are trusted only while their cycle count agrees with the tick within 2x (Azahar accepts the system
    // call and leaves junk; a misconfigured counter on the device would show the same way).
    // Only long measurements are compared: on the device the first, tiny ones read 2,870 counter cycles against 690 by
    // the tick (job 176), where the tick's granularity and the system calls around it dominate.
    // A disagreement is that run's alone (job 181: the code-install case, around the whole-I-cache invalidate, read
    // 0.98 M counter cycles against 2.1 M by the tick, and the counters were dropped for the rest of the probe);
    // measure() drops them only after three measurements in a row disagreed.
    if (g_pmuOk && pmu_read(cyc, e0, e1) && *cyc && (byTick < 2000000 || (*cyc >= byTick / 2 && *cyc <= byTick * 2))) return;
    if (g_pmuOk && !g_disagreed) out("# timing: counters disagree with the tick (%llu cycles vs %llu): runs by the tick, events zero\n", *cyc, byTick);
    g_disagreed = true;
    *cyc = byTick;
    *e0 = *e1 = 0;
}

// Runs k for about 20 M cycles per counter pair and writes one line: cycles and the ten events per operation (ops:
// operations per unit of n).
static void measure(const char* name, const char* param, Kernel k, void* ctx, double opsPerUnit) {
    unsigned long long cyc, e0, e1;
    u32 n = 1;
    g_disagreed = false;
    for (;;) {  // size n to ~20 M cycles (also warms the caches and TLB)
        sel(0);
        k(ctx, n);
        rd(&cyc, &e0, &e1);
        if (cyc >= 2000000 || n >= (1u << 28)) break;
        n *= cyc < 200000 ? 16 : 4;
    }
    n = (u32)((double)n * 20e6 / (double)(cyc ? cyc : 1));
    if (n < 1) n = 1;
    // All nine PMU pairs (pmu.c): the last four (unpredicted branches, software PC changes, D-cache reads, folded
    // branches, micro-TLB misses, external requests, store-buffer drains) say what those events count in known loops.
    double c[9], ev[9][2];
    for (unsigned p = 0; p < 9; p++) {
        sel(p);
        k(ctx, n);
        rd(&cyc, &e0, &e1);
        double ops = opsPerUnit * n;
        c[p] = (double)cyc / ops;
        ev[p][0] = (double)e0 / ops;
        ev[p][1] = (double)e1 / ops;
    }
    double cmin = c[0];
    for (int p = 1; p < 9; p++) if (c[p] < cmin) cmin = c[p];
    out("# timing %s %s: cyc %.3f (max %.3f) inst %.3f brmiss %.4f icmiss %.4f dcrmiss %.4f stall_inst %.3f stall_data %.3f "
        "dcwmiss %.4f tlbmiss %.4f branch %.3f stall_lsu %.3f brnopred %.3f pcchg %.3f dcread %.3f folded %.3f imtlb %.4f "
        "dmtlb %.4f extreq %.4f sbdrain %.4f\n", name, param, cmin,
        c[0] > c[1] ? (c[0] > c[2] ? c[0] : c[2]) : (c[1] > c[2] ? c[1] : c[2]), ev[0][0], ev[0][1], ev[1][0], ev[1][1],
        ev[2][0], ev[2][1], ev[3][0], ev[3][1], ev[4][0], ev[4][1], ev[5][0], ev[5][1], ev[6][0], ev[6][1], ev[7][0],
        ev[7][1], ev[8][0], ev[8][1]);
    g_badRun = g_disagreed ? g_badRun + 1 : 0;
    if (g_pmuOk && g_badRun >= 3) {
        out("# timing: three measurements in a row disagreed: the tick from here on\n");
        g_pmuOk = false;
    }
}

// ---- instruction fetch ----
typedef struct { void (*fn)(void); } Code;
static void k_icode(void* ctx, u32 n) {
    void (*fn)(void) = ((Code*)ctx)->fn;
    while (n--) fn();
}
// SIZE bytes of "add rX, rX, #1" over r0-r3 (no dependence between neighbours), then "bx lr".
static void icode(u32* buf, u32 bufWords) {
    static const u32 kSizes[] = {1u << 10, 2u << 10, 4u << 10, 8u << 10, 12u << 10, 16u << 10, 20u << 10, 24u << 10,
                                 32u << 10, 64u << 10, 128u << 10, 256u << 10, 512u << 10, 1u << 20, 2u << 20, 3u << 20,
                                 4u << 20, 8u << 20};
    for (unsigned s = 0; s < sizeof kSizes / sizeof kSizes[0]; s++) {
        u32 words = kSizes[s] / 4;
        if (words + 1 > bufWords) break;
        for (u32 i = 0; i < words - 1; i++) {
            u32 r = i & 3;
            buf[i] = 0xE2800001u | (r << 16) | (r << 12);  // add rR, rR, #1
        }
        buf[words - 1] = 0xE12FFF1Eu;  // bx lr
        jit_host_sync_icache(buf, words * 4);
        Code c = {(void (*)(void))(void*)buf};
        char p[16];
        snprintf(p, sizeof p, "%luK", (unsigned long)(kSizes[s] >> 10));
        measure("icode", p, k_icode, &c, (double)words);
    }
}

// ---- data latency ----
static void k_chase(void* ctx, u32 n) {
    void** p = *(void***)ctx;
    while (n--) {
        p = (void**)*p; p = (void**)*p; p = (void**)*p; p = (void**)*p;
        p = (void**)*p; p = (void**)*p; p = (void**)*p; p = (void**)*p;
    }
    *(void***)ctx = p;
    g_sink = (u32)(uintptr_t)p;  // observable: inlined through a constant function pointer, the chain was dropped as dead
}
static u32 g_lcg = 12345;
static u32 rnd(void) { return g_lcg = g_lcg * 1664525u + 1013904223u; }
// A random cyclic order over `count` slots; slot i's address is base + addr(i). Each slot points at the next.
static void** build_cycle(u8* base, u32 count, u32 (*addr)(u32), u32* order) {
    for (u32 i = 0; i < count; i++) order[i] = i;
    for (u32 i = count - 1; i > 0; i--) {
        u32 j = rnd() % (i + 1), t = order[i];
        order[i] = order[j];
        order[j] = t;
    }
    for (u32 i = 0; i < count; i++) *(void**)(base + addr(order[i])) = base + addr(order[(i + 1) % count]);
    return (void**)(base + addr(order[0]));
}
static u32 line_addr(u32 i) { return i * 32; }
static u32 page_addr(u32 i) { return i * 4096 + ((i * 97) & 127) * 32; }  // one line per page, spread over the sets

static void chase(u8* mem, u32 memBytes, u32* order) {
    static const u32 kSizes[] = {4u << 10, 8u << 10, 12u << 10, 16u << 10, 24u << 10, 32u << 10, 64u << 10,
                                 128u << 10, 256u << 10, 512u << 10, 1u << 20, 1536u << 10, 2u << 20, 3u << 20,
                                 4u << 20, 8u << 20, 16u << 20};
    for (unsigned s = 0; s < sizeof kSizes / sizeof kSizes[0]; s++) {
        if (kSizes[s] > memBytes) break;
        void** start = build_cycle(mem, kSizes[s] / 32, line_addr, order);
        char p[16];
        snprintf(p, sizeof p, "%luK", (unsigned long)(kSizes[s] >> 10));
        measure("chase", p, k_chase, &start, 8);
    }
}
static void tlb(u8* mem, u32 memBytes, u32* order) {
    static const u32 kPages[] = {8, 16, 32, 48, 64, 96, 128, 256, 512, 1024, 4096};
    for (unsigned s = 0; s < sizeof kPages / sizeof kPages[0]; s++) {
        if (kPages[s] * 4096 > memBytes) break;
        void** start = build_cycle(mem, kPages[s], page_addr, order);
        char p[16];
        snprintf(p, sizeof p, "%lu", (unsigned long)kPages[s]);
        measure("tlb", p, k_chase, &start, 8);
    }
}

// ---- instruction latency and throughput: 16 per loop round ----
#define R4(x) x x x x
#define R16(x) R4(x) R4(x) R4(x) R4(x)
static u32 g_word[64] __attribute__((aligned(32)));
static void k_mul_dep(void* c, u32 n) { u32 a = 3, b = 5; while (n--) __asm__ volatile(R16("mul %0, %0, %1\n") : "+r"(a) : "r"(b)); g_sink = a; }
static void k_mul_ind(void* c, u32 n) {
    u32 a = 3, b = 5, d = 7, e = 9;
    while (n--) __asm__ volatile(R4("mul %0, %4, %4\n mul %1, %4, %4\n mul %2, %4, %4\n mul %3, %4, %4\n") : "+r"(a), "+r"(d), "+r"(e), "+r"(b) : "r"(n));
    g_sink = a + d + e + b;
}
static void k_add_dep(void* c, u32 n) { u32 a = 0; while (n--) __asm__ volatile(R16("add %0, %0, #1\n") : "+r"(a)); g_sink = a; }
static void k_rev_dep(void* c, u32 n) { u32 a = 0x12345678; while (n--) __asm__ volatile(R16("rev %0, %0\n") : "+r"(a)); g_sink = a; }
static void k_ldr_dep(void* c, u32 n) {  // a load whose address is the previous load's result (L1 hit latency)
    void* p = g_word;
    g_word[0] = (u32)(uintptr_t)g_word;
    while (n--) __asm__ volatile(R16("ldr %0, [%0]\n") : "+r"(p));
    g_sink = (u32)(uintptr_t)p;
}
static void k_ldr_use(void* c, u32 n) {  // a load, then its value used by the next instruction
    u32 a = 0, t;
    while (n--) __asm__ volatile(R16("ldr %1, [%2]\n add %0, %0, %1\n") : "+r"(a), "=&r"(t) : "r"(g_word));
    g_sink = a;
}
static void k_ldr_gap(void* c, u32 n) {  // the same with one independent instruction between
    u32 a = 0, b = 0, t;
    while (n--) __asm__ volatile(R16("ldr %2, [%3]\n add %1, %1, #1\n add %0, %0, %2\n") : "+r"(a), "+r"(b), "=&r"(t) : "r"(g_word));
    g_sink = a + b;
}
static void k_ldr_ind(void* c, u32 n) {  // independent loads (throughput)
    u32 a, b, d, e;
    while (n--) __asm__ volatile(R4("ldr %0, [%4]\n ldr %1, [%4, #4]\n ldr %2, [%4, #8]\n ldr %3, [%4, #12]\n") : "=&r"(a), "=&r"(b), "=&r"(d), "=&r"(e) : "r"(g_word));
    g_sink = a + b + d + e;
}
static void k_vadd_s_dep(void* c, u32 n) { while (n--) __asm__ volatile(R16("vadd.f32 s0, s0, s1\n") ::: "s0"); }
static void k_vmul_s_dep(void* c, u32 n) { while (n--) __asm__ volatile(R16("vmul.f32 s0, s0, s1\n") ::: "s0"); }
static void k_vadd_d_dep(void* c, u32 n) { while (n--) __asm__ volatile(R16("vadd.f64 d0, d0, d1\n") ::: "d0"); }
static void k_vmul_d_dep(void* c, u32 n) { while (n--) __asm__ volatile(R16("vmul.f64 d0, d0, d1\n") ::: "d0"); }
static void k_vadd_s_ind(void* c, u32 n) {
    while (n--) __asm__ volatile(R4("vadd.f32 s0, s4, s5\n vadd.f32 s1, s4, s5\n vadd.f32 s2, s4, s5\n vadd.f32 s3, s4, s5\n") ::: "s0", "s1", "s2", "s3");
}
static void k_vdiv_s(void* c, u32 n) { while (n--) __asm__ volatile(R16("vdiv.f32 s0, s0, s1\n") ::: "s0"); }
static void k_vsqrt_s(void* c, u32 n) { while (n--) __asm__ volatile(R16("vsqrt.f32 s0, s0\n") ::: "s0"); }
static void k_vmov_rt(void* c, u32 n) {  // core -> VFP -> core
    u32 a = 1;
    while (n--) __asm__ volatile(R16("vmov s0, %0\n vmov %0, s0\n") : "+r"(a) :: "s0");
    g_sink = a;
}
static void k_vldr_s(void* c, u32 n) { while (n--) __asm__ volatile(R16("vldr s0, [%0]\n vadd.f32 s1, s1, s0\n") :: "r"(g_word) : "s0", "s1"); }

static void pipeline(void) {
    static const float kOne __attribute__((aligned(8))) = 1.0f;
    static const double kOneD __attribute__((aligned(8))) = 1.0;
    __asm__ volatile("vldr s0, [%0]\n vldr s1, [%0]\n vldr s4, [%0]\n vldr s5, [%0]\n vldr d1, [%1]"  // VFPv2: no vmov immediates
                     :: "r"(&kOne), "r"(&kOneD) : "s0", "s1", "s4", "s5", "d1");
    static const struct { const char* name; Kernel k; } kK[] = {
        {"add_dep", k_add_dep},      {"mul_dep", k_mul_dep},     {"mul_ind", k_mul_ind},     {"rev_dep", k_rev_dep},
        {"ldr_dep", k_ldr_dep},      {"ldr_use", k_ldr_use},     {"ldr_gap", k_ldr_gap},     {"ldr_ind", k_ldr_ind},
        {"vadd_s_dep", k_vadd_s_dep}, {"vmul_s_dep", k_vmul_s_dep}, {"vadd_d_dep", k_vadd_d_dep}, {"vmul_d_dep", k_vmul_d_dep},
        {"vadd_s_ind", k_vadd_s_ind}, {"vdiv_s", k_vdiv_s},       {"vsqrt_s", k_vsqrt_s},     {"vmov_rt", k_vmov_rt},
        {"vldr_use", k_vldr_s},
    };
    for (unsigned i = 0; i < sizeof kK / sizeof kK[0]; i++) measure("op", kK[i].name, kK[i].k, NULL, 16);
}

// ---- branches: 16 conditional branches per round, on bits of an LFSR (random) or of a constant (predictable) ----
static void k_branch(void* ctx, u32 n) {
    u32 s = *(u32*)ctx, acc = 0;
    const bool random = s != 0;  // 0: bit 0 stays clear, the branch is always taken
    while (n--) {
        __asm__ volatile(R16("tst %1, #1\n addne %0, %0, #3\n beq 1f\n add %0, %0, #1\n1:\n mov %1, %1, ror #1\n")
                         : "+r"(acc), "+r"(s));
        if (random) s = s * 1664525u + 1013904223u;
    }
    g_sink = acc;
}
static void branches(void) {
    u32 pred = 0, rand = 0x1234567u;
    measure("branch", "predictable", k_branch, &pred, 16);
    measure("branch", "random", k_branch, &rand, 16);
}

// ---- stores ----
typedef struct { u8* mem; u32 bytes; } Buf;
static void k_stream(void* ctx, u32 n) {
    Buf* b = ctx;
    while (n--)
        for (u32* p = (u32*)b->mem, *e = (u32*)(b->mem + b->bytes); p < e; p += 8) {
            p[0] = n; p[1] = n; p[2] = n; p[3] = n; p[4] = n; p[5] = n; p[6] = n; p[7] = n;
        }
}

// ---- system calls ----
static void k_tick(void* c, u32 n) { u64 t = 0; while (n--) t += svcGetSystemTick(); g_sink = (u32)t; }
static void k_sync(void* c, u32 n) { while (n--) jit_host_sync_icache(g_word, 4); }

// ---- another core's traffic ----
static volatile u32 g_stop;
static u8* g_streamMem;
static volatile s32 g_streamCore;
static void streamer(void* arg) {
    u32 bytes = (u32)(uintptr_t)arg, v = 0;
    g_streamCore = svcGetProcessorID();
    while (!g_stop) {
        for (u32* p = (u32*)g_streamMem, *e = (u32*)(g_streamMem + bytes); p < e && !g_stop; p += 8) p[0] = p[4] = v++;
    }
}
static void xcore(u8* mem, u32 memBytes, u32* order) {
    if (memBytes < (12u << 20)) return;
    u8* chaseMem = mem;
    g_streamMem = mem + (8u << 20);  // the other core's 4 MB, after the chase's 8 MB
    static const u32 kSizes[] = {256u << 10, 1u << 20, 8u << 20};
    for (unsigned s = 0; s < 3; s++) {
        void** start = build_cycle(chaseMem, kSizes[s] / 32, line_addr, order);
        char p[32];
        snprintf(p, sizeof p, "%luK alone", (unsigned long)(kSizes[s] >> 10));
        measure("xcore", p, k_chase, &start, 8);
        g_stop = 0;
        Thread t = threadCreate(streamer, (void*)(uintptr_t)(4u << 20), 4096, 0x30, 2, false);
        if (!t) { out("# timing xcore: no thread on core 2\n"); return; }
        svcSleepThread(2000000);
        snprintf(p, sizeof p, "%luK core2-streams", (unsigned long)(kSizes[s] >> 10));
        measure("xcore", p, k_chase, &start, 8);
        g_stop = 1;
        threadJoin(t, U64_MAX);
        threadFree(t);
    }
}

// ---- memory bandwidth from the CPU (memcpy / memset / sequential reads in FCRAM; writes and reads of VRAM) ----
static double mbps(u64 ticks, u32 bytes) { return (double)bytes / 1048576.0 / ((double)ticks / SYSCLOCK_ARM11); }
static u64 best_of(int n, void (*fn)(void*), void* ctx) {
    u64 best = ~0ull;
    for (int i = 0; i < n; i++) {
        u64 t = svcGetSystemTick();
        fn(ctx);
        t = svcGetSystemTick() - t;
        if (t < best) best = t;
    }
    return best;
}
typedef struct { u8* a; u8* b; u32 bytes; } Copy;
static void f_memcpy(void* c) { Copy* x = c; memcpy(x->b, x->a, x->bytes); }
static void f_memset(void* c) { Copy* x = c; memset(x->a, (int)g_sink, x->bytes); }
static void f_read(void* c) {
    Copy* x = c;
    u32 s = 0;
    for (const u32 *p = (const u32*)x->a, *e = (const u32*)(x->a + x->bytes); p < e; p += 8) s += p[0] + p[4];
    g_sink = s;
}
static void bandwidth(u8* mem, u32 memBytes) {
    const u32 kB = 4u << 20;
    if (mem && memBytes >= 2 * kB) {
        Copy c = {mem, mem + kB, kB};
        SECTION("bandwidth memcpy");
        out("# timing bw memcpy 4 MB FCRAM: %.0f MB/s\n", mbps(best_of(4, f_memcpy, &c), kB));
        SECTION("bandwidth memset");
        out("# timing bw memset 4 MB FCRAM: %.0f MB/s\n", mbps(best_of(4, f_memset, &c), kB));
        SECTION("bandwidth read");
        out("# timing bw read 4 MB FCRAM (2 words per line): %.0f MB/s\n", mbps(best_of(4, f_read, &c), kB));
    }
    // CPU access to VRAM is left out: job 176 stopped in this section on the device (where exactly is open; the
    // VRAM part, the one new kind of access, is the suspect). The GPU probe measures VRAM through the GPU.
}

// ---- kernel: thread switches through a LightEvent ping-pong, on the same core and across cores ----
static LightEvent g_ping, g_pong;
static volatile u32 g_pongStop;
static void ponger(void* arg) {
    (void)arg;
    for (;;) {
        LightEvent_Wait(&g_ping);
        if (g_pongStop) break;
        LightEvent_Signal(&g_pong);
    }
}
static void kernel_switch(void) {
    static const struct { int core; const char* name; } kC[] = {{0, "same core (0)"}, {2, "core 0 <-> core 2"}};
    for (int k = 0; k < 2; k++) {
        out("# timing: section kernel_switch %s\n", kC[k].name);
        LightEvent_Init(&g_ping, RESET_ONESHOT);
        LightEvent_Init(&g_pong, RESET_ONESHOT);
        g_pongStop = 0;
        s32 prio = 0x30;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        Thread t = threadCreate(ponger, NULL, 4096, prio, kC[k].core, false);
        if (!t) {
            out("# timing kernel switch %s: no thread\n", kC[k].name);
            continue;
        }
        const int kN = 2000;
        u64 t0 = svcGetSystemTick();
        for (int i = 0; i < kN; i++) {
            LightEvent_Signal(&g_ping);
            LightEvent_Wait(&g_pong);
        }
        u64 dt = svcGetSystemTick() - t0;
        g_pongStop = 1;
        LightEvent_Signal(&g_ping);
        threadJoin(t, U64_MAX);
        threadFree(t);
        out("# timing kernel switch %s: %.2f us per round trip (signal, wake, signal back, wake)\n", kC[k].name,
            (double)dt * 1e6 / SYSCLOCK_ARM11 / kN);
    }
    SECTION("kernel_switch yield");
    u64 best = ~0ull;
    for (int i = 0; i < 2000; i++) {
        u64 t = svcGetSystemTick();
        svcSleepThread(0);
        t = svcGetSystemTick() - t;
        if (t < best) best = t;
    }
    out("# timing kernel svcSleepThread(0) (a yield): %.2f us\n", (double)best * 1e6 / SYSCLOCK_ARM11);
}

// ---- storage: reads of the disc image from the SD card ----
static void storage(const char* path) {
    if (!path || !path[0]) return;
    SECTION("storage open");
    FILE* f = fopen(path, "rb");
    if (!f) {
        out("# timing sd: cannot open %s\n", path);
        return;
    }
    setvbuf(f, NULL, _IONBF, 0);
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    static u8 buf[64 << 10];
    if (size > (64 << 20)) {
        // Sequential: 8 MB in 64 KB reads, from a point not read before in this probe.
        long at = (size / 2) & ~0xFFFFl;
        fseek(f, at, SEEK_SET);
        u64 t = svcGetSystemTick();
        for (int i = 0; i < 128; i++) fread(buf, 1, sizeof buf, f);
        t = svcGetSystemTick() - t;
        out("# timing sd sequential 64 KB reads: %.2f MB/s\n", mbps(t, 128u * sizeof buf));
        static const struct { u32 bytes; int n; } kR[] = {{32u << 10, 64}, {2u << 10, 256}};
        for (int k = 0; k < 2; k++) {
            u64 sum = 0;
            for (int i = 0; i < kR[k].n; i++) {
                long off = (long)(((u64)rnd() * (u64)(size - (64 << 10))) >> 32) & ~0x7FFl;
                fseek(f, off, SEEK_SET);
                u64 t1 = svcGetSystemTick();
                fread(buf, 1, kR[k].bytes, f);
                sum += svcGetSystemTick() - t1;
            }
            out("# timing sd random %lu KB reads: %.3f ms each\n", (unsigned long)(kR[k].bytes >> 10),
                (double)sum * 1000.0 / SYSCLOCK_ARM11 / kR[k].n);
        }
    }
    fclose(f);
}

// ---- clocks: the same work at 268 MHz (osSetSpeedupEnable(false): on the New 3DS that also turns the L2 off) ----
static double ns_per_op(Kernel k, void* ctx, u32 n, double ops) {
    u64 best = ~0ull;
    for (int i = 0; i < 3; i++) {
        u64 t = svcGetSystemTick();
        k(ctx, n);
        t = svcGetSystemTick() - t;
        if (t < best) best = t;
    }
    return (double)best * 1e9 / SYSCLOCK_ARM11 / (ops * n);
}
static void clocks(u8* mem, u32* order) {
    if (!mem) return;
    void** start = build_cycle(mem, (1u << 20) / 32, line_addr, order);
    for (int pass = 0; pass < 2; pass++) {
        out("# timing: section clocks pass %d\n", pass);
        bool fast = pass == 0;
        osSetSpeedupEnable(fast);
        svcSleepThread(10000000);  // let the clock change settle
        double add = ns_per_op(k_add_dep, NULL, 200000, 16), ch = ns_per_op(k_chase, &start, 100000, 8);
        out("# timing clock %s: add_dep %.3f ns (%.0f MHz), chase 1024K %.1f ns per load\n",
            fast ? "speedup on (804 MHz, L2 on)" : "speedup off (268 MHz, L2 off)", add, 1e3 / add, ch);
    }
    osSetSpeedupEnable(true);
}

// Runs every benchmark; `code` / `mem` are scratch (the JIT's buffer past its stubs, MEM1); `disc` the image the
// storage reads use. Returns the text to put in perf.csv (valid until the next call).
// ---- more CPU probes (docs/research/BENCHMARK_TOOLING_SURVEY.md section 5, added 2026-10-15) ----

// Code install as the JIT does it: write SIZE bytes of code, clean the data cache over it, invalidate the whole
// I-cache, run it once. "run only" is the same code warm; the differences are the sync and the cold first run.
typedef struct { u32* buf; const u32* tmpl; u32 words; bool write; } Inst;
static void k_install(void* ctx, u32 n) {
    Inst* x = ctx;
    void (*fn)(void) = (void (*)(void))(void*)x->buf;
    while (n--) {
        if (x->write) memcpy(x->buf, x->tmpl, x->words * 4);
        jit_host_sync_icache(x->buf, x->words * 4);
        fn();
    }
}
static void install(u32* code, u32 codeWords) {
    static const u32 kBytes[] = {64, 256, 1024, 4096, 16384, 65536};
    const u32 maxWords = 65536 / 4;
    if (codeWords < 2 * maxWords) return;
    u32* tmpl = code + maxWords;
    for (unsigned s = 0; s < sizeof kBytes / sizeof kBytes[0]; s++) {
        u32 words = kBytes[s] / 4;
        for (u32 i = 0; i < words - 1; i++) {
            u32 r = i & 3;
            tmpl[i] = 0xE2800001u | (r << 16) | (r << 12);  // add rR, rR, #1
        }
        tmpl[words - 1] = 0xE12FFF1Eu;  // bx lr
        memcpy(code, tmpl, words * 4);
        jit_host_sync_icache(code, words * 4);
        Code c = {(void (*)(void))(void*)code};
        Inst a = {code, tmpl, words, false}, b = {code, tmpl, words, true};
        char p[40];
        snprintf(p, sizeof p, "%luB run only", (unsigned long)kBytes[s]);
        measure("install", p, k_icode, &c, 1);
        snprintf(p, sizeof p, "%luB sync+run", (unsigned long)kBytes[s]);
        measure("install", p, k_install, &a, 1);
        snprintf(p, sizeof p, "%luB write+sync+run", (unsigned long)kBytes[s]);
        measure("install", p, k_install, &b, 1);
    }
}

// Indirect calls from one call site through a table of 16 distinct functions, by a 256-entry target pattern.
#define TFN(i) static void __attribute__((noinline, noclone)) t##i(void) { g_sink += i; }
TFN(0) TFN(1) TFN(2) TFN(3) TFN(4) TFN(5) TFN(6) TFN(7) TFN(8) TFN(9) TFN(10) TFN(11) TFN(12) TFN(13) TFN(14) TFN(15)
static void (*const kT[16])(void) = {t0, t1, t2, t3, t4, t5, t6, t7, t8, t9, t10, t11, t12, t13, t14, t15};
static void k_indirect(void* ctx, u32 n) {
    const u8* idx = ctx;
    while (n--)
        for (u32 i = 0; i < 256; i++) kT[idx[i]]();
}
static void indirect(void) {
    static u8 idx[256];
    static const struct { const char* name; int targets; bool random; } kP[] = {
        {"1 target", 1, false}, {"2 alternating", 2, false}, {"4 round-robin", 4, false},
        {"16 round-robin", 16, false}, {"16 random", 16, true}};
    for (unsigned k = 0; k < sizeof kP / sizeof kP[0]; k++) {
        for (u32 i = 0; i < 256; i++) idx[i] = kP[k].random ? (u8)(rnd() >> 28) : (u8)(i % kP[k].targets);
        measure("indirect", kP[k].name, k_indirect, idx, 256);
    }
}

// Call chains of distinct functions, depth D: cD calls cD-1 ... c0. Per call + return; the return stack's depth shows
// as the depth where the cost per call jumps.
static u32 __attribute__((noinline, noclone)) c0(void) { return g_sink; }
#define CFN(i, j) static u32 __attribute__((noinline, noclone)) c##i(void) { u32 r = c##j(); g_sink = r; return r + i; }
CFN(1, 0) CFN(2, 1) CFN(3, 2) CFN(4, 3) CFN(5, 4) CFN(6, 5) CFN(7, 6) CFN(8, 7) CFN(9, 8) CFN(10, 9) CFN(11, 10) CFN(12, 11)
static u32 (*const kC[13])(void) = {c0, c1, c2, c3, c4, c5, c6, c7, c8, c9, c10, c11, c12};
static void k_calls(void* ctx, u32 n) {
    u32 (*fn)(void) = kC[*(u32*)ctx];
    while (n--) fn();
}
static void returns(void) {
    static const u32 kD[] = {1, 2, 3, 4, 5, 6, 8, 12};
    for (unsigned k = 0; k < sizeof kD / sizeof kD[0]; k++) {
        u32 d = kD[k];
        char p[24];
        snprintf(p, sizeof p, "depth %lu", (unsigned long)d);
        measure("calls", p, k_calls, &d, d + 1);
    }
}

// N taken unconditional branches, one per 8 bytes of generated code ("b" to the next, then a filler): the BTAC's
// capacity shows as the N where the cost per branch rises.
static void btac(u32* code, u32 codeWords) {
    static const u32 kN[] = {16, 32, 64, 96, 128, 192, 256, 512, 1024};
    for (unsigned k = 0; k < sizeof kN / sizeof kN[0]; k++) {
        u32 n = kN[k];
        if (2 * n + 2 > codeWords) break;
        for (u32 i = 0; i < n; i++) {
            code[2 * i] = 0xEA000000u;  // b +8: the next pair
            code[2 * i + 1] = 0xE1A00000u;  // mov r0, r0 (never run)
        }
        code[2 * n] = 0xE12FFF1Eu;  // bx lr
        jit_host_sync_icache(code, (2 * n + 1) * 4);
        Code c = {(void (*)(void))(void*)code};
        char p[16];
        snprintf(p, sizeof p, "%lu", (unsigned long)n);
        measure("btac", p, k_icode, &c, n);
    }
}

// Associativity: K lines all STRIDE apart in a random cycle. A cache with way size STRIDE holds as many as it has
// ways; one more and every load misses (for an LRU / round-robin replacement).
static u32 g_stride;
static u32 stride_addr(u32 i) { return i * g_stride; }
static void assoc(u8* mem, u32 memBytes, u32* order) {
    static const struct { u32 stride; u8 ks[10]; } kA[] = {
        {4u << 10, {2, 3, 4, 5, 6, 8, 16}},  // L1 D: 16 KB, 4-way would be a 4 KB way
        {2u << 10, {4, 8, 9, 10, 12, 16}},
        {64u << 10, {8, 16, 24, 32, 33, 40}},  // L2: 2 MB, 8-way (256 KB way) or 16-way (128 KB)
        {128u << 10, {4, 8, 12, 16, 17, 20, 24, 32, 33}},
        {256u << 10, {4, 8, 9, 12, 16, 17, 20, 24, 32}},
    };
    for (unsigned a = 0; a < sizeof kA / sizeof kA[0]; a++)
        for (unsigned k = 0; k < 10 && kA[a].ks[k]; k++) {
            u32 n = kA[a].ks[k];
            if (n * kA[a].stride > memBytes) break;
            g_stride = kA[a].stride;
            void** start = build_cycle(mem, n, stride_addr, order);
            char p[32];
            snprintf(p, sizeof p, "%luK x%lu", (unsigned long)(kA[a].stride >> 10), (unsigned long)n);
            measure("assoc", p, k_chase, &start, 8);
        }
}

// Sequential dependent loads STRIDE apart over FOOTPRINT (each points at the next): line size (a miss every
// line / stride loads) and hardware prefetch (a sequential miss cheaper than the random chase's).
static void seqchase(u8* mem, u32 memBytes) {
    static const u32 kFoot[] = {1u << 20, 8u << 20};
    static const u32 kStride[] = {4, 8, 16, 32, 64, 128, 256, 4096};
    for (unsigned f = 0; f < 2; f++) {
        if (kFoot[f] > memBytes) break;
        for (unsigned s = 0; s < sizeof kStride / sizeof kStride[0]; s++) {
            u32 count = kFoot[f] / kStride[s];
            for (u32 i = 0; i < count; i++) *(void**)(mem + i * kStride[s]) = mem + ((i + 1) % count) * kStride[s];
            void** start = (void**)mem;
            char p[32];
            snprintf(p, sizeof p, "%luK stride %lu", (unsigned long)(kFoot[f] >> 10), (unsigned long)kStride[s]);
            measure("seqchase", p, k_chase, &start, 8);
        }
    }
}

// Memory-level parallelism: L independent chases through one random 8 MB cycle (memory), per load.
typedef struct { void** p[6]; } Lanes;
#define LANE(i) l->p[i] = (void**)*l->p[i];
static void k_lane1(void* c, u32 n) { Lanes* l = c; while (n--) { R4(LANE(0)) } g_sink = (u32)(uintptr_t)l->p[0]; }
static void k_lane2(void* c, u32 n) {
    void** a = ((Lanes*)c)->p[0]; void** b = ((Lanes*)c)->p[1];
    while (n--) { R4(a = (void**)*a; b = (void**)*b;) }
    ((Lanes*)c)->p[0] = a; ((Lanes*)c)->p[1] = b; g_sink = (u32)(uintptr_t)a + (u32)(uintptr_t)b;
}
static void k_lane3(void* c, u32 n) {
    void** a = ((Lanes*)c)->p[0]; void** b = ((Lanes*)c)->p[1]; void** d = ((Lanes*)c)->p[2];
    while (n--) { R4(a = (void**)*a; b = (void**)*b; d = (void**)*d;) }
    ((Lanes*)c)->p[0] = a; ((Lanes*)c)->p[1] = b; ((Lanes*)c)->p[2] = d;
    g_sink = (u32)(uintptr_t)a + (u32)(uintptr_t)b + (u32)(uintptr_t)d;
}
static void k_lane4(void* c, u32 n) {
    void** a = ((Lanes*)c)->p[0]; void** b = ((Lanes*)c)->p[1]; void** d = ((Lanes*)c)->p[2]; void** e = ((Lanes*)c)->p[3];
    while (n--) { R4(a = (void**)*a; b = (void**)*b; d = (void**)*d; e = (void**)*e;) }
    ((Lanes*)c)->p[0] = a; ((Lanes*)c)->p[1] = b; ((Lanes*)c)->p[2] = d; ((Lanes*)c)->p[3] = e;
    g_sink = (u32)(uintptr_t)a + (u32)(uintptr_t)b + (u32)(uintptr_t)d + (u32)(uintptr_t)e;
}
static void k_lane6(void* c, u32 n) {
    void** a = ((Lanes*)c)->p[0]; void** b = ((Lanes*)c)->p[1]; void** d = ((Lanes*)c)->p[2];
    void** e = ((Lanes*)c)->p[3]; void** f = ((Lanes*)c)->p[4]; void** g = ((Lanes*)c)->p[5];
    while (n--) { R4(a = (void**)*a; b = (void**)*b; d = (void**)*d; e = (void**)*e; f = (void**)*f; g = (void**)*g;) }
    ((Lanes*)c)->p[0] = a; ((Lanes*)c)->p[1] = b; ((Lanes*)c)->p[2] = d; ((Lanes*)c)->p[3] = e; ((Lanes*)c)->p[4] = f;
    ((Lanes*)c)->p[5] = g;
    g_sink = (u32)(uintptr_t)a + (u32)(uintptr_t)b + (u32)(uintptr_t)d + (u32)(uintptr_t)e + (u32)(uintptr_t)f + (u32)(uintptr_t)g;
}
static void mlp(u8* mem, u32 memBytes, u32* order) {
    const u32 bytes = 8u << 20, count = bytes / 32;
    if (bytes > memBytes) return;
    build_cycle(mem, count, line_addr, order);
    static const struct { u32 lanes; Kernel k; } kL[] = {{1, k_lane1}, {2, k_lane2}, {3, k_lane3}, {4, k_lane4}, {6, k_lane6}};
    for (unsigned k = 0; k < sizeof kL / sizeof kL[0]; k++) {
        Lanes l;
        for (u32 i = 0; i < kL[k].lanes; i++) l.p[i] = (void**)(mem + line_addr(order[i * (count / kL[k].lanes)]));
        char p[16];
        snprintf(p, sizeof p, "%lu lanes", (unsigned long)kL[k].lanes);
        measure("mlp 8M", p, kL[k].k, &l, 4.0 * kL[k].lanes);
    }
}

// Store path: one store (or one load) per 32-byte line over FOOTPRINT; store-to-load forwarding; store throughput.
static void k_store_lines(void* ctx, u32 n) {
    Buf* b = ctx;
    while (n--)
        for (u32* p = (u32*)b->mem, *e = (u32*)(b->mem + b->bytes); p < e; p += 32) {
            p[0] = n; p[8] = n; p[16] = n; p[24] = n;
        }
}
static void k_load_lines(void* ctx, u32 n) {
    Buf* b = ctx;
    u32 acc = 0;
    while (n--)
        for (u32* p = (u32*)b->mem, *e = (u32*)(b->mem + b->bytes); p < e; p += 32) acc += p[0] + p[8] + p[16] + p[24];
    g_sink = acc;
}
static void k_stl(void* c, u32 n) {  // store, load the same word back, add: the store -> load round trip
    u32 a = 0;
    while (n--) __asm__ volatile(R16("str %0, [%1]\n ldr %0, [%1]\n add %0, %0, #1\n") : "+r"(a) : "r"(g_word) : "memory");
    g_sink = a;
}
static void k_str_ind(void* c, u32 n) {
    u32 a = n;
    while (n--) __asm__ volatile(R4("str %0, [%1]\n str %0, [%1, #4]\n str %0, [%1, #8]\n str %0, [%1, #12]\n") :: "r"(a), "r"(g_word) : "memory");
}
static void stores(u8* mem, u32 memBytes) {
    static const u32 kFoot[] = {8u << 10, 512u << 10, 8u << 20};
    for (unsigned f = 0; f < 3; f++) {
        if (kFoot[f] > memBytes) break;
        Buf b = {mem, kFoot[f]};
        char p[32];
        snprintf(p, sizeof p, "%luK one per line", (unsigned long)(kFoot[f] >> 10));
        measure("store", p, k_store_lines, &b, kFoot[f] / 32.0);
        measure("load", p, k_load_lines, &b, kFoot[f] / 32.0);
    }
    measure("op", "str_then_ldr", k_stl, NULL, 16);
    measure("op", "str_ind", k_str_ind, NULL, 16);
}

// Load / store forms (L1 hits): sub-word loads then use, doubleword and multiple transfers, and the big-endian guest
// load (ldr then rev) as a dependent chain: a cycle of byte-swapped pointers.
static void k_ldrh_use(void* c, u32 n) {
    u32 a = 0, t;
    while (n--) __asm__ volatile(R16("ldrh %1, [%2]\n add %0, %0, %1\n") : "+r"(a), "=&r"(t) : "r"(g_word));
    g_sink = a;
}
static void k_ldrb_use(void* c, u32 n) {
    u32 a = 0, t;
    while (n--) __asm__ volatile(R16("ldrb %1, [%2]\n add %0, %0, %1\n") : "+r"(a), "=&r"(t) : "r"(g_word));
    g_sink = a;
}
static void k_ldrd(void* c, u32 n) { while (n--) __asm__ volatile(R16("ldrd r4, r5, [%0]\n") :: "r"(g_word) : "r4", "r5"); }
static void k_strd(void* c, u32 n) { while (n--) __asm__ volatile(R16("strd r4, r5, [%0]\n") :: "r"(g_word) : "memory"); }
static void k_ldm4(void* c, u32 n) { while (n--) __asm__ volatile(R16("ldm %0, {r4-r7}\n") :: "r"(g_word) : "r4", "r5", "r6", "r7"); }
static void k_stm4(void* c, u32 n) { while (n--) __asm__ volatile(R16("stm %0, {r4-r7}\n") :: "r"(g_word) : "memory"); }
static void k_ldm7(void* c, u32 n) {
    while (n--) __asm__ volatile(R16("ldm %0, {r4-r10}\n") :: "r"(g_word) : "r4", "r5", "r6", "r7", "r8", "r9", "r10");
}
static void k_ldr_rev(void* c, u32 n) {  // p = rev(*p): the guest's big-endian pointer chase
    void* p = *(void**)c;
    while (n--) __asm__ volatile(R16("ldr %0, [%0]\n rev %0, %0\n") : "+r"(p));
    g_sink = (u32)(uintptr_t)p;
}
static void forms(u8* mem, u32* order) {
    static const struct { const char* name; Kernel k; } kK[] = {
        {"ldrh_use", k_ldrh_use}, {"ldrb_use", k_ldrb_use}, {"ldrd_ind", k_ldrd}, {"strd_ind", k_strd},
        {"ldm4_ind", k_ldm4},     {"stm4_ind", k_stm4},     {"ldm7_ind", k_ldm7}};
    for (unsigned i = 0; i < sizeof kK / sizeof kK[0]; i++) measure("op", kK[i].name, kK[i].k, NULL, 16);
    if (!mem) return;
    void** start = build_cycle(mem, (4u << 10) / 32, line_addr, order);  // 4 KB: L1 hits
    for (u32 i = 0; i < (4u << 10) / 32; i++) {
        u32* slot = (u32*)(mem + i * 32);
        *slot = __builtin_bswap32(*slot);
    }
    measure("op", "ldr_rev_dep (BE chase, 4K)", k_ldr_rev, &start, 16);
}

// ---- contention: core 0's memory latency while another bus master works (BENCHMARK_TOOLING_SURVEY.md probe 11) ----
// The scratch the chase uses, kept for gpuprobe.c's GPU-load case (it runs after this probe, on the same memory).
static u8* g_probeMem;
static u32 g_probeBytes;
static u32* g_probeOrder;
bool g_probeRisky;  // --probe-risky: the DMA engine too (gc3ds never drives it; a run only when James can restart the 2DS)
void** probe_chase_prepare(u32 bytes) {
    if (!g_probeMem || !g_probeOrder || bytes > g_probeBytes) return NULL;
    return build_cycle(g_probeMem, bytes / 32, line_addr, g_probeOrder);
}
double probe_chase_run(void*** p, u32 loads) {  // ns per load, timed with the tick
    u32 n = loads / 8 ? loads / 8 : 1;
    u64 t = svcGetSystemTick();
    k_chase(p, n);
    t = svcGetSystemTick() - t;
    return (double)t * 1e9 / SYSCLOCK_ARM11 / (8.0 * n);
}

static void dma_probe(void) {
    const u32 kBytes = 4u << 20;
    u8* src = linearAlloc(kBytes);
    u8* dst = linearAlloc(kBytes);
    if (!src || !dst) {
        out("# timing dma: cannot allocate 2 x 4 MB of linear memory\n");
        if (src) linearFree(src);
        if (dst) linearFree(dst);
        return;
    }
    memset(src, 0x5A, kBytes);
    memset(dst, 0, kBytes);
    svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)src, kBytes);
    svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)dst, kBytes);
    void** p = probe_chase_prepare(1u << 20);
    double alone = p ? probe_chase_run(&p, 400000) : -1;
    for (int pass = 0; pass < 2; pass++) {  // pass 1: core 0 chases 1 MB while the copy runs
        DmaConfig cfg;
        dmaConfigInitDefault(&cfg);
        Handle h = 0;
        out("# timing: section dma pass %d\n", pass);
        u64 t0 = svcGetSystemTick();
        Result r = svcStartInterProcessDma(&h, CUR_PROCESS_HANDLE, (u32)dst, CUR_PROCESS_HANDLE, (u32)src, kBytes, &cfg);
        if (R_FAILED(r)) { out("# timing dma: start failed %08lx\n", (unsigned long)r); break; }
        double during = pass && p ? probe_chase_run(&p, 100000) : -1;
        DmaState st = DMASTATE_STARTING;
        u64 limit = t0 + 2ull * SYSCLOCK_ARM11;
        do { svcGetDmaState(&st, h); } while (st != DMASTATE_DONE && svcGetSystemTick() < limit);
        u64 t1 = svcGetSystemTick();
        svcCloseHandle(h);
        svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)dst, kBytes);
        bool same = dst[0] == 0x5A && dst[kBytes / 2] == 0x5A && dst[kBytes - 1] == 0x5A;
        double mbs = (double)kBytes / 1048576.0 / ((double)(t1 - t0) / SYSCLOCK_ARM11);
        if (!pass) out("# timing dma 4 MB FCRAM -> FCRAM (kernel DMA, default config): %.0f MB/s%s%s\n", mbs,
                       st == DMASTATE_DONE ? "" : " (not done after 2 s)", same ? "" : " (copy wrong)");
        else out("# timing dma: core 0 chase 1 MB %.1f ns per load alone, %.1f ns while the DMA copies (that copy %.0f MB/s)\n",
                 alone, during, mbs);
        memset(dst, 0, kBytes);
        svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)dst, kBytes);
    }
    linearFree(src);
    linearFree(dst);
}

const char* timing_probe(u32* code, u32 codeWords, u8* mem, u32 memBytes, const char* disc) {
    static char buf[96 << 10];
    g_out = buf;
    g_outLen = 0;
    g_outCap = sizeof buf;
    buf[0] = 0;
    g_pmuOk = pmu_init();
    if (!g_pmuOk) out("# timing: counters unavailable (%s); cycles from the system tick, events zero\n", pmu_status());
    u64 t0 = svcGetSystemTick();
    out("# timing: probe start (core 0; per operation; cyc = fewest cycles of the five pair runs)\n");
    u32* order = NULL;  // the random order of a cycle (up to 16 MB / 32 entries): the last 2 MB of the scratch
    if (mem && memBytes > (4u << 20)) {
        memBytes -= 2u << 20;
        order = (u32*)(mem + memBytes);
    } else
        mem = NULL;
    SECTION("pipeline");
    pipeline();
    SECTION("branches");
    branches();
    SECTION("indirect");
    indirect();
    SECTION("calls");
    returns();
    SECTION("icode");
    if (code && codeWords) icode(code, codeWords);
    SECTION("btac");
    if (code && codeWords) btac(code, codeWords);
    SECTION("install");
    if (code && codeWords) install(code, codeWords);
    SECTION("forms");
    forms(mem, order);
    if (mem) {
        SECTION("chase");
        chase(mem, memBytes, order);
        SECTION("tlb");
        tlb(mem, memBytes, order);
        Buf b = {mem, 4u << 20};
        if (memBytes >= b.bytes) measure("stream", "4M stores", k_stream, &b, b.bytes / 4.0);
        SECTION("assoc");
        assoc(mem, memBytes, order);
        SECTION("seqchase");
        seqchase(mem, memBytes);
        SECTION("mlp");
        mlp(mem, memBytes, order);
        SECTION("stores");
        stores(mem, memBytes);
        SECTION("xcore");
        xcore(mem, memBytes, order);
    }
    measure("svc", "GetSystemTick", k_tick, NULL, 1);
    measure("svc", "icache sync (flush + 0x94)", k_sync, NULL, 1);
    SECTION("bandwidth");
    bandwidth(mem, memBytes);
    SECTION("kernel_switch");
    kernel_switch();
    SECTION("storage");
    storage(disc);
    SECTION("clocks");
    clocks(mem, order);
    g_probeMem = mem;  // for the contention cases (here and gpuprobe.c)
    g_probeBytes = mem ? memBytes : 0;
    g_probeOrder = order;
    if (g_probeRisky) {
        SECTION("dma");
        dma_probe();
    }
    out("# timing: probe end after %.1f s\n", (double)(svcGetSystemTick() - t0) / SYSCLOCK_ARM11);
    return buf;
}

// X2 deliberately bypasses the full timing/GPU suite and game boot. No production cache setting changes.
bool mirror_mmio_read(unsigned offset, u32* value);
bool mirror_mmio_write(unsigned offset, u32 value);
static bool lock_read(void* ctx, unsigned offset, uint32_t* value) {
    (void)ctx;
    bool ok = mirror_mmio_read(offset, value);
    if (!ok) out("# l2lock: READ FAULT offset %03x\n", offset);
    return ok;
}
static bool lock_write(void* ctx, unsigned offset, uint32_t value) {
    (void)ctx;
    out("# l2lock: write %03x %08lx\n", offset, (unsigned long)value);
    bool ok = mirror_mmio_write(offset, value);
    if (!ok) out("# l2lock: WRITE FAULT offset %03x\n", offset);
    return ok;
}
static bool lock_dump(const char* phase, const L2LockIo* io) {
    bool ok = true;
    for (unsigned i = 0; i < L2LOCK_WORDS; i++) {
        uint32_t value = 0;
        bool read = io->read(io->ctx, L2LOCK_FIRST + i * 4, &value);
        out("# l2lock: %s %03x %08lx %s\n", phase, L2LOCK_FIRST + i * 4,
            (unsigned long)value, read ? "read" : "FAULT");
        if (!read) ok = false;
    }
    return ok;
}
static bool lock_chase(const char* phase, void** start) {
    for (unsigned trial = 0; trial < 3; trial++) {
        char param[80];
        snprintf(param, sizeof param, "l2lock-%s trial%u 1024K alone", phase, trial);
        measure("xcore", param, k_chase, &start, 8);
        g_stop = 0;
        g_streamCore = -1;
        Thread t = threadCreate(streamer, (void*)(uintptr_t)(4u << 20), 4096, 0x30, 2, false);
        if (!t) { out("# l2lock: ERROR creating core2 streamer\n"); return false; }
        svcSleepThread(2000000);
        bool ok = g_streamCore == 2;
        out("# l2lock: %s trial%u streamer core %ld\n", phase, trial, (long)g_streamCore);
        if (ok) {
            snprintf(param, sizeof param, "l2lock-%s trial%u 1024K core2-streams", phase, trial);
            measure("xcore", param, k_chase, &start, 8);
        }
        g_stop = 1;
        Result joined = threadJoin(t, U64_MAX);
        if (R_SUCCEEDED(joined)) threadFree(t);
        if (!ok || R_FAILED(joined)) { out("# l2lock: ERROR stopping core2 streamer\n"); return false; }
    }
    return true;
}
bool l2lock_probe(FILE* f, u8* mem, u32 memBytes) {
    static char buf[32 << 10];
    g_out = buf; g_outLen = 0; g_outCap = sizeof buf; buf[0] = 0;
    probe_live = f;
    bool measured = false, restored = false;
    void** start = NULL;
    L2Lock state;
    memset(&state, 0, sizeof state);
    const L2LockIo io = {NULL, lock_read, lock_write};
    out("# l2lock: X2 ARM DDI0246H r3p3; mirror %08lx; core0 %ld\n",
        (unsigned long)L2LOCK_BASE, (long)svcGetProcessorID());
    out("# l2lock: master selector = AyUSER[7:5]; assumes selectors 1-3 are cores 1-3; unverified SoC wiring\n");
    if (!mem || memBytes < (14u << 20) || svcGetProcessorID() != 0) goto cleanup;
    if (!l2lock_snapshot(&state, &io)) goto cleanup;
    out("# l2lock: id %08lx control@100 %08lx aux@104 %08lx NS-lockdown %u ways %u (aux read only)\n",
        (unsigned long)state.id, (unsigned long)state.control, (unsigned long)state.aux,
        (unsigned)((state.aux >> 26) & 1), (state.aux & (1u << 16)) ? 16 : 8);
    if (!lock_dump("saved", &io)) goto cleanup;
    g_pmuOk = pmu_init(); g_badRun = 0;
    out("# l2lock: counters %s; nine pairs per measurement, minimum cycles, identical chase throughout\n",
        g_pmuOk ? "PMU" : "tick fallback");
    u32* order = (u32*)(mem + memBytes - (2u << 20));
    start = build_cycle(mem, (1u << 20) / 32, line_addr, order);
    g_streamMem = mem + (8u << 20);
    if (!lock_chase("baseline", start)) goto cleanup;
    if (!l2lock_apply(&state, &io)) {
        out("# l2lock: APPLY FAILED at %03x (zero/ignored/aliased readback, access fault, or unsupported setup)\n",
            state.badOffset);
        lock_dump("rejected", &io);
        goto cleanup;
    }
    if (!lock_dump("partition", &io)) goto cleanup;
    measured = lock_chase("partition", start);
cleanup:
    // There are no returns between the first MMIO store and this cleanup. Even a failed write is restored.
    restored = l2lock_restore(&state, &io);
    if (!restored) {
        out("# l2lock: retrying full restoration after error\n");
        restored = l2lock_restore(&state, &io);
    }
    out("# l2lock: restore %s attempted-mask %04x\n",
        !state.snapshot ? "NOT-TOUCHED" : restored ? "VERIFIED" : "FAILED", state.attempted);
    if (state.snapshot && !lock_dump("restored", &io)) restored = false;
    if (measured && restored) measured = lock_chase("restored", start);
    if (!restored) g_l2lockUnsafe = true;
    out("# l2lock: end measured %u restored %u; no guest boot\n", measured, restored);
    probe_live = NULL;
    return measured && restored;
}

// X2's conditional whole-run scoring uses the SAME transaction after the chase supports it. Default off.
// main.c owns the finally wrapper and defers closing perf.csv until this has restored the hardware.
static L2Lock g_runLock;
bool g_l2lockRunPending;
bool l2lock_run_begin(FILE* f) {
    static char buf[8 << 10];
    g_out = buf; g_outLen = 0; g_outCap = sizeof buf; buf[0] = 0;
    probe_live = f;
    g_l2lockRunPending = true;
    const L2LockIo io = {NULL, lock_read, lock_write};
    bool ok = l2lock_snapshot(&g_runLock, &io);
    if (ok) {
        out("# l2lock run: id %08lx control %08lx aux %08lx; selectors 1-3, half ways\n",
            (unsigned long)g_runLock.id, (unsigned long)g_runLock.control, (unsigned long)g_runLock.aux);
        ok = lock_dump("saved", &io) && l2lock_apply(&g_runLock, &io);
    }
    if (ok) ok = lock_dump("partition", &io);
    out("# l2lock run: begin %s\n", ok ? "VERIFIED" : "FAILED");
    probe_live = NULL;
    return ok;
}
bool l2lock_run_end(FILE* f) {
    if (!g_l2lockRunPending) return true;
    // Reset the bounded output buffer: normal perf logging does not append to it during the run.
    g_outLen = 0; g_out[0] = 0;
    probe_live = f;
    const L2LockIo io = {NULL, lock_read, lock_write};
    bool ok = l2lock_restore(&g_runLock, &io);
    if (!ok) {
        out("# l2lock run: retrying restoration\n");
        ok = l2lock_restore(&g_runLock, &io);
    }
    out("# l2lock run: restore %s\n", ok ? "VERIFIED" : "FAILED");
    if (g_runLock.snapshot && !lock_dump("restored", &io)) ok = false;
    if (!ok) g_l2lockUnsafe = true;
    g_l2lockRunPending = false;
    probe_live = NULL;
    return ok;
}
