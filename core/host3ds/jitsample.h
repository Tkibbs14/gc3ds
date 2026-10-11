#pragma once
// Sampling profiler for core 0's compiled code (jitsample.c).
#include <stdbool.h>
#include <stdio.h>
#include "../src/jit/jit.h"

bool jitsample_start(const Gekko* g);
// Writes the hottest guest blocks of the samples since the last report to f and starts over.
void jitsample_report(FILE* f, const Jit* j);
