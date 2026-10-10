// CPU benchmarks: what it costs to run Gekko/Broadway code on the New 3DS ARM11 through a JIT.
#include <malloc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bench.h"
#include "kernels.h"
#include "ppc.h"
#include "progs.h"

#define RAM_SIZE (32u * 1024 * 1024)
#define JIT_SIZE 0x40000u
#define GEKKO_MHZ 486.0
#define BROADWAY_MHZ 729.0

static Handle g_proc;
static u32* g_jit;

static s32 kernel_sync_caches(void) {
    __asm__ volatile(
        "mov r0, #0\n"
        "mcr p15, 0, r0, c7, c10, 0\n"  // clean entire data cache
        "mcr p15, 0, r0, c7, c10, 4\n"  // drain write buffer
        "mcr p15, 0, r0, c7, c5, 0\n"   // invalidate entire instruction cache
        "mcr p15, 0, r0, c7, c5, 6\n"   // flush branch target cache
        "mcr p15, 0, r0, c7, c5, 4\n"   // flush prefetch buffer
        ::: "r0", "memory");
    return 0;
}

static int jit_memory_init(void) {
    g_jit = memalign(0x1000, JIT_SIZE);
    if (!g_jit) { P("JIT: memalign failed\n"); return 0; }
    Result r = svcDuplicateHandle(&g_proc, CUR_PROCESS_HANDLE);
    if (R_FAILED(r)) { P("JIT: svcDuplicateHandle %08lx\n", r); return 0; }
    r = svcControlProcessMemory(g_proc, (u32)g_jit, (u32)g_jit, JIT_SIZE, MEMOP_PROT,
                                MEMPERM_READ | MEMPERM_WRITE | MEMPERM_EXECUTE);
    P("JIT: RWX %s (%08lx)\n", R_SUCCEEDED(r) ? "OK" : "FAILED", r);
    return R_SUCCEEDED(r);
}

static void publish(u32 words, int syncICache) {
    svcFlushProcessDataCache(g_proc, (u32)g_jit, words * 4);
    if (syncICache) svcBackdoor(kernel_sync_caches);
}

static u32 read_fpscr(void) { u32 v; __asm__ volatile("vmrs %0, fpscr" : "=r"(v)); return v; }

// ---- 1. self-modifying code: is the instruction-cache sync needed, does it work, and what does it cost? ----
static void test_icache(void) {
    step("icache");
    typedef u32 (*RetFn)(void);
    int staleWith = 0, staleWithout = 0;
    for (int pass = 0; pass < 2; pass++) {
        for (u32 k = 1; k <= 200; k++) {
            g_jit[0] = 0xE3A00000u | k;  // mov r0, #k
            g_jit[1] = 0xE12FFF1Eu;      // bx lr
            publish(2, pass == 0);
            if (((RetFn)g_jit)() != k) { if (pass == 0) staleWith++; else staleWithout++; }
        }
    }
    P("icache: stale %d/200 with sync, %d/200 without\n", staleWith, staleWithout);
    double t = now_s();
    for (int i = 0; i < 1000; i++) svcFlushProcessDataCache(g_proc, (u32)g_jit, 256);
    double tFlush = (now_s() - t) / 1000;
    t = now_s();
    for (int i = 0; i < 1000; i++) svcBackdoor(kernel_sync_caches);
    double tSync = (now_s() - t) / 1000;
    P("dcache flush 256B %.1f us, full sync %.1f us\n", tFlush * 1e6, tSync * 1e6);
}

// ---- 2. JIT correctness and speed for the three guest programs ----
static int same_state(const PPCState* a, const PPCState* b, const u8* ra, const u8* rb) {
    return memcmp(a->gpr, b->gpr, sizeof a->gpr) == 0 && memcmp(a->ps0, b->ps0, sizeof a->ps0) == 0 &&
           memcmp(a->ps1, b->ps1, sizeof a->ps1) == 0 && a->cr == b->cr && a->xer == b->xer &&
           memcmp(ra, rb, GUEST_TOUCHED) == 0;
}

static JitBlock compile(Program* p, Decoded* dec, int mode, int layout, const u32* gqr) {
    char err[128];
    if (ppc_decode(p->code, p->n, dec, err, sizeof err) < 0) { P("%s\n", err); return NULL; }
    int words = jit_compile(dec, p->n, g_jit, JIT_SIZE / 4, mode, layout, gqr, err, sizeof err);
    if (words < 0) { P("%s\n", err); return NULL; }
    publish((u32)words, 1);
    return (JitBlock)g_jit;
}

typedef void (*MakeFn)(Program*, u32);
// interp = also time the interpreter on this program (BE layout only; its swapped path is a byte-wise reference).
static const struct { MakeFn make; int mode, layout, interp; const char* label; } kRuns[] = {
    { make_int, JIT_PS_SCALAR, RAM_BE, 1, "int BE" },
    { make_int, JIT_PS_SCALAR, RAM_SWAPPED, 0, "int sw" },
    { make_float, JIT_PS_SCALAR, RAM_BE, 1, "float BE" },
    { make_float, JIT_PS_SCALAR, RAM_SWAPPED, 0, "float sw" },
    { make_ps, JIT_PS_SCALAR, RAM_BE, 1, "paired BE" },
    { make_ps, JIT_PS_SCALAR, RAM_SWAPPED, 0, "paired sw" },
    { make_ps, JIT_PS_VECTOR, RAM_SWAPPED, 0, "paired sw vec" },
    { make_pseq, JIT_PS_SCALAR, RAM_BE, 0, "pseq BE" },
    { make_pseq, JIT_PS_SCALAR, RAM_SWAPPED, 0, "pseq sw" },
    { make_pilp, JIT_PS_SCALAR, RAM_BE, 0, "pilp BE" },
    { make_pilp, JIT_PS_SCALAR, RAM_SWAPPED, 0, "pilp sw" },
    { make_pilp, JIT_PS_VECTOR, RAM_SWAPPED, 0, "pilp sw vec" },
    { make_int, JIT_PS_SCALAR, RAM_BE_SETEND, 0, "int setend" },
    { make_float, JIT_PS_SCALAR, RAM_BE_SETEND, 0, "float setend" },
    { make_ps, JIT_PS_SCALAR, RAM_BE_SETEND, 0, "paired setend" },
    { make_pilp, JIT_PS_SCALAR, RAM_BE_SETEND, 0, "pilp setend" },
};
#define NRUNS (int)(sizeof kRuns / sizeof kRuns[0])

static void report_mips(const char* what, double instrs, double secs) {
    double mips = instrs / secs / 1e6;
    P("%-16s %6.1f MIPS %5.1f%% GC %5.1f%% Wii\n", what, mips, 100 * mips / GEKKO_MHZ, 100 * mips / BROADWAY_MHZ);
}

static int test_jit(u8* ram, u8* ram2) {
    Decoded dec[48];
    PPCState a, b;
    char err[128];
    int allOk = 1;
    for (int k = 0; k < NRUNS; k++) {
        step(kRuns[k].label);
        Program p;
        kRuns[k].make(&p, 1000);
        if (ppc_decode(p.code, p.n, dec, err, sizeof err) < 0) { P("%s\n", err); return 0; }
        int lay = kRuns[k].layout;
        reset_guest(&a, ram, lay); ppc_interp(&a, ram, dec, lay);
        memcpy(ram2, ram, GUEST_TOUCHED);
        reset_guest(&b, ram, lay);
        JitBlock fn = compile(&p, dec, kRuns[k].mode, lay, b.gqr);
        if (!fn) return 0;
        fn(&b, ram);
        int same = same_state(&a, &b, ram2, ram) && b.pc != PC_BAILED;
        P("check %-13s %s\n", kRuns[k].label, b.pc == PC_BAILED ? "BAILED" : same ? "OK" : "MISMATCH");
        if (!same) {
            for (int r = 0; r < 8; r++) if (a.ps0[r] != b.ps0[r] || a.ps1[r] != b.ps1[r])
                P("  f%d i(%g,%g) j(%g,%g)\n", r, a.ps0[r], a.ps1[r], b.ps0[r], b.ps1[r]);
            allOk = 0;
        }
    }
    P("\n-- guest MIPS (GC 486 MHz, Wii 729 MHz) --\n");
    const u32 interpIters = 500000, jitIters = 10000000;
    for (int k = 0; k < NRUNS; k++) {
        Program p;
        char name[32];
        if (kRuns[k].interp) {
            step("interp");
            kRuns[k].make(&p, interpIters);
            ppc_decode(p.code, p.n, dec, err, sizeof err);
            reset_guest(&a, ram, RAM_BE);
            double t = now_s(); ppc_interp(&a, ram, dec, RAM_BE); t = now_s() - t;
            snprintf(name, sizeof name, "interp %s", p.name);
            report_mips(name, p.setup + (double)p.body * interpIters, t);
        }
        step(kRuns[k].label);
        kRuns[k].make(&p, jitIters);
        reset_guest(&a, ram, kRuns[k].layout);
        JitBlock fn = compile(&p, dec, kRuns[k].mode, kRuns[k].layout, a.gqr);
        if (!fn) continue;
        double t = now_s(); fn(&a, ram); t = now_s() - t;
        snprintf(name, sizeof name, "JIT %s", kRuns[k].label);
        report_mips(name, p.setup + (double)p.body * jitIters, t);
    }
    return allOk;
}

static u32 native_int(u32 iters, u32* mem) {
    u32 r3 = 0, r5 = 0, r7;
    for (u32 i = 0; i < iters; i++) {
        u32 r6 = mem[r5 / 4];
        r3 += r6;
        r7 = r3 << 3;
        r3 ^= r7;
        mem[(r5 + 4) / 4] = r3;
        r5 = (r5 + 8) & 0x1FFF;
    }
    return r3;
}

static void test_native(void) {
    step("native");
    u32* mem = calloc(1, 0x2000 + 8);
    if (!mem) return;
    const u32 iters = 10000000;
    double t = now_s();
    volatile u32 sink = native_int(iters, mem);
    (void)sink;
    t = now_s() - t;
    report_mips("native C int", 9.0 * iters, t);
    free(mem);
}

// ---- 3. translation speed: how long the JIT takes per guest instruction (stutter when new code appears) ----
static void test_compile_speed(void) {
    step("compile speed");
    Program p;
    make_ps(&p, 1000);
    Decoded dec[48];
    char err[128];
    PPCState s;
    memset(&s, 0, sizeof s);
    const int reps = 2000;
    double t = now_s();
    for (int i = 0; i < reps; i++) {
        ppc_decode(p.code, p.n, dec, err, sizeof err);
        if (jit_compile(dec, p.n, g_jit, JIT_SIZE / 4, JIT_PS_SCALAR, RAM_SWAPPED, s.gqr, err, sizeof err) < 0) { P("%s\n", err); return; }
    }
    t = now_s() - t;
    double perInstr = t / reps / p.n;
    P("JIT compile: %.2f us/guest instr (%.0f k instr/s)\n", perInstr * 1e6, 1e-3 / perInstr);
}

// ---- 4. block chaining: cost of leaving a block through a C dispatcher vs a direct linked branch ----
static void test_dispatch(void) {
    static const u32 Ks[2] = { 64, 2048 };
    const u32 slot = 16;
    for (int ki = 0; ki < 2; ki++) {
        u32 K = Ks[ki];
        step("linked");
        int w = kern_linked(g_jit, JIT_SIZE / 4, K, slot);
        if (w < 0) { P("linked K=%lu: buffer too small\n", K); continue; }
        publish((u32)w, 1);
        const u32 steps = 20000000;
        double t = now_s();
        u32 r = ((u32 (*)(u32))g_jit)(steps);
        t = now_s() - t;
        P("linked   %4lu blocks: %5.1f ns/block %s\n", K, t / steps * 1e9, r == 4 * steps ? "" : "BAD");

        step("dispatch");
        u32* words = malloc(K * sizeof(u32));
        KernEntry* tab = malloc(K * sizeof(KernEntry));
        if (!words || !tab) { free(words); free(tab); continue; }
        w = kern_dispatch(g_jit, JIT_SIZE / 4, K, slot, words);
        if (w < 0) { free(words); free(tab); continue; }
        publish((u32)w, 1);
        for (u32 i = 0; i < K; i++) { tab[i].pc = kern_block_pc(i); tab[i].fn = (u32 (*)(void))(g_jit + words[i]); }
        const u32 dsteps = 5000000;
        u32 expect = 0;
        for (u32 i = 0; i < dsteps; i++) expect = kern_next(expect, K);
        t = now_s();
        u32 pc = kern_run_dispatch(tab, K, dsteps);
        t = now_s() - t;
        P("dispatch %4lu blocks: %5.1f ns/block %s\n", K, t / dsteps * 1e9, pc == kern_block_pc(expect) ? "" : "BAD");
        free(words); free(tab);
    }
}

// ---- 4b. where the instruction cache runs out: linked-block cost as the JIT code footprint grows ----
static void test_icache_sweep(void) {
    const u32 slot = 16;  // 64 bytes of code per block
    P("linked block cost vs code size:\n");
    for (u32 K = 32; K * slot * 4 + 64 <= JIT_SIZE; K *= 2) {
        step("icache sweep");
        int w = kern_linked(g_jit, JIT_SIZE / 4, K, slot);
        if (w < 0) break;
        publish((u32)w, 1);
        const u32 steps = 4000000;
        double t = now_s();
        u32 r = ((u32 (*)(u32))g_jit)(steps);
        t = now_s() - t;
        P("  %4lu KB: %5.1f ns/block%s\n", (unsigned long)(K * slot * 4 / 1024), t / steps * 1e9, r == 4 * steps ? "" : " BAD");
    }
}

// ---- 4c. data latency vs working set: a random pointer chase, one access per 32-byte cache line ----
// Sattolo's shuffle gives a single random cycle through every line of the first `bytes`, defeating the prefetcher.
static int build_chain(u32* buf, u32 bytes) {
    u32 lines = bytes / 32, stride = 8;  // u32 index of each line's first word
    u32* order = malloc(lines * sizeof(u32));
    if (!order) return 0;
    u32 x = 2463534242u;
    for (u32 i = 0; i < lines; i++) order[i] = i;
    for (u32 i = lines - 1; i > 0; i--) {
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        u32 j = x % i, tmp = order[i]; order[i] = order[j]; order[j] = tmp;
    }
    for (u32 i = 0; i < lines; i++) buf[order[i] * stride] = order[(i + 1) % lines] * stride;
    free(order);
    u32 p = 0;
    for (u32 i = 0; i < lines; i++) p = buf[p];  // warm
    return 1;
}

static volatile u32 g_chaseSink;

static double chase_ns(const u32* buf, u32 n) {
    u32 p = 0;
    double t = now_s();
    for (u32 i = 0; i < n; i++) p = buf[p];
    t = now_s() - t;
    g_chaseSink = p;
    return t / n * 1e9;
}

static void test_latency_sweep(void) {
    const u32 maxBytes = 32u << 20;
    u32* buf = malloc(maxBytes);
    if (!buf) { P("latency sweep: SKIPPED (malloc 32 MB)\n"); return; }
    P("load latency vs working set:\n");
    for (u32 bytes = 4096; bytes <= maxBytes; bytes *= 2) {
        step("latency sweep");
        if (!build_chain(buf, bytes)) break;
        double ns = chase_ns(buf, 2000000);
        P("  %6lu KB: %6.1f ns (%4.0f cycles)\n", (unsigned long)(bytes / 1024), ns, ns * 0.804);
    }
    free(buf);
}

// ---- 4d. the CPU's own report of its caches (CP15 Cache Type Register, readable only in kernel mode) ----
static volatile u32 g_ctr;
static s32 kernel_read_ctr(void) {
    u32 v;
    __asm__ volatile("mrc p15, 0, %0, c0, c0, 1" : "=r"(v));
    g_ctr = v;
    return 0;
}

static void print_cache_field(const char* name, u32 f) {
    u32 m = (f >> 2) & 1;
    u32 bytes = (1u << (((f >> 6) & 15) + 8)) * (m ? 3 : 2);
    u32 ways = (1u << ((f >> 3) & 7)) * (m ? 3 : 2) / 2;
    P("  %s: %lu KB, %lu-way, %u-byte lines\n", name, (unsigned long)(bytes / 1024), (unsigned long)ways, 8u << (f & 3));
}

static void test_cache_info(void) {
    step("cache type");
    g_ctr = 0;
    svcBackdoor(kernel_read_ctr);
    P("CP15 cache type %08lx\n", (unsigned long)g_ctr);
    if (g_ctr) { print_cache_field("L1 I", g_ctr & 0xFFF); print_cache_field("L1 D", (g_ctr >> 12) & 0xFFF); }
}

// ---- 4e. New 3DS clock and L2 cache settings, one at a time: 0 = 268 MHz, 1 = 804 MHz, 2 = L2, 3 = both ----
static void test_speed_modes(u8* ram) {
    step("speed modes");
    Result r = ptmSysmInit();
    if (R_FAILED(r)) { P("speed modes: SKIPPED (ptm:sysm %08lx)\n", (unsigned long)r); return; }
    u32* chain = malloc(16u << 20);
    if (!chain) { P("speed modes: SKIPPED (malloc)\n"); ptmSysmExit(); return; }
    static const char* const names[4] = { "268 MHz, no L2", "804 MHz, no L2", "268 MHz, L2   ", "804 MHz, L2   " };
    P("clock/L2   JIT int   chase 1 MB   chase 16 MB\n");
    for (int cfg = 0; cfg < 4; cfg++) {
        step(names[cfg]);
        r = PTMSYSM_ConfigureNew3DSCPU((u8)cfg);
        if (R_FAILED(r)) { P("%s: config %08lx\n", names[cfg], (unsigned long)r); continue; }
        svcSleepThread(20000000LL);
        Program p;
        Decoded dec[48];
        PPCState s;
        const u32 iters = 2000000;
        make_int(&p, iters);
        reset_guest(&s, ram, RAM_BE);
        JitBlock fn = compile(&p, dec, JIT_PS_SCALAR, RAM_BE, s.gqr);
        if (!fn) break;
        double t = now_s();
        fn(&s, ram);
        t = now_s() - t;
        double mips = (p.setup + (double)p.body * iters) / t / 1e6;
        double c1 = build_chain(chain, 1u << 20) ? chase_ns(chain, 500000) : 0;
        double c16 = build_chain(chain, 16u << 20) ? chase_ns(chain, 500000) : 0;
        P("%s %6.0f MIPS %8.1f ns %10.1f ns\n", names[cfg], mips, c1, c16);
    }
    PTMSYSM_ConfigureNew3DSCPU(3);
    free(chain);
    ptmSysmExit();
}

// ---- 5. memory access paths: masked fastmem vs range check vs a C call per access (MMIO slow path) ----
static void test_mem_paths(u8* ram) {
    const u32 count = 1000000;
    kern_set_ram(ram);
    u32 sums[KMEM_COUNT];
    for (int v = 0; v < KMEM_COUNT; v++) {
        step(kern_mem_names[v]);
        int w = kern_mem(g_jit, JIT_SIZE / 4, v);
        if (w < 0) return;
        publish((u32)w, 1);
        double t = now_s();
        sums[v] = ((u32 (*)(u32, u8*))g_jit)(count, ram);
        t = now_s() - t;
        P("load %-11s %5.2f ns/access\n", kern_mem_names[v], t / count * 1e9);
    }
    P("load paths agree: %s\n", sums[0] == sums[1] && sums[1] == sums[2] ? "yes" : "NO");
}

// ---- 6. VFP operation latency (dependent chain) and throughput (independent ops) ----
static void test_fp_ops(void) {
    const u32 iters = 131072;  // x32 unrolled = 4.2 M ops
    const double ops = (double)iters * KERN_FP_UNROLL;
    P("\n-- VFP op cost, cycles at 804 MHz --\n");
    P("op            latency  throughput\n");
    for (int k = 0; k < KFP_COUNT; k++) {
        double c[2];
        for (int dep = 1; dep >= 0; dep--) {
            step(kern_fp_names[k]);
            int w = kern_fp(g_jit, JIT_SIZE / 4, k, dep);
            if (w < 0) return;
            publish((u32)w, 1);
            double t = now_s();
            ((void (*)(u32))g_jit)(iters);
            t = now_s() - t;
            c[dep] = t / ops * 804e6;
        }
        P("%-13s %6.2f  %6.2f\n", kern_fp_names[k], c[1], c[0]);
    }
    P("FPSCR after kernels: %08lx\n", read_fpscr());
}

// ---- 7. cores: does core 2 run alongside core 0, and how much of core 1 do we get? ----
typedef struct { JitBlock fn; u8* ram; double secs; } CoreJob;

static void core_job(void* arg) {
    CoreJob* j = arg;
    PPCState s;
    reset_guest(&s, j->ram, RAM_BE);
    double t = now_s();
    j->fn(&s, j->ram);
    j->secs = now_s() - t;
}

static void test_cores(void) {
    Program p;
    Decoded dec[48];
    PPCState s;
    memset(&s, 0, sizeof s);
    const u32 iters = 10000000;
    make_int(&p, iters);
    JitBlock fn = compile(&p, dec, JIT_PS_SCALAR, RAM_BE, s.gqr);
    if (!fn) return;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    static const int cores[3] = { 2, 2, 1 };
    for (int i = 0; i < 3; i++) {
        step(i == 2 ? "core 1" : "core 2");
        if (cores[i] == 1) APT_SetAppCpuTimeLimit(80);
        CoreJob* j = calloc(1, sizeof *j);
        u8* jr = calloc(1, 0x10000);
        if (!j || !jr) { free(j); free(jr); return; }
        j->fn = fn; j->ram = jr;
        double t0 = now_s();
        Thread th = threadCreate(core_job, j, 64 * 1024, prio, cores[i], false);
        if (!th) { P("core %d: threadCreate FAILED\n", cores[i]); free(j); free(jr); continue; }
        if (i == 1) {  // same work on core 0 at the same time
            u8* r0 = calloc(1, 0x10000);
            if (r0) { PPCState s0; reset_guest(&s0, r0, RAM_BE); fn(&s0, r0); free(r0); }
        }
        threadJoin(th, U64_MAX);
        double wall = now_s() - t0;
        threadFree(th);
        if (i == 0) P("core 2 alone: %.2f s\n", j->secs);
        else if (i == 1) P("core 0+2 together: %.2f s wall\n", wall);
        else P("core 1 (80%% limit): %.2f s\n", j->secs);
        free(j); free(jr);
    }
}

void bench_cpu(void) {
    bool isNew = false;
    APT_CheckNew3DS(&isNew);
    P("== CPU (%s 3DS, FPSCR %08lx) ==\n", isNew ? "New" : "Old", read_fpscr());
    u8* ram = malloc(RAM_SIZE);
    u8* ram2 = malloc(GUEST_TOUCHED);
    if (!ram || !ram2 || !jit_memory_init()) { P("CPU: setup failed, skipping\n"); free(ram); free(ram2); return; }
    test_icache();
    test_jit(ram, ram2);
    test_native();
    test_compile_speed();
    P("\n-- block exits --\n");
    test_dispatch();
    test_icache_sweep();
    P("\n-- memory paths --\n");
    test_mem_paths(ram);
    P("\n-- caches and clock --\n");
    test_cache_info();
    test_speed_modes(ram);
    free(ram);
    ram = NULL;
    test_latency_sweep();
    test_fp_ops();
    P("\n-- cores (JIT int) --\n");
    test_cores();
    free(ram2);
    // g_jit stays allocated: its pages were re-protected as executable and are not returned to the heap.
}
