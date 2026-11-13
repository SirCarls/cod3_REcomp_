// f2 - the F2 settings window shared by every game's port. [f2-menus]
//
// The plan: dev/agents/acct2-bro/F2-MENU-PLAN.md (owner 2026-11-08 12:12: "yes we want an f2 menu for all these
// games"; 2026-11-09 20:05: "CoD3's window, own colors", "Back + Start held 1 s", "WaW first, then the rest").
//
// The window is Call of Duty 3's (recomp/src/ui/hud_overlay.cpp, SettingsWindow): the title in bold capitals, a tab
// per page, each page scrolling under the tabs, and a Save row that stays at the bottom ("Save *" while something
// is unsaved, "Restart the game to apply some changes." when a change waits for the next start). Closing it saves
// what changed. Each game gives only its title and its pages; a page draws its rows (f2_rows.h) into a PageContext.
//
// Opening and closing: the F2 key (Overlay::OnKey), or Back + Start held a second on a controller (f2_pad.h). While
// the window is open the game gets no input (the game's input active callback asks GameGetsInput()).

#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <rex/ui/imgui_dialog.h>
#include <rex/ui/window_listener.h>

#include "f2/f2_pad.h"

namespace rex::ui {
class ImGuiDrawer;
class Window;
}  // namespace rex::ui

namespace f2 {

// What a page's rows tell the window.
struct PageContext {
  bool dirty = false;     // something changed since the last save
  bool restart = false;   // a change takes effect at the next start
  bool save_now = false;  // a row asks to be saved at once (the sound output, as Call of Duty 3's)
};

struct Page {
  const char* label;  // the tab, in capitals: VIDEO, SOUND, GAME ...
  std::function<void(PageContext&)> draw;
};

struct WindowSpec {
  std::string title;  // "CALL OF DUTY: WORLD AT WAR"; the window adds "  ·  SETTINGS"
  std::string id;     // a stable Dear ImGui id ("waw_settings"), so the window keeps its place and size
  std::filesystem::path config_path;  // the game's settings file, written by Save
  std::vector<Page> pages;
};

class SettingsWindow : public rex::ui::ImGuiDialog {
 public:
  SettingsWindow(rex::ui::ImGuiDrawer* drawer, WindowSpec spec, PadShortcut* pad, std::function<void()> on_close);
  bool open() const { return open_; }
  void Save();
  void SaveIfDirty();
  // For a change made outside a row (a test script): marks the window unsaved.
  void MarkDirty() { context_.dirty = true; }
  // Shows the page with this label at the next draw (tests); false when there is none.
  bool ShowPage(const std::string& label);

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  WindowSpec spec_;
  PadShortcut* pad_;
  std::function<void()> on_close_;
  PageContext context_;
  std::string status_;
  bool open_ = true;
  bool first_draw_ = true;
  int current_page_ = -1;
  int wanted_page_ = -1;  // a page LB / RB (or a test) chose, selected on the next draw
};

// The always-on part: watches the controllers for Back + Start, feeds them to the window while it is open, shows a
// small "hold to open the settings" hint while the hold fills, and opens / closes the window. One per game, made in
// the game's OnCreateDialogs and gone before the ImGui drawer is.
class Overlay : public rex::ui::ImGuiDialog, public rex::ui::WindowInputListener {
 public:
  struct Options {
    WindowSpec spec;
    // The game's setting that turns the controller shortcut off ("" = always on).
    const char* pad_shortcut_setting = "";
    // Runs a function after the current frame (ReXApp::app_context().CallInUIThreadDeferred): the window is made and
    // destroyed outside the drawer's loop over its dialogs.
    std::function<void(std::function<void()>)> defer;
    // Once a UI frame, before the window draws (the game's test script, settings that follow a change).
    std::function<void(Overlay&)> on_frame;
  };

  Overlay(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window, Options options);
  ~Overlay() override;

  bool WindowOpen() const { return window_ != nullptr; }
  SettingsWindow* settings_window() const { return window_.get(); }
  // From the F2 key, the controller, or a test setting; `why` goes into the log line ("F2 key", "controller").
  void Toggle(const char* why = "");
  void SaveIfDirty();  // shutdown

  // The game's input active callback: false while the window is open, or while another Dear ImGui window (the
  // SDK's own overlays) has the mouse or a text box.
  bool GameGetsInput() const;

  bool WantsContinuousRepaint() const override { return false; }  // drawn with the game's frames
  void OnKeyDown(rex::ui::KeyEvent& e) override;

 protected:
  void OnDraw(ImGuiIO& io) override;

 private:
  rex::ui::ImGuiDrawer* drawer_;
  rex::ui::Window* game_window_;
  Options options_;
  PadShortcut pad_;
  std::unique_ptr<SettingsWindow> window_;
  bool toggle_pending_ = false;
  // Deferred work (Toggle, the [x]) holds this weakly, so it does nothing once the overlay is gone (shutdown).
  std::shared_ptr<int> alive_ = std::make_shared<int>(0);
};

}  // namespace f2
