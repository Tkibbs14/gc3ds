#pragma once
// Sampling profiler for core 0's compiled code (jitsample.c).
#include <stdbool.h>
#include <stdio.h>
#include "../src/jit/jit.h"

bool jitsample_start(const Gekko* g);
// Also walk the guest call stack at each sample (MEM1's host copy) and report return addresses ('# guest stacks 10s').
void jitsample_stacks(const u8* mem1);
// Writes the hottest guest blocks of the samples since the last report to f and starts over.
void jitsample_report(FILE* f, const Jit* j);
// The blocks sampled since the last report, most sampled first (jit_hot_blocks); call before code is dropped.
u32 jitsample_hot(const Jit* j, u32* pcs, u32 max);
