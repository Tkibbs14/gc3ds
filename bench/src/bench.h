#pragma once
// Shared harness for gcbench. Every module logs through P(); lines go to the bottom-screen console, the HTTP
// results page and sdmc:/gcbench/results.txt (flushed per line, so a crash still leaves everything before it).
#include <3ds.h>

void P(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
double now_s(void);

// Call before each individual test: records which test is running, so a hang or crash is attributable.
void step(const char* name);

// Each module runs all of its tests and returns. The top screen is free for rendering; the console is on the bottom.
void bench_cpu(void);
void bench_sys(void);
void bench_gpu(void);
