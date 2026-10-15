// gc3ds native mode on the 3DS: the process entry point, the platform functions m_Do_main_3ds.cpp needs (native.h),
// and the feasibility measurement. Each second the bottom screen shows, and sdmc:/gc3ds/native/perf.csv records,
// the average milliseconds per frame of the game's own work (logic and GX submission, frame_begin to
// frame_logic_done) and of the whole frame, frames per second, and the GX stream bytes per frame.
#include <3ds.h>

#include "native.h"

#include "dusk/main.h"
#include "dusk/settings.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <strings.h>
#include <sys/stat.h>

int game_main(int argc, char* argv[]);
namespace native {
extern std::atomic<uint32_t> g_fifoBytes;  // aurora_3ds.cpp
extern std::atomic<uint32_t> g_drawDones;
}  // namespace native

static char g_disc[512];
static bool g_exit;
static FILE* g_perf;

static bool contains_nocase(const char* s, const char* word) {
    for (size_t n = strlen(word); *s; s++)
        if (!strncasecmp(s, word, n)) return true;
    return false;
}

// The disc image: the first argument, else the first .iso / .gcm under sdmc:/gc3ds or sdmc:/gc3ds/games whose name
// mentions Twilight (where gc3ds's menu looks).
static bool find_disc(int argc, char** argv) {
    if (argc > 1 && argv[1] && argv[1][0]) {
        snprintf(g_disc, sizeof g_disc, "%s", argv[1]);
        return true;
    }
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
void apply_settings() { dusk::getSettings().game.enableFrameInterpolation.setValue(dusk::FrameInterpMode::Off); }
// The game's heaps: the GameCube had 24 MB of MEM1 (and 16 MB of ARAM, which the game streams through); structures
// are the GameCube's sizes again on a 32-bit host, so the arena is near the original.
uint32_t arena_bytes() { return 48u << 20; }
const char* disc_path() { return g_disc; }
bool audio_enabled() { return false; }
void poll_events() {
    if (!aptMainLoop()) g_exit = true;
    hidScanInput();
    if ((hidKeysHeld() & (KEY_START | KEY_SELECT)) == (KEY_START | KEY_SELECT)) g_exit = true;
}
bool exit_requested() { return g_exit; }

static u64 s_t0, s_tLogic, s_sumLogic, s_sumFrame, s_window;
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
    s_frames++;
    if (t - s_window < SYSCLOCK_ARM11) return;
    double secs = (double)(t - s_window) / SYSCLOCK_ARM11;
    double logicMs = ticks_to_us(s_sumLogic) / 1000.0 / s_frames, frameMs = ticks_to_us(s_sumFrame) / 1000.0 / s_frames;
    uint32_t bytes = g_fifoBytes.exchange(0), dd = g_drawDones.exchange(0);
    printf("\x1b[2;1H%5.1f fps  logic %6.2f ms  frame %6.2f ms   \n GX %6u KB/frame  draw-done %u/s   \n", s_frames / secs,
           logicMs, frameMs, (unsigned)(bytes / s_frames / 1024), (unsigned)(dd / secs));
    if (g_perf) {
        fprintf(g_perf, "%.1f,%.2f,%.2f,%.2f,%u,%u\n", (double)ticks_to_us(t) / 1e6, s_frames / secs, logicMs, frameMs,
                (unsigned)(bytes / s_frames), (unsigned)(dd / secs));
        fflush(g_perf);
    }
    s_sumLogic = s_sumFrame = 0;
    s_frames = 0;
    s_window = t;
}
[[noreturn]] void fatal(const char* message) {
    printf("\nnative mode: %s\nSTART: exit\n", message);
    if (g_perf) {
        fprintf(g_perf, "# fatal: %s\n", message);
        fflush(g_perf);
    }
    while (aptMainLoop()) {
        hidScanInput();
        if (hidKeysDown() & KEY_START) break;
        gspWaitForVBlank();
    }
    gfxExit();
    exit(1);
}
}  // namespace native

int main(int argc, char** argv) {
    osSetSpeedupEnable(true);
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, nullptr);
    printf("gc3ds native mode: Twilight Princess\n");
    if (!find_disc(argc, argv)) native::fatal("no Twilight Princess image under sdmc:/gc3ds or sdmc:/gc3ds/games");
    mkdir("sdmc:/gc3ds", 0777);
    mkdir("sdmc:/gc3ds/native", 0777);
    g_perf = fopen("sdmc:/gc3ds/native/perf.csv", "w");
    if (g_perf) fprintf(g_perf, "# disc %s\nreal_s,fps,logic_ms,frame_ms,gx_bytes_per_frame,drawdone_per_s\n", g_disc);
    char* args[] = {argv && argc > 0 ? argv[0] : (char*)"gc3ds-native", g_disc, nullptr};
    int rc = game_main(2, args);
    if (g_perf) fclose(g_perf);
    gfxExit();
    return rc;
}
