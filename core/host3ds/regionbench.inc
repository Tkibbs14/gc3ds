// X4 only: replay one real call. No recognizer, allocator, or region compiler is installed in the emulator.
#include <stdlib.h>
#include <stddef.h>
#include "../src/cpu/memo.h"
#include "regionbench.h"

_Static_assert(offsetof(Gekko, fpr) == 128 && offsetof(Gekko, lr) == 648 && offsetof(Gekko, pc) == 664,
               "region_oracle.S uses these architectural offsets");
extern void x4_empty(Gekko*, u8*);
extern void x4_pane(Gekko*, u8*);
extern void x4_tex(Gekko*, u8*);
extern void x4_pos(Gekko*, u8*);
extern void (*gp_capture)(const u8*, u32);
extern void* g_memoRec;
bool region_bench_done, region_bench_pending;
static FILE* x4_log;
static unsigned x4_which;
static bool x4_trace, x4_bad, x4_planting;
static u32 x4_ci, x4_nc;
static struct { u32 target, ret, n; } x4_calls[2048];
typedef struct {
    u8 pipe[262144], pending[32], want_pipe[262144];
    u32 before[CHK_MAX][2], after[CHK_MAX];
} X4Buffers;
static X4Buffers* x4_buf;
#define x4_pipe (x4_buf->pipe)
#define x4_pending (x4_buf->pending)
#define x4_want_pipe (x4_buf->want_pipe)
#define x4_before (x4_buf->before)
#define x4_after (x4_buf->after)
static u32 x4_pn, x4_len;

static void x4_capture(const u8* d, u32 n) {
    if (n > sizeof x4_pipe - x4_pn) { x4_bad = true; return; }
    memcpy(x4_pipe + x4_pn, d, n); x4_pn += n;
}
static void x4_direct(const u8* d, u32 n) {
    // Native callees hand whole byte strings to gp_fifo_write; preserve preceding JIT-buffered bytes.
    if (x4_len) { x4_capture(x4_pending, x4_len); x4_len = 0; }
    x4_capture(d, n);
}
static void x4_put(void* ctx, u32 v, u32 n) {
    (void)ctx;
    for (u32 i = 0; i < n; i++) {
        x4_pending[x4_len++] = (u8)(v >> (8 * (n - i - 1)));
        if (x4_len == 32) { x4_capture(x4_pending, 32); x4_len = 0; }
    }
}
// Mirror gekko_run's contract: a zero JIT return asks the caller to interpret one instruction.
static u32 x4_jit_step(Gekko* g, u32 budget) {
    u32 n = jit_run(g_j, budget);
    if (n) return n;
    u32 pa;
    bool stop;
    if (!gk_translate(g, g->pc, false, false, &pa, false) || pa >= 0x1800000u) { x4_bad = true; return 0; }
    g->pc = gekko_interp_one(g, g->pc, ram_rd32(g_s->mem1, pa), &stop);
    if (g->pc < 0x80000000u) x4_bad = true;
    return 1;
}
// Called by the fixed assembly at an external call boundary. Callees stay on today's JIT, not a new translator.
// Exact budgets come from an untimed interpreter pass of this same captured call, including nested calls.
void x4_call(Gekko* g, u32 target, u32 ret) {
    g->pc = target; g->lr = ret;
    if (x4_trace) {
        int why;
        u32 n = gekko_run_call(g, ret, g->gpr[1], 10000000, &why, false);
        if (why || x4_nc == 2048) { x4_bad = true; return; }
        fprintf(x4_log, "# regionbench external target %08lx ret %08lx guest %lu\n",
                (unsigned long)target, (unsigned long)ret, (unsigned long)n);
        x4_calls[x4_nc].target = target; x4_calls[x4_nc].ret = ret; x4_calls[x4_nc++].n = n;
    } else {
        if (x4_ci >= x4_nc || x4_calls[x4_ci].target != target || x4_calls[x4_ci].ret != ret) {
            x4_bad = true; return;
        }
        u32 n = x4_calls[x4_ci++].n, ran = 0;
        while (ran < n) {
            GKP(g)->brk = GKP(g)->stopReq = false;
            u32 k = x4_jit_step(g, n - ran);
            if (!k || k > n - ran) { x4_bad = true; break; }
            ran += k;
        }
        if (g->pc != ret) x4_bad = true;
    }
}
void x4_pipe_word(Gekko* g, u32 v, u32 n) { (void)g; x4_put(NULL, v, n); }

static u32 x4_nw;
static Gekko x4_entry, x4_expect;
static u8 x4_priv[sizeof(GkPriv) - offsetof(GkPriv, npc)];
static u32 x4_want_n;
static u64 x4_live_hash;
static u64 x4_hash(const u8* p, u32 n) {
    u64 h = 1469598103934665603ull;
    while (n--) h = (h ^ *p++) * 1099511628211ull;
    return h;
}
static u32 x4_dead_stack_bytes(void) {
    // The captured pane is a childless base pane: frame 272 + PSMTXConcat's 64. Texture uses 64; position is a leaf.
    // Do not exclude an arbitrary span below SP: it can contain live globals/vtables.
    return x4_which == 1 ? 336 : x4_which == 2 ? 64 : 0;
}
static u64 x4_memory_hash(void) { return x4_hash(g_s->mem1, 0x1800000u); }
static bool x4_no_extra_writes(void) {
    for (u32 k = 0; k < x4_nw; k++) ram_wr32(g_s->mem1, x4_before[k][0] & 0x3fffffffu, x4_before[k][1]);
    bool ok = x4_memory_hash() == x4_live_hash;
    if (!ok && !x4_planting) fprintf(x4_log, "# regionbench unrecorded live memory writes; FAILED\n");
    return ok;
}
static void x4_reset(Gekko* g) {
    for (u32 k = 0; k < x4_nw; k++) ram_wr32(g_s->mem1, x4_before[k][0] & 0x3fffffffu, x4_before[k][1]);
    *g = x4_entry; memcpy(&GKP(g)->npc, x4_priv, sizeof x4_priv);
    g->extIrq = false; g->msr &= ~MSR_EE;
    GKP(g)->brk = GKP(g)->stopReq = false;
    x4_pn = x4_len = x4_ci = 0;
}
static void x4_finish_pipe(void) { x4_capture(x4_pending, x4_len); x4_len = 0; }
static bool x4_equal(Gekko* g) {
    bool ok = g->pc == x4_expect.pc && g->gpr[1] == x4_expect.gpr[1] &&
              g->gpr[2] == x4_expect.gpr[2] && !memcmp(g->gpr + 13, x4_expect.gpr + 13, 19 * 4) &&
              !memcmp(g->fpr + 14, x4_expect.fpr + 14, 18 * sizeof(FPR)) &&
              (g->cr & 0x00fff000u) == (x4_expect.cr & 0x00fff000u) &&
              x4_pn == x4_want_n && !memcmp(x4_pipe, x4_want_pipe, x4_pn);
    // Exclude only the dead callee stack; globals below the stack are still live caller memory.
    for (u32 k = 0; k < x4_nw; k++) {
        u32 a = x4_before[k][0];
        if (!(a < x4_entry.gpr[1] && a >= x4_entry.gpr[1] - x4_dead_stack_bytes()) && ram_rd32(g_s->mem1, a & 0x3fffffffu) != x4_after[k]) {
            if (!x4_planting) fprintf(x4_log, "# regionbench memory mismatch %08lx expected %08lx got %08lx\n",
                    (unsigned long)a, (unsigned long)x4_after[k],
                    (unsigned long)ram_rd32(g_s->mem1, a & 0x3fffffffu)); ok = false; break;
        }
    }
    if ((!ok || x4_bad) && !x4_planting) {
        u32 diff = 0; while (diff < x4_pn && diff < x4_want_n && x4_pipe[diff] == x4_want_pipe[diff]) diff++;
        fprintf(x4_log, "# regionbench mismatch pc %08lx/%08lx sp %08lx/%08lx pipe %lu/%lu firstdiff %lu bad %d regs %d fpr %d cr %d\n",
                (unsigned long)g->pc, (unsigned long)x4_expect.pc, (unsigned long)g->gpr[1], (unsigned long)x4_expect.gpr[1],
                (unsigned long)x4_pn, (unsigned long)x4_want_n, (unsigned long)diff, x4_bad,
                memcmp(g->gpr+13,x4_expect.gpr+13,19*4), memcmp(g->fpr+14,x4_expect.fpr+14,18*sizeof(FPR)),
                (g->cr & 0x00fff000u) != (x4_expect.cr & 0x00fff000u));
    }
    return ok && !x4_bad;
}
static void (*x4_oracle)(Gekko*, u8*);
static u32 x4_count;
static void x4_once(bool native) {
    Gekko* g = &g_s->cpu;
    x4_reset(g);
    if (native) x4_oracle(g, g_s->mem1);
    else {
        u32 ran = 0;
        while (ran < x4_count) {
            GKP(g)->brk = GKP(g)->stopReq = false;
            u32 n = x4_jit_step(g, x4_count - ran);
            if (!n || n > x4_count - ran) {
                fprintf(x4_log,"# regionbench budget ran %lu got %lu total %lu pc %08lx status %lu\n",
                        (unsigned long)ran,(unsigned long)n,(unsigned long)x4_count,(unsigned long)g->pc,
                        (unsigned long)g->jitStatus);
                x4_bad = true; break;
            }
            ran += n;
        }
    }
    x4_finish_pipe();
}
static void x4_once_empty(bool native) {
    Gekko* g = &g_s->cpu;
    x4_reset(g);
    if (native) x4_empty(g, g_s->mem1);
    else { g->pc = 0x80000000u | CODE_PA; x4_jit_step(g, 1); }
    x4_finish_pipe();
}
static bool x4_empty_equal(const Gekko* g) {
    return g->pc == x4_entry.lr && g->gpr[1] == x4_entry.gpr[1] && x4_pn == 0 &&
           !memcmp(g->gpr + 13, x4_entry.gpr + 13, 19 * 4) &&
           !memcmp(g->fpr + 14, x4_entry.fpr + 14, 18 * sizeof(FPR)) && !x4_bad;
}
static void x4_dump(const u32* code, u32 words) {
    // The callback is invoked before linking; retain emitted ARM words on the SD, never in git or relay logs.
    static FILE* f;
    static unsigned previous;
    if (previous != x4_which) { if (f) fclose(f); f = NULL; previous = x4_which; }
    if (!f) {
        char path[80]; snprintf(path, sizeof path, "sdmc:/gc3ds/x4_jit_%u.bin", x4_which);
        f = fopen(path, "wb");
    }
    if (f) {
        u32 header[] = {g_s->cpu.pc, (u32)(uintptr_t)code, words};
        bool ok = fwrite(header, sizeof header, 1, f) == 1 && fwrite(code, 4, words, f) == words &&
                  fflush(f) == 0;
        fprintf(x4_log, "# regionbench ARM record pc %08lx words %lu file-bytes %ld %s\n",
                (unsigned long)header[0], (unsigned long)words, ftell(f), ok ? "saved" : "FAILED");
        if (!ok) x4_bad = true;
    } else { x4_bad = true; fprintf(x4_log, "# regionbench ARM dump open FAILED\n"); }
}
static u32 x4_run(Gekko* g) {
    gxthread_drain();
    char capture_path[80];
    snprintf(capture_path, sizeof capture_path, "sdmc:/gc3ds/x4_input_%u.state", x4_which);
    fprintf(x4_log, "# regionbench saving real-call input %s\n", capture_path); fflush(x4_log);
    if (!sys_save_state(g_s, capture_path)) { fprintf(x4_log, "# regionbench capture save FAILED\n"); }
    u32 memo = g_memoAddr; g_memoAddr = 0;
    x4_entry = *g; memcpy(x4_priv, &GKP(g)->npc, sizeof x4_priv);
    x4_entry.extIrq = false; x4_entry.msr &= ~MSR_EE;
    *g = x4_entry; GKP(g)->brk = GKP(g)->stopReq = false;
    // chk_note reads the lazily populated rhost map before the store queries it. Prime standard MEM1 mappings,
    // otherwise the first LR save on a cold data page is absent from the undo log (X4 job 734 caught this).
    for (u32 pa = 0; pa < 0x1800000u; pa += GK_BLK_SIZE) {
        for (u32 mirror = 0; mirror < 2; mirror++) {
            u32 ea = (mirror ? 0xc0000000u : 0x80000000u) + pa, translated, value;
            if (gk_translate(g, ea, false, true, &translated, false) && translated == pa) gk_rd8(g, ea, &value);
        }
    }
    x4_live_hash = x4_memory_hash();
    g_chkN = 0; x4_pn = x4_len = 0;
    gp_capture = x4_direct; g_memoRec = (void*)2;
    int why = 0;
    x4_count = gekko_run_call(g, g->lr, g->gpr[1], 10000000, &why, false);
    g_memoRec = NULL; x4_expect = *g;
    x4_nw = g_chkN;
    memcpy(x4_before, g_chkW, x4_nw * 8);
    for (u32 k = 0; k < x4_nw; k++) x4_after[k] = ram_rd32(g_s->mem1, x4_before[k][0] & 0x3fffffffu);
    x4_want_n = x4_pn; memcpy(x4_want_pipe, x4_pipe, x4_pn);
    fprintf(x4_log, "# regionbench capture pc %08lx r3 %08lx guest %lu writes %lu pipe %lu why %d input-reg-hash %016llx\n",
            (unsigned long)x4_entry.pc, (unsigned long)x4_entry.gpr[3], (unsigned long)x4_count,
            (unsigned long)x4_nw, (unsigned long)x4_want_n, why,
            (unsigned long long)x4_hash((u8*)&x4_entry, offsetof(Gekko, hostPc)));
    x4_bad = why != 0 || x4_nw == CHK_MAX;
    jit_set_gather(g_j, x4_put, NULL, GP_WGP_EA);
    jit_set_gather_buffer(g_j, x4_pending, &x4_len);
    jit_invalidate_all(g_j);
    x4_oracle = x4_which == 1 ? x4_pane : x4_which == 2 ? x4_tex : x4_pos;
    x4_trace = true; x4_nc = 0;
    x4_once(true); x4_trace = false;
    bool oracle_ok = x4_equal(g); oracle_ok &= x4_no_extra_writes();
    jit_debug_on_compile = x4_dump;
    x4_once(false); jit_debug_on_compile = NULL;
    bool jit_ok = x4_equal(g); jit_ok &= x4_no_extra_writes();
    x4_once(true); oracle_ok &= x4_equal(g);
    // Prove the comparator rejects each live-output category; do not call an already-failing check a plant pass.
    bool plant_caught = oracle_ok && jit_ok;
    if (plant_caught) {
        x4_planting = true;
        g->pc ^= 4; plant_caught &= !x4_equal(g); g->pc ^= 4;
        g->gpr[14] ^= 1; plant_caught &= !x4_equal(g); g->gpr[14] ^= 1;
        u8* fp = (u8*)&g->fpr[14]; fp[0] ^= 1; plant_caught &= !x4_equal(g); fp[0] ^= 1;
        g->cr ^= 0x1000; plant_caught &= !x4_equal(g); g->cr ^= 0x1000;
        if (x4_pn) {
            x4_pipe[x4_pn - 1] ^= 1; plant_caught &= !x4_equal(g); x4_pipe[x4_pn - 1] ^= 1;
        } else {
            x4_pipe[0] = 0x80; x4_pn = 1; plant_caught &= !x4_equal(g); x4_pn = 0;
        }
        for (u32 k = 0; k < x4_nw; k++) {
            u32 a = x4_before[k][0];
            if (a < x4_entry.gpr[1] && a >= x4_entry.gpr[1] - x4_dead_stack_bytes()) continue;
            u32 pa = a & 0x3fffffffu;
            ram_wr32(g_s->mem1, pa, x4_after[k] ^ 1);
            plant_caught &= !x4_equal(g);
            ram_wr32(g_s->mem1, pa, x4_after[k]);
            break;
        }
    }
    x4_planting = false;
    oracle_ok &= x4_no_extra_writes();
    if (oracle_ok && jit_ok && plant_caught) {
        x4_planting = true;
        u32 probe = (x4_entry.gpr[1] & 0x3fffffffu) - 4096;
        g_s->mem1[probe] ^= 1;
        plant_caught &= !x4_no_extra_writes();
        g_s->mem1[probe] ^= 1;
        plant_caught &= x4_no_extra_writes();
        x4_planting = false;
    }
    fprintf(x4_log, "# regionbench check oracle %s jit %s plant %s callouts %lu\n",
            oracle_ok ? "PASS" : "FAIL", jit_ok ? "PASS" : "FAIL", plant_caught ? "CAUGHT" : oracle_ok && jit_ok ? "MISSED" : "NOT-RUN",
            (unsigned long)x4_nc);
    fflush(x4_log);
    u32 empty_word = ram_rd32(g_s->mem1, CODE_PA);
    ram_wr32(g_s->mem1, CODE_PA, 0x4e800020u);  // synthetic blr, as in the existing jitbench
    x4_once_empty(false); bool empty_ok = x4_empty_equal(g);
    x4_once_empty(true); empty_ok &= x4_empty_equal(g);
    g->pc ^= 4; bool empty_plant = !x4_empty_equal(g); g->pc ^= 4;
    fprintf(x4_log, "# regionbench empty controls %s plant %s\n",
            empty_ok ? "PASS" : "FAIL", empty_ok && empty_plant ? "CAUGHT" : "NOT-RUN");
    oracle_ok &= empty_ok && empty_plant;
    u64 compiled_before = jit_stats(g_j)->blocksCompiled;
    if (oracle_ok && jit_ok && plant_caught) {
        // Resetting the same captured write set is common to both arms and reported as a separate control.
        // Every row is a separate PMU interval. Report all repeats; do not select the minimum.
        for (u32 rep = 0; rep < 7; rep++) for (u32 native = 0; native < 5; native++) for (u32 pair = 0; pair < 2; pair++) {
            pmu_select(pair);
            for (u32 k = 0; k < 256; k++) {
                if (native == 2) x4_reset(g);
                else if (native >= 3) x4_once_empty(native == 4);
                else x4_once(native != 0);
            }
            unsigned long long cy, e0, e1;
            pmu_read(&cy, &e0, &e1);
            if ((native < 2 && !x4_equal(g)) || (native >= 3 && !x4_empty_equal(g))) {
                fprintf(x4_log, "# regionbench timed replay FAILED; discard samples\n");
                goto finished;
            }
            fprintf(x4_log, "# regionbench sample rep %lu arm %s pair %lu cycles %.3f ev0 %.5f ev1 %.5f calls 256\n",
                    (unsigned long)rep, native == 4 ? "region_empty" : native == 3 ? "jit_empty" : native == 2 ? "reset" : native ? "region" : "jit", (unsigned long)pair,
                    (double)cy / 256, (double)e0 / 256, (double)e1 / 256);
            fflush(x4_log);
        }
    }
finished:
    ram_wr32(g_s->mem1, CODE_PA, empty_word);
    fprintf(x4_log, "# regionbench timed compiles %llu; jit ARM dump sdmc:/gc3ds/x4_jit_%u.bin\n",
            (unsigned long long)(jit_stats(g_j)->blocksCompiled - compiled_before), x4_which);
    // Exit after the experiment; no replayed state or pipe bytes are committed back to gameplay.
    gp_capture = NULL; g_memoAddr = memo;
    x4_reset(g); free(x4_buf); x4_buf = NULL; region_bench_done = true;
    GKP(g)->stopReq = true;
    return 1;
}
static Gekko x4_live;
static u32 x4_capture_entry(Gekko* g) {
    if (!(g->msr & MSR_FP)) return 0;
    if (x4_which == 1) {
        u32 a = g->gpr[3] & 0x3fffffffu;
        if (a >= 0x1800000u - 240 || !g_s->mem1[a + 176] || ram_rd32(g_s->mem1, a + 220)) return 0;
    }
    if (region_bench_pending) { GKP(g)->stopReq = true; g_s->stopRequested = true; return 1; }
    x4_live = *g;
    g_s->stopRequested = true;
    region_bench_pending = true;
    GKP(g)->stopReq = true;
    return 1;
}
void region_bench_execute(void) {
    g_s->cpu = x4_live;
    GKP(&g_s->cpu)->stopReq = GKP(&g_s->cpu)->brk = false;
    g_memoBench = NULL;
    x4_run(&g_s->cpu);
    region_bench_pending = false;
}
void region_bench_arm(Jit* j, System* s, FILE* f, unsigned which) {
    region_bench_done = region_bench_pending = false; x4_bad = false;
    g_j = j; g_s = s; x4_log = f; x4_which = which;
    if (which < 1 || which > 3) { fprintf(f, "# regionbench invalid selector\n"); region_bench_done = true; return; }
    if (!x4_buf) x4_buf = calloc(1, sizeof *x4_buf);
    if (!x4_buf) { fprintf(f, "# regionbench allocation FAILED\n"); region_bench_done = true; return; }
    if (!pmu_init()) { fprintf(f, "# regionbench PMU unavailable: %s\n", pmu_status()); region_bench_done = true; return; }
    static const struct { u32 pa, bytes; u64 hash; } code[] = {
        {0x2f67e0,0x538,0x8ad3f442a33f352bull}, {0x35e298,0x17c,0xf7a78fcbce33d188ull},
        {0x36024c,0x50,0x632dd08e77c0d266ull}, {0x3464b0,0x34,0x6114825e233ef23dull},
        {0x3464e4,0xcc,0x44b676a3c27677e6ull}, {0x35c984,0x28,0xafe359f44ca4b304ull}
    };
    for (u32 k = 0; k < sizeof code / sizeof code[0]; k++) {
        if (x4_hash(s->mem1 + code[k].pa, code[k].bytes) == code[k].hash) continue;
        fprintf(f, "# regionbench source code mismatch %08lx; refused\n", (unsigned long)code[k].pa);
        region_bench_done = true; return;
    }
    g_memoAddr = which == 1 ? 0x802f67e0u : which == 2 ? 0x8035e298u : 0x8036024cu;
    g_memoBench = x4_capture_entry;
}
