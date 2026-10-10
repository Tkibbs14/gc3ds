// 3DS logging: everything goes to sdmc:/gc3ds/log.txt (flushed per line, so it survives a crash); errors and
// OSReport output also go to the bottom-screen console.
#include <stdarg.h>
#include <stdio.h>
#include <sys/stat.h>
#include "../src/common.h"

u32 g_logMask = (1u << LOG_BOOT) | (1u << LOG_OS) | (1u << LOG_MEM) | (1u << LOG_HW) | (1u << LOG_GX) | (1u << LOG_DSP);

static FILE* g_log;
static const char* const kNames[LOG_NCAT] = { "cpu", "mem", "hw", "boot", "gx", "dsp", "exi", "si", "di", "vi", "os" };

static FILE* log_file(void) {
    if (!g_log) {
        mkdir("sdmc:/gc3ds", 0777);
        g_log = fopen("sdmc:/gc3ds/log.txt", "w");
    }
    return g_log;
}

static void emit(const char* tag, bool toScreen, const char* fmt, va_list ap) {
    char line[512];
    vsnprintf(line, sizeof line, fmt, ap);
    FILE* f = log_file();
    if (f) { fprintf(f, "[%s] %s", tag, line); fflush(f); }
    if (toScreen) printf("[%s] %s", tag, line);
}

void emu_log(LogCat cat, const char* fmt, ...) {
    if (!(g_logMask & (1u << cat))) return;
    va_list ap;
    va_start(ap, fmt);
    emit(kNames[cat], cat == LOG_OS, fmt, ap);
    va_end(ap);
}

void emu_error(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    emit("error", true, fmt, ap);
    va_end(ap);
}
