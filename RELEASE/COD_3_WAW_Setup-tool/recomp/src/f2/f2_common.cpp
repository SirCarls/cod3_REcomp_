// f2 - pages and helpers every game's F2 window shares (see f2_common.h). [f2-menus]

#include "f2/f2_common.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <random>
#include <thread>
#include <utility>
#include <vector>

#include <imgui.h>

#include <rex/audio/output_device.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/window.h>

#include "f2/f2_rows.h"
#include "f2/f2_theme.h"
#include "f2/f2_window.h"

// [f2-menus] 2026-11-11 02:10 the GPU emulation's running totals (SDK patch 0012), as Call of Duty 3's
// game/perf_stats.cpp reads them; a build whose SDK lacks the header says so in the log and shows nothing.
#if __has_include(<rex/graphics/gpu_stats.h>)
#include <rex/graphics/gpu_stats.h>
#define F2_HAS_GPU_STATS 1
#else
#define F2_HAS_GPU_STATS 0
#endif

// [f2-menus] 2026-11-11 03:45 the settings of the mouse acceleration, the sensitivity randomizer and the sensitivity
// scales (f2_accel*, f2_sens_*) are in f2_accel.cpp with their code, which Call of Duty 3 builds too.

// [f2-menus] 2026-11-11 02:40 (owner 02:15: "also resolution options aren't on par with cod3"; 02:33: "keep going
// with settings from our cod3 menu and adding it to the other games."). Call of Duty 3's Render resolution list
// and its Display and Refresh rate rows, for the ports that share this window.
REXCVAR_DEFINE_STRING(f2_render_resolution, "", "f2/Display",
                      "The resolution to draw at, by name, as Call of Duty 3's list: auto (match the screen), 720p, "
                      "1080p, 1440p, 2160p, 4320p, original (the console's own size) or WIDTHxHEIGHT. It picks "
                      "resolution_scale (and game_draw_size where the game has it); \"\" leaves those as they are");
REXCVAR_DEFINE_DOUBLE(f2_refresh_rate, 0.0, "f2/Display",
                      "Fullscreen refresh rate in Hz (0 = the desktop's own, borderless fullscreen)")
    .range(0.0, 1000.0);

REXCVAR_DEFINE_BOOL(f2_frame_cost, false, "f2/Performance",
                    "Show what each frame costs in the top left corner: the frame rate and time, and the Xbox 360 GPU "
                    "emulation's draws, resolves, transfers and waits a frame");
REXCVAR_DEFINE_INT32(f2_frame_cost_log, 0, "f2/Performance",
                     "Write the frame cost line to the log every N seconds, averaged over them (0 = never)")
    .range(0, 3600);

REXCVAR_DEFINE_STRING(f2_game_show_crosshair, "", "f2/Game",
                      "The game's own crosshair (its cg_drawCrosshair): \"\" the game's own setting, 1 on, 0 off");
REXCVAR_DEFINE_STRING(f2_game_enemy_crosshair, "", "f2/Game",
                      "Red crosshair over an enemy (the game's cg_crosshairEnemyColor): \"\" the game's own, 1, 0");
REXCVAR_DEFINE_STRING(f2_game_subtitles, "", "f2/Game", "Subtitles (the game's cg_subtitles): \"\" the game's own, 1, 0");
REXCVAR_DEFINE_STRING(f2_game_blood, "", "f2/Game", "Blood (the game's cg_blood): \"\" the game's own, 1, 0");
REXCVAR_DEFINE_STRING(f2_game_aim_autoaim, "", "f2/Game",
                      "Aim assist, snap to a target when the sights go up (the game's aim_autoaim_enabled): \"\" the "
                      "game's own, 1, 0");
REXCVAR_DEFINE_STRING(f2_game_aim_lockon, "", "f2/Game",
                      "Aim assist, follow a moving target (the game's aim_lockon_enabled): \"\" the game's own, 1, 0");
REXCVAR_DEFINE_STRING(f2_game_aim_slowdown, "", "f2/Game",
                      "Aim assist, slow the turn over a target (the game's aim_slowdown_enabled): \"\" the game's own, "
                      "1, 0");
REXCVAR_DEFINE_STRING(f2_game_friendly_names, "", "f2/Game",
                      "Names over friendly soldiers (the game's cg_drawFriendlyNames): \"\" the game's own, 1, 0");
REXCVAR_DEFINE_STRING(f2_game_crosshair_names, "", "f2/Game",
                      "The name of who is under the crosshair (the game's cg_drawCrosshairNames): \"\" the game's own, "
                      "1, 0");

// [f2-menus] 2026-11-10 17:40 more of the game's own options (owner 17:16: "f2 menus", "keep going."): the weapon
// in view and the HUD on or off, the weapon's place on screen and the game crosshair's opacity. Run cheat1: Call of
// Duty 4's and World at War's consoles take all of them in a level started with `map` (cheats off).
REXCVAR_DEFINE_STRING(f2_game_draw_gun, "", "f2/Game",
                      "Your weapon in view (the game's cg_drawGun): \"\" the game's own, 1, 0");
REXCVAR_DEFINE_STRING(f2_game_draw_hud, "", "f2/Game",
                      "The HUD (the game's cg_drawHUD): \"\" the game's own, 1, 0");
// [f2-menus] 2026-11-10 20:30 rows the three titles' dvar lists have in common (dev/agents/f2-menus/FEATURES.md;
// owner 18:28: "getting the features aligned between tiltes"). Run t2022: Call of Duty 4's and World at War's
// consoles take every one of them in a level started with `map` (cheats off). 17 to 21 are IW3's (Call of Duty 2
// has none of them).
REXCVAR_DEFINE_STRING(f2_game_invert_pitch, "", "f2/Game",
                      "Invert the controller's look up / down (the game's input_invertPitch): \"\" the game's own, "
                      "1, 0");
REXCVAR_DEFINE_STRING(f2_game_rumble, "", "f2/Game",
                      "Controller rumble (the game's gpad_rumble): \"\" the game's own, 1, 0");
REXCVAR_DEFINE_STRING(f2_game_brass, "", "f2/Game",
                      "Shell casings thrown by weapons (the game's cg_brass): \"\" the game's own, 1, 0");
REXCVAR_DEFINE_STRING(f2_game_breath_hint, "", "f2/Game",
                      "The hold-breath hint in a scope (the game's cg_drawBreathHint): \"\" the game's own, 1, 0");
REXCVAR_DEFINE_STRING(f2_game_mantle_hint, "", "f2/Game",
                      "The climb-over hint (the game's cg_drawMantleHint): \"\" the game's own, 1, 0");
REXCVAR_DEFINE_STRING(f2_game_turret_crosshair, "", "f2/Game",
                      "The crosshair on a mounted gun (the game's cg_drawTurretCrosshair): \"\" the game's own, 1, 0");
REXCVAR_DEFINE_STRING(f2_game_sun_shadows, "", "f2/Game",
                      "Shadows cast by the sun (the game's sm_sunEnable): \"\" the game's own, 1, 0");
REXCVAR_DEFINE_STRING(f2_game_dof, "", "f2/Game",
                      "Depth of field (the game's r_dof_enable): \"\" the game's own, 1, 0");
REXCVAR_DEFINE_STRING(f2_game_glow, "", "f2/Game", "Glow (the game's r_glow_allowed): \"\" the game's own, 1, 0");
REXCVAR_DEFINE_STRING(f2_game_specular, "", "f2/Game",
                      "Specular highlights (the game's r_specular): \"\" the game's own, 1, 0");
REXCVAR_DEFINE_STRING(f2_game_distortion, "", "f2/Game",
                      "Heat and water distortion (the game's r_distortion): \"\" the game's own, 1, 0");
// [f2-menus] 2026-11-10 21:00 the game's own look numbers (FEATURES.md gaps 1 and 3). Run t2022: Call of Duty 4's
// and World at War's consoles take input_viewSensitivity, sensitivity and m_filter with cheats off.
REXCVAR_DEFINE_STRING(f2_game_mouse_filter, "", "f2/Game",
                      "Smooth mouse in the game's own mouse look (the game's m_filter): \"\" the game's own, 1, 0");
REXCVAR_DEFINE_BOOL(f2_stick_sens_own, false, "f2/Game",
                    "Use f2_stick_sens for the right stick's look speed (the game's input_viewSensitivity)");
REXCVAR_DEFINE_DOUBLE(f2_stick_sens, 1.0, "f2/Game",
                      "The right stick's look speed as a multiple of the game's own, with f2_stick_sens_own")
    .range(0.2, 5.0);
REXCVAR_DEFINE_BOOL(f2_mouse_sens_own, false, "f2/Game",
                    "Use f2_mouse_sens for the game's own mouse look (the game's sensitivity)");
REXCVAR_DEFINE_DOUBLE(f2_mouse_sens, 5.0, "f2/Game",
                      "The game's own mouse sensitivity (its default is 5), with f2_mouse_sens_own")
    .range(0.01, 100.0);  // [f2-menus] 03:20 wider than its slider: a sensitivity typed on another game's scale
// [f2-menus] 2026-11-10 22:30 the game's own gamma and brightness. Run g2214 (tools/f2_runs_gamma.sh): with cheats off
// a frame's mean brightness in Call of Duty 4's port went 58 -> 120 at r_gamma 2, 24 at 0.6 and 139 at r_brightness
// 0.3, and back to 58 each time the dvar was given back; World at War 88 -> 147, 49 and 168 (FEATURES.md, section 0).
// [f2-menus] 2026-11-11 07:35 Call of Duty 2's picture switches (its PC copy's Graphics menu; the 360 campaign has
// these dvars, found by asking the running game: dvars/COD2_SP_DVARS.md, last section; each one taken by the console
// in a level, run take0732).
REXCVAR_DEFINE_STRING(f2_game_model_shadows, "", "f2/Game",
                      "Model shadows (the game's sc_enable): \"\" the game's own, 1, 0");
REXCVAR_DEFINE_STRING(f2_game_soft_smoke, "", "f2/Game", "Soft smoke (the game's r_zfeather): \"\" the game's own, 1, 0");
REXCVAR_DEFINE_STRING(f2_game_cod2_glow, "", "f2/Game", "Glow (the game's r_glow): \"\" the game's own, 1, 0");
REXCVAR_DEFINE_STRING(f2_game_marks, "", "f2/Game",
                      "Bullet holes and scorch marks (the game's cg_marks): \"\" the game's own, 1, 0");
REXCVAR_DEFINE_BOOL(f2_dlights_own, false, "f2/Game",
                    "Use f2_dlights for the most dynamic lights at once (the game's r_dlightLimit)");
REXCVAR_DEFINE_DOUBLE(f2_dlights, 4.0, "f2/Game", "The most dynamic lights at once (0 to 4), with f2_dlights_own")
    .range(0.0, 4.0);
// [f2-menus] 2026-11-11 07:52 the PC copy's "Number of corpses" (ai_corpseCount: int, the 360 campaign's default 5,
// 0 to 64; taken by the console in a level, run take0750: 5 -> 12). Multiplayer's list has no ai_corpseCount.
REXCVAR_DEFINE_BOOL(f2_corpses_own, false, "f2/Game",
                    "Use f2_corpses for the most bodies left lying at once (the game's ai_corpseCount)");
REXCVAR_DEFINE_DOUBLE(f2_corpses, 5.0, "f2/Game", "The most bodies left lying at once (0 to 64), with f2_corpses_own")
    .range(0.0, 64.0);
REXCVAR_DEFINE_BOOL(f2_gamma_own, false, "f2/Game", "Use f2_gamma for the picture's gamma (the game's r_gamma)");
REXCVAR_DEFINE_DOUBLE(f2_gamma, 1.0, "f2/Game",
                      "The game's gamma (its own is 1; larger is brighter), with f2_gamma_own")
    .range(0.5, 3.0);
REXCVAR_DEFINE_BOOL(f2_brightness_own, false, "f2/Game",
                    "Use f2_brightness for the picture's brightness (the game's r_brightness)");
REXCVAR_DEFINE_DOUBLE(f2_brightness, 0.0, "f2/Game",
                      "Brightness added to the game's picture (its own is 0; the game takes -1 to 1), with "
                      "f2_brightness_own")
    .range(-0.5, 0.5);
REXCVAR_DEFINE_BOOL(f2_gun_own, false, "f2/Game",
                    "Use f2_gun_x / _y / _z for the weapon's place on screen (the game's cg_gun_x / _y / _z)");
REXCVAR_DEFINE_DOUBLE(f2_gun_x, 0.0, "f2/Game", "The weapon forward (+) or back (-), with f2_gun_own")
    .range(-10.0, 10.0);
REXCVAR_DEFINE_DOUBLE(f2_gun_y, 0.0, "f2/Game", "The weapon left (+) or right (-), with f2_gun_own")
    .range(-10.0, 10.0);
REXCVAR_DEFINE_DOUBLE(f2_gun_z, 0.0, "f2/Game", "The weapon up (+) or down (-), with f2_gun_own")
    .range(-10.0, 10.0);
REXCVAR_DEFINE_BOOL(f2_crosshair_alpha_own, false, "f2/Game",
                    "Use f2_crosshair_alpha for the game crosshair's opacity (the game's cg_crosshairAlpha)");
REXCVAR_DEFINE_DOUBLE(f2_crosshair_alpha, 1.0, "f2/Game",
                      "The game crosshair's opacity, 0 to 1, with f2_crosshair_alpha_own")
    .range(0.0, 1.0);

REXCVAR_DEFINE_BOOL(f2_crosshair, false, "f2/Crosshair", "Replace the game's hip-fire crosshair with a customizable one");
REXCVAR_DEFINE_INT32(f2_crosshair_style, 0, "f2/Crosshair",
                     "Custom crosshair shape: 0 cross, 1 cross with dot, 2 dot, 3 circle, 4 T (no top line)")
    .range(0, 4);
REXCVAR_DEFINE_DOUBLE(f2_crosshair_size, 6.0, "f2/Crosshair", "Custom crosshair arm length (px at 1080 lines)")
    .range(0.0, 60.0);
REXCVAR_DEFINE_DOUBLE(f2_crosshair_thickness, 2.0, "f2/Crosshair", "Custom crosshair line thickness (px at 1080 lines)")
    .range(1.0, 12.0);
REXCVAR_DEFINE_DOUBLE(f2_crosshair_gap, 4.0, "f2/Crosshair", "Custom crosshair gap from the center (px at 1080 lines)")
    .range(0.0, 40.0);
REXCVAR_DEFINE_BOOL(f2_crosshair_outline, true, "f2/Crosshair", "Custom crosshair: black outline");
REXCVAR_DEFINE_DOUBLE(f2_crosshair_outline_thickness, 1.0, "f2/Crosshair",
                      "Custom crosshair outline thickness (px at 1080 lines)")
    .range(0.5, 4.0);
REXCVAR_DEFINE_BOOL(f2_crosshair_dot, false, "f2/Crosshair", "Custom crosshair: center dot");
REXCVAR_DEFINE_BOOL(f2_crosshair_constant_color, false, "f2/Crosshair",
                    "Always draw the custom crosshair in f2_crosshair_color. Off: the game's own color for the "
                    "moment (white, red over an enemy, green over a friendly)");
REXCVAR_DEFINE_STRING(f2_crosshair_color, "0 255 0 255", "f2/Crosshair",
                      "Custom crosshair color when constant: \"red green blue alpha\", 0-255 each");
REXCVAR_DEFINE_INT32(f2_crosshair_dynamic, 1, "f2/Crosshair",
                     "How the custom crosshair moves: 0 static (CS2's Classic Static), 1 dynamic: the gap follows the "
                     "game's own spread (as the game's crosshair and CS2's Classic Dynamic), 2 dynamic split (CS2's "
                     "Classic: past the split distance the inner part of each line stays and the outer part goes on)")
    .range(0, 2);
REXCVAR_DEFINE_DOUBLE(f2_crosshair_dynamic_scale, 1.0, "f2/Crosshair",
                      "Dynamic custom crosshair: how far it opens (1 = exactly the game's spread)")
    .range(0.0, 3.0);
REXCVAR_DEFINE_DOUBLE(f2_crosshair_split_distance, 7.0, "f2/Crosshair",
                      "Dynamic split: the spread (px at 1080 lines) past which each line splits "
                      "(CS2's cl_crosshair_dynamic_splitdist)")
    .range(0.0, 40.0);
REXCVAR_DEFINE_DOUBLE(f2_crosshair_split_ratio, 0.35, "f2/Crosshair",
                      "Dynamic split: the share of each line that goes on outward "
                      "(CS2's cl_crosshair_dynamic_maxdist_splitratio)")
    .range(0.0, 1.0);
REXCVAR_DEFINE_DOUBLE(f2_crosshair_split_alpha_inner, 1.0, "f2/Crosshair",
                      "Dynamic split: opacity of the inner part (CS2's cl_crosshair_dynamic_splitalpha_innermod)")
    .range(0.0, 1.0);
REXCVAR_DEFINE_DOUBLE(f2_crosshair_split_alpha_outer, 0.5, "f2/Crosshair",
                      "Dynamic split: opacity of the outer part (CS2's cl_crosshair_dynamic_splitalpha_outermod)")
    .range(0.3, 1.0);
REXCVAR_DEFINE_BOOL(f2_crosshair_own_colors, false, "f2/Crosshair",
                    "Use f2_crosshair_color_normal / _enemy / _friendly in place of the game's white, red and green "
                    "(the crosshair still changes when the game's does)");
REXCVAR_DEFINE_STRING(f2_crosshair_color_normal, "255 255 255 255", "f2/Crosshair",
                      "Own crosshair color over nothing: \"red green blue alpha\", 0-255 each");
REXCVAR_DEFINE_STRING(f2_crosshair_color_enemy, "255 0 0 255", "f2/Crosshair",
                      "Own crosshair color over an enemy: \"red green blue alpha\", 0-255 each");
REXCVAR_DEFINE_STRING(f2_crosshair_color_friendly, "64 255 64 255", "f2/Crosshair",
                      "Own crosshair color over a friendly: \"red green blue alpha\", 0-255 each");
REXCVAR_DEFINE_BOOL(f2_debug_crosshair, false, "f2",
                    "Debug: log once a second where the custom crosshair is drawn, its color and alpha (tests)");

namespace f2 {

// ------------------------------------------------------------------------------------------------- commands
void CommandQueue::Push(std::string command) {
  std::lock_guard<std::mutex> lock(mutex_);
  commands_.push_back(std::move(command));
  has_.store(true, std::memory_order_release);
}

void CommandQueue::Take(std::vector<std::string>& out) {
  std::lock_guard<std::mutex> lock(mutex_);
  out.swap(commands_);
  commands_.clear();
  has_.store(false, std::memory_order_release);
}

// ------------------------------------------------------------------------------------------------- display
// [f2-menus] 2026-11-11 02:40 The render resolution by name. The emulated GPU can only draw a whole number of times
// the game's own 1280 x 720 (resolution_scale), so a name picks the smallest scale that reaches it and the
// presenter fits that picture to the window: 1080p is drawn as 2560 x 1440 and shown at the window's size. (Call of
// Duty 3 goes further and has the game itself draw 1920 x 1080; the IW games' video memory layout doesn't allow
// that yet: dev/agents/f2-menus/FEATURES.md section 7.) "original" is the console's own: scale 1, and in Call of
// Duty 4 and World at War the 3D scene at the console's 1024 x 600 (game_draw_size).
namespace display {
namespace {

rex::ui::Window* g_window = nullptr;
bool g_first_tick = true;
std::string g_applied = "\x01";  // the f2_render_resolution last applied (no setting has this value)
std::string g_note;              // what the last choice came to, for the row

struct NamedSize {
  const char* value;  // f2_render_resolution
  const char* name;   // the row; Call of Duty 3's names and order (game/render_resolution.h kResolutionChoices)
  uint32_t width, height;
};
constexpr NamedSize kSizes[] = {
    {"auto", "Match the screen", 0, 0},
    {"720p", "720p (1280x720)", 1280, 720},
    {"1080p", "1080p (1920x1080)", 1920, 1080},
    {"1440p", "1440p (2560x1440)", 2560, 1440},
    {"2160p", "2160p / 4K (3840x2160)", 3840, 2160},
    {"4320p", "4320p / 8K (7680x4320)", 7680, 4320},
    {"original", "Xbox 360 original (the console's size, stretched)", 0, 0},
};
constexpr uint32_t kGameWidth = 1280, kGameHeight = 720;
constexpr uint32_t kMaxScale = 6;  // the Render scale row's largest (7680 x 4320)

// A name from the list, "4k" / "8k", or "WIDTHxHEIGHT" within Call of Duty 3's custom limits (640 x 360 to
// 15360 x 4320); false for auto, original, "" and anything else.
bool ParseSize(std::string choice, uint32_t* width, uint32_t* height) {
  choice.erase(std::remove(choice.begin(), choice.end(), ' '), choice.end());
  for (char& ch : choice)
    if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
  if (choice == "4k") choice = "2160p";
  if (choice == "8k") choice = "4320p";
  for (const NamedSize& s : kSizes) {
    if (s.width && choice == s.value) {
      *width = s.width;
      *height = s.height;
      return true;
    }
  }
  unsigned w = 0, h = 0;
  char x = 0, rest = 0;
  if (std::sscanf(choice.c_str(), "%u%c%u%c", &w, &x, &h, &rest) != 3 || x != 'x') return false;
  if (w < 640 || h < 360 || w > 15360 || h > 4320) return false;
  *width = w;
  *height = h;
  return true;
}

// The screen the game is on, in pixels; the main screen when the window can't say.
bool ScreenSize(uint32_t* width, uint32_t* height) {
  if (g_window && g_window->GetCurrentDisplaySize(width, height) && *width && *height) return true;
  for (const rex::ui::Window::DisplayInfo& d : rex::ui::Window::ListDisplays()) {
    if (d.primary && d.width && d.height) {
      *width = d.width;
      *height = d.height;
      return true;
    }
  }
  return false;
}

// Sets resolution_scale (and game_draw_size) for `choice`. True when a setting changed (it applies after a restart).
bool Apply(const std::string& choice) {
  g_note.clear();
  if (choice.empty() || !Has("resolution_scale")) return false;
  const bool original = choice == "original";
  uint32_t w = kGameWidth, h = kGameHeight, scale = 1;
  std::string why;
  if (choice == "auto") {
    if (!ScreenSize(&w, &h)) {
      w = kGameWidth;
      h = kGameHeight;
      why = "; the screen's size isn't known, so 1x";
    }
  } else if (!original && !ParseSize(choice, &w, &h)) {
    g_note = "\"" + choice + "\" isn't a resolution (a name from the list, or WIDTHxHEIGHT)";
    REXLOG_WARN("f2: render resolution: {}", g_note);
    return false;
  }
  if (!original) {
    scale = std::max((w + kGameWidth - 1) / kGameWidth, (h + kGameHeight - 1) / kGameHeight);
    if (scale > kMaxScale) why += "; more than the largest scale, so " + std::to_string(kMaxScale) + "x";
    scale = std::clamp(scale, 1u, kMaxScale);
  }
  bool changed = false;
  if (GetInt("resolution_scale", 1) != static_cast<int>(scale)) {
    rex::cvar::SetFlagByName("resolution_scale", std::to_string(scale));
    changed = true;
  }
  for (const char* axis : {"draw_resolution_scale_x", "draw_resolution_scale_y"}) {  // an axis of its own wins
    if (Has(axis) && GetInt(axis, 1) != 1) {
      rex::cvar::SetFlagByName(axis, "1");
      changed = true;
    }
  }
  if (Has("game_draw_size")) {
    const char* size = original ? "console" : "screen";
    if (Get("game_draw_size") != size) {
      rex::cvar::SetFlagByName("game_draw_size", size);
      changed = true;
    }
  }
  char text[200];
  if (original) {
    std::snprintf(text, sizeof(text), "the console's own size, drawn once (1x) and stretched to the window");
  } else if (w == kGameWidth * scale && h == kGameHeight * scale) {
    std::snprintf(text, sizeof(text), "%u x %u, drawn at exactly that size (%ux the game's 1280 x 720)", w, h, scale);
  } else {
    std::snprintf(text, sizeof(text), "%u x %u is drawn as %u x %u (%ux the game's 1280 x 720) and fitted to the window",
                  w, h, kGameWidth * scale, kGameHeight * scale, scale);
  }
  g_note = std::string(text) + why;
  REXLOG_INFO("f2: render resolution \"{}\": {}{}", choice, g_note, changed ? " (a setting changed: after a restart)" : "");
  return changed;
}

// A pick in the window, or the Render scale row moved by hand ("" = custom).
bool Pick(const std::string& choice) {
  rex::cvar::SetFlagByName("f2_render_resolution", choice);
  g_applied = choice;
  return Apply(choice);
}

}  // namespace

void SetGameWindow(rex::ui::Window* window) {
  g_window = window;
  const double hz = REXCVAR_GET(f2_refresh_rate);
  if (window && hz > 0.0) {
    window->SetFullscreenRefreshRate(hz);
    REXLOG_INFO("f2: fullscreen refresh rate {} Hz (f2_refresh_rate)", hz);
  }
}

bool Tick() {
  const bool first = g_first_tick;
  g_first_tick = false;
  const std::string choice = REXCVAR_GET(f2_render_resolution);
  if (choice == g_applied) return false;
  g_applied = choice;
  // "Match the screen" follows the screen the game starts on: when that changed the scale, the settings are saved
  // so the next start draws at it. Any other change waits for the window's Save, as every row does.
  return Apply(choice) && first && choice == "auto";
}

}  // namespace display

void DrawDisplayRows(PageContext& c) {
  using Window = rex::ui::Window;
  Window* window = display::g_window;
  if (Has("monitor") && Has("monitor_name")) {
    const std::vector<Window::DisplayInfo> displays = Window::ListDisplays();
    Window::DisplayChoice choice;
    if (window) {
      choice = window->GetDisplayChoice();
    } else {
      choice.name = Get("monitor_name");
      choice.index = GetInt("monitor", 0);
    }
    auto label = [&](size_t i) {
      const Window::DisplayInfo& d = displays[i];
      char text[192];
      std::snprintf(text, sizeof(text), "%zu: %s (%ux%u, %.0f Hz%s)", i + 1, d.name.c_str(), d.width, d.height,
                    d.refresh_rate, d.primary ? ", main screen" : "");
      return std::string(text);
    };
    const int32_t chosen = Window::FindDisplay(choice, displays);
    std::string preview = "Automatic";
    if (chosen > 0 && static_cast<size_t>(chosen) <= displays.size())
      preview = label(static_cast<size_t>(chosen - 1));
    else if (!choice.IsAutomatic())
      preview = (choice.name.empty() ? "Screen " + std::to_string(choice.index) : choice.name) + " (not connected)";
    auto pick = [&](const Window::DisplayChoice& picked) {
      rex::cvar::SetFlagByName("monitor_name", picked.name);
      rex::cvar::SetFlagByName("monitor", std::to_string(picked.index));
      if (window) window->SetDisplayChoice(picked);
      REXLOG_INFO("f2: display \"{}\" ({})", picked.name, picked.index);
      c.dirty = true;
      c.save_now = true;  // at once, as Call of Duty 3's
    };
    if (ImGui::BeginCombo("Display", preview.c_str())) {
      if (ImGui::Selectable("Automatic (the system's choice)", choice.IsAutomatic())) pick({});
      for (size_t i = 0; i < displays.size(); ++i) {
        if (ImGui::Selectable(label(i).c_str(), chosen == static_cast<int32_t>(i + 1)))
          pick({displays[i].name, static_cast<int32_t>(i + 1)});
      }
      ImGui::EndCombo();
    }
    if (window) {
      const int32_t now = window->GetCurrentDisplay();
      if (now > 0 && static_cast<size_t>(now) <= displays.size())
        ImGui::TextDisabled("The game is on %s.", label(static_cast<size_t>(now - 1)).c_str());
    }
    Help("Which screen the game plays on. It's kept by the screen's name, so it stays the same when other screens "
         "are plugged in or out; if that screen isn't connected, the system picks one. Applies at once (fullscreen "
         "moves along) and is kept for the next start.");
  }
  {
    const double cur = REXCVAR_GET(f2_refresh_rate);
    std::vector<double> rates;
    if (window) rates = window->GetDisplayRefreshRates();
    {
      static bool logged = false;  // once, for test runs: what the SDK's window reports here
      if (!logged) {
        logged = true;
        REXLOG_INFO("f2: display rows: {} display(s), {} refresh rate(s), the monitor at {} Hz, the game on display {}",
                    rex::ui::Window::ListDisplays().size(), rates.size(),
                    window ? window->GetDisplayRefreshRate() : 0.0, window ? window->GetCurrentDisplay() : 0);
      }
    }
    auto label = [](double hz) {
      char text[32];
      if (std::abs(hz - std::round(hz)) < 0.005)
        std::snprintf(text, sizeof(text), "%.0f Hz", hz);
      else
        std::snprintf(text, sizeof(text), "%.2f Hz", hz);
      return std::string(text);
    };
    auto pick = [&](double hz) {
      char text[32];
      std::snprintf(text, sizeof(text), "%.3f", hz);
      rex::cvar::SetFlagByName("f2_refresh_rate", text);
      if (window) window->SetFullscreenRefreshRate(hz);
      REXLOG_INFO("f2: fullscreen refresh rate {} Hz", hz);
      c.dirty = true;
    };
    const std::string preview = cur <= 0.0 ? std::string("Desktop's own (borderless)") : label(cur);
    if (ImGui::BeginCombo("Refresh rate (fullscreen)", preview.c_str())) {
      if (ImGui::Selectable("Desktop's own (borderless)", cur <= 0.0) && cur > 0.0) pick(0.0);
      for (double hz : rates) {
        const bool current = std::abs(hz - cur) < 0.01;
        if (ImGui::Selectable(label(hz).c_str(), current) && !current) pick(hz);
      }
      ImGui::EndCombo();
    }
    if (window) {
      const double now = window->GetDisplayRefreshRate();
      if (now > 0.0) ImGui::TextDisabled("The monitor is running at %s.", label(now).c_str());
    }
    Help("How many times a second the monitor refreshes in fullscreen. Pick your monitor's highest rate to see every "
         "frame the game draws (with V-Sync on, the game shows at most this many frames a second). \"Desktop's own\" "
         "keeps whatever the desktop uses, in borderless fullscreen. Applies at once when fullscreen; windowed play "
         "always uses the desktop's rate.");
  }
}

// ------------------------------------------------------------------------------------------------- resolution
// [f2-menus] 2026-11-11 04:12 the render resolution by name and the refresh rate are reset with the rest (the page's
// Reset left them standing).
const char* const kResolutionSettings[8] = {"f2_render_resolution", "f2_refresh_rate",
                                            "resolution_scale", "draw_resolution_scale_x", "draw_resolution_scale_y",
                                            "present_letterbox", "window_width",           "window_height"};

void DrawResolutionSection(PageContext& c) {
  Section("Resolution");
  if (Has("resolution_scale")) {
    // [f2-menus] 2026-11-11 02:40 Call of Duty 3's Render resolution row: the same names in the same order, and a
    // size typed as WIDTHxHEIGHT. Each picks the Render scale below (display::Apply).
    const std::string cur = REXCVAR_GET(f2_render_resolution);
    std::string preview = cur.empty() ? "Custom (the render scale below)" : cur;
    for (const display::NamedSize& s : display::kSizes)
      if (cur == s.value) preview = s.name;
    auto pick = [&](const std::string& value) {
      if (display::Pick(value)) c.restart = true;
      c.dirty = true;
    };
    if (ImGui::BeginCombo("Render resolution", preview.c_str())) {
      for (const display::NamedSize& s : display::kSizes)
        if (ImGui::Selectable(s.name, cur == s.value) && cur != s.value) pick(s.value);
      if (ImGui::Selectable("Custom (the render scale below)", cur.empty()) && !cur.empty()) pick("");
      ImGui::EndCombo();
    }
    static char typed[24] = "";
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
    const bool entered = ImGui::InputTextWithHint("##f2_typed_resolution", "1920x1080", typed, sizeof(typed),
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if ((ImGui::Button("Use this size") || entered) && typed[0]) {
      uint32_t w = 0, h = 0;
      if (display::ParseSize(typed, &w, &h)) {
        pick(std::to_string(w) + "x" + std::to_string(h));
        typed[0] = 0;
      } else {
        display::g_note = std::string("\"") + typed + "\" isn't a size: type WIDTHxHEIGHT, 640x360 to 15360x4320";
      }
    }
    if (!display::g_note.empty()) ImGui::TextDisabled("%s", display::g_note.c_str());
    Help("The resolution the game is drawn at, by name. The Xbox 360 GPU emulation draws a whole number of times "
         "the game's own 1280 x 720, so a name picks the smallest multiple that reaches it and that picture is "
         "fitted to the window: 1440p, 4K and 8K are drawn at exactly their size, 1080p as 2560 x 1440 made smaller "
         "(smoother edges, slower than a true 1080p would be). \"Match the screen\" does this for the monitor the "
         "game starts on. \"Xbox 360 original\" is what the console drew. Applies after a restart.");
  }
  {
    // The SDK takes resolution_scale for both axes unless an axis has its own (draw_resolution_scale_x/_y), so a
    // choice here clears those.
    // [f2-menus] 2026-11-10 (owner 16:08: "We also need to get native 8k rendering theoretically working."): 5x and
    // 6x. The games draw a 1280 x 720 screen, so 6x is 7680 x 4320; the SDK takes up to 7 an axis. Measured on the
    // owner's 10 GB card (f2-menus/cod2k8a, k8iw3c, k8iw3e): Call of Duty 2 hands over a 7680 x 4320 picture at 6x
    // with no errors; Call of Duty 4 and World at War draw 4x (5120 x 2880) cleanly, and above that the card's
    // memory is full (8967 of 10240 MB in use with Call of Duty 4 at 4x) and the GPU emulation's draws fail.
    const char* v[] = {"1", "2", "3", "4", "5", "6"};
    const char* n[] = {"1x (the console's size, 1280 x 720)", "2x (2560 x 1440)", "3x (3840 x 2160, 4K)",
                       "4x (5120 x 2880, 5K)",                "5x (6400 x 3600)", "6x (7680 x 4320, 8K)"};
    if (Choice("Render scale", "resolution_scale", v, n, 6, Applies::kAfterRestart, &c.restart)) {
      rex::cvar::SetFlagByName("draw_resolution_scale_x", "1");
      rex::cvar::SetFlagByName("draw_resolution_scale_y", "1");
      display::Pick("");  // [f2-menus] moved by hand: the Render resolution row shows Custom
      c.dirty = true;
    }
    Help("Everything the game draws, drawn this many times larger in each direction: sharper edges, text and distant "
         "detail at the same frame rate on a fast graphics card. Each step costs the square in graphics memory and "
         "time (4x is 16 times the pixels, 6x is 36 times). 5x and 6x need a graphics card with a lot of memory: "
         "when it runs out the picture stops drawing properly, and a lower scale brings it back. Applies after a "
         "restart.");
  }
  {
    const char* v[] = {"true", "false"};
    const char* n[] = {"Keep the game's shape (black bars)", "Stretch to fill the window"};
    c.dirty |= Choice("Picture shape", "present_letterbox", v, n, 2, Applies::kAtOnce, &c.restart);
    Help("When the window isn't the game's shape (16:9): black bars at the sides or top, or the picture stretched to "
         "the window's shape.");
  }
  if (Has("window_width") && Has("window_height")) {
    static const struct {
      const char* w;
      const char* h;
      const char* name;
    } kSizes[] = {{"0", "0", "The game's default"}, {"1280", "720", "1280 x 720"},  {"1600", "900", "1600 x 900"},
                  {"1920", "1080", "1920 x 1080"},   {"2560", "1440", "2560 x 1440"}, {"3840", "2160", "3840 x 2160"},
                  {"5120", "2880", "5120 x 2880"},   {"7680", "4320", "7680 x 4320 (8K)"}};
    constexpr int kSizeCount = int(sizeof(kSizes) / sizeof(kSizes[0]));
    const std::string w = Get("window_width"), h = Get("window_height");
    int current = -1;
    for (int i = 0; i < kSizeCount; ++i)
      if (w == kSizes[i].w && h == kSizes[i].h) current = i;
    const std::string preview = current >= 0 ? kSizes[current].name : w + " x " + h;
    if (ImGui::BeginCombo("Window size", preview.c_str())) {
      for (int i = 0; i < kSizeCount; ++i) {
        if (ImGui::Selectable(kSizes[i].name, i == current) && i != current) {
          rex::cvar::SetFlagByName("window_width", kSizes[i].w);
          rex::cvar::SetFlagByName("window_height", kSizes[i].h);
          c.dirty = true;
          c.restart = true;
        }
      }
      ImGui::EndCombo();
    }
    Help("The window's size when the game starts (full screen fills the monitor whatever this is). Applies after a "
         "restart.");
  }
}

// ------------------------------------------------------------------------------------------------- picture
const char* const kPictureSettings[5] = {"swap_post_effect", "present_effect", "present_fsr_sharpness_reduction",
                                         "present_cas_additional_sharpness", "anisotropic_override"};

namespace {

// A 0-100% slider over a setting kept as another number (to_setting turns the percent into the setting's text).
bool PercentSlider(const char* label, const char* name, float percent, double (*to_setting)(double)) {
  if (!ImGui::SliderFloat(label, &percent, 0.0f, 100.0f, "%.0f%%")) return false;
  char text[32];
  std::snprintf(text, sizeof text, "%.3f", to_setting(std::clamp(percent, 0.0f, 100.0f)));
  rex::cvar::SetFlagByName(name, text);
  return true;
}

}  // namespace

void DrawPictureSection(PageContext& c) {
  if (Has("swap_post_effect")) {
    const char* v[] = {"none", "fxaa", "fxaa_extreme"};
    const char* n[] = {"Off", "FXAA", "FXAA (extreme)"};
    c.dirty |= Choice("Post-process AA", "swap_post_effect", v, n, 3, Applies::kAfterRestart, &c.restart);
    Help("Smooths jagged edges after the picture is drawn. Costs very little; Extreme smooths more and softens the "
         "picture a bit more. Applies after a restart.");
  }
  if (Has("present_effect")) {
    const char* v[] = {"bilinear", "cas", "fsr"};
    const char* n[] = {"Bilinear", "AMD CAS (sharpen)", "AMD FSR 1 (spatial)"};
    c.dirty |= Choice("Output scaling", "present_effect", v, n, 3, Applies::kAfterRestart, &c.restart);
    Help("How the finished picture is fitted to the window. Bilinear is plain and soft. AMD CAS sharpens it. AMD FSR "
         "1 upscales with sharper edges when the game draws fewer pixels than the window shows; at the window's own "
         "size it sharpens like CAS. Applies after a restart.");
    // The sharpness as a slider, as Call of Duty 3's: FSR 1's RCAS takes a reduction in stops (0 = sharpest, 2 =
    // softest), CAS an extra sharpness (0-1); both shown as 0-100%. The SDK reads them when the window's output starts.
    const std::string effect = Get("present_effect");
    if (effect == "fsr" && Has("present_fsr_sharpness_reduction")) {
      const float percent = static_cast<float>((1.0 - GetDouble("present_fsr_sharpness_reduction", 0.2) / 2.0) * 100.0);
      if (PercentSlider("FSR sharpness", "present_fsr_sharpness_reduction", percent,
                        [](double p) { return (1.0 - p / 100.0) * 2.0; }))
        c.dirty = c.restart = true;
      Help("How much FSR 1 sharpens the picture: 100% the most, 0% none. 90% is AMD's default. Applies after a "
           "restart.");
    } else if (effect == "cas" && Has("present_cas_additional_sharpness")) {
      const float percent = static_cast<float>(GetDouble("present_cas_additional_sharpness", 0.0) * 100.0);
      if (PercentSlider("CAS sharpness", "present_cas_additional_sharpness", percent,
                        [](double p) { return p / 100.0; }))
        c.dirty = c.restart = true;
      Help("Extra sharpening on top of AMD CAS's own: 0% its default, 100% the most. Applies after a restart.");
    } else if (Has("present_fsr_sharpness_reduction") || Has("present_cas_additional_sharpness")) {
      ImGui::TextDisabled("Sharpness: pick AMD CAS or AMD FSR 1 above.");
    }
  }
  if (Has("anisotropic_override")) {
    const char* v[] = {"-1", "0", "1", "2", "3", "4", "5"};
    const char* n[] = {"Game default", "Off", "1x", "2x", "4x", "8x", "16x"};
    c.dirty |= Choice("Anisotropic filtering", "anisotropic_override", v, n, 7, Applies::kAtOnce, &c.restart);
    Help("Keeps floors, roads and walls sharp when you look along them at a distance. 16x costs almost nothing on a "
         "modern GPU.");
  }
}

// ------------------------------------------------------------------------------------------------- EMULATION
namespace {

struct EmulationCheck {
  const char* name;
  const char* label;
  const char* help;
};

// Call of Duty 3's wording (recomp/src/ui/hud_overlay.cpp, EMULATION), for the settings that are the SDK's.
const EmulationCheck kReadbackChecks[] = {
    {"readback_resolve_cpu_reads_only", "Only the results the game reads",
     "Copies back only the results the game's own code reads, not every one. Off copies them all, as the emulator "
     "first did (slower, the same picture)."},
    {"readback_resolve_cpu_pages_only", "Only the parts the game reads",
     "Of those, copies only the pieces of memory the game reads. Off copies each result whole."},
    {"readback_resolve_full_deferred", "Full readback without waiting",
     "With Full, the GPU emulation doesn't stop at each copy to wait for the graphics card. Off waits there, as "
     "before (slower)."},
};
const EmulationCheck kSpeedupChecks[] = {
    {"vulkan_reuse_draw_state", "Reuse the last draw's state when nothing changed",
     "A draw that changes no setting reuses everything the previous draw set up, instead of setting it all up "
     "again."},
    {"vulkan_skip_unchanged_fetch_constants", "Skip slots the game rewrote unchanged",
     "When the game writes a texture or geometry slot with the value it already had, nothing is looked up again."},
    {"vulkan_sampler_cache", "Cache texture samplers",
     "Keeps each texture filter setting once it's worked out, instead of working it out for every draw."},
    {"vulkan_reuse_texture_descriptors", "Reuse texture sets",
     "A draw that uses the same textures as an earlier one in the frame reuses that one's set."},
    {"vulkan_dynamic_constant_offsets", "Draw constants without copying",
     "Points the graphics card at each draw's constants with an offset instead of setting up a new binding for "
     "every draw."},
    {"vulkan_resolve_into_textures", "Copy resolved pictures straight into textures",
     "When the game turns a finished picture into a texture, it's copied there directly instead of through the "
     "emulated memory."},
    {"vulkan_vertex_refetch", "Re-check rewritten geometry",
     "Sends geometry to the graphics card again when the game rewrote it in place, even at the same address."},
    {"vulkan_texture_memo", "Remember each draw's textures",
     "When a draw's textures haven't changed since the last draw, the same textures and filters are used without "
     "looking them up again."},
};
const EmulationCheck kSoundChecks[] = {
    {"xma_restart_without_reopen", "Reuse the sound decoder on a restart",
     "When a sound starts again, the emulated sound chip's decoder is reset instead of built again. Much less work "
     "for the sound thread."},
    {"audio_pace_callbacks", "Steady sound timing",
     "Hands sound to your speakers in evenly spaced pieces instead of bursts. Off can make sounds repeat."},
};
// Every SDK setting the page shows, for its Reset.
const char* const kEmulationSettings[] = {
    "render_target_path_vulkan", "render_target_path_d3d12", "native_2x_msaa", "readback_resolve",
    "readback_resolve_cpu_reads_only", "readback_resolve_cpu_pages_only", "readback_resolve_full_deferred",
    "clear_memory_page_state", "vulkan_reuse_draw_state", "vulkan_skip_unchanged_fetch_constants",
    "vulkan_sampler_cache", "vulkan_reuse_texture_descriptors", "vulkan_dynamic_constant_offsets",
    "vulkan_resolve_into_textures", "vulkan_vertex_refetch", "vulkan_texture_memo",
    "vulkan_pipeline_creation_threads", "d3d12_pipeline_creation_threads", "async_shader_compilation",
    "store_shaders", "xma_restart_without_reopen", "audio_pace_callbacks",
};

template <size_t N>
bool AnyOf(const EmulationCheck (&checks)[N]) {
  for (const EmulationCheck& k : checks)
    if (Has(k.name)) return true;
  return false;
}

template <size_t N>
void DrawChecks(PageContext& c, const EmulationCheck (&checks)[N]) {
  for (const EmulationCheck& k : checks) {
    if (!Has(k.name)) continue;
    c.dirty |= Check(k.label, k.name, Applies::kAfterRestart, &c.restart);
    Help(k.help);
  }
}

}  // namespace

void DrawEmulationPage(PageContext& c, const std::function<void(PageContext&)>& game_rows,
                       const std::vector<const char*>& game_settings) {
  if (ResetButton("reset_emulation", "emulation settings")) {
    for (const char* name : kEmulationSettings)
      if (Has(name)) c.restart |= ResetSettings({name});
    for (const char* name : game_settings) c.restart |= ResetSettings({name});
    c.dirty = true;
  }
  Help("How the Xbox 360's hardware is emulated. The defaults are the tested ones: change a row to compare, or when "
       "something in the picture or the sound is wrong. Most apply after a restart.");
  {
    static bool logged = false;  // once, for test runs: which of the SDK's settings this build has
    if (!logged) {
      logged = true;
      std::string missing;
      int have = 0, all = 0;
      for (const char* name : kEmulationSettings) {
        ++all;
        if (Has(name)) ++have;
        else missing += std::string(" ") + name;
      }
      REXLOG_INFO("f2: emulation page: {} of {} SDK settings in this build{}{}", have, all,
                  missing.empty() ? "" : "; missing:", missing);
    }
  }
  if (game_rows) game_rows(c);

  if (Has("render_target_path_vulkan") || Has("render_target_path_d3d12") || Has("native_2x_msaa")) {
    Section("Video memory");
    if (Has("render_target_path_vulkan")) {
      const char* v[] = {"", "fbo", "fsi"};
      const char* n[] = {"Automatic (host render targets)", "Host render targets",
                         "Pixel shader interlock (experimental)"};
      c.dirty |= Choice("Render target emulation (Vulkan)", "render_target_path_vulkan", v, n, 3,
                        Applies::kAfterRestart, &c.restart);
    }
    if (Has("render_target_path_d3d12")) {
      const char* v[] = {"", "rtv", "rov"};
      const char* n[] = {"Automatic", "Host render targets", "Rasterizer-ordered views"};
      c.dirty |= Choice("Render target emulation (Direct3D 12)", "render_target_path_d3d12", v, n, 3,
                        Applies::kAfterRestart, &c.restart);
    }
    if (Has("render_target_path_vulkan") || Has("render_target_path_d3d12"))
      Help("How the Xbox 360's own render memory is emulated. Host render targets draw with the graphics card's own "
           "blending, but copy pictures between render targets whenever the game reuses its video memory. Pixel "
           "shader interlock emulates that memory exactly and never copies, but every pixel does more work. Applies "
           "after a restart.");
    if (Has("native_2x_msaa")) {
      c.dirty |= Check("Draw the game's 2x MSAA as real 2x (faster)", "native_2x_msaa", Applies::kAfterRestart,
                       &c.restart);
      Help("On: your GPU's 2x MSAA. Off: 4x MSAA keeping two of the samples, which places them exactly like the "
           "Xbox 360 but is about twice the work for the GPU. Applies after a restart.");
    }
  }

  if (Has("readback_resolve")) {
    Section("Resolve readback");
    const char* v[] = {"none", "fast", "some", "full"};
    const char* n[] = {"Off", "Fast", "On cache miss", "Full (accurate, slow)"};
    c.dirty |= Choice("Resolve readback", "readback_resolve", v, n, 4, Applies::kAfterRestart, &c.restart);
    Help("Copies some of the emulated GPU's results back so the game can read them (in Call of Duty 3 lens flares "
         "and the sun shine through walls without it). Full is exact but slow. Applies after a restart.");
    DrawChecks(c, kReadbackChecks);
  }

  if (Has("clear_memory_page_state")) {
    Section("Memory");
    c.dirty |= Check("Re-send all memory the game wrote, every frame", "clear_memory_page_state",
                     Applies::kAfterRestart, &c.restart);
    Help("Off: only memory the game changed since the last frame is sent to the emulated GPU again. On: everything "
         "the game's code ever wrote is sent again every frame. Slower; for when something in the world goes "
         "missing or stops updating.");
  }

  if (AnyOf(kSpeedupChecks)) {
    Section("GPU speed-ups (Vulkan)");
    Help("Shortcuts in the GPU emulation. Each was checked to draw the same picture in Call of Duty 3; turning "
         "one off is for comparing, or for a picture problem.");
    DrawChecks(c, kSpeedupChecks);
  }

  Section("Shaders");
  {
    const unsigned cpus = std::max(1u, std::thread::hardware_concurrency());
    for (const char* name : {"vulkan_pipeline_creation_threads", "d3d12_pipeline_creation_threads"}) {
      if (!Has(name)) continue;
      int threads = GetInt(name, -1);
      ImGui::PushID(name);
      const bool changed =
          ImGui::SliderInt(name[0] == 'v' ? "Shader build threads (Vulkan)" : "Shader build threads (Direct3D 12)",
                           &threads, -1, static_cast<int>(cpus), threads < 0 ? "auto (CPUs - 1)" : "%d");
      ImGui::PopID();
      if (changed) {
        rex::cvar::SetFlagByName(name, std::to_string(threads));
        c.dirty = c.restart = true;
      }
      Help("CPU threads that turn the Xbox 360's shaders into your graphics card's the first time a new effect "
           "appears. Auto uses all but one. Applies after a restart.");
    }
  }
  if (Has("async_shader_compilation")) {
    c.dirty |= Check("Asynchronous shader compilation", "async_shader_compilation", Applies::kAtOnce, &c.restart);
    Help("While a new effect's shader is still being built, keep playing and draw it a moment later, instead of "
         "pausing the game until it's ready.");
  }
  if (Has("store_shaders")) {
    c.dirty |= Check("Store shader cache on disk", "store_shaders", Applies::kAfterRestart, &c.restart);
    Help("Saves built shaders, so the next session doesn't stutter on effects you've already seen.");
  }

  if (AnyOf(kSoundChecks)) {
    Section("Sound chip");
    DrawChecks(c, kSoundChecks);
  }
}

// ------------------------------------------------------------------------------------------------- SOUND
void DrawSoundPage(PageContext& c) {
  if (ResetButton("reset_sound", "sound settings")) {
    c.restart |= ResetSettings({"audio_device", "audio_mute", "audio_output_layout"});
    c.dirty = true;
  }
  Section("Output");
  if (Has("audio_device")) {
    static std::vector<std::string> outputs;  // refreshed when the list opens
    const std::string chosen = Get("audio_device");
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
  DrawSpeakersRow(c);
  c.dirty |= Check("Mute all sound", "audio_mute", Applies::kAtOnce, &c.restart);
  Help("The game's own volume sliders are in its Options.");
}

// Call of Duty 3's Speakers row (ui/hud_overlay.cpp DrawSpeakerLayout), for every game.
void DrawSpeakersRow(PageContext& c) {
  if (!Has("audio_output_layout")) return;
  using rex::audio::OutputLayout;
  static constexpr struct {
    OutputLayout layout;
    const char* label;
  } kLayouts[] = {
      {OutputLayout::kAuto, "Automatic (the device's own)"},
      {OutputLayout::kStereo, "Stereo speakers"},
      {OutputLayout::kHeadphones, "Headphones (virtual surround)"},
      {OutputLayout::kSurround51, "5.1 surround"},
      {OutputLayout::kSurround71, "7.1 surround"},
  };
  const OutputLayout chosen = rex::audio::ParseOutputLayout(Get("audio_output_layout"));
  const char* preview = kLayouts[0].label;
  for (const auto& l : kLayouts) {
    if (l.layout == chosen) preview = l.label;
  }
  if (ImGui::BeginCombo("Speakers", preview)) {
    for (const auto& l : kLayouts) {
      if (ImGui::Selectable(l.label, l.layout == chosen) && l.layout != chosen) {
        rex::cvar::SetFlagByName("audio_output_layout", rex::audio::OutputLayoutName(l.layout));
        c.save_now = true;  // at once, as the sound output is
      }
    }
    ImGui::EndCombo();
  }
  const std::string mixing = rex::audio::CurrentOutputLayout();
  if (!mixing.empty()) ImGui::TextDisabled("Mixing for: %s", mixing.c_str());
  Help("What the game's 5.1 sound is mixed for. Automatic follows the output: stereo for two channels, 5.1 or 7.1 "
       "for a surround system. Headphones places the five speakers around your head, for headphones on a stereo "
       "output. 7.1 plays each surround on the side and the back speaker. A change takes effect at once, with a "
       "moment of silence.");
}

// ------------------------------------------------------------------------------------------------- player
const char* const kPlayerSettings[5] = {"user_name", "user_name_2", "user_name_3", "user_name_4",
                                        "input_player_order"};

namespace {

// A name the game can take: at most `max` bytes, no control characters and none of the characters its console
// reads as something else (quotes, \, %, ;). The SDK applies its own rule again when the game asks for the name.
std::string SafeName(const char* text, size_t max) {
  std::string out;
  for (const char* p = text; *p && out.size() < max; ++p) {
    const unsigned char ch = static_cast<unsigned char>(*p);
    if (ch < 0x20 || ch == 0x7F || ch == '"' || ch == '\\' || ch == '%' || ch == ';') continue;
    out += static_cast<char>(ch);
  }
  while (!out.empty() && out.back() == ' ') out.pop_back();
  return out;
}

// One name row: a text box that keeps what is being typed until Enter or the box is left.
bool NameRow(const char* label, const char* hint, const char* setting, size_t max, char* buffer, size_t size,
             bool* editing) {
  if (!*editing) std::snprintf(buffer, size, "%s", Get(setting).c_str());
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
  const bool entered =
      ImGui::InputTextWithHint(label, hint, buffer, max + 1, ImGuiInputTextFlags_EnterReturnsTrue);
  *editing = ImGui::IsItemActive();
  if (!entered && !ImGui::IsItemDeactivatedAfterEdit()) return false;
  *editing = false;
  const std::string name = SafeName(buffer, max);
  if (name == Get(setting)) return false;
  rex::cvar::SetFlagByName(setting, name);
  return true;
}

}  // namespace

void DrawPlayerSection(PageContext& c) {
  const bool names = Has("user_name");
  const bool order_row = Has("input_player_order");
  if (!names && !order_row) return;
  Section("Player");
  if (names) {
    static char name[32] = "";
    static bool editing = false;
    if (NameRow("Player name", "User", "user_name", 15, name, sizeof(name), &editing)) {
      c.restart = true;
      c.dirty = true;
    }
    Help("Your name in the game, up to 15 characters (the gamertag's room): the name others see in multiplayer. "
         "Empty: \"User\". The game reads it when it starts. Quotes, \\, % and ; are left out.");
    if (Has("user_name_2")) {
      static char others[3][32] = {};
      static bool other_editing[3] = {};
      static const char* const kNames[] = {"user_name_2", "user_name_3", "user_name_4"};
      static const char* const kLabels[] = {"Player 2 name", "Player 3 name", "Player 4 name"};
      static const char* const kHints[] = {"Player 2", "Player 3", "Player 4"};
      for (int i = 0; i < 3; ++i) {
        if (!Has(kNames[i])) continue;
        if (NameRow(kLabels[i], kHints[i], kNames[i], 14, others[i], sizeof(others[i]), &other_editing[i])) {
          c.restart = true;
          c.dirty = true;
        }
      }
      Help("The names of split-screen players 2 to 4, up to 14 characters each. Empty: the game's own. The game "
           "reads them when it starts.");
    }
  }
  if (order_row) {
    int order[4] = {0, 1, 2, 3};  // the player (0-3) of each controller, in connection order
    {
      const std::string text = Get("input_player_order");
      int parsed[4] = {0, 1, 2, 3};
      bool used[4] = {};
      int count = 0;
      bool ok = true;
      for (char ch : text) {
        if (ch == ' ' || ch == ',') continue;
        if (ch < '1' || ch > '4' || count >= 4 || used[ch - '1']) {
          ok = false;
          break;
        }
        used[ch - '1'] = true;
        parsed[count++] = ch - '1';
      }
      if (ok) {
        for (int i = count; i < 4; ++i) {
          int u = 0;
          while (used[u]) ++u;
          used[u] = true;
          parsed[i] = u;
        }
        for (int i = 0; i < 4; ++i) order[i] = parsed[i];
      }
    }
    static const char* const kPlayers[] = {"Player 1", "Player 2", "Player 3", "Player 4"};
    static const char* const kPads[] = {"First controller to connect", "Second controller", "Third controller",
                                        "Fourth controller"};
    bool changed = false;
    for (int i = 0; i < 4; ++i) {
      int player = order[i];
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 8.0f);
      if (ImGui::Combo(kPads[i], &player, kPlayers, 4) && player != order[i]) {
        // Still one controller a player: the one that had this player takes this one's old one.
        for (int j = 0; j < 4; ++j)
          if (order[j] == player) order[j] = order[i];
        order[i] = player;
        changed = true;
      }
    }
    if (ImGui::Button("Reset to connection order")) {
      for (int i = 0; i < 4; ++i) order[i] = i;
      changed = true;
    }
    if (changed) {
      std::string text;
      for (int i = 0; i < 4; ++i) text += static_cast<char>('1' + order[i]);
      rex::cvar::SetFlagByName("input_player_order", text == "1234" ? "" : text);
      c.dirty = true;
    }
    Help("Which player each controller plays as, by the order the controllers connected. It applies at once. The "
         "keyboard and mouse always play as player 1, together with the controller that is player 1.");
  }
}

// ------------------------------------------------------------------------------------------------- INTERFACE
void DrawInterfacePage(PageContext& c) {
  const LookSettings& look = Look();
  if (ResetButton("reset_interface", "interface settings")) {
    c.restart |= ResetSettings({look.theme_name, look.text_size_name, look.opacity_name});
    c.dirty = true;
  }
  Section("Look");
  size_t count = 0;
  const Theme* themes = Themes(&count);
  const Theme& chosen = CurrentTheme();
  if (ImGui::BeginCombo("Theme", chosen.label)) {
    for (size_t i = 0; i < count; ++i) {
      const bool selected = &themes[i] == &chosen;
      if (ImGui::Selectable(themes[i].label, selected) && !selected) {
        rex::cvar::SetFlagByName(look.theme_name, themes[i].id);
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
    c.dirty |= Choice("Text size", look.text_size_name, v, n, 5, Applies::kAtOnce, &c.restart);
    Help("The size of the text in this window. The game's own menus are not changed.");
  }
  c.dirty |= SliderDouble("Window opacity", look.opacity_name, 0.6, 1.0, "%.2f", Applies::kAtOnce, &c.restart);
  Help("Lower it to see the game behind this window while you change settings.");
}

// ------------------------------------------------------------------------------------------------- CONTROLS
void DrawControlsPage(PageContext& c, const char* pad_shortcut_setting,
                      const std::function<void(PageContext&)>& game_rows,
                      const std::vector<const char*>& game_settings) {
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
  if (ResetButton("reset_controls", "control settings")) {
    c.restart |= ResetSettings({"mnk_mode", "mnk_mouse", "mnk_sensitivity", pad_shortcut_setting});
    for (const char* name : game_settings) c.restart |= ResetSettings({name});
    for (const auto& bind : kBinds) c.restart |= ResetSettings({bind[0]});
    {  // [f2-menus] 2026-11-11 04:12 the mouse acceleration, the sensitivity randomizer and the scales too
      const char* const* names = nullptr;
      const int count = accel::Settings(&names);
      for (int i = 0; i < count; ++i) c.restart |= ResetSettings({names[i]});
    }
    c.dirty = true;
  }
  Section("Keyboard and mouse");
  c.dirty |= Check("Play with keyboard and mouse", "mnk_mode", Applies::kAtOnce, &c.restart);
  // [f2-menus] 2026-11-11 ctl 14:50: a port with the PC copy's keys (Call of Duty 2) plays a level with those.
  Help("The keyboard and mouse play as a controller: each key of Keys below presses a controller button, in the menus "
       "and, unless the game's own PC keys are on (where a port has them), in a level too.");
  c.dirty |= Check("The mouse turns the view", "mnk_mouse", Applies::kAtOnce, &c.restart);
  c.dirty |= SliderDouble("Mouse sensitivity", "mnk_sensitivity", 0.1, 10.0, "%.2f", Applies::kAtOnce, &c.restart);
  if (game_rows) game_rows(c);
  accel::DrawRows(c);  // [f2-menus] 2026-11-11 mouse acceleration inside the game (a port that applies it says so)

  Section("Controller");
  c.dirty |= Check("Back + Start held 1 second opens these settings", pad_shortcut_setting, Applies::kAtOnce,
                   &c.restart);
  Help("Any controller. While this window is open the game gets no input; the D-pad or left stick moves, A chooses, "
       "B goes back, LB and RB change the page. The game's own button layouts and look sensitivity are in its "
       "Options.");

  if (GetBool("mnk_mode")) {
    Section("Keys");
    Help("A key for each controller button. Several keys go with commas (Semicolon,Space). Names: letters and "
         "digits, Space, Return, Tab, Backspace, Up, Down, Left, Right, Shift+Up, LMB, RMB. Type, then press Enter.");
    for (const auto& bind : kBinds) c.dirty |= TextRow(bind[1], bind[0], &c.restart);
  }
}

// ------------------------------------------------------------------------------------------------- test script
TestScript::TestScript(const std::string& script) {
  size_t start = 0;
  while (start < script.size()) {
    size_t end = script.find('|', start);
    if (end == std::string::npos) end = script.size();
    const std::string entry = script.substr(start, end - start);
    start = end + 1;
    const size_t colon = entry.find(':');
    if (colon == std::string::npos || colon == 0 || colon + 1 >= entry.size()) continue;
    steps_.push_back({std::strtod(entry.c_str(), nullptr), entry.substr(colon + 1)});
  }
}

void TestScript::Run(Overlay& overlay, CommandQueue& commands) {
  if (next_ >= steps_.size()) return;
  const auto now = std::chrono::steady_clock::now();
  if (!started_) {
    first_ = now;
    started_ = true;
  }
  const double since = std::chrono::duration<double>(now - first_).count();
  while (next_ < steps_.size() && since >= steps_[next_].seconds) {
    const std::string& action = steps_[next_++].action;
    REXLOG_INFO("f2: test step: {}", action);
    if (action == "open" && !overlay.WindowOpen()) {
      overlay.Toggle("test step");
    } else if (action == "close" && overlay.WindowOpen()) {
      overlay.Toggle("test step");
    } else if (action.rfind("page ", 0) == 0) {
      if (auto* w = overlay.settings_window(); !w || !w->ShowPage(action.substr(5)))
        REXLOG_WARN("f2: test step: no page {}", action.substr(5));
    } else if (action.rfind("set ", 0) == 0) {
      const std::string rest = action.substr(4);
      const size_t space = rest.find(' ');
      if (space != std::string::npos) {
        const std::string name = rest.substr(0, space);
        const bool ok = rex::cvar::SetFlagByName(name, rest.substr(space + 1));
        if (auto* w = overlay.settings_window()) w->MarkDirty();
        REXLOG_INFO("f2: test step: {} = {} ({})", name, Get(name.c_str()), ok ? "set" : "refused");
      }
    } else if (action == "save") {
      if (auto* w = overlay.settings_window()) w->Save();
    } else if (action.rfind("console ", 0) == 0) {
      commands.Push(action.substr(8));  // as if typed on the CONSOLE page
    }
  }
}

// ------------------------------------------------------------------------------------------------- frame cost
// [f2-menus] 2026-11-11 02:10 What each frame costs, as Call of Duty 3's F2 > PERFORMANCE shows it (its
// game/perf_stats.h), for the ports that share this window: the frames the game handed to the emulated GPU (not the
// window's repaints, which the SDK makes on its own too) and what the GPU emulation did in each. One line on screen
// (f2_frame_cost), refreshed every second; one in the log every f2_frame_cost_log seconds, averaged over them, for
// test runs that compare settings by numbers.
namespace frame_cost {
namespace {

struct Report {
  bool valid = false;
  double frames = 0, seconds = 0;
  float fps = 0, frame_ms = 0, draws = 0, resolves = 0, transfers = 0, gpu_waits = 0, gpu_wait_ms = 0;
  float gpu_thread_ms = -1;  // the GPU emulation's own thread, busy time a frame (-1: this SDK doesn't time it)
  float host_gpu_ms = -1;    // the graphics card's time a frame on its command buffers (-1: not measured)
};
[[maybe_unused]] Report g_report;
float g_corner_bottom = 0.0f;  // where the corner lines ended this frame (0 = none drawn), for a line under them

[[maybe_unused]] std::string Describe(const Report& r) {
  char text[320];
  int n = std::snprintf(text, sizeof(text),
                        "%.1f fps, %.2f ms a frame; draws %.0f, resolves %.1f, transfers %.1f, GPU waits %.1f (%.2f ms)",
                        r.fps, r.frame_ms, r.draws, r.resolves, r.transfers, r.gpu_waits, r.gpu_wait_ms);
  if (r.gpu_thread_ms >= 0 && n > 0 && n < static_cast<int>(sizeof(text)))
    n += std::snprintf(text + n, sizeof(text) - static_cast<size_t>(n), ", GPU thread %.2f ms", r.gpu_thread_ms);
  if (r.host_gpu_ms >= 0 && n > 0 && n < static_cast<int>(sizeof(text)))
    std::snprintf(text + n, sizeof(text) - static_cast<size_t>(n), ", graphics card %.2f ms", r.host_gpu_ms);
  return text;
}

#if F2_HAS_GPU_STATS
Report Between(const rex::graphics::stats::Snapshot& prev, const rex::graphics::stats::Snapshot& cur) {
  namespace gs = rex::graphics::stats;
  Report r;
  auto delta = [&](gs::Counter c) { return double(cur[c] - prev[c]); };
  r.frames = delta(gs::Counter::kFrames);
  r.seconds = double(cur.time_us - prev.time_us) / 1e6;
  if (r.frames < 1 || r.seconds <= 0) return r;
  auto per_frame = [&](gs::Counter c) { return float(delta(c) / r.frames); };
  r.valid = true;
  r.fps = float(r.frames / r.seconds);
  r.frame_ms = float(r.seconds * 1000.0 / r.frames);
  r.draws = per_frame(gs::Counter::kDraws);
  r.resolves = per_frame(gs::Counter::kResolves);
  r.transfers = per_frame(gs::Counter::kRenderTargetTransfers);
  r.gpu_waits = per_frame(gs::Counter::kGpuWaits);
  r.gpu_wait_ms = per_frame(gs::Counter::kGpuWaitMicros) / 1000.0f;
#if defined(REX_GRAPHICS_STATS_VERSION) && REX_GRAPHICS_STATS_VERSION >= 2
  r.gpu_thread_ms = per_frame(gs::Counter::kGpuThreadMicros) / 1000.0f;
#endif
#if defined(REX_GRAPHICS_STATS_VERSION) && REX_GRAPHICS_STATS_VERSION >= 4
  if (cur[gs::Counter::kHostGpuMicros] > 0) r.host_gpu_ms = per_frame(gs::Counter::kHostGpuMicros) / 1000.0f;
#endif
  return r;
}

// The lines in the top left corner (the games' own frame rate counter is top right).
void DrawCorner() {
  const ImGuiIO& io = ImGui::GetIO();
  const Report& r = g_report;
  char lines[3][160];
  int n = 0;
  if (!r.valid) {
    std::snprintf(lines[n++], sizeof(lines[0]), "frame cost: waiting for frames");
  } else {
    std::snprintf(lines[n++], sizeof(lines[0]), "%.0f fps  %.2f ms", r.fps, r.frame_ms);
    std::snprintf(lines[n++], sizeof(lines[0]), "draws %.0f  resolves %.1f  transfers %.1f", r.draws, r.resolves,
                  r.transfers);
    if (r.gpu_thread_ms >= 0)
      std::snprintf(lines[n++], sizeof(lines[0]), "GPU waits %.1f (%.2f ms)  GPU thread %.2f ms", r.gpu_waits,
                    r.gpu_wait_ms, r.gpu_thread_ms);
    else
      std::snprintf(lines[n++], sizeof(lines[0]), "GPU waits %.1f (%.2f ms)", r.gpu_waits, r.gpu_wait_ms);
  }
  const float margin = std::floor(12.0f * std::max(1.0f, io.DisplaySize.y / 1080.0f));
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  float y = margin;
  for (int i = 0; i < n; ++i) {
    const ImVec2 pos(margin, std::floor(y));
    dl->AddText(ImVec2(pos.x + 1.0f, pos.y + 1.0f), IM_COL32(0, 0, 0, 200), lines[i]);
    dl->AddText(pos, IM_COL32(230, 230, 230, 255), lines[i]);
    y += ImGui::CalcTextSize(lines[i]).y + 2.0f;
  }
  g_corner_bottom = y;
  accel::SetCornerTop(y);  // [f2-menus] 03:45 the randomizer's line goes under these
}
#endif  // F2_HAS_GPU_STATS

}  // namespace

void Tick() {
  g_corner_bottom = 0.0f;
  accel::SetCornerTop(0.0f);
  const bool show = REXCVAR_GET(f2_frame_cost);
  const int every = REXCVAR_GET(f2_frame_cost_log);
#if F2_HAS_GPU_STATS
  namespace gs = rex::graphics::stats;
  using Clock = std::chrono::steady_clock;
  static gs::Snapshot shown_from, logged_from;
  static Clock::time_point shown_time{}, logged_time{};
  if (!show && every <= 0) {  // off: the next time it's turned on starts from a fresh second
    shown_time = logged_time = Clock::time_point{};
    g_report = Report{};
    return;
  }
  const auto now = Clock::now();
  const bool second = shown_time == Clock::time_point{} || now - shown_time >= std::chrono::seconds(1);
  const bool log =
      every > 0 && (logged_time == Clock::time_point{} || now - logged_time >= std::chrono::seconds(every));
  if (second || log) {
    const gs::Snapshot cur = gs::Read();
    if (second) {
      if (shown_time != Clock::time_point{}) g_report = Between(shown_from, cur);
      shown_from = cur;
      shown_time = now;
    }
    if (log) {
      if (logged_time != Clock::time_point{}) {
        const Report r = Between(logged_from, cur);
        if (r.valid) {
          char tail[64];
          std::snprintf(tail, sizeof(tail), " (over %.1f s, %.0f frames)", r.seconds, r.frames);
          REXLOG_INFO("f2: frame cost: {}{}", Describe(r), tail);
        } else {
          REXLOG_INFO("f2: frame cost: no frames in the last {} s", every);
        }
      }
      logged_from = cur;
      logged_time = now;
    }
  }
  if (show) DrawCorner();
#else
  static bool said = false;
  if ((show || every > 0) && !said) {
    said = true;
    REXLOG_INFO("f2: frame cost: this build's SDK doesn't count frames (no rex/graphics/gpu_stats.h)");
  }
#endif
}

}  // namespace frame_cost

// ------------------------------------------------------------------------------------------------- low-end PC
// [f2-menus] 2026-11-11 13:15 Call of Duty 3's PERFORMANCE > Low-end PC in every port (FEATURES.md section 6 item 2):
// one click for a PC with 2-4 CPU cores and integrated graphics. The SDK settings that cost the most for the least
// difference on screen (as Call of Duty 3's preset), the render resolution back to the game's own 1280 x 720, and
// the game's own picture switches this game has (sun and model shadows, depth of field, glow, distortion, soft
// smoke) off. The game's speed, the frame rate limit and V-Sync stay; a setting already cheaper is kept.
namespace low_end {

std::vector<int> g_game_rows;  // the kGameOptions rows this game sends (SendGameOptions)

// Shader build threads: half the logical CPUs minus one, at least one, so shader bursts leave the game's own
// threads their cores.
int ShaderThreads() {
  const int hw = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
  return std::max(1, hw / 2 - 1);
}

// The game's own rows the preset turns off, where this game has them.
const char* const kGameOff[] = {"f2_game_sun_shadows",   "f2_game_dof",        "f2_game_glow",     "f2_game_distortion",
                                "f2_game_model_shadows", "f2_game_soft_smoke", "f2_game_cod2_glow"};

bool GameHasRow(const char* setting) {
  for (const int i : g_game_rows)
    if (i >= 0 && i < kGameOptionCount && std::strcmp(kGameOptions[i].setting, setting) == 0) return true;
  return false;
}

// True when a change applies only after a restart.
bool Apply() {
  const std::string threads = std::to_string(ShaderThreads());
  struct Change {
    const char* cvar;
    std::string value;
  };
  const Change changes[] = {
      // Extra full-screen passes after the game's own picture; the game's own texture filtering.
      {"swap_post_effect", "none"},
      {"present_effect", "bilinear"},
      {"anisotropic_override", "-1"},
      // Don't send geometry the game hasn't changed to the emulated GPU again every frame.
      {"clear_memory_page_state", "false"},
      {"vulkan_pipeline_creation_threads", threads},
      {"d3d12_pipeline_creation_threads", threads},
      {"async_shader_compilation", "true"},
      {"store_shaders", "true"},
      // Waits sleep instead of spinning: on 2-4 cores a spinning thread takes the GPU emulation's core.
      {"global_lock_spin_us", "-1"},
      {"gpu_idle_spin_count", "-1"},
      // The GPU emulation's speed-ups (on by default; EMULATION can turn them off for a comparison).
      {"vulkan_reuse_draw_state", "true"},
      {"vulkan_skip_unchanged_fetch_constants", "true"},
      {"vulkan_sampler_cache", "true"},
      {"vulkan_reuse_texture_descriptors", "true"},
      {"vulkan_dynamic_constant_offsets", "true"},
      {"vulkan_resolve_into_textures", "true"},
      {"vulkan_texture_memo", "true"},
      {"readback_resolve_cpu_reads_only", "true"},
      {"readback_resolve_cpu_pages_only", "true"},
      {"readback_resolve_full_deferred", "true"},
      {"texture_replacement_async", "true"},
      {"xma_restart_without_reopen", "true"},
      // Checks and counters that only tests need.
      {"vulkan_reuse_draw_state_verify", "false"},
      {"vulkan_texture_memo_verify", "false"},
      {"vulkan_log_state_reuse", "false"},
      {"f2_frame_cost", "false"},
      {"f2_frame_cost_log", "0"},
  };
  bool restart = false;
  for (const Change& ch : changes) {
    if (!Has(ch.cvar)) continue;
    const std::string before = Get(ch.cvar);
    if (before == ch.value) continue;
    const std::string name = ch.cvar;
    if (name == "anisotropic_override" && before == "0") continue;
    if (name.size() > 26 && name.compare(name.size() - 26, 26, "_pipeline_creation_threads") == 0 &&
        !before.empty() && before != "-1" && std::atoi(before.c_str()) < std::atoi(ch.value.c_str()))
      continue;
    rex::cvar::SetFlagByName(ch.cvar, ch.value);
    if (name.compare(0, 3, "f2_") != 0) restart = true;  // the SDK's: most apply after a restart
  }
  // The game's own 1280 x 720 (render scale 1); "original" (the console's own) stays.
  if (Has("f2_render_resolution") && Has("resolution_scale") && GetInt("resolution_scale", 1) > 1 &&
      Get("f2_render_resolution") != "original") {
    rex::cvar::SetFlagByName("f2_render_resolution", "720p");
    restart = true;
  }
  for (const char* row : kGameOff)
    if (Has(row) && GameHasRow(row) && Get(row) != "0") rex::cvar::SetFlagByName(row, "0");
  REXLOG_INFO("f2: low-end PC settings applied from the settings window");
  return restart;
}

void DrawSection(PageContext& c) {
  Section("Low-end PC");
  if (ImGui::Button("Use low-end PC settings...")) ImGui::OpenPopup("Low-end PC settings##lowend");
  if (ImGui::BeginPopupModal("Low-end PC settings##lowend", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("For a PC with 2-4 CPU cores and integrated graphics:");
    ImGui::BulletText("Render resolution: the game's own 1280 x 720 (the console's own size stays if chosen)");
    ImGui::BulletText("No FXAA, plain bilinear scaling, the game's own texture filtering");
    ImGui::BulletText("Geometry the game hasn't changed isn't sent to the GPU again every frame");
    ImGui::BulletText("%d shader build thread(s), shaders kept on disk", ShaderThreads());
    ImGui::BulletText("The game's shadows, depth of field, glow and distortion off (those it has)");
    ImGui::BulletText("Waiting threads sleep instead of spinning, so the GPU emulation keeps its core");
    ImGui::BulletText("Every EMULATION speed-up on; test checks off; the frame cost line off");
    ImGui::TextDisabled("The game's speed, the frame rate limit and V-Sync don't change.");
    if (ImGui::Button("Apply")) {
      c.restart |= Apply();
      c.dirty = true;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  Help("Changes the settings in VIDEO, GAME, EMULATION and on this page that cost the most on a slow PC, for the "
       "least difference on screen. Each can still be changed back by hand; most apply after a restart. If something "
       "in the world goes missing or stops updating afterwards, turn on EMULATION's \"Re-send all memory the game "
       "wrote, every frame\".");
}

}  // namespace low_end

void DrawPerformanceRows(PageContext& c) {
  low_end::DrawSection(c);  // [f2-menus] 2026-11-11 13:15 Call of Duty 3's Low-end PC
  Section("Frame cost");
  c.dirty |= Check("Show what each frame costs", "f2_frame_cost", Applies::kAtOnce, &c.restart);
#if F2_HAS_GPU_STATS
  Help("In the top left corner: the frame rate and the time a frame takes, then what the Xbox 360 GPU emulation did "
       "in each frame (draws, pictures copied out of its video memory, pictures moved between render targets) and "
       "how often it stopped to wait for your graphics card. For finding what a setting costs on this PC: change "
       "it and watch the numbers.");
#else
  Help("This build can't count the game's frames, so nothing is shown.");
#endif
}

// ------------------------------------------------------------------------------------------------- mouse acceleration
// [f2-menus] 2026-11-11 03:45 (owner 03:05: "This will be needed within cod3 as well. Same goes for raw accel"): the
// acceleration, the sensitivity randomizer and the sensitivity scales are in f2_accel.{h,cpp} now, with no code of
// this window in them, so Call of Duty 3 builds the same file. Here only their rows, drawn with this window's rows.
namespace accel {
void DrawRows(PageContext& c) {
  static const Widgets widgets = {
      [](const char* title) { Section(title); },
      [](const char* label, const char* name, bool* restart) { return Check(label, name, Applies::kAtOnce, restart); },
      [](const char* label, const char* name, const char* const* values, const char* const* names, int count,
         bool* restart) { return Choice(label, name, values, names, count, Applies::kAtOnce, restart); },
      [](const char* label, const char* name, int min, int max, const char* format, bool* restart) {
        return SliderInt(label, name, min, max, format, Applies::kAtOnce, restart);
      },
      [](const char* label, const char* name, double min, double max, const char* format, bool* restart) {
        return SliderDouble(label, name, min, max, format, Applies::kAtOnce, restart);
      },
      [](const char* text) { Help(text); },
  };
  c.dirty |= DrawRows(widgets, &c.restart);
}
}  // namespace accel

// ------------------------------------------------------------------------------------------------- game options
const GameOption kGameOptions[] = {
    {"f2_game_show_crosshair", "cg_drawCrosshair", "The game's crosshair",
     "The game's own hip-fire crosshair on or off (its Options have the same switch)."},
    {"f2_game_enemy_crosshair", "cg_crosshairEnemyColor", "Red crosshair over an enemy",
     "Whether the crosshair turns red over an enemy. The custom crosshair follows it too, since it takes the game's "
     "color."},
    {"f2_game_subtitles", "cg_subtitles", "Subtitles", "Spoken lines as text."},
    {"f2_game_blood", "cg_blood", "Blood", "Blood effects on or off."},
    {"f2_game_aim_autoaim", "aim_autoaim_enabled", "Aim assist: snap to a target",
     "Raising the sights near an enemy pulls the aim onto him (controller aim)."},
    {"f2_game_aim_lockon", "aim_lockon_enabled", "Aim assist: follow a target",
     "The aim follows an enemy who moves while he is under the crosshair (controller aim)."},
    {"f2_game_aim_slowdown", "aim_slowdown_enabled", "Aim assist: slow down over a target",
     "The turn slows while the crosshair is over an enemy (controller aim)."},
    {"f2_game_friendly_names", "cg_drawFriendlyNames", "Names over friendly soldiers", "Your squad's names."},
    {"f2_game_crosshair_names", "cg_drawCrosshairNames", "Name under the crosshair",
     "The name of the soldier you are looking at."},
    {"f2_game_draw_gun", "cg_drawGun", "Your weapon in view", "Your own weapon and hands drawn, or not."},
    {"f2_game_draw_hud", "cg_drawHUD", "The HUD", "The compass, ammunition, stance and the rest of the HUD."},
    // [f2-menus] 2026-11-10 20:30: 11, 12 the controller; 13 to 16 hints and small effects; 17 to 21 the picture (IW3)
    {"f2_game_invert_pitch", "input_invertPitch", "Invert look up / down (controller)",
     "Pushing the right stick up looks down (the game's own Options have the same switch)."},
    {"f2_game_rumble", "gpad_rumble", "Controller rumble", "The controller shakes when you fire or are hit."},
    {"f2_game_brass", "cg_brass", "Shell casings", "Spent casings thrown out of weapons."},
    {"f2_game_breath_hint", "cg_drawBreathHint", "Hold-breath hint",
     "The line that tells you which button steadies a scope."},
    {"f2_game_mantle_hint", "cg_drawMantleHint", "Climb hint",
     "The line that tells you which button climbs over what is in front of you."},
    {"f2_game_turret_crosshair", "cg_drawTurretCrosshair", "Crosshair on a mounted gun",
     "The crosshair shown while you are on a mounted machine gun."},
    {"f2_game_sun_shadows", "sm_sunEnable", "Sun shadows", "Shadows the sun casts from soldiers and objects."},
    {"f2_game_dof", "r_dof_enable", "Depth of field",
     "The blur of what is out of focus, as when aiming down sights."},
    {"f2_game_glow", "r_glow_allowed", "Glow", "The glow around bright parts of the picture."},
    {"f2_game_specular", "r_specular", "Specular highlights", "Shine on metal, wet ground and the like."},
    {"f2_game_distortion", "r_distortion", "Heat and water distortion",
     "The ripple of hot air, explosions and water in front of the picture."},
    // [f2-menus] 2026-11-10 21:00: 22 the game's own mouse look
    {"f2_game_mouse_filter", "m_filter", "Smooth mouse (the game's own mouse look)",
     "Each turn is the average of the last two reads of the mouse: smoother, and a little later. It counts while "
     "\"Aim with the game's own mouse look\" is on."},
    // [f2-menus] 2026-11-11 07:35: 23 to 26 Call of Duty 2's picture switches (its PC copy's Graphics menu has
    // shadows and soft smoke; glow and marks are its other dvars of the kind)
    {"f2_game_model_shadows", "sc_enable", "Model shadows",
     "The soft shadows soldiers and objects cast on the ground under them."},
    {"f2_game_soft_smoke", "r_zfeather", "Soft smoke",
     "Smoke and dust fade where they meet walls and the ground, instead of ending in a hard line."},
    {"f2_game_cod2_glow", "r_glow", "Glow", "The glow around bright parts of the picture."},
    {"f2_game_marks", "cg_marks", "Bullet holes and scorch marks",
     "The marks shots and explosions leave on walls and the ground."},
};
const int kGameOptionCount = int(sizeof(kGameOptions) / sizeof(kGameOptions[0]));

void DrawGameOptionRows(PageContext& c, const int* indices, int count, int first, int last) {
  static const char* const kValues[] = {"", "1", "0"};
  static const char* const kNames[] = {"The game's own setting", "On", "Off"};
  for (int i = 0; i < count; ++i) {
    if (indices[i] < 0 || indices[i] >= kGameOptionCount) continue;
    if (indices[i] < first || indices[i] > last) continue;  // [f2-menus] a page draws its rows in groups
    const GameOption& o = kGameOptions[indices[i]];
    if (!Has(o.setting)) continue;
    c.dirty |= Choice(o.label, o.setting, kValues, kNames, 3, Applies::kAtOnce, &c.restart);
    Help(o.help);
  }
}

// [f2-menus] 2026-11-11 00:15 A row given back before the game has registered its dvar: "reset" then leaves the value
// the row's "set" made (run mp2224: Call of Duty 2 registers cg_drawBreathHint and the like when a level starts;
// at its menu they read "0" after the row was forced and given back), and the game takes that value over when it
// registers the dvar. A port that knows whether a level runs says so (SetLevelRuns); a dvar given back while none
// runs is reset once more two seconds into the next level, then forgotten, so the game's own Options aren't fought.
namespace {
bool g_level_known = false;
bool g_level_runs = false;
std::vector<std::string> g_reset_again;
}  // namespace

void ResetAgainAtLevelStart(const char* dvar) {
  if (!g_level_known || g_level_runs) return;
  if (std::find(g_reset_again.begin(), g_reset_again.end(), dvar) == g_reset_again.end())
    g_reset_again.emplace_back(dvar);
}

namespace {
// The row is forced again: its value is sent every few seconds, so no reset is owed any more.
void ForgetResetAgain(const char* dvar) {
  g_reset_again.erase(std::remove(g_reset_again.begin(), g_reset_again.end(), dvar), g_reset_again.end());
}
}  // namespace

void SetLevelRuns(bool runs, const std::function<void(std::string)>& push) {
  static std::chrono::steady_clock::time_point since{};
  const auto time = std::chrono::steady_clock::now();
  if (!g_level_known || runs != g_level_runs) {
    g_level_known = true;
    g_level_runs = runs;
    since = time;
  }
  if (!runs || g_reset_again.empty() || time - since < std::chrono::seconds(2)) return;
  for (const std::string& dvar : g_reset_again) {
    push("reset " + dvar);
    REXLOG_INFO("f2: {} -> the game's own (reset again: a level started)", dvar);
  }
  g_reset_again.clear();
}

void SendGameOptions(const int* indices, int count, const std::function<void(std::string)>& push) {
  low_end::g_game_rows.assign(indices, indices + count);  // [f2-menus] 2026-11-11 13:15 for Low-end PC
  static std::string sent[sizeof(kGameOptions) / sizeof(kGameOptions[0])];
  // A forced choice is sent again every few seconds: the game puts its own value back when it loads the player's
  // profile or a level (run go1: Call of Duty 4's cg_subtitles set before the level was 1 again in it).
  static std::chrono::steady_clock::time_point last_resend{};
  const auto time = std::chrono::steady_clock::now();
  const bool resend = time - last_resend > std::chrono::seconds(3);
  if (resend) last_resend = time;
  for (int i = 0; i < count; ++i) {
    if (indices[i] < 0 || indices[i] >= kGameOptionCount) continue;
    const GameOption& o = kGameOptions[indices[i]];
    std::string now = Get(o.setting);
    if (now != "1" && now != "0") now.clear();
    const bool changed = now != sent[indices[i]];
    if (!changed && !(resend && !now.empty())) continue;
    sent[indices[i]] = now;
    push(now.empty() ? std::string("reset ") + o.dvar : std::string("set ") + o.dvar + " " + now);
    if (changed && now.empty()) ResetAgainAtLevelStart(o.dvar);  // [f2-menus] 00:15
    if (changed && !now.empty()) ForgetResetAgain(o.dvar);
    if (changed) REXLOG_INFO("f2: game option {} -> {}", o.dvar, now.empty() ? "the game's own (reset)" : now.c_str());
  }
}

// The game's own numbers: the weapon's place on screen and the game crosshair's opacity. Each group has a switch
// ("use my own") and follows it: on, the dvars are set on a change and again every 3 seconds (a level's start puts
// the game's own back); switched off, they are reset once.
const char* const kGameNumberSettings[6] = {"f2_gun_own", "f2_gun_x", "f2_gun_y", "f2_gun_z", "f2_crosshair_alpha_own",
                                            "f2_crosshair_alpha"};

// [f2-menus] 2026-11-10 21:00 the game's own look numbers: the right stick's speed, and (mouse) the sensitivity of
// the game's own mouse look. A switch each, as the rows below.
const char* const kGameInputSettings[4] = {"f2_stick_sens_own", "f2_stick_sens", "f2_mouse_sens_own", "f2_mouse_sens"};

void DrawGameInputRows(PageContext& c, bool mouse) {
  if (!Has("f2_stick_sens_own")) return;
  c.dirty |= Check("Use my own stick look speed", "f2_stick_sens_own", Applies::kAtOnce, &c.restart);
  if (GetBool("f2_stick_sens_own"))
    c.dirty |= SliderDouble("Stick look speed", "f2_stick_sens", 0.2, 5.0, "%.2f", Applies::kAtOnce, &c.restart);
  Help("How fast the right stick turns the view, as a multiple of the game's own (1). Off: the game's own.");
  if (!mouse) return;
  c.dirty |= Check("Use my own mouse sensitivity in the game's mouse look", "f2_mouse_sens_own", Applies::kAtOnce,
                   &c.restart);
  if (GetBool("f2_mouse_sens_own"))
    c.dirty |= SliderDouble("Mouse sensitivity (game)", "f2_mouse_sens", 0.5, 30.0, "%.1f", Applies::kAtOnce,
                            &c.restart);
  Help("The game's own mouse sensitivity (5 unless its Options changed it). It counts while \"Aim with the game's "
       "own mouse look\" is on. Off: the game's own.");
}

// [f2-menus] 2026-11-10 22:30 the game's own gamma and (brightness) brightness. A switch each, as the rows above.
const char* const kGamePictureSettings[4] = {"f2_gamma_own", "f2_gamma", "f2_brightness_own", "f2_brightness"};

void DrawGamePictureRows(PageContext& c, bool brightness) {
  if (!Has("f2_gamma_own")) return;
  c.dirty |= Check("Use my own gamma", "f2_gamma_own", Applies::kAtOnce, &c.restart);
  if (GetBool("f2_gamma_own"))
    c.dirty |= SliderDouble("Gamma", "f2_gamma", 0.5, 3.0, "%.2f", Applies::kAtOnce, &c.restart);
  Help("The game's own gamma: larger lifts the dark parts of the picture, smaller darkens them. Off: the game's own "
       "(1; 1.25 in Call of Duty 2).");
  if (!brightness) return;
  c.dirty |= Check("Use my own brightness", "f2_brightness_own", Applies::kAtOnce, &c.restart);
  if (GetBool("f2_brightness_own"))
    c.dirty |= SliderDouble("Brightness", "f2_brightness", -0.5, 0.5, "%.2f", Applies::kAtOnce, &c.restart);
  Help("Added to the whole picture by the game: above 0 brighter, below 0 darker. Small steps go a long way (0.3 "
       "more than doubles a dim scene's brightness). Off: the game's own (0).");
}

// [f2-menus] 2026-11-11 07:35 Call of Duty 2's dynamic lights (its PC copy's Graphics menu, r_dlightLimit).
const char* const kGameLightSettings[2] = {"f2_dlights_own", "f2_dlights"};

void DrawGameLightRows(PageContext& c) {
  if (!Has("f2_dlights_own")) return;
  c.dirty |= Check("Use my own number of dynamic lights", "f2_dlights_own", Applies::kAtOnce, &c.restart);
  if (GetBool("f2_dlights_own"))
    c.dirty |= SliderDouble("Dynamic lights", "f2_dlights", 0.0, 4.0, "%.0f", Applies::kAtOnce, &c.restart);
  Help("The most moving lights (muzzle flashes, explosions, fires) that light the world at once. Fewer costs less. "
       "Off: the game's own (4).");
}

// [f2-menus] 2026-11-11 07:52 Call of Duty 2's campaign: the PC copy's "Number of corpses" (ai_corpseCount).
const char* const kGameCorpseSettings[2] = {"f2_corpses_own", "f2_corpses"};

void DrawGameCorpseRows(PageContext& c) {
  if (!Has("f2_corpses_own")) return;
  c.dirty |= Check("Use my own number of bodies", "f2_corpses_own", Applies::kAtOnce, &c.restart);
  if (GetBool("f2_corpses_own"))
    c.dirty |= SliderDouble("Bodies left lying", "f2_corpses", 0.0, 64.0, "%.0f", Applies::kAtOnce, &c.restart);
  Help("The most fallen soldiers left lying at once; above it the oldest ones go. Fewer costs less in big fights. "
       "Off: the game's own (5).");
}

void DrawGameNumberRows(PageContext& c) {
  if (!Has("f2_gun_own")) return;
  c.dirty |= Check("Use my own weapon position", "f2_gun_own", Applies::kAtOnce, &c.restart);
  if (GetBool("f2_gun_own")) {
    c.dirty |= SliderDouble("Weapon forward / back", "f2_gun_x", -10.0, 10.0, "%.1f", Applies::kAtOnce, &c.restart);
    c.dirty |= SliderDouble("Weapon left / right", "f2_gun_y", -10.0, 10.0, "%.1f", Applies::kAtOnce, &c.restart);
    c.dirty |= SliderDouble("Weapon up / down", "f2_gun_z", -10.0, 10.0, "%.1f", Applies::kAtOnce, &c.restart);
  }
  Help("Where your weapon sits on screen, in the game's own units from its own place (0 0 0): + is forward, left "
       "and up. Off: the game's own.");
  c.dirty |= Check("Use my own crosshair opacity", "f2_crosshair_alpha_own", Applies::kAtOnce, &c.restart);
  if (GetBool("f2_crosshair_alpha_own"))
    c.dirty |= SliderDouble("Crosshair opacity", "f2_crosshair_alpha", 0.0, 1.0, "%.2f", Applies::kAtOnce, &c.restart);
  Help("How solid the game's own hip-fire crosshair is (1 = solid, 0 = invisible). Off: the game's own.");
}

void SendGameNumbers(const std::function<void(std::string)>& push) {
  struct Number {
    const char* own;      // the group's switch
    const char* setting;  // ours
    const char* dvar;     // the game's
    bool sent_own = false;
    double sent = 0.0;
  };
  static Number numbers[] = {
      {"f2_gun_own", "f2_gun_x", "cg_gun_x"},
      {"f2_gun_own", "f2_gun_y", "cg_gun_y"},
      {"f2_gun_own", "f2_gun_z", "cg_gun_z"},
      {"f2_crosshair_alpha_own", "f2_crosshair_alpha", "cg_crosshairAlpha"},
      {"f2_stick_sens_own", "f2_stick_sens", "input_viewSensitivity"},  // [f2-menus] 2026-11-10 21:00
      {"f2_mouse_sens_own", "f2_mouse_sens", "sensitivity"},
      {"f2_gamma_own", "f2_gamma", "r_gamma"},  // [f2-menus] 2026-11-10 22:30
      {"f2_dlights_own", "f2_dlights", "r_dlightLimit"},  // [f2-menus] 2026-11-11 07:35 Call of Duty 2
      {"f2_corpses_own", "f2_corpses", "ai_corpseCount"},  // [f2-menus] 2026-11-11 07:52 Call of Duty 2's campaign
      {"f2_brightness_own", "f2_brightness", "r_brightness"},
  };
  static std::chrono::steady_clock::time_point last_resend{};
  const auto time = std::chrono::steady_clock::now();
  const bool resend = time - last_resend > std::chrono::seconds(3);
  if (resend) last_resend = time;
  for (Number& n : numbers) {
    const bool own = GetBool(n.own);
    const double value = GetDouble(n.setting);
    if (!own) {
      if (n.sent_own) {
        n.sent_own = false;
        push(std::string("reset ") + n.dvar);
        REXLOG_INFO("f2: game number {} -> the game's own (reset)", n.dvar);
        ResetAgainAtLevelStart(n.dvar);  // [f2-menus] 00:15
      }
      continue;
    }
    const bool changed = !n.sent_own || value != n.sent;
    if (!changed && !resend) continue;
    if (!n.sent_own) ForgetResetAgain(n.dvar);  // [f2-menus] 00:15
    n.sent_own = true;
    n.sent = value;
    char text[64];
    std::snprintf(text, sizeof(text), "set %s %g", n.dvar, value);
    push(text);
    if (changed) REXLOG_INFO("f2: game number {} -> {:.3f}", n.dvar, value);
  }
}

// ------------------------------------------------------------------------------------------------- crosshair
namespace crosshair {

namespace {

// What the game's hook said last: when (0 = never) and in which color.
std::atomic<int64_t> g_asked_ns{0};
std::atomic<float> g_asked_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
std::atomic<float> g_asked_spread{0.0f};  // the game's 640 x 480 HUD units

// The capture of the game's own side pieces (the game's thread).
thread_local bool t_capturing = false;
thread_local int t_rects = 0;
thread_local float t_x[4] = {};
thread_local float t_width[4] = {};

float ReadGuestFloat(const uint8_t* base, uint32_t address) {
  uint32_t bits;
  std::memcpy(&bits, base + address, sizeof(bits));
  bits = __builtin_bswap32(bits);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}
// The game calls its crosshair drawing once a frame; past this long without a call the crosshair is gone (a menu,
// a cutscene, the sights raised).
constexpr int64_t kShownNs = 150'000'000;

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

struct Shape {
  int style;
  float size, thickness, gap, outline_thickness;
  bool outline, dot;
};

Shape CurrentShape() {
  return {REXCVAR_GET(f2_crosshair_style),
          float(REXCVAR_GET(f2_crosshair_size)),
          float(REXCVAR_GET(f2_crosshair_thickness)),
          float(REXCVAR_GET(f2_crosshair_gap)),
          float(REXCVAR_GET(f2_crosshair_outline_thickness)),
          REXCVAR_GET(f2_crosshair_outline),
          REXCVAR_GET(f2_crosshair_dot)};
}

// "r g b a" (0-255 each) to 0-1 floats; white when it can't be read.
void ParseColor(const std::string& text, float rgba[4]) {
  int v[4] = {255, 255, 255, 255};
  if (std::sscanf(text.c_str(), "%d %d %d %d", &v[0], &v[1], &v[2], &v[3]) < 3) v[0] = v[1] = v[2] = v[3] = 255;
  for (int i = 0; i < 4; ++i) rgba[i] = float(std::clamp(v[i], 0, 255)) / 255.0f;
}

// Call of Duty 3's shapes (ui/hud_overlay.cpp DrawCrosshairShape): `extra` widens every bar for the outline.
// `open` is how far the lines are pushed out (pixels: the dynamic spread). The four lines from `from` to `from + len`
// out of the middle, `t` thick.
void DrawArms(ImDrawList* dl, ImVec2 c, bool top, float from, float len, float t, ImU32 col) {
  if (len <= 0.0f) return;
  auto bar = [&](float x0, float y0, float x1, float y1) {
    dl->AddRectFilled(ImVec2(std::floor(x0), std::floor(y0)), ImVec2(std::floor(x1), std::floor(y1)), col);
  };
  if (top) bar(c.x - t / 2, c.y - from - len, c.x + t / 2, c.y - from);  // up
  bar(c.x - t / 2, c.y + from, c.x + t / 2, c.y + from + len);            // down
  bar(c.x - from - len, c.y - t / 2, c.x - from, c.y + t / 2);            // left
  bar(c.x + from, c.y - t / 2, c.x + from + len, c.y + t / 2);            // right
}

ImU32 WithAlpha(ImU32 col, float factor) {
  const ImU32 a = ImU32(float((col >> IM_COL32_A_SHIFT) & 0xFF) * std::clamp(factor, 0.0f, 1.0f) + 0.5f);
  return (col & ~IM_COL32_A_MASK) | (a << IM_COL32_A_SHIFT);
}

void DrawShape(ImDrawList* dl, ImVec2 c, const Shape& s, float scale, ImU32 col, float extra, float open = 0.0f) {
  const float t = s.thickness * scale + extra * 2.0f;
  const float len = s.size * scale + extra * 2.0f;
  const float gap = s.gap * scale - extra;
  auto bar = [&](float x0, float y0, float x1, float y1) {
    dl->AddRectFilled(ImVec2(std::floor(x0), std::floor(y0)), ImVec2(std::floor(x1), std::floor(y1)), col);
  };
  const bool cross = s.style == 0 || s.style == 1 || s.style == 4;
  if (cross && len > 0.0f) {
    const int dynamic = REXCVAR_GET(f2_crosshair_dynamic);
    const float split_at = float(REXCVAR_GET(f2_crosshair_split_distance)) * scale;
    if (dynamic == 2 && open > split_at) {
      // CS2's Classic: the inner part of each line stops at the split distance, the outer part goes on with the
      // spread, each with its own opacity.
      const float ratio = std::clamp(float(REXCVAR_GET(f2_crosshair_split_ratio)), 0.0f, 1.0f);
      const float core = s.size * scale;  // without the outline's widening
      const float inner = core * (1.0f - ratio) + extra * 2.0f;
      const float outer = core * ratio + extra * 2.0f;
      DrawArms(dl, c, s.style != 4, gap + split_at, inner, t,
               WithAlpha(col, float(REXCVAR_GET(f2_crosshair_split_alpha_inner))));
      DrawArms(dl, c, s.style != 4, gap + open + core * (1.0f - ratio), outer, t,
               WithAlpha(col, float(REXCVAR_GET(f2_crosshair_split_alpha_outer))));
    } else {
      DrawArms(dl, c, s.style != 4, gap + (dynamic != 0 ? open : 0.0f), len, t, col);
    }
  }
  if (s.style == 3)
    dl->AddCircle(c, std::max(2.0f, s.gap * scale + s.size * scale * 0.5f +
                                        (REXCVAR_GET(f2_crosshair_dynamic) != 0 ? open : 0.0f)),
                  col, 48, s.thickness * scale + extra * 2.0f);
  if (s.dot || s.style == 1 || s.style == 2) {
    const float d = std::max(1.0f, t);
    bar(c.x - d / 2, c.y - d / 2, c.x + d / 2, c.y + d / 2);
  }
}

void DrawAt(ImDrawList* dl, ImVec2 c, float scale, const float rgba[4], float open) {
  const Shape s = CurrentShape();
  if (s.outline)
    DrawShape(dl, c, s, scale, IM_COL32(0, 0, 0, int(rgba[3] * 255.0f)), s.outline_thickness * scale, open);
  DrawShape(dl, c, s, scale, ImGui::ColorConvertFloat4ToU32(ImVec4(rgba[0], rgba[1], rgba[2], rgba[3])), 0.0f, open);
}

// The game's color (white, red over an enemy, green over a friendly: CG_CalcCrosshairColor) replaced by the
// player's own for that case, keeping the game's alpha (its fades) times the own color's.
void ApplyOwnColor(float rgba[4]) {
  const char* setting = "f2_crosshair_color_normal";
  if (rgba[0] > 0.9f && rgba[1] < 0.6f && rgba[2] < 0.6f) setting = "f2_crosshair_color_enemy";
  if (rgba[1] > 0.9f && rgba[0] < 0.6f && rgba[2] < 0.6f) setting = "f2_crosshair_color_friendly";
  float own[4];
  ParseColor(Get(setting), own);
  rgba[0] = own[0];
  rgba[1] = own[1];
  rgba[2] = own[2];
  rgba[3] *= own[3];
}

}  // namespace

bool Enabled() { return REXCVAR_GET(f2_crosshair); }

void Notify(int view, const float rgba[4], float spread) {
  if (view != 0) return;
  for (int i = 0; i < 4; ++i) g_asked_color[i].store(rgba[i], std::memory_order_relaxed);
  g_asked_spread.store(std::isfinite(spread) ? std::clamp(spread, 0.0f, 240.0f) : 0.0f, std::memory_order_relaxed);
  g_asked_ns.store(NowNs(), std::memory_order_relaxed);
}

void CaptureBegin() {
  t_capturing = true;
  t_rects = 0;
}

bool Capturing() { return t_capturing; }

void CaptureRect(const uint8_t* base, uint32_t x_address, uint32_t width_address) {
  if (!t_capturing) return;
  if (t_rects < 4 && x_address >= 0x10000 && width_address >= 0x10000) {
    t_x[t_rects] = ReadGuestFloat(base, x_address);
    t_width[t_rects] = ReadGuestFloat(base, width_address);
  }
  ++t_rects;
}

int CaptureEnd(float* spread) {
  t_capturing = false;
  *spread = 0.0f;
  if (t_rects >= 4) *spread = (t_x[1] - t_x[3] - 1.125f * t_width[1]) * 0.5f;
  return t_rects;
}

bool ReadGuestColor(const uint8_t* base, uint32_t address, float rgba[4]) {
  if (address < 0x10000000) return false;  // a color is on the game's stack or in its data, never a small number
  float out[4];
  for (int i = 0; i < 4; ++i) {
    uint32_t bits;
    std::memcpy(&bits, base + address + 4 * i, sizeof(bits));
    bits = __builtin_bswap32(bits);
    std::memcpy(&out[i], &bits, sizeof(bits));
    if (!(out[i] >= 0.0f && out[i] <= 1.0f)) return false;
  }
  if (out[0] == 0.0f && out[1] == 0.0f && out[2] == 0.0f) return false;  // empty memory, not a crosshair color
  for (int i = 0; i < 4; ++i) rgba[i] = out[i];
  return true;
}

void Draw() {
  if (!Enabled()) return;
  const int64_t asked = g_asked_ns.load(std::memory_order_relaxed);
  const int64_t now = NowNs();
  if (asked == 0 || now - asked > kShownNs) return;
  float color[4];
  if (REXCVAR_GET(f2_crosshair_constant_color)) {
    ParseColor(REXCVAR_GET(f2_crosshair_color), color);
    color[3] *= g_asked_color[3].load(std::memory_order_relaxed);  // the game's fade stays
  } else {
    for (int i = 0; i < 4; ++i) color[i] = g_asked_color[i].load(std::memory_order_relaxed);
    if (REXCVAR_GET(f2_crosshair_own_colors)) ApplyOwnColor(color);
  }
  if (color[3] <= 0.0f) return;
  const ImGuiIO& io = ImGui::GetIO();
  const ImVec2 c(std::floor(io.DisplaySize.x * 0.5f), std::floor(io.DisplaySize.y * 0.5f));
  const float scale = std::max(0.5f, io.DisplaySize.y / 1080.0f);
  // The game's spread is in its 640 x 480 HUD units, which fill the picture's height.
  const float spread = g_asked_spread.load(std::memory_order_relaxed);
  const float open = spread * io.DisplaySize.y / 480.0f * float(REXCVAR_GET(f2_crosshair_dynamic_scale));
  DrawAt(ImGui::GetBackgroundDrawList(), c, scale, color, open);
  if (REXCVAR_GET(f2_debug_crosshair)) {
    static int64_t logged_ns = 0;
    if (now - logged_ns > 1'000'000'000) {
      logged_ns = now;
      REXLOG_INFO("f2: crosshair drawn at {:.0f},{:.0f} of {:.0f}x{:.0f}, scale {:.3f}, color {:.2f} {:.2f} {:.2f}, "
                  "alpha {:.2f}, the game's spread {:.2f} units = {:.1f} px open (mode {})",
                  c.x, c.y, io.DisplaySize.x, io.DisplaySize.y, scale, color[0], color[1], color[2], color[3], spread,
                  open, REXCVAR_GET(f2_crosshair_dynamic));
    }
  }
}

void DrawPage(PageContext& c) {
  static const char* const kSettings[] = {"f2_crosshair",         "f2_crosshair_style",   "f2_crosshair_size",
                                          "f2_crosshair_thickness", "f2_crosshair_gap",     "f2_crosshair_outline",
                                          "f2_crosshair_outline_thickness", "f2_crosshair_dot",
                                          "f2_crosshair_constant_color",    "f2_crosshair_color",
                                          "f2_crosshair_dynamic",           "f2_crosshair_dynamic_scale",
                                          "f2_crosshair_split_distance",    "f2_crosshair_split_ratio",
                                          "f2_crosshair_split_alpha_inner", "f2_crosshair_split_alpha_outer",
                                          "f2_crosshair_own_colors",        "f2_crosshair_color_normal",
                                          "f2_crosshair_color_enemy",       "f2_crosshair_color_friendly"};
  if (ResetButton("reset_crosshair", "crosshair settings")) {
    for (const char* name : kSettings) c.restart |= ResetSettings({name});
    c.dirty = true;
  }
  Section("Custom crosshair");
  c.dirty |= Check("Use my own crosshair", "f2_crosshair", Applies::kAtOnce, &c.restart);
  Help("Replaces the game's hip-fire crosshair with the one below, shown exactly when the game would show its own "
       "(not in menus or cutscenes, and it goes when you raise the sights). Mounted guns and scopes keep the game's.");

  // The preview: the crosshair at its in-game size for this window, on a dark box.
  {
    const float side = ImGui::GetFontSize() * 7.0f;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + side, p0.y + side);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(40, 44, 40, 255));
    dl->AddRect(p0, p1, IM_COL32(120, 120, 120, 255));
    float color[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    if (REXCVAR_GET(f2_crosshair_constant_color)) {
      ParseColor(REXCVAR_GET(f2_crosshair_color), color);
    } else if (REXCVAR_GET(f2_crosshair_own_colors)) {
      ApplyOwnColor(color);
    }
    if (!Enabled()) color[3] *= 0.4f;
    const float scale = std::max(0.5f, ImGui::GetIO().DisplaySize.y / 1080.0f);
    // The preview breathes in and out so the dynamic setting shows: 0 to 12 units of spread every 3 seconds.
    const float wave = 0.5f - 0.5f * std::cos(float(ImGui::GetTime()) * 2.0943951f);
    const float open = REXCVAR_GET(f2_crosshair_dynamic) != 0
                           ? wave * 12.0f * ImGui::GetIO().DisplaySize.y / 480.0f *
                                 float(REXCVAR_GET(f2_crosshair_dynamic_scale))
                           : 0.0f;
    dl->PushClipRect(p0, p1, true);
    DrawAt(dl, ImVec2(std::floor((p0.x + p1.x) * 0.5f), std::floor((p0.y + p1.y) * 0.5f)), scale, color, open);
    dl->PopClipRect();
    ImGui::Dummy(ImVec2(side, side));
  }

  Section("Shape");
  {
    const char* v[] = {"0", "1", "2", "3", "4"};
    const char* n[] = {"Cross", "Cross with a dot", "Dot", "Circle", "T (no top line)"};
    c.dirty |= Choice("Style", "f2_crosshair_style", v, n, 5, Applies::kAtOnce, &c.restart);
  }
  c.dirty |= SliderDouble("Length", "f2_crosshair_size", 0.0, 60.0, "%.1f", Applies::kAtOnce, &c.restart);
  c.dirty |= SliderDouble("Thickness", "f2_crosshair_thickness", 1.0, 12.0, "%.1f", Applies::kAtOnce, &c.restart);
  c.dirty |= SliderDouble("Gap", "f2_crosshair_gap", 0.0, 40.0, "%.1f", Applies::kAtOnce, &c.restart);
  c.dirty |= Check("Center dot", "f2_crosshair_dot", Applies::kAtOnce, &c.restart);
  c.dirty |= Check("Black outline", "f2_crosshair_outline", Applies::kAtOnce, &c.restart);
  if (REXCVAR_GET(f2_crosshair_outline))
    c.dirty |= SliderDouble("Outline thickness", "f2_crosshair_outline_thickness", 0.5, 4.0, "%.1f",
                            Applies::kAtOnce, &c.restart);
  Help("Sizes are in pixels on a 1080-line picture and grow with the window.");

  Section("Movement");
  {
    const char* v[] = {"0", "1", "2"};
    const char* n[] = {"Static", "Dynamic (the game's spread)", "Dynamic, split lines (CS2's Classic)"};
    c.dirty |= Choice("Behavior", "f2_crosshair_dynamic", v, n, 3, Applies::kAtOnce, &c.restart);
    Help("Static never moves (CS2's Classic Static). Dynamic opens and closes exactly as the game's own crosshair "
         "does: wider when you move or fire, tighter when you crouch or stand still (CS2's Classic Dynamic looks "
         "the same). Split lines: past the split distance the inner part of each line stays put and the outer "
         "part goes on, fainter, as CS2's Classic does.");
  }
  if (REXCVAR_GET(f2_crosshair_dynamic) != 0) {
    c.dirty |= SliderDouble("How far it opens", "f2_crosshair_dynamic_scale", 0.0, 3.0, "%.2f x", Applies::kAtOnce,
                            &c.restart);
    Help("1 is exactly the game's spread; lower keeps the crosshair tighter.");
  }
  if (REXCVAR_GET(f2_crosshair_dynamic) == 2) {
    c.dirty |= SliderDouble("Split distance", "f2_crosshair_split_distance", 0.0, 40.0, "%.1f", Applies::kAtOnce,
                            &c.restart);
    c.dirty |= SliderDouble("Outer share of the line", "f2_crosshair_split_ratio", 0.0, 1.0, "%.2f",
                            Applies::kAtOnce, &c.restart);
    c.dirty |= SliderDouble("Inner opacity", "f2_crosshair_split_alpha_inner", 0.0, 1.0, "%.2f", Applies::kAtOnce,
                            &c.restart);
    c.dirty |= SliderDouble("Outer opacity", "f2_crosshair_split_alpha_outer", 0.3, 1.0, "%.2f", Applies::kAtOnce,
                            &c.restart);
    Help("CS2's cl_crosshair_dynamic_splitdist, _maxdist_splitratio, _splitalpha_innermod and _outermod, with "
         "CS2's defaults (7, 0.35, 1, 0.5).");
  }

  Section("Color");
  c.dirty |= Check("One constant color", "f2_crosshair_constant_color", Applies::kAtOnce, &c.restart);
  Help("Off: the game's own color for the moment (white; red over an enemy and green over a friendly where the game "
       "does that). On: always the color below.");
  if (REXCVAR_GET(f2_crosshair_constant_color)) {
    float color[4];
    ParseColor(REXCVAR_GET(f2_crosshair_color), color);
    if (ImGui::ColorEdit4("Color", color, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) {
      char text[48];
      std::snprintf(text, sizeof(text), "%d %d %d %d", int(color[0] * 255.0f + 0.5f), int(color[1] * 255.0f + 0.5f),
                    int(color[2] * 255.0f + 0.5f), int(color[3] * 255.0f + 0.5f));
      rex::cvar::SetFlagByName("f2_crosshair_color", text);
      c.dirty = true;
    }
  } else {
    c.dirty |= Check("My own colors", "f2_crosshair_own_colors", Applies::kAtOnce, &c.restart);
    Help("Your colors in place of the game's white, red and green. The crosshair still changes when the game's "
         "does: over nothing, over an enemy, over a friendly.");
    if (REXCVAR_GET(f2_crosshair_own_colors)) {
      static const struct {
        const char* label;
        const char* setting;
      } kCases[] = {{"Over nothing", "f2_crosshair_color_normal"},
                    {"Over an enemy", "f2_crosshair_color_enemy"},
                    {"Over a friendly", "f2_crosshair_color_friendly"}};
      for (const auto& k : kCases) {
        float color[4];
        ParseColor(Get(k.setting), color);
        if (ImGui::ColorEdit4(k.label, color, ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf)) {
          char text[48];
          std::snprintf(text, sizeof(text), "%d %d %d %d", int(color[0] * 255.0f + 0.5f),
                        int(color[1] * 255.0f + 0.5f), int(color[2] * 255.0f + 0.5f), int(color[3] * 255.0f + 0.5f));
          rex::cvar::SetFlagByName(k.setting, text);
          c.dirty = true;
        }
      }
    }
  }
}

}  // namespace crosshair

}  // namespace f2
