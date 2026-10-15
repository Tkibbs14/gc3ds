// Crash records: a CPU exception in one of our threads (main / JIT, GX on core 2, vertex worker on core 3) is written
// to sdmc:/gc3ds/crash.bin before the process ends the default way, and the next run prints it into perf.csv
// ("# previous crash"), so a crash on the 2DS is read from the relay instead of from the screen. The file is opened
// at startup and the handler writes it with FSFILE_Write: no allocation and no newlib locks in the crashing process.
#include <3ds.h>
#include <stdio.h>
#include <dirent.h>
#include <string.h>
#include <stdlib.h>
#include "crashlog.h"

#define CRASH_MAGIC 0x43524153u  // "CRAS"
typedef struct {
    u32 magic, thread;
    ERRF_ExceptionInfo ex;
    CpuRegisters regs;
    u32 info[CRASH_INFO_N];  // crashlog_set_info: the JIT code buffer, MEM1
    u32 code[8];             // words at pc - 16 .. pc + 12 (0 for a prefetch abort)
    u32 stack[24];           // words at sp (0 when sp is outside the heap and stack areas)
    char build[24];
} CrashRec;

static const char* const kThreads[CRASH_THREADS] = { "main", "gx", "vworker" };
static Handle g_file;
static u32 g_info[CRASH_INFO_N];
static char g_prev[1400], g_status[96];
static u8 g_stacks[CRASH_THREADS][0x2000] __attribute__((aligned(8)));
static CrashRec g_rec;

void crashlog_set_info(int k, u32 v) { if (k >= 0 && k < CRASH_INFO_N) g_info[k] = v; }

static int put(char* p, int left, const char* fmt, u32 v) { int n = snprintf(p, (size_t)left, fmt, (unsigned long)v); return n < 0 ? 0 : n < left ? n : left - 1; }

// Luma3DS's own dumps (sdmc:/luma/dumps/arm11/ and arm9/crash_dump_NNNNNNNN.dmp), for an exception our handlers never
// saw (a thread without one, another process, or the ARM9: Process9 runs the SD card and the file system): the newest
// one of each kind not reported yet goes to the next perf.csv as hex words (Luma's ExceptionDumpHeader, the registers,
// then the end of the file: the process name and title ID). Only arm11 was read until 2026-10-13, when a Luma
// exception screen left no new arm11 dump.
static char g_luma[3200];
// One dump as hex words into out (count: how many there are, or -1 when not known).
static bool luma_dump_print(const char* kind, const char* name, int count, char* out, int outLeft) {
    char path[128];
    snprintf(path, sizeof path, "sdmc:/luma/dumps/%s/%s", kind, name);
    FILE* f = fopen(path, "rb");
    if (!f) return false;
    static u32 w[400];
    size_t n = fread(w, 4, 400, f);
    fclose(f);
    char* p = out;
    int left = outLeft;
    int k = snprintf(p, (size_t)left, "# luma dump %s/%s (%d dumps, %u bytes read):", kind, name, count, (unsigned)(n * 4));
    p += k; left -= k;
    for (size_t i = 0; i < n && i < 40 && left > 12; i++) { k = put(p, left, " %08lx", w[i]); p += k; left -= k; }
    k = snprintf(p, (size_t)left, "\n# luma dump end:"); p += k; left -= k;
    for (size_t i = n > 12 ? n - 12 : 0; i < n && left > 12; i++) { k = put(p, left, " %08lx", w[i]); p += k; left -= k; }
    if (left > 2) { *p++ = '\n'; *p = 0; }
    return true;
}
static void luma_dump_scan_one(const char* kind, char* out, int outLeft) {
    char dir[48], rep[48];
    snprintf(dir, sizeof dir, "sdmc:/luma/dumps/%s", kind);
    snprintf(rep, sizeof rep, "sdmc:/gc3ds/luma_reported%s.txt", strcmp(kind, "arm11") ? "_arm9" : "");
    DIR* d = opendir(dir);
    if (!d) { snprintf(out, (size_t)outLeft, "# luma %s dumps: none (no %s)\n", kind, dir); return; }
    char newest[64] = "";
    int count = 0;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (!strstr(e->d_name, ".dmp")) continue;
        count++;
        if (strlen(e->d_name) < sizeof newest && strcmp(e->d_name, newest) > 0) strcpy(newest, e->d_name);
    }
    closedir(d);
    if (!newest[0]) { snprintf(out, (size_t)outLeft, "# luma %s dumps: none\n", kind); return; }
    char last[64] = "";
    FILE* f = fopen(rep, "r");
    if (f) { if (!fgets(last, sizeof last, f)) last[0] = 0; fclose(f); last[strcspn(last, "\r\n")] = 0; }
    if (!strcmp(last, newest)) { snprintf(out, (size_t)outLeft, "# luma %s dumps: %d, newest %s already reported\n", kind, count, newest); return; }
    if (!luma_dump_print(kind, newest, count, out, outLeft)) return;
    f = fopen(rep, "w");
    if (f) { fprintf(f, "%s\n", newest); fclose(f); }
}

// Reports arm11 dumps by number again (--luma-dumps N[,N...]): one whose report went to a run whose log was lost.
void crashlog_luma_again(FILE* f, const char* list) {
    static char buf[1600];
    while (f && *list) {
        char* end;
        unsigned long n = strtoul(list, &end, 10);
        if (end == list) break;
        char name[40];
        snprintf(name, sizeof name, "crash_dump_%08lu.dmp", n);
        buf[0] = 0;
        if (luma_dump_print("arm11", name, -1, buf, (int)sizeof buf)) fputs(buf, f);
        else fprintf(f, "# luma dump arm11/%s: cannot read\n", name);
        list = *end == ',' ? end + 1 : end;
    }
}
static void luma_dump_scan(void) {
    g_luma[0] = 0;
    luma_dump_scan_one("arm11", g_luma, (int)sizeof g_luma / 2);
    size_t used = strlen(g_luma);
    luma_dump_scan_one("arm9", g_luma + used, (int)(sizeof g_luma - used));
}

void crashlog_open(void) {
    luma_dump_scan();
    Result rc = FSUSER_OpenFileDirectly(&g_file, ARCHIVE_SDMC, fsMakePath(PATH_EMPTY, ""), fsMakePath(PATH_ASCII, "/gc3ds/crash.bin"),
                                        FS_OPEN_READ | FS_OPEN_WRITE | FS_OPEN_CREATE, 0);
    if (R_FAILED(rc)) { g_file = 0; snprintf(g_status, sizeof g_status, "# crash log: cannot open sdmc:/gc3ds/crash.bin (%08lx)\n", (unsigned long)rc); return; }
    snprintf(g_status, sizeof g_status, "# crash log: sdmc:/gc3ds/crash.bin open\n");
    CrashRec r;
    u32 got = 0;
    if (R_SUCCEEDED(FSFILE_Read(g_file, &got, 0, &r, sizeof r)) && got == sizeof r && r.magic == CRASH_MAGIC) {
        char* p = g_prev;
        int left = sizeof g_prev;
        r.build[sizeof r.build - 1] = 0;
        int n = snprintf(p, (size_t)left, "# previous crash: build %s, thread %s, exception %u, pc %08lx, far %08lx, fsr %08lx, lr %08lx, sp %08lx, cpsr %08lx; jit code %08lx-%08lx, MEM1 %08lx\n# previous crash regs:",
                         r.build, r.thread < CRASH_THREADS ? kThreads[r.thread] : "?", (unsigned)r.ex.type, (unsigned long)r.regs.pc,
                         (unsigned long)r.ex.far, (unsigned long)r.ex.fsr, (unsigned long)r.regs.lr, (unsigned long)r.regs.sp,
                         (unsigned long)r.regs.cpsr, (unsigned long)r.info[0], (unsigned long)r.info[1], (unsigned long)r.info[2]);
        p += n; left -= n;
        for (int i = 0; i < 13 && left > 16; i++) { n = put(p, left, " %08lx", r.regs.r[i]); p += n; left -= n; }
        n = snprintf(p, (size_t)left, "\n# previous crash code (pc-16..):"); p += n; left -= n;
        for (int i = 0; i < 8 && left > 16; i++) { n = put(p, left, " %08lx", r.code[i]); p += n; left -= n; }
        n = snprintf(p, (size_t)left, "\n# previous crash stack:"); p += n; left -= n;
        for (int i = 0; i < 24 && left > 16; i++) { n = put(p, left, " %08lx", r.stack[i]); p += n; left -= n; }
        if (left > 2) { *p++ = '\n'; *p = 0; }
        u32 zero = 0, wrote;
        FSFILE_Write(g_file, &wrote, 0, &zero, 4, FS_WRITE_FLUSH);  // reported once
    }
}

void crashlog_report(FILE* f) {
    if (!f) return;
    fputs(g_status, f);
    fputs(g_luma, f);
    fputs(g_prev, f);
    g_prev[0] = g_luma[0] = 0;
}

static bool readable(u32 a) { return (a >= 0x00100000u && a < 0x10000000u) || (a >= 0x14000000u && a < 0x40000000u); }

void crashlog_record(ERRF_ExceptionInfo* ex, CpuRegisters* r, int thread) {
    if (!g_file) return;
    memset(&g_rec, 0, sizeof g_rec);
    g_rec.magic = CRASH_MAGIC;
    g_rec.thread = (u32)thread;
    g_rec.ex = *ex;
    g_rec.regs = *r;
    memcpy(g_rec.info, g_info, sizeof g_info);
    u32 pc = r->pc & ~3u, sp = r->sp & ~3u;
    if (ex->type != ERRF_EXCEPTION_PREFETCH_ABORT && readable(pc - 16) && readable(pc + 31))
        for (int i = 0; i < 8; i++) g_rec.code[i] = ((const u32*)(pc - 16))[i];
    if (readable(sp) && readable(sp + 95))
        for (int i = 0; i < 24; i++) g_rec.stack[i] = ((const u32*)sp)[i];
    strncpy(g_rec.build, __DATE__ " " __TIME__, sizeof g_rec.build - 1);
    u32 wrote;
    FSFILE_Write(g_file, &wrote, 0, &g_rec, sizeof g_rec, FS_WRITE_FLUSH);
}

void crash_resume(CpuRegisters* r) __attribute__((noreturn));
__asm__(".text; .arm; .align 2; .global crash_resume; .type crash_resume, %function;"
        "crash_resume: ldr r1, [r0, #64]; msr cpsr_f, r1; ldmia r0, {r0-r15};");

// Records, then removes the handler and re-runs the instruction: it faults again, the default way.
static void handle(ERRF_ExceptionInfo* ex, CpuRegisters* r, int thread) {
    crashlog_record(ex, r, thread);
    threadOnException(NULL, NULL, NULL);
    crash_resume(r);
}
static void h_main(ERRF_ExceptionInfo* ex, CpuRegisters* r) { handle(ex, r, CRASH_MAIN); }
static void h_gx(ERRF_ExceptionInfo* ex, CpuRegisters* r) { handle(ex, r, CRASH_GX); }
static void h_vw(ERRF_ExceptionInfo* ex, CpuRegisters* r) { handle(ex, r, CRASH_VWORKER); }

void crashlog_thread(int thread) {
    static const ExceptionHandler kH[CRASH_THREADS] = { h_main, h_gx, h_vw };
    if (thread < 0 || thread >= CRASH_THREADS) return;
    threadOnException(kH[thread], g_stacks[thread] + sizeof g_stacks[thread], WRITE_DATA_TO_HANDLER_STACK);
}
