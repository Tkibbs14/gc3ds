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
    return p;
}

void jit_host_sync_icache(void* start, u32 bytes) {
    svcFlushProcessDataCache(g_proc, (u32)start, bytes);
    luma_invalidate_entire_icache();
}
