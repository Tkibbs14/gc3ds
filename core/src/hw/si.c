// Serial interface: controller ports. Port 0 has a standard controller (state from SIState.buttons/stick), the
// others are empty. Command transfers complete instantly; polled input is refreshed whenever it is read.
#include "../sys/system.h"

enum { SI_POLL = 0x30, SI_COMCSR = 0x34, SI_SR = 0x38, SI_EXILK = 0x3C, SI_BUF = 0x80 };
#define COMCSR_TCINT (1u << 31)
#define COMCSR_TCINTMSK (1u << 30)
#define COMCSR_RDSTINT (1u << 28)
#define COMCSR_RDSTINTMSK (1u << 27)
#define COMCSR_TSTART 1u

static bool port_present(int ch) { return ch == 0; }

static void update_irq(System* s) {
    u32 c = s->si.comcsr;
    bool tc = (c & COMCSR_TCINT) && (c & COMCSR_TCINTMSK);
    bool rd = (c & COMCSR_RDSTINT) && (c & COMCSR_RDSTINTMSK);
    if (tc || rd) pi_raise(s, PI_INT_SI); else pi_clear(s, PI_INT_SI);
}

static void refresh_input(System* s, int ch) {
    const u8* st = s->si.stick[ch];
    // Status word: buttons (with the "use origin" bit 0x0080 set as real controllers report), then main stick.
    s->si.inbufH[ch] = ((u32)(s->si.buttons[ch] | 0x0080) << 16) | ((u32)st[0] << 8) | st[1];
    s->si.inbufL[ch] = ((u32)st[2] << 24) | ((u32)st[3] << 16) | ((u32)st[4] << 8) | st[5];
}

static void command(System* s) {
    int ch = (int)((s->si.comcsr >> 1) & 3);
    u32 outLen = (s->si.comcsr >> 16) & 0x7F, inLen = (s->si.comcsr >> 8) & 0x7F;
    if (!outLen) outLen = 128;
    if (!inLen) inLen = 128;
    u8 cmd = s->si.buf[0];
    u8 resp[128];
    memset(resp, 0, sizeof resp);
    if (!port_present(ch)) {
        s->si.sr |= 1u << (27 - 8 * ch);  // NOREP: no response from this port
    } else {
        switch (cmd) {
        case 0x00:
        case 0xFF:  // reset / get type: standard controller
            resp[0] = 0x09; resp[1] = 0x00; resp[2] = 0x00;
            break;
        case 0x41:
        case 0x42: {  // origin / recalibrate: buttons 0, sticks centred, triggers released
            refresh_input(s, ch);
            const u8* st = s->si.stick[ch];
            resp[0] = 0x00; resp[1] = 0x80;
            for (int i = 0; i < 6; i++) resp[2 + i] = st[i];
            break;
        }
        default:
            emu_log(LOG_SI, "SI ch%d command %02x\n", ch, cmd);
            break;
        }
    }
    for (u32 i = 0; i < inLen && i < 128; i++) s->si.buf[i] = resp[i];
    s->si.comcsr &= ~COMCSR_TSTART;
    s->si.comcsr |= COMCSR_TCINT;
    update_irq(s);
}

void si_reset(System* s) {
    memset(&s->si, 0, sizeof s->si);
    for (int ch = 0; ch < 4; ch++) {
        s->si.stick[ch][0] = s->si.stick[ch][1] = s->si.stick[ch][2] = s->si.stick[ch][3] = 0x80;
    }
}

u32 si_read32(System* s, u32 off) {
    if (off < 0x30) {
        int ch = (int)(off / 12);
        u32 r = off % 12;
        if (r == 0) return s->si.outbuf[ch];
        if (!port_present(ch)) return 0x80000000u;  // error status bit
        refresh_input(s, ch);
        s->si.sr &= ~(1u << (29 - 8 * ch));  // reading the input clears RDST for the port
        return r == 4 ? s->si.inbufH[ch] : s->si.inbufL[ch];
    }
    if (off >= SI_BUF && off < SI_BUF + 128) {
        u32 i = off - SI_BUF;
        return ((u32)s->si.buf[i] << 24) | ((u32)s->si.buf[i + 1] << 16) | ((u32)s->si.buf[i + 2] << 8) | s->si.buf[i + 3];
    }
    switch (off) {
    case SI_POLL: return s->si.poll;
    case SI_COMCSR: return s->si.comcsr;
    case SI_SR: {
        // Every polled, present port has fresh data whenever software looks.
        u32 v = s->si.sr;
        for (int ch = 0; ch < 4; ch++) if (port_present(ch) && (s->si.poll & (0x80u >> ch))) v |= 1u << (29 - 8 * ch);
        return v;
    }
    case SI_EXILK: return s->si.exilk;
    }
    return 0;
}

void si_write32(System* s, u32 off, u32 v) {
    if (off < 0x30) {
        if (off % 12 == 0) s->si.outbuf[off / 12] = v;
        return;
    }
    if (off >= SI_BUF && off < SI_BUF + 128) {
        u32 i = off - SI_BUF;
        for (int k = 0; k < 4; k++) s->si.buf[i + k] = (u8)(v >> (24 - 8 * k));
        return;
    }
    switch (off) {
    case SI_POLL: s->si.poll = v; break;
    case SI_COMCSR: {
        u32 clear = v & (COMCSR_TCINT | COMCSR_RDSTINT);
        s->si.comcsr = (s->si.comcsr & (COMCSR_TCINT | COMCSR_RDSTINT) & ~clear) | (v & ~(COMCSR_TCINT | COMCSR_RDSTINT));
        if (v & COMCSR_TSTART) command(s);
        else update_irq(s);
        break;
    }
    case SI_SR:
        if (v & (1u << 31)) {}  // WR: copy output buffers to the ports (immediate here)
        s->si.sr &= ~(v & 0x0F0F0F0Fu);  // error bits are write-one-to-clear
        break;
    case SI_EXILK: s->si.exilk = v; break;
    }
}
