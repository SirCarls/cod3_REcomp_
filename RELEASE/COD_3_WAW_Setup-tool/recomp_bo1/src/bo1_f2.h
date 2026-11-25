// bo1 / bo1mp - Black Ops' F2 settings window: Call of Duty 3's window (../recomp/src/f2/) on this port. [menus-native]
//
// Owner 2026-11-24 15:55: "Start working all of what can be ported from our cod3 f2 menu to the other games." First
// step: every page whose rows edit the port's or the SDK's own settings (VIDEO, SOUND, GAME's player and language
// rows, CONTROLS, INTERFACE, PERFORMANCE, EMULATION). Pages that need hooks in the game's code (CROSSHAIR, CONSOLE,
// the game's own options) come once Black Ops' hooks for them exist (dev/agents/waw-decomp/BLACK-OPS.md).

#pragma once

#include <filesystem>
#include <functional>

struct ImFontAtlas;
struct ImGuiStyle;

namespace rex::ui {
class ImGuiDrawer;
class Window;
struct Style;
}  // namespace rex::ui

namespace bo1::f2ui {

// Bo1App's hooks (the UI thread).
void ConfigureFonts(ImFontAtlas* atlas);
void ConfigureStyle(ImGuiStyle& imgui_style, rex::ui::Style& ui_style);
void Attach(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window, std::filesystem::path config_path,
            std::function<void(std::function<void()>)> defer);
void Detach();  // shutdown: saves what changed, before the ImGui drawer goes

// The game's input active callback: false while the F2 window is open (and while another Dear ImGui window has the
// mouse or a text box).
bool GameGetsInput();

}  // namespace bo1::f2ui
