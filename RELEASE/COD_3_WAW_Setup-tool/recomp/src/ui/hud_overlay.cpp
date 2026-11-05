// cod3 - custom crosshair HUD and the F2 "Recomp Settings" window.

#include "util.h"
#include "ui/hud_overlay.h"

#include <algorithm>
#include <array>  // [claude-9b] the crosshair preview's box
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstdlib>  // [claude-9b] std::atoi, the Display row
#include <initializer_list>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <imgui.h>

#include <rex/audio/output_device.h>
#include <rex/cvar.h>
#include <rex/hook.h>  // PPCRegister, for the midasm hooks below
#include <rex/ui/hdr_output.h>  // SDK patch 0044
#include <rex/logging.h>
#include <rex/system/kernel_state.h>  // [claude-9b] the game's crosshair option (F2 > Crosshair)

#include "app_defaults.h"
#include "game/aim_assist.h"
#include "game/cheat_watch.h"
#include "game/cutscene_skip.h"
#include "game/button_prompts.h"  // [acct2-decomp] key caps, the skip prompt's device
#include "game/color_codes.h"     // [claude-setup] World at War's ^ colors (Rainbow (^ colors), names)
#include "game/diagnostics.h"
#include "game/engine.h"
#include "game/fov.h"
#include "game/hd_textures.h"
#include "game/post_effects.h"  // [post-effects]
#include "game/program.h"
#include "game/mouse_look.h"
#include "game/keybinds/binds.h"     // [keybinds] both programs
#if !defined(COD3_MP)
#include "game/movement/settings.h"  // [movement] campaign only for now
#endif
#include "game/render_resolution.h"
#include "game/screenshot.h"
#include "game/shadows.h"
#include "game/viewmodel.h"  // [claude-9b] CS2 weapon position
#include "game/aim_sensitivity.h"  // [claude-9b] sensitivity per weapon (F2 > Controls)
#include "game/sticky_aim.h"  // [claude-9b] what applies now (F2 > Aim)
#include "ui/server_tab.h"  // [claude-9b] F2 > Game > Server, the match's rules, host only
#include "ui/slider_box.h"  // [claude-9b] sliders with a number box (owner: "sliders + textbox")
#include "game/ultrawide.h"
#include "game/voice_language.h"  // [voice]
#include "game/frame_rate.h"
#include "game/perf_stats.h"
#include "game/machine_profile.h"  // [hw-scaling]
#include "game/thread_profiler.h"  // [hw-scaling]
#include "game/guest.h"
#include "game/addresses.h"
#include "ui/announcement_feed.h"
#include "ui/console_pages.h"
#include "ui/console_view.h"
#include "ui/qte_spinner.h"
#include "ui/theme.h"

REXCVAR_DECLARE(bool, cod3_raw_mouse);
REXCVAR_DECLARE(bool, cod3_ultrawide_hud);
REXCVAR_DECLARE(bool, cod3_live_resolution);
REXCVAR_DECLARE(double, cod3_sensitivity);
REXCVAR_DECLARE(double, cod3_zoom_sensitivity);
REXCVAR_DECLARE(double, cod3_m_yaw);
REXCVAR_DECLARE(double, cod3_m_pitch);
REXCVAR_DECLARE(bool, cod3_perf_overlay);
REXCVAR_DECLARE(bool, cod3_invert_mouse);

// ---------------------------------------------------------------------------
// Host cvars (saved to cod3.toml with the rest of the settings)
// ---------------------------------------------------------------------------
REXCVAR_DEFINE_BOOL(cod3_crosshair, false, "cod3/Crosshair",
                    "Replace the game's crosshair with a customizable one");
// [claude-9b] 2026-10-27 owner 18:31: "please add a version number to the top right corner with toggle on/off.
// Shouldn't be intrusive."
REXCVAR_DEFINE_BOOL(cod3_show_version, true, "cod3/Interface",
                    "Show the setup tool version this game was built with, small and faint in the top right corner")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [claude-9b] 2026-10-27 tests: each view's custom crosshair in the log once a second (screenshots don't show it)
REXCVAR_DEFINE_BOOL(cod3_debug_crosshair, false, "cod3",
                    "Log once a second where each view's custom crosshair is drawn, its size and alpha (tests)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(cod3_crosshair_style, 0, "cod3/Crosshair",
                     "0 cross, 1 cross+dot, 2 dot, 3 circle, 4 T")
    .range(0, 4);
REXCVAR_DEFINE_DOUBLE(cod3_crosshair_size, 6.0, "cod3/Crosshair", "Arm length (px @1080p)")
    .range(0.0, 64.0);
REXCVAR_DEFINE_DOUBLE(cod3_crosshair_thickness, 2.0, "cod3/Crosshair", "Line thickness (px @1080p)")
    .range(0.5, 16.0);
REXCVAR_DEFINE_DOUBLE(cod3_crosshair_gap, 4.0, "cod3/Crosshair", "Gap from center (px @1080p)")
    .range(-8.0, 64.0);
REXCVAR_DEFINE_BOOL(cod3_crosshair_outline, true, "cod3/Crosshair", "Black outline");
REXCVAR_DEFINE_DOUBLE(cod3_crosshair_outline_thickness, 1.0, "cod3/Crosshair",
                      "Outline thickness (px @1080p)")
    .range(0.5, 4.0);
REXCVAR_DEFINE_BOOL(cod3_crosshair_dot, false, "cod3/Crosshair", "Center dot");
REXCVAR_DEFINE_BOOL(cod3_crosshair_constant_color, false, "cod3/Crosshair",
                    "Always draw the custom crosshair in cod3_crosshair_color. Off: use the "
                    "game's own colors (white, red over an enemy, green over a friendly, "
                    "black over a dead body)");
REXCVAR_DEFINE_STRING(cod3_crosshair_color, "0 255 0 255", "cod3/Crosshair",
                      "R G B A (0-255)");
// [claude-9b] Colors of the player's own in place of the game's four (owner 2026-10-26: "Allow for
// custom colors that switch like the default ones but allowing for custom individual colors that
// replace them"). The crosshair still changes when the game's does (over nothing, an enemy, a
// friendly, a dead body); each case shows its color below. Ignored while cod3_crosshair_constant_color.
REXCVAR_DEFINE_BOOL(cod3_crosshair_own_colors, false, "cod3/Crosshair",
                    "Use cod3_crosshair_color_normal / _enemy / _friendly / _dead in place of the "
                    "game's white, red, green and black");
REXCVAR_DEFINE_STRING(cod3_crosshair_color_normal, "255 255 255 255", "cod3/Crosshair",
                      "R G B A over nothing (the game's white)");
REXCVAR_DEFINE_STRING(cod3_crosshair_color_enemy, "255 0 0 255", "cod3/Crosshair",
                      "R G B A over an enemy (the game's red)");
REXCVAR_DEFINE_STRING(cod3_crosshair_color_friendly, "64 255 64 255", "cod3/Crosshair",
                      "R G B A over a friendly (the game's green)");
REXCVAR_DEFINE_STRING(cod3_crosshair_color_dead, "0 0 0 255", "cod3/Crosshair",
                      "R G B A over a dead body (the game's black)");

// [claude-9b-think] 2026-10-27 UNTESTED (owner 13:59: "forgot to add the option to change the color of the glyphs if
// the user wants", "default button is needed tho"): the colors of our prompt pictures, hex RRGGBB or RRGGBBAA (a '#'
// in front allowed; one that doesn't parse counts as the default). F2 > Game > Prompts. Both programs.
// [claude-setup] 2026-11-03 (owner ~09:55: "the toggle for the button colors seems backwards in logic. By default it
// should show the button as if it were the original, the toggle enables a customized color"; ~10:00: "also add
// rainbow option"): which colors the pictures use. Replaces cod3_glyph_colors (off: the gold key-cap style, on: the
// colors below), so the controller pictures look like the controller's own buttons by default and the colors below
// are used only when asked for. An old cod3_glyph_colors line in cod3.toml is ignored (the SDK logs it as unknown).
REXCVAR_DEFINE_STRING(cod3_glyph_color_mode, "original", "cod3/Interface",
                      "Colors of our prompt pictures: original (the controller's own button colors, the key caps "
                      "in the game's menu gold), custom (the cod3_glyph_color* settings), rainbow (slowly through the "
                      "rainbow), rainbow_codes (stepping through World at War's ^ colors) or gold (controller buttons "
                      "in the key caps' gold style too)")
    .allowed({"original", "custom", "rainbow", "rainbow_codes", "gold"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(cod3_glyph_color, "D6AC68", "cod3/Interface",
                      "With cod3_glyph_color_mode custom: our prompt pictures' accent, the key caps' edge and "
                      "name, the mouse and the skip prompt; the game's menu gold by default (hex RRGGBB or RRGGBBAA)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(cod3_glyph_fill, "14100ACC", "cod3/Interface",
                      "With cod3_glyph_color_mode custom: the inside of the key caps, with its opacity (hex RRGGBB "
                      "or RRGGBBAA)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(cod3_glyph_color_ps_cross, "7CACEA", "cod3/Interface",
                      "Colored buttons: the PlayStation cross (hex RRGGBB or RRGGBBAA)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(cod3_glyph_color_ps_circle, "EE6060", "cod3/Interface",
                      "Colored buttons: the PlayStation circle (hex RRGGBB or RRGGBBAA)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(cod3_glyph_color_ps_square, "EC84D6", "cod3/Interface",
                      "Colored buttons: the PlayStation square (hex RRGGBB or RRGGBBAA)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(cod3_glyph_color_ps_triangle, "48CEA0", "cod3/Interface",
                      "Colored buttons: the PlayStation triangle (hex RRGGBB or RRGGBBAA)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(cod3_glyph_color_gc_a, "2EA860", "cod3/Interface",
                      "Colored buttons: GameCube A (hex RRGGBB or RRGGBBAA)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(cod3_glyph_color_gc_b, "CC3A3A", "cod3/Interface",
                      "Colored buttons: GameCube B (hex RRGGBB or RRGGBBAA)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(cod3_glyph_color_gc_x, "96969E", "cod3/Interface",
                      "Colored buttons: GameCube X (hex RRGGBB or RRGGBBAA)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(cod3_glyph_color_gc_y, "96969E", "cod3/Interface",
                      "Colored buttons: GameCube Y (hex RRGGBB or RRGGBBAA)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(cod3_glyph_color_gc_z, "7850B0", "cod3/Interface",
                      "Colored buttons: GameCube Z (hex RRGGBB or RRGGBBAA)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(cod3_glyph_color_nx_a, "303034E6", "cod3/Interface",
                      "Colored buttons: Nintendo A (hex RRGGBB or RRGGBBAA)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(cod3_glyph_color_nx_b, "303034E6", "cod3/Interface",
                      "Colored buttons: Nintendo B (hex RRGGBB or RRGGBBAA)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(cod3_glyph_color_nx_x, "303034E6", "cod3/Interface",
                      "Colored buttons: Nintendo X (hex RRGGBB or RRGGBBAA)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(cod3_glyph_color_nx_y, "303034E6", "cod3/Interface",
                      "Colored buttons: Nintendo Y (hex RRGGBB or RRGGBBAA)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(cod3_render_resolution, "auto", "cod3/Display",
                      "Render resolution: auto (the screen, or the window when windowed), 720p, "
                      "1080p, 1440p, 2160p, 4320p, WxH or original (the Xbox 360's 1040x624). "
                      "Natively the game draws it itself, no MSAA, no scaling; above what the "
                      "emulated EDRAM holds the GPU's draw scale adds the rest "
                      "(cod3_render_method, game/render_resolution.h). Applies at once (a live "
                      "resolution change).");

REXCVAR_DEFINE_STRING(cod3_graphics_api, "auto", "cod3/Display",
                      "Windows: the renderer, \"d3d12\" or \"vulkan\" (\"auto\" = Vulkan, and "
                      "Direct3D 12 when Vulkan can't start); Linux always uses Vulkan")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
REXCVAR_DEFINE_DOUBLE(cod3_refresh_rate, 0.0, "cod3/Display",
                      "Fullscreen refresh rate in Hz (0 = the desktop's own, borderless); "
                      "applies at once")
    .range(0.0, 1000.0);
REXCVAR_DEFINE_INT32(cod3_max_fps, 60, "cod3/Display",
                     "Frame rate limit (com_maxfps; 0 = no limit). F2, the game's Options and "
                     "com_maxfps in the console all change it; the game starts with it.")
    .range(0, 1000);

namespace cod3::ui {

namespace {

// [claude-9b] What the game's CG_DrawCrosshair hooks said for each local player's view (written by
// the guest thread, read when the overlay draws). The campaign has only view 0, the whole picture.
constexpr int kCrosshairViews = 4;
struct GameCrosshairView {
  std::atomic<int64_t> ns{0};       // when the game last wanted this view's crosshair
  std::atomic<float> aim{0.0f};     // 0 hip fire .. 1 aiming down sights
  std::atomic<float> color[4] = {1.0f, 1.0f, 1.0f, 1.0f};  // the game's crosshair color
  std::atomic<float> rect[4] = {0.0f, 0.0f, 1.0f, 1.0f};   // x, y, width, height on the picture (0-1)
};
GameCrosshairView g_crosshair_views[kCrosshairViews];
// The view the next NotifyGameCrosshair is for (SetGameCrosshairView, same guest thread).
int g_next_view = 0;
float g_next_rect[4] = {0.0f, 0.0f, 1.0f, 1.0f};
constexpr int64_t kCrosshairShownNs = 150'000'000;

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

bool Has(const char* name) { return rex::cvar::GetFlagInfo(name) != nullptr; }

std::string Get(const char* name) { return rex::cvar::GetFlagByName(name); }

bool GetBool(const char* name) {
  auto v = Get(name);
  return v == "true" || v == "1";
}
int GetInt(const char* name, int fallback = 0) {
  auto v = Get(name);
  return v.empty() ? fallback : std::atoi(v.c_str());
}
double GetDouble(const char* name, double fallback = 0) {
  auto v = Get(name);
  return v.empty() ? fallback : std::strtod(v.c_str(), nullptr);
}

// [claude-9b] "R G B A" (0-255) as floats 0-1; `fallback` when it doesn't parse.
std::array<float, 4> ParseRgba(const std::string& text, std::array<float, 4> fallback) {
  int r, g, b, a;
  if (std::sscanf(text.c_str(), "%d %d %d %d", &r, &g, &b, &a) != 4) return fallback;
  return {std::clamp(r, 0, 255) / 255.0f, std::clamp(g, 0, 255) / 255.0f, std::clamp(b, 0, 255) / 255.0f,
          std::clamp(a, 0, 255) / 255.0f};
}

// [claude-9b-think] 2026-10-27 UNTESTED the prompt pictures' colors (the cod3_glyph_color* settings above).
// "RRGGBB" or "RRGGBBAA" (a '#' in front and spaces around allowed) as 0xRRGGBBAA; false when it doesn't parse.
bool ParseHexColor(const std::string& text, uint32_t* rgba) {
  const size_t b = text.find_first_not_of(" \t");
  if (b == std::string::npos) return false;
  const size_t e = text.find_last_not_of(" \t");
  std::string_view t(text.data() + b, e - b + 1);
  if (t.front() == '#') t.remove_prefix(1);
  if (t.size() != 6 && t.size() != 8) return false;
  uint32_t v = 0;
  for (char ch : t) {
    const char l = char(ch | 0x20);
    const int d = ch >= '0' && ch <= '9' ? ch - '0' : l >= 'a' && l <= 'f' ? l - 'a' + 10 : -1;
    if (d < 0) return false;
    v = v * 16 + uint32_t(d);
  }
  *rgba = t.size() == 6 ? (v << 8) | 0xFFu : v;
  return true;
}

std::string HexColor(const float c[4], bool alpha) {
  auto byte = [](float f) { return std::clamp(int(f * 255.0f + 0.5f), 0, 255); };
  char buf[16];
  if (alpha)
    std::snprintf(buf, sizeof(buf), "%02X%02X%02X%02X", byte(c[0]), byte(c[1]), byte(c[2]), byte(c[3]));
  else
    std::snprintf(buf, sizeof(buf), "%02X%02X%02X", byte(c[0]), byte(c[1]), byte(c[2]));
  return buf;
}

enum GlyphColorId : int {
  kGlyphAccent, kGlyphFill,
  kPsCross, kPsCircle, kPsSquare, kPsTriangle,
  kGcA, kGcB, kGcX, kGcY, kGcZ,
  kNxA, kNxB, kNxX, kNxY,
  kGlyphColorCount
};
struct GlyphColorSetting {
  const char* cvar;
  const char* label;
  const char* def;  // the cvar's default; 8 digits: the color has an opacity of its own
};
constexpr GlyphColorSetting kGlyphColorSettings[kGlyphColorCount] = {
    {"cod3_glyph_color", "Accent", "D6AC68"},
    {"cod3_glyph_fill", "Cap fill", "14100ACC"},
    {"cod3_glyph_color_ps_cross", "Cross", "7CACEA"},
    {"cod3_glyph_color_ps_circle", "Circle", "EE6060"},
    {"cod3_glyph_color_ps_square", "Square", "EC84D6"},
    {"cod3_glyph_color_ps_triangle", "Triangle", "48CEA0"},
    {"cod3_glyph_color_gc_a", "A", "2EA860"},
    {"cod3_glyph_color_gc_b", "B", "CC3A3A"},
    {"cod3_glyph_color_gc_x", "X", "96969E"},
    {"cod3_glyph_color_gc_y", "Y", "96969E"},
    {"cod3_glyph_color_gc_z", "Z", "7850B0"},
    {"cod3_glyph_color_nx_a", "A", "303034E6"},
    {"cod3_glyph_color_nx_b", "B", "303034E6"},
    {"cod3_glyph_color_nx_x", "X", "303034E6"},
    {"cod3_glyph_color_nx_y", "Y", "303034E6"},
};

// The color setting `id` holds (F2's rows show and change it).
uint32_t GlyphSettingRgba(int id) {
  const GlyphColorSetting& g = kGlyphColorSettings[id];
  uint32_t v = 0;
  if (!ParseHexColor(Get(g.cvar), &v)) ParseHexColor(g.def, &v);
  return v;
}
uint32_t GlyphDefaultRgba(int id) {
  uint32_t v = 0;
  ParseHexColor(kGlyphColorSettings[id].def, &v);
  return v;
}

// [claude-setup] 2026-11-03 cod3_glyph_color_mode (its definition above says why).
enum class GlyphMode : uint8_t { kOriginal, kCustom, kRainbow, kRainbowCodes, kGold };
GlyphMode CurrentGlyphMode() {
  const std::string m = Get("cod3_glyph_color_mode");
  if (m == "custom") return GlyphMode::kCustom;
  if (m == "rainbow") return GlyphMode::kRainbow;
  if (m == "rainbow_codes") return GlyphMode::kRainbowCodes;
  if (m == "gold") return GlyphMode::kGold;
  return GlyphMode::kOriginal;
}

// Rainbow: once round the color wheel every kRainbowSeconds of the overlay's own clock (the game's speed is never
// touched), each button of a controller a step further round so no two share a color; bright but not glaring, and
// the color keeps its own opacity (the Nintendo buttons'). Slow and smooth: no flashing.
constexpr double kRainbowSeconds = 6.0;
uint32_t RainbowRgba(int id) {
  static constexpr float kStep[kGlyphColorCount] = {0.0f,  0.0f,                       // accent, fill (not used)
                                                    0.0f,  0.25f, 0.5f, 0.75f,         // cross circle square triangle
                                                    0.0f,  0.2f,  0.4f, 0.6f, 0.8f,    // GameCube A B X Y Z
                                                    0.0f,  0.25f, 0.5f, 0.75f};        // Nintendo A B X Y
  const double t = ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0;
  const float h = float(std::fmod(t / kRainbowSeconds + double(kStep[id]), 1.0));
  float r = 1.0f, g = 1.0f, b = 1.0f;
  ImGui::ColorConvertHSVtoRGB(h, 0.70f, 1.0f, r, g, b);
  auto byte = [](float f) { return uint32_t(std::clamp(int(f * 255.0f + 0.5f), 0, 255)); };
  return (byte(r) << 24) | (byte(g) << 16) | (byte(b) << 8) | (GlyphDefaultRgba(id) & 0xFFu);
}

// [claude-setup] 2026-11-03 (owner: "similar to ^: from WAW"; asked, "Both as choices"): the rainbow as World at War's
// rainbow names do it, stepping through its ^ colors (game/color_codes.h: red, orange, yellow, green, cyan, blue)
// every kRainbowCodeSeconds, each button of a controller one step on; the color keeps its own opacity.
constexpr double kRainbowCodeSeconds = 0.5;
uint32_t RainbowCodesRgba(int id) {
  namespace cc = cod3::color_codes;
  static constexpr int kStep[kGlyphColorCount] = {0, 0, 0, 1, 2, 3, 0, 1, 2, 3, 4, 0, 1, 2, 3};
  const double t = ImGui::GetCurrentContext() ? ImGui::GetTime() : 0.0;
  constexpr int kCount = int(sizeof(cc::kRainbowCodes) / sizeof(cc::kRainbowCodes[0]));
  const int64_t n = int64_t(std::floor(t / kRainbowCodeSeconds)) + kStep[id];
  const int code = cc::kRainbowCodes[int(((n % kCount) + kCount) % kCount)];
  return (cc::kCodeRgba[code] & 0xFFFFFF00u) | (GlyphDefaultRgba(id) & 0xFFu);
}

// The color the pictures are drawn in: the setting's own only in custom mode; rainbow for everything but the key
// caps' fill (kept dark so the names stay readable); else the original.
uint32_t GlyphRgba(int id) {
  switch (CurrentGlyphMode()) {
    case GlyphMode::kCustom: return GlyphSettingRgba(id);
    case GlyphMode::kRainbow:
      if (id != kGlyphFill) return RainbowRgba(id);
      break;
    case GlyphMode::kRainbowCodes:
      if (id != kGlyphFill) return RainbowCodesRgba(id);
      break;
    default: break;
  }
  return GlyphDefaultRgba(id);
}
// Color `id` at the picture's opacity `a` (0-255): its own opacity times `a`.
ImU32 GlyphColor(int id, int a) {
  const uint32_t v = GlyphRgba(id);
  return IM_COL32(v >> 24, (v >> 16) & 0xFF, (v >> 8) & 0xFF, int((v & 0xFF) * uint32_t(std::clamp(a, 0, 255)) / 255));
}
bool GlyphColorIsDefault(int id) { return GlyphRgba(id) == GlyphDefaultRgba(id); }

// [claude-setup] 2026-11-03 Text with World at War's ^0-^9 and ^: color codes (game/color_codes.h; owner: "names and
// other user supplied text things can inherit those various color codes. ^0-9", "rainbow code is "^:" with the
// colon") drawn in its colors as one ImGui item at the cursor: the codes aren't printed, text before the first code is
// in the window's text color, a ^: rainbow steps one color a character and moves on every half second. A dark run
// (^0, ^4) gets a light edge, so a name is never invisible on a dark background. Bounds-checked for any bytes.
void CodedText(std::string_view text) {
  namespace cc = cod3::color_codes;
  ImFont* font = ImGui::GetFont();
  const float size = ImGui::GetFontSize();
  const ImVec2 start = ImGui::GetCursorScreenPos();
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const double t = ImGui::GetTime();
  const ImU32 normal = ImGui::GetColorU32(ImGuiCol_Text);
  float x = start.x;
  int64_t rainbow_n = 0;
  auto draw = [&](std::string_view s, uint32_t rgba, bool own) {
    if (s.empty()) return;
    const char* b = s.data();
    const char* e = b + s.size();
    const ImU32 col = own ? normal : IM_COL32(rgba >> 24, (rgba >> 16) & 0xFF, (rgba >> 8) & 0xFF, 255);
    const float luma = own ? 1.0f
                           : (0.299f * float(rgba >> 24) + 0.587f * float((rgba >> 16) & 0xFF) +
                              0.114f * float((rgba >> 8) & 0xFF)) / 255.0f;
    if (luma < 0.3f) {
      static constexpr float kEdge[4][2] = {{-1, 0}, {1, 0}, {0, -1}, {0, 1}};
      for (const auto& o : kEdge)
        dl->AddText(font, size, ImVec2(x + o[0], start.y + o[1]), IM_COL32(220, 220, 220, 200), b, e);
    }
    dl->AddText(font, size, ImVec2(x, start.y), col, b, e);
    x += font->CalcTextSizeA(size, FLT_MAX, 0.0f, b, e).x;
  };
  cc::ForEachRun(text, [&](std::string_view run, int code) {
    if (code == cc::kRainbow) {
      for (size_t i = 0; i < run.size(); ++i) draw(run.substr(i, 1), cc::RainbowRgba(t, rainbow_n++), false);
    } else {
      draw(run, code >= 0 && code < 10 ? cc::kCodeRgba[code] : 0u, code < 0 || code >= 10);
    }
  });
  ImGui::Dummy(ImVec2(std::max(x - start.x, 1.0f), size));
}

// [claude-setup] 2026-11-03 Under a name field: the name as the game will be given it (made safe), in its colors,
// when it has color codes or the rule changed it; nothing for a plain name.
void NameShowsAs(const char* typed, size_t max_bytes) {
  namespace cc = cod3::color_codes;
  const std::string_view raw(typed);
  if (raw.empty()) return;
  const std::string safe = cc::SanitizeName(raw, max_bytes);
  const bool coded = safe.find('^') != std::string::npos;
  if (!coded && safe == raw) return;
  ImGui::Indent();
  ImGui::TextDisabled("Shows as:");
  ImGui::SameLine();
  if (safe.empty())
    ImGui::TextDisabled("(nothing visible: the game's own name)");
  else
    CodedText(safe);
  ImGui::Unindent();
}

// [claude-9b] The game's crosshair color (white, red, green or black: CG_CalcCrosshairColor,
// addresses.h `crosshair`) replaced by the player's own for that case, keeping the game's alpha.
void ApplyOwnCrosshairColor(float rgba[4]) {
  struct Case {
    float r, g, b;
    const char* setting;
  };
  static constexpr Case kCases[] = {{1.0f, 1.0f, 1.0f, "cod3_crosshair_color_normal"},
                                    {1.0f, 0.0f, 0.0f, "cod3_crosshair_color_enemy"},
                                    {0.25f, 1.0f, 0.25f, "cod3_crosshair_color_friendly"},
                                    {0.0f, 0.0f, 0.0f, "cod3_crosshair_color_dead"}};
  int best = 0;
  float best_d = 1e9f;
  for (int i = 0; i < 4; ++i) {
    const float dr = rgba[0] - kCases[i].r, dg = rgba[1] - kCases[i].g, db = rgba[2] - kCases[i].b;
    const float d = dr * dr + dg * dg + db * db;
    if (d < best_d) best_d = d, best = i;
  }
  const auto own = ParseRgba(Get(kCases[best].setting), {kCases[best].r, kCases[best].g, kCases[best].b, 1.0f});
  rgba[0] = own[0];
  rgba[1] = own[1];
  rgba[2] = own[2];
  rgba[3] *= own[3];
}

// [claude-9b] The game's own Options > Gameplay > Crosshair (player 1's control settings,
// addresses.h `controls`; in cod3mp the first profile's block). -1 while the game isn't running.
uint8_t* GuestBase() {
  auto* ks = rex::system::kernel_state();
  return ks && ks->memory() ? ks->memory()->virtual_membase() : nullptr;
}
int GameCrosshairOption() {
  uint8_t* base = GuestBase();
  if (!base) return -1;
  return cod3::guest::ReadU8(base, cod3::addr::controls::kSettings + cod3::addr::controls::kCrosshair) != 0;
}
void SetGameCrosshairOption(bool on) {
  if (uint8_t* base = GuestBase())
    cod3::guest::WriteU8(base, cod3::addr::controls::kSettings + cod3::addr::controls::kCrosshair, on ? 1 : 0);
}

#if defined(COD3_MP)
// [claude-setup] 2026-11-03 (owner ~10:10: "draw crosshair needs to be player independent. Currently when the custom
// crosshair is on and you toggle visibility the custom crosshair is still shown on the splitscreen player's screen";
// asked, he chose one "Show crosshair" box per split-screen player in F2). The game decides each view's crosshair
// from that local player's own switch (sub_824528A0, which CG_DrawCrosshair checks at 0x8245DD40 and the mounted
// guns' crosshair at 0x82452D10: the local client in use, byte +212 of its block, then byte +57 of the profile block
// of the controller it plays with, +180), and the custom crosshair is drawn only where the game would draw its own,
// so it follows each player's switch; F2's single box changed only the first profile's. The local players in the
// game, each with the address of its crosshair byte:
struct LocalCrosshair {
  uint32_t player;  // the local client, 0-3
  uint32_t byte;    // its controller's profile block + kCrosshair
};
constexpr uint32_t kLocalClientInUse = 212;  // u8 (sub_824528A0: lbzx at +212 of the local client's block)
constexpr uint32_t kProfileStride = 6984;    // the profiles' blocks, one per controller (game/mp/controls_mp.cpp)
std::vector<LocalCrosshair> LocalCrosshairs(uint8_t* base) {
  namespace kb = cod3::addr::keybinds;
  std::vector<LocalCrosshair> out;
  for (uint32_t c = 0; c < 4; ++c) {
    const uint32_t client = kb::kLocalClients + c * kb::kLocalClientStride;
    if (cod3::guest::ReadU8(base, client + kLocalClientInUse) == 0) continue;
    const uint32_t pad = cod3::guest::ReadU32(base, client + kb::kLocalClientPad);
    if (pad >= 4) continue;
    out.push_back({c, cod3::addr::controls::kSettings + pad * kProfileStride + cod3::addr::controls::kCrosshair});
  }
  return out;
}
#endif

}  // namespace

void SetGameCrosshairView(int view, float x, float y, float width, float height) {
  g_next_view = std::clamp(view, 0, kCrosshairViews - 1);
  const float r[4] = {x, y, width, height};
  const bool ok = std::isfinite(x) && std::isfinite(y) && width > 0.0f && height > 0.0f && x >= -0.01f &&
                  y >= -0.01f && x + width <= 1.01f && y + height <= 1.01f;
  for (int i = 0; i < 4; ++i) g_next_rect[i] = ok ? r[i] : (i < 2 ? 0.0f : 1.0f);  // else the whole picture
}

void NotifyGameCrosshair(float aim_fraction, const float rgba[4]) {
  GameCrosshairView& v = g_crosshair_views[g_next_view];
  for (int i = 0; i < 4; ++i) {
    float c = std::isfinite(rgba[i]) ? std::clamp(rgba[i], 0.0f, 1.0f) : 1.0f;
    v.color[i].store(c, std::memory_order_relaxed);
    v.rect[i].store(g_next_rect[i], std::memory_order_relaxed);
  }
  if (!std::isfinite(aim_fraction)) aim_fraction = 0.0f;
  v.aim.store(std::clamp(aim_fraction, 0.0f, 1.0f), std::memory_order_relaxed);
  v.ns.store(NowNs(), std::memory_order_relaxed);
  // The next call is player 1's whole picture again unless its hook says otherwise.
  g_next_view = 0;
  g_next_rect[0] = g_next_rect[1] = 0.0f;
  g_next_rect[2] = g_next_rect[3] = 1.0f;
}

bool GameCrosshairVisible() {
  const int64_t now = NowNs();
  for (const auto& v : g_crosshair_views)
    if (now - v.ns.load(std::memory_order_relaxed) <= kCrosshairShownNs) return true;
  return false;
}

// [claude-9b] The crosshair preview's box (hud_overlay.h), in menu space.
namespace {
std::atomic<int64_t> g_preview_ns{0};
std::array<std::atomic<float>, 4> g_preview_box{};
}  // namespace

void ShowCrosshairPreview(float x0, float y0, float x1, float y1) {
  const float box[4] = {x0, y0, x1, y1};
  for (int i = 0; i < 4; ++i) g_preview_box[i].store(std::isfinite(box[i]) ? box[i] : 0.0f, std::memory_order_relaxed);
  g_preview_ns.store(NowNs(), std::memory_order_relaxed);
}

CrosshairStyle CurrentCrosshair() {
  CrosshairStyle s{};
  s.enabled = REXCVAR_GET(cod3_crosshair);
  s.style = REXCVAR_GET(cod3_crosshair_style);
  s.size = static_cast<float>(REXCVAR_GET(cod3_crosshair_size));
  s.thickness = static_cast<float>(REXCVAR_GET(cod3_crosshair_thickness));
  s.gap = static_cast<float>(REXCVAR_GET(cod3_crosshair_gap));
  s.outline = REXCVAR_GET(cod3_crosshair_outline);
  s.outline_thickness = static_cast<float>(REXCVAR_GET(cod3_crosshair_outline_thickness));
  s.dot = REXCVAR_GET(cod3_crosshair_dot);
  s.constant_color = REXCVAR_GET(cod3_crosshair_constant_color);
  int r = 0, g = 255, b = 0, a = 255;
  std::sscanf(REXCVAR_GET(cod3_crosshair_color).c_str(), "%d %d %d %d", &r, &g, &b, &a);
  s.color[0] = r / 255.0f;
  s.color[1] = g / 255.0f;
  s.color[2] = b / 255.0f;
  s.color[3] = a / 255.0f;
  return s;
}

static void DrawCrosshairShape(ImDrawList* dl, ImVec2 c, const CrosshairStyle& s, float scale,
                               ImU32 col, float extra) {
  const float t = s.thickness * scale + extra * 2.0f;
  const float len = s.size * scale + extra * 2.0f;
  const float gap = s.gap * scale - extra;
  auto bar = [&](float x0, float y0, float x1, float y1) {
    dl->AddRectFilled(ImVec2(std::floor(x0), std::floor(y0)), ImVec2(std::floor(x1), std::floor(y1)),
                      col);
  };
  const bool cross = s.style == 0 || s.style == 1 || s.style == 4;
  if (cross && len > 0.0f) {
    if (s.style != 4) bar(c.x - t / 2, c.y - gap - len, c.x + t / 2, c.y - gap);  // up
    bar(c.x - t / 2, c.y + gap, c.x + t / 2, c.y + gap + len);                       // down
    bar(c.x - gap - len, c.y - t / 2, c.x - gap, c.y + t / 2);                       // left
    bar(c.x + gap, c.y - t / 2, c.x + gap + len, c.y + t / 2);                       // right
  }
  if (s.style == 3) {
    dl->AddCircle(c, std::max(2.0f, s.gap * scale + s.size * scale * 0.5f), col, 48,
                  s.thickness * scale + extra * 2.0f);
  }
  if (s.dot || s.style == 1 || s.style == 2) {
    float d = std::max(1.0f, t);
    bar(c.x - d / 2, c.y - d / 2, c.x + d / 2, c.y + d / 2);
  }
}

// [claude-9b] The custom crosshair as a preview: its shape, outline and color
// (white for the game's own colors: what it shows over nothing), at the HUD's
// scale; dimmed while the custom crosshair is off.
static void DrawCrosshairPreviewShape(ImDrawList* dl, ImVec2 c, float scale) {
  CrosshairStyle s = CurrentCrosshair();
  if (!s.constant_color) s.color[0] = s.color[1] = s.color[2] = s.color[3] = 1.0f;
  const float alpha = s.color[3] * (s.enabled ? 1.0f : 0.35f);
  if (s.outline)
    DrawCrosshairShape(dl, c, s, scale, IM_COL32(0, 0, 0, static_cast<int>(alpha * 255)), s.outline_thickness * scale);
  DrawCrosshairShape(dl, c, s, scale,
                     ImGui::ColorConvertFloat4ToU32(ImVec4(s.color[0], s.color[1], s.color[2], alpha)), 0.0f);
}

// cg_drawFPS: top-right corner, like World at War.
//   1 frame rate, 2 + slowest / fastest over the last half second,
//   3 + average / longest frame time and the limit.
// [claude-9b] 2026-10-27 the version label's height when it is shown (cod3_show_version): the frame rate and the
// frame cost lines start that much lower, so the label stays the top right corner's first line.
static float g_top_right_offset = 0.0f;

// [claude-9b] 2026-10-27 The setup tool version the game was built with (game/diagnostics.h ToolVersion, the same
// as the log's session line). A game built some other way shows the date it was compiled.
static const std::string& VersionText() {
  static const std::string text = [] {
    const std::string v = cod3::diag::ToolVersion();
    return v.empty() ? std::string("build ") + __DATE__ : "v" + v;
  }();
  return text;
}

// [claude-9b] 2026-10-27 cod3_show_version: the version in the top right corner, small and faint, with a soft
// shadow so it reads on bright and dark pictures. Returns the height it takes (0 when hidden).
static float DrawVersionLabel(ImGuiIO& io) {
  if (!REXCVAR_GET(cod3_show_version)) return 0.0f;
  const std::string& text = VersionText();
  const float size = std::max(10.0f, 13.0f * io.DisplaySize.y / 1080.0f);
  ImFont* font = FontFor(size);
  const ImVec2 ext = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
  const float margin = std::floor(12.0f * io.DisplaySize.y / 1080.0f);
  const ImVec2 pos(std::floor(io.DisplaySize.x - ext.x - margin), std::floor(margin * 0.5f));
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddText(font, size, ImVec2(pos.x + 1, pos.y + 1), IM_COL32(0, 0, 0, 90), text.c_str());
  dl->AddText(font, size, pos, IM_COL32(255, 255, 255, 105), text.c_str());
  return std::max(0.0f, std::floor(pos.y + ext.y + 2.0f - margin));  // the frame rate starts below the label
}

static void DrawFpsCounter(ImGuiIO& io, int mode) {
  const auto st = cod3::FrameRate::Get().Stats();
  if (!st.valid) return;
  char text[160];
  const int fps = static_cast<int>(st.fps + 0.5f);
  if (mode == 1) {
    std::snprintf(text, sizeof(text), "%d FPS", fps);
  } else if (mode == 2) {
    std::snprintf(text, sizeof(text), "%d FPS (%d-%d)", fps,
                  st.max_ms > 0 ? static_cast<int>(1000.0f / st.max_ms + 0.5f) : 0,
                  st.min_ms > 0 ? static_cast<int>(1000.0f / st.min_ms + 0.5f) : 0);
  } else {
    char limit[24];
    if (st.limit > 0)
      std::snprintf(limit, sizeof(limit), "%d", st.limit);
    else
      std::snprintf(limit, sizeof(limit), "none");
    std::snprintf(text, sizeof(text), "%d FPS  %.1f ms  (max %.1f ms)  limit %s", fps, st.frame_ms,
                  st.max_ms, limit);
  }
  // Green at the limit (or 60+ without one), yellow from 30, red below.
  const float good = st.limit > 0 ? st.limit * 0.95f : 59.0f;
  const ImU32 col = st.fps >= good  ? IM_COL32(110, 255, 110, 255)
                    : st.fps >= 29.5f ? IM_COL32(255, 230, 90, 255)
                                      : IM_COL32(255, 90, 80, 255);
  const float size = std::max(13.0f, 20.0f * io.DisplaySize.y / 1080.0f);
  ImFont* font = FontFor(size);
  const ImVec2 ext = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
  const float margin = std::floor(12.0f * io.DisplaySize.y / 1080.0f);
  const ImVec2 pos(std::floor(io.DisplaySize.x - ext.x - margin), margin + g_top_right_offset);  // [claude-9b]
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddText(font, size, ImVec2(pos.x + 1, pos.y + 1), IM_COL32(0, 0, 0, 200), text);
  dl->AddText(font, size, pos, col, text);
}

// cod3_perf_overlay: what each frame costs (game/perf_stats.h), under the
// frame rate.

static void DrawPerfOverlay(ImGuiIO& io) {
  const cod3::PerfReport r = cod3::PerfMonitor::Get().Report();
  char lines[9][160];
  int n = 0;
  if (!r.valid) {
    std::snprintf(lines[n++], sizeof(lines[0]), "perf: waiting for frames");
  } else {
    std::snprintf(lines[n++], sizeof(lines[0]), "%.0f fps  %.2f ms  game thread %.2f ms", r.fps,
                  r.frame_ms, r.game_ms);
    std::snprintf(lines[n++], sizeof(lines[0]), "draws %.0f  resolves %.1f  transfers %.1f",
                  r.draws, r.resolves, r.transfers);
    std::snprintf(lines[n++], sizeof(lines[0]), "GPU waits %.1f (%.2f ms)  submits %.1f",
                  r.gpu_waits, r.gpu_wait_ms, r.submissions);
    std::snprintf(lines[n++], sizeof(lines[0]), "readbacks %.1f (%.0f KB)  queries %.1f",
                  r.readbacks, r.readback_kb, r.queries);
    std::snprintf(lines[n++], sizeof(lines[0]), "upload %.0f KB  textures %.1f  faults %.0f",
                  r.upload_kb, r.texture_loads, r.watch_faults);
    // [hw-scaling] the GPU emulation's one thread, the busiest threads and
    // what limits the frame rate (game/thread_profiler.h).
    std::snprintf(lines[n++], sizeof(lines[0]),
                  "GPU thread %.2f ms  (submit %.2f, shaders %.2f ms)", r.gpu_thread_ms,
                  r.submit_ms, r.shader_ms);
    const cod3::ThreadReport threads = cod3::ThreadProfiler::Get().Report();
    if (threads.valid) {
      std::string busiest;
      for (size_t i = 0; i < threads.rows.size() && i < 3; ++i) {
        char part[64];
        std::snprintf(part, sizeof(part), "%s%s %.0f%%", i ? "   " : "",
                      threads.rows[i].name.c_str(), threads.rows[i].cpu_pct);
        busiest += part;
      }
      std::snprintf(lines[n++], sizeof(lines[0]), "%s", busiest.c_str());
    }
    if (!r.limited_by.empty())
      std::snprintf(lines[n++], sizeof(lines[0]), "%s", r.limited_by.c_str());
  }
  const float size = std::max(12.0f, 16.0f * io.DisplaySize.y / 1080.0f);
  ImFont* font = FontFor(size);
  const float margin = std::floor(12.0f * io.DisplaySize.y / 1080.0f);
  float y = margin + g_top_right_offset + std::max(13.0f, 20.0f * io.DisplaySize.y / 1080.0f) + 6.0f;  // [claude-9b]
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  for (int i = 0; i < n; ++i) {
    const ImVec2 ext = font->CalcTextSizeA(size, FLT_MAX, 0.0f, lines[i]);
    const ImVec2 pos(std::floor(io.DisplaySize.x - ext.x - margin), std::floor(y));
    dl->AddText(font, size, ImVec2(pos.x + 1, pos.y + 1), IM_COL32(0, 0, 0, 200), lines[i]);
    dl->AddText(font, size, pos, IM_COL32(230, 230, 230, 255), lines[i]);
    y += ext.y + 2.0f;
  }
}

// [claude-9b-think] 2026-10-27 UNTESTED (owner 15:27: "lettering within the glyphs isn't centered"). Where AddText
// has to start so that `text`'s ink sits in the middle of p0-p1. Until now each name was centered on
// CalcTextSizeA's box: the font's whole line height (ascent + descent) and the advance widths. In the shorter
// caps, capitals sat up to a few pixels off center vertically, and side bearings moved names sideways.
//  * Horizontally: from the first glyph's left ink edge (X0) to the last glyph's right ink edge (X1), with the
//    advances in between.
//  * Vertically: the capitals' band (the top of 'H' down to the baseline) when the text has a capital or a digit;
//    otherwise the text's own ink (+, -).
// Rounded to the nearest pixel (AddText truncates its position). If the font gives nothing to measure, falls back to
// the box, rounded the same way. Uses ImGui 1.92's baked fonts (the overlay already uses 1.92's
// PushFont(font, size)).
static ImVec2 InkCenteredPos(ImFont* font, float size, const char* text, ImVec2 p0, ImVec2 p1) {
  const ImVec2 mid((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
  if (!font || !text || !*text) return ImVec2(std::floor(mid.x + 0.5f), std::floor(mid.y + 0.5f));
  const ImVec2 ext = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
  const ImVec2 box(std::floor(mid.x - ext.x * 0.5f + 0.5f), std::floor(mid.y - ext.y * 0.5f + 0.5f));
  ImFontBaked* baked = font->GetFontBaked(size);
  if (!baked || baked->Size <= 0.0f) return box;
  const float k = size / baked->Size;  // AddText's own scale for this bake
  float pen = 0.0f, ink_l = FLT_MAX, ink_r = -FLT_MAX, ink_t = FLT_MAX, ink_b = -FLT_MAX;
  bool capitals = false;
  for (const unsigned char* s = reinterpret_cast<const unsigned char*>(text); *s;) {
    unsigned int c = *s++;
    if (c >= 0xC0) {  // UTF-8: a lead byte and its continuation bytes
      const int more = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : 1;
      c &= 0x3Fu >> more;
      for (int i = 0; i < more && (*s & 0xC0) == 0x80; ++i) c = (c << 6) | (*s++ & 0x3Fu);
    }
    if (c > IM_UNICODE_CODEPOINT_MAX) c = IM_UNICODE_CODEPOINT_INVALID;
    if ((c >= '0' && c <= '9') || (c >= 'A' && c <= 'Z')) capitals = true;
    const ImFontGlyph* g = baked->FindGlyph(ImWchar(c));
    if (!g) continue;
    if (g->Visible) {
      ink_l = std::min(ink_l, pen + g->X0 * k);
      ink_r = std::max(ink_r, pen + g->X1 * k);
      ink_t = std::min(ink_t, g->Y0 * k);
      ink_b = std::max(ink_b, g->Y1 * k);
    }
    pen += g->AdvanceX * k;
  }
  if (ink_l > ink_r) return box;  // nothing visible (spaces only)
  if (capitals) {
    const ImFontGlyph* h = baked->FindGlyphNoFallback(ImWchar('H'));
    if (h && h->Visible && h->Y1 > h->Y0) ink_t = h->Y0 * k, ink_b = h->Y1 * k;
  }
  return ImVec2(std::floor(mid.x - (ink_l + ink_r) * 0.5f + 0.5f), std::floor(mid.y - (ink_t + ink_b) * 0.5f + 0.5f));
}

// [acct2-decomp] A key cap as "SPACE Skip" draws it (owner 2026-10-25 23:42:
// "the keyboard glyphs need to match the 'SPACE to skip' button"): a shadow, a
// dark rounded box with a gold edge and the key's name in gold, centered.
// The game's prompts' keys are drawn with it too (DrawPromptKeyCaps).
static void DrawKeyCapBox(ImDrawList* dl, ImFont* font, float text_size, ImVec2 p0, ImVec2 p1,
                          const char* text, int a, float scale) {
  const ImU32 gold = GlyphColor(kGlyphAccent, a);  // the game's menu and loading text [claude-9b-think] or the player's
  const ImU32 shadow = IM_COL32(0, 0, 0, a * 3 / 4);
  const float round = 4.0f * scale;
  dl->AddRectFilled(ImVec2(p0.x + 2, p0.y + 2), ImVec2(p1.x + 2, p1.y + 2), shadow, round);
  dl->AddRectFilled(p0, p1, GlyphColor(kGlyphFill, a), round);
  dl->AddRect(p0, p1, gold, round, 0, 1.5f * scale);
  // [claude-9b-think] centered on the name's ink (InkCenteredPos), was on CalcTextSizeA's box.
  dl->AddText(font, text_size, InkCenteredPos(font, text_size, text, p0, p1), gold, text);
}

// [claude-9b-think] 2026-10-27 UNTESTED (owner 12:28-12:37): pictures of our own in the game's prompts
// (game/button_prompts.cpp PictureCode / MouseCode), each over the room the game keeps for it ("{code}"):
//  * keys: a mouse button as a small mouse on a key cap (outline, the pressed button filled: 1 left, 2 right,
//    3 the wheel, U / D the wheel with an arrow up / down, 4 / 5 the side buttons, back / forward);
//  * the PlayStation (PS4/5 and PS2), Nintendo and GameCube styles: simple shapes and the buttons' printed names in
//    the overlay's own font, filled in colors close to the controller's own buttons (no gold box or edge);
//    [claude-setup] 2026-11-03: that is cod3_glyph_color_mode original, the default (was cod3_glyph_colors on);
//    custom and the two rainbows change the colors, gold draws them in the key caps' gold style (was the default).
// Copyright: no company's logo, artwork, font or exact button shapes; generic geometry (circles, a cross, a
// square, a triangle, rounded caps, our own arrows) and plain letters. The colored option uses simple shapes and
// colors only to show which button to press on the player's own controller. The PS2 style: domed buttons and bolder
// strokes, SELECT / START; PS4/5: flat buttons and thinner strokes, SHARE / OPTIONS.
namespace prompt_glyph {

using cod3::prompts::Look;

struct Palette {
  ImU32 fill, edge, ink;  // the button, its rim, the shape or the writing
};

Palette Gold(int a) {
  const ImU32 gold = GlyphColor(kGlyphAccent, a);  // [claude-9b-think] the player's colors (cod3_glyph_color, _fill)
  return {GlyphColor(kGlyphFill, a), gold, gold};
}
// The colored option's plain buttons, D-pad and labels: dark grey with light writing.
Palette DarkGrey(int a) { return {IM_COL32(48, 48, 52, a * 9 / 10), IM_COL32(96, 96, 104, a), IM_COL32(232, 232, 232, a)}; }
Palette Filled(int a, int r, int g, int b, bool dark_ink = false) {
  return {IM_COL32(r, g, b, a), IM_COL32(r * 3 / 4, g * 3 / 4, b * 3 / 4, a),
          dark_ink ? IM_COL32(30, 30, 30, a) : IM_COL32(245, 245, 245, a)};
}
// [claude-9b-think] A button filled with the player's color `id` (cod3_glyph_color_*), at opacity `a`.
Palette FilledWith(int id, int a) {
  const uint32_t v = GlyphRgba(id);
  const int alpha = int((v & 0xFF) * uint32_t(std::clamp(a, 0, 255)) / 255);
  const int r = int(v >> 24), g = int((v >> 16) & 0xFF), b = int((v >> 8) & 0xFF);
  return {IM_COL32(r, g, b, alpha), IM_COL32(r * 3 / 4, g * 3 / 4, b * 3 / 4, a), IM_COL32(245, 245, 245, a)};
}

void Shadow(ImDrawList* dl, ImVec2 c, float r, int a) {
  dl->AddCircleFilled(ImVec2(c.x + 1.5f, c.y + 1.5f), r, IM_COL32(0, 0, 0, a * 3 / 4));
}

// A round button; `dome`: the PS2 style's highlight arc at the top left.
void RoundButton(ImDrawList* dl, ImVec2 c, float r, const Palette& p, float edge, bool dome, int a) {
  Shadow(dl, c, r, a);
  dl->AddCircleFilled(c, r, p.fill);
  if (dome) {
    constexpr float kPi = 3.14159265f;
    dl->PathArcTo(c, r * 0.72f, kPi * 1.05f, kPi * 1.55f, 12);
    dl->PathStroke(IM_COL32(255, 255, 255, a / 4), 0, std::max(1.0f, r * 0.12f));
  }
  dl->AddCircle(c, r, p.edge, 0, edge);
}

// Text centered in a box, made smaller to fit its width.
void CenteredText(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* text, ImU32 col, float size) {
  ImFont* font = FontFor(size);
  const ImVec2 ext = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text);
  const float room = (p1.x - p0.x) * 0.86f;
  if (ext.x > room && room > 0.0f) {
    size *= std::max(0.45f, room / ext.x);
    font = FontFor(size);
  }
  // [claude-9b-think] centered on the ink (InkCenteredPos): L1 / R1, OPTIONS, the GameCube and Nintendo letters.
  dl->AddText(font, size, InkCenteredPos(font, size, text, p0, p1), col, text);
}

// A labeled cap (L1, OPTIONS, Z ...): a rounded box with the name; `pill` fully rounded ends (START, SELECT ...).
void LabelCap(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const char* label, const Palette& p, float edge, bool pill,
              int a) {
  const float h = p1.y - p0.y;
  const float round = pill ? h * 0.5f : h * 0.22f;
  dl->AddRectFilled(ImVec2(p0.x + 1.5f, p0.y + 1.5f), ImVec2(p1.x + 1.5f, p1.y + 1.5f), IM_COL32(0, 0, 0, a * 3 / 4),
                    round);
  dl->AddRectFilled(p0, p1, p.fill, round);
  dl->AddRect(p0, p1, p.edge, round, 0, edge);
  CenteredText(dl, p0, p1, label, p.ink, h * 0.62f);
}

// The D-pad as our own plus shape (two bars, outlined); `arms`: which of "^v<>" are pressed (filled).
void DPad(ImDrawList* dl, ImVec2 c, float s, const std::string& arms, const Palette& p, float edge, int a) {
  const float h = s * 0.5f, w = s * 0.17f;  // half the length, half an arm's width
  const ImU32 shadow = IM_COL32(0, 0, 0, a * 3 / 4);
  dl->AddRectFilled(ImVec2(c.x - w + 1.5f, c.y - h + 1.5f), ImVec2(c.x + w + 1.5f, c.y + h + 1.5f), shadow);
  dl->AddRectFilled(ImVec2(c.x - h + 1.5f, c.y - w + 1.5f), ImVec2(c.x + h + 1.5f, c.y + w + 1.5f), shadow);
  dl->AddRectFilled(ImVec2(c.x - w, c.y - h), ImVec2(c.x + w, c.y + h), p.fill);
  dl->AddRectFilled(ImVec2(c.x - h, c.y - w), ImVec2(c.x + h, c.y + w), p.fill);
  const float in = w * 0.35f;  // the pressed arm, inside the outline
  for (char arm : arms) {
    ImVec2 q0, q1;
    switch (arm) {
      case '^': q0 = ImVec2(c.x - w + in, c.y - h + in), q1 = ImVec2(c.x + w - in, c.y - w); break;
      case 'v': q0 = ImVec2(c.x - w + in, c.y + w), q1 = ImVec2(c.x + w - in, c.y + h - in); break;
      case '<': q0 = ImVec2(c.x - h + in, c.y - w + in), q1 = ImVec2(c.x - w, c.y + w - in); break;
      case '>': q0 = ImVec2(c.x + w, c.y - w + in), q1 = ImVec2(c.x + h - in, c.y + w - in); break;
      default: continue;
    }
    dl->AddRectFilled(q0, q1, p.ink);
  }
  const ImVec2 pts[12] = {ImVec2(c.x - w, c.y - h), ImVec2(c.x + w, c.y - h), ImVec2(c.x + w, c.y - w),
                          ImVec2(c.x + h, c.y - w), ImVec2(c.x + h, c.y + w), ImVec2(c.x + w, c.y + w),
                          ImVec2(c.x + w, c.y + h), ImVec2(c.x - w, c.y + h), ImVec2(c.x - w, c.y + w),
                          ImVec2(c.x - h, c.y + w), ImVec2(c.x - h, c.y - w), ImVec2(c.x - w, c.y - w)};
  dl->AddPolyline(pts, 12, p.edge, ImDrawFlags_Closed, edge);
}

// A mouse on a key cap: the body, its two buttons and the wheel; the pressed one filled.
void Mouse(ImDrawList* dl, ImVec2 p0, ImVec2 p1, char code, int a, float scale) {
  const Palette p = Gold(a);
  const float h = p1.y - p0.y;
  const float side = std::min(p1.x - p0.x, h);
  const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
  // The cap, as wide as it is tall.
  const ImVec2 b0(std::floor(c.x - side * 0.5f), p0.y), b1(std::floor(c.x + side * 0.5f), p1.y);
  dl->AddRectFilled(ImVec2(b0.x + 2, b0.y + 2), ImVec2(b1.x + 2, b1.y + 2), IM_COL32(0, 0, 0, a * 3 / 4), 4.0f * scale);
  dl->AddRectFilled(b0, b1, p.fill, 4.0f * scale);
  dl->AddRect(b0, b1, p.edge, 4.0f * scale, 0, 1.5f * scale);
  // The body: a rounded upright box.
  const float bw = side * 0.42f, bh = h * 0.70f;
  const ImVec2 m0(c.x - bw * 0.5f, c.y - bh * 0.5f), m1(c.x + bw * 0.5f, c.y + bh * 0.5f);
  const float round = bw * 0.48f, line = std::max(1.0f, 1.2f * scale);
  const float split = m0.y + bh * 0.42f;  // the buttons' lower edge
  const float wheel_w = bw * 0.18f, wheel_h = bh * 0.20f;
  const ImVec2 w0(c.x - wheel_w * 0.5f, m0.y + bh * 0.12f), w1(c.x + wheel_w * 0.5f, m0.y + bh * 0.12f + wheel_h);
  if (code == '1') dl->AddRectFilled(m0, ImVec2(c.x, split), p.ink, round, ImDrawFlags_RoundCornersTopLeft);
  if (code == '2') dl->AddRectFilled(ImVec2(c.x, m0.y), ImVec2(m1.x, split), p.ink, round, ImDrawFlags_RoundCornersTopRight);
  dl->AddRect(m0, m1, p.edge, round, 0, line);
  dl->AddLine(ImVec2(m0.x, split), ImVec2(m1.x, split), p.edge, line);
  dl->AddLine(ImVec2(c.x, m0.y), ImVec2(c.x, w0.y), p.edge, line);
  dl->AddLine(ImVec2(c.x, w1.y), ImVec2(c.x, split), p.edge, line);
  const bool wheel = code == '3' || code == 'U' || code == 'D';
  if (wheel)
    dl->AddRectFilled(w0, w1, p.ink, wheel_w * 0.5f);
  else
    dl->AddRect(w0, w1, p.edge, wheel_w * 0.5f, 0, line);
  if (code == 'U' || code == 'D') {  // an arrow beside the body
    const float ax = m1.x + (b1.x - m1.x) * 0.5f, ay = (w0.y + w1.y) * 0.5f, ah = bh * 0.16f;
    const float dir = code == 'U' ? -1.0f : 1.0f;
    dl->AddTriangleFilled(ImVec2(ax, ay + dir * ah), ImVec2(ax - ah * 0.8f, ay - dir * ah * 0.4f),
                          ImVec2(ax + ah * 0.8f, ay - dir * ah * 0.4f), p.ink);
  }
  if (code == '4' || code == '5') {  // the side buttons on the left: 5 forward (upper), 4 back (lower)
    for (int i = 0; i < 2; ++i) {
      const float y = m0.y + bh * (0.50f + 0.18f * float(i));
      const ImVec2 s0(m0.x - bw * 0.16f, y), s1(m0.x + bw * 0.02f, y + bh * 0.13f);
      const bool pressed = (i == 0 && code == '5') || (i == 1 && code == '4');
      if (pressed)
        dl->AddRectFilled(s0, s1, p.ink, 1.0f);
      else
        dl->AddRect(s0, s1, p.edge, 1.0f, 0, line);
    }
  }
}

// PlayStation (PS4/5 and PS2 styles).
void PlayStation(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const std::string& code, bool ps2, bool colors, int a,
                 float scale) {
  const float h = p1.y - p0.y;
  const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
  const float r = std::min(p1.x - p0.x, h) * 0.5f;
  const float edge = (ps2 ? 2.0f : 1.2f) * scale, stroke = (ps2 ? 2.6f : 1.6f) * scale;
  if (code.find_first_not_of("^v<>") == std::string::npos) {
    DPad(dl, c, r * 2.0f, code, colors ? DarkGrey(a) : Gold(a), edge, a);
    return;
  }
  if (code.size() == 1 && std::string("xost").find(code[0]) != std::string::npos) {
    Palette p = colors ? DarkGrey(a) : Gold(a);
    if (colors) {  // the shape in a color close to its button's
      switch (code[0]) {  // [claude-9b-think] cod3_glyph_color_ps_* (the defaults are these colors)
        case 'x': p.ink = GlyphColor(kPsCross, a); break;
        case 'o': p.ink = GlyphColor(kPsCircle, a); break;
        case 's': p.ink = GlyphColor(kPsSquare, a); break;
        default: p.ink = GlyphColor(kPsTriangle, a); break;
      }
    }
    RoundButton(dl, c, r, p, edge, ps2, a);
    const float d = r * 0.42f;
    switch (code[0]) {
      case 'x':
        dl->AddLine(ImVec2(c.x - d, c.y - d), ImVec2(c.x + d, c.y + d), p.ink, stroke);
        dl->AddLine(ImVec2(c.x - d, c.y + d), ImVec2(c.x + d, c.y - d), p.ink, stroke);
        break;
      case 'o': dl->AddCircle(c, r * 0.46f, p.ink, 0, stroke); break;
      case 's': dl->AddRect(ImVec2(c.x - d, c.y - d), ImVec2(c.x + d, c.y + d), p.ink, 0.0f, 0, stroke); break;
      default:
        dl->AddTriangle(ImVec2(c.x, c.y - r * 0.50f), ImVec2(c.x - r * 0.47f, c.y + r * 0.32f),
                        ImVec2(c.x + r * 0.47f, c.y + r * 0.32f), p.ink, stroke);
        break;
    }
    return;
  }
  const Palette p = colors ? DarkGrey(a) : Gold(a);
  if (code == "L3" || code == "R3") {  // the sticks' clicks: a round cap
    RoundButton(dl, c, r, p, edge, ps2, a);
    CenteredText(dl, ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), code.c_str(), p.ink, r * 1.05f);
    return;
  }
  const bool pill = code == "START" || code == "SELECT" || code == "OPTIONS" || code == "SHARE";
  LabelCap(dl, p0, p1, code.c_str(), p, edge, pill, a);
}

// GameCube: a large round A, a smaller round B, X and Y as rounded caps, Z / L / R / START labeled caps, the C-stick a
// small round cap; colored: green A, red B, grey X / Y, purple Z, grey START and shoulders.
void GameCube(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const std::string& code, bool colors, int a, float scale) {
  const float h = p1.y - p0.y;
  const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
  const float r = std::min(p1.x - p0.x, h) * 0.5f, edge = 1.5f * scale;
  const Palette grey = colors ? Filled(a, 150, 150, 158) : Gold(a);
  if (code.find_first_not_of("^v<>") == std::string::npos) {
    DPad(dl, c, r * 2.0f, code, colors ? DarkGrey(a) : Gold(a), edge, a);
  } else if (code == "A") {
    const Palette p = colors ? FilledWith(kGcA, a) : Gold(a);  // [claude-9b-think] cod3_glyph_color_gc_*
    RoundButton(dl, c, r, p, edge, false, a);
    CenteredText(dl, ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), "A", p.ink, r * 1.3f);
  } else if (code == "B") {
    const Palette p = colors ? FilledWith(kGcB, a) : Gold(a);
    RoundButton(dl, c, r * 0.78f, p, edge, false, a);
    CenteredText(dl, ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), "B", p.ink, r * 1.0f);
  } else if (code == "X" || code == "Y") {
    const Palette p = colors ? FilledWith(code == "X" ? kGcX : kGcY, a) : Gold(a);
    LabelCap(dl, ImVec2(c.x - r * 0.95f, c.y - r * 0.70f), ImVec2(c.x + r * 0.95f, c.y + r * 0.70f), code.c_str(), p,
             edge, true, a);
  } else if (code == "C") {
    const Palette p = colors ? DarkGrey(a) : Gold(a);
    RoundButton(dl, c, r * 0.80f, p, edge, false, a);
    CenteredText(dl, ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), "C", p.ink, r * 1.0f);
  } else {
    const Palette p = !colors ? Gold(a) : code == "Z" ? FilledWith(kGcZ, a) : grey;
    LabelCap(dl, p0, p1, code.c_str(), p, edge, code == "START", a);
  }
}

// Nintendo (Switch): round lettered face buttons, labeled shoulder caps (L, R, ZL, ZR), small round + and -.
void Nintendo(ImDrawList* dl, ImVec2 p0, ImVec2 p1, const std::string& code, bool colors, int a, float scale) {
  const float h = p1.y - p0.y;
  const ImVec2 c((p0.x + p1.x) * 0.5f, (p0.y + p1.y) * 0.5f);
  const float r = std::min(p1.x - p0.x, h) * 0.5f, edge = 1.5f * scale;
  const Palette p = colors ? DarkGrey(a) : Gold(a);
  if (code.find_first_not_of("^v<>") == std::string::npos) {
    DPad(dl, c, r * 2.0f, code, p, edge, a);
  } else if (code == "A" || code == "B" || code == "X" || code == "Y" || code == "+" || code == "-") {
    const float rr = code.size() == 1 && (code[0] == '+' || code[0] == '-') ? r * 0.75f : r;
    // [claude-9b-think] cod3_glyph_color_nx_*: a face button of the player's color (the default: the dark grey).
    const int id = code == "A" ? kNxA : code == "B" ? kNxB : code == "X" ? kNxX : code == "Y" ? kNxY : -1;
    const Palette q = colors && id >= 0 && !GlyphColorIsDefault(id) ? FilledWith(id, a) : p;
    RoundButton(dl, c, rr, q, edge, false, a);
    CenteredText(dl, ImVec2(c.x - r, c.y - r), ImVec2(c.x + r, c.y + r), code.c_str(), q.ink, rr * 1.2f);
  } else {
    LabelCap(dl, p0, p1, code.c_str(), p, edge, false, a);
  }
}

}  // namespace prompt_glyph

// [claude-9b-think] One picture of ours (KeyCap::picture) in its room (window pixels).
static void DrawPromptPicture(ImDrawList* dl, const cod3::prompts::KeyCap& cap, ImVec2 p0, ImVec2 p1, int a,
                              float scale) {
  using cod3::prompts::Look;
  const bool colors = CurrentGlyphMode() != GlyphMode::kGold;  // [claude-setup] 2026-11-03 cod3_glyph_color_mode
  switch (cap.look) {
    case Look::kKeys:
      if (cap.name.size() == 1) prompt_glyph::Mouse(dl, p0, p1, cap.name[0], a, scale);
      break;
    case Look::kPlayStation:
    case Look::kPlayStation2:
      prompt_glyph::PlayStation(dl, p0, p1, cap.name, cap.look == Look::kPlayStation2, colors, a, scale);
      break;
    case Look::kGameCube: prompt_glyph::GameCube(dl, p0, p1, cap.name, colors, a, scale); break;
    case Look::kNintendo: prompt_glyph::Nintendo(dl, p0, p1, cap.name, colors, a, scale); break;
    default: break;
  }
}

// [acct2-decomp] A controller's skip button in the same colors: a round
// button with its label, the bottom face button (the game's A) as each
// controller names it: A, a cross for a PlayStation pad, B for a Nintendo
// one (simple shapes of our own, not any company's art).
static void DrawPadButton(ImDrawList* dl, ImFont* font, float text_size, ImVec2 center, float radius,
                          cod3::prompts::Look look, int a, float scale) {
  // [claude-9b-think] PlayStation (both styles), Nintendo and GameCube: the bottom face button as the prompts draw it
  // (DrawPromptPicture: a cross, B, A; colored as cod3_glyph_color_mode says).
  using cod3::prompts::Look;
  if (look == Look::kPlayStation || look == Look::kPlayStation2 || look == Look::kNintendo || look == Look::kGameCube) {
    cod3::prompts::KeyCap cap{};
    cap.look = look;
    cap.picture = true;
    cap.name = look == Look::kNintendo ? "B" : look == Look::kGameCube ? "A" : "x";
    DrawPromptPicture(dl, cap, ImVec2(center.x - radius, center.y - radius), ImVec2(center.x + radius, center.y + radius),
                      a, scale);
    return;
  }
  const ImU32 gold = GlyphColor(kGlyphAccent, a);  // [claude-9b-think] the player's colors
  dl->AddCircleFilled(ImVec2(center.x + 2, center.y + 2), radius, IM_COL32(0, 0, 0, a * 3 / 4));
  dl->AddCircleFilled(center, radius, GlyphColor(kGlyphFill, a));
  dl->AddCircle(center, radius, gold, 0, 1.5f * scale);
  const char* label = "A";  // [claude-9b-think] Xbox (the other styles above); centered on its ink
  dl->AddText(font, text_size,
              InkCenteredPos(font, text_size, label, ImVec2(center.x - radius, center.y - radius),
                             ImVec2(center.x + radius, center.y + radius)),
              gold, label);
}

// "SPACE  Skip" while a level's briefing can be skipped (game/cutscene_skip.h),
// in the game's own colors at the bottom of the screen, where the loading
// screen shows its "Loading ..." line. [acct2-decomp] With a controller in use
// (owner 2026-10-25 23:43: "the space to skip button glyph doesn't change to
// controller buttons when it detects input from one") its A button instead,
// switching with the device as the game's prompts do; `alpha` fades it in
// and out (cutscene_skip.cpp).
// [claude-9b-think] 2026-10-27 UNTESTED (owner 13:20: "the space to skip will cover subtitles a lot of the time";
// 13:21: held to skip). Smaller (18 px at 1080p, was 24), in the bottom-right corner of the HUD's title-safe area
// (5% in from its right and bottom edges; the HUD area is the 16:9 part of a wider picture) and right-aligned, clear
// of the game's centered lines (cod3_skip_prompt_position bottom_center: centered at 86% of the height, as before).
// "Hold to skip" while cod3_skip_hold_ms asks for a hold, with the hold's progress (CutsceneSkip::HoldProgress) as
// a bar along the inside of the key cap's bottom edge, or a ring around a controller's button.
static void DrawSkipPrompt(ImGuiIO& io, float alpha) {
  const HudArea area = CurrentHudArea(io);
  if (area.width < 1.0f || area.height < 1.0f) return;
  const bool corner = Get("cod3_skip_prompt_position") != "bottom_center";
  const float scale = std::max(0.6f, area.height / 1080.0f);
  const float size = 18.0f * scale;
  ImFont* font = FontFor(size);
  const cod3::prompts::Look look = cod3::prompts::FirstPlayerLook();
  const bool keys = look == cod3::prompts::Look::kKeys;
  const float hold = cod3::CutsceneSkip::Get().HoldProgress();  // < 0: nothing held
  const char* key = "SPACE";
  const char* label = GetInt("cod3_skip_hold_ms", 0) > 0 ? "Hold to skip" : "Skip";
  const ImVec2 key_ext = font->CalcTextSizeA(size * 0.8f, FLT_MAX, 0.0f, key);
  const ImVec2 label_ext = font->CalcTextSizeA(size, FLT_MAX, 0.0f, label);
  const float pad_x = 8.0f * scale, pad_y = 3.0f * scale, gap = 10.0f * scale;
  const float key_h = std::max(key_ext.y, label_ext.y) + 2 * pad_y;
  const float key_w = keys ? key_ext.x + 2 * pad_x : key_h;  // the button is round
  const float total = key_w + gap + label_ext.x;
  float x, y;
  if (corner) {
    x = std::floor(area.x + area.width * 0.95f - total);
    y = std::floor(area.y + area.height * 0.95f - key_h);
  } else {
    x = std::floor(area.x + (area.width - total) * 0.5f);
    y = std::floor(area.y + area.height * 0.86f);
  }
  const int a = static_cast<int>(alpha * 255);
  const ImU32 gold = GlyphColor(kGlyphAccent, a);  // [claude-9b-think] the label, the bar and the ring too
  const ImU32 shadow = IM_COL32(0, 0, 0, a * 3 / 4);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  if (keys) {
    DrawKeyCapBox(dl, font, size * 0.8f, ImVec2(x, y), ImVec2(x + key_w, y + key_h), key, a, scale);
    if (hold >= 0.0f) {  // the hold: a bar filling from the left along the cap's bottom edge, inside it
      const float in = std::floor(3.0f * scale), bar = std::max(2.0f, std::floor(3.0f * scale));
      const float x0 = x + in, x1 = x + key_w - in;
      if (hold > 0.0f)
        dl->AddRectFilled(ImVec2(x0, y + key_h - in - bar), ImVec2(x0 + (x1 - x0) * hold, y + key_h - in), gold,
                          bar * 0.5f);
    }
  } else {
    const ImVec2 c(x + key_w * 0.5f, y + key_h * 0.5f);
    DrawPadButton(dl, font, size * 0.8f, c, key_h * 0.5f, look, a, scale);
    if (hold > 0.0f) {  // the hold: a ring around the button, clockwise from the top
      constexpr float kPi = 3.14159265f;
      const float r = key_h * 0.5f + 3.0f * scale;
      dl->PathArcTo(c, r, -0.5f * kPi, -0.5f * kPi + 2.0f * kPi * hold, 40);
      dl->PathStroke(gold, 0, std::max(2.0f, 2.5f * scale));
    }
  }
  // Label. [claude-9b-think] its capitals centered on the cap's middle (InkCenteredPos), was its line box.
  const float label_x = x + key_w + gap;
  const ImVec2 lp(label_x,
                  InkCenteredPos(font, size, label, ImVec2(label_x, y), ImVec2(label_x + label_ext.x, y + key_h)).y);
  dl->AddText(font, size, ImVec2(lp.x + 2, lp.y + 2), shadow, label);
  dl->AddText(font, size, lp, gold, label);
}

// The game's HUD on the window (hud_overlay.h): the picture where the
// presenter shows it with the shape the game draws (game/ultrawide.h: bars,
// or stretched with Letterbox off), and on a wider picture its centered 16:9
// part when the HUD is kept 16:9.
HudArea CurrentHudArea(const ImGuiIO& io) {
  HudArea a{0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y};
  if (a.width < 1.0f || a.height < 1.0f) return a;
  const float picture = std::max(cod3::DisplayAspect(), 0.1f);
  const cod3::PictureRect r = cod3::PictureInView(uint32_t(io.DisplaySize.x + 0.5f),
                                                  uint32_t(io.DisplaySize.y + 0.5f), picture);
  a = HudArea{float(r.x), float(r.y), float(r.width), float(r.height)};
  if (picture > cod3::kSixteenByNine + cod3::kSameShape && REXCVAR_GET(cod3_ultrawide_hud)) {
    // The 16:9 share of the picture's width (the same on a stretched picture).
    const float hud_width = std::floor(a.width * cod3::kSixteenByNine / picture);
    a.x += std::floor((a.width - hud_width) * 0.5f);
    a.width = hud_width;
  }
  return a;
}

// [acct2-decomp] The keys in the game's prompts as key caps (owner
// 2026-10-25 23:42): game/button_prompts.cpp notes each key name a text line
// draws, in the game's 640x480 menu space (the game draws the name itself
// invisibly to keep its room); here each becomes DrawKeyCapBox's cap over
// that room, mapped like the menus' other overlay pieces (the HUD area: the
// menus are drawn over its whole width and height), at the opacity the game
// drew the text with, so a hint fading in or out takes its caps along.
static void DrawPromptKeyCaps(ImGuiIO& io) {
  // [claude-9b] Only lines drawn in the last few frames (was 150 ms: a prompt that went away left
  // its key cap on screen alone for a moment).
  const std::vector<cod3::prompts::KeyCap> caps = cod3::prompts::CurrentKeyCaps(50);
  if (caps.empty()) return;
  const HudArea area = CurrentHudArea(io);
  if (area.width < 1.0f || area.height < 1.0f) return;
  const float sx = area.width / 640.0f, sy = area.height / 480.0f;
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  for (const auto& cap : caps) {
    const int a = static_cast<int>(std::clamp(cap.alpha, 0.0f, 1.0f) * 255.0f);
    if (a <= 0 || cap.name.empty()) continue;
    ImVec2 p0(area.x + cap.x0 * sx, area.y + cap.y0 * sy);
    ImVec2 p1(area.x + cap.x1 * sx, area.y + cap.y1 * sy);
    const float h = p1.y - p0.y;
    if (h < 4.0f || p1.x - p0.x < 4.0f) continue;
    // [claude-9b-think] 2026-10-27 UNTESTED (owner, Part B: the TAB cap was ~80x28 px next to text ~16 px tall at
    // 1280x720). The cap was the line's whole height (less 6% top and bottom) and the whole room the game keeps for
    // "[NAME]" in its own font; now it is 1.3x the text's capitals tall, centered on their middle, and as wide as our
    // name plus its margins (at least square), centered in the room. Measured in c9b-test-pr-look-s40.png (1280x720):
    // the line is 36 px, the old cap rows 224-255 and 514-595, the game's capitals ("P" of Press) rows 232-249: the
    // capitals are 0.47 of the line, their middle 0.53 of it down. Now about 22 px tall, TAB about 41 px wide.
    constexpr float kCapitals = 0.47f, kCapitalsMiddle = 0.53f, kCapTall = 1.3f;
    const float scale = std::max(0.5f, h * 0.88f / 37.0f);  // as before: the edge and corners keep their width
    // [claude-9b-think] 2026-10-27 UNTESTED cod3_key_cap_scale (owner 15:01): the cap's height times the scale, but
    // at most 0.95 of the line and kept inside it, so it never reaches the line above or below (the line ~36 px, the
    // capitals ~17 px at 1080p; 1.55 is the tallest scale that fits).
    const float cap_scale = std::clamp(float(GetDouble("cod3_key_cap_scale", 1.0)), 0.8f, 1.8f);
    const float cap_h = std::max(4.0f, std::round(std::min(h * kCapitals * kCapTall * cap_scale, h * 0.95f)));
    const float cap_top = std::clamp(h * kCapitalsMiddle - cap_h * 0.5f, 0.0f, std::max(0.0f, h - cap_h));
    p0.y = std::floor(p0.y + cap_top);
    p1.y = p0.y + cap_h;
    // Our name's capitals about 0.55 of the cap (the overlay font's capitals are half its size: TAB's were 10 px at
    // 19.8), narrowed to fit a small room as before.
    float text_size = cap_h * 1.1f;
    ImFont* font = FontFor(text_size);
    const float margin = std::floor(cap_h * 0.3f);
    const float room = p1.x - p0.x;
    float wide = font->CalcTextSizeA(text_size, FLT_MAX, 0.0f, cap.name.c_str()).x;
    if (!cap.picture && wide > room - 2 * margin && room > 2 * margin) {
      text_size *= std::max(0.6f, (room - 2 * margin) / wide);
      wide = font->CalcTextSizeA(text_size, FLT_MAX, 0.0f, cap.name.c_str()).x;
    }
    // [claude-9b-think] a fitted room (button_prompts.cpp FittedRoom) is the cap's own width: the cap fills it, so the
    // words sit as close to it as the game's own spacing puts them.
    const float w = cap.fitted ? std::floor(std::max(room, 4.0f))
                               : std::floor(std::min(std::max(wide + 2 * margin, cap_h), std::max(room, cap_h)));
    const float x0 = std::floor(p0.x + (room - w) * 0.5f);
    const ImVec2 q0(x0, p0.y), q1(x0 + w, p1.y);
    // [claude-9b-think] our pictures (mouse buttons, controller styles) in the room the game kept for them.
    if (cap.picture) {
      DrawPromptPicture(dl, cap, q0, q1, a, scale);
      continue;
    }
    DrawKeyCapBox(dl, font, text_size, q0, q1, cap.name.c_str(), a, scale);
  }
}

// [claude-9b] The crosshair preview a menu asked for (ShowCrosshairPreview):
// its box from the game's menu space to the window, a dark panel with a thin
// line like the menu's, and the crosshair in the middle at its in-game size.
// The game draws its 640x480 menus over the whole HUD area (stretched across
// a 16:9 picture: a menu unit is wider than it is tall), so the box keeps its
// right edge, top and height from the menu and is as wide as it is tall on
// the screen (owner 2026-10-20 21:15: "please also make sure the menu scales").
static void DrawMenuCrosshairPreview(ImGuiIO& io) {
  if (NowNs() - g_preview_ns.load(std::memory_order_relaxed) > 150'000'000) return;
  const HudArea a = CurrentHudArea(io);
  if (a.width < 1.0f || a.height < 1.0f) return;
  const float unit = a.height / 480.0f;  // the menu's units up and down
  float box[4];
  for (int i = 0; i < 4; ++i) box[i] = g_preview_box[i].load(std::memory_order_relaxed);
  const float right = std::floor(a.x + box[2] * a.width / 640.0f);
  const float top = std::floor(a.y + box[1] * unit);
  const float side = std::floor((box[3] - box[1]) * unit);
  const ImVec2 p0(right - side, top);
  const ImVec2 p1(right, top + side);
  if (p1.x - p0.x < 4.0f || p1.y - p0.y < 4.0f) return;
  static ImVec2 logged_size{};
  if (logged_size.x != io.DisplaySize.x || logged_size.y != io.DisplaySize.y) {
    logged_size = io.DisplaySize;
    REXLOG_INFO("cod3: crosshair preview drawn at {:.0f},{:.0f}-{:.0f},{:.0f} in a {:.0f}x{:.0f} window", p0.x, p0.y,
                p1.x, p1.y, io.DisplaySize.x, io.DisplaySize.y);
  }
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  dl->AddRectFilled(p0, p1, IM_COL32(12, 10, 8, 215));
  dl->AddRect(p0, p1, IM_COL32(160, 130, 70, 200), 0.0f, 0, std::max(1.0f, unit * 0.75f));
  dl->PushClipRect(p0, p1, true);
  DrawCrosshairPreviewShape(dl, ImVec2(std::floor((p0.x + p1.x) * 0.5f), std::floor((p0.y + p1.y) * 0.5f)),
                            std::max(0.5f, io.DisplaySize.y / 1080.0f));
  dl->PopClipRect();
}

// "CHEATS USED" watermark (game/cheat_watch.h): top-left corner of the HUD
// (owner), small and see-through, on every screen (menus too) until the
// cheats are all reset.
static void DrawCheatWatermark(ImGuiIO& io) {
  auto& watch = cod3::CheatWatch::Get();
  if (!watch.Active()) return;
  const HudArea area = CurrentHudArea(io);
  const float scale = std::max(0.6f, area.height / 1080.0f);
  ImFont* font = FontFor(32.0f * scale);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  const float x = area.x + std::floor(24.0f * scale);
  auto line = [&](const std::string& text, float size, float y, ImU32 col) {
    const ImVec2 ext = font->CalcTextSizeA(size, FLT_MAX, 0.0f, text.c_str());
    const ImVec2 pos(x, std::floor(y));
    dl->AddText(font, size, ImVec2(pos.x + 2, pos.y + 2), IM_COL32(0, 0, 0, 120), text.c_str());
    dl->AddText(font, size, pos, col, text.c_str());
    return ext.y;
  };
  float y = area.y + 24.0f * scale;
  y += line("CHEATS USED", 32.0f * scale, y, IM_COL32(255, 255, 255, 140)) + 2.0f * scale;
  const std::string reasons = watch.Reasons();
  if (!reasons.empty()) line(reasons, 18.0f * scale, y, IM_COL32(255, 255, 255, 120));
}

void HudOverlay::OnDraw(ImGuiIO& io) {
  if (on_frame) on_frame();
  cod3::PerfMonitor::Get().Tick();
  DrawCheatWatermark(io);
  cod3::hud::AnnouncementFeed::Draw(io);
  DrawQteSpinner(io);  // stick battle actions (ui/qte_spinner.h)
  DrawPromptKeyCaps(io);  // [acct2-decomp] the game's prompts' keys as key caps
  if (const float cover = cod3::CutsceneSkip::Get().CoverAlpha(); cover > 0.0f) {
    // Skipping a level intro: black while game time runs fast behind it.
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, static_cast<int>(cover * 255)));
  }
  if (const float a = cod3::CutsceneSkip::Get().PromptAlpha(); a > 0.0f) DrawSkipPrompt(io, a);
  g_top_right_offset = DrawVersionLabel(io);  // [claude-9b] cod3_show_version, above the frame rate
  if (const int mode = cod3::Engine::Get().draw_fps(); mode > 0) DrawFpsCounter(io, mode);
  if (REXCVAR_GET(cod3_perf_overlay)) DrawPerfOverlay(io);
  DrawMenuCrosshairPreview(io);  // [claude-9b] the AIM page's preview (menus only)
  auto s = CurrentCrosshair();
  if (!s.enabled) return;
  // Only while the game itself would draw its crosshair (not in menus or
  // cutscenes): the CG_DrawCrosshair hooks stamp that every frame, for each
  // local player's view in split screen. Raising the sights fades it out over
  // the first half of the motion.
  // [claude-9b] Each view's crosshair at the middle of that view on the game's picture (bars and
  // picture shape included), sized by the view's height; a view whose player left or died stops
  // being stamped and so stops being drawn.
  // The whole window is the whole picture's place (centered, as before); a split-screen view's
  // place is worked out from the picture as the presenter shows it, once a frame when needed.
  cod3::PictureRect picture{};
  bool picture_known = false;
  const int64_t now = NowNs();
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  for (auto& v : g_crosshair_views) {
    if (now - v.ns.load(std::memory_order_relaxed) > kCrosshairShownNs) continue;
    const float fade = std::clamp(1.0f - 2.0f * v.aim.load(std::memory_order_relaxed), 0.0f, 1.0f);
    if (fade <= 0.0f) continue;
    float color[4] = {s.color[0], s.color[1], s.color[2], s.color[3]};
    // The game's own color unless the player chose a constant one.
    if (!s.constant_color) {
      for (int i = 0; i < 4; ++i) color[i] = v.color[i].load(std::memory_order_relaxed);
      if (REXCVAR_GET(cod3_crosshair_own_colors)) ApplyOwnCrosshairColor(color);  // [claude-9b]
    }
    const float alpha = color[3] * fade;
    if (alpha <= 0.0f) continue;
    const float rx = v.rect[0].load(std::memory_order_relaxed), ry = v.rect[1].load(std::memory_order_relaxed);
    const float rw = v.rect[2].load(std::memory_order_relaxed), rh = v.rect[3].load(std::memory_order_relaxed);
    ImVec2 c(std::floor(io.DisplaySize.x * 0.5f), std::floor(io.DisplaySize.y * 0.5f));
    float view_h = io.DisplaySize.y;
    if (rw < 0.99f || rh < 0.99f) {
      if (!picture_known) {
        picture = cod3::PictureInView(uint32_t(io.DisplaySize.x + 0.5f), uint32_t(io.DisplaySize.y + 0.5f),
                                      cod3::DisplayAspect());
        picture_known = true;
      }
      view_h = float(picture.height) * rh;
      c = ImVec2(std::floor(float(picture.x) + float(picture.width) * (rx + rw * 0.5f)),
                 std::floor(float(picture.y) + float(picture.height) * (ry + rh * 0.5f)));
    }
    // [claude-9b] A split-screen view's crosshair is the whole picture's size times the view's share
    // of its height (owner 2026-10-27 05:49 "splitscreen hud drawing seems off"): the 0.5 minimum
    // applies to the whole picture, so a half view in a 720-line window gets 0.33, not 0.5 (1.5x too
    // big before), like the game's own HUD, which is drawn smaller in split screen.
    const float scale = (rw < 0.99f || rh < 0.99f)
                            ? std::max(0.25f, std::max(0.5f, float(picture.height) / 1080.0f) * rh)
                            : std::max(0.5f, view_h / 1080.0f);
    // [claude-9b] 2026-10-27 cod3_debug_crosshair: screenshots are the game's own picture without this
    // overlay (game/screenshot.cpp), so tests read each view's crosshair from the log instead.
    if (REXCVAR_GET(cod3_debug_crosshair)) {
      static int64_t logged_ns[kCrosshairViews] = {};
      const int view = int(&v - g_crosshair_views);
      if (now - logged_ns[view] > 1'000'000'000) {
        logged_ns[view] = now;
        REXLOG_INFO("cod3: crosshair: view {} drawn at {:.0f},{:.0f} of {:.0f}x{:.0f}, scale {:.3f} (view {:.2f} of "
                    "the picture's height), alpha {:.2f}",
                    view + 1, c.x, c.y, io.DisplaySize.x, io.DisplaySize.y, scale, rh, alpha);
      }
    }
    if (s.outline) {
      DrawCrosshairShape(dl, c, s, scale, IM_COL32(0, 0, 0, static_cast<int>(alpha * 255)),
                         s.outline_thickness * scale);
    }
    DrawCrosshairShape(dl, c, s, scale, ImGui::ColorConvertFloat4ToU32(ImVec4(color[0], color[1], color[2], alpha)),
                       0.0f);
  }
}

// ===========================================================================
// Settings window
// ===========================================================================
SettingsWindow::SettingsWindow(rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path,
                               std::function<void()> on_close)
    : ImGuiDialog(drawer), config_path_(std::move(config_path)), on_close_(std::move(on_close)) {}

void SettingsWindow::Save() {
  cod3::binds::CommitF2Edits();  // [keybinds] text typed into a key row and not applied yet
  try {
    rex::cvar::SaveConfig(config_path_);
    status_ = "Saved to " + cod3::PathToUtf8(config_path_);
    dirty_ = false;
  } catch (const std::exception& e) {
    status_ = std::string("Save failed: ") + e.what();
  }
}

void SettingsWindow::SaveIfDirty() {
  dirty_ |= cod3::binds::CommitF2Edits();  // [keybinds] F2 closed while a key row had typed text
  if (dirty_) Save();
}

// Combo bound to a cvar; returns true when the value changed.
static bool CvarCombo(const char* label, const char* cvar, const char* const* values,
                      const char* const* names, int count, bool* restart) {
  if (!Has(cvar)) {
    ImGui::BeginDisabled();
    int dummy = 0;
    ImGui::Combo(label, &dummy, names, count);
    ImGui::EndDisabled();
    return false;
  }
  // Only offer values this build accepts (e.g. the FSR / CAS present effects
  // exist only when the SDK was built with FidelityFX).
  auto* info = rex::cvar::GetFlagInfo(cvar);
  std::vector<int> shown;
  for (int i = 0; i < count; ++i) {
    if (info && info->constraints.HasAllowedValues()) {
      const auto& allowed = info->constraints.allowed_values;
      if (std::find(allowed.begin(), allowed.end(), values[i]) == allowed.end()) continue;
    }
    shown.push_back(i);
  }
  if (shown.empty()) return false;
  std::vector<const char*> shown_names;
  for (int i : shown) shown_names.push_back(names[i]);

  std::string cur = Get(cvar);
  int idx = 0;
  for (size_t j = 0; j < shown.size(); ++j)
    if (cur == values[shown[j]]) idx = static_cast<int>(j);
  if (ImGui::Combo(label, &idx, shown_names.data(), static_cast<int>(shown_names.size()))) {
    rex::cvar::SetFlagByName(cvar, values[shown[idx]]);
    if (info && info->lifecycle != rex::cvar::Lifecycle::kHotReload) *restart = true;
    return true;
  }
  return false;
}

static bool CvarCheckbox(const char* label, const char* cvar, bool* restart) {
  if (!Has(cvar)) return false;
  bool v = GetBool(cvar);
  if (ImGui::Checkbox(label, &v)) {
    rex::cvar::SetFlagByName(cvar, v ? "true" : "false");
    auto* info = rex::cvar::GetFlagInfo(cvar);
    if (info && info->lifecycle != rex::cvar::Lifecycle::kHotReload) *restart = true;
    return true;
  }
  return false;
}

// A short, plain description under an option (dimmed, wrapped to the window).
static void Help(const char* text) {
  ImGui::PushTextWrapPos(0.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextUnformatted(text);
  ImGui::PopStyleColor();
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
}

// Puts settings back to their defaults (the game's own from app_defaults.h,
// which Cod3App made the defaults, else the SDK's) the way a change in F2
// does: each change callback runs, so a setting that applies while the game
// runs does so at once (the window leaves fullscreen, shadows, picture colors,
// the volume) instead of at the next start. Returns true when one of the
// changes needs a restart to take effect.
static bool ResetSettings(std::initializer_list<const char*> names) {
  bool restart = false;
  for (const char* name : names) {
    const auto* info = rex::cvar::GetFlagInfo(name);
    if (!info) continue;
    const std::string def = info->default_value;   // copied: the pointer goes stale
    const bool hot = info->lifecycle == rex::cvar::Lifecycle::kHotReload;
    const std::string before = Get(name);
    if (before != def) rex::cvar::SetFlagByName(name, def);
    if (Get(name) != before && !hot) restart = true;
  }
  return restart;
}

// "Reset ... to defaults" with a confirmation, one per settings group; each
// only touches its own group. Returns true once confirmed.
static bool ResetButton(const char* id, const char* what) {
  char label[128], title[96];
  std::snprintf(label, sizeof(label), "Reset %s to defaults##%s", what, id);
  std::snprintf(title, sizeof(title), "Reset to defaults##%s", id);   // shown title, unique id
  const float w = ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
  ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - w));
  if (ImGui::SmallButton(label)) ImGui::OpenPopup(title);
  bool confirmed = false;
  if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Put the %s back to their defaults?", what);
    ImGui::TextDisabled("Nothing else changes.");
    if (ImGui::Button("Reset")) {
      confirmed = true;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  return confirmed;
}

// [claude-9b-think] 2026-10-27 UNTESTED the prompt pictures' colors in F2 > Game > Prompts (owner 13:59). One row: the
// color picker and "Default", which puts just this color back ([claude-setup] the default in its tooltip).
static bool GlyphColorRow(int id) {
  const GlyphColorSetting& g = kGlyphColorSettings[id];
  const bool alpha = std::string_view(g.def).size() == 8;  // the fill and Nintendo's buttons: an opacity of their own
  const uint32_t cur = GlyphSettingRgba(id), def = GlyphDefaultRgba(id);  // [claude-setup] the setting itself
  auto floats = [](uint32_t v, float* c) {
    for (int i = 0; i < 4; ++i) c[i] = float((v >> (24 - 8 * i)) & 0xFF) / 255.0f;
  };
  float c[4], d[4];
  floats(cur, c);
  floats(def, d);
  bool changed = false;
  ImGui::PushID(g.cvar);
  // [claude-setup] 2026-11-03 (owner: "one of the keycap color settings doesn't change anything. It's like it's
  // opacity, but it's just a checkerboard"): ImGui 1.92 previews a see-through color over a checkerboard, so the
  // Cap fill (#14100A at 80 %) looked like a dark checkerboard, and the default's swatch beside it did nothing when
  // clicked. Now a color with an opacity of its own shows solid on the swatch's left half and see-through on its
  // right, and the default is shown in the Default button's tooltip instead of a swatch of its own.
  const ImGuiColorEditFlags flags =
      alpha ? ImGuiColorEditFlags_AlphaBar | ImGuiColorEditFlags_AlphaPreviewHalf : ImGuiColorEditFlags_NoAlpha;
  ImGui::SetNextItemWidth(ImGui::GetContentRegionAvail().x * 0.5f);
  if (alpha ? ImGui::ColorEdit4(g.label, c, flags) : ImGui::ColorEdit3(g.label, c, flags)) {
    rex::cvar::SetFlagByName(g.cvar, HexColor(c, alpha));
    // [claude-setup] 2026-11-03 a color picked is meant to be seen: Button colors goes to Custom.
    if (CurrentGlyphMode() != GlyphMode::kCustom) rex::cvar::SetFlagByName("cod3_glyph_color_mode", "custom");
    changed = true;
  }
  ImGui::SameLine();
  ImGui::BeginDisabled(cur == def);
  if (ImGui::SmallButton("Default")) {
    ResetSettings({g.cvar});
    changed = true;
  }
  ImGui::EndDisabled();
  if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) {
    ImGui::BeginTooltip();
    ImGui::ColorButton("##default", ImVec4(d[0], d[1], d[2], d[3]), flags | ImGuiColorEditFlags_NoTooltip,
                       ImVec2(ImGui::GetFrameHeight(), ImGui::GetFrameHeight()));
    ImGui::SameLine();
    if (cur == def)
      ImGui::Text("Default: #%s (in use)", g.def);
    else
      ImGui::Text("Default: #%s", g.def);
    ImGui::EndTooltip();
  }
  ImGui::PopID();
  return changed;
}

// Every prompt picture color back to its default ("Reset colors", and the Game tab's reset).
static void ResetGlyphColors() {
  for (const GlyphColorSetting& g : kGlyphColorSettings) ResetSettings({g.cvar});
}

// A preview row on a scene-grey band: a key cap, the mouse and controller pictures as the prompts draw them now.
static void DrawGlyphPreview() {
  using cod3::prompts::Look;
  const float h = std::floor(ImGui::GetFrameHeight() * 1.3f), pad = std::floor(h * 0.3f);
  const float scale = std::max(0.5f, h / 37.0f);
  ImDrawList* dl = ImGui::GetWindowDrawList();
  const ImVec2 o = ImGui::GetCursorScreenPos();
  struct Item {
    Look look;
    const char* name;
    bool picture;
    float width;  // in heights
  };
  static constexpr Item kItems[] = {{Look::kKeys, "TAB", false, 1.9f}, {Look::kKeys, "1", true, 1.0f},
                                    {Look::kPlayStation, "x", true, 1.0f}, {Look::kPlayStation, "o", true, 1.0f},
                                    {Look::kPlayStation, "s", true, 1.0f}, {Look::kPlayStation, "t", true, 1.0f},
                                    {Look::kGameCube, "A", true, 1.0f},    {Look::kGameCube, "B", true, 1.0f},
                                    {Look::kGameCube, "X", true, 1.0f},    {Look::kGameCube, "Z", true, 1.4f},
                                    {Look::kNintendo, "A", true, 1.0f},    {Look::kNintendo, "B", true, 1.0f}};
  float w = pad;
  for (const Item& it : kItems) w += std::floor(h * it.width) + pad;
  dl->AddRectFilled(o, ImVec2(o.x + w, o.y + h + 2 * pad), IM_COL32(86, 92, 84, 255), 4.0f);
  float x = o.x + pad;
  for (const Item& it : kItems) {
    const float iw = std::floor(h * it.width);
    const ImVec2 p0(x, o.y + pad), p1(x + iw, o.y + pad + h);
    if (it.picture) {
      cod3::prompts::KeyCap cap{};
      cap.look = it.look;
      cap.name = it.name;
      cap.picture = true;
      DrawPromptPicture(dl, cap, p0, p1, 255, scale);
    } else {
      const float text_size = h * 0.6f;
      DrawKeyCapBox(dl, FontFor(text_size), text_size, p0, p1, it.name, 255, scale);
    }
    x += iw + pad;
  }
  ImGui::Dummy(ImVec2(w, h + 2 * pad));
}

void SettingsWindow::DrawVideo() {
  if (ResetButton("reset_video", "video settings")) {
    auto& engine = cod3::Engine::Get();
    restart_needed_ |= ResetSettings({"fullscreen", "cod3_render_resolution", "cod3_render_method",
                                      "cod3_render_percent", "cod3_aspect_ratio",
                                      "vulkan_allow_present_mode_immediate",
                                      "d3d12_allow_variable_refresh_rate_and_tearing", "present_letterbox",
                                      "swap_post_effect", "present_effect",
                                      "anisotropic_override", "cod3_hd_textures",
                                      "cod3_max_fps", "cod3_shadow_quality", "cod3_color_space",
                                      "display_hdr", "display_hdr_paper_white", "display_hdr_peak",
                                      "cod3_refresh_rate", "cod3_live_resolution", "cod3_graphics_api",
                                      cod3::post_effects::kDepthOfField,     // [post-effects]
                                      cod3::post_effects::kMotionBlur});     // [post-effects]
#if defined(COD3_MP)
    restart_needed_ |= ResetSettings({"cod3mp_split_shadows"});  // [claude-9b-think] F2 > Video > Shadows
#endif
    if (engine.GameReady()) {
      engine.SetMaxFps(REXCVAR_GET(cod3_max_fps));
      engine.SetDrawFps(0);
    }
    cod3::RequestRenderResolution();  // a reset doesn't announce itself
    dirty_ = true;
  }
  // [claude-9b] Subtabs (owner 2026-10-21: "please make sub tabs for the video settings"), as
  // GAME and CONSOLE have them; the reset button above covers all of them.
  if (!ImGui::BeginTabBar("##video_views")) return;
  struct Page {
    const char* label;
    void (SettingsWindow::*draw)();
  };
  static constexpr Page kPages[] = {
      {"Display", &SettingsWindow::DrawVideoDisplay},
      {"Resolution", &SettingsWindow::DrawVideoResolution},
      {"Frame rate", &SettingsWindow::DrawVideoFrameRate},
      {"Colors", &SettingsWindow::DrawVideoColors},
      {"Quality", &SettingsWindow::DrawVideoQuality},
      // [claude-9b] 2026-10-27 "Accuracy" (the resolve readback) moved to EMULATION > GPU.
  };
  for (const Page& page : kPages) {
    if (!ImGui::BeginTabItem(page.label)) continue;
    (this->*page.draw)();
    ImGui::EndTabItem();
  }
  ImGui::EndTabBar();
}

// Video > Display: the window, the screen it's on and how pictures reach it.
void SettingsWindow::DrawVideoDisplay() {
  // [claude-9b] 2026-10-27 the "Native renderer" toggle moved to EMULATION > GPU (owner 17:09: "make a
  // emulation tab in the f2 menu for the emulation toggles").
#ifdef _WIN32
  {
    // Both renderers are built into Windows builds; Linux only has Vulkan
    // (Direct3D 12 there would need Wine).
    const char* v[] = {"auto", "d3d12", "vulkan"};
    const char* n[] = {"Automatic (Vulkan)", "Direct3D 12", "Vulkan"};
    dirty_ |= CvarCombo("Graphics API", "cod3_graphics_api", v, n, 3, &restart_needed_);
    Help("Which graphics API draws the game. Vulkan (the automatic choice, as on Linux) gives the "
         "emulated GPU twice the Xbox 360's render target memory, so the game draws up to "
         "1920x1080 itself (native) and the settings match the Linux version. Direct3D 12 is there if Vulkan "
         "doesn't work on your PC. If the chosen one can't start, the other is used. Applies after "
         "a restart.");
  }
#endif
  {
    const char* v[] = {"false", "true"};
    const char* n[] = {"Windowed", "Fullscreen (borderless)"};
    dirty_ |= CvarCombo("Display mode", "fullscreen", v, n, 2, &restart_needed_);
    Help("Play in a window, or fill the whole monitor without changing its resolution "
         "(borderless). Applies at once and is kept for the next start.");
  }
  {
    // [claude-9b] The screen the game plays on (SDK patch 0068; owner 2026-10-21: "we also need
    // to add the feature to bind to a specific display"). Kept by the screen's name (and its
    // number, for settings from before names), so it stays put when other screens are plugged in
    // or out. Each window has its own choice (rex::ui::Window::SetDisplayChoice), ready for
    // split-screen players' own windows (dev/agents/hw-scaling/SPLITSCREEN_WINDOWS.md).
    using Window = rex::ui::Window;
    const std::vector<Window::DisplayInfo> displays = Window::ListDisplays();
    Window::DisplayChoice choice;
    if (window_) {
      choice = window_->GetDisplayChoice();
    } else {
      choice.name = rex::cvar::GetFlagByName("monitor_name");
      choice.index = std::atoi(rex::cvar::GetFlagByName("monitor").c_str());
    }
    auto label = [&](size_t i) {
      const Window::DisplayInfo& d = displays[i];
      char text[192];
      std::snprintf(text, sizeof(text), "%zu: %s (%ux%u, %.0f Hz%s)", i + 1, d.name.c_str(), d.width,
                    d.height, d.refresh_rate, d.primary ? ", main screen" : "");
      return std::string(text);
    };
    const int32_t chosen = Window::FindDisplay(choice, displays);
    std::string preview = "Automatic";
    if (chosen > 0)
      preview = label(size_t(chosen - 1));
    else if (!choice.IsAutomatic())
      preview = (choice.name.empty() ? "Screen " + std::to_string(choice.index) : choice.name) +
                " (not connected)";
    auto pick = [&](const Window::DisplayChoice& c) {
      rex::cvar::SetFlagByName("monitor_name", c.name);
      rex::cvar::SetFlagByName("monitor", std::to_string(c.index));
      if (window_) window_->SetDisplayChoice(c);
      Save();  // at once: the restart at a mission's end doesn't save F2's changes
    };
    if (ImGui::BeginCombo("Display", preview.c_str())) {
      if (ImGui::Selectable("Automatic (the system's choice)", choice.IsAutomatic())) pick({});
      for (size_t i = 0; i < displays.size(); ++i) {
        if (ImGui::Selectable(label(i).c_str(), chosen == int32_t(i + 1)))
          pick({displays[i].name, int32_t(i + 1)});
      }
      ImGui::EndCombo();
    }
    if (window_) {
      const int32_t now = window_->GetCurrentDisplay();
      if (now > 0 && size_t(now) <= displays.size())
        ImGui::TextDisabled("The game is on %s.", label(size_t(now - 1)).c_str());
    }
    Help("Which screen the game plays on. It's kept by the screen's name, so it stays the same when "
         "other screens are plugged in or out; if that screen isn't connected, the system picks one. "
         "Applies at once (fullscreen moves along) and is kept for the next start.");
  }
  // [claude-9b] No "Start behind other windows" row: start_in_background (SDK
  // patch 0063) is a launch option for testing only (owner 2026-10-20), see
  // UseStartInBackgroundAsLaunchOptionOnly in game/relaunch.h.
  {
    // The refresh rates the monitor offers at its current resolution
    // (SDK patch 0013). Picking one makes fullscreen an exclusive display mode
    // at that rate; "Desktop" keeps borderless at the desktop's own mode.
    const double cur = REXCVAR_GET(cod3_refresh_rate);
    std::vector<double> rates;
    if (window_) rates = window_->GetDisplayRefreshRates();
    auto label = [](double hz) {
      char text[32];
      if (std::abs(hz - std::round(hz)) < 0.005)
        std::snprintf(text, sizeof(text), "%.0f Hz", hz);
      else
        std::snprintf(text, sizeof(text), "%.2f Hz", hz);
      return std::string(text);
    };
    std::string preview = cur <= 0.0 ? std::string("Desktop's own (borderless)") : label(cur);
    if (ImGui::BeginCombo("Refresh rate (fullscreen)", preview.c_str())) {
      if (ImGui::Selectable("Desktop's own (borderless)", cur <= 0.0)) {
        REXCVAR_SET(cod3_refresh_rate, 0.0);
        if (window_) window_->SetFullscreenRefreshRate(0.0);
        dirty_ = true;
      }
      for (double hz : rates) {
        if (ImGui::Selectable(label(hz).c_str(), std::abs(hz - cur) < 0.01)) {
          REXCVAR_SET(cod3_refresh_rate, hz);
          if (window_) window_->SetFullscreenRefreshRate(hz);
          dirty_ = true;
        }
      }
      ImGui::EndCombo();
    }
    if (window_) {
      const double now = window_->GetDisplayRefreshRate();
      if (now > 0.0) ImGui::TextDisabled("The monitor is running at %s.", label(now).c_str());
    }
    Help("How many times a second the monitor refreshes in fullscreen. Pick your monitor's highest "
         "rate to see every frame the game draws (with V-Sync on, the game shows at most this many "
         "frames per second). \"Desktop's own\" keeps whatever the desktop uses, in borderless "
         "fullscreen. Applies at once when fullscreen; windowed play always uses the desktop's "
         "rate.");
  }
  {
    // Host V-Sync: wait for the monitor instead of showing a frame the
    // moment it is ready (tearing). The game's frame rate isn't tied to it.
    const bool vk = Has("vulkan_allow_present_mode_immediate");
    const bool dx = Has("d3d12_allow_variable_refresh_rate_and_tearing");
    if (vk || dx) {
      bool on = vk ? !GetBool("vulkan_allow_present_mode_immediate")
                   : !GetBool("d3d12_allow_variable_refresh_rate_and_tearing");
      if (ImGui::Checkbox("V-Sync (no tearing)", &on)) {
        if (vk) rex::cvar::SetFlagByName("vulkan_allow_present_mode_immediate", on ? "false" : "true");
        if (dx)
          rex::cvar::SetFlagByName("d3d12_allow_variable_refresh_rate_and_tearing", on ? "false" : "true");
        restart_needed_ = true;
        dirty_ = true;
      }
      Help("Shows each new picture only when the monitor starts a new refresh, so there is no "
           "tearing (a horizontal split while turning). It doesn't cap the game's frame rate; use "
           "the frame rate limit for that. Applies after a restart.");
    }
  }
  dirty_ |= CvarCheckbox("Black bars instead of stretching", "present_letterbox", &restart_needed_);  // [acct2-decomp]
  Help("Keeps the picture's shape with black bars when the window has another shape (for example "
       "16:9 chosen on an ultrawide monitor, or 4:3 on a 16:9 one), instead of stretching it to the "
       "window.");
}

// Video > Resolution: what the game draws and the picture's shape.
void SettingsWindow::DrawVideoResolution() {
  {
    // What the game draws and how (game/render_resolution.h): natively the
    // game draws the whole picture itself, no MSAA, no scaling, where it fits
    // the emulated EDRAM; never bigger than the window and then shrunk. The
    // same choices as the game's Options > Video and the setup tool.
    const std::string cur = REXCVAR_GET(cod3_render_resolution);
    uint32_t custom_w = 0, custom_h = 0;
    bool custom = cur.find('x') != std::string::npos &&
                  cod3::ParseResolutionChoice(cur, &custom_w, &custom_h);
    // The other names the game takes for two listed sizes.
    const std::string listed_as = cur == "4k" ? "2160p" : cur == "8k" ? "4320p" : cur;
    static bool show_custom = false;
    static char custom_text[24] = "";
    static std::string last_seen;
    if (cur != last_seen) {
      // A change made elsewhere (the game's Options, the console, the setup
      // tool) is shown as it is.
      show_custom = custom;
      if (custom) std::snprintf(custom_text, sizeof(custom_text), "%s", cur.c_str());
      last_seen = cur;
    }
    std::string preview = custom ? "Custom (" + cur + ")"
                                 : cur + " (not a size: \"Match the screen\" is used)";
    for (const auto& c : cod3::kResolutionChoices)
      if (listed_as == c.value) preview = c.name;
    if (ImGui::BeginCombo("Render resolution", preview.c_str())) {
      for (const auto& c : cod3::kResolutionChoices) {
        // One row is marked: the custom row while its box is open.
        if (ImGui::Selectable(c.name, !show_custom && listed_as == c.value)) {
          REXCVAR_SET(cod3_render_resolution, std::string(c.value));
          cod3::RequestRenderResolution();  // a live change (game/render_resolution.h)
          dirty_ = true;
          show_custom = false;  // the custom box closes at once
          custom = false;
        }
      }
      if (ImGui::Selectable("Custom size...", custom || show_custom)) {
        show_custom = true;
        std::snprintf(custom_text, sizeof(custom_text), "%s", custom ? cur.c_str() : "");
      }
      ImGui::EndCombo();
    }
    if (custom) show_custom = true;
    if (show_custom) {
      // A size of your own, WIDTHxHEIGHT, within the limits the game takes.
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
      const bool entered = ImGui::InputText("##custom_resolution", custom_text, sizeof(custom_text),
                                            ImGuiInputTextFlags_EnterReturnsTrue |
                                                ImGuiInputTextFlags_CharsNoBlank);
      ImGui::SameLine();
      uint32_t w = 0, h = 0;
      std::string typed = custom_text;
      for (char& ch : typed) ch = ch == 'X' ? 'x' : ch;
      const bool valid = typed.find('x') != std::string::npos && cod3::ParseResolutionChoice(typed, &w, &h);
      ImGui::BeginDisabled(!valid);
      if ((ImGui::Button("Use this size") || entered) && valid) {
        const std::string value = std::to_string(w) + "x" + std::to_string(h);
        REXCVAR_SET(cod3_render_resolution, value);
        cod3::RequestRenderResolution();
        dirty_ = true;
      }
      ImGui::EndDisabled();
      if (!valid && custom_text[0])
        ImGui::TextDisabled("Width x height, from %ux%u to %ux%u (for example 2560x1080).",
                            cod3::kMinCustomWidth, cod3::kMinCustomHeight, cod3::kMaxCustomWidth,
                            cod3::kMaxCustomHeight);
      // A size of another shape than the picture is drawn in the picture's
      // shape (game/render_resolution.h FitAspect), unless the shape follows it.
      const float picture = cod3::DisplayAspect();
      if (custom && Get("cod3_aspect_ratio") != cod3::kMatchResolution) {
        const float size_shape = float(custom_w) / float(custom_h);
        if (std::abs(size_shape - picture) > picture * 0.01f)
          ImGui::TextDisabled("%ux%u is %.2f:1 and the picture %.2f:1, so it is drawn in the "
                              "picture's shape. Aspect ratio \"Match the render resolution\" "
                              "keeps the size's own shape.",
                              custom_w, custom_h, size_shape, picture);
      }
    }
    // [acct2-decomp] "Xbox 360 original" draws the console's own size: these two don't apply to it.
    const bool original = Get("cod3_render_resolution") == "original";
    ImGui::BeginDisabled(original);
    {
      const char* v[] = {"native", "scaled"};
      const char* n[] = {"Native: the game draws every pixel itself",
                         "Scaled: smaller with the game's 2x MSAA, multiplied by the GPU"};
      if (CvarCombo("How it is drawn", "cod3_render_method", v, n, 2, &restart_needed_)) {
        cod3::RequestRenderResolution();
        dirty_ = true;
      }
    }
    if (Has("cod3_render_percent")) {
      // Applied when the slider is let go: each change remakes the game's
      // render targets.
      static int percent = 100;
      if (!ImGui::IsAnyItemActive()) percent = std::atoi(Get("cod3_render_percent").c_str());
      // [claude-9b] up to 200%: more pixels than the window shows, shrunk to fit (supersampling)
      ImGui::SliderInt("Resolution scale", &percent, int(cod3::kMinRenderPercent), int(cod3::kMaxRenderPercent),
                       "%d%%");
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        rex::cvar::SetFlagByName("cod3_render_percent", std::to_string(percent));
        cod3::RequestRenderResolution();
        dirty_ = true;
      }
    }
    ImGui::EndDisabled();
    if (original) ImGui::TextDisabled("With Xbox 360 original, the way it is drawn and the scale don't apply.");
    const cod3::RenderPlan now = cod3::GetRenderPlan();
    if (now.game_width) ImGui::TextWrapped("Now: the game draws %s.", cod3::DescribePlan(now).c_str());
    else if (cur == "original") ImGui::TextWrapped("Now: the game draws the Xbox 360's own 1040x624.");
    std::string detail;
    if (!now.note.empty()) detail = now.note + " ";
    if (const std::string note = cod3::RenderResolutionNote(); !note.empty()) detail += note + " ";
    Help((detail + "Render resolution: how many pixels the picture has. \"Match the screen\" uses "
                   "your monitor in fullscreen and the window's size when windowed; a size bigger "
                   "than that is drawn at the window's size, since it would only be shrunk. "
                   "Native (the default) means the game itself draws every pixel, without MSAA and "
                   "without any scaling: up to about 2 million pixels (1920x1080, 1920x1200) with "
                   "Vulkan, about 1440x810 with Direct3D 12, which keeps the Xbox 360's 10 MB of "
                   "render target memory. Above that (1440p, 4K, 8K, 2560x1080) the game draws the "
                   "largest size that fits and the GPU draws each of its pixels as 2x2, 3x3 or 4x4 "
                   "real pixels: a real render at the full resolution, not a stretched picture, "
                   "though the game's own effects stay at the smaller size. Scaled is the older "
                   "plan: at most 1280x720 with the game's own 2x MSAA, multiplied by the GPU. "
                   "Resolution scale below 100% draws fewer pixels and stretches them to the "
                   "screen, for speed. \"Xbox 360 original\" is the console's 1040x624, stretched, "
                   "for comparison. Applies at once (the picture stops for a moment).")
             .c_str());
    // The live switch (game/render_resolution.h): the game remakes its
    // render targets at the next frame boundary. Off, a change waits for the
    // next start, as before.
    if (CvarCheckbox("Change the resolution without restarting", "cod3_live_resolution", &restart_needed_)) {
      dirty_ = true;
      if (REXCVAR_GET(cod3_live_resolution)) cod3::RequestRenderResolution();
    }
    Help("On: a new render resolution (or a window of another size with \"Match the screen\") is "
         "applied while the game runs, between two frames. Off: it applies at the next start.");
  }
  {
    // The picture's shape (game/ultrawide.h). The view up and down stays the
    // game's: wider than 16:9 shows more at the sides (the HUD and menus stay
    // 16:9), narrower (down to the console's 4:3) less, with the HUD filling
    // the picture.
    const char* v[] = {"auto", cod3::kMatchResolution, "4:3", "16:10", "16:9", "21:9", "32:9"};
    const char* n[] = {"Automatic (the window's shape)", "Match the render resolution",
                       "4:3 (the classic TV shape)", "16:10", "16:9 (the game's own)",
                       "21:9 (ultrawide)", "32:9 (super ultrawide)"};
    constexpr int kCount = int(sizeof(v) / sizeof(v[0]));
    const std::string cur = Get("cod3_aspect_ratio");
    int listed = -1;
    for (int i = 0; i < kCount; ++i)
      if (cur == v[i]) listed = i;
    // The box for a shape of your own is open while one is set, or after
    // "Custom shape..." until a listed shape is picked. A change made
    // elsewhere (the console, the setup tool) is shown as it is.
    static bool show_custom = false;
    static char custom_text[24] = "";
    static std::string last_seen;
    if (cur != last_seen) {
      if (listed >= 0)
        show_custom = false;
      else
        std::snprintf(custom_text, sizeof(custom_text), "%s", cur.c_str());
      last_seen = cur;
    }
    auto in_range = [](float shape) {
      return shape >= cod3::kMinAspect - 0.001f && shape <= cod3::kMaxAspect + 0.001f;
    };
    std::string preview;
    float set_shape = 0.0f;
    if (listed >= 0)
      preview = n[listed];
    else if (!cod3::ParseAspectText(cur, &set_shape))
      preview = "Custom (" + cur + ": not a shape, Automatic is used)";
    else if (!in_range(set_shape))
      preview = "Custom (" + cur + ", kept to " + (set_shape < cod3::kMinAspect ? "4:3" : "48:9") + ")";
    else
      preview = "Custom (" + cur + ")";
    if (ImGui::BeginCombo("Aspect ratio", preview.c_str())) {
      for (int i = 0; i < kCount; ++i) {
        // One row is marked: the custom row while its box is open.
        if (ImGui::Selectable(n[i], !show_custom && listed == i)) {
          rex::cvar::SetFlagByName("cod3_aspect_ratio", v[i]);
          dirty_ = true;
          show_custom = false;  // the custom box closes at once (it stayed open before)
          listed = i;
        }
      }
      if (ImGui::Selectable("Custom shape...", show_custom || listed < 0)) {
        show_custom = true;
        std::snprintf(custom_text, sizeof(custom_text), "%s", listed >= 0 ? "" : cur.c_str());
      }
      ImGui::EndCombo();
    }
    if (listed < 0) show_custom = true;
    if (show_custom) {
      // A shape of your own (W:H or a number), from 4:3 to 48:9.
      ImGui::SetNextItemWidth(ImGui::GetFontSize() * 9.0f);
      const bool entered = ImGui::InputText("##custom_aspect", custom_text, sizeof(custom_text),
                                            ImGuiInputTextFlags_EnterReturnsTrue |
                                                ImGuiInputTextFlags_CharsNoBlank);
      ImGui::SameLine();
      float typed = 0.0f;
      const bool parses = cod3::ParseAspectText(custom_text, &typed);
      const bool valid = parses && in_range(typed);
      ImGui::BeginDisabled(!valid);
      if ((ImGui::Button("Use this shape") || entered) && valid) {
        rex::cvar::SetFlagByName("cod3_aspect_ratio", custom_text);
        dirty_ = true;
      }
      ImGui::EndDisabled();
      if (custom_text[0] && !parses)
        ImGui::TextDisabled("Width:height, for example 24:10, 16:10 or 2.39:1 (from 4:3 to 48:9).");
      else if (custom_text[0] && !valid)
        ImGui::TextDisabled("%.3f:1 is %s: a shape goes from 4:3 (1.333:1) to 48:9 (5.333:1).", typed,
                            typed < cod3::kMinAspect ? "narrower than 4:3" : "wider than 48:9");
      else if (custom_text[0])
        ImGui::TextDisabled("%.3f:1", typed);
    }
    const float shape = cod3::DisplayAspect();
    const char* from = cur == cod3::kMatchResolution ? ", the render resolution's shape" : "";
    if (shape > cod3::kSixteenByNine + cod3::kSameShape)
      ImGui::TextDisabled("Now: %.2f:1%s, %.0f%% wider than 16:9, with the HUD and menus 16:9 in the "
                          "middle.",
                          shape, from, (shape / cod3::kSixteenByNine - 1.0f) * 100.0f);
    else if (!cod3::IsSixteenByNine(shape))
      ImGui::TextDisabled("Now: %.2f:1%s, %.0f%% narrower than 16:9: less at the sides, the same view "
                          "up and down.",
                          shape, from, (1.0f - shape / cod3::kSixteenByNine) * 100.0f);
    else
      ImGui::TextDisabled("Now: 16:9, the game's own picture%s.", from);
    Help("The shape of the picture. The view up and down always stays the game's. Wider than 16:9 "
         "shows more of the world at the sides (the field of view settings still apply); the HUD, "
         "menus, movies and briefings stay 16:9 in the middle. Narrower (16:10, 4:3) shows less at "
         "the sides, and the HUD and menus fill the picture as on a 4:3 TV. Automatic follows the "
         "window, or the monitor in fullscreen; a window narrower than 16:9 gets the 16:9 picture "
         "with bars. Match the render resolution uses the shape of the render resolution's size: a "
         "custom 2560x1080 gives 2.37:1 and 1650x900 1.83:1, a named size (1080p, 1440p) 16:9, and "
         "\"Match the screen\" the window's shape. A custom shape can be anything from 4:3 to 48:9. "
         "A picture of another shape than the window gets black bars, or is stretched with "
         "Letterbox off. Applies at once; the game's resolution follows the new shape a moment "
         "later.");
  }
}

// Video > Frame rate: the limit and the counter.
void SettingsWindow::DrawVideoFrameRate() {
  {
    // com_maxfps is a limit, not a target: the game runs as fast as it can
    // up to this. Applies at once (the console's com_maxfps is the same thing).
    auto& engine = cod3::Engine::Get();
    static const int values[] = {0, 30, 60, 85, 120, 144, 165, 200, 240, 300, 360, 500, 1000};
    constexpr int count = static_cast<int>(sizeof(values) / sizeof(values[0]));
    const int cur = cod3::FrameRate::Get().limit();
    std::vector<std::string> labels;
    int idx = -1;
    for (int i = 0; i < count; ++i) {
      labels.push_back(values[i] == 0 ? "No limit" : std::to_string(values[i]));
      if (values[i] == cur) idx = i;
    }
    std::string preview = idx >= 0 ? labels[idx] : std::to_string(cur) + " (set in the console)";
    if (!engine.GameReady()) ImGui::BeginDisabled();
    if (ImGui::BeginCombo("Frame rate limit (com_maxfps)", preview.c_str())) {
      for (int i = 0; i < count; ++i) {
        if (ImGui::Selectable(labels[i].c_str(), i == idx)) {
          engine.SetMaxFps(values[i]);
          REXCVAR_SET(cod3_max_fps, values[i]);
          dirty_ = true;
        }
      }
      ImGui::EndCombo();
    }
    if (!engine.GameReady()) ImGui::EndDisabled();
    Help("The most frames per second the game may draw. It's a cap, not a target: when the PC "
         "can't keep up the game simply runs a little slower, instead of dropping to half. Applies "
         "immediately, like typing com_maxfps in the ~ console. Above 60 is new ground: the "
         "game's clock keeps its speed correct, but the levels were only ever played at 30-60 on "
         "the Xbox 360.");
  }
  {
    auto& engine = cod3::Engine::Get();
    const char* n[] = {"Off", "Frame rate", "Frame rate + range", "Frame rate + frame times"};
    int mode = engine.draw_fps();
    if (!engine.GameReady()) ImGui::BeginDisabled();
    if (ImGui::Combo("Show FPS (cg_drawFPS)", &mode, n, 4)) engine.SetDrawFps(mode);
    if (!engine.GameReady()) ImGui::EndDisabled();
    Help("A frame rate counter in the top-right corner, updated twice a second. \"Range\" adds the "
         "lowest and highest rate in that time; \"frame times\" adds the average and longest frame "
         "in milliseconds and the current limit. Green: at the limit (or 60+), yellow: 30+, red: "
         "below 30.");
  }
}

// Video > Colors: the picture's colors and HDR.
void SettingsWindow::DrawVideoColors() {
  {
    // game/color_output.h (claude-5d, SDK patch 0040): the display the game is
    // told about and the tone curve the picture goes out with. Applies at once.
    const char* v[] = {"display", "bt709"};
    const char* n[] = {"Matched to your display (sRGB)", "Console on an HDTV (BT.709)"};
    dirty_ |= CvarCombo("Picture colors", "cod3_color_space", v, n, 2, &restart_needed_);
    Help("Matched to your display: the game's colors as a PC screen should show them. Console on "
         "an HDTV: the Xbox 360 on a TV, a little darker with more contrast. Applies at once.");
  }
  if (Has("display_hdr")) {
    // SDK patch 0044 (rex/ui/hdr_output.h): an HDR10 picture (BT.2020, PQ)
    // for an HDR display. The picture keeps its look at the white level; only
    // its brightest highlights go up to the highlight level. Live.
    dirty_ |= CvarCheckbox("HDR output (HDR10, BT.2020)", "display_hdr", &restart_needed_);
    Help("For an HDR display. The game keeps its look at the white level below; only its brightest "
         "highlights (sky, sun, lamps, fire, muzzle flashes) get brighter, up to the highlight "
         "level. F2, the console and the port's other overlays stay at the white level. Needs HDR "
         "turned on in your system's display settings (on Linux a Wayland desktop with HDR on, such "
         "as KDE Plasma or GNOME) and the Vulkan graphics API. Applies at once.");
    if (GetBool("display_hdr")) {
      switch (rex::ui::GetHdrOutput()) {
        case rex::ui::HdrOutput::kActive:
          Help("Now: HDR10 picture.");
          break;
        case rex::ui::HdrOutput::kNotOffered:
          Help("Now: normal (SDR) picture: the display doesn't offer HDR to this window. Turn on HDR "
               "in your system's display settings, then switch this off and on again.");
          break;
        case rex::ui::HdrOutput::kNotSupported:
          Help("Now: normal (SDR) picture: HDR output needs the Vulkan graphics API.");
          break;
        default:
          break;
      }
      auto nits_slider = [&](const char* label, const char* cvar, float lo, float hi) {
        float value = static_cast<float>(std::strtod(Get(cvar).c_str(), nullptr));
        if (ImGui::SliderFloat(label, &value, lo, hi, "%.0f nits",
                               ImGuiSliderFlags_AlwaysClamp)) {
          rex::cvar::SetFlagByName(cvar, std::to_string(static_cast<int>(value + 0.5f)));
          dirty_ = true;
        }
      };
      nits_slider("HDR white level", "display_hdr_paper_white", 80.0f, 500.0f);
      Help("How bright the game's white is (and F2's). Match your desktop's brightness for normal "
           "content (KDE: SDR brightness; Windows: SDR content brightness). 203 is the standard.");
      nits_slider("HDR highlight level", "display_hdr_peak", 200.0f, 4000.0f);
      Help("The brightest the highlights get. At or below your display's peak brightness (its HDR "
           "rating: HDR400 = 400, HDR600 = 600, HDR1000 = 1000). The game's own pure white text "
           "(subtitles, some HUD) counts as a highlight too: lower this if it's too bright. Equal "
           "to the white level: no brighter highlights.");
    }
  }
}

// Video > Quality: edges, scaling, shadows, effects and textures.
void SettingsWindow::DrawVideoQuality() {
  Section("Anti-aliasing & scaling");
  {
    const char* v[] = {"none", "fxaa", "fxaa_extreme"};
    const char* n[] = {"Off", "FXAA", "FXAA (extreme)"};
    dirty_ |= CvarCombo("Post-process AA", "swap_post_effect", v, n, 3, &restart_needed_);
    Help("Smooths jagged edges after the picture is drawn. Costs very little; Extreme smooths more "
         "and softens the picture a bit more. Applies after a restart.");
  }
  // [claude-9b] 2026-10-27 "Draw the game's 2x MSAA as real 2x" moved to EMULATION > GPU.
  {
    // FSR 2 / 3 aren't offered (owner 2026-10-19): the game gives them no depth
    // or motion vectors, so they can't work as meant. A value already saved (a
    // Windows build made with the FidelityFX option) is still shown as the
    // current one; picking another drops it.
    const std::string cur = Get("present_effect");
    const bool temporal = cur == "fsr2" || cur == "fsr3";
    const char* v[] = {"bilinear", "cas", "fsr", cur.c_str()};
    const char* n[] = {"Bilinear", "AMD CAS (sharpen)", "AMD FSR 1 (spatial)",
                       cur == "fsr3" ? "AMD FSR 3 (experimental, no longer offered)"
                                     : "AMD FSR 2 (experimental, no longer offered)"};
    dirty_ |= CvarCombo("Output scaling", "present_effect", v, n, temporal ? 4 : 3,
                        &restart_needed_);
    // SDK patch 0064: CAS and FSR 1 are in every build.
    Help("How the finished picture is fitted to the window. Bilinear is plain and soft. AMD CAS "
         "sharpens it. AMD FSR 1 upscales with sharper edges, but only when the game draws fewer "
         "pixels than the window shows (a lower Render resolution, a Resolution scale under "
         "100%, or Xbox 360 original); at the window's own size it sharpens like CAS. Applies "
         "after a restart.");
    if (auto* info = rex::cvar::GetFlagInfo("present_effect"); info) {
      const auto& allowed = info->constraints.allowed_values;
      if (info->constraints.HasAllowedValues() &&
          std::find(allowed.begin(), allowed.end(), "cas") == allowed.end())
        Help("AMD CAS and FSR need the game built with SDK patch 0064 (run the setup tool's "
             "build again).");
    }
    // [claude-9b] Sharpening as a slider (owner 2026-10-26: "FSR sharpening should be an option with
    // slider"): FSR 1's RCAS takes a reduction in stops (0 = sharpest, 2 = softest), CAS an extra
    // sharpness (0-1); both shown as 0-100%. The SDK reads them when the window's output starts.
    if (cur == "fsr" && Has("present_fsr_sharpness_reduction")) {
      float pct = static_cast<float>((1.0 - GetDouble("present_fsr_sharpness_reduction", 0.2) / 2.0) * 100.0);
      if (SliderFloatBox("FSR sharpness", &pct, 0.0f, 100.0f, "%.0f%%")) {
        rex::cvar::SetFlagByName("present_fsr_sharpness_reduction",
                                 fmt::format("{:.3f}", (1.0 - std::clamp(pct, 0.0f, 100.0f) / 100.0) * 2.0));
        restart_needed_ = true;
        dirty_ = true;
      }
      Help("How much FSR 1 sharpens the picture: 100% the most, 0% none. 90% is AMD's default. "
           "Applies after a restart.");
    } else if (cur == "cas" && Has("present_cas_additional_sharpness")) {
      float pct = static_cast<float>(GetDouble("present_cas_additional_sharpness", 0.0) * 100.0);
      if (SliderFloatBox("CAS sharpness", &pct, 0.0f, 100.0f, "%.0f%%")) {
        rex::cvar::SetFlagByName("present_cas_additional_sharpness",
                                 fmt::format("{:.3f}", std::clamp(pct, 0.0f, 100.0f) / 100.0));
        restart_needed_ = true;
        dirty_ = true;
      }
      Help("Extra sharpening on top of AMD CAS's own: 0% its default, 100% the most. Applies after "
           "a restart.");
    } else {
      ImGui::TextDisabled("Sharpness: pick AMD CAS or AMD FSR 1 above.");
    }
  }
  {
    const char* v[] = {"-1", "0", "1", "2", "3", "4", "5"};
    const char* n[] = {"Game default", "Off", "1x", "2x", "4x", "8x", "16x"};
    dirty_ |= CvarCombo("Anisotropic filtering", "anisotropic_override", v, n, 7, &restart_needed_);
    Help("Keeps floors, roads and walls sharp when you look along them at a distance. 16x costs "
         "almost nothing on a modern GPU.");
  }

  Section("Shadows");
  {
    // game/shadows.h: "off" adds no model to the shadow maps (at once; the
    // sunlight they carry stays). A saved "high" (retired) shows as Original.
    const char* v[] = {"original", "off"};
    const char* n[] = {"Original", "Off"};
    if (CvarCombo("Shadow quality", "cod3_shadow_quality", v, n, 2, &restart_needed_)) dirty_ = true;
    Help("The shadows that soldiers, vehicles and other moving things cast in sunlight. The "
         "level's own light and shade are painted into it and stay either way. Original is the "
         "Xbox 360 game; at a resolution scale of 2 or 3 its shadow maps are drawn 2 or 3 times "
         "as sharp. Off removes them and keeps the sunlight (applies at once). The game's speed "
         "doesn't change.");
  }
#if defined(COD3_MP)
  {
    // [claude-9b-think] 2026-10-27 UNTESTED game/shadows.cpp cod3mp_split_shadows (FINDINGS-SHADOWS 1b): with 2-4
    // local players, how many shadow maps each view aims at nearby soldiers and vehicles.
    const char* v[] = {"game", "nearest", "off"};
    const char* n[] = {"As the game", "Nearest group only", "Off"};
    if (CvarCombo("Split-screen shadows", "cod3mp_split_shadows", v, n, 3, &restart_needed_)) dirty_ = true;
    Help("Split screen only. Each view draws a shadow map for the nearest group of soldiers or vehicles and one "
         "for the next group. Nearest group only skips the second; Off skips both (the sunlight stays, nothing "
         "moving casts a shadow). Fewer maps make split screen faster. Applies at once.");
  }
#endif

  // [post-effects] The game's own depth of field and motion blur on or off
  // (game/post_effects.h); both apply at once, in the campaign and in
  // multiplayer.
  Section("Effects");
  dirty_ |= CvarCheckbox("Depth of field", cod3::post_effects::kDepthOfField, &restart_needed_);
  Help(cod3::kMultiplayer
           ? "The blurred background the game draws where a map switches depth of field on. Off keeps "
             "the whole picture sharp. Only your screen changes. Applies at once."
           : "The blurred background the game draws while you aim down the sights, where a mission "
             "switches depth of field on. Off keeps the whole picture sharp. Applies at once.");
  dirty_ |= CvarCheckbox("Motion blur", cod3::post_effects::kMotionBlur, &restart_needed_);
  Help("The screen trails the game shows at some moments (each frame keeps part of the last one). "
       "Off never draws them; the moments themselves play the same. Applies at once.");

  Section("Textures");
  // The setup tool's waifu2x pack, handed to the SDK's texture replacement
  // when the GPU starts (game/hd_textures.h), so a change waits for a restart.
  dirty_ |= CvarCheckbox("HD textures (waifu2x)", "cod3_hd_textures", &restart_needed_);
  ImGui::TextDisabled("Now: %s.", cod3::DescribeHdTextures(cod3::GetHdTextures()));
  Help("The game's own textures upscaled to twice the detail by waifu2x, kept in the same "
       "compressed formats as the originals, so they take about 4 times the video memory for "
       "textures. The setup tool makes them (data/cod3/hd_textures) when \"Upscale the extracted "
       "textures\" is ticked; without them the game uses its own textures. Applies after a "
       "restart.");
}

void SettingsWindow::DrawGame() {
  auto& engine = cod3::Engine::Get();
  if (ResetButton("reset_game", "game settings")) {
    engine.SetFovSettings(cod3::fov::kDefaultFov, cod3::fov::kDefaultScale);
    // Aim assist follows the game's Sticky Aim again.
    // [claude-9b-think] Game > Prompts' rows (from CONTROLS: the button prompts and the battle actions) and the
    // letterbox, skip prompt position and hold.
    restart_needed_ |= ResetSettings({"cod3_aim_assist", "cod3_skip_movies", "cod3_skip_prompt",
                                      cod3::viewmodel::kOffsetX, cod3::viewmodel::kOffsetY, cod3::viewmodel::kOffsetZ,
                                      cod3::viewmodel::kPreset, "cod3_vehicle_fov_scale",  // [claude-9b]
                                      "cod3_key_prompts", "cod3_key_caps", "cod3_glyph_style", "cod3_glyph_color_mode",
                                      "cod3_key_cap_scale",  // [claude-9b-think]
                                      "cod3_qte_style", "cod3_qte_spinner_with_pad", "cod3_qte_spinner_scale",
                                      "cod3_letterbox", "cod3_skip_prompt_position", "cod3_skip_hold_ms"});
    ResetGlyphColors();  // [claude-9b-think] the prompt pictures' colors
    dirty_ = true;
  }
  // [claude-9b] Subtabs (owner 2026-10-20: "please add subtabs for whats under game in F2"),
  // as the Console tab has them; the reset button above covers all of them.
  if (!ImGui::BeginTabBar("##game_views")) return;
  if (ImGui::BeginTabItem("View")) {
    // Each slider's range follows the other value, so the effective FOV
    // (cg_fov x cg_fovScale) stays within 65-165 (game/fov.h).
    namespace fov = cod3::fov;
    float value = engine.fov();
    const fov::Range fov_range = fov::FovRange(engine.fov_scale());
    if (ImGui::SliderFloat("Field of view (cg_fov)", &value, fov_range.min, fov_range.max, "%.0f",
                           ImGuiSliderFlags_AlwaysClamp))
      engine.SetFov(value);
    Help("How wide your view is, in degrees. The Xbox 360 game uses 65. Also in Options > Gameplay "
         "and the ~ console (cg_fov). Its range follows the FOV scale below.");
    ImGui::TextDisabled("With FOV scale %.2f: %.0f to %.0f.", engine.fov_scale(), fov_range.min,
                        fov_range.max);
    float scale = engine.fov_scale();
    const fov::Range scale_range = fov::ScaleRange(engine.fov());
    if (ImGui::SliderFloat("FOV scale (cg_fovScale)", &scale, scale_range.min, scale_range.max, "%.2f",
                           ImGuiSliderFlags_AlwaysClamp))
      engine.SetFovScale(scale);
    Help("Multiplies the field of view, World at War style: 1.2 turns 65 into 78. Aiming down the "
         "sights is scaled the same way. Its range follows the field of view above.");
    ImGui::TextDisabled("With field of view %.0f: %.2f to %.2f.", engine.fov(), scale_range.min,
                        scale_range.max);
    ImGui::Text("Effective FOV: %.1f (kept within %.0f to %.0f)", engine.fov() * engine.fov_scale(),
                fov::kMinEffective, fov::kMaxEffective);
    Help("The field of view times the FOV scale. It never goes below 65 or above 165 (170 with the vehicle FOV "
         "scale in a vehicle), from here, the game's Options or the console. Vehicles, turrets and scripted moments may change the view "
         "while they last (aiming down the sights narrows it too); your values come back after them "
         "and at every level start, and only your own changes are saved.");
    if (fov::ScriptedViewActive())
      ImGui::TextDisabled("A scripted view has the FOV right now; yours comes back after it.");
    // [claude-9b] 2026-10-27 (owner 03:15: "vehicle fov needs to be a slider"; game/fov.cpp)
    if (Has("cod3_vehicle_fov_scale")) {
      float vehicle = static_cast<float>(GetDouble("cod3_vehicle_fov_scale", 1.0));
      if (SliderFloatBox("Vehicle FOV scale", &vehicle, 0.5f, 2.0f, "%.2f")) {
        rex::cvar::SetFlagByName("cod3_vehicle_fov_scale", std::to_string(std::clamp(vehicle, 0.5f, 2.0f)));
        dirty_ = true;
      }
      Help("Multiplies the field of view while you ride a vehicle or work a tank's gun (tanks, jeeps, "
           "bikes, their guns), on top of the FOV scale above; the mouse turns to match. 1 is the game's "
           "own view. In the ~ console: cod3_vehicle_fov_scale.");
    }
    ImGui::EndTabItem();
  }
  // [claude-9b] Weapon position with Counter-Strike 2's commands (game/viewmodel.h),
  // laid out like the Crosshair tab (owner 19:32: "slider bars similar to the
  // crosshair"): a preset list, then a slider per axis. By name, so a program
  // without that module has no Weapon subtab.
  if (Has(cod3::viewmodel::kPreset) && ImGui::BeginTabItem("Weapon")) {
    namespace vm = cod3::viewmodel;
    const int preset = std::clamp(GetInt(vm::kPreset), 0, 3);
    vm::Offsets shown{GetDouble(vm::kOffsetX), GetDouble(vm::kOffsetY), GetDouble(vm::kOffsetZ)};
    vm::PresetOffsets(preset, &shown);  // a preset shows its own position
    // Custom takes the position that is shown (a preset's own, until a slider moves).
    auto make_custom = [&] {
      rex::cvar::SetFlagByName(vm::kOffsetX, std::to_string(shown.x));
      rex::cvar::SetFlagByName(vm::kOffsetY, std::to_string(shown.y));
      rex::cvar::SetFlagByName(vm::kOffsetZ, std::to_string(shown.z));
      rex::cvar::SetFlagByName(vm::kPreset, "0");
    };
    const char* presets[] = {"Custom", "Desktop", "Couch", "Classic"};
    int picked = preset;
    if (ImGui::Combo("Preset", &picked, presets, 4)) {
      if (picked == 0) {
        make_custom();
      } else {
        rex::cvar::SetFlagByName(vm::kPreset, std::to_string(picked));
      }
      dirty_ = true;
    }
    Help("Where the weapon sits on screen, as in Counter-Strike 2. Aiming down the sights never moves: "
         "only FOV scale changes the aimed view.");
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Desktop, Couch and Classic are CS2's own positions (viewmodel_presetpos 1, 2, 3);\n"
                        "Custom is the sliders below. The offset fades out as the weapon comes up.");
    auto slider = [&](const char* label, const char* name, double value, float lo, float hi) {
      float f = static_cast<float>(value);
      if (SliderFloatBox(label, &f, lo, hi, "%.1f")) {  // slider + number box
        if (GetInt(vm::kPreset) != 0) make_custom();  // moving a slider makes the position Custom
        rex::cvar::SetFlagByName(name, std::to_string(f));
        dirty_ = true;
      }
    };
    slider("Right", vm::kOffsetX, shown.x, -2.5f, 2.5f);
    slider("Forward", vm::kOffsetY, shown.y, -2.0f, 2.0f);
    slider("Up", vm::kOffsetZ, shown.z, -2.0f, 2.0f);
    Help("Game units, Counter-Strike 2's ranges; 0 is the game's own place.");
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("In the ~ console: viewmodel_offset_x (right), viewmodel_offset_y (forward),\n"
                        "viewmodel_offset_z (up) and viewmodel_presetpos: a CS2 config line pastes as it is.\n"
                        "CS2's viewmodel_fov has no counterpart: the weapon uses the view's field of view.");
    // [claude-9b] 2026-10-27 The weapon in the left hand (owner 05:11), per local player.
    if (Has(vm::kFlip)) {
      ImGui::Spacing();
      dirty_ |= CvarCheckbox("Left-handed weapon (mirrored)", vm::kFlip, &restart_needed_);
      Help("Mirrors the weapon and your hands to the left. \"Right\" above then moves the weapon outward, to the "
           "left. Not on mounted guns. A key can switch it too: \"Flip weapon hand\" in the key list.");
#if defined(COD3_MP)
      dirty_ |= CvarCheckbox("Player 2: left-handed weapon", vm::kFlipPlayers[1], &restart_needed_);
      dirty_ |= CvarCheckbox("Player 3: left-handed weapon", vm::kFlipPlayers[2], &restart_needed_);
      dirty_ |= CvarCheckbox("Player 4: left-handed weapon", vm::kFlipPlayers[3], &restart_needed_);
      Help("Split-screen players 2 to 4, each their own.");
#endif
    }
    ImGui::EndTabItem();
  }
  // [claude-9b-think] 2026-10-27 UNTESTED Game > Prompts (owner 13:28, 13:29): campaign and multiplayer.
  if (ImGui::BeginTabItem("Prompts")) {
    DrawGamePrompts();
    ImGui::EndTabItem();
  }
#if !defined(COD3_MP)  // [mp] multiplayer plays no films; aim assist is per profile there (later)
  if (ImGui::BeginTabItem("Cutscenes")) {
    dirty_ |= CvarCheckbox("Skip cutscenes automatically", "cod3_skip_movies", &restart_needed_);
    Help("Skips the logos at start, each level's briefing film and the level's opening intro as "
         "soon as the game allows it (the legal screen stays 5 seconds; a briefing only once the "
         "level has loaded). Without it, hold Space, Enter or Escape, the left mouse button or the "
         "controller's A to skip one (Hold to skip, below).");
    // [claude-9b-think] 2026-10-27 UNTESTED (owner 13:19) game/ultrawide.cpp cod3_letterbox.
    dirty_ |= CvarCheckbox("Black bars in in-game cutscenes", "cod3_letterbox", &restart_needed_);  // [acct2-decomp]
    Help("The black bars at the top and bottom of the screen during the levels' scripted in-game scenes. Off: "
         "the scene fills the screen; the scene itself is unchanged (the HUD stays hidden, the weapon put away, "
         "the subtitles where the game puts them).");
    // [acct2-decomp] The skip prompt's rows (they were on the Prompts tab): greyed out while they do nothing.
    const bool auto_skip = GetBool("cod3_skip_movies");
    Section("Skip prompt and hold");
    if (auto_skip) ImGui::TextDisabled("Skip cutscenes automatically is on: there is nothing to hold or show.");
    ImGui::BeginDisabled(auto_skip);
    dirty_ |= CvarCheckbox("Show the skip prompt", "cod3_skip_prompt", &restart_needed_);
    Help("Shows \"SPACE Skip\" (a controller's button while you use one) while a level's briefing can be "
         "skipped: for 4 seconds when it starts and again for 4 seconds after any key or button press.");
    {
      const char* v[] = {"corner", "bottom_center"};
      const char* n[] = {"Bottom-right corner", "Bottom center"};
      ImGui::BeginDisabled(!GetBool("cod3_skip_prompt"));
      dirty_ |= CvarCombo("Skip prompt position", "cod3_skip_prompt_position", v, n, 2, &restart_needed_);
      ImGui::EndDisabled();
      Help("The corner keeps it out of the subtitles' way; bottom center is where it was.");
    }
    if (Has("cod3_skip_hold_ms")) {
      int ms = std::clamp(GetInt("cod3_skip_hold_ms", 1000), 0, 5000);
      if (SliderIntBox("Hold to skip", &ms, 0, 5000, "%d ms")) {
        rex::cvar::SetFlagByName("cod3_skip_hold_ms", std::to_string(std::clamp(ms, 0, 5000)));
        dirty_ = true;
      }
      Help("How long Space, Enter, Escape, the left mouse button or the controller's A / START is held to skip; "
           "the prompt fills while you hold, and letting go early cancels. While the prompt is hidden, the first "
           "press only shows it. 0: a press skips at once.");
      if (!GetBool("cod3_skip_prompt") && ms > 0)
        ImGui::TextDisabled("The prompt is off: the hold still counts, with no fill to show it (0 skips on a press).");
    }
    ImGui::EndDisabled();
    ImGui::EndTabItem();
  }
  if (ImGui::BeginTabItem("Aim")) {
    if (auto on = cod3::AimAssistEnabled()) {
      bool v = *on;
      if (ImGui::Checkbox("Aim assist (Sticky Aim)", &v)) {
        cod3::SetAimAssist(v);
        dirty_ = true;
      }
    } else {
      ImGui::BeginDisabled();
      bool v = false;
      ImGui::Checkbox("Aim assist (Sticky Aim)", &v);
      ImGui::EndDisabled();
    }
    Help("Slows your aim down while it passes over an enemy, as on the console. Controller only: with the "
         "keyboard and mouse the pull is off. It's the same switch as Options > Gameplay > Sticky Aim in the game's menu "
         "(available once a profile is loaded).");
    cod3::sticky_aim::DrawF2Section();  // [claude-9b] what applies now (game/sticky_aim.h)
    ImGui::EndTabItem();
  }
  // [movement] Sprint and the movement style (game/movement/settings.h): the
  // same rows as the game's Pause > Options > MOVEMENT OPTIONS.
  if (ImGui::BeginTabItem("Movement")) {
    dirty_ |= cod3::movement::DrawF2Section();
    ImGui::EndTabItem();
  }
  // [claude-9b] Every server_sided_protected dvar on its own tab, only while
  // this machine is the host (owner 2026-10-20 20:42): ui/server_tab.h.
  if (server_tab::Shown() && ImGui::BeginTabItem("Server")) {
    dirty_ |= server_tab::Draw();
    ImGui::EndTabItem();
  }
#endif
  // [claude-9b] The player's name (owner 2026-10-26: "Selectable names... the in-game dvar still uses
  // the gamertag"): the gamertag the game is given (SDK patch 0070's user_name), both programs.
  if (Has("user_name") && ImGui::BeginTabItem("Player")) {
    static char name[32] = "";
    static bool editing = false;
    if (!editing) std::snprintf(name, sizeof(name), "%s", Get("user_name").c_str());
    ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
    const bool entered = ImGui::InputTextWithHint("Player name", "User", name, 16,
                                                  ImGuiInputTextFlags_EnterReturnsTrue);
    editing = ImGui::IsItemActive();
    if (entered || ImGui::IsItemDeactivatedAfterEdit()) {
      // [claude-setup] 2026-11-03 Made safe by the game's name rule (game/color_codes.h SanitizeName; owner: "these
      // user supplied entries need to be safe guarded too"); the SDK applies the same rule when the game asks.
      rex::cvar::SetFlagByName("user_name", cod3::color_codes::SanitizeName(name, 15));
      restart_needed_ = true;
      dirty_ = true;
      editing = false;
    }
    Help("Your name in the game, up to 15 characters (the gamertag's room): the name others see in "
         "multiplayer and the game's name setting. Empty: \"User\", the profile's own. The game reads "
         "it when it starts. Colors as in World at War: ^0 black, ^1 red, ^2 green, ^3 yellow, ^4 blue, ^5 cyan, "
         "^6 orange, ^7 white, ^8 and ^9 the team colors, ^: a rainbow; each code colors the letters after it and "
         "counts in the 15. Quotes, \\, % and ; are left out.");
    NameShowsAs(name, 15);
    // [claude-9b] 2026-10-27 Split-screen players 2-4 (owner 2026-10-26: "Player names: setup tool and per
    // split-screen player"): SDK patch 0074's user_name_2..4, the names XamUserGetName gives the game for
    // users 1-3 (no one is signed in for them).
    if (Has("user_name_2")) {
      static char others[3][32] = {};
      static bool other_editing[3] = {};
      static const char* const kNames[] = {"user_name_2", "user_name_3", "user_name_4"};
      static const char* const kLabels[] = {"Player 2 name", "Player 3 name", "Player 4 name"};
      static const char* const kHints[] = {"Player 2", "Player 3", "Player 4"};
      for (int i = 0; i < 3; ++i) {
        if (!other_editing[i]) std::snprintf(others[i], sizeof(others[i]), "%s", Get(kNames[i]).c_str());
        ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12.0f);
        const bool done = ImGui::InputTextWithHint(kLabels[i], kHints[i], others[i], 15,
                                                   ImGuiInputTextFlags_EnterReturnsTrue);
        other_editing[i] = ImGui::IsItemActive();
        if (done || ImGui::IsItemDeactivatedAfterEdit()) {
          // [claude-setup] 2026-11-03 The game's name rule, color codes included (SDK patch 0165 lets them through).
          rex::cvar::SetFlagByName(kNames[i], cod3::color_codes::SanitizeName(others[i], 14));
          restart_needed_ = true;
          dirty_ = true;
          other_editing[i] = false;
        }
        NameShowsAs(others[i], 14);
      }
      Help("The names of split-screen players 2 to 4 in multiplayer, up to 14 characters each, with the same "
           "colors as the player name. Empty: the game's own \"Player 2\" and so on. The game reads them when it "
           "starts.");
    }
    // [claude-9b] 2026-10-27 The player order (owner 2026-10-26: "player order 1-4 in F2"): which
    // player each controller plays as, by the order the controllers connected (SDK patch 0071's
    // input_player_order: one digit a controller, the first controller's player first).
    if (Has("input_player_order")) {
      ImGui::Spacing();
      ImGui::Separator();
      ImGui::TextUnformatted("Controllers");
      int order[4] = {0, 1, 2, 3};  // the player (0-3) of each controller, in connection order
      {
        const std::string text = Get("input_player_order");
        int parsed[4] = {0, 1, 2, 3};
        bool used[4] = {};
        int count = 0;
        bool ok = true;
        for (char c : text) {
          if (c == ' ' || c == ',') continue;
          if (c < '1' || c > '4' || count >= 4 || used[c - '1']) {
            ok = false;
            break;
          }
          used[c - '1'] = true;
          parsed[count++] = c - '1';
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
      static const char* const kPads[] = {"First controller to connect", "Second controller",
                                          "Third controller", "Fourth controller"};
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
      if (ImGui::Button("Reset to connection order")) {  // [acct2-decomp]
        for (int i = 0; i < 4; ++i) order[i] = i;
        changed = true;
      }
      if (changed) {
        std::string text;
        for (int i = 0; i < 4; ++i) text += static_cast<char>('1' + order[i]);
        rex::cvar::SetFlagByName("input_player_order", text == "1234" ? "" : text);
        dirty_ = true;
      }
      Help("Which player each controller plays as in split screen, by the order the controllers "
           "connected. It applies at once. The game's log names each one when they connect "
           "(\"controller 1 (...) plays as player 2\"). The keyboard and mouse always play as "
           "player 1, together with the controller that is player 1.");
    }
    ImGui::EndTabItem();
  }
  ImGui::EndTabBar();
}

// [claude-9b-think] 2026-10-27 UNTESTED Game > Prompts (owner 13:28, 13:29: "The glyphs should be under gameplay"):
// how the game's prompts show keys and controller buttons and the battle actions' prompts (both were on CONTROLS),
// and in the campaign the cutscene skip prompt. Names, behavior and help as they were; the Game tab's reset covers
// them.
void SettingsWindow::DrawGamePrompts() {
  Section("Button prompts");
  // Button prompts (game/button_prompts.h).
  dirty_ |= CvarCheckbox("Show keys in button prompts", "cod3_key_prompts", &restart_needed_);
  Help("While you play with the keyboard and mouse, the game's prompts show the key to press "
       "([R], [ENTER]) instead of a controller button. Pressing a controller button brings the "
       "buttons back.");
  // [acct2-decomp] The keys as key caps, like "SPACE Skip".
  dirty_ |= CvarCheckbox("Keys as key caps", "cod3_key_caps", &restart_needed_);
  Help("Draws the keys in the game's prompts as key caps, like the \"SPACE Skip\" prompt, mouse "
       "buttons as a small mouse, and the PlayStation, Nintendo and GameCube buttons as pictures. Off: "
       "the names in brackets ([R], [CROSS]).");
  // [claude-9b-think] 2026-10-27 UNTESTED (owner 15:01): the caps' size; the text keeps room for them.
  if (Has("cod3_key_cap_scale")) {
    if (!GetBool("cod3_key_caps")) ImGui::BeginDisabled();
    float cap_scale = static_cast<float>(GetDouble("cod3_key_cap_scale", 1.0));
    if (SliderFloatBox("Key cap size", &cap_scale, 0.8f, 1.8f, "%.2f")) {
      rex::cvar::SetFlagByName("cod3_key_cap_scale", std::to_string(std::clamp(cap_scale, 0.8f, 1.8f)));
      dirty_ = true;
    }
    if (!GetBool("cod3_key_caps")) ImGui::EndDisabled();
    Help("How big the key caps and button pictures are in the game's prompts (1: a little taller than the "
         "text's capital letters). The text keeps room for them, so bigger caps can move words to the next line "
         "(texts already on screen follow within a second). They stop short of the lines above and below.");
  }
  {
    // [mp-menus] auto: each player's own controller (split screen too).
    // [claude-9b-think] playstation2: the PS2 style (a choice only; auto shows a PlayStation pad the PS4/5 style).
    const char* v[] = {"auto", "xbox", "playstation", "playstation2", "nintendo", "gamecube"};
    const char* n[] = {"Each controller's own", "Xbox 360 (the game's own)", "PlayStation 4 / 5",
                       "PlayStation 2", "Nintendo Switch", "GameCube"};
    dirty_ |= CvarCombo("Controller button style", "cod3_glyph_style", v, n, 6, &restart_needed_);
    Help("How the prompts show controller buttons. Each controller's own: every player sees the "
         "buttons of the controller in their hands (an Xbox pad the game's pictures, a "
         "PlayStation pad the PlayStation 4 / 5 style, a Switch pad its own). The other choices "
         "show one style to everyone. PlayStation, Nintendo and GameCube buttons are simple "
         "pictures of our own (with \"Keys as key caps\" on; else their names).");
  }
  {
    // [claude-setup] 2026-11-03 (owner ~09:55: by default the buttons as the original, a toggle for customized
    // colors; ~10:00: "also add rainbow option"): was the "Colored controller buttons" checkbox (off: gold).
    // [claude-setup] 2026-11-03 (owner: "similar to ^: from WAW"; asked, "Both as choices"): Rainbow (^ colors).
    const char* v[] = {"original", "custom", "rainbow", "rainbow_codes", "gold"};
    const char* n[] = {"Original (like the controller's own)", "Custom (the colors below)", "Rainbow (smooth)",
                       "Rainbow (^ colors)", "Gold, like the key caps"};
    dirty_ |= CvarCombo("Button colors", "cod3_glyph_color_mode", v, n, 5, &restart_needed_);
    Help("Original: the PlayStation, Nintendo and GameCube buttons in colors close to the controller's own, the "
         "key caps in the game's menu gold. Custom: the colors below. Rainbow (smooth): the buttons and the key "
         "caps' edge fade slowly round the rainbow. Rainbow (^ colors): they step through World at War's ^ colors "
         "(red, orange, yellow, green, cyan, blue) twice a second, like its rainbow names. Gold: the controller "
         "buttons in the key caps' gold style too.");
  }
  // [claude-9b-think] 2026-10-27 UNTESTED the pictures' colors (owner 13:59), like the crosshair's own colors.
  ImGui::Spacing();
  ImGui::TextUnformatted("Colors");
  if (CurrentGlyphMode() != GlyphMode::kCustom)
    ImGui::TextDisabled("Used with Button colors \"Custom\"; changing one switches to it.");
  dirty_ |= GlyphColorRow(kGlyphAccent);
  Help("The key caps' edge and name, the mouse and the skip prompt (the game's menu gold by default). Default puts "
       "it back (hold the pointer over it to see the default).");
  dirty_ |= GlyphColorRow(kGlyphFill);
  Help("The inside of the key caps, and how see-through it is: A (or the picker's last bar) from 255, solid, to 0, "
       "clear. The swatch shows the color solid on its left half and as it is drawn, over a checkerboard, on its "
       "right. The preview below shows the result.");
  if (ImGui::TreeNode("Each button's color")) {
    ImGui::TextDisabled("PlayStation (the shapes)");
    for (int id : {kPsCross, kPsCircle, kPsSquare, kPsTriangle}) dirty_ |= GlyphColorRow(id);
    ImGui::TextDisabled("GameCube");
    for (int id : {kGcA, kGcB, kGcX, kGcY, kGcZ}) dirty_ |= GlyphColorRow(id);
    ImGui::TextDisabled("Nintendo (the dark grey by default)");
    for (int id : {kNxA, kNxB, kNxX, kNxY}) dirty_ |= GlyphColorRow(id);
    ImGui::TreePop();
  }
  if (ImGui::Button("Reset colors")) {
    ResetGlyphColors();
    dirty_ = true;
  }
  ImGui::SameLine();
  ImGui::TextDisabled("Every color above back to its default.");
  DrawGlyphPreview();
  dirty_ |= DrawQteSettings();  // Battle actions (ui/qte_spinner.h): the stick actions' prompts
  // [acct2-decomp] The cutscene skip prompt's rows are on the Cutscenes tab now, next to the automatic skip.
}

// [claude-9b] SOUND, right next to VIDEO (owner 2026-10-21: "sound needs to be right next to
// video within f2"); it was a GAME subtab.
void SettingsWindow::DrawSound() {
  if (ResetButton("reset_sound", "sound settings")) {
    ResetSettings({"cod3_volume", "audio_device"});
    Save();  // at once, as a picked sound output is (DrawSoundOutput)
  }
  if (Has("cod3_volume")) {
    float vol = float(GetDouble("cod3_volume", 1.0)) * 100.0f;
    if (ImGui::SliderFloat("Master volume", &vol, 0.0f, 100.0f, "%.0f%%")) {
      rex::cvar::SetFlagByName("cod3_volume", std::to_string(vol / 100.0f));
      dirty_ = true;
    }
    Help("Everything the game plays. Music, effects and voices have their own sliders in the "
         "game's Options > Sound. Console: snd_volume 0-1.");
  }
  DrawSoundOutput();
  if (cod3::voice::DrawSettingsRow()) Save();  // [voice] saved at once, as the sound output is
}

// The sound output device (SDK patch 0032, rex/audio/output_device.h). The
// game restarts its process between missions, and a device picked in the
// system's own mixer (pavucontrol, Windows' Volume mixer) doesn't survive
// that, because SDL opens the system default by name each time. A device
// chosen here is a setting, so the new process opens it again.
void SettingsWindow::DrawSoundOutput() {
  if (!Has("audio_device")) return;
  static std::vector<std::string> outputs;  // refreshed when the list opens
  const std::string chosen = rex::cvar::GetFlagByName("audio_device");
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
    Save();   // at once: the restart at a mission's end doesn't save F2's changes
  }
  const std::string playing = rex::audio::CurrentOutputDevice();
  if (!playing.empty()) ImGui::TextDisabled("Playing on: %s", playing.c_str());
  Help("Where the game's sound plays, saved at once and kept when the game restarts between "
       "missions. Switching the output in the system's sound settings while playing moves the "
       "game's sound too, and this row takes the new device. A stream moved in a mixer instead "
       "goes back at that restart.");
}

void SettingsWindow::DrawCrosshair() {
  bool changed = false;
  if (ResetButton("reset_crosshair", "crosshair settings")) {
    ResetSettings({"cod3_crosshair", "cod3_crosshair_style", "cod3_crosshair_size", "cod3_crosshair_thickness",
                   "cod3_crosshair_gap", "cod3_crosshair_outline", "cod3_crosshair_outline_thickness",
                   "cod3_crosshair_dot", "cod3_crosshair_constant_color", "cod3_crosshair_color",
                   "cod3_crosshair_own_colors", "cod3_crosshair_color_normal", "cod3_crosshair_color_enemy",
                   "cod3_crosshair_color_friendly", "cod3_crosshair_color_dead"});
    changed = true;
  }
  bool per_player = false;
#if defined(COD3_MP)
  // [claude-setup] 2026-11-03 split screen: one box per local player, each that player's own switch (LocalCrosshairs).
  if (uint8_t* base = GuestBase()) {
    const std::vector<LocalCrosshair> players = LocalCrosshairs(base);
    if (players.size() > 1) {
      per_player = true;
      ImGui::TextUnformatted("Show crosshair");
      for (const LocalCrosshair& p : players) {
        ImGui::SameLine();
        bool on = cod3::guest::ReadU8(base, p.byte) != 0;
        char label[40];
        std::snprintf(label, sizeof(label), "Player %u##show_crosshair_%u", p.player + 1, p.player);
        if (ImGui::Checkbox(label, &on)) cod3::guest::WriteU8(base, p.byte, on ? 1 : 0);
      }
      Help("Each split-screen player's own crosshair switch, the same as that player's Options > Gameplay > "
           "Crosshair: off hides that player's crosshair, the game's and the custom one, on their part of the "
           "screen only. Player 1's is remembered as before; for the others, set it in their own Options to keep "
           "it after the game closes.");
    }
  }
#endif
  if (!per_player) {
    // [claude-9b] The game's own crosshair switch (owner 2026-10-26: "F2 crosshair menu should have a
    // toggle box for show crosshair that reflects the in-game one menu toggle"): Options > Gameplay >
    // Crosshair, read from the game every time F2 draws, so both places always agree.
    const int game_on = GameCrosshairOption();
    bool show = game_on != 0;
    ImGui::BeginDisabled(game_on < 0);
    if (ImGui::Checkbox("Show crosshair", &show)) SetGameCrosshairOption(show);
    ImGui::EndDisabled();
    Help(game_on < 0 ? "The game's own crosshair switch (Options > Gameplay > Crosshair): shown once the "
                       "game has started."
                     : "The game's own crosshair switch, the same as Options > Gameplay > Crosshair: off "
                       "hides the crosshair, the game's and the custom one. The game saves it with your "
                       "profile.");
  }
  bool en = REXCVAR_GET(cod3_crosshair);
  if (ImGui::Checkbox("Use custom crosshair", &en)) {
    REXCVAR_SET(cod3_crosshair, en);
    changed = true;
  }
  Help("Draws the crosshair below instead of the game's. It appears only when the game would show "
       "its own: not in menus or cutscenes, and it fades out as you aim down the sights. Show "
       "crosshair above (the game's own switch) still hides it.");
  {
    // [claude-9b] Preview at its in-game size (owner 2026-10-20: "the crosshair menu needs a preview").
    const float side = std::floor(ImGui::GetFrameHeight() * 5.0f);
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const ImVec2 p1(p0.x + side, p0.y + side);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(p0, p1, IM_COL32(40, 40, 40, 255));
    dl->AddRect(p0, p1, ImGui::GetColorU32(ImGuiCol_Border));
    dl->PushClipRect(p0, p1, true);
    DrawCrosshairPreviewShape(dl, ImVec2(std::floor((p0.x + p1.x) * 0.5f), std::floor((p0.y + p1.y) * 0.5f)),
                              std::max(0.5f, ImGui::GetIO().DisplaySize.y / 1080.0f));
    dl->PopClipRect();
    ImGui::Dummy(ImVec2(side, side));
    ImGui::SameLine();
    ImGui::TextDisabled("Preview, at its size in the game%s.", REXCVAR_GET(cod3_crosshair) ? "" : " (off now)");
  }
  const char* styles[] = {"Classic cross", "Cross + dot", "Dot", "Circle", "T"};
  int st = REXCVAR_GET(cod3_crosshair_style);
  if (ImGui::Combo("Style", &st, styles, 5)) {
    REXCVAR_SET(cod3_crosshair_style, st);
    changed = true;
  }
  auto slider = [&](const char* label, double& value, float lo, float hi) {
    float f = static_cast<float>(value);
    if (SliderFloatBox(label, &f, lo, hi, "%.1f")) {  // [claude-9b] slider + number box (owner: "sliders + textbox")
      value = f;
      changed = true;
    }
  };
  // [acct2-decomp] the cvars' own ranges (they were 0-32, 0.5-8, -4..32: a console value beyond was cut)
  slider("Length", REXCVAR_GET(cod3_crosshair_size), 0.0f, 64.0f);
  slider("Thickness", REXCVAR_GET(cod3_crosshair_thickness), 0.5f, 16.0f);
  slider("Gap", REXCVAR_GET(cod3_crosshair_gap), -8.0f, 64.0f);
  Help("Sizes are in pixels on a 1080p screen and scale with the window. Gap is the space between "
       "the center and each arm.");
  bool outline = REXCVAR_GET(cod3_crosshair_outline);
  if (ImGui::Checkbox("Outline", &outline)) {
    REXCVAR_SET(cod3_crosshair_outline, outline);
    changed = true;
  }
  if (outline) slider("Outline thickness", REXCVAR_GET(cod3_crosshair_outline_thickness), 0.5f, 4.0f);
  Help("A thin black border that keeps the crosshair visible on bright backgrounds.");
  bool dot = REXCVAR_GET(cod3_crosshair_dot);
  if (ImGui::Checkbox("Center dot", &dot)) {
    REXCVAR_SET(cod3_crosshair_dot, dot);
    changed = true;
  }
  bool constant = REXCVAR_GET(cod3_crosshair_constant_color);
  if (ImGui::Checkbox("Constant color", &constant)) {
    REXCVAR_SET(cod3_crosshair_constant_color, constant);
    changed = true;
  }
  Help("Off (default): the crosshair changes color the way the game's own does: white, red over an "
       "enemy, green over a friendly, black over a dead body. On: always the color below.");
  auto s = CurrentCrosshair();
  float col[4] = {s.color[0], s.color[1], s.color[2], s.color[3]};
  if (!constant) ImGui::BeginDisabled();
  if (ImGui::ColorEdit4("Color", col)) {
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%d %d %d %d", int(col[0] * 255 + .5f), int(col[1] * 255 + .5f),
                  int(col[2] * 255 + .5f), int(col[3] * 255 + .5f));
    REXCVAR_SET(cod3_crosshair_color, std::string(buf));
    changed = true;
  }
  if (!constant) ImGui::EndDisabled();
  {
    // [claude-9b] A color of your own for each of the game's cases (owner 2026-10-26).
    bool own = REXCVAR_GET(cod3_crosshair_own_colors);
    ImGui::BeginDisabled(constant);
    if (ImGui::Checkbox("My own colors for each case", &own)) {
      REXCVAR_SET(cod3_crosshair_own_colors, own);
      changed = true;
    }
    Help("The crosshair still changes color when the game's does, but to your colors: over nothing, "
         "over an enemy, over a friendly and over a dead body. Off: the game's white, red, green and "
         "black. Not used with Constant color.");
    if (own && !constant) {
      struct Case {
        const char* label;
        const char* setting;
      };
      static constexpr Case kCases[] = {{"Over nothing", "cod3_crosshair_color_normal"},
                                        {"Over an enemy", "cod3_crosshair_color_enemy"},
                                        {"Over a friendly", "cod3_crosshair_color_friendly"},
                                        {"Over a dead body", "cod3_crosshair_color_dead"}};
      for (const Case& k : kCases) {
        auto c = ParseRgba(Get(k.setting), {1.0f, 1.0f, 1.0f, 1.0f});
        if (ImGui::ColorEdit4(k.label, c.data())) {
          rex::cvar::SetFlagByName(k.setting, fmt::format("{} {} {} {}", int(c[0] * 255 + .5f), int(c[1] * 255 + .5f),
                                                          int(c[2] * 255 + .5f), int(c[3] * 255 + .5f)));
          changed = true;
        }
      }
    }
    ImGui::EndDisabled();
    // [claude-9b] Owner 2026-10-26: "an option for resetting the colors".
    if (ImGui::Button("Reset colors")) {
      ResetSettings({"cod3_crosshair_constant_color", "cod3_crosshair_color", "cod3_crosshair_own_colors",
                     "cod3_crosshair_color_normal", "cod3_crosshair_color_enemy", "cod3_crosshair_color_friendly",
                     "cod3_crosshair_color_dead"});
      changed = true;
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Back to the game's own colors.");
  }
  // Preview: the chosen color, or the game's normal white.
  ImGui::Text("Preview:");
  ImVec2 p = ImGui::GetCursorScreenPos();
  ImGui::Dummy(ImVec2(120, 120));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->AddRectFilled(p, ImVec2(p.x + 120, p.y + 120), IM_COL32(70, 80, 70, 255));
  auto ps = CurrentCrosshair();
  if (!ps.constant_color) ps.color[0] = ps.color[1] = ps.color[2] = ps.color[3] = 1.0f;
  ImVec2 c(p.x + 60, p.y + 60);
  if (ps.outline) DrawCrosshairShape(dl, c, ps, 1.5f, IM_COL32(0, 0, 0, 255), ps.outline_thickness * 1.5f);
  DrawCrosshairShape(dl, c, ps, 1.5f,
                     ImGui::ColorConvertFloat4ToU32(ImVec4(ps.color[0], ps.color[1], ps.color[2], ps.color[3])),
                     0.0f);
  if (changed) dirty_ = true;
}

// [claude-9b-think] 2026-10-27 UNTESTED CONTROLS in subtabs (owner 13:28, 13:29): Mouse, Controller, Per weapon and
// Key bindings, each with its own reset. The button prompts and the battle actions moved to Game > Prompts.
void SettingsWindow::DrawControls() {
  if (!ImGui::BeginTabBar("##controls_views")) return;
  if (ImGui::BeginTabItem("Mouse")) {
    DrawControlsMouse();
    ImGui::EndTabItem();
  }
  if (ImGui::BeginTabItem("Controller")) {
    DrawControlsController();
    ImGui::EndTabItem();
  }
  if (ImGui::BeginTabItem("Per weapon")) {
    DrawControlsPerWeapon();
    ImGui::EndTabItem();
  }
  if (ImGui::BeginTabItem("Key bindings")) {
    DrawControlsKeys();
    ImGui::EndTabItem();
  }
  ImGui::EndTabBar();
}

void SettingsWindow::DrawControlsMouse() {
  if (ResetButton("reset_mouse", "mouse settings")) {
    // cod3_raw_mouse first: mnk_mouse's default depends on it.
    restart_needed_ |= ResetSettings({"mnk_mode", "cod3_raw_mouse", "mnk_mouse", "cod3_sensitivity",
                                      "cod3_zoom_sensitivity", "cod3_invert_mouse", "cod3_m_yaw",
                                      "cod3_m_pitch",
                                      "cod3_mouse_game_sensitivity"});  // [claude-9b]
    // [acct2-decomp] mnk_mouse's default was fixed at start (cod3_app.cpp), not by the reset: the Mouse aim
    // combo's own rule, so raw input and the stick emulation are never on together (the view would turn twice).
    if (REXCVAR_GET(cod3_raw_mouse)) rex::cvar::SetFlagByName("mnk_mouse", "false");
    dirty_ = true;
  }
  dirty_ |= CvarCheckbox("Keyboard & mouse", "mnk_mode", &restart_needed_);
  Help("Play with keyboard and mouse. A controller keeps working either way. The mouse is held by the game "
       "only during gameplay; menus, the console and this window get a normal cursor.");
  // [acct2-decomp] Off, the mouse aim, the key bindings' actions and the key prompts all stop
  // (mouse_look.cpp, keybinds/binds.cpp, button_prompts.cpp): the rows below say so instead of doing nothing.
  const bool mnk = GetBool("mnk_mode");
  if (!mnk) ImGui::TextDisabled("Keyboard & mouse is off: the rows below apply once it's on.");
  ImGui::BeginDisabled(!mnk);

  // One choice for the two underlying switches: raw aim (ours) or the SDK's
  // mouse-to-right-stick emulation. Never both, or the view turns twice.
  {
    bool raw = REXCVAR_GET(cod3_raw_mouse);
    bool stick = GetBool("mnk_mouse");
    int mode = raw ? 0 : (stick ? 1 : 2);
    const char* modes[] = {"Raw input (World at War style, 1:1)", "Right-stick emulation", "Off"};
    if (ImGui::Combo("Mouse aim", &mode, modes, 3)) {
      REXCVAR_SET(cod3_raw_mouse, mode == 0);
      rex::cvar::SetFlagByName("mnk_mouse", mode == 1 ? "true" : "false");
      dirty_ = true;
    }
    Help("Raw input turns the view exactly as far as you move the mouse, like World at War, at any "
         "frame rate. Right-stick emulation turns mouse movement into a stick push (has a top "
         "speed). Off leaves the mouse for menus only.");
    if (mode == 0) {
      float sens = static_cast<float>(REXCVAR_GET(cod3_sensitivity));
      if (ImGui::SliderFloat("Sensitivity (WaW scale)", &sens, 0.1f, 30.0f, "%.2f",
                             ImGuiSliderFlags_Logarithmic)) {
        REXCVAR_SET(cod3_sensitivity, static_cast<double>(sens));
        dirty_ = true;
      }
      // [claude-9b-think] The readout right under the sensitivity it follows.
      float cm360 = 0.0f;
      if (REXCVAR_GET(cod3_m_yaw) != 0.0 && sens > 0.0f)
        cm360 = static_cast<float>(360.0 / (sens * std::abs(REXCVAR_GET(cod3_m_yaw))));
      ImGui::Text("%.0f mouse counts per full turn", cm360);
      Help("The same number as World at War's sensitivity setting (its default is 5).");
      float zoom = static_cast<float>(REXCVAR_GET(cod3_zoom_sensitivity));
      if (ImGui::SliderFloat("ADS sensitivity multiplier", &zoom, 0.1f, 2.0f, "%.2f")) {
        REXCVAR_SET(cod3_zoom_sensitivity, static_cast<double>(zoom));
        dirty_ = true;
      }
      Help("Extra factor while aiming down the sights. The aim already slows down with the zoom; "
           "below 1 slows it more, above 1 less. Every weapon; Per weapon has each weapon's own.");
      bool inv = cod3::MouseLook::MouseInverted();
      if (ImGui::Checkbox("Invert vertical", &inv)) {
        cod3::MouseLook::SetMouseInverted(inv);
        dirty_ = true;
      }
      Help("Pushing the mouse forward looks down. The game's own Invert aim option (Options > "
           "Controls) inverts the controller and the mouse; this box shows the mouse as it is, so "
           "changing Invert aim in the game flips this box too.");  // [acct2-decomp] the two combine
      // [claude-9b] Owner 2026-10-26: the game's Horizontal / Vertical rows are the controller's only.
      dirty_ |= CvarCheckbox("Game's horizontal / vertical rows scale the mouse too", "cod3_mouse_game_sensitivity",
                             &restart_needed_);
      Help("Off (default): the mouse uses only the sensitivity above; the game's Options > Controls "
           "Horizontal / Vertical sensitivity rows are the controller's. On: those rows scale the mouse "
           "too (25 = 1x), as in earlier versions. The game's Invert aim flips both either way.");
      if (ImGui::TreeNode("Advanced (m_yaw / m_pitch)")) {
        float yaw = static_cast<float>(REXCVAR_GET(cod3_m_yaw));
        float pitch = static_cast<float>(REXCVAR_GET(cod3_m_pitch));
        // [acct2-decomp] Kept within the cvars' -1..1 and never 0 (0 stops that axis of the aim).
        auto usable = [](float v) { return std::isfinite(v) && std::abs(v) >= 0.0001f && std::abs(v) <= 1.0f; };
        if (ImGui::InputFloat("m_yaw", &yaw, 0.001f, 0.01f, "%.4f") && usable(yaw)) {
          REXCVAR_SET(cod3_m_yaw, static_cast<double>(yaw));
          dirty_ = true;
        }
        if (ImGui::InputFloat("m_pitch", &pitch, 0.001f, 0.01f, "%.4f") && usable(pitch)) {
          REXCVAR_SET(cod3_m_pitch, static_cast<double>(pitch));
          dirty_ = true;
        }
        Help("Degrees turned per mouse count at sensitivity 1, sideways and up/down. World at War "
             "uses 0.022 for both. A negative value turns that way round; 0 isn't taken.");
        ImGui::TreePop();
      }
    } else if (mode == 1) {
      // [claude-9b-think] mnk_sensitivity's row is on the Controller tab (owner 13:28).
      ImGui::TextDisabled("Its speed: Stick sensitivity, on the Controller tab.");
    }
  }
  ImGui::EndDisabled();  // [acct2-decomp] Keyboard & mouse
}

// [claude-9b-think] 2026-10-27 UNTESTED The Controller subtab (owner 13:28: "Stick sensitivity (mnk_sensitivity) and
// anything controller-only"). mnk_sensitivity is the SDK's mouse-to-right-stick factor: it applies only with Mouse
// aim set to Right-stick emulation (it was shown under that choice). A controller's own settings are the game's.
void SettingsWindow::DrawControlsController() {
  if (ResetButton("reset_controller", "controller settings")) {
    restart_needed_ |= ResetSettings({"mnk_sensitivity"});
    dirty_ = true;
  }
  if (Has("mnk_sensitivity")) {
    float sens = static_cast<float>(GetDouble("mnk_sensitivity", 1.0));
    if (ImGui::SliderFloat("Stick sensitivity", &sens, 0.05f, 10.0f, "%.2f",
                           ImGuiSliderFlags_Logarithmic)) {
      char buf[32];
      std::snprintf(buf, sizeof(buf), "%.3f", sens);
      rex::cvar::SetFlagByName("mnk_sensitivity", buf);
      dirty_ = true;
    }
    Help("How far a mouse movement pushes the emulated right stick: only with Mouse aim set to Right-stick "
         "emulation (Mouse tab).");
    if (REXCVAR_GET(cod3_raw_mouse) || !GetBool("mnk_mouse"))
      ImGui::TextDisabled("Not in use now: Mouse aim isn't Right-stick emulation.");
  }
  ImGui::Spacing();
  Help("A controller's own stick speed, Invert aim and layout are the game's Options > Controls; Sticky Aim is "
       "Game > Aim (Options > Gameplay in the game); each weapon's speed, the Per weapon tab; how the prompts "
       "show its buttons, Game > Prompts.");
}

// [claude-9b-think] 2026-10-27 UNTESTED Per weapon (owner 13:28): the table (mouse and controller alike), and once
// how it adds to the mouse's ADS multiplier (game/mouse_look.cpp: sensitivity x the zoom's own slow-down x the ADS
// multiplier while aimed, x the weapon's percentage, hip blending into aiming as the sights come up).
void SettingsWindow::DrawControlsPerWeapon() {
  // [claude-9b] Sensitivity per weapon (owner 2026-10-26: "the sliders for individual weapons sensitivity need
  // to be organized with other sensitivity controls").
  dirty_ |= cod3::aimsens::DrawF2Section();  // sensitivity per weapon, hip and aiming (game/aim_sensitivity.h)
  Help("With the mouse, aiming down the sights uses both: the Mouse tab's ADS sensitivity multiplier (every "
       "weapon) times Aiming here (the weapon in hand); 0.8 there and 50% here turn at 0.4x the zoom's own speed. "
       "A controller uses only this table.");
}

// [claude-9b-think] The Key bindings subtab: as it was (the section's title is the tab's).
void SettingsWindow::DrawControlsKeys() {
  if (ResetButton("reset_binds", "key bindings")) {
    restart_needed_ |= ResetSettings(
        {"keybind_a", "keybind_b", "keybind_x", "keybind_y", "keybind_left_trigger", "keybind_right_trigger",
         "keybind_left_shoulder", "keybind_right_shoulder", "keybind_lstick_up", "keybind_lstick_down",
         "keybind_lstick_left", "keybind_lstick_right", "keybind_lstick_press", "keybind_rstick_up",
         "keybind_rstick_down", "keybind_rstick_left", "keybind_rstick_right", "keybind_rstick_press",
         "keybind_dpad_up", "keybind_dpad_down", "keybind_dpad_left", "keybind_dpad_right", "keybind_back",
         "keybind_start", "keybind_guide", "cod3_binds_kbm_p1", "cod3_binds_actions", "cod3_aim_toggle_kbm_p1"});
    dirty_ = true;
  }
  // [keybinds] The game's own actions with keys of their own (fire, aim, use,
  // reload ...; game/keybinds/binds.h), above the controller rows.
  dirty_ |= cod3::binds::DrawF2Rows();
  // [keybinds] "Keys call the game's actions" on: the keyboard doesn't play as a
  // controller, so its controller button rows are hidden (owner 2026-10-20 21:18:
  // "if the user isn't using emulate controller for MNK remove access to the bind
  // controller buttons"); the movement rows stay (the left stick's rows underneath:
  // movement isn't an action of its own). Off: every row, as before.
  const bool pad_rows = !GetBool("cod3_binds_actions");
  auto movement_row = [](std::string_view name) {
    return name == "keybind_lstick_up" || name == "keybind_lstick_down" || name == "keybind_lstick_left" ||
           name == "keybind_lstick_right";
  };
  if (!pad_rows) {
    Help("The keys that walk and strafe (in a vehicle: steer). Separate keys with commas; what you type "
         "applies on Enter, when you click anywhere else (Save too) or close F2. Key names: A-Z, 0-9, "
         "SPACE, SHIFT, CONTROL, ALT, UP/DOWN/LEFT/RIGHT, mouse buttons LMB, RMB, MMB, MB4, MB5.");
  } else {
#if !defined(COD3_MP)
    Help("Each row is a controller button and the keys that press it: in the game's menus, and while you "
         "play for keys that no action above has (WASD moves through the left stick rows). Separate keys "
         "with commas; what you type applies on Enter, when you click anywhere else (Save too) or close F2. "
         "Key names: A-Z, 0-9, SPACE, TAB, ESCAPE, SHIFT, CONTROL, ALT, UP/DOWN/LEFT/RIGHT; mouse buttons "
         "LMB, RMB, MMB, and the side buttons MB4 (back) and MB5 (forward), also written MOUSE4 / MOUSE5. "
         "The left stick click and the D-pad are empty by default.");
#else
    Help("Each row is a controller button and the keys that press it: in the game's menus, and while you "
         "play for keys that no action above has (WASD moves through the left stick rows). Separate keys "
         "with commas; what you type applies on Enter, when you click anywhere else (Save too) or close F2. "
         "Key names: A-Z, 0-9, SPACE, TAB, ESCAPE, SHIFT, CONTROL, ALT, UP/DOWN/LEFT/RIGHT; mouse buttons "
         "LMB, RMB, MMB, and the side buttons MB4 (back) and MB5 (forward), also written MOUSE4 / MOUSE5. "
         "SHIFT+UP means both together. The two grenade rows throw the frag and the special grenade also "
         "when the controller layout swaps the shoulder buttons.");
#endif
  }
  static const char* kBinds[][2] = {
      {"keybind_right_trigger", "Fire (RT)"},   {"keybind_left_trigger", "Aim (LT)"},
#if !defined(COD3_MP)
      {"keybind_right_shoulder", "Grenade (RB)"}, {"keybind_left_shoulder", "Smoke grenade (LB)"},
#else  // [mp-controls] these follow a layout that swaps the shoulders (game/mp/controls_mp.h)
      {"keybind_right_shoulder", "Frag grenade"}, {"keybind_left_shoulder", "Special grenade"},
#endif
      {"keybind_a", "Jump (A)"},                {"keybind_b", "Stance (B)"},
      {"keybind_x", "Use/Reload (X)"},          {"keybind_y", "Switch weapon (Y)"},
#if !defined(COD3_MP)  // [keybinds] the campaign's L3 is +binoculars, which has its own key
      {"keybind_rstick_press", "Melee (RS)"},   {"keybind_lstick_press", "Left stick click (LS)"},
#else
      {"keybind_rstick_press", "Melee (RS)"},   {"keybind_lstick_press", "Sprint / hold breath (LS)"},
#endif
      {"keybind_lstick_up", "Forward"},         {"keybind_lstick_down", "Back"},
      {"keybind_lstick_left", "Strafe left"},   {"keybind_lstick_right", "Strafe right"},
      {"keybind_dpad_up", "D-pad up"},          {"keybind_dpad_down", "D-pad down"},
      {"keybind_dpad_left", "D-pad left"},      {"keybind_dpad_right", "D-pad right"},
      {"keybind_start", "Pause (Start)"},       {"keybind_back", "Objectives (Back)"},
  };
  if (ImGui::BeginTable("##binds", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
    for (auto& b : kBinds) {
      if (!Has(b[0])) continue;
      if (!pad_rows && !movement_row(b[0])) continue;  // [keybinds]
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(b[1]);
      ImGui::TableNextColumn();
      // [keybinds] capitals; applies on Enter, leaving the box or Save (game/keybinds/binds.h)
      dirty_ |= cod3::binds::DrawPadKeyRow(b[0]);
    }
    ImGui::EndTable();
  }
}

// Shader build threads for a low-end PC: half the logical CPUs minus one, at
// least one, so shader bursts leave the game's own threads (main, render job,
// GPU commands) their cores.
static int LowEndShaderThreads() {
  const int hw = static_cast<int>(std::max(1u, std::thread::hardware_concurrency()));
  return std::max(1, hw / 2 - 1);
}

// One click for a slow PC (2-4 CPU cores, integrated graphics): the settings
// that cost the most CPU or GPU time for the least difference on screen. The
// game's speed, the frame rate limit, V-Sync and the lens-flare fix stay as
// they are, and a setting already cheaper than the preset is kept. Returns
// true when a change needs a restart.
static bool ApplyLowEndPreset() {
  const std::string threads = std::to_string(LowEndShaderThreads());
  struct Change {
    const char* cvar;
    std::string value;
  };
  const Change changes[] = {
      // The GPU emulation's work grows with the pixel count (1080p is 2.25x).
      {"cod3_render_resolution", "720p"},
      // Extra full-screen passes after the game's own picture.
      {"swap_post_effect", "none"},
      {"present_effect", "bilinear"},
      // The game's own texture filtering instead of forced 4x anisotropic.
      {"anisotropic_override", "-1"},
      // The HD pack needs about 4x the texture memory (system memory on
      // integrated graphics).
      {"cod3_hd_textures", "false"},
      // Off draws the game's 2x MSAA with 4x samples: twice the GPU work.
      {"native_2x_msaa", "true"},
      // Don't upload geometry the game hasn't changed again every frame
      // (10-17 MB a frame in big scenes). Applies at once.
      {"clear_memory_page_state", "false"},
      {"vulkan_pipeline_creation_threads", threads},
      {"d3d12_pipeline_creation_threads", threads},
      {"async_shader_compilation", "true"},
      {"store_shaders", "true"},
      // [claude-9b] 2026-10-27 (owner 18:13: "refine the potato pc preset with the new optimizations"). On a
      // small PC the GPU emulation thread is the limit (REA suite potato: 3-7 ms a frame against the main
      // thread's 1.5-2.6; two cores cost 20-28%), so fewer draws and less spinning count most.
      // Model shadows off: fewer draws (Saint-Lo +8-10% fps, the card 8-17% less busy, c9b-potato8); the
      // sunlight on the ground stays. Covers split screen too.
      {"cod3_shadow_quality", "off"},
      // Split screen (cod3mp only, UNTESTED until s151): grass ends at 60% of its distance in each view.
      {"cod3mp_split_grass", "60"},
      // The game's full-screen blur passes (depth of field while aiming, motion trails).
      {"cod3_depth_of_field", "false"},
      {"cod3_motion_blur", "false"},
      // Waits sleep instead of spinning: on 2-4 cores a spinning thread takes the GPU emulation's core.
      {"cod3_job_wait_sleeps", "true"},
      {"cod3_gpu_wait_sleeps", "true"},
      {"global_lock_spin_us", "-1"},  // automatic: no spinning below 6 logical CPUs
      {"gpu_idle_spin_count", "-1"},  // same
      // The GPU emulation's speed-ups and the native code back on (they're on by default; F2 > EMULATION can
      // turn them off for a comparison).
      {"vulkan_reuse_draw_state", "true"},
      {"vulkan_skip_unchanged_fetch_constants", "true"},
      {"vulkan_sampler_cache", "true"},
      {"vulkan_reuse_texture_descriptors", "true"},
      {"vulkan_dynamic_constant_offsets", "true"},
      {"vulkan_resolve_into_textures", "true"},  // the card 7-32% less busy (SDK 0062)
      {"vulkan_texture_memo", "true"},           // when this build has SDK 0135
      {"readback_resolve_cpu_reads_only", "true"},
      {"readback_resolve_cpu_pages_only", "true"},
      {"readback_resolve_full_deferred", "true"},
      {"texture_replacement_async", "true"},
      {"cod3_native_crt", "true"},
      {"cod3_native_math", "true"},
      {"cod3_native_printf", "true"},
      {"cod3_native_strtol", "true"},
      {"cod3_native_lzo", "true"},
      {"cod3_native_fmod", "true"},
      {"cod3_native_fmod_mix", "true"},
      {"cod3_native_resampler", "true"},
      {"xma_restart_without_reopen", "true"},
      // Checks and counters that only tests need.
      {"vulkan_reuse_draw_state_verify", "false"},
      {"vulkan_texture_memo_verify", "false"},
      {"vulkan_log_state_reuse", "false"},
      {"cod3_native_crt_counts", "false"},
      {"cod3_heap_check", "false"},
      {"cod3_perf_overlay", "false"},
      {"cod3_perf_log", "0"},
      {"cod3_log_detail", "normal"},
  };
  bool restart = false;
  for (const Change& c : changes) {
    if (!Has(c.cvar)) continue;
    const std::string before = Get(c.cvar);
    const std::string_view name = c.cvar;
    if (before == c.value) continue;
    // Keep choices that are already cheaper.
    if (name == "cod3_render_resolution") {
      uint32_t w = 0, h = 0;
      if (before == "original" || (cod3::ParseResolutionChoice(before, &w, &h) && h <= 720))
        continue;
    }
    if (name == "anisotropic_override" && before == "0") continue;
    if (name == "cod3mp_split_grass" && std::atoi(before.c_str()) > 0 &&
        std::atoi(before.c_str()) < std::atoi(c.value.c_str()))
      continue;  // [claude-9b] already shorter
    if (name.ends_with("_pipeline_creation_threads") && !before.empty() && before != "-1" &&
        std::atoi(before.c_str()) < std::atoi(c.value.c_str()))
      continue;
    rex::cvar::SetFlagByName(c.cvar, c.value);
    const auto* info = rex::cvar::GetFlagInfo(c.cvar);
    if (info && info->lifecycle != rex::cvar::Lifecycle::kHotReload) restart = true;
  }
  REXLOG_INFO("cod3: low-end PC settings applied from the settings window");
  return restart;
}

void SettingsWindow::DrawPerformance() {
  if (ResetButton("reset_perf", "performance settings")) {
    // [claude-9b] 2026-10-27 the shader, render target and memory settings moved to EMULATION (its own reset).
    restart_needed_ |= ResetSettings({"cod3_perf_overlay", "cod3_perf_log"});
    dirty_ = true;
  }
  Section("Low-end PC");
  if (ImGui::Button("Use low-end PC settings...")) ImGui::OpenPopup("Low-end PC settings##lowend");
  if (ImGui::BeginPopupModal("Low-end PC settings##lowend", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("For a PC with 2-4 CPU cores and integrated graphics:");
    ImGui::BulletText("Render resolution 720p (Xbox 360 original stays if chosen); with Aspect ratio \"Match "
                      "the render resolution\" the picture becomes 16:9");  // [acct2-decomp] ultrawide.cpp
    ImGui::BulletText("No FXAA, plain bilinear scaling, the game's own texture filtering");
    ImGui::BulletText("HD textures off; the game's 2x MSAA drawn as real 2x");
    ImGui::BulletText("Geometry the game hasn't changed isn't sent to the GPU again every frame");
    ImGui::BulletText("%d shader build thread(s), shaders kept on disk", LowEndShaderThreads());
    // [claude-9b] 2026-10-27 the new parts (owner 18:13).
    ImGui::BulletText("Model shadows off (the sunlight stays); depth of field and motion blur off");
    if (cod3::kMultiplayer) ImGui::BulletText("Split screen: grass drawn to 60%% of its distance");
    ImGui::BulletText("Waiting threads sleep instead of spinning, so the GPU emulation keeps its core");
    ImGui::BulletText("Every EMULATION speed-up and native code switch on; test checks off");
    ImGui::BulletText("Frame cost overlay off, normal log detail");
    ImGui::TextDisabled("The game's speed, the frame rate limit, V-Sync and the lens-flare fix "
                        "don't change.");
    if (ImGui::Button("Apply")) {
      restart_needed_ |= ApplyLowEndPreset();
      dirty_ = true;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  Help("Changes the options in Video, EMULATION and on this page that cost the most on a slow PC, for the "
       "least difference on screen. Each can still be changed back by hand; most apply after a "
       "restart. If something in the world goes missing or stops updating afterwards, turn on "
       "EMULATION > GPU > \"Re-send all memory the game wrote, every frame\".");
  Section("Logging");
  {
    const char* v[] = {"normal", "verbose", "debug"};
    const char* n[] = {"Normal", "Verbose (slow frames, load times, costs)", "Debug (everything)"};
    dirty_ |= CvarCombo("Log detail", "cod3_log_detail", v, n, 3, &restart_needed_);
    Help("How much goes into userdata/logs/cod3.log. Verbose adds every slow frame with what the "
         "emulated GPU did in it, level load times, the frame cost every 10 seconds and every "
         "settings change. Debug also adds the SDK's own debug messages (a much bigger log). "
         "Applies at once.");
    if (ImGui::Button("Write a report")) {
      const auto path = cod3::diag::WriteReport();
      status_ = path.empty() ? "Couldn't write the report."
                             : "Report written: userdata/logs/" + cod3::PathToUtf8(path.filename());
    }
    Help("Writes userdata/logs/report-<date>-<time>.txt: the build, the settings that matter, every "
         "setting you changed, level load times, the last minute of frame costs and the slowest "
         "frames. Attach it (and cod3.log) to a bug report. Same as cod3_report in the console.");
    if (ImGui::Button("Take a screenshot")) {
      const std::string file =
          cod3::shot::Request(cod3::shot::Kind::kAsked, "asked for in F2");
      status_ = !file.empty() ? "Screenshot: userdata/logs/screenshots/" + file
                : cod3::shot::Available() ? std::string("One was just taken; try again in a moment.")
                                          : std::string("Screenshots need the GPU emulation.");
    }
    Help("Saves the game's own picture (not this window, the console, the desktop or anything "
         "else on screen) to userdata/logs/screenshots/. Same as screenshot in the console.");
    {
      const auto hd = cod3::GetHdTextures();
      const bool can_compare = (hd == cod3::HdTextures::kOn || hd == cod3::HdTextures::kCommandLine) &&
                               !cod3::kMultiplayer && !cod3::hd::IsMultiplayerMode();
      ImGui::BeginDisabled(!can_compare);
      if (ImGui::Button("Compare HD textures")) {
        const std::string stem = cod3::shot::RequestHdCompare(2.0);
        status_ = stem.empty() ? std::string("A comparison is already under way.")
                               : "Screenshots: userdata/logs/screenshots/" + stem + "-on.png and -off.png";
      }
      ImGui::EndDisabled();
    }
    Help("Two screenshots of the same view: with the HD textures, then without them (they go off "
         "for two seconds and come back by themselves). Stand still. Needs HD textures on at "
         "start; campaign only. Same as cod3_hd_compare in the console.");
    {
      const char* v[] = {"0", "30", "60", "120", "300"};
      const char* n[] = {"Off", "Every 30 seconds", "Every minute", "Every 2 minutes",
                         "Every 5 minutes"};
      ImGui::BeginDisabled(Get("cod3_log_detail") != "debug");  // [acct2-decomp] its help: Debug only
      dirty_ |= CvarCombo("Debug screenshots", "cod3_debug_screenshots", v, n, 5, &restart_needed_);
      ImGui::EndDisabled();
    }
    Help("With Log detail on Debug only: the game's picture every so often while it changes, "
         "when the game gets stuck, and with each report, into userdata/logs/screenshots/ "
         "(the newest 100 automatic ones are kept). Only the game's own picture is saved.");
  }
  Section("Frame cost");
  dirty_ |= CvarCheckbox("Show what each frame costs", "cod3_perf_overlay", &restart_needed_);
  Help("Under the frame rate: the game thread's time per frame, the emulated GPU's draws, "
       "resolves and render target transfers, how often it waited for the graphics card, and "
       "memory sent to the card. Same as cod3_perf_overlay in the console. cod3_perf_log N also "
       "writes it to the log every N seconds.");
  // [hw-scaling]
  Help("It also shows the GPU emulation thread's time, the three busiest threads and what limits "
       "the frame rate: the frame limit, your graphics card, the GPU emulation thread, the "
       "game's main thread, or the game's other threads. \"Write a report\" lists every "
       "thread and what this PC has.");
  // [hw-scaling] the same, in full: what limits the frame rate, every busy
  // thread with its waits, and what this PC has (game/thread_profiler.h,
  // game/machine_profile.h).
  if (ImGui::TreeNode("Threads and this PC")) {
    const cod3::ThreadReport threads = cod3::ThreadProfiler::Get().Report();
    const std::string verdict = cod3::ThreadProfiler::Verdict(threads);
    ImGui::PushTextWrapPos(0.0f);
    ImGui::TextUnformatted(verdict.empty() ? "Measuring (needs a second of frames)..."
                                           : verdict.c_str());
    ImGui::PopTextWrapPos();
    if (threads.valid &&
        ImGui::BeginTable("cod3_threads", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit)) {
      ImGui::TableSetupColumn("CPU");
      ImGui::TableSetupColumn("Thread");
      ImGui::TableSetupColumn("What it is");
      ImGui::TableSetupColumn("Waits", ImGuiTableColumnFlags_WidthStretch);
      ImGui::TableHeadersRow();
      for (size_t i = 0; i < threads.rows.size() && i < 16; ++i) {
        const cod3::ThreadRow& t = threads.rows[i];
        if (t.cpu_pct < 0.5f && t.guest_wait_pct < 0.5f && t.lock_pct < 0.5f) break;
        ImGui::TableNextRow();
        ImGui::TableNextColumn();
        ImGui::Text("%.0f%%", t.cpu_pct);
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(t.name.c_str());
        ImGui::TableNextColumn();
        ImGui::TextUnformatted(t.role.c_str());
        ImGui::TableNextColumn();
        std::string waits;
        auto add = [&](const char* what, float pct) {
          if (pct < 1.0f) return;
          char part[48];
          std::snprintf(part, sizeof(part), "%s%s %.0f%%", waits.empty() ? "" : ", ", what, pct);
          waits += part;
        };
        add("waits", t.guest_wait_pct);
        add("sleeps", t.sleep_pct);
        add("spins", t.spin_pct);
        add("global lock", t.lock_pct);
        ImGui::TextUnformatted(waits.c_str());
      }
      ImGui::EndTable();
    }
    Help("CPU is % of one core over the last second; waits are the share of that second a thread "
         "spent waiting on the game's own events, sleeping, spinning on a lock or waiting for the "
         "emulator's global lock.");
    const auto& machine = cod3::machine::Get();
    const std::string pc = machine.Describe() + "Thread plan: " +
                           cod3::machine::Budget(machine).Describe();
    ImGui::TextUnformatted(pc.c_str());
    ImGui::TreePop();
  }
  // [claude-9b] 2026-10-27 render target emulation and the shader settings moved to EMULATION.
}

// [claude-9b] 2026-10-27 EMULATION (owner 17:09: "make a emulation tab in the f2 menu for the emulation
// toggles"): every setting about how the Xbox 360 is emulated, or which of the game's own code runs as native
// code instead, in one place. Moved here: the native renderer toggle (was Video > Display), the resolve
// readback (was Video > Accuracy), real 2x MSAA (was Video > Quality), render target emulation and the shader
// settings (were PERFORMANCE). New rows for settings that had none (the SDK's speed-ups and the native code
// switches). A row only shows when this build has its setting (Has()), so builds without an SDK patch, the
// other graphics API and cod3mp all work.
static constexpr const char* kEmulationSettings[] = {
    "cod3_renderer", "render_target_path_vulkan", "render_target_path_d3d12", "readback_resolve",
    "readback_resolve_cpu_reads_only", "readback_resolve_cpu_pages_only", "readback_resolve_full_deferred",
    "native_2x_msaa", "clear_memory_page_state",
    // GPU speed-ups (SDK patches)
    "vulkan_reuse_draw_state", "vulkan_skip_unchanged_fetch_constants", "vulkan_sampler_cache",
    "vulkan_reuse_texture_descriptors", "vulkan_dynamic_constant_offsets", "vulkan_resolve_into_textures",
    "vulkan_vertex_refetch", "vulkan_texture_memo", "texture_replacement_async",
    "vulkan_pipeline_creation_threads", "d3d12_pipeline_creation_threads", "async_shader_compilation",
    "store_shaders",
    // Native code
    "cod3_native_crt", "cod3_native_math", "cod3_native_printf", "cod3_native_strtol", "cod3_native_lzo",
    "cod3_job_wait_sleeps", "cod3_gpu_wait_sleeps",
    // Sound
    "cod3_native_fmod", "cod3_native_fmod_mix", "cod3_native_resampler", "xma_restart_without_reopen",
    "audio_pace_callbacks",
};

void SettingsWindow::DrawEmulation() {
  if (ResetButton("reset_emulation", "emulation settings")) {
    for (const char* name : kEmulationSettings) restart_needed_ |= ResetSettings({name});
    dirty_ = true;
  }
  Help("How the Xbox 360 is emulated, and which parts of the game run as native PC code. The defaults are "
       "the tested, fastest choices. Change these to compare, or when something looks wrong; most apply "
       "after a restart.");
  if (!ImGui::BeginTabBar("##emulation_views")) return;
  struct Page {
    const char* label;
    void (SettingsWindow::*draw)();
  };
  static constexpr Page kPages[] = {
      {"GPU", &SettingsWindow::DrawEmulationGpu},
      {"GPU speed-ups", &SettingsWindow::DrawEmulationSpeedups},
      {"Native code", &SettingsWindow::DrawEmulationNative},
      {"Audio engine", &SettingsWindow::DrawEmulationSound},  // [acct2-decomp] not the SOUND tab
  };
  for (const Page& page : kPages) {
    if (!ImGui::BeginTabItem(page.label)) continue;
    (this->*page.draw)();
    ImGui::EndTabItem();
  }
  ImGui::EndTabBar();
}

// EMULATION > GPU: how the Xbox 360's graphics chip and its memory are emulated.
void SettingsWindow::DrawEmulationGpu() {
  // [acct2-decomp] Who draws, as a toggle (owner 2026-10-24 23:45: "Please make
  // sure the option for emulation/native is a toggle"). Off: cod3_renderer
  // "xenos", the emulated Xbox 360 GPU (the default). On: "rust", the native
  // Rust renderer with the GPU emulation off (render/rust_renderer.h, acct2-main's
  // setting; nothing about it changes here). Both programs; after a restart.
  if (Has("cod3_renderer")) {
    bool native = Get("cod3_renderer") == "rust";
    // [claude-9b] Greyed out for now (owner 2026-10-26: "The F2 \"Native Renderer\" option/toggle
    // needs to be grayed out for right now as it's not ready yet"): only turning it off is offered.
    const bool can_turn_on = false;
    ImGui::BeginDisabled(!can_turn_on && !native);
    if (ImGui::Checkbox("Native renderer (no GPU emulation)", &native)) {
      rex::cvar::SetFlagByName("cod3_renderer", native ? "rust" : "xenos");
      restart_needed_ = true;
      dirty_ = true;
    }
    ImGui::EndDisabled();
    Help("Not ready yet: the native Rust renderer is still being written, so the emulated Xbox 360 "
         "GPU draws the game. If it was turned on earlier, untick it here (applies after a restart).");
  }
  {
    // How the Xbox 360's EDRAM (the GPU's own render memory) is emulated.
    const char* v[] = {"", "fbo", "fsi"};
    const char* n[] = {"Automatic (host render targets)", "Host render targets",
                       "Pixel shader interlock (experimental)"};
    if (Has("render_target_path_vulkan"))
      dirty_ |= CvarCombo("Render target emulation (Vulkan)", "render_target_path_vulkan", v, n, 3,
                          &restart_needed_);
    const char* dv[] = {"", "rtv", "rov"};
    const char* dn[] = {"Automatic", "Host render targets", "Rasterizer-ordered views"};
    if (Has("render_target_path_d3d12"))
      dirty_ |= CvarCombo("Render target emulation (Direct3D 12)", "render_target_path_d3d12", dv,
                          dn, 3, &restart_needed_);
    Help("Host render targets draw with the graphics card's own blending, but have to copy pictures "
         "between render targets whenever the game reuses its video memory (\"transfers\" in the "
         "frame cost readout). Pixel shader interlock emulates that memory exactly and never copies, "
         "but every pixel does more work. Busy scenes with many transfers may run faster with it. "
         "Applies after a restart.");
  }
  dirty_ |= CvarCheckbox("Draw the game's 2x MSAA as real 2x (faster)", "native_2x_msaa",
                         &restart_needed_);
  Help("How the game's own 2x MSAA is drawn (it's on whenever it fits the emulated video memory: "
       "always with Vulkan). On: your GPU's 2x MSAA. Off: 4x MSAA keeping two of the samples, which "
       "places them exactly like the Xbox 360 but is about twice the work for the GPU. Leave it "
       "on. Applies after a restart.");

  Section("Resolve readback");
  {
    const char* v[] = {"none", "fast", "some", "full"};
    const char* n[] = {"Off", "Fast (recommended)", "On cache miss", "Full (accurate, slow)"};
    dirty_ |= CvarCombo("Resolve readback (lens-flare fix)", "readback_resolve", v, n, 4,
                        &restart_needed_);
    Help("Copies some of the emulated GPU's results back so the game can read them. Without it, "
         "lens flares and the sun shine through walls. Fast is the best balance; Full is exact but "
         "slow. If smoke and explosions stutter, try On cache miss or Off and compare.");
  }
  dirty_ |= CvarCheckbox("Only the results the game reads", "readback_resolve_cpu_reads_only", &restart_needed_);
  Help("Copies back only the results the game's own code reads, not every one. Off copies them all, as the "
       "emulator first did (slower, the same picture).");
  dirty_ |= CvarCheckbox("Only the parts the game reads", "readback_resolve_cpu_pages_only", &restart_needed_);
  Help("Of those, copies only the pieces of memory the game reads. Off copies each result whole.");
  dirty_ |= CvarCheckbox("Full readback without waiting", "readback_resolve_full_deferred", &restart_needed_);
  Help("With Full, the GPU emulation doesn't stop at each copy to wait for the graphics card. Off waits "
       "there, as before (slower).");

  Section("Memory");
  dirty_ |= CvarCheckbox("Re-send all memory the game wrote, every frame", "clear_memory_page_state",
                         &restart_needed_);
  Help("Off (default): only memory the game changed since the last frame is sent to the emulated GPU "
       "again. On: everything the game's code ever wrote is sent again every frame. Slower; turn it on "
       "only if something in the world goes missing or stops updating.");
}

// EMULATION > GPU speed-ups: shortcuts our SDK patches added to the GPU emulation. Each one is on by default
// and was checked to draw the same picture; turning one off is for comparing, or for a picture problem.
void SettingsWindow::DrawEmulationSpeedups() {
  if (!Has("vulkan_reuse_draw_state") && !Has("vulkan_sampler_cache"))
    ImGui::TextDisabled("Most of these are Vulkan only, and this build doesn't have them.");
  else if (Has("cod3_graphics_api") && Get("cod3_graphics_api") == "d3d12")
    ImGui::TextDisabled("The Vulkan rows below do nothing with Direct3D 12 (Video > Display > Graphics API).");
  dirty_ |= CvarCheckbox("Reuse the last draw's state when nothing changed", "vulkan_reuse_draw_state",
                         &restart_needed_);
  Help("A draw that changes no setting reuses everything the previous draw set up, instead of setting it "
       "all up again. The biggest of these: about 9-16% more frames in 4-player split screen.");
  dirty_ |= CvarCheckbox("Skip slots the game rewrote unchanged", "vulkan_skip_unchanged_fetch_constants",
                         &restart_needed_);
  Help("When the game writes a texture or geometry slot with the value it already had, nothing is looked "
       "up again.");
  dirty_ |= CvarCheckbox("Cache texture samplers", "vulkan_sampler_cache", &restart_needed_);
  Help("Keeps each texture filter setting once it's worked out, instead of working it out for every draw.");
  dirty_ |= CvarCheckbox("Reuse texture sets", "vulkan_reuse_texture_descriptors", &restart_needed_);
  Help("A draw that uses the same textures as an earlier one in the frame reuses that one's set.");
  dirty_ |= CvarCheckbox("Draw constants without copying", "vulkan_dynamic_constant_offsets", &restart_needed_);
  Help("Points the graphics card at each draw's constants with an offset instead of setting up a new "
       "binding for every draw.");
  dirty_ |= CvarCheckbox("Copy resolved pictures straight into textures", "vulkan_resolve_into_textures",
                         &restart_needed_);
  Help("When the game turns a finished picture into a texture, it's copied there directly instead of "
       "through the emulated memory.");
  dirty_ |= CvarCheckbox("Re-check rewritten geometry", "vulkan_vertex_refetch", &restart_needed_);
  Help("Sends geometry to the graphics card again when the game rewrote it in place, even at the same "
       "address. Fixes a rare one-frame wrong model.");
  dirty_ |= CvarCheckbox("Remember each draw's textures", "vulkan_texture_memo", &restart_needed_);
  Help("When a draw's textures haven't changed since the last draw, the same textures and filters are used "
       "without looking them up again.");
  dirty_ |= CvarCheckbox("Load HD textures in the background", "texture_replacement_async", &restart_needed_);
  Help("Reads the HD texture files on their own thread, so the GPU emulation doesn't stop for them. Off "
       "reads them on the GPU thread (stutters when new textures appear).");

  Section("Shaders");
  unsigned hw = std::max(1u, std::thread::hardware_concurrency());
  ImGui::Text("Logical CPUs: %u", hw);
  for (const char* cvar : {"vulkan_pipeline_creation_threads", "d3d12_pipeline_creation_threads"}) {
    if (!Has(cvar)) continue;
    int v = GetInt(cvar, -1);
    // [acct2-decomp] Both rows had the label "Shader build threads", so ImGui gave them one ID: dragging one
    // moved both (Windows, where both exist). Each its own label and ID now.
    const bool vk = cvar[0] == 'v';
    ImGui::PushID(cvar);
    const bool changed = ImGui::SliderInt(vk ? "Shader build threads (Vulkan)" : "Shader build threads (Direct3D 12)",
                                          &v, -1, static_cast<int>(hw), v < 0 ? "auto (CPUs - 1)" : "%d");
    ImGui::PopID();
    if (changed) {
      rex::cvar::SetFlagByName(cvar, std::to_string(v));
      restart_needed_ = true;
      dirty_ = true;
    }
    Help("CPU threads that turn the Xbox 360's shaders into your graphics card's the first time a new "
         "effect appears. Auto uses all but one. Applies after a restart.");
  }
  dirty_ |= CvarCheckbox("Asynchronous shader compilation", "async_shader_compilation",
                         &restart_needed_);
  Help("While a new effect's shader is still being built, keep playing and draw it a moment later, "
       "instead of pausing the game until it's ready. Fewer stutters the first time you see an "
       "effect.");
  dirty_ |= CvarCheckbox("Store shader cache on disk", "store_shaders", &restart_needed_);
  Help("Saves built shaders, so the next session doesn't stutter on effects you've already seen.");
}

// EMULATION > Native code: parts of the game's own code that run as hand-written PC code instead of the
// translated Xbox 360 code. Each one is checked against the game's code; turning one off runs the game's own
// code there again.
void SettingsWindow::DrawEmulationNative() {
  Help("Parts of the game's own code that run as hand-written PC code instead of the translated Xbox 360 "
       "code. Each gives exactly what the game's code gives (checked against it). Untick one to run the game's "
       "own code there again, for comparing.");
  dirty_ |= CvarCheckbox("Memory and text functions", "cod3_native_crt", &restart_needed_);
  Help("Copying, filling and comparing memory and text (memcpy, memset, strcmp and the like).");
  dirty_ |= CvarCheckbox("Math functions", "cod3_native_math", &restart_needed_);
  Help("Square roots, sines, powers and the other math library functions.");
  dirty_ |= CvarCheckbox("Text formatting", "cod3_native_printf", &restart_needed_);
  Help("Building text from numbers and words (sprintf and the like).");
  dirty_ |= CvarCheckbox("Reading numbers from text", "cod3_native_strtol", &restart_needed_);
  Help("Turning text into numbers (strtol, atoi and the like).");
  dirty_ |= CvarCheckbox("Level file unpacking", "cod3_native_lzo", &restart_needed_);
  Help("Unpacking the level files while a level loads (LZO).");

  Section("Waiting");
  dirty_ |= CvarCheckbox("Sleep while waiting for the game's own jobs", "cod3_job_wait_sleeps",
                         &restart_needed_);
  Help("While the game waits for its own jobs (the render job, every frame), its thread sleeps until the "
       "job queue changes instead of checking it over and over, which leaves CPU time for the rest.");
  dirty_ |= CvarCheckbox("Sleep while waiting for the GPU emulation", "cod3_gpu_wait_sleeps", &restart_needed_);
  Help("While the game waits for the graphics emulation, its thread sleeps instead of keeping a CPU core "
       "busy.");
}

// EMULATION > Sound: the sound engine's busiest parts as native code, and how the Xbox 360's sound chip is
// emulated.
void SettingsWindow::DrawEmulationSound() {
  dirty_ |= CvarCheckbox("Native reverb (FMOD)", "cod3_native_fmod", &restart_needed_);
  Help("The sound engine's reverb, which was half of its work, as native code.");
  dirty_ |= CvarCheckbox("Native mixing (FMOD)", "cod3_native_fmod_mix", &restart_needed_);
  Help("Mixing each sound into the speakers, as native code.");
  dirty_ |= CvarCheckbox("Native resampling (FMOD)", "cod3_native_resampler", &restart_needed_);
  Help("Changing each sound's pitch and rate, as native code.");
  dirty_ |= CvarCheckbox("Reuse the sound decoder on a restart", "xma_restart_without_reopen", &restart_needed_);
  Help("When a sound starts again, the emulated sound chip's decoder is reset instead of built again. Much "
       "less work for the sound thread.");
  dirty_ |= CvarCheckbox("Steady sound timing", "audio_pace_callbacks", &restart_needed_);
  Help("Hands sound to your speakers in evenly spaced pieces instead of bursts. Off can make sounds "
       "repeat.");
}

// What the console has saved (seta, and changes to com_maxfps, cg_drawFPS,
// cg_fov, cg_fovScale): userdata/cod3_config.cfg.
void SettingsWindow::DrawInterface() {
  if (ResetButton("reset_interface", "interface settings")) {
    ResetSettings({"cod3_ui_theme", "cod3_ui_text_size", "cod3_ui_opacity", "cod3_show_version"});
    dirty_ = true;
  }
  Section("Look");
  size_t count = 0;
  const ThemeInfo* themes = Themes(&count);
  const std::string current = Get("cod3_ui_theme");
  const ThemeInfo* chosen = &themes[0];
  for (size_t i = 0; i < count; ++i)
    if (current == themes[i].id) chosen = &themes[i];
  if (ImGui::BeginCombo("Theme", chosen->label)) {  // [acct2-decomp] not Video > Colors
    for (size_t i = 0; i < count; ++i) {
      const bool selected = &themes[i] == chosen;
      if (ImGui::Selectable(themes[i].label, selected)) {
        rex::cvar::SetFlagByName("cod3_ui_theme", themes[i].id);
        dirty_ = true;
      }
      if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", themes[i].description);
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  Help(chosen->description);
  {
    static const char* const values[] = {"14", "16", "18", "20", "24"};
    static const char* const names[] = {"Small", "Normal", "Large", "Larger", "Largest"};
    dirty_ |= CvarCombo("Text size", "cod3_ui_text_size", values, names, 5, &restart_needed_);
    Help("The size of the text in this window and its lists. The game's own menus are not changed.");
  }
  if (Has("cod3_ui_opacity")) {
    float opacity = float(GetDouble("cod3_ui_opacity", 0.94)) * 100.0f;
    if (ImGui::SliderFloat("Window opacity", &opacity, 60.0f, 100.0f, "%.0f%%")) {
      rex::cvar::SetFlagByName("cod3_ui_opacity", std::to_string(opacity / 100.0f));
      dirty_ = true;
    }
    Help("Lower it to see the game behind this window while you change settings.");
  }
  // [claude-9b] 2026-10-27 owner 18:31 (the version in the top right corner, with a toggle).
  dirty_ |= CvarCheckbox("Show the version number (top right)", "cod3_show_version", &restart_needed_);
  ImGui::TextDisabled("This game: %s.", VersionText().c_str());
  Help("The setup tool version this game was built with, small and faint in the top right corner. The log and "
       "every report also start with it, so a problem can be matched to its version.");
}

// F2 > Console: the ~ console itself, the problems logged this session
// (ui/console_pages.h), and the settings the console saved.
void SettingsWindow::DrawConsole() {
  if (!ImGui::BeginTabBar("##console_views")) return;
  if (ImGui::BeginTabItem("Console")) {
    DrawConsolePage(console_view_);
    ImGui::EndTabItem();
  }
  if (ImGui::BeginTabItem(ProblemsTabLabel())) {
    DrawProblemsPage();
    ImGui::EndTabItem();
  }
  if (ImGui::BeginTabItem("Saved settings")) {
    DrawSavedConsoleSettings();
    ImGui::EndTabItem();
  }
  ImGui::EndTabBar();
}

void SettingsWindow::DrawSavedConsoleSettings() {
  auto& engine = cod3::Engine::Get();
  if (!engine.GameReady()) ImGui::BeginDisabled();
  if (ResetButton("reset_console", "console settings (dvars)")) engine.ResetArchivedCvars();
  if (!engine.GameReady()) ImGui::EndDisabled();
  Help("Settings changed in the ~ console are kept here and come back every time the game starts: anything "
       "set with seta, plus com_maxfps and cg_drawFPS however they were changed, and your own cg_fov and "
       "cg_fovScale (not a vehicle's or a scripted view's). "
       "Reset puts them back to the game's defaults (the frame rate limit to the one in Video) and empties "
       "the list. That includes your field of view, FOV scale (Game > View) and the FPS counter (Video > "
       "Frame rate); the other settings aren't touched.");  // [acct2-decomp] engine.cpp ResetArchivedCvars
  const auto saved = engine.ArchivedCvars();
  if (saved.empty()) {
    ImGui::TextDisabled("Nothing saved from the console.");
    return;
  }
  if (ImGui::BeginTable("##saved", 2, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingStretchProp)) {
    for (const auto& [name, value] : saved) {
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(name.c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(value.c_str());
    }
    ImGui::EndTable();
  }
}

void SettingsWindow::OnDraw(ImGuiIO& io) {
  if (!open_) return;
  ApplyThemeIfChanged();
  const float fs = ImGui::GetStyle().FontSizeBase;
  ImGui::SetNextWindowSize(ImVec2(std::min(fs * 48.0f, io.DisplaySize.x * 0.9f),
                                  std::min(fs * 40.0f, io.DisplaySize.y * 0.85f)),
                           ImGuiCond_FirstUseEver);
  ImGui::SetNextWindowPos(ImVec2(io.DisplaySize.x * 0.5f, io.DisplaySize.y * 0.5f),
                          ImGuiCond_FirstUseEver, ImVec2(0.5f, 0.5f));
  // Scrollbars appear on either axis when the window is too small for its
  // content. The title (bold, upper case like the game's menus) is drawn by
  // Begin, so the bold font is only pushed around it.
  ImGui::PushFont(BoldFont(), 0.0f);
  const bool shown = ImGui::Begin("CALL OF DUTY 3  \xC2\xB7  SETTINGS###cod3_settings", &open_,
                                  ImGuiWindowFlags_NoCollapse);
  ImGui::PopFont();
  if (shown) {
    // Tab labels in the bold font, upper case; their pages in the regular one.
    struct Tab {
      const char* label;
      void (SettingsWindow::*draw)();
    };
    static constexpr Tab kTabs[] = {
        {"VIDEO", &SettingsWindow::DrawVideo},         {"SOUND", &SettingsWindow::DrawSound},
        {"GAME", &SettingsWindow::DrawGame},
        {"CROSSHAIR", &SettingsWindow::DrawCrosshair}, {"CONTROLS", &SettingsWindow::DrawControls},
        {"INTERFACE", &SettingsWindow::DrawInterface}, {"PERFORMANCE", &SettingsWindow::DrawPerformance},
        {"EMULATION", &SettingsWindow::DrawEmulation},  // [claude-9b] 2026-10-27
        {"CONSOLE", &SettingsWindow::DrawConsole},
    };
    ImGui::PushFont(BoldFont(), 0.0f);
    if (ImGui::BeginTabBar("##tabs", ImGuiTabBarFlags_FittingPolicyScroll)) {
      for (const Tab& tab : kTabs) {
        if (!ImGui::BeginTabItem(tab.label)) continue;
        ImGui::PopFont();
        // The page scrolls up and down under the tabs; the Save row stays.
        // Text wraps to the page (no sideways scrolling, which made the
        // descriptions run under the scroll bar).
        ImGui::BeginChild("##page", ImVec2(0, -ImGui::GetFrameHeightWithSpacing() - 6.0f));
        (this->*tab.draw)();
        ImGui::EndChild();
        ImGui::PushFont(BoldFont(), 0.0f);
        ImGui::EndTabItem();
      }
      ImGui::EndTabBar();
    }
    ImGui::PopFont();
    ImGui::Separator();
    if (ImGui::Button(dirty_ ? "Save *" : "Save")) Save();
    ImGui::SameLine();
    if (restart_needed_) ImGui::TextColored(CurrentPalette().warn, "Restart the game to apply some changes.");
    if (!status_.empty()) ImGui::TextDisabled("%s", status_.c_str());
  }
  ImGui::End();
  if (!open_) {
    SaveIfDirty();
    if (on_close_) on_close_();
  }
}

}  // namespace cod3::ui

// ---------------------------------------------------------------------------
// midasm hooks in CG_DrawCrosshair (config/default.toml): reached when the
// game has decided to show a crosshair. With the custom crosshair on, tell the
// HUD and skip the game's own drawing (jump to the epilogue).
// ---------------------------------------------------------------------------
// Infantry weapons @ 0x82444BB8; f29 = aim-down-sights fraction, the color
// CG_CalcCrosshairColor worked out is on the stack.
bool Cod3CrosshairHook(PPCRegister& f29, PPCRegister& r1, uint8_t* base) {
  if (!REXCVAR_GET(cod3_crosshair)) return false;
  namespace ch = cod3::addr::crosshair;
  const uint32_t at = r1.u32 + ch::kColorOnStack;
  const float rgba[4] = {cod3::guest::ReadF32(base, at), cod3::guest::ReadF32(base, at + 4),
                         cod3::guest::ReadF32(base, at + 8), cod3::guest::ReadF32(base, at + 12)};
  cod3::ui::NotifyGameCrosshair(static_cast<float>(f29.f64), rgba);
  return true;
}

// Mounted guns and vehicles @ 0x824396D4: white at cg_crosshairAlpha.
bool Cod3MountedCrosshairHook(PPCRegister& r1, uint8_t* base) {
  if (!REXCVAR_GET(cod3_crosshair)) return false;
  namespace ch = cod3::addr::crosshair;
  const uint32_t at = r1.u32 + ch::kMountedColorOnStack;
  const float rgba[4] = {cod3::guest::ReadF32(base, at), cod3::guest::ReadF32(base, at + 4),
                         cod3::guest::ReadF32(base, at + 8),
                         cod3::guest::ReadF32(base, ch::kAlphaVmCvar + ch::kVmCvarValue)};
  cod3::ui::NotifyGameCrosshair(0.0f, rgba);
  return true;
}
