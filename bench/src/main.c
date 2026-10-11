// gcbench: what the New 3DS can do for a GameCube/Wii emulator. Runs the CPU, system and GPU modules in turn.
// Results go to the bottom screen, sdmc:/gcbench/results.txt (flushed per line; the previous run is kept as
// results_prev.txt) and http://<3ds-ip>:8333/, answered between tests and after the run.
//
// A CPU exception on the main thread is caught: the handler logs the test, exception type and registers, then jumps
// back to the end of the module that crashed, so the remaining modules still run.
#include <3ds.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <malloc.h>
#include <netinet/in.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#include "bench.h"

#define OUT_CAP (64 * 1024)
#define RESULTS_PATH "sdmc:/gcbench/results.txt"

static char g_out[OUT_CAP];
static size_t g_outLen;
static FILE* g_file;
static char g_step[64] = "start";
static int g_listen = -1;
bool g_w2only, g_quick, g_followup;

static void append(const char* s) {
    size_t n = strlen(s);
    if (g_outLen + n < OUT_CAP) { memcpy(g_out + g_outLen, s, n); g_outLen += n; g_out[g_outLen] = 0; }
    if (g_file) { fputs(s, g_file); fflush(g_file); }
}

void P(const char* fmt, ...) {
    char line[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    fputs(line, stdout);
    append(line);
}

double now_s(void) { return (double)svcGetSystemTick() / SYSCLOCK_ARM11; }

// ---- results server: polled from the main thread (between tests), never during a timed region ----
static void serve_pending(void) {
    if (g_listen < 0) return;
    for (;;) {
        int c = accept(g_listen, NULL, NULL);
        if (c < 0) return;
        fcntl(c, F_SETFL, fcntl(c, F_GETFL, 0) & ~O_NONBLOCK);
        char req[512];
        recv(c, req, sizeof req, 0);
        char hdr[160];
        snprintf(hdr, sizeof hdr, "HTTP/1.0 200 OK\r\nContent-Type: text/plain\r\nConnection: close\r\n\r\n[at: %s]\n", g_step);
        send(c, hdr, strlen(hdr), 0);
        for (size_t off = 0; off < g_outLen;) {
            int sent = send(c, g_out + off, g_outLen - off, 0);
            if (sent <= 0) break;
            off += (size_t)sent;
        }
        close(c);
    }
}

static void start_server(void) {
    u32* socBuf = memalign(0x1000, 0x100000);
    if (!socBuf || R_FAILED(socInit(socBuf, 0x100000))) { P("socInit failed; no HTTP results\n"); return; }
    int s = socket(AF_INET, SOCK_STREAM, IPPROTO_IP);
    struct sockaddr_in addr = { 0 };
    addr.sin_family = AF_INET;
    addr.sin_port = htons(8333);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (s < 0 || bind(s, (struct sockaddr*)&addr, sizeof addr) < 0 || listen(s, 4) < 0) { P("socket failed\n"); return; }
    fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK);
    g_listen = s;
    struct in_addr me = { .s_addr = gethostid() };
    P("results: http://%s:8333/\n", inet_ntoa(me));
}

static volatile double g_stepStart;

void step(const char* name) {
    snprintf(g_step, sizeof g_step, "%s", name);
    g_stepStart = now_s();
    if (g_file) { fprintf(g_file, "  [%s]\n", name); fflush(g_file); }
    serve_pending();
}

// ---- hang capture: a watchdog thread notes any test that runs for more than 30 s ----
static void watchdog(void* arg) {
    char reported[64] = "";
    for (;;) {
        svcSleepThread(1000000000LL);
        if (now_s() - g_stepStart > 30 && strcmp(reported, g_step) != 0 && strcmp(g_step, "done") != 0) {
            snprintf(reported, sizeof reported, "%s", g_step);
            char b[128];
            snprintf(b, sizeof b, "\n!! HANG? [%s] has run for 30 s\n", reported);
            append(b);
        }
    }
}

// ---- crash capture ----
static u8 g_excStack[32 * 1024] __attribute__((aligned(8)));
static ERRF_ExceptionData g_excData;
static jmp_buf g_moduleJmp;
static volatile int g_inModule;
static u32 g_fpscrStart;

static const char* exc_name(int t) {
    switch (t) {
    case ERRF_EXCEPTION_PREFETCH_ABORT: return "prefetch abort";
    case ERRF_EXCEPTION_DATA_ABORT: return "data abort";
    case ERRF_EXCEPTION_UNDEFINED: return "undefined instr";
    case ERRF_EXCEPTION_VFP: return "VFP";
    default: return "exception";
    }
}

static void on_exception(ERRF_ExceptionInfo* ex, CpuRegisters* r) {
    char b[256];
    snprintf(b, sizeof b, "\n!! CRASH in [%s]: %s pc %08lx lr %08lx far %08lx fsr %08lx\n", g_step, exc_name(ex->type),
             r->pc, r->lr, ex->far, ex->fsr);
    fputs(b, stdout);
    append(b);
    snprintf(b, sizeof b, "!! r0 %08lx r1 %08lx r2 %08lx r3 %08lx r12 %08lx sp %08lx cpsr %08lx\n",
             r->r[0], r->r[1], r->r[2], r->r[3], r->r[12], r->sp, r->cpsr);
    append(b);
    if (g_inModule) longjmp(g_moduleJmp, 1);
    svcExitProcess();
}

static void run_module(const char* name, void (*fn)(void)) {
    P("== %s ==\n", name);
    g_inModule = 1;
    if (setjmp(g_moduleJmp) == 0) {
        fn();
    } else {
        // The crash may have left the FPU in short-vector mode; C code must not run like that.
        __asm__ volatile("vmsr fpscr, %0" : : "r"(g_fpscrStart));
        P("!! %s module aborted; continuing with the next one\n", name);
    }
    g_inModule = 0;
    P("\n");
}

static void cpu_module(void) { bench_cpu(); }

int main(void) {
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, NULL);
    osSetSpeedupEnable(true);
    __asm__ volatile("vmrs %0, fpscr" : "=r"(g_fpscrStart));
    threadOnException(on_exception, g_excStack + sizeof g_excStack, &g_excData);
    mkdir("sdmc:/gcbench", 0777);
    remove("sdmc:/gcbench/results_prev.txt");
    rename(RESULTS_PATH, "sdmc:/gcbench/results_prev.txt");
    g_file = fopen(RESULTS_PATH, "w");
    P("gcbench 4 (wave 2)\n");
    start_server();
    g_stepStart = now_s();
    // Core 1 when the system lends it, else core 0 at high priority; it sleeps 1 s between checks.
    APT_SetAppCpuTimeLimit(30);
    if (!threadCreate(watchdog, NULL, 8 * 1024, 0x18, 1, true)) threadCreate(watchdog, NULL, 8 * 1024, 0x18, 0, true);

    double t0 = now_s();
    // sdmc:/gcbench/w2only.txt selects a subset (see bench.h); without it everything runs.
    FILE* w2f = fopen("sdmc:/gcbench/w2only.txt", "r");
    g_w2only = w2f != NULL;
    bool skipGpu = false;
    if (w2f) {
        int mode = fgetc(w2f);
        fclose(w2f);
        skipGpu = mode == 'w';
        g_quick = mode == 'q' || mode == 'w' || mode == 'F';   // F: the follow-up set, quick (emulator)
        g_followup = mode == 'f' || mode == 'F';
        P("w2only.txt mode '%c': %s\n", mode > ' ' ? mode : '?', g_followup ? "follow-up tests (timed)" :
          g_quick ? "wave 2, quick emulator check" : "wave 2 only");
    }
    if (!g_w2only) {
        run_module("CPU", cpu_module);
        run_module("SYSTEM", bench_sys);
    }
    if (!skipGpu) run_module("GPU", bench_gpu);
    if (!g_followup) run_module("WAVE2", bench_wave);
    step("done");
    P("ALL DONE in %.0f s. START exits.\n", now_s() - t0);

    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        serve_pending();
        gspWaitForVBlank();
    }
    if (g_listen >= 0) close(g_listen);
    if (g_file) fclose(g_file);
    socExit();
    gfxExit();
    return 0;
}
