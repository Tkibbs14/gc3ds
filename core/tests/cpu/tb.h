#pragma once
// Flat test bus for the Gekko interpreter tests: 64 MB of RAM at physical 0, a small MMIO register file at
// 0x0C000000 (always through the bus, never host_ptr), and a syscall trap: the CPU fetching physical 0x00000C00 (the
// system-call vector with MSR[IP] = 0) stops the run so the harness can service Linux-style write/exit calls.
#include "../../src/cpu/gekko.h"

#define TB_RAM_SIZE (64u << 20)
#define TB_MMIO_BASE 0x0C000000u
#define TB_MMIO_SIZE 0x100u

typedef struct TestBus {
    GekkoBus bus;
    Gekko* g;
    u8* ram;
    u32 mmio[TB_MMIO_SIZE / 4];
    u32 mmioReads, mmioWrites;
    bool hitSyscall;
    bool noSyscallTrap;  // directed tests execute the real vector at 0xC00
    u32 cacheOps[4];
    u32 lastCacheOpPa;
    u32 dmaCount, dmaRam, dmaCache, dmaLen;
    bool dmaToRam;
    u8 lockedCache[16384];  // 0xE0000000..0xE0003FFF
    char lastFatal[256];
    u32 fatalCount;
} TestBus;

void tb_init(TestBus* tb, Gekko* g);
void tb_free(TestBus* tb);
u32 tb_r32(TestBus* tb, u32 pa);
void tb_w32(TestBus* tb, u32 pa, u32 v);
u64 tb_r64(TestBus* tb, u32 pa);
void tb_w64(TestBus* tb, u32 pa, u64 v);

// Loads a big-endian ELF32 PowerPC executable's PT_LOAD segments into RAM; returns the entry point or 0 on error.
u32 tb_load_elf(TestBus* tb, const char* path);

// Runs a Linux-user-style program (real mode, FP on) until it calls exit; appends write(1, ...) data to out.
// Returns false if it did not exit within maxCycles.
bool tb_run_linux(TestBus* tb, u32 entry, u8* out, u32 outCap, u32* outLen, u64 maxCycles);
