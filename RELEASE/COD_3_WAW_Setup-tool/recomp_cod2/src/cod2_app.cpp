// cod2 - Call of Duty 2 (Xbox 360) recompiled for PC: paths and graphics.
// [acct2-cod2]
//
// The game's folder looks like Call of Duty 3's:
//   <root>/assets     the disc's files (game_data_root), read only
//   <root>/userdata   saves, caches and logs (userdata/logs/cod2.log, cod2mp.log)
//   <root>/cod2.toml  the port's settings (cod2mp.toml for multiplayer)
// The program sits in <root> or in its bin/ folder. Built twice: cod2 (the
// campaign, default.xex) and, with COD2_MP, cod2mp (default_mp.xex; mp/).

#include "cod2_app.h"

#include <algorithm>
#include <cctype>
#include <filesystem>
#include <string>
#include <system_error>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/input_system.h>  // [f2-menus] the input system's active callback
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/presenter.h>

#include "cod2_f2.h"  // [f2-menus] the F2 settings window
#include "cod2_pc_input.h"  // [f2-menus] the PC copy's keys and mouse
#include "game/debug_shots.h"
#include "game/frame_rate.h"
#include "game/front_end.h"
#include "game/native_compare.h"  // [waw-decomp] 2026-11-11 the native renderer's live A/B compare
#include "game/native_side.h"  // [waw-decomp] 2026-11-10
#include "game/native_window.h"  // [waw-decomp] 2026-11-11 4z61: cod2_native_vulkan

#if defined(COD2_MP)
#include "generated/default_mp/cod2mp_init.h"
#else
#include "generated/default/cod2_init.h"
#endif

REXCVAR_DEFINE_STRING(cod2_graphics_api, "auto", "cod2/Display",
                      "Windows: the graphics API: auto (Vulkan, or Direct3D 12 when Vulkan can't start), "
                      "vulkan or d3d12. Linux has Vulkan only.");
REXCVAR_DEFINE_BOOL(cod2_no_gpu, false, "cod2",
                    "Start without the GPU emulation (tests only: no picture)");

namespace {

#if defined(COD2_MP)
constexpr const char* kProgram = "cod2mp";
constexpr const char* kXex = "default_mp.xex";
#else
constexpr const char* kProgram = "cod2";
constexpr const char* kXex = "default.xex";
#endif

// The folder that holds assets/<this program's xex>: the program's own, or the one
// above it (the setup tool puts the program in bin/).
std::filesystem::path FindRoot() {
  const auto exe_dir = rex::filesystem::GetExecutableFolder();
  const std::vector<std::filesystem::path> candidates = {exe_dir, exe_dir.parent_path()};
  for (const auto& c : candidates) {
    std::error_code ec;
    if (std::filesystem::exists(c / "assets" / kXex, ec)) return c;
  }
  return exe_dir;
}

// A GPU plugin is loaded by name from the program's folder, so only a plain
// name is taken (a shared settings file never loads one from elsewhere).
bool IsPlainName(const std::string& name) {
  return !name.empty() && name.size() <= 32 &&
         std::all_of(name.begin(), name.end(), [](char c) {
           return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
         });
}

}  // namespace

std::unique_ptr<rex::ui::WindowedApp> Cod2App::Create(rex::ui::WindowedAppContext& ctx) {
  return std::unique_ptr<Cod2App>(new Cod2App(ctx, kProgram, PPCImageConfig));
}

void Cod2App::OnConfigurePaths(rex::PathConfig& paths) {
  const auto root = FindRoot();
  std::error_code ec;
  if (paths.game_data_root.empty() && std::filesystem::exists(root / "assets" / kXex, ec))
    paths.game_data_root = root / "assets";
  if (rex::cvar::GetFlagByName("user_data_root").empty()) {
    paths.user_data_root = root / "userdata";
    paths.cache_root = root / "userdata" / "cache";
  }
  paths.config_path = root / (std::string(kProgram) + ".toml");
  config_path_ = paths.config_path;  // [f2-menus] where the F2 window saves
  userdata_dir_ = paths.user_data_root;
  if (userdata_dir_.empty()) {
    const std::string flag{rex::cvar::GetFlagByName("user_data_root")};
    userdata_dir_ = flag.empty() ? root / "userdata" : std::filesystem::path(flag);
  }

  // The log goes to userdata/logs/<program>.log unless the command line names one
  // (the SDK's default, <program folder>/logs, may be read only).
  if (const auto source = rex::cvar::GetFlagSource("log_file");
      source != rex::cvar::Source::kCommandLine && source != rex::cvar::Source::kEnvironment) {
    const auto logs = (paths.user_data_root.empty() ? root / "userdata" : paths.user_data_root) / "logs";
    std::filesystem::create_directories(logs, ec);
    rex::cvar::SetFlagByName("log_file", (logs / (std::string(kProgram) + ".log")).generic_string());
  }
}

// The SDK starts game:\default.xex, the campaign. cod2mp is built from
// default_mp.xex and has to run that one: with the campaign's program loaded,
// its first thread started at an address of the campaign's program, which in
// the multiplayer code is the middle of another function (the "No function
// registered at 8228BAB0" of the first multiplayer runs).
void Cod2App::OnLoadXexImage(std::string& xex_image) { xex_image = std::string("game:\\") + kXex; }

void Cod2App::OnPreSetup(rex::RuntimeConfig& config) {
  // The keyboard as a controller, so Return reaches the Press START screen
  // (game/front_end.h).
  cod2::front_end::ApplyInputDefaults();
  if (!config.gpu_plugin.empty() && !IsPlainName(config.gpu_plugin)) {
    REXLOG_WARN("cod2: gpu_plugin '{}' isn't a plain plugin name; using xenos", config.gpu_plugin);
    config.gpu_plugin = "xenos";
  }
  // The Xenos GPU emulation unless the user picked another plugin (or none).
  if (config.gpu_plugin.empty() && !REXCVAR_GET(cod2_no_gpu)) config.gpu_plugin = "xenos";
  // [waw-decomp] 2026-11-11 4z61: the native renderer as the game's picture (cod2_native_vulkan, off by default): the
  // plugin's host backend in place of the GPU emulation (game/native_window.h); then the block below is skipped
  cod2::native::StartNativeVulkan(config);
#ifdef _WIN32
  // Windows builds have Direct3D 12 and Vulkan; "auto" tries Vulkan first, as
  // on Linux, and the SDK falls back to Direct3D 12 when it can't start.
  if (std::string api = REXCVAR_GET(cod2_graphics_api);
      !config.graphics && !config.gpu_plugin.empty() && (api == "auto" || api == "d3d12" || api == "vulkan")) {
    const bool automatic = api == "auto";
    if (automatic) api = "vulkan";
    config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin, api);
    if (config.graphics)
      REXLOG_INFO("cod2: graphics API {} ({})", api, automatic ? "automatic" : "chosen in the settings");
    else
      REXLOG_WARN("cod2: the {} renderer couldn't start; using the other one", api);
  }
#endif
}

// [f2-menus] The window's keys and mouse for the PC copy's keys and the game's own mouse look (cod2_pc_input.h).
static cod2::pc_input::Listener g_pc_input_listener;

void Cod2App::OnPostSetup() {
  // The guest vblank at 1000 Hz, the frame limit the port's (game/frame_rate.h).
  // The GPU plugin's settings exist once it is loaded, which on Linux is
  // during the runtime's set-up; the SDK reads the rate every millisecond, so
  // setting it here, before the game's first frame, is in time.
  cod2::frame_rate::ApplyGpuDefaults();
  // Pictures of the game for test runs (cod2_debug_shot_seconds; off by default).
  cod2::shots::Start(userdata_dir_ / "logs" / "screenshots", [this](rex::ui::RawImage& image) {
    auto* graphics = runtime() ? runtime()->graphics_system() : nullptr;
    auto* presenter = graphics ? graphics->presenter() : nullptr;
    return presenter && presenter->CaptureGuestOutput(image);
  });
  // [waw-decomp] 2026-11-11: the native renderer's live A/B compare (game/native_compare.h; cod2_native_compare,
  // Ctrl+Home; off by default): a drawer on the presenter, under ImGui's, and its keys, before the PC copy's (only
  // Ctrl+Home, and Ctrl+arrows while the compare is on, are taken); never while the F2 window has the input.
  if (auto* graphics = runtime() ? runtime()->graphics_system() : nullptr)
    cod2::compare::Attach(window(), graphics->presenter(), graphics->provider(), 202,
                          []() { return !cod2::f2ui::GameGetsInput(); });
  // [waw-decomp] 2026-11-10: the native renderer's 2D beside the game's (cod2_debug_native_side; off by default).
  cod2::native::SideStart(userdata_dir_ / "logs" / "screenshots", [this](rex::ui::RawImage& image) {
    auto* graphics = runtime() ? runtime()->graphics_system() : nullptr;
    auto* presenter = graphics ? graphics->presenter() : nullptr;
    return presenter && presenter->CaptureGuestOutput(image);
  });
  // [f2-menus] While the F2 settings window is open the game gets no input: no keys, mouse or controller.
  if (auto* input = static_cast<rex::input::InputSystem*>(runtime() ? runtime()->input_system() : nullptr))
    input->SetActiveCallback([]() { return cod2::f2ui::GameGetsInput(); });
  // [f2-menus] The PC copy's keys and mouse (cod2_pc_input.h): above the SDK's keyboard/mouse driver, so a key it
  // takes doesn't press a controller button as well.
  if (auto* win = window()) {
    g_pc_input_listener.window = win;
    win->AddInputListener(&g_pc_input_listener, 199);
  }
}

// [f2-menus] The F2 settings window (cod2_f2.h): Call of Duty 3's window with Call of Duty 2's pages.
void Cod2App::OnConfigureFonts(ImFontAtlas* atlas) { cod2::f2ui::ConfigureFonts(atlas); }

void Cod2App::OnConfigureStyle(ImGuiStyle& imgui_style, rex::ui::Style& ui_style) {
  cod2::f2ui::ConfigureStyle(imgui_style, ui_style);
}

void Cod2App::OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) {
  // The window is made and closed after the frame that asks for it, outside the drawer's loop over its windows.
  cod2::f2ui::Attach(drawer, window(), config_path_,
                     [this](std::function<void()> fn) { app_context().CallInUIThreadDeferred(std::move(fn)); });
}

void Cod2App::OnShutdown() {
  if (auto* win = window()) win->RemoveInputListener(&g_pc_input_listener);  // [f2-menus]
  cod2::f2ui::Detach();  // [f2-menus] saves what changed; before the ImGui drawer goes
  cod2::native::SideStop();  // [waw-decomp] 2026-11-10
  cod2::compare::Detach(window());  // [waw-decomp] 2026-11-11: before the presenter goes
  cod2::shots::Stop();
}
