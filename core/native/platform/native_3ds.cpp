// gc3ds native mode on the 3DS: the process entry point, the platform functions m_Do_main_3ds.cpp needs (native.h),
// and the feasibility measurement. Each second the bottom screen shows, and sdmc:/gc3ds/native/perf.csv records,
// the average milliseconds per frame of the game's own work (logic and GX submission, frame_begin to
// frame_logic_done) and of the whole frame, frames per second, and the GX stream bytes per frame.
#include <3ds.h>

#include "native.h"

#include "dusk/main.h"
#include "dusk/settings.h"
#include "dusk/os.h"

#include <atomic>
#include <cstdarg>
#include <malloc.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>
#include <thread>

// Dusklight's own measure of the game's work: JFWDisplay's waitForTick paces the frame with its Limiter and sets this to
// the share of the frame not spent sleeping (the 2DS's first native run: "logic" equalled the whole frame, 16.4 ms,
// because that sleep is inside the game's frame).
namespace dusk {
extern float frameUsagePct;
}

int game_main(int argc, char* argv[]);
namespace native {
extern std::atomic<uint32_t> g_fifoBytes;  // aurora_3ds.cpp
extern std::atomic<uint32_t> g_drawDones;
}  // namespace native

static char g_disc[512];
static bool g_exit;
static FILE* g_perf;

// Run control for automated runs (gc3ds's relay job "--native"): --seconds N ends the run cleanly after N seconds,
// --return PATH hands the 3DS back to that .3dsx afterwards (gc3ds, which then posts this run's logs), --press SCRIPT
// presses buttons on given frames. A plain argument is the disc image.
static u32 g_seconds;
static char g_returnPath[256];
static u64 g_startMs;
static u32 g_frameNo;  // frames since the game loop started (the script's clock)
struct Press { u32 keys; int dx, dy; u32 from, to; };
static Press g_press[64];
static int g_nPress;

static void parse_press(const char* script) {
    static const struct { const char* name; u32 keys; int dx, dy; } kNames[] = {
        {"A", KEY_A, 0, 0}, {"B", KEY_B, 0, 0}, {"X", KEY_X, 0, 0}, {"Y", KEY_Y, 0, 0}, {"START", KEY_START, 0, 0},
        {"L", KEY_L, 0, 0}, {"R", KEY_R, 0, 0}, {"Z", KEY_ZR, 0, 0}, {"UP", KEY_DUP, 0, 0}, {"DOWN", KEY_DDOWN, 0, 0},
        {"LEFT", KEY_DLEFT, 0, 0}, {"RIGHT", KEY_DRIGHT, 0, 0}, {"SUP", 0, 0, 156}, {"SDOWN", 0, 0, -156},
        {"SLEFT", 0, -156, 0}, {"SRIGHT", 0, 156, 0}};
    char buf[512];
    snprintf(buf, sizeof buf, "%s", script);
    for (char* tok = strtok(buf, ","); tok && g_nPress < 64; tok = strtok(nullptr, ",")) {
        char* at = strchr(tok, '@');
        if (!at) continue;
        *at = 0;
        unsigned from = 0, to = 0;
        if (sscanf(at + 1, "%u-%u", &from, &to) < 1) continue;
        if (to < from) to = from;
        for (const auto& k : kNames)
            if (!strcasecmp(tok, k.name)) g_press[g_nPress++] = {k.keys, k.dx, k.dy, from, to};
    }
}

static void parse_args(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (!argv[i]) continue;
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) g_seconds = (u32)strtoul(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--return") && i + 1 < argc) snprintf(g_returnPath, sizeof g_returnPath, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--press") && i + 1 < argc) parse_press(argv[++i]);
        else if (argv[i][0] && argv[i][0] != '-') snprintf(g_disc, sizeof g_disc, "%s", argv[i]);
    }
}

// Hands the 3DS to the .3dsx at `path` when this process ends: Luma3DS's homebrew loader (port hb:ldr) loads it the
// next time the homebrew title starts, and the chainloader restarts that title. As gc3ds's relaunch_ipc (main.c):
// command 2 SetTarget (path without "sdmc:"), 3 SetArgv (argc, then NUL-separated strings).
static bool return_to(const char* path) {
    Handle h;
    if (R_FAILED(svcConnectToPort(&h, "hb:ldr"))) return false;
    const char* target = !strncmp(path, "sdmc:", 5) ? path + 5 : path;
    static u32 argBuf[0x400 / 4];
    memset(argBuf, 0, sizeof argBuf);
    argBuf[0] = 1;
    snprintf((char*)&argBuf[1], sizeof argBuf - 4, "%s", path);
    u32* cmd = getThreadCommandBuffer();
    cmd[0] = IPC_MakeHeader(2, 0, 2);
    cmd[1] = IPC_Desc_StaticBuffer(strlen(target) + 1, 0);
    cmd[2] = (u32)target;
    Result rc = svcSendSyncRequest(h);
    if (R_SUCCEEDED(rc)) rc = (Result)cmd[1];
    if (R_SUCCEEDED(rc)) {
        cmd = getThreadCommandBuffer();
        cmd[0] = IPC_MakeHeader(3, 0, 2);
        cmd[1] = IPC_Desc_StaticBuffer(sizeof argBuf, 1);
        cmd[2] = (u32)argBuf;
        rc = svcSendSyncRequest(h);
        if (R_SUCCEEDED(rc)) rc = (Result)cmd[1];
    }
    svcCloseHandle(h);
    if (R_FAILED(rc)) return false;
    aptSetChainloaderToSelf();
    return true;
}

static bool contains_nocase(const char* s, const char* word) {
    for (size_t n = strlen(word); *s; s++)
        if (!strncasecmp(s, word, n)) return true;
    return false;
}

// The disc image: the first argument, else the first .iso / .gcm under sdmc:/gc3ds or sdmc:/gc3ds/games whose name
// mentions Twilight (where gc3ds's menu looks).
static bool find_disc() {
    if (g_disc[0]) return true;  // given as an argument
    static const char* const kDirs[] = {"sdmc:/gc3ds/games", "sdmc:/gc3ds"};
    for (const char* dir : kDirs) {
        DIR* d = opendir(dir);
        if (!d) continue;
        while (struct dirent* e = readdir(d)) {
            size_t n = strlen(e->d_name);
            const char* ext = n > 4 ? e->d_name + n - 4 : "";
            if ((!strcasecmp(ext, ".iso") || !strcasecmp(ext, ".gcm")) && contains_nocase(e->d_name, "twilight")) {
                snprintf(g_disc, sizeof g_disc, "%s/%s", dir, e->d_name);
                closedir(d);
                return true;
            }
        }
        closedir(d);
    }
    return false;
}

namespace native {
void init_data_paths() {
    mkdir("sdmc:/gc3ds", 0777);
    mkdir("sdmc:/gc3ds/native", 0777);
    mkdir("sdmc:/gc3ds/native/cache", 0777);
    dusk::ConfigPath = "sdmc:/gc3ds/native";
    dusk::CachePath = "sdmc:/gc3ds/native/cache";
}
void apply_settings() {
    dusk::getSettings().game.enableFrameInterpolation.setValue(dusk::FrameInterpMode::Off);
    dusk::OSReportReallyForceEnable = true;  // the game's OSReport messages into the log while native mode is brought up
}
// The game's heaps: the GameCube had 24 MB of MEM1 (and 16 MB of ARAM, which the game streams through); structures
// are the GameCube's sizes again on a 32-bit host, so the arena is near the original.
uint32_t arena_bytes() { return 44u << 20; }  // the heaps m_Do_machine.cpp carves: ~37 MB with the 3DS sizes
const char* disc_path() { return g_disc; }
bool audio_enabled() { return false; }
void poll_events() {
    if (!aptMainLoop()) g_exit = true;
    hidScanInput();
    if ((hidKeysHeld() & (KEY_START | KEY_SELECT)) == (KEY_START | KEY_SELECT)) g_exit = true;
    if (g_seconds && osGetTime() - g_startMs >= (u64)g_seconds * 1000) g_exit = true;  // --seconds
}
bool exit_requested() { return g_exit; }
uint32_t scripted_keys() {
    u32 keys = 0;
    for (int i = 0; i < g_nPress; i++)
        if (g_frameNo >= g_press[i].from && g_frameNo <= g_press[i].to) keys |= g_press[i].keys;
    return keys;
}
bool scripted_stick(int* dx, int* dy) {
    bool any = false;
    for (int i = 0; i < g_nPress; i++)
        if ((g_press[i].dx || g_press[i].dy) && g_frameNo >= g_press[i].from && g_frameNo <= g_press[i].to) {
            *dx = g_press[i].dx;
            *dy = g_press[i].dy;
            any = true;
        }
    return any;
}

static u64 s_t0, s_tLogic, s_sumLogic, s_sumFrame, s_window;
static double s_sumUsage;
static u32 s_frames;
static u64 ticks_to_us(u64 t) { return t * 1000000ull / SYSCLOCK_ARM11; }
void frame_begin() {
    s_t0 = svcGetSystemTick();
    if (!s_window) s_window = s_t0;
}
void frame_logic_done() { s_tLogic = svcGetSystemTick(); }
void frame_end() {
    u64 t = svcGetSystemTick();
    s_sumLogic += s_tLogic - s_t0;
    s_sumFrame += t - s_t0;
    s_sumUsage += dusk::frameUsagePct;
    s_frames++;
    g_frameNo++;
    if (t - s_window < SYSCLOCK_ARM11) return;
    double secs = (double)(t - s_window) / SYSCLOCK_ARM11;
    double logicMs = ticks_to_us(s_sumLogic) / 1000.0 / s_frames, frameMs = ticks_to_us(s_sumFrame) / 1000.0 / s_frames;
    uint32_t bytes = g_fifoBytes.exchange(0), dd = g_drawDones.exchange(0);
    double busy = s_sumUsage / s_frames;  // % of each frame the game worked (Dusklight's frame limiter)
    printf("\x1b[2;1H%5.1f fps  logic %6.2f ms  frame %6.2f ms   \n GX %6u KB/frame  draw-done %u/s   \n busy %5.1f%% of the frame (%5.2f ms)   \n",
           s_frames / secs, logicMs, frameMs, (unsigned)(bytes / s_frames / 1024), (unsigned)(dd / secs), busy, busy * frameMs / 100.0);
    if (g_perf) {
        fprintf(g_perf, "%.1f,%.2f,%.2f,%.2f,%u,%u,%.1f\n", (double)ticks_to_us(t) / 1e6, s_frames / secs, logicMs, frameMs,
                (unsigned)(bytes / s_frames), (unsigned)(dd / secs), busy);
        fflush(g_perf);
    }
    s_sumLogic = s_sumFrame = 0;
    s_sumUsage = 0;
    s_frames = 0;
    s_window = t;
}
void trace(const char* fmt, ...) {
    static FILE* f = fopen("sdmc:/gc3ds/native/trace.txt", "w");
    if (!f) return;
    va_list ap;
    va_start(ap, fmt);
    fprintf(f, "%9.3f ", (double)svcGetSystemTick() / SYSCLOCK_ARM11);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
}
void trace_memory(const char* where) {
    // The largest block malloc still gives (touching its first and last byte, so a lazily backed heap cannot fake it),
    // the heap's own accounting, and the linear heap.
    u32 lo = 0, hi = 256u << 20;
    while (hi - lo > (256u << 10)) {
        u32 mid = lo + (hi - lo) / 2;
        volatile u8* p = (volatile u8*)malloc(mid);
        if (p) {
            p[0] = 1;
            p[mid - 1] = 1;
            free((void*)p);
            lo = mid;
        } else
            hi = mid;
    }
    struct mallinfo mi = mallinfo();
    trace("memory %s: largest malloc block %.1f MB; heap arena %.1f MB, in use %.1f MB; linear free %.1f MB", where,
          lo / 1048576.0, mi.arena / 1048576.0, mi.uordblks / 1048576.0, linearSpaceFree() / 1048576.0);
}
[[noreturn]] void fatal(const char* message) {
    printf("\nnative mode: %s\nSTART: exit%s\n", message, g_returnPath[0] ? " (back to gc3ds by itself in 10 s)" : "");
    if (g_perf) {
        fprintf(g_perf, "# fatal: %s\n", message);
        fflush(g_perf);
    }
    native::trace("fatal: %s", message);
    u64 until = osGetTime() + 10000;
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        if (g_returnPath[0] && osGetTime() >= until) break;  // an automated run: back to gc3ds, which posts the logs
        gspWaitForVBlank();
    }
    if (g_returnPath[0]) return_to(g_returnPath);
    gfxExit();
    exit(1);
}
}  // namespace native

int main(int argc, char** argv) {
    osSetSpeedupEnable(true);
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, nullptr);
    printf("gc3ds native mode: Twilight Princess\n");
    g_startMs = osGetTime();
    parse_args(argc, argv);
    if (!find_disc()) native::fatal("no Twilight Princess image under sdmc:/gc3ds or sdmc:/gc3ds/games");
    mkdir("sdmc:/gc3ds", 0777);
    mkdir("sdmc:/gc3ds/native", 0777);
    g_perf = fopen("sdmc:/gc3ds/native/perf.csv", "w");
    if (g_perf) fprintf(g_perf, "# disc %s\nreal_s,fps,logic_ms,frame_ms,gx_bytes_per_frame,drawdone_per_s,busy_pct\n", g_disc);
    native::trace_memory("start");
    native::trace("run control: %u s, return to %s, %d presses", (unsigned)g_seconds, g_returnPath[0] ? g_returnPath : "-", g_nPress);
    {  // std::thread on this toolchain: does a thread start and finish?
        native::trace("std::thread test: creating");
        volatile bool ran = false;
        std::thread t([&ran] { ran = true; });
        native::trace("std::thread test: created");
        t.join();
        native::trace("std::thread test: joined, ran=%d", (int)ran);
    }
    char* args[] = {argv && argc > 0 ? argv[0] : (char*)"gc3ds-native", g_disc, nullptr};
    int rc = game_main(2, args);
    if (g_perf) fclose(g_perf);
    if (g_returnPath[0]) native::trace(return_to(g_returnPath) ? "returning to %s" : "cannot return to %s (hb:ldr)", g_returnPath);
    gfxExit();
    return rc;
}
