// cod3mp - the multiplayer program's host application (see cod3mp_app.h).

#include "cod3mp_app.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#endif

#include <SDL3/SDL.h>
#include <toml++/toml.hpp>

#include <rex/audio/downmix.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/window.h>

#include "app_defaults.h"
#include "game/color_output.h"
#include "game/relaunch.h"
#include "test_input.h"
#include "ui/theme.h"
#include "util.h"
#include "generated/codmp/cod3mp_init.h"

// The same name and meaning as the campaign's setting (cod3_app.cpp), so the
// campaign's volume carries over.
REXCVAR_DEFINE_DOUBLE(cod3_volume, 1.0, "cod3/Audio",
                      "Master volume: 0 silent, 1 as the game mixes it (World at War: snd_volume)")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// The campaign's scripted controller for automated tests (test_input.h).
REXCVAR_DEFINE_STRING(cod3_test_input, "", "cod3",
                      "Scripted controller input for automated testing (see src/test_input.h)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

std::unique_ptr<rex::ui::WindowedApp> Cod3MpApp::Create(rex::ui::WindowedAppContext& ctx) {
  return std::unique_ptr<Cod3MpApp>(new Cod3MpApp(ctx, "cod3mp", PPCImageConfig));
}

namespace {

// A default that only applies while the user hasn't set that option.
void SetDefault(const char* name, const char* value) {
  if (auto* info = rex::cvar::GetFlagInfo(name); info && info->source == rex::cvar::Source::kDefault)
    rex::cvar::SetFlagByName(name, value);
}

// Ends the process at once, as the campaign does (cod3_app.cpp, EndProcess):
// the SDK's teardown of the emulated console waits on game threads that
// never stop. Nothing in it keeps data: the game writes its files straight
// to the OS, and this program's settings are saved by the SDK's settings
// window when they change.
[[noreturn]] void EndProcess(const char* why) {
  REXLOG_INFO("cod3mp: {}; exiting", why);
  rex::FlushLogging();
#ifdef _WIN32
  ClipCursor(nullptr);
  TerminateProcess(GetCurrentProcess(), 0);
#endif
  std::_Exit(0);
}

// The safety net of the campaign's StartExitWatchdog: once shutdown has
// taken this long, the process ends anyway.
constexpr auto kExitGrace = std::chrono::seconds(5);
std::once_flag g_exit_watchdog;

void StartExitWatchdog(const char* why) {
  std::call_once(g_exit_watchdog, [why] {
    REXLOG_INFO("cod3mp: {}; the process ends within {} s even if a game thread hangs", why,
                kExitGrace.count());
    std::thread([] {
      std::this_thread::sleep_for(kExitGrace);
      REXLOG_WARN("cod3mp: shutdown stalled (a game thread didn't stop)");
      EndProcess("ending the process");
    }).detach();
  });
}

// Settings that belong to one program and are never taken from the
// campaign's cod3.toml: where files go (each program has its own log), what
// a relaunch hands over, the guest command line, and the mouse mode (the
// campaign aims with raw mouse input, which this program doesn't have yet).
bool IsPerProgramSetting(std::string_view name) {
  for (std::string_view own : {"log_file", "user_data_root", "game_data_root", "cache_root",
                               "update_data_root", "metadata_root", "cod3_launch_data", "cod3_test_input", "cl",
                               "mnk_mouse", "allow_game_relative_writes", "gpu_plugin"})
    if (name == own) return true;
  return false;
}

// Applies one TOML table the way the SDK reads a config file (nested tables
// join their keys with '_'), but only to settings this program has that are
// still at their default: cod3mp.toml, the environment and the command line
// all come first, and the campaign's own settings are left alone.
int ApplySharedTable(const toml::table& table, const std::string& prefix) {
  int applied = 0;
  for (const auto& [key, value] : table) {
    const std::string name = prefix.empty() ? std::string(key) : prefix + "_" + std::string(key);
    if (value.is_table()) {
      applied += ApplySharedTable(*value.as_table(), name);
      continue;
    }
    if (IsPerProgramSetting(name)) continue;
    const auto* info = rex::cvar::GetFlagInfo(name);
    if (!info || info->source != rex::cvar::Source::kDefault) continue;
    std::string text;
    if (value.is_boolean())
      text = value.as_boolean()->get() ? "true" : "false";
    else if (value.is_integer())
      text = std::to_string(value.as_integer()->get());
    else if (value.is_floating_point())
      text = std::to_string(value.as_floating_point()->get());
    else if (value.is_string())
      text = value.as_string()->get();
    else
      continue;
    if (rex::cvar::SetFlagByName(name, text)) ++applied;
  }
  return applied;
}

}  // namespace

// The campaign's portable layout (Cod3App::OnConfigurePaths): one folder with
// assets/, userdata/ and the settings, the executable in it, in its bin/
// subfolder, or an AppImage placed in it.
void Cod3MpApp::OnConfigurePaths(rex::PathConfig& paths) {
  std::vector<std::filesystem::path> candidates;
  if (const char* appimage = std::getenv("APPIMAGE"); appimage && *appimage)
    candidates.push_back(std::filesystem::path(appimage).parent_path());
  const auto exe_dir = rex::filesystem::GetExecutableFolder();
  candidates.push_back(exe_dir);
  candidates.push_back(exe_dir.parent_path());
  std::filesystem::path root = candidates.front();
  for (const auto& c : candidates) {
    std::error_code ec;
    if (std::filesystem::exists(c / "assets" / "codmp_xenonf.xex", ec)) {
      root = c;
      break;
    }
  }
  root_dir_ = root;
  if (paths.game_data_root.empty() && std::filesystem::exists(root / "assets" / "codmp_xenonf.xex"))
    paths.game_data_root = root / "assets";
  if (rex::cvar::GetFlagByName("user_data_root").empty()) {
    paths.user_data_root = root / "userdata";
    paths.cache_root = root / "userdata" / "cache";
  }
  userdata_dir_ = paths.user_data_root.empty() ? root / "userdata" : paths.user_data_root;
  // This program's own changes; the campaign's settings are read in
  // OnPostInitLogging, after this file.
  paths.config_path = root / "cod3mp.toml";

  // Logs go to userdata/logs/cod3mp.log (never into a read-only AppImage
  // mount; forward slashes, see PathToUtf8Generic in util.h).
  if (rex::cvar::GetFlagByName("log_file").empty()) {
    std::error_code ec;
    std::filesystem::create_directories(userdata_dir_ / "logs", ec);
    rex::cvar::SetFlagByName("log_file", cod3::PathToUtf8Generic(userdata_dir_ / "logs" / "cod3mp.log"));
  }
}

// The campaign's settings: keys, controls, window, video, audio, network.
// Read after cod3mp.toml so that anything changed here wins.
void Cod3MpApp::OnPostInitLogging() {
  const auto shared = root_dir_ / "cod3.toml";
  std::error_code ec;
  if (!std::filesystem::exists(shared, ec)) return;
  try {
    const toml::table table = toml::parse_file(cod3::PathToUtf8(shared));
    const int applied = ApplySharedTable(table, "");
    REXLOG_INFO("cod3mp: {} settings taken from the campaign's {}", applied, cod3::PathToUtf8(shared));
  } catch (const toml::parse_error& e) {
    REXLOG_WARN("cod3mp: couldn't read the campaign's settings ({}): {}", cod3::PathToUtf8(shared),
                e.what());
  }
}

// The disc's multiplayer executable (the SDK starts game:\\default.xex).
void Cod3MpApp::OnLoadXexImage(std::string& xex_image) { xex_image = "game:\\codmp_xenonf.xex"; }

void Cod3MpApp::OnPreSetup(rex::RuntimeConfig& config) {
  // Without the disc's multiplayer files the game reads a missing
  // mp\global.cod and stops on a null pointer, so say so and stop here
  // instead (the campaign's Multiplayer item checks the same, game/multiplayer.h).
  {
    std::error_code ec;
    if (!std::filesystem::is_regular_file(root_dir_ / "assets" / "mp" / "global.cod", ec)) {
      REXLOG_ERROR("cod3mp: no assets/mp/global.cod in {}", cod3::PathToUtf8(root_dir_));
      rex::ShowSimpleMessageBox(rex::SimpleMessageBoxType::Error,
                                "Call of Duty 3 multiplayer needs the disc's multiplayer files "
                                "(assets/mp), and this copy doesn't have them. Extract the disc again "
                                "with the setup tool.");
      EndProcess("the multiplayer files are missing");
    }
  }
  // The GPU plugin: a plain name only (Cod3App::OnPreSetup has the reason).
  if (!config.gpu_plugin.empty() &&
      (config.gpu_plugin.size() > 32 ||
       !std::all_of(config.gpu_plugin.begin(), config.gpu_plugin.end(), [](char c) {
         return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
       }))) {
    REXLOG_WARN("cod3mp: gpu_plugin '{}' isn't a plain plugin name; using xenos", config.gpu_plugin);
    config.gpu_plugin = "xenos";
  }
  if (config.gpu_plugin.empty()) config.gpu_plugin = "xenos";
  // No guest command line (Com_Init runs it as forced writes into a fixed
  // buffer), and the game's own files stay read-only, as in the campaign.
  if (!rex::cvar::GetFlagByName("cl").empty()) {
    REXLOG_WARN("cod3mp: the guest command line (cl) isn't used");
    rex::cvar::SetFlagByName("cl", "");
  }
  if (rex::cvar::GetFlagByName("allow_game_relative_writes") == "true")
    rex::cvar::SetFlagByName("allow_game_relative_writes", "false");
  // The program's name in the system's sound settings, as the campaign does
  // (Cod3App::OnPreSetup); SDL_APP_NAME in the environment still wins.
  SDL_SetHint(SDL_HINT_APP_NAME, "Call of Duty 3 Multiplayer");
#ifdef _WIN32
  // 1 ms sleeps for the vblank timer and the GPU emulation's waits (the
  // campaign's SetUpHostTiming, game/frame_rate.h).
  timeBeginPeriod(1);
#endif

  // Keyboard and mouse with the campaign's World at War-like binds. The
  // mouse moves the right stick (the SDK's mouse emulation) until this
  // program has the campaign's raw mouse aim.
  SetDefault("mnk_mode", "true");
  SetDefault("mnk_mouse", "true");
  for (const auto& d : cod3::kKeybindDefaults) SetDefault(d.cvar, d.value);
}

// The GPU plugin's settings exist only once it is loaded (Cod3App has the
// story). The title's known-good settings (app_defaults.h) suit this
// program too: it is the same title (415607E1). The vblank stays at the
// console's 60 Hz: the campaign's frame limiter, which makes a faster vblank
// safe, hooks default.xex.
void Cod3MpApp::ApplyGpuDefaults() {
  for (const auto& d : cod3::kVideoDefaults) SetDefault(d.cvar, d.value);
  cod3::ApplyColorSpace();  // the picture's colors, as in the campaign (game/color_output.h)
}

bool Cod3MpApp::ConstructRuntime(const rex::PathConfig& paths) {
  ApplyGpuDefaults();
  return rex::ReXApp::ConstructRuntime(paths);
}

void Cod3MpApp::OnPostSetup() {
  ApplyGpuDefaults();  // again, in case the runtime loaded the plugin later
  // While one of the SDK's windows (log, settings) wants the mouse, the game
  // gets no input.
  if (auto* input = static_cast<rex::input::InputSystem*>(runtime()->input_system())) {
    if (!REXCVAR_GET(cod3_test_input).empty())
      input->AddDriver(std::make_unique<cod3::ScriptedInputDriver>(REXCVAR_GET(cod3_test_input)));
    input->SetActiveCallback([this]() {
      auto* drawer = imgui_drawer();
      if (!drawer || !drawer->HasDialogs()) return true;
      return !drawer->GetIO().WantCaptureMouse;
    });
  }
  rex::audio::SetOutputGain(float(REXCVAR_GET(cod3_volume)));
  rex::cvar::RegisterChangeCallback("cod3_volume", [](std::string_view, std::string_view value) {
    rex::audio::SetOutputGain(std::clamp(std::strtof(std::string(value).c_str(), nullptr), 0.0f, 2.0f));
  });
  REXLOG_INFO("cod3mp: Call of Duty 3 multiplayer (codmp_xenonf.xex), system link over the local network");
}

void Cod3MpApp::OnConfigureFonts(ImFontAtlas* atlas) { cod3::ui::ConfigureFonts(atlas); }

void Cod3MpApp::OnConfigureStyle(ImGuiStyle& imgui_style, rex::ui::Style& ui_style) {
  cod3::ui::ApplyTheme(imgui_style);
  const auto& p = cod3::ui::CurrentPalette();
  ui_style.settings.warning = p.warn;
  ui_style.console.completion_highlight = ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.45f);
}

bool Cod3MpApp::OnWindowCloseRequested() { EndProcess("window closed"); }

// The game's main thread ended by itself: multiplayer's way back to the
// campaign (its `reboot` command launches default.xex), its own restart, or
// the dashboard. As in the campaign, a launch starts the program asked for
// with the game's launch data (game/relaunch.h), and the process ends here.
void Cod3MpApp::OnGuestThreadExit(rex::system::XThread* thread) {
  (void)thread;
  const bool relaunch = cod3::RelaunchRequested();
  StartExitWatchdog("game exited");
  app_context().CallInUIThread([this, relaunch]() {
    if (auto* win = window())
      cod3::NoteWindowState(win->IsFullscreen(), win->GetActualLogicalWidth(), win->GetActualLogicalHeight());
    if (relaunch && cod3::Relaunch(userdata_dir_)) EndProcess("the new instance takes over");
    EndProcess("the game ended");
  });
}

// Before the game's first instruction: the controls block the campaign
// handed over (MP_ReadLaunchData 0x82415AA0 reads it).
void Cod3MpApp::OnPostLaunchModule(rex::system::XThread* thread) {
  (void)thread;
  cod3::RestoreLaunchData(userdata_dir_);
}

void Cod3MpApp::OnShutdown() { StartExitWatchdog("shutting down"); }
