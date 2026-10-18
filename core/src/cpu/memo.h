#pragma once
// Function memoization (memo.c): probe stage.
#include "gekko.h"

typedef struct {
    u64 calls, recorded, hits, misses, firsts, insns, reads, writes, pipe;
    u64 abort[4];  // [0] read something that is not RAM, [1] an exception, [2] too long, [3] record full / other
} MemoStats;
extern MemoStats g_memoStats;
extern u32 g_memoAddr;  // the hooked function's entry (0: off)
u32 memo_probe(Gekko* g);  // runs the call at g->pc in the interpreter, recorded; returns its instructions
// The port checker's recorder (port_j2d.c): the words the interpreter wrote, with their values before.
#define CHK_MAX 16384u
extern u32 g_chkW[CHK_MAX][2];
extern u32 g_chkN;
extern bool g_chkOn;
