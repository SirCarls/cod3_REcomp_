// cod2 / cod2mp - Call of Duty 2's F2 settings window (see cod2_f2.h). [f2-menus]
//
// Rows edit settings the port or the SDK already has, or one of the game's own dvars through its console (the
// PC copy's Game options: crosshair, subtitles, blood; read from the PC copy's ui/options_game.menu, see
// claude/sprint-cod2-bro-classic-design.md). New here: the window's own look, the controller shortcut's switch, the
// field of view (the game's cg_fov) and a test script. SOUND, INTERFACE, EMULATION and most of CONTROLS are the
// shared pages (../recomp/src/f2/f2_common.h).
//
// CONTROLS has the PC copy's Look options (sensitivity, invert, smoothing; free look is always on), the switch for
// its keys (cod2_pc_input.h) and a row for each action of the PC copy's controls menus: click the row's button, then
// press the key or mouse button for it (the rows edit the one setting cod2_pc_binds). Not here yet: a crosshair page
// (the hooks are Call of Duty 4's and World at War's).

#include "cod2_f2.h"

#include <cctype>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/style.h>

#include "cod2_pc_input.h"
#include "f2/f2_common.h"
#include "f2/f2_console.h"
#include "f2/f2_rows.h"
#include "f2/f2_theme.h"
#include "f2/f2_window.h"

REXCVAR_DEFINE_STRING(cod2_ui_theme, "cod2", "cod2/Interface",
                      "Colors of the F2 settings window: cod2 (Call of Duty 2's), cod3 (Call of Duty 3's), olive, "
                      "parchment or contrast");
REXCVAR_DEFINE_INT32(cod2_ui_text_size, 16, "cod2/Interface", "Text size of the F2 settings window, in pixels (14-24)")
    .range(14, 24);
REXCVAR_DEFINE_DOUBLE(cod2_ui_opacity, 0.94, "cod2/Interface",
                      "How solid the F2 settings window is (0.6 = see the game through it, 1 = solid)")
    .range(0.6, 1.0);
REXCVAR_DEFINE_BOOL(cod2_f2_pad_shortcut, true, "cod2/Interface",
                    "Back + Start held for a second on a controller opens and closes the F2 settings window");
REXCVAR_DEFINE_INT32(cod2_fov, 0, "cod2/Display",
                     "Field of view in degrees, through the game's own cg_fov (0 = leave the game's own alone)")
    .range(0, 160);
// [f2-menus] 2026-11-10 21:40 as Call of Duty 4's and World at War's row (FEATURES.md gap 2)
REXCVAR_DEFINE_BOOL(cod2_show_fps, false, "cod2/Display",
                    "Show the game's own frame rate counter (sets the game's cg_drawFPS)");
REXCVAR_DEFINE_STRING(cod2_f2_test_script, "", "cod2/Debug",
                      "Debug: F2 window steps for test runs, \"<seconds>:<step>|...\" from the first drawn frame; a "
                      "step is open, close, page <LABEL>, set <setting> <value> (as a row would, then unsaved), "
                      "save, or console <command> (as typed on the CONSOLE page)");

namespace cod2::f2ui {

namespace {

// Call of Duty 2's look: dark olive-brown panels, off-white text and the menus' gold for headings. NOT counted from
// the game's own menus yet (Call of Duty 4's and World at War's were, with tools/ff_colors.py); chosen by eye from
// the game's palette, to be replaced by counted colors.
constexpr ImVec4 Rgb(int r, int g, int b, float a = 1.0f) {
  return ImVec4(r / 255.0f, g / 255.0f, b / 255.0f, a);
}
const f2::ThemeColors kCod2Colors = {
    Rgb(18, 17, 13),         // window
    Rgb(24, 23, 18, 0.98f),  // popup
    Rgb(40, 38, 30),         // frame
    Rgb(64, 60, 46),         //   hovered
    Rgb(86, 80, 60),         //   pressed
    Rgb(48, 45, 35),         // button
    Rgb(70, 65, 49),
    Rgb(104, 94, 62),        //   pressed
    Rgb(10, 10, 8),          // title
    Rgb(28, 26, 20),
    Rgb(82, 76, 58, 0.8f),   // border
    Rgb(236, 232, 218),      // text
    Rgb(170, 164, 146),      // descriptions
    Rgb(214, 180, 96),       // accent: gold
    Rgb(228, 196, 112),      // headings and check marks
    Rgb(236, 232, 218, 0.05f),  // every other row
    Rgb(132, 200, 110),      // good
    Rgb(228, 196, 112),      // warn
    Rgb(230, 72, 56),        // bad
};

std::unique_ptr<f2::Overlay> g_overlay;
f2::ConsolePage g_console_page;
f2::CommandQueue g_commands;

using f2::Applies;
using f2::Help;
using f2::PageContext;
using f2::Section;

// [f2-menus] 2026-11-11 07:35 (moved up: the VIDEO page draws the picture switches of this list)
// The game's own options (indices into f2::kGameOptions): the PC copy's Game options (crosshair, subtitles, blood)
// and the Xbox 360's aim assist. Both programs have all seven dvars (dev/waw-decomp/cod2_360/{sp,mp}/globals.tsv).
#ifdef COD2_MP
// [f2-menus] 2026-11-10 22:25: multiplayer's dvar list is made (dev/agents/f2-menus/dvars/COD2_MP_DVARS.md): it has
// the dvars of 11 to 16 too, and cg_drawCrosshairNames (8), which the campaign lacks. Not 10: cg_drawHUD is the
// campaign's only.
const int kGameOptionRows[] = {0, 1, 2, 3, 4, 5, 6, 8, 9, 11, 12, 13, 14, 15, 16,
                               23, 24, 25, 26};  // [f2-menus] 2026-11-11 07:50 the picture switches (VIDEO page)
#else
// [f2-menus] 2026-11-10 20:30: 11 to 16 (controller, hints and small effects), all in the campaign's dvar list
// (dev/agents/f2-menus/dvars/COD2_SP_DVARS.md).
const int kGameOptionRows[] = {0, 1, 2, 3, 4, 5, 6, 9, 10,  // 9, 10: the weapon in view, the HUD
                               11, 12, 13, 14, 15, 16,
                               23, 24, 25, 26};  // [f2-menus] 2026-11-11 07:35 the picture switches (VIDEO page)
#endif
constexpr int kFirstPictureRow = 23;  // [f2-menus] rows from here are drawn and reset on the VIDEO page
constexpr int kGameOptionRowCount = int(sizeof(kGameOptionRows) / sizeof(kGameOptionRows[0]));

// ------------------------------------------------------------------------------------------------- VIDEO
void DrawVideo(PageContext& c) {
  if (f2::ResetButton("reset_video", "video settings")) {
    c.restart |= f2::ResetSettings({"fullscreen", "vsync", "cod2_max_fps", "cod2_fov", "cod2_graphics_api",
                                    "cod2_show_fps"});
    if (f2::Has("cod2_native_vulkan")) c.restart |= f2::ResetSettings({"cod2_native_vulkan"});  // [f2-menus] 13:35
    for (const char* name : f2::kPictureSettings)
      if (f2::Has(name)) c.restart |= f2::ResetSettings({name});
    for (const char* name : f2::kResolutionSettings) c.restart |= f2::ResetSettings({name});
    // [f2-menus] 2026-11-11 07:35 the game's own picture switches and numbers below (both programs since 07:50)
    for (int row : kGameOptionRows)
      if (row >= kFirstPictureRow) c.restart |= f2::ResetSettings({f2::kGameOptions[row].setting});
    for (const char* name : f2::kGamePictureSettings) c.restart |= f2::ResetSettings({name});
    for (const char* name : f2::kGameLightSettings) c.restart |= f2::ResetSettings({name});
    for (const char* name : f2::kGameCorpseSettings) c.restart |= f2::ResetSettings({name});  // [f2-menus] 07:52
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
    c.dirty |= f2::Choice("Graphics API", "cod2_graphics_api", v, n, 3, Applies::kAfterRestart, &c.restart);
    Help("Which graphics API draws the game. Applies after a restart.");
  }
#endif
  // [f2-menus] 2026-11-11 13:35 waw-decomp's cod2_native_vulkan (their 13:17), as Call of Duty 3's row.
  if (f2::Has("cod2_native_vulkan")) {  // [f2-menus]
    if (f2::Check("Native renderer (Vulkan)", "cod2_native_vulkan", Applies::kAfterRestart, &c.restart)) {  // [f2-menus]
      c.dirty = true;  // [f2-menus]
#ifdef _WIN32
      if (f2::GetBool("cod2_native_vulkan") && f2::Has("cod2_graphics_api") &&  // [f2-menus]
          f2::Get("cod2_graphics_api") == "d3d12")  // [f2-menus]
        rex::cvar::SetFlagByName("cod2_graphics_api", "vulkan");  // [f2-menus] it draws through Vulkan
#endif
    }  // [f2-menus]
    Help("On: the port's own Vulkan renderer draws the game, and the Xbox 360 GPU emulation is off. Off (the "  // [f2-menus]
         "default): the emulated Xbox 360 GPU draws it, as the game always has. The Vulkan renderer is still being "  // [f2-menus]
         "finished (no model shadows or glow yet), so a few things look different. Applies after a restart.");  // [f2-menus]
  }  // [f2-menus]
  if (f2::Has("cod2_max_fps")) {
    c.dirty |= f2::SliderInt("Frame rate limit", "cod2_max_fps", 0, 500, "%d fps", Applies::kAtOnce, &c.restart);
    Help("The most frames a second the game draws (the Xbox 360's 60 by default). 0 = no limit. The player's "
         "movement keeps its own fixed steps, so jumps and speeds don't change with it.");
  }
  c.dirty |= f2::Check("Show the frame rate", "cod2_show_fps", Applies::kAtOnce, &c.restart);  // [f2-menus] 21:40
  Help("The game's own frame rate counter in the corner (its cg_drawFPS).");

  f2::DrawResolutionSection(c);  // render scale, picture shape, window size (the same in every game)

  Section("View");
  {
    bool own = f2::GetInt("cod2_fov") > 0;
    if (ImGui::Checkbox("Use my own field of view", &own)) {
      rex::cvar::SetFlagByName("cod2_fov", own ? "80" : "0");
      c.dirty = true;
    }
    if (own) c.dirty |= f2::SliderInt("Field of view", "cod2_fov", 65, 120, "%d", Applies::kAtOnce, &c.restart);
    Help("Off: the game's own. On: your field of view in degrees, set through the game's cg_fov and sent again every "
         "few seconds, so a level's start doesn't put it back. Aiming down the sights still zooms from it.");
  }

  Section("Picture");
  f2::DrawPictureSection(c);  // post-process AA, output scaling and its sharpness, anisotropic filtering
  // [f2-menus] 2026-11-11 07:35 the PC copy's Graphics menu: brightness (r_gamma), shadows, soft smoke, dynamic
  // lights; with the game's glow and marks. Each was taken by the console in a level (run take0732). Every row
  // leaves the game's own value alone until it is changed. 07:50: multiplayer too; its console took all six at
  // its menu (run takemp0746; a match can't be started in this port yet, so not tried in one).
  Section("The game's own picture (as the PC copy's Graphics menu)");
  f2::DrawGamePictureRows(c, false);
  f2::DrawGameOptionRows(c, kGameOptionRows, kGameOptionRowCount, kFirstPictureRow, kFirstPictureRow + 3);
  f2::DrawGameLightRows(c);
#ifndef COD2_MP
  f2::DrawGameCorpseRows(c);  // [f2-menus] 2026-11-11 07:52 the campaign's ai_corpseCount (multiplayer has none)
#endif
}

// ------------------------------------------------------------------------------------------------- GAME
void DrawGame(PageContext& c) {
  if (f2::ResetButton("reset_game", "game settings")) {
#ifndef COD2_MP
    c.restart |= f2::ResetSettings({"cod2_skip_intro_movies", "cod2_move_hz", "cod2_sprint", "cod2_sprint_speed",
                                    "cod2_sprint_time", "cod2_sprint_recover", "cod2_pc_movement"});
#endif
    for (const char* name : f2::kPlayerSettings)
      if (f2::Has(name)) c.restart |= f2::ResetSettings({name});
    for (int row : kGameOptionRows)
      if (row < kFirstPictureRow && f2::Has(f2::kGameOptions[row].setting))  // [f2-menus] 07:35 VIDEO's are its own
        c.restart |= f2::ResetSettings({f2::kGameOptions[row].setting});
    for (const char* name : f2::kGameNumberSettings)
      if (f2::Has(name)) c.restart |= f2::ResetSettings({name});
    for (const char* name : f2::kGameInputSettings)  // [f2-menus] 21:00
      if (f2::Has(name)) c.restart |= f2::ResetSettings({name});
    c.dirty = true;
  }
  f2::DrawPlayerSection(c);  // the player's name, split-screen names, which player each controller is
#ifndef COD2_MP
  if (f2::Has("cod2_skip_intro_movies")) {
    Section("Start");
    c.dirty |= f2::Check("Skip the start-up films", "cod2_skip_intro_movies", Applies::kAfterRestart, &c.restart);
    Help("The publisher's and studio's films are left out, so the Press START screen comes at once. Applies at the "
         "next start.");
  }
  if (f2::Has("cod2_sprint")) {
    Section("Sprint");
    c.dirty |= f2::Check("Sprint", "cod2_sprint", Applies::kAtOnce, &c.restart);
    Help("Hold Left Shift, or press the left stick in, while moving: you run faster until you tire. Call of Duty 2 "
         "has no sprint of its own (on the Xbox 360 or the PC); this follows United Offensive's: not while "
         "crouched, prone, aiming or firing, and the gun stays up for now.");
    c.dirty |= f2::SliderDouble("Sprint speed", "cod2_sprint_speed", 1.0, 2.0, "%.2f x", Applies::kAtOnce, &c.restart);
    Help("How many times the running speed. United Offensive's is 1.6.");
    c.dirty |= f2::SliderDouble("Sprint time", "cod2_sprint_time", 0.0, 15.0, "%.1f s", Applies::kAtOnce, &c.restart);
    Help("Seconds of sprint from fresh. United Offensive's is 3. 0 = you never tire.");
    c.dirty |= f2::SliderDouble("Recovery time", "cod2_sprint_recover", 1.0, 20.0, "%.1f s", Applies::kAtOnce,
                                &c.restart);
    Help("Seconds to be fresh again. It starts a second after the sprint, with the sprint key let go. United "
         "Offensive's is 7.");
  }
  if (f2::Has("cod2_pc_movement") || f2::Has("cod2_move_hz")) Section("Movement");
  if (f2::Has("cod2_pc_movement")) {
    c.dirty |= f2::Check("Move as in the PC copy", "cod2_pc_movement", Applies::kAtOnce, &c.restart);
    Help("The PC copy's numbers where the Xbox 360 program's differ: sideways at 0.8 and backwards at 0.7 of the "
         "forward speed, the view up and down to 85 degrees instead of 70, and binoculars stay up after a press. "
         "Off: the Xbox 360's own (full speed in every direction, 70 degrees, binoculars while held).");
  }
  if (f2::Has("cod2_move_hz")) {
    const char* v[] = {"60", "125", "0"};
    const char* n[] = {"The Xbox 360's (60 steps a second, the same at any frame rate)", "125 steps a second",
                       "The game's own (one step a frame: jumps change with the frame rate)"};
    c.dirty |= f2::Choice("Movement steps", "cod2_move_hz", v, n, 3, Applies::kAtOnce, &c.restart);
    Help("The game moves the player once a frame, so jump heights and speeds change with the frame rate. Fixed "
         "steps keep them the same at any frame rate.");
  }
#endif
  Section("The game's own options");
  f2::DrawGameOptionRows(c, kGameOptionRows, kGameOptionRowCount, 0, 10);
  f2::DrawGameNumberRows(c);  // the weapon's place on screen, the game crosshair's opacity
  // [f2-menus] 2026-11-10 20:30 more of the game's own switches, in groups (dev/agents/f2-menus/FEATURES.md);
  // 22:25 in multiplayer too.
  Section("Controller (the game's own switches)");
  f2::DrawGameOptionRows(c, kGameOptionRows, kGameOptionRowCount, 11, 12);
  f2::DrawGameInputRows(c, false);  // [f2-menus] 21:00 the stick's look speed (the mouse rows are CONTROLS')
  Section("Hints and small effects");
  f2::DrawGameOptionRows(c, kGameOptionRows, kGameOptionRowCount, 13, 16);
}

// ------------------------------------------------------------------------------------------------- CONTROLS
// The keys of cod2_pc_keys, a row an action. The actions are the PC copy's controls menus (ui/options_move.menu,
// options_shoot.menu, options_look.menu, options_misc.menu, read with dev/agents/f2-menus/tools/cod2_pc_options.py);
// what each command does was checked by pressing it in a level and reading the usercmd (runs cod2pc1, cod2bd2:
// +speed aims only while held, +movedown and +prone hold the stance, lowerstance goes stand, crouch, prone a press,
// +melee_breath is melee and hold breath in one). Left out: the PC menus' +moveup (the same as +gostand), the
// keyboard look and turn commands, and the screenshot.
struct KeyAction {
  const char* label;
  const char* command;
};
const KeyAction kMoveActions[] = {
    {"Forward", "+forward"},          {"Backwards", "+back"},           {"Left", "+moveleft"},
    {"Right", "+moveright"},          {"Lean left", "+leanleft"},       {"Lean right", "+leanright"},
    {"Stand up / jump", "+gostand"},  {"Crouch", "gocrouch"},           {"Go prone", "goprone"},
    {"Crouch (toggle)", "togglecrouch"}, {"Prone (toggle)", "toggleprone"},
    {"Crouch (while held)", "+movedown"}, {"Prone (while held)", "+prone"},
    {"Lower stance", "lowerstance"},  {"Raise stance", "raisestance"},
};
const KeyAction kFightActions[] = {
    {"Fire", "+attack"},              {"Aim down the sight (toggle)", "toggleads"},
    {"Aim down the sight (while held)", "+speed"},
    {"Next weapon", "weapnext"},      {"Reload", "+reload"},            {"Use", "+activate"},
    {"Frag grenade", "+frag"},        {"Smoke grenade", "+smoke"},      {"Melee", "+melee"},
    {"Hold breath", "+holdbreath"},   {"Melee and hold breath (the PC's Shift)", "+melee_breath"},
    {"Binoculars", "+binoculars"},    {"Objectives", "+scores"},
};

struct KeyBind {
  std::string key;
  std::string command;
};

std::vector<KeyBind> ParseBinds(const std::string& text) {
  std::vector<KeyBind> binds;
  size_t at = 0;
  while (at < text.size()) {
    size_t end = text.find(';', at);
    if (end == std::string::npos) end = text.size();
    const std::string item = text.substr(at, end - at);
    at = end + 1;
    const size_t eq = item.find('=');
    if (eq == std::string::npos || eq == 0 || eq + 1 >= item.size()) continue;
    binds.push_back({item.substr(0, eq), item.substr(eq + 1)});
  }
  return binds;
}

std::string JoinBinds(const std::vector<KeyBind>& binds) {
  std::string text;
  for (const KeyBind& b : binds) text += (text.empty() ? "" : ";") + b.key + "=" + b.command;
  return text;
}

bool SameKey(const std::string& a, const std::string& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i]))) return false;
  return true;
}

// The name cod2_pc_binds has for a key Dear ImGui reports ("" for one that can't be bound: Escape, F2, the rest).
std::string BindKeyName(ImGuiKey key) {
  if (key >= ImGuiKey_A && key <= ImGuiKey_Z) return std::string(1, char('A' + (key - ImGuiKey_A)));
  if (key >= ImGuiKey_0 && key <= ImGuiKey_9) return std::string(1, char('0' + (key - ImGuiKey_0)));
  if (key >= ImGuiKey_F1 && key <= ImGuiKey_F12) {
    const int f = 1 + (key - ImGuiKey_F1);
    return f == 2 ? std::string() : "F" + std::to_string(f);
  }
  switch (key) {
    case ImGuiKey_LeftShift:
    case ImGuiKey_RightShift: return "Shift";
    case ImGuiKey_LeftCtrl:
    case ImGuiKey_RightCtrl: return "Ctrl";
    case ImGuiKey_LeftAlt:
    case ImGuiKey_RightAlt: return "Alt";
    case ImGuiKey_Space: return "Space";
    case ImGuiKey_Tab: return "Tab";
    case ImGuiKey_Enter: return "Enter";
    case ImGuiKey_CapsLock: return "CapsLock";
    case ImGuiKey_Backspace: return "Backspace";
    case ImGuiKey_LeftArrow: return "Left";
    case ImGuiKey_RightArrow: return "Right";
    case ImGuiKey_UpArrow: return "Up";
    case ImGuiKey_DownArrow: return "Down";
    case ImGuiKey_Insert: return "Ins";
    case ImGuiKey_Delete: return "Del";
    case ImGuiKey_Home: return "Home";
    case ImGuiKey_End: return "End";
    case ImGuiKey_PageUp: return "PgUp";
    case ImGuiKey_PageDown: return "PgDn";
    case ImGuiKey_MouseLeft: return "Mouse1";
    case ImGuiKey_MouseRight: return "Mouse2";
    case ImGuiKey_MouseMiddle: return "Mouse3";
    case ImGuiKey_MouseX1: return "Mouse4";
    case ImGuiKey_MouseX2: return "Mouse5";
    default: return std::string();
  }
}

// The row that waits for a key, if any: its command, whether the key is added to the action's keys or replaces
// them, and the frames that keep one click from counting twice.
std::string g_listen_command;
bool g_listen_add = false;
bool g_listen_armed = false;  // every mouse button was up once since the row's button was clicked
int g_listen_frame = -1;      // the last frame the rows were drawn while one waited
int g_listen_done_frame = -1;  // the frame a key was taken (the same press mustn't click a button too)

// Once a frame, before the rows: the key or mouse button pressed for the row that waits. True when the keys changed.
bool TakeListenedKey(std::vector<KeyBind>& binds) {
  const int frame = ImGui::GetFrameCount();
  if (g_listen_command.empty()) return false;
  if (g_listen_frame >= 0 && frame > g_listen_frame + 1) {  // the page wasn't drawn: the window or page changed
    g_listen_command.clear();
    g_listen_frame = -1;
    return false;
  }
  g_listen_frame = frame;
  if (!g_listen_armed) {
    if (!ImGui::IsAnyMouseDown()) g_listen_armed = true;
    return false;
  }
  if (ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
    g_listen_command.clear();
    g_listen_frame = -1;
    g_listen_done_frame = frame;
    return false;
  }
  std::string name;
  for (int k = ImGuiKey_NamedKey_BEGIN; k < ImGuiKey_NamedKey_END && name.empty(); ++k) {
    const ImGuiKey key = static_cast<ImGuiKey>(k);
    if (ImGui::IsKeyPressed(key, false)) name = BindKeyName(key);
  }
  if (name.empty()) {
    const float wheel = ImGui::GetIO().MouseWheel;
    if (wheel > 0.0f) name = "MWheelUp";
    if (wheel < 0.0f) name = "MWheelDown";
  }
  if (name.empty()) return false;
  // A key has one action: it leaves the one it was on. Without "+", it replaces the action's other keys.
  for (size_t i = binds.size(); i-- > 0;)
    if (SameKey(binds[i].key, name) || (!g_listen_add && binds[i].command == g_listen_command))
      binds.erase(binds.begin() + static_cast<std::ptrdiff_t>(i));
  binds.push_back({name, g_listen_command});
  REXLOG_INFO("f2: key {} = {}", name, g_listen_command);
  g_listen_command.clear();
  g_listen_frame = -1;
  g_listen_done_frame = frame;
  return true;
}

// One action's row: a button with its keys (click, then press the new key), "+" to add a second key, "x" for none.
bool KeyRow(const KeyAction& action, std::vector<KeyBind>& binds) {
  bool changed = false;
  std::string keys;
  for (const KeyBind& b : binds)
    if (b.command == action.command) keys += (keys.empty() ? "" : ", ") + b.key;
  const bool listening = g_listen_command == action.command;
  const bool clickable = ImGui::GetFrameCount() > g_listen_done_frame + 1;
  ImGui::PushID(action.command);
  const std::string text =
      listening ? std::string("Press a key or mouse button (Esc: leave it)") : (keys.empty() ? "(no key)" : keys);
  const float width = ImGui::CalcItemWidth();
  const float small = ImGui::GetFrameHeight();
  const float spacing = ImGui::GetStyle().ItemInnerSpacing.x;
  if (ImGui::Button((text + "##key").c_str(), ImVec2(width - 2.0f * (small + spacing), 0.0f)) && clickable) {
    g_listen_command = action.command;
    g_listen_add = false;
    g_listen_armed = false;
    g_listen_frame = -1;
  }
  ImGui::SameLine(0.0f, spacing);
  if (ImGui::Button("+", ImVec2(small, 0.0f)) && clickable) {
    g_listen_command = action.command;
    g_listen_add = true;
    g_listen_armed = false;
    g_listen_frame = -1;
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Add another key for this");
  ImGui::SameLine(0.0f, spacing);
  if (ImGui::Button("x", ImVec2(small, 0.0f)) && clickable && !keys.empty()) {
    for (size_t i = binds.size(); i-- > 0;)
      if (binds[i].command == action.command) binds.erase(binds.begin() + static_cast<std::ptrdiff_t>(i));
    changed = true;
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("No key for this");
  ImGui::SameLine(0.0f, spacing);
  ImGui::TextUnformatted(action.label);
  ImGui::PopID();
  return changed;
}

void DrawKeyRows(PageContext& g) {
  std::vector<KeyBind> binds = ParseBinds(f2::Get("cod2_pc_binds"));
  bool changed = TakeListenedKey(binds);
  Section("Keys in a level: moving");
  for (const KeyAction& action : kMoveActions) changed |= KeyRow(action, binds);
  Section("Keys in a level: fighting");
  for (const KeyAction& action : kFightActions) changed |= KeyRow(action, binds);
  Help("Click an action's button, then press the key or mouse button for it (a wheel notch counts too). + adds a "
       "second key, x leaves the action without one. A key does one thing: giving it to an action takes it from "
       "the one it had. Escape, F2 and the console key can't be used.");
#ifndef COD2_MP
  Help("Sprint is Left Shift, or the left stick pressed in, and can't be moved yet.");
#endif
  if (changed) {
    rex::cvar::SetFlagByName("cod2_pc_binds", JoinBinds(binds));
    g.dirty = true;
  }
}

void DrawControls(PageContext& c) {
  f2::DrawControlsPage(
      c, "cod2_f2_pad_shortcut",
      [](PageContext& g) {
        if (!f2::Has("cod2_mouse_look")) return;
        g.dirty |= f2::Check("Aim with the game's own mouse look", "cod2_mouse_look", Applies::kAtOnce, &g.restart);
        Help("The mouse turns the view through Call of Duty 2's own PC mouse code, as on the PC, and the pointer is "
             "held while a level plays. Off: the mouse moves the right stick, at the mouse sensitivity above.");
        g.dirty |= f2::SliderDouble("Mouse sensitivity (game)", "cod2_mouse_sensitivity", 1.0, 30.0, "%.1f",
                                    Applies::kAtOnce, &g.restart);
        Help("The game's own sensitivity, for its mouse look. The PC copy's default is 5.");
        g.dirty |= f2::Check("Turn as far as the PC copy", "cod2_mouse_match_pc", Applies::kAtOnce, &g.restart);
        Help("The same sensitivity turns the view as far as in the PC copy. Off: the Xbox 360 program's own turn, "
             "about a quarter more, because its wide picture counts as a wider view.");
        g.dirty |= f2::Check("Invert mouse", "cod2_mouse_invert", Applies::kAtOnce, &g.restart);
        Help("Pushing the mouse away looks down.");
        g.dirty |= f2::Check("Smooth mouse", "cod2_mouse_smooth", Applies::kAtOnce, &g.restart);
        Help("Each turn is the average of the last two reads of the mouse: smoother, and a little later.");
        g.dirty |= f2::Check("The PC copy's keys", "cod2_pc_keys", Applies::kAtOnce, &g.restart);
        Help("While a level plays, the keys do what they do in the PC copy: W A S D, lean on Q and E, Space to "
             "stand or jump, C to crouch, Ctrl to go prone, the right mouse button toggles aiming, 1 or 2 switch "
             "weapons, R reloads, F uses, G or the middle button throws a grenade, 4 smoke, B binoculars, Tab the "
             "objectives. Shift holds your breath (and sprints, in the campaign), V is melee (the PC copy has melee on "
             "Shift). In "
             "menus the keys are the controller's, as before. Off: every key presses a controller button.");
        if (f2::GetBool("cod2_pc_keys")) DrawKeyRows(g);
      },
      {"cod2_mouse_look", "cod2_mouse_sensitivity", "cod2_mouse_match_pc", "cod2_mouse_invert", "cod2_mouse_smooth",
       "cod2_pc_keys", "cod2_pc_binds"});
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
  // The field of view: the game's cg_fov follows cod2_fov while it is above 0, on a change and every three seconds
  // (a level's start sets the game's own again). Back at 0, cg_fov is reset once.
  using Clock = std::chrono::steady_clock;
  static int fov = 0;
  static Clock::time_point next_fov{};
  const int wanted = REXCVAR_GET(cod2_fov);
  const auto now = Clock::now();
  if (wanted != fov || (wanted > 0 && now >= next_fov)) {
    if (wanted > 0)
      g_commands.Push("set cg_fov " + std::to_string(wanted));
    else if (fov > 0) {
      g_commands.Push("reset cg_fov");
      f2::ResetAgainAtLevelStart("cg_fov");  // [f2-menus] 2026-11-11 00:15
    }
    fov = wanted;
    next_fov = now + std::chrono::seconds(3);
  }
  // [f2-menus] 21:40 the frame rate counter: the game's cg_drawFPS follows cod2_show_fps; only a change is sent.
  static bool shown_fps = false;
  if (REXCVAR_GET(cod2_show_fps) != shown_fps) {
    shown_fps = REXCVAR_GET(cod2_show_fps);
    g_commands.Push(shown_fps ? "set cg_drawFPS 1" : "set cg_drawFPS 0");
  }
  // [f2-menus] 2026-11-11 00:15 a row given back at a menu is reset once more when a level starts (f2_common.h)
  f2::SetLevelRuns(cod2::pc_input::LevelRuns(), [](std::string command) { g_commands.Push(std::move(command)); });
  f2::SendGameOptions(kGameOptionRows, kGameOptionRowCount,
                      [](std::string command) { g_commands.Push(std::move(command)); });
  f2::SendGameNumbers([](std::string command) { g_commands.Push(std::move(command)); });
  static f2::TestScript script(REXCVAR_GET(cod2_f2_test_script));
  script.Run(overlay, g_commands);
}

}  // namespace

void ConfigureFonts(ImFontAtlas* atlas) {
  f2::LookSettings look;
  look.theme_name = "cod2_ui_theme";
  look.theme = &FLAGS_cod2_ui_theme_storage_;
  look.text_size_name = "cod2_ui_text_size";
  look.text_size = &FLAGS_cod2_ui_text_size_storage_;
  look.opacity_name = "cod2_ui_opacity";
  look.opacity = &FLAGS_cod2_ui_opacity_storage_;
  look.own = {"cod2", "Call of Duty 2", "Dark olive panels, off-white text and gold headings.", &kCod2Colors};
  f2::SetLook(look);
  f2::ConfigureFonts(atlas);
  REXLOG_INFO("f2: look {} (text {} px, opacity {:.2f})", f2::CurrentTheme().id, REXCVAR_GET(cod2_ui_text_size),
              REXCVAR_GET(cod2_ui_opacity));
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
  options.spec.title = "CALL OF DUTY 2";
#ifdef COD2_MP
  options.spec.id = "cod2mp_settings";
#else
  options.spec.id = "cod2_settings";
#endif
  options.spec.pages = {
      {"VIDEO", DrawVideo},       {"SOUND", f2::DrawSoundPage},         {"GAME", DrawGame},
      {"CONTROLS", DrawControls}, {"INTERFACE", f2::DrawInterfacePage}, {"PERFORMANCE", DrawPerformance},
      {"EMULATION", DrawEmulation},   {"CONSOLE", DrawConsole},
  };
  options.spec.config_path = std::move(config_path);
  options.pad_shortcut_setting = "cod2_f2_pad_shortcut";
  options.defer = std::move(defer);
  options.on_frame = OnUiFrame;
  g_overlay = std::make_unique<f2::Overlay>(drawer, window, std::move(options));
  f2::accel::SetSupported(true);  // [f2-menus] 2026-11-11 cod2_pc_input.cpp applies it (the game's own mouse look)
  // [f2-menus] 03:20 sensitivity scales: with "as the PC copy" a count turns 0.022 degrees x the sensitivity.
  f2::accel::SetGameSensitivity("cod2_mouse_sensitivity", "cod2_mouse_match_pc", 0.022, 0.01, 100.0);
  REXLOG_INFO("f2: ready (F2, or Back + Start held 1 s on a controller)");
}

void Detach() { g_overlay.reset(); }

bool GameGetsInput() { return !g_overlay || g_overlay->GameGetsInput(); }

void NoteConsoleLine(int channel, std::string_view line) { f2::ConsoleLog::Get().Add(channel, line); }

bool HasCommands() { return g_commands.Has(); }

void TakeCommands(std::vector<std::string>& out) { g_commands.Take(out); }

}  // namespace cod2::f2ui
