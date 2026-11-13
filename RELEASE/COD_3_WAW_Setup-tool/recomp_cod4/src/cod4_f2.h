// cod4 / cod4mp - Call of Duty 4's F2 settings window: its pages and colors on the shared window (../recomp/src/f2/).
// [f2-menus]
//
// Owner 2026-11-08 12:12: "yes we want an f2 menu for all these games"; 2026-11-09 20:05: Call of Duty 3's window
// in each game's own colors, Back + Start held 1 s on a controller, World at War first, then Call of Duty 4.

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

namespace cod4::f2ui {

// Cod4App's hooks (the UI thread).
void ConfigureFonts(ImFontAtlas* atlas);
void ConfigureStyle(ImGuiStyle& imgui_style, rex::ui::Style& ui_style);
void Attach(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window, std::filesystem::path config_path,
            std::function<void(std::function<void()>)> defer);
void Detach();  // shutdown: saves what changed, before the ImGui drawer goes

// The game's input active callback: false while the F2 window is open (and while another Dear ImGui window has the
// mouse or a text box). The mouse look (cod4_mouse.cpp) lets the pointer go while it is false.
bool GameGetsInput();

// The game's thread (cod4_hooks.cpp):
// every line the game's console prints (CL_ConsolePrint's hook), for the CONSOLE page;
void NoteConsoleLine(int channel, std::string_view line);
// the commands F2 wants the game's console to run (typed on the CONSOLE page, or a row that sets a dvar), taken at
// the start of each frame and queued through Cbuf_AddText. HasCommands is a cheap check for every frame.
bool HasCommands();
void TakeCommands(std::vector<std::string>& out);

// [f2-menus] 2026-11-11 04:22 The frame rate limit (cod4_max_fps; 0 = none): called first in the game's frame hook, on
// the game's thread, it waits until this frame's time on the host's clock, as World at War's waw_frame_start does.
// (Until now the row only set the game's com_maxfps, which this program takes and does not act on: runs fpsiw0404
// and fpsiw0412 stayed at 60 frames a second with it at 30.)
void LimitFrameRate();

}  // namespace cod4::f2ui
