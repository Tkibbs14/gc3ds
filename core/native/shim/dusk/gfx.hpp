// Native mode (gc3ds): Dusklight's mod render stages without WebGPU (no mods on the 3DS; the calls do nothing).
#pragma once
class view_class;
class view_port_class;
typedef enum GfxStage {
    GFX_STAGE_SCENE_AFTER_TERRAIN = 0,
    GFX_STAGE_FRAME_BEFORE_HUD = 1,
    GFX_STAGE_FRAME_AFTER_HUD = 2,
    GFX_STAGE_SCENE_BEGIN = 3,
    GFX_STAGE_SCENE_AFTER_OPAQUE = 4,
} GfxStage;
namespace dusk::mods {
inline void gfx_run_stage(GfxStage, const view_class* = nullptr, const view_port_class* = nullptr) {}
}  // namespace dusk::mods
