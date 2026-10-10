// Save states for development: the whole machine to a file and back, within the same build of the emulator
// (the file holds raw structures; a different build refuses it). Host pointers are never taken from the file.
#include <stdio.h>
#include <stdlib.h>
#include "system.h"

#define STATE_MAGIC 0x47433353u  // "GC3S"
#define ARAM_BYTES (16u * 1024 * 1024)

typedef struct { u32 magic, sysSize, gxSize, version; u64 refFn; u32 pendingLen, pad; } StateHeader;

static bool put(FILE* f, const void* p, size_t n) { return fwrite(p, 1, n, f) == n; }
static bool get(FILE* f, void* p, size_t n) { return fread(p, 1, n, f) == n; }

bool sys_save_state(System* s, const char* path) {
    FILE* f = fopen(path, "wb");
    if (!f) return false;
    GXCore* gx = &s->gx.core;
    StateHeader h = { STATE_MAGIC, (u32)sizeof *s, (u32)sizeof *gx, 1, (u64)(uintptr_t)sys_run, gx->pendingLen, 0 };
    bool ok = put(f, &h, sizeof h) && put(f, s, sizeof *s) && put(f, s->mem1, MEM1_SIZE) &&
              put(f, s->lockedCache, LOCKED_CACHE_SIZE) && put(f, s->dsp.aram, ARAM_BYTES) && put(f, gx->tmem, 1u << 20) &&
              put(f, gx->efb, EFB_W * EFB_H * 4) && put(f, gx->efbZ, EFB_W * EFB_H * 4) &&
              (!gx->pendingLen || put(f, gx->pending, gx->pendingLen));
    return fclose(f) == 0 && ok;
}

bool sys_load_state(System* s, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    StateHeader h;
    System* t = malloc(sizeof *t);
    GXCore* gx = &s->gx.core;
    bool ok = t && get(f, &h, sizeof h) && h.magic == STATE_MAGIC && h.sysSize == sizeof *s && h.gxSize == sizeof *gx &&
              get(f, t, sizeof *t);
    if (ok && h.pendingLen > gx->pendingCap) {
        u8* p = realloc(gx->pending, h.pendingLen);
        if (p) { gx->pending = p; gx->pendingCap = h.pendingLen; } else ok = false;
    }
    ok = ok && get(f, s->mem1, MEM1_SIZE) && get(f, s->lockedCache, LOCKED_CACHE_SIZE) && get(f, s->dsp.aram, ARAM_BYTES) &&
         get(f, gx->tmem, 1u << 20) && get(f, gx->efb, EFB_W * EFB_H * 4) && get(f, gx->efbZ, EFB_W * EFB_H * 4) &&
         (!h.pendingLen || get(f, gx->pending, h.pendingLen));
    fclose(f);
    if (!ok) { free(t); return false; }

    // CPU: architectural state over a freshly reset core, then the BATs and MSR rewritten so that its lookup tables
    // are rebuilt.
    Gekko* g = &s->cpu;
    gekko_reset(g);
    const GekkoBus* bus = g->bus;
    void* priv = g->priv;
    *g = t->cpu;
    g->bus = bus;
    g->priv = priv;
    for (u32 i = 0; i < 8; i++) {
        gekko_set_spr(g, SPR_IBAT0U + i, t->cpu.spr[SPR_IBAT0U + i]);
        gekko_set_spr(g, SPR_DBAT0U + i, t->cpu.spr[SPR_DBAT0U + i]);
    }
    gekko_set_msr(g, ~t->cpu.msr & (MSR_IR | MSR_DR));  // force a change so the maps are rebuilt
    gekko_set_msr(g, t->cpu.msr);
    gekko_set_spr(g, SPR_DEC, (u32)(t->cpu.decZeroAt / 12 - t->cpu.cycles / 12));
    gekko_set_external_irq(g, t->cpu.extIrq);

    s->pi = t->pi; s->vi = t->vi; s->exi = t->exi; s->si = t->si; s->ai = t->ai; s->di = t->di;
    u8* aram = s->dsp.aram;
    s->dsp = t->dsp;
    s->dsp.aram = aram;
    GXCore keep = *gx;
    s->gx = t->gx;
    gx->pending = keep.pending; gx->pendingCap = keep.pendingCap; gx->pendingLen = h.pendingLen;
    gx->tmem = keep.tmem; gx->efb = keep.efb; gx->efbZ = keep.efbZ;
    gx->backend = keep.backend; gx->backendCtx = keep.backendCtx;
    gx->texVersion++;
    // Scheduled events hold function pointers into this program; move them by the load address difference.
    intptr_t delta = (intptr_t)(uintptr_t)sys_run - (intptr_t)h.refFn;
    for (int i = 0; i < MAX_EVENTS; i++) {
        s->events[i] = t->events[i];
        if (s->events[i].used) s->events[i].fn = (EventFn)(uintptr_t)((intptr_t)(uintptr_t)t->events[i].fn + delta);
    }
    s->nextEvent = t->nextEvent;
    s->unknownMmioCount = t->unknownMmioCount;
    pi_update(s);
    free(t);
    return true;
}
