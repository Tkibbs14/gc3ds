// gc3ds native mode: Aurora's platform half on the 3DS (built with Aurora's flags and headers). Aurora's own aurora.cpp
// creates an SDL window and a WebGPU device; here there is neither. The GX API (lib/dolphin/gx) and the FIFO
// (lib/gx/fifo.cpp, compiled unchanged, its worker thread included) stay Aurora's; the consumer behind the FIFO,
// process(), is gc3ds's.
//
// Feasibility build: process() only finds draw-done markers (so the game's draw-done callbacks and GXDrawDone waits
// behave) and counts the stream; nothing is drawn yet. Step 2 hands the stream to gc3ds's GX pipeline instead.
#include <3ds.h>

#include "internal.hpp"
#include "thread.hpp"
#include "gx/command_processor.hpp"
#include "gx/fifo.hpp"

#include <aurora/aurora.h>
#include <aurora/event.h>
#include <dolphin/gx.h>
#include <aurora/texture.hpp>

#include <atomic>
#include <cstdlib>
#include <cstring>

namespace aurora {
AuroraConfig g_config;
char g_gameName[4];
}  // namespace aurora

namespace native {
std::atomic<uint32_t> g_fifoBytes{0};  // GX stream bytes consumed since the last read (frame statistics)
std::atomic<uint32_t> g_drawDones{0};
uint64_t g_procTicks;  // ticks spent in process() (Inline mode: on the game's thread)
}  // namespace native

static char* dup_or(const char* s, const char* fallback) { return strdup(s ? s : fallback); }

namespace native {
void trace(const char* fmt, ...);
}
// Aurora's log (with no callback it goes to stderr, which the 3DS does not show) into native mode's trace file.
static void log_to_trace(AuroraLogLevel level, const char* module, const char* message, unsigned int len) {
    static const char* const kLevel[] = {"debug", "info", "warning", "error", "fatal"};
    native::trace("aurora %s [%s] %.*s", (unsigned)level < 5 ? kLevel[level] : "?", module ? module : "", (int)len, message);
}

AuroraInfo aurora_initialize(int, char*[], const AuroraConfig* config) {
    using namespace aurora;
    g_config = *config;
    if (!g_config.logCallback) g_config.logCallback = log_to_trace;
    g_config.appName = dup_or(config->appName, "gc3ds");
    g_config.userPath = dup_or(config->userPath, "sdmc:/gc3ds/native/");
    g_config.cachePath = dup_or(config->cachePath, "sdmc:/gc3ds/native/cache/");
    g_config.resourcesPath = dup_or(config->resourcesPath, "romfs:/");
    if (g_config.msaa == 0) g_config.msaa = 1;
    thread::set_current({.name = "Main thread"});
    gx::fifo::init();
    AuroraInfo info{};
    info.backend = BACKEND_NULL;
    info.userPath = g_config.userPath;
    info.cachePath = g_config.cachePath;
    info.window = nullptr;
    info.windowSize.width = info.windowSize.fb_width = info.windowSize.native_fb_width = 400;
    info.windowSize.height = info.windowSize.fb_height = info.windowSize.native_fb_height = 240;
    info.windowSize.scale = 1.0f;
    return info;
}

void aurora_shutdown() { aurora::gx::fifo::shutdown(); }

const AuroraEvent* aurora_update() {
    static AuroraEvent none{};
    none.type = AURORA_NONE;
    return &none;
}

bool aurora_begin_frame() {
    aurora::gx::fifo::begin_frame();
    return true;
}

void aurora_end_frame() { aurora::gx::fifo::end_frame(); }

namespace aurora::thread {
// libctru threads are named nowhere and placed at creation; the FIFO worker stays where std::thread puts it for now.
void set_current(const Options&) noexcept {}
}  // namespace aurora::thread

// Display-list copies (GXCallDisplayList, patched in patch_dusk.sh): bytes and ticks, read by native_3ds.cpp each second.
namespace native {
uint64_t g_dlBytes, g_dlTicks, g_dlCalls;
}
extern "C" uint64_t gc3ds_ticks(void) { return svcGetSystemTick(); }
extern "C" void gc3ds_dl_account(uint32_t bytes, uint64_t ticks) {
    native::g_dlBytes += bytes;
    native::g_dlTicks += ticks;
    native::g_dlCalls++;
}

namespace aurora::gx::fifo {
// The draw-done marker GXSetDrawDone / GXDrawDone write: a raster register write (0x61) of BP 0x45, value 2.
static constexpr uint8_t kDrawDone[5] = {0x61, 0x45, 0x00, 0x00, 0x02};

ProcessResult process(const uint8_t* data, uint32_t size) noexcept {
    const u64 t0 = svcGetSystemTick();  // native::g_procTicks: the consumer's share of the frame (perf.csv)
    const uint8_t* end = data + size;
    for (const uint8_t* p = data; p + sizeof kDrawDone <= end;) {
        p = (const uint8_t*)memchr(p, 0x61, (size_t)(end - p) - (sizeof kDrawDone - 1));
        if (!p) break;
        if (memcmp(p, kDrawDone, sizeof kDrawDone) == 0) {
            uint32_t n = (uint32_t)(p - data) + (uint32_t)sizeof kDrawDone;
            native::g_fifoBytes += n;
            native::g_drawDones++;
            native::g_procTicks += svcGetSystemTick() - t0;
            return {n, true};
        }
        p++;
    }
    native::g_fifoBytes += size;
    native::g_procTicks += svcGetSystemTick() - t0;
    return {size, false};
}
void clear_draw_cache() noexcept {}
}  // namespace aurora::gx::fifo

// No texture replacements on the 3DS.
namespace aurora::texture {
bool has_replacement(const GXTexObj*, const GXTlutObj*) { return false; }
ReplacementRegistration register_replacement(ReplacementKey, RawTextureReplacement, ReplacementOptions) { return {}; }
void unregister_replacement(const ReplacementRegistration&) {}
void unregister_replacements(const ReplacementGroup&) {}
ReplacementGroup load_replacement_directory(const std::filesystem::path&, ReplacementOptions) { return {}; }
}  // namespace aurora::texture

// Settings that only mean something with a window and a GPU resampler.
void aurora_set_pause_on_focus_lost(bool) {}
void aurora_set_resampler(AuroraSampler) {}

// Depth readback (the game's lens-flare occlusion test through Aurora's renderer): not available until the renderer
// is gc3ds's; "not read" makes the caller keep its previous answer.
namespace aurora::gfx::depth_peek {
bool read_latest(uint16_t, uint16_t, uint32_t&) noexcept { return false; }
}
