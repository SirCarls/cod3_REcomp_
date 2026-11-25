// codclassic - Call of Duty Classic (Xbox Live Arcade) recompiled for PC: paths and graphics.
// [acct2-uo]
//
// The game's folder looks like Call of Duty 3's:
//   <root>/assets           the package's files (game_data_root), read only
//   <root>/userdata         saves, caches and logs (userdata/logs/codclassic.log, codclassicmp.log)
//   <root>/codclassic.toml  the port's settings (codclassicmp.toml for multiplayer)
// The program sits in <root> or in its bin/ folder. Built twice: codclassic (the campaign,
// default.xex) and, with CODCLASSIC_MP, codclassicmp (cod_mp.xex; mp/).

#include "codclassic_app.h"

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

#include "game/port_defaults.h"

#if defined(CODCLASSIC_MP)
#include "generated/cod_mp/codclassicmp_init.h"
#else
#include "generated/default/codclassic_init.h"
#endif

REXCVAR_DEFINE_STRING(codclassic_graphics_api, "auto", "codclassic/Display",
                      "Windows: the graphics API: auto (Vulkan, or Direct3D 12 when Vulkan can't start), "
                      "vulkan or d3d12. Linux has Vulkan only.");
REXCVAR_DEFINE_BOOL(codclassic_no_gpu, false, "codclassic",
                    "Start without the GPU emulation (tests only: no picture)");

namespace {

#if defined(CODCLASSIC_MP)
constexpr const char* kProgram = "codclassicmp";
constexpr const char* kXex = "cod_mp.xex";
#else
constexpr const char* kProgram = "codclassic";
constexpr const char* kXex = "default.xex";
#endif

// The folder that holds assets/<this program's xex>: the program's own, or the one above it (a
// setup tool puts the program in bin/).
std::filesystem::path FindRoot() {
  const auto exe_dir = rex::filesystem::GetExecutableFolder();
  const std::vector<std::filesystem::path> candidates = {exe_dir, exe_dir.parent_path()};
  for (const auto& candidate : candidates) {
    std::error_code ec;
    if (std::filesystem::exists(candidate / "assets" / kXex, ec)) return candidate;
  }
  return exe_dir;
}

// A GPU plugin is loaded by name from the program's folder, so only a plain name is taken (a
// shared settings file never loads one from elsewhere).
bool IsPlainName(const std::string& name) {
  return !name.empty() && name.size() <= 32 &&
         std::all_of(name.begin(), name.end(), [](char c) {
           return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
         });
}

}  // namespace

std::unique_ptr<rex::ui::WindowedApp> CodClassicApp::Create(rex::ui::WindowedAppContext& ctx) {
  return std::unique_ptr<CodClassicApp>(new CodClassicApp(ctx, kProgram, PPCImageConfig));
}

void CodClassicApp::OnConfigurePaths(rex::PathConfig& paths) {
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

  // The log goes to userdata/logs/<program>.log unless the command line names one (the SDK's
  // default, <program folder>/logs, may be read only).
  if (const auto source = rex::cvar::GetFlagSource("log_file");
      source != rex::cvar::Source::kCommandLine && source != rex::cvar::Source::kEnvironment) {
    const auto logs = userdata_dir_ / "logs";
    std::filesystem::create_directories(logs, ec);
    rex::cvar::SetFlagByName("log_file", (logs / (std::string(kProgram) + ".log")).generic_string());
  }
}

// The SDK starts game:\default.xex, the campaign. codclassicmp is built from cod_mp.xex and has
// to run that one (with the campaign's program loaded, its threads would start at the campaign's
// addresses: acct2-cod2 met this with cod2mp).
void CodClassicApp::OnLoadXexImage(std::string& xex_image) { xex_image = std::string("game:\\") + kXex; }

void CodClassicApp::OnPreSetup(rex::RuntimeConfig& config) {
  // The keyboard as a controller, before the input drivers start.
  codclassic::port_defaults::ApplyInputDefaults();
  if (!config.gpu_plugin.empty() && !IsPlainName(config.gpu_plugin)) {
    REXLOG_WARN("{}: gpu_plugin '{}' isn't a plain plugin name; using xenos", kProgram, config.gpu_plugin);
    config.gpu_plugin = "xenos";
  }
  // The Xenos GPU emulation unless the user picked another plugin (or none).
  if (config.gpu_plugin.empty() && !REXCVAR_GET(codclassic_no_gpu)) config.gpu_plugin = "xenos";
#ifdef _WIN32
  // Windows builds have Direct3D 12 and Vulkan; "auto" tries Vulkan first, as on Linux, and the
  // SDK falls back to Direct3D 12 when it can't start.
  if (std::string api = REXCVAR_GET(codclassic_graphics_api);
      !config.graphics && !config.gpu_plugin.empty() && (api == "auto" || api == "d3d12" || api == "vulkan")) {
    const bool automatic = api == "auto";
    if (automatic) api = "vulkan";
    config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin, api);
    if (config.graphics)
      REXLOG_INFO("{}: graphics API {} ({})", kProgram, api, automatic ? "automatic" : "chosen in the settings");
    else
      REXLOG_WARN("{}: the {} renderer couldn't start; using the other one", kProgram, api);
  }
#endif
}

void CodClassicApp::OnPostSetup() {
  // The GPU plugin's settings exist once it is loaded (on Linux during the runtime's set-up); the
  // SDK reads the vblank rate every millisecond, so setting it here, before the game's first frame,
  // is in time (as Call of Duty 2's port does).
  codclassic::port_defaults::ApplyGpuDefaults();
}
