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
// dusk/audio.h's DUSK_AUDIO_SKIP on the 3DS (patch_dusk.sh): every mDoAud_* call returns its default while this is false.
bool gc3ds_native_audio_enabled();
bool native_player_pos(float* x, float* y, float* z);  // m_Do_main_3ds.cpp
int native_event_running();
namespace native {
extern std::atomic<uint32_t> g_fifoBytes;  // aurora_3ds.cpp
extern std::atomic<uint32_t> g_drawDones;
extern uint64_t g_procTicks;  // aurora_3ds.cpp: the GX stream consumer's time
extern uint64_t g_dlBytes, g_dlTicks, g_dlCalls;  // aurora_3ds.cpp: display-list copies into the stream
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

// --gxopt LETTERS: the GX pipeline's options (platform/gx/gx_3ds.c), as sdmc:/gc3ds/native/gxopt.txt gives them.
extern "C" char g_gxopt[32];
char g_gxopt[32];
static void parse_args(int argc, char** argv) {
    for (int i = 1; i < argc; i++) {
        if (!argv[i]) continue;
        if (!strcmp(argv[i], "--seconds") && i + 1 < argc) g_seconds = (u32)strtoul(argv[++i], nullptr, 10);
        else if (!strcmp(argv[i], "--return") && i + 1 < argc) snprintf(g_returnPath, sizeof g_returnPath, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--press") && i + 1 < argc) parse_press(argv[++i]);
        else if (!strcmp(argv[i], "--gxopt") && i + 1 < argc) snprintf(g_gxopt, sizeof g_gxopt, "%s", argv[++i]);
        else if (!strcmp(argv[i], "--stage") && i + 1 < argc) {
            // Dusklight's boot-to-stage (its --stage NAME,room,point,layer): the logo scene starts a blank save in that
            // stage's play scene, no title or file select (e.g. F_SP121,0,0 for Hyrule Field).
            char name[32] = "";
            int room = 0, point = 0, layer = -1;
            if (sscanf(argv[++i], "%31[^,],%d,%d,%d", name, &room, &point, &layer) >= 1 && name[0])
                dusk::StageRequested = {name, true, (s8)room, (s16)point, (s8)layer};
        }
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
}  // namespace native
bool gc3ds_native_audio_enabled() { return native::audio_enabled(); }
extern "C" void native_trace_c(const char* text) { native::trace("%s", text); }
extern "C" void gc3ds_gx_stats(char* out, u32 size);  // platform/gx/gx_3ds.c
namespace native {
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
    double busy = s_sumUsage / s_frames;
    double procMs = ticks_to_us(g_procTicks) / 1000.0 / s_frames;  // per frame
    g_procTicks = 0;
    double dlMs = ticks_to_us(g_dlTicks) / 1000.0 / s_frames, dlKb = g_dlBytes / 1024.0 / s_frames,
           dlCalls = (double)g_dlCalls / s_frames;  // per frame
    g_dlTicks = g_dlBytes = g_dlCalls = 0;
    static char gxStats[640];  // gc3ds's GX: draws, vertices, Aurora command counts (gx_3ds.c)
    gc3ds_gx_stats(gxStats, sizeof gxStats);
    float px = 0, py = 0, pz = 0;
    native_player_pos(&px, &py, &pz);  // % of each frame the game worked (Dusklight's frame limiter)
    printf("\x1b[2;1H%5.1f fps  logic %6.2f ms  frame %6.2f ms   \n GX %6u KB/frame  draw-done %u/s   \n busy %5.1f%% of the frame (%5.2f ms)   \n",
           s_frames / secs, logicMs, frameMs, (unsigned)(bytes / s_frames / 1024), (unsigned)(dd / secs), busy, busy * frameMs / 100.0);
    if (g_perf) {
        fprintf(g_perf, "%.1f,%.2f,%.2f,%.2f,%u,%u,%.1f,%.0f,%.0f,%.0f,%lu,%d,%.2f,%.2f,%.1f,%.0f,%s\n", (double)ticks_to_us(t) / 1e6, s_frames / secs, logicMs, frameMs,
                (unsigned)(bytes / s_frames), (unsigned)(dd / secs), busy, px, py, pz, (unsigned long)g_frameNo, native_event_running(), procMs, dlMs, dlKb, dlCalls, gxStats);
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
    static LightLock lock = [] { LightLock l; LightLock_Init(&l); return l; }();  // the GX thread reports here too
    LightLock_Lock(&lock);
    va_list ap;
    va_start(ap, fmt);
    fprintf(f, "%9.3f ", (double)svcGetSystemTick() / SYSCLOCK_ARM11);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
    LightLock_Unlock(&lock);
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

// Crashes on the game's main thread go here instead of Luma's exception screen, so an automated run (gc3ds's --native
// job) comes back by itself: the handler (on its own stack, in the exception's context) only copies the registers and
// sends the thread on to crash_exit, which writes sdmc:/gc3ds/native/crash.txt, hands the 3DS back to gc3ds (--return)
// and ends the process; gc3ds posts crash.txt with the run's logs. A second exception, or one on another thread, ends
// the default way (Luma's screen). The 2DS's first run (2026-10-15) stopped at that screen until James pressed a key.
static u8 g_exStack[0x2000] __attribute__((aligned(8)));
static u8 g_exitStack[0x8000] __attribute__((aligned(8)));
static ERRF_ExceptionInfo g_exInfo;
static CpuRegisters g_exRegs;
static volatile bool g_crashed;
extern "C" void native_crash_resume(CpuRegisters* r) __attribute__((noreturn));
__asm__(".text; .arm; .align 2; .global native_crash_resume; .type native_crash_resume, %function;"
        "native_crash_resume: ldr r1, [r0, #64]; msr cpsr_f, r1; ldmia r0, {r0-r15};");
static bool readable(u32 a) { return (a >= 0x00100000u && a < 0x10000000u) || (a >= 0x14000000u && a < 0x40000000u); }

static void crash_exit() {
    threadOnException(nullptr, nullptr, nullptr);  // a fault from here on: Luma's screen
    static const char* const kType[] = {"prefetch abort", "data abort", "undefined instruction", "VFP exception"};
    FILE* f = fopen("sdmc:/gc3ds/native/crash.txt", "w");
    if (f) {
        const ERRF_ExceptionInfo& e = g_exInfo;
        const CpuRegisters& r = g_exRegs;
        fprintf(f, "crash: %s on the main thread at pc %08lx lr %08lx sp %08lx cpsr %08lx far %08lx fsr %08lx fpexc %08lx\n",
                (unsigned)e.type < 4 ? kType[e.type] : "exception", (unsigned long)r.pc, (unsigned long)r.lr, (unsigned long)r.sp,
                (unsigned long)r.cpsr, (unsigned long)e.far, (unsigned long)e.fsr, (unsigned long)e.fpexc);
        fprintf(f, "registers:");
        for (int i = 0; i < 13; i++) fprintf(f, " r%d=%08lx", i, (unsigned long)r.r[i]);
        fprintf(f, "\nstack:");
        u32 sp = r.sp & ~3u;
        for (int i = 0; i < 48 && readable(sp + 4 * i); i++) fprintf(f, "%s%08lx", i % 8 ? " " : "\n  ", (unsigned long)((const u32*)sp)[i]);
        fprintf(f, "\nbuild %s %s (decode with addr2line on /root/native/build/trial.elf)\n", __DATE__, __TIME__);
        fclose(f);
    }
    native::trace("crash: pc %08lx lr %08lx far %08lx (crash.txt)", (unsigned long)g_exRegs.pc, (unsigned long)g_exRegs.lr,
                  (unsigned long)g_exInfo.far);
    if (g_perf) fflush(g_perf);
    if (g_returnPath[0]) return_to(g_returnPath);
    exit(1);
}

static void on_exception(ERRF_ExceptionInfo* ex, CpuRegisters* r) {
    if (g_crashed) {  // crash_exit itself faulted: re-run the instruction without a handler (Luma's screen)
        threadOnException(nullptr, nullptr, nullptr);
        native_crash_resume(r);
    }
    g_crashed = true;
    g_exInfo = *ex;
    g_exRegs = *r;
    r->pc = (u32)(uintptr_t)crash_exit;
    r->sp = (u32)(uintptr_t)(g_exitStack + sizeof g_exitStack);
    native_crash_resume(r);
}

// A hang on an automated run (--return): no new game frame for 20 s once the loop has started (a GPU that stopped, a
// wait that never ends) ends the run as a crash does, crash.txt saying so, and the 2DS goes back to gc3ds without hands.
// It leaves without the game's static destructors (threads that may be the stuck ones): the chainload, then the exit.
static void watchdog(void*) {
    u32 last = 0, still = 0;
    for (;;) {
        svcSleepThread(1000000000LL);
        u32 f = g_frameNo;
        if (!f || f != last) { last = f; still = 0; continue; }
        if (++still < 20) continue;
        FILE* c = fopen("sdmc:/gc3ds/native/crash.txt", "w");
        if (c) {
            fprintf(c, "hang: no game frame for 20 s at frame %lu (watchdog)\n", (unsigned long)f);
            fclose(c);
        }
        if (g_perf) fflush(g_perf);
        native::trace("hang: no game frame for 20 s at frame %lu: back to gc3ds", (unsigned long)f);
        if (g_returnPath[0]) return_to(g_returnPath);
        aptExit();
        svcExitProcess();
    }
}

// The GX thread and the vertex worker (gc3ds's host3ds/crashlog.h hook, called in each): the same handler, so a fault
// there also writes crash.txt and returns to gc3ds.
extern "C" void crashlog_thread(int) { threadOnException(on_exception, g_exStack + sizeof g_exStack, WRITE_DATA_TO_HANDLER_STACK); }

int main(int argc, char** argv) {
    threadOnException(on_exception, g_exStack + sizeof g_exStack, WRITE_DATA_TO_HANDLER_STACK);
    remove("sdmc:/gc3ds/native/crash.txt");
    osSetSpeedupEnable(true);
    gfxInitDefault();
    consoleInit(GFX_BOTTOM, nullptr);
    printf("gc3ds native mode: Twilight Princess\n");
    g_startMs = osGetTime();
    parse_args(argc, argv);
    if (argc <= 1) {  // started by hand (or in Azahar, which passes none): sdmc:/gc3ds/native/args.txt, if there
        static char text[512];
        static char* av[32];
        FILE* af = fopen("sdmc:/gc3ds/native/args.txt", "r");
        if (af) {
            size_t n = fread(text, 1, sizeof text - 1, af);
            fclose(af);
            text[n] = 0;
            int ac = 0;
            av[ac++] = argv && argc > 0 ? argv[0] : (char*)"gc3ds-native";
            for (char* tok = strtok(text, " \t\r\n"); tok && ac < 31; tok = strtok(nullptr, " \t\r\n")) av[ac++] = tok;
            parse_args(ac, av);
        }
    }
    if (!find_disc()) native::fatal("no Twilight Princess image under sdmc:/gc3ds or sdmc:/gc3ds/games");
    mkdir("sdmc:/gc3ds", 0777);
    mkdir("sdmc:/gc3ds/native", 0777);
    g_perf = fopen("sdmc:/gc3ds/native/perf.csv", "w");
    if (g_perf) fprintf(g_perf, "# disc %s\nreal_s,fps,logic_ms,frame_ms,gx_bytes_per_frame,drawdone_per_s,busy_pct,link_x,link_y,link_z,frame,event,gx_proc_ms,dl_copy_ms,dl_kb,dl_calls,gx\n", g_disc);
    native::trace_memory("start");
    if (g_returnPath[0]) threadCreate(watchdog, nullptr, 8192, 0x20, -2, true);  // core 0, above the game's priority
    native::trace("run control: %u s, return to %s, %d presses, stage %s", (unsigned)g_seconds, g_returnPath[0] ? g_returnPath : "-",
                  g_nPress, dusk::StageRequested.set ? dusk::StageRequested.stage.c_str() : "-");
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
