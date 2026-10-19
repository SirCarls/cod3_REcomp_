// cod3mp - the multiplayer program's host application (see cod3mp_app.h).

#include "cod3mp_app.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#endif

#include <SDL3/SDL.h>
#include <toml++/toml.hpp>

#include <rex/audio/downmix.h>
#include <rex/cvar.h>
#include <rex/cvar_file.h>
#include <rex/filesystem.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/system.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/presenter.h>
#include <rex/ui/window.h>

#include <typeinfo>

#include <rex/graphics/xenos.h>
#include <rex/system/kernel_state.h>

#include "app_defaults.h"
#include "game/addresses.h"
#include "game/color_output.h"
#include "game/diagnostics.h"
#include "game/engine.h"
#include "game/frame_rate.h"
#include "game/guest.h"
#include "game/hd_textures.h"
#include "game/mouse_look.h"
#include "game/mp/menus_mp.h"
#include "game/mp/session.h"
#include "game/relaunch.h"
#include "game/render_resolution.h"
#include "game/screenshot.h"
#include "game/ultrawide.h"
#include "game/watchdog.h"
#include "test_input.h"
#include "ui/problem_log.h"
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

REXCVAR_DECLARE(bool, cod3_raw_mouse);
REXCVAR_DECLARE(double, cod3_refresh_rate);

std::unique_ptr<rex::ui::WindowedApp> Cod3MpApp::Create(rex::ui::WindowedAppContext& ctx) {
  return std::unique_ptr<Cod3MpApp>(new Cod3MpApp(ctx, "cod3mp", PPCImageConfig));
}

namespace {

// This program's default for a setting: kept while the player hasn't chosen
// another, and left out of the settings file (as the campaign's SetDefault,
// rex/cvar_file.h, SDK patch 0042).
void SetDefault(const char* name, const char* value) {
  if (rex::cvar::GetFlagInfo(name) && !rex::cvar::SetDefaultValue(name, value))
    REXLOG_WARN("cod3mp: {} doesn't take the default '{}'", name, value);
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
// a relaunch hands over, the guest command line and one-off test switches.
//
// Also the campaign's own derived or policy values, which this program sets
// for itself or leaves alone: the render plan's scales and display shape
// (game/render_resolution.cpp, game/ultrawide.cpp), the texture packs
// (multiplayer allows only the insignia list, game/hd_textures.h), the 1000 Hz
// vblank (each program sets it with its own frame limiter) and the display
// gamma, which cod3_color_space sets in both.
bool IsPerProgramSetting(std::string_view name) {
  for (std::string_view own :
       {"log_file", "user_data_root", "game_data_root", "cache_root", "update_data_root", "metadata_root",
        "cod3_launch_data", "cod3_test_input", "cod3_autoexec", "cod3_autoexec_delay", "cl",
        "allow_game_relative_writes", "gpu_plugin",
        "resolution_scale", "draw_resolution_scale_x", "draw_resolution_scale_y", "video_mode_width",
        "video_mode_height", "window_width", "window_height", "guest_vblank_rate",
        "display_output_transfer", "kernel_display_gamma_type", "kernel_display_gamma_power"})
    if (name == own) return true;
  return name.rfind("texture_replacement", 0) == 0;
}

// The campaign's cod3.toml, read once and applied twice: before the GPU
// plugin is loaded, and again once its settings exist (ConstructRuntime).
std::optional<toml::table> g_shared_settings;
// cod3.toml was saved after cod3mp.toml: the campaign (or the setup tool)
// changed the shared settings last, so its values win over this program's
// saved copies too. Else only settings still at their default take them.
bool g_shared_settings_newer = false;

// Applies one TOML table the way the SDK reads a config file (nested tables
// join their keys with '_'), to the settings this program has. The
// environment and the command line come first; cod3mp.toml too, unless the
// campaign's file is the newer one (the last change, made in either program
// or the setup tool, wins).
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
    if (!info) continue;
    const bool open = info->source == rex::cvar::Source::kDefault ||
                      (g_shared_settings_newer && info->source == rex::cvar::Source::kConfig);
    if (!open) continue;
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
    if (rex::cvar::GetFlagByName(name) == text && info->source != rex::cvar::Source::kDefault) continue;
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
  config_path_ = paths.config_path;

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
    g_shared_settings = toml::parse_file(cod3::PathToUtf8(shared));
    std::error_code own_ec, shared_ec;
    const auto own_time = std::filesystem::last_write_time(root_dir_ / "cod3mp.toml", own_ec);
    const auto shared_time = std::filesystem::last_write_time(shared, shared_ec);
    g_shared_settings_newer = own_ec || (!shared_ec && shared_time > own_time);
    const int applied = ApplySharedTable(*g_shared_settings, "");
    REXLOG_INFO("cod3mp: {} settings taken from the campaign's {}{}", applied, cod3::PathToUtf8(shared),
                g_shared_settings_newer ? " (the newer file: it wins over cod3mp.toml)" : "");
  } catch (const toml::parse_error& e) {
    REXLOG_WARN("cod3mp: couldn't read the campaign's settings ({}): {}", cod3::PathToUtf8(shared),
                e.what());
  }
}

// The disc's multiplayer executable (the SDK starts game:\\default.xex).
void Cod3MpApp::OnLoadXexImage(std::string& xex_image) { xex_image = "game:\\codmp_xenonf.xex"; }

void Cod3MpApp::OnPreSetup(rex::RuntimeConfig& config) {
  // F2 > Console > Problems: every warning and error from here on (ui/problem_log.h).
  cod3::ui::ProblemLog::Get().Install();
  runtime_config_ = &config;  // its GPU plugin is loaded right after this (render resolution)
  // Before the window opens: the guest display shape the port sets for wide
  // pictures (game/ultrawide.h) would otherwise also size the window.
  cod3::ForgetSavedDisplayShape();
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
  // Windows: 1 ms sleeps for the vblank timer, the GPU emulation's waits and
  // the frame limiter, kept while the window seems hidden (game/frame_rate.h).
  cod3::SetUpHostTiming();
  if (auto* info = rex::cvar::GetFlagInfo("video_mode_refresh_rate");
      info && info->source != rex::cvar::Source::kCommandLine &&
      info->source != rex::cvar::Source::kEnvironment)
    rex::cvar::SetFlagByName("video_mode_refresh_rate", "60");  // the video mode the game sees

  // Keyboard and raw mouse aim with the campaign's World at War-like binds
  // (game/mouse_look.h); with raw mouse off, the mouse moves the right stick.
  for (const auto& d : cod3::kControlDefaults) {
    if (std::string_view(d.cvar) == "mnk_mouse" && !REXCVAR_GET(cod3_raw_mouse)) continue;
    SetDefault(d.cvar, d.value);
  }
  for (const auto& d : cod3::kKeybindDefaults) SetDefault(d.cvar, d.value);
}

// The GPU plugin's settings exist only once it is loaded (Cod3App has the
// story). The title's known-good settings (app_defaults.h) suit this
// program too: it is the same title (415607E1). The guest vblank runs at
// 1000 Hz as in the campaign: the frame limiter (game/frame_rate.h, hooked
// at this program's Com_Frame) makes com_maxfps the only limit.
void Cod3MpApp::ApplyGpuDefaults() {
  if (rex::cvar::GetFlagInfo("guest_vblank_rate"))
    SetDefault("guest_vblank_rate", "1000");
  else
    SetDefault("vsync", "false");  // SDK without patch 0008: the same 1000 Hz vblank
  for (const auto& d : cod3::kVideoDefaults) SetDefault(d.cvar, d.value);
  cod3::ApplyColorSpace();  // the picture's colors, as in the campaign (game/color_output.h)
  if (!gpu_defaults_logged_ && rex::cvar::GetFlagInfo("readback_resolve")) {
    gpu_defaults_logged_ = true;
    const std::string vblank = rex::cvar::GetFlagByName("guest_vblank_rate");
    REXLOG_INFO("cod3mp: guest vblank {} (vsync {}), readback_resolve {}, starting com_maxfps {}",
                vblank.empty() ? "1000 Hz via vsync off (SDK without patch 0008)" : vblank + " Hz",
                rex::cvar::GetFlagByName("vsync"), rex::cvar::GetFlagByName("readback_resolve"),
                rex::cvar::GetFlagByName("cod3_max_fps"));
  }
}

bool Cod3MpApp::ConstructRuntime(const rex::PathConfig& paths) {
  // The GPU plugin's settings (anti-aliasing, anisotropic filtering, resolve
  // readback ...) exist only now: the campaign's values for them too.
  if (g_shared_settings) {
    const int applied = ApplySharedTable(*g_shared_settings, "");
    if (applied) REXLOG_INFO("cod3mp: {} more settings (the GPU's) taken from the campaign's", applied);
  }
  ApplyGpuDefaults();
  ApplyRenderResolution();
  // The texture packs (game/hd_textures.h; in a match only the allowed list).
  cod3::ApplyHdTextures(root_dir_, runtime_config_ && runtime_config_->graphics);
  // The session header and the log detail (game/diagnostics.h).
  cod3::diag::Start(userdata_dir_ / "logs");
  // The hang watchdog and, on Windows, the crash logger (game/watchdog.h).
  cod3::watchdog::Start();
  return rex::ReXApp::ConstructRuntime(paths);
}

// The render resolution (render_resolution.h), as the campaign plans it
// (Cod3App::ApplyRenderResolution): the game draws the window's or screen's
// size itself; later changes are live switches.
void Cod3MpApp::ApplyRenderResolution() {
  if (render_plan_applied_ || !rex::cvar::GetFlagInfo("resolution_scale")) return;
  render_plan_applied_ = true;
  // The larger emulated EDRAM (SDK patch 0010) needs the edited resolve
  // shaders, which only the Vulkan backend has.
  bool vulkan = false;
  if (runtime_config_ && runtime_config_->graphics) {
    const auto& graphics = *runtime_config_->graphics;
    vulkan = std::string_view(typeid(graphics).name()).find("ulkan") != std::string_view::npos;
  }
  cod3::SetEdramTilesAvailable(vulkan ? std::max<uint32_t>(rex::graphics::xenos::kEdramTileCount,
                                                           cod3::kConsoleEdramTiles)
                                      : cod3::kConsoleEdramTiles);
  const std::string choice = rex::cvar::GetFlagByName("cod3_render_resolution");
  uint32_t view_width = 1280, view_height = 720;
  const char* view_source = "the default window";
  const bool fullscreen = rex::cvar::GetFlagByName("fullscreen") == "true";
  if (fullscreen && cod3::GetScreenSize(&view_width, &view_height)) {
    view_source = "the screen";
  } else if (window() && window()->GetActualPhysicalWidth()) {
    view_width = window()->GetActualPhysicalWidth();
    view_height = window()->GetActualPhysicalHeight();
    view_source = "the window";
  }
  const float aspect = cod3::AspectForWindow(view_width, view_height);
  cod3::UpdateDisplayAspect(view_width, view_height);
  const cod3::RenderPlan plan = cod3::PlanForView(choice, view_width, view_height);
  cod3::SetRenderPlan(plan);
  const std::string scale = std::to_string(plan.scale);
  rex::cvar::SetFlagByName("resolution_scale", scale);
  rex::cvar::SetFlagByName("draw_resolution_scale_x", scale);
  rex::cvar::SetFlagByName("draw_resolution_scale_y", scale);
  if (choice == "original") {
    REXLOG_INFO("cod3mp: render resolution original: the game draws its own 1040x624 with its 2x MSAA, "
                "stretched to {}x{} on screen",
                plan.display_width, plan.display_height);
    return;
  }
  REXLOG_INFO("cod3mp: render resolution {} ({} method) for {} {}x{} (shape {:.3f}:1; EDRAM {} tiles): "
              "the game draws {}",
              plan.choice, cod3::RenderMethodName(plan.method), view_source, view_width, view_height,
              aspect, plan.edram_tiles, cod3::DescribePlan(plan));
  if (!plan.note.empty()) REXLOG_INFO("cod3mp: render resolution: {}", plan.note);
}

void Cod3MpApp::OnPostSetup() {
  ApplyGpuDefaults();  // again, in case the runtime loaded the plugin later
  // This program's own console config (seta lines): the campaign's FOV,
  // binds and the like don't carry over by themselves (cg_fov's default is
  // 80 here, 65 there).
  cod3::Engine::Get().SetConfigPath(user_data_root() / "cod3mp_config.cfg");
  cod3::ui::GameConsole::Get().SetUserDir(user_data_root());
  // While the console is open, or one of the windows wants the keyboard or
  // the mouse, the game gets no input.
  if (auto* input = static_cast<rex::input::InputSystem*>(runtime()->input_system())) {
    if (!REXCVAR_GET(cod3_test_input).empty())
      input->AddDriver(std::make_unique<cod3::ScriptedInputDriver>(REXCVAR_GET(cod3_test_input)));
    input->SetActiveCallback([this]() {
      if (cod3::ui::GameConsole::IsOpen()) return false;
      auto* drawer = imgui_drawer();
      if (!drawer || !drawer->HasDialogs()) return true;
      return !drawer->GetIO().WantCaptureMouse && !drawer->GetIO().WantTextInput;
    });
  }
  // Console quit / exit: save and end, like the window's close button.
  cod3::ui::GameConsole::Get().SetQuitHandler([this]() {
    app_context().CallInUIThreadDeferred([this]() { SaveAndExit("quit from the console"); });
  });
  // The main menu's QUIT TO DESKTOP (game/mp/menus_mp.h), the same way.
  cod3::mpmenu::SetQuitHandler([this]() {
    app_context().CallInUIThreadDeferred([this]() { SaveAndExit("quit to desktop from the main menu"); });
  });
  // A World at War name changed a port setting: save cod3mp.toml like F2's Save.
  cod3::ui::GameConsole::Get().SetSettingsChangedHandler([this]() {
    try {
      rex::cvar::SaveConfig(config_path_);
    } catch (const std::exception& e) {
      REXLOG_WARN("cod3mp: couldn't save {}: {}", cod3::PathToUtf8(config_path_), e.what());
    }
  });
  // The sound output moved with a switch in the system's sound settings
  // (SDK patch 0050 sets audio_device on the audio thread): saved on the UI
  // thread like F2's choice.
  rex::cvar::RegisterChangeCallback("audio_device", [this](std::string_view, std::string_view) {
    app_context().CallInUIThreadDeferred([this]() {
      try {
        rex::cvar::SaveConfig(config_path_);
      } catch (const std::exception& e) {
        REXLOG_WARN("cod3mp: couldn't save {}: {}", cod3::PathToUtf8(config_path_), e.what());
      }
    });
  });
  // Screenshots for the debug log and bug reports (game/screenshot.h): the
  // game's own picture from the GPU emulation's output, as in the campaign.
  cod3::shot::Start(userdata_dir_ / "logs" / "screenshots", [this](rex::ui::RawImage& image) {
    auto* graphics = runtime() ? runtime()->graphics_system() : nullptr;
    auto* presenter = graphics ? graphics->presenter() : nullptr;
    return presenter && presenter->CaptureGuestOutput(image);
  });
  rex::audio::SetOutputGain(float(REXCVAR_GET(cod3_volume)));
  rex::cvar::RegisterChangeCallback("cod3_volume", [](std::string_view, std::string_view value) {
    rex::audio::SetOutputGain(std::clamp(std::strtof(std::string(value).c_str(), nullptr), 0.0f, 2.0f));
  });
  cod3::ui::GameConsole::Get().Print(
      cod3::ui::ConsoleChannel::kSystem,
      "Call of Duty 3 multiplayer, static recomp. Press ~ for the console, Shift+~ for the full console, "
      "F2 for the settings.\n");
  REXLOG_INFO("cod3mp: Call of Duty 3 multiplayer (codmp_xenonf.xex), system link over the local network");
}

// The console keys, the settings window, raw mouse aim and the HUD (the
// campaign's Cod3App::OnCreateDialogs, without what only the campaign has).
void Cod3MpApp::OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) {
  (void)drawer;
  // The SDK's log viewer defaults to the backtick key: that key opens the
  // game console here too, and the log viewer moves to F10.
  if (rex::cvar::GetFlagByName("bind_console") == "Backtick") rex::cvar::SetFlagByName("bind_console", "F10");
  key_listener_.on_toggle = [this](bool shift) { ToggleConsole(shift); };
  key_listener_.on_close = [this]() { CloseConsole(); };
  key_listener_.on_settings = [this]() { ToggleSettings(); };
  hud_ = std::make_unique<cod3::ui::HudOverlay>(imgui_drawer());
  hud_->on_frame = [this]() {
    UpdateMouseCapture();
    if (auto* win = window()) {
      // The picture follows the window's shape live (game/ultrawide.h).
      cod3::UpdateDisplayAspect(win->GetActualPhysicalWidth(), win->GetActualPhysicalHeight());
      // A relaunch (back to the campaign) opens the window the same way (game/relaunch.h).
      cod3::NoteWindowState(win->IsFullscreen(), win->GetActualLogicalWidth(), win->GetActualLogicalHeight());
    }
    cod3::LogFramePacingOnce(window());  // one log line on the frame pacing (game/frame_rate.h)
    if (module_launched_.load(std::memory_order_acquire)) {
      cod3::mp::Tick();      // match state (game/mp/session.h)
      cod3::mpmenu::Tick();  // the voice / session system check (game/mp/menus_mp.h)
    }
  };
  // Above the ImGui drawer (z 64) so the toggle key never reaches a text box.
  window()->AddInputListener(&key_listener_, 200);
  mouse_listener_.window = window();
  window()->AddInputListener(&mouse_listener_, 199);
  // Fullscreen refresh rate (F2 > Video; SDK patch 0013).
  window()->SetFullscreenRefreshRate(REXCVAR_GET(cod3_refresh_rate));
}

void Cod3MpApp::ToggleConsole(bool full) {
  if (!imgui_drawer()) return;
  if (!console_) {
    console_ = std::make_unique<cod3::ui::GameConsoleDialog>(imgui_drawer(), full);
  } else if (full && !console_->full()) {
    console_->SetFull(true);
  } else {
    CloseConsole();
  }
}

void Cod3MpApp::CloseConsole() { console_.reset(); }

void Cod3MpApp::ToggleSettings() {
  if (!imgui_drawer()) return;
  if (settings_) {
    settings_->SaveIfDirty();
    settings_.reset();
    return;
  }
  settings_ = std::make_unique<cod3::ui::SettingsWindow>(imgui_drawer(), config_path_, [this]() {
    app_context().CallInUIThreadDeferred([this]() { settings_.reset(); });
  });
  settings_->set_window(window());
}

void MpConsoleKeyListener::OnKeyDown(rex::ui::KeyEvent& e) {
  // A held key repeats (prev_state): it toggles once, on the first press.
  if (e.virtual_key() == rex::ui::VirtualKey::kOem3) {  // ` and ~ on US layouts
    if (on_toggle && !e.prev_state()) on_toggle(e.is_shift_pressed());
    e.set_handled(true);
  } else if (e.virtual_key() == rex::ui::VirtualKey::kF2) {
    if (on_settings && !e.prev_state()) on_settings();
    e.set_handled(true);
  } else if (e.virtual_key() == rex::ui::VirtualKey::kEscape && cod3::ui::GameConsole::IsOpen()) {
    if (on_close) on_close();
    e.set_handled(true);
  }
}

void MpConsoleKeyListener::OnKeyChar(rex::ui::KeyEvent& e) {
  const auto c = static_cast<unsigned>(e.virtual_key());
  if (c == '`' || c == '~') e.set_handled(true);  // never type the toggle key
}

// Raw mouse aim (the campaign's MouseLookListener): the counts are added up
// here and applied once per usercmd (MouseLook::ApplyToGuest), so the event
// rate never changes the game's speed.
void MpMouseLookListener::OnMouseMove(rex::ui::MouseEvent& e) {
  if (!captured) return;
  e.set_handled(true);
  ++moves;
  if (relative) {
    cod3::MouseLook::Get().AddDelta(e.dx(), e.dy());
    return;
  }
  cod3::MouseLook::Get().AddDelta(float(e.x() - prev_x), float(e.y() - prev_y));
  int32_t cx = 0, cy = 0;
  if (window && window->WarpMouseToCenter(cx, cy)) {
    prev_x = cx;
    prev_y = cy;
  } else {
    prev_x = e.x();
    prev_y = e.y();
  }
}

// Hold the pointer only while the game builds usercmds (play) and neither
// a game menu (the pause menu over play too), the console nor F2 is open.
// Player 1's view angles (pitch, yaw) in the game's client state, for the
// capture log lines: they show whether the mouse turned the view.
static std::pair<float, float> ViewAngles() {
  auto* ks = rex::system::kernel_state();
  uint8_t* base = ks && ks->memory() ? ks->memory()->virtual_membase() : nullptr;
  if (!base) return {0.0f, 0.0f};
  const uint32_t angles = cod3::addr::kClientActive + cod3::addr::kViewAnglesOffset;
  return {cod3::guest::ReadF32(base, angles), cod3::guest::ReadF32(base, angles + 4)};
}

void Cod3MpApp::UpdateMouseCapture() {
  auto* win = window();
  if (!win) return;
  const bool want = cod3::MouseLook::Get().WantsCapture() && win->HasFocus() &&
                    !cod3::ui::GameConsole::IsOpen() && !settings_ && !cod3::mpmenu::MenuActive();
  auto& m = mouse_listener_;
  if (want == m.captured) return;
  cod3::MouseLook::Get().ClearPending();
  if (want) {
    m.prev_cursor = win->GetCursorVisibility();
    win->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
    win->CaptureMouse();
    m.relative = win->SetRelativeMouseMode(true);
    if (!m.relative) win->WarpMouseToCenter(m.prev_x, m.prev_y);
    m.captured = true;
    m.moves = 0;
    m.view_at_capture = ViewAngles();
    REXLOG_INFO("cod3mp: mouse captured for aiming ({}); view pitch {:.1f}, yaw {:.1f}",
                m.relative ? "relative mode" : "recentering", m.view_at_capture.first,
                m.view_at_capture.second);
  } else {
    if (m.relative) win->SetRelativeMouseMode(false);
    win->ReleaseMouse();
    win->SetCursorVisibility(m.prev_cursor);
    m.captured = false;
    m.relative = false;
    const auto now = ViewAngles();
    REXLOG_INFO("cod3mp: mouse released after {} motion events; view pitch {:.1f} -> {:.1f}, yaw {:.1f} -> {:.1f}",
                m.moves, m.view_at_capture.first, now.first, m.view_at_capture.second, now.second);
  }
}

void Cod3MpApp::SaveAll() {
  if (settings_) settings_->SaveIfDirty();
  cod3::Engine::Get().SaveConfig();
}

void Cod3MpApp::SaveAndExit(const char* why) {
  SaveAll();
  EndProcess(why);
}

void Cod3MpApp::OnConfigureFonts(ImFontAtlas* atlas) { cod3::ui::ConfigureFonts(atlas); }

void Cod3MpApp::OnConfigureStyle(ImGuiStyle& imgui_style, rex::ui::Style& ui_style) {
  cod3::ui::ApplyTheme(imgui_style);
  const auto& p = cod3::ui::CurrentPalette();
  ui_style.settings.warning = p.warn;
  ui_style.console.completion_highlight = ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.45f);
}

bool Cod3MpApp::OnWindowCloseRequested() { SaveAndExit("window closed"); }

// The game's main thread ended by itself: multiplayer's way back to the
// campaign (its `reboot` command launches default.xex), its own restart, or
// the dashboard. As in the campaign, a launch starts the program asked for
// with the game's launch data (game/relaunch.h), and the process ends here.
void Cod3MpApp::OnGuestThreadExit(rex::system::XThread* thread) {
  (void)thread;
  const bool relaunch = cod3::RelaunchRequested();
  StartExitWatchdog("game exited");
  app_context().CallInUIThread([this, relaunch]() {
    SaveAll();
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
  module_launched_.store(true, std::memory_order_release);
}

void Cod3MpApp::OnShutdown() {
  StartExitWatchdog("shutting down");
  cod3::shot::Stop();  // before the GPU emulation's presenter goes
  console_.reset();
  if (settings_) settings_->SaveIfDirty();
  settings_.reset();
  hud_.reset();
  if (window()) {
    if (mouse_listener_.captured) {
      window()->SetRelativeMouseMode(false);
      window()->ReleaseMouse();
    }
    window()->RemoveInputListener(&mouse_listener_);
    window()->RemoveInputListener(&key_listener_);
  }
  cod3::Engine::Get().SaveConfig();
}
