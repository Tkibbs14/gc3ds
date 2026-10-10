// PC runner: boots a DOL, runs it for a while and saves what the console would display.
//   gcemu program.dol [--seconds N] [--out DIR] [--every N] [--log cat,cat,...]
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <time.h>
#include "../boot/boot.h"
#include "png.h"

typedef struct {
    const char* outDir;
    u32 every;     // save every Nth field (0 = only the last)
    u32 fields;
    u8* rgb;
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
            int u = p[1] - 128, v = p[3] - 128;
            for (int k = 0; k < 2; k++) {
                int yy = (p[k * 2] - 16) * 298;
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

static void on_field(System* s, void* user) {
    Frontend* fe = user;
    fe->fields++;
    if (fe->every && fe->fields % fe->every == 0) {
        char name[32];
        snprintf(name, sizeof name, "field%05u", fe->fields);
        save_frame(s, fe, name);
    }
}

static void parse_log(const char* list) {
    static const char* const names[LOG_NCAT] = { "cpu", "mem", "hw", "boot", "gx", "dsp", "exi", "si", "di", "vi", "os" };
    g_logMask = 0;
    for (int i = 0; i < LOG_NCAT; i++) if (strstr(list, names[i])) g_logMask |= 1u << i;
    if (strstr(list, "all")) g_logMask = ~0u;
}

int main(int argc, char** argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s program.dol [--seconds N] [--out DIR] [--every N] [--log cats|all]\n", argv[0]);
        return 2;
    }
    double seconds = 2.0;
    Frontend fe = { .outDir = "out", .every = 0 };
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) seconds = atof(argv[++i]);
        else if (!strcmp(argv[i], "--out") && i + 1 < argc) fe.outDir = argv[++i];
        else if (!strcmp(argv[i], "--every") && i + 1 < argc) fe.every = (u32)atoi(argv[++i]);
        else if (!strcmp(argv[i], "--log") && i + 1 < argc) parse_log(argv[++i]);
    }
    FILE* f = fopen(argv[1], "rb");
    if (!f) { perror(argv[1]); return 1; }
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8* dol = malloc((size_t)size);
    if (!dol || fread(dol, 1, (size_t)size, f) != (size_t)size) { fprintf(stderr, "read failed\n"); return 1; }
    fclose(f);
    mkdir(fe.outDir, 0777);

    static System sys;
    if (!sys_init(&sys)) { fprintf(stderr, "init failed\n"); return 1; }
    fe.rgb = malloc(720 * 576 * 3);
    gx_soft_attach(&sys);
    sys.on_field = on_field;
    sys.user = &fe;
    char err[256];
    if (!boot_dol(&sys, dol, (size_t)size, err, sizeof err)) { fprintf(stderr, "boot: %s\n", err); return 1; }

    struct timespec t0, t1;
    clock_gettime(CLOCK_MONOTONIC, &t0);
    sys_run(&sys, (u64)(seconds * GEKKO_HZ));
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double wall = (double)(t1.tv_sec - t0.tv_sec) + (double)(t1.tv_nsec - t0.tv_nsec) * 1e-9;

    save_frame(&sys, &fe, "last");
    // Where the program is and what it might be waiting for.
    Gekko* g = &sys.cpu;
    fprintf(stderr, "msr=%08x lr=%08x ctr=%08x r3=%08x r4=%08x  PI intsr=%08x intmr=%08x  VI DI0=%04x%04x TFBL=%04x%04x BFBL=%04x%04x HSW=%04x VTR=%04x line=%u\n",
            g->msr, g->lr, g->ctr, g->gpr[3], g->gpr[4], sys.pi.intsr, sys.pi.intmr, sys.vi.regs[0x30 / 2],
            sys.vi.regs[0x32 / 2], sys.vi.regs[0x1C / 2], sys.vi.regs[0x1E / 2], sys.vi.regs[0x24 / 2], sys.vi.regs[0x26 / 2], sys.vi.regs[0x48 / 2], sys.vi.regs[0], sys.vi.line);
    for (u32 a = g->pc - 16; a <= g->pc + 16; a += 4) {
        u32 pa;
        if (!gekko_translate_data(g, a, &pa) || pa + 4 > PA_MEM1_END) continue;
        char dis[96];
        gekko_disasm(a, mem1_read32(&sys, pa), dis, sizeof dis);
        fprintf(stderr, "%s %08x  %s\n", a == g->pc ? ">" : " ", a, dis);
    }
    fprintf(stderr, "ran %.2f emulated s (%llu cycles, %u fields) in %.2f s wall: %.1f MIPS; pc=%08x; GX bytes %llu\n",
            (double)sys.cpu.cycles / GEKKO_HZ, (unsigned long long)sys.cpu.cycles, fe.fields, wall,
            (double)sys.cpu.cycles / wall / 1e6, sys.cpu.pc, (unsigned long long)sys.gx.bytesReceived);
    sys_free(&sys);
    free(dol);
    free(fe.rgb);
    return 0;
}
