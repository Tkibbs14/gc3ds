#pragma once
// Native library ports (port_j2d.c; docs/research/LIBRARY_PORTS.md).
#include "../common.h"

typedef struct Gekko Gekko;
typedef struct {
    u64 calls, refused, pipe, checked, bad;
    u32 badMat, badWhat, badAddr, badWant, badMine, badPipe[2];  // the first difference check mode found
    u32 badHash;  // a guest function whose code differs from the one ported (the port stays off)
    u64 refusedBy[8];  // by reason (port_j2d.c give_up)
    u32 otherTev;      // the last TEV block setGX not ported
} PortStats;
extern PortStats g_portStats;
extern int g_portMode;  // --port: 0 off, 1 on, 2 check (each call compared with the interpreter, whose result is kept), 3 plant
void port_set_memory(u8* mem1, u32 size);
void port_set_pipe(void (*write)(void* ctx, const u8* data, u32 len), void* ctx);
bool port_is_hook(u32 pc);
u32 port_run_hook(Gekko* g);  // guest instructions stood for (0: not taken, the guest code runs)
