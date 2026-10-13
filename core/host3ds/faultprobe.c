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
    u32 v = fp_load(0x50000000u);
    fprintf(f, "# fault probe: load 50000000 -> %08lx, hits %lu, type %lu, far %08lx, fsr %08lx, pc %+ld from the ldr\n",
            (unsigned long)v, (unsigned long)g_hits, (unsigned long)g_type, (unsigned long)g_far, (unsigned long)g_fsr,
            (long)(g_pc - (u32)fp_load_insn));
    u32 h0 = g_hits;
    v = fp_store(0x50000000u, 0x12345678u);
    fprintf(f, "# fault probe: store 50000000 -> %08lx, hits +%lu, far %08lx, fsr %08lx, pc %+ld from the str\n",
            (unsigned long)v, (unsigned long)(g_hits - h0), (unsigned long)g_far, (unsigned long)g_fsr,
            (long)(g_pc - (u32)fp_store_insn));
    // Cost of a fault taken and resumed.
    h0 = g_hits;
    u64 t0 = svcGetSystemTick();
    for (int i = 0; i < 200; i++) fp_load(0x50000000u + 64u * (u32)i);
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
    threadOnException(NULL, NULL, NULL);
    fprintf(f, "# fault probe: done, handler removed\n");
    fflush(f);
}
