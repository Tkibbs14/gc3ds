#pragma once
// ARM11 MPCore performance counters on the 2DS (svcControlPerformanceCounter): the CPU's own count of what core 0
// does per second, so the cost of emulation can be split into instructions, stalls, cache and TLB misses and branch
// mispredictions without guessing. Each core has a cycle counter and two event counters; four event pairs rotate
// one per second. The SCU counters see the four cores' traffic to the L2 cache and memory.
#include <stdbool.h>
#include <stdio.h>

// Enables the counters (returns false and leaves the reason in pmu_status() if the kernel refuses).
bool pmu_init(void);
const char* pmu_status(void);
// Column names for perf.csv (with a leading comma) and one row of values: the last interval's counts per second,
// empty for events not measured in it. Call once a second from the thread on core 0; it then arms the next pair.
const char* pmu_columns(void);
void pmu_row(FILE* f, double secs);
// Self-test for hw.txt: known loops on core 0, so the counters' meaning (per core? which clock?) is checked.
void pmu_probe(FILE* f);
// For measurements of a piece of code: pmu_select clears the counters and puts event pair p (0: instructions and
// mispredicted branches, 1: I-cache and D-cache read misses, ...) on the two event counters; pmu_read returns the
// cycles and the two events since then (false when the counters are unavailable).
void pmu_select(unsigned p);
bool pmu_read(unsigned long long* cyc, unsigned long long* ev0, unsigned long long* ev1);
