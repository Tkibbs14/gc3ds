#pragma once
// Gekko (PowerPC 750CL derivative) CPU core: state, the physical bus it talks to, and the run loop.
//
// Ownership: the CPU core owns everything architectural to the processor, including address translation (BATs; MSR
// IR/DR), the timebase and decrementer, exceptions and their vectors, and all SPRs. The system (src/hw) owns the
// physical address space: RAM, memory-mapped hardware, the write-gather pipe and the locked L1 cache, and reaches the
// CPU only through the functions declared at the bottom of this file.
#include "../common.h"

// Physical bus implemented by the system. Addresses are PHYSICAL (after BAT translation). Values are host-order.
typedef struct GekkoBus {
    void* ctx;
    u8 (*read8)(void* ctx, u32 pa);
    u16 (*read16)(void* ctx, u32 pa);
    u32 (*read32)(void* ctx, u32 pa);
    u64 (*read64)(void* ctx, u32 pa);
    void (*write8)(void* ctx, u32 pa, u8 v);
    void (*write16)(void* ctx, u32 pa, u16 v);
    void (*write32)(void* ctx, u32 pa, u32 v);
    void (*write64)(void* ctx, u32 pa, u64 v);
    u32 (*fetch)(void* ctx, u32 pa);  // instruction fetch (word aligned)
    // Optional fast path: host pointer to a big-endian RAM window for [pa, pa+len), or NULL. The interpreter may use
    // it for instruction fetch and data accesses; the system guarantees the pointer stays valid.
    u8* (*host_ptr)(void* ctx, u32 pa, u32 len);
    void (*dcbz)(void* ctx, u32 pa);  // zero the 32-byte line at pa (pa is 32-byte aligned)
    // Cache maintenance the system must see: dcbf/dcbst/dcbi (data, e.g. before DMA) and icbi (code may change).
    void (*cache_op)(void* ctx, int op, u32 pa);
    // Locked-cache DMA (HID2.LCE, DMA_U/DMA_L SPRs): copy len bytes between RAM and the locked cache.
    void (*lc_dma)(void* ctx, bool toRam, u32 ramPa, u32 cachePa, u32 len);
} GekkoBus;

enum { CACHE_DCBF, CACHE_DCBST, CACHE_DCBI, CACHE_ICBI };

// SPR numbers used outside the CPU core.
enum {
    SPR_XER = 1, SPR_LR = 8, SPR_CTR = 9, SPR_DSISR = 18, SPR_DAR = 19, SPR_DEC = 22, SPR_SDR1 = 25,
    SPR_SRR0 = 26, SPR_SRR1 = 27, SPR_SPRG0 = 272, SPR_TBL_W = 284, SPR_TBU_W = 285, SPR_PVR = 287,
    SPR_IBAT0U = 528, SPR_DBAT0U = 536, SPR_GQR0 = 912, SPR_HID2 = 920, SPR_WPAR = 921, SPR_DMAU = 922,
    SPR_DMAL = 923, SPR_HID0 = 1008, SPR_HID1 = 1009, SPR_L2CR = 1017, SPR_TBL_R = 268, SPR_TBU_R = 269,
};

// One floating-point register: ps0 is the scalar double; ps1 is the second paired-single lane (held as a double).
typedef struct { double ps0, ps1; } FPR;

typedef struct Gekko {
    u32 gpr[32];
    FPR fpr[32];
    u32 cr, xer, lr, ctr, msr, fpscr;
    u32 pc;
    u32 sr[16];
    u32 spr[1024];  // backing store for SPRs without dedicated fields (GQRs, HIDs, BATs, SRR0/1, SPRGs, ...)

    // Timing. One instruction = one cycle (as Dolphin's interpreter counts). Timebase and decrementer tick once
    // per 12 CPU cycles (bus clock / 4 with a 486 MHz core and 162 MHz bus).
    u64 cycles;      // CPU cycles executed since reset
    s64 downcount;   // cycles left in the current gekko_run slice
    u64 tbOffset;    // TB = cycles / 12 + tbOffset (adjusted when software writes TBL/TBU)
    u64 decZeroAt;   // cycles value at which DEC reaches 0 (decrementer exception when it goes negative)

    // Interrupt inputs, driven by the system.
    bool extIrq;     // external interrupt line from the processor interface (PI)

    const GekkoBus* bus;
    void* priv;      // core-private data (decode tables, BAT lookup, etc.)
} Gekko;

// MSR bits.
#define MSR_EE (1u << 15)
#define MSR_PR (1u << 14)
#define MSR_FP (1u << 13)
#define MSR_IP (1u << 6)
#define MSR_IR (1u << 5)
#define MSR_DR (1u << 4)
#define MSR_RI (1u << 1)

// Exception vector offsets.
enum {
    EXC_RESET = 0x100, EXC_MACHINE_CHECK = 0x200, EXC_DSI = 0x300, EXC_ISI = 0x400, EXC_EXTERNAL = 0x500,
    EXC_ALIGNMENT = 0x600, EXC_PROGRAM = 0x700, EXC_FP_UNAVAILABLE = 0x800, EXC_DECREMENTER = 0x900,
    EXC_SYSCALL = 0xC00, EXC_PERF = 0xF00,
};

// ---- API used by the system ----
bool gekko_init(Gekko* g, const GekkoBus* bus);  // allocates priv; false on failure
void gekko_free(Gekko* g);
void gekko_reset(Gekko* g);                         // power-on state (MSR, HID, BATs cleared, PC = 0xFFF00100)
// Runs until `cycles` cycles have been executed (or gekko_stop is called) and returns the number executed.
u32 gekko_run(Gekko* g, u32 cycles);
void gekko_stop(Gekko* g);                          // ends the current gekko_run slice early (callable from bus code)
void gekko_set_external_irq(Gekko* g, bool level);
u32 gekko_get_spr(Gekko* g, u32 spr);               // as mfspr would read it (TB, DEC computed)
void gekko_set_spr(Gekko* g, u32 spr, u32 v);       // as mtspr would write it (BAT tables rebuilt, DEC rearmed)
void gekko_set_msr(Gekko* g, u32 msr);
// Virtual-to-physical translation as the data side sees it; false if no BAT matches (with translation on).
bool gekko_translate_data(Gekko* g, u32 ea, u32* pa);
// Debug: disassemble one instruction into buf (for traces and error messages).
void gekko_disasm(u32 pc, u32 inst, char* buf, size_t len);

// ---- Hooks the system provides (called by the CPU core) ----
// Called when the CPU executes an instruction it does not implement or hits an unrecoverable condition; the system
// logs and stops. Returning normally continues with a program exception.
void gekko_on_fatal(Gekko* g, const char* what);
