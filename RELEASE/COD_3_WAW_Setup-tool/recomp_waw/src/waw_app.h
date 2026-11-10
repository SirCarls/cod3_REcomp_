// waw - Call of Duty: World at War (Xbox 360) static recompilation, host application. [acct2-waw]
//
// The SDK's app with the project's game-folder layout and its choice of graphics API. World at War's own features
// (controls, menus, the PC copy's files) come later: dev/agents/acct2-waw/PLAN.md.

#pragma once

#include <filesystem>
#include <memory>

#include <rex/rex_app.h>

class WawApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx);

 protected:
  void OnConfigurePaths(rex::PathConfig& paths) override;
  void OnPreSetup(rex::RuntimeConfig& config) override;
  void OnPostSetup() override;
  void OnShutdown() override;

 private:
  std::filesystem::path root_dir_;  // the game folder: assets/, userdata/, waw.toml (OnConfigurePaths)
};
