// G2 stage a (g2.h): the list table, the decoding into ops, and the checks that a decoding still holds. The replay
// itself is in fifo.c, next to the state functions it calls.
#include <stdlib.h>
#include <string.h>
#include "../sys/system.h"
#include "g2.h"

u64 g_g2Stats[14];
bool g_g2;
u8 g_g2Check;
static void* g2_calloc(u32 bytes) { return calloc(1, bytes); }
void* (*g_g2Alloc)(u32 bytes) = g2_calloc;

// Lists by address (open addressing, 16 probes), and their ops in one pool, flushed whole when full.
#define G2_SLOTS 4096
#define G2_PROBE 16
#define G2_POOL (1u << 16)  // ops (32 bytes each: 2 MB; 1 MB flushed once in 200 title fields, ~23 ops a list)
#define G2_MAXOPS 4096      // ops in one list
#define G2_TRIES 4          // decodings an address may take before it is left to the decoder
static G2Entry* g_tab;
static G2Op* g_pool;
static u32 g_poolUsed;

static bool g2_alloc(void) {
    if (g_tab) return true;
    g_tab = g_g2Alloc(G2_SLOTS * sizeof *g_tab);
    g_pool = g_g2Alloc(G2_POOL * sizeof *g_pool);
    if (g_tab && g_pool) return true;
    g_tab = NULL;  // (left allocated: the 3DS's linear memory and the PC's heap both outlive a failed start)
    g_g2 = false;
    return false;
}

void g2_reset(void) {
    if (!g_tab) return;
    memset(g_tab, 0, G2_SLOTS * sizeof *g_tab);
    g_poolUsed = 0;
}

G2Entry* g2_lookup(u32 addr, u32 size) {
    if (!g2_alloc()) return NULL;
    u32 h = (addr >> 5) * 2654435761u;
    G2Entry* free = NULL;
    for (u32 i = 0; i < G2_PROBE; i++) {
        G2Entry* e = &g_tab[(h + i) & (G2_SLOTS - 1)];
        if (e->state == G2E_FREE) { if (!free) free = e; continue; }
        if (e->addr == addr) {
            if (e->size != size) { memset(e, 0, sizeof *e); e->addr = addr; e->size = size; e->state = G2E_SEEN; }
            return e;
        }
    }
    if (!free) { g_g2Stats[8]++; return NULL; }
    free->addr = addr;
    free->size = size;
    free->state = G2E_SEEN;
    return free;
}

const G2Op* g2_ops(const G2Entry* e) { return g_pool + e->ops; }

u32 g2_hash(const u8* p, u32 size) {
    u32 h = 2166136261u, i = 0;
    for (; i + 4 <= size; i += 4) { u32 w; memcpy(&w, p + i, 4); h = (h ^ w) * 16777619u; }
    for (; i < size; i++) h = (h ^ p[i]) * 16777619u;
    return h;
}

// The list's pages stamped after its decoding (fifo.c's MemStamps: flushes, DMA, copies, pipe bytes held in RAM).
static bool g2_stale(const G2Entry* e) {
    for (u32 pg = e->addr >> 12; pg <= (e->addr + e->size - 1) >> 12; pg++)
        if ((s32)(g_memStamps.page[pg] - e->stamp) > 0) return true;
    return false;
}

static bool g2_guards_hold(const G2Entry* e, const GXCore* gx) {
    for (u32 i = 0; i < e->ng; i++) if (gx->cp[e->gid[i]] != e->gval[i]) return false;
    return true;
}

// Decoding: run_list's walk (fifo.c), parse only. `cp` follows the list's CP writes; a vertex format the parse reads
// before the list writes it becomes a guard. Returns the ops made, or -1 when the list cannot be decoded.
typedef struct { G2Entry* e; const GXCore* gx; u32 cp[256]; u8 written[256 / 8]; G2Op* out; u32 n, cap; } Dec;

static bool dec_guard(Dec* d, u32 r) {
    if ((d->written[r >> 3] >> (r & 7)) & 1) return true;
    for (u32 i = 0; i < d->e->ng; i++) if (d->e->gid[i] == r) return true;
    if (d->e->ng == G2_GUARDS) return false;
    d->e->gid[d->e->ng] = (u8)r;
    d->e->gval[d->e->ng++] = d->gx->cp[r];
    return true;
}
static bool dec_vsize(Dec* d, u32 vat, u32* vsize) {
    if (!dec_guard(d, 0x50) || !dec_guard(d, 0x60) || !dec_guard(d, 0x70 + vat) || !dec_guard(d, 0x80 + vat) ||
        !dec_guard(d, 0x90 + vat))
        return false;
    *vsize = gx_vertex_size_cp(d->cp, vat);
    return true;
}
static G2Op* dec_op(Dec* d, u8 kind) {
    if (d->n == d->cap) return NULL;
    G2Op* o = &d->out[d->n++];
    memset(o, 0, sizeof *o);
    o->kind = kind;
    return o;
}

static int g2_decode(G2Entry* e, const GXCore* gx, const u8* p, u32 len, G2Op* out, u32 cap) {
    static Dec d;  // (the GX thread's only)
    d.e = e; d.gx = gx; d.out = out; d.n = 0; d.cap = cap;
    memcpy(d.cp, gx->cp, sizeof d.cp);
    memset(d.written, 0, sizeof d.written);
    e->ng = 0;
    e->side = 0;
    u32 off = 0, stop = 0;
    G2Op* o;
    while (off < len) {
        u32 op = ramp_rd8(p + off);
        if (op >= 0x80 && op < 0xC0 && len - off >= 3 && off >= stop) {
            u32 vat = op & 7, vsize, end = off, draws = 0, verts = 0, cmds = 0;
            if (!dec_vsize(&d, vat, &vsize)) return -1;
            bool other = false;
            u16 pc[8] = { 0 };
            while (end < len) {
                u32 c = ramp_rd8(p + end);
                if (c == 0x00) { end++; cmds++; continue; }
                if (c < 0x80 || c >= 0xC0 || (c & 7) != vat || len - end < 3) break;
                u32 n = ramp_rd16(p + end + 1);
                if (3 + n * vsize > len - end) break;
                if (((c >> 3) & 7) >= 5) other = true;
                draws++; cmds++; verts += n; pc[(c >> 3) & 7]++;
                end += 3 + n * vsize;
            }
            if (draws > 0xFFFF || cmds > 0xFFFF || !(o = dec_op(&d, G2_RUN))) return -1;
            o->a = (u8)vat; o->vsize = (u8)vsize; o->n = (u16)draws; o->m = (u16)cmds; o->v = off; o->w = end - off; o->x = verts;
            memcpy(o->pc, pc, sizeof pc);
            o->flags = other ? 1 : 0;
            if (end < len) {
                u32 c = ramp_rd8(p + end);
                o->flags |= (u8)((c >= 0x80 && c < 0xC0 ? 9 : c == 0x08 ? 6 : c == 0x10 || (c >= 0x20 && c <= 0x38) ? 7 : c == 0x61 ? 8 : 10) << 4);
            }
            // fifo.c replays the run whole or, when the backend does not take it, draw by draw: no further ops for it.
            off = stop = end;
            continue;
        }
        if (op >= 0x80 && op < 0xC0) {  // a draw run_list executes alone (its run ended within three bytes)
            u32 vsize;
            if (len - off < 3 || !dec_vsize(&d, op & 7, &vsize)) return -1;
            u32 count = ramp_rd16(p + off + 1);
            if (3 + count * vsize > len - off || !(o = dec_op(&d, G2_DRAW))) return -1;
            o->a = (u8)op; o->vsize = (u8)vsize; o->v = off + 3; o->w = count;
            off += 3 + count * vsize;
            continue;
        }
        switch (op) {
        case 0x00: case 0x44: case 0x48:
            if (d.n && d.out[d.n - 1].kind == G2_NOP && d.out[d.n - 1].n < 0xFFFF) d.out[d.n - 1].n++;
            else { if (!(o = dec_op(&d, G2_NOP))) return -1; o->n = 1; }
            off++;
            break;
        case 0x08: {
            if (len - off < 6 || !(o = dec_op(&d, G2_CP))) return -1;
            u32 r = ramp_rd8(p + off + 1), v = ramp_rd32(p + off + 2);
            o->a = (u8)r; o->v = v;
            d.cp[r] = v;
            d.written[r >> 3] |= (u8)(1u << (r & 7));
            off += 6;
            break;
        }
        case 0x10: {
            if (len - off < 5) return -1;
            u32 h = ramp_rd32(p + off + 1), n = (h >> 16) + 1;
            if (len - off < 5 + 4 * n || n > 0xFFFF || !(o = dec_op(&d, G2_XF))) return -1;
            o->n = (u16)n; o->v = h & 0xFFFF; o->w = off + 5;
            off += 5 + 4 * n;
            break;
        }
        case 0x20: case 0x28: case 0x30: case 0x38:
            if (len - off < 5 || !(o = dec_op(&d, G2_IDX))) return -1;
            o->a = (u8)((op - 0x20) >> 3); o->v = ramp_rd32(p + off + 1);
            off += 5;
            break;
        case 0x61: {
            if (len - off < 5 || !(o = dec_op(&d, G2_BP))) return -1;
            u32 r = ramp_rd8(p + off + 1);
            o->a = (u8)r; o->v = ramp_rd32(p + off + 1) & 0xFFFFFF;
            if (r == 0x45 || r == 0x47 || r == 0x48 || r == 0x52 || r == 0x65) e->side = 1;
            off += 5;
            break;
        }
        default:  // nested calls (run_list ignores them, with a log line), unknown opcodes: left to the decoder
            return -1;
        }
    }
    return (int)d.n;
}

bool g2_prepare(G2Entry* e, const GXCore* gx, const u8* p) {
    if (e->state == G2E_NEVER) return false;
    if (e->state == G2E_READY) {
        bool stale = g2_stale(e);
        if (!stale && g2_guards_hold(e, gx)) return true;
        g_g2Stats[stale ? 3 : 4]++;
    } else if (e->state == G2E_SEEN && !e->tries) {  // first sight: the decoder runs it; decoded on the second
        e->tries = 1;
        return false;
    }
    if (e->tries > G2_TRIES) { e->state = G2E_NEVER; g_g2Stats[6]++; return false; }
    e->tries++;
    if (g_poolUsed + G2_MAXOPS > G2_POOL) {  // full: every list decoded again as it comes
        for (u32 i = 0; i < G2_SLOTS; i++) if (g_tab[i].state == G2E_READY) g_tab[i].state = G2E_SEEN;
        g_poolUsed = 0;
        g_g2Stats[7]++;
    }
    u32 stamp = g_memStamps.now;  // read first: a change while decoding leaves the decoding stale at once
    int n = g2_decode(e, gx, p, e->size, g_pool + g_poolUsed, G2_MAXOPS);
    if (n < 0) { e->state = G2E_NEVER; g_g2Stats[5]++; return false; }
    e->ops = g_poolUsed;
    e->nops = (u16)n;
    g_poolUsed += (u32)n;
    e->stamp = stamp;
    e->hash = g2_hash(p, e->size);
    e->state = G2E_READY;
    g_g2Stats[2]++;
    return true;
}
