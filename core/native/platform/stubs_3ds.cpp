// gc3ds native mode: the remaining pieces of Dusklight's PC layer the game calls, as they behave when the feature is
// absent: no touch / mouse menu pointer (nothing is ever clicked), no rebindable actions beyond the game's defaults,
// no mods (the loader is never started), no ImGui bloom tuning, no gamepad LEDs, no LiveSplit connection. Logging goes
// to sdmc:/gc3ds/native/log.txt.
#include "dusk/action_bindings.h"
#include "dusk/gamepad_color.h"
#include "dusk/menu_pointer.h"
#include "dusk/mod_loader.hpp"
#include "dusk/mods/loader/loader.hpp"

#include "JSystem/JAWWinLib/JAWHioReceiver.h"
#include "JSystem/JKernel/JKRHeap.h"

#include <borealis/io.hpp>
#include <borealis/log.hpp>
#include <borealis/net.hpp>

#include <cstdio>
#include <cstdlib>

class J2DPane;

// ---- menu pointer: none ----
namespace dusk::menu_pointer {
static State s_state;
void begin_game_frame() noexcept {}
void end_game_frame() noexcept {}
void begin_context(Context) noexcept {}
const State& state() noexcept { return s_state; }
void set_hover_target(TargetId) noexcept {}
bool consume_click() noexcept { return false; }
bool peek_click() noexcept { return false; }
void set_dialog_choice(u8, bool) noexcept {}
bool get_dialog_choice(u8&) noexcept { return false; }
bool consume_dialog_click(u8&) noexcept { return false; }
void defer_activation(Context, TargetId) noexcept {}
bool consume_deferred_activation(Context, TargetId) noexcept { return false; }
void clear_deferred_activation(Context) noexcept {}
u32 suppressed_pad_buttons(u32) noexcept { return 0; }
void finish_pad_suppression_read(u32) noexcept {}
bool hit_pane(CPaneMgr*, f32) noexcept { return false; }
}  // namespace dusk::menu_pointer

// ---- action bindings: only the game's own controls ----
namespace dusk {
bool isActionBound(ActionBinds, u32) { return false; }
bool getActionBindTrig(ActionBinds, u32) { return false; }
void updateActionBindings() {}
void ApplyBloomOverride() {}
}  // namespace dusk
namespace dusk::input {
void handleGamepadColor() {}
}
namespace dusk::ui {
void update_midna_icon_texture(J2DPane*) noexcept {}
}
namespace dusk::audio {
void Initialize() {}
}

// ---- mods: the loader is never started ----
// A loaded native mod's handle (mods/loader); never created here, defined only so ModLoader can be destroyed.
namespace dusk::mods::loader {
class NativeModule {};
}
namespace dusk::mods {
ModLoader& ModLoader::instance() {
    static ModLoader loader;
    return loader;
}
void ModLoader::tick() {}
void fail_mod(LoadedMod&, ModResult, std::string_view) {}
}  // namespace dusk::mods

// ---- odds and ends of the game's debug / host-IO paths ----
bool JKRHeap::dump_sort() { return true; }  // as Dusklight's m_Do_main.cpp
bool JAWHioReceiver::TSeqList::getSeqData(JAISoundID, JAISeqData*) const { return false; }  // no host-IO sequences
namespace ImGui {
void MemFree(void* p) { free(p); }
}

// ---- borealis: logging to the SD card, file replacement, no network ----
namespace borealis::log::detail {
static FILE* log_file() {
    static FILE* f = fopen("sdmc:/gc3ds/native/log.txt", "w");
    return f;
}
bool enabled(LogLevel level, std::string_view) noexcept { return level >= LogLevel::Info; }
void write(LogLevel level, std::string_view module, std::string_view text) {
    static const char* const kLevel[] = {"trace", "debug", "info", "warn", "error", "fatal"};
    if (FILE* f = log_file()) {
        fprintf(f, "[%s] %.*s: %.*s\n", kLevel[(int)level], (int)module.size(), module.data(), (int)text.size(), text.data());
        fflush(f);
    }
}
[[noreturn]] void write_fatal(std::string_view module, std::string_view text) {
    write(LogLevel::Fatal, module, text);
    printf("\nfatal: %.*s\n", (int)text.size(), text.data());
    abort();
}
}  // namespace borealis::log::detail
namespace borealis::io {
bool atomic_replace(const std::filesystem::path& source, const std::filesystem::path& destination, std::string& error) {
    std::remove(destination.c_str());
    if (std::rename(source.c_str(), destination.c_str()) == 0) return true;
    error = "rename failed";
    return false;
}
}  // namespace borealis::io
namespace borealis::net {
struct Context::Impl {};  // no network on the 3DS: LiveSplit's context is never connected
Context::~Context() = default;
}  // namespace borealis::net
