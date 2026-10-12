// gc3ds for the New 3DS: program picker, emulation loop, video output to the top screen, pad input, stats.
// Programs are listed from romfs:/dols (bundled test programs) and sdmc:/gc3ds (the user's own files).
#include <3ds.h>
#include <citro3d.h>
#define GC3DS_HOST_TYPES
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
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
static double cpu_mhz(void) {
    u32 n = 1000000;
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
    return 18e6 / secs / 1e6;
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
    bool abTraces, abHle;  // --ab traces|hle|none: which setting alternates every AB_SECS
    bool traces, hle;    // --traces on|off, --hle on|off: start values (an A/B flips them)
    bool unattended;     // arguments or relay: no "press A" at the end
} RunOpts;
static RunOpts g_opt;

static void opts_default(RunOpts* o) {
    memset(o, 0, sizeof *o);
    o->abTraces = true;
    o->traces = true;
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
        else if (!strcmp(a, "--ab")) { o->abTraces = !strcmp(v, "traces"); o->abHle = !strcmp(v, "hle"); }
        else if (!strcmp(a, "--traces")) o->traces = !strcmp(v, "on");
        else if (!strcmp(a, "--hle")) o->hle = !strcmp(v, "on");
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
                                                "vwait", "gxpres", "gxstate", "gxend", "vprep", "vpal", "vqueue" };
_Static_assert(PROF_N == 23, "kProfNames must list every PROF_ area");
static JitOpCounts g_fbPrev;  // fallbackRun at the last dump

static void perf_open(const char* path) {
    mkdir("sdmc:/gc3ds", 0777);
    g_perf = fopen("sdmc:/gc3ds/perf.csv", "a");
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
                    ",c3d_cpu_ms,c3d_gpu_ms,heap_free_mb,linear_free_mb,vram_free_kb,jit_link,links_s,vworker,earlyq,gpupos,esplit,indexed,memstub,pipefast,hle,traces");
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
    pica_capture(path);
    if (g_perf) fprintf(g_perf, "# shot %s at real %.1f s, emulated %.2f s\n", path + 5, realS, emuS);
    g_shotNext = (g_shotNext + 1) % 16;
    FILE* f = fopen("sdmc:/gc3ds/shots/next.txt", "w");
    if (f) { fprintf(f, "%d\n", g_shotNext); fclose(f); }
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
    if (g_perf && g_opt.state[0]) fprintf(g_perf, "# loaded state %s at field %lu\n", g_opt.state, (unsigned long)g_sys.vi.field);
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
    PicaStats pLast = g_picaStats;
    u64 linksLast = g_jit ? jit_stats(g_jit)->links : 0;
    if (g_jit) {
        jit_set_linking(g_jit, true);
        jit_set_mem_stubs(g_jit, true);
        g_pipeFast = true;
        jit_set_gather_buffer(g_jit, g_sys.gx.wgp, &g_sys.gx.wgpLen);
        hle_set_enabled(g_opt.hle || g_opt.abHle);
        jit_set_traces(g_jit, g_opt.traces);
        jit_invalidate_all(g_jit);
    }
    if (g_pica) {
        pica_set_vertex_worker(true);
        pica_set_early_queue(true);
        pica_set_gpu_transform(g_gpuPosFixed);
        pica_set_early_split(true);
        pica_set_indexed(true);
    }
    if (g_jit && g_useJit) jitsample_start(&g_sys.cpu);
    g_profClock = prof_clock;  // X toggles the profile lines
    memset(g_hostProf, 0, sizeof g_hostProf);
    while (aptMainLoop()) {
        g_where = WH_FRONT;
        update_input(&g_sys);
        if (hidKeysDown() & KEY_SELECT) break;
        if (g_opt.seconds && svcGetSystemTick() - t0 >= (u64)g_opt.seconds * SYSCLOCK_ARM11) break;
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
            printf("\x1b[5;0H%6.2f MIPS  %5.2f fields/s  (%4.1f%% speed)   \n", mips, fps, 100.0 * mips / 486.0);
            printf("emulated %.1f s, real %.1f s     \n", (double)g_sys.cpu.cycles / GEKKO_HZ, (double)(now - t0) / SYSCLOCK_ARM11);
            if (g_jit && g_useJit) {
                const JitStats* js = jit_stats(g_jit);
                printf("JIT %llu blocks, %.1f%% native   \n", (unsigned long long)js->blocksCompiled,
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
                linksLast = links;
                pmu_row(g_perf, secs);
                fputc('\n', g_perf);
                memcpy(xLast, x, sizeof xLast);
                pLast = *ps;
                ++perfRows;
                if (perfRows == 5 || perfRows == 15 || perfRows % 30 == 0)
                    perf_shot((double)(now - t0) / SYSCLOCK_ARM11, (double)g_sys.cpu.cycles / GEKKO_HZ);
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
                if (perfRows % 10 == 0) {
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
                        static u64 lastPal, lastSub;
                        fprintf(g_perf, "# gpu transform 10s: palette uploads %llu, draw calls for palette or projection "
                                "changes %llu, draws over the palette (CPU) %llu\n",
                                (unsigned long long)(g_picaStats.palUploads - lastPal),
                                (unsigned long long)(g_picaStats.subDraws - lastSub), (unsigned long long)(p[7] - lastP[7]));
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
                    fprintf(g_perf, "# clock %.0f MHz\n", cpu_mhz());
                }
                fflush(g_perf);
                prof_end(PROF_LOG, t0p);
            }
            if (g_profClock) {
                // GX, then its parts: textures, vertices, waiting for the GPU (40 columns)
                printf("\x1b[9;0HGX%5.1f%% tex%5.1f vtx%5.1f gpu%5.1f \n", p[PROF_GX], p[PROF_GXTEX], p[PROF_GXVTX],
                       p[PROF_GXSYNC]);
                // disc, log; then the vertex path's gather (dec) and lighting (lit) parts of vtx
                printf("disc%5.1f log%4.1f  dec%5.1f lit%5.1f  \n", p[PROF_DISC], p[PROF_LOG], p[PROF_VDEC],
                       p[PROF_VLIGHT]);
                printf("JIT compile %4.1f%%  present %4.1f%%   \n", p[PROF_JITC], p[PROF_PRESENT]);
                printf("wait %4.1f%%  guest+other %4.1f%%   \n", 100.0 * (double)waitTicks / tot, rest);
                // cpu: inside gekko_run (includes GX and disc work the guest triggers); fpfb / fb: interpreted
                // float and other instructions from compiled code (sampled)
                printf("cpu%5.1f fpfb%5.1f fb%5.1f    \n", p[PROF_CPU], p[PROF_FBFP], p[PROF_FB]);
                // core 2: the GX thread's busy time, and the CPU thread waiting for it
                printf("GX thread%5.1f%%  cpu waits%5.1f%%  \n", p[PROF_GXTHREAD], p[PROF_GXWAIT]);
                printf("draws/s %5.0f  verts/s %6.0f   \n", draws, verts);
                if (g_jit && g_useJit) {
                    printf("k/s ins %5.0f fb %4.0f disp %5.0f %s\n", k[0], k[1], k[2], jit_linking(g_jit) ? "L" : "-");
                    printf("k/s slow ld %4.0f st %4.0f  icbi %llu   \n", k[3], k[4], (unsigned long long)jit_stats(g_jit)->icbiFlushes);
                }
            }
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
    char line[16];
    memset(j, 0, sizeof *j);
    bool ok = fgets(line, sizeof line, f) && fgets(j->build, sizeof j->build, f) && fgets(j->args, sizeof j->args, f);
    fclose(f);
    remove(RELAY_PENDING);
    j->id = (unsigned)strtoul(line, NULL, 10);
    j->build[strcspn(j->build, "\r\n")] = 0;
    j->args[strcspn(j->args, "\r\n")] = 0;
    return ok && j->id;
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
        relay_job_begin(job.id);
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
            printf("\nNew build installed at %s.\nPress START, then open gc3ds again:\nthe job runs when Remote runs starts.\n", selfPath);
            wait_key(KEY_START);
            g_quitApp = true;
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
        const char* result = run(g_files[gi]);
        if (g_runErr[0]) relay_note(g_runErr);
        relay_job_end(result);
        opts_default(&g_opt);
        lastPoll = 0;
    }
}

int main(int argc, char** argv) {
    opts_default(&g_opt);
    // Our own .3dsx (a relay job for another build replaces it). 3dslink runs us from memory ("3dslink:/...").
    const char* selfPath = argc > 0 && argv[0] && !strncmp(argv[0], "sdmc:/", 6) ? argv[0] : "sdmc:/3ds/gc3ds.3dsx";
    jit_host_fpscr_init();  // the kernel starts threads rounding toward zero; compiled float code assumes nearest
    gfxInitDefault();
    gfxSetDoubleBuffering(GFX_TOP, true);
    consoleInit(GFX_BOTTOM, NULL);
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
    scan_dir("romfs:/dols");
    scan_dir("sdmc:/gc3ds");
    scan_dir("sdmc:/gc3ds/games");
    if (!sys_init(&g_sys)) { printf("out of memory\n"); svcSleepThread(3000000000LL); gfxExit(); return 1; }
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
        printf("\nRemote runs start in 3 s (B: menu)\n");
        u64 t = svcGetSystemTick();
        bool menu = false;
        while (!menu && aptMainLoop() && svcGetSystemTick() - t < 3ull * SYSCLOCK_ARM11) {
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
