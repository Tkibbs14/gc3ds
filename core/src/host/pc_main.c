// PC runner: boots a DOL or a disc image, runs it for a while and saves what the console would display.
//   gcemu program.dol|game.iso [--seconds N | --frames N] [--out DIR] [--every N] [--log cat,cat,...|all]
//         [--trace N [--trace-at CYCLES]] [--profile] [--disc-info] [--press BUTTON@from-to,...]
//         [--disc game.iso (with a DOL)] [--rtc SECONDS_SINCE_2000] [--from FIELD (for --every/--log)]
//         [--dump-efb FIELD] [--save-state FIELD:PATH] [--load-state PATH]
#include "../cpu/aot.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include "../boot/boot.h"
#include "../disc/disc.h"
#include "jit_glue.h"
#include "png.h"

typedef struct { u32 from, to; u32 buttons; } Press;

typedef struct {
    const char* outDir;
    u32 every;      // save every Nth field (0 = only the last)
    u32 fields;
    u32 stopAfter;  // stop after this many fields (0 = run for the given time)
    u8* rgb;
    Press presses[32];
    u32 numPresses;
    u32 from;       // --from: --every and --log only take effect from this field on
    u32 logMask;    // the --log selection, applied at field `from`
    u32 dumpEfb, efbDumps;
    u32 saveAt;             // --save-state FIELD:PATH
    const char* savePath;
    bool saveNow;
} Frontend;

static u8 clamp8(int v) { return (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }

// XFB is YUYV 4:2:2 (Y0 U Y1 V per pixel pair). Interlaced frames come from the top and bottom field buffers.
static bool xfb_to_rgb(System* s, u8* rgb, u32* outW, u32* outH) {
    u32 top, bottom, w, h, stride;
    if (!vi_get_xfb(s, &top, &bottom, &w, &h, &stride)) return false;
    if (w > 720 || h > 576) return false;
    if (!bottom) bottom = top + w * 2;
    for (u32 y = 0; y < h; y++) {
        u32 line = (y & 1 ? bottom : top) + (y >> 1) * stride;
        for (u32 x = 0; x < w; x += 2) {
            u32 pa = line + x * 2;
            if (pa + 4 > PA_MEM1_END) return false;
            const u8* p = s->mem1 + pa;
            int u = (int)ramp_rd8(p + 1) - 128, v = (int)ramp_rd8(p + 3) - 128;  // (guest memory's layout)
            for (int k = 0; k < 2; k++) {
                int yy = ((int)ramp_rd8(p + k * 2) - 16) * 298;
                u8* o = rgb + 3 * (y * w + x + k);
                o[0] = clamp8((yy + 409 * v + 128) >> 8);
                o[1] = clamp8((yy - 100 * u - 208 * v + 128) >> 8);
                o[2] = clamp8((yy + 516 * u + 128) >> 8);
            }
        }
    }
    *outW = w;
    *outH = h;
    return true;
}

static void save_frame(System* s, Frontend* fe, const char* name) {
    u32 w, h;
    if (!xfb_to_rgb(s, fe->rgb, &w, &h)) { fprintf(stderr, "no displayable XFB for %s\n", name); return; }
    char path[512];
    snprintf(path, sizeof path, "%s/%s.png", fe->outDir, name);
    if (png_write_rgb(path, fe->rgb, w, h)) fprintf(stderr, "saved %s (%ux%u)\n", path, w, h);
}

// --dump-efb FIELD: during that field and the next, saves the EFB (colour, alpha, depth top byte) before each copy.
static void on_efb_copy(System* s, void* user, u32 ctl) {
    Frontend* fe = user;
    if (!fe->dumpEfb || fe->fields < fe->dumpEfb || fe->fields > fe->dumpEfb + 1) return;
    const GXCore* gx = &s->gx.core;
    u32 tl = gx->bp[0x49], wh = gx->bp[0x4A];
    char path[512];
    static const char* const kinds[3] = { "rgb", "alpha", "z" };
    for (int k = 0; k < 3; k++) {
        u8* p = fe->rgb;
        for (u32 i = 0; i < EFB_W * EFB_H && i < 720 * 576; i++) {
            u32 c = k == 2 ? gx->efbZ[i] >> 16 : gx->efb[i];
            if (k == 0) { p[0] = (u8)(c >> 24); p[1] = (u8)(c >> 16); p[2] = (u8)(c >> 8); }
            else p[0] = p[1] = p[2] = (u8)c;
            p += 3;
        }
        snprintf(path, sizeof path, "%s/efb_f%05u_%03u_%s_%s_%ux%u.png", fe->outDir, fe->fields, fe->efbDumps,
                 (ctl >> 14) & 1 ? "xfb" : "tex", kinds[k], (wh & 0x3FF) + 1, ((wh >> 10) & 0x3FF) + 1);
        png_write_rgb(path, fe->rgb, EFB_W, EFB_H);
    }
    emu_log(LOG_GX, "dumped EFB %u before copy ctl %06x src %u,%u clear AR %06x GB %06x Z %06x zmode %06x pectl %06x\n",
            fe->efbDumps, ctl, tl & 0x3FF, (tl >> 10) & 0x3FF, gx->bp[0x4F], gx->bp[0x50], gx->bp[0x51], gx->bp[0x40], gx->bp[0x43]);
    fe->efbDumps++;
}

static void on_field(System* s, void* user) {
    Frontend* fe = user;
    fe->fields++;
    if (fe->from && fe->fields == fe->from) g_logMask = fe->logMask;
    if (fe->dumpEfb) s->gx.core.traceDraws = fe->fields >= fe->dumpEfb && fe->fields <= fe->dumpEfb + 1;
    if (fe->every && fe->fields % fe->every == 0 && fe->fields >= fe->from) {
        char name[32];
        snprintf(name, sizeof name, "field%05u", fe->fields);
        save_frame(s, fe, name);
    }
    // Scripted controller input for port 1: buttons held from field `from` to field `to`.
    // Pseudo-buttons above 0xFFFF push the main stick to an edge (SLEFT/SRIGHT/SUP/SDOWN).
    u32 held = 0;
    for (u32 i = 0; i < fe->numPresses; i++)
        if (fe->fields >= fe->presses[i].from && fe->fields <= fe->presses[i].to) held |= fe->presses[i].buttons;
    s->si.buttons[0] = (u16)held;
    s->si.stick[0][0] = held & 0x10000 ? 0x10 : held & 0x20000 ? 0xF0 : 0x80;
    s->si.stick[0][1] = held & 0x40000 ? 0xF0 : held & 0x80000 ? 0x10 : 0x80;
    if (fe->stopAfter && fe->fields >= fe->stopAfter) sys_stop(s, NULL);
    // A state is written between run slices, not from inside this event (the VI event is not rescheduled yet).
    if (fe->saveAt && fe->fields == fe->saveAt) { fe->saveNow = true; sys_stop(s, NULL); }
}

// "--press A@600-610,START@900-905": GameCube pad bits as the controller reports them (SI status word, high half).
static bool parse_presses(Frontend* fe, const char* spec) {
    static const struct { const char* name; u32 bit; } kButtons[] = {
        { "LEFT", 0x0001 }, { "RIGHT", 0x0002 }, { "DOWN", 0x0004 }, { "UP", 0x0008 }, { "Z", 0x0010 }, { "R", 0x0020 },
        { "L", 0x0040 }, { "A", 0x0100 }, { "B", 0x0200 }, { "X", 0x0400 }, { "Y", 0x0800 }, { "START", 0x1000 },
        { "SLEFT", 0x10000 }, { "SRIGHT", 0x20000 }, { "SUP", 0x40000 }, { "SDOWN", 0x80000 },
    };
    const char* p = spec;
    while (*p && fe->numPresses < sizeof fe->presses / sizeof fe->presses[0]) {
        char name[16];
        unsigned from, to;
        int n = 0;
        if (sscanf(p, "%15[A-Za-z]@%u-%u%n", name, &from, &to, &n) != 3) return false;
        u32 bit = 0;
        for (size_t i = 0; i < sizeof kButtons / sizeof kButtons[0]; i++) if (!strcmp(name, kButtons[i].name)) bit = kButtons[i].bit;
        if (!bit) return false;
        fe->presses[fe->numPresses++] = (Press){ from, to, bit };
        p += n;
        if (*p == ',') p++;
    }
    return true;
}

static void parse_log(const char* list) {
    static const char* const names[LOG_NCAT] = { "cpu", "mem", "hw", "boot", "gx", "dsp", "exi", "si", "di", "vi", "os" };
    g_logMask = 0;
    for (int i = 0; i < LOG_NCAT; i++) if (strstr(list, names[i])) g_logMask |= 1u << i;
    if (strstr(list, "all")) g_logMask = ~0u;
}

static void disasm_at(System* s, u32 a, char* dis, size_t len) {
    u32 pa;
    if (!gekko_translate_data(&s->cpu, a, &pa) || pa + 4 > PA_MEM1_END) { snprintf(dis, len, "?"); return; }
    gekko_disasm(a, mem1_read32(s, pa), dis, len);
}

// Single-steps n instructions, printing each with the registers it is most likely to need.
static void trace(System* s, u32 n) {
    Gekko* g = &s->cpu;
    for (u32 i = 0; i < n && !s->stopRequested; i++) {
        char dis[96];
        disasm_at(s, g->pc, dis, sizeof dis);
        fprintf(stderr, "T %08x  %-40s r3=%08x r4=%08x r5=%08x r31=%08x lr=%08x\n", g->pc, dis, g->gpr[3], g->gpr[4],
                g->gpr[5], g->gpr[31], g->lr);
        sys_run(s, 1);
    }
}

// PC histogram: samples where the CPU is every few thousand cycles (finds the loop a program is waiting in).
#define PROF_SLOTS 4096
typedef struct { u32 pc, hits; } ProfSlot;
static ProfSlot g_prof[PROF_SLOTS];

static void prof_add(u32 pc) {
    u32 h = (pc >> 2) * 2654435761u % PROF_SLOTS;
    for (u32 k = 0; k < PROF_SLOTS; k++, h = (h + 1) % PROF_SLOTS) {
        if (g_prof[h].pc == pc || !g_prof[h].hits) { g_prof[h].pc = pc; g_prof[h].hits++; return; }
    }
}

static int prof_cmp(const void* a, const void* b) { return (int)((const ProfSlot*)b)->hits - (int)((const ProfSlot*)a)->hits; }

static void prof_print(System* s) {
    qsort(g_prof, PROF_SLOTS, sizeof g_prof[0], prof_cmp);
    u64 total = 0;
    for (int i = 0; i < PROF_SLOTS; i++) total += g_prof[i].hits;
    fprintf(stderr, "PC profile (%llu samples):\n", (unsigned long long)total);
    for (int i = 0; i < 25 && g_prof[i].hits; i++) {
        char dis[96];
        disasm_at(s, g_prof[i].pc, dis, sizeof dis);
        fprintf(stderr, "  %5.1f%%  %08x  %s\n", 100.0 * g_prof[i].hits / (double)total, g_prof[i].pc, dis);
    }
}

static bool has_ext(const char* path, const char* ext) {
    size_t n = strlen(path), e = strlen(ext);
    if (n < e) return false;
    for (size_t i = 0; i < e; i++) if ((path[n - e + i] | 0x20) != (ext[i] | 0x20)) return false;
    return true;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s program.dol|game.iso [--seconds N | --frames N] [--out DIR] [--every N] [--log cats|all]\n"
                        "       [--trace N [--trace-at CYCLES]] [--profile] [--disc-info] [--press BUTTON@from-to,...]\n"
                        "       [--disc game.iso (with a DOL)] [--rtc SECONDS_SINCE_2000] [--from FIELD (for --every/--log)]\n"
                        "       [--dump-efb FIELD] [--save-state FIELD:PATH] [--load-state PATH]\n", argv[0]);
        return 2;
    }
    double seconds = 2.0;
    u32 traceN = 0;
    u64 traceAt = 0;
    bool profile = false, discInfo = false, useJit = false;
    const char* discPath = NULL;  // a disc in the drive while a DOL runs
    const char* loadPath = NULL;
    u32 rtc = 0;
    Frontend fe = { .outDir = "out", .every = 0 };
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--frames") && i + 1 < argc) { fe.stopAfter = (u32)atoi(argv[++i]); seconds = 1e6; }
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) fe.outDir = argv[++i];
        else if (!strcmp(argv[i], "--every") && i + 1 < argc) fe.every = (u32)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) parse_log(argv[++i]);
        else if (!strcmp(argv[i], "--trace") && i + 1 < argc) traceN = (u32)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--trace-at") && i + 1 < argc) traceAt = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--profile")) profile = true;
        else if (!strcmp(argv[i], "--disc-info")) discInfo = true;
        else if (!strcmp(argv[i], "--press") && i + 1 < argc) {
            if (!parse_presses(&fe, argv[++i])) { fprintf(stderr, "bad --press list (BUTTON@from-to,...)\n"); return 2; }
        }
        else if (!strcmp(argv[i], "--jit")) useJit = true;
        else if (!strcmp(argv[i], "--pica-tev")) gx_soft_set_pica_tev(true);
        else if (!strcmp(argv[i], "--disc") && i + 1 < argc) discPath = argv[++i];
        else if (!strcmp(argv[i], "--rtc") && i + 1 < argc) rtc = (u32)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--from") && i + 1 < argc) fe.from = (u32)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--dump-efb") && i + 1 < argc) fe.dumpEfb = (u32)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--load-state") && i + 1 < argc) loadPath = argv[++i];
        else if (!strcmp(argv[i], "--save-state") && i + 1 < argc) {
            char* colon = strchr(argv[++i], ':');
            if (!colon) { fprintf(stderr, "--save-state FIELD:PATH\n"); return 2; }
            fe.saveAt = (u32)atoi(argv[i]);
            fe.savePath = colon + 1;
        }
        else { fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
    }
    if (fe.from) { fe.logMask = g_logMask; g_logMask &= (1u << LOG_BOOT) | (1u << LOG_OS) | (1u << LOG_MEM); }
    mkdir(fe.outDir, 0777);
    static System sys;
    if (!sys_init(&sys)) { fprintf(stderr, "init failed\n"); return 1; }
    fe.rgb = malloc(720 * 576 * 3);
    gx_soft_attach(&sys);
    if (useJit && !host_jit_enable(&sys)) { fprintf(stderr, "--jit: no JIT on this host (ARM only)\n"); return 1; }
    sys.on_field = on_field;
    sys.user = &fe;
    sys.fixedRtc = rtc;
    if (fe.dumpEfb) sys.on_efb_copy = on_efb_copy;
    char err[256];
    static Disc disc;
    u8* dol = NULL;
    bool isDisc = has_ext(argv[1], ".iso") || has_ext(argv[1], ".gcm") || disc_probe(argv[1]);
    if (!isDisc && discPath) {
        if (!disc_open(&disc, discPath, err, sizeof err)) { fprintf(stderr, "disc: %s\n", err); return 1; }
        sys.disc = &disc;
    }
    if (isDisc) {
        if (!disc_open(&disc, argv[1], err, sizeof err)) { fprintf(stderr, "disc: %s\n", err); return 1; }
        if (discInfo) { disc_print_info(&disc); disc_close(&disc); return 0; }
        if (!boot_disc(&sys, &disc, err, sizeof err)) { fprintf(stderr, "boot: %s\n", err); return 1; }
    } else {
        FILE* f = fopen(argv[1], "rb");
        if (!f) { perror(argv[1]); return 1; }
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        dol = malloc((size_t)size);
        if (!dol || fread(dol, 1, (size_t)size, f) != (size_t)size) { fprintf(stderr, "read failed\n"); return 1; }
        fclose(f);
        if (!boot_dol(&sys, dol, (size_t)size, err, sizeof err)) { fprintf(stderr, "boot: %s\n", err); return 1; }
    }

    if (loadPath) {
        if (!sys_load_state(&sys, loadPath)) { fprintf(stderr, "cannot load state %s (other build?)\n", loadPath); return 1; }
        fe.fields = sys.vi.field;
        fprintf(stderr, "loaded state %s at field %u, cycle %llu\n", loadPath, fe.fields, (unsigned long long)sys.cpu.cycles);
    }

    // AOT=1: ahead-of-time translated code (aot.h) where it was generated; AOT_CHECK=1 also compares every call with the interpreter.
    if (getenv("AOT")) {
        aot_set_enabled(true);
        if (getenv("AOT_PIPE")) {  // the write-gather pipe's direct path, as the 3DS has it through the JIT
            aot_set_gather(gp_wgp_put, &sys, GP_WGP_EA);
            aot_set_gather_buffer(sys.gx.wgp, &sys.gx.wgpLen);
        }
        fprintf(stderr, "aot: %s\n", aot_enabled() ? "on" : "no translated code in this build");
    }
    // OPMIX=1: dynamic instruction mix of the interpreter, printed at exit (which instructions a JIT must cover).
    if (getenv("OPMIX")) g_gekkoOpMix = calloc(65536, sizeof(u64));
    if (getenv("GC3DS_NOGPBULK")) { extern bool g_gpBulk; g_gpBulk = false; }  // pipe bursts byte by byte (hw/gx.c)
    if (getenv("GC3DS_FEEDDIRECT")) { extern bool g_feedDirect; g_feedDirect = true; }  // GX commands parsed in place (gx/fifo.c)
    // PCHIST=file: executed instructions by address (0x80000000-0x81FFFFFF), written at exit as raw u32 counts.
    const char* pcHistPath = getenv("PCHIST");
    if (pcHistPath) g_gekkoPcHist = calloc(1u << 23, sizeof(u32));
    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    u64 startCycles = sys.cpu.cycles, end = startCycles + (u64)(seconds * GEKKO_HZ);
    if (traceN) {
        if (traceAt > sys.cpu.cycles) sys_run(&sys, traceAt - sys.cpu.cycles);
        trace(&sys, traceN);
    } else {
        // In steps, so that a --frames stop or a state save (requested from the field callback) ends a step.
        while (sys.cpu.cycles < end) {
            if (profile) { sys_run(&sys, 4999); prof_add(sys.cpu.pc); }
            else { u64 left = end - sys.cpu.cycles; sys_run(&sys, left > 100000000ull ? 100000000ull : left); }
            if (fe.saveNow) {
                fe.saveNow = false;
                sys.stopRequested = false;
                if (sys_save_state(&sys, fe.savePath)) fprintf(stderr, "saved state %s at field %u\n", fe.savePath, fe.fields);
                else fprintf(stderr, "cannot write state %s\n", fe.savePath);
                continue;
            }
            if (sys.stopRequested) break;
        }
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double wall = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;

    save_frame(&sys, &fe, "last");
    {  // the machine state as a hash (compare runs without frames, e.g. GC3DS_NODRAW)
        u64 h = 0xCBF29CE484222325ull;
        for (u32 i = 0; i < MEM1_SIZE; i++) { h ^= ram_rd8(sys.mem1, i); h *= 0x100000001B3ull; }  // (the guest's bytes, in order)
        for (u32 i = 0; i < 32; i++) { h ^= sys.cpu.gpr[i]; h *= 0x100000001B3ull; }
        h ^= sys.cpu.pc; h *= 0x100000001B3ull;
        fprintf(stderr, "state hash %016llx at cycle %llu\n", (unsigned long long)h, (unsigned long long)sys.cpu.cycles);
    }
    host_jit_report();
    if (aot_enabled())
        fprintf(stderr, "aot: %llu runs, %llu calls, %llu guest instructions (%.1f%% of %llu), %llu fallbacks, %llu slow accesses, %llu hash failures\n",
                (unsigned long long)aot_stats.runs, (unsigned long long)aot_stats.calls, (unsigned long long)aot_stats.insns,
                100.0 * (double)aot_stats.insns / (double)(sys.cpu.cycles - startCycles + 1), (unsigned long long)(sys.cpu.cycles - startCycles),
                (unsigned long long)aot_stats.fallbacks, (unsigned long long)aot_stats.slow, (unsigned long long)aot_stats.hashFail);
    if (g_gekkoPcHist) {
        FILE* f = fopen(pcHistPath, "wb");
        if (f) { fwrite(g_gekkoPcHist, sizeof(u32), 1u << 23, f); fclose(f); }
    }
    if (g_gekkoOpMix) {
        u64 total = 0;
        for (u32 k = 0; k < 65536; k++) total += g_gekkoOpMix[k];
        fprintf(stderr, "OPMIX: %llu instructions interpreted\n", (unsigned long long)total);
        for (int n = 0; n < 40 && total; n++) {
            u32 best = 0;
            for (u32 k = 1; k < 65536; k++) if (g_gekkoOpMix[k] > g_gekkoOpMix[best]) best = k;
            if (!g_gekkoOpMix[best]) break;
            char dis[64];
            gekko_disasm(0, ((best >> 10) << 26) | ((best & 0x3FF) << 1), dis, sizeof dis);
            char* sp = strchr(dis, ' ');
            if (sp) *sp = 0;
            fprintf(stderr, "  %5.2f%%  op %2u xo %4u  %s\n", 100.0 * (double)g_gekkoOpMix[best] / (double)total, best >> 10,
                    best & 0x3FF, dis);
            g_gekkoOpMix[best] = 0;
        }
    }
    if (profile) prof_print(&sys);
    // Where the program is and what it might be waiting for.
    Gekko* g = &sys.cpu;
    fprintf(stderr, "msr=%08x lr=%08x ctr=%08x r3=%08x r4=%08x  PI intsr=%08x intmr=%08x  VI DI0=%04x%04x TFBL=%04x%04x BFBL=%04x%04x HSW=%04x VTR=%04x line=%u\n",
            g->msr, g->lr, g->ctr, g->gpr[3], g->gpr[4], sys.pi.intsr, sys.pi.intmr, sys.vi.regs[0x30 / 2],
            sys.vi.regs[0x32 / 2], sys.vi.regs[0x1C / 2], sys.vi.regs[0x1E / 2], sys.vi.regs[0x24 / 2], sys.vi.regs[0x26 / 2], sys.vi.regs[0x48 / 2], sys.vi.regs[0], sys.vi.line);
    if (isDisc)
        fprintf(stderr, "DI: %llu reads, %llu bytes; DSP: microcode %08x, %u mails in, %u out; GX: %llu draws, %llu vertices\n",
                (unsigned long long)sys.di.reads, (unsigned long long)sys.di.bytesRead, sys.dsp.hle.ucodeHash,
                sys.dsp.hle.mailsIn, sys.dsp.hle.mailsOut, (unsigned long long)sys.gx.core.draws,
                (unsigned long long)sys.gx.core.vertices);
    if (isDisc) {
        // GX features the program used that the software renderer may not draw (see soft.c's header).
        const GXCore* gc = &sys.gx.core;
        #define BPNZ(r) ((gc->bpNonZero[(r) >> 5] >> ((r) & 31)) & 1)
        u32 ind = 0;
        for (u32 r = 0x10; r <= 0x1F; r++) ind += BPNZ(r);
        fprintf(stderr, "GX use: EFB copies %llu; prims quads %llu tris %llu strips %llu fans %llu lines %llu linestrips %llu points %llu;"
                " indirect-stage regs %u, ind matrices %u, fog %u, z-texture %u\n",
                (unsigned long long)gc->efbCopies, (unsigned long long)(gc->primCount[0] + gc->primCount[1]),
                (unsigned long long)gc->primCount[2], (unsigned long long)gc->primCount[3], (unsigned long long)gc->primCount[4],
                (unsigned long long)gc->primCount[5], (unsigned long long)gc->primCount[6], (unsigned long long)gc->primCount[7],
                ind, BPNZ(0x06) | BPNZ(0x09) | BPNZ(0x0C), BPNZ(0xF1), BPNZ(0xF5));
        #undef BPNZ
    }
    // DISASM=start:end (hex): guest code in that range at exit, e.g. a hot spot the 3DS sampler names.
    if (getenv("DISASM")) {
        u32 a0 = 0, a1 = 0;
        if (sscanf(getenv("DISASM"), "%x:%x", &a0, &a1) == 2)
            for (u32 a = a0 & ~3u; a <= a1; a += 4) {
                char dis[96];
                disasm_at(&sys, a, dis, sizeof dis);
                fprintf(stderr, "DISASM %08x  %s\n", a, dis);
            }
    }
    for (u32 a = g->pc - 16; a <= g->pc + 16; a += 4) {
        char dis[96];
        disasm_at(&sys, a, dis, sizeof dis);
        fprintf(stderr, "%s %08x  %s\n", a == g->pc ? ">" : " ", a, dis);
    }
    double emu = (double)(sys.cpu.cycles - startCycles) / GEKKO_HZ;
    fprintf(stderr, "ran %.2f emulated s (%llu cycles, %u fields) in %.2f s wall: %.1f MIPS; pc=%08x; GX bytes %llu\n",
            (double)sys.cpu.cycles / GEKKO_HZ, (unsigned long long)sys.cpu.cycles, fe.fields, wall,
            emu * GEKKO_HZ / wall / 1e6, sys.cpu.pc, (unsigned long long)sys.gx.bytesReceived);
    u64 skips, skipped;
    gekko_poll_stats(&sys.cpu, &skips, &skipped);
    if (skips) fprintf(stderr, "polling-loop skips: %llu, %.1f%% of emulated time\n", (unsigned long long)skips,
                       100.0 * (double)skipped / (double)sys.cpu.cycles);
    sys_free(&sys);
    if (isDisc || discPath) disc_close(&disc);
    free(dol);
    free(fe.rgb);
    return 0;
}
