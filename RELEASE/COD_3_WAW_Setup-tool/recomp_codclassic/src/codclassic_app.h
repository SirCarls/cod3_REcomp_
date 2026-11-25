// codclassic - Call of Duty Classic (Xbox Live Arcade) recompiled for PC on the ReXGlue SDK: the
// program around the recompiled game. [acct2-uo]
//
// Kept small on purpose: what Call of Duty 3's port adds (settings window, keyboard and mouse, the
// frame limiter, ...) comes over piece by piece once the game reaches its menu, and United
// Offensive's additions follow (dev/agents/acct2-uo/PLAN.md).

#pragma once

#include <filesystem>
#include <memory>
#include <string>

#include <rex/rex_app.h>

class CodClassicApp : public rex::ReXApp {
 public:
  using rex::ReXApp::ReXApp;

  static std::unique_ptr<rex::ui::WindowedApp> Create(rex::ui::WindowedAppContext& ctx);

 protected:
  // Where the package's files, the saves and the settings are.
  void OnConfigurePaths(rex::PathConfig& paths) override;
  // Which of the package's programs runs: default.xex, or cod_mp.xex for codclassicmp.
  void OnLoadXexImage(std::string& xex_image) override;
  // The keyboard pad and the GPU emulation (Vulkan; on Windows Direct3D 12 or Vulkan).
  void OnPreSetup(rex::RuntimeConfig& config) override;
  // The guest vblank's rate (game/port_defaults.h), once the GPU plugin's settings exist.
  void OnPostSetup() override;

 private:
  std::filesystem::path userdata_dir_;
};
