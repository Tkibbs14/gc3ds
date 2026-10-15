// gc3ds native mode: the parts of Dusklight's PC layer the game code still calls, as the 3DS has them. Declared by the
// stand-in headers in core/native/shim: no RmlUi menus or ImGui console (nothing is ever shown and no debug key is
// down), no mouse, no SDL window; the SDL timer is the 3DS's own clock.
#include <3ds.h>

#include "dusk/imgui/ImGuiConsole.hpp"
#include "dusk/mouse.h"
#include "dusk/ui/prelaunch.hpp"
#include "dusk/ui/touch_controls.hpp"
#include "dusk/ui/ui.hpp"

#include <SDL3/SDL_timer.h>
#include <SDL3/SDL_video.h>
#include <aurora/lib/window.hpp>

#include <cstring>

// ---- SDL timer (dusk/time.h's frame limiter) ----
extern "C" Uint64 SDL_GetTicksNS(void) {
    // The ARM11 system tick counts at SYSCLOCK_ARM11 (268,111,856 Hz): ns = ticks * 1e9 / SYSCLOCK_ARM11.
    const u64 t = svcGetSystemTick();
    return (t / SYSCLOCK_ARM11) * 1000000000ull + (t % SYSCLOCK_ARM11) * 1000000000ull / SYSCLOCK_ARM11;
}
extern "C" void SDL_DelayPrecise(Uint64 ns) { svcSleepThread((s64)ns); }

// ---- SDL window (m_Do_graphic.cpp's safe area: there is no window, so it is never asked) ----
extern "C" bool SDL_GetWindowSafeArea(SDL_Window*, SDL_Rect*) { return false; }
namespace aurora::window {
void request_frame_buffer_resize() {}
void set_frame_buffer_scale(float) {}
SDL_Window* get_sdl_window() { return nullptr; }
AuroraWindowSize get_window_size() {
    AuroraWindowSize s{};
    s.width = 400;
    s.height = 240;
    s.fb_width = 400;
    s.fb_height = 240;
    s.native_fb_width = 400;
    s.native_fb_height = 240;
    s.scale = 1.0f;
    return s;
}
}  // namespace aurora::window

// ---- RmlUi menus: none ----
namespace dusk::ui {
static Document g_none;
bool initialize() noexcept { return true; }
void shutdown() noexcept {}
void handle_event(const SDL_Event&) noexcept {}
void update() noexcept {}
Document& push_document(std::unique_ptr<Document>, bool, bool) noexcept { return g_none; }
bool any_document_visible() noexcept { return false; }
bool is_prelaunch_open() noexcept { return false; }
void apply_scale() noexcept {}
void push_toast(Toast) noexcept {}
bool get_equip_target(int, EquipTarget&) noexcept { return false; }
void set_control_override(Control, ControlOverride) noexcept {}
void sync_virtual_input() noexcept {}
PrelaunchState& prelaunch_state() noexcept {
    static PrelaunchState s;
    return s;
}
void return_to_prelaunch() noexcept {}
}  // namespace dusk::ui

// ---- mouse: none ----
namespace dusk::mouse {
void read() {}
void get_aim_deltas(float& yaw, float& pitch) { yaw = pitch = 0.0f; }
void get_camera_deltas(float& yaw, float& pitch) { yaw = pitch = 0.0f; }
void handle_event(const SDL_Event&) noexcept {}
void on_focus_lost() {}
void on_focus_gained() {}
}  // namespace dusk::mouse

// ---- ImGui: no console; the game's debug keys read as up ----
namespace dusk {
ImGuiConsole g_imguiConsole;
}
ImGuiIO::ImGuiIO() { memset((void*)this, 0, sizeof(*this)); }
namespace ImGui {
static ImGuiIO g_io;
ImGuiIO& GetIO() { return g_io; }
bool IsKeyDown(ImGuiKey) { return false; }
bool IsKeyPressed(ImGuiKey, bool) { return false; }
bool IsMouseDown(ImGuiMouseButton) { return false; }
}  // namespace ImGui
