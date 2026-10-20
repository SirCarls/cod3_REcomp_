// cod3 - custom crosshair HUD and the F2 "Recomp Settings" window.

#include "util.h"
#include "ui/hud_overlay.h"

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
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

#include "app_defaults.h"
#include "game/aim_assist.h"
#include "game/cheat_watch.h"
#include "game/cutscene_skip.h"
#include "game/diagnostics.h"
#include "game/engine.h"
#include "game/fov.h"
#include "game/hd_textures.h"
#include "game/program.h"
#include "game/mouse_look.h"
#include "game/keybinds/binds.h"     // [keybinds] both programs
#if !defined(COD3_MP)
#include "game/movement/settings.h"  // [movement] campaign only for now
#endif
#include "game/render_resolution.h"
#include "game/screenshot.h"
#include "game/shadows.h"
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

std::atomic<int64_t> g_last_game_crosshair_ns{0};
std::atomic<float> g_aim_fraction{0.0f};  // 0 hip fire .. 1 aiming down sights
// The game's crosshair color this frame (written by the guest thread).
std::atomic<float> g_game_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};

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

}  // namespace

void NotifyGameCrosshair(float aim_fraction, const float rgba[4]) {
  for (int i = 0; i < 4; ++i) {
    float c = std::isfinite(rgba[i]) ? std::clamp(rgba[i], 0.0f, 1.0f) : 1.0f;
    g_game_color[i].store(c, std::memory_order_relaxed);
  }
  if (!std::isfinite(aim_fraction)) aim_fraction = 0.0f;
  g_aim_fraction.store(std::clamp(aim_fraction, 0.0f, 1.0f), std::memory_order_relaxed);
  g_last_game_crosshair_ns.store(NowNs(), std::memory_order_relaxed);
}

bool GameCrosshairVisible() {
  return NowNs() - g_last_game_crosshair_ns.load(std::memory_order_relaxed) <= 150'000'000;
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

// cg_drawFPS: top-right corner, like World at War.
//   1 frame rate, 2 + slowest / fastest over the last half second,
//   3 + average / longest frame time and the limit.
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
  const ImVec2 pos(std::floor(io.DisplaySize.x - ext.x - margin), margin);
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
  float y = margin + std::max(13.0f, 20.0f * io.DisplaySize.y / 1080.0f) + 6.0f;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  for (int i = 0; i < n; ++i) {
    const ImVec2 ext = font->CalcTextSizeA(size, FLT_MAX, 0.0f, lines[i]);
    const ImVec2 pos(std::floor(io.DisplaySize.x - ext.x - margin), std::floor(y));
    dl->AddText(font, size, ImVec2(pos.x + 1, pos.y + 1), IM_COL32(0, 0, 0, 200), lines[i]);
    dl->AddText(font, size, pos, IM_COL32(230, 230, 230, 255), lines[i]);
    y += ext.y + 2.0f;
  }
}

// "SPACE  Skip" while a movie can be skipped (game/cutscene_skip.h), in the
// game's own colors at the bottom of the screen, where the loading screen
// shows its "Loading ..." line.
static void DrawSkipPrompt(ImGuiIO& io, float alpha) {
  const float scale = std::max(0.6f, io.DisplaySize.y / 1080.0f);
  const float size = 24.0f * scale;
  ImFont* font = FontFor(size);
  const char* key = "SPACE";
  const char* label = "Skip";
  const ImVec2 key_ext = font->CalcTextSizeA(size * 0.8f, FLT_MAX, 0.0f, key);
  const ImVec2 label_ext = font->CalcTextSizeA(size, FLT_MAX, 0.0f, label);
  const float pad_x = 10.0f * scale, pad_y = 4.0f * scale, gap = 12.0f * scale;
  const float key_w = key_ext.x + 2 * pad_x, key_h = std::max(key_ext.y, label_ext.y) + 2 * pad_y;
  const float total = key_w + gap + label_ext.x;
  const float x = std::floor((io.DisplaySize.x - total) * 0.5f);
  const float y = std::floor(io.DisplaySize.y * 0.86f);
  const int a = static_cast<int>(alpha * 255);
  const ImU32 gold = IM_COL32(214, 172, 104, a);       // the game's menu and loading text
  const ImU32 shadow = IM_COL32(0, 0, 0, a * 3 / 4);
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  // Key cap.
  dl->AddRectFilled(ImVec2(x + 2, y + 2), ImVec2(x + key_w + 2, y + key_h + 2), shadow, 4.0f * scale);
  dl->AddRectFilled(ImVec2(x, y), ImVec2(x + key_w, y + key_h), IM_COL32(20, 16, 10, a * 4 / 5),
                    4.0f * scale);
  dl->AddRect(ImVec2(x, y), ImVec2(x + key_w, y + key_h), gold, 4.0f * scale, 0, 1.5f * scale);
  dl->AddText(font, size * 0.8f, ImVec2(x + pad_x, y + (key_h - key_ext.y) * 0.5f), gold, key);
  // Label.
  const ImVec2 lp(x + key_w + gap, y + (key_h - label_ext.y) * 0.5f);
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
  if (const float cover = cod3::CutsceneSkip::Get().CoverAlpha(); cover > 0.0f) {
    // Skipping a level intro: black while game time runs fast behind it.
    ImGui::GetBackgroundDrawList()->AddRectFilled(
        ImVec2(0, 0), io.DisplaySize, IM_COL32(0, 0, 0, static_cast<int>(cover * 255)));
  }
  if (const float a = cod3::CutsceneSkip::Get().PromptAlpha(); a > 0.0f) DrawSkipPrompt(io, a);
  if (const int mode = cod3::Engine::Get().draw_fps(); mode > 0) DrawFpsCounter(io, mode);
  if (REXCVAR_GET(cod3_perf_overlay)) DrawPerfOverlay(io);
  auto s = CurrentCrosshair();
  if (!s.enabled) return;
  // Only while the game itself would draw its crosshair (not in menus or
  // cutscenes): the CG_DrawCrosshair hooks stamp that every frame. Raising the
  // sights fades it out over the first half of the motion.
  if (!GameCrosshairVisible()) return;
  const float fade = std::clamp(1.0f - 2.0f * g_aim_fraction.load(std::memory_order_relaxed), 0.0f, 1.0f);
  if (fade <= 0.0f) return;
  // The game's own color unless the player chose a constant one.
  if (!s.constant_color) {
    for (int i = 0; i < 4; ++i) s.color[i] = g_game_color[i].load(std::memory_order_relaxed);
  }
  const float alpha = s.color[3] * fade;
  if (alpha <= 0.0f) return;
  ImDrawList* dl = ImGui::GetForegroundDrawList();
  ImVec2 c(std::floor(io.DisplaySize.x * 0.5f), std::floor(io.DisplaySize.y * 0.5f));
  float scale = std::max(0.5f, io.DisplaySize.y / 1080.0f);
  if (s.outline) {
    DrawCrosshairShape(dl, c, s, scale, IM_COL32(0, 0, 0, static_cast<int>(alpha * 255)),
                       s.outline_thickness * scale);
  }
  DrawCrosshairShape(dl, c, s, scale,
                     ImGui::ColorConvertFloat4ToU32(ImVec4(s.color[0], s.color[1], s.color[2], alpha)),
                     0.0f);
}

// ===========================================================================
// Settings window
// ===========================================================================
SettingsWindow::SettingsWindow(rex::ui::ImGuiDrawer* drawer, std::filesystem::path config_path,
                               std::function<void()> on_close)
    : ImGuiDialog(drawer), config_path_(std::move(config_path)), on_close_(std::move(on_close)) {}

void SettingsWindow::Save() {
  try {
    rex::cvar::SaveConfig(config_path_);
    status_ = "Saved to " + cod3::PathToUtf8(config_path_);
    dirty_ = false;
  } catch (const std::exception& e) {
    status_ = std::string("Save failed: ") + e.what();
  }
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

void SettingsWindow::DrawVideo() {
  if (ResetButton("reset_video", "video settings")) {
    auto& engine = cod3::Engine::Get();
    restart_needed_ |= ResetSettings({"fullscreen", "cod3_render_resolution", "cod3_render_method",
                                      "cod3_render_percent", "cod3_aspect_ratio",
                                      "vulkan_allow_present_mode_immediate",
                                      "d3d12_allow_variable_refresh_rate_and_tearing", "present_letterbox",
                                      "swap_post_effect", "native_2x_msaa", "present_effect",
                                      "anisotropic_override", "cod3_hd_textures", "readback_resolve",
                                      "cod3_max_fps", "cod3_shadow_quality", "cod3_color_space",
                                      "display_hdr", "display_hdr_paper_white", "display_hdr_peak",
                                      "cod3_refresh_rate", "cod3_live_resolution", "cod3_graphics_api"});
    if (engine.GameReady()) {
      engine.SetMaxFps(REXCVAR_GET(cod3_max_fps));
      engine.SetDrawFps(0);
    }
    cod3::RequestRenderResolution();  // a reset doesn't announce itself
    dirty_ = true;
  }
  Section("Screen");
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
      ImGui::SliderInt("Resolution scale", &percent, int(cod3::kMinRenderPercent), 100, "%d%%");
      if (ImGui::IsItemDeactivatedAfterEdit()) {
        rex::cvar::SetFlagByName("cod3_render_percent", std::to_string(percent));
        cod3::RequestRenderResolution();
        dirty_ = true;
      }
    }
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
  dirty_ |= CvarCheckbox("Letterbox (keep the picture's shape)", "present_letterbox", &restart_needed_);
  Help("Keeps the picture's shape with black bars when the window has another shape (for example "
       "16:9 chosen on an ultrawide monitor, or 4:3 on a 16:9 one), instead of stretching it to the "
       "window.");

  Section("Frame rate");
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

  Section("Colors");
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

  Section("Anti-aliasing & scaling");
  {
    const char* v[] = {"none", "fxaa", "fxaa_extreme"};
    const char* n[] = {"Off", "FXAA", "FXAA (extreme)"};
    dirty_ |= CvarCombo("Post-process AA", "swap_post_effect", v, n, 3, &restart_needed_);
    Help("Smooths jagged edges after the picture is drawn. Costs very little; Extreme smooths more "
         "and softens the picture a bit more. Applies after a restart.");
  }
  dirty_ |= CvarCheckbox("Draw the game's 2x MSAA as real 2x (faster)", "native_2x_msaa",
                         &restart_needed_);
  Help("How the game's own 2x MSAA is drawn (it's on whenever it fits the emulated video memory: "
       "always with Vulkan). On: your GPU's 2x MSAA. Off: 4x MSAA keeping two of the samples, which "
       "places them exactly like the Xbox 360 but is about twice the work for the GPU. Leave it "
       "on. Applies after a restart.");
  {
    const char* v[] = {"bilinear", "cas", "fsr", "fsr2", "fsr3"};
    const char* n[] = {"Bilinear", "AMD CAS (sharpen)", "AMD FSR 1 (spatial)",
                       "AMD FSR 2 (experimental)", "AMD FSR 3 (experimental)"};
    dirty_ |= CvarCombo("Output scaling", "present_effect", v, n, 5, &restart_needed_);
    Help("How the finished picture is stretched to the window. Bilinear is plain and soft; CAS "
         "sharpens it; FSR upscales while rebuilding edges (FSR 2 and 3 are experimental here).");
    if (auto* info = rex::cvar::GetFlagInfo("present_effect");
        info && info->constraints.allowed_values.size() <= 1)
      Help("AMD CAS / FSR need a build with FidelityFX enabled (setup tool option).");
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

  Section("Accuracy");
  {
    const char* v[] = {"none", "fast", "some", "full"};
    const char* n[] = {"Off", "Fast (recommended)", "On cache miss", "Full (accurate, slow)"};
    dirty_ |= CvarCombo("Resolve readback (lens-flare fix)", "readback_resolve", v, n, 4,
                        &restart_needed_);
    Help("Copies some of the emulated GPU's results back so the game can read them. Without it, "
         "lens flares and the sun shine through walls. Fast is the best balance; Full is exact but "
         "slow. If smoke and explosions stutter, try On cache miss or Off and compare.");
  }
}

void SettingsWindow::DrawGame() {
  auto& engine = cod3::Engine::Get();
  if (ResetButton("reset_game", "game settings")) {
    engine.SetFovSettings(cod3::fov::kDefaultFov, cod3::fov::kDefaultScale);
    // Aim assist follows the game's Sticky Aim again.
    ResetSettings({"cod3_aim_assist", "cod3_volume", "audio_device", "cod3_skip_movies", "cod3_skip_prompt"});
    dirty_ = true;
  }
  Section("View");
  {
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
    Help("The field of view times the FOV scale. It never goes below 65 or above 165, from here, the "
         "game's Options or the console. Vehicles, turrets and scripted moments may change the view "
         "while they last (aiming down the sights narrows it too); your values come back after them "
         "and at every level start, and only your own changes are saved.");
    if (fov::ScriptedViewActive())
      ImGui::TextDisabled("A scripted view has the FOV right now; yours comes back after it.");
  }
#if !defined(COD3_MP)  // [mp] multiplayer plays no films; aim assist is per profile there (later)
  Section("Cutscenes");
  dirty_ |= CvarCheckbox("Skip cutscenes automatically", "cod3_skip_movies", &restart_needed_);
  Help("Skips the logos at start, each level's briefing film and the level's opening intro as "
       "soon as the game allows it (the legal screen stays 5 seconds; a briefing only once the "
       "level has loaded). Without it, press Space, Enter or Escape, or click, to skip one.");
  dirty_ |= CvarCheckbox("Show the skip prompt", "cod3_skip_prompt", &restart_needed_);
  Help("Shows \"SPACE Skip\" at the bottom of the screen while a cutscene can be skipped.");
  Section("Aim");
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
  Help("Slows your aim down while it passes over an enemy, as on the console. Mostly helps with a "
       "controller. It's the same switch as Options > Gameplay > Sticky Aim in the game's menu "
       "(available once a profile is loaded).");
  // [movement] Sprint and the movement style (game/movement/settings.h): the
  // same rows as the game's Pause > Options > MOVEMENT OPTIONS.
  dirty_ |= cod3::movement::DrawF2Section();
#endif
  Section("Sound");
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
                   "cod3_crosshair_dot", "cod3_crosshair_constant_color", "cod3_crosshair_color"});
    changed = true;
  }
  bool en = REXCVAR_GET(cod3_crosshair);
  if (ImGui::Checkbox("Use custom crosshair", &en)) {
    REXCVAR_SET(cod3_crosshair, en);
    changed = true;
  }
  Help("Draws the crosshair below instead of the game's. It appears only when the game would show "
       "its own: not in menus or cutscenes, and it fades out as you aim down the sights. The game's "
       "Options > Gameplay > Crosshair switch still hides it.");
  const char* styles[] = {"Classic cross", "Cross + dot", "Dot", "Circle", "T"};
  int st = REXCVAR_GET(cod3_crosshair_style);
  if (ImGui::Combo("Style", &st, styles, 5)) {
    REXCVAR_SET(cod3_crosshair_style, st);
    changed = true;
  }
  auto slider = [&](const char* label, double& value, float lo, float hi) {
    float f = static_cast<float>(value);
    if (ImGui::SliderFloat(label, &f, lo, hi, "%.1f")) {
      value = f;
      changed = true;
    }
  };
  slider("Length", REXCVAR_GET(cod3_crosshair_size), 0.0f, 32.0f);
  slider("Thickness", REXCVAR_GET(cod3_crosshair_thickness), 0.5f, 8.0f);
  slider("Gap", REXCVAR_GET(cod3_crosshair_gap), -4.0f, 32.0f);
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

void SettingsWindow::DrawControls() {
  if (ResetButton("reset_mouse", "mouse settings")) {
    // cod3_raw_mouse first: mnk_mouse's default depends on it.
    restart_needed_ |= ResetSettings({"mnk_mode", "cod3_raw_mouse", "mnk_mouse", "cod3_sensitivity",
                                      "cod3_zoom_sensitivity", "cod3_invert_mouse", "cod3_m_yaw",
                                      "cod3_m_pitch", "mnk_sensitivity", "cod3_key_prompts",
                                      "cod3_glyph_style"});
    dirty_ = true;
  }
  dirty_ |= CvarCheckbox("Keyboard & mouse", "mnk_mode", &restart_needed_);
  Help("Play with keyboard and mouse. A controller keeps working either way.");
  // Button prompts (game/button_prompts.h).
  dirty_ |= CvarCheckbox("Show keys in button prompts", "cod3_key_prompts", &restart_needed_);
  Help("While you play with the keyboard and mouse, the game's prompts show the key to press "
       "([R], [ENTER]) instead of a controller button. Pressing a controller button brings the "
       "buttons back.");
  {
    const char* v[] = {"xbox", "playstation", "gamecube"};
    const char* n[] = {"Xbox 360 (the game's own)", "PlayStation", "GameCube"};
    dirty_ |= CvarCombo("Controller button style", "cod3_glyph_style", v, n, 3, &restart_needed_);
    Help("How the prompts show controller buttons. PlayStation and GameCube show the button "
         "names ([CROSS], [Z]) for now.");
  }

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
      Help("The same number as World at War's sensitivity setting (its default is 5).");
      float zoom = static_cast<float>(REXCVAR_GET(cod3_zoom_sensitivity));
      if (ImGui::SliderFloat("ADS sensitivity multiplier", &zoom, 0.1f, 2.0f, "%.2f")) {
        REXCVAR_SET(cod3_zoom_sensitivity, static_cast<double>(zoom));
        dirty_ = true;
      }
      Help("Extra factor while aiming down the sights. The aim already slows down with the zoom; "
           "below 1 slows it more, above 1 less.");
      bool inv = cod3::MouseLook::MouseInverted();
      if (ImGui::Checkbox("Invert vertical", &inv)) {
        cod3::MouseLook::SetMouseInverted(inv);
        dirty_ = true;
      }
      Help("Pushing the mouse forward looks down. The game's own Invert aim option (Options > "
           "Controls) inverts the controller and the mouse; this box shows the mouse as it is.");
      if (ImGui::TreeNode("Advanced (m_yaw / m_pitch)")) {
        float yaw = static_cast<float>(REXCVAR_GET(cod3_m_yaw));
        float pitch = static_cast<float>(REXCVAR_GET(cod3_m_pitch));
        if (ImGui::InputFloat("m_yaw", &yaw, 0.001f, 0.01f, "%.4f")) {
          REXCVAR_SET(cod3_m_yaw, static_cast<double>(yaw));
          dirty_ = true;
        }
        if (ImGui::InputFloat("m_pitch", &pitch, 0.001f, 0.01f, "%.4f")) {
          REXCVAR_SET(cod3_m_pitch, static_cast<double>(pitch));
          dirty_ = true;
        }
        Help("Degrees turned per mouse count at sensitivity 1, sideways and up/down. World at War "
             "uses 0.022 for both.");
        ImGui::TreePop();
      }
      float cm360 = 0.0f;
      if (REXCVAR_GET(cod3_m_yaw) != 0.0 && sens > 0.0f)
        cm360 = static_cast<float>(360.0 / (sens * std::abs(REXCVAR_GET(cod3_m_yaw))));
      ImGui::Text("%.0f mouse counts per full turn", cm360);
      Help("The mouse is held by the game only during gameplay; menus, the console and this window "
           "get a normal cursor. The game's Options > Controls rows apply on top: Horizontal / "
           "Vertical sensitivity (25 = 1x) and Invert aim.");
    } else if (mode == 1 && Has("mnk_sensitivity")) {
      float sens = static_cast<float>(GetDouble("mnk_sensitivity", 1.0));
      if (ImGui::SliderFloat("Stick sensitivity", &sens, 0.05f, 10.0f, "%.2f",
                             ImGuiSliderFlags_Logarithmic)) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "%.3f", sens);
        rex::cvar::SetFlagByName("mnk_sensitivity", buf);
        dirty_ = true;
      }
      Help("How far a mouse movement pushes the emulated right stick.");
    }
  }
  dirty_ |= DrawQteSettings();  // Battle actions (ui/qte_spinner.h)
  Section("Key bindings");
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
#if !defined(COD3_MP)
  Help("Each row is a controller button and the keys that press it: in the game's menus, and while you "
       "play for keys that no action above has (WASD moves through the left stick rows). Separate keys "
       "with commas; press Enter to apply. Key names: A-Z, 0-9, Space, Tab, Escape, Shift, Control, Alt, "
       "Up/Down/Left/Right; mouse buttons LMB, RMB, MMB, and the side buttons MB4 (back) and MB5 "
       "(forward), also written MOUSE4 / MOUSE5. The left stick click and the D-pad are empty by default.");
#else
  Help("Each row is a controller button and the keys that press it: in the game's menus, and while you "
       "play for keys that no action above has (WASD moves through the left stick rows). Separate keys "
       "with commas; press Enter to apply. Key names: A-Z, 0-9, Space, Tab, Escape, Shift, Control, Alt, "
       "Up/Down/Left/Right; mouse buttons LMB, RMB, MMB, and the side buttons MB4 (back) and MB5 "
       "(forward), also written MOUSE4 / MOUSE5. Shift+Up means both together. The two grenade rows "
       "throw the frag and the special grenade also when the controller layout swaps the shoulder buttons.");
#endif
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
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(b[1]);
      ImGui::TableNextColumn();
      char buf[128];
      std::snprintf(buf, sizeof(buf), "%s", Get(b[0]).c_str());
      ImGui::PushID(b[0]);
      ImGui::SetNextItemWidth(-1);
      if (ImGui::InputText("##v", buf, sizeof(buf), ImGuiInputTextFlags_EnterReturnsTrue)) {
        rex::cvar::SetFlagByName(b[0], buf);
        dirty_ = true;
      }
      ImGui::PopID();
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
      {"cod3_perf_overlay", "false"},
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
    restart_needed_ |= ResetSettings({"vulkan_pipeline_creation_threads", "d3d12_pipeline_creation_threads",
                                      "async_shader_compilation", "store_shaders",
                                      "render_target_path_vulkan", "render_target_path_d3d12",
                                      "cod3_perf_overlay", "cod3_perf_log",
                                      "clear_memory_page_state"});
    dirty_ = true;
  }
  Section("Low-end PC");
  if (ImGui::Button("Use low-end PC settings...")) ImGui::OpenPopup("Low-end PC settings##lowend");
  if (ImGui::BeginPopupModal("Low-end PC settings##lowend", nullptr,
                             ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::TextUnformatted("For a PC with 2-4 CPU cores and integrated graphics:");
    ImGui::BulletText("Render resolution 720p (Xbox 360 original stays if chosen)");
    ImGui::BulletText("No FXAA, plain bilinear scaling, the game's own texture filtering");
    ImGui::BulletText("HD textures off; the game's 2x MSAA drawn as real 2x");
    ImGui::BulletText("Geometry the game hasn't changed isn't sent to the GPU again every frame");
    ImGui::BulletText("%d shader build thread(s), shaders kept on disk", LowEndShaderThreads());
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
  Help("Changes the options in Video and on this page that cost the most on a slow PC, for the "
       "least difference on screen. Each can still be changed back by hand; most apply after a "
       "restart. If something in the world goes missing or stops updating afterwards, set "
       "clear_memory_page_state = true in cod3.toml.");
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
      dirty_ |= CvarCombo("Debug screenshots", "cod3_debug_screenshots", v, n, 5, &restart_needed_);
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
  ImGui::Separator();
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
  ImGui::Separator();
  unsigned hw = std::max(1u, std::thread::hardware_concurrency());
  ImGui::Text("Logical CPUs: %u", hw);
  for (const char* cvar : {"vulkan_pipeline_creation_threads", "d3d12_pipeline_creation_threads"}) {
    if (!Has(cvar)) continue;
    int v = GetInt(cvar, -1);
    if (ImGui::SliderInt("Shader build threads", &v, -1, static_cast<int>(hw),
                         v < 0 ? "auto (CPUs - 1)" : "%d")) {
      rex::cvar::SetFlagByName(cvar, std::to_string(v));
      restart_needed_ = true;
      dirty_ = true;
    }
    Help("CPU threads that build GPU shaders the first time a new effect appears. Auto uses all "
         "but one. Applies after a restart.");
  }
  dirty_ |= CvarCheckbox("Asynchronous shader compilation", "async_shader_compilation",
                         &restart_needed_);
  Help("While a new effect's shader is still being built, keep playing and draw it a moment later, "
       "instead of pausing the game until it's ready. Fewer stutters the first time you see an "
       "effect.");
  dirty_ |= CvarCheckbox("Store shader cache on disk", "store_shaders", &restart_needed_);
  Help("Saves built shaders, so the next session doesn't stutter on effects you've already seen.");
  Help("The recompiled game keeps the Xbox 360's own thread layout; these settings only control "
       "the extra threads used for shader work.");
}

// What the console has saved (seta, and changes to com_maxfps, cg_drawFPS,
// cg_fov, cg_fovScale): userdata/cod3_config.cfg.
void SettingsWindow::DrawInterface() {
  if (ResetButton("reset_interface", "interface settings")) {
    ResetSettings({"cod3_ui_theme", "cod3_ui_text_size", "cod3_ui_opacity"});
    dirty_ = true;
  }
  Section("Look");
  size_t count = 0;
  const ThemeInfo* themes = Themes(&count);
  const std::string current = Get("cod3_ui_theme");
  const ThemeInfo* chosen = &themes[0];
  for (size_t i = 0; i < count; ++i)
    if (current == themes[i].id) chosen = &themes[i];
  if (ImGui::BeginCombo("Colors", chosen->label)) {
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
       "the list. The settings in the other tabs aren't touched.");
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
        {"VIDEO", &SettingsWindow::DrawVideo},         {"GAME", &SettingsWindow::DrawGame},
        {"CROSSHAIR", &SettingsWindow::DrawCrosshair}, {"CONTROLS", &SettingsWindow::DrawControls},
        {"INTERFACE", &SettingsWindow::DrawInterface}, {"PERFORMANCE", &SettingsWindow::DrawPerformance},
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
