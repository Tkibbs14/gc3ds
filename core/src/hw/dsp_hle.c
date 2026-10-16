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

// Voice lifetime per rendered frame (no mixing). A real DSP walks each voice through its samples and sets VPB[1] done
// when a one-shot voice runs out (or when the game asks it to end), which is how JAudio learns to free the channel.
// Without it every voice the game started stayed alive (2DS relay 297: ~30 enabled voices every frame, none ever done)
// and JAudio updated them all, every frame. Layout and rules as Dolphin's DSPHLE/UCodes/Zelda.cpp (read for reference):
// 0xC0 big-endian words a voice, 0x180 bytes apart from the base in command 0x01; a voice is processed when its bit is
// set in its 16-voice group's mask (the sync mails); 0x00 enabled, 0x01 done, 0x02 resampling ratio (4.12), 0x04 reset,
// 0x05 end reached, 0x06 constant sample, 0x08 + 4 n channel n (id, target volume, current volume), 0x2A / 0x2B Dolby
// current / target volume, 0x2C Dolby on, 0x30 position fraction (0.12), 0x34-0x35 position, 0x3A-0x3B samples left,
// 0x80 source type, 0x81 looping, 0x85 end requested, 0x88-0x89 loop address, 0x8A-0x8B loop start position (the end).
// A frame takes (fraction + 0x50 * ratio) >> 12 raw samples. AFC sources count samples, not whole 16-sample blocks: their
// end may come up to a frame early or late (timing only). g_dspVoices: frames, voices processed (enabled, not done) summed
// over frames, sample voices among them, voices ended by running out, by an end request.
u64 g_dspVoices[6];
bool g_dspVoiceLife = true;  // --voices off: the old behaviour (nothing ever done); --ab voices
static u32 g_vpbBase;        // command 0x01's base; kept out of System, whose size a save state checks
static u16 g_voiceMask[16];  // per 16-voice group, the voices the last sync mail released (bit 15 - v % 16)
static u32 vpb_base(System* s) {
    if (g_vpbBase) return g_vpbBase;
    // After a state load it is 0 until the game sends 0x01 again (only at boot); Twilight Princess's JAudio keeps it in
    // JASDsp::CH_BUF (zeldaret/tp symbols: 0x804512E8).
    if (s->dsp.hle.ucodeHash == 0x6CA33A6Du) return mem1_read32(s, 0x004512E8u);
    return 0;
}
static inline u32 vw(const u8* p, u32 i) { return (u32)p[2 * i] << 8 | p[2 * i + 1]; }
static inline void vset(u8* p, u32 i, u32 v) { p[2 * i] = (u8)(v >> 8); p[2 * i + 1] = (u8)v; }
static inline u32 v32(const u8* p, u32 i) { return vw(p, i) << 16 | vw(p, i + 1); }
static inline void vset32(u8* p, u32 i, u32 v) { vset(p, i, v >> 16); vset(p, i + 1, v & 0xFFFF); }

static void zelda_render_frame(System* s) {
    DSPHle* h = &s->dsp.hle;
    u32 base = vpb_base(s) & 0x01FFFFFFu, n = h->zVoicesPerFrame;
    g_dspVoices[0]++;
    if (n > 256) n = 256;
    if (!base || base + n * 0x180 > PA_MEM1_END) return;
    for (u32 v = 0; v < n; v++) {
        if (!((g_voiceMask[v >> 4 & 15] >> (15 - (v & 15))) & 1)) continue;
        u8* p = s->mem1 + base + v * 0x180;
        if (!vw(p, 0x00) || vw(p, 0x01)) continue;
        g_dspVoices[1]++;
        u32 type = vw(p, 0x80);
        bool sampled = type == 5 || type == 8 || type == 9 || type == 16 || type == 33;
        if (sampled) g_dspVoices[2]++;
        if (!g_dspVoiceLife) continue;
        if (vw(p, 0x85)) {  // end requested: the volumes halve each frame; done once all reach zero
            bool mute = true;
            if (vw(p, 0x2C)) {
                u32 cur = (u32)((s32)(s16)vw(p, 0x2A) / 2) & 0xFFFF;
                vset(p, 0x2A, cur); vset(p, 0x2B, cur);
                mute = !cur;
            } else {
                for (u32 c = 0; c < 6; c++) {
                    if (!vw(p, 0x08 + 4 * c)) continue;
                    u32 cur = (u32)((s32)(s16)vw(p, 0x0A + 4 * c) / 2) & 0xFFFF;
                    vset(p, 0x09 + 4 * c, cur); vset(p, 0x0A + 4 * c, cur);
                    if (cur) mute = false;
                }
            }
            if (mute) { vset(p, 0x01, 1); g_dspVoices[5]++; continue; }
        }
        if (sampled) {
            u32 pos = v32(p, 0x34), left = v32(p, 0x3A), loopAt = v32(p, 0x88), end = v32(p, 0x8A);
            if (vw(p, 0x04)) left = end - pos;  // reset: from the position to the end
            u32 step = vw(p, 0x30) + 0x50u * vw(p, 0x02), need = step >> 12;
            vset(p, 0x30, step & 0xFFF);
            bool ended = false, done = false;
            while (need) {
                if (ended) {
                    ended = false;
                    if (!vw(p, 0x81)) { done = true; break; }
                    pos = loopAt;
                    left = end - pos;
                }
                u32 take = left < need ? left : need;
                left -= take; pos += take; need -= take;
                if (!left) ended = true;
            }
            vset32(p, 0x34, pos);
            vset32(p, 0x3A, left);
            vset(p, 0x05, ended ? 1 : 0);
            if (done) { vset(p, 0x01, 1); g_dspVoices[3]++; }
        }
        if (vw(p, 0x04) && !vw(p, 0x06)) vset(p, 0x04, 0);
    }
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
            g_vpbBase = zelda_read(h);
            for (int i = 0; i < 3; i++) zelda_read(h);
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
        g_voiceMask[group] = (u16)mail;  // the voices of this group to process (zelda_render_frame)
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
