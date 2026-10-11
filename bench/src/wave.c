// gcbench wave 2 (2026-10-11): memory, cross-core and audio measurements gc3ds needs (docs/research/MEASURE_PLAN.md
// tests M1, M3, M6, C1, C2, A1, A2). Runs after the GPU module.
#include <3ds.h>
#include <malloc.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "bench.h"

static volatile u32 g_sink2;

// Worker threads run at the main thread's priority, as sys.c's proven cross-core spin test does.
static s32 main_prio(void) { s32 p = 0x30; svcGetThreadPriority(&p, CUR_THREAD_HANDLE); return p; }

// ---- M1: bandwidth by heap and size ----------------------------------------------------------------------------
static u32 read_sum(const u32* p, u32 words)
{
    u32 s = 0;
    for (u32 i = 0; i + 8 <= words; i += 8) s += p[i] + p[i + 1] + p[i + 2] + p[i + 3] + p[i + 4] + p[i + 5] + p[i + 6] + p[i + 7];
    return s;
}

static void bw_one(const char* heap, u8* a, u8* b, u32 n)
{
    double mb = n / 1e6, t, r, w, c;
    int reps;
    t = now_s(); reps = 0;
    do { g_sink2 += read_sum((const u32*)a, n / 4); reps++; } while (now_s() - t < 0.08);
    r = reps * mb / (now_s() - t);
    t = now_s(); reps = 0;
    do { memset(b, reps, n); reps++; } while (now_s() - t < 0.08);
    w = reps * mb / (now_s() - t);
    t = now_s(); reps = 0;
    do { memcpy(b, a, n); reps++; } while (now_s() - t < 0.08);
    c = reps * mb / (now_s() - t);
    P("%-7s %6lu KB: read %5.0f write %5.0f copy %5.0f MB/s\n", heap, (unsigned long)(n >> 10), r, w, c);
}

static void test_bandwidth(void)
{
    P("-- M1 bandwidth by heap --\n");
    static const u32 sizes[4] = { 16u << 10, 256u << 10, 4u << 20, 12u << 20 };
    for (int h = 0; h < 2; h++) {
        for (int s = 0; s < 4; s++) {
            step(h ? "bw linear" : "bw heap");
            u32 n = sizes[s];
            u8 *a = h ? linearAlloc(n) : malloc(n), *b = h ? linearAlloc(n) : malloc(n);
            if (!a || !b) { P("%s %lu KB SKIPPED: alloc\n", h ? "linear" : "heap", (unsigned long)(n >> 10)); }
            else { memset(a, 3, n); bw_one(h ? "linear" : "heap", a, b, n); }
            if (h) { if (a) linearFree(a); if (b) linearFree(b); } else { free(a); free(b); }
        }
    }
}

// ---- M3: cache-flush cost ------------------------------------------------------------------------------------
static void test_flush(void)
{
    P("-- M3 data-cache flush cost (dirty lines) --\n");
    Handle proc = 0;
    svcDuplicateHandle(&proc, CUR_PROCESS_HANDLE);
    static const u32 sizes[5] = { 4u << 10, 64u << 10, 256u << 10, 1u << 20, 4u << 20 };
    u8* buf = linearAlloc(4u << 20);
    if (!buf) { P("M3 SKIPPED: alloc\n"); return; }
    for (int s = 0; s < 5; s++) {
        step("flush");
        u32 n = sizes[s];
        double tg = 0, tk = 0, tc = 0;
        for (int r = 0; r < 4; r++) {
            memset(buf, r, n);
            double t0 = now_s();
            GSPGPU_FlushDataCache(buf, n);
            tg += now_s() - t0;
            memset(buf, r + 1, n);
            t0 = now_s();
            svcFlushProcessDataCache(proc, (u32)buf, n);
            tk += now_s() - t0;
            t0 = now_s();
            GSPGPU_FlushDataCache(buf, n);   // already clean
            tc += now_s() - t0;
        }
        P("%5lu KB: GSPGPU %7.1f us  svc %7.1f us  clean %6.1f us\n", (unsigned long)(n >> 10), tg * 1e6 / 4,
          tk * 1e6 / 4, tc * 1e6 / 4);
    }
    linearFree(buf);
    if (proc) svcCloseHandle(proc);
}

// ---- M6: cache lines shared between cores ---------------------------------------------------------------------
static volatile u32 g_ping __attribute__((aligned(32)));
static volatile u32 g_pong __attribute__((aligned(32)));
static volatile u32 g_ctrs[16] __attribute__((aligned(32)));   // [0]/[1] same line, [0]/[8] 32 bytes apart
static volatile int g_stop2, g_ctrSlot;

static void pong_thread(void* arg)
{
    (void)arg;
    u32 last = 0;
    while (!g_stop2) {
        u32 v = g_ping;
        if (v != last) { last = v; g_pong = v; }
    }
}

static void ctr_thread(void* arg)
{
    volatile u32* c = &g_ctrs[(int)arg];
    while (!g_stop2) (*c)++;
}

static void test_coreshare(void)
{
    P("-- M6 cross-core cache line --\n");
    step("pingpong");
    g_stop2 = 0; g_ping = 0; g_pong = 0;
    Thread th = threadCreate(pong_thread, NULL, 8 * 1024, main_prio(), 2, false);
    if (!th) { P("M6 SKIPPED: thread on core 2\n"); return; }
    const u32 n = 100000;
    double t0 = now_s();
    for (u32 i = 1; i <= n; i++) {
        g_ping = i;
        double s = now_s();
        while (g_pong != i) if (now_s() - s > 0.5) { P("ping-pong STUCK at %lu\n", (unsigned long)i); i = n; break; }
    }
    double t = now_s() - t0;
    g_stop2 = 1;
    threadJoin(th, U64_MAX);
    threadFree(th);
    P("ping-pong core0<->core2: %.0f ns per round trip\n", t * 1e9 / n);
    for (int far = 0; far < 2; far++) {
        step("false sharing");
        memset((void*)g_ctrs, 0, sizeof g_ctrs);
        g_stop2 = 0;
        int other = far ? 8 : 1;
        Thread t2 = threadCreate(ctr_thread, (void*)other, 8 * 1024, main_prio(), 2, false);
        if (!t2) break;
        double s = now_s();
        while (now_s() - s < 0.2) g_ctrs[0]++;
        g_stop2 = 1;
        threadJoin(t2, U64_MAX);
        threadFree(t2);
        P("counters %s: core0 %.1f M/s, core2 %.1f M/s\n", far ? "on separate lines" : "on one line",
          g_ctrs[0] / 0.2e6, g_ctrs[other] / 0.2e6);
    }
}

// ---- C1: WFE / SEV from user mode -----------------------------------------------------------------------------
static volatile int g_wfeReady, g_wfeQuit;
static volatile u64 g_sevTick;

static void sev_thread(void* arg)
{
    (void)arg;
    while (!g_wfeQuit) {
        if (!g_wfeReady) continue;
        g_wfeReady = 0;
        u64 s = svcGetSystemTick();
        while (svcGetSystemTick() - s < 2000) {}    // ~7 us: let the other core enter WFE
        g_sevTick = svcGetSystemTick();
        __asm__ volatile("mcr p15, 0, %0, c7, c10, 4\n\tsev" : : "r"(0) : "memory");   // DSB (ARMv6 CP15 form), SEV
    }
}

static void test_wfe(void)
{
    P("-- C1 WFE/SEV wake-up from user mode --\n");
    step("wfe/sev");
    g_wfeQuit = 0; g_wfeReady = 0;
    Thread th = threadCreate(sev_thread, NULL, 8 * 1024, main_prio(), 2, false);
    if (!th) { P("C1 SKIPPED: thread\n"); return; }
    __asm__ volatile("sev\n\twfe" ::: "memory");   // clears a pending event (raises an exception if WFE is not allowed)
    const int n = 2000;
    u64 lat = 0, maxLat = 0;
    int early = 0;
    for (int i = 0; i < n; i++) {
        g_sevTick = 0;
        g_wfeReady = 1;
        __asm__ volatile("wfe" ::: "memory");
        u64 w = svcGetSystemTick();
        if (!g_sevTick || w < g_sevTick) { early++; while (!g_sevTick) {} continue; }
        u64 d = w - g_sevTick;
        lat += d;
        if (d > maxLat) maxLat = d;
    }
    g_wfeQuit = 1;
    threadJoin(th, U64_MAX);
    threadFree(th);
    int good = n - early;
    P("WFE works in user mode: %d wakes, %d early; mean %.2f us, max %.2f us (SEV->wake)\n", good, early,
      good ? (double)lat / good / (SYSCLOCK_ARM11 / 1e6) : 0.0, (double)maxLat / (SYSCLOCK_ARM11 / 1e6));
}

// ---- C2: core 1 above 80% -------------------------------------------------------------------------------------
static volatile u32 g_core1Count;
static volatile int g_core1Stop;

static void count_thread(void* arg)
{
    (void)arg;
    u32 c = 0;
    while (!g_core1Stop) c++;
    g_core1Count = c;
}

static double core_rate(int core, s32 prio)
{
    g_core1Stop = 0;
    g_core1Count = 0;
    Thread th = threadCreate(count_thread, NULL, 8 * 1024, prio, core, false);
    if (!th) return -1;
    svcSleepThread(400 * 1000 * 1000);
    g_core1Stop = 1;
    threadJoin(th, U64_MAX);
    threadFree(th);
    return g_core1Count / 0.4;
}

static void test_core1(void)
{
    P("-- C2 core 1 time limit --\n");
    step("core1 limit");
    double ref = core_rate(2, main_prio());
    static const int lim[3] = { 80, 85, 89 };
    for (int i = 0; i < 3; i++) {
        Result r = APT_SetAppCpuTimeLimit(lim[i]);
        double c1 = core_rate(1, main_prio());
        P("limit %d%%: %s, core 1 runs %.0f%% of core 2's rate\n", lim[i], R_SUCCEEDED(r) ? "accepted" : "REFUSED",
          ref > 0 && c1 > 0 ? 100.0 * c1 / ref : -1.0);
    }
}

// ---- A1/A2: GameCube DSP-ADPCM through ndsp ---------------------------------------------------------------------
#define ADPCM_N 7000                    // samples (500 frames of 14)
static const s16 kCoef[16] = { 0, 0, 2048, 0, 3840, -1792, 3072, -1024, 3584, -1536, 1024, 0, 4096, -2048, 2560, -512 };

static s16 clamp16(s32 v) { return v > 32767 ? 32767 : v < -32768 ? -32768 : (s16)v; }

// GameCube DSP-ADPCM: 8-byte frames, header byte (coefficient pair << 4 | scale exponent), 14 4-bit samples.
static void adpcm_encode(const s16* pcm, u32 n, u8* out, s16* ref)
{
    s32 h1 = 0, h2 = 0;
    for (u32 f = 0; f * 14 < n; f++) {
        s32 bestErr = 0x7FFFFFFF, bestP = 0, bestS = 0;
        for (int p = 0; p < 8; p++)
            for (int sc = 0; sc < 12; sc++) {
                s32 a = h1, b = h2, err = 0;
                for (int k = 0; k < 14; k++) {
                    u32 i = f * 14 + k;
                    s32 target = i < n ? pcm[i] : 0;
                    s32 pred = kCoef[2 * p] * a + kCoef[2 * p + 1] * b;
                    s32 nib = (s32)lroundf(((target << 11) - pred - 1024) / (float)((1 << sc) << 11));
                    if (nib > 7) nib = 7;
                    if (nib < -8) nib = -8;
                    s32 s = clamp16((((nib << sc) << 11) + 1024 + pred) >> 11);
                    s32 e = s - target;
                    err += e * e > 0x7FFFFF ? 0x7FFFFF : e * e;
                    b = a; a = s;
                }
                if (err < bestErr) { bestErr = err; bestP = p; bestS = sc; }
            }
        u8* fr = out + f * 8;
        fr[0] = (u8)(bestP << 4 | bestS);
        memset(fr + 1, 0, 7);
        for (int k = 0; k < 14; k++) {
            u32 i = f * 14 + k;
            s32 target = i < n ? pcm[i] : 0;
            s32 pred = kCoef[2 * bestP] * h1 + kCoef[2 * bestP + 1] * h2;
            s32 nib = (s32)lroundf(((target << 11) - pred - 1024) / (float)((1 << bestS) << 11));
            if (nib > 7) nib = 7;
            if (nib < -8) nib = -8;
            s16 s = clamp16((((nib << bestS) << 11) + 1024 + pred) >> 11);
            fr[1 + k / 2] |= (u8)((nib & 15) << (k & 1 ? 0 : 4));
            if (i < n) ref[i] = s;
            h2 = h1; h1 = s;
        }
    }
}

// Best match of ref (len n) inside the captured left channel: offset, correlation and max error at gain 1.
static void compare(const char* what, const s16* cap, u32 capN, const s16* ref, u32 n)
{
    double bestC = -2;
    u32 bestO = 0;
    for (u32 o = 0; o + n <= capN; o += 1) {
        double sxy = 0, sxx = 0, syy = 0;
        for (u32 i = 0; i < n; i += 7) { double x = cap[2 * (o + i)], y = ref[i]; sxy += x * y; sxx += x * x; syy += y * y; }
        double c = (sxx > 0 && syy > 0) ? sxy / sqrt(sxx * syy) : 0;
        if (c > bestC) { bestC = c; bestO = o; }
    }
    s32 maxErr = 0;
    double gain = 0, gx = 0, gy = 0;
    for (u32 i = 0; i < n && bestO + i < capN; i++) {
        s32 e = cap[2 * (bestO + i)] - ref[i];
        if (e < 0) e = -e;
        if (e > maxErr) maxErr = e;
        gx += (double)cap[2 * (bestO + i)] * ref[i];
        gy += (double)ref[i] * ref[i];
    }
    gain = gy > 0 ? gx / gy : 0;
    P("%-6s correlation %.5f at offset %lu, output/ref gain %.3f, max |diff| %ld\n", what, bestC,
      (unsigned long)bestO, gain, (long)maxErr);
}

static void play_and_capture(const char* what, ndspWaveBuf* wb, const s16* ref, u32 n, s16* cap, u32 capN)
{
    ndspWaveBuf cb;
    memset(&cb, 0, sizeof cb);
    memset(cap, 0, capN * 4);
    DSP_FlushDataCache(cap, capN * 4);
    cb.data_pcm16 = cap;
    cb.nsamples = capN;
    ndspSetCapture(&cb);
    svcSleepThread(30 * 1000 * 1000);
    ndspChnWaveBufAdd(0, wb);
    double t0 = now_s();
    while (wb->status != NDSP_WBUF_DONE && now_s() - t0 < 2.0) svcSleepThread(5 * 1000 * 1000);
    svcSleepThread(60 * 1000 * 1000);
    ndspSetCapture(NULL);
    DSP_InvalidateDataCache(cap, capN * 4);
    if (wb->status != NDSP_WBUF_DONE) { P("%s: buffer did not finish\n", what); return; }
    u32 nz = 0;
    for (u32 i = 0; i < capN * 2; i++) nz += cap[i] != 0;
    if (!nz) { P("%s: capture is all zero (capture not supported?)\n", what); return; }
    compare(what, cap, capN, ref, n);
}

static void test_audio(void)
{
    P("-- A1/A2 GameCube ADPCM through the 3DS DSP --\n");
    step("ndspInit");
    if (R_FAILED(ndspInit())) { P("A1 SKIPPED: ndspInit (sdmc:/3ds/dspfirm.cdc?)\n"); return; }
    const u32 capN = 32768;
    s16* pcm = linearAlloc(ADPCM_N * 2);
    s16* ref = malloc(ADPCM_N * 2);
    u8* adp = linearAlloc((ADPCM_N / 14 + 1) * 8);
    s16* cap = linearAlloc(capN * 4);
    if (!pcm || !ref || !adp || !cap) { P("A1 SKIPPED: alloc\n"); goto out; }
    for (u32 i = 0; i < ADPCM_N; i++) {
        float e = i < 400 ? i / 400.0f : i > ADPCM_N - 400 ? (ADPCM_N - i) / 400.0f : 1.0f;
        pcm[i] = (s16)(e * (6000.0f * sinf(i * 0.0469f) + 3000.0f * sinf(i * 0.1713f + 1.0f)));
    }
    step("adpcm encode");
    adpcm_encode(pcm, ADPCM_N, adp, ref);
    DSP_FlushDataCache(pcm, ADPCM_N * 2);
    DSP_FlushDataCache(adp, (ADPCM_N / 14 + 1) * 8);
    ndspSetOutputMode(NDSP_OUTPUT_STEREO);
    ndspSetMasterVol(1.0f);
    float mix[12] = { 0 };
    mix[0] = mix[1] = 1.0f;

    step("pcm16 baseline");
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_NONE);
    ndspChnSetRate(0, NDSP_SAMPLE_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_MONO_PCM16);
    ndspChnSetMix(0, mix);
    ndspWaveBuf wb;
    memset(&wb, 0, sizeof wb);
    wb.data_pcm16 = pcm;
    wb.nsamples = ADPCM_N;
    play_and_capture("PCM16", &wb, pcm, ADPCM_N, cap, capN);

    step("adpcm via ndsp");
    ndspChnWaveBufClear(0);
    ndspChnReset(0);
    ndspChnSetInterp(0, NDSP_INTERP_NONE);
    ndspChnSetRate(0, NDSP_SAMPLE_RATE);
    ndspChnSetFormat(0, NDSP_FORMAT_MONO_ADPCM);
    ndspChnSetAdpcmCoefs(0, (u16*)kCoef);
    ndspChnSetMix(0, mix);
    ndspAdpcmData ctx = { adp[0], 0, 0 };
    memset(&wb, 0, sizeof wb);
    wb.data_adpcm = adp;
    wb.nsamples = ADPCM_N;
    wb.adpcm_data = &ctx;
    play_and_capture("ADPCM", &wb, ref, ADPCM_N, cap, capN);
    P("  (ADPCM matching its own decode at correlation ~1 and small max diff = GameCube ADPCM plays natively)\n");

    // A2: CPU taken from core 0 by ndsp with 24 looping channels (volume 0).
    step("ndsp cpu cost");
    ndspChnWaveBufClear(0);
    double base = 0, busy = 0;
    for (int pass = 0; pass < 2; pass++) {
        static ndspWaveBuf lb[24];
        if (pass) {
            float z[12] = { 0 };
            for (int c = 0; c < 24; c++) {
                ndspChnReset(c);
                ndspChnSetInterp(c, NDSP_INTERP_POLYPHASE);
                ndspChnSetRate(c, 22050.0f + 1000.0f * c);
                ndspChnSetFormat(c, NDSP_FORMAT_MONO_ADPCM);
                ndspChnSetAdpcmCoefs(c, (u16*)kCoef);
                ndspChnSetMix(c, z);
                memset(&lb[c], 0, sizeof lb[c]);
                lb[c].data_adpcm = adp;
                lb[c].nsamples = ADPCM_N;
                lb[c].adpcm_data = &ctx;
                lb[c].looping = true;
                ndspChnWaveBufAdd(c, &lb[c]);
            }
            svcSleepThread(50 * 1000 * 1000);
        }
        u32 cnt = 0;
        double t0 = now_s();
        while (now_s() - t0 < 0.5) { for (volatile int k = 0; k < 1000; k++) {} cnt++; }
        if (pass) busy = cnt; else base = cnt;
        if (pass) for (int c = 0; c < 24; c++) { ndspChnWaveBufClear(c); ndspChnReset(c); }
    }
    P("core 0 left with 24 ADPCM channels playing: %.1f%% (ndsp's ARM-side cost)\n", base > 0 ? 100.0 * busy / base : -1.0);
    svcSleepThread(30 * 1000 * 1000);
out:
    ndspChnWaveBufClear(0);
    ndspChnReset(0);
    svcSleepThread(30 * 1000 * 1000);
    ndspExit();
    if (pcm) linearFree(pcm);
    free(ref);
    if (adp) linearFree(adp);
    if (cap) linearFree(cap);
}

// ==== gcbench 5: the measurements still open after gcbench 4/4b (MEASURE_PLAN.md) ====

// C3: a core 0 <-> core 2 handshake through the kernel (svcArbitrateAddress), check-then-wait with barriers, a 2 ms
// timeout per wait so a lost wake-up is counted, not hung on.
static volatile s32 g_arbReq __attribute__((aligned(32)));
static volatile s32 g_arbAck __attribute__((aligned(32)));
static volatile int g_arbQuit;
static u32 g_arbTimeouts;

static void arb_partner(void* arg)
{
    (void)arg;
    s32 seen = 0;
    while (!g_arbQuit) {
        __dmb();
        s32 r = g_arbReq;
        if (r == seen) {
            if (R_DESCRIPTION(syncArbitrateAddressWithTimeout((s32*)&g_arbReq, ARBITRATION_WAIT_IF_LESS_THAN_TIMEOUT,
                                                              seen + 1, 2000000)) == RD_TIMEOUT && !g_arbQuit)
                g_arbTimeouts++;
            continue;
        }
        seen = r;
        g_arbAck = r;
        __dmb();
        syncArbitrateAddress((s32*)&g_arbAck, ARBITRATION_SIGNAL, 1);
    }
}

static void test_arbitrate(void)
{
    P("-- C3 kernel wake-up round trip (svcArbitrateAddress) --\n");
    step("arbitrate");
    g_arbReq = 0; g_arbAck = 0; g_arbQuit = 0; g_arbTimeouts = 0;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    Thread th = threadCreate(arb_partner, NULL, 8 * 1024, prio, 2, false);
    if (!th) { P("C3 SKIPPED: thread\n"); return; }
    svcSleepThread(10 * 1000 * 1000);
    const int n = 2000;
    u32 mainTimeouts = 0;
    double t0 = now_s();
    for (int i = 1; i <= n; i++) {
        g_arbReq = i;
        __dmb();
        syncArbitrateAddress((s32*)&g_arbReq, ARBITRATION_SIGNAL, 1);
        while (g_arbAck != i) {
            if (R_DESCRIPTION(syncArbitrateAddressWithTimeout((s32*)&g_arbAck, ARBITRATION_WAIT_IF_LESS_THAN_TIMEOUT,
                                                              i, 2000000)) == RD_TIMEOUT) mainTimeouts++;
            __dmb();
        }
    }
    double t = now_s() - t0;
    g_arbQuit = 1;
    g_arbReq = n + 1;
    syncArbitrateAddress((s32*)&g_arbReq, ARBITRATION_SIGNAL, 1);
    threadJoin(th, 1000000000ULL);
    threadFree(th);
    P("arbitrate round trip: %.1f us mean; waits ended by the 2 ms timeout: partner %lu, main %lu (of %d)\n",
      t * 1e6 / n, (unsigned long)g_arbTimeouts, (unsigned long)mainTimeouts, n);
}

// C4: load-use latency (manual: LDR result latency 3). Each loop: 8 loads, each used at once vs 2 instructions later.
static u32 __attribute__((noinline)) loaduse_now(const u32* p, u32 n)
{
    u32 acc = 0;
    __asm__ volatile(
        "1:\n\t"
        "ldr r4, [%1]\n\tadd %0, %0, r4\n\tldr r4, [%1, #4]\n\tadd %0, %0, r4\n\t"
        "ldr r4, [%1, #8]\n\tadd %0, %0, r4\n\tldr r4, [%1, #12]\n\tadd %0, %0, r4\n\t"
        "ldr r4, [%1, #16]\n\tadd %0, %0, r4\n\tldr r4, [%1, #20]\n\tadd %0, %0, r4\n\t"
        "ldr r4, [%1, #24]\n\tadd %0, %0, r4\n\tldr r4, [%1, #28]\n\tadd %0, %0, r4\n\t"
        "subs %2, %2, #1\n\tbne 1b\n\t"
        : "+r"(acc), "+r"(p), "+r"(n) : : "r4", "cc", "memory");
    return acc;
}
static u32 __attribute__((noinline)) loaduse_later(const u32* p, u32 n)
{
    u32 acc = 0;
    __asm__ volatile(
        "1:\n\t"
        "ldr r4, [%1]\n\tldr r5, [%1, #4]\n\tldr r6, [%1, #8]\n\tadd %0, %0, r4\n\t"
        "ldr r4, [%1, #12]\n\tadd %0, %0, r5\n\tldr r5, [%1, #16]\n\tadd %0, %0, r6\n\t"
        "ldr r6, [%1, #20]\n\tadd %0, %0, r4\n\tldr r4, [%1, #24]\n\tadd %0, %0, r5\n\t"
        "ldr r5, [%1, #28]\n\tadd %0, %0, r6\n\tadd %0, %0, r4\n\tadd %0, %0, r5\n\t"
        "subs %2, %2, #1\n\tbne 1b\n\t"
        : "+r"(acc), "+r"(p), "+r"(n) : : "r4", "r5", "r6", "cc", "memory");
    return acc;
}

static void test_loaduse(void)
{
    P("-- C4 load-use scheduling (8 loads + 8 adds per loop, L1-resident) --\n");
    step("load-use");
    static u32 buf[8] __attribute__((aligned(32))) = { 1, 2, 3, 4, 5, 6, 7, 8 };
    const u32 n = 4000000;
    u64 t0 = svcGetSystemTick();
    g_sink2 += loaduse_now(buf, n);
    u64 t1 = svcGetSystemTick();
    g_sink2 += loaduse_later(buf, n);
    u64 t2 = svcGetSystemTick();
    double c1 = (double)(t1 - t0) * 3.0 / n, c2 = (double)(t2 - t1) * 3.0 / n;  // 268 MHz ticks -> 804 MHz cycles
    P("used at once: %.1f cycles/loop; used 2 later: %.1f cycles/loop (%.1f cycles per load saved)\n", c1, c2, (c1 - c2) / 8);
}

// M5: PLD prefetch on an indexed gather over 16 MB (the shape of a vertex-array gather).
static void test_prefetch(void)
{
    P("-- M5 PLD on a random gather over 16 MB --\n");
    step("prefetch");
    const u32 words = 4u << 20, n = 1u << 20;
    u32* big = malloc(words * 4);
    u32* idx = malloc(n * 4);
    if (!big || !idx) { P("M5 SKIPPED: malloc\n"); free(big); free(idx); return; }
    for (u32 i = 0; i < words; i += 1024) big[i] = i;
    u32 x = 12345;
    for (u32 i = 0; i < n; i++) { x = x * 1103515245u + 12345u; idx[i] = (x >> 4) % words; }
    for (int d = 0; d < 4; d++) {
        static const int dist[4] = { 0, 4, 8, 16 };
        u32 s = 0;
        double t0 = now_s();
        if (dist[d] == 0) for (u32 i = 0; i < n; i++) s += big[idx[i]];
        else for (u32 i = 0; i < n; i++) {
            if (i + dist[d] < n) __builtin_prefetch(&big[idx[i + dist[d]]]);
            s += big[idx[i]];
        }
        double t = now_s() - t0;
        g_sink2 += s;
        P("prefetch %2d ahead: %.1f ns per gathered word\n", dist[d], t * 1e9 / n);
    }
    free(big);
    free(idx);
}

// A3/A4: ndsp per-channel rate range (does a buffer at rate r play in nsamples / r seconds?) and the frame period.
static volatile u32 g_frames;
static void frame_cb(void* d) { (void)d; g_frames++; }

static void test_ndsp_more(void)
{
    P("-- A3/A4 ndsp rates and frame period --\n");
    step("ndsp rates");
    if (R_FAILED(ndspInit())) { P("A3 SKIPPED: ndspInit\n"); return; }
    const u32 n = 16000;
    s16* pcm = linearAlloc(n * 2);
    if (pcm) {
        for (u32 i = 0; i < n; i++) pcm[i] = (s16)(i * 37);
        DSP_FlushDataCache(pcm, n * 2);
        float zero[12] = { 0 };
        static const float rates[6] = { 8000, 22050, 32000, 48000, 96000, 192000 };
        for (int r = 0; r < 6; r++) {
            ndspChnReset(0);
            ndspChnSetInterp(0, NDSP_INTERP_LINEAR);
            ndspChnSetRate(0, rates[r]);
            ndspChnSetFormat(0, NDSP_FORMAT_MONO_PCM16);
            ndspChnSetMix(0, zero);
            ndspWaveBuf wb;
            memset(&wb, 0, sizeof wb);
            wb.data_pcm16 = pcm;
            wb.nsamples = n;
            double t0 = now_s();
            ndspChnWaveBufAdd(0, &wb);
            while (wb.status != NDSP_WBUF_DONE && now_s() - t0 < 4.0) svcSleepThread(1000000);
            double t = now_s() - t0;
            P("rate %6.0f Hz: %u samples took %.3f s -> effective %.0f Hz%s\n", rates[r], (unsigned)n, t, n / t,
              wb.status == NDSP_WBUF_DONE ? "" : " (not done)");
            ndspChnWaveBufClear(0);
        }
        ndspChnReset(0);
    }
    step("ndsp frame period");
    g_frames = 0;
    ndspSetCallback(frame_cb, NULL);
    svcSleepThread(1000 * 1000 * 1000);
    u32 f = g_frames;
    ndspSetCallback(NULL, NULL);
    P("ndsp sound-frame callbacks: %lu per second (%.2f ms per frame)\n", (unsigned long)f, f ? 1000.0 / f : 0.0);
    svcSleepThread(20 * 1000 * 1000);
    ndspExit();
    if (pcm) linearFree(pcm);
}

// A5: CPU cost of a WSOLA time-stretch (audio at normal pitch while the emulator runs slow): 32728 Hz stereo,
// 20 ms windows, 10 ms hop, +/-5 ms search, for stretch factors 1.25, 2 and 4.
static void test_stretch(void)
{
    P("-- A5 WSOLA time-stretch cost (core 0) --\n");
    step("stretch");
    const int rate = 32728, win = rate / 50, hop = win / 2, search = rate / 200;
    const int inN = rate * 2;   // 2 s of input
    s16* in = malloc((inN + win + 2 * search) * 2 * sizeof(s16));
    s32* out = malloc((inN * 4 + win) * 2 * sizeof(s32));
    if (!in || !out) { P("A5 SKIPPED: malloc\n"); free(in); free(out); return; }
    for (int i = 0; i < inN + win + 2 * search; i++) {
        s16 v = (s16)(4000 * sinf(i * 0.05f) + 2000 * sinf(i * 0.013f));
        in[2 * i] = v; in[2 * i + 1] = v;
    }
    static const float factors[3] = { 1.25f, 2.0f, 4.0f };
    for (int fi = 0; fi < 3; fi++) {
        float f = factors[fi];
        int outN = (int)(inN * f);
        memset(out, 0, (outN + win) * 2 * sizeof(s32));
        double t0 = now_s();
        int prevEnd = 0;
        for (int o = 0; o + win < outN; o += hop) {
            int nominal = (int)(o / f);
            int best = nominal;
            if (o > 0) {
                s64 bestC = INT64_MIN;
                for (int d = -search; d <= search; d += 4) {   // coarse search, every 4th lag, on the left channel
                    int c0 = nominal + d;
                    if (c0 < 0) continue;
                    s64 c = 0;
                    for (int k = 0; k < hop; k += 8) c += (s32)in[2 * (c0 + k)] * in[2 * (prevEnd + k)];
                    if (c > bestC) { bestC = c; best = c0; }
                }
            }
            for (int k = 0; k < win; k++) {   // triangular window, overlap-add
                s32 w = k < hop ? k : win - k;
                out[2 * (o + k)] += in[2 * (best + k)] * w / hop;
                out[2 * (o + k) + 1] += in[2 * (best + k) + 1] * w / hop;
            }
            prevEnd = best + hop;
        }
        double t = now_s() - t0;
        double outSecs = (double)outN / rate;
        P("stretch x%.2f: %.2f ms CPU per second of output (%.2f%% of a core)\n", f, t * 1000 / outSecs,
          100.0 * t / outSecs);
    }
    free(in);
    free(out);
}

void bench_wave5(void)
{
    P("== gcbench 5: CPU, memory and audio (open items) ==\n");
    test_arbitrate();
    test_loaduse();
    test_prefetch();
    test_ndsp_more();
    test_stretch();
}

void bench_wave(void)
{
    P("== WAVE 2: memory, cores, audio ==\n");
    test_bandwidth();
    test_flush();
    if (g_quick) P("quick mode: cross-core tests skipped (an emulator does not run cores 1-3 in parallel)\n");
    else {
        test_coreshare();
        test_core1();
    }
    test_audio();
    if (!g_quick) test_wfe();   // last: if WFE is not allowed in user mode it raises an exception and ends this module
}
