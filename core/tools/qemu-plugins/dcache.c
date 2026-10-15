// qemu TCG plugin: the ARM11's L1 data cache and the New 3DS's L2, simulated for one thread, with misses counted per
// cache line, so the 2DS's data stalls (22% of core 0 in TP, relay job 184) can be traced to the data they come from:
// guest RAM, the JIT's own tables, the emulator's structures, the stack (dcache_report.py maps lines to regions).
//   qemu-arm -plugin libdcache.so,out=path[,kb=16][,ways=4][,l2kb=2048][,l2ways=16][,vcpu=0] -d plugin build-arm/gcemu ...
// Geometry as measured on the New 2DS XL (timingprobe.c, jobs 181 / 183; docs/research/TIMING_MODEL_PLAN.md): L1 D
// 16 KB 4-way and L2 2 MB 16-way, 32-byte lines, neither LRU (L1: round-robin here; L2: LRU here, an approximation),
// write-allocate (a store miss fills its line: one store per line cost ~2x a load miss). The L2 sees only this
// thread's data, not code or the other cores (it is shared with both on the device), so its misses are a lower bound.
// `out` gets one line per cache line that missed in L1: "addr rd_miss wr_miss l2_miss accesses" (hex, then decimal),
// then one per instruction that missed: "pc ADDR rd_miss wr_miss l2_miss".
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <glib.h>
#include <qemu-plugin.h>

QEMU_PLUGIN_EXPORT int qemu_plugin_version = QEMU_PLUGIN_VERSION;

#define LINE 32u
static uint32_t g_sets, g_ways = 4, g_kb = 16, g_l2sets, g_l2ways = 16, g_l2kb = 2048;
static uint64_t *g_tag, *g_l2tag, *g_l2age, g_clock;  // line address + 1 (0: empty); L2 last-use times (LRU)
static uint32_t* g_next;  // L1 round-robin victim per set
static GHashTable* g_lines;
static GHashTable* g_pcs;  // instruction address -> Stat: which code made the misses (JIT code vs C functions)
static uint64_t g_rd, g_wr, g_rdMiss, g_wrMiss, g_l2Miss;
static unsigned g_vcpu;
static char g_out[512] = "dcache.txt";
static GMutex g_lock;

typedef struct { uint64_t rd, wr, l2, access; } Stat;

static bool l2_touch(uint64_t line) {  // true on a hit
    uint32_t set = (uint32_t)((line / LINE) % g_l2sets);
    uint64_t *t = g_l2tag + (size_t)set * g_l2ways, *a = g_l2age + (size_t)set * g_l2ways;
    g_clock++;
    uint32_t victim = 0;
    for (uint32_t w = 0; w < g_l2ways; w++) {
        if (t[w] == line + 1) { a[w] = g_clock; return true; }
        if (a[w] < a[victim]) victim = w;
    }
    t[victim] = line + 1;
    a[victim] = g_clock;
    return false;
}

static void touch(uint64_t addr, bool store, uint64_t pc) {
    uint64_t line = addr & ~(uint64_t)(LINE - 1);
    uint32_t set = (uint32_t)((line / LINE) % g_sets);
    uint64_t* t = g_tag + (size_t)set * g_ways;
    if (store) g_wr++; else g_rd++;
    for (uint32_t w = 0; w < g_ways; w++) if (t[w] == line + 1) return;
    Stat* s = g_hash_table_lookup(g_lines, (gpointer)(uintptr_t)line);
    if (!s) { s = g_new0(Stat, 1); g_hash_table_insert(g_lines, (gpointer)(uintptr_t)line, s); }
    s->access++;
    if (store) { g_wrMiss++; s->wr++; } else { g_rdMiss++; s->rd++; }
    Stat* q = g_hash_table_lookup(g_pcs, (gpointer)(uintptr_t)pc);
    if (!q) { q = g_new0(Stat, 1); g_hash_table_insert(g_pcs, (gpointer)(uintptr_t)pc, q); }
    if (store) q->wr++; else q->rd++;
    if (!l2_touch(line)) { g_l2Miss++; s->l2++; q->l2++; }
    t[g_next[set]] = line + 1;  // write-allocate: loads and stores both fill the line
    g_next[set] = (g_next[set] + 1) % g_ways;
}

static uint64_t g_codeLo, g_codeHi = UINT64_MAX;  // code=lo-hi (hex): only accesses made by code there (e.g. the JIT
                                                  // buffer: the PC build renders on the same thread, the 2DS on others)
static uint64_t g_skip, g_seen;  // skip=N: the first N accesses only warm the caches (not counted): a run's start
                                 // after a state load is compile and flush work, not steady play
static void mem_cb(unsigned vcpu, qemu_plugin_meminfo_t info, uint64_t vaddr, void* udata) {
    if (vcpu != g_vcpu) return;
    uint64_t ipc = (uint64_t)(uintptr_t)udata;
    if (ipc < g_codeLo || ipc >= g_codeHi) return;
    if (g_seen < g_skip) {
        g_seen++;
        if (g_seen == g_skip) { g_rd = g_wr = g_rdMiss = g_wrMiss = g_l2Miss = 0; g_hash_table_remove_all(g_lines); g_hash_table_remove_all(g_pcs); }
    }
    unsigned size = 1u << qemu_plugin_mem_size_shift(info);
    bool store = qemu_plugin_mem_is_store(info);
    g_mutex_lock(&g_lock);
    uint64_t pc = (uint64_t)(uintptr_t)udata;
    touch(vaddr, store, pc);
    if (((vaddr + size - 1) ^ vaddr) & ~(uint64_t)(LINE - 1)) touch(vaddr + size - 1, store, pc);  // crosses a line
    g_mutex_unlock(&g_lock);
}

static void tb_trans(qemu_plugin_id_t id, struct qemu_plugin_tb* tb) {
    size_t n = qemu_plugin_tb_n_insns(tb);
    for (size_t i = 0; i < n; i++)
    {
        struct qemu_plugin_insn* in = qemu_plugin_tb_get_insn(tb, i);
        qemu_plugin_register_vcpu_mem_cb(in, mem_cb, QEMU_PLUGIN_CB_NO_REGS, QEMU_PLUGIN_MEM_RW,
                                         (void*)(uintptr_t)qemu_plugin_insn_vaddr(in));
    }
}

static void at_exit(qemu_plugin_id_t id, void* p) {
    FILE* f = fopen(g_out, "w");
    if (!f) return;
    fprintf(f, "# total loads %" PRIu64 " stores %" PRIu64 " load misses %" PRIu64 " store misses %" PRIu64
            " l2 misses %" PRIu64 " (L1 %u KB %u ways, L2 %u KB %u ways, %u-byte lines, vcpu %u)\n",
            g_rd, g_wr, g_rdMiss, g_wrMiss, g_l2Miss, g_kb, g_ways, g_l2kb, g_l2ways, LINE, g_vcpu);
    GHashTableIter it;
    gpointer k, v;
    g_hash_table_iter_init(&it, g_lines);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        const Stat* s = v;
        fprintf(f, "%" PRIx64 " %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 "\n", (uint64_t)(uintptr_t)k, s->rd, s->wr, s->l2, s->access);
    }
    // Then the code that made them: "pc ADDR rd_miss wr_miss l2_miss".
    g_hash_table_iter_init(&it, g_pcs);
    while (g_hash_table_iter_next(&it, &k, &v)) {
        const Stat* s = v;
        fprintf(f, "pc %" PRIx64 " %" PRIu64 " %" PRIu64 " %" PRIu64 "\n", (uint64_t)(uintptr_t)k, s->rd, s->wr, s->l2);
    }
    fclose(f);
}

QEMU_PLUGIN_EXPORT int qemu_plugin_install(qemu_plugin_id_t id, const qemu_info_t* info, int argc, char** argv) {
    for (int i = 0; i < argc; i++) {
        if (!strncmp(argv[i], "out=", 4)) snprintf(g_out, sizeof g_out, "%s", argv[i] + 4);
        else if (!strncmp(argv[i], "kb=", 3)) g_kb = (uint32_t)atoi(argv[i] + 3);
        else if (!strncmp(argv[i], "ways=", 5)) g_ways = (uint32_t)atoi(argv[i] + 5);
        else if (!strncmp(argv[i], "l2kb=", 5)) g_l2kb = (uint32_t)atoi(argv[i] + 5);
        else if (!strncmp(argv[i], "l2ways=", 7)) g_l2ways = (uint32_t)atoi(argv[i] + 7);
        else if (!strncmp(argv[i], "vcpu=", 5)) g_vcpu = (unsigned)atoi(argv[i] + 5);
        else if (!strncmp(argv[i], "skip=", 5)) g_skip = strtoull(argv[i] + 5, NULL, 10);
        else if (!strncmp(argv[i], "code=", 5)) {
            char* dash;
            g_codeLo = strtoull(argv[i] + 5, &dash, 16);
            if (*dash == '-') g_codeHi = strtoull(dash + 1, NULL, 16);
        }
    }
    g_sets = g_kb * 1024 / LINE / g_ways;
    g_l2sets = g_l2kb * 1024 / LINE / g_l2ways;
    g_tag = calloc((size_t)g_sets * g_ways, sizeof *g_tag);
    g_next = calloc(g_sets, sizeof *g_next);
    g_l2tag = calloc((size_t)g_l2sets * g_l2ways, sizeof *g_l2tag);
    g_l2age = calloc((size_t)g_l2sets * g_l2ways, sizeof *g_l2age);
    g_lines = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    g_pcs = g_hash_table_new_full(g_direct_hash, g_direct_equal, NULL, g_free);
    qemu_plugin_register_vcpu_tb_trans_cb(id, tb_trans);
    qemu_plugin_register_atexit_cb(id, at_exit, NULL);
    return 0;
}
