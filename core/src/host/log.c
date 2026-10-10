#include <stdarg.h>
#include <stdio.h>
#include "../common.h"

u32 g_logMask = (1u << LOG_BOOT) | (1u << LOG_OS) | (1u << LOG_MEM);

static const char* const kNames[LOG_NCAT] = { "cpu", "mem", "hw", "boot", "gx", "dsp", "exi", "si", "di", "vi", "os" };

void emu_log(LogCat cat, const char* fmt, ...) {
    if (!(g_logMask & (1u << cat))) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[%s] ", kNames[cat]);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}

void emu_error(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "[error] ");
    vfprintf(stderr, fmt, ap);
    va_end(ap);
}
