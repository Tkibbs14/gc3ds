// X1: observation only. No GX state or guest memory is modified, including the plant.
#include "gxcensus.h"
#ifdef GX_CENSUS_PC
#include "../sys/system.h"
#include "../cpu/hle.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

bool g_gxcensus;
#define SET_N (1u << 18)
#define U(x) ((unsigned long long)(x))
typedef struct { u32 lo, hi; char name[256]; bool gx; } Symbol;
enum { B_DEP_ACTIVE, B_DEP_LAST, B_LIGHT1, B_LIGHT2, B_LIGHT4, B_PICTURES,
       B_WORD_BP, B_WORD_CP, B_WORD_XF, B_WORD_TEV, B_WORD_MASK, B_PLAN_FIRST, B_PLAN_CHANGED, B_DRAWS, B_N };
enum { DEC_BP_EQUAL, DEC_BP_OTHER, DEC_CP_EQUAL, DEC_CP_CHANGE, DEC_XF_EQUAL, DEC_XF_CHANGE,
       DEC_IDX_EQUAL, DEC_IDX_CHANGE, DEC_DRAW, DEC_CALL, DEC_OTHER, DEC_PARTIAL, DEC_INGRESS, DEC_N };
typedef struct {
    u64 writes[6], same[6], calls, upper, lower, lastuse, active[3], fifo[3], lit, hit, spec, singular, outside[4];
    u64 b[B_N], dec_n[DEC_N], dec_s[DEC_N], dec_ticks[DEC_N], dec_raw[DEC_N], dec_segments[DEC_N];
} Counts;
static void x1b_emit(const char* tag, u32 field_no, const Counts* c);
typedef struct { u32 pc; u64 n[4]; } Watch;
static Symbol symbols[40000];
static u32 nsym, field, first, fields, gxptr, gxbase, gxsize;
static FILE* report;
static System* machine;
static bool plant;
static Counts row, total;
static Watch watches[4096];
static u32 nwatch;
static u64 *sets[2][3], listkey;
static unsigned current, active_current[3];
static u64* active_sets[2][3];
static bool active_used[3];
static struct { u32 addr; u64 key; bool used; } lastlist[65536];
static bool refresh;
static const char* groups[] = { "TEV", "texture", "XF_matrices", "XF_lights", "CP_arrays", "other" };

static void fail(const char* why) { fprintf(stderr, "GXCENSUS: %s\n", why); exit(2); }
static u64 word(u64 h, u32 v) { return (h ^ v) * 0x100000001b3ull; }
static u64 words(u64 h, const u32* p, u32 n) { while (n--) h = word(h, *p++); return h; }
static u64 bytes(const u8* p, u32 n) {
    u64 h = 0xcbf29ce484222325ull;
    for (u32 i = 0; i < n; i++) h = word(h, ramp_rd8(p + i));
    return word(h, n);
}
static bool contains(u64* table, u64 key, bool insert) {
    if (!key) key = 1;
    u32 slot = (u32)(key ^ (key >> 32)) & (SET_N - 1);
    for (u32 n = 0; n < SET_N; n++, slot = (slot + 1) & (SET_N - 1)) {
        if (table[slot] == key) return true;
        if (!table[slot]) { if (insert) table[slot] = key; return false; }
    }
    fail("field key set full; no result is valid"); return false;
}
static bool repeat(unsigned kind, u64 key) {
    bool hit = contains(sets[current ^ 1][kind], key, false);
    contains(sets[current][kind], key, true);
    row.active[kind] += contains(active_sets[active_current[kind] ^ 1][kind], key, false);
    contains(active_sets[active_current[kind]][kind], key, true);
    active_used[kind] = true;
    return hit;
}
static int symcmp(const void* a, const void* b) {
    u32 x = ((const Symbol*)a)->lo, y = ((const Symbol*)b)->lo;
    return x < y ? -1 : x > y;
}
static const Symbol* symbol(u32 pc) {
    u32 lo = 0, hi = nsym;
    while (lo < hi) { u32 m = (lo + hi) / 2; if (symbols[m].lo <= pc) lo = m + 1; else hi = m; }
    return lo && pc < symbols[lo - 1].hi ? &symbols[lo - 1] : NULL;
}
static bool sdk(u32 pc) { const Symbol* f = symbol(pc); return f && f->gx; }
static void load_symbols(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) fail("cannot open GXCENSUS_SYMBOLS (default /root/tpdecomp/config/GZ2E01/symbols.txt)");
    char line[2048], name[256], section[64]; unsigned addr, size;
    while (fgets(line, sizeof line, f)) {
        if (sscanf(line, "%255s = %63[^:]:0x%x;", name, section, &addr) != 3) continue;
        char* p = strstr(line, "size:0x");
        if (!p || sscanf(p, "size:0x%x", &size) != 1) continue;
        if (!strcmp(name, "__GXData")) gxptr = addr & 0x3ffffff;
        if (!strcmp(name, "gxData")) { gxbase = addr & 0x3ffffff; gxsize = size; }
        if (!strstr(line, "type:function")) continue;
        if (nsym == 40000) fail("symbol table full");
        Symbol* s = &symbols[nsym++]; s->lo = addr; s->hi = addr + size;
        strcpy(s->name, name);
        s->gx = !strncmp(name, "GX", 2) || !strncmp(name, "__GX", 4);
    }
    fclose(f); qsort(symbols, nsym, sizeof *symbols, symcmp);
    // The linked GX library also contains local helpers such as __SetSURegs and AbortFrame.
    // Use each symbol's exact extent in the GX object span, including those local functions.
    u32 begin = 0xffffffff, end = 0;
    for (u32 i = 0; i < nsym; i++) if (symbols[i].gx) {
        if (symbols[i].lo < begin) begin = symbols[i].lo;
        if (symbols[i].hi > end) end = symbols[i].hi;
    }
    for (u32 i = 0; i < nsym; i++) symbols[i].gx = symbols[i].lo >= begin && symbols[i].hi <= end;
    if (!gxptr || !gxsize) fail("__GXData pointer / gxData object missing from symbols");
}
static void emit(const char* tag, u32 f, const Counts* c) {
    fprintf(report, "%s %u", tag, f);
    for (unsigned i = 0; i < 6; i++) fprintf(report, " %s=%llu/%llu", groups[i], U(c->same[i]), U(c->writes[i]));
    fprintf(report, " lists=%llu upper=%llu lower=%llu lastuse=%llu activeupper=%llu activelower=%llu activelight=%llu sdk=%llu direct=%llu dlbytes=%llu"
            " lit=%llu lhit=%llu spec=%llu singular=%llu reads=%llu stores=%llu ptrreads=%llu ptrstores=%llu",
            U(c->calls), U(c->upper), U(c->lower), U(c->lastuse), U(c->active[0]), U(c->active[1]), U(c->active[2]), U(c->fifo[0]), U(c->fifo[1]), U(c->fifo[2]),
            U(c->lit), U(c->hit), U(c->spec), U(c->singular), U(c->outside[0]), U(c->outside[1]),
            U(c->outside[2]), U(c->outside[3]));
    static const char* names[B_N] = { "depactive", "deplast", "light1", "light2", "light4", "pictures",
        "dep_bp", "dep_cp", "dep_xf", "dep_tev", "dep_mask", "plan_first", "plan_changed", "draw_commands" };
    for (unsigned i = 0; i < B_N; i++) fprintf(report, " %s=%llu", names[i], U(c->b[i]));
    fputc('\n', report);
    x1b_emit(tag, f, c);
}
static bool overlap(u32 a, u32 n, u32 b, u32 len) { return (u64)a < (u64)b + len && (u64)b < (u64)a + n; }
#include "gxcensus_x1b.inc"
void gxcensus_mem(u32 pc, u32 pa, u32 size, bool write) {
    if (refresh) {
        u32 live = ram_rd32(machine->mem1, gxptr) & 0x3ffffff;
        if (live && live <= MEM1_SIZE - gxsize) gxbase = live;
        refresh = false;
    }
    if (write && overlap(pa, size, gxptr, 4)) refresh = true;
    bool pipe = write && pa >= 0x0c008000 && pa < 0x0c009000;
    // Most interpreter accesses need neither symbol lookup nor a guest pointer read.
    if (!pipe && !overlap(pa, size, gxbase, gxsize) && !overlap(pa, size, gxptr, 4) &&
        !overlap(pa, size, 0x0c003014, 4)) return;
    bool isgx = sdk(pc);
    if (write && pa >= 0x0c008000 && pa < 0x0c009000)
        row.fifo[(isgx ? 0 : 1) ^ (plant ? 1 : 0)] += size;
    if (isgx) return;
    bool data = overlap(pa, size, gxbase, gxsize);
    bool ptr = overlap(pa, size, gxptr, 4) || overlap(pa, size, 0x0c003014, 4);
    if (!data && !ptr) return;
    unsigned k = (ptr ? 2 : 0) + write;
    row.outside[k]++;
    u32 i; for (i = 0; i < nwatch && watches[i].pc != pc; i++);
    if (i == nwatch) {
        if (nwatch == 4096) fail("outside-PC table full");
        watches[nwatch++].pc = pc;
    }
    watches[i].n[k]++;
}
void gxcensus_reg(unsigned bank, u32 reg, u32 old, u32 value) {
    unsigned group = 5;
    if (bank == 0) {
        if ((reg >= 0xc0 && reg <= 0xfd) || (reg >= 0x28 && reg <= 0x2f) || (reg >= 6 && reg <= 0x1f)) group = 0;
        else if ((reg >= 0x80 && reg <= 0xbb) || (reg >= 0x30 && reg <= 0x3f) || (reg >= 0x60 && reg <= 0x69)) group = 1;
    } else if (bank == 1 && reg >= 0xa0 && reg <= 0xbf) group = 4;
    else if (bank == 2) { if (reg < 0x600) group = 2; else if (reg < 0x680) group = 3; }
    row.writes[group]++;
    // Corrupt the observed incoming shadow, then use the ordinary equality comparison.
    // The emulated shadow is untouched, so the plant also has an exactness oracle.
    if (plant) old = value ^ 1u;
    row.same[group] += old == value;
}
void gxcensus_list(const GXCore* gx, const u8* p, u32 size) {
    dep_begin(gx, p, size);
    listkey = bytes(p, size);
    u64 upper = words(listkey, gx->cp + 0xa0, 16);
    if (plant) upper = word(upper, field);
    u64 lower = words(upper, gx->bp, 256);
    lower = word(lower, gx->bpMask);
    lower = words(lower, gx->tevLo, 4); lower = words(lower, gx->tevHi, 4);
    lower = words(lower, gx->kLo, 4); lower = words(lower, gx->kHi, 4);
    lower = words(lower, gx->cp, 256); lower = words(lower, gx->xf, 0x1058);
    // Job 107 compared a list address with its last use, not membership in the last VI field.
    u32 addr = (u32)(p - machine->mem1), slot = (addr >> 5) & 65535;
    for (u32 n = 0; n < 65536; n++, slot = (slot + 1) & 65535) {
        if (!lastlist[slot].used || lastlist[slot].addr == addr) {
            row.lastuse += lastlist[slot].used && lastlist[slot].key == upper;
            lastlist[slot].used = true; lastlist[slot].addr = addr; lastlist[slot].key = upper;
            break;
        }
        if (n == 65535) fail("last-use list table full");
    }
    row.calls++; row.upper += repeat(0, upper); row.lower += repeat(1, lower); row.fifo[2] += size;
}
void gxcensus_list_end(void) { dep_end(); listkey = 0; }
static float xf(const GXCore* gx, u32 a) { float f; memcpy(&f, gx->xf + a, 4); return f; }
// General affine inverse: modelview can include scale and shear.
static bool inverse(const GXCore* gx, u32 pm, double inv[3][3], double t[3]) {
    double a[3][3];
    for (int r = 0; r < 3; r++) {
        for (int c = 0; c < 3; c++) a[r][c] = xf(gx, pm + 4*r + c);
        t[r] = xf(gx, pm + 4*r + 3);
    }
    double det = a[0][0]*(a[1][1]*a[2][2]-a[1][2]*a[2][1]) - a[0][1]*(a[1][0]*a[2][2]-a[1][2]*a[2][0])
               + a[0][2]*(a[1][0]*a[2][1]-a[1][1]*a[2][0]);
    if (!isfinite(det) || fabs(det) < 1e-20) return false;
    for (int r = 0; r < 3; r++) for (int c = 0; c < 3; c++) {
        int r1=(c+1)%3, r2=(c+2)%3, c1=(r+1)%3, c2=(r+2)%3;
        inv[r][c]=(a[r1][c1]*a[r2][c2]-a[r1][c2]*a[r2][c1])/det;
    }
    return true;
}
static u64 quant(u64 h, double v) {
    if (!isfinite(v) || fabs(v) > 1e12) return word(word(h, field), 0xbad);
    long long q = llround(v * 1024.0);
    return word(word(h, (u32)q), (u32)((u64)q >> 32));
}
static u64 lighting_key(const GXCore* gx, u32 pn, u32 mask, u64 base, bool* valid) {
    double inv[3][3], t[3];
    *valid = inverse(gx, pn * 4, inv, t);
    if (!*valid) return 0;
    u64 h = word(base, pn);
    for (int l = 0; l < 8; l++) if (mask & (1u << l)) {
        u32 b = 0x600 + 16*l;
        h = words(h, gx->xf + b + 3, 7);
        for (int kind = 0; kind < 2; kind++) for (int r = 0; r < 3; r++) {
            double v = 0;
            for (int c = 0; c < 3; c++) v += inv[r][c] * (xf(gx, b + 10 + kind*3 + c) - (kind ? 0 : t[c]));
            h = quant(h, v);
        }
    }
    if (plant) h = word(h, field);
    return h;
}
void gxcensus_draw(System* s, const GXCore* gx, u32 vat, const u8* p, u32 count, u32 stride) {
    if (count) row.b[B_DRAWS]++;
    u32 chans = gx->xf[0x1009] & 3, mask = 0; bool lit = false, spec = false, vertexColor = false;
    for (u32 ch = 0; ch < chans && ch < 2; ch++) for (u32 alpha = 0; alpha < 2; alpha++) {
        u32 ctrl = gx->xf[0x100e + ch + 2*alpha];
        if (!(ctrl & 2)) continue;
        lit = true; u32 lm = ((ctrl >> 2) & 15) | (((ctrl >> 11) & 15) << 4); mask |= lm;
        spec |= lm && (ctrl & (1u << 9)) && !(ctrl & (1u << 10));
        vertexColor |= (ctrl & (1u | (1u << 6))) != 0;
    }
    if (!lit) return;
    if (spec) { row.spec += count; return; }
    row.lit += count;
    u64 base = listkey ? listkey : bytes(p, count * stride);
    base = words(base, gx->cp + 0xa0, 32);
    base = words(base, gx->xf + 0x1009, 9);
    u64 keys[64] = {0}; bool valid[64] = {0}, seen[64] = {0};
    for (u32 i = 0; i < count; i++) {
        GXVertex v; gx_decode_vertex(s, gx, vat, p + i*stride, &v);
        u32 pn = v.pnmtx & 63;
        if (!seen[pn]) { keys[pn] = lighting_key(gx, pn, mask, base, &valid[pn]); seen[pn] = true; }
        if (!valid[pn]) { row.singular++; continue; }
        u64 key = keys[pn];
        if (vertexColor) key = words(key, v.color, 2);
        row.hit += repeat(2, key);
        retain_light(key);
    }
}
static void close_field(const char* tag) {
    x1b_close_field();
    if (plant) {
        gxcensus_mem(0xfffffffc, gxbase, 4, false); gxcensus_mem(0xfffffffc, gxptr, 4, true);
    }
    emit(tag, field, &row);
    for (unsigned i = 0; i < 6; i++) { total.writes[i] += row.writes[i]; total.same[i] += row.same[i]; }
    total.calls += row.calls; total.upper += row.upper; total.lower += row.lower; total.lastuse += row.lastuse;
    for (unsigned i = 0; i < 3; i++) { total.fifo[i] += row.fifo[i]; total.active[i] += row.active[i]; }
    total.lit += row.lit; total.hit += row.hit; total.spec += row.spec; total.singular += row.singular;
    for (unsigned i = 0; i < 4; i++) total.outside[i] += row.outside[i];
    for (unsigned i = 0; i < B_N; i++) total.b[i] += row.b[i];
    for (unsigned i = 0; i < DEC_N; i++) {
        total.dec_n[i] += row.dec_n[i]; total.dec_s[i] += row.dec_s[i];
        total.dec_ticks[i] += row.dec_ticks[i]; total.dec_raw[i] += row.dec_raw[i];
        total.dec_segments[i] += row.dec_segments[i];
    }
    memset(&row, 0, sizeof row);
}
void gxcensus_field(u32 next) {
    close_field("field"); fields++; field = next + 1;
    for (unsigned k = 0; k < 3; k++) if (active_used[k]) {
        active_current[k] ^= 1;
        memset(active_sets[active_current[k]][k], 0, SET_N * sizeof(u64));
        active_used[k] = false;
    }
    current ^= 1;
    for (unsigned k = 0; k < 3; k++) memset(sets[current][k], 0, SET_N * sizeof(u64));
}
void gxcensus_start(System* s, const char* path) {
    machine = s; first = s->vi.field; field = first + 1;
    plant = getenv("GXCENSUS_PLANT") && !strcmp(getenv("GXCENSUS_PLANT"), "1");
    const char* sp = getenv("GXCENSUS_SYMBOLS");
    load_symbols(sp ? sp : "/root/tpdecomp/config/GZ2E01/symbols.txt");
    u32 live = ram_rd32(s->mem1, gxptr) & 0x3ffffff;
    if (live && live <= MEM1_SIZE - gxsize) gxbase = live;
    report = fopen(path, "w"); if (!report) fail("cannot create report");
    for (unsigned i = 0; i < 2; i++) for (unsigned k = 0; k < 3; k++) {
        sets[i][k] = calloc(SET_N, sizeof(u64)); active_sets[i][k] = calloc(SET_N, sizeof(u64));
        if (!sets[i][k] || !active_sets[i][k]) fail("out of memory");
    }
    fprintf(report, "GXCENSUS X1 plant=%u start=%u symbols=%s\n", plant, first, sp ? sp : "GZ2E01/default");
    fprintf(report, "__GXData=%08x gxData=%08x size=%x PI_FIFO_WPTR=0c003014\n", gxptr, gxbase, gxsize);
    fprintf(report, "lower=whole BP+mask+TEV banks+CP+XF shadow; upper=list bytes+16 array bases; previous field membership\n");
    fprintf(report, "lighting=affine inverse modelview; quantisation=1/1024; specular vertices excluded; no guest data retained\n");
    fprintf(report, "native_standins=%u (use GC3DS_NOHLE=1 and no AOT/PORT for complete storing-PC and memory watches)\n", hle_enabled());
    x1b_init();
    g_gxcensus = true;
}
static int watchcmp(const void* a, const void* b) {
    const Watch *x=a, *y=b; u64 nx=0, ny=0;
    for (unsigned i=0;i<4;i++) { nx+=x->n[i]; ny+=y->n[i]; }
    return nx > ny ? -1 : nx < ny;
}
void gxcensus_finish(void) {
    Counts zero = {0};
    if (memcmp(&row, &zero, sizeof row)) close_field("partial");
    emit("total", fields, &total);
    qsort(watches, nwatch, sizeof *watches, watchcmp);
    fprintf(report, "outside PCs=%u (top 20; reads/stores struct, reads/stores pointer)\n", nwatch);
    for (u32 i = 0; i < nwatch && i < 20; i++) {
        const Symbol* f = symbol(watches[i].pc);
        fprintf(report, "pc %08x %llu %llu %llu %llu %s\n", watches[i].pc, U(watches[i].n[0]),
                U(watches[i].n[1]), U(watches[i].n[2]), U(watches[i].n[3]),
                watches[i].pc == 0xfffffffc ? "PLANTED_OUTSIDE" : f ? f->name : "<unknown>");
    }
    u64 same=0, writes=0; for (unsigned i=0;i<6;i++) { same+=total.same[i]; writes+=total.writes[i]; }
    fprintf(stderr, "GXCENSUS: %u fields; unchanged %.2f%%; lists upper/lower %.2f/%.2f%%; lighting %.2f%%; outside PCs %u; plant %u\n",
            fields, writes ? 100.0*same/writes : 0, total.calls ? 100.0*total.upper/total.calls : 0,
            total.calls ? 100.0*total.lower/total.calls : 0, total.lit ? 100.0*total.hit/total.lit : 0, nwatch, plant);
    x1b_summary();
    if (fclose(report)) fail("report write failed");
    x1b_free();
    g_gxcensus = false;
    for (unsigned i=0;i<2;i++) for (unsigned k=0;k<3;k++) { free(sets[i][k]); free(active_sets[i][k]); }
}
#endif
