// Native mode (gc3ds): Dusklight's RmlUi menus are not built for the 3DS; this keeps the interface the game code calls
// (core/native/platform defines the functions: no documents, nothing visible).
#pragma once
#include "dolphin/types.h"
#include <chrono>
#include <memory>
#include <string>
union SDL_Event;
namespace dusk::ui {
enum class DocumentScope : u8 { None, CommandConsole, Prelaunch, Window, MenuBar, Overlay, TouchControls, GraphicsTuner };
class Document {
public:
    Document() = default;
    virtual ~Document() = default;
    virtual void show() {}
    virtual void hide(bool) {}
    virtual void update() {}
    virtual bool focus() { return false; }
    virtual bool visible() const { return false; }
};
struct Toast {
    std::string type, title, content;
    std::chrono::steady_clock::duration duration{};
    std::string modId;
};
void push_toast(Toast toast) noexcept;
bool initialize() noexcept;
void shutdown() noexcept;
void handle_event(const SDL_Event& event) noexcept;
void update() noexcept;
Document& push_document(std::unique_ptr<Document> doc, bool show = true, bool passive = false) noexcept;
bool any_document_visible() noexcept;
bool is_prelaunch_open() noexcept;
void apply_scale() noexcept;
}  // namespace dusk::ui
