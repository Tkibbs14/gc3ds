// Native mode (gc3ds): no ImGui debug console on the 3DS. The game's debug keys (ImGui::IsKeyDown) read as up
// (core/native/platform defines the few ImGui functions used); imgui.h is Dear ImGui's own (v1.91.9b-docking).
#pragma once
#include <imgui.h>
namespace dusk {
class ImGuiConsole {
public:
    void UpdateSettings() {}
    void PreDraw() {}
    void PostDraw() {}
    static bool CheckMenuViewToggle(ImGuiKey, bool&) { return false; }
};
extern ImGuiConsole g_imguiConsole;
}  // namespace dusk
