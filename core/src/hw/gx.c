// Graphics: command processor (CP) and pixel engine (PE) registers, and the write-gather FIFO input.
// Milestone 1 only accepts and counts FIFO data; command parsing and rendering come next.
#include <string.h>
#include "../sys/system.h"

void gx_reset(System* s) {
    GXCore core = s->gx.core;  // keep its allocations across resets
    memset(&s->gx, 0, sizeof s->gx);
    s->gx.core = core;
    gxcore_init(&s->gx.core);
}

u16 cp_read16(System* s, u32 off) {
    if (off == 0x00) return 0x0C;  // status: GP read idle, command idle
    // The GP executes FIFO data as it arrives, so its FIFO is always drained: the read and write pointers both sit
    // at the CPU write pointer and the read-write distance is 0 (CP 0x30 distance, 0x34 write, 0x38 read pointer;
    // low half first).
    u32 wp = s->pi.fifoWrite & 0x03FFFFFF;
    switch (off) {
    case 0x30: case 0x32: return 0;
    case 0x34: case 0x38: return (u16)wp;
    case 0x36: case 0x3A: return (u16)(wp >> 16);
    }
    return off < 0x80 ? s->gx.cpRegs[off / 2] : 0;
}

void cp_write16(System* s, u32 off, u16 v) {
    if (off < 0x80) s->gx.cpRegs[off / 2] = v;
}

// g_peReadSync false (the default; --pesync on for the old wait): a PE register read takes the signals the GX thread
// has posted so far instead of waiting for it to execute everything fed (the hardware does not wait either; emulated
// timing then depends on the GX thread's progress). The interrupt handlers read the status right after a finish
// interrupt: 2DS jobs 354-355, cutscene 29.10 -> 30.03%, core 0's GX wait 7.4 -> 4.1%. (Inline GX, the PC: no wait.)
bool g_peReadSync = false;
u16 pe_read16(System* s, u32 off) {
    if (s->gxAsync) {  // tokens and finish flags of everything sent so far
        if (g_peReadSync) s->gxAsync->sync(s);
        else s->gxAsync->poll(s);
    }
    return off < 0x100 ? s->gx.peRegs[off / 2] : 0;
}

void pe_write16(System* s, u32 off, u16 v) {
    if (off >= 0x100) return;
    if (off == 0x0A) {  // interrupt control: bits 2/3 (token/finish status) are write-one-to-clear
        u16 old = s->gx.peRegs[off / 2];
        u16 status = old & 0x0C & ~(v & 0x0C);
        s->gx.peRegs[off / 2] = (u16)(status | (v & 0x03));
        if (!(status & 0x04)) pi_clear(s, PI_INT_PE_TOKEN);
        if (!(status & 0x08)) pi_clear(s, PI_INT_PE_FINISH);
        return;
    }
    s->gx.peRegs[off / 2] = v;
}

// One 32-byte burst reaches the FIFO: mirrored into the CPU-side FIFO in RAM, the write pointer advanced, and the
// GP executes it. g_gpBulk (--gpbulk, --ab gpbulk): a burst that neither leaves MEM1 nor wraps the FIFO before its
// last byte is mirrored with one copy, and gp_fifo_write moves whole runs of bytes (the same bytes as the loops).
bool g_gpBulk = true;  // on: heavy area +3.2%, Ordon +4.5% (jobs 477-478); PC state hash unchanged over 400 fields
u64 g_gpLinked, g_gpUnlinked;  // bursts fed to the GP, and held in RAM while the GP is not linked (reports)
static void gp_burst(System* s, const u8* data) {
    PIState* pi = &s->pi;
    u32 wp = pi->fifoWrite & 0x03FFFFFF, wrap = pi->fifoWrite & (1u << 29), start = wp;
    if (g_gpBulk && !GC_SWAPRAM && wp + 32 <= PA_MEM1_END && (!pi->fifoEnd || wp + 32 <= pi->fifoEnd)) {
        memcpy(s->mem1 + wp, data, 32);
        wp += 32;
        if (pi->fifoEnd && wp >= pi->fifoEnd) { wp = pi->fifoBase; wrap = 1u << 29; }
    } else {
        for (u32 i = 0; i < 32; i++) {
            if (wp < PA_MEM1_END) ram_wr8(s->mem1, wp, data[i]);
            wp++;
            if (pi->fifoEnd && wp >= pi->fifoEnd) { wp = pi->fifoBase; wrap = 1u << 29; }
        }
    }
    pi->fifoWrite = wp | wrap;
    s->gx.bytesReceived += 32;
    // CP control bit 4 links the GP to the CPU's FIFO. GXSetCPUFifo clears it when the CPU FIFO is not the GP's, as
    // GXBeginDisplayList does while a list is recorded through the pipe (Mario Party 4's models): the bytes then only
    // go to RAM, and the GP sees them when the list is called. A control register never written (0) counts as linked.
    // Such a list is RAM the GP reads later, written without a cache flush: its page is stamped like a flushed one,
    // so the converted shapes and translated lists made from an earlier recording there are made again.
    u16 cr = s->gx.cpRegs[0x02 / 2];
    if (cr && !(cr & 0x10)) { g_gpUnlinked++; mem_dirty(start, 32); return; }
    g_gpLinked++;
    if (s->gxAsync) s->gxAsync->feed(s, data, 32);
    else gxcore_feed(s, &s->gx.core, data, 32);
}

// CPU stores to the write-gather pipe. As on the hardware, the GP sees them only in whole 32-byte bursts; programs
// end a command batch with GXFlush, which pads the last burst. (Feeding every 1-4 byte store separately cost a
// parse attempt and two clock reads each, tens of thousands per field in Twilight Princess.)
// gp_capture: when set (HLE_CHECK on the PC, hle_ps.c), the CPU's pipe bytes go there instead of to the GP.
void (*gp_capture)(const u8* data, u32 len);
u64 g_gpWrites, g_gpWriteBytes;  // CPU stores into the pipe (reports)
void gp_fifo_write(System* s, const u8* data, u32 len) {
    g_gpWrites++; g_gpWriteBytes += len;
    if (gp_capture) { gp_capture(data, len); return; }
    GXState* gx = &s->gx;
    if (g_gpBulk) {
        while (len) {
            if (!gx->wgpLen && len >= 32) { gp_burst(s, data); data += 32; len -= 32; continue; }
            u32 n = 32 - gx->wgpLen < len ? 32 - gx->wgpLen : len;
            memcpy(gx->wgp + gx->wgpLen, data, n);
            gx->wgpLen += n;
            data += n;
            len -= n;
            if (gx->wgpLen == 32) { gx->wgpLen = 0; gp_burst(s, gx->wgp); }
        }
        return;
    }
    for (u32 i = 0; i < len; i++) {
        gx->wgp[gx->wgpLen++] = data[i];
        if (gx->wgpLen == 32) { gx->wgpLen = 0; gp_burst(s, gx->wgp); }
    }
}

// One CPU store of `size` (1, 2 or 4) bytes to the pipe, the value's low bytes in console order (the JIT's direct path).
void gp_wgp_put(void* ctx, u32 v, u32 size) {
    System* s = ctx;
    GXState* gx = &s->gx;
    if (gx->wgpLen + size < 32) {
        for (u32 i = 0; i < size; i++) gx->wgp[gx->wgpLen++] = (u8)(v >> (8 * (size - 1 - i)));
        return;
    }
    u8 b[4];
    for (u32 i = 0; i < size; i++) b[i] = (u8)(v >> (8 * (size - 1 - i)));
    gp_fifo_write(s, b, size);
}
