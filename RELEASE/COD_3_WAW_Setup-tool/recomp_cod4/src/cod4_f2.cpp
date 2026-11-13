// cod4 / cod4mp - Call of Duty 4's F2 settings window (see cod4_f2.h). [f2-menus]
//
// Every row edits a setting the port or the SDK already has; the only new ones are the window's own look, the
// controller shortcut's switch, the frame rate counter (which sets the game's cg_drawFPS) and a test script. The
// SOUND and INTERFACE pages and most of CONTROLS are the shared ones (../recomp/src/f2/f2_common.h).

#include "cod4_f2.h"

#include <algorithm>  // [f2-menus] std::min, the frame rate limit
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>  // [f2-menus] the frame rate limit's wait
#include <utility>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ui/style.h>

#include "cod4_mouse.h"
#include "f2/f2_common.h"
#include "f2/f2_render_size.h"
#include "f2/f2_console.h"
#include "f2/f2_rows.h"
#include "f2/f2_theme.h"
#include "f2/f2_window.h"
// [f2-menus] the crosshair hooks at the end of this file
#ifdef COD4_MP
#include "generated/default_mp/cod4mp_pch.h"
#else
#include "generated/default/cod4_pch.h"
#endif

REXCVAR_DEFINE_STRING(cod4_ui_theme, "cod4", "cod4/Interface",
                      "Colors of the F2 settings window: cod4 (Call of Duty 4's own menus), cod3 (Call of Duty 3's), "
                      "olive, parchment or contrast");
REXCVAR_DEFINE_INT32(cod4_ui_text_size, 16, "cod4/Interface", "Text size of the F2 settings window, in pixels (14-24)")
    .range(14, 24);
REXCVAR_DEFINE_DOUBLE(cod4_ui_opacity, 0.94, "cod4/Interface",
                      "How solid the F2 settings window is (0.6 = see the game through it, 1 = solid)")
    .range(0.6, 1.0);
REXCVAR_DEFINE_BOOL(cod4_f2_pad_shortcut, true, "cod4/Interface",
                    "Back + Start held for a second on a controller opens and closes the F2 settings window");
REXCVAR_DEFINE_BOOL(cod4_show_fps, false, "cod4", "Show the game's own frame rate counter (sets the game's cg_drawFPS)");
REXCVAR_DEFINE_INT32(cod4_max_fps, 0, "cod4",
                     "Frame rate limit: the most frames a second the game runs, by a wait at each frame's start "
                     "(0 = none; the game stops at 60 by itself)")
    .range(0, 250);
REXCVAR_DEFINE_STRING(cod4_f2_test_script, "", "cod4",
                      "Debug: F2 window steps for test runs, \"<seconds>:<step>|...\" from the first drawn frame; a "
                      "step is open, close, page <LABEL>, set <setting> <value> (as a row would, then unsaved), "
                      "save, or console <command> (as typed on the CONSOLE page)");

namespace cod4::f2ui {

namespace {

// Call of Duty 4's own menu colors, counted in the Xbox 360 disc's menus (ui.ff and code_post_gfx.ff, by
// dev/agents/f2-menus/tools/ff_colors.py, 2026-11-09): black panels (0 0 0 at 1, 0.9, 0.69, 0.55, 0.375), white
// text (1 1 1), grey text 0.69, the focused item's bar a pale blue-white (0.9 0.95 1 at 0.25-0.4, 0.8 0.95 1 at
// 0.4), faint lines 0.9 0.9 1 at 0.07, and titles 1 0.8 0.4 (the same as World at War's COLOR_TITLE). World at War's
// amber focus color is not in them. The window's shades are those colors over black; boxes are a little lighter than
// the menus' faint lines, so a box still reads as a box.
constexpr ImVec4 Rgb(int r, int g, int b, float a = 1.0f) {
  return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
}
const f2::ThemeColors kCod4Colors = {
    Rgb(8, 9, 10),          // window: the black panels
    Rgb(14, 15, 17, 0.98f),
    Rgb(28, 29, 32),        // frame: 0.9 0.95 1 at about 0.12 over black
    Rgb(57, 61, 64),        //   hovered: at 0.25, as the menus' focus bar
    Rgb(80, 85, 89),        //   pressed: at 0.35
    Rgb(40, 42, 45),        // button
    Rgb(57, 61, 64),
    Rgb(82, 97, 102),       //   pressed: 0.8 0.95 1 at 0.4
    Rgb(0, 0, 0),           // title: black
    Rgb(20, 21, 23),
    Rgb(57, 57, 64, 0.8f),  // border: 0.9 0.9 1 at 0.25
    Rgb(255, 255, 255),     // text: white
    Rgb(176, 176, 176),     // descriptions: 0.69 grey
    Rgb(204, 242, 255),     // accent: 0.8 0.95 1, the focus bar's pale blue (lists use it at 0.3-0.6)
    Rgb(255, 204, 102),     // headings and check marks: the titles' 1 0.8 0.4
    Rgb(230, 230, 255, 0.07f),  // every other row: the faint lines' 0.9 0.9 1 at 0.07
    Rgb(132, 200, 110),     // good
    Rgb(255, 204, 102),     // warn
    Rgb(230, 72, 56),       // bad
};

std::unique_ptr<f2::Overlay> g_overlay;
f2::ConsolePage g_console_page;
f2::CommandQueue g_commands;

using f2::Applies;
using f2::Help;
using f2::PageContext;
using f2::Section;

// ------------------------------------------------------------------------------------------------- VIDEO
void DrawVideo(PageContext& c) {
  if (f2::ResetButton("reset_video", "video settings")) {
    c.restart |= f2::ResetSettings({"fullscreen", "vsync", "cod4_show_fps", "cod4_max_fps", "cod4_fov",
                                    "cod4_graphics_api"});
    for (const char* name : f2::kPictureSettings)
      if (f2::Has(name)) c.restart |= f2::ResetSettings({name});
    for (const char* name : f2::kResolutionSettings) c.restart |= f2::ResetSettings({name});
    for (const char* name : f2::kGameDrawSizeSettings) c.restart |= f2::ResetSettings({name});
    c.dirty = true;
  }
  Section("Display");
  {
    const char* v[] = {"false", "true"};
    const char* n[] = {"Windowed", "Fullscreen (borderless)"};
    c.dirty |= f2::Choice("Display mode", "fullscreen", v, n, 2, Applies::kAtOnce, &c.restart);
    Help("Play in a window, or fill the whole monitor without changing its resolution (borderless).");
  }
  f2::DrawDisplayRows(c);  // [f2-menus] 2026-11-11 02:40 Call of Duty 3's Display and Refresh rate rows
  c.dirty |= f2::Check("V-Sync", "vsync", Applies::kAfterRestart, &c.restart);
  Help("Waits for the monitor between pictures: no tearing, but no more frames than the monitor's refresh rate. "
       "Applies after a restart.");
#ifdef _WIN32
  {
    const char* v[] = {"auto", "vulkan", "d3d12"};
    const char* n[] = {"Automatic (Vulkan, else Direct3D 12)", "Vulkan", "Direct3D 12"};
    c.dirty |= f2::Choice("Graphics API", "cod4_graphics_api", v, n, 3, Applies::kAfterRestart, &c.restart);
    Help("Which graphics API draws the game. Applies after a restart.");
  }
#endif
  // [f2-menus] 2026-11-11 05:47 the slider ends at 60: this port's game stops at 60 frames a second by itself (run
  // fpsiw0428: a limit of 120 gave 60), so a higher limit did nothing.
  c.dirty |= f2::SliderInt("Frame rate limit", "cod4_max_fps", 0, 60, "%d fps", Applies::kAtOnce, &c.restart);
  Help("The most frames a second the game draws, by a wait at the start of each frame; 0 is no limit of this "
       "window's. The game itself never goes above 60 frames a second in this port. The mouse turns the same at "
       "any frame rate. With V-Sync on, the monitor's refresh rate is the limit when it is lower.");
  c.dirty |= f2::Check("Show the frame rate", "cod4_show_fps", Applies::kAtOnce, &c.restart);
  Help("The game's own frame rate counter in the corner (its cg_drawFPS).");

  f2::DrawResolutionSection(c);  // render scale, picture shape, window size (the same in every game)
  f2::DrawGameDrawSizeRows(c);   // the 3D scene at the console's size or the screen's (IW3 games)

  Section("View");
  if (f2::Has("cod4_fov")) {
    bool own = f2::GetDouble("cod4_fov") > 0.0;
    if (ImGui::Checkbox("Use my own field of view", &own)) {
      rex::cvar::SetFlagByName("cod4_fov", own ? "80" : "0");
      c.dirty = true;
    }
    if (own)
      c.dirty |= f2::SliderDouble("Field of view", "cod4_fov", 65.0, 160.0, "%.0f", Applies::kAtOnce, &c.restart);
    Help("Off: the game's own (65 on the console). On: your field of view in degrees, beyond the console's 65-80, "
         "kept every frame so the game's menus and levels can't put it back. Aiming down the sights still zooms "
         "from it.");
  }

  Section("Picture");
  f2::DrawPictureSection(c);  // post-process AA, output scaling and its sharpness, anisotropic filtering
}

// The game's own options this game has (indices into f2::kGameOptions; no cg_drawFriendlyNames or
// cg_drawCrosshairNames in the campaign: run dv1). 9 and 10 (2026-11-10): the weapon in view and the HUD.
// [f2-menus] 2026-11-10 20:30: 11 to 21 (controller, hints and small effects, the game's picture switches): all in
// the campaign's dvar list and taken by its console with cheats off (run t2022).
// 22 (21:00): smooth mouse in the game's own mouse look.
#ifdef COD4_MP
// [f2-menus] 2026-11-10 21:40: not 10. The multiplayer program has no cg_drawHUD (the name is not among its strings;
// dev/agents/f2-menus/dvars/COD4_MP_DVARS.md), so that row did nothing there.
// [f2-menus] 2026-11-10 23:45: 7 (names over friendly soldiers): cg_drawFriendlyNames is in multiplayer's list and
// its console takes it in a local match (runs c4mp-map, c4mp-devmap), where every row here was set and given back.
// [f2-menus] 2026-11-11 01:20: 8 (the name under the crosshair, cg_drawCrosshairNames): in multiplayer's list too
// (cheat flagged, which a local match doesn't mind: run c4mp3-map).
const int kGameOptionRows[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22};
#else
const int kGameOptionRows[] = {0, 1, 2, 3, 4, 5, 6, 9, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19, 20, 21, 22};
#endif
constexpr int kGameOptionRowCount = int(sizeof(kGameOptionRows) / sizeof(kGameOptionRows[0]));

void DrawGameOptions(PageContext& c) {
  Section("The game's own options");
  f2::DrawGameOptionRows(c, kGameOptionRows, kGameOptionRowCount, 0, 10);
  f2::DrawGameNumberRows(c);  // the weapon's place on screen, the game crosshair's opacity
  // [f2-menus] 2026-11-10 20:30 more of the game's own switches, in groups (dev/agents/f2-menus/FEATURES.md)
  Section("Controller and mouse (the game's own)");
  f2::DrawGameOptionRows(c, kGameOptionRows, kGameOptionRowCount, 11, 12);
  f2::DrawGameInputRows(c, true);  // [f2-menus] 21:00 the stick's look speed, the game's own mouse sensitivity
  f2::DrawGameOptionRows(c, kGameOptionRows, kGameOptionRowCount, 22, 22);  // smooth mouse
  Section("Hints and small effects");
  f2::DrawGameOptionRows(c, kGameOptionRows, kGameOptionRowCount, 13, 16);
  Section("The game's own picture switches");
  f2::DrawGameOptionRows(c, kGameOptionRows, kGameOptionRowCount, 17, 21);
  // [f2-menus] 22:30 gamma and brightness (run g2214: both change the picture). 23:45: both in multiplayer too:
  // r_brightness carries the cheat flag there, but a local match takes it with cheats off (run c4mp2-map).
  f2::DrawGamePictureRows(c, true);
}

// The shared Player rows' settings back to their defaults, for GAME's Reset.
bool ResetPlayer() {
  bool restart = false;
  for (const char* name : f2::kPlayerSettings)
    if (f2::Has(name)) restart |= f2::ResetSettings({name});
  for (int row : kGameOptionRows)
    if (f2::Has(f2::kGameOptions[row].setting)) restart |= f2::ResetSettings({f2::kGameOptions[row].setting});
  for (const char* name : f2::kGameNumberSettings)
    if (f2::Has(name)) restart |= f2::ResetSettings({name});
  for (const char* name : f2::kGameInputSettings)  // [f2-menus] 21:00
    if (f2::Has(name)) restart |= f2::ResetSettings({name});
  for (const char* name : f2::kGamePictureSettings)  // [f2-menus] 22:30
    if (f2::Has(name)) restart |= f2::ResetSettings({name});
  return restart;
}

#ifdef COD4_MP
// ------------------------------------------------------------------------------------------------- GAME (multiplayer)
void DrawGame(PageContext& c) {
  if (f2::ResetButton("reset_game", "game settings")) {
    c.restart |= f2::ResetSettings({"cod4_move_snap_hz"});
    if (f2::Has("cod4_script_folder")) c.restart |= f2::ResetSettings({"cod4_script_folder"});  // [f2-menus] 13:35
    c.restart |= ResetPlayer();
    c.dirty = true;
  }
  f2::DrawPlayerSection(c);  // the player's name, split-screen names, which player each controller is
  Section("Movement");
  {
    const char* v[] = {"60", "0"};
    const char* n[] = {"The Xbox 360's (the same at any frame rate)", "The game's own (lower at high frame rates)"};
    c.dirty |= f2::Choice("Jump height", "cod4_move_snap_hz", v, n, 2, Applies::kAtOnce, &c.restart);
    Help("Multiplayer rounds the player's speed to whole units after every movement step, so the more frames a "
         "second, the lower a jump. The Xbox 360's keeps the rounding to its 60 Hz display, so a jump is as high as "
         "on the console at any frame rate. (The campaign doesn't round, so it needs nothing.)");
  }
  // [f2-menus] 2026-11-11 13:35 waw-decomp's cod4_script_folder (their 13:17; Promod's first step).
  if (f2::Has("cod4_script_folder")) {  // [f2-menus]
    Section("Game scripts");  // [f2-menus]
    c.dirty |= f2::TextRow("Script folder", "cod4_script_folder", &c.restart);  // [f2-menus]
    Help("A folder of game scripts (.gsc) read in place of the game's own where it has them, at the same paths "  // [f2-menus]
         "(for example <folder>/maps/mp/gametypes/_globallogic.gsc), for script mods such as Promod. Empty (the "  // [f2-menus]
         "default) = the game's own scripts only. Scripts are read when a map loads.");  // [f2-menus]
  }  // [f2-menus]
  DrawGameOptions(c);
}
#else
// ------------------------------------------------------------------------------------------------- GAME (campaign)
void DrawGame(PageContext& c) {
  if (f2::ResetButton("reset_game", "game settings")) {
    c.restart |= f2::ResetSettings({"cod4_skip_intro_movies", "cod4_auto_save_device"});
    c.restart |= ResetPlayer();
    c.dirty = true;
  }
  f2::DrawPlayerSection(c);  // the player's name, split-screen names, which player each controller is
  Section("Start");
  c.dirty |= f2::Check("Skip the start-up films", "cod4_skip_intro_movies", Applies::kAfterRestart, &c.restart);
  Help("The legal screen, Infinity Ward's logo and the intro are left out, so the main menu comes at once. Applies "
       "at the next start.");
  Section("Saves");
  c.dirty |= f2::Check("Choose the save device by itself", "cod4_auto_save_device", Applies::kAfterRestart,
                       &c.restart);
  Help("The game's own select_save_device a few seconds after start, so a level never opens the \"Game save Error\" "
       "box because no device was chosen. Applies at the next start.");
  DrawGameOptions(c);
}
#endif

// ------------------------------------------------------------------------------------------------- CONTROLS
void DrawControls(PageContext& c) {
  f2::DrawControlsPage(
      c, "cod4_f2_pad_shortcut",
      [](PageContext& g) {
        g.dirty |= f2::Check("Aim with the game's own mouse look", "cod4_mouse_look", Applies::kAtOnce, &g.restart);
        Help("The mouse turns the view through Call of Duty 4's own PC mouse code: the game's sensitivity in its "
             "Options, as on the PC, and the pointer is held while you play. Off: the mouse moves the right stick, "
             "at the mouse sensitivity above.");
      },
      {"cod4_mouse_look"});
}

// ------------------------------------------------------------------------------------------------- EMULATION
// [f2-menus] 2026-11-11 02:10 PERFORMANCE, as Call of Duty 3's and World at War's windows have: the shared rows.
void DrawPerformance(PageContext& c) { f2::DrawPerformanceRows(c); }
void DrawEmulation(PageContext& c) { f2::DrawEmulationPage(c, nullptr, {}); }

// ------------------------------------------------------------------------------------------------- CONSOLE
void DrawConsole(PageContext& c) {
  (void)c;
  f2::DrawConsolePage(g_console_page, [](const std::string& command) {
    REXLOG_INFO("f2: console command: {}", command);
    g_commands.Push(command);
  });
}

// ------------------------------------------------------------------------------------------------- once a UI frame
// Settings that follow a change made anywhere (F2, the command line, the setup tool), and the test script.
void OnUiFrame(f2::Overlay& overlay) {
  // The frame rate counter: the game's cg_drawFPS follows cod4_show_fps ("set", so it works before the game has
  // registered it). Only a change is sent, so a test's own cg_drawFPS stays.
  static bool shown_fps = false;
  if (REXCVAR_GET(cod4_show_fps) != shown_fps) {
    shown_fps = REXCVAR_GET(cod4_show_fps);
    g_commands.Push(shown_fps ? "set cg_drawFPS 1" : "set cg_drawFPS 0");
  }
  // [f2-menus] 2026-11-11 04:22 the frame rate limit is LimitFrameRate now (the game's frame hook): the com_maxfps this
  // sent did nothing in this program.
  f2::crosshair::Draw();  // the custom crosshair, window open or not
  // [f2-menus] 2026-11-11 00:50 a row given back at a menu is reset once more when a level starts (f2_common.h):
  // the campaign registers 13 of the rows' dvars only then (run menu0011).
  f2::SetLevelRuns(cod4::mouse::InGameplay(), [](std::string command) { g_commands.Push(std::move(command)); });
  f2::SendGameOptions(kGameOptionRows, kGameOptionRowCount, [](std::string command) { g_commands.Push(std::move(command)); });
  f2::SendGameNumbers([](std::string command) { g_commands.Push(std::move(command)); });
  static f2::TestScript script(REXCVAR_GET(cod4_f2_test_script));
  script.Run(overlay, g_commands);
}

}  // namespace

void ConfigureFonts(ImFontAtlas* atlas) {
  f2::LookSettings look;
  look.theme_name = "cod4_ui_theme";
  look.theme = &FLAGS_cod4_ui_theme_storage_;
  look.text_size_name = "cod4_ui_text_size";
  look.text_size = &FLAGS_cod4_ui_text_size_storage_;
  look.opacity_name = "cod4_ui_opacity";
  look.opacity = &FLAGS_cod4_ui_opacity_storage_;
  look.own = {"cod4", "Call of Duty 4", "Call of Duty 4's own menu colors: black panels, white text, pale blue "
                                        "highlights, orange titles.",
              &kCod4Colors};
  f2::SetLook(look);
  f2::ConfigureFonts(atlas);
  REXLOG_INFO("f2: look {} (text {} px, opacity {:.2f})", f2::CurrentTheme().id, REXCVAR_GET(cod4_ui_text_size),
              REXCVAR_GET(cod4_ui_opacity));
}

void ConfigureStyle(ImGuiStyle& imgui_style, rex::ui::Style& ui_style) {
  f2::ApplyTheme(imgui_style);
  const f2::ThemeColors& c = f2::CurrentColors();
  ui_style.settings.warning = c.warn;
  ui_style.console.completion_highlight = ImVec4(c.accent.x, c.accent.y, c.accent.z, 0.45f);
}

void Attach(rex::ui::ImGuiDrawer* drawer, rex::ui::Window* window, std::filesystem::path config_path,
            std::function<void(std::function<void()>)> defer) {
  f2::Overlay::Options options;
  options.spec.title = "CALL OF DUTY 4: MODERN WARFARE";
#ifdef COD4_MP
  options.spec.id = "cod4mp_settings";
#else
  options.spec.id = "cod4_settings";
#endif
  options.spec.pages = {
      {"VIDEO", DrawVideo},           {"SOUND", f2::DrawSoundPage},         {"GAME", DrawGame},
      {"CROSSHAIR", f2::crosshair::DrawPage},
      {"CONTROLS", DrawControls},     {"INTERFACE", f2::DrawInterfacePage}, {"PERFORMANCE", DrawPerformance},
      {"EMULATION", DrawEmulation},   {"CONSOLE", DrawConsole},
  };
  options.spec.config_path = std::move(config_path);
  options.pad_shortcut_setting = "cod4_f2_pad_shortcut";
  options.defer = std::move(defer);
  options.on_frame = OnUiFrame;
  g_overlay = std::make_unique<f2::Overlay>(drawer, window, std::move(options));
  // [f2-menus] 2026-11-11 03:20 mouse acceleration, the sensitivity randomizer and the sensitivity scales (owner
  // 02:14, 03:02, 03:09): the mouse file applies them to the game's own mouse look; the scale sets the game's
  // sensitivity dvar through the "Mouse sensitivity (game)" row's settings (a count turns 0.022 degrees x it).
  f2::accel::SetSupported(true);
  f2::accel::SetGameSensitivity("f2_mouse_sens", "f2_mouse_sens_own", 0.022, 0.01, 100.0);
  REXLOG_INFO("f2: ready (F2, or Back + Start held 1 s on a controller)");
}

void Detach() { g_overlay.reset(); }

bool GameGetsInput() { return !g_overlay || g_overlay->GameGetsInput(); }

void NoteConsoleLine(int channel, std::string_view line) { f2::ConsoleLog::Get().Add(channel, line); }

// [f2-menus] 2026-11-11 04:22 cod4_max_fps: wait here, at the frame's start, until this frame's time on the host's clock.
// Most of the wait sleeps; the last millisecond yields, so the limit holds to well under a millisecond. A frame that
// ran late moves the schedule on instead of running the next ones fast to catch up. (World at War's waw_frame_start.)
void LimitFrameRate() {
  using clock = std::chrono::steady_clock;
  const int32_t max_fps = REXCVAR_GET(cod4_max_fps);
  if (max_fps <= 0) return;
  static clock::time_point next_frame = clock::now();
  const auto period = std::chrono::nanoseconds(1000000000LL / std::min(max_fps, 1000));
  auto waiting = clock::now();
  if (next_frame + period < waiting) next_frame = waiting;
  while (waiting < next_frame) {
    const auto left = next_frame - waiting;
    if (left > std::chrono::microseconds(1500)) {
      std::this_thread::sleep_for(left - std::chrono::milliseconds(1));
    } else {
      std::this_thread::yield();
    }
    waiting = clock::now();
  }
  next_frame += period;
}

bool HasCommands() { return g_commands.Has(); }

void TakeCommands(std::vector<std::string>& out) { g_commands.Take(out); }

}  // namespace cod4::f2ui

// ------------------------------------------------------------------------------------------------- crosshair
// [f2-menus] 2026-11-10 The custom crosshair (f2/f2_common.h, f2::crosshair). The game's CG_DrawCrosshair (campaign
// 0x822FBFF8, multiplayer 0x82312308; found in dev/x360-family/cod4_360 as the only reader of cg_crosshairDynamic)
// draws the hip-fire crosshair, after every check of its own, with two calls: the center piece (campaign 0x822FB320,
// multiplayer 0x82311610) and the sides (0x822FBD88 / 0x82312058), both (x f1, y f2, scale f3, local client, weapon,
// color): the local client in r3, the weapon in r4, the color in r5 (World at War, run xh2). With the custom
// crosshair on, the center piece is skipped and the sides' function still runs, so the game
// works out its spread; its four pieces go through ScrPlace_ApplyRect and CG_DrawRotatedPicPhysical, whose hooks
// measure them and leave them undrawn (f2::crosshair's capture). The overlay gets the game's color and spread. The color is looked for in r5 first, then r8, r6, r7: the first that points at four
// numbers between 0 and 1 that aren't all 0. f2_debug_crosshair logs the registers once a second.
namespace {
// `game` runs the game's own function: with the capture on it works out its spread and asks for its four side
// pieces, which the two hooks below it measure and leave undrawn.
template <typename Ctx, typename Game>
void CrosshairSides(Ctx& ctx, uint8_t* base, Game game) {
  float rgba[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  const uint32_t r[6] = {ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r8.u32};
  const uint32_t candidates[] = {r[2], r[5], r[3], r[4]};  // r5 (seen 2026-11-10 in World at War), r8, r6, r7
  int found = -1;
  for (int i = 0; i < 4 && found < 0; ++i)
    if (f2::crosshair::ReadGuestColor(base, candidates[i], rgba)) found = i;
  f2::crosshair::CaptureBegin();
  game();
  float spread = 0.0f;
  const int rects = f2::crosshair::CaptureEnd(&spread);
  if (rects > 0) f2::crosshair::Notify(0, rgba, spread);  // 0: the game chose not to draw (faded out)
  static std::atomic<int64_t> logged{0};
  const int64_t now = std::chrono::duration_cast<std::chrono::seconds>(
                          std::chrono::steady_clock::now().time_since_epoch())
                          .count();
  if (f2::GetBool("f2_debug_crosshair") && logged.exchange(now) != now)
    REXLOG_INFO("f2: crosshair hook: r3 {:08X} r4 {:08X} r5 {:08X} r6 {:08X} r7 {:08X} r8 {:08X}, color from "
                "candidate {} = {:.2f} {:.2f} {:.2f} {:.2f}, {} rectangles, spread {:.2f}",
                r[0], r[1], r[2], r[3], r[4], r[5], found, rgba[0], rgba[1], rgba[2], rgba[3], rects, spread);
}
}  // namespace
#ifdef COD4_MP
REX_EXTERN(__imp__sub_82311610);
REX_HOOK_RAW(sub_82311610) {
  if (f2::crosshair::Enabled()) return;  // the center piece: the custom crosshair has its own
  __imp__sub_82311610(ctx, base);
}
REX_EXTERN(__imp__sub_82312058);
REX_HOOK_RAW(sub_82312058) {
  if (f2::crosshair::Enabled()) {
    CrosshairSides(ctx, base, [&] { __imp__sub_82312058(ctx, base); });
    return;
  }
  __imp__sub_82312058(ctx, base);
}
REX_EXTERN(__imp__sub_822B1D58);
REX_HOOK_RAW(sub_822B1D58) {  // ScrPlace_ApplyRect(place r3, &x r4, &y r5, &width r6, &height r7, ...)
  if (f2::crosshair::Capturing()) f2::crosshair::CaptureRect(base, ctx.r4.u32, ctx.r6.u32);
  __imp__sub_822B1D58(ctx, base);
}
REX_EXTERN(__imp__sub_82310460);
REX_HOOK_RAW(sub_82310460) {  // CG_DrawRotatedPicPhysical: the side piece itself
  if (f2::crosshair::Capturing()) return;
  __imp__sub_82310460(ctx, base);
}
#else
REX_EXTERN(__imp__sub_822FB320);
REX_HOOK_RAW(sub_822FB320) {
  if (f2::crosshair::Enabled()) return;  // the center piece: the custom crosshair has its own
  __imp__sub_822FB320(ctx, base);
}
REX_EXTERN(__imp__sub_822FBD88);
REX_HOOK_RAW(sub_822FBD88) {
  if (f2::crosshair::Enabled()) {
    CrosshairSides(ctx, base, [&] { __imp__sub_822FBD88(ctx, base); });
    return;
  }
  __imp__sub_822FBD88(ctx, base);
}
REX_EXTERN(__imp__sub_822C2B90);
REX_HOOK_RAW(sub_822C2B90) {  // ScrPlace_ApplyRect(place r3, &x r4, &y r5, &width r6, &height r7, ...)
  if (f2::crosshair::Capturing()) f2::crosshair::CaptureRect(base, ctx.r4.u32, ctx.r6.u32);
  __imp__sub_822C2B90(ctx, base);
}
REX_EXTERN(__imp__sub_822FA318);
REX_HOOK_RAW(sub_822FA318) {  // CG_DrawRotatedPicPhysical: the side piece itself
  if (f2::crosshair::Capturing()) return;
  __imp__sub_822FA318(ctx, base);
}
#endif
