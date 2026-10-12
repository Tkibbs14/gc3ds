// DSP interface: mailboxes, control register, ARAM DMA and audio DMA. The DSP itself is not executed: its boot ROM
// and microcode are emulated at the mail level in dsp_hle.c.
#include "../sys/system.h"

enum {
    DSP_MBOX_IN_H = 0x00, DSP_MBOX_IN_L = 0x02, DSP_MBOX_OUT_H = 0x04, DSP_MBOX_OUT_L = 0x06, DSP_CR = 0x0A,
    AR_SIZE = 0x12, AR_MODE = 0x16, AR_REFRESH = 0x1A, AR_MM_H = 0x20, AR_MM_L = 0x22, AR_AR_H = 0x24,
    AR_AR_L = 0x26, AR_CNT_H = 0x28, AR_CNT_L = 0x2A, AID_START_H = 0x30, AID_START_L = 0x32, AID_LEN = 0x36,
    AID_LEFT = 0x3A,
};
enum {
    CR_RES = 1 << 0, CR_PIINT = 1 << 1, CR_HALT = 1 << 2, CR_AIDINT = 1 << 3, CR_AIDINTMSK = 1 << 4,
    CR_ARINT = 1 << 5, CR_ARINTMSK = 1 << 6, CR_DSPINT = 1 << 7, CR_DSPINTMSK = 1 << 8, CR_DSPDMA = 1 << 9,
    CR_DSPRESET = 1 << 11,
};
#define CR_W1C (CR_AIDINT | CR_ARINT | CR_DSPINT)
#define ARAM_SIZE (16u * 1024 * 1024)

static void update_irq(System* s) {
    u16 c = s->dsp.cr;
    bool any = ((c & CR_AIDINT) && (c & CR_AIDINTMSK)) || ((c & CR_ARINT) && (c & CR_ARINTMSK)) ||
               ((c & CR_DSPINT) && (c & CR_DSPINTMSK));
    if (any) pi_raise(s, PI_INT_DSP); else pi_clear(s, PI_INT_DSP);
}

static void aram_dma(System* s) {
    DSPState* d = &s->dsp;
    u32 mm = d->aramDmaMm & 0x03FFFFE0, ar = d->aramDmaAr & 0x00FFFFE0, cnt = d->aramDmaCnt & 0x03FFFFE0;
    bool toMm = d->aramDmaCnt & 0x80000000u;
    if (d->aram && mm + cnt <= PA_MEM1_END && ar + cnt <= ARAM_SIZE) {  // no ARAM: the JIT fuzzer's machines
        if (toMm) memcpy(s->mem1 + mm, d->aram + ar, cnt);
        else memcpy(d->aram + ar, s->mem1 + mm, cnt);
    } else {
        emu_log(LOG_DSP, "ARAM DMA out of range mm=%08x ar=%08x cnt=%08x\n", mm, ar, cnt);
    }
    d->aramDmaCnt &= 0x80000000u;  // count reads back as 0 when done
    d->cr |= CR_ARINT;
    update_irq(s);
}

// Audio DMA: plays aiDmaLen 32-byte blocks from aiDmaStart at 32 kHz stereo 16-bit (8 samples per block), then
// raises AIDINT and restarts with the latched values, as the hardware does.
void dsp_aid_event(System* s, u64 now, u32 arg) {
    (void)arg;
    DSPState* d = &s->dsp;
    if (!(d->aiDmaLen & 0x8000)) return;
    d->cr |= CR_AIDINT;
    update_irq(s);
    u32 blocks = d->aiDmaLen & 0x7FFF;
    d->aiDmaBlocksLeft = (u16)blocks;
    u64 cycles = (u64)blocks * 8 * GEKKO_HZ / 32000;
    sched_add(s, now + (cycles ? cycles : 1000), dsp_aid_event, 0);
}

void dsp_reset(System* s) {
    u8* aram = s->dsp.aram;
    memset(&s->dsp, 0, sizeof s->dsp);
    s->dsp.aram = aram;
    s->dsp.cr = CR_DSPRESET;
}

void dsp_raise_int(System* s) {
    s->dsp.cr |= CR_DSPINT;
    update_irq(s);
}

u16 dsp_read16(System* s, u32 off) {
    DSPState* d = &s->dsp;
    switch (off) {
    case DSP_MBOX_IN_H: return d->mailboxCpuHi;
    case DSP_MBOX_IN_L: return d->mailboxCpuLo;
    case DSP_MBOX_OUT_H: return (u16)(dsphle_peek_mail(s) >> 16);
    case DSP_MBOX_OUT_L: return (u16)dsphle_pop_mail(s);  // reading the low half takes the mail
    case DSP_CR: return d->cr;
    case AR_SIZE: return d->aramSize;
    case AR_MODE: return 1;     // ARAM initialised
    case AR_REFRESH: return 0;
    case AR_MM_H: return (u16)(d->aramDmaMm >> 16);
    case AR_MM_L: return (u16)d->aramDmaMm;
    case AR_AR_H: return (u16)(d->aramDmaAr >> 16);
    case AR_AR_L: return (u16)d->aramDmaAr;
    case AR_CNT_H: return (u16)(d->aramDmaCnt >> 16);
    case AR_CNT_L: return (u16)d->aramDmaCnt;
    case AID_START_H: return (u16)(d->aiDmaStart >> 16);
    case AID_START_L: return (u16)d->aiDmaStart;
    case AID_LEN: return (u16)d->aiDmaLen;
    case AID_LEFT: return d->aiDmaBlocksLeft;
    }
    emu_log(LOG_DSP, "DSP read %03x\n", off);
    return 0;
}

void dsp_write16(System* s, u32 off, u16 v) {
    DSPState* d = &s->dsp;
    switch (off) {
    case DSP_MBOX_IN_H: d->mailboxCpuHi = v; break;
    case DSP_MBOX_IN_L:
        d->mailboxCpuLo = v;
        // The DSP takes the mail at once; clearing bit 15 tells the CPU the mailbox is free again.
        dsphle_cpu_mail(s, ((u32)d->mailboxCpuHi << 16) | v);
        d->mailboxCpuHi &= 0x7FFF;
        break;
    case DSP_CR: {
        u16 keep = d->cr & CR_W1C & ~(v & CR_W1C);
        bool wasHalted = d->cr & CR_HALT;
        d->cr = (u16)(keep | (v & ~CR_W1C & ~CR_RES));
        // A reset boots the ROM, which mails 0x8071FEED to say it is ready for microcode. Leaving halt with no
        // microcode loaded does the same (the OS's audio init and libogc's DSP bootstrap wait for that mail).
        if (v & CR_RES) { dsphle_reset(s); d->cr &= ~CR_DSPRESET; }
        else if (wasHalted && !(d->cr & CR_HALT) && d->hle.ucode <= DSP_UC_ROM) dsphle_reset(s);
        update_irq(s);
        break;
    }
    case AR_SIZE: d->aramSize = v; break;
    case AR_MODE: case AR_REFRESH: break;
    case AR_MM_H: d->aramDmaMm = (d->aramDmaMm & 0xFFFF) | ((u32)v << 16); break;
    case AR_MM_L: d->aramDmaMm = (d->aramDmaMm & 0xFFFF0000u) | v; break;
    case AR_AR_H: d->aramDmaAr = (d->aramDmaAr & 0xFFFF) | ((u32)v << 16); break;
    case AR_AR_L: d->aramDmaAr = (d->aramDmaAr & 0xFFFF0000u) | v; break;
    case AR_CNT_H: d->aramDmaCnt = (d->aramDmaCnt & 0xFFFF) | ((u32)v << 16); break;
    case AR_CNT_L: d->aramDmaCnt = (d->aramDmaCnt & 0xFFFF0000u) | v; aram_dma(s); break;
    case AID_START_H: d->aiDmaStart = (d->aiDmaStart & 0xFFFF) | ((u32)v << 16); break;
    case AID_START_L: d->aiDmaStart = (d->aiDmaStart & 0xFFFF0000u) | v; break;
    case AID_LEN: {
        bool was = d->aiDmaLen & 0x8000;
        d->aiDmaLen = v;
        if ((v & 0x8000) && !was) { sched_remove(s, dsp_aid_event); dsp_aid_event(s, sys_now(s), 0); }
        else if (!(v & 0x8000)) sched_remove(s, dsp_aid_event);
        break;
    }
    default: emu_log(LOG_DSP, "DSP write %03x = %04x\n", off, v); break;
    }
}

// ---- audio interface: streaming sample counter and its interrupt ----
enum { AI_CR = 0x00, AI_VR = 0x04, AI_SCNT = 0x08, AI_IT = 0x0C };
enum { AICR_PSTAT = 1 << 0, AICR_AFR = 1 << 1, AICR_AIINTMSK = 1 << 2, AICR_AIINT = 1 << 3, AICR_AIINTVLD = 1 << 4,
       AICR_SCRESET = 1 << 5 };

static u32 ai_rate(System* s) { return (s->ai.cr & AICR_AFR) ? 48000 : 32000; }

static u32 ai_counter(System* s) {
    if (!(s->ai.cr & AICR_PSTAT)) return s->ai.sampleCounter;
    return s->ai.sampleCounter + (u32)((sys_now(s) - s->ai.counterBase) * ai_rate(s) / GEKKO_HZ);
}

void ai_reset(System* s) { memset(&s->ai, 0, sizeof s->ai); }

u32 ai_read32(System* s, u32 off) {
    switch (off) {
    case AI_CR: return s->ai.cr;
    case AI_VR: return s->ai.vr;
    case AI_SCNT: return ai_counter(s);
    case AI_IT: return s->ai.interruptTiming;
    }
    return 0;
}

void ai_write32(System* s, u32 off, u32 v) {
    switch (off) {
    case AI_CR: {
        u32 count = ai_counter(s);
        if (v & AICR_SCRESET) count = 0;
        s->ai.sampleCounter = count;
        s->ai.counterBase = sys_now(s);
        u32 keep = s->ai.cr & AICR_AIINT & ~(v & AICR_AIINT);
        s->ai.cr = keep | (v & ~(AICR_AIINT | AICR_SCRESET));
        if ((s->ai.cr & AICR_AIINT) && (s->ai.cr & AICR_AIINTMSK)) pi_raise(s, PI_INT_AI); else pi_clear(s, PI_INT_AI);
        break;
    }
    case AI_VR: s->ai.vr = v; break;
    case AI_SCNT: break;
    case AI_IT: s->ai.interruptTiming = v; break;
    }
}
