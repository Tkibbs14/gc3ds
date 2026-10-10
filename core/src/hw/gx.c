// Graphics: command processor (CP) and pixel engine (PE) registers, and the write-gather FIFO input.
// Milestone 1 only accepts and counts FIFO data; command parsing and rendering come next.
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

u16 pe_read16(System* s, u32 off) { return off < 0x100 ? s->gx.peRegs[off / 2] : 0; }

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

void gp_fifo_write(System* s, const u8* data, u32 len) {
    // Mirror the bytes into the CPU-side FIFO in RAM, as the write-gather pipe does, and advance the pointer.
    PIState* pi = &s->pi;
    u32 wp = pi->fifoWrite & 0x03FFFFFF, wrap = pi->fifoWrite & (1u << 29);
    for (u32 i = 0; i < len; i++) {
        if (wp < PA_MEM1_END) s->mem1[wp] = data[i];
        wp++;
        if (pi->fifoEnd && wp >= pi->fifoEnd) { wp = pi->fifoBase; wrap = 1u << 29; }
    }
    pi->fifoWrite = wp | wrap;
    s->gx.bytesReceived += len;
    gxcore_feed(s, &s->gx.core, data, len);
}
