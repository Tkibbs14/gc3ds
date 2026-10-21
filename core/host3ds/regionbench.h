#pragma once
#include <stdio.h>
#include "../src/sys/system.h"
#include "../src/jit/jit.h"
void region_bench_arm(Jit* j, System* s, FILE* f, unsigned which);
extern bool region_bench_done;
extern bool region_bench_pending;
void region_bench_execute(void);
