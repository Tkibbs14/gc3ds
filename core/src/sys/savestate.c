// Save states for development: the whole machine to a file and back. The file holds raw structures, so a build
// whose structures differ in size refuses it. Host pointers are never taken from the file; scheduled events are
// stored as indices into kEvents (relocating function pointers by one function's address broke as soon as any code
// before an event function changed size).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include "system.h"

#define STATE_MAGIC 0x47433353u  // "GC3S"
#define ARAM_BYTES (16u * 1024 * 1024)

// 3: Gekko gained jitLeft, jitStatus and jitFn[16] (JIT runtime words, refilled on every JIT entry). Version 2 files
// load with those inserted as zeros, so states made before still work (a qemu-made state takes hours to remake).
#define STATE_VERSION 3
#define V2_INSERT_AT ((u32)offsetof(System, cpu) + (u32)offsetof(Gekko, jitLeft))
#define V2_INSERTED ((u32)(offsetof(Gekko, sr) - offsetof(Gekko, jitLeft)))

typedef struct { u32 magic, sysSize, gxSize, version; u8 eventFn[MAX_EVENTS]; u32 pendingLen, pad; } StateHeader;

static const EventFn kEvents[] = { vi_line_event, di_complete_event, dsp_aid_event };
#define NUM_EVENT_FNS (sizeof kEvents / sizeof kEvents[0])

static bool put(FILE* f, const void* p, size_t n) { return fwrite(p, 1, n, f) == n; }
static bool get(FILE* f, void* p, size_t n) { return fread(p, 1, n, f) == n; }

// Guest memory (MEM1, the locked cache, the gather pipe's pending bytes) is big-endian in the file, whatever its layout
// in memory (ramlayout.h), so a state loads in either. Whole words swap in place (their own inverse).
static void swap_words(u8* p, u32 n) {
#if GC_SWAPRAM
    for (u32 i = 0; i < n; i += 4) { u32 w; memcpy(&w, p + i, 4); w = bswap32(w); memcpy(p + i, &w, 4); }
#else
    (void)p; (void)n;
#endif
}

bool sys_save_state(System* s, const char* path) {
    GXCore* gx = &s->gx.core;
    StateHeader h = { STATE_MAGIC, (u32)sizeof *s, (u32)sizeof *gx, STATE_VERSION, { 0 }, gx->pendingLen, 0 };
    for (int i = 0; i < MAX_EVENTS; i++) {
        if (!s->events[i].used) continue;
        u32 k = 0;
        while (k < NUM_EVENT_FNS && kEvents[k] != s->events[i].fn) k++;
        if (k == NUM_EVENT_FNS) { emu_error("save state: scheduled event %d is not in kEvents\n", i); return false; }
        h.eventFn[i] = (u8)k;
    }
    u8* pend = gx->pending;
    if (GC_SWAPRAM && gx->pendingLen) {
        pend = malloc(gx->pendingLen);
        if (!pend) return false;
        ram_to_be(pend, gx->pending, 0, gx->pendingLen);
    }
    FILE* f = fopen(path, "wb");
    if (!f) { if (pend != gx->pending) free(pend); return false; }
    swap_words(s->mem1, MEM1_SIZE);
    swap_words(s->lockedCache, LOCKED_CACHE_SIZE);
    bool ok = put(f, &h, sizeof h) && put(f, s, sizeof *s) && put(f, s->mem1, MEM1_SIZE) &&
              put(f, s->lockedCache, LOCKED_CACHE_SIZE) && put(f, s->dsp.aram, ARAM_BYTES) && put(f, gx->tmem, 1u << 20) &&
              put(f, gx->efb, EFB_W * EFB_H * 4) && put(f, gx->efbZ, EFB_W * EFB_H * 4) &&
              (!gx->pendingLen || put(f, pend, gx->pendingLen));
    swap_words(s->mem1, MEM1_SIZE);
    swap_words(s->lockedCache, LOCKED_CACHE_SIZE);
    if (pend != gx->pending) free(pend);
    return fclose(f) == 0 && ok;
}

bool sys_load_state(System* s, const char* path) {
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    StateHeader h;
    System* t = malloc(sizeof *t);
    GXCore* gx = &s->gx.core;
    bool ok = t && get(f, &h, sizeof h) && h.magic == STATE_MAGIC && h.gxSize == sizeof *gx;
    if (ok && h.version == STATE_VERSION && h.sysSize == sizeof *s) ok = get(f, t, sizeof *t);
    else if (ok && h.version == 2 && h.sysSize == sizeof *s - V2_INSERTED) {
        u8* d = (u8*)t;
        memset(t, 0, sizeof *t);
        ok = get(f, d, V2_INSERT_AT) && get(f, d + V2_INSERT_AT + V2_INSERTED, h.sysSize - V2_INSERT_AT);
    } else ok = false;
    for (int i = 0; ok && i < MAX_EVENTS; i++) ok = !t->events[i].used || h.eventFn[i] < NUM_EVENT_FNS;
    if (ok && h.pendingLen > gx->pendingCap) {
        u32 cap = (h.pendingLen + 3) & ~3u;  // (whole words: ramlayout.h)
        u8* p = realloc(gx->pending, cap);
        if (p) { gx->pending = p; gx->pendingCap = cap; } else ok = false;
    }
    ok = ok && get(f, s->mem1, MEM1_SIZE) && get(f, s->lockedCache, LOCKED_CACHE_SIZE) && get(f, s->dsp.aram, ARAM_BYTES) &&
         get(f, gx->tmem, 1u << 20) && get(f, gx->efb, EFB_W * EFB_H * 4) && get(f, gx->efbZ, EFB_W * EFB_H * 4) &&
         (!h.pendingLen || get(f, gx->pending, h.pendingLen));
    fclose(f);
    if (!ok) { free(t); return false; }
    swap_words(s->mem1, MEM1_SIZE);
    swap_words(s->lockedCache, LOCKED_CACHE_SIZE);
    mem_dirty(0, MEM1_SIZE);  // all of RAM replaced: what was converted or translated from it before is stale
    if (GC_SWAPRAM && h.pendingLen) {
        u8* be = malloc(h.pendingLen);
        if (!be) { free(t); return false; }
        memcpy(be, gx->pending, h.pendingLen);
        ram_from_be(gx->pending, 0, be, h.pendingLen);
        free(be);
    }

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
    for (int i = 0; i < MAX_EVENTS; i++) {
        s->events[i] = t->events[i];
        if (s->events[i].used) s->events[i].fn = kEvents[h.eventFn[i]];
    }
    s->nextEvent = t->nextEvent;
    s->unknownMmioCount = t->unknownMmioCount;
    pi_update(s);
    free(t);
    return true;
}
