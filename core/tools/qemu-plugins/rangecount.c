// qemu TCG plugin: ARM instructions executed inside address ranges, for comparing the code quality of the JIT's blocks
// and the ahead-of-time translated code (docs/research/AOT_PLAN.md).
//   qemu-arm -plugin librangecount.so,range0=lo-hi,file1=path -d plugin -D out.txt build-arm/gcemu ...
// rangeN=lo-hi (hex) gives range N one span; fileN=path gives range N every "lo hi" line (hex) of the file, e.g. each
// translated function from the symbol table (core/tools/aot/rangecount.sh writes it). The emulator prints its JIT's
// span with JIT_RANGES=1. Build against a qemu source tree configured with --enable-plugins (build.sh here).
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define MAX_RANGES 8
typedef struct { uint64_t lo, hi; int r; uint64_t insns; } Span;
static Span* g_spans;
static size_t g_nspans, g_capspans;
static uint64_t g_insns[MAX_RANGES], g_tbs[MAX_RANGES], g_other;
static int g_nranges;

typedef struct { int r; uint32_t n; Span* sp; } TbInfo;

static void add_span(uint64_t lo, uint64_t hi, int r) {
    if (g_nspans == g_capspans) {
        g_capspans = g_capspans ? 2 * g_capspans : 256;
        g_spans = realloc(g_spans, g_capspans * sizeof *g_spans);
    }
    g_spans[g_nspans++] = (Span){ lo, hi, r, 0 };
}
static int by_lo(const void* a, const void* b) {
    uint64_t x = ((const Span*)a)->lo, y = ((const Span*)b)->lo;
    return x < y ? -1 : x > y;
}
static Span* find(uint64_t pc) {  // the last span starting at or below pc
    size_t a = 0, b = g_nspans;
    while (a < b) { size_t m = (a + b) / 2; if (g_spans[m].lo <= pc) a = m + 1; else b = m; }
    if (a == 0) return NULL;
    Span* s = &g_spans[a - 1];
    return pc < s->hi ? s : NULL;
}

static void on_exec(unsigned int vcpu, void* udata) {
    (void)vcpu;
    TbInfo* t = udata;
    if (t->r >= 0) { g_insns[t->r] += t->n; g_tbs[t->r]++; t->sp->insns += t->n; }
    else g_other += t->n;
}

static void on_trans(qemu_plugin_id_t id, struct qemu_plugin_tb* tb) {
    (void)id;
    TbInfo* t = g_new0(TbInfo, 1);
    t->n = (uint32_t)qemu_plugin_tb_n_insns(tb);
    t->sp = find(qemu_plugin_tb_vaddr(tb));
    t->r = t->sp ? t->sp->r : -1;
    qemu_plugin_register_vcpu_tb_exec_cb(tb, on_exec, QEMU_PLUGIN_CB_NO_REGS, t);
}

static void at_exit(qemu_plugin_id_t id, void* p) {
    (void)id; (void)p;
    GString* s = g_string_new("rangecount:");
    for (int i = 0; i < g_nranges; i++)
        g_string_append_printf(s, " range%d insns %" PRIu64 " blocks %" PRIu64 ";", i, g_insns[i], g_tbs[i]);
    g_string_append_printf(s, " other insns %" PRIu64 "\n", g_other);
    for (size_t i = 0; i < g_nspans; i++)  // every span that ran, for per-function costs
        if (g_spans[i].insns) g_string_append_printf(s, "span %" PRIx64 " %d %" PRIu64 "\n", g_spans[i].lo, g_spans[i].r, g_spans[i].insns);
    qemu_plugin_outs(s->str);
    g_string_free(s, TRUE);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t* info, int argc, char** argv) {
    (void)info;
    for (int i = 0; i < argc; i++) {
        int r;
        unsigned long long lo, hi;
        char path[512];
        if (sscanf(argv[i], "range%d=%llx-%llx", &r, &lo, &hi) == 3 && r >= 0 && r < MAX_RANGES) {
            add_span(lo, hi, r);
            if (r + 1 > g_nranges) g_nranges = r + 1;
        } else if (sscanf(argv[i], "file%d=%511s", &r, path) == 2 && r >= 0 && r < MAX_RANGES) {
            FILE* f = fopen(path, "r");
            if (!f) { fprintf(stderr, "rangecount: cannot open %s\n", path); return -1; }
            while (fscanf(f, "%llx %llx", &lo, &hi) == 2) add_span(lo, hi, r);
            fclose(f);
            if (r + 1 > g_nranges) g_nranges = r + 1;
        }
    }
    qsort(g_spans, g_nspans, sizeof *g_spans, by_lo);
    qemu_plugin_register_vcpu_tb_trans_cb(id, on_trans);
    qemu_plugin_register_atexit_cb(id, at_exit, NULL);
    return 0;
}
