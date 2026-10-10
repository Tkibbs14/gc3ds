// JIT host services on the 3DS: executable memory through svcControlProcessMemory (Luma3DS allows RWX for
// homebrew) and cache maintenance through Luma3DS's kernel-extension SVCs:
//   0x91 svcFlushDataCacheRange(addr, len)            (L2 included)
//   0x93 svcInvalidateInstructionCacheRange(addr, len)
// (numbers from Luma3DS sysmodules/rosalina/source/csvc.s). An earlier version ran its own cache routine in kernel
// mode through svcBackdoor; a kernel-mode prefetch abort followed on hardware (2026-10-10), so it is not used.
#include <3ds.h>
#define GC3DS_HOST_TYPES
#include <malloc.h>
#include "../src/jit/jit.h"

static Handle g_proc;

static void __attribute__((naked, noinline)) luma_flush_dcache_range(void* addr, u32 len) {
    (void)addr; (void)len;
    __asm__ volatile("svc 0x91\n\tbx lr\n");
}

static void __attribute__((naked, noinline)) luma_invalidate_icache_range(void* addr, u32 len) {
    (void)addr; (void)len;
    __asm__ volatile("svc 0x93\n\tbx lr\n");
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
    luma_flush_dcache_range(start, bytes);
    luma_invalidate_icache_range(start, bytes);
}
