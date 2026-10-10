// Video interface: field/line timing, display interrupts (DI0-DI3) and the external framebuffer (XFB) address.
// NTSC interlaced timing: 59.94 fields/s, 263 lines per field (the half line is not modelled).
#include "../sys/system.h"

#define LINES_PER_FIELD 263u
#define FIELD_HZ_X100 5994u
#define CYCLES_PER_FIELD ((u64)GEKKO_HZ * 100 / FIELD_HZ_X100)
#define CYCLES_PER_LINE (CYCLES_PER_FIELD / LINES_PER_FIELD)

// Register offsets (bytes; registers are 16-bit, some pairs form 32-bit values).
enum {
    VI_VTR = 0x00, VI_DCR = 0x02, VI_TFBL = 0x1C, VI_TFBR = 0x20, VI_BFBL = 0x24, VI_BFBR = 0x28,
    VI_DPV = 0x2C, VI_DPH = 0x2E, VI_DI0 = 0x30, VI_HSW = 0x48, VI_HSR = 0x4A, VI_VISEL = 0x6E,
};

static u32 reg32(System* s, u32 off) { return ((u32)s->vi.regs[off / 2] << 16) | s->vi.regs[off / 2 + 1]; }

static void update_irq(System* s) {
    bool any = false;
    for (int i = 0; i < 4; i++) if (s->vi.regs[(VI_DI0 + 4 * i) / 2] & 0x8000) any = true;
    if (any) pi_raise(s, PI_INT_VI); else pi_clear(s, PI_INT_VI);
}

static void line_event(System* s, u64 now, u32 arg) {
    (void)arg;
    VIState* v = &s->vi;
    v->line++;
    if (v->line > LINES_PER_FIELD) {
        v->line = 1;
        v->field++;
        v->fieldStart = now;
        u32 tfbl = reg32(s, VI_TFBL);
        v->xfbLatched = (tfbl & (1u << 28)) ? (tfbl & 0xFFFFFF) << 5 : (tfbl & 0xFFFFFF);
        if (s->on_field) s->on_field(s, s->user);
    }
    // A display interrupt fires when the beam reaches its programmed line (vertical count, 1-based).
    for (int i = 0; i < 4; i++) {
        u16* hi = &v->regs[(VI_DI0 + 4 * i) / 2];
        if ((*hi & 0x1000) && (*hi & 0x3FF) == v->line) { *hi |= 0x8000; update_irq(s); }
    }
    sched_add(s, now + CYCLES_PER_LINE, line_event, 0);
}

void vi_reset(System* s) {
    memset(&s->vi, 0, sizeof s->vi);
    s->vi.line = 1;
    s->vi.regs[VI_VISEL / 2] = 0;                      // composite cable, NTSC
    s->vi.regs[VI_VTR / 2] = (240u << 4) | 6;           // 240 active lines per field, as the IPL leaves it
    s->vi.regs[VI_HSW / 2] = (40u << 10) | 40;          // 640 pixels wide, stride 640
    sched_add(s, sys_now(s) + CYCLES_PER_LINE, line_event, 0);
}

u16 vi_read16(System* s, u32 off) {
    if (off >= 0x80) return 0;
    if (off == VI_DPV) return (u16)s->vi.line;
    if (off == VI_DPH) {
        u64 inLine = (sys_now(s) - s->vi.fieldStart) % CYCLES_PER_LINE;
        return (u16)(1 + inLine * 858 / CYCLES_PER_LINE);
    }
    return s->vi.regs[off / 2];
}

void vi_write16(System* s, u32 off, u16 val) {
    if (off >= 0x80) return;
    u16* r = &s->vi.regs[off / 2];
    if (off >= VI_DI0 && off < VI_DI0 + 16 && !(off & 2)) {
        // Interrupt status (bit 15) is cleared by writing 0 to it, and is never set by software.
        *r = (u16)((val & 0x7FFF) | (*r & val & 0x8000));
        update_irq(s);
        return;
    }
    *r = val;
}

static u32 fb_addr(u32 reg) { return (reg & (1u << 28)) ? (reg & 0xFFFFFF) << 5 : (reg & 0xFFFFFF); }

bool vi_get_xfb(System* s, u32* pa, u32* bottom, u32* width, u32* height, u32* stride) {
    u16 hsw = s->vi.regs[VI_HSW / 2];
    u32 activeLines = (s->vi.regs[VI_VTR / 2] >> 4) & 0x3FF;
    *pa = s->vi.xfbLatched;
    *bottom = fb_addr(reg32(s, VI_BFBL));
    *width = ((hsw >> 10) & 0x3F) * 16;
    *stride = (hsw & 0x3FF) * 32;  // bytes per line of the field
    *height = activeLines * 2;     // interlaced frame (two fields)
    if (*width == 0) *width = 640;
    return *pa != 0 && *pa < PA_MEM1_END;
}
