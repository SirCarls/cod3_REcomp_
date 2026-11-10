// cod4 / cod4mp - Call of Duty 4: Modern Warfare (Xbox 360) static recompilation, host application. [acct2-cod4]
//
// The SDK's app with the project's game-folder layout and its choice of graphics API, the same for both programs.
// Call of Duty 4's own features (controls, menus, the frame-rate audit) come later: dev/agents/acct2-cod4/PLAN.md.

#pragma once

#include <filesystem>
#include <memory>

#include <rex/rex_app.h>

class Cod4App : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx);

 protected:
  void OnConfigurePaths(rex::PathConfig& paths) override;
  void OnLoadXexImage(std::string& xex_image) override;
  void OnPreSetup(rex::RuntimeConfig& config) override;
  void OnPostSetup() override;
  void OnShutdown() override;

 private:
  std::filesystem::path root_dir_;  // the game folder: assets/, userdata/, cod4.toml (OnConfigurePaths)
};
