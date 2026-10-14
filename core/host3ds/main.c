// gc3ds for the New 3DS: program picker, emulation loop, video output to the top screen, pad input, stats.
// Programs are listed from romfs:/dols (bundled test programs) and sdmc:/gc3ds (the user's own files).
#include <3ds.h>
#include <citro3d.h>
#define GC3DS_HOST_TYPES
#include <dirent.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "../src/cpu/aot.h"
#include "../src/cpu/hle.h"
#include "../src/boot/boot.h"
#include "../src/disc/disc.h"
#include "../src/gx/gx.h"
#include "../src/gx/pica/pica.h"
#include "../src/gx/vfast.h"
#include "../src/jit/jit.h"
#include "gxthread.h"
#include "pmu.h"
#include "jitsample.h"
#include "crashlog.h"

// Main-thread stack: translated code (aot.h) nests C calls for guest calls, up to AOT_DEPTH deep.
u32 __stacksize__ = 1024 * 1024;
#include "asyncfile.h"
#include "relay.h"

#define MAX_FILES 64
#define CYCLES_PER_FIELD (GEKKO_HZ / 60)

static System g_sys;
static char g_files[MAX_FILES][320];
static int g_nFiles;
static u32 g_fields;
static Jit* g_jit;
static bool g_useJit = true;
static bool g_pica;  // GX draws on the PICA200 (false: software renderer)
static char g_clockLine[96];  // measured CPU clock at startup (menu)
static char g_coreLine[160];  // which CPU cores our threads can use, and how fast (startup probe)
static u32 g_presents;        // fields shown on the top screen
static bool g_gxThread;       // the GX thread exists (gxthread_start succeeded)

static void on_icbi(void* ctx, u32 pa) { jit_invalidate(ctx, pa); }

void fault_probe(FILE* f, const u8* mem1);  // faultprobe.c: --probe-faults
extern const char* g_mem1Guard;             // jit_3ds.c: whether MEM1's guard regions are inaccessible
void hwprobe_run(System* s, Jit* j);  // hwprobe.c: hardware self-test to sdmc:/gc3ds/hw.txt (j may be NULL)

static void scan_dir(const char* dir) {
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) && g_nFiles < MAX_FILES) {
        size_t n = strlen(e->d_name);
        const char* ext = n > 4 ? e->d_name + n - 4 : "";
        if (!strcasecmp(ext, ".dol") || !strcasecmp(ext, ".iso") || !strcasecmp(ext, ".gcm"))
            snprintf(g_files[g_nFiles++], sizeof g_files[0], "%s/%s", dir, e->d_name);
    }
    closedir(d);
}

static u8* load_file(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8* buf = malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

static inline u8 clamp8(int v) { return (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }

static volatile bool g_frameReady;

// VI field callback: only notes that a new field is out. Presenting happens in the main loop, at most once per
// 3DS refresh (presenting every emulated field flooded the GPU service when emulation ran ahead of real time).
static void on_field(System* s, void* user) {
    (void)s; (void)user;
    g_fields++;
    g_frameReady = true;
}

// Shows the field the VI scans out. With the GPU backend the image never left the GPU (virtual XFB); otherwise, and
// for programs that draw the XFB with the CPU, the XFB in RAM (YUYV, 640 wide) is scaled to the 400x240 top screen.
// The top framebuffer is stored rotated: column-major, 240 pixels per column, BGR8.
static void present_field(System* s, const GXPresent* p);

// The VI state is read here on the CPU thread; drawing happens where GX runs (the GX thread when it is on).
static void present(System* s) {
    u64 t0 = prof_begin();
    g_where = WH_PRESENT;
    g_presents++;
    g_frameReady = false;
    GXPresent p = { 0, 0, 0, 0, 0, false };
    p.valid = vi_get_xfb(s, &p.top, &p.bottom, &p.w, &p.h, &p.stride) && p.w;
    if (gxthread_enabled()) gxthread_present(&p);
    else present_field(s, &p);
    prof_end(PROF_PRESENT, t0);
    g_where = WH_FRONT;
}

static u64 prof_clock(void) { return svcGetSystemTick(); }

// CPU clock estimate: a loop of 17 dependent single-cycle instructions plus a taken branch (about 18 cycles per
// iteration on the ARM11), timed with the system tick. Tells 268 MHz from 804 MHz apart, not more.
// The bottom screen's once-a-second numbers are drawn by a thread on core 1: the console draws every glyph itself, and
// the ~13 lines a second took 2.6% of core 0 (2DS, 9b96050: perf_log in "# core0 outside compiled code"). Core 0 only
// formats them; when the thread is still drawing the previous second's, this second's are dropped.
static char g_conBuf[2][2048];
static u32 g_conLen, g_conFill;
static volatile int g_conReady = -1;  // the buffer the thread is to draw, or -1
static Thread g_conThread;

static void con_printf(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(g_conBuf[g_conFill] + g_conLen, sizeof g_conBuf[0] - g_conLen, fmt, ap);
    va_end(ap);
    if (n > 0) g_conLen = g_conLen + (u32)n < sizeof g_conBuf[0] ? g_conLen + (u32)n : (u32)sizeof g_conBuf[0] - 1;
}

static void con_main(void* arg) {
    (void)arg;
    for (;;) {
        int r = g_conReady;
        if (r >= 0) {
            __asm__ volatile("mcr p15, 0, %0, c7, c10, 5" : : "r"(0) : "memory");
            fputs(g_conBuf[r], stdout);
            fflush(stdout);
            __asm__ volatile("mcr p15, 0, %0, c7, c10, 5" : : "r"(0) : "memory");
            g_conReady = -1;
        }
        svcSleepThread(20000000LL);
    }
}

static void con_flush(void) {
    if (!g_conThread) {  // no thread: draw here
        s32 prio = 0x30;
        svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
        g_conThread = threadCreate(con_main, NULL, 8 * 1024, prio + 3 > 0x3F ? 0x3F : prio + 3, 1, true);
        if (!g_conThread) { fputs(g_conBuf[g_conFill], stdout); g_conLen = 0; return; }
    }
    if (g_conReady < 0) {
        __asm__ volatile("mcr p15, 0, %0, c7, c10, 5" : : "r"(0) : "memory");
        g_conReady = (int)g_conFill;
        g_conFill ^= 1;
    }
    g_conLen = 0;
    g_conBuf[g_conFill][0] = 0;
}

static double cpu_mhz(void) {
    u32 n = 100000;  // ~2 ms at 804 MHz (it runs every 10 s on core 0)
    u32 x = 0;
    u64 t0 = svcGetSystemTick();
    __asm__ volatile(
        "1:\n"
        "add %1, %1, #1\n add %1, %1, #1\n add %1, %1, #1\n add %1, %1, #1\n"
        "add %1, %1, #1\n add %1, %1, #1\n add %1, %1, #1\n add %1, %1, #1\n"
        "add %1, %1, #1\n add %1, %1, #1\n add %1, %1, #1\n add %1, %1, #1\n"
        "add %1, %1, #1\n add %1, %1, #1\n add %1, %1, #1\n add %1, %1, #1\n"
        "subs %0, %0, #1\n bne 1b\n"
        : "+r"(n), "+r"(x) : : "cc");
    double secs = (double)(svcGetSystemTick() - t0) / SYSCLOCK_ARM11;
    return 1.8e6 / secs / 1e6;  // 18 cycles per round
}

// Which CPU cores can run our threads, and how much work each gets done in 50 ms (a counting loop) compared with
// the main thread's core 0: the New 3DS has four cores, core 1 needs APT_SetAppCpuTimeLimit and core 3 belongs to
// the system. Decides whether vertex or GPU work can move to another core.
static volatile u32 g_coreCount[4];
static void core_spin(void* arg) {
    u32 c = (u32)arg;
    u64 end = svcGetSystemTick() + SYSCLOCK_ARM11 / 20;
    while (svcGetSystemTick() < end) g_coreCount[c]++;
}

static void probe_cores(void) {
    APT_SetAppCpuTimeLimit(80);
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    core_spin((void*)0);
    int n = snprintf(g_coreLine, sizeof g_coreLine, "cores (work in 50 ms vs core 0):");
    for (u32 c = 1; c < 4; c++) {
        Thread t = threadCreate(core_spin, (void*)c, 4096, prio, (int)c, false);
        if (!t) { n += snprintf(g_coreLine + n, sizeof g_coreLine - n, " c%lu none", (unsigned long)c); continue; }
        bool done = R_SUCCEEDED(threadJoin(t, 500000000ULL));
        n += snprintf(g_coreLine + n, sizeof g_coreLine - n, " c%lu %.2f%s", (unsigned long)c,
                      g_coreCount[0] ? (double)g_coreCount[c] / g_coreCount[0] : 0.0, done ? "" : " (stuck)");
        if (done) threadFree(t);
    }
}

static void present_field(System* s, const GXPresent* p) {
    u32 top = p->top, w = p->w, h = p->h, stride = p->stride;
    if (g_pica && pica_present(s, top)) return;
    if (!p->valid) return;
    u16 fbW, fbH;
    u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fbW, &fbH);
    u32 lines = h / 2 < 240 ? h / 2 : 240;
    (void)fbW; (void)fbH;
    for (u32 y = 0; y < lines; y++) {
        u32 line = top + y * stride;
        if (line + w * 2 > PA_MEM1_END) break;
        const u8* src = s->mem1 + line;
        for (u32 x = 0; x < 400; x++) {
            u32 sx = x * w / 400;
            const u8* p = src + (sx & ~1u) * 2;
            int yy = (p[(sx & 1) ? 2 : 0] - 16) * 298, u = p[1] - 128, v = p[3] - 128;
            u8* o = fb + (x * 240 + (239 - y)) * 3;
            o[0] = clamp8((yy + 516 * u + 128) >> 8);
            o[1] = clamp8((yy - 100 * u - 208 * v + 128) >> 8);
            o[2] = clamp8((yy + 409 * v + 128) >> 8);
        }
    }
    gfxFlushBuffers();
    gfxSwapBuffers();
}

// 3DS controls -> GameCube pad: A/B/X/Y as labelled, L/R -> L/R, ZR -> Z, Start, D-pad, Circle Pad -> main stick,
// C-Stick -> C stick.
static u16 scripted_buttons(void);  // --press (after RunOpts)

static void update_input(System* s) {
    hidScanInput();
    u32 k = hidKeysHeld();
    u16 b = 0;
    if (k & KEY_DLEFT) b |= 0x0001;
    if (k & KEY_DRIGHT) b |= 0x0002;
    if (k & KEY_DDOWN) b |= 0x0004;
    if (k & KEY_DUP) b |= 0x0008;
    if (k & KEY_ZR) b |= 0x0010;
    if (k & KEY_R) b |= 0x0020;
    if (k & KEY_L) b |= 0x0040;
    if (k & KEY_A) b |= 0x0100;
    if (k & KEY_B) b |= 0x0200;
    if (k & KEY_X) b |= 0x0400;
    if (k & KEY_Y) b |= 0x0800;
    if (k & KEY_START) b |= 0x1000;
    b |= scripted_buttons();
    s->si.buttons[0] = b;
    circlePosition cp, cs;
    hidCircleRead(&cp);
    hidCstickRead(&cs);
    u8* st = s->si.stick[0];
    st[0] = clamp8(128 + cp.dx * 100 / 156);
    st[1] = clamp8(128 + cp.dy * 100 / 156);
    st[2] = clamp8(128 + cs.dx * 100 / 156);
    st[3] = clamp8(128 + cs.dy * 100 / 156);
    st[4] = (k & KEY_L) ? 0xFF : 0;
    st[5] = (k & KEY_R) ? 0xFF : 0;
}

static int pick(void) {
    int sel = 0;
    for (;;) {
        consoleClear();
        printf("gc3ds - A run, Y JIT, B GX thread, X shader lighting, START quit\n");
        printf("GPU: %s, GX %s, lighting %s\n", g_pica ? "PICA200" : "software", gxthread_enabled() ? "on core 2" : "inline",
               g_pica && pica_shader_light() ? "shader" : "CPU");
        printf("CPU: %s\n", !g_jit ? "interpreter (JIT unavailable)" : g_useJit ? "JIT" : "interpreter");
        printf("%s\n%s\n", g_clockLine, g_coreLine);
        printf("%s\n", relay_configured() ? "L: remote runs (jobs from the PC through the relay)\n" : "");
        if (!g_nFiles) printf("No programs in romfs:/dols, sdmc:/gc3ds or sdmc:/gc3ds/games\n");
        int first = sel > 20 ? sel - 20 : 0;
        for (int i = first; i < g_nFiles && i < first + 24; i++) {
            const char* name = strrchr(g_files[i], '/');
            printf("%c %.36s\n", i == sel ? '>' : ' ', name ? name + 1 : g_files[i]);
        }
        for (;;) {
            if (!aptMainLoop()) return -1;
            hidScanInput();
            u32 d = hidKeysDown();
            if (d & KEY_START) return -1;
            if ((d & KEY_A) && g_nFiles) return sel;
            if ((d & KEY_L) && relay_configured()) return -2;
            if ((d & KEY_Y) && g_jit) { g_useJit = !g_useJit; break; }
            if ((d & KEY_B) && g_gxThread) { gxthread_enable(&g_sys, !gxthread_enabled()); break; }
            if ((d & KEY_X) && g_pica) { pica_set_shader_light(!pica_shader_light()); break; }
            if ((d & KEY_DDOWN) && sel + 1 < g_nFiles) { sel++; break; }
            if ((d & KEY_DUP) && sel > 0) { sel--; break; }
            gspWaitForVBlank();
        }
    }
}

static Disc g_disc;

// ---- performance log: sdmc:/gc3ds/perf.csv ----
// One row per second with everything the bottom-screen profile shows, so a run can be read from the card instead of
// photographed. Comment lines (#) mark each run, the measured CPU clock every 10 s, and the most frequent
// interpreted instructions of the last 10 s.
static FILE* g_perf;
#define AB_SECS 20  // A/B period: JIT traces on and off (each switch recompiles all code)
// The native shadow-scan stand-in (hle_tp.c) alternates on and off in the A/B. Its first version froze Twilight Princess at
// the first Link and Epona close-up (2DS, 9d7c239: the hook read a stale PC); the fix was proven under qemu with the JIT:
// 1,235 calls to field 1100, all identical to the interpreter. Off here until the budget fix (e7e7cbe) has had the same
// check; --ab hle puts it back in the A/B.
// The A/Bs and start values are run options (RunOpts below): menu runs use the defaults.

// One run's settings: the defaults for menu runs, or from 3dslink arguments / a relay job (relay.c), e.g.
//   --game Twilight --state sdmc:/gc3ds/states/tp850.state --seconds 300 --ab traces
typedef struct {
    char game[96];       // --game: the first program whose path contains this (case-insensitive)
    char state[160];     // --state PATH: load this save state right after boot (a state from the qemu ARM build works)
    u32 saveAt;          // --save-state FIELD:PATH: save the machine when the VI reaches FIELD
    char savePath[160];
    u32 seconds;         // --seconds N: end the run after N real seconds
    bool abTraces, abHle, abMerge, abVwmem, abSplit, abPld;  // --ab traces|hle|merge|vwmem|splitopen|pld|none: alternates every AB_SECS
    bool traces, hle;    // --traces on|off, --hle on|off: start values (an A/B flips them)
    bool merge;          // --merge on|off: draw merging on the GX thread (pica.c; on by default)
    u32 vwmemKb;         // --vwmem KB: the vertex worker's job ring (pica_set_job_ring; --ab vwmem alternates 256 and 1024)
    bool traceFollow;    // --trace-follow on|off: traces follow unconditional b
    u32 traceSide;       // --trace-side on|off|MASK: side exits (TRACE_SIDE_*: 1 bc on CR, 2 bc with CTR, 4 blr/bctr, 8 fused flags)
    bool link;           // --link on|off: block linking (off: every exit returns to the dispatcher)
    bool gxInline;       // --gx inline|thread: GX on the CPU thread instead of core 2
    bool slowInline;     // --slow inline|stub: memory slow paths per site (old) or through the shared stubs
    bool pmuSplit;       // --pmu-split on: performance counters split between compiled code and the rest (# pmu split)
    bool profile;        // --profile on: blocks store their start for the sampler (# hot guest code); one store per block
    struct { u32 from, to; u16 bits; } press[16];  // --press A@200-206,START@900-905: buttons held over field ranges
    u32 nPress;          // (fields counted from the run's start, as gcemu's --press counts from boot)
    u32 traceLen;        // --trace-len N: block length limit (0: the JIT's default)
    bool unattended;     // arguments or relay: no "press A" at the end
    bool probeFaults;    // --probe-faults on: the fault-handling probe (faultprobe.c) at the run's start
    bool faultMem;       // --faultmem on: the JIT's fault mode (unchecked MEM1 loads and stores, jit_set_fault_mem)
    bool abFault;        // --ab faultmem: fault mode on and off (faultmem column; each switch drops all compiled code)
    bool sched;          // --sched on|off (on): code scheduled against interlocks (jit_set_sched SCHED_ALL)
    bool abSched;        // --ab sched: on and off (sched column; each switch drops all compiled code)
    bool opt;            // --opt on|off (on): the optimizing JIT's stages (jit_set_opt OPT_ALL, docs/OPT_JIT_PLAN.md)
    bool abOpt;          // --ab opt: on and off (opt and deopts columns; each switch drops all compiled code)
    bool abVtrim;        // --ab vtrim: the vertex worker's job trim on and off (vtrim column)
    bool dlcache;        // --dlcache on|off (off): the display-list vertex cache (vfast.c, docs/DL_CACHE_PLAN.md)
    bool abDlcache;      // --ab dlcache: the cache on and off (dlcache column; starts off)
    bool vw2;            // --vw2 on|off (off): a second vertex worker on core 1 (pica_set_worker2)
    bool abVw2;          // --ab vw2: on and off (vw2 column; starts off)
    bool fskip;          // --fskip on|off (off): every other rendered frame draws nothing (pica_set_fskip)
    bool kstore;         // --kstore on|off (on): stfs / psq_st of known exact singles skip the round trip (OPT_KSTORE)
    bool abKstore;       // --ab kstore: OPT_KSTORE on and off (opt column; each switch drops all compiled code)
    bool fcache;         // --fcache on|off (off): float lanes in VFP registers within blocks (OPT_FCACHE)
    bool abFcache;       // --ab fcache: OPT_FCACHE on and off (opt column; each switch drops all compiled code)
    bool abFskip;        // --ab fskip: on and off (fskip column; starts off)
    bool aot;            // --aot on|off (off): ahead-of-time translated game code where the build has it (aot.h, C12)
    bool abAot;          // --ab aot: translated code on and off (aot column; starts off; each switch drops all compiled code)
    int aotCall, aotAbi, aotPipe;  // --aotcall N, --aotabi 0|1, --aotpipe 0|1 (-1: default); live: set aotcall / aotabi / aotpipe
} RunOpts;
static RunOpts g_opt;

static u16 scripted_buttons(void) {
    u16 b = 0;
    for (u32 i = 0; i < g_opt.nPress; i++)
        if (g_fields >= g_opt.press[i].from && g_fields <= g_opt.press[i].to) b |= g_opt.press[i].bits;
    return b;
}

static void opts_default(RunOpts* o) {
    memset(o, 0, sizeof *o);
    // Traces off by default: on the 2DS their tail duplication overflows the 16 MB code buffer (HANDOFF, 2026-10-12).
    o->traceFollow = true;
    o->traceSide = TRACE_SIDE_ALL;
    o->link = true;
    o->merge = true;
    o->sched = true;   // relay 91-92: on vs off within noise (cutscene -0.2, title +1.1 points), fewer instructions and words
    o->opt = true;     // check hoisting: core 0 +5.7% (job 97), +5.0% cutscene and +4.7% core-0-bound section (job 117)
    o->kstore = true;  // known-single stores: core 0 +1% (job 129, within noise), exact (fuzzer opt 3)
    o->aotCall = o->aotAbi = o->aotPipe = -1;  // aot switches at their defaults (aot.h)
    o->vwmemKb = 128;  // 1 MB 18.9%, 256 KB 21.2%, 128 KB 21.4%, 96 KB 21.5% (cutscene, relay 76-80)
}

static bool opts_parse(RunOpts* o, int argc, char** argv, char* err, size_t n) {
    for (int i = 0; i < argc; i++) {
        const char* a = argv[i];
        const char* v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strncmp(a, "--", 2) && !v) { snprintf(err, n, "%s needs a value", a); return false; }
        if (!strcmp(a, "--game")) snprintf(o->game, sizeof o->game, "%s", v);
        else if (!strcmp(a, "--state")) snprintf(o->state, sizeof o->state, "%s", v);
        else if (!strcmp(a, "--save-state")) {
            const char* c = strchr(v, ':');
            if (!c) { snprintf(err, n, "--save-state FIELD:PATH"); return false; }
            o->saveAt = (u32)strtoul(v, NULL, 10);
            snprintf(o->savePath, sizeof o->savePath, "%s", c + 1);
        } else if (!strcmp(a, "--seconds")) o->seconds = (u32)strtoul(v, NULL, 10);
        else if (!strcmp(a, "--ab")) { o->abTraces = !strcmp(v, "traces"); o->abHle = !strcmp(v, "hle"); o->abMerge = !strcmp(v, "merge"); o->abVwmem = !strcmp(v, "vwmem"); o->abSplit = !strcmp(v, "splitopen"); o->abPld = !strcmp(v, "pld"); o->abFault = !strcmp(v, "faultmem"); o->abSched = !strcmp(v, "sched"); o->abOpt = !strcmp(v, "opt"); o->abVtrim = !strcmp(v, "vtrim"); o->abDlcache = !strcmp(v, "dlcache"); o->abVw2 = !strcmp(v, "vw2"); o->abFskip = !strcmp(v, "fskip"); o->abKstore = !strcmp(v, "kstore"); o->abFcache = !strcmp(v, "fcache"); o->abAot = !strcmp(v, "aot"); }
        else if (!strcmp(a, "--vwmem")) o->vwmemKb = (u32)strtoul(v, NULL, 10);
        else if (!strcmp(a, "--merge")) o->merge = !strcmp(v, "on");
        else if (!strcmp(a, "--traces")) o->traces = !strcmp(v, "on");
        else if (!strcmp(a, "--hle")) o->hle = !strcmp(v, "on");
        else if (!strcmp(a, "--trace-follow")) o->traceFollow = !strcmp(v, "on");
        else if (!strcmp(a, "--trace-side")) o->traceSide = !strcmp(v, "on") ? TRACE_SIDE_ALL : !strcmp(v, "off") ? 0 : (u32)strtoul(v, NULL, 0);
        else if (!strcmp(a, "--link")) o->link = !strcmp(v, "on");
        else if (!strcmp(a, "--gx")) o->gxInline = !strcmp(v, "inline");
        else if (!strcmp(a, "--slow")) o->slowInline = !strcmp(v, "inline");
        else if (!strcmp(a, "--pmu-split")) o->pmuSplit = !strcmp(v, "on");
        else if (!strcmp(a, "--profile")) o->profile = !strcmp(v, "on");
        else if (!strcmp(a, "--probe-faults")) o->probeFaults = !strcmp(v, "on");
        else if (!strcmp(a, "--faultmem")) o->faultMem = !strcmp(v, "on");
        else if (!strcmp(a, "--sched")) o->sched = !strcmp(v, "on");
        else if (!strcmp(a, "--opt")) o->opt = !strcmp(v, "on");
        else if (!strcmp(a, "--dlcache")) o->dlcache = !strcmp(v, "on");
        else if (!strcmp(a, "--vw2")) o->vw2 = !strcmp(v, "on");
        else if (!strcmp(a, "--fskip")) o->fskip = !strcmp(v, "on");
        else if (!strcmp(a, "--kstore")) o->kstore = !strcmp(v, "on");
        else if (!strcmp(a, "--fcache")) o->fcache = !strcmp(v, "on");
        else if (!strcmp(a, "--aot")) o->aot = !strcmp(v, "on");
        else if (!strcmp(a, "--aotcall")) o->aotCall = atoi(v);
        else if (!strcmp(a, "--aotabi")) o->aotAbi = atoi(v);
        else if (!strcmp(a, "--aotpipe")) o->aotPipe = atoi(v);
        else if (!strcmp(a, "--press")) {
            static const struct { const char* name; u16 bit; } kButtons[] = {
                { "LEFT", 0x0001 }, { "RIGHT", 0x0002 }, { "DOWN", 0x0004 }, { "UP", 0x0008 }, { "Z", 0x0010 }, { "R", 0x0020 },
                { "L", 0x0040 }, { "A", 0x0100 }, { "B", 0x0200 }, { "X", 0x0400 }, { "Y", 0x0800 }, { "START", 0x1000 },
            };
            for (const char* q = v; *q && o->nPress < sizeof o->press / sizeof o->press[0];) {
                char name[16];
                unsigned from, to;
                int k = 0;
                u16 bit = 0;
                if (sscanf(q, "%15[A-Za-z]@%u-%u%n", name, &from, &to, &k) != 3) { snprintf(err, n, "--press BUTTON@from-to,..."); return false; }
                for (size_t b = 0; b < sizeof kButtons / sizeof kButtons[0]; b++) if (!strcmp(name, kButtons[b].name)) bit = kButtons[b].bit;
                if (!bit) { snprintf(err, n, "--press: unknown button %s", name); return false; }
                o->press[o->nPress].from = from; o->press[o->nPress].to = to; o->press[o->nPress].bits = bit;
                o->nPress++;
                q += k;
                if (*q == ',') q++;
            }
        }
        else if (!strcmp(a, "--trace-len")) o->traceLen = (u32)strtoul(v, NULL, 10);
        else { snprintf(err, n, "unknown option %s", a); return false; }
        i++;
    }
    return true;
}
static bool g_pipeFast = true;  // compiled code appends pipe stores itself (jit_set_gather_buffer)
static bool g_abGpuPos = true;    // the gpupos A/B runs (test builds fix the transform instead)
static bool g_gpuPosFixed = true; // the transform a run starts with
static const char* const kProfNames[PROF_N] = { "gx", "disc", "log", "jitc", "present", "gxtex", "gxvtx", "gxsync",
                                                "vdec", "vlight", "cpu", "fbfp", "fb", "gxthread", "gxwait", "vwork",
                                                "vwait", "gxpres", "gxstate", "gxend", "vprep", "vpal", "vqueue", "valloc" };
_Static_assert(PROF_N == 24, "kProfNames must list every PROF_ area");
static JitOpCounts g_fbPrev;  // fallbackRun at the last dump

static void perf_open(const char* path) {
    mkdir("sdmc:/gc3ds", 0777);
    g_perf = asyncfile_open("sdmc:/gc3ds/perf.csv", "a");  // written to the card by a thread on core 1
    if (!g_perf) return;
    g_perf = relay_tee(g_perf);  // a relay job also streams the log (relay.c)
    pmu_init();
    fprintf(g_perf, "# run %s | %s | jit %s | gx %s | lighting %s | pmu %s | GPU queue at %lx | A/B period %d s, 2 phases (traces: blocks follow branches) | hot guest code sampled\n# %s\n",
            strrchr(path, '/') + 1, g_clockLine, g_useJit ? "on" : "off", gxthread_enabled() ? "core 2" : "inline",
            g_pica && pica_shader_light() ? "shader" : "cpu", pmu_status(), g_pica ? (unsigned long)pica_queue_offset() : 0UL, AB_SECS, g_coreLine);
    fprintf(g_perf, "real_s,emu_s,speed_pct,fields_s,shown_s,dropped_s,field_ms_avg,field_ms_max");
    for (int k = 0; k < PROF_N; k++) fprintf(g_perf, ",%s_pct", kProfNames[k]);
    fprintf(g_perf, ",wait_pct,rest_pct,draws_s,verts_s,ins_k,fb_k,disp_k,slowld_k,slowst_k,native_pct"
                    ",compiles_s,polls_s,jitcode_pct,icbi_flushes,gxbytes_k,discbytes_k"
                    ",gpu_calls_s,gpu_submits_s,gpu_splits_s,texdec_s,texdec_kb,efbcopy_s,xfbcopy_s,clears_s"
                    ",c3d_cpu_ms,c3d_gpu_ms,heap_free_mb,linear_free_mb,vram_free_kb,jit_link,links_s,vworker,earlyq,gpupos,esplit,indexed,memstub,pipefast,hle,traces,merge,vwmem,splitopen,pld,faultmem,faults,sched,opt,deopts,vtrim,dlcache,vw2,fskip,aot");
    // ARM11 counters for core 0 (pmu.c): cycles every second, two events in rotation, SCU traffic.
    fprintf(g_perf, "%s\n", pmu_columns());
    fflush(g_perf);
    if (g_jit) g_fbPrev = jit_stats(g_jit)->fallbackRun;
}

// Screenshots of the top screen at key points of a run (5 s, 15 s, 30 s, then every 30 s), in a ring of 16 files
// sdmc:/gc3ds/shots/shot_NN.bmp (about 290 KB each); perf.csv names each one with its time.
static int g_shotNext = -1;

static void perf_shot(double realS, double emuS) {
    if (!g_pica) return;
    mkdir("sdmc:/gc3ds/shots", 0777);
    if (g_shotNext < 0) {
        g_shotNext = 0;
        FILE* f = fopen("sdmc:/gc3ds/shots/next.txt", "r");
        if (f) { if (fscanf(f, "%d", &g_shotNext) != 1) g_shotNext = 0; fclose(f); }
    }
    char path[64];
    snprintf(path, sizeof path, "sdmc:/gc3ds/shots/shot_%02d.bmp", g_shotNext % 16);
    pica_capture_done = (void (*)(const void*, u32))relay_frame;  // a relay job also gets the frame (relay.sh frames)
    pica_capture(path);
    if (g_perf) fprintf(g_perf, "# shot %s at real %.1f s, emulated %.2f s\n", path + 5, realS, emuS);
    g_shotNext = (g_shotNext + 1) % 16;
    FILE* f = fopen("sdmc:/gc3ds/shots/next.txt", "w");
    if (f) { fprintf(f, "%d\n", g_shotNext); fclose(f); }
}

// A live command from the PC (relay.sh cmd JOB "..."), applied between perf rows; perf.csv records each one, and the
// option columns show what was in effect on every row. Returns false for "stop" (the run ends).
//   shot                      a frame now (uploaded to the job)
//   set opt|sched N           JIT optimizations / scheduling masks
//   set merge|vtrim|pld|splitopen 0|1, set vwmem KB
//   tev [N]                   describe the next N draws' TEV, textures and combiner result (default 6)
//   stop                      end the run
static bool g_stopReq;
static bool g_relayRun;  // a relay job is running
static u32 g_dlOnSecs;   // seconds with the display-list cache on since the last # dlcache report (A/B)
static void apply_cmd(const char* cmd, double realS, double emuS) {
    char name[24] = "", val[24] = "";
    bool ok = true;
    if (!strcmp(cmd, "shot")) perf_shot(realS, emuS);
    else if (!strcmp(cmd, "stop")) g_stopReq = true;
    else if (!strncmp(cmd, "tev", 3) && g_pica) pica_describe(cmd[3] ? (u32)strtoul(cmd + 3, NULL, 10) : 6);
    else if (sscanf(cmd, "set %23s %23s", name, val) == 2) {
        u32 v = (u32)strtoul(val, NULL, 0);
        if (!strcmp(name, "opt") && g_jit) jit_set_opt(g_jit, v);
        else if (!strcmp(name, "sched") && g_jit) jit_set_sched(g_jit, v);
        else if (!strcmp(name, "merge") && g_pica) pica_set_merge(v != 0);
        else if (!strcmp(name, "vtrim")) pica_set_vw_trim(v != 0);
        else if (!strcmp(name, "dlcache")) g_vcacheOn = v != 0;
        else if (!strcmp(name, "vw2") && g_pica) pica_set_worker2(v != 0);
        else if (!strcmp(name, "fskip") && g_pica) pica_set_fskip(v != 0);
        else if (!strcmp(name, "aot") && g_jit) { aot_set_enabled(v != 0); jit_invalidate_all(g_jit); }
        else if (!strcmp(name, "aotcall") || !strcmp(name, "aotabi") || !strcmp(name, "aotpipe")) {
            u32 d; bool abi, pipe;
            aot_get_switches(&d, &abi, &pipe);
            if (!strcmp(name, "aotcall")) d = v;
            else if (!strcmp(name, "aotabi")) abi = v != 0;
            else pipe = v != 0;
            aot_set_switches(d, abi, pipe);
        }
        else if (!strcmp(name, "pld")) vfast_set_prefetch(v != 0);
        else if (!strcmp(name, "splitopen") && g_pica) pica_set_split_open(v != 0);
        else if (!strcmp(name, "vwmem") && g_pica) pica_set_job_ring(v << 10);
        else ok = false;
    } else ok = false;
    if (g_perf) fprintf(g_perf, "# cmd \"%s\" at real %.1f s: %s\n", cmd, realS, ok ? "applied" : "unknown");
}

// The 8 most frequent interpreted instructions since the last call.
static void perf_fallbacks(void) {
    if (!g_perf || !g_jit) return;
    static JitOpCounts d;
    const JitOpCounts* c = &jit_stats(g_jit)->fallbackRun;
    const u32* cur = (const u32*)c;
    u32* prev = (u32*)&g_fbPrev;
    u32* dd = (u32*)&d;
    u32 n = sizeof d / 4;
    for (u32 i = 0; i < n; i++) { dd[i] = cur[i] - prev[i]; prev[i] = cur[i]; }
    fprintf(g_perf, "# fallbacks 10s:");
    // Index i of the flattened table: primary[64], x31[1024], x19[1024], x63[1024], x59[32], x4[1024].
    for (int k = 0; k < 8; k++) {
        u32 best = 0;
        for (u32 i = 1; i < n; i++) if (dd[i] > dd[best]) best = i;
        if (!dd[best]) break;
        u32 op, xo;
        if (best < 64) { op = best; xo = 0; }
        else if (best < 64 + 1024) { op = 31; xo = best - 64; }
        else if (best < 64 + 2048) { op = 19; xo = best - 64 - 1024; }
        else if (best < 64 + 3072) { op = 63; xo = best - 64 - 2048; }
        else if (best < 64 + 3072 + 32) { op = 59; xo = best - 64 - 3072; }
        else { op = 4; xo = best - 64 - 3072 - 32; }
        char dis[64];
        gekko_disasm(0, (op << 26) | (xo << 1), dis, sizeof dis);
        char* sp = strchr(dis, ' ');
        if (sp) *sp = 0;
        // Primary-opcode totals double-count the grouped ones; list groups by their extended opcode only.
        if (best < 64 && (op == 31 || op == 19 || op == 63 || op == 59 || op == 4)) { dd[best] = 0; k--; continue; }
        fprintf(g_perf, " %s=%lu", dis, (unsigned long)dd[best]);
        dd[best] = 0;
    }
    fprintf(g_perf, "\n");
    // The interpreted mfspr / mtspr by SPR number (the ones worth compiling next).
    static u32 sprPrev[2][1024];
    const JitStats* js = jit_stats(g_jit);
    fprintf(g_perf, "# spr fallbacks 10s:");
    for (int k = 0; k < 6; k++) {
        u32 bm = 0, bs = 0, bn = 0;
        for (u32 m = 0; m < 2; m++)
            for (u32 s = 0; s < 1024; s++) {
                u32 n = js->sprRun[m][s] - sprPrev[m][s];
                if (n > bn) { bn = n; bm = m; bs = s; }
            }
        if (!bn) break;
        fprintf(g_perf, " %s %lu=%lu", bm ? "mtspr" : "mfspr", (unsigned long)bs, (unsigned long)bn);
        sprPrev[bm][bs] += bn;
    }
    fprintf(g_perf, "\n");
    memcpy(sprPrev, js->sprRun, sizeof sprPrev);
}

static bool is_disc(const char* path) {
    size_t n = strlen(path);
    return n > 4 && (!strcasecmp(path + n - 4, ".iso") || !strcasecmp(path + n - 4, ".gcm"));
}

static char g_runErr[400];  // why the last run failed (relay jobs report it)

// Runs one program with g_opt; returns "done" (time limit or SELECT), "stopped" (the machine stopped) or "failed".
static const char* run(const char* path) {
    size_t size = 0;
    u8* dol = NULL;
    char err[256];
    g_runErr[0] = 0;
    consoleClear();
    if (g_disc.file) disc_close(&g_disc);
    if (is_disc(path)) {
        printf("opening disc...\n");
        if (!disc_open(&g_disc, path, err, sizeof err)) { snprintf(g_runErr, sizeof g_runErr, "disc: %s", err); printf("%s\n", g_runErr); return "failed"; }
    } else {
        dol = load_file(path, &size);
        if (!dol) { snprintf(g_runErr, sizeof g_runErr, "cannot read %s", path); printf("%s\n", g_runErr); return "failed"; }
    }
    gxthread_drain();  // the GX thread must be idle before the GPU backend and the machine are reset
    if (g_pica) pica_reset();
    if (g_jit) {
        jit_invalidate_all(g_jit);
        if (g_useJit) jit_attach(g_jit); else gekko_set_jit(&g_sys.cpu, NULL, NULL);
    }
    bool booted = dol ? boot_dol(&g_sys, dol, size, err, sizeof err) : boot_disc(&g_sys, &g_disc, err, sizeof err);
    free(dol);
    if (!booted) { snprintf(g_runErr, sizeof g_runErr, "boot failed: %s", err); printf("%s\n", g_runErr); return "failed"; }
    if (g_opt.state[0]) {
        // A save state replaces the booted machine (its disc stays the one just opened); compiled code is dropped.
        gxthread_drain();
        if (!sys_load_state(&g_sys, g_opt.state)) {
            snprintf(g_runErr, sizeof g_runErr, "cannot load state %s (missing, or written by a build with other structures)", g_opt.state);
            printf("%s\n", g_runErr);
            return "failed";
        }
        if (g_jit) jit_invalidate_all(g_jit);
        printf("loaded state %s at field %lu\n", g_opt.state, (unsigned long)g_sys.vi.field);
    }
    printf("running %s\nSELECT: back to menu\n\n", strrchr(path, '/') + 1);
    perf_open(path);
    crashlog_report(g_perf);
    if (g_perf) {  // malloc's headroom: the largest block it still gives (osGetMemRegionFree reads 0 once libctru took the heap)
        u32 lo = 0, hi = 64u << 20;
        while (hi - lo > (64u << 10)) { u32 mid = lo + (hi - lo) / 2; void* p = malloc(mid); if (p) { free(p); lo = mid; } else hi = mid; }
        fprintf(g_perf, "# heap: largest malloc block %.1f MB\n", lo / 1048576.0);
    }
    if (g_perf && g_opt.state[0]) fprintf(g_perf, "# loaded state %s at field %lu\n", g_opt.state, (unsigned long)g_sys.vi.field);
    if (g_opt.probeFaults) fault_probe(g_perf, g_sys.mem1);
    const char* result = "done";
    bool saved = false;
    u32 perfRows = 0;
    g_fields = 0;
    u64 t0 = svcGetSystemTick(), tLast = t0, cycLast = g_sys.cpu.cycles, cyc0 = g_sys.cpu.cycles, tPresent = t0;
    u64 tPace = t0, cycPace = cyc0;  // origin of the real-time pacing (moves when emulation falls behind)
    u32 fieldsLast = 0;
    u64 waitTicks = 0, drawsLast = g_sys.gx.core.draws, vertsLast = g_sys.gx.core.vertices;
    u64 jLast[5] = { 0 };  // instructions run, interpreted, blocks run, slow loads, slow stores (JIT, per second)
    // Per second: host time per emulated field (excluding the wait for the 3DS refresh), fields shown, and the other
    // counters' previous values.
    u64 fieldMax = 0, fieldSum = 0;
    u32 fieldN = 0, presentsLast = g_presents;
    u64 xLast[4] = { 0, 0, g_sys.gx.bytesReceived, g_sys.di.bytesRead };  // compiles, polls, GX bytes, disc bytes
    if (g_jit) { xLast[0] = jit_stats(g_jit)->blocksCompiled; xLast[1] = jit_stats(g_jit)->pollSkips; }
    g_vcacheOn = g_opt.dlcache;  // --ab dlcache starts off
    if (g_pica) pica_set_worker2(g_opt.vw2);  // --ab vw2 starts off
    if (g_pica) pica_set_fskip(g_opt.fskip);  // --ab fskip starts off
    PicaStats pLast = g_picaStats;
    u64 linksLast = g_jit ? jit_stats(g_jit)->links : 0;
    if (g_jit) {
        jit_set_linking(g_jit, g_opt.link);
        jit_set_slow_inline(g_jit, g_opt.slowInline);
        jit_set_profile(g_jit, g_opt.profile);
        jit_run_begin_hook = g_opt.pmuSplit ? pmu_split_begin : NULL;
        jit_run_end_hook = g_opt.pmuSplit ? pmu_split_end : NULL;
        if (g_gxThread) gxthread_enable(&g_sys, !g_opt.gxInline);
        jit_set_mem_stubs(g_jit, true);
        jit_set_fault_mem(g_jit, g_opt.faultMem || g_opt.abFault);
        jit_set_sched(g_jit, g_opt.sched || g_opt.abSched ? SCHED_ALL : 0);  // on unless --sched off (relay 91-92)
        jit_set_opt(g_jit, (g_opt.opt && !g_opt.abOpt ? OPT_HOIST : 0) | (g_opt.kstore && !g_opt.abKstore ? OPT_KSTORE : 0) |
                           (g_opt.fcache && !g_opt.abFcache ? OPT_FCACHE : 0));  // A/Bs start off
        jit_set_mem_stamps(g_jit, &g_memStamps);  // dcbf / dcbst / dcbz stamp their page for the display-list cache
        aot_set_enabled(g_opt.aot && !g_opt.abAot);  // --ab aot starts off
        aot_set_switches(g_opt.aotCall >= 0 ? (u32)g_opt.aotCall : 40u, g_opt.aotAbi != 0, g_opt.aotPipe != 0);
        if (g_perf) fprintf(g_perf, "# aot: translated code %s in this build, %s\n", aot_available() ? "present" : "absent",
                            aot_enabled() ? "on" : g_opt.abAot ? "A/B" : "off");
        if (g_perf) fprintf(g_perf, "# fault mode %s (A/B %s); MEM1 at %08lx, guard regions: %s\n", jit_fault_mem(g_jit) ? "on" : "off",
                            g_opt.abFault ? "on" : "off", (unsigned long)(u32)g_sys.mem1, g_mem1Guard);
        g_pipeFast = true;
        jit_set_gather_buffer(g_jit, g_sys.gx.wgp, &g_sys.gx.wgpLen);
        hle_set_enabled(g_opt.hle || g_opt.abHle);
        jit_set_traces(g_jit, g_opt.traces);
        jit_set_trace_parts(g_jit, g_opt.traceFollow, g_opt.traceSide, g_opt.traceLen);
        jit_invalidate_all(g_jit);
    }
    if (g_pica) {
        pica_set_vertex_worker(true);
        pica_set_early_queue(true);
        pica_set_gpu_transform(g_gpuPosFixed);
        pica_set_early_split(true);
        pica_set_indexed(true);
        pica_set_merge(g_opt.merge);
        pica_set_job_ring(g_opt.vwmemKb << 10);
    }
    if (g_jit && g_useJit) jitsample_start(&g_sys.cpu);
    g_profClock = prof_clock;  // X toggles the profile lines
    memset(g_hostProf, 0, sizeof g_hostProf);
    while (aptMainLoop()) {
        g_where = WH_FRONT;
        update_input(&g_sys);
        if (hidKeysDown() & KEY_SELECT) break;
        if (g_opt.seconds && svcGetSystemTick() - t0 >= (u64)g_opt.seconds * SYSCLOCK_ARM11) break;
        if (g_stopReq) { g_stopReq = false; break; }  // a live "stop" (apply_cmd)
        if (hidKeysDown() & KEY_X) { g_profClock = g_profClock ? NULL : prof_clock; printf("\x1b[9;0H\x1b[0J"); }
        u64 it0 = svcGetSystemTick(), waitBefore = waitTicks;
        sys_run(&g_sys, CYCLES_PER_FIELD);
        if (g_sys.stopRequested) {
            printf("\nstopped at pc=%08x\n", (unsigned)g_sys.cpu.pc);
            if (g_perf) fprintf(g_perf, "# machine stopped at pc=%08x\n", (unsigned)g_sys.cpu.pc);
            result = "stopped";
            break;
        }
        if (g_opt.saveAt && !saved && g_sys.vi.field >= g_opt.saveAt) {
            gxthread_drain();  // GX state must be complete before it is written
            bool ok = sys_save_state(&g_sys, g_opt.savePath);
            saved = true;
            printf("save state %s at field %lu: %s\n", g_opt.savePath, (unsigned long)g_sys.vi.field, ok ? "ok" : "FAILED");
            if (g_perf) fprintf(g_perf, "# save state %s at field %lu: %s\n", g_opt.savePath, (unsigned long)g_sys.vi.field, ok ? "ok" : "FAILED");
        }
        // Pace to real time: when emulation is ahead, present and wait for the next 3DS refresh; when behind,
        // present at most once per refresh and keep going.
        u64 now = svcGetSystemTick();
        double emulated = (double)(g_sys.cpu.cycles - cycPace) / GEKKO_HZ, real = (double)(now - tPace) / SYSCLOCK_ARM11;
        // Time lost while emulation was slower than real time is not made up afterwards (the logos ran at 144% after
        // a slow load): the pacing origin follows, keeping at most 50 ms of lag.
        if (real - emulated > 0.05) {
            tPace += (u64)((real - emulated - 0.05) * SYSCLOCK_ARM11);
            real = emulated + 0.05;
        }
        if (emulated > real) {
            if (g_frameReady) present(&g_sys);
            u64 w0 = svcGetSystemTick();
            g_where = WH_WAIT;
            gspWaitForVBlank();
            g_where = WH_FRONT;
            waitTicks += svcGetSystemTick() - w0;
            tPresent = svcGetSystemTick();
        } else if (g_frameReady && now - tPresent > SYSCLOCK_ARM11 / 60) {
            present(&g_sys);
            tPresent = now;
        }
        now = svcGetSystemTick();
        u64 fieldTicks = now - it0 - (waitTicks - waitBefore);
        fieldSum += fieldTicks;
        fieldN++;
        if (fieldTicks > fieldMax) fieldMax = fieldTicks;
        if (now - tLast > SYSCLOCK_ARM11) {  // once a second
            g_where = WH_PERF;
            double secs = (double)(now - tLast) / SYSCLOCK_ARM11;
            double mips = (double)(g_sys.cpu.cycles - cycLast) / secs / 1e6;
            double fps = (g_fields - fieldsLast) / secs;
            con_printf("\x1b[5;0H%6.2f MIPS  %5.2f fields/s  (%4.1f%% speed)   \n", mips, fps, 100.0 * mips / 486.0);
            con_printf("emulated %.1f s, real %.1f s     \n", (double)g_sys.cpu.cycles / GEKKO_HZ, (double)(now - t0) / SYSCLOCK_ARM11);
            if (g_jit && g_useJit) {
                const JitStats* js = jit_stats(g_jit);
                con_printf("JIT %llu blocks, %.1f%% native   \n", (unsigned long long)js->blocksCompiled,
                       js->instrsCompiled ? 100.0 * (double)js->instrsNative / (double)js->instrsCompiled : 0.0);
            }
            // Share of real time per area; the rest is guest code, interpreter fallbacks and emulated hardware.
            double tot = (double)(now - tLast), p[PROF_N], rest = 100.0 - 100.0 * (double)waitTicks / tot;
            // The CPU thread's own areas; with the GX thread on, GX runs on core 2 and the CPU thread's GX cost is the
            // time it waits for it.
            bool async = gxthread_enabled();
            for (int k = 0; k < PROF_N; k++) {
                p[k] = 100.0 * (double)g_hostProf[k] / tot;
                if (k <= PROF_PRESENT && !(async && k == PROF_GX)) rest -= p[k];  // others are nested or views
            }
            if (async) rest -= p[PROF_GXWAIT];
            // Thousands per second: guest instructions run by compiled blocks, interpreted ones among them, blocks
            // entered, and loads / stores through the C helpers (MMIO, write-gather pipe).
            double k[5] = { 0, 0, 0, 0, 0 }, native = 0;
            if (g_jit && g_useJit) {
                const JitStats* js = jit_stats(g_jit);
                u64 now5[5] = { js->instrsRun, js->fallbacksRun, js->blocksRun, js->slowLoads, js->slowStores };
                for (int i = 0; i < 5; i++) k[i] = (double)(now5[i] - jLast[i]) / secs / 1000.0;
                memcpy(jLast, now5, sizeof jLast);
                native = js->instrsCompiled ? 100.0 * (double)js->instrsNative / (double)js->instrsCompiled : 0.0;
            }
            double draws = (double)(g_sys.gx.core.draws - drawsLast) / secs, verts = (double)(g_sys.gx.core.vertices - vertsLast) / secs;
            if (g_perf) {
                g_where = WH_PERFCSV;
                u64 t0p = prof_begin();
                double shown = (double)(g_presents - presentsLast) / secs, tickMs = 1000.0 / SYSCLOCK_ARM11;
                fprintf(g_perf, "%.1f,%.2f,%.2f,%.2f,%.2f,%.2f,%.1f,%.1f", (double)(now - t0) / SYSCLOCK_ARM11,
                        (double)g_sys.cpu.cycles / GEKKO_HZ, 100.0 * mips / 486.0, fps, shown, fps > shown ? fps - shown : 0.0,
                        fieldN ? (double)fieldSum / fieldN * tickMs : 0.0, (double)fieldMax * tickMs);
                for (int i = 0; i < PROF_N; i++) fprintf(g_perf, ",%.1f", g_profClock ? p[i] : 0.0);
                fprintf(g_perf, ",%.1f,%.1f,%.0f,%.0f,%.0f,%.0f,%.0f,%.1f,%.1f,%.1f", 100.0 * (double)waitTicks / tot,
                        g_profClock ? rest : 0.0, draws, verts, k[0], k[1], k[2], k[3], k[4], native);
                u64 x[4] = { 0, 0, g_sys.gx.bytesReceived, g_sys.di.bytesRead };
                if (g_jit) { x[0] = jit_stats(g_jit)->blocksCompiled; x[1] = jit_stats(g_jit)->pollSkips; }
                const PicaStats* ps = &g_picaStats;
                fprintf(g_perf, ",%.0f,%.0f,%.1f,%llu,%.1f,%.1f", (x[0] - xLast[0]) / secs, (x[1] - xLast[1]) / secs,
                        g_jit ? 100.0 * jit_code_used(g_jit) : 0.0, g_jit ? (unsigned long long)jit_stats(g_jit)->icbiFlushes : 0ULL,
                        (x[2] - xLast[2]) / secs / 1024.0, (x[3] - xLast[3]) / secs / 1024.0);
                fprintf(g_perf, ",%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f", (ps->drawCalls - pLast.drawCalls) / secs,
                        (ps->submits - pLast.submits) / secs, (ps->splits - pLast.splits) / secs,
                        (ps->texDecodes - pLast.texDecodes) / secs, (ps->texDecodeBytes - pLast.texDecodeBytes) / secs / 1024.0,
                        (ps->efbCopies - pLast.efbCopies) / secs, (ps->xfbCopies - pLast.xfbCopies) / secs,
                        (ps->clears - pLast.clears) / secs);
                fprintf(g_perf, ",%.2f,%.2f,%.1f,%.1f,%.0f", g_pica ? C3D_GetProcessingTime() : 0.0f,
                        g_pica ? C3D_GetDrawingTime() : 0.0f, osGetMemRegionFree(MEMREGION_APPLICATION) / 1048576.0,
                        linearSpaceFree() / 1048576.0, vramSpaceFree() / 1024.0);
                u64 links = g_jit ? jit_stats(g_jit)->links : 0;
                fprintf(g_perf, ",%d,%.0f,%d", g_jit && jit_linking(g_jit) ? 1 : 0, (links - linksLast) / secs,
                        g_pica && pica_vertex_worker_wanted() ? 1 : 0);
                fprintf(g_perf, ",%d", g_pica && pica_early_queue() ? 1 : 0);
                fprintf(g_perf, ",%d", g_pica && pica_gpu_transform() ? 1 : 0);
                fprintf(g_perf, ",%d", g_pica && pica_early_split() ? 1 : 0);
                fprintf(g_perf, ",%d", g_pica && pica_indexed() ? 1 : 0);
                fprintf(g_perf, ",%d", g_jit && jit_mem_stubs(g_jit) ? 1 : 0);
                fprintf(g_perf, ",%d", g_pipeFast ? 1 : 0);
                fprintf(g_perf, ",%d", hle_enabled() ? 1 : 0);
                fprintf(g_perf, ",%d", g_jit && jit_traces(g_jit) ? 1 : 0);
                fprintf(g_perf, ",%d", g_pica && pica_merge() ? 1 : 0);
                fprintf(g_perf, ",%u", g_pica ? (unsigned)(pica_job_ring() >> 10) : 0u);
                fprintf(g_perf, ",%d", g_pica && pica_split_open() ? 1 : 0);
                fprintf(g_perf, ",%d", g_vfastPrefetch ? 1 : 0);
                fprintf(g_perf, ",%d,%llu", g_jit && jit_fault_mem(g_jit) ? 1 : 0, g_jit ? (unsigned long long)jit_stats(g_jit)->faults : 0ULL);
                fprintf(g_perf, ",%lu", g_jit ? (unsigned long)jit_sched(g_jit) : 0UL);
                fprintf(g_perf, ",%lu,%llu", g_jit ? (unsigned long)jit_opt(g_jit) : 0UL, g_jit ? (unsigned long long)jit_stats(g_jit)->deopts : 0ULL);
                fprintf(g_perf, ",%d", pica_vw_trim() ? 1 : 0);
                fprintf(g_perf, ",%d", g_vcacheOn ? 1 : 0);
                fprintf(g_perf, ",%d", g_pica && pica_worker2() ? 1 : 0);
                fprintf(g_perf, ",%d", g_pica && pica_fskip() ? 1 : 0);
                fprintf(g_perf, ",%d", aot_enabled() ? 1 : 0);
                if (g_vcacheOn) g_dlOnSecs++;
                linksLast = links;
                pmu_row(g_perf, secs);
                fputc('\n', g_perf);
                memcpy(xLast, x, sizeof xLast);
                pLast = *ps;
                ++perfRows;
                if (perfRows == 5 || perfRows == 15 || perfRows % 30 == 0)
                    perf_shot((double)(now - t0) / SYSCLOCK_ARM11, (double)g_sys.cpu.cycles / GEKKO_HZ);
                if (g_pica && pica_take_description()) {
                    if (g_perf) fputs(pica_take_description(), g_perf);
                    pica_description_taken();
                }
                for (char cmd[128]; relay_take_cmd(cmd, sizeof cmd);)
                    apply_cmd(cmd, (double)(now - t0) / SYSCLOCK_ARM11, (double)g_sys.cpu.cycles / GEKKO_HZ);
                // A/B in two phases of AB_SECS: the native stand-in for Twilight Princess's shadow scan (hle_tp.c) on
                // and off (hle column; each switch drops all compiled code, so read each phase after its first seconds).
                if (g_opt.abHle && g_jit && g_useJit && g_abGpuPos && perfRows % AB_SECS == 0) {
                    hle_set_enabled(!hle_enabled());
                    jit_invalidate_all(g_jit);
                }
                // A/B in two phases of AB_SECS: JIT traces (blocks follow branches, conditional ones become side exits) on
                // and off (traces column; each switch drops all compiled code, so read each phase after its first seconds).
                if (g_opt.abTraces && g_jit && g_useJit && g_abGpuPos && perfRows % AB_SECS == 0)
                    jit_set_traces(g_jit, !jit_traces(g_jit));
                // A/B in two phases of AB_SECS: draw merging on the GX thread (merge column; no recompile).
                if (g_opt.abMerge && g_pica && perfRows % AB_SECS == 0) pica_set_merge(!pica_merge());
                // A/B in two phases of AB_SECS: the vertex worker's job ring, 256 KB and 1 MB (vwmem column).
                if (g_opt.abSplit && g_pica && perfRows % AB_SECS == 0) pica_set_split_open(!pica_split_open());
                if (g_opt.abPld && perfRows % AB_SECS == 0) vfast_set_prefetch(!g_vfastPrefetch);  // worker reads it per job
                if (g_opt.abFault && g_jit && g_useJit && perfRows % AB_SECS == 0) jit_set_fault_mem(g_jit, !jit_fault_mem(g_jit));
                if (g_opt.abSched && g_jit && g_useJit && perfRows % AB_SECS == 0) jit_set_sched(g_jit, jit_sched(g_jit) ? 0 : SCHED_ALL);
                if (g_opt.abOpt && g_jit && g_useJit && perfRows % AB_SECS == 0) jit_set_opt(g_jit, jit_opt(g_jit) ^ OPT_HOIST);
                if (g_opt.abKstore && g_jit && g_useJit && perfRows % AB_SECS == 0) jit_set_opt(g_jit, jit_opt(g_jit) ^ OPT_KSTORE);
                if (g_opt.abFcache && g_jit && g_useJit && perfRows % AB_SECS == 0) jit_set_opt(g_jit, jit_opt(g_jit) ^ OPT_FCACHE);
                if (g_opt.abVtrim && perfRows % AB_SECS == 0) pica_set_vw_trim(!pica_vw_trim());
                if (g_opt.abDlcache && perfRows % AB_SECS == 0) g_vcacheOn = !g_vcacheOn;  // entries stay stamped while off
                if (g_opt.abVw2 && g_pica && perfRows % AB_SECS == 0) pica_set_worker2(!pica_worker2());
                if (g_opt.abFskip && g_pica && perfRows % AB_SECS == 0) pica_set_fskip(!pica_fskip());
                if (g_opt.abAot && g_jit && perfRows % AB_SECS == 0) { aot_set_enabled(!aot_enabled()); jit_invalidate_all(g_jit); }
                if (g_opt.abVwmem && g_pica && perfRows % AB_SECS == 0) pica_set_job_ring(pica_job_ring() > (256u << 10) ? 256u << 10 : 1024u << 10);
                if (perfRows % 10 == 0) {
                    g_where = WH_PERFREP;
                    perf_fallbacks();
                    {  // the GX thread's per-draw paths over the last 10 s (see g_picaDrawPaths, VFastStats)
                        static u64 lastP[8], lastSetups, lastDraws, lastWhy[6];
                        u64 p[8], why[6];
                        memcpy(p, g_picaDrawPaths, sizeof p);
                        memcpy(why, g_vfastStats.setupWhy, sizeof why);
                        u64 setups = g_vfastStats.setups, vdraws = g_vfastStats.draws;
                        double n = (double)(p[0] + p[1] + p[2] - lastP[0] - lastP[1] - lastP[2]);
                        if (n < 1) n = 1;
                        fprintf(g_perf, "# gx draws 10s: %.0f/s; batch path fast %.1f%%, key equal %.1f%%, new batch %.1f%% "
                                "(bp %llu tlut %llu lights %llu xfb %llu); vertex setups %llu of %llu draws "
                                "(cp %llu xf %llu lights %llu format %llu sel %llu reset %llu)\n", n / 10.0,
                                100.0 * (double)(p[0] - lastP[0]) / n, 100.0 * (double)(p[1] - lastP[1]) / n,
                                100.0 * (double)(p[2] - lastP[2]) / n, (unsigned long long)(p[3] - lastP[3]),
                                (unsigned long long)(p[4] - lastP[4]), (unsigned long long)(p[5] - lastP[5]),
                                (unsigned long long)(p[6] - lastP[6]), (unsigned long long)(setups - lastSetups),
                                (unsigned long long)(vdraws - lastDraws), (unsigned long long)(why[0] - lastWhy[0]),
                                (unsigned long long)(why[1] - lastWhy[1]), (unsigned long long)(why[2] - lastWhy[2]),
                                (unsigned long long)(why[3] - lastWhy[3]), (unsigned long long)(why[4] - lastWhy[4]),
                                (unsigned long long)(why[5] - lastWhy[5]));
                        static u64 lastHleRuns, lastHleInsns;
                        fprintf(g_perf, "# hle 10s: stand-in runs %llu, guest instructions stood in for %llu\n",
                                (unsigned long long)(g_hleRuns - lastHleRuns), (unsigned long long)(g_hleInsns - lastHleInsns));
                        lastHleRuns = g_hleRuns;
                        lastHleInsns = g_hleInsns;
                        static u64 lastTevC, lastTevK;
                        fprintf(g_perf, "# tev 10s: %.0f program changes/s, %.0f compiled/s (the rest from the combiner-plan cache); "
                                "%llu textures not decoded for want of memory so far\n",
                                (double)(g_picaStats.tevChanges - lastTevC) / 10.0, (double)(g_picaStats.tevCompiles - lastTevK) / 10.0,
                                (unsigned long long)g_picaStats.texFails);
                        lastTevC = g_picaStats.tevChanges;
                        lastTevK = g_picaStats.tevCompiles;
                        static u64 lastPal, lastSub;
                        fprintf(g_perf, "# gpu transform 10s: palette uploads %llu, draw calls for palette or projection "
                                "changes %llu, draws over the palette (CPU) %llu\n",
                                (unsigned long long)(g_picaStats.palUploads - lastPal),
                                (unsigned long long)(g_picaStats.subDraws - lastSub), (unsigned long long)(p[7] - lastP[7]));
                        static u64 lastM[7];
                        u64 m[7];
                        memcpy(m, g_picaMerge, sizeof m);
                        double md = (double)(m[0] - lastM[0]), mj = (double)(m[2] - lastM[2]);
                        fprintf(g_perf, "# merge 10s: %.0f draws/s, %.1f%% joined an open draw, %.0f jobs/s (%.2f draws each); open "
                                "draws issued for a state change %llu, a draw that could not join %llu, an empty ring %llu, "
                                "present %llu\n", md / 10.0, md > 0 ? 100.0 * (double)(m[1] - lastM[1]) / md : 0.0, mj / 10.0,
                                mj > 0 ? md / mj : 0.0, (unsigned long long)(m[3] - lastM[3]),
                                (unsigned long long)(m[4] - lastM[4]), (unsigned long long)(m[5] - lastM[5]),
                                (unsigned long long)(m[6] - lastM[6]));
                        memcpy(lastM, m, sizeof m);
                        {  // display lists (fifo.c dl_note): could their gathered vertices be cached across frames?
                            static u64 lastD[7];
                            u64 d[7];
                            memcpy(d, g_dlStats, sizeof d);
                            double calls = (double)(d[0] - lastD[0]), din = (double)(d[1] - lastD[1]), dout = (double)(d[2] - lastD[2]);
                            double smp = (double)(d[3] - lastD[3]);
                            fprintf(g_perf, "# dlist 10s: %.0f list calls/s, %.1f%% of draws inside lists, %.0f new list addresses/s; "
                                    "sampled calls with the same bytes %.1f%%, and the same array bases too %.1f%%\n", calls / 10.0,
                                    din + dout > 0 ? 100.0 * din / (din + dout) : 0.0, (double)(d[6] - lastD[6]) / 10.0,
                                    smp > 0 ? 100.0 * (double)(d[4] - lastD[4]) / smp : 0.0, smp > 0 ? 100.0 * (double)(d[5] - lastD[5]) / smp : 0.0);
                            memcpy(lastD, d, sizeof d);
                        }
                        {  // the vertex worker's job ring (pica.c g_vwStats)
                            static u64 lastV[5];
                            u64 v[5];
                            memcpy(v, g_vwStats, sizeof v);
                            double jobs = (double)(v[0] - lastV[0]), waits = (double)(v[3] - lastV[3]);
                            fprintf(g_perf, "# vworker 10s: %.0f jobs/s, %.1f KB reserved and %.1f KB kept per job; %.0f waits for "
                                    "ring space/s with %.1f jobs in flight\n", jobs / 10.0,
                                    jobs > 0 ? (double)(v[1] - lastV[1]) / jobs / 1024.0 : 0.0,
                                    jobs > 0 ? (double)(v[2] - lastV[2]) / jobs / 1024.0 : 0.0, waits / 10.0,
                                    waits > 0 ? (double)(v[4] - lastV[4]) / waits : 0.0);
                            memcpy(lastV, v, sizeof v);
                            {  // REDESIGN_CANDIDATES C5: vertices that could use the PICA's one index buffer
                                static u64 lastS[5];
                                u64 sv[5] = { g_vfastStats.idxDraws, g_vfastStats.idxVerts, g_vfastStats.sharedVerts,
                                              g_vfastStats.sharedDraws, g_vfastStats.oneIdxVerts };
                                double dv = (double)(sv[1] - lastS[1]), dd = (double)(sv[0] - lastS[0]);
                                fprintf(g_perf, "# shared index 10s (1 draw in 32): %.1f%% of indexed vertices give all indexed attributes "
                                        "one index; %.1f%% of indexed draws do on every vertex; %.1f%% of indexed vertices have a "
                                        "single indexed attribute\n", dv > 0 ? 100.0 * (double)(sv[2] - lastS[2]) / dv : 0.0,
                                        dd > 0 ? 100.0 * (double)(sv[3] - lastS[3]) / dd : 0.0,
                                        dv > 0 ? 100.0 * (double)(sv[4] - lastS[4]) / dv : 0.0);
                                memcpy(lastS, sv, sizeof sv);
                            }
                            static u64 lastFsD, lastFsF;
                            fprintf(g_perf, "# frame skip 10s: %.1f frames/s skipped, %.0f draws/s not made (%s)\n",
                                    (double)(g_fskipFrames - lastFsF) / 10.0, (double)(g_fskipDraws - lastFsD) / 10.0,
                                    g_pica && pica_fskip() ? "on" : "off");
                            lastFsD = g_fskipDraws; lastFsF = g_fskipFrames;
                            {  // C12: ahead-of-time translated code
                                static AotStats lastA;
                                static u64 lastCyc;
                                AotStats a = aot_stats;
                                double gi = (double)(g_sys.cpu.cycles - lastCyc);
                                fprintf(g_perf, "# aot 10s: %.1f%% of guest instructions translated, %.0f calls/s, %.0f runs/s, %.0f fallbacks/s, "
                                        "%.0f slow accesses/s, %llu hash failures (%s)\n", gi > 0 ? 100.0 * (double)(a.insns - lastA.insns) / gi : 0.0,
                                        (double)(a.calls - lastA.calls) / 10.0, (double)(a.runs - lastA.runs) / 10.0,
                                        (double)(a.fallbacks - lastA.fallbacks) / 10.0, (double)(a.slow - lastA.slow) / 10.0,
                                        (unsigned long long)a.hashFail, aot_enabled() ? "on" : "off");
                                lastA = a;
                                lastCyc = g_sys.cpu.cycles;
                            }
                            static u64 lastW2;
                            fprintf(g_perf, "# vworker 2 10s: %.0f jobs/s on core 1 (%s)\n", (double)(g_vw2Jobs - lastW2) / 10.0,
                                    g_pica && pica_worker2() ? "on" : "off");
                            lastW2 = g_vw2Jobs;
                            static u64 lastPh[4], lastVc[8];
                            u64 ph[4] = { g_vfPhase[0], g_vfPhase[1], g_vfPhase[2], g_vwPhaseAll };
                            u64 vcp[8];
                            memcpy(vcp, g_vcPhase, sizeof vcp);
                            double all = (double)(ph[3] - lastPh[3]);
                            if (all > 0) {
                                double a = (double)(ph[0] - lastPh[0]), b = (double)(ph[1] - lastPh[1]), c = (double)(ph[2] - lastPh[2]);
                                fprintf(g_perf, "# vworker phases 10s: gather and colours %.1f%%, positions and normals %.1f%%, "
                                        "texture coordinates %.1f%%, rest (copy, cull, triangles) %.1f%% of the worker's time\n",
                                        100 * a / all, 100 * b / all, 100 * c / all, 100 * (all - a - b - c) / all);
                                double h = (double)(vcp[0] - lastVc[0]), m = (double)(vcp[1] - lastVc[1]), i = (double)(vcp[2] - lastVc[2]);
                                if (h + m + i > 0)
                                    fprintf(g_perf, "# vworker cache phases 10s: hits (lookups, copies) %.1f%%, missed runs gathered %.1f%%, "
                                            "inserts %.1f%% of the worker's time\n", 100 * h / all, 100 * m / all, 100 * i / all);
                                double lk = (double)(vcp[3] - lastVc[3]), cp = (double)(vcp[4] - lastVc[4]);
                                double reads = (double)(vcp[5] - lastVc[5]), calls = (double)(vcp[7] - lastVc[7]);
                                double per = calls > 0 ? (double)(vcp[6] - lastVc[6]) / (64.0 * calls) : 0.0;
                                if (h > 0)
                                    fprintf(g_perf, "# vworker cache hit parts 10s: lookup passes %.1f%%, record copies %.1f%% of the worker's "
                                            "time; %.0f clock reads in timed sections at %.0f ticks each: %.1f%% of the worker's time\n",
                                            100 * lk / all, 100 * cp / all, reads, per, 100 * reads * per / all);
                            }
                            memcpy(lastPh, ph, sizeof ph);
                            memcpy(lastVc, vcp, sizeof vcp);
                        }
                        for (int w = 0; w < 2; w++) {  // the display-list vertex caches (vfast.c): GX thread, vertex worker
                            static VCacheStats lastC[2];
                            const VCacheStats* c = pica_vcache_stats(w);
                            if (!c) continue;
                            double look = (double)(c->hits - lastC[w].hits + c->misses - lastC[w].misses + c->stale - lastC[w].stale +
                                                   c->staleRing - lastC[w].staleRing);
                            fprintf(g_perf, "# dlcache 10s (%s): %.0f hits/s (%.0f vertices/s), %.0f misses/s, %.0f stale/s (page "
                                    "stamped), %.0f written over/s, %.1f%% of lookups hit; %.0f inserts/s, %.0f evictions/s, %.0f "
                                    "uncacheable segments/s; %.1f pages checked per lookup; cache on %u of the 10 s\n", w ? "worker" : "gx thread",
                                    (double)(c->hits - lastC[w].hits) / 10.0, (double)(c->hitVerts - lastC[w].hitVerts) / 10.0,
                                    (double)(c->misses - lastC[w].misses) / 10.0, (double)(c->stale - lastC[w].stale) / 10.0,
                                    (double)(c->staleRing - lastC[w].staleRing) / 10.0,
                                    look > 0 ? 100.0 * (double)(c->hits - lastC[w].hits) / look : 0.0,
                                    (double)(c->inserts - lastC[w].inserts) / 10.0, (double)(c->evicted - lastC[w].evicted) / 10.0,
                                    (double)(c->uncacheable - lastC[w].uncacheable) / 10.0,
                                    look > 0 ? (double)(c->pagesChecked - lastC[w].pagesChecked) / look : 0.0, (unsigned)g_dlOnSecs);
                            lastC[w] = *c;
                        }
                        g_dlOnSecs = 0;
                        lastPal = g_picaStats.palUploads;
                        lastSub = g_picaStats.subDraws;
                        memcpy(lastP, p, sizeof p);
                        memcpy(lastWhy, why, sizeof why);
                        lastSetups = setups;
                        lastDraws = vdraws;
                    }
                    if (g_jit && g_useJit) jitsample_report(g_perf, g_jit);
                    if (g_jit) {
                        const JitStats* js = jit_stats(g_jit);
                        fprintf(g_perf, "# jit code: %.2f host words per guest instruction, %.0f%% of the buffer; "
                                "fused compare-branches %llu, cached GPR reads %llu, pipe stores %llu, links %llu; "
                                "GPU queue at %lx\n",
                                jit_words_per_insn(g_jit), 100.0 * jit_code_used(g_jit),
                                (unsigned long long)js->fusedBranches, (unsigned long long)js->forwards,
                                (unsigned long long)js->gatherStores, (unsigned long long)js->links,
                                g_pica ? (unsigned long)pica_queue_offset() : 0UL);
                        {  // exceptions the guest took (gk_exception), by vector
                            static u32 lastExc[32];
                            fprintf(g_perf, "# exceptions 10s:");
                            for (int v = 0; v < 32; v++) {
                                if (g_gekkoExc[v] != lastExc[v]) fprintf(g_perf, " %x00=%lu", v, (unsigned long)(g_gekkoExc[v] - lastExc[v]));
                                lastExc[v] = g_gekkoExc[v];
                            }
                            fprintf(g_perf, "\n");
                        }
                        static u64 lastFlush, lastFollow, lastSide;
                        static u64 lastRep, lastComp;
                        fprintf(g_perf, "# jit repeats 10s: %llu compiles, %llu at a PC already compiled since the flush; keys (all time):",
                                (unsigned long long)(js->blocksCompiled - lastComp), (unsigned long long)(js->compileRepeats - lastRep));
                        for (int k = 0; k < 8 && js->keyCount[k]; k++) fprintf(g_perf, " %08lx=%lu", (unsigned long)js->keySeen[k], (unsigned long)js->keyCount[k]);
                        fprintf(g_perf, "\n");
                        lastRep = js->compileRepeats; lastComp = js->blocksCompiled;
                        fprintf(g_perf, "# jit cache 10s: %lu blocks, flushes %llu, compile fails %llu/%llu/%llu, traces "
                                "follow %d side %lu len %lu link %d: b followed %llu, side exits %llu\n", (unsigned long)jit_entries(g_jit),
                                (unsigned long long)(js->flushes - lastFlush), (unsigned long long)js->compileFails[0],
                                (unsigned long long)js->compileFails[1], (unsigned long long)js->compileFails[2],
                                g_opt.traceFollow, (unsigned long)g_opt.traceSide, (unsigned long)g_opt.traceLen, g_opt.link,
                                (unsigned long long)(js->traceFollowed - lastFollow), (unsigned long long)(js->traceSideExits - lastSide));
                        lastFlush = js->flushes; lastFollow = js->traceFollowed; lastSide = js->traceSideExits;
                        double kept = js->insnsKept ? (double)js->insnsKept : 1.0;
                        fprintf(g_perf, "# jit words per guest instruction (all blocks kept so far): hot %.1f, exit stubs %.1f, "
                                "cold %.1f\n", (double)js->wordsHot / kept, (double)js->wordsExit / kept,
                                (double)js->wordsCold / kept);
                        // Returns to the C dispatcher per second over the last 10 s, by reason, and emulation slices.
                        static u64 last[6];
                        u64 cur[6] = { js->dispLink, js->dispIndirect, js->dispBudget, js->dispOther, g_gekkoRuns, js->fastSwitches };
                        fprintf(g_perf, "# dispatches/s: link requests %.0f, indirect misses %.0f, budget ends %.0f, other "
                                "%.0f; gekko_run slices/s %.0f; indirect table resets/s %.0f\n", (cur[0] - last[0]) / 10.0, (cur[1] - last[1]) / 10.0,
                                (cur[2] - last[2]) / 10.0, (cur[3] - last[3]) / 10.0, (cur[4] - last[4]) / 10.0, (cur[5] - last[5]) / 10.0);
                        memcpy(last, cur, sizeof last);
                    }
                    if (g_opt.pmuSplit) pmu_split_report(g_perf);
                    fprintf(g_perf, "# clock %.0f MHz\n", cpu_mhz());
                }
                g_where = WH_PERFCSV;
                fflush(g_perf);
                prof_end(PROF_LOG, t0p);
            }
            g_where = WH_PERF;
            if (g_profClock) {
                // GX, then its parts: textures, vertices, waiting for the GPU (40 columns)
                con_printf("\x1b[9;0HGX%5.1f%% tex%5.1f vtx%5.1f gpu%5.1f \n", p[PROF_GX], p[PROF_GXTEX], p[PROF_GXVTX],
                       p[PROF_GXSYNC]);
                // disc, log; then the vertex path's gather (dec) and lighting (lit) parts of vtx
                con_printf("disc%5.1f log%4.1f  dec%5.1f lit%5.1f  \n", p[PROF_DISC], p[PROF_LOG], p[PROF_VDEC],
                       p[PROF_VLIGHT]);
                con_printf("JIT compile %4.1f%%  present %4.1f%%   \n", p[PROF_JITC], p[PROF_PRESENT]);
                con_printf("wait %4.1f%%  guest+other %4.1f%%   \n", 100.0 * (double)waitTicks / tot, rest);
                // cpu: inside gekko_run (includes GX and disc work the guest triggers); fpfb / fb: interpreted
                // float and other instructions from compiled code (sampled)
                con_printf("cpu%5.1f fpfb%5.1f fb%5.1f    \n", p[PROF_CPU], p[PROF_FBFP], p[PROF_FB]);
                // core 2: the GX thread's busy time, and the CPU thread waiting for it
                con_printf("GX thread%5.1f%%  cpu waits%5.1f%%  \n", p[PROF_GXTHREAD], p[PROF_GXWAIT]);
                con_printf("draws/s %5.0f  verts/s %6.0f   \n", draws, verts);
                if (g_jit && g_useJit) {
                    con_printf("k/s ins %5.0f fb %4.0f disp %5.0f %s\n", k[0], k[1], k[2], jit_linking(g_jit) ? "L" : "-");
                    con_printf("k/s slow ld %4.0f st %4.0f  icbi %llu   \n", k[3], k[4], (unsigned long long)jit_stats(g_jit)->icbiFlushes);
                }
            }
            con_flush();
            vertsLast = g_sys.gx.core.vertices;
            presentsLast = g_presents;
            fieldMax = fieldSum = 0;
            fieldN = 0;
            memset(g_hostProf, 0, sizeof g_hostProf);
            waitTicks = 0;
            drawsLast = g_sys.gx.core.draws;
            tLast = now; cycLast = g_sys.cpu.cycles; fieldsLast = g_fields;
        }
    }
    if (g_perf) { fclose(g_perf); g_perf = NULL; }
    gxthread_drain();
    if (g_opt.unattended) return result;
    printf("\nPress A to return to the menu\n");
    while (aptMainLoop()) { hidScanInput(); if (hidKeysDown() & (KEY_A | KEY_START)) break; gspWaitForVBlank(); }
    return result;
}

// The first program whose path contains `name` (ignoring case), or -1.
static int find_program(const char* name) {
    size_t n = strlen(name);
    for (int i = 0; i < g_nFiles && n; i++)
        for (const char* p = g_files[i]; *p; p++)
            if (!strncasecmp(p, name, n)) return i;
    return -1;
}

static void wait_key(u32 keys) {
    while (aptMainLoop()) { hidScanInput(); if (hidKeysDown() & keys) return; gspWaitForVBlank(); }
}

static bool g_quitApp;  // remote mode installed a new build: leave so it can be started

int jit_fuzz_main(int argc, char** argv);  // tests/jit/jit_fuzz.c

// A comment line in the current relay job's log (errors before the run's perf log exists).
static void relay_note(const char* msg) {
    FILE* t = relay_tee(NULL);
    if (t) { fprintf(t, "# %s\n", msg); fclose(t); }
}

// A job whose build had to be downloaded first is kept here across the restart into the new build.
#define RELAY_PENDING "sdmc:/gc3ds/relay_pending.txt"
static void save_pending(const RelayJob* j) {
    FILE* f = fopen(RELAY_PENDING, "w");
    if (f) { fprintf(f, "%u\n%s\n%s\n", j->id, j->build, j->args); fclose(f); }
}
static bool load_pending(RelayJob* j) {
    FILE* f = fopen(RELAY_PENDING, "r");
    if (!f) return false;
    char line[16], build[40];  // the build line is read whole: a 16-character id fills j->build, leaving its newline
    memset(j, 0, sizeof *j);
    bool ok = fgets(line, sizeof line, f) && fgets(build, sizeof build, f) && fgets(j->args, sizeof j->args, f);
    fclose(f);
    remove(RELAY_PENDING);
    j->id = (unsigned)strtoul(line, NULL, 10);
    build[strcspn(build, "\r\n")] = 0;
    snprintf(j->build, sizeof j->build, "%s", build);
    j->args[strcspn(j->args, "\r\n")] = 0;
    return ok && j->id;
}

// Restart into the .3dsx at `path` without the owner touching anything: Luma3DS's homebrew loader (port hb:ldr, the
// one the Homebrew Launcher uses to start a program) loads it the next time the homebrew title starts, and the
// chainloader restarts that title when gc3ds exits. false: the service is not there (the caller asks for a restart
// by hand). Commands as new-hbmenu's hbldr.c: 2 SetTarget (path without "sdmc:", static buffer 0), 3 SetArgv (u32 argc,
// then the NUL-separated strings, static buffer 1).
static Result g_relaunchRc;  // why relaunch_into failed (0: it did not)
// The requests to hb:ldr can block for good (2DS, job 108: stuck at "Restarting..." with the session obtained without
// blocking), so they run on their own thread and relaunch_into waits for it at most 3 s; a thread still blocked then
// is left behind (the process ends soon after) and the owner restarts by hand.
static const char* g_relaunchPath;
static volatile int g_relaunchDone;  // 0 running, 1 done (g_relaunchRc says how)
static bool relaunch_ipc(const char* path);
static void relaunch_thread(void* arg) {
    (void)arg;
    bool ok = relaunch_ipc(g_relaunchPath);
    if (ok && !g_relaunchRc) g_relaunchRc = 0;
    else if (!ok && !g_relaunchRc) g_relaunchRc = (Result)0xFFFFFFFF;
    __sync_synchronize();
    g_relaunchDone = 1;
}
static bool relaunch_into(const char* path) {
    g_relaunchRc = 0;
    g_relaunchDone = 0;
    g_relaunchPath = path;
    s32 prio = 0x30;
    svcGetThreadPriority(&prio, CUR_THREAD_HANDLE);
    Thread t = threadCreate(relaunch_thread, NULL, 8 * 1024, prio, 1, true);
    if (!t) { g_relaunchRc = (Result)0xFFFFFFFE; return false; }
    u64 limit = svcGetSystemTick() + 3ull * SYSCLOCK_ARM11;
    while (!g_relaunchDone && svcGetSystemTick() < limit) svcSleepThread(10000000LL);
    if (!g_relaunchDone) { g_relaunchRc = (Result)0xFFFFFFFD; return false; }  // no answer within 3 s
    __sync_synchronize();
    if (R_FAILED(g_relaunchRc)) return false;
    aptSetChainloaderToSelf();
    return true;
}
static bool relaunch_ipc(const char* path) {
    Handle h;
    // hb:ldr is a named port of Luma's loader sysmodule (sysmodules/loader/source/main.c, 2 sessions), and the Homebrew
    // Launcher opens it with svcConnectToPort (3ds-hbmenu source/loaders/rosalina.c). Through srv (jobs 97-112) the
    // request never answered: "hb:ldr gave no answer in 3 s" on every new build.
    Result got = svcConnectToPort(&h, "hb:ldr");
    if (R_FAILED(got)) { g_relaunchRc = got; return false; }
    const char* target = !strncmp(path, "sdmc:", 5) ? path + 5 : path;
    static u32 argBuf[0x400 / 4];
    memset(argBuf, 0, sizeof argBuf);
    argBuf[0] = 1;  // argv[0] = the full path, as the launcher passes it (main takes its own path from it)
    snprintf((char*)&argBuf[1], sizeof argBuf - 4, "%s", path);
    u32* cmd = getThreadCommandBuffer();
    cmd[0] = IPC_MakeHeader(2, 0, 2);
    cmd[1] = IPC_Desc_StaticBuffer(strlen(target) + 1, 0);
    cmd[2] = (u32)target;
    Result rc = svcSendSyncRequest(h);
    if (R_SUCCEEDED(rc)) rc = (Result)cmd[1];
    if (R_SUCCEEDED(rc)) {
        cmd = getThreadCommandBuffer();
        cmd[0] = IPC_MakeHeader(3, 0, 2);
        cmd[1] = IPC_Desc_StaticBuffer(sizeof argBuf, 1);
        cmd[2] = (u32)argBuf;
        rc = svcSendSyncRequest(h);
        if (R_SUCCEEDED(rc)) rc = (Result)cmd[1];
    }
    svcCloseHandle(h);
    if (R_FAILED(rc)) { g_relaunchRc = rc; return false; }
    return true;
}

// When a restart into a new build began (ms, osGetTime): the new build reports how long it took.
#define RELAY_RESTART "sdmc:/gc3ds/relay_restart.txt"

// After a job was kept for the next start (save_pending): restart into `selfPath` by itself, or ask for it.
static void relay_note(const char* msg);
static void restart_for_job(const char* selfPath, const char* why) {
    printf("%s\nRestarting...\n", why);
    FILE* rf = fopen(RELAY_RESTART, "w");
    if (rf) { fprintf(rf, "%llu\n", (unsigned long long)osGetTime()); fclose(rf); }
    bool autoRestart = relaunch_into(selfPath);
    if (!autoRestart) {
        char note[96];
        snprintf(note, sizeof note, "relay: automatic restart unavailable (%08lx%s): asked for a restart by hand", (unsigned long)g_relaunchRc,
                 g_relaunchRc == (Result)0xFFFFFFFD ? ", hb:ldr gave no answer in 3 s" : "");
        printf("(automatic restart unavailable: %08lx)\n", (unsigned long)g_relaunchRc);
        relay_note(note);
    }
    relay_flush();  // the download notes reach the job's log before this process ends
    if (autoRestart) {
        printf("Restarting by itself...\n");
        for (int i = 0; i < 60 && aptMainLoop(); i++) gspWaitForVBlank();  // a second to read it
    } else {
        printf("Press START, then open gc3ds again:\nthe job runs when Remote runs starts.\n");
        wait_key(KEY_START);
    }
    g_quitApp = true;
}

// Remote runs: jobs queued from the PC through the relay (relay.c, core/tools/relay.sh) run one after another, each
// streaming its perf log back; a job for another build downloads it, keeps the job and asks for a restart.
// SELECT leaves. Jobs default to 600 s so a forgotten --seconds cannot run forever.
static void remote_mode(const char* selfPath) {
    char err[256];
    consoleClear();
    printf("Remote runs: starting the network...\n");
    if (!relay_start(err, sizeof err)) { printf("relay: %s\n\nPress A\n", err); wait_key(KEY_A); return; }
    const char* self = relay_self_id(selfPath);
    RelayJob job;
    bool have = load_pending(&job), fromPending = have;
    FILE* rf = fopen(RELAY_RESTART, "r");
    unsigned long long restartAt = 0;
    if (rf) { if (fscanf(rf, "%llu", &restartAt) != 1) restartAt = 0; fclose(rf); remove(RELAY_RESTART); }
    u64 lastPoll = 0;
    u32 polls = 0;
    for (;;) {
        if (!have) {
            if (!aptMainLoop()) { g_quitApp = true; return; }
            hidScanInput();
            if (hidKeysDown() & KEY_SELECT) return;
            if (lastPoll && svcGetSystemTick() - lastPoll < 10ull * SYSCLOCK_ARM11) { gspWaitForVBlank(); continue; }
            lastPoll = svcGetSystemTick();
            int r = relay_next(&job, err, sizeof err);
            consoleClear();
            printf("Remote runs - this build %s\nSELECT: back to the menu\n\n", self);
            if (r < 0) { printf("relay error (retrying every 10 s):\n%s\n", err); continue; }
            if (r == 0) { printf("waiting for jobs (checked %lu times)\n", (unsigned long)++polls); continue; }
            have = true;
            fromPending = false;
        }
        have = false;
        printf("job %u: %s\n", job.id, job.args);
        relay_job_begin(job.id, fromPending ? RELAY_RESUME_SEQ : 0);
        if (fromPending && restartAt) {
            char note[96];
            snprintf(note, sizeof note, "relay: restarted into this build %.1f s after the download", (double)(osGetTime() - restartAt) / 1000.0);
            relay_note(note);
            restartAt = 0;
        }
        if (strcmp(job.build, "-") && strcmp(job.build, self)) {
            if (fromPending) {
                snprintf(err, sizeof err, "still build %s after the update to %s", self, job.build);
                printf("%s\n", err); relay_note(err); relay_job_end("failed");
                continue;
            }
            printf("downloading build %s...\n", job.build);
            if (!relay_fetch_build(job.build, selfPath, err, sizeof err)) {
                printf("%s\n", err); relay_note(err); relay_job_end("failed");
                continue;
            }
            save_pending(&job);
            char why[200];
            snprintf(why, sizeof why, "New build installed at %s.", selfPath);
            restart_for_job(selfPath, why);
            return;
        }
        if (strstr(job.args, "--fuzz")) {
            // The JIT fuzzer needs two machines of its own: it runs at the next start, before gc3ds allocates anything.
            save_pending(&job);
            restart_for_job(selfPath, "This job runs the JIT fuzzer at startup.");
            return;
        }
        RunOpts o;
        opts_default(&o);
        o.unattended = true;
        o.seconds = 600;
        char args[sizeof job.args], *av[64];
        int ac = 0;
        snprintf(args, sizeof args, "%s", job.args);
        for (char* t = strtok(args, " "); t && ac < 64; t = strtok(NULL, " ")) av[ac++] = t;
        int gi = -1;
        if (!opts_parse(&o, ac, av, err, sizeof err)) {}
        else if ((gi = find_program(o.game)) < 0) snprintf(err, sizeof err, "no program matches --game \"%s\"", o.game);
        if (gi < 0) { printf("%s\n", err); relay_note(err); relay_job_end("failed"); continue; }
        g_opt = o;
        g_relayRun = true;
        const char* result = run(g_files[gi]);
        g_relayRun = false;
        if (g_runErr[0]) relay_note(g_runErr);
        relay_job_end(result);
        opts_default(&g_opt);
        lastPoll = 0;
    }
}

// A relay job "--fuzz CASES [SEED]" kept across a restart: the JIT differential fuzzer (tests/jit/jit_fuzz.c) on this
// CPU, its output streamed to the job, before gc3ds allocates its own machine. qemu cannot show behaviour that only the
// ARM11 has; this can.
static void fuzz_job_at_startup(void) {
    RelayJob job;
    if (!relay_configured()) return;
    FILE* f = fopen(RELAY_PENDING, "r");
    if (!f) return;
    char buf[1200] = "";
    size_t n = fread(buf, 1, sizeof buf - 1, f);
    fclose(f);
    buf[n] = 0;
    if (!strstr(buf, "--fuzz") || !load_pending(&job)) return;
    char err[200];
    printf("JIT fuzzer for relay job %u...\n", job.id);
    if (!relay_start(err, sizeof err)) { printf("relay: %s\n", err); return; }
    relay_job_begin(job.id, RELAY_RESUME_SEQ);
    char cases[16] = "20000", seed[24] = "1", mode[24] = "";
    const char* p = strstr(job.args, "--fuzz");
    sscanf(p, "--fuzz %15s %23s %23s", cases, seed, mode);
    char* av[4] = { "jit_fuzz", cases, seed, mode };
    FILE* t = relay_tee(NULL);
    FILE* con = stdout;
    if (t) stdout = t;
    int rc = jit_fuzz_main(mode[0] ? 4 : 3, av);
    fflush(stdout);
    stdout = con;
    if (t) fclose(t);
    printf("fuzzer finished: %s\n", rc ? "FAILURES (see the relay log)" : "no failures");
    relay_job_end("done");
}

int main(int argc, char** argv) {
    opts_default(&g_opt);
    // Our own .3dsx (a relay job for another build replaces it). 3dslink runs us from memory ("3dslink:/...").
    const char* selfPath = argc > 0 && argv[0] && !strncmp(argv[0], "sdmc:/", 6) ? argv[0] : "sdmc:/3ds/gc3ds.3dsx";
    jit_host_fpscr_init();  // the kernel starts threads rounding toward zero; compiled float code assumes nearest
    gfxInitDefault();
    gfxSetDoubleBuffering(GFX_TOP, true);
    consoleInit(GFX_BOTTOM, NULL);
    crashlog_open();               // a crash of the last run goes to the next perf.csv
    crashlog_thread(CRASH_MAIN);
    double mhzBefore = cpu_mhz();
    osSetSpeedupEnable(true);
    double mhzSpeedup = cpu_mhz();
    // The explicit request gcbench used (it measured 109 vs 329 MIPS between the clocks), in case the libctru call
    // above has no effect for this app.
    bool ptm = R_SUCCEEDED(ptmSysmInit());
    Result cfg = ptm ? PTMSYSM_ConfigureNew3DSCPU(3) : -1;
    if (ptm) ptmSysmExit();
    snprintf(g_clockLine, sizeof g_clockLine, "clock MHz: start %.0f, speedup %.0f, ptm %.0f (%s %08lx)",
             mhzBefore, mhzSpeedup, cpu_mhz(), ptm ? "ok" : "none", (unsigned long)cfg);
    probe_cores();
    romfsInit();
    fuzz_job_at_startup();
    scan_dir("romfs:/dols");
    scan_dir("sdmc:/gc3ds");
    scan_dir("sdmc:/gc3ds/games");
    relay_reserve();  // before MEM1, ARAM and the JIT's code take the heap
    if (!sys_init(&g_sys)) { printf("out of memory\n"); svcSleepThread(3000000000LL); gfxExit(); return 1; }
    crashlog_set_info(CRASH_INFO_MEM1, (u32)g_sys.mem1);
    // 16 MB of code: Twilight Princess's title scene did not fit in 4 MB, and a full cache is dropped whole, so it
    // recompiled about a million blocks in 12 emulated seconds (35% of the time).
    g_jit = jit_create(&g_sys.cpu, g_sys.mem1, MEM1_SIZE, 16u << 20);
    printf("measuring the hardware (sdmc:/gc3ds/hw.txt)...\n");
    // Test builds that autorun a program (Azahar) skip the JIT benchmarks: Azahar runs them very slowly.
    FILE* testBuild = fopen("romfs:/autorun.txt", "r");
    if (testBuild) fclose(testBuild);
    hwprobe_run(&g_sys, testBuild ? NULL : g_jit);
    g_pica = pica_attach(&g_sys);
    if (!g_pica) { printf("GPU backend unavailable, using the software renderer\n"); gx_soft_attach(&g_sys); }
    // GX on core 2 (gxthread.c); B in the menu switches it off for comparison.
    g_gxThread = g_pica && gxthread_start(&g_sys, present_field);
    g_sys.on_field = on_field;
    if (g_jit) {
        g_sys.icbiHook = on_icbi;
        g_sys.icbiCtx = g_jit;
        jit_set_gather(g_jit, gp_wgp_put, &g_sys, GP_WGP_EA);
        jit_set_gather_buffer(g_jit, g_sys.gx.wgp, &g_sys.gx.wgpLen);
    }
    // romfs:/autorun.txt (test builds) names a program to start without the menu.
    FILE* ar = fopen("romfs:/autorun.txt", "r");
    if (ar) {
        char name[200] = "";
        if (fgets(name, sizeof name, ar)) {
            name[strcspn(name, "\r\n")] = 0;
            char opt[64] = "";  // optional second line gpupos=0 / gpupos=1: the transform, fixed for the run
            g_abGpuPos = false;
            g_gpuPosFixed = !(fgets(opt, sizeof opt, ar) && strstr(opt, "gpupos=0"));
            for (int i = 0; i < g_nFiles; i++) if (strstr(g_files[i], name)) { run(g_files[i]); break; }
        }
        fclose(ar);
    }
    // Arguments (3dslink -a <ip> gc3ds.3dsx -- --game Twilight --seconds 120 ...): one run, then back to the launcher.
    if (argc > 1) {
        char err[160];
        g_opt.unattended = true;
        int gi = -1;
        if (!opts_parse(&g_opt, argc - 1, argv + 1, err, sizeof err)) printf("%s\n", err);
        else if ((gi = find_program(g_opt.game)) < 0) printf("no program matches --game \"%s\"\n", g_opt.game);
        else run(g_files[gi]);
        if (gi < 0) { printf("Press A\n"); wait_key(KEY_A); }
        g_quitApp = true;
    }
    // relay.txt "auto 1": remote runs start by themselves (B within 3 s for the menu), so the 2DS can be left waiting.
    if (!g_quitApp && relay_auto()) {
        bool pending = false;  // a job kept across a restart into a new build starts at once
        FILE* pf = fopen(RELAY_PENDING, "r");
        if (pf) { pending = true; fclose(pf); }
        printf("\nRemote runs start in %s (B: menu)\n", pending ? "1 s" : "3 s");
        u64 t = svcGetSystemTick();
        bool menu = false;
        while (!menu && aptMainLoop() && svcGetSystemTick() - t < (pending ? 1ull : 3ull) * SYSCLOCK_ARM11) {
            hidScanInput();
            menu = hidKeysDown() & KEY_B;
            gspWaitForVBlank();
        }
        if (!menu) remote_mode(selfPath);
    }
    while (!g_quitApp) {
        int i = pick();
        if (i == -2) { remote_mode(selfPath); continue; }
        if (i < 0) break;
        run(g_files[i]);
    }
    relay_stop();
    sys_free(&g_sys);
    romfsExit();
    gfxExit();
    return 0;
}
