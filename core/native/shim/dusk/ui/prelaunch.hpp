// Native mode (gc3ds): gc3ds's own menu picks the disc, so there is no prelaunch screen; the state the game reads.
#pragma once
#include "dusk/ui/ui.hpp"
namespace dusk::ui {
class Prelaunch : public Document {
public:
    static void refresh_menu_buttons() {}
};
struct PrelaunchState {
    bool returnToPrelaunchOnReset = false;
};
PrelaunchState& prelaunch_state() noexcept;
void return_to_prelaunch() noexcept;
}  // namespace dusk::ui
