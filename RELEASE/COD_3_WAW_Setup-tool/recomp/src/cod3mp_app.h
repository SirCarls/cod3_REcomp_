// cod3mp - Call of Duty 3's multiplayer program (codmp_xenonf.xex), host
// application.
//
// The second program of the port (docs/MULTIPLAYER.md): the campaign's
// Multiplayer item starts it and its way back starts the campaign
// (game/relaunch.h). It shares the campaign's folder: assets/, userdata/
// (saves, logs) and the campaign's settings (cod3.toml, read at start; its
// own changes go to cod3mp.toml, so it never rewrites the campaign's file).
//
// This first version runs the game as the console did, with the SDK's
// keyboard and mouse controls (the mouse moves the right stick) and system
// link over the local network (SDK patch 0025). The campaign's own additions
// (F2 settings, the game console, raw mouse aim, native resolution ...) hook
// default.xex's addresses and come to multiplayer one by one.

#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include <rex/rex_app.h>

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
  void OnConfigureFonts(ImFontAtlas* atlas) override;
  void OnConfigureStyle(ImGuiStyle& imgui_style, rex::ui::Style& ui_style) override;
  bool OnWindowCloseRequested() override;
  void OnGuestThreadExit(rex::system::XThread* thread) override;
  void OnPostLaunchModule(rex::system::XThread* thread) override;
  void OnShutdown() override;

 private:
  void ApplyGpuDefaults();  // once the GPU plugin is loaded

  std::filesystem::path root_dir_;      // the game folder: assets/, userdata/, cod3.toml
  std::filesystem::path userdata_dir_;  // <root>/userdata
};
