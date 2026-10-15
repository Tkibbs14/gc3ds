// Crash records (crashlog.c): an exception in our threads goes to sdmc:/gc3ds/crash.bin; the next run reports it.
#pragma once
#include <3ds.h>
#include <stdio.h>

enum { CRASH_MAIN, CRASH_GX, CRASH_VWORKER, CRASH_THREADS };
enum { CRASH_INFO_JIT_START, CRASH_INFO_JIT_END, CRASH_INFO_MEM1, CRASH_INFO_N };

void crashlog_open(void);                    // at startup: reads a previous record, keeps the file open
void crashlog_thread(int thread);            // in the thread: installs the recording handler
void crashlog_record(ERRF_ExceptionInfo* ex, CpuRegisters* r, int thread);  // for a handler of our own (jit_3ds.c)
void crashlog_set_info(int k, u32 v);
void crashlog_report(FILE* f);               // prints the previous record once (perf.csv)
void crashlog_luma_again(FILE* f, const char* list);  // --luma-dumps N[,N...]: those arm11 dumps reported again
// Set while a relay job runs (main.c): after recording, the crashed thread goes on to this function (on a stack of its
// own) instead of faulting again into Luma's exception screen, so gc3ds restarts by itself and reports the crash.
extern void (*crashlog_recover)(void);
