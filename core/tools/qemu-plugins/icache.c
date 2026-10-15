// qemu TCG plugin: the ARM11's L1 instruction cache, simulated for one thread, with misses counted per cache line, so
// the 2DS's instruction-fetch misses can be traced to the code they come from (JIT hot / cold code, shared stubs, C).
//   qemu-arm -plugin libicache.so,out=path[,kb=16][,ways=4][,vcpu=0] -d plugin build-arm/gcemu ...
// The 3DS's ARM11 MPCore cores have 16 KB, 4-way, 32-byte-line L1 instruction caches with round-robin replacement
// (ARM DDI 0360F). Each executed translation block touches, in order, every line its instructions cover. `out` gets
// one line per cache line that missed ("addr misses accesses", hex then decimal), after a "# total" line.
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define LINE 32u
static uint32_t g_sets, g_ways = 4, g_kb = 16;
static uint64_t* g_tag;   // g_sets * g_ways line addresses (0: empty)
static uint32_t* g_next;  // round-robin victim per set
static GHashTable* g_lines;  // line address -> Stat
static uint64_t g_access, g_miss;
static unsigned g_vcpu;
static char g_out[512] = "icache.txt";
static GMutex g_lock;

typedef struct { uint64_t miss, access; } Stat;
typedef struct { uint64_t lo; uint32_t n; } Tb;  // first line and line count of a translation block

static void touch(uint64_t line) {
    uint32_t set = (uint32_t)((line / LINE) % g_sets);
    uint64_t* t = g_tag + (size_t)set * g_ways;
    g_access++;
    Stat* s = g_hash_table_lookup(g_lines, (gpointer)(uintptr_t)line);
    if (!s) { s = g_new0(Stat, 1); g_hash_table_insert(g_lines, (gpointer)(uintptr_t)line, s); }
    s->access++;
    for (uint32_t w = 0; w < g_ways; w++) if (t[w] == line + 1) return;
    g_miss++;
    s->miss++;
    t[g_next[set]] = line + 1;
    g_next[set] = (g_next[set] + 1) % g_ways;
}

static void tb_exec(unsigned vcpu, void* udata) {
    if (vcpu != g_vcpu) return;
    const Tb* tb = udata;
    g_mutex_lock(&g_lock);
    for (uint32_t k = 0; k < tb->n; k++) touch(tb->lo + (uint64_t)k * LINE);
    g_mutex_unlock(&g_lock);
}

static void tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb* tb) {
    size_t n = qemu_plugin_tb_n_insns(tb);
    if (!n) return;
    uint64_t lo = qemu_plugin_insn_vaddr(qemu_plugin_tb_get_insn(tb, 0));
    struct qemu_plugin_insn* last = qemu_plugin_tb_get_insn(tb, n - 1);
    uint64_t hi = qemu_plugin_insn_vaddr(last) + qemu_plugin_insn_size(last) - 1;
    Tb* t = g_new(Tb, 1);  // never freed: qemu keeps translations around, and so do we
    t->lo = lo & ~(uint64_t)(LINE - 1);
    t->n = (uint32_t)(((hi & ~(uint64_t)(LINE - 1)) - t->lo) / LINE + 1);
    qemu_plugin_register_vcpu_tb_exec_cb(tb, tb_exec, QEMU_PLUGIN_CB_NO_REGS, t);
}

static void at_exit(qemu_plugin_id_t id, void* p) {
    FILE* f = fopen(g_out, "w");
    if (!f) return;
    fprintf(f, "# total accesses %" PRIu64 " misses %" PRIu64 " (%u KB, %u ways, %u-byte lines, vcpu %u)\n", g_access, g_miss,
            g_kb, g_ways, LINE, g_vcpu);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, g_lines);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        const Stat* s = v;
        if (s->miss) fprintf(f, "%" PRIx64 " %" PRIu64 " %" PRIu64 "\n", (uint64_t)(uintptr_t)k, s->miss, s->access);
    }
    fclose(f);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t* info, int argc, char** argv) {
    for (int i = 0; i < argc; i++) {
        if (!strncmp(argv[i], "out=", 4)) snprintf(g_out, sizeof g_out, "%s", argv[i] + 4);
        else if (!strncmp(argv[i], "kb=", 3)) g_kb = (uint32_t)atoi(argv[i] + 3);
        else if (!strncmp(argv[i], "ways=", 5)) g_ways = (uint32_t)atoi(argv[i] + 5);
        else if (!strncmp(argv[i], "vcpu=", 5)) g_vcpu = (unsigned)atoi(argv[i] + 5);
    }
    g_sets = g_kb * 1024 / LINE / g_ways;
    g_tag = calloc((size_t)g_sets * g_ways, sizeof *g_tag);
    g_next = calloc(g_sets, sizeof *g_next);
    g_lines = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans);
    qemu_plugin_register_atexit_cb(id, at_exit, NULL);
    return 0;
}
