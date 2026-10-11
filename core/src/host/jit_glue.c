// JIT glue for the PC runner. The JIT exists only on 32-bit ARM hosts (here: ARM Linux under qemu-arm); on other
// hosts host_jit_enable reports that it is unavailable.
#include <stdio.h>
#include <stdlib.h>
#include "../sys/system.h"

#if defined(__arm__)
#include <sys/mman.h>
#include "../jit/jit.h"

void* jit_host_alloc_exec(u32 bytes) {
    void* p = mmap(NULL, bytes, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

void jit_host_sync_icache(void* start, u32 bytes) { __builtin___clear_cache((char*)start, (char*)start + bytes); }

static Jit* g_hostJit;
static void on_icbi(void* ctx, u32 pa) { jit_invalidate(ctx, pa); }

bool host_jit_enable(System* s) {
    g_hostJit = jit_create(&s->cpu, s->mem1, MEM1_SIZE, 8u << 20);
    if (!g_hostJit) return false;
    jit_attach(g_hostJit);
    s->icbiHook = on_icbi;
    jit_set_gather(g_hostJit, gp_wgp_put, s, GP_WGP_EA);
    jit_set_gather_buffer(g_hostJit, s->gx.wgp, &s->gx.wgpLen);
    if (getenv("GC3DS_JIT_NOLINK")) jit_set_linking(g_hostJit, false);  // comparison runs
    s->icbiCtx = g_hostJit;
    return true;
}

static void print_line(const char* line) { fprintf(stderr, "%s\n", line); }
void host_jit_report(void) { if (g_hostJit) jit_print_stats(g_hostJit, print_line); }
#else
bool host_jit_enable(System* s) { (void)s; return false; }
void host_jit_report(void) {}
#endif
