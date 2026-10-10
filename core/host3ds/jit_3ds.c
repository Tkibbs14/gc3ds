// JIT host services on the 3DS: executable memory through svcControlProcessMemory (Luma3DS allows RWX for
// homebrew) and cache maintenance through svcFlushProcessDataCache plus a kernel-mode instruction-cache
// invalidate (svcBackdoor). gcbench measured both on a New 2DS XL: stale code without the sync, 2.8 us per sync.
#include <3ds.h>
#define GC3DS_HOST_TYPES
#include <malloc.h>
#include "../src/jit/jit.h"

static Handle g_proc;

static s32 kernel_sync_icache(void) {
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

void* jit_host_alloc_exec(u32 bytes) {
    bytes = (bytes + 0xFFF) & ~0xFFFu;
    void* p = memalign(0x1000, bytes);
    if (!p) return NULL;
    if (!g_proc && R_FAILED(svcDuplicateHandle(&g_proc, CUR_PROCESS_HANDLE))) return NULL;
    if (R_FAILED(svcControlProcessMemory(g_proc, (u32)p, (u32)p, bytes, MEMOP_PROT,
                                         MEMPERM_READ | MEMPERM_WRITE | MEMPERM_EXECUTE))) return NULL;
    return p;
}

void jit_host_sync_icache(void* start, u32 bytes) {
    svcFlushProcessDataCache(g_proc, (u32)start, bytes);
    svcBackdoor(kernel_sync_icache);
}
