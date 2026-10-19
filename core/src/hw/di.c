// Disc interface (DI): the drive's command registers, DMA into RAM and its interrupts.
//
// Registers (YAGCD 5.7): 0x00 DISR, 0x04 DICVR (cover), 0x08-0x10 command buffer, 0x14 DMA address, 0x18 DMA length,
// 0x1C DICR (bit 0 start, 1 DMA, 2 write), 0x20 immediate data, 0x24 configuration. A command completes after a delay
// with TCINT (success) or DEINT (drive error, details through REQUEST ERROR); a DMA leaves the address advanced by
// the length and the length at 0. Command set and error codes: YAGCD 5.7 and Dolphin's DVDInterface (read for
// reference). Timing: 300 us per command, 600 us plus transfer time per read (Dolphin's latency constants; the
// transfer rate is a fixed approximation of the drive, not its CAV model).
#include <stdio.h>
#include "../sys/system.h"
#include "../disc/disc.h"

enum { DI_SR = 0x00, DI_CVR = 0x04, DI_CMD0 = 0x08, DI_CMD1 = 0x0C, DI_CMD2 = 0x10, DI_MAR = 0x14, DI_LEN = 0x18,
       DI_CR = 0x1C, DI_IMM = 0x20, DI_CFG = 0x24 };
enum { SR_BRK = 1 << 0, SR_DEINTMASK = 1 << 1, SR_DEINT = 1 << 2, SR_TCINTMASK = 1 << 3, SR_TCINT = 1 << 4,
       SR_BRKINTMASK = 1 << 5, SR_BRKINT = 1 << 6 };
enum { CVR_OPEN = 1 << 0, CVR_INTMASK = 1 << 1, CVR_INT = 1 << 2 };
enum { CR_TSTART = 1 << 0, CR_DMA = 1 << 1, CR_WRITE = 1 << 2 };

// Drive state as REQUEST ERROR reports it (upper byte), and error codes (lower 24 bits).
enum { DRIVE_READY, DRIVE_READY_NO_READS, DRIVE_COVER_OPEN, DRIVE_DISC_CHANGED, DRIVE_NO_DISC, DRIVE_MOTOR_STOPPED,
       DRIVE_ID_NOT_READ };
enum { ERR_NONE = 0, ERR_MOTOR_STOPPED = 0x020400, ERR_NO_DISC_ID = 0x020401, ERR_NO_MEDIUM = 0x023A00,
       ERR_INVALID_CMD = 0x052000, ERR_NO_AUDIO_BUF = 0x052001, ERR_BLOCK_OOB = 0x052100,
       ERR_INVALID_AUDIO_CMD = 0x052401, ERR_INVALID_PERIOD = 0x052402 };

#define US(n) ((u64)(n) * (GEKKO_HZ / 1000000u))
#define READ_BYTES_PER_SEC (8u * 1024 * 1024)
#define GC_DISC_END 0x57058000u

static void update_irq(System* s) {
    u32 sr = s->di.sr, cv = s->di.cvr;
    bool irq = ((sr & SR_DEINT) && (sr & SR_DEINTMASK)) || ((sr & SR_TCINT) && (sr & SR_TCINTMASK)) ||
               ((sr & SR_BRKINT) && (sr & SR_BRKINTMASK)) || ((cv & CVR_INT) && (cv & CVR_INTMASK));
    if (irq) pi_raise(s, PI_INT_DI); else pi_clear(s, PI_INT_DI);
}

void di_reset(System* s) {
    memset(&s->di, 0, sizeof s->di);
    s->di.cfg = 1;  // bit 0: boot ROM descrambler disabled, as on every retail console after the IPL
    // Lid closed; with no disc inside, every command fails with "medium not present".
    s->di.cvr = 0;
    s->di.state = s->disc ? DRIVE_ID_NOT_READ : DRIVE_NO_DISC;
}

// The OS pulses the DI bit of the PI reset register to reset the drive; the disc stays spun up, its ID unread.
void di_reset_drive(System* s) {
    emu_log(LOG_DI, "drive reset\n");
    if (s->disc) s->di.state = DRIVE_ID_NOT_READ;
}

void di_complete_event(System* s, u64 now, u32 arg) {
    (void)now; (void)arg;
    DIState* d = &s->di;
    if (!d->busy) return;
    d->busy = false;
    d->cr &= ~CR_TSTART;
    d->sr |= d->pendingSr;
    if (d->pendingSr == SR_TCINT && (d->cr & CR_DMA)) { d->mar += d->len; d->len = 0; }
    update_irq(s);
}

static void finish(System* s, u32 error, u64 delay) {
    DIState* d = &s->di;
    d->error = error;
    d->pendingSr = error ? SR_DEINT : SR_TCINT;
    d->busy = true;
    sched_remove(s, di_complete_event);
    sched_add(s, sys_now(s) + delay, di_complete_event, 0);
}

static u8* dma_target(System* s, u32 len) {
    u32 pa = s->di.mar & 0x03FFFFE0;
    return pa + len <= PA_MEM1_END ? s->mem1 + pa : NULL;
}

static void cmd_read(System* s, u64 offset, u32 len, bool discId) {
    DIState* d = &s->di;
    if (!s->disc) { finish(s, ERR_NO_MEDIUM, US(300)); return; }
    if (d->state == DRIVE_MOTOR_STOPPED) { finish(s, ERR_MOTOR_STOPPED, US(300)); return; }
    if (offset + len > GC_DISC_END) { finish(s, ERR_BLOCK_OOB, US(300)); return; }
    u8* dst = dma_target(s, len);
    if (!dst || !(d->cr & CR_DMA)) {
        emu_log(LOG_DI, "read to %08x+%x not supported (cr %x)\n", d->mar, len, d->cr);
        finish(s, ERR_INVALID_CMD, US(300));
        return;
    }
    // The data lands at once; software only looks at it after the transfer-complete interrupt.
    if (!disc_read(s->disc, offset, dst, len)) emu_error("disc read failed at %llx\n", (unsigned long long)offset);
    ram_fix_be(s->mem1, (u32)(dst - s->mem1), len);  // (the disc's bytes into the RAM layout)
    mem_dirty((u32)(dst - s->mem1), len);
    dma_note((u32)(dst - s->mem1), len);
    if (discId) d->state = d->state == DRIVE_ID_NOT_READ ? DRIVE_READY_NO_READS : DRIVE_READY;
    else if (d->state == DRIVE_READY_NO_READS) d->state = DRIVE_READY;
    d->reads++;
    d->bytesRead += len;
    emu_log(LOG_DI, "read %09llx+%x -> %08x\n", (unsigned long long)offset, len, d->mar);
    finish(s, ERR_NONE, US(600) + (u64)len * GEKKO_HZ / READ_BYTES_PER_SEC);
}

static void execute(System* s) {
    DIState* d = &s->di;
    u32 c0 = d->cmd[0], op = c0 >> 24;
    if (op != 0xA8) emu_log(LOG_DI, "command %08x %08x %08x mar %08x len %x cr %x\n", c0, d->cmd[1], d->cmd[2], d->mar, d->len, d->cr);
    switch (op) {
    case 0xA8:
        if ((c0 & 0xFF) == 0x40) cmd_read(s, 0, d->len < 0x20 ? d->len : 0x20, true);  // READ DISC ID
        else cmd_read(s, (u64)d->cmd[1] << 2, d->cmd[2], false);
        return;
    case 0x12: {  // INQUIRY: drive revision, date code, version
        u8* p = dma_target(s, 0x20);
        if (p) {
            static const u8 info[12] = { 0, 0, 0, 2, 0x20, 0x06, 0x05, 0x26, 0x41, 0, 0, 0 };
            memset(p, 0, 0x20);
            ram_from_be(s->mem1, (u32)(p - s->mem1), info, sizeof info);  // (into the RAM layout)
        }
        finish(s, ERR_NONE, US(300));
        return;
    }
    case 0xAB: finish(s, ERR_NONE, US(300)); return;  // SEEK
    case 0xE0:  // REQUEST ERROR: state in the top byte, error code below; reading it clears the error
        d->imm = ((u32)(d->state == DRIVE_READY ? 0 : d->state - 1) << 24) | d->error;
        finish(s, ERR_NONE, US(300));
        return;
    case 0xE1: finish(s, d->audioEnabled ? ERR_NONE : ERR_NO_AUDIO_BUF, US(300)); return;  // AUDIO STREAM (no tracks)
    case 0xE2: d->imm = 0; finish(s, ERR_NONE, US(300)); return;                           // AUDIO STATUS: not playing
    case 0xE3: d->state = DRIVE_MOTOR_STOPPED; finish(s, ERR_NONE, US(300)); return;      // STOP MOTOR
    case 0xE4:  // AUDIO BUFFER CONFIG: only before the first read after the disc ID
        if (d->state == DRIVE_READY) { finish(s, ERR_INVALID_PERIOD, US(300)); return; }
        d->audioEnabled = (c0 >> 16) & 1;
        finish(s, ERR_NONE, US(300));
        return;
    default:
        emu_log(LOG_DI, "unsupported command %08x\n", c0);
        finish(s, ERR_INVALID_CMD, US(300));
        return;
    }
}

u32 di_read32(System* s, u32 off) {
    DIState* d = &s->di;
    switch (off) {
    case DI_SR: return d->sr;
    case DI_CVR: return d->cvr;
    case DI_CMD0: case DI_CMD1: case DI_CMD2: return d->cmd[(off - DI_CMD0) / 4];
    case DI_MAR: return d->mar;
    case DI_LEN: return d->len;
    case DI_CR: return d->cr;
    case DI_IMM: return d->imm;
    case DI_CFG: return d->cfg;
    }
    emu_log(LOG_DI, "read of unknown register %02x\n", off);
    return 0;
}

void di_write32(System* s, u32 off, u32 v) {
    DIState* d = &s->di;
    switch (off) {
    case DI_SR: {
        u32 w1c = SR_DEINT | SR_TCINT | SR_BRKINT;
        d->sr = (d->sr & w1c & ~(v & w1c)) | (v & (SR_DEINTMASK | SR_TCINTMASK | SR_BRKINTMASK));
        if ((v & SR_BRK) && d->busy) {  // break: abandon the running command
            sched_remove(s, di_complete_event);
            d->busy = false;
            d->cr &= ~CR_TSTART;
            d->sr |= SR_BRKINT;
            emu_log(LOG_DI, "break\n");
        }
        break;
    }
    case DI_CVR: d->cvr = (d->cvr & CVR_OPEN) | (d->cvr & CVR_INT & ~(v & CVR_INT)) | (v & CVR_INTMASK); break;
    case DI_CMD0: case DI_CMD1: case DI_CMD2: d->cmd[(off - DI_CMD0) / 4] = v; break;
    case DI_MAR: d->mar = v & 0x03FFFFE0; break;
    case DI_LEN: d->len = v & ~0x1Fu; break;
    case DI_CR:
        d->cr = v & 7;
        if (v & CR_TSTART) {
            if (d->busy) emu_log(LOG_DI, "command started while busy\n");
            execute(s);
        }
        break;
    case DI_IMM: d->imm = v; break;
    case DI_CFG: break;
    default: emu_log(LOG_DI, "write of unknown register %02x = %08x\n", off, v); break;
    }
    update_irq(s);
}
