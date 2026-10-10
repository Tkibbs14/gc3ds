#pragma once
// The emulated GameCube: physical memory, hardware blocks, the event scheduler and the run loop around the CPU.
#include "../common.h"
#include "../cpu/gekko.h"
#include "../gx/gx.h"

#define MEM1_SIZE (24u * 1024 * 1024)
#define LOCKED_CACHE_SIZE (16u * 1024)
#define GEKKO_HZ 486000000u
#define BUS_HZ 162000000u

// Physical address map (hardware registers are reached at effective 0xCC000000 through the uncached BAT).
#define PA_MEM1_END 0x01800000u
#define PA_EFB 0x08000000u
#define PA_MMIO 0x0C000000u
#define PA_GP_FIFO 0x0C008000u
#define PA_LOCKED_CACHE 0xE0000000u

// Processor-interface interrupt causes (PI INTSR / INTMR bits).
enum {
    PI_INT_GPERR = 1 << 0, PI_INT_RSW = 1 << 1, PI_INT_DI = 1 << 2, PI_INT_SI = 1 << 3, PI_INT_EXI = 1 << 4,
    PI_INT_AI = 1 << 5, PI_INT_DSP = 1 << 6, PI_INT_MEM = 1 << 7, PI_INT_VI = 1 << 8, PI_INT_PE_TOKEN = 1 << 9,
    PI_INT_PE_FINISH = 1 << 10, PI_INT_CP = 1 << 11, PI_INT_DEBUG = 1 << 12, PI_INT_HSP = 1 << 13,
};

// ---- scheduler: events fire at absolute CPU cycle counts ----
typedef struct System System;
typedef void (*EventFn)(System* s, u64 now, u32 arg);
#define MAX_EVENTS 32
typedef struct { u64 when; EventFn fn; u32 arg; bool used; } Event;

typedef struct {
    u32 intsr, intmr;
    u32 fifoBase, fifoEnd, fifoWrite;  // CPU-side GP FIFO (0x0C, 0x10, 0x14)
    u32 resetCode;
    bool resetWritten;
} PIState;

typedef struct {
    u16 regs[0x40];            // 0x80 bytes of 16-bit registers
    u32 line;                  // current half-frame line (1-based, VCT)
    u64 fieldStart;            // cycle count at the start of the current field
    u32 field;                 // field counter
    u32 xfbLatched;            // XFB physical address latched at the start of the field
} VIState;

typedef struct {
    u32 csr, mar, len, cr, data;
    u32 devCmd[3];             // per-device command state (first word of a transfer)
    u32 devPos[3];             // per-device byte position within the current selection
    bool devSelected[3];
} EXIChannel;

typedef struct {
    EXIChannel ch[3];
    u8 sram[64];
    u32 rtc;
} EXIState;

typedef struct {
    u32 outbuf[4], inbufH[4], inbufL[4];
    u32 poll, comcsr, sr, exilk;
    u8 buf[128];
    u16 buttons[4];            // host input, GameCube pad bit layout
    u8 stick[4][6];            // main X/Y, C X/Y, L, R
} SIState;

// High-level emulation of the DSP's microcode (src/hw/dsp_hle.c): which program runs and its mail state.
#define DSP_MAILQ 64
typedef struct {
    u32 q[DSP_MAILQ];                 // DSP -> CPU mails not yet read
    bool qIrq[DSP_MAILQ];             // raise the DSP interrupt when this mail becomes readable
    u32 qHead, qCount;
    u32 lastMail;                     // what the CPU-side mailbox shows when the queue is empty
    int ucode;                        // DspUcode
    u32 ucodeHash;                    // Ector hash of the uploaded IRAM image (as Dolphin's ucode table keys it)
    u32 romParam;                     // ROM boot loader: header mail awaiting its parameter
    u32 bootRam, bootIram, bootLen, bootDmem, bootPc;
    // Zelda-family microcode (Twilight Princess, Wind Waker, Sunshine, Pikmin...)
    int zState;                       // ZeldaMailState
    u32 zExpect;                      // words still to come for the command being written
    u32 zCmd[256], zCmdRd, zCmdWr;    // command ring buffer (words)
    u32 zPending;                     // complete commands not yet executed
    bool zCanExec;
    u32 zVoicesPerFrame, zFramesReq, zFrame, zVoice, zSyncMax;
    bool zRendering;
    u32 uploadStep, upload[10];       // MAIL_NEW_UCODE: ten words describing the next microcode
    bool uploading;
    u32 mailsIn, mailsOut;            // statistics
} DSPHle;

typedef struct {
    u16 mailboxCpuHi, mailboxCpuLo;   // CPU -> DSP
    u16 cr;                           // DSPCR
    u32 aramDmaMm, aramDmaAr, aramDmaCnt;
    u32 aiDmaStart, aiDmaLen;
    u16 aiDmaBlocksLeft;
    u16 aramSize;
    u8* aram;                         // 16 MB auxiliary RAM
    DSPHle hle;
} DSPState;

typedef struct {
    u32 cr, vr, sampleCounter, interruptTiming;
    u64 counterBase;
} AIState;

typedef struct {
    u32 sr, cvr, cmd[3], mar, len, cr, imm, cfg;
    int state;                 // drive state (DriveState in di.c)
    u32 error;                 // last error, returned by REQUEST ERROR
    bool busy;
    u32 pendingSr;             // status bit (TCINT/DEINT) the running command completes with
    bool audioEnabled;
    u64 reads, bytesRead;
} DIState;

typedef struct {
    u16 cpRegs[0x40];
    u16 peRegs[0x80];
    u64 bytesReceived;
    GXCore core;           // command stream, register files, EFB
} GXState;

struct System {
    Gekko cpu;
    GekkoBus bus;
    u8* mem1;
    u8* lockedCache;
    PIState pi;
    VIState vi;
    EXIState exi;
    SIState si;
    DSPState dsp;
    AIState ai;
    DIState di;
    GXState gx;
    struct Disc* disc;         // the disc in the drive, or NULL (owned by the frontend)
    u32 fixedRtc;              // real-time clock at reset, seconds since 2000 (0: the host clock)
    void (*on_efb_copy)(System* s, void* user, u32 ctl);  // debug: called before each EFB copy executes
    Event events[MAX_EVENTS];
    u64 nextEvent;
    // Frontend hooks.
    void (*on_field)(System* s, void* user);  // called at each VI field start (present a frame)
    void* user;
    bool stopRequested;
    u32 unknownMmioCount;
    // icbi: code at physical pa may have changed (the JIT drops compiled blocks there).
    void (*icbiHook)(void* ctx, u32 pa);
    void* icbiCtx;
};

bool sys_init(System* s);
void sys_free(System* s);
// Development save states (savestate.c): only valid for the build that wrote them. Call between sys_run calls.
bool sys_save_state(System* s, const char* path);
bool sys_load_state(System* s, const char* path);
void sys_reset(System* s);
// Runs the machine until `cycles` CPU cycles have elapsed or a stop is requested.
void sys_run(System* s, u64 cycles);
void sys_stop(System* s, const char* why);

void sched_add(System* s, u64 when, EventFn fn, u32 arg);
void sched_remove(System* s, EventFn fn);
static inline u64 sys_now(System* s) { return s->cpu.cycles; }

// Interrupts.
void pi_raise(System* s, u32 cause);
void pi_clear(System* s, u32 cause);
void pi_update(System* s);

// Hardware blocks: register access (offset within the block), reset, and event setup.
u32 pi_read32(System* s, u32 off);   void pi_write32(System* s, u32 off, u32 v);
u16 vi_read16(System* s, u32 off);   void vi_write16(System* s, u32 off, u16 v);
u32 exi_read32(System* s, u32 off);  void exi_write32(System* s, u32 off, u32 v);
u32 si_read32(System* s, u32 off);   void si_write32(System* s, u32 off, u32 v);
u16 dsp_read16(System* s, u32 off);  void dsp_write16(System* s, u32 off, u16 v);
u32 ai_read32(System* s, u32 off);   void ai_write32(System* s, u32 off, u32 v);
u32 di_read32(System* s, u32 off);   void di_write32(System* s, u32 off, u32 v);
u16 cp_read16(System* s, u32 off);   void cp_write16(System* s, u32 off, u16 v);
u16 pe_read16(System* s, u32 off);   void pe_write16(System* s, u32 off, u16 v);
u16 mi_read16(System* s, u32 off);   void mi_write16(System* s, u32 off, u16 v);
void gp_fifo_write(System* s, const u8* data, u32 len);  // bytes in console (big-endian) order

void vi_reset(System* s); void exi_reset(System* s); void si_reset(System* s); void dsp_reset(System* s);
void ai_reset(System* s); void di_reset(System* s); void gx_reset(System* s);
void di_reset_drive(System* s);  // PI reset-code DI bit pulsed

// DSP microcode HLE (dsp_hle.c), driven by dsp.c.
enum { DSP_UC_NONE, DSP_UC_ROM, DSP_UC_ZELDA, DSP_UC_UNKNOWN };
void dsphle_reset(System* s);                  // DSP reset: the boot ROM runs and greets the CPU
void dsphle_cpu_mail(System* s, u32 mail);     // a complete CPU -> DSP mail
bool dsphle_mail_pending(System* s);
u32 dsphle_peek_mail(System* s);               // DSP -> CPU mailbox as the CPU sees it (bit 31 = unread)
u32 dsphle_pop_mail(System* s);                // the CPU read the low half
void dsp_raise_int(System* s);                 // the DSP sets DSPCR.DSPINT

// VI helpers for the frontend.
// Top/bottom field framebuffer addresses, pixel width, frame height (both fields) and field line stride in bytes.
bool vi_get_xfb(System* s, u32* top, u32* bottom, u32* width, u32* height, u32* stride);

// Memory helpers (physical, big-endian RAM).
static inline u32 mem1_read32(System* s, u32 pa) { u32 v; __builtin_memcpy(&v, s->mem1 + pa, 4); return bswap32(v); }
static inline void mem1_write32(System* s, u32 pa, u32 v) { v = bswap32(v); __builtin_memcpy(s->mem1 + pa, &v, 4); }
