// bo1 / bo1mp - Call of Duty: Black Ops (Xbox 360) static recompilation, host application. [waw-decomp]
// Laid out as Call of Duty 4's (../recomp_cod4/src/cod4_app.cpp): the game folder, the program's settings file and
// log, the GPU plugin, the keyboard pad's defaults and the scripted test pad. Nothing game-specific yet.

#include "bo1_app.h"

#include <cstdlib>
#include <functional>  // [menus-native] the F2 window's deferred calls
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

#ifdef BO1_MP
#include "generated/default_mp/bo1mp_init.h"
#else
#include "generated/default/bo1_init.h"
#endif
#include "bo1_f2.h"  // [menus-native] the F2 settings window
#include "bo1_shots.h"
#include "test_input.h"  // Call of Duty 3's scripted test pad (../recomp/src), built here as well

REXCVAR_DEFINE_BOOL(bo1_no_gpu, false, "bo1",
                    "Run without the GPU emulation (no picture; for debugging the start)");
REXCVAR_DEFINE_STRING(bo1_graphics_api, "auto", "bo1",
                      "Windows: the graphics API: auto (Vulkan, else Direct3D 12), vulkan or d3d12. "
                      "Linux has Vulkan only");
REXCVAR_DEFINE_STRING(bo1_test_input, "", "bo1",
                      "Debug: scripted controller input for tests, a synthetic pad on player 1, for example "
                      "\"20:START;24:A\" (the format: ../recomp/src/test_input.h)");

namespace {

#ifdef BO1_MP
constexpr const char* kProgram = "bo1mp";             // the program's name: its settings file and log
constexpr const char* kProgramXex = "default_mp.xex";  // the disc's program it is translated from
#else
constexpr const char* kProgram = "bo1";
constexpr const char* kProgramXex = "default.xex";
#endif

// Keyboard and mouse as a PC shooter's, through the SDK's keyboard pad (off unless mnk_mode is set; the SDK's default
// is false). The 360 game reads a pad, so each key presses the button that does the same in the 360's default layout.
// The same keys as Call of Duty 3's and Call of Duty 4's (World at War style); Black Ops' 360 layout matches.
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
    {"keybind_right_shoulder", "G"},          // lethal grenade
    {"keybind_left_shoulder", "4"},           // tactical grenade
    {"keybind_lstick_press", "Shift"},        // sprint; hold breath while scoped
    {"keybind_rstick_press", "V,MMB"},        // melee
    {"keybind_dpad_up", "N,Shift+Up"},        // equipment
    {"keybind_back", "Tab"},                  // objectives (campaign), scores (multiplayer)
    {"keybind_start", "Escape,Return"},       // pause menu; START at the title screen
};

// Each only while nobody chose: the command line, the environment or the settings file keeps its value.
void ApplyInputDefaults() {
  for (const auto& d : kInputDefaults) {
    if (rex::cvar::GetFlagInfo(d.flag) && rex::cvar::GetFlagSource(d.flag) == rex::cvar::Source::kDefault)
      rex::cvar::SetFlagByName(d.flag, d.value);
  }
  REXLOG_INFO("bo1: keyboard pad {}, mouse look {}, fire {}, aim {}, START {}",
              rex::cvar::GetFlagByName("mnk_mode"), rex::cvar::GetFlagByName("mnk_mouse"),
              rex::cvar::GetFlagByName("keybind_right_trigger"), rex::cvar::GetFlagByName("keybind_left_trigger"),
              rex::cvar::GetFlagByName("keybind_start"));
}

}  // namespace

std::unique_ptr<rex::ui::WindowedApp> Bo1App::Create(rex::ui::WindowedAppContext& ctx) {
  return std::unique_ptr<Bo1App>(new Bo1App(ctx, kProgram, PPCImageConfig));
}

// The game folder, as Call of Duty 3's and Call of Duty 4's (one folder, never inside a read-only AppImage). The
// executable sits in it, in its bin/ folder (the setup tool's layout) or is an AppImage placed in it:
//   <root>/assets       the game's files (from the player's disc; both programs read the same folder)
//   <root>/userdata     saves, caches, logs
//   <root>/<program>.toml  the port's settings (bo1.toml, bo1mp.toml)
void Bo1App::OnConfigurePaths(rex::PathConfig& paths) {
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
// multiplayer program has to name default_mp.xex.
void Bo1App::OnLoadXexImage(std::string& xex_image) { xex_image = std::string("game:\\") + kProgramXex; }

void Bo1App::OnPreSetup(rex::RuntimeConfig& config) {
  // The game's files stay read-only, as on the disc.
  if (rex::cvar::GetFlagByName("allow_game_relative_writes") == "true") {
    REXLOG_WARN("bo1: allow_game_relative_writes isn't used; the game's files stay read-only");
    rex::cvar::SetFlagByName("allow_game_relative_writes", "false");
  }
  // Keyboard and mouse (kInputDefaults), before the input drivers start.
  ApplyInputDefaults();
  // The Xenos GPU emulation, unless another plugin was picked or none was asked for. Only a plain plugin name: the
  // plugin is a library loaded by name from the program's folder.
  if (config.gpu_plugin.empty() && !REXCVAR_GET(bo1_no_gpu)) config.gpu_plugin = "xenos";
  if (config.gpu_plugin.size() > 32 ||
      config.gpu_plugin.find_first_not_of("abcdefghijklmnopqrstuvwxyz"
                                          "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos) {
    REXLOG_WARN("bo1: gpu_plugin '{}' isn't a plain plugin name; using xenos", config.gpu_plugin);
    config.gpu_plugin = "xenos";
  }
#ifdef _WIN32
  // Windows builds have Vulkan and Direct3D 12. "auto" is Vulkan, as on Linux, and Direct3D 12 when Vulkan can't
  // start (then the SDK loads the plugin with its own default).
  if (std::string api = REXCVAR_GET(bo1_graphics_api);
      !config.graphics && !config.gpu_plugin.empty() && (api == "auto" || api == "d3d12" || api == "vulkan")) {
    const bool automatic = api == "auto";
    if (automatic) api = "vulkan";
    config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin, api);
    if (config.graphics)
      REXLOG_INFO("bo1: graphics API {} ({})", api, automatic ? "automatic" : "chosen in the settings");
    else
      REXLOG_WARN("bo1: the {} renderer couldn't start; using the other one", api);
  }
#endif
}

void Bo1App::OnPostSetup() {
  // A scripted pad for tests (bo1_test_input). Its times count from here.
  if (const std::string script = REXCVAR_GET(bo1_test_input); !script.empty()) {
    if (auto* input = static_cast<rex::input::InputSystem*>(runtime() ? runtime()->input_system() : nullptr))
      input->AddDriver(std::make_unique<cod3::ScriptedInputDriver>(script));
    else
      REXLOG_WARN("bo1: no input system; bo1_test_input not used");
  }

  // Debug screenshots (bo1_shots.h): the game's own picture, read back from the GPU emulation's output.
  const auto user_dir = rex::cvar::GetFlagByName("user_data_root").empty()
                            ? root_dir_ / "userdata"
                            : std::filesystem::path(rex::cvar::GetFlagByName("user_data_root"));
  bo1::shots::Start(user_dir / "logs" / "screenshots", [this](rex::ui::RawImage& image) {
    auto* graphics = runtime() ? runtime()->graphics_system() : nullptr;
    auto* presenter = graphics ? graphics->presenter() : nullptr;
    return presenter && presenter->CaptureGuestOutput(image);
  });

  // [menus-native] While the F2 settings window is open the game gets no input: no keys, mouse or controller.
  if (auto* input = static_cast<rex::input::InputSystem*>(runtime() ? runtime()->input_system() : nullptr))
    input->SetActiveCallback([]() { return bo1::f2ui::GameGetsInput(); });
}

// [menus-native] The F2 settings window (bo1_f2.h): Call of Duty 3's window on Black Ops.
void Bo1App::OnConfigureFonts(ImFontAtlas* atlas) { bo1::f2ui::ConfigureFonts(atlas); }

void Bo1App::OnConfigureStyle(ImGuiStyle& imgui_style, rex::ui::Style& ui_style) {
  bo1::f2ui::ConfigureStyle(imgui_style, ui_style);
}

void Bo1App::OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) {
  // The window is made and closed after the frame that asks for it, outside the drawer's loop over its windows.
  bo1::f2ui::Attach(drawer, window(), root_dir_ / (std::string(kProgram) + ".toml"),
                    [this](std::function<void()> fn) { app_context().CallInUIThreadDeferred(std::move(fn)); });
}

void Bo1App::OnShutdown() {
  bo1::f2ui::Detach();  // [menus-native] saves what changed; before the ImGui drawer goes
  bo1::shots::Stop();  // before the GPU emulation's presenter goes
}
