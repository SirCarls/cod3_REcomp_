// cod2 / cod2mp - Call of Duty 2's F2 settings window: its pages and colors on the shared window (../recomp/src/f2/).
// [f2-menus]
//
// Owner 2026-11-08 12:12: "yes we want an f2 menu for all these games"; 2026-11-10 15:57: "cod2 should copy what the
// PC copy has." (asked: the PC copy's options and dvars first, then its keys and mouse, then its movement feel).
//
// The window is built when the project's CMakeLists finds the shared window's sources (COD3_RECOMP_SRC, by default
// ../recomp/src next to this project) and defines COD2_HAVE_F2. A work copy of recomp_cod2 alone, without that
// folder, still builds: these functions then do nothing, and the game has no F2 window.

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

#if defined(COD2_HAVE_F2)

namespace cod2::f2ui {

// Cod2App's hooks (the UI thread).
void ConfigureFonts(ImFontAtlas* atlas);
void ConfigureStyle(ImGuiStyle& imgui_style, rex::ui::Style& ui_style);
void Attach(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window, std::filesystem::path config_path,
            std::function<void(std::function<void()>)> defer);
void Detach();  // shutdown: saves what changed, before the ImGui drawer goes

// The game's input active callback: false while the F2 window is open (and while another Dear ImGui window has the
// mouse or a text box).
bool GameGetsInput();

// The game's thread:
// every line the game's console prints (Com_PrintMessage's hook, game/diagnostics.cpp), for the CONSOLE page;
void NoteConsoleLine(int channel, std::string_view line);
// the commands F2 wants the game's console to run (typed on the CONSOLE page, or a row that sets a dvar), taken on
// the game's main thread (game/test_start.cpp's controller-read hook) and added with Cbuf_AddText.
bool HasCommands();
void TakeCommands(std::vector<std::string>& out);

}  // namespace cod2::f2ui

#else  // !COD2_HAVE_F2: no shared window in this build

namespace cod2::f2ui {

inline void ConfigureFonts(ImFontAtlas*) {}
inline void ConfigureStyle(ImGuiStyle&, rex::ui::Style&) {}
inline void Attach(rex::ui::ImGuiDrawer*, rex::ui::Window*, std::filesystem::path,
                   std::function<void(std::function<void()>)>) {}
inline void Detach() {}
inline bool GameGetsInput() { return true; }
inline void NoteConsoleLine(int, std::string_view) {}
inline bool HasCommands() { return false; }
inline void TakeCommands(std::vector<std::string>&) {}

}  // namespace cod2::f2ui

#endif
