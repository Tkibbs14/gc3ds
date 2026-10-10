#include "tb.h"
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- Hooks the system would provide ----
u32 g_logMask = 0;
void emu_log(LogCat cat, const char* fmt, ...) {
    if (!(g_logMask & (1u << cat))) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
void emu_error(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
static TestBus* s_tb;
void gekko_on_fatal(Gekko* g, const char* what) {
    (void)g;
    if (s_tb) {
        snprintf(s_tb->lastFatal, sizeof s_tb->lastFatal, "%s", what);
        s_tb->fatalCount++;
    }
}

// ---- Bus ----
static bool in_ram(u32 pa, u32 n) { return pa < TB_RAM_SIZE && TB_RAM_SIZE - pa >= n; }
static bool in_lc(u32 pa, u32 n) { return pa >= 0xE0000000u && pa - 0xE0000000u + n <= 16384; }

static u8 r8(void* c, u32 pa) {
    TestBus* tb = c;
    if (in_ram(pa, 1)) return tb->ram[pa];
    if (in_lc(pa, 1)) return tb->lockedCache[pa - 0xE0000000u];
    if (pa >= TB_MMIO_BASE && pa < TB_MMIO_BASE + TB_MMIO_SIZE) { tb->mmioReads++; return (u8)(tb->mmio[(pa - TB_MMIO_BASE) / 4] >> (24 - 8 * (pa & 3))); }
    return 0;
}
static u16 r16(void* c, u32 pa) { return (u16)((r8(c, pa) << 8) | r8(c, pa + 1)); }
static u32 r32(void* c, u32 pa) {
    TestBus* tb = c;
    if (pa >= TB_MMIO_BASE && pa < TB_MMIO_BASE + TB_MMIO_SIZE) { tb->mmioReads++; return tb->mmio[(pa - TB_MMIO_BASE) / 4]; }
    return ((u32)r16(c, pa) << 16) | r16(c, pa + 2);
}
static u64 r64(void* c, u32 pa) { return ((u64)r32(c, pa) << 32) | r32(c, pa + 4); }
static void w8(void* c, u32 pa, u8 v) {
    TestBus* tb = c;
    if (in_ram(pa, 1)) tb->ram[pa] = v;
    else if (in_lc(pa, 1)) tb->lockedCache[pa - 0xE0000000u] = v;
    else if (pa >= TB_MMIO_BASE && pa < TB_MMIO_BASE + TB_MMIO_SIZE) {
        tb->mmioWrites++;
        u32 sh = 24 - 8 * (pa & 3);
        u32* r = &tb->mmio[(pa - TB_MMIO_BASE) / 4];
        *r = (*r & ~(0xFFu << sh)) | ((u32)v << sh);
    }
}
static void w16(void* c, u32 pa, u16 v) { w8(c, pa, (u8)(v >> 8)); w8(c, pa + 1, (u8)v); }
static void w32(void* c, u32 pa, u32 v) {
    TestBus* tb = c;
    if (pa >= TB_MMIO_BASE && pa < TB_MMIO_BASE + TB_MMIO_SIZE) { tb->mmioWrites++; tb->mmio[(pa - TB_MMIO_BASE) / 4] = v; return; }
    w16(c, pa, (u16)(v >> 16));
    w16(c, pa + 2, (u16)v);
}
static void w64(void* c, u32 pa, u64 v) { w32(c, pa, (u32)(v >> 32)); w32(c, pa + 4, (u32)v); }
static u32 fetch(void* c, u32 pa) {
    TestBus* tb = c;
    if (pa == 0x00000C00u && tb->g && !tb->noSyscallTrap) {
        tb->hitSyscall = true;
        gekko_stop(tb->g);
        return 0x60000000u;  // nop, so SRR0/SRR1 survive until the harness services the call
    }
    return r32(c, pa);
}
// RAM is exposed by 128 KB blocks, except block 0 (vectors), which stays on the bus so the syscall trap sees fetches.
static u8* host_ptr(void* c, u32 pa, u32 len) {
    TestBus* tb = c;
    if (pa < 0x20000u) return NULL;
    if (in_ram(pa, len)) return tb->ram + pa;
    return NULL;
}
static void dcbz(void* c, u32 pa) {
    for (u32 k = 0; k < 32; k++) w8(c, pa + k, 0);
}
static void cache_op(void* c, int op, u32 pa) {
    TestBus* tb = c;
    if (op >= 0 && op < 4) tb->cacheOps[op]++;
    tb->lastCacheOpPa = pa;
}
static void lc_dma(void* c, bool toRam, u32 ramPa, u32 cachePa, u32 len) {
    TestBus* tb = c;
    tb->dmaCount++;
    tb->dmaToRam = toRam;
    tb->dmaRam = ramPa;
    tb->dmaCache = cachePa;
    tb->dmaLen = len;
}

void tb_init(TestBus* tb, Gekko* g) {
    memset(tb, 0, sizeof *tb);
    tb->ram = calloc(1, TB_RAM_SIZE);
    tb->g = g;
    GekkoBus* b = &tb->bus;
    b->ctx = tb;
    b->read8 = r8; b->read16 = r16; b->read32 = r32; b->read64 = r64;
    b->write8 = w8; b->write16 = w16; b->write32 = w32; b->write64 = w64;
    b->fetch = fetch; b->host_ptr = host_ptr; b->dcbz = dcbz; b->cache_op = cache_op; b->lc_dma = lc_dma;
    s_tb = tb;
}
void tb_free(TestBus* tb) { free(tb->ram); tb->ram = NULL; }
u32 tb_r32(TestBus* tb, u32 pa) { return r32(tb, pa); }
void tb_w32(TestBus* tb, u32 pa, u32 v) { w32(tb, pa, v); }
u64 tb_r64(TestBus* tb, u32 pa) { return r64(tb, pa); }
void tb_w64(TestBus* tb, u32 pa, u64 v) { w64(tb, pa, v); }

static u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static u16 be16(const u8* p) { return (u16)((p[0] << 8) | p[1]); }

u32 tb_load_elf(TestBus* tb, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8* d = malloc((size_t)n);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); return 0; }
    fclose(f);
    if (n < 52 || memcmp(d, "\x7f" "ELF", 4) || d[4] != 1 || d[5] != 2) { free(d); return 0; }
    u32 entry = be32(d + 24), phoff = be32(d + 28);
    u16 phentsize = be16(d + 42), phnum = be16(d + 44);
    for (u32 k = 0; k < phnum; k++) {
        const u8* ph = d + phoff + k * phentsize;
        if (be32(ph) != 1) continue;  // PT_LOAD
        u32 off = be32(ph + 4), va = be32(ph + 8), fsz = be32(ph + 16), msz = be32(ph + 20);
        if (!in_ram(va, msz)) { free(d); return 0; }
        memset(tb->ram + va, 0, msz);
        memcpy(tb->ram + va, d + off, fsz);
    }
    free(d);
    return entry;
}

bool tb_run_linux(TestBus* tb, u32 entry, u8* out, u32 outCap, u32* outLen, u64 maxCycles) {
    Gekko* g = tb->g;
    g->pc = entry;
    gekko_set_msr(g, MSR_FP);
    gekko_set_spr(g, SPR_HID2, 0xA0000000u);  // LSQE | PSE
    *outLen = 0;
    u64 start = g->cycles;
    while (g->cycles - start < maxCycles) {
        tb->hitSyscall = false;
        gekko_run(g, 100000);
        if (!tb->hitSyscall) continue;
        u32 nr = g->gpr[0];
        if (nr == 1) return true;  // exit
        if (nr == 4) {             // write(fd, buf, len)
            u32 buf = g->gpr[4], len = g->gpr[5];
            for (u32 k = 0; k < len && *outLen < outCap; k++) out[(*outLen)++] = tb->ram[buf + k];
            g->gpr[3] = len;
        }
        g->pc = g->spr[SPR_SRR0];
        gekko_set_msr(g, g->spr[SPR_SRR1]);
    }
    return false;
}
