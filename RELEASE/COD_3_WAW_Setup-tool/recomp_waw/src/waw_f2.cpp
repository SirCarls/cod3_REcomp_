// waw - World at War's F2 settings window (see waw_f2.h). [f2-menus]
//
// Every row edits a setting the port or the SDK already has; the only new ones are the window's own look, the
// controller shortcut's switch, the frame rate counter (which sets the game's cg_drawFPS) and a test script.

#include "waw_f2.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include <imgui.h>

#include <rex/audio/output_device.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/ui/style.h>

#include "waw_mouse.h"
#include "f2/f2_common.h"
#include "f2/f2_render_size.h"
#include "f2/f2_console.h"
#include "f2/f2_rows.h"
#include "f2/f2_theme.h"
#include "f2/f2_window.h"
#include "generated/default/waw_pch.h"  // [f2-menus] the crosshair hooks below

REXCVAR_DEFINE_STRING(waw_ui_theme, "waw", "waw/Interface",
                      "Colors of the F2 settings window: waw (World at War's own menus), cod3 (Call of Duty 3's), "
                      "olive, parchment or contrast");
REXCVAR_DEFINE_INT32(waw_ui_text_size, 16, "waw/Interface", "Text size of the F2 settings window, in pixels (14-24)")
    .range(14, 24);
REXCVAR_DEFINE_DOUBLE(waw_ui_opacity, 0.94, "waw/Interface",
                      "How solid the F2 settings window is (0.6 = see the game through it, 1 = solid)")
    .range(0.6, 1.0);
REXCVAR_DEFINE_BOOL(waw_f2_pad_shortcut, true, "waw/Interface",
                    "Back + Start held for a second on a controller opens and closes the F2 settings window");
REXCVAR_DEFINE_BOOL(waw_show_fps, false, "waw",
                    "Show the game's own frame rate counter (sets the game's cg_drawFPS)");
REXCVAR_DEFINE_STRING(waw_f2_test_script, "", "waw",
                      "Debug: F2 window steps for test runs, \"<seconds>:<step>|...\" from the first drawn frame; a "
                      "step is open, close, page <LABEL>, set <setting> <value> (as a row would, then unsaved), "
                      "save, or console <command> (as typed on the CONSOLE page)");

REXCVAR_DECLARE(int32_t, waw_max_fps);

namespace waw::f2ui {

namespace {

// World at War's own menu colors, from its menu files (the mod tools' raw/ui/*.menu and *.inc, read 2026-11-09):
// COLOR_TIER_FRAME 0.118 0.122 0.137 (the panels), COLOR_TITLEBAR 0.145 0.161 0.169, BORDER_COLOR_TIER_FRAME
// 0.35 0.35 0.37, COLOR_TEXT 0.843 (text), COLOR_BODY_TEXT 0.627 0.667 0.675 (descriptions), COLOR_FOCUSED
// 0.953 0.722 0.212 (the focused item's amber), COLOR_TITLE 1 0.8 0.4 (titles), COLOR_RED_TEXT 0.69 0.008 0.008.
// The in-between shades (hover, pressed) are steps between these.
constexpr ImVec4 Rgb(int r, int g, int b, float a = 1.0f) {
  return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
}
const f2::ThemeColors kWawColors = {
    Rgb(30, 31, 35),     // window: COLOR_TIER_FRAME
    Rgb(34, 36, 40, 0.98f),
    Rgb(46, 48, 54),     // frame
    Rgb(62, 65, 72),
    Rgb(80, 84, 92),
    Rgb(54, 57, 63),     // button
    Rgb(76, 80, 88),
    Rgb(150, 112, 34),   // pressed: COLOR_FOCUSED, darker
    Rgb(37, 41, 43),     // title: COLOR_TITLEBAR
    Rgb(52, 57, 60),
    Rgb(89, 89, 94, 0.8f),  // border: BORDER_COLOR_TIER_FRAME
    Rgb(215, 215, 215),  // text: COLOR_TEXT
    Rgb(160, 170, 172),  // descriptions: COLOR_BODY_TEXT
    Rgb(243, 184, 54),   // accent: COLOR_FOCUSED
    Rgb(255, 204, 102),  // headings: COLOR_TITLE
    Rgb(230, 242, 255, 0.03f),
    Rgb(132, 186, 96),   // good
    Rgb(255, 204, 102),  // warn
    Rgb(214, 48, 40),    // bad: COLOR_RED_TEXT, lightened to read on the dark panels
};

std::unique_ptr<f2::Overlay> g_overlay;
f2::ConsolePage g_console_page;

// Commands for the game's console, from the UI thread to the game's frame hook.
std::mutex g_commands_mutex;
std::vector<std::string> g_commands;
std::atomic<bool> g_has_commands{false};

void QueueCommand(std::string command) {
  std::lock_guard<std::mutex> lock(g_commands_mutex);
  g_commands.push_back(std::move(command));
  g_has_commands.store(true, std::memory_order_release);
}

using f2::Applies;
using f2::Help;
using f2::PageContext;
using f2::Section;

// ------------------------------------------------------------------------------------------------- VIDEO
void DrawVideo(PageContext& c) {
  if (f2::ResetButton("reset_video", "video settings")) {
    c.restart |= f2::ResetSettings({"fullscreen", "vsync", "waw_max_fps", "waw_show_fps", "waw_fov",
                                    "waw_sun_shadows", "waw_graphics_api"});
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
       "Turn it off for a frame rate limit above the monitor's. Applies after a restart.");
#ifdef _WIN32
  {
    const char* v[] = {"auto", "vulkan", "d3d12"};
    const char* n[] = {"Automatic (Vulkan, else Direct3D 12)", "Vulkan", "Direct3D 12"};
    c.dirty |= f2::Choice("Graphics API", "waw_graphics_api", v, n, 3, Applies::kAfterRestart, &c.restart);
    Help("Which graphics API draws the game. Applies after a restart.");
  }
#endif

  f2::DrawResolutionSection(c);  // render scale, picture shape, window size (the same in every game)
  f2::DrawGameDrawSizeRows(c);   // the 3D scene at the console's size or the screen's (IW3 games)

  Section("Frame rate");
  if (f2::SliderInt("Frame rate limit", "waw_max_fps", 0, 1000, "%d fps", Applies::kAtOnce, &c.restart)) {
    c.dirty = true;
    // The game's own limit (com_maxfps) stops at 255; past it the port's limit is the only one, so the game's is
    // turned off once, when the slider crosses 255 (not on every frame of a drag).
    static bool above_game_limit = false;
    const bool above = REXCVAR_GET(waw_max_fps) > 255;
    if (above && !above_game_limit) QueueCommand("set com_maxfps 0");
    above_game_limit = above;
  }
  Help("The most frames a second the game draws, paced on the PC's own clock; the game's speed doesn't change with "
       "it. 0 is the game's own limit. Above your monitor's refresh rate, turn V-Sync off as well.");
  c.dirty |= f2::Check("Show the frame rate", "waw_show_fps", Applies::kAtOnce, &c.restart);
  Help("The game's own frame rate counter in the corner (its cg_drawFPS).");

  Section("View");
  if (f2::Has("waw_fov")) {
    bool own = f2::GetDouble("waw_fov") > 0.0;
    if (ImGui::Checkbox("Use my own field of view", &own)) {
      rex::cvar::SetFlagByName("waw_fov", own ? "80" : "0");
      c.dirty = true;
    }
    if (own) c.dirty |= f2::SliderDouble("Field of view", "waw_fov", 65.0, 160.0, "%.0f", Applies::kAtOnce, &c.restart);
    Help("Off: the game's own (65, set by each level). On: your field of view in degrees, kept every frame even when "
         "a level sets its own.");
  }

  Section("Picture");
  c.dirty |= f2::Check("Sun shadows", "waw_sun_shadows", Applies::kAtOnce, &c.restart);
  Help("The sun's shadows (the game's sm_sunEnable). Off by default for now: through the GPU emulation they come "
       "out as blue or purple patches and a grid of dots on soldiers, leaves and walls.");
  f2::DrawPictureSection(c);  // post-process AA, output scaling and its sharpness, anisotropic filtering
}

// ------------------------------------------------------------------------------------------------- SOUND
void DrawSound(PageContext& c) {
  if (f2::ResetButton("reset_sound", "sound settings")) {
    c.restart |= f2::ResetSettings({"audio_device", "audio_mute", "audio_output_layout"});
    c.dirty = true;
  }
  Section("Output");
  if (f2::Has("audio_device")) {
    static std::vector<std::string> outputs;  // refreshed when the list opens
    const std::string chosen = f2::Get("audio_device");
    const std::string preview = chosen.empty() ? std::string("System default") : chosen;
    std::string pick;
    bool picked = false;
    if (ImGui::BeginCombo("Sound output", preview.c_str())) {
      if (ImGui::IsWindowAppearing()) outputs = rex::audio::ListOutputDevices();
      if (ImGui::Selectable("System default", chosen.empty())) picked = true;
      bool chosen_listed = chosen.empty();
      for (const std::string& name : outputs) {
        chosen_listed |= name == chosen;
        if (ImGui::Selectable(name.c_str(), name == chosen)) {
          pick = name;
          picked = true;
        }
      }
      if (!chosen_listed) {
        const std::string missing = chosen + " (not connected)";
        ImGui::Selectable(missing.c_str(), true);
      }
      ImGui::EndCombo();
    }
    if (picked && pick != chosen) {
      rex::cvar::SetFlagByName("audio_device", pick);
      c.save_now = true;  // at once, as Call of Duty 3's
    }
    const std::string playing = rex::audio::CurrentOutputDevice();
    if (!playing.empty()) ImGui::TextDisabled("Playing on: %s", playing.c_str());
    Help("Where the game's sound plays, saved at once.");
  }
  f2::DrawSpeakersRow(c);  // what the 5.1 sound is mixed for, as Call of Duty 3's
  c.dirty |= f2::Check("Mute all sound", "audio_mute", Applies::kAtOnce, &c.restart);
  Help("The game's own volume sliders are in its Options > Volume.");
}

// ------------------------------------------------------------------------------------------------- GAME
// The game's own options this game has (indices into f2::kGameOptions; all nine answered on its console, run dv1).
// [f2-menus] 2026-11-10 20:30: 11 to 16 and 18 to 21 (controller, hints and small effects, the game's picture
// switches; taken by the console with cheats off, run t2022). Not 17: sun shadows are waw_sun_shadows' row here.
const int kGameOptionRows[] = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10,  // 9, 10: the weapon in view, the HUD
                               11, 12, 13, 14, 15, 16, 18, 19, 20, 21,
                               22};  // 22 (21:00): smooth mouse in the game's own mouse look
constexpr int kGameOptionRowCount = int(sizeof(kGameOptionRows) / sizeof(kGameOptionRows[0]));

void DrawGame(PageContext& c) {
  if (f2::ResetButton("reset_game", "game settings")) {
    c.restart |= f2::ResetSettings({"waw_move_snap_hz", "waw_move_hz", "waw_fix_rifle_grenade_switch",
                                    "waw_fix_vehicle_push_while_mantling", "waw_fix_standing_but_looks_crouched",
                                    "waw_fix_dpad_item_firing"});
    for (const char* name : f2::kPlayerSettings)
      if (f2::Has(name)) c.restart |= f2::ResetSettings({name});
    for (int row : kGameOptionRows)
      if (f2::Has(f2::kGameOptions[row].setting)) c.restart |= f2::ResetSettings({f2::kGameOptions[row].setting});
    for (const char* name : f2::kGameNumberSettings)
      if (f2::Has(name)) c.restart |= f2::ResetSettings({name});
    for (const char* name : f2::kGameInputSettings)  // [f2-menus] 21:00
      if (f2::Has(name)) c.restart |= f2::ResetSettings({name});
    for (const char* name : f2::kGamePictureSettings)  // [f2-menus] 22:30
      if (f2::Has(name)) c.restart |= f2::ResetSettings({name});
    c.dirty = true;
  }
  f2::DrawPlayerSection(c);  // the player's name, split-screen names, which player each controller is
  Section("Movement");
  {
    const char* v[] = {"60", "0"};
    const char* n[] = {"The Xbox 360's (about 38 units at any frame rate)", "The game's own (lower at high frame rates)"};
    c.dirty |= f2::Choice("Jump height", "waw_move_snap_hz", v, n, 2, Applies::kAtOnce, &c.restart);
    Help("The game cuts the player's speed to whole units after every movement step, so the more frames a second, "
         "the lower a jump (38 units at 30 fps, about 31 at 250 fps and up). The Xbox 360's keeps the cut to its "
         "60 Hz display, so every jump is about 38 units, as on the console.");
  }
  {
    const char* v[] = {"0", "60", "125", "250"};
    const char* n[] = {"The game's own (one step a frame)", "60 steps a second", "125 steps a second",
                       "250 steps a second"};
    c.dirty |= f2::Choice("Movement steps", "waw_move_hz", v, n, 4, Applies::kAtOnce, &c.restart);
    Help("The player's movement in fixed steps, for the server and the client's prediction alike, so a walk or a "
         "jump is the same at any frame rate. Above that many frames a second the view moves once per step.");
  }
  Section("Title update 7's fixes (redone, no update needed)");
  c.dirty |= f2::Check("Rifle grenade switch fix", "waw_fix_rifle_grenade_switch", Applies::kAtOnce, &c.restart);
  Help("When a weapon change ends on the rifle grenade, the game doesn't start a second change back to the other "
       "weapon.");
  c.dirty |= f2::Check("No vehicle push while climbing", "waw_fix_vehicle_push_while_mantling", Applies::kAtOnce,
                       &c.restart);
  Help("A vehicle doesn't push a player who is climbing over something.");
  {
    const char* v[] = {"2", "1", "0"};
    const char* n[] = {"Just before landing (the update's)", "The whole time in the air", "Off"};
    c.dirty |= f2::Choice("Legs that look crouched", "waw_fix_standing_but_looks_crouched", v, n, 3,
                          Applies::kAtOnce, &c.restart);
    Help("A player who is standing but whose legs still look crouched or prone gets the right legs again while in "
         "the air.");
  }
  c.dirty |= f2::Check("No firing d-pad items by holding fire", "waw_fix_dpad_item_firing", Applies::kAtOnce,
                       &c.restart);
  Help("Holding fire doesn't fire a weapon the game treats as a d-pad item.");
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
  f2::DrawGamePictureRows(c, true);  // [f2-menus] 22:30 gamma and brightness (run g2214: both change the picture)
}

// ------------------------------------------------------------------------------------------------- CONTROLS
void DrawControls(PageContext& c) {
  static const char* const kBinds[][2] = {
      {"keybind_a", "A"}, {"keybind_b", "B"}, {"keybind_x", "X"}, {"keybind_y", "Y"},
      {"keybind_left_trigger", "Left trigger (aim)"}, {"keybind_right_trigger", "Right trigger (fire)"},
      {"keybind_left_shoulder", "Left shoulder"}, {"keybind_right_shoulder", "Right shoulder"},
      {"keybind_lstick_up", "Move forward"}, {"keybind_lstick_down", "Move back"},
      {"keybind_lstick_left", "Move left"}, {"keybind_lstick_right", "Move right"},
      {"keybind_lstick_press", "Left stick press (sprint)"}, {"keybind_rstick_up", "Look up"},
      {"keybind_rstick_down", "Look down"}, {"keybind_rstick_left", "Look left"},
      {"keybind_rstick_right", "Look right"}, {"keybind_rstick_press", "Right stick press (melee)"},
      {"keybind_dpad_up", "D-pad up"}, {"keybind_dpad_down", "D-pad down"}, {"keybind_dpad_left", "D-pad left"},
      {"keybind_dpad_right", "D-pad right"}, {"keybind_back", "Back"}, {"keybind_start", "Start"},
  };
  if (f2::ResetButton("reset_controls", "control settings")) {
    c.restart |= f2::ResetSettings({"mnk_mode", "mnk_mouse", "mnk_sensitivity", "waw_f2_pad_shortcut",
                                    "waw_mouse_look", "waw_pc_keys"});  // [f2-menus] ctl 14:50 + waw_pc_keys
    for (const auto& bind : kBinds) c.restart |= f2::ResetSettings({bind[0]});
    {  // [f2-menus] 2026-11-11 ctl 14:50 the mouse acceleration, the randomizer and the scales, as the shared page
      const char* const* names = nullptr;
      const int count = f2::accel::Settings(&names);
      for (int i = 0; i < count; ++i) c.restart |= f2::ResetSettings({names[i]});
    }
    c.dirty = true;
  }
  Section("Keyboard and mouse");
  c.dirty |= f2::Check("Play with keyboard and mouse", "mnk_mode", Applies::kAtOnce, &c.restart);
  Help("The keyboard and mouse play the game: in a level the PC keys below, in the menus each key of Keys presses a "  // [f2-menus] ctl 14:50
       "controller button.");  // [f2-menus] ctl 14:50
  c.dirty |= f2::Check("The mouse turns the view", "mnk_mouse", Applies::kAtOnce, &c.restart);
  c.dirty |= f2::SliderDouble("Mouse sensitivity", "mnk_sensitivity", 0.1, 10.0, "%.2f", Applies::kAtOnce, &c.restart);
  c.dirty |= f2::Check("Aim with the game's own mouse look", "waw_mouse_look", Applies::kAtOnce, &c.restart);
  Help("The mouse turns the view through World at War's own PC mouse code: the game's look sensitivity in its "
       "Options (and sensitivity, m_yaw, m_pitch, m_filter, cl_mouseAccel in the console), as on the PC, and the "
       "pointer is held while you play. Off: the mouse moves the right stick, at the mouse sensitivity above.");
  // [f2-menus] 2026-11-11 ctl 14:50 (owner 14:29: "Check all control options and make sure they work."): the PC keys
  // (waw_keys.cpp, on by default) had no row, and the acceleration rows weren't drawn though waw_mouse.cpp applies them.
  c.dirty |= f2::Check("The PC copy's keys in a level", "waw_pc_keys", Applies::kAtOnce, &c.restart);  // [f2-menus]
  Help("While a level plays, a key the game has a bind for runs that bind, as on the PC: W A S D, Space, C, Ctrl, "  // [f2-menus]
       "Shift (sprint, hold breath), the mouse buttons, R, F, V, G, 4, 1 / 2 and the wheel, Q / E lean, Tab. The "  // [f2-menus]
       "first time a level starts, keys without a bind get these (a key you bound yourself keeps yours); change one "  // [f2-menus]
       "with bind in the CONSOLE page (bind f +activate). Off: every key presses a controller button, as in the "  // [f2-menus]
       "menus.");  // [f2-menus]
  f2::accel::DrawRows(c);  // [f2-menus] ctl 14:50 mouse acceleration, the randomizer and the scales (game's mouse look)

  Section("Controller");
  c.dirty |= f2::Check("Back + Start held 1 second opens these settings", "waw_f2_pad_shortcut", Applies::kAtOnce,
                       &c.restart);
  Help("Any controller. While this window is open the game gets no input; the D-pad or left stick moves, A chooses, "
       "B goes back, LB and RB change the page. The game's own button layouts and look sensitivity are in its "
       "Options.");

  if (f2::GetBool("mnk_mode")) {
    Section("Keys");
    Help("A key for each controller button: in the menus, and in a level for the keys the PC keys don't take "  // [f2-menus] ctl 14:50
         "(all of them with the PC keys off). Several keys go with commas (Semicolon,Space). Names: letters and "
         "digits, Space, Return, Tab, Backspace, Up, Down, Left, Right, Shift+Up, LMB, RMB. Type, then press Enter.");
    for (const auto& bind : kBinds) c.dirty |= f2::TextRow(bind[1], bind[0], &c.restart);
  }
}

// ------------------------------------------------------------------------------------------------- INTERFACE
void DrawInterface(PageContext& c) {
  if (f2::ResetButton("reset_interface", "interface settings")) {
    c.restart |= f2::ResetSettings({"waw_ui_theme", "waw_ui_text_size", "waw_ui_opacity"});
    c.dirty = true;
  }
  Section("Look");
  size_t count = 0;
  const f2::Theme* themes = f2::Themes(&count);
  const f2::Theme& chosen = f2::CurrentTheme();
  if (ImGui::BeginCombo("Theme", chosen.label)) {
    for (size_t i = 0; i < count; ++i) {
      const bool selected = &themes[i] == &chosen;
      if (ImGui::Selectable(themes[i].label, selected) && !selected) {
        rex::cvar::SetFlagByName("waw_ui_theme", themes[i].id);
        c.dirty = true;
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", themes[i].description);
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  Help(chosen.description);
  {
    const char* v[] = {"14", "16", "18", "20", "24"};
    const char* n[] = {"Small", "Normal", "Large", "Larger", "Largest"};
    c.dirty |= f2::Choice("Text size", "waw_ui_text_size", v, n, 5, Applies::kAtOnce, &c.restart);
    Help("The size of the text in this window. The game's own menus are not changed.");
  }
  c.dirty |= f2::SliderDouble("Window opacity", "waw_ui_opacity", 0.6, 1.0, "%.2f", Applies::kAtOnce, &c.restart);
  Help("Lower it to see the game behind this window while you change settings.");
}

// ------------------------------------------------------------------------------------------------- PERFORMANCE
void DrawPerformance(PageContext& c) {
  if (f2::ResetButton("reset_performance", "performance settings")) {
    c.restart |= f2::ResetSettings({"waw_extra_worker", "waw_native_skin"});
    c.dirty = true;
  }
  f2::DrawPerformanceRows(c);  // [f2-menus] 2026-11-11 02:10 the shared rows: what each frame costs
  Section("The game's work");
  c.dirty |= f2::Check("A third scene worker", "waw_extra_worker", Applies::kAfterRestart, &c.restart);
  Help("One more thread for the game's scene work (the Xbox 360 code makes two). Applies after a restart.");
  {
    const char* v[] = {"0", "1", "2"};
    const char* n[] = {"The translated code (default)", "Native code", "Both, compared (for testing)"};
    c.dirty |= f2::Choice("Model skinning", "waw_native_skin", v, n, 3, Applies::kAtOnce, &c.restart);
    Help("How soldiers' and weapons' vertices follow their bones: the game's own code as translated, or the same "
         "work in native code.");
  }
}

// ------------------------------------------------------------------------------------------------- EMULATION
void DrawEmulation(PageContext& c) { f2::DrawEmulationPage(c, nullptr, {}); }
void DrawCrosshair(PageContext& c) { f2::crosshair::DrawPage(c); }

// ------------------------------------------------------------------------------------------------- CONSOLE
void DrawConsole(PageContext& c) {
  (void)c;
  f2::DrawConsolePage(g_console_page, [](const std::string& command) {
    REXLOG_INFO("f2: console command: {}", command);
    QueueCommand(command);
  });
}

// ------------------------------------------------------------------------------------------------- once a UI frame
// Settings that follow a change made anywhere (F2, the console, the command line, the setup tool), and the test
// script.
void OnUiFrame(f2::Overlay& overlay) {
  // The frame rate counter: the game's cg_drawFPS follows waw_show_fps ("set", so it works before the game has
  // registered it). Only a change is sent, so a test's own cg_drawFPS stays.
  static bool shown_fps = false;
  if (REXCVAR_GET(waw_show_fps) != shown_fps) {
    shown_fps = REXCVAR_GET(waw_show_fps);
    QueueCommand(shown_fps ? "set cg_drawFPS 1" : "set cg_drawFPS 0");
  }

  f2::crosshair::Draw();  // the custom crosshair, window open or not
  // [f2-menus] 2026-11-11 00:50 a row given back at a menu is reset once more when a level starts (f2_common.h);
  // here the three aim assist dvars are the ones the game registers only then (run menu0011).
  f2::SetLevelRuns(waw::mouse::InGameplay(), [](std::string command) { QueueCommand(std::move(command)); });
  f2::SendGameOptions(kGameOptionRows, kGameOptionRowCount, [](std::string command) { QueueCommand(std::move(command)); });
  f2::SendGameNumbers([](std::string command) { QueueCommand(std::move(command)); });

  // waw_f2_test_script: parsed once; each step runs once, in order, when its time has come.
  struct Step {
    double seconds;
    std::string action;
  };
  static const std::vector<Step> steps = [] {
    std::vector<Step> out;
    const std::string all = REXCVAR_GET(waw_f2_test_script);
    size_t start = 0;
    while (start < all.size()) {
      size_t end = all.find('|', start);
      if (end == std::string::npos) end = all.size();
      const std::string entry = all.substr(start, end - start);
      start = end + 1;
      const size_t colon = entry.find(':');
      if (colon == std::string::npos || colon == 0 || colon + 1 >= entry.size()) continue;
      out.push_back({std::strtod(entry.c_str(), nullptr), entry.substr(colon + 1)});
    }
    return out;
  }();
  if (steps.empty()) return;
  static const auto first = std::chrono::steady_clock::now();
  static size_t next = 0;
  const double since = std::chrono::duration<double>(std::chrono::steady_clock::now() - first).count();
  while (next < steps.size() && since >= steps[next].seconds) {
    const std::string& action = steps[next++].action;
    REXLOG_INFO("f2: test step: {}", action);
    if (action == "open" && !overlay.WindowOpen()) {
      overlay.Toggle();
    } else if (action == "close" && overlay.WindowOpen()) {
      overlay.Toggle();
    } else if (action.rfind("page ", 0) == 0) {
      if (auto* w = overlay.settings_window(); !w || !w->ShowPage(action.substr(5)))
        REXLOG_WARN("f2: test step: no page {}", action.substr(5));
    } else if (action.rfind("set ", 0) == 0) {
      const std::string rest = action.substr(4);
      const size_t space = rest.find(' ');
      if (space != std::string::npos) {
        const bool ok = rex::cvar::SetFlagByName(rest.substr(0, space), rest.substr(space + 1));
        if (auto* w = overlay.settings_window()) w->MarkDirty();
        REXLOG_INFO("f2: test step: {} = {} ({})", rest.substr(0, space), f2::Get(rest.substr(0, space).c_str()),
                    ok ? "set" : "refused");
      }
    } else if (action == "save") {
      if (auto* w = overlay.settings_window()) w->Save();
    } else if (action.rfind("console ", 0) == 0) {
      QueueCommand(action.substr(8));  // as if typed on the CONSOLE page
    }
  }
}

}  // namespace

void ConfigureFonts(ImFontAtlas* atlas) {
  f2::LookSettings look;
  look.theme_name = "waw_ui_theme";
  look.theme = &FLAGS_waw_ui_theme_storage_;
  look.text_size_name = "waw_ui_text_size";
  look.text_size = &FLAGS_waw_ui_text_size_storage_;
  look.opacity_name = "waw_ui_opacity";
  look.opacity = &FLAGS_waw_ui_opacity_storage_;
  look.own = {"waw", "World at War", "World at War's own menu colors: dark steel panels, amber highlights.",
              &kWawColors};
  f2::SetLook(look);
  f2::ConfigureFonts(atlas);
  REXLOG_INFO("f2: look {} (text {} px, opacity {:.2f})", f2::CurrentTheme().id, REXCVAR_GET(waw_ui_text_size),
              REXCVAR_GET(waw_ui_opacity));
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
  options.spec.title = "CALL OF DUTY: WORLD AT WAR";
  options.spec.id = "waw_settings";
  options.spec.config_path = std::move(config_path);
  options.spec.pages = {
      {"VIDEO", DrawVideo},           {"SOUND", DrawSound},     {"GAME", DrawGame},
      {"CROSSHAIR", DrawCrosshair},
      {"CONTROLS", DrawControls},     {"INTERFACE", DrawInterface}, {"PERFORMANCE", DrawPerformance},
      {"EMULATION", DrawEmulation},   {"CONSOLE", DrawConsole},
  };
  options.pad_shortcut_setting = "waw_f2_pad_shortcut";
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

bool HasCommands() { return g_has_commands.load(std::memory_order_acquire); }

void TakeCommands(std::vector<std::string>& out) {
  std::lock_guard<std::mutex> lock(g_commands_mutex);
  out.swap(g_commands);
  g_commands.clear();
  g_has_commands.store(false, std::memory_order_release);
}

}  // namespace waw::f2ui

// ------------------------------------------------------------------------------------------------- crosshair
// [f2-menus] 2026-11-10 The custom crosshair (f2/f2_common.h, f2::crosshair). CG_DrawCrosshair (0x82105D30, names
// from dev/waw-decomp/x360_sp) draws the hip-fire crosshair, after every check of its own, with two calls: the
// center piece 0x821058D0 and CG_DrawReticleSides 0x82105AE8, both (x f1, y f2, scale f3, local client, weapon,
// color): the local client in r3, the weapon in r4, the color in r5 (World at War, run xh2). With the custom
// crosshair on, the center piece is skipped and the sides' function still runs, so the game
// works out its spread; its four pieces go through ScrPlace_ApplyRect and CG_DrawRotatedPicPhysical, whose hooks
// measure them and leave them undrawn (f2::crosshair's capture). The overlay gets the game's color and spread. The color is looked for in r5 first, then r8, r6, r7: the first that points at four
// numbers between 0 and 1 that aren't all 0. f2_debug_crosshair logs the
// registers once a second.
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
REX_EXTERN(__imp__sub_821058D0);
REX_HOOK_RAW(sub_821058D0) {
  if (f2::crosshair::Enabled()) return;  // the center piece: the custom crosshair has its own
  __imp__sub_821058D0(ctx, base);
}
REX_EXTERN(__imp__sub_82105AE8);
REX_HOOK_RAW(sub_82105AE8) {
  if (f2::crosshair::Enabled()) {
    CrosshairSides(ctx, base, [&] { __imp__sub_82105AE8(ctx, base); });
    return;
  }
  __imp__sub_82105AE8(ctx, base);
}
REX_EXTERN(__imp__sub_82146A00);
REX_HOOK_RAW(sub_82146A00) {  // ScrPlace_ApplyRect(place r3, &x r4, &y r5, &width r6, &height r7, ...)
  if (f2::crosshair::Capturing()) f2::crosshair::CaptureRect(base, ctx.r4.u32, ctx.r6.u32);
  __imp__sub_82146A00(ctx, base);
}
REX_EXTERN(__imp__sub_82106170);
REX_HOOK_RAW(sub_82106170) {  // CG_DrawRotatedPicPhysical: the side piece itself
  if (f2::crosshair::Capturing()) return;
  __imp__sub_82106170(ctx, base);
}
