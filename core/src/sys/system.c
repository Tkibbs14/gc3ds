#include "system.h"
#include "../cpu/hle.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

u64 g_hostProf[PROF_N];
bool g_drawTimed = true;
u32 g_drawScale = 1;
u64 (*g_profClock)(void);

// ---- physical bus ----

static void unknown_access(System* s, const char* what, u32 pa, u32 v) {
    if (s->unknownMmioCount++ < 200) emu_log(LOG_MEM, "unmapped %s pa=%08x v=%08x pc=%08x\n", what, pa, v, s->cpu.pc);
}

static bool in_mem1(u32 pa, u32 len) { return pa < PA_MEM1_END && pa + len <= PA_MEM1_END; }
static bool in_lc(u32 pa, u32 len) { return pa >= PA_LOCKED_CACHE && pa + len <= PA_LOCKED_CACHE + LOCKED_CACHE_SIZE; }

// Hardware blocks are 16-bit or 32-bit wide; other widths are composed from the native one.
typedef enum { W16, W32 } Width;

static Width block_width(u32 off) {
    u32 b = off & 0xFC00;
    if (b < 0x3000 || (b >= 0x4000 && b < 0x6000)) return W16;  // CP, PE, VI, MI, DSP
    return W32;                                                // PI, DI, SI, EXI, AI
}

static u16 mmio_read16_native(System* s, u32 off) {
    if (off < 0x1000) return cp_read16(s, off & 0xFFF);
    if (off < 0x2000) return pe_read16(s, off & 0xFFF);
    if (off < 0x3000) return vi_read16(s, off & 0xFFF);
    if (off >= 0x4000 && off < 0x5000) return mi_read16(s, off & 0xFFF);
    if (off >= 0x5000 && off < 0x6000) return dsp_read16(s, off & 0xFFF);
    unknown_access(s, "read16", PA_MMIO + off, 0);
    return 0;
}

static void mmio_write16_native(System* s, u32 off, u16 v) {
    if (off < 0x1000) cp_write16(s, off & 0xFFF, v);
    else if (off < 0x2000) pe_write16(s, off & 0xFFF, v);
    else if (off < 0x3000) vi_write16(s, off & 0xFFF, v);
    else if (off >= 0x4000 && off < 0x5000) mi_write16(s, off & 0xFFF, v);
    else if (off >= 0x5000 && off < 0x6000) dsp_write16(s, off & 0xFFF, v);
    else unknown_access(s, "write16", PA_MMIO + off, v);
}

static u32 mmio_read32_native(System* s, u32 off) {
    if (off >= 0x3000 && off < 0x4000) return pi_read32(s, off & 0xFFF);
    if (off >= 0x6000 && off < 0x6400) return di_read32(s, off & 0x3FF);
    if (off >= 0x6400 && off < 0x6800) return si_read32(s, off & 0x3FF);
    if (off >= 0x6800 && off < 0x6C00) return exi_read32(s, off & 0x3FF);
    if (off >= 0x6C00 && off < 0x7000) return ai_read32(s, off & 0x3FF);
    unknown_access(s, "read32", PA_MMIO + off, 0);
    return 0;
}

static void mmio_write32_native(System* s, u32 off, u32 v) {
    if (off >= 0x3000 && off < 0x4000) pi_write32(s, off & 0xFFF, v);
    else if (off >= 0x6000 && off < 0x6400) di_write32(s, off & 0x3FF, v);
    else if (off >= 0x6400 && off < 0x6800) si_write32(s, off & 0x3FF, v);
    else if (off >= 0x6800 && off < 0x6C00) exi_write32(s, off & 0x3FF, v);
    else if (off >= 0x6C00 && off < 0x7000) ai_write32(s, off & 0x3FF, v);
    else unknown_access(s, "write32", PA_MMIO + off, v);
}

static u32 mmio_read(System* s, u32 pa, int size) {
    u32 off = pa - PA_MMIO;
    if (block_width(off) == W16) {
        if (size == 4) return ((u32)mmio_read16_native(s, off) << 16) | mmio_read16_native(s, off + 2);
        u16 h = mmio_read16_native(s, off & ~1u);
        return size == 2 ? h : (off & 1) ? (h & 0xFF) : (h >> 8);
    }
    u32 w = mmio_read32_native(s, off & ~3u);
    if (size == 4) return w;
    if (size == 2) return (off & 2) ? (w & 0xFFFF) : (w >> 16);
    return (w >> (24 - 8 * (off & 3))) & 0xFF;
}

static void mmio_write(System* s, u32 pa, u32 v, int size) {
    u32 off = pa - PA_MMIO;
    if (off >= 0x8000 && off < 0x9000) {  // write-gather pipe
        u8 b[4];
        for (int i = 0; i < size; i++) b[i] = (u8)(v >> (8 * (size - 1 - i)));
        gp_fifo_write(s, b, (u32)size);
        return;
    }
    if (block_width(off) == W16) {
        if (size == 4) { mmio_write16_native(s, off, (u16)(v >> 16)); mmio_write16_native(s, off + 2, (u16)v); }
        else if (size == 2) mmio_write16_native(s, off, (u16)v);
        else unknown_access(s, "write8", pa, v);
        return;
    }
    if (size == 4) mmio_write32_native(s, off, v);
    else {
        // Sub-word writes to 32-bit registers: read-modify-write of the containing word.
        u32 base = off & ~3u, w = mmio_read32_native(s, base);
        u32 shift = size == 2 ? ((off & 2) ? 0 : 16) : 24 - 8 * (off & 3);
        u32 mask = (size == 2 ? 0xFFFFu : 0xFFu) << shift;
        mmio_write32_native(s, base, (w & ~mask) | ((v << shift) & mask));
    }
}

#define S ((System*)ctx)

static u8 bus_read8(void* ctx, u32 pa) {
    if (in_mem1(pa, 1)) return S->mem1[pa];
    if (in_lc(pa, 1)) return S->lockedCache[pa - PA_LOCKED_CACHE];
    if ((pa & 0xFFFF0000u) == PA_MMIO) return (u8)mmio_read(S, pa, 1);
    unknown_access(S, "read8", pa, 0);
    return 0;
}
static u16 bus_read16(void* ctx, u32 pa) {
    u16 v;
    if (in_mem1(pa, 2)) { memcpy(&v, S->mem1 + pa, 2); return bswap16(v); }
    if (in_lc(pa, 2)) { memcpy(&v, S->lockedCache + pa - PA_LOCKED_CACHE, 2); return bswap16(v); }
    if ((pa & 0xFFFF0000u) == PA_MMIO) return (u16)mmio_read(S, pa, 2);
    unknown_access(S, "read16", pa, 0);
    return 0;
}
static u32 bus_read32(void* ctx, u32 pa) {
    u32 v;
    if (in_mem1(pa, 4)) { memcpy(&v, S->mem1 + pa, 4); return bswap32(v); }
    if (in_lc(pa, 4)) { memcpy(&v, S->lockedCache + pa - PA_LOCKED_CACHE, 4); return bswap32(v); }
    if ((pa & 0xFFFF0000u) == PA_MMIO) return mmio_read(S, pa, 4);
    if (pa >= PA_EFB && pa < PA_MMIO) return 0;  // EFB peeks: not emulated yet
    unknown_access(S, "read32", pa, 0);
    return 0;
}
static u64 bus_read64(void* ctx, u32 pa) { return ((u64)bus_read32(ctx, pa) << 32) | bus_read32(ctx, pa + 4); }

static void bus_write8(void* ctx, u32 pa, u8 v) {
    if (in_mem1(pa, 1)) { S->mem1[pa] = v; return; }
    if (in_lc(pa, 1)) { S->lockedCache[pa - PA_LOCKED_CACHE] = v; return; }
    if ((pa & 0xFFFF0000u) == PA_MMIO) { mmio_write(S, pa, v, 1); return; }
    unknown_access(S, "write8", pa, v);
}
static void bus_write16(void* ctx, u32 pa, u16 v) {
    u16 be = bswap16(v);
    if (in_mem1(pa, 2)) { memcpy(S->mem1 + pa, &be, 2); return; }
    if (in_lc(pa, 2)) { memcpy(S->lockedCache + pa - PA_LOCKED_CACHE, &be, 2); return; }
    if ((pa & 0xFFFF0000u) == PA_MMIO) { mmio_write(S, pa, v, 2); return; }
    unknown_access(S, "write16", pa, v);
}
static void bus_write32(void* ctx, u32 pa, u32 v) {
    u32 be = bswap32(v);
    if (in_mem1(pa, 4)) { memcpy(S->mem1 + pa, &be, 4); return; }
    if (in_lc(pa, 4)) { memcpy(S->lockedCache + pa - PA_LOCKED_CACHE, &be, 4); return; }
    if ((pa & 0xFFFF0000u) == PA_MMIO) { mmio_write(S, pa, v, 4); return; }
    if (pa >= PA_EFB && pa < PA_MMIO) return;  // EFB pokes: not emulated yet
    unknown_access(S, "write32", pa, v);
}
static void bus_write64(void* ctx, u32 pa, u64 v) {
    // The write-gather pipe takes 64-bit stores (stfd) as two words, in order.
    bus_write32(ctx, pa, (u32)(v >> 32));
    bus_write32(ctx, (pa & 0xFFFF8000u) == PA_GP_FIFO ? pa : pa + 4, (u32)v);
}
static u32 bus_fetch(void* ctx, u32 pa) {
    if (in_mem1(pa, 4)) { u32 v; memcpy(&v, S->mem1 + pa, 4); return bswap32(v); }
    if (in_lc(pa, 4)) { u32 v; memcpy(&v, S->lockedCache + pa - PA_LOCKED_CACHE, 4); return bswap32(v); }
    char msg[64];
    snprintf(msg, sizeof msg, "instruction fetch from unmapped pa %08x", pa);
    gekko_on_fatal(&S->cpu, msg);
    return 0;
}
static u8* bus_host_ptr(void* ctx, u32 pa, u32 len) {
    if (in_mem1(pa, len)) return S->mem1 + pa;
    if (in_lc(pa, len)) return S->lockedCache + pa - PA_LOCKED_CACHE;
    return NULL;
}
static void bus_dcbz(void* ctx, u32 pa) {
    u8* p = bus_host_ptr(ctx, pa & ~31u, 32);
    if (p) memset(p, 0, 32);
}
// No data cache model; instruction-cache invalidation is forwarded to the JIT (code at pa may have changed).
static void bus_cache_op(void* ctx, int op, u32 pa) {
    if (op != CACHE_ICBI) return;
    hle_invalidate(pa);
    if (S->icbiHook) S->icbiHook(S->icbiCtx, pa);
}
static void bus_lc_dma(void* ctx, bool toRam, u32 ramPa, u32 cachePa, u32 len) {
    u8* r = bus_host_ptr(ctx, ramPa, len);
    u8* c = bus_host_ptr(ctx, cachePa, len);
    if (!r || !c) { unknown_access(S, "locked-cache DMA", ramPa, cachePa); return; }
    if (toRam) memmove(r, c, len); else memmove(c, r, len);
}
#undef S

// ---- scheduler ----

static void recompute_next(System* s) {
    u64 n = UINT64_MAX;
    for (int i = 0; i < MAX_EVENTS; i++) if (s->events[i].used && s->events[i].when < n) n = s->events[i].when;
    s->nextEvent = n;
}

void sched_add(System* s, u64 when, EventFn fn, u32 arg) {
    for (int i = 0; i < MAX_EVENTS; i++) {
        if (!s->events[i].used) {
            s->events[i] = (Event){ when, fn, arg, true };
            if (when < s->nextEvent) { s->nextEvent = when; gekko_stop(&s->cpu); }
            return;
        }
    }
    emu_error("scheduler full\n");
}

void sched_remove(System* s, EventFn fn) {
    for (int i = 0; i < MAX_EVENTS; i++) if (s->events[i].used && s->events[i].fn == fn) s->events[i].used = false;
    recompute_next(s);
}

static void run_due_events(System* s) {
    for (;;) {
        u64 now = sys_now(s);
        int due = -1;
        for (int i = 0; i < MAX_EVENTS; i++)
            if (s->events[i].used && s->events[i].when <= now && (due < 0 || s->events[i].when < s->events[due].when)) due = i;
        if (due < 0) break;
        Event ev = s->events[due];
        s->events[due].used = false;
        ev.fn(s, now, ev.arg);
    }
    recompute_next(s);
}

// ---- interrupts ----

void pi_update(System* s) { gekko_set_external_irq(&s->cpu, (s->pi.intsr & s->pi.intmr) != 0); }
void pi_raise(System* s, u32 cause) { s->pi.intsr |= cause; pi_update(s); }
void pi_clear(System* s, u32 cause) { s->pi.intsr &= ~cause; pi_update(s); }

u32 pi_read32(System* s, u32 off) {
    switch (off) {
    case 0x00: return s->pi.intsr | (1u << 16);  // bit 16: reset switch released
    case 0x04: return s->pi.intmr;
    case 0x0C: return s->pi.fifoBase;
    case 0x10: return s->pi.fifoEnd;
    case 0x14: return s->pi.fifoWrite;
    case 0x24: return s->pi.resetCode;
    case 0x2C: return 0x246500B1;  // console revision (retail GameCube, as libogc expects a non-zero value)
    default: emu_log(LOG_HW, "PI read %03x\n", off); return 0;
    }
}

void pi_write32(System* s, u32 off, u32 v) {
    switch (off) {
    case 0x00: s->pi.intsr &= ~(v & (PI_INT_RSW | PI_INT_GPERR)); pi_update(s); break;  // only these are cleared here
    case 0x04: s->pi.intmr = v; pi_update(s); break;
    case 0x0C: s->pi.fifoBase = v & 0x03FFFFE0; break;
    case 0x10: s->pi.fifoEnd = v & 0x03FFFFE0; break;
    case 0x14: s->pi.fifoWrite = v & 0x03FFFFE0; break;
    case 0x18: break;  // FIFO reset
    case 0x24:  // reset control: bit 2 low holds the disc drive in reset (DVDLowReset pulses it)
        if (s->pi.resetWritten && !(s->pi.resetCode & 4) && (v & 4)) di_reset_drive(s);
        s->pi.resetCode = v;
        s->pi.resetWritten = true;
        break;
    default: emu_log(LOG_HW, "PI write %03x = %08x\n", off, v); break;
    }
}

// MI (memory protection): accepted and ignored.
u16 mi_read16(System* s, u32 off) { (void)s; (void)off; return 0; }
void mi_write16(System* s, u32 off, u16 v) { (void)s; (void)off; (void)v; }

// ---- lifecycle ----

bool sys_init(System* s) {
    memset(s, 0, sizeof *s);
    s->mem1 = calloc(1, MEM1_SIZE);
    s->lockedCache = calloc(1, LOCKED_CACHE_SIZE);
    s->dsp.aram = calloc(1, 16u * 1024 * 1024);
    if (!s->mem1 || !s->lockedCache || !s->dsp.aram) return false;
    hle_set_memory(s->mem1, MEM1_SIZE);
    s->bus = (GekkoBus){
        .ctx = s,
        .read8 = bus_read8, .read16 = bus_read16, .read32 = bus_read32, .read64 = bus_read64,
        .write8 = bus_write8, .write16 = bus_write16, .write32 = bus_write32, .write64 = bus_write64,
        .fetch = bus_fetch, .host_ptr = bus_host_ptr, .dcbz = bus_dcbz, .cache_op = bus_cache_op, .lc_dma = bus_lc_dma,
    };
    if (!gekko_init(&s->cpu, &s->bus)) return false;
    gekko_set_poll_ram(&s->cpu, MEM1_SIZE);
    sys_reset(s);
    return true;
}

void sys_free(System* s) {
    gekko_free(&s->cpu);
    gxcore_free(&s->gx.core);
    free(s->mem1);
    free(s->lockedCache);
    free(s->dsp.aram);
}

void sys_reset(System* s) {
    memset(s->events, 0, sizeof s->events);
    s->nextEvent = UINT64_MAX;
    gekko_reset(&s->cpu);
    memset(&s->pi, 0, sizeof s->pi);
    vi_reset(s); exi_reset(s); si_reset(s); dsp_reset(s); ai_reset(s); di_reset(s); gx_reset(s);
}

void sys_stop(System* s, const char* why) {
    if (why) emu_error("stop: %s (pc=%08x)\n", why, s->cpu.pc);
    s->stopRequested = true;
    gekko_stop(&s->cpu);
}

void gekko_on_fatal(Gekko* g, const char* what) {
    System* s = (System*)g->bus->ctx;
    sys_stop(s, what);
}

void sys_run(System* s, u64 cycles) {
    u64 end = sys_now(s) + cycles;
    s->stopRequested = false;
    while (!s->stopRequested && sys_now(s) < end) {
        if (s->gxAsync) s->gxAsync->poll(s);
        run_due_events(s);
        u64 now = sys_now(s);
        u64 until = s->nextEvent < end ? s->nextEvent : end;
        if (until <= now) continue;
        u64 slice = until - now;
        u64 t0 = prof_begin();
        gekko_run(&s->cpu, slice > 1000000 ? 1000000u : (u32)slice);
        prof_end(PROF_CPU, t0);
    }
}
