// bo1 / bo1mp - Call of Duty: Black Ops (Xbox 360) static recompilation, host application. [waw-decomp]
//
// The SDK's app with the project's game-folder layout and its choice of graphics API, the same as Call of Duty 4's
// (../recomp_cod4/src/cod4_app.h). Black Ops' own features (controls, menus, the frame-rate work) come over from
// World at War once the game boots: dev/agents/waw-decomp/BLACK-OPS.md.

#pragma once

#include <filesystem>
#include <memory>

#include <rex/rex_app.h>

class Bo1App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx);

 protected:
  void OnConfigurePaths(rex::PathConfig& paths) override;
  void OnLoadXexImage(std::string& xex_image) override;
  void OnPreSetup(rex::RuntimeConfig& config) override;
  void OnPostSetup() override;
  void OnShutdown() override;
  // [menus-native] The F2 settings window (bo1_f2.h): its fonts, colors and the window itself.
  void OnConfigureFonts(ImFontAtlas* atlas) override;
  void OnConfigureStyle(ImGuiStyle& imgui_style, rex::ui::Style& ui_style) override;
  void OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) override;

 private:
  std::filesystem::path root_dir_;  // the game folder: assets/, userdata/, bo1.toml (OnConfigurePaths)
};
