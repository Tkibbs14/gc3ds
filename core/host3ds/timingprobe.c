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
#include <3ds.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pmu.h"

void jit_host_sync_icache(void* start, u32 bytes);

static char* g_out;
static size_t g_outLen, g_outCap;
static void out(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(g_out + g_outLen, g_outCap - g_outLen, fmt, ap);
    va_end(ap);
    if (n > 0) g_outLen += (size_t)n < g_outCap - g_outLen ? (size_t)n : g_outCap - g_outLen - 1;
}

typedef void (*Kernel)(void* ctx, u32 n);
static volatile u32 g_sink;

// Without the performance counters (an emulator such as Azahar refuses them), cycles come from the system tick at the
// ARM11's clock and the events read zero: every benchmark still runs, so the probe itself is tested off the device.
static bool g_pmuOk;
static u64 g_tick0;
static void sel(unsigned p) {
    if (g_pmuOk) pmu_select(p);
    g_tick0 = svcGetSystemTick();
}
static void rd(unsigned long long* cyc, unsigned long long* e0, unsigned long long* e1) {
    const unsigned long long byTick = (svcGetSystemTick() - g_tick0) * 804 / 268;  // ticks at 268 MHz -> cycles at 804 MHz
    // The counters are trusted only while their cycle count agrees with the tick within 2x (Azahar accepts the system
    // call and leaves junk; a misconfigured counter on the device would show the same way).
    if (g_pmuOk && pmu_read(cyc, e0, e1) && *cyc >= byTick / 2 && *cyc <= byTick * 2 + 1000) return;
    if (g_pmuOk) out("# timing: counters disagree with the tick (%llu cycles vs %llu); tick from here on, events zero\n", *cyc, byTick);
    g_pmuOk = false;
    *cyc = byTick;
    *e0 = *e1 = 0;
}

// Runs k for about 20 M cycles per counter pair and writes one line: cycles and the ten events per operation (ops:
// operations per unit of n).
static void measure(const char* name, const char* param, Kernel k, void* ctx, double opsPerUnit) {
    unsigned long long cyc, e0, e1;
    u32 n = 1;
    for (;;) {  // size n to ~20 M cycles (also warms the caches and TLB)
        sel(0);
        k(ctx, n);
        rd(&cyc, &e0, &e1);
        if (cyc >= 2000000 || n >= (1u << 28)) break;
        n *= cyc < 200000 ? 16 : 4;
    }
    n = (u32)((double)n * 20e6 / (double)(cyc ? cyc : 1));
    if (n < 1) n = 1;
    double c[5], ev[5][2];
    for (unsigned p = 0; p < 5; p++) {
        sel(p);
        k(ctx, n);
        rd(&cyc, &e0, &e1);
        double ops = opsPerUnit * n;
        c[p] = (double)cyc / ops;
        ev[p][0] = (double)e0 / ops;
        ev[p][1] = (double)e1 / ops;
    }
    double cmin = c[0];
    for (int p = 1; p < 5; p++) if (c[p] < cmin) cmin = c[p];
    out("# timing %s %s: cyc %.3f (max %.3f) inst %.3f brmiss %.4f icmiss %.4f dcrmiss %.4f stall_inst %.3f stall_data %.3f "
        "dcwmiss %.4f tlbmiss %.4f branch %.3f stall_lsu %.3f\n", name, param, cmin,
        c[0] > c[1] ? (c[0] > c[2] ? c[0] : c[2]) : (c[1] > c[2] ? c[1] : c[2]), ev[0][0], ev[0][1], ev[1][0], ev[1][1],
        ev[2][0], ev[2][1], ev[3][0], ev[3][1], ev[4][0], ev[4][1]);
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
static void streamer(void* arg) {
    u32 bytes = (u32)(uintptr_t)arg, v = 0;
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

// Runs every benchmark; `code` / `mem` are scratch (the JIT's buffer past its stubs, MEM1). Returns the text to put
// in perf.csv (valid until the next call).
const char* timing_probe(u32* code, u32 codeWords, u8* mem, u32 memBytes) {
    static char buf[24 << 10];
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
    pipeline();
    branches();
    if (code && codeWords) icode(code, codeWords);
    if (mem) {
        chase(mem, memBytes, order);
        tlb(mem, memBytes, order);
        Buf b = {mem, 4u << 20};
        if (memBytes >= b.bytes) measure("stream", "4M stores", k_stream, &b, b.bytes / 4.0);
        xcore(mem, memBytes, order);
    }
    measure("svc", "GetSystemTick", k_tick, NULL, 1);
    measure("svc", "icache sync (flush + 0x94)", k_sync, NULL, 1);
    out("# timing: probe end after %.1f s\n", (double)(svcGetSystemTick() - t0) / SYSCLOCK_ARM11);
    return buf;
}
