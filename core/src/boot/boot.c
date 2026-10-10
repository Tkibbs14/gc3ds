#include "boot.h"
#include <stdio.h>

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

bool boot_dol(System* s, const u8* dol, size_t size, char* err, size_t errLen) {
    sys_reset(s);
    setup_lowmem(s, 0);
    if (!load_sections(s, dol, size, err, errLen)) return false;
    u32 entry = be32(dol + DOL_ENTRY);
    Gekko* g = &s->cpu;
    // BATs as the IPL leaves them: 256 MB cached at 0x80000000 and uncached at 0xC0000000, both mapping physical 0.
    gekko_set_spr(g, SPR_IBAT0U, 0x80001FFF);
    gekko_set_spr(g, SPR_IBAT0U + 1, 0x00000002);
    gekko_set_spr(g, SPR_DBAT0U, 0x80001FFF);
    gekko_set_spr(g, SPR_DBAT0U + 1, 0x00000002);
    gekko_set_spr(g, SPR_DBAT0U + 2, 0xC0001FFF);
    gekko_set_spr(g, SPR_DBAT0U + 3, 0x0000002A);  // cache-inhibited, guarded
    gekko_set_spr(g, SPR_HID0, 0x0011C464);
    gekko_set_spr(g, SPR_HID2, 0xE0000000);       // quantized load/store, write-gather pipe and paired singles enabled
    gekko_set_msr(g, MSR_FP | MSR_IR | MSR_DR | MSR_RI);
    g->gpr[1] = 0x816FFFF0;                      // a stack below the arena top until the program sets its own
    g->pc = entry;
    emu_log(LOG_BOOT, "entry %08x\n", entry);
    return true;
}
