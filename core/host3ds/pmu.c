// ARM11 MPCore performance counters (see pmu.h). Kernel interface: svcControlPerformanceCounter; event numbers are
// the ARM11 MPCore TRM's (CP15 c15 PMNC events and SCU monitor events).
#include <3ds.h>
#include <stdlib.h>
#include <string.h>
#include "pmu.h"

typedef struct { PerfCounterEvent ev; const char* col; double scale; } Ev;  // scale: per-second value / scale
// The pairs that rotate on the two core event counters, one pair per second.
static const Ev kPairs[][2] = {
    { { PERFCOUNTEREVT_CORE_INST_EXECUTED, "inst_m", 1e6 }, { PERFCOUNTEREVT_CORE_BRANCH_MISS_PREDICTED, "brmiss_k", 1e3 } },
    { { PERFCOUNTEREVT_CORE_INST_CACHE_MISS, "icmiss_k", 1e3 }, { PERFCOUNTEREVT_CORE_DATA_CACHE_READ_MISS, "dcrmiss_k", 1e3 } },
    { { PERFCOUNTEREVT_CORE_STALL_BY_LACK_OF_INST, "stall_inst_m", 1e6 }, { PERFCOUNTEREVT_CORE_STALL_BY_DATA_HAZARD, "stall_data_m", 1e6 } },
    { { PERFCOUNTEREVT_CORE_DATA_CACHE_WRITE_MISS, "dcwmiss_k", 1e3 }, { PERFCOUNTEREVT_CORE_MAIN_TLB_MISS, "tlbmiss_k", 1e3 } },
    { { PERFCOUNTEREVT_CORE_BRANCH_INST, "branch_m", 1e6 }, { PERFCOUNTEREVT_CORE_STALL_BY_LSU_FULL, "stall_lsu_m", 1e6 } },
    // For the ~21% of cycles no event above explains (MEASUREMENTS.md): branches with no prediction (the BTAC had no
    // entry: a fetch bubble each) and PC writes by software (returns, indirect jumps); D-cache reads (load-use waits
    // per access) and folded branches; micro-TLB misses; external requests and store-buffer drains. ARM11 MPCore TRM
    // events 0x06, 0x0F, 0x0A, 0x09, 0x03, 0x04, 0x11, 0x13. The VFP has no stall event: its latency is what remains.
    { { PERFCOUNTEREVT_CORE_BRANCH_NOT_PREDICTED, "brnopred_m", 1e6 }, { PERFCOUNTEREVT_CORE_PC_CHANGED, "pcchg_m", 1e6 } },
    { { PERFCOUNTEREVT_CORE_DATA_CACHE_READ, "dcread_m", 1e6 }, { PERFCOUNTEREVT_CORE_FOLDED_INST_EXECUTED, "folded_m", 1e6 } },
    { { PERFCOUNTEREVT_CORE_INST_MICRO_TLB_MISS, "imtlb_k", 1e3 }, { PERFCOUNTEREVT_CORE_DATA_MICRO_TLB_MISS, "dmtlb_k", 1e3 } },
    { { PERFCOUNTEREVT_CORE_EXTERNAL_REQUEST, "extreq_k", 1e3 }, { PERFCOUNTEREVT_CORE_STORE_BUFFER_DRAIN, "sbdrain_k", 1e3 } },
};
#define NPAIRS (sizeof kPairs / sizeof kPairs[0])
// The SCU counters, fixed: line fills from cores 0 and 2 that missed in every L1 (they go to the L2 / memory), and
// the SCU's reads and writes on its external ports.
static const Ev kScu[4] = {
    { PERFCOUNTEREVT_SCU_LINEFILL_MISS_FROM_CORE0, "scu_c0miss_k", 1e3 },
    { PERFCOUNTEREVT_SCU_LINEFILL_MISS_FROM_CORE2, "scu_c2miss_k", 1e3 },
    { PERFCOUNTEREVT_SCU_EXTERNAL_READ, "scu_rd_k", 1e3 },
    { PERFCOUNTEREVT_SCU_EXTERNAL_WRITE, "scu_wr_k", 1e3 },
};

static bool g_on;
static u32 g_pair;
static char g_status[96] = "not started";
static char g_cols[400];

static Result ctl(u64* out, PerfCounterOperation op, u32 p1, u64 p2) {
    u64 dummy;
    return svcControlPerformanceCounter(out ? out : &dummy, op, p1, p2);
}

static u64 get(u32 reg) {
    u64 v = 0;
    ctl(&v, PERFCOUNTEROP_GET_VALUE, reg, 0);
    return v;
}

// Arms pair p on the core counters and clears every counter.
static void arm_pair(u32 p) {
    g_pair = p;
    ctl(NULL, PERFCOUNTEROP_SET_EVENT, PERFCOUNTERREG_CORE_COUNT_REG_0, kPairs[p][0].ev);
    ctl(NULL, PERFCOUNTEROP_SET_EVENT, PERFCOUNTERREG_CORE_COUNT_REG_1, kPairs[p][1].ev);
    u32 mask = 0x7u | (0xFu << PERFCOUNTERREG_SCU_0);
    ctl(NULL, PERFCOUNTEROP_RESET, mask, mask);
}

bool pmu_init(void) {
    if (g_on) return true;
    Result r = ctl(NULL, PERFCOUNTEROP_ENABLE, 0, 0);
    if (R_FAILED(r)) { snprintf(g_status, sizeof g_status, "enable refused (%08lx)", (unsigned long)r); return false; }
    ctl(NULL, PERFCOUNTEROP_SET_VIRTUAL_COUNTER_ENABLED, 1, 0);  // 64-bit counts, overflows tracked by the kernel
    r = ctl(NULL, PERFCOUNTEROP_SET_EVENT, PERFCOUNTERREG_CORE_CYCLE_COUNTER, PERFCOUNTEREVT_CORE_CYCLE_COUNT);
    for (u32 k = 0; k < 4; k++) ctl(NULL, PERFCOUNTEROP_SET_EVENT, PERFCOUNTERREG_SCU_0 + k, kScu[k].ev);
    arm_pair(0);
    g_on = true;
    snprintf(g_status, sizeof g_status, "on (cycle event %08lx)", (unsigned long)r);
    return true;
}

const char* pmu_status(void) { return g_status; }

const char* pmu_columns(void) {
    if (g_cols[0]) return g_cols;
    size_t n = (size_t)snprintf(g_cols, sizeof g_cols, ",pmu_mcyc");
    for (u32 p = 0; p < NPAIRS; p++)
        for (u32 k = 0; k < 2; k++) n += (size_t)snprintf(g_cols + n, sizeof g_cols - n, ",%s", kPairs[p][k].col);
    for (u32 k = 0; k < 4; k++) n += (size_t)snprintf(g_cols + n, sizeof g_cols - n, ",%s", kScu[k].col);
    return g_cols;
}

// pmu_split_*: inside = sampled JIT runs (x SPLIT_EVERY), all = what pmu_row saw, both per event pair.
#define SPLIT_EVERY 8
static u64 g_inCyc[NPAIRS], g_inEv[NPAIRS][2], g_allCyc[NPAIRS], g_allEv[NPAIRS][2], g_s0[3];
static u32 g_splitN;
static bool g_inSample;

void pmu_split_begin(void) {
    if (!g_on || ++g_splitN % SPLIT_EVERY) return;
    g_s0[0] = get(PERFCOUNTERREG_CORE_CYCLE_COUNTER);
    g_s0[1] = get(PERFCOUNTERREG_CORE_COUNT_REG_0);
    g_s0[2] = get(PERFCOUNTERREG_CORE_COUNT_REG_1);
    g_inSample = true;
}

void pmu_split_end(void) {
    if (!g_inSample) return;
    g_inSample = false;
    g_inCyc[g_pair] += (get(PERFCOUNTERREG_CORE_CYCLE_COUNTER) - g_s0[0]) * SPLIT_EVERY;
    g_inEv[g_pair][0] += (get(PERFCOUNTERREG_CORE_COUNT_REG_0) - g_s0[1]) * SPLIT_EVERY;
    g_inEv[g_pair][1] += (get(PERFCOUNTERREG_CORE_COUNT_REG_1) - g_s0[2]) * SPLIT_EVERY;
}

void pmu_split_report(FILE* f) {
    if (!g_on) return;
    u64 ic = 0, ac = 0;
    for (u32 p = 0; p < NPAIRS; p++) { ic += g_inCyc[p]; ac += g_allCyc[p]; }
    fprintf(f, "# pmu split 10s (compiled code incl. its helpers, share of core 0; runs sampled 1/%d): cycles %.0f%%", SPLIT_EVERY,
            ac ? 100.0 * (double)ic / (double)ac : 0.0);
    for (u32 p = 0; p < NPAIRS; p++)
        for (u32 k = 0; k < 2; k++)
            if (g_allEv[p][k]) fprintf(f, ", %s %.0f%%", kPairs[p][k].col, 100.0 * (double)g_inEv[p][k] / (double)g_allEv[p][k]);
    fprintf(f, "\n");
    memset(g_inCyc, 0, sizeof g_inCyc); memset(g_inEv, 0, sizeof g_inEv);
    memset(g_allCyc, 0, sizeof g_allCyc); memset(g_allEv, 0, sizeof g_allEv);
}

void pmu_row(FILE* f, double secs) {
    if (!g_on) {
        for (u32 k = 0; k < 1 + 2 * NPAIRS + 4; k++) fputc(',', f);
        return;
    }
    u64 cyc = get(PERFCOUNTERREG_CORE_CYCLE_COUNTER), a = get(PERFCOUNTERREG_CORE_COUNT_REG_0),
        b = get(PERFCOUNTERREG_CORE_COUNT_REG_1), s[4];
    for (u32 k = 0; k < 4; k++) s[k] = get(PERFCOUNTERREG_SCU_0 + k);
    g_allCyc[g_pair] += cyc; g_allEv[g_pair][0] += a; g_allEv[g_pair][1] += b;  // for pmu_split_report
    fprintf(f, ",%.1f", (double)cyc / secs / 1e6);
    for (u32 p = 0; p < NPAIRS; p++)
        for (u32 k = 0; k < 2; k++) {
            if (p == g_pair) fprintf(f, ",%.2f", (double)(k ? b : a) / secs / kPairs[p][k].scale);
            else fputc(',', f);
        }
    for (u32 k = 0; k < 4; k++) fprintf(f, ",%.1f", (double)s[k] / secs / kScu[k].scale);
    arm_pair((g_pair + 1) % NPAIRS);
}

void pmu_select(unsigned p) { if (g_on) arm_pair(p % NPAIRS); }

bool pmu_read(unsigned long long* cyc, unsigned long long* ev0, unsigned long long* ev1) {
    if (!g_on) { *cyc = *ev0 = *ev1 = 0; return false; }
    *cyc = get(PERFCOUNTERREG_CORE_CYCLE_COUNTER);
    *ev0 = get(PERFCOUNTERREG_CORE_COUNT_REG_0);
    *ev1 = get(PERFCOUNTERREG_CORE_COUNT_REG_1);
    return true;
}

// 2 instructions per round, no memory access.
static void __attribute__((noinline)) spin(u32 n) {
    __asm__ volatile("1: subs %0, %0, #1\n bne 1b\n" : "+r"(n) : : "cc");
}

void pmu_probe(FILE* f) {
    if (!pmu_init()) { fprintf(f, "pmu: %s\n", g_status); return; }
    // Pair 0 counts instructions and mispredicted branches.
    arm_pair(0);
    u64 t0 = svcGetSystemTick();
    spin(10000000);
    u64 t1 = svcGetSystemTick();
    u64 cyc = get(PERFCOUNTERREG_CORE_CYCLE_COUNTER), inst = get(PERFCOUNTERREG_CORE_COUNT_REG_0),
        miss = get(PERFCOUNTERREG_CORE_COUNT_REG_1);
    double secs = (double)(t1 - t0) / SYSCLOCK_ARM11;
    fprintf(f, "pmu: %s\n", g_status);
    fprintf(f, "pmu loop of 20M instructions: %llu instructions, %llu cycles (%.0f MHz by the tick), %llu mispredicted\n",
            (unsigned long long)inst, (unsigned long long)cyc, (double)cyc / secs / 1e6, (unsigned long long)miss);
    // Pair 1: cache misses while streaming a 4 MB buffer (reads, then writes).
    u32 bytes = 4u << 20;
    u32* buf = malloc(bytes);
    if (buf) {
        arm_pair(1);
        u32 sum = 0;
        for (u32 i = 0; i < bytes / 4; i += 8) sum += buf[i];
        u64 ic = get(PERFCOUNTERREG_CORE_COUNT_REG_0), dc = get(PERFCOUNTERREG_CORE_COUNT_REG_1), sc[4];
        for (u32 k = 0; k < 4; k++) sc[k] = get(PERFCOUNTERREG_SCU_0 + k);
        fprintf(f, "pmu read of 4 MB (one word per 32-byte line, %lu lines): icache misses %llu, dcache read misses %llu, "
                "scu core0 miss %llu core2 miss %llu ext read %llu ext write %llu (sum %lx)\n",
                (unsigned long)(bytes / 32), (unsigned long long)ic, (unsigned long long)dc, (unsigned long long)sc[0],
                (unsigned long long)sc[1], (unsigned long long)sc[2], (unsigned long long)sc[3], (unsigned long)sum);
        free(buf);
    }
    arm_pair(0);
}
