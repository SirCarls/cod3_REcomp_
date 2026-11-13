// waw - Call of Duty: World at War (Xbox 360) static recompilation, host application. [acct2-waw]

#include "waw_app.h"

#include <cstdlib>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>
#include <rex/ui/presenter.h>

#include "generated/default/waw_init.h"
#include "test_input.h"  // Call of Duty 3's scripted test pad (../recomp/src), built here as well
#include "waw_f2.h"     // [f2-menus] the F2 settings window
#include "waw_keys.h"   // [f2-menus] keys the PC way: the game's own binds during gameplay
#include "waw_mouse.h"  // [f2-menus] mouse look and the keyboard pad's defaults
#include "waw_shots.h"
#include "waw_native_side.h"  // [waw-decomp-waw] 2026-11-11 the native renderer's check pictures
#include "waw_native_window.h"  // [waw-decomp-waw] 2026-11-11 round 5: waw_native_vulkan

REXCVAR_DEFINE_BOOL(waw_no_gpu, false, "waw",
                    "Run without the GPU emulation (no picture; for debugging the start)");
REXCVAR_DEFINE_STRING(waw_graphics_api, "auto", "waw",
                      "Windows: the graphics API: auto (Vulkan, else Direct3D 12), vulkan or d3d12. "
                      "Linux has Vulkan only");
REXCVAR_DEFINE_STRING(waw_test_input, "", "waw",
                      "Debug: scripted controller input for tests, a synthetic pad on player 1, for example "
                      "\"20:START;24:A\" (the format: ../recomp/src/test_input.h)");

// [f2-menus] The window's mouse for the game's own mouse look (waw_mouse.h).
static waw::mouse::Listener g_mouse_listener;
// [f2-menus] The window's keys, mouse buttons and wheel for the game's own binds (waw_keys.h).
static waw::keys::Listener g_keys_listener;

std::unique_ptr<rex::ui::WindowedApp> WawApp::Create(rex::ui::WindowedAppContext& ctx) {
  return std::unique_ptr<WawApp>(new WawApp(ctx, "waw", PPCImageConfig));
}

// The game folder, as Call of Duty 3's (one folder, never inside a read-only AppImage). The executable sits in it,
// in its bin/ folder (the setup tool's layout) or is an AppImage placed in it:
//   <root>/assets     the game's files (from the player's disc)
//   <root>/userdata   saves, caches, logs
//   <root>/waw.toml   the port's settings
void WawApp::OnConfigurePaths(rex::PathConfig& paths) {
  std::vector<std::filesystem::path> candidates;
  if (const char* appimage = std::getenv("APPIMAGE"); appimage && *appimage)
    candidates.push_back(std::filesystem::path(appimage).parent_path());
  const auto exe_dir = rex::filesystem::GetExecutableFolder();
  candidates.push_back(exe_dir);
  candidates.push_back(exe_dir.parent_path());
  std::filesystem::path root = candidates.front();
  for (const auto& c : candidates) {
    std::error_code ec;
    if (std::filesystem::exists(c / "assets" / "default.xex", ec)) {
      root = c;
      break;
    }
  }
  root_dir_ = root;
  if (paths.game_data_root.empty() && std::filesystem::exists(root / "assets" / "default.xex"))
    paths.game_data_root = root / "assets";
  if (rex::cvar::GetFlagByName("user_data_root").empty()) {
    paths.user_data_root = root / "userdata";
    paths.cache_root = root / "userdata" / "cache";
  }
  paths.config_path = root / "waw.toml";

  // The log goes to userdata/logs/waw.log unless the command line names another file (the SDK's default, a logs/
  // folder next to the executable, can't be made inside an AppImage).
  if (const auto source = rex::cvar::GetFlagSource("log_file");
      source != rex::cvar::Source::kCommandLine && source != rex::cvar::Source::kEnvironment) {
    const auto user_dir = paths.user_data_root.empty() ? root / "userdata" : paths.user_data_root;
    std::error_code ec;
    std::filesystem::create_directories(user_dir / "logs", ec);
    rex::cvar::SetFlagByName("log_file", (user_dir / "logs" / "waw.log").generic_string());
  }
}

void WawApp::OnPreSetup(rex::RuntimeConfig& config) {
  // The game's files stay read-only, as on the disc.
  if (rex::cvar::GetFlagByName("allow_game_relative_writes") == "true") {
    REXLOG_WARN("waw: allow_game_relative_writes isn't used; the game's files stay read-only");
    rex::cvar::SetFlagByName("allow_game_relative_writes", "false");
  }
  // [f2-menus] Keyboard and mouse as a PC shooter's (waw_mouse.h), before the input drivers start.
  waw::mouse::ApplyInputDefaults();
  // The Xenos GPU emulation, unless another plugin was picked or none was asked for. Only a plain plugin name: the
  // plugin is a library loaded by name from the program's folder.
  if (config.gpu_plugin.empty() && !REXCVAR_GET(waw_no_gpu)) config.gpu_plugin = "xenos";
  if (config.gpu_plugin.size() > 32 ||
      config.gpu_plugin.find_first_not_of("abcdefghijklmnopqrstuvwxyz"
                                          "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos) {
    REXLOG_WARN("waw: gpu_plugin '{}' isn't a plain plugin name; using xenos", config.gpu_plugin);
    config.gpu_plugin = "xenos";
  }
  // [waw-decomp-waw] 2026-11-11 round 5: the native renderer as the game's picture (waw_native_vulkan, off by
  // default): the plugin's host backend in place of the GPU emulation (waw_native_window.h); then the block below is
  // skipped (config.graphics is set)
  waw::native::StartNativeVulkan(config);
#ifdef _WIN32
  // Windows builds have Vulkan and Direct3D 12. "auto" is Vulkan, as on Linux, and Direct3D 12 when Vulkan can't
  // start (then the SDK loads the plugin with its own default).
  if (std::string api = REXCVAR_GET(waw_graphics_api);
      !config.graphics && !config.gpu_plugin.empty() && (api == "auto" || api == "d3d12" || api == "vulkan")) {
    const bool automatic = api == "auto";
    if (automatic) api = "vulkan";
    config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin, api);
    if (config.graphics)
      REXLOG_INFO("waw: graphics API {} ({})", api, automatic ? "automatic" : "chosen in the settings");
    else
      REXLOG_WARN("waw: the {} renderer couldn't start; using the other one", api);
  }
#endif
}

void WawApp::OnPostSetup() {
  // Debug screenshots (waw_shots.h): the game's own picture, read back from the GPU emulation's output.
  const auto user_dir = rex::cvar::GetFlagByName("user_data_root").empty()
                            ? root_dir_ / "userdata"
                            : std::filesystem::path(rex::cvar::GetFlagByName("user_data_root"));
  waw::shots::Start(user_dir / "logs" / "screenshots", [this](rex::ui::RawImage& image) {
    auto* graphics = runtime() ? runtime()->graphics_system() : nullptr;
    auto* presenter = graphics ? graphics->presenter() : nullptr;
    return presenter && presenter->CaptureGuestOutput(image);
  });

  // [waw-decomp-waw] 2026-11-11: the native renderer's check pictures (waw_debug_native_side, off by default), saved
  // beside the game's own picture, read the same way.
  waw::native::SideStart(user_dir / "logs" / "screenshots", [this](rex::ui::RawImage& image) {
    auto* graphics = runtime() ? runtime()->graphics_system() : nullptr;
    auto* presenter = graphics ? graphics->presenter() : nullptr;
    return presenter && presenter->CaptureGuestOutput(image);
  });

  // A scripted pad for tests (waw_test_input). Its times count from here.
  if (const std::string script = REXCVAR_GET(waw_test_input); !script.empty()) {
    if (auto* input = static_cast<rex::input::InputSystem*>(runtime() ? runtime()->input_system() : nullptr))
      input->AddDriver(std::make_unique<cod3::ScriptedInputDriver>(script));
    else
      REXLOG_WARN("waw: no input system; waw_test_input not used");
  }

  // [f2-menus] The game's own mouse look (waw_mouse.h): above the SDK's keyboard/mouse driver.
  if (auto* win = window()) {
    g_mouse_listener.window = win;
    win->AddInputListener(&g_mouse_listener, 199);
    g_keys_listener.window = win;                   // [f2-menus] below the mouse look, above the SDK's driver
    win->AddInputListener(&g_keys_listener, 198);  // [f2-menus]
  }

  // [f2-menus] While the F2 settings window is open the game gets no input: no keys, mouse or controller.
  if (auto* input = static_cast<rex::input::InputSystem*>(runtime() ? runtime()->input_system() : nullptr))
    input->SetActiveCallback([]() { return waw::f2ui::GameGetsInput(); });
}

// [f2-menus] The F2 settings window (waw_f2.h): Call of Duty 3's window in World at War's own colors.
void WawApp::OnConfigureFonts(ImFontAtlas* atlas) { waw::f2ui::ConfigureFonts(atlas); }

void WawApp::OnConfigureStyle(ImGuiStyle& imgui_style, rex::ui::Style& ui_style) {
  waw::f2ui::ConfigureStyle(imgui_style, ui_style);
}

void WawApp::OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) {
  // The window is made and closed after the frame that asks for it, outside the drawer's loop over its windows.
  waw::f2ui::Attach(drawer, window(), root_dir_ / "waw.toml",
                    [this](std::function<void()> fn) { app_context().CallInUIThreadDeferred(std::move(fn)); });
}

void WawApp::OnShutdown() {
  waw::f2ui::Detach();  // [f2-menus] saves what changed; before the ImGui drawer goes
  if (auto* win = window()) win->RemoveInputListener(&g_mouse_listener);  // [f2-menus]
  if (auto* win = window()) win->RemoveInputListener(&g_keys_listener);   // [f2-menus]
  waw::native::SideStop();  // [waw-decomp-waw] 2026-11-11 before the presenter goes
  waw::shots::Stop();  // before the GPU emulation's presenter goes
}
