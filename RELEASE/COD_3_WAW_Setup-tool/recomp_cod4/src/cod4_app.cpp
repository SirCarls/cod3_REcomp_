// cod4 / cod4mp - Call of Duty 4: Modern Warfare (Xbox 360) static recompilation, host application. [acct2-cod4]

#include "cod4_app.h"

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

#ifdef COD4_MP
#include "generated/default_mp/cod4mp_init.h"
#else
#include "generated/default/cod4_init.h"
#endif
#include "cod4_f2.h"  // [f2-menus] the F2 settings window
#include "cod4_mouse.h"
#include "cod4_native_side.h"  // [waw-decomp-cod4] 2026-11-11 the native renderer's side check
#include "cod4_native_window.h"  // [waw-decomp-cod4] 2026-11-11 round 6: cod4_native_vulkan
#include "cod4_profile.h"
#include "cod4_shots.h"
#include "test_input.h"  // Call of Duty 3's scripted test pad (../recomp/src), built here as well

REXCVAR_DEFINE_BOOL(cod4_no_gpu, false, "cod4",
                    "Run without the GPU emulation (no picture; for debugging the start)");
REXCVAR_DEFINE_STRING(cod4_graphics_api, "auto", "cod4",
                      "Windows: the graphics API: auto (Vulkan, else Direct3D 12), vulkan or d3d12. "
                      "Linux has Vulkan only");
REXCVAR_DEFINE_STRING(cod4_test_input, "", "cod4",
                      "Debug: scripted controller input for tests, a synthetic pad on player 1, for example "
                      "\"20:START;24:A\" (the format: ../recomp/src/test_input.h)");

namespace {

#ifdef COD4_MP
constexpr const char* kProgram = "cod4mp";        // the program's name: its settings file and log
constexpr const char* kProgramXex = "default_mp.xex";  // the disc's program it is translated from
#else
constexpr const char* kProgram = "cod4";
constexpr const char* kProgramXex = "default.xex";
#endif

// The window's mouse for the game's own mouse look (cod4_mouse.h).
cod4::mouse::Listener g_mouse_listener;

// Keyboard and mouse as a PC shooter's, through the SDK's keyboard pad (off unless mnk_mode is set; the SDK's default
// is false). The 360 game reads a pad, so each key presses the button that does the same in the 360's default layout.
// The same keys as Call of Duty 3's (../recomp/src/app_defaults.h, World at War style). Until CoD4 has its own raw
// mouse aim (as cod3_raw_mouse), the mouse moves the right stick (mnk_mouse).
struct InputDefault {
  const char* flag;
  const char* value;
};
constexpr InputDefault kInputDefaults[] = {
    {"mnk_mode", "true"},
    {"mnk_mouse", "true"},                    // look and turn
    {"keybind_right_trigger", "LMB"},         // fire
    {"keybind_left_trigger", "RMB"},          // aim down the sights
    {"keybind_a", "Space"},                   // jump; select in menus
    {"keybind_b", "C,Backspace"},             // crouch (hold: prone); back in menus
    {"keybind_x", "R,F"},                     // reload; hold: use
    {"keybind_y", "1,2"},                     // switch weapon
    {"keybind_right_shoulder", "G"},          // frag grenade
    {"keybind_left_shoulder", "4"},           // special grenade (flash, stun or smoke)
    {"keybind_lstick_press", "Shift"},        // sprint; hold breath while scoped
    {"keybind_rstick_press", "V,MMB"},        // melee
    {"keybind_dpad_up", "N,Shift+Up"},        // night vision
    {"keybind_back", "Tab"},                  // objectives (campaign), scores (multiplayer)
    {"keybind_start", "Escape,Return"},       // pause menu; START at the title screen
};

// Each only while nobody chose: the command line, the environment or the settings file keeps its value.
void ApplyInputDefaults() {
  for (const auto& d : kInputDefaults) {
    if (rex::cvar::GetFlagInfo(d.flag) && rex::cvar::GetFlagSource(d.flag) == rex::cvar::Source::kDefault)
      rex::cvar::SetFlagByName(d.flag, d.value);
  }
  REXLOG_INFO("cod4: keyboard pad {}, mouse look {}, fire {}, aim {}, START {}",
              rex::cvar::GetFlagByName("mnk_mode"), rex::cvar::GetFlagByName("mnk_mouse"),
              rex::cvar::GetFlagByName("keybind_right_trigger"), rex::cvar::GetFlagByName("keybind_left_trigger"),
              rex::cvar::GetFlagByName("keybind_start"));
}

}  // namespace

std::unique_ptr<rex::ui::WindowedApp> Cod4App::Create(rex::ui::WindowedAppContext& ctx) {
  return std::unique_ptr<Cod4App>(new Cod4App(ctx, kProgram, PPCImageConfig));
}

// The game folder, as Call of Duty 3's (one folder, never inside a read-only AppImage). The executable sits in it,
// in its bin/ folder (the setup tool's layout) or is an AppImage placed in it:
//   <root>/assets       the game's files (from the player's disc; both programs read the same folder)
//   <root>/userdata     saves, caches, logs
//   <root>/<program>.toml  the port's settings (cod4.toml, cod4mp.toml)
void Cod4App::OnConfigurePaths(rex::PathConfig& paths) {
  std::vector<std::filesystem::path> candidates;
  if (const char* appimage = std::getenv("APPIMAGE"); appimage && *appimage)
    candidates.push_back(std::filesystem::path(appimage).parent_path());
  const auto exe_dir = rex::filesystem::GetExecutableFolder();
  candidates.push_back(exe_dir);
  candidates.push_back(exe_dir.parent_path());
  std::filesystem::path root = candidates.front();
  for (const auto& c : candidates) {
    std::error_code ec;
    if (std::filesystem::exists(c / "assets" / kProgramXex, ec)) {
      root = c;
      break;
    }
  }
  root_dir_ = root;
  if (paths.game_data_root.empty() && std::filesystem::exists(root / "assets" / kProgramXex))
    paths.game_data_root = root / "assets";
  if (rex::cvar::GetFlagByName("user_data_root").empty()) {
    paths.user_data_root = root / "userdata";
    paths.cache_root = root / "userdata" / "cache";
  }
  paths.config_path = root / (std::string(kProgram) + ".toml");

  // The log goes to userdata/logs/<program>.log unless the command line names another file (the SDK's default, a
  // logs/ folder next to the executable, can't be made inside an AppImage).
  if (const auto source = rex::cvar::GetFlagSource("log_file");
      source != rex::cvar::Source::kCommandLine && source != rex::cvar::Source::kEnvironment) {
    const auto user_dir = paths.user_data_root.empty() ? root / "userdata" : paths.user_data_root;
    std::error_code ec;
    std::filesystem::create_directories(user_dir / "logs", ec);
    rex::cvar::SetFlagByName("log_file", (user_dir / "logs" / (std::string(kProgram) + ".log")).generic_string());
  }
}

// The disc's program this one is translated from. The SDK starts game:\\default.xex unless told otherwise, so the
// multiplayer program has to name default_mp.xex (otherwise it loads the campaign's image, whose entry point
// 0x820E4600 isn't in this program's function table: "No function registered at 820E4600").
void Cod4App::OnLoadXexImage(std::string& xex_image) { xex_image = std::string("game:\\") + kProgramXex; }

void Cod4App::OnPreSetup(rex::RuntimeConfig& config) {
  // The game's files stay read-only, as on the disc.
  if (rex::cvar::GetFlagByName("allow_game_relative_writes") == "true") {
    REXLOG_WARN("cod4: allow_game_relative_writes isn't used; the game's files stay read-only");
    rex::cvar::SetFlagByName("allow_game_relative_writes", "false");
  }
  // Keyboard and mouse (kInputDefaults), before the input drivers start.
  ApplyInputDefaults();
  // The Xenos GPU emulation, unless another plugin was picked or none was asked for. Only a plain plugin name: the
  // plugin is a library loaded by name from the program's folder.
  if (config.gpu_plugin.empty() && !REXCVAR_GET(cod4_no_gpu)) config.gpu_plugin = "xenos";
  if (config.gpu_plugin.size() > 32 ||
      config.gpu_plugin.find_first_not_of("abcdefghijklmnopqrstuvwxyz"
                                          "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos) {
    REXLOG_WARN("cod4: gpu_plugin '{}' isn't a plain plugin name; using xenos", config.gpu_plugin);
    config.gpu_plugin = "xenos";
  }
  // [waw-decomp-cod4] 2026-11-11 round 6: the native renderer as the game's picture (cod4_native_vulkan, off by
  // default): the plugin's host backend in place of the GPU emulation (cod4_native_window.h); then the block below is
  // skipped
  cod4::native::StartNativeVulkan(config);
#ifdef _WIN32
  // Windows builds have Vulkan and Direct3D 12. "auto" is Vulkan, as on Linux, and Direct3D 12 when Vulkan can't
  // start (then the SDK loads the plugin with its own default).
  if (std::string api = REXCVAR_GET(cod4_graphics_api);
      !config.graphics && !config.gpu_plugin.empty() && (api == "auto" || api == "d3d12" || api == "vulkan")) {
    const bool automatic = api == "auto";
    if (automatic) api = "vulkan";
    config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin, api);
    if (config.graphics)
      REXLOG_INFO("cod4: graphics API {} ({})", api, automatic ? "automatic" : "chosen in the settings");
    else
      REXLOG_WARN("cod4: the {} renderer couldn't start; using the other one", api);
  }
#endif
}

void Cod4App::OnPostSetup() {
  // Debug screenshots (cod4_shots.h): the game's own picture, read back from the GPU emulation's output.
  const auto user_dir = rex::cvar::GetFlagByName("user_data_root").empty()
                            ? root_dir_ / "userdata"
                            : std::filesystem::path(rex::cvar::GetFlagByName("user_data_root"));
  cod4::shots::Start(user_dir / "logs" / "screenshots", [this](rex::ui::RawImage& image) {
    auto* graphics = runtime() ? runtime()->graphics_system() : nullptr;
    auto* presenter = graphics ? graphics->presenter() : nullptr;
    return presenter && presenter->CaptureGuestOutput(image);
  });

  // [waw-decomp-cod4] 2026-11-11: the native renderer's side check (cod4_native_side.h, cod4_debug_native_side, off
  // by default): its pictures go beside the debug screenshots, the game's own read the same way.
  cod4::native::SideStart(user_dir / "logs" / "screenshots", [this](rex::ui::RawImage& image) {
    auto* graphics = runtime() ? runtime()->graphics_system() : nullptr;
    auto* presenter = graphics ? graphics->presenter() : nullptr;
    return presenter && presenter->CaptureGuestOutput(image);
  });

  // A sampling profile of one thread, for the frame-rate audit (cod4_debug_profile_thread, cod4_profile.h).
  cod4::profile::Start(user_dir / "logs");

  // The game's own mouse look (cod4_mouse.h): above the SDK's keyboard/mouse driver.
  if (auto* win = window()) {
    g_mouse_listener.window = win;
    win->AddInputListener(&g_mouse_listener, 199);
  }

  // A scripted pad for tests (cod4_test_input). Its times count from here.
  if (const std::string script = REXCVAR_GET(cod4_test_input); !script.empty()) {
    if (auto* input = static_cast<rex::input::InputSystem*>(runtime() ? runtime()->input_system() : nullptr))
      input->AddDriver(std::make_unique<cod3::ScriptedInputDriver>(script));
    else
      REXLOG_WARN("cod4: no input system; cod4_test_input not used");
  }

  // [f2-menus] While the F2 settings window is open the game gets no input: no keys, mouse or controller (and the
  // mouse look lets the pointer go, cod4_mouse.cpp).
  if (auto* input = static_cast<rex::input::InputSystem*>(runtime() ? runtime()->input_system() : nullptr))
    input->SetActiveCallback([]() { return cod4::f2ui::GameGetsInput(); });
}

// [f2-menus] The F2 settings window (cod4_f2.h): Call of Duty 3's window in Call of Duty 4's own colors.
void Cod4App::OnConfigureFonts(ImFontAtlas* atlas) { cod4::f2ui::ConfigureFonts(atlas); }

void Cod4App::OnConfigureStyle(ImGuiStyle& imgui_style, rex::ui::Style& ui_style) {
  cod4::f2ui::ConfigureStyle(imgui_style, ui_style);
}

void Cod4App::OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) {
  // The window is made and closed after the frame that asks for it, outside the drawer's loop over its windows.
  cod4::f2ui::Attach(drawer, window(), root_dir_ / (std::string(kProgram) + ".toml"),
                     [this](std::function<void()> fn) { app_context().CallInUIThreadDeferred(std::move(fn)); });
}

void Cod4App::OnShutdown() {
  cod4::f2ui::Detach();  // [f2-menus] saves what changed; before the ImGui drawer goes
  cod4::profile::Stop();
  if (auto* win = window()) win->RemoveInputListener(&g_mouse_listener);
  cod4::native::SideStop();  // [waw-decomp-cod4] 2026-11-11 before the GPU emulation's presenter goes
  cod4::shots::Stop();  // before the GPU emulation's presenter goes
}
