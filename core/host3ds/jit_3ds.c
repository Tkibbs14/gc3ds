// JIT host services on the 3DS: executable memory through svcControlProcessMemory (Luma3DS allows RWX for
// homebrew) and cache maintenance that never hands our virtual addresses to another core:
//   svcFlushProcessDataCache(process, addr, len)  - official SVC, resolved in our own process
//   svc 0x94 svcInvalidateEntireInstructionCache  - Luma3DS kernel extension (sysmodules/rosalina/source/csvc.s)
// History (New 2DS XL, 2026-10-10): running our own cache routine through svcBackdoor ended in a kernel prefetch
// abort; Luma's ranged SVCs 0x91/0x93 ended in a data abort in gsp on core 1 at our JIT buffer address (the range
// operation reached a core running another process). Neither is used.
#include <3ds.h>
#define GC3DS_HOST_TYPES
#include <malloc.h>
#include "crashlog.h"
#include <string.h>
#include "../src/jit/jit.h"

static Handle g_proc;

static void __attribute__((naked, noinline)) luma_invalidate_entire_icache(void) {
    __asm__ volatile("svc 0x94\n\tbx lr\n");
}

// GC3DS_CODE_ALIGN (EXTRA_CFLAGS=-DGC3DS_CODE_ALIGN=bytes): the buffer's alignment, 4 KB by default (a build option: the
// buffer is made at startup, before a job's options). The kernel can map an aligned range with 64 KB large pages
// or 1 MB sections, each one micro-TLB entry instead of 16 or 256 (ARM11 MPCore TRM: the instruction micro-TLB has
// 10 entries); TP's instruction micro-TLB misses moved 2-3x between builds that only moved the heap (jobs 967 vs 979).
#ifndef GC3DS_CODE_ALIGN
#define GC3DS_CODE_ALIGN 0x1000
#endif
u32 jit_host_exec_align = GC3DS_CODE_ALIGN, jit_host_exec_base;
// Large data tables (fault mode's 2 MB site table) go to linear memory: the heap had 0.2-0.5 MB free in gameplay, and
// the fault table taken from it starved the relay's TLS reconnects ("relay error", jobs 1058 and 1060); the linear heap
// had 7-12 MB free. Never freed (the tables live as long as the process).
void* jit_host_alloc_data(u32 bytes) {
    void* p = linearAlloc(bytes);
    return p ? p : malloc(bytes);
}

void* jit_host_alloc_exec(u32 bytes) {
    u32 align = jit_host_exec_align < 0x1000 ? 0x1000 : jit_host_exec_align;
    bytes = (bytes + align - 1) & ~(align - 1);
    void* p = memalign(align, bytes);
    if (!p) return NULL;
    if (!g_proc && R_FAILED(svcDuplicateHandle(&g_proc, CUR_PROCESS_HANDLE))) return NULL;
    if (R_FAILED(svcControlProcessMemory(g_proc, (u32)p, (u32)p, bytes, MEMOP_PROT,
                                         MEMPERM_READ | MEMPERM_WRITE | MEMPERM_EXECUTE))) return NULL;
    jit_host_exec_base = (u32)p;
    crashlog_set_info(CRASH_INFO_JIT_START, (u32)p);
    crashlog_set_info(CRASH_INFO_JIT_END, (u32)p + bytes);
    return p;
}

void jit_host_sync_icache(void* start, u32 bytes) {
    svcFlushProcessDataCache(g_proc, (u32)start, bytes);
    luma_invalidate_entire_icache();
}

// Fault mode (jit_set_fault_mem): a data abort in a fault-mode site resumes in its slow path. The handler is set with
// threadOnException for the thread that runs compiled code (jit_set_fault_mem is called there) and runs on its own
// stack; it resumes the thread itself (flags, then r0-r15 in one load). Any other abort clears the handler and
// re-runs the instruction, which then faults the default way. With ram below 0x40000000 - MEM1, every non-MEM1 guest
// region lands at 0x40000000 or above, which user mode cannot access (3dbrew, "Memory layout"), so nothing is reserved.
// Except 0x80000000-0xAFFFFFFF: Luma3DS maps physical memory there for everyone (uncached; job 443 read and wrote it),
// so a guest address that lands there reads unrelated physical memory instead of faulting.
static u8 g_faultStack[0x2000] __attribute__((aligned(8)));
void jit_fault_resume(CpuRegisters* r) __attribute__((noreturn));
__asm__(".text; .arm; .align 2; .global jit_fault_resume; .type jit_fault_resume, %function;"
        "jit_fault_resume: ldr r1, [r0, #64]; msr cpsr_f, r1; ldmia r0, {r0-r15};");

static void on_fault(ERRF_ExceptionInfo* ex, CpuRegisters* r) {
    u32 resume;
    if (ex->type == ERRF_EXCEPTION_DATA_ABORT && jit_fault(r->pc, &resume)) {
        r->pc = resume;
        jit_fault_resume(r);
    }
    crashlog_record(ex, r, CRASH_MAIN);  // not a fault-mode site: recorded, then the default handling
    threadOnException(NULL, NULL, NULL);
    jit_fault_resume(r);
}

// MEM1's buffer between two no-access guard regions (sys/system.c): heap pages whose permission is taken away, and
// given back before the memory is freed. g_mem1Guard says whether the kernel accepted it (perf.csv header).
const char* g_mem1Guard = "not set";
u8* host_mem1_alloc(u32 size, u32 guard) {
    u8* p = memalign(0x1000, size + 2 * guard);
    if (!p) return NULL;
    memset(p, 0, size + 2 * guard);
    if (!g_proc && R_FAILED(svcDuplicateHandle(&g_proc, CUR_PROCESS_HANDLE))) return p + guard;
    bool none = R_SUCCEEDED(svcControlProcessMemory(g_proc, (u32)p, (u32)p, guard, MEMOP_PROT, 0)) &&
                R_SUCCEEDED(svcControlProcessMemory(g_proc, (u32)(p + guard + size), (u32)(p + guard + size), guard, MEMOP_PROT, 0));
    g_mem1Guard = none ? "no access" : "refused";
    return p + guard;
}
void host_mem1_free(u8* mem1, u32 size, u32 guard) {
    if (!mem1) return;
    u8* p = mem1 - guard;
    if (g_proc) {
        svcControlProcessMemory(g_proc, (u32)p, (u32)p, guard, MEMOP_PROT, MEMPERM_READ | MEMPERM_WRITE);
        svcControlProcessMemory(g_proc, (u32)(mem1 + size), (u32)(mem1 + size), guard, MEMOP_PROT, MEMPERM_READ | MEMPERM_WRITE);
    }
    free(p);
}

// Where the kernel puts each fault's data. Exception delivery to a user handler is Luma3DS's (libctru thread.h: retail
// kernels need UNITINFO / svcKernelSetState 6), and its kernel crashed in fault mode every few thousand faults (Luma
// ARM11 dump 11, 2026-10-16: SVC mode, lr ffff0260 in the exception page, data abort at 541ab000; relay 253-254).
// --faultdata fixed: a fixed buffer instead of the handler's stack, another path through that code (the test).
bool jit_fault_fixed_data;
static ERRF_ExceptionData g_faultData __attribute__((aligned(8)));

// Fault mode's accesses clear bit 30 of the guest address (JIT_FAULT_BIC): the regions other than MEM1 then land in
// user space, where faults are delivered safely (relay 293: 20,000 faults on a user-space page with no access went
// through; faults above 0x40000000 took the kernel down after ~13,000). Each region's host range must be free (not
// mapped) or an access there would read or write whatever is mapped: libctru maps shared memory at 0x10000000 and up.
// g_faultRegions: what was checked (perf.csv header); fault mode stays off when a range is in use.
const char* g_faultRegions = "not checked";
static bool range_free(u32 addr, u32 len) {
    MemInfo mi;
    PageInfo pi;
    if (R_FAILED(svcQueryMemory(&mi, &pi, addr))) return false;
    return mi.state == MEMSTATE_FREE && mi.base_addr <= addr && addr + len <= mi.base_addr + mi.size;
}

bool jit_host_fault_init(const u8* ram) {
    if ((u32)ram + 0x01800000u > 0x40000000u) return false;
    static char desc[160];
    static const struct { const char* name; u32 ea, len; } kRegions[] = {
        { "EFB", 0xC8000000u, 0x00400000u }, { "MMIO", 0xCC000000u, 0x00010000u }, { "locked cache", 0xE0000000u, 0x00020000u },
    };
    u32 n = 0;
    bool ok = true;
    for (u32 i = 0; i < sizeof kRegions / sizeof kRegions[0]; i++) {
        u32 host = (u32)ram - 0x80000000u + (kRegions[i].ea & ~JIT_FAULT_BIC);
        bool f = host + kRegions[i].len <= 0x40000000u && range_free(host, kRegions[i].len);
        ok = ok && f;
        n += (u32)snprintf(desc + n, sizeof desc - n, "%s%s %08lx %s", i ? ", " : "", kRegions[i].name, (unsigned long)host, f ? "free" : "IN USE");
        if (n >= sizeof desc) break;
    }
    g_faultRegions = desc;
    if (!ok) return false;
    threadOnException(on_fault, g_faultStack + sizeof g_faultStack, jit_fault_fixed_data ? &g_faultData : WRITE_DATA_TO_HANDLER_STACK);
    return true;
}
