// gc3ds for the New 3DS: program picker, emulation loop, video output to the top screen, pad input, stats.
// Programs are listed from romfs:/dols (bundled test programs) and sdmc:/gc3ds (the user's own files).
#include <3ds.h>
#define GC3DS_HOST_TYPES
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/boot/boot.h"
#include "../src/disc/disc.h"
#include "../src/gx/gx.h"
#include "../src/gx/pica/pica.h"
#include "../src/jit/jit.h"

#define MAX_FILES 64
#define CYCLES_PER_FIELD (GEKKO_HZ / 60)

static System g_sys;
static char g_files[MAX_FILES][320];
static int g_nFiles;
static u32 g_fields;
static Jit* g_jit;
static bool g_useJit = true;
static bool g_pica;  // GX draws on the PICA200 (false: software renderer)

static void on_icbi(void* ctx, u32 pa) { jit_invalidate(ctx, pa); }

static void scan_dir(const char* dir) {
    DIR* d = opendir(dir);
    if (!d) return;
    struct dirent* e;
    while ((e = readdir(d)) && g_nFiles < MAX_FILES) {
        size_t n = strlen(e->d_name);
        const char* ext = n > 4 ? e->d_name + n - 4 : "";
        if (!strcasecmp(ext, ".dol") || !strcasecmp(ext, ".iso") || !strcasecmp(ext, ".gcm"))
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

static volatile bool g_frameReady;

// VI field callback: only notes that a new field is out. Presenting happens in the main loop, at most once per
// 3DS refresh (presenting every emulated field flooded the GPU service when emulation ran ahead of real time).
static void on_field(System* s, void* user) {
    (void)s; (void)user;
    g_fields++;
    g_frameReady = true;
}

// Shows the field the VI scans out. With the GPU backend the image never left the GPU (virtual XFB); otherwise, and
// for programs that draw the XFB with the CPU, the XFB in RAM (YUYV, 640 wide) is scaled to the 400x240 top screen.
// The top framebuffer is stored rotated: column-major, 240 pixels per column, BGR8.
static void present_field(System* s);
static void present(System* s) {
    u64 t0 = prof_begin();
    present_field(s);
    prof_end(PROF_PRESENT, t0);
}

static u64 prof_clock(void) { return svcGetSystemTick(); }

static void present_field(System* s) {
    g_frameReady = false;
    u32 top = 0, bottom, w = 0, h, stride;
    bool vi = vi_get_xfb(s, &top, &bottom, &w, &h, &stride) && w;
    if (g_pica && pica_present(s, top)) return;
    if (!vi) return;
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
        printf("GPU: %s\n", g_pica ? "PICA200" : "software");
        printf("CPU: %s\n\n", !g_jit ? "interpreter (JIT unavailable)" : g_useJit ? "JIT" : "interpreter");
        if (!g_nFiles) printf("No programs in romfs:/dols, sdmc:/gc3ds or sdmc:/gc3ds/games\n");
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

static Disc g_disc;

static bool is_disc(const char* path) {
    size_t n = strlen(path);
    return n > 4 && (!strcasecmp(path + n - 4, ".iso") || !strcasecmp(path + n - 4, ".gcm"));
}

static void run(const char* path) {
    size_t size = 0;
    u8* dol = NULL;
    char err[256];
    consoleClear();
    if (g_disc.file) disc_close(&g_disc);
    if (is_disc(path)) {
        printf("opening disc...\n");
        if (!disc_open(&g_disc, path, err, sizeof err)) { printf("disc: %s\n", err); return; }
    } else {
        dol = load_file(path, &size);
        if (!dol) { printf("cannot read %s\n", path); return; }
    }
    if (g_pica) pica_reset();
    if (g_jit) {
        jit_invalidate_all(g_jit);
        if (g_useJit) jit_attach(g_jit); else gekko_set_jit(&g_sys.cpu, NULL, NULL);
    }
    bool booted = dol ? boot_dol(&g_sys, dol, size, err, sizeof err) : boot_disc(&g_sys, &g_disc, err, sizeof err);
    free(dol);
    if (!booted) { printf("boot failed: %s\n", err); return; }
    printf("running %s\nSELECT: back to menu\n\n", strrchr(path, '/') + 1);
    g_fields = 0;
    u64 t0 = svcGetSystemTick(), tLast = t0, cycLast = g_sys.cpu.cycles, cyc0 = g_sys.cpu.cycles, tPresent = t0;
    u32 fieldsLast = 0;
    u64 waitTicks = 0, drawsLast = g_sys.gx.core.draws, vertsLast = g_sys.gx.core.vertices;
    u64 jLast[5] = { 0 };  // instructions run, interpreted, blocks run, slow loads, slow stores (JIT, per second)
    g_profClock = prof_clock;  // X toggles the profile lines
    memset(g_hostProf, 0, sizeof g_hostProf);
    while (aptMainLoop()) {
        update_input(&g_sys);
        if (hidKeysDown() & KEY_SELECT) break;
        if (hidKeysDown() & KEY_X) { g_profClock = g_profClock ? NULL : prof_clock; printf("\x1b[9;0H\x1b[0J"); }
        sys_run(&g_sys, CYCLES_PER_FIELD);
        if (g_sys.stopRequested) { printf("\nstopped at pc=%08x\n", (unsigned)g_sys.cpu.pc); break; }
        // Pace to real time: when emulation is ahead, present and wait for the next 3DS refresh; when behind,
        // present at most once per refresh and keep going.
        u64 now = svcGetSystemTick();
        double emulated = (double)(g_sys.cpu.cycles - cyc0) / GEKKO_HZ, real = (double)(now - t0) / SYSCLOCK_ARM11;
        if (emulated > real) {
            if (g_frameReady) present(&g_sys);
            u64 w0 = svcGetSystemTick();
            gspWaitForVBlank();
            waitTicks += svcGetSystemTick() - w0;
            tPresent = svcGetSystemTick();
        } else if (g_frameReady && now - tPresent > SYSCLOCK_ARM11 / 60) {
            present(&g_sys);
            tPresent = now;
        }
        now = svcGetSystemTick();
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
            if (g_profClock) {
                // Share of real time per area; the rest is guest code, interpreter fallbacks and emulated hardware.
                double tot = (double)(now - tLast), p[PROF_N], rest = 100.0 - 100.0 * (double)waitTicks / tot;
                for (int k = 0; k < PROF_N; k++) {
                    p[k] = 100.0 * (double)g_hostProf[k] / tot;
                    if (k <= PROF_PRESENT) rest -= p[k];  // the others are nested inside GX or are overlapping views
                }
                // GX, then its parts: textures, vertices, waiting for the GPU (40 columns)
                printf("\x1b[9;0HGX%5.1f%% tex%5.1f vtx%5.1f gpu%5.1f \n", p[PROF_GX], p[PROF_GXTEX], p[PROF_GXVTX],
                       p[PROF_GXSYNC]);
                // disc, log; then the vertex path's gather (dec) and lighting (lit) parts of vtx
                printf("disc%5.1f log%4.1f  dec%5.1f lit%5.1f  \n", p[PROF_DISC], p[PROF_LOG], p[PROF_VDEC],
                       p[PROF_VLIGHT]);
                printf("JIT compile %4.1f%%  present %4.1f%%   \n", p[PROF_JITC], p[PROF_PRESENT]);
                printf("wait %4.1f%%  guest+other %4.1f%%   \n", 100.0 * (double)waitTicks / tot, rest);
                // cpu: inside gekko_run (includes GX and disc work the guest triggers); fpfb / fb: interpreted
                // float and other instructions from compiled code (sampled)
                printf("cpu%5.1f fpfb%5.1f fb%5.1f    \n", p[PROF_CPU], p[PROF_FBFP], p[PROF_FB]);
                printf("draws/s %5.0f  verts/s %6.0f   \n", (double)(g_sys.gx.core.draws - drawsLast) / secs,
                       (double)(g_sys.gx.core.vertices - vertsLast) / secs);
                if (g_jit && g_useJit) {
                    // Thousands per second: guest instructions run by compiled blocks, interpreted ones among them,
                    // blocks entered, and loads / stores through the C helpers (MMIO, write-gather pipe).
                    const JitStats* js = jit_stats(g_jit);
                    u64 now5[5] = { js->instrsRun, js->fallbacksRun, js->blocksRun, js->slowLoads, js->slowStores };
                    double k[5];
                    for (int i = 0; i < 5; i++) k[i] = (double)(now5[i] - jLast[i]) / secs / 1000.0;
                    printf("k/s ins %5.0f fb %4.0f blk %5.0f   \n", k[0], k[1], k[2]);
                    printf("k/s slow ld %4.0f st %4.0f  icbi %llu   \n", k[3], k[4], (unsigned long long)js->icbiFlushes);
                    memcpy(jLast, now5, sizeof jLast);
                }
            }
            vertsLast = g_sys.gx.core.vertices;
            memset(g_hostProf, 0, sizeof g_hostProf);
            waitTicks = 0;
            drawsLast = g_sys.gx.core.draws;
            tLast = now; cycLast = g_sys.cpu.cycles; fieldsLast = g_fields;
        }
    }
    printf("\nPress A to return to the menu\n");
    while (aptMainLoop()) { hidScanInput(); if (hidKeysDown() & (KEY_A | KEY_START)) break; gspWaitForVBlank(); }
}

int main(void) {
    jit_host_fpscr_init();  // the kernel starts threads rounding toward zero; compiled float code assumes nearest
    gfxInitDefault();
    gfxSetDoubleBuffering(GFX_TOP, true);
    consoleInit(GFX_BOTTOM, NULL);
    osSetSpeedupEnable(true);
    romfsInit();
    scan_dir("romfs:/dols");
    scan_dir("sdmc:/gc3ds");
    scan_dir("sdmc:/gc3ds/games");
    if (!sys_init(&g_sys)) { printf("out of memory\n"); svcSleepThread(3000000000LL); gfxExit(); return 1; }
    g_pica = pica_attach(&g_sys);
    if (!g_pica) { printf("GPU backend unavailable, using the software renderer\n"); gx_soft_attach(&g_sys); }
    g_sys.on_field = on_field;
    // 16 MB of code: Twilight Princess's title scene did not fit in 4 MB, and a full cache is dropped whole, so it
    // recompiled about a million blocks in 12 emulated seconds (35% of the time).
    g_jit = jit_create(&g_sys.cpu, g_sys.mem1, MEM1_SIZE, 16u << 20);
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
