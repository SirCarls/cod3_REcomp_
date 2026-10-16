// cod3 - host application: wires the console, config and input gating.

#include "cod3_app.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <mutex>
#include <system_error>
#include <string_view>
#include <thread>
#include <typeinfo>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#endif

#include <SDL3/SDL.h>
#include <rex/audio/downmix.h>
#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/input/input_system.h>
#include <rex/logging.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/graphics/xenos.h>
#include <rex/runtime.h>
#include <rex/system/gpu_plugin.h>
#include <rex/ui/window.h>

#include "app_defaults.h"
#include "util.h"
#include "game/engine.h"
#include "game/mouse_look.h"
#include "game/pc_menu.h"
#include "game/button_prompts.h"
#include "game/color_output.h"
#include "game/diagnostics.h"
#include "game/frame_rate.h"
#include "game/hd_textures.h"
#include "game/menu_video.h"
#include "game/multiplayer.h"
#include "game/relaunch.h"
#include "game/render_resolution.h"
#include "game/ultrawide.h"
#include "game/watchdog.h"
#include "test_input.h"
#include "ui/theme.h"
#include "generated/default/cod3_init.h"

REXCVAR_DEFINE_INT32(cod3_settings_version, 0, "cod3",
                     "Which one-time fixes of saved settings have run (set by the game)");

REXCVAR_DEFINE_DOUBLE(cod3_volume, 1.0, "cod3/Audio",
                      "Master volume: 0 silent, 1 as the game mixes it (World at War: snd_volume)")
    .range(0.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(cod3_no_gpu, false, "cod3",
                    "Run without GPU emulation (no picture; for debugging boot issues)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

std::unique_ptr<rex::ui::WindowedApp> Cod3App::Create(rex::ui::WindowedAppContext& ctx) {
  return std::unique_ptr<Cod3App>(new Cod3App(ctx, "cod3", PPCImageConfig));
}

REXCVAR_DEFINE_STRING(cod3_test_input, "", "cod3",
                      "Scripted controller input for automated testing (see src/test_input.h)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DECLARE(bool, cod3_raw_mouse);
REXCVAR_DECLARE(double, cod3_refresh_rate);
REXCVAR_DECLARE(std::string, cod3_graphics_api);

namespace {
// A default that only applies while the user hasn't set that option
// themselves (config file, environment or command line).
void SetDefault(const char* name, const char* value) {
  if (auto* info = rex::cvar::GetFlagInfo(name); info && info->source == rex::cvar::Source::kDefault)
    rex::cvar::SetFlagByName(name, value);
}

// Ends the process at once. Every way out of the game (window close button,
// console quit, the game ending by itself) saves the settings first and then
// comes here, instead of the SDK's teardown of the emulated console:
//  - The game's threads never all stop. Three of the four job workers stay
//    blocked on their auto-reset event (game/job_wake.cpp): stopping the
//    title (KernelState::TerminateTitle) sets every event once, and a set
//    auto-reset event lets only one waiter go.
//  - With those threads still waiting, the teardown hangs. When the game
//    ends by itself (the Multiplayer item asked for codmp_xenonf.xex, the
//    dashboard, a failed reboot), the SDK destroys the window before the
//    input system, and the keyboard/mouse driver then calls into the freed
//    window from its destructor and blocks for good (gdb: ~MnkInputDriver ->
//    CallInUIThreadSynchronous on the dead window's context). That was the
//    "shutdown stalled" after 5 s.
// Nothing in that teardown keeps data: the game writes its files straight to
// the OS (pwrite / WriteFile), the shader cache is flushed every frame, and
// the port's settings are saved before this. The SDK's own window close
// ends the process the same way (std::_Exit after stopping the title).
[[noreturn]] void EndProcess(const char* why) {
  REXLOG_INFO("cod3: {}; exiting", why);
  rex::FlushLogging();
#ifdef _WIN32
  // Hand the mouse back in case the game was holding it, and end with
  // TerminateProcess: std::_Exit is ExitProcess there, which still runs every
  // DLL's detach code after the other threads are gone and can hang on a
  // lock one of them held.
  ClipCursor(nullptr);
  TerminateProcess(GetCurrentProcess(), 0);
#endif
  std::_Exit(0);
}

// A safety net for a path that still runs the SDK's teardown (OnShutdown),
// or a UI thread that never gets to EndProcess: once shutdown has taken this
// long the process ends anyway instead of staying behind without a window
// (the "ghost" in the task manager / system monitor). Settings are saved
// before it starts.
constexpr auto kExitGrace = std::chrono::seconds(5);
std::once_flag g_exit_watchdog;

void StartExitWatchdog(const char* why) {
  std::call_once(g_exit_watchdog, [why] {
    REXLOG_INFO("cod3: {}; the process ends within {} s even if a game thread hangs", why,
                kExitGrace.count());
    std::thread([] {
      std::this_thread::sleep_for(kExitGrace);
      REXLOG_WARN("cod3: shutdown stalled (a game thread didn't stop)");
      EndProcess("ending the process");
    }).detach();
  });
}
}  // namespace

// Portable layout: everything lives in one folder, never in the read-only
// AppImage mount. The executable sits either in that folder, in its bin/
// subfolder (setup tool layout; bin-windows/ for a Windows build made on
// Linux), or is an AppImage file placed in it:
//   <root>/assets        extracted game data      (game_data_root)
//   <root>/data          the setup tool's rips; data/cod3/hd_textures (game/hd_textures.h)
//   <root>/userdata      saves, console config, caches
//   <root>/cod3.toml     recomp settings (F2 window)
void Cod3App::OnConfigurePaths(rex::PathConfig& paths) {
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
  // [cod3mp] Where the Multiplayer item looks for the disc's multiplayer files.
  cod3::mp::SetGameDataRoot(paths.game_data_root.empty() ? root / "assets" : paths.game_data_root);
  if (rex::cvar::GetFlagByName("user_data_root").empty()) {
    paths.user_data_root = root / "userdata";
    paths.cache_root = root / "userdata" / "cache";
  }
  userdata_dir_ = paths.user_data_root.empty() ? root / "userdata" : paths.user_data_root;
  paths.config_path = root / "cod3.toml";
  config_path_ = paths.config_path;

  // Without a log_file the SDK logs into <executable folder>/logs, which is
  // read-only inside an AppImage (creating it throws and the game exits
  // before opening a window). Log into userdata/logs/ instead; a log_file
  // from the command line or cod3.toml still wins. Forward slashes: F2's
  // Save writes this into cod3.toml unescaped, where a Windows "C:\..." path
  // would make the whole file unreadable (see PathToUtf8Generic in util.h).
  if (rex::cvar::GetFlagByName("log_file").empty()) {
    const auto user_dir = paths.user_data_root.empty() ? root / "userdata" : paths.user_data_root;
    std::error_code ec;
    std::filesystem::create_directories(user_dir / "logs", ec);
    rex::cvar::SetFlagByName("log_file", (user_dir / "logs" / "cod3.log").generic_string());
  }
}

void Cod3App::OnPreSetup(rex::RuntimeConfig& config) {
  runtime_config_ = &config;  // its GPU plugin is loaded right after this (render resolution)
  // The game's name in the system's sound settings (PipeWire, PulseAudio:
  // the stream's application name; the SDK's is "rexglue"). SDL reads this
  // hint before the SDK's app metadata; SDL_APP_NAME in the environment still
  // wins. The output device itself is F2's Sound output (audio_device).
  SDL_SetHint(SDL_HINT_APP_NAME, "Call of Duty 3");
  // Before the window opens: the guest display shape the port sets for wide
  // pictures (game/ultrawide.h) would otherwise also size the window.
  cod3::ForgetSavedDisplayShape();
  // The GPU plugin is a library loaded by name from the program's folder
  // (librexgpu-<name>.so, rexgpu-<name>.dll): only a plain name, so a
  // setting (cod3.toml can be shared) never loads one from anywhere else.
  if (!config.gpu_plugin.empty() &&
      (config.gpu_plugin.size() > 32 ||
       !std::all_of(config.gpu_plugin.begin(), config.gpu_plugin.end(), [](char c) {
         return std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-';
       }))) {
    REXLOG_WARN("cod3: gpu_plugin '{}' isn't a plain plugin name; using xenos", config.gpu_plugin);
    config.gpu_plugin = "xenos";
  }
  // The guest command line goes to CoD3's Com_Init, which runs "+set" there
  // as forced writes (past the cheat protection) and copies the whole line
  // into a fixed buffer without a length check. The port passes nothing.
  if (!rex::cvar::GetFlagByName("cl").empty()) {
    REXLOG_WARN("cod3: the guest command line (cl) isn't used; use --cod3_autoexec for console "
                "commands");
    rex::cvar::SetFlagByName("cl", "");
  }
  // The game's own files (assets/) stay read-only, as on the disc (the game
  // writes only to its saves and userdata/).
  if (rex::cvar::GetFlagByName("allow_game_relative_writes") == "true") {
    REXLOG_WARN("cod3: allow_game_relative_writes isn't used; the game's files stay read-only");
    rex::cvar::SetFlagByName("allow_game_relative_writes", "false");
  }
  // Use the Xenos GPU plugin unless the user picked one (or asked for none).
  if (config.gpu_plugin.empty() && !REXCVAR_GET(cod3_no_gpu)) config.gpu_plugin = "xenos";
#ifdef _WIN32
  // Graphics API (F2 > Video): Windows builds have Direct3D 12 and Vulkan.
  // "auto" is Vulkan, as on Linux (the same 4096-tile EDRAM, so the game
  // draws up to 1920x1080 itself there too, and the same F2 rows), and
  // Direct3D 12 when Vulkan can't start. Linux has only Vulkan.
  if (std::string api = REXCVAR_GET(cod3_graphics_api);
      !config.graphics && !config.gpu_plugin.empty() && (api == "auto" || api == "d3d12" || api == "vulkan")) {
    const bool automatic = api == "auto";
    if (automatic) api = "vulkan";
    config.graphics = rex::system::LoadGpuPlugin(config.gpu_plugin, api);
    if (config.graphics)
      REXLOG_INFO("cod3: graphics API {} ({})", api, automatic ? "automatic" : "chosen in the settings");
    else
      REXLOG_WARN("cod3: the {} renderer couldn't start; using the other one", api);
  }
#endif

  auto set_default = SetDefault;

  // windows-fps: Windows sleeps in steps of the system timer's tick, 15.6 ms
  // unless a program asks for 1 ms; the vblank timer, the GPU emulation's
  // waits and the frame limiter all sleep in 1 ms steps. This asks for 1 ms
  // and keeps it while Windows 11 thinks the window is hidden
  // (game/frame_rate.h). Nothing to do on Linux.
  cod3::SetUpHostTiming();
  // Frame rate: com_maxfps is the only limit (game/frame_rate.h). The game
  // waits for a vblank before showing each frame, so at a 60 Hz vblank a
  // frame that takes a little over 16.7 ms waits for the next one and the
  // frame rate drops straight to 30. The vblank interrupts run at 1000 Hz
  // instead, which never holds a frame back by more than a millisecond.
  // The video mode the game sees stays at the 60 Hz it was made for.
  if (auto* info = rex::cvar::GetFlagInfo("video_mode_refresh_rate");
      info && info->source != rex::cvar::Source::kCommandLine &&
      info->source != rex::cvar::Source::kEnvironment)
    rex::cvar::SetFlagByName("video_mode_refresh_rate", "60");  // older builds saved the fps here
  // (guest_vblank_rate and the other GPU settings exist only once the GPU
  // plugin is loaded: ApplyGpuDefaults.)

  // PC-style controls out of the box: keyboard + raw mouse aim with World at
  // War-like binds, and the GPU settings known to suit this title
  // (app_defaults.h; each only while the user hasn't set it).
  for (const auto& d : cod3::kControlDefaults) {
    if (std::string_view(d.cvar) == "mnk_mouse" && !REXCVAR_GET(cod3_raw_mouse)) continue;
    set_default(d.cvar, d.value);
  }
  for (const auto& d : cod3::kKeybindDefaults) set_default(d.cvar, d.value);
}

// The GPU plugin's settings (vblank rate, resolve readback ...) only exist
// once the plugin is loaded, which happens after OnPreSetup. Setting them in
// OnPreSetup silently did nothing, so the vblank stayed at 60 Hz and capped
// the frame rate at 60 whatever com_maxfps said (owner, 2026-10-10).
bool Cod3App::ConstructRuntime(const rex::PathConfig& paths) {
  ApplyGpuDefaults();
  ApplyRenderResolution();
  // The HD texture pack (game/hd_textures.h): the SDK's texture replacement
  // setting is the GPU plugin's, so it exists only now, and the GPU reads it
  // when it starts below.
  cod3::ApplyHdTextures(root_dir_, runtime_config_ && runtime_config_->graphics);
  // The session header and the log detail (game/diagnostics.h).
  cod3::diag::Start(userdata_dir_ / "logs");
  // The hang watchdog and, on Windows, the crash logger (game/watchdog.h).
  cod3::watchdog::Start();
  return rex::ReXApp::ConstructRuntime(paths);
}

// The render resolution (render_resolution.h): natively the game draws the
// window's or screen's size itself (no MSAA, no scaling); only above what the
// emulated EDRAM holds does the GPU's draw scale make up the rest. Planned
// here before the GPU starts; later changes are live switches.
void Cod3App::ApplyRenderResolution() {
  if (render_plan_applied_ || !rex::cvar::GetFlagInfo("resolution_scale")) return;
  render_plan_applied_ = true;

  // The larger emulated EDRAM (SDK patch 0010) needs the edited resolve
  // shaders, which only the Vulkan backend has (c3w/sdk_shaders.py).
  bool vulkan = false;
  if (runtime_config_ && runtime_config_->graphics) {
    const auto& graphics = *runtime_config_->graphics;
    vulkan = std::string_view(typeid(graphics).name()).find("ulkan") != std::string_view::npos;
  }
  cod3::SetEdramTilesAvailable(vulkan ? std::max<uint32_t>(rex::graphics::xenos::kEdramTileCount,
                                                           cod3::kConsoleEdramTiles)
                                      : cod3::kConsoleEdramTiles);
  const std::string choice = rex::cvar::GetFlagByName("cod3_render_resolution");
  // The screen when fullscreen, else the window: the size for "auto" and the
  // shape for an "auto" picture shape (game/ultrawide.h).
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

  // The same plan a live change makes (game/render_resolution.h): native,
  // or the GPU's scale above what the EDRAM holds.
  const cod3::RenderPlan plan = cod3::PlanForView(choice, view_width, view_height);
  cod3::SetRenderPlan(plan);

  const std::string scale = std::to_string(plan.scale);
  rex::cvar::SetFlagByName("resolution_scale", scale);
  rex::cvar::SetFlagByName("draw_resolution_scale_x", scale);
  rex::cvar::SetFlagByName("draw_resolution_scale_y", scale);
  if (choice == "original") {
    // The Xbox 360's own 1040x624 with 2x MSAA at scale 1, stretched to the
    // screen; for comparing and as a fallback.
    REXLOG_INFO("cod3: render resolution original: the game draws its own 1040x624 with its 2x MSAA, "
                "stretched to {}x{} on screen",
                plan.display_width, plan.display_height);
    return;
  }
  REXLOG_INFO("cod3: render resolution {} ({} method) for {} {}x{} (shape {:.3f}:1; EDRAM {} tiles): "
              "the game draws {}",
              plan.choice, cod3::RenderMethodName(plan.method), view_source, view_width, view_height,
              aspect, plan.edram_tiles, cod3::DescribePlan(plan));
  if (!plan.note.empty()) REXLOG_INFO("cod3: render resolution: {}", plan.note);
}

void Cod3App::ApplyGpuDefaults() {
  // The game waits for a vblank before showing each frame; at the video
  // mode's 60 Hz that is a hidden 60 fps cap (and a frame just over 16.7 ms
  // waits for the next one: 30 fps). At 1000 Hz it never holds a frame back
  // by more than a millisecond, and com_maxfps is the only limit.
  if (rex::cvar::GetFlagInfo("guest_vblank_rate"))
    SetDefault("guest_vblank_rate", "1000");
  else
    SetDefault("vsync", "false");  // SDK without patch 0008: same 1000 Hz vblank
  for (const auto& d : cod3::kVideoDefaults) SetDefault(d.cvar, d.value);
  // [color] The picture's colors: matched to the display or BT.709 (game/color_output.h).
  cod3::ApplyColorSpace();

  // One-time fixes of saved settings. Before version 2, F2 called
  // native_2x_msaa "the game's own MSAA", so players unticked it to save GPU
  // time; off actually draws the game's 2x MSAA with 4x, twice the work.
  if (REXCVAR_GET(cod3_settings_version) < 2 && rex::cvar::GetFlagInfo("native_2x_msaa")) {
    REXCVAR_SET(cod3_settings_version, 2);
    if (rex::cvar::GetFlagByName("native_2x_msaa") == "false") {
      rex::cvar::SetFlagByName("native_2x_msaa", "true");
      REXLOG_INFO("cod3: native_2x_msaa back on (the old settings label made it look like the "
                  "game's MSAA switch; off doubled the GPU's MSAA work)");
      try {
        rex::cvar::SaveConfig(config_path_);
      } catch (const std::exception& e) {
        REXLOG_WARN("cod3: couldn't save {}: {}", cod3::PathToUtf8(config_path_), e.what());
      }
    }
  }

  if (!gpu_defaults_logged_ && rex::cvar::GetFlagInfo("readback_resolve")) {
    gpu_defaults_logged_ = true;
    const std::string vblank = rex::cvar::GetFlagByName("guest_vblank_rate");
    REXLOG_INFO("cod3: guest vblank {} (vsync {}), readback_resolve {}, starting com_maxfps {}",
                vblank.empty() ? "1000 Hz via vsync off (SDK without patch 0008)" : vblank + " Hz",
                rex::cvar::GetFlagByName("vsync"),
                rex::cvar::GetFlagByName("readback_resolve"), rex::cvar::GetFlagByName("cod3_max_fps"));
  }
}

void Cod3App::OnPostSetup() {
  ApplyGpuDefaults();   // again, in case the runtime loaded the plugin later
  auto& engine = cod3::Engine::Get();
  engine.SetConfigPath(user_data_root() / "cod3_config.cfg");
  cod3::ui::GameConsole::Get().SetUserDir(user_data_root());

  // While the console is open the game gets no controller / keyboard input.
  if (auto* input = static_cast<rex::input::InputSystem*>(runtime()->input_system())) {
    if (!REXCVAR_GET(cod3_test_input).empty())
      input->AddDriver(std::make_unique<cod3::ScriptedInputDriver>(REXCVAR_GET(cod3_test_input)));
    input->SetActiveCallback([this]() {
      if (cod3::ui::GameConsole::IsOpen()) return false;
      auto* drawer = imgui_drawer();
      if (!drawer || !drawer->HasDialogs()) return true;
      return !drawer->GetIO().WantCaptureMouse;
    });
  }

  // Console quit / exit: save and end, like the window's close button.
  cod3::ui::GameConsole::Get().SetQuitHandler([this]() {
    app_context().CallInUIThreadDeferred([this]() { SaveAndExit("quit from the console"); });
  });

  // A World at War name changed a port setting (sensitivity, r_mode ...):
  // save the settings file like F2's Save does.
  cod3::ui::GameConsole::Get().SetSettingsChangedHandler([this]() {
    try {
      rex::cvar::SaveConfig(config_path_);
    } catch (const std::exception& e) {
      REXLOG_WARN("cod3: couldn't save {}: {}", cod3::PathToUtf8(config_path_), e.what());
    }
  });
  // The pause menu's QUIT TO DESKTOP, confirmed (game thread): save and end
  // like the window's close button.
  cod3::pcmenu::SetQuitToDesktopHandler([this]() {
    app_context().CallInUIThreadDeferred([this]() { SaveAndExit("quit to desktop from the game's menu"); });
  });
  // A video setting changed in the game's own Options menus (game thread):
  // save the settings file on the UI thread, like F2's Save.
  cod3::videomenu::SetSaveHandler([this]() {
    app_context().CallInUIThreadDeferred([this]() {
      try {
        rex::cvar::SaveConfig(config_path_);
      } catch (const std::exception& e) {
        REXLOG_WARN("cod3: couldn't save {}: {}", cod3::PathToUtf8(config_path_), e.what());
      }
    });
  });

  // Master volume (F2 > Audio, snd_volume in the console).
  rex::audio::SetOutputGain(float(REXCVAR_GET(cod3_volume)));
  rex::cvar::RegisterChangeCallback("cod3_volume", [](std::string_view, std::string_view value) {
    rex::audio::SetOutputGain(std::clamp(std::strtof(std::string(value).c_str(), nullptr), 0.0f, 2.0f));
  });

  cod3::ui::GameConsole::Get().Print(
      cod3::ui::ConsoleChannel::kSystem,
      "Call of Duty 3 static recomp. Press ~ for the console, Shift+~ for the full console, "
      "type help for console commands.\n");
}

// The port's own windows take the game's look (ui/theme.h) instead of the
// SDK's green debug style.
void Cod3App::OnConfigureFonts(ImFontAtlas* atlas) { cod3::ui::ConfigureFonts(atlas); }

void Cod3App::OnConfigureStyle(ImGuiStyle& imgui_style, rex::ui::Style& ui_style) {
  cod3::ui::ApplyTheme(imgui_style);
  const auto& p = cod3::ui::CurrentPalette();
  ui_style.settings.warning = p.warn;
  ui_style.console.completion_highlight = ImVec4(p.accent.x, p.accent.y, p.accent.z, 0.45f);
}

void Cod3App::OnCreateDialogs(rex::ui::ImGuiDrawer* drawer) {
  (void)drawer;
  // The SDK log console defaults to the backtick key; give that key to the
  // game console and move the log viewer to F10.
  if (rex::cvar::GetFlagByName("bind_console") == "Backtick")
    rex::cvar::SetFlagByName("bind_console", "F10");

  key_listener_.on_toggle = [this](bool shift) { ToggleConsole(shift); };
  key_listener_.on_close = [this]() { CloseConsole(); };
  key_listener_.on_settings = [this]() { ToggleSettings(); };
  hud_ = std::make_unique<cod3::ui::HudOverlay>(imgui_drawer());
  hud_->on_frame = [this]() {
    UpdateMouseCapture();
    // The OS cursor over the game's menus (game/pc_menu.h).
    if (!mouse_listener_.captured) cod3::pcmenu::UpdateCursor(window());
    // The picture follows the window's shape live (game/ultrawide.h).
    if (auto* win = window()) {
      cod3::UpdateDisplayAspect(win->GetActualPhysicalWidth(), win->GetActualPhysicalHeight());
      // A new instance (mission end, quit to menu, multiplayer) opens the same way (game/relaunch.h).
      cod3::NoteWindowState(win->IsFullscreen(), win->GetActualLogicalWidth(), win->GetActualLogicalHeight());
    }
    // windows-fps: one log line on the frame pacing (game/frame_rate.h).
    cod3::LogFramePacingOnce(window());
  };
  // Above the ImGui drawer (z 64) so the toggle key never reaches a text box.
  window()->AddInputListener(&key_listener_, 200);
  mouse_listener_.window = window();
  window()->AddInputListener(&mouse_listener_, 199);
  // Mouse and keyboard in the game's menus (game/pc_menu.h): below the two
  // above, above ImGui and the keyboard/mouse driver; never while the console
  // or the settings window has the input.
  cod3::pcmenu::Attach(window(), 198,
                       [this]() { return cod3::ui::GameConsole::IsOpen() || settings_ != nullptr; });
  // Keys in the game's button prompts (game/button_prompts.h): first of all
  // listeners, it only watches which device is in use and takes nothing.
  cod3::prompts::Attach(window(), 201);
  // Fullscreen refresh rate (F2 > Video; SDK patch 0013): an exclusive mode
  // at that rate, or borderless at the desktop's own mode when 0.
  window()->SetFullscreenRefreshRate(REXCVAR_GET(cod3_refresh_rate));
}

void Cod3App::ToggleConsole(bool full) {
  if (!imgui_drawer()) return;
  if (!console_) {
    console_ = std::make_unique<cod3::ui::GameConsoleDialog>(imgui_drawer(), full);
  } else if (full && !console_->full()) {
    console_->SetFull(true);
  } else {
    CloseConsole();
  }
}

void Cod3App::CloseConsole() {
  console_.reset();
}

void Cod3App::ToggleSettings() {
  if (!imgui_drawer()) return;
  if (settings_) {
    settings_->SaveIfDirty();
    settings_.reset();
    return;
  }
  settings_ = std::make_unique<cod3::ui::SettingsWindow>(imgui_drawer(), config_path_, [this]() {
    // Closed with its own [x]: destroy it after the current ImGui frame.
    app_context().CallInUIThreadDeferred([this]() { settings_.reset(); });
  });
  settings_->set_window(window());
}

// ---------------------------------------------------------------------------
// Raw mouse aim: hold the pointer (relative mode) only while gameplay is
// building usercmds and no overlay needs the cursor.
// ---------------------------------------------------------------------------
void MouseLookListener::OnMouseMove(rex::ui::MouseEvent& e) {
  if (!captured) return;
  // While aiming, the motion is ours alone: stopping it here keeps a
  // high-polling-rate mouse (1000-8000 events a second) from also running
  // through the overlay and the keyboard/mouse driver for every event. The
  // counts are only added up here and applied once per game frame
  // (MouseLook::ApplyToGuest), so the event rate never changes the game's
  // speed or frame rate, only how finely the turn is measured.
  e.set_handled(true);
  if (relative) {
    cod3::MouseLook::Get().AddDelta(e.dx(), e.dy());
    return;
  }
  // No pointer lock available: measure from the window center and put the
  // cursor back there.
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

void Cod3App::UpdateMouseCapture() {
  auto* win = window();
  if (!win) return;
  // A game menu (the pause menu over gameplay too) gets the pointer back.
  bool want = cod3::MouseLook::Get().WantsCapture() && win->HasFocus() &&
              !cod3::ui::GameConsole::IsOpen() && !settings_ && !cod3::pcmenu::MenuActive();
  auto& m = mouse_listener_;
  if (want == m.captured) return;
  // Motion from before the switch belongs to the menu or the previous session
  // of gameplay, not to the view.
  cod3::MouseLook::Get().ClearPending();
  if (want) {
    m.prev_cursor = win->GetCursorVisibility();
    win->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
    win->CaptureMouse();
    m.relative = win->SetRelativeMouseMode(true);
    if (!m.relative) win->WarpMouseToCenter(m.prev_x, m.prev_y);
    m.captured = true;
  } else {
    if (m.relative) win->SetRelativeMouseMode(false);
    win->ReleaseMouse();
    win->SetCursorVisibility(m.prev_cursor);
    m.captured = false;
    m.relative = false;
  }
}

void Cod3App::SaveAll() {
  if (settings_) settings_->SaveIfDirty();
  cod3::Engine::Get().SaveConfig();
}

// UI thread: saves the settings and ends the process (EndProcess).
void Cod3App::SaveAndExit(const char* why) {
  SaveAll();
  EndProcess(why);
}

// The close button (also Alt+F4 and the desktop session ending). The SDK
// would stop the game's threads and then end the process the same way
// (ReXApp::OnClosing: TerminateTitle, std::_Exit); ending here, before any
// of the window's closing work, leaves nothing that can stall.
bool Cod3App::OnWindowCloseRequested() { SaveAndExit("window closed"); }

// The game's main thread ended by itself: its reboot to the main menu (Quit
// in the pause menu, the end of a mission), the hand-over to multiplayer
// (game/multiplayer.h: cod3mp starts) or the dashboard. The SDK would
// then run its teardown, which hangs (EndProcess), so the process ends here,
// on the UI thread, ahead of the quit the SDK queues after this returns.
void Cod3App::OnGuestThreadExit(rex::system::XThread* thread) {
  (void)thread;
  const bool relaunch = cod3::RelaunchRequested();
  StartExitWatchdog("game exited");  // in case the UI thread never runs the call below
  app_context().CallInUIThread([this, relaunch]() {
    SaveAll();
    // The reboot: a new instance starts with the game's launch data
    // (game/relaunch.h) and takes over.
    if (relaunch && cod3::Relaunch(userdata_dir_)) EndProcess("the new instance takes over");
    EndProcess("the game ended");
  });
}

// Before the game's first instruction: a relaunch hands the game's launch
// data back (game/relaunch.h).
void Cod3App::OnPostLaunchModule(rex::system::XThread* thread) {
  (void)thread;
  cod3::RestoreLaunchData(userdata_dir_);
}

void Cod3App::OnShutdown() {
  StartExitWatchdog("shutting down");
  console_.reset();
  if (settings_) settings_->SaveIfDirty();
  settings_.reset();
  hud_.reset();
  if (window()) {
    if (mouse_listener_.captured) {
      window()->SetRelativeMouseMode(false);
      window()->ReleaseMouse();
    }
    cod3::pcmenu::Detach(window());
    cod3::prompts::Detach(window());
    window()->RemoveInputListener(&mouse_listener_);
    window()->RemoveInputListener(&key_listener_);
  }
  cod3::Engine::Get().SaveConfig();
}
