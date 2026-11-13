// cod2 - Call of Duty 2 (Xbox 360) recompiled for PC on the ReXGlue SDK:
// the program around the recompiled game. [acct2-cod2]
//
// Kept small on purpose: what Call of Duty 3's port adds (settings window,
// keyboard and mouse, the game's fixes) comes over piece by piece once the
// game reaches its menu (dev/agents/acct2-cod2/PLAN.md).

#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include <rex/rex_app.h>

class Cod2App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx);

 protected:
  // Where the disc's files, the saves and the settings are.
  void OnConfigurePaths(rex::PathConfig& paths) override;
  // Which of the disc's programs runs: default.xex, or default_mp.xex for cod2mp.
  void OnLoadXexImage(std::string& xex_image) override;
  // The GPU emulation (Vulkan; on Windows Direct3D 12 or Vulkan).
  void OnPreSetup(rex::RuntimeConfig& config) override;
  // The guest vblank's rate (game/frame_rate.h) and the test-run helpers
  // (game/debug_shots.h) once the runtime is up.
  void OnPostSetup() override;
  void OnShutdown() override;
  // [f2-menus] The F2 settings window (cod2_f2.h): its fonts, colors and the window itself.
  void OnConfigureFonts(ImFontAtlas* atlas) override;
  void OnConfigureStyle(ImGuiStyle& imgui_style, rex::ui::Style& ui_style) override;
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override;

 private:
  std::filesystem::path userdata_dir_;
  std::filesystem::path config_path_;  // [f2-menus] the settings file the F2 window saves to (OnConfigurePaths)
};
