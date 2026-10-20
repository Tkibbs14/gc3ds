// X1b command semantics and exclusive decode timing. Included only by the PC census companion.
#include <time.h>

enum { D_CP = 256, D_XF = 512, D_TEV = D_XF + 0x1058, D_MASK = D_TEV + 16, D_N = D_MASK + 1 };
static const GXCore* dep_gx;
static bool dep_on;
static u32 incoming[D_N];
static u8 dep_read[D_N], dep_written[D_N];
static u64 dep_base, dep_sequence;
static u32 dep_address;
static unsigned dep_current, light_current;
static u64 *dep_sets[2], *light_sets[5];
static struct { u32 address; bool used; u64 key, manifest, bytes; } dep_last[65536];

static u32 dep_value(u32 id) {
    if (dep_on && !dep_written[id]) dep_read[id] = 1;
    if (id < D_CP) return dep_gx->bp[id];
    if (id < D_XF) return dep_gx->cp[id - D_CP];
    if (id < D_TEV) return dep_gx->xf[id - D_XF];
    if (id == D_MASK) return dep_gx->bpMask;
    const u32* banks[] = { dep_gx->tevLo, dep_gx->tevHi, dep_gx->kLo, dep_gx->kHi };
    return banks[(id - D_TEV) / 4][(id - D_TEV) & 3];
}
static void dep_range(u32 first, u32 n) {
    if (!dep_on) return;
    for (u32 i = 0; i < n && first + i < D_N; i++) (void)dep_value(first + i);
}
static void dep_store(u32 id) { if (dep_on && id < D_N) dep_written[id] = 1; }
static u32 dbp(u32 id) { return dep_value(id); }
static u32 dcp(u32 id) { return dep_value(D_CP + id); }
static u32 dxf(u32 id) { return dep_value(D_XF + id); }

static void dep_begin(const GXCore* gx, const u8* p, u32 size) {
    dep_gx = gx; dep_on = true;
    memset(dep_read, 0, sizeof dep_read); memset(dep_written, 0, sizeof dep_written);
    memcpy(incoming, gx->bp, sizeof gx->bp);
    memcpy(incoming + D_CP, gx->cp, sizeof gx->cp);
    memcpy(incoming + D_XF, gx->xf, sizeof gx->xf);
    memcpy(incoming + D_TEV, gx->tevLo, 16); memcpy(incoming + D_TEV + 4, gx->tevHi, 16);
    memcpy(incoming + D_TEV + 8, gx->kLo, 16); memcpy(incoming + D_TEV + 12, gx->kHi, 16);
    incoming[D_MASK] = gx->bpMask;
    dep_base = words(bytes(p, size), gx->cp + 0xa0, 16);
    dep_address = (u32)(p - machine->mem1);
}
static void dep_end(void) {
    if (!dep_on) return;
    u64 key = dep_base, manifest = 0xcbf29ce484222325ull;
    for (u32 id = 0; id < D_N; id++) if (dep_read[id]) {
        key = word(word(key, id), incoming[id]);
        manifest = word(manifest, id);
        unsigned bank = id < D_CP ? B_WORD_BP : id < D_XF ? B_WORD_CP :
                        id < D_TEV ? B_WORD_XF : id < D_MASK ? B_WORD_TEV : B_WORD_MASK;
        row.b[bank]++;
    }
    // Plant a different incoming array-base observation on every call, without touching guest memory.
    if (plant) key = word(key, (u32)++dep_sequence);
    row.b[B_DEP_ACTIVE] += contains(dep_sets[dep_current ^ 1], key, false);
    contains(dep_sets[dep_current], key, true);
    u32 slot = (dep_address >> 5) & 65535;
    for (u32 n = 0; n < 65536; n++, slot = (slot + 1) & 65535) {
        if (!dep_last[slot].used || dep_last[slot].address == dep_address) {
            row.b[B_DEP_LAST] += dep_last[slot].used && dep_last[slot].key == key;
            if (!dep_last[slot].used || dep_last[slot].bytes != listkey) row.b[B_PLAN_FIRST]++;
            else if (dep_last[slot].manifest != manifest) row.b[B_PLAN_CHANGED]++;
            dep_last[slot].used = true; dep_last[slot].address = dep_address;
            dep_last[slot].key = key; dep_last[slot].manifest = manifest; dep_last[slot].bytes = listkey;
            break;
        }
        if (n == 65535) fail("dependency manifest table full");
    }
    dep_on = false;
}

static void dep_texture(u32 map) {
    u32 m = (map & 3) + (map >= 4 ? 0x20 : 0);
    // The reference get_tex() reads these four words; unused texture units never enter the key.
    (void)dbp(0x80 + m); (void)dbp(0x88 + m); (void)dbp(0x94 + m); (void)dbp(0x98 + m);
}
static void dep_draw(u32 vat, const u8* raw, u32 count, u32 stride, u32 prim) {
    if (!dep_on || !count) return;
    u32 lo = dcp(0x50), hi = dcp(0x60);
    (void)dcp(0x70 + vat); (void)dcp(0x80 + vat); (void)dcp(0x90 + vat);
    u32 modes[12] = { (lo >> 9) & 3, (lo >> 11) & 3, (lo >> 13) & 3, (lo >> 15) & 3 };
    for (u32 i = 0; i < 8; i++) modes[4 + i] = (hi >> (2*i)) & 3;
    for (u32 i = 0; i < 12; i++) if (modes[i] >= 2) { (void)dcp(0xa0+i); (void)dcp(0xb0+i); }
    u32 gen = dbp(0), stages = ((gen >> 10) & 15) + 1;
    (void)dbp(0x20); (void)dbp(0x21); (void)dbp(0x40); (void)dbp(0x41); (void)dbp(0xf3);
    if (prim >= 5) (void)dbp(0x22);
    u32 fog = dbp(0xf1), fogfn = (fog >> 21) & 7;
    if (fogfn >= 2 && fogfn != 3) dep_range(0xee, 5);
    if ((dbp(0xf5) >> 2) & 3) (void)dbp(0xf4);
    // Reference run_tev initialises both logical colour banks before its stage loop.
    dep_range(D_TEV, 16);
    bool indirect = false;
    for (u32 st = 0; st < stages; st++) indirect |= (dbp(0x10 + st) & 0x1fffff) != 0;
    if (indirect) {
        u32 ref = dbp(0x27), ni = (gen >> 16) & 7;
        for (u32 i = 0; i < ni && i < 4; i++) {
            u32 coord = (ref >> (6*i+3)) & 7;
            (void)dbp(0x25 + i/2); dep_texture((ref >> (6*i)) & 7);
            dep_range(0x30 + 2*coord, 2);
        }
    }
    for (u32 st = 0; st < stages; st++) {
        u32 ref = dbp(0x28 + st/2) >> ((st & 1)*12);
        (void)dbp(0xc0 + 2*st); u32 alpha = dbp(0xc1 + 2*st);
        (void)dbp(0xf6 + st/2);
        dep_range(0xf6 + 2*(alpha & 3), 2); dep_range(0xf6 + 2*((alpha >> 2) & 3), 2);
        if (ref & 0x40) dep_texture(ref & 7);
        if (indirect) {
            dep_range(0x30 + 2*((ref >> 3) & 7), 2);
            u32 matrix = (dbp(0x10 + st) >> 9) & 3;
            if (matrix) dep_range(0x06 + 3*(matrix-1), 3);
        }
    }
    dep_range(D_XF + 0x1020, 7); dep_range(D_XF + 0x101a, 6);
    u32 chans = dxf(0x1009) & 3;
    for (u32 ch = 0; ch < chans && ch < 2; ch++) {
        (void)dxf(0x100a + ch); (void)dxf(0x100c + ch);
        for (u32 a = 0; a < 2; a++) {
            u32 ctrl = dxf(0x100e + ch + 2*a);
            if (!(ctrl & 2)) continue;
            u32 mask = ((ctrl >> 2) & 15) | (((ctrl >> 11) & 15) << 4);
            for (u32 l = 0; l < 8; l++) if (mask & (1u << l)) dep_range(D_XF + 0x603 + 16*l, 13);
        }
    }
    u32 ng = dxf(0x103f) & 15, cfg[8] = {0};
    for (u32 t = 0; t < ng && t < 8; t++) cfg[t] = dxf(0x1040 + t);
    u32 ma = 0, mb = 0;
    bool need_ma = !(lo & 1), need_mb = false;
    for (u32 t = 0; t < ng && t < 8; t++) if (((cfg[t] >> 4) & 7) < 2 && !((lo >> (t+1)) & 1)) {
        if (t < 4) need_ma = true; else need_mb = true;
    }
    if (need_ma) ma = dcp(0x30);
    if (need_mb) mb = dcp(0x40);
    u64 pm_seen = 0, tm_seen[8] = {0};
    for (u32 i = 0; i < count; i++) {
        const u8* p = raw + i*stride;
        u32 pn = lo & 1 ? ramp_rd8(p++) & 63 : ma & 63;
        if (!(pm_seen & (1ull << pn))) {
            pm_seen |= 1ull << pn;
            dep_range(D_XF + pn*4, 12);
            dep_range(D_XF + 0x400 + (pn & 31)*3, 9);
        }
        for (u32 t = 0; t < 8; t++) {
            u32 tm = t < 4 ? (ma >> (6+6*t)) & 63 : (mb >> (6*(t-4))) & 63;
            if ((lo >> (1+t)) & 1) tm = ramp_rd8(p++) & 63;
            if (t >= ng || ((cfg[t] >> 4) & 7) >= 2 || (tm_seen[t] & (1ull << tm))) continue;
            tm_seen[t] |= 1ull << tm;
            dep_range(D_XF + tm*4, cfg[t] & 2 ? 12 : 8);
        }
    }
}

static void retain_light(u64 key) {
    unsigned closest = 5;
    for (unsigned age = 1; age <= 4; age++)
        if (contains(light_sets[(light_current + 5 - age) % 5], key, false)) { closest = age; break; }
    if (closest <= 1) row.b[B_LIGHT1]++;
    if (closest <= 2) row.b[B_LIGHT2]++;
    if (closest <= 4) row.b[B_LIGHT4]++;
    contains(light_sets[light_current], key, true);
}
static void finish_picture(void) {
    // The same new-picture boundary as pica.c's frame-rate instrument: an EFB -> XFB copy.
    // Empty VI fields and intermediate offscreen copies do not age the lighting window.
    row.b[B_PICTURES]++;
    light_current = (light_current+1)%5; memset(light_sets[light_current], 0, SET_N*8);
}

static u64 tick(void) {
#if defined(__x86_64__) || defined(__i386__)
    u32 lo, hi;
    __asm__ volatile("lfence\n\trdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return (u64)hi << 32 | lo;
#else
    struct timespec t; clock_gettime(CLOCK_MONOTONIC_RAW, &t);
    return (u64)t.tv_sec * 1000000000ull + t.tv_nsec;
#endif
}
typedef struct { int tag; unsigned pause, segments; bool sample, old_skip; u64 start, elapsed; } DecodeTimer;
static DecodeTimer timers[8];
static unsigned timer_depth;
static u64 tick_overhead;
static u32 sample_seed = 0x13579bdf;
static u64 decode_seen[DEC_N];
bool g_gxcensusSkip;
static const char* decode_names[DEC_N] = {
    "BP_equal", "BP_other", "CP_equal", "CP_change", "XF_equal", "XF_change",
    "IDX_equal", "IDX_change", "draw", "call", "other", "partial", "ingress"
};
void gxcensus_pause(void) {
    if (!timer_depth) return;
    DecodeTimer* t = &timers[timer_depth-1];
    if (!t->pause++ && t->sample) { t->elapsed += tick() - t->start; t->segments++; }
}
void gxcensus_resume(void) {
    if (!timer_depth) return;
    DecodeTimer* t = &timers[timer_depth-1];
    if (!t->pause) fail("unbalanced decode timer resume");
    if (!--t->pause && t->sample) t->start = tick();
}
void gxcensus_timer_enter(int tag, bool leaf) {
    if (tag == -1) tag = DEC_PARTIAL;
    else if (tag == -2) tag = DEC_INGRESS;
    if (timer_depth == 8) fail("decode timer nesting overflow");
    DecodeTimer* t = &timers[timer_depth++];
    memset(t, 0, sizeof *t); t->tag = tag; t->old_skip = g_gxcensusSkip;
    sample_seed ^= sample_seed << 13; sample_seed ^= sample_seed >> 17; sample_seed ^= sample_seed << 5;
    t->sample = tag >= 0 && (!decode_seen[tag]++ || !(sample_seed & 63));
    g_gxcensusSkip = leaf;
    if (t->sample) t->start = tick();
}
void gxcensus_timer_leave(u32 consumed) {
    gxcensus_pause();
    DecodeTimer t = timers[--timer_depth];
    g_gxcensusSkip = t.old_skip;
    if (!consumed && t.tag != DEC_PARTIAL && t.tag != DEC_INGRESS)
        fail("complete-command timer unexpectedly consumed no bytes");
    row.dec_n[t.tag]++;
    if (!t.sample) return;
    u64 overhead = tick_overhead * t.segments;
    row.dec_s[t.tag]++; row.dec_raw[t.tag] += t.elapsed;
    row.dec_ticks[t.tag] += t.elapsed > overhead ? t.elapsed - overhead : 0;
    row.dec_segments[t.tag] += t.segments;
}

int gxcensus_prepare(System* s, const GXCore* gx, const u8* p, u32 avail, bool in_list) {
    (void)in_list;
    u32 op = ramp_rd8(p);
    if (op >= 0x80 && op < 0xc0) {
        if (avail < 3) return -1;
        u32 n = ramp_rd16(p+1), stride = gx_vertex_size(gx, op & 7);
        if (avail < 3 + n*stride) return -1;
        dep_draw(op & 7, p+3, n, stride, (op >> 3) & 7);
        gxcensus_draw(s, gx, op & 7, p+3, n, stride);
        return DEC_DRAW;
    }
    if (op == 0x40) return avail < 9 ? -1 : DEC_CALL;
    if (op == 0x08) {
        if (avail < 6) return -1;
        u32 r = ramp_rd8(p+1), v = ramp_rd32(p+2);
        bool same = gx->cp[r] == v;
        gxcensus_reg(1, r, gx->cp[r], v); dep_store(D_CP+r);
        return same && !plant ? DEC_CP_EQUAL : DEC_CP_CHANGE;
    }
    if (op == 0x61) {
        if (avail < 5) return -1;
        u32 r = ramp_rd8(p+1), v = ramp_rd32(p+1) & 0xffffff, mask = gx->bpMask;
        u32 nv = (gx->bp[r] & ~mask) | (v & mask), old = gx->bp[r];
        bool side = r == 0x45 || r == 0x47 || r == 0x48 || r == 0x52 || r == 0x65 || r == 0x66 || r == 0xfe;
        if (dep_on) {
            if (r != 0xfe) {
                (void)dep_value(D_MASK);
                if (mask != 0xffffff) (void)dbp(r);
            }
            dep_store(r); dep_store(D_MASK); // every non-mask write consumes/reset the mask
            if (r == 0x65) (void)dbp(0x64);
            if (r == 0x52) {
                dep_range(0x49, 3); (void)dbp(0x4d);
                // fifo.c's copy side effect uses the command value, not the masked BP mirror.
                if (!(v & (1u << 14))) (void)dbp(0x43);
                if (v & (1u << 11)) dep_range(0x4f, 3);
            }
        }
        if (r >= 0xe0 && r <= 0xe7) {
            u32 b = (nv >> 23) & 1, i = (r-0xe0)/2, bank = b*2 + (r & 1);
            const u32* banks[] = { gx->tevLo, gx->tevHi, gx->kLo, gx->kHi };
            old = banks[bank][i]; dep_store(D_TEV+4*bank+i);
        }
        gxcensus_reg(0, r, old, nv);
        if (r == 0x52 && (v & (1u << 14))) finish_picture();
        // Logical bank equality alone is insufficient: a changed BP mirror still bumps the decoder's versions.
        bool same = old == nv && gx->bp[r] == nv && mask == 0xffffff && !side;
        return same && !plant ? DEC_BP_EQUAL : DEC_BP_OTHER;
    }
    if (op == 0x10 || op == 0x20 || op == 0x28 || op == 0x30 || op == 0x38) {
        if (avail < 5) return -1;
        bool indexed = op != 0x10, same = true;
        u32 h = ramp_rd32(p+1), n = indexed ? ((h >> 12) & 15)+1 : (h >> 16)+1;
        u32 addr = h & (indexed ? 0xfff : 0xffff), pa = 0;
        if (!indexed && avail < 5+4*n) return -1;
        if (indexed) {
            u32 a = 12 + ((op-0x20) >> 3);
            if (dep_on) { (void)dcp(0xa0+a); (void)dcp(0xb0+a); }
            pa = (gx->cp[0xa0+a] & GX_ARRAY_MASK) + (h >> 16)*(gx->cp[0xb0+a] & 0xfff);
        }
        for (u32 i = 0; i < n; i++) {
            if (indexed && (u64)pa + 4*i+4 > PA_MEM1_END) break;
            u32 v = indexed ? mem1_read32(s, pa+4*i) : ramp_rd32(p+5+4*i);
            if (addr+i >= 0x1058) continue;
            same &= gx->xf[addr+i] == v;
            gxcensus_reg(2, addr+i, gx->xf[addr+i], v); dep_store(D_XF+addr+i);
        }
        if (plant) same = false;
        return indexed ? (same ? DEC_IDX_EQUAL : DEC_IDX_CHANGE) : (same ? DEC_XF_EQUAL : DEC_XF_CHANGE);
    }
    return DEC_OTHER;
}

static void x1b_init(void) {
    for (unsigned i = 0; i < 2; i++) { dep_sets[i] = calloc(SET_N, 8); if (!dep_sets[i]) fail("dep set OOM"); }
    for (unsigned i = 0; i < 5; i++) { light_sets[i] = calloc(SET_N, 8); if (!light_sets[i]) fail("light set OOM"); }
    tick_overhead = ~(u64)0;
    for (unsigned i = 0; i < 4096; i++) { u64 a = tick(), b = tick(); if (b-a < tick_overhead) tick_overhead = b-a; }
    fprintf(report, "X1b decode_clock=%s empty_segment_ticks=%llu sample=first_in_class_then_1/64; command/partial/ingress counts exact; probes/backend/copies excluded\n",
#if defined(__x86_64__) || defined(__i386__)
            "fenced_TSC",
#else
            "MONOTONIC_RAW_ns",
#endif
            U(tick_overhead));
    fprintf(report, "X1b lighting age=completed EFB-to-XFB copies (N=1,2,4); current picture excluded from lookups\n");
}
static void x1b_emit(const char* tag, u32 field_no, const Counts* c) {
    for (unsigned d = 0; d < DEC_N; d++)
        fprintf(report, "decode_%s %u %s commands=%llu samples=%llu ticks=%llu raw=%llu segments=%llu\n",
                tag, field_no, decode_names[d], U(c->dec_n[d]), U(c->dec_s[d]), U(c->dec_ticks[d]),
                U(c->dec_raw[d]), U(c->dec_segments[d]));
}
static void x1b_close_field(void) {
    if (row.calls) { dep_current ^= 1; memset(dep_sets[dep_current], 0, SET_N*8); }
}
static void x1b_free(void) {
    for (unsigned i = 0; i < 2; i++) free(dep_sets[i]);
    for (unsigned i = 0; i < 5; i++) free(light_sets[i]);
}
static void x1b_summary(void) {
    double decode = 0, equal = 0;
    for (unsigned d = 0; d < DEC_N; d++) if (total.dec_s[d]) {
        double cost = (double)total.dec_n[d] * total.dec_ticks[d] / total.dec_s[d];
        decode += cost;
        if (d <= DEC_IDX_EQUAL && !(d & 1)) equal += cost;
    }
    fprintf(report, "X1b_derived estimated_decode_ticks=%.0f estimated_equal_ticks=%.0f equal_share=%.6f\n",
            decode, equal, decode ? equal/decode : 0);
    fprintf(stderr, "GXCENSUS X1b: sampled equal decode budget %.2f%%; dependent lists active/last %.2f/%.2f%%; "
            "lighting N=1/2/4 %.2f/%.2f/%.2f%%\n", decode ? 100*equal/decode : 0,
            total.calls ? 100.0*total.b[B_DEP_ACTIVE]/total.calls : 0,
            total.calls ? 100.0*total.b[B_DEP_LAST]/total.calls : 0,
            total.lit ? 100.0*total.b[B_LIGHT1]/total.lit : 0,
            total.lit ? 100.0*total.b[B_LIGHT2]/total.lit : 0,
            total.lit ? 100.0*total.b[B_LIGHT4]/total.lit : 0);
}
