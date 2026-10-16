// JIT glue for the PC runner. The JIT exists only on 32-bit ARM hosts (here: ARM Linux under qemu-arm); on other
// hosts host_jit_enable reports that it is unavailable.
#include <stdio.h>
#include <stdlib.h>
#include "../sys/system.h"

#if defined(__arm__)
#include <signal.h>
#include <string.h>
#include <sys/mman.h>
#include <ucontext.h>
#include "../jit/jit.h"
#include "../cpu/hle.h"

void* jit_host_alloc_exec(u32 bytes) {
    void* p = mmap(NULL, bytes, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    return p == MAP_FAILED ? NULL : p;
}

void jit_host_sync_icache(void* start, u32 bytes) { __builtin___clear_cache((char*)start, (char*)start + bytes); }

// Fault mode (jit_set_fault_mem): SIGSEGV / SIGBUS in a fault-mode site resumes in its slow path; any other fault
// goes back to the previous handler. The guest's other regions (MEM1's uncached mirror, the EFB, MMIO, the locked
// cache, physical addresses used as virtual ones) land at fixed offsets from ram; they are reserved inaccessible here
// so that an access there faults instead of reaching whatever else this process has mapped.
static struct sigaction g_oldSegv, g_oldBus;
static void on_fault(int sig, siginfo_t* si, void* ctx) {
    (void)si;
    ucontext_t* uc = ctx;
    u32 resume;
    if (jit_fault((u32)uc->uc_mcontext.arm_pc, &resume)) { uc->uc_mcontext.arm_pc = resume; return; }
    sigaction(sig, sig == SIGSEGV ? &g_oldSegv : &g_oldBus, NULL);  // not ours: fault again under the old handler
}

bool jit_host_fault_init(const u8* ram) {
    static const u8* done;
    if (done) return done == ram;
    static const struct { u32 ea, len; } kRegions[] = {
        // Fault-mode accesses clear bit 30 first (JIT_FAULT_BIC): uncached MEM1 is MEM1, the others land here.
        { 0xC8000000u & ~JIT_FAULT_BIC, 0x00400000u },  // EFB
        { 0xCC000000u & ~JIT_FAULT_BIC, 0x00010000u },  // MMIO and the write-gather pipe
        { 0xE0000000u & ~JIT_FAULT_BIC, 0x00020000u },  // locked cache (its BAT spans 128 KB)
        { 0x00000000u, 0x01800000u },  // physical addresses
    };
    for (u32 i = 0; i < sizeof kRegions / sizeof kRegions[0]; i++) {
        u32 at = (u32)(uintptr_t)ram - 0x80000000u + kRegions[i].ea, start = at & ~0xFFFu;  // pages around it
        u32 len = (at + kRegions[i].len - start + 0xFFFu) & ~0xFFFu;
        void* want = (void*)(uintptr_t)start;
        void* p = mmap(want, len, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
        if (p != want) {
            if (p != MAP_FAILED) munmap(p, len);
            fprintf(stderr, "jit: fault mode off (cannot reserve %08x for guest %08x)\n", (u32)(uintptr_t)want, kRegions[i].ea);
            return false;
        }
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_sigaction = on_fault;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigemptyset(&sa.sa_mask);
    if (sigaction(SIGSEGV, &sa, &g_oldSegv) || sigaction(SIGBUS, &sa, &g_oldBus)) return false;
    done = ram;
    return true;
}

static Jit* g_hostJit;
static void on_icbi(void* ctx, u32 pa) { jit_invalidate(ctx, pa); }

bool host_jit_enable(System* s) {
    // GC3DS_JIT_MB: the code buffer's size (the 2DS has 16 MB; 8 MB flushes more often)
    u32 mb = getenv("GC3DS_JIT_MB") ? (u32)atoi(getenv("GC3DS_JIT_MB")) : 8u;
    g_hostJit = jit_create(&s->cpu, s->mem1, MEM1_SIZE, (mb ? mb : 8u) << 20);
    if (!g_hostJit) return false;
    jit_attach(g_hostJit);
    s->icbiHook = on_icbi;
    jit_set_gather(g_hostJit, gp_wgp_put, s, GP_WGP_EA);
    jit_set_locked_cache(g_hostJit, s->lockedCache);
    jit_set_gather_buffer(g_hostJit, s->gx.wgp, &s->gx.wgpLen);
    if (getenv("GC3DS_JIT_NOLINK")) jit_set_linking(g_hostJit, false);  // comparison runs
    if (getenv("GC3DS_JIT_NOTRACE")) jit_set_traces(g_hostJit, false);
    if (getenv("GC3DS_NOHLE")) hle_set_enabled(false);  // as on the 2DS when the stand-in is off
    if (getenv("GC3DS_SLOW_INLINE")) jit_set_slow_inline(g_hostJit, true);  // the old per-site memory slow paths
    if (!getenv("GC3DS_NOFAULTMEM")) jit_set_fault_mem(g_hostJit, true);  // checked loads and stores for comparison
    jit_set_sched(g_hostJit, getenv("GC3DS_NOSCHED") ? 0 : SCHED_ALL);
    jit_set_mem_stamps(g_hostJit, &g_memStamps);  // dcbf / dcbst / dcbz stamp their page for the display-list cache
    if (getenv("GC3DS_OPT")) jit_set_opt(g_hostJit, (u32)strtoul(getenv("GC3DS_OPT"), NULL, 0));  // optimizing-JIT stages (with GC3DS_NOFAULTMEM)
    s->icbiCtx = g_hostJit;
    return true;
}

static void print_line(const char* line) { fprintf(stderr, "%s\n", line); }
void host_jit_report(void) { if (g_hostJit) jit_print_stats(g_hostJit, print_line); }
#else
bool jit_host_fault_init(const u8* ram) { (void)ram; return false; }
bool host_jit_enable(System* s) { (void)s; return false; }
void host_jit_report(void) {}
#endif
