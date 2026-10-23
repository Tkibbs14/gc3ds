// G2-0: display-list key census on the device (dlkey.h). The dependency rules are X1b's (core/src/host/gxcensus_x1b.inc,
// MEASUREMENTS.md "X1b"): a word enters a list's key when the reference decoder or renderer reads it before the list
// writes it. X1b ran on the PC's title states only; this runs inside the 3DS build on gameplay states, and splits the
// key by class: a translated list could check the structural words, but would have to apply parametric ones (animated
// colours, the projection: Ordon job 1138) and re-upload matrices and lights (camera-relative) on every replay.
//
// Keys hash words in the order they are first read. Two calls with the same bytes that read the same values follow
// the same path through the decoder, so they read in the same order: an order hash repeats exactly when X1b's sorted
// hash does.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../sys/system.h"
#include "dlkey.h"

u64 g_dlKeyStats[DKS_N];
u32 g_dlKeyMiss[DLK_IDS];
bool g_dlKey, g_dlKeyPlant, g_dlKeyActive;
static void* dk_calloc(u32 bytes) { return calloc(1, bytes); }
void* (*g_dlKeyAlloc)(u32 bytes) = dk_calloc;

enum { D_CP = 256, D_XF = 512, D_TEV = D_XF + 0x1058, D_MASK = D_TEV + 16, D_N = D_MASK + 1 };
enum { ST_READ = 1, ST_WRITTEN = 2 };

static const GXCore* g_gx;
static u8 g_st[D_N];
static u16 g_touched[D_N];
static u32 g_nTouched;
static u64 g_hk[DK_CLASSES], g_manifest, g_base, g_bytes, g_seq;
static u32 g_addr, g_reads[DK_CLASSES], g_draws, g_distinct;
static bool g_side, g_xfLoad;
// The guard words (structural and parametric) in the order read, for attributing a miss to the words that differed (48
// cut most of TP's lists, jobs 1137-1138).
#define NG 128
static u16 g_gid[NG];
static u32 g_gval[NG], g_gn;
static bool g_gTrunc;

static inline u64 word(u64 h, u32 v) { return (h ^ v) * 0x100000001b3ull; }
#define FNV64 0xcbf29ce484222325ull

// Parametric: the TEV colour banks and their BP registers (0xE0-0xE7), fog (0xEE-0xF2), ambient and material colours
// (XF 0x100A-0x100D), viewport and projection (XF 0x101A-0x1026).
static bool dk_param(u32 id) {
    if (id < D_CP) return (id >= 0xE0 && id <= 0xE7) || (id >= 0xEE && id <= 0xF2);
    if (id < D_XF) return false;
    if (id < D_TEV) { u32 a = id - D_XF; return (a >= 0x100A && a <= 0x100D) || (a >= 0x101A && a <= 0x1026); }
    return id < D_MASK;
}
static u32 dk_class(u32 id) {
    if (dk_param(id)) return DK_PAR;
    if (id < D_CP) return DK_BP;
    if (id < D_XF) { u32 r = id - D_CP; return r >= 0xA0 && r <= 0xBF ? DK_ARR : DK_CP; }
    if (id < D_TEV) { u32 a = id - D_XF; return a < 0x600 ? DK_MTX : a < 0x1000 ? DK_LIT : DK_XFR; }
    return DK_BP;  // the BP mask
}

static u32 dk_raw(u32 id) {
    const GXCore* gx = g_gx;
    if (id < D_CP) return gx->bp[id];
    if (id < D_XF) return gx->cp[id - D_CP];
    if (id < D_TEV) return gx->xf[id - D_XF];
    if (id == D_MASK) return gx->bpMask;
    const u32* banks[] = { gx->tevLo, gx->tevHi, gx->kLo, gx->kHi };
    return banks[(id - D_TEV) / 4][(id - D_TEV) & 3];
}

static u32 dep_value(u32 id) {
    u32 v = dk_raw(id);
    u8 st = g_st[id];
    if (!st) g_touched[g_nTouched++] = (u16)id;
    if (!(st & (ST_READ | ST_WRITTEN))) {
        g_st[id] = st | ST_READ;
        u32 c = dk_class(id);
        g_hk[c] = word(word(g_hk[c], id), v);
        g_manifest = word(g_manifest, id);
        g_reads[c]++;
        if (c <= DK_PAR) {
            if (g_gn < NG) { g_gid[g_gn] = (u16)id; g_gval[g_gn++] = v; } else g_gTrunc = true;
        }
    }
    return v;
}
static void dep_range(u32 first, u32 n) { for (u32 i = 0; i < n && first + i < D_N; i++) (void)dep_value(first + i); }
static void dep_store(u32 id) {
    if (id >= D_N) return;
    if (!g_st[id]) g_touched[g_nTouched++] = (u16)id;
    g_st[id] |= ST_WRITTEN;
}
static u32 dbp(u32 r) { return dep_value(r); }
static u32 dcp(u32 r) { return dep_value(D_CP + r); }
static u32 dxf(u32 a) { return dep_value(D_XF + a); }

// What the reference renderer reads for one draw (X1b's dep_texture / dep_draw, unchanged).
static void dep_texture(u32 map) {
    u32 m = (map & 3) + (map >= 4 ? 0x20 : 0);
    (void)dbp(0x80 + m); (void)dbp(0x88 + m); (void)dbp(0x94 + m); (void)dbp(0x98 + m);
}
static void dep_draw(u32 vat, const u8* raw, u32 count, u32 stride, u32 prim) {
    if (!count) return;
    g_draws++;
    u32 lo = dcp(0x50), hi = dcp(0x60);
    (void)dcp(0x70 + vat); (void)dcp(0x80 + vat); (void)dcp(0x90 + vat);
    u32 modes[12] = { (lo >> 9) & 3, (lo >> 11) & 3, (lo >> 13) & 3, (lo >> 15) & 3 };
    for (u32 i = 0; i < 8; i++) modes[4 + i] = (hi >> (2 * i)) & 3;
    for (u32 i = 0; i < 12; i++) if (modes[i] >= 2) { (void)dcp(0xa0 + i); (void)dcp(0xb0 + i); }
    u32 gen = dbp(0), stages = ((gen >> 10) & 15) + 1;
    (void)dbp(0x20); (void)dbp(0x21); (void)dbp(0x40); (void)dbp(0x41); (void)dbp(0xf3);
    if (prim >= 5) (void)dbp(0x22);
    u32 fog = dbp(0xf1), fogfn = (fog >> 21) & 7;
    if (fogfn >= 2 && fogfn != 3) dep_range(0xee, 5);
    if ((dbp(0xf5) >> 2) & 3) (void)dbp(0xf4);
    dep_range(D_TEV, 16);
    bool indirect = false;
    for (u32 st = 0; st < stages; st++) indirect |= (dbp(0x10 + st) & 0x1fffff) != 0;
    if (indirect) {
        u32 ref = dbp(0x27), ni = (gen >> 16) & 7;
        for (u32 i = 0; i < ni && i < 4; i++) {
            u32 coord = (ref >> (6 * i + 3)) & 7;
            (void)dbp(0x25 + i / 2); dep_texture((ref >> (6 * i)) & 7);
            dep_range(0x30 + 2 * coord, 2);
        }
    }
    for (u32 st = 0; st < stages; st++) {
        u32 ref = dbp(0x28 + st / 2) >> ((st & 1) * 12);
        (void)dbp(0xc0 + 2 * st); u32 alpha = dbp(0xc1 + 2 * st);
        (void)dbp(0xf6 + st / 2);
        dep_range(0xf6 + 2 * (alpha & 3), 2); dep_range(0xf6 + 2 * ((alpha >> 2) & 3), 2);
        if (ref & 0x40) dep_texture(ref & 7);
        if (indirect) {
            dep_range(0x30 + 2 * ((ref >> 3) & 7), 2);
            u32 matrix = (dbp(0x10 + st) >> 9) & 3;
            if (matrix) dep_range(0x06 + 3 * (matrix - 1), 3);
        }
    }
    dep_range(D_XF + 0x1020, 7); dep_range(D_XF + 0x101a, 6);
    u32 chans = dxf(0x1009) & 3;
    for (u32 ch = 0; ch < chans && ch < 2; ch++) {
        (void)dxf(0x100a + ch); (void)dxf(0x100c + ch);
        for (u32 a = 0; a < 2; a++) {
            u32 ctrl = dxf(0x100e + ch + 2 * a);
            if (!(ctrl & 2)) continue;
            u32 mask = ((ctrl >> 2) & 15) | (((ctrl >> 11) & 15) << 4);
            for (u32 l = 0; l < 8; l++) if (mask & (1u << l)) dep_range(D_XF + 0x603 + 16 * l, 13);
        }
    }
    u32 ng = dxf(0x103f) & 15, cfg[8] = { 0 };
    for (u32 t = 0; t < ng && t < 8; t++) cfg[t] = dxf(0x1040 + t);
    u32 ma = 0, mb = 0;
    bool need_ma = !(lo & 1), need_mb = false;
    for (u32 t = 0; t < ng && t < 8; t++) if (((cfg[t] >> 4) & 7) < 2 && !((lo >> (t + 1)) & 1)) {
        if (t < 4) need_ma = true; else need_mb = true;
    }
    if (need_ma) ma = dcp(0x30);
    if (need_mb) mb = dcp(0x40);
    u64 pm_seen = 0, tm_seen[8] = { 0 };
    for (u32 i = 0; i < count; i++) {
        const u8* p = raw + i * stride;
        u32 pn = lo & 1 ? ramp_rd8(p++) & 63 : ma & 63;
        if (!(pm_seen & (1ull << pn))) {
            pm_seen |= 1ull << pn;
            dep_range(D_XF + pn * 4, 12);
            dep_range(D_XF + 0x400 + (pn & 31) * 3, 9);
        }
        for (u32 t = 0; t < 8; t++) {
            u32 tm = t < 4 ? (ma >> (6 + 6 * t)) & 63 : (mb >> (6 * (t - 4))) & 63;
            if ((lo >> (1 + t)) & 1) tm = ramp_rd8(p++) & 63;
            if (t >= ng || ((cfg[t] >> 4) & 7) >= 2 || (tm_seen[t] & (1ull << tm))) continue;
            tm_seen[t] |= 1ull << tm;
            dep_range(D_XF + tm * 4, cfg[t] & 2 ? 12 : 8);
        }
    }
}

// The reads and writes of one command (X1b's gxcensus_prepare). Indexed XF loads also fold the words they load from
// RAM into the matrix-side key (X1b's state-only key left them out).
void dlkey_cmd(System* s, const GXCore* gx, const u8* p, u32 avail) {
    u32 op = ramp_rd8(p);
    if (op >= 0x80 && op < 0xc0) {
        if (avail < 3) return;
        u32 n = ramp_rd16(p + 1), stride = gx_vertex_size(gx, op & 7);
        if (avail < 3 + n * stride) return;
        dep_draw(op & 7, p + 3, n, stride, (op >> 3) & 7);
        return;
    }
    if (op == 0x08) {
        if (avail >= 6) dep_store(D_CP + ramp_rd8(p + 1));
        return;
    }
    if (op == 0x61) {
        if (avail < 5) return;
        u32 r = ramp_rd8(p + 1), v = ramp_rd32(p + 1) & 0xffffff, mask = gx->bpMask;
        if (r == 0x45 || r == 0x47 || r == 0x48 || r == 0x52 || r == 0x65 || r == 0x66) g_side = true;
        u32 nv = (gx->bp[r] & ~mask) | (v & mask);
        if (r != 0xfe) {
            (void)dep_value(D_MASK);
            if (mask != 0xffffff) (void)dbp(r);
        }
        dep_store(r); dep_store(D_MASK);
        if (r == 0x65) (void)dbp(0x64);
        if (r == 0x52) {
            dep_range(0x49, 3); (void)dbp(0x4d);
            if (!(v & (1u << 14))) (void)dbp(0x43);
            if (v & (1u << 11)) dep_range(0x4f, 3);
        }
        if (r >= 0xe0 && r <= 0xe7) {
            u32 b = (nv >> 23) & 1, i = (r - 0xe0) / 2, bank = b * 2 + (r & 1);
            dep_store(D_TEV + 4 * bank + i);
        }
        return;
    }
    if (op == 0x10 || op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {
        if (avail < 5) return;
        bool indexed = op != 0x10;
        u32 h = ramp_rd32(p + 1), n = indexed ? ((h >> 12) & 15) + 1 : (h >> 16) + 1;
        u32 addr = h & (indexed ? 0xfff : 0xffff), pa = 0;
        if (!indexed && avail < 5 + 4 * n) return;
        if (indexed) {
            u32 a = 12 + ((op - 0x20) >> 3);
            (void)dcp(0xa0 + a); (void)dcp(0xb0 + a);
            pa = (gx->cp[0xa0 + a] & GX_ARRAY_MASK) + (h >> 16) * (gx->cp[0xb0 + a] & 0xfff);
        }
        if (addr < 0x1000) g_xfLoad = true;
        for (u32 i = 0; i < n; i++) {
            if (indexed && (u64)pa + 4 * i + 4 > PA_MEM1_END) break;
            if (indexed) { g_hk[DK_RAM] = word(g_hk[DK_RAM], mem1_read32(s, pa + 4 * i)); g_reads[DK_RAM]++; }
            if (addr + i < 0x1058) dep_store(D_XF + addr + i);
        }
        return;
    }
    if (op != 0x00 && op != 0x44 && op != 0x48) g_side = true;  // a nested call (ignored by fifo.c) or an unknown opcode
}

// Previous / current picture: open-addressed sets of keys (zero marks empty), kept at most half full.
#define SET_N 32768
static u64* g_set[2];
static u32 g_setUsed[2], g_cur;
static bool set_has(const u64* t, u64 k) {
    for (u32 i = (u32)(k ^ (k >> 29)) & (SET_N - 1);; i = (i + 1) & (SET_N - 1)) {
        if (!t[i]) return false;
        if (t[i] == k) return true;
    }
}
static bool set_add(u32 w, u64 k) {  // true: newly kept
    u64* t = g_set[w];
    u32 i = (u32)(k ^ (k >> 29)) & (SET_N - 1);
    for (; t[i]; i = (i + 1) & (SET_N - 1)) if (t[i] == k) return false;
    if (g_setUsed[w] >= SET_N / 2) { g_dlKeyStats[DS_NOTKEPT]++; return false; }
    t[i] = k;
    g_setUsed[w]++;
    return true;
}
// Seen in the previous picture's set; kept in this one's. Kinds tag the keys apart (the superset keys after DKK_N).
static bool seen(u64 key, u32 kind, bool* added) {
    u64 t = word(key, 0x9E37u + kind);
    if (!t) t = 1;
    bool last = set_has(g_set[g_cur ^ 1], t);
    *added = set_add(g_cur, t);
    return last;
}

// The same address's last use (open addressing, probing at most 32 slots, then the home slot is replaced), with its
// guard words for attributing a miss.
#define LAST_N 4096
typedef struct { u32 addr, used; u64 key[DKK_N], manifest, bytes, zs, zp; u16 gid[NG]; u32 gval[NG], gn; } LastUse;
static LastUse* g_last;

// The superset keys' words: every BP register but PE signals and tokens (0x45-0x48), the copy, clear and bounding-box
// registers (0x49-0x58), palette loads, texture cache invalidation and counters (0x64-0x69) and the mask; the CP's
// matrix indices, descriptors, formats and strides; XF registers but error, clock and performance words; the TEV banks.
// The structural one leaves the parametric words out.
static u16 g_zid[600];
static u32 g_zn;
static u64 g_zs, g_zp;
static void z_init(void) {
    for (u32 r = 0; r < 256; r++)
        if (!(r >= 0x45 && r <= 0x58) && !(r >= 0x64 && r <= 0x69) && r != 0xFE && r != 0xFF) g_zid[g_zn++] = (u16)r;
    static const u8 cp[] = { 0x30, 0x40, 0x50, 0x60 };
    for (u32 i = 0; i < 4; i++) g_zid[g_zn++] = (u16)(D_CP + cp[i]);
    for (u32 r = 0x70; r < 0x98; r++) if ((r & 0xF) < 8) g_zid[g_zn++] = (u16)(D_CP + r);
    for (u32 r = 0xB0; r < 0xC0; r++) g_zid[g_zn++] = (u16)(D_CP + r);
    for (u32 a = 0x1002; a < 0x1058; a++) if (a != 0x1004 && a != 0x1006 && a != 0x1007) g_zid[g_zn++] = (u16)(D_XF + a);
    for (u32 i = 0; i < 16; i++) g_zid[g_zn++] = (u16)(D_TEV + i);
}

static bool dk_alloc(void) {
    if (g_last) return true;
    g_last = g_dlKeyAlloc(LAST_N * sizeof *g_last);
    g_set[0] = g_dlKeyAlloc(SET_N * 8);
    g_set[1] = g_dlKeyAlloc(SET_N * 8);
    if (!g_last || !g_set[0] || !g_set[1]) { g_last = NULL; g_dlKey = false; return false; }  // (a measurement run)
    z_init();
    return true;
}

void dlkey_name(u32 id, char* out, u32 n) {
    if (id < D_CP) snprintf(out, n, "bp%02x", id);
    else if (id < D_XF) snprintf(out, n, "cp%02x", id - D_CP);
    else if (id < D_TEV) snprintf(out, n, "xf%04x", id - D_XF);
    else if (id < D_MASK) snprintf(out, n, "%s%u", (id - D_TEV) / 8 ? "konst" : "tevreg", ((id - D_TEV) / 4 & 1) * 4 + ((id - D_TEV) & 3));
    else snprintf(out, n, "bpmask");
}

void dlkey_begin(System* s, const GXCore* gx, u32 addr, const u8* p, u32 size) {
    (void)s;
    if (!dk_alloc()) return;
    g_gx = gx;
    g_addr = addr;
    u64 h = FNV64;
    u32 i = 0;
    for (; i + 4 <= size; i += 4) { u32 w; memcpy(&w, p + i, 4); h = word(h, w); }
    for (; i < size; i++) h = word(h, p[i]);
    g_bytes = word(h, size);
    u64 b = word(g_bytes, addr);
    for (u32 a = 0; a < 16; a++) b = word(b, gx->cp[0xA0 + a]);
    g_base = b;
    u64 zs = b, zp = b;
    for (u32 k = 0; k < g_zn; k++) {
        u32 id = g_zid[k], v = dk_raw(id);
        zp = word(zp, v);
        if (!dk_param(id)) zs = word(zs, v);
    }
    g_zs = zs; g_zp = zp;
    for (u32 c = 0; c < DK_CLASSES; c++) { g_hk[c] = FNV64; g_reads[c] = 0; }
    g_manifest = FNV64;
    g_draws = 0;
    g_gn = 0;
    g_side = g_xfLoad = g_gTrunc = false;
    g_dlKeyActive = true;
}

static u64 fold(u64 h, u64 k) { return word(word(h, (u32)k), (u32)(k >> 32)); }

void dlkey_end(u64 ticks) {
    if (!g_dlKeyActive) return;
    g_dlKeyActive = false;
    for (u32 i = 0; i < g_nTouched; i++) g_st[g_touched[i]] = 0;
    g_nTouched = 0;
    u64 k[DKK_N];
    k[DKK_BYTES] = g_base;
    u64 h = g_base;
    for (u32 c = DK_BP; c <= DK_XFR; c++) h = fold(h, g_hk[c]);
    k[DKK_STRUCT] = h;
    k[DKK_PARAM] = h = fold(h, g_hk[DK_PAR]);
    k[DKK_LIGHT] = h = fold(h, g_hk[DK_LIT]);
    h = fold(h, g_hk[DK_MTX]);
    k[DKK_FULL] = fold(h, g_hk[DK_RAM]);
    u64 zs = g_zs, zp = g_zp;
    if (g_dlKeyPlant) {
        g_seq++;
        for (u32 q = 0; q < DKK_N; q++) k[q] = word(k[q], (u32)g_seq);
        zs = word(zs, (u32)g_seq); zp = word(zp, (u32)g_seq);
    }
    u64* st = g_dlKeyStats;
    st[DS_CALLS]++;
    st[DS_DRAWS] += g_draws;
    st[DS_SIDE] += g_side;
    st[DS_XFLOAD] += g_xfLoad;
    st[DS_CUT] += g_gTrunc;
    for (u32 c = 0; c < DK_CLASSES; c++) st[DS_WORDS + c] += g_reads[c];
    for (u32 q = 0; q < DKK_N; q++) {
        bool added;
        if (seen(k[q], q, &added)) { st[DS_PIC + q]++; st[DS_DRAWPIC + q] += g_draws; }
        if (added && q == DKK_STRUCT) g_distinct++;
    }
    bool added;
    if (seen(zs, DKK_N, &added)) { st[DS_ZS_PIC]++; st[DS_ZS_DRAWPIC] += g_draws; }
    if (seen(zp, DKK_N + 1, &added)) { st[DS_ZP_PIC]++; st[DS_ZP_DRAWPIC] += g_draws; }
    u32 slot = (g_addr >> 5) & (LAST_N - 1), home = slot;
    LastUse* e = NULL;
    for (u32 n = 0; n < 32; n++, slot = (slot + 1) & (LAST_N - 1))
        if (!g_last[slot].used || g_last[slot].addr == g_addr) { e = &g_last[slot]; break; }
    if (!e) { e = &g_last[home]; e->used = 0; }
    bool rep[DKK_N] = { false };
    if (!e->used) st[DS_NEW]++;
    else {
        for (u32 q = 0; q < DKK_N; q++) if (e->key[q] == k[q]) { rep[q] = true; st[DS_LAST + q]++; st[DS_DRAWLAST + q] += g_draws; }
        if (e->zs == zs) st[DS_ZS_LAST]++;
        if (e->zp == zp) st[DS_ZP_LAST]++;
        if (e->bytes == g_bytes && e->manifest != g_manifest) st[DS_SETCHG]++;
        // The same path through the list (same bytes, same words read in the same order) with another parametric
        // key: the structural or parametric words that differed.
        if (!rep[DKK_PARAM] && !g_dlKeyPlant && e->bytes == g_bytes && e->manifest == g_manifest) {
            st[DS_MISS]++;
            u32 n = g_gn < e->gn ? g_gn : e->gn;
            for (u32 i = 0; i < n; i++)
                if (e->gid[i] == g_gid[i] && e->gval[i] != g_gval[i]) { g_dlKeyMiss[g_gid[i]]++; st[DS_MISSWORDS]++; }
        }
    }
    e->used = 1; e->addr = g_addr; e->bytes = g_bytes; e->manifest = g_manifest; e->zs = zs; e->zp = zp;
    memcpy(e->key, k, sizeof k);
    memcpy(e->gid, g_gid, g_gn * sizeof g_gid[0]);
    memcpy(e->gval, g_gval, g_gn * sizeof g_gval[0]);
    e->gn = g_gn;
    if (ticks) {
        st[DS_TIMED]++; st[DS_TICKS] += ticks;
        for (u32 q = 0; q < DKK_N; q++) if (rep[q]) st[DS_TICKREP + q] += ticks;
    }
}

void dlkey_picture(void) {
    if (!g_last) return;
    g_dlKeyStats[DS_PICTURES]++;
    g_dlKeyStats[DS_DISTINCT] += g_distinct;
    g_distinct = 0;
    g_cur ^= 1;
    memset(g_set[g_cur], 0, SET_N * 8);
    g_setUsed[g_cur] = 0;
}
