// waw - World at War's F2 settings window: its pages and colors on the shared window (../recomp/src/f2/).
// [f2-menus]
//
// Owner 2026-11-08 12:12: "yes we want an f2 menu for all these games"; 2026-11-09 20:05: Call of Duty 3's window
// in World at War's own colors, Back + Start held 1 s on a controller, World at War first.

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

struct ImFontAtlas;
struct ImGuiStyle;

namespace rex::ui {
class ImGuiDrawer;
class Window;
struct Style;
}  // namespace rex::ui

namespace waw::f2ui {

// WawApp's hooks (the UI thread).
void ConfigureFonts(ImFontAtlas* atlas);
void ConfigureStyle(ImGuiStyle& imgui_style, rex::ui::Style& ui_style);
void Attach(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window, std::filesystem::path config_path,
            std::function<void(std::function<void()>)> defer);
void Detach();  // shutdown: saves what changed, before the ImGui drawer goes

// The game's input active callback: false while the F2 window is open (and while another Dear ImGui window has the
// mouse or a text box).
bool GameGetsInput();

// The game's thread (waw_hooks.cpp):
// every line the game's console prints (Com_PrintMessage's hook), for the CONSOLE page;
void NoteConsoleLine(int channel, std::string_view line);
// the commands F2 wants the game's console to run (typed on the CONSOLE page, or a row that sets a dvar), taken at
// the start of each frame and queued through Cbuf_AddText. HasCommands is a cheap check for every frame.
bool HasCommands();
void TakeCommands(std::vector<std::string>& out);

}  // namespace waw::f2ui
