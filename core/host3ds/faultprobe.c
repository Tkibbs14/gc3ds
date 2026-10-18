// Probe for fault-based guest memory access ("fastmem": a load or store to MEM1 inline with no checks, and a site that
// faults is patched to its slow path). Run once with --probe-faults; the answers go to perf.csv as "# fault probe" lines:
// - whether a data abort in our process reaches a handler set with threadOnException (libctru: delivery needs
//   UNITINFO or svcKernelSetState type 6, which Luma3DS is said to force on), what it reports, and whether the handler
//   can resume the faulting thread;
// - what one fault costs, and what the instruction cache invalidation a patch needs costs;
// - which host addresses the guest's non-MEM1 regions land on with base = mem1 - 0x80000000 (they must fault);
// - whether an unaligned LDR loads the right bytes (SCTLR.U) or faults (SCTLR.A).
// If delivery does not work the process dies at the first test (the default handler).
#include <3ds.h>
#include <stdio.h>
#include <string.h>

u32 fp_load(u32 addr);  // ldr r0, [r0]: the word, or 0xFFFFFFFF when it faulted (the handler resumes at fp_recover)
u32 fp_store(u32 addr, u32 v);  // str r1, [r0]: 0, or 0xFFFFFFFF when it faulted
void fp_resume(CpuRegisters* r) __attribute__((noreturn));
extern char fp_load_insn[], fp_store_insn[], fp_recover[];
__asm__(
    ".text; .arm; .align 2;"
    ".global fp_load; .type fp_load, %function;"
    "fp_load:;"
    ".global fp_load_insn; fp_load_insn: ldr r0, [r0];"
    "bx lr;"
    ".global fp_store; .type fp_store, %function;"
    "fp_store:;"
    ".global fp_store_insn; fp_store_insn: str r1, [r0];"
    "mov r0, #0;"
    "bx lr;"
    ".global fp_recover; fp_recover: mvn r0, #0;"
    "bx lr;"
    // Resumes the faulting thread from the handler's register record: flags first (user mode may write them), then
    // r0-r15 in one load (CpuRegisters is r0-r12, sp, lr, pc in order).
    ".global fp_resume; .type fp_resume, %function;"
    "fp_resume: ldr r1, [r0, #64];"
    "msr cpsr_f, r1;"
    "ldmia r0, {r0-r15};"
);

static u8 g_hstack[0x2000] __attribute__((aligned(8)));
static volatile u32 g_hits, g_far, g_fsr, g_type, g_pc;

static void handler(ERRF_ExceptionInfo* ex, CpuRegisters* r) {
    g_hits = g_hits + 1;
    g_far = ex->far;
    g_fsr = ex->fsr;
    g_type = ex->type;
    g_pc = r->pc;
    r->pc = (u32)fp_recover;
    fp_resume(r);
}

static double ns_since(u64 t0) { return (double)(svcGetSystemTick() - t0) * 1e9 / SYSCLOCK_ARM11; }

void jit_host_sync_icache(void* start, u32 bytes);

void fault_probe(FILE* f, const u8* mem1) {
    if (!f) return;
    fprintf(f, "# fault probe: mem1 at %08lx; first fault next (a crash here: no delivery)\n", (unsigned long)(u32)mem1);
    fflush(f);
    threadOnException(handler, g_hstack + sizeof g_hstack, WRITE_DATA_TO_HANDLER_STACK);
    // Every fault here lands in user space: faults above 0x40000000 crash the kernel at random, after ~220 (relay 294)
    // to ~13,000 (relay 293). The probe faults on the guard page below MEM1 (sys/system.c: no access when the kernel
    // allowed it, g_mem1Guard; else on the bit-30-cleared MMIO address, user space and free when fault mode would run).
    extern const char* g_mem1Guard;
    const u32 bad = !strcmp(g_mem1Guard, "no access") ? (u32)mem1 - 4096 : (u32)mem1 - 0x80000000u + 0x8C000000u;
    u32 v = fp_load(bad);
    fprintf(f, "# fault probe: load (user-space test page) -> %08lx, hits %lu, type %lu, far %08lx, fsr %08lx, pc %+ld from the ldr\n",
            (unsigned long)v, (unsigned long)g_hits, (unsigned long)g_type, (unsigned long)g_far, (unsigned long)g_fsr,
            (long)(g_pc - (u32)fp_load_insn));
    u32 h0 = g_hits;
    v = fp_store(bad, 0x12345678u);
    fprintf(f, "# fault probe: store (user-space test page) -> %08lx, hits +%lu, far %08lx, fsr %08lx, pc %+ld from the str\n",
            (unsigned long)v, (unsigned long)(g_hits - h0), (unsigned long)g_far, (unsigned long)g_fsr,
            (long)(g_pc - (u32)fp_store_insn));
    // Cost of a fault taken and resumed.
    h0 = g_hits;
    u64 t0 = svcGetSystemTick();
    for (int i = 0; i < 200; i++) fp_load(bad + 4u * (u32)(i % 64));
    double ns = ns_since(t0);
    fprintf(f, "# fault probe: 200 faults %.0f ns each (hits +%lu)\n", ns / 200, (unsigned long)(g_hits - h0));
    // Cost of the patch's cache maintenance (data cache flush of a word, whole instruction cache invalidation).
    static u32 word[8] __attribute__((aligned(32)));
    t0 = svcGetSystemTick();
    for (int i = 0; i < 100; i++) jit_host_sync_icache(word, 4);
    fprintf(f, "# fault probe: instruction cache sync %.0f ns each\n", ns_since(t0) / 100);
    // The guest's other regions through base = mem1 - 0x80000000 (guest address -> host address).
    static const struct { const char* name; u32 ea; } kEa[] = {
        { "MEM1", 0x80001000u }, { "MEM1 end", 0x817FFFFCu }, { "past MEM1", 0x81800000u }, { "past MEM1 +1M", 0x81900000u },
        { "uncached MEM1", 0xC0001000u }, { "EFB", 0xC8000000u }, { "MMIO", 0xCC003000u }, { "gather pipe", 0xCC008000u },
        { "locked cache", 0xE0000000u }, { "guest 0", 0x00000000u }, { "guest 01000000", 0x01000000u },
        { "guest 7FFFF000", 0x7FFFF000u },
    };
    fprintf(f, "# fault probe: regions (guest ea -> host):");
    for (u32 i = 0; i < sizeof kEa / sizeof kEa[0]; i++) {
        u32 host = (u32)mem1 - 0x80000000u + kEa[i].ea;
        if (host >= 0x40000000u) { fprintf(f, " %s %08lx not touched (above 0x40000000);", kEa[i].name, (unsigned long)host); continue; }
        h0 = g_hits;
        fp_load(host);
        fprintf(f, " %s %08lx %s;", kEa[i].name, (unsigned long)host, g_hits != h0 ? "faults" : "MAPPED");
    }
    fprintf(f, "\n");
    // Unaligned word load: 05040302 when SCTLR.U is set, 01040302 (rotated) in the legacy mode, a fault with SCTLR.A.
    static u8 buf[16] __attribute__((aligned(8))) = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    h0 = g_hits;
    v = fp_load((u32)(buf + 1));
    u32 v3 = fp_load((u32)(buf + 3));
    fprintf(f, "# fault probe: unaligned ldr +1 -> %08lx, +3 -> %08lx (want 05040302, 07060504), faults %lu\n",
            (unsigned long)v, (unsigned long)v3, (unsigned long)(g_hits - h0));
    // Stress (2026-10-16): fault mode crashed the kernel every few hundred to few thousand faults, a data abort in
    // Luma's exception page at the faulting guest address (541ab000: above 0x40000000, outside user space). Is it the
    // address? First faults on MEM1's guard page (user space, no access), then the same number above 0x40000000. The
    // count goes to the log every 1,000, so a crash shows how far each got.
    extern const char* g_mem1Guard;
    // Relay 293: the guard page took 20,000 faults; above 0x40000000 the kernel went down after ~13,000. Fault mode now
    // clears bit 30 (jit.h JIT_FAULT_BIC), so its non-MEM1 faults land where the last two entries point (free user space).
    static const struct { const char* name; u32 base, span; } kStress[] = {
        { "guard page (user space, no access)", 0, 0 },
        { "MMIO with bit 30 cleared (fault mode's gather-pipe and register sites)", 0x8C000000u, 0x00010000u },
        { "EFB with bit 30 cleared", 0x88000000u, 0x00400000u },
    };
    for (u32 s = 0; s < 3; s++) {
        u32 base = (u32)mem1 - 0x80000000u + kStress[s].base, span = kStress[s].span;
        if (s == 0) {
            if (strcmp(g_mem1Guard, "no access")) { fprintf(f, "# fault probe stress: guard pages %s, skipped\n", g_mem1Guard); continue; }
            base = (u32)mem1 - 4096; span = 4096;  // the guard below MEM1
        }
        h0 = g_hits;
        fp_load(base);
        if (g_hits == h0) { fprintf(f, "# fault probe stress: %s at %08lx is MAPPED: skipped\n", kStress[s].name, (unsigned long)base); continue; }
        fprintf(f, "# fault probe stress: %s at %08lx, 20000 faults\n", kStress[s].name, (unsigned long)base);
        fflush(f);
        h0 = g_hits;
        t0 = svcGetSystemTick();
        for (u32 i = 0; i < 20000; i++) {
            fp_load(base + ((i * 2654435761u) % span & ~3u));
            if ((i + 1) % 1000 == 0) { fprintf(f, "# fault probe stress: %lu\n", (unsigned long)(i + 1)); fflush(f); }
        }
        fprintf(f, "# fault probe stress: %s survived, hits +%lu, %.0f ns each\n", kStress[s].name,
                (unsigned long)(g_hits - h0), ns_since(t0) / 20000);
        fflush(f);
    }
    threadOnException(NULL, NULL, NULL);
    fprintf(f, "# fault probe: done, handler removed\n");
    fflush(f);
}

// Luma3DS's physical-memory mirror (k11_extension main.c: PA 0x00000000-0x30000000 at VA | 0x80000000, a supersection
// of strongly-ordered memory, "rwx for all"): read-only checks, one load each, under the probe's handler (a fault above
// 0x40000000 has crashed the kernel after a few hundred, so this takes at most a handful). (1) a linear buffer read
// through the mirror; (2) a store through the mirror read back through the buffer; (3) the New 3DS L2C-310's registers
// (PA 0x17E10000, 3dbrew "Memory layout"): ID, type, control, auxiliary control, latencies, prefetch and power control.
void mirror_probe(FILE* f) {
    if (!f) return;
    fprintf(f, "# mirror probe: start (a crash here: the mirror is not mapped)\n");
    fflush(f);
    threadOnException(handler, g_hstack + sizeof g_hstack, WRITE_DATA_TO_HANDLER_STACK);
    u32* buf = linearAlloc(64);
    if (!buf) { fprintf(f, "# mirror probe: no linear memory\n"); threadOnException(NULL, NULL, NULL); return; }
    buf[0] = 0xC0FFEE11u;
    buf[1] = 0;
    svcFlushProcessDataCache(CUR_PROCESS_HANDLE, (u32)buf, 64);
    u32 pa = osConvertVirtToPhys(buf), h0 = g_hits;
    u32 v = pa ? fp_load(0x80000000u | pa) : 0;
    fprintf(f, "# mirror probe: buffer va %08lx pa %08lx; read through %08lx -> %08lx (%s), faults %lu, fsr %08lx\n",
            (unsigned long)(u32)buf, (unsigned long)pa, (unsigned long)(0x80000000u | pa), (unsigned long)v,
            v == 0xC0FFEE11u ? "matches" : "differs", (unsigned long)(g_hits - h0), (unsigned long)g_fsr);
    fflush(f);
    if (g_hits == h0 && v == 0xC0FFEE11u) {
        u32 r = fp_store(0x80000000u | (pa + 4), 0x5A5A1234u);
        svcInvalidateProcessDataCache(CUR_PROCESS_HANDLE, (u32)buf, 64);
        fprintf(f, "# mirror probe: store through the mirror -> %08lx; buffer word now %08lx (%s)\n", (unsigned long)r,
                (unsigned long)buf[1], buf[1] == 0x5A5A1234u ? "written" : "not written");
        static const u16 kRegs[] = { 0x000, 0x004, 0x100, 0x104, 0x108, 0x10C, 0xC00, 0xF40, 0xF60, 0xF80 };
        static const char* const kNames[] = { "id", "type", "control", "aux", "tag_lat", "data_lat", "filter_start",
                                              "debug", "prefetch", "power" };
        fprintf(f, "# mirror probe: L2C-310 at 0x97E10000:");
        for (u32 i = 0; i < sizeof kRegs / sizeof kRegs[0]; i++) {
            h0 = g_hits;
            u32 x = fp_load(0x97E10000u + kRegs[i]);
            fprintf(f, " %s %08lx%s", kNames[i], (unsigned long)x, g_hits != h0 ? "(fault)" : "");
            if (g_hits != h0) break;  // (no more faults above 0x40000000)
        }
        fprintf(f, "\n");
    }
    linearFree(buf);
    threadOnException(NULL, NULL, NULL);
    fprintf(f, "# mirror probe: done\n");
    fflush(f);
}
