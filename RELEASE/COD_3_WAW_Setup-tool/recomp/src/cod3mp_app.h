// cod3mp - Call of Duty 3's multiplayer program (codmp_xenonf.xex), host
// application.
//
// The second program of the port (docs/MULTIPLAYER.md): the campaign's
// Multiplayer item starts it and its way back starts the campaign
// (game/relaunch.h). It shares the campaign's folder: assets/, userdata/
// (saves, logs) and the campaign's settings (cod3.toml, read at start; its
// own changes go to cod3mp.toml, so it never rewrites the campaign's file).
//
// The campaign's port features run here too, compiled from the same sources
// at this program's addresses (tools/mp_aliases.py, mp/CMakeLists.txt): the
// F2 settings, the ~ console with its cheat rules, raw mouse aim, the frame
// rate limit (com_maxfps) and counter, FOV, the custom crosshair, native and
// live render resolution, wide pictures, shadows, textures, the profilers
// and the hang watchdog. What only the campaign has (films, battle actions,
// its own menu pages) is left out. System link runs over the local network
// (SDK patch 0025).

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include <rex/rex_app.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/virtual_key.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

#include "ui/game_console.h"
#include "ui/hud_overlay.h"

// The console keys (~, Shift+~) and F2, before ImGui or the game see them.
class MpConsoleKeyListener : public rex::ui::WindowInputListener {
 public:
  std::function<void(bool shift)> on_toggle;
  std::function<void()> on_close;
  std::function<void()> on_settings;

  void OnKeyDown(rex::ui::KeyEvent& e) override;
  void OnKeyChar(rex::ui::KeyEvent& e) override;
};

// Raw relative mouse motion for cod3::MouseLook while the game holds the mouse.
class MpMouseLookListener : public rex::ui::WindowInputListener {
 public:
  rex::ui::Window* window = nullptr;
  bool captured = false;
  bool relative = false;  // pointer lock / raw input active (else recentering)
  int32_t prev_x = 0;
  int32_t prev_y = 0;
  rex::ui::Window::CursorVisibility prev_cursor = rex::ui::Window::CursorVisibility::kVisible;
  uint32_t moves = 0;                               // motion events while held (log)
  std::pair<float, float> view_at_capture{0, 0};  // pitch, yaw (log)

  void OnMouseMove(rex::ui::MouseEvent& e) override;
};

class Cod3MpApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx);

 protected:
  void OnConfigurePaths(rex::PathConfig& paths) override;
  void OnPostInitLogging() override;
  void OnLoadXexImage(std::string& xex_image) override;
  void OnPreSetup(rex::RuntimeConfig& config) override;
  bool ConstructRuntime(const rex::PathConfig& paths) override;
  void OnPostSetup() override;
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override;
  void OnConfigureFonts(ImFontAtlas* atlas) override;
  void OnConfigureStyle(ImGuiStyle& imgui_style, rex::ui::Style& ui_style) override;
  bool OnWindowCloseRequested() override;
  void OnGuestThreadExit(rex::system::XThread* thread) override;
  void OnPostLaunchModule(rex::system::XThread* thread) override;
  void OnShutdown() override;

 private:
  void ApplyGpuDefaults();       // once the GPU plugin is loaded
  void ApplyRenderResolution();  // the game's own resolution and the GPU scale (render_resolution.h)
  void ToggleConsole(bool full);
  void CloseConsole();
  void ToggleSettings();
  void UpdateMouseCapture();  // UI thread, every frame
  void SaveAll();             // F2 settings + console config
  [[noreturn]] void SaveAndExit(const char* why);

  bool gpu_defaults_logged_ = false;
  bool render_plan_applied_ = false;
  std::atomic<bool> module_launched_{false};  // the game's image is loaded (guest memory readable)
  rex::RuntimeConfig* runtime_config_ = nullptr;  // ReXApp's, set in OnPreSetup

  MpConsoleKeyListener key_listener_;
  MpMouseLookListener mouse_listener_;
  std::unique_ptr<cod3::ui::GameConsoleDialog> console_;
  std::unique_ptr<cod3::ui::HudOverlay> hud_;
  std::unique_ptr<cod3::ui::SettingsWindow> settings_;
  std::filesystem::path config_path_;   // <root>/cod3mp.toml
  std::filesystem::path root_dir_;      // the game folder: assets/, userdata/, cod3.toml
  std::filesystem::path userdata_dir_;  // <root>/userdata
};
