// gc3ds for the New 3DS: program picker, emulation loop, video output to the top screen, pad input, stats.
// Programs are listed from romfs:/dols (bundled test programs) and sdmc:/gc3ds (the user's own files).
#include <3ds.h>
#define GC3DS_HOST_TYPES
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/boot/boot.h"
#include "../src/gx/gx.h"
#include "../src/jit/jit.h"

#define MAX_FILES 64
#define CYCLES_PER_FIELD (GEKKO_HZ / 60)

static System g_sys;
static char g_files[MAX_FILES][256];
static int g_nFiles;
static u32 g_fields;
static Jit* g_jit;
static bool g_useJit = true;

static void on_icbi(void* ctx, u32 pa) { jit_invalidate(ctx, pa); }

static void scan_dir(const char* dir) {
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) && g_nFiles < MAX_FILES) {
        size_t n = strlen(e->d_name);
        if (n > 4 && strcasecmp(e->d_name + n - 4, ".dol") == 0)
            snprintf(g_files[g_nFiles++], sizeof g_files[0], "%s/%s", dir, e->d_name);
    }
    closedir(d);
}

static u8* load_file(const char* path, size_t* size) {
    FILE* f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    u8* buf = malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, f) != (size_t)n) { free(buf); buf = NULL; }
    fclose(f);
    *size = (size_t)n;
    return buf;
}

static inline u8 clamp8(int v) { return (u8)(v < 0 ? 0 : v > 255 ? 255 : v); }

// One field of the XFB (YUYV, 640 wide) scaled to the 400x240 top screen. The top framebuffer is stored rotated:
// column-major, 240 pixels per column, BGR8.
static void present(System* s, void* user) {
    (void)user;
    g_fields++;
    u32 top, bottom, w, h, stride;
    if (!vi_get_xfb(s, &top, &bottom, &w, &h, &stride) || w == 0) return;
    u16 fbW, fbH;
    u8* fb = gfxGetFramebuffer(GFX_TOP, GFX_LEFT, &fbW, &fbH);
    u32 lines = h / 2 < 240 ? h / 2 : 240;
    (void)fbW; (void)fbH; (void)bottom;
    for (u32 y = 0; y < lines; y++) {
        u32 line = top + y * stride;
        if (line + w * 2 > PA_MEM1_END) break;
        const u8* src = s->mem1 + line;
        for (u32 x = 0; x < 400; x++) {
            u32 sx = x * w / 400;
            const u8* p = src + (sx & ~1u) * 2;
            int yy = (p[(sx & 1) ? 2 : 0] - 16) * 298, u = p[1] - 128, v = p[3] - 128;
            u8* o = fb + (x * 240 + (239 - y)) * 3;
            o[0] = clamp8((yy + 516 * u + 128) >> 8);
            o[1] = clamp8((yy - 100 * u - 208 * v + 128) >> 8);
            o[2] = clamp8((yy + 409 * v + 128) >> 8);
        }
    }
    gfxFlushBuffers();
    gfxSwapBuffers();
}

// 3DS controls -> GameCube pad: A/B/X/Y as labelled, L/R -> L/R, ZR -> Z, Start, D-pad, Circle Pad -> main stick,
// C-Stick -> C stick.
static void update_input(System* s) {
    hidScanInput();
    u32 k = hidKeysHeld();
    u16 b = 0;
    if (k & KEY_DLEFT) b |= 0x0001;
    if (k & KEY_DRIGHT) b |= 0x0002;
    if (k & KEY_DDOWN) b |= 0x0004;
    if (k & KEY_DUP) b |= 0x0008;
    if (k & KEY_ZR) b |= 0x0010;
    if (k & KEY_R) b |= 0x0020;
    if (k & KEY_L) b |= 0x0040;
    if (k & KEY_A) b |= 0x0100;
    if (k & KEY_B) b |= 0x0200;
    if (k & KEY_X) b |= 0x0400;
    if (k & KEY_Y) b |= 0x0800;
    if (k & KEY_START) b |= 0x1000;
    s->si.buttons[0] = b;
    circlePosition cp, cs;
    hidCircleRead(&cp);
    hidCstickRead(&cs);
    u8* st = s->si.stick[0];
    st[0] = clamp8(128 + cp.dx * 100 / 156);
    st[1] = clamp8(128 + cp.dy * 100 / 156);
    st[2] = clamp8(128 + cs.dx * 100 / 156);
    st[3] = clamp8(128 + cs.dy * 100 / 156);
    st[4] = (k & KEY_L) ? 0xFF : 0;
    st[5] = (k & KEY_R) ? 0xFF : 0;
}

static int pick(void) {
    int sel = 0;
    for (;;) {
        consoleClear();
        printf("gc3ds - A run, Y JIT on/off, START quit\n");
        printf("CPU: %s\n\n", !g_jit ? "interpreter (JIT unavailable)" : g_useJit ? "JIT" : "interpreter");
        if (!g_nFiles) printf("No .dol files in romfs:/dols or sdmc:/gc3ds\n");
        int first = sel > 20 ? sel - 20 : 0;
        for (int i = first; i < g_nFiles && i < first + 24; i++) {
            const char* name = strrchr(g_files[i], '/');
            printf("%c %.36s\n", i == sel ? '>' : ' ', name ? name + 1 : g_files[i]);
        }
        for (;;) {
            if (!aptMainLoop()) return -1;
            hidScanInput();
            u32 d = hidKeysDown();
            if (d & KEY_START) return -1;
            if ((d & KEY_A) && g_nFiles) return sel;
            if ((d & KEY_Y) && g_jit) { g_useJit = !g_useJit; break; }
            if ((d & KEY_DDOWN) && sel + 1 < g_nFiles) { sel++; break; }
            if ((d & KEY_DUP) && sel > 0) { sel--; break; }
            gspWaitForVBlank();
        }
    }
}

static void run(const char* path) {
    size_t size;
    u8* dol = load_file(path, &size);
    consoleClear();
    if (!dol) { printf("cannot read %s\n", path); return; }
    char err[256];
    if (g_jit) {
        jit_invalidate_all(g_jit);
        if (g_useJit) jit_attach(g_jit); else gekko_set_jit(&g_sys.cpu, NULL, NULL);
    }
    if (!boot_dol(&g_sys, dol, size, err, sizeof err)) { printf("boot failed: %s\n", err); free(dol); return; }
    free(dol);
    printf("running %s\nSELECT: back to menu\n\n", strrchr(path, '/') + 1);
    g_fields = 0;
    u64 t0 = svcGetSystemTick(), tLast = t0, cycLast = g_sys.cpu.cycles;
    u32 fieldsLast = 0;
    while (aptMainLoop()) {
        update_input(&g_sys);
        if (hidKeysDown() & KEY_SELECT) break;
        sys_run(&g_sys, CYCLES_PER_FIELD);
        if (g_sys.stopRequested) { printf("\nstopped at pc=%08x\n", (unsigned)g_sys.cpu.pc); break; }
        u64 now = svcGetSystemTick();
        if (now - tLast > SYSCLOCK_ARM11) {  // once a second
            double secs = (double)(now - tLast) / SYSCLOCK_ARM11;
            double mips = (double)(g_sys.cpu.cycles - cycLast) / secs / 1e6;
            double fps = (g_fields - fieldsLast) / secs;
            printf("\x1b[5;0H%6.2f MIPS  %5.2f fields/s  (%4.1f%% speed)   \n", mips, fps, 100.0 * mips / 486.0);
            printf("emulated %.1f s, real %.1f s     \n", (double)g_sys.cpu.cycles / GEKKO_HZ, (double)(now - t0) / SYSCLOCK_ARM11);
            if (g_jit && g_useJit) {
                const JitStats* js = jit_stats(g_jit);
                printf("JIT %llu blocks, %.1f%% native   \n", (unsigned long long)js->blocksCompiled,
                       js->instrsCompiled ? 100.0 * (double)js->instrsNative / (double)js->instrsCompiled : 0.0);
            }
            tLast = now; cycLast = g_sys.cpu.cycles; fieldsLast = g_fields;
        }
    }
    printf("\nPress A to return to the menu\n");
    while (aptMainLoop()) { hidScanInput(); if (hidKeysDown() & (KEY_A | KEY_START)) break; gspWaitForVBlank(); }
}

int main(void) {
    gfxInitDefault();
    gfxSetDoubleBuffering(GFX_TOP, true);
    consoleInit(GFX_BOTTOM, NULL);
    osSetSpeedupEnable(true);
    romfsInit();
    scan_dir("romfs:/dols");
    scan_dir("sdmc:/gc3ds");
    if (!sys_init(&g_sys)) { printf("out of memory\n"); svcSleepThread(3000000000LL); gfxExit(); return 1; }
    gx_soft_attach(&g_sys);
    g_sys.on_field = present;
    g_jit = jit_create(&g_sys.cpu, g_sys.mem1, MEM1_SIZE, 4u << 20);
    if (g_jit) { g_sys.icbiHook = on_icbi; g_sys.icbiCtx = g_jit; }
    // romfs:/autorun.txt (test builds) names a program to start without the menu.
    FILE* ar = fopen("romfs:/autorun.txt", "r");
    if (ar) {
        char name[200] = "";
        if (fgets(name, sizeof name, ar)) {
            name[strcspn(name, "\r\n")] = 0;
            for (int i = 0; i < g_nFiles; i++) if (strstr(g_files[i], name)) { run(g_files[i]); break; }
        }
        fclose(ar);
    }
    for (;;) {
        int i = pick();
        if (i < 0) break;
        run(g_files[i]);
    }
    sys_free(&g_sys);
    romfsExit();
    gfxExit();
    return 0;
}
