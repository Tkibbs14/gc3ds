// DSP microcode emulated at the mail level (no DSP instructions run).
//
// Boot ROM: after a reset it mails 0x8071FEED; the CPU then sends header/parameter mail pairs (0x80F3A001 IRAM
// image address in RAM, 0x80F3C002 IRAM destination, 0x80F3A002 length, 0x80F3B002 DMEM length, 0x80F3D001 start
// PC, which boots it). The uploaded image is identified by the hash Dolphin's microcode table uses (XOR each byte in,
// rotate left 3), so its game lists apply: 0x6CA33A6D is Twilight Princess (GC), a "Zelda" family microcode.
//
// Task mails (Dolphin DSPHLE/UCodes/UCodes.h): to the CPU 0xDCD1000x (0 init, 1 resume, 2 yield, 3 done, 4 sync,
// 5 frame end), to the DSP 0xCDD1000x (0 resume, 1 new microcode, 2 reset, 3 continue). A mail queued with an
// interrupt raises DSPCR.DSPINT when it becomes the readable one (Dolphin MailHandler behaviour).
//
// The Zelda-family protocol below follows the behaviour described by Dolphin's DSPHLE/UCodes/Zelda.cpp (read for
// reference, not copied). Audio is not mixed: commands are parsed and acknowledged, render requests complete without
// producing samples. A mixer would hook in at zelda_render_frame().
#include "../sys/system.h"

#define TASK_TO_CPU 0xDCD10000u
#define DSP_INIT (TASK_TO_CPU | 0)
#define DSP_RESUME (TASK_TO_CPU | 1)
#define DSP_SYNC (TASK_TO_CPU | 4)
#define DSP_FRAME_END (TASK_TO_CPU | 5)
#define MAIL_RESUME 0xCDD10000u
#define MAIL_NEW_UCODE 0xCDD10001u
#define MAIL_RESET 0xCDD10002u
#define MAIL_CONTINUE 0xCDD10003u

enum { Z_WAITING, Z_RENDERING, Z_WRITING_CMD, Z_HALTED };

typedef struct { u32 hash; int ucode; const char* name; } KnownUcode;
static const KnownUcode kUcodes[] = {
    { 0x6CA33A6D, DSP_UC_ZELDA, "Zelda (Twilight Princess GC / DK Jungle Beat)" },
    { 0x86840740, DSP_UC_ZELDA, "Zelda (Wind Waker)" },
    { 0x56D36052, DSP_UC_ZELDA, "Zelda (Super Mario Sunshine)" },
    { 0x42F64AC4, DSP_UC_ZELDA, "Zelda (Luigi's Mansion)" },
};

static u32 ector_hash(const u8* p, u32 len) {
    u32 h = 0;
    for (u32 i = 0; i < len; i++) { h ^= p[i]; h = (h << 3) | (h >> 29); }
    return h;
}

// ---- DSP -> CPU mail queue ----
static void push_mail(System* s, u32 mail, bool irq) {
    DSPHle* h = &s->dsp.hle;
    if (h->qCount == DSP_MAILQ) { emu_log(LOG_DSP, "mail queue full, dropping %08x\n", mail); return; }
    h->mailsOut++;
    emu_log(LOG_DSP, "DSP -> CPU %08x%s\n", mail, irq ? " (int)" : "");
    if (irq) {
        if (!h->qCount) dsp_raise_int(s);
        else h->qIrq[(h->qHead + h->qCount - 1) % DSP_MAILQ] = true;  // fires when the mail before it is read
    }
    u32 at = (h->qHead + h->qCount) % DSP_MAILQ;
    h->q[at] = mail;
    h->qIrq[at] = false;
    h->qCount++;
}

bool dsphle_mail_pending(System* s) { return s->dsp.hle.qCount != 0; }

u32 dsphle_peek_mail(System* s) {
    DSPHle* h = &s->dsp.hle;
    if (h->qCount) h->lastMail = h->q[h->qHead];
    return h->lastMail;
}

u32 dsphle_pop_mail(System* s) {
    DSPHle* h = &s->dsp.hle;
    if (!h->qCount) { u32 v = h->lastMail; h->lastMail &= 0x7FFFFFFF; return v; }
    u32 v = h->q[h->qHead];
    bool irq = h->qIrq[h->qHead];
    h->qHead = (h->qHead + 1) % DSP_MAILQ;
    h->qCount--;
    h->lastMail = v & 0x7FFFFFFF;  // the unread bit drops once the low half is read
    if (irq) dsp_raise_int(s);
    return v;
}

static void clear_queue(DSPHle* h) { h->qHead = h->qCount = 0; }

// ---- boot ROM ----
void dsphle_reset(System* s) {
    DSPHle* h = &s->dsp.hle;
    u32 in = h->mailsIn, out = h->mailsOut;
    memset(h, 0, sizeof *h);
    h->mailsIn = in;
    h->mailsOut = out;
    h->ucode = DSP_UC_ROM;
    push_mail(s, 0x8071FEED, false);
}

static void zelda_init(System* s);

static void select_ucode(System* s, u32 ram, u32 len, u32 pc) {
    DSPHle* h = &s->dsp.hle;
    u32 pa = ram & 0x01FFFFFF;
    if (pa + len > PA_MEM1_END) { emu_log(LOG_DSP, "microcode image %08x+%x out of range\n", ram, len); return; }
    h->ucodeHash = ector_hash(s->mem1 + pa, len);
    const KnownUcode* k = NULL;
    for (size_t i = 0; i < sizeof kUcodes / sizeof kUcodes[0]; i++) if (kUcodes[i].hash == h->ucodeHash) k = &kUcodes[i];
    clear_queue(h);
    emu_log(LOG_DSP, "microcode %08x+%x start %04x hash %08x: %s\n", ram, len, pc, h->ucodeHash, k ? k->name : "unknown");
    h->ucode = k ? k->ucode : DSP_UC_UNKNOWN;
    if (h->ucode == DSP_UC_ZELDA) zelda_init(s);
}

static void rom_mail(System* s, u32 mail) {
    DSPHle* h = &s->dsp.hle;
    if (!h->romParam) {
        if ((mail & 0xFFFF0000u) == 0x80F30000u) h->romParam = mail;
        else push_mail(s, 0xFEEE0000u | (mail & 0xFFFF), false);  // not a boot header: the ROM complains
        return;
    }
    u32 hdr = h->romParam;
    h->romParam = 0;
    switch (hdr) {
    case 0x80F3A001: h->bootRam = mail; break;
    case 0x80F3A002: h->bootLen = mail & 0xFFFF; break;
    case 0x80F3B002: h->bootDmem = mail & 0xFFFF; break;
    case 0x80F3C002: h->bootIram = mail & 0xFFFF; break;
    case 0x80F3D001: h->bootPc = mail & 0xFFFF; select_ucode(s, h->bootRam, h->bootLen, h->bootPc); break;
    default: emu_log(LOG_DSP, "boot ROM: unknown header %08x\n", hdr); break;
    }
}

// ---- Zelda family ----
static void zelda_init(System* s) {
    DSPHle* h = &s->dsp.hle;
    h->zState = Z_WAITING;
    h->zCanExec = true;
    push_mail(s, DSP_INIT, true);
    push_mail(s, 0xF3551111, false);
}

static void zelda_ack(System* s, u32 sync) {
    push_mail(s, DSP_SYNC, true);
    push_mail(s, 0xF3550000u | (sync & 0xFFFF), false);
}

static u32 zelda_read(DSPHle* h) {
    u32 v = h->zCmd[h->zCmdRd];
    h->zCmdRd = (h->zCmdRd + 1) % 256;
    return v;
}

static void zelda_render_frame(System* s) {
    (void)s;  // a mixer reads the voice parameter blocks here and writes the output buffers
}

// Renders requested frames as far as the voice syncs received allow (the CPU releases voices in groups of 16).
static void zelda_render(System* s) {
    DSPHle* h = &s->dsp.hle;
    while (h->zFrame < h->zFramesReq) {
        if (h->zVoice < h->zVoicesPerFrame) {
            if (h->zSyncMax <= h->zVoice) return;  // wait for the next sync mail
            h->zVoice = h->zSyncMax < h->zVoicesPerFrame ? h->zSyncMax : h->zVoicesPerFrame;
            if (h->zVoice < h->zVoicesPerFrame) return;
        }
        zelda_render_frame(s);
        zelda_ack(s, 0xFF00 | h->zFrame);
        h->zVoice = h->zSyncMax = 0;
        h->zFrame++;
    }
    h->zRendering = false;
    push_mail(s, DSP_FRAME_END, true);
    h->zCanExec = false;  // until the CPU says MAIL_CONTINUE
}

static void zelda_run_commands(System* s) {
    DSPHle* h = &s->dsp.hle;
    while (h->zCanExec && h->zPending) {
        u32 m = zelda_read(h);
        if (!(m & 0x80000000u)) continue;  // not a command header
        h->zPending--;
        u32 cmd = (m >> 24) & 0x7F, sync = m >> 16, extra = m & 0xFFFF;
        emu_log(LOG_DSP, "Zelda command %02x (%08x)\n", cmd, m);
        switch (cmd) {
        case 0x00: case 0x03: case 0x0A: case 0x0B: case 0x0C: case 0x0F:
            zelda_ack(s, sync);
            break;
        case 0x01:  // voices per frame; voice block base, coefficient tables, AFC coefficients, reverb blocks
            h->zVoicesPerFrame = extra;
            for (int i = 0; i < 4; i++) zelda_read(h);
            zelda_ack(s, sync);
            break;
        case 0x02:  // render: frames in bits 16-23, output volume; left and right buffers, then two more words
            h->zFramesReq = (m >> 16) & 0xFF;
            for (int i = 0; i < 4; i++) zelda_read(h);  // this microcode version combines command 0x0D into 0x02
            h->zFrame = h->zVoice = h->zSyncMax = 0;
            h->zRendering = true;
            zelda_render(s);
            return;
        case 0x0D: zelda_read(h); zelda_ack(s, sync); break;
        case 0x0E: zelda_read(h); zelda_ack(s, sync); break;  // ARAM base
        default:
            emu_log(LOG_DSP, "Zelda: command %02x halts the microcode\n", cmd);
            h->zState = Z_HALTED;
            return;
        }
    }
}

static void zelda_mail(System* s, u32 mail) {
    DSPHle* h = &s->dsp.hle;
    switch (h->zState) {
    case Z_WAITING:
        if (mail & 0x80000000u) {
            mail = (mail & 0xFFFF) | 0xCDD10000u;
            if (mail == MAIL_NEW_UCODE) { h->zCanExec = true; zelda_run_commands(s); h->uploading = true; h->uploadStep = 0; }
            else if (mail == MAIL_RESET) { h->zState = Z_HALTED; dsphle_reset(s); }
            else if (mail == MAIL_CONTINUE) { h->zCanExec = true; zelda_run_commands(s); }
            else h->zState = Z_HALTED;
        } else if (!(mail & 0xFFFF)) {
            h->zState = h->zRendering ? Z_RENDERING : Z_HALTED;
            if (!h->zRendering) emu_log(LOG_DSP, "Zelda: sync mail while not rendering\n");
        } else {
            h->zExpect = mail & 0xFFFF;
            h->zState = Z_WRITING_CMD;
        }
        break;
    case Z_WRITING_CMD:
        h->zCmd[h->zCmdWr] = mail;
        h->zCmdWr = (h->zCmdWr + 1) % 256;
        if (--h->zExpect == 0) {
            h->zPending++;
            h->zState = Z_WAITING;
            zelda_run_commands(s);
        }
        break;
    case Z_RENDERING: {
        u32 group = (mail >> 16) & 0xF;
        h->zSyncMax = (group + 1) << 4;
        h->zState = Z_WAITING;
        zelda_render(s);
        if (!h->zRendering) zelda_run_commands(s);
        break;
    }
    default:
        emu_log(LOG_DSP, "Zelda: mail %08x ignored (halted)\n", mail);
        break;
    }
}

// MAIL_NEW_UCODE is followed by ten words: RAM->DMEM save (address, size, DSP address), the IRAM image (RAM address,
// size, IRAM address, start PC), and the DMEM image (RAM address, size, DSP address).
static void upload_mail(System* s, u32 mail) {
    DSPHle* h = &s->dsp.hle;
    h->upload[h->uploadStep++] = mail;
    if (h->uploadStep < 10) return;
    h->uploading = false;
    select_ucode(s, h->upload[3], h->upload[4] & 0xFFFF, h->upload[6] & 0xFFFF);
    push_mail(s, DSP_RESUME, true);
}

void dsphle_cpu_mail(System* s, u32 mail) {
    DSPHle* h = &s->dsp.hle;
    h->mailsIn++;
    emu_log(LOG_DSP, "CPU -> DSP %08x\n", mail);
    if (h->uploading) { upload_mail(s, mail); return; }
    switch (h->ucode) {
    case DSP_UC_ROM: rom_mail(s, mail); break;
    case DSP_UC_ZELDA: zelda_mail(s, mail); break;
    default: break;  // no microcode, or one without an HLE: mails are accepted and dropped
    }
}
