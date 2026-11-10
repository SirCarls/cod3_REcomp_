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
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/presenter.h>

#include "game/debug_shots.h"
#include "game/frame_rate.h"
#include "game/front_end.h"

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
}

void Cod2App::OnShutdown() { cod2::shots::Stop(); }
