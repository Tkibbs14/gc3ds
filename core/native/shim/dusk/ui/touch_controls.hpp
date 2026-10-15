// Native mode (gc3ds): the touch-control interface the game code calls (no on-screen controls yet).
#pragma once
#include "dusk/ui/ui.hpp"
#include "dusk/ui/controls.hpp"
namespace dusk::ui {
enum class ControlOverride { Default, Action };
bool get_equip_target(int slot, EquipTarget& target) noexcept;
void set_control_override(Control control, ControlOverride override) noexcept;
void sync_virtual_input() noexcept;
class TouchControls final : public Document {};
}  // namespace dusk::ui
