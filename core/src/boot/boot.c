#include "boot.h"
#include <stdio.h>
#include "../disc/disc.h"

static u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }

// DOL header: 7 text + 11 data section file offsets, load addresses and sizes, then BSS address/size and entry.
enum { DOL_OFFSETS = 0x00, DOL_ADDRS = 0x48, DOL_SIZES = 0x90, DOL_BSS_ADDR = 0xD8, DOL_BSS_SIZE = 0xDC, DOL_ENTRY = 0xE0,
       DOL_HEADER = 0x100, DOL_SECTIONS = 18 };

static bool load_sections(System* s, const u8* dol, size_t size, char* err, size_t errLen) {
    if (size < DOL_HEADER) { snprintf(err, errLen, "file too small for a DOL header"); return false; }
    for (int i = 0; i < DOL_SECTIONS; i++) {
        u32 off = be32(dol + DOL_OFFSETS + 4 * i), addr = be32(dol + DOL_ADDRS + 4 * i), len = be32(dol + DOL_SIZES + 4 * i);
        if (!len) continue;
        u32 pa = addr & 0x3FFFFFFF;
        if ((size_t)off + len > size || pa + len > PA_MEM1_END) {
            snprintf(err, errLen, "section %d (off %08x addr %08x len %08x) out of range", i, off, addr, len);
            return false;
        }
        memcpy(s->mem1 + pa, dol + off, len);
        emu_log(LOG_BOOT, "%s%d: %08x..%08x\n", i < 7 ? "text" : "data", i < 7 ? i : i - 7, addr, addr + len);
    }
    u32 bss = be32(dol + DOL_BSS_ADDR) & 0x3FFFFFFF, bssLen = be32(dol + DOL_BSS_SIZE);
    // The BSS range overlaps data sections in many DOLs; clearing it before the sections were copied would lose
    // them, so only zero bytes no section covers.
    if (bssLen && bss + bssLen <= PA_MEM1_END) {
        for (u32 a = bss; a < bss + bssLen; a++) {
            bool covered = false;
            for (int i = 0; i < DOL_SECTIONS && !covered; i++) {
                u32 sa = be32(dol + DOL_ADDRS + 4 * i) & 0x3FFFFFFF, sl = be32(dol + DOL_SIZES + 4 * i);
                if (sl && a >= sa && a < sa + sl) covered = true;
            }
            if (!covered) s->mem1[a] = 0;
        }
    }
    return true;
}

// Low-memory globals the IPL/apploader leave at 0x80000000 (see YAGCD "Dolphin OS globals"); libogc reads several.
static void setup_lowmem(System* s, u32 entry) {
    (void)entry;
    memset(s->mem1, 0, 0x3100);
    mem1_write32(s, 0x20, 0x0D15EA5E);  // boot magic: normal boot
    mem1_write32(s, 0x24, 1);           // version
    mem1_write32(s, 0x28, MEM1_SIZE);   // physical memory size
    mem1_write32(s, 0x2C, 1);           // console type: retail (production board)
    mem1_write32(s, 0x30, 0);           // arena low: set by the program
    mem1_write32(s, 0x34, 0x817FE8C0);  // arena high, below the IPL's reserved top of memory
    mem1_write32(s, 0x38, 0);           // FST location (no disc)
    mem1_write32(s, 0xCC, 0);           // TV mode: NTSC
    mem1_write32(s, 0xD0, 16u * 1024 * 1024);  // ARAM size
    mem1_write32(s, 0xEC, 0x81800000);  // top of memory as the OS sees it
    mem1_write32(s, 0xF0, MEM1_SIZE);   // simulated memory size
    mem1_write32(s, 0xF8, BUS_HZ);
    mem1_write32(s, 0xFC, GEKKO_HZ);
}

// CPU state as the IPL leaves it for a program.
static void setup_cpu(System* s) {
    Gekko* g = &s->cpu;
    // BATs: 256 MB cached at 0x80000000 and uncached at 0xC0000000, both mapping physical 0.
    gekko_set_spr(g, SPR_IBAT0U, 0x80001FFF);
    gekko_set_spr(g, SPR_IBAT0U + 1, 0x00000002);
    gekko_set_spr(g, SPR_DBAT0U, 0x80001FFF);
    gekko_set_spr(g, SPR_DBAT0U + 1, 0x00000002);
    gekko_set_spr(g, SPR_DBAT0U + 2, 0xC0001FFF);
    gekko_set_spr(g, SPR_DBAT0U + 3, 0x0000002A);  // cache-inhibited, guarded
    gekko_set_spr(g, SPR_HID0, 0x0011C464);
    gekko_set_spr(g, SPR_HID2, 0xE0000000);       // quantized load/store, write-gather pipe and paired singles enabled
    gekko_set_msr(g, MSR_FP | MSR_IR | MSR_DR | MSR_RI);
}

bool boot_dol(System* s, const u8* dol, size_t size, char* err, size_t errLen) {
    sys_reset(s);
    setup_lowmem(s, 0);
    if (!load_sections(s, dol, size, err, errLen)) return false;
    u32 entry = be32(dol + DOL_ENTRY);
    Gekko* g = &s->cpu;
    setup_cpu(s);
    g->gpr[1] = 0x816FFFF0;                      // a stack below the arena top until the program sets its own
    g->pc = entry;
    emu_log(LOG_BOOT, "entry %08x\n", entry);
    return true;
}

// ---- disc boot: what the IPL does after the logo, run high level ----
//
// The IPL copies the disc ID to 0x80000000, fills OS globals, loads the disc's apploader to 0x81200000 and calls it:
// entry(&init, &main, &close); init(OSReport); then main(&dst, &len, &discOffset) repeatedly, servicing each request
// with a disc read, until it returns 0; close() returns the game's entry point. The apploader itself loads the DOL
// and the FST and sets the FST globals (0x80000038/3C). Sources: YAGCD 4.2 (OS globals) and 13 (apploader); register
// and global values as Dolphin's BS2 emulation (Boot_BS2Emu.cpp) sets them for a GameCube disc.
#define AL_FUNCS 0x80003100u   // the three function pointers the apploader entry fills in
#define AL_REPORT 0x81300000u  // OSReport given to init: an HLE stub
#define AL_ARGS 0x81300004u    // main's output words: destination, length, disc offset
#define AL_TRAP 0x81300010u    // return address for HLE calls (a branch-to-self)
#define AL_LOAD 0x81200000u

static u32 guest_pa(u32 ea) { return ea & 0x01FFFFFF; }

static u32 rd_guest32(System* s, u32 ea) {
    u32 pa = guest_pa(ea);
    return pa + 4 <= PA_MEM1_END ? mem1_read32(s, pa) : 0;
}

static void guest_str(System* s, u32 ea, char* out, size_t len) {
    size_t i = 0;
    for (u32 pa = guest_pa(ea); i + 1 < len && pa < PA_MEM1_END && s->mem1[pa]; pa++) out[i++] = (char)s->mem1[pa];
    out[i] = 0;
}

// The apploader's OSReport: a small printf over guest memory (integer and string arguments in r4-r10).
static void apploader_report(System* s) {
    Gekko* g = &s->cpu;
    char fmt[256], out[512];
    guest_str(s, g->gpr[3], fmt, sizeof fmt);
    size_t o = 0;
    int arg = 4;
    for (const char* p = fmt; *p && o + 64 < sizeof out; p++) {
        if (*p != '%') { if (*p != '\n') out[o++] = *p; continue; }
        char spec[16];
        size_t k = 0;
        spec[k++] = '%';
        while (p[1] && strchr("0123456789.-+ #l", p[1]) && k < sizeof spec - 2) { p++; if (*p != 'l') spec[k++] = *p; }
        if (!p[1]) break;
        char conv = *++p;
        if (conv == '%') { out[o++] = '%'; continue; }
        u32 v = arg <= 10 ? g->gpr[arg++] : 0;
        spec[k++] = conv == 's' ? 's' : conv == 'p' ? 'x' : conv;
        spec[k] = 0;
        if (conv == 's') { char str[64]; guest_str(s, v, str, sizeof str); o += (size_t)snprintf(out + o, sizeof out - o, spec, str); }
        else if (strchr("diuxXc", conv) || conv == 'p') o += (size_t)snprintf(out + o, sizeof out - o, spec, v);
    }
    out[o < sizeof out ? o : sizeof out - 1] = 0;
    emu_log(LOG_BOOT, "apploader: %s\n", out);
}

// Calls a guest function with the CPU (interrupts off) until it returns to AL_TRAP.
static bool call_guest(System* s, u32 fn, char* err, size_t errLen) {
    Gekko* g = &s->cpu;
    g->lr = AL_TRAP;
    g->pc = fn;
    u64 start = s->cpu.cycles;
    while (g->pc != AL_TRAP) {
        if (g->pc == AL_REPORT) { apploader_report(s); g->pc = g->lr; continue; }
        if (s->cpu.cycles - start > 2000000000ull) { snprintf(err, errLen, "apploader function %08x did not return (pc %08x)", fn, g->pc); return false; }
        sys_run(s, 512);
        if (s->stopRequested) { snprintf(err, errLen, "CPU stopped in apploader at %08x", g->pc); return false; }
    }
    return true;
}

static void setup_lowmem_disc(System* s) {
    memset(s->mem1, 0, 0x3100);
    if (!disc_read(s->disc, 0, s->mem1, 0x20)) emu_error("disc ID read failed\n");  // disc ID, as READ DISC ID leaves it
    mem1_write32(s, 0x20, 0x0D15EA5E);  // boot magic: normal boot (0xE5207C22 would be a JTAG boot)
    mem1_write32(s, 0x24, 1);           // boot info version
    mem1_write32(s, 0x28, MEM1_SIZE);   // physical memory size
    mem1_write32(s, 0x2C, 3);           // console type: latest production board (YAGCD 4.2.1.1.2)
    mem1_write32(s, 0xCC, s->disc->header[3] == 'P' ? 1 : 0);  // TV mode the IPL set up: PAL for 'P' discs, else NTSC
    mem1_write32(s, 0xD0, 16u * 1024 * 1024);  // ARAM size
    mem1_write32(s, 0xF8, BUS_HZ);
    mem1_write32(s, 0xFC, GEKKO_HZ);
    // Default exception handlers the IPL leaves (rfi) until the OS installs its own.
    mem1_write32(s, 0x300, 0x4C000064);
    mem1_write32(s, 0x800, 0x4C000064);
    mem1_write32(s, 0xC00, 0x4C000064);
    // System time at boot, in timebase ticks (bus clock / 4) since 2000-01-01 (OS global 0x800030D8).
    u64 tb = (u64)s->exi.rtc * (BUS_HZ / 4);
    mem1_write32(s, 0x30D8, (u32)(tb >> 32));
    mem1_write32(s, 0x30DC, (u32)tb);
}

bool boot_disc(System* s, struct Disc* d, char* err, size_t errLen) {
    s->disc = d;
    sys_reset(s);
    setup_lowmem_disc(s);
    u8 al[0x20];
    if (!disc_read(d, 0x2440, al, sizeof al)) { snprintf(err, errLen, "cannot read the apploader header"); return false; }
    u32 alEntry = be32(al + 0x10), alSize = be32(al + 0x14) + be32(al + 0x18);
    if (!alSize || alSize > 0x100000 || (alEntry & 0xFE000000u) != 0x80000000u) {
        snprintf(err, errLen, "bad apploader header (entry %08x size %08x)", alEntry, alSize);
        return false;
    }
    if (!disc_read(d, 0x2460, s->mem1 + guest_pa(AL_LOAD), alSize)) { snprintf(err, errLen, "apploader read failed"); return false; }
    emu_log(LOG_BOOT, "apploader %.10s: entry %08x, %x bytes at %08x\n", (const char*)al, alEntry, alSize, AL_LOAD);
    mem1_write32(s, guest_pa(AL_REPORT), 0x48000000);  // b . (intercepted)
    mem1_write32(s, guest_pa(AL_TRAP), 0x48000000);
    setup_cpu(s);
    Gekko* g = &s->cpu;
    // Registers the NTSC IPL leaves: stack, small-data bases (some apploaders use r13).
    g->gpr[1] = 0x81566550;
    g->gpr[2] = 0x81465CC0;
    g->gpr[13] = 0x81465320;

    g->gpr[3] = AL_FUNCS; g->gpr[4] = AL_FUNCS + 4; g->gpr[5] = AL_FUNCS + 8;
    if (!call_guest(s, alEntry, err, errLen)) return false;
    u32 fInit = rd_guest32(s, AL_FUNCS), fMain = rd_guest32(s, AL_FUNCS + 4), fClose = rd_guest32(s, AL_FUNCS + 8);
    emu_log(LOG_BOOT, "apploader init %08x main %08x close %08x\n", fInit, fMain, fClose);
    g->gpr[3] = AL_REPORT;
    if (!call_guest(s, fInit, err, errLen)) return false;
    for (int n = 0;; n++) {
        g->gpr[3] = AL_ARGS; g->gpr[4] = AL_ARGS + 4; g->gpr[5] = AL_ARGS + 8;
        if (!call_guest(s, fMain, err, errLen)) return false;
        if (!g->gpr[3]) break;
        u32 dst = rd_guest32(s, AL_ARGS), len = rd_guest32(s, AL_ARGS + 4), off = rd_guest32(s, AL_ARGS + 8);
        emu_log(LOG_BOOT, "apploader request %d: disc %08x+%x -> %08x\n", n, off, len, dst);
        if (guest_pa(dst) + len > PA_MEM1_END) { snprintf(err, errLen, "apploader request out of range: %08x+%x", dst, len); return false; }
        if (!disc_read(d, off, s->mem1 + guest_pa(dst), len)) { snprintf(err, errLen, "disc read failed at %08x", off); return false; }
        if (n > 10000) { snprintf(err, errLen, "apploader never finished"); return false; }
    }
    if (!call_guest(s, fClose, err, errLen)) return false;
    u32 entry = g->gpr[3];
    emu_log(LOG_BOOT, "apploader done in %llu cycles: entry %08x, FST %08x size %x, arena %08x-%08x, bi2 %08x\n",
            (unsigned long long)s->cpu.cycles, entry, mem1_read32(s, 0x38), mem1_read32(s, 0x3C), mem1_read32(s, 0x30),
            mem1_read32(s, 0x34), mem1_read32(s, 0xF4));
    if ((entry & 0xFE000000u) != 0x80000000u) { snprintf(err, errLen, "bad entry point %08x", entry); return false; }
    // The IPL read the disc ID through the drive, so the drive is spun up with the ID read and no other reads made.
    s->di.state = 1;  // DRIVE_READY_NO_READS
    mem1_write32(s, guest_pa(AL_REPORT), 0);
    mem1_write32(s, guest_pa(AL_TRAP), 0);
    g->pc = entry;
    return true;
}
