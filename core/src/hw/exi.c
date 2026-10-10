// External interface (EXI): three SPI-like channels. Modelled devices:
//   channel 0 device 1: the IPL chip (MX): real-time clock, 64-byte SRAM settings, debug UART; ROM reads return 0
//   memory card slots (channel 0/1 device 0): empty
// Transfers complete instantly and raise the transfer-complete interrupt.
#include <stdio.h>
#include <time.h>
#include "../sys/system.h"

enum { CSR = 0x00, MAR = 0x04, LEN = 0x08, CR = 0x0C, DATA = 0x10 };
enum { CSR_EXIINTMASK = 1 << 0, CSR_EXIINT = 1 << 1, CSR_TCINTMASK = 1 << 2, CSR_TCINT = 1 << 3,
       CSR_EXTINTMASK = 1 << 10, CSR_EXTINT = 1 << 11, CSR_EXT = 1 << 12, CSR_ROMDIS = 1 << 13 };

// IPL chip command words (first 4 bytes after selecting channel 0 device 1).
#define IPL_RTC 0x20000000u
#define IPL_SRAM 0x20000100u
#define IPL_UART 0x20010000u

static char g_uartLine[256];
static u32 g_uartLen;

static void uart_putc(char c) {
    if (c == '\n' || c == '\r' || g_uartLen + 1 >= sizeof g_uartLine) {
        if (g_uartLen) { g_uartLine[g_uartLen] = 0; emu_log(LOG_OS, "OSReport: %s\n", g_uartLine); g_uartLen = 0; }
        return;
    }
    g_uartLine[g_uartLen++] = c;
}

static void sram_checksum(u8* sram) {
    u16 sum = 0, inv = 0;
    for (int i = 0x0C; i < 0x14; i += 2) {
        u16 w = (u16)((sram[i] << 8) | sram[i + 1]);
        sum += w;
        inv += (u16)~w;
    }
    sram[0] = (u8)(sum >> 8); sram[1] = (u8)sum;
    sram[2] = (u8)(inv >> 8); sram[3] = (u8)inv;
}

static int selected_device(u32 csr) {
    u32 cs = (csr >> 7) & 7;
    return cs & 1 ? 0 : cs & 2 ? 1 : cs & 4 ? 2 : -1;
}

static bool device_present(int ch, int dev) { return ch == 0 && dev == 1; }

// One byte in each direction for the selected device.
static u8 device_xfer(System* s, int ch, int dev, u8 out, bool writing) {
    EXIChannel* c = &s->exi.ch[ch];
    if (!device_present(ch, dev)) return 0;
    u32 pos = c->devPos[dev]++;
    if (pos < 4) {  // command word, most significant byte first
        if (writing) c->devCmd[dev] = (c->devCmd[dev] << 8) | out;
        return 0;
    }
    u32 cmd = c->devCmd[dev] & 0x7FFFFFFFu;
    u32 i = pos - 4;
    if ((cmd & ~0xFFu) == IPL_RTC) {
        u32 rtc = s->exi.rtc;
        if (writing) return 0;
        return i < 4 ? (u8)(rtc >> (24 - 8 * i)) : 0;
    }
    if (cmd >= IPL_SRAM && cmd < IPL_SRAM + 0x40 * 64) {  // byte offset into SRAM is encoded << 6
        u32 at = (i + ((cmd - IPL_SRAM) >> 6)) & 63;
        if (writing) { s->exi.sram[at] = out; return 0; }
        return s->exi.sram[at];
    }
    if ((cmd & 0x7FFFFF00u) == IPL_UART) {
        if (writing) uart_putc((char)out);
        return 0;
    }
    return 0;  // IPL ROM contents are not available (HLE boot); reads return zeros
}

static void transfer(System* s, int ch) {
    EXIChannel* c = &s->exi.ch[ch];
    int dev = selected_device(c->csr);
    u32 rw = (c->cr >> 2) & 3;  // 0 read, 1 write, 2 read-write
    if (c->cr & 2) {  // DMA
        u32 pa = c->mar & 0x03FFFFE0, len = c->len & 0x03FFFFE0;
        for (u32 k = 0; k < len && pa + k < PA_MEM1_END; k++) {
            if (rw == 1) device_xfer(s, ch, dev, s->mem1[pa + k], true);
            else s->mem1[pa + k] = device_xfer(s, ch, dev, 0, false);
        }
    } else {
        u32 n = ((c->cr >> 4) & 3) + 1;
        u32 in = 0;
        for (u32 k = 0; k < n; k++) {
            u8 out = (u8)(c->data >> (24 - 8 * k));
            u8 b = device_xfer(s, ch, dev, out, rw != 0);
            in |= (u32)b << (24 - 8 * k);
        }
        if (rw != 1) c->data = in;
    }
    c->cr &= ~1u;
    c->csr |= CSR_TCINT;
}

static void update_irq(System* s) {
    bool any = false;
    for (int i = 0; i < 3; i++) {
        u32 v = s->exi.ch[i].csr;
        if (((v & CSR_TCINT) && (v & CSR_TCINTMASK)) || ((v & CSR_EXIINT) && (v & CSR_EXIINTMASK)) ||
            ((v & CSR_EXTINT) && (v & CSR_EXTINTMASK))) any = true;
    }
    if (any) pi_raise(s, PI_INT_EXI); else pi_clear(s, PI_INT_EXI);
}

void exi_reset(System* s) {
    memset(&s->exi, 0, sizeof s->exi);
    s->exi.ch[0].csr = CSR_ROMDIS;  // the IPL disables ROM decryption before handing over
    // Seconds since 2000-01-01 (the GameCube epoch), from the host clock.
    s->exi.rtc = s->fixedRtc ? s->fixedRtc : (u32)(time(NULL) - 946684800);
    u8* sr = s->exi.sram;
    sr[0x11] = 0x00;  // video mode: NTSC, no progressive
    sr[0x12] = 0x00;  // language: English
    sr[0x13] = 0x2C;  // flags: stereo sound, IPL boot settings as the console leaves them
    sram_checksum(sr);
}

u32 exi_read32(System* s, u32 off) {
    int ch = (int)(off / 0x14);
    if (ch > 2) return 0;
    EXIChannel* c = &s->exi.ch[ch];
    switch (off % 0x14) {
    case CSR: {
        u32 v = c->csr & ~CSR_EXT;
        // EXT: something is plugged into this channel's slot (memory cards only; none are).
        return v;
    }
    case MAR: return c->mar;
    case LEN: return c->len;
    case CR: return c->cr;
    case DATA: return c->data;
    }
    return 0;
}

void exi_write32(System* s, u32 off, u32 v) {
    int ch = (int)(off / 0x14);
    if (ch > 2) return;
    EXIChannel* c = &s->exi.ch[ch];
    switch (off % 0x14) {
    case CSR: {
        int before = selected_device(c->csr);
        u32 clear = v & (CSR_EXIINT | CSR_TCINT | CSR_EXTINT);  // write-one-to-clear status bits
        c->csr = (c->csr & (CSR_EXIINT | CSR_TCINT | CSR_EXTINT) & ~clear) | (v & ~(CSR_EXIINT | CSR_TCINT | CSR_EXTINT | CSR_EXT));
        if (ch == 0) c->csr |= CSR_ROMDIS & (v | c->csr);
        int after = selected_device(c->csr);
        if (after != before && after >= 0) { c->devPos[after] = 0; c->devCmd[after] = 0; }
        update_irq(s);
        break;
    }
    case MAR: c->mar = v; break;
    case LEN: c->len = v; break;
    case CR:
        c->cr = v;
        if (v & 1) { transfer(s, ch); update_irq(s); }
        break;
    case DATA: c->data = v; break;
    }
}
