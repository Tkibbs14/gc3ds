// Runs a fuzz ELF (see gen_fuzz.py) through the interpreter and compares its output with qemu-ppc's.
//   fuzz_compare [-c compatFlags] [-v] [-m mnemonic-histogram-file] <elf> <qemu-output>
// Exit status 0 = identical, 1 = mismatch, 2 = harness error. Prints one summary line:
//   RESULT <elf> cases=<n> mismatched=<m>
#include "../tb.h"
#include "../../../src/cpu/gekko_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CASE_SIZE 1536u
#define OUT_CAP (8u << 20)

static u32 be32(const u8* p) { return ((u32)p[0] << 24) | ((u32)p[1] << 16) | ((u32)p[2] << 8) | p[3]; }
static u64 be64(const u8* p) { return ((u64)be32(p) << 32) | be32(p + 4); }

static void field_name(u32 off, char* buf, size_t n, u32* width) {
    *width = 4;
    if (off < 128) snprintf(buf, n, "in.r%u", off / 4);
    else if (off < 384) { snprintf(buf, n, "in.f%u", (off - 128) / 8); *width = 8; }
    else if (off < 512) snprintf(buf, n, "in+%u", off);
    else if (off < 1024) snprintf(buf, n, "mem[r31%+d]", (int)off - 768);
    else if (off < 1152) snprintf(buf, n, "r%u", (off - 1024) / 4);
    else if (off < 1408) { snprintf(buf, n, "f%u", (off - 1152) / 8); *width = 8; }
    else if (off == 1408) snprintf(buf, n, "CR");
    else if (off == 1412) snprintf(buf, n, "XER");
    else if (off == 1416) snprintf(buf, n, "LR");
    else if (off == 1420) snprintf(buf, n, "CTR");
    else if (off == 1424) { snprintf(buf, n, "FPSCR(mffs)"); *width = 8; }
    else snprintf(buf, n, "out+%u", off);
}

int main(int argc, char** argv) {
    u32 compat = 0;
    bool verbose = false;
    const char* histFile = NULL;
    int traceCase = -1;
    int ai = 1;
    for (; ai < argc && argv[ai][0] == '-'; ai++) {
        if (!strcmp(argv[ai], "-c") && ai + 1 < argc) compat = (u32)strtoul(argv[++ai], NULL, 0);
        else if (!strcmp(argv[ai], "-t") && ai + 1 < argc) traceCase = atoi(argv[++ai]);
        else if (!strcmp(argv[ai], "-v")) verbose = true;
        else if (!strcmp(argv[ai], "-m") && ai + 1 < argc) histFile = argv[++ai];
    }
    if (argc - ai != 2) {
        fprintf(stderr, "usage: fuzz_compare [-c compat] [-v] [-m hist] <elf> <qemu-out>\n");
        return 2;
    }
    const char* elf = argv[ai];
    FILE* f = fopen(argv[ai + 1], "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", argv[ai + 1]); return 2; }
    u8* q = malloc(OUT_CAP);
    u32 qn = (u32)fread(q, 1, OUT_CAP, f);
    fclose(f);

    static Gekko g;
    static TestBus tb;
    tb_init(&tb, &g);
    if (!gekko_init(&g, &tb.bus)) return 2;
    gekko_set_compat(&g, compat);
    u32 entry = tb_load_elf(&tb, elf);
    if (!entry) { fprintf(stderr, "cannot load %s\n", elf); return 2; }
    u8* o = malloc(OUT_CAP);
    u32 on = 0;
    if (traceCase >= 0) {
        // Single-step the chosen case's body, printing FP operands and FPSCR around every instruction.
        u32 hdr0 = tb_r32(&tb, 0);
        (void)hdr0;
        u32 dataBase = 0;
        for (u32 a = 0x01000000u; a < 0x02000000u; a += 0x10000)
            if (tb_r32(&tb, a) == 0x47454B4Fu) { dataBase = a; break; }
        u32 tb0 = tb_r32(&tb, dataBase + 8 + 8 * (u32)traceCase), te = tb_r32(&tb, dataBase + 12 + 8 * (u32)traceCase);
        g.pc = entry;
        gekko_set_msr(&g, MSR_FP);
        gekko_set_spr(&g, SPR_HID2, 0xA0000000u);
        for (u64 n = 0; n < 50000000 && !(g.pc >= te && g.pc < te + 4); n++) {
            u32 pc = g.pc;
            bool in = pc >= tb0 && pc < te;
            u32 inst = in ? tb_r32(&tb, pc) : 0;
            u64 before[4] = {0};
            int regs[4] = {(inst >> 21) & 31, (inst >> 16) & 31, (inst >> 11) & 31, (inst >> 6) & 31};
            if (in) {
                char d[96];
                gekko_disasm(pc, inst, d, sizeof d);
                for (int k = 0; k < 4; k++) memcpy(&before[k], &g.fpr[regs[k]].ps0, 8);
                printf("%08x %-28s FPSCR=%08x CR=%08x | D=%016llx A=%016llx B=%016llx C=%016llx\n", pc, d, g.fpscr,
                       g.cr, (unsigned long long)before[0], (unsigned long long)before[1],
                       (unsigned long long)before[2], (unsigned long long)before[3]);
            }
            gekko_run(&g, 1);
            if (in) {
                u64 after;
                memcpy(&after, &g.fpr[regs[0]].ps0, 8);
                printf("         -> D=%016llx FPSCR=%08x CR=%08x\n", (unsigned long long)after, g.fpscr, g.cr);
            }
        }
        return 0;
    }
    if (!tb_run_linux(&tb, entry, o, OUT_CAP, &on, 2000000000ull)) {
        fprintf(stderr, "%s: interpreter did not exit (pc=%08x)\n", elf, g.pc);
        return 2;
    }
    if (tb.fatalCount) fprintf(stderr, "%s: gekko_on_fatal called %u times, last: %s\n", elf, tb.fatalCount, tb.lastFatal);
    if (qn < 8 || be32(q) != 0x47454B4Fu) { fprintf(stderr, "%s: bad qemu output (%u bytes)\n", elf, qn); return 2; }
    u32 ncases = be32(q + 4);
    u32 hdr = ((8 + 8 * ncases + 511) / 512) * 512;
    if (qn != on || qn != hdr + ncases * CASE_SIZE) {
        fprintf(stderr, "%s: output size qemu=%u interp=%u expected=%u\n", elf, qn, on, hdr + ncases * CASE_SIZE);
        return 2;
    }
    if (histFile) {
        FILE* h = fopen(histFile, "a");
        for (u32 c = 0; h && c < ncases; c++) {
            u32 b = be32(q + 8 + 8 * c), e = be32(q + 12 + 8 * c);
            for (u32 pc = b; pc < e; pc += 4) {
                char d[96];
                gekko_disasm(pc, tb_r32(&tb, pc), d, sizeof d);
                char* sp = strchr(d, ' ');
                if (sp) *sp = 0;
                fprintf(h, "%s\n", d);
            }
        }
        if (h) fclose(h);
    }
    u32 bad = 0;
    for (u32 c = 0; c < ncases; c++) {
        const u8* qa = q + hdr + c * CASE_SIZE;
        const u8* ia = o + hdr + c * CASE_SIZE;
        if (!memcmp(qa, ia, CASE_SIZE)) continue;
        bad++;
        if (bad > 3 && !verbose) continue;
        printf("MISMATCH %s case %u\n", elf, c);
        for (u32 off = 0; off < CASE_SIZE;) {
            char name[32];
            u32 w;
            field_name(off, name, sizeof name, &w);
            if (memcmp(qa + off, ia + off, w)) {
                if (w == 8) printf("  %-12s qemu=%016llx interp=%016llx\n", name, (unsigned long long)be64(qa + off), (unsigned long long)be64(ia + off));
                else printf("  %-12s qemu=%08x interp=%08x\n", name, be32(qa + off), be32(ia + off));
            }
            off += w;
        }
        u32 b = be32(q + 8 + 8 * c), e = be32(q + 12 + 8 * c);
        printf("  body %08x..%08x:\n", b, e);
        for (u32 pc = b; pc < e; pc += 4) {
            char d[96];
            gekko_disasm(pc, tb_r32(&tb, pc), d, sizeof d);
            printf("    %08x  %s\n", pc, d);
        }
    }
    printf("RESULT %s cases=%u mismatched=%u\n", elf, ncases, bad);
    return bad ? 1 : 0;
}
