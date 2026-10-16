// cod3 - Call of Duty 3 (Xbox 360) static recompilation, host application.

#pragma once

#include <functional>
#include <memory>

#include <rex/rex_app.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include "game/cutscene_skip.h"
#include "ui/game_console.h"
#include "ui/hud_overlay.h"

// Intercepts the console keys before ImGui or the game see them.
class ConsoleKeyListener : public rex::ui::WindowInputListener {
 public:
  std::function<void(bool shift)> on_toggle;
  std::function<void()> on_close;
  std::function<void()> on_settings;

  void OnKeyDown(rex::ui::KeyEvent& e) override {
    // Space, Enter and Escape skip a movie (game/cutscene_skip.h); the key
    // still reaches its binds.
    if (!cod3::ui::GameConsole::IsOpen() &&
        (e.virtual_key() == rex::ui::VirtualKey::kSpace ||
         e.virtual_key() == rex::ui::VirtualKey::kReturn ||
         e.virtual_key() == rex::ui::VirtualKey::kEscape))
      cod3::CutsceneSkip::Get().RequestSkip();
    if (e.virtual_key() == rex::ui::VirtualKey::kOem3) {  // ` and ~ on US layouts
      if (on_toggle) on_toggle(e.is_shift_pressed());
      e.set_handled(true);
    } else if (e.virtual_key() == rex::ui::VirtualKey::kF2) {
      if (on_settings) on_settings();
      e.set_handled(true);
    } else if (e.virtual_key() == rex::ui::VirtualKey::kEscape &&
               cod3::ui::GameConsole::IsOpen()) {
      if (on_close) on_close();
      e.set_handled(true);
    }
  }
  void OnKeyChar(rex::ui::KeyEvent& e) override {
    auto c = static_cast<unsigned>(e.virtual_key());
    if (c == '`' || c == '~') e.set_handled(true);  // never type the toggle key
  }
};

// Raw relative mouse motion for cod3::MouseLook while the game holds the mouse.
class MouseLookListener : public rex::ui::WindowInputListener {
 public:
  rex::ui::Window* window = nullptr;
  bool captured = false;
  bool relative = false;  // pointer lock / raw input active (else recentering)
  int32_t prev_x = 0;
  int32_t prev_y = 0;
  rex::ui::Window::CursorVisibility prev_cursor = rex::ui::Window::CursorVisibility::kVisible;

  void OnMouseMove(rex::ui::MouseEvent& e) override;
  // A left click skips a movie (game/cutscene_skip.h).
  void OnMouseDown(rex::ui::MouseEvent& e) override {
    if (e.button() == rex::ui::MouseEvent::Button::kLeft) cod3::CutsceneSkip::Get().RequestSkip();
  }
};

class Cod3App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx);

 protected:
  void OnConfigurePaths(rex::PathConfig& paths) override;
  void OnPreSetup(rex::RuntimeConfig& config) override;
  void OnPostSetup() override;
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override;
  void OnConfigureFonts(ImFontAtlas* atlas) override;
  void OnConfigureStyle(ImGuiStyle& imgui_style, rex::ui::Style& ui_style) override;
  void OnShutdown() override;
  bool ConstructRuntime(const rex::PathConfig& paths) override;
  bool OnWindowCloseRequested() override;
  void OnGuestThreadExit(rex::system::XThread* thread) override;
  void OnPostLaunchModule(rex::system::XThread* thread) override;

 private:
  void ToggleConsole(bool full);
  void CloseConsole();
  void ToggleSettings();
  void UpdateMouseCapture();  // UI thread, every frame
  void SaveAll();             // F2 settings + console config
  [[noreturn]] void SaveAndExit(const char* why);  // UI thread: SaveAll, then end the process
  void ApplyGpuDefaults();    // the GPU plugin's settings, once it is loaded
  void ApplyRenderResolution();  // the game's own resolution and the GPU scale (render_resolution.h)
  bool gpu_defaults_logged_ = false;
  bool render_plan_applied_ = false;
  rex::RuntimeConfig* runtime_config_ = nullptr;  // ReXApp's, set in OnPreSetup

  ConsoleKeyListener key_listener_;
  MouseLookListener mouse_listener_;
  std::unique_ptr<cod3::ui::GameConsoleDialog> console_;
  std::unique_ptr<cod3::ui::HudOverlay> hud_;
  std::unique_ptr<cod3::ui::SettingsWindow> settings_;
  std::filesystem::path config_path_;
  std::filesystem::path root_dir_;      // the game folder: assets/, data/, userdata/ (OnConfigurePaths)
  std::filesystem::path userdata_dir_;  // <root>/userdata (OnConfigurePaths)
};
