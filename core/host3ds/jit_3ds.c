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

void* jit_host_alloc_exec(u32 bytes) {
    bytes = (bytes + 0xFFF) & ~0xFFFu;
    void* p = memalign(0x1000, bytes);
    if (!p) return NULL;
    if (!g_proc && R_FAILED(svcDuplicateHandle(&g_proc, CUR_PROCESS_HANDLE))) return NULL;
    if (R_FAILED(svcControlProcessMemory(g_proc, (u32)p, (u32)p, bytes, MEMOP_PROT,
                                         MEMPERM_READ | MEMPERM_WRITE | MEMPERM_EXECUTE))) return NULL;
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

bool jit_host_fault_init(const u8* ram) {
    if ((u32)ram + 0x01800000u > 0x40000000u) return false;
    threadOnException(on_fault, g_faultStack + sizeof g_faultStack, WRITE_DATA_TO_HANDLER_STACK);
    return true;
}
