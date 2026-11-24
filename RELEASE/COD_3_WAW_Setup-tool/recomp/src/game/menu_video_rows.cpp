// cod3 - the video settings rows of the game's own Options menus (see
// menu_video.h): what each row shows and what a step on it changes, with
// its label and help text. [menus-categories] Moved here from
// menu_video.cpp, unchanged, so the multiplayer program's VIDEO page
// (game/mp/options_mp.cpp) shows the same rows: this file is built into
// both programs and calls nothing in the game itself. The pages that show
// the rows are menu_fov.cpp (campaign) and game/mp/options_mp.cpp
// (multiplayer); menu_video.cpp keeps the campaign's front end VIDEO item.

#include "game/menu_video.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <functional>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>  // [acct2-bro Text lane] 2026-11-22 std::pair (Shown)
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "game/engine.h"
#include "game/frame_rate.h"
#include "game/hd_textures.h"
#include "game/menu_port_rows.h"  // [acct2-bro Text lane] 2026-11-22 Words(): the texts in the chosen language
#include "game/multiplayer.h"
#include "game/post_effects.h"  // [post-effects]
#include "game/program.h"       // [post-effects]
#if !defined(COD3_MP)
#include "game/qte_actions.h"   // battle actions: the campaign only
#endif
#include "game/render_resolution.h"
#include "game/shadows.h"

namespace {

using cod3::videomenu::Row;

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
std::string Get(const char* cvar) { return rex::cvar::GetFlagByName(cvar); }
bool Has(const char* cvar) { return rex::cvar::GetFlagInfo(cvar) != nullptr; }
bool GetBool(const char* cvar) { return Get(cvar) == "true"; }

std::string Upper(std::string s) {
  for (char& c : s) c = char(std::toupper(static_cast<unsigned char>(c)));
  return s;
}

// [acct2-bro Text lane] 2026-11-22 The rows' help texts, values and notes in the chosen language: each English
// text has a key whose "@port" row (game/text_port_words.inc, or a pack's own row) answers it, else the English stays
// (as before). "{0}" in a text: the number or size put in by a plain replace (a text is never a format string).
std::string Say(const char* key, const char* english) { return cod3::portrows::Words(key, english); }
std::string SayWith(const char* key, const char* english, const std::string& what) {
  std::string text = Say(key, english);
  if (const size_t at = text.find("{0}"); at != std::string::npos) text.replace(at, 3, what);
  return text;
}
// A choice's or a switch's shown word ("OFF", "ON", "ORIGINAL", ...): its key, else as written ("FXAA", sizes).
std::string Shown(const char* text) {
  static constexpr std::pair<const char*, const char*> kKeys[] = {
      {"ON", "COD3_PORT_V_ON"},           {"OFF", "COD3_PORT_V_OFF"},     {"AUTO", "COD3_PORT_V_AUTO"},
      {"ORIGINAL", "COD3_PORT_V_ORIGINAL"}, {"FXAA EXTREME", "COD3_PORT_V_FXAA_EXTREME"},
      {"NO LIMIT", "COD3_PORT_V_NO_LIMIT"}};
  for (const auto& [english, key] : kKeys)
    if (std::strcmp(text, english) == 0) return Say(key, english);
  return text;
}

struct Choice {
  const char* value;  // the setting's value
  const char* text;   // what the row shows
};

// The same choices as F2 > Video and the setup tool (game/render_resolution.h).
const auto& kResolutions = cod3::kResolutionChoices;
constexpr Choice kAAChoices[] = {{"none", "OFF"}, {"fxaa", "FXAA"}, {"fxaa_extreme", "FXAA EXTREME"}};
// Left is less: off, the game's own.
constexpr Choice kShadowQuality[] = {{"off", "OFF"}, {"original", "ORIGINAL"}};
// com_maxfps steps, low to high; 0 (no limit) last.
constexpr int kFrameRates[] = {30, 60, 75, 85, 120, 144, 165, 200, 240, 300, 360, 500, 1000, 0};

constexpr const char* kVsyncVulkan = "vulkan_allow_present_mode_immediate";
constexpr const char* kVsyncD3D12 = "d3d12_allow_variable_refresh_rate_and_tearing";

template <typename T, size_t N>
int FindChoice(const T (&list)[N], const std::string& value) {
  for (size_t i = 0; i < N; ++i)
    if (value == list[i].value) return int(i);
  return -1;
}

// The choices of `cvar` this build accepts (like F2: FidelityFX-only values
// and the like are left out).
template <size_t N>
std::vector<int> Allowed(const Choice (&list)[N], const char* cvar) {
  std::vector<int> out;
  const auto* info = rex::cvar::GetFlagInfo(cvar);
  for (size_t i = 0; i < N; ++i) {
    if (info && info->constraints.HasAllowedValues()) {
      const auto& allowed = info->constraints.allowed_values;
      if (std::find(allowed.begin(), allowed.end(), list[i].value) == allowed.end()) continue;
    }
    out.push_back(int(i));
  }
  return out;
}

// One step along `order` (indices into a list) from `current` (-1 = not in
// the list: the first or last one); stops at both ends like the game's own
// field of view row.
int StepIndex(const std::vector<int>& order, int current, bool right) {
  if (order.empty()) return -1;
  auto it = std::find(order.begin(), order.end(), current);
  if (it == order.end()) return right ? order.front() : order.back();
  const size_t at = size_t(it - order.begin());
  if (right) return order[std::min(at + 1, order.size() - 1)];
  return order[at == 0 ? 0 : at - 1];
}

bool RestartOnly(const char* cvar) {
  const auto* info = rex::cvar::GetFlagInfo(cvar);
  return info && info->lifecycle != rex::cvar::Lifecycle::kHotReload;
}

// [acct2-main] 2026-11-14 Windows builds have both flags (the SDK's two presenters). The row writes both, but shows
// the one of the graphics API the game starts with (cod3_graphics_api; "auto" is Vulkan), so a cod3.toml edited by
// hand, where the two differ, shows the one that applies. Linux has only Vulkan's.
bool VsyncOn() {
  const bool d3d12 = Has("cod3_graphics_api") && Get("cod3_graphics_api") == "d3d12";
  const char* first = d3d12 ? kVsyncD3D12 : kVsyncVulkan;
  const char* second = d3d12 ? kVsyncVulkan : kVsyncD3D12;
  if (Has(first)) return !GetBool(first);
  if (Has(second)) return !GetBool(second);
  return false;
}

// What the running game started with (NoteStartValues), for the rows whose
// change waits for a restart.
std::mutex g_start_mutex;
bool g_start_noted = false;
bool g_start_vsync = false;
std::string g_start_aa;

bool VsyncPending() {
  std::lock_guard lock(g_start_mutex);
  return g_start_noted && VsyncOn() != g_start_vsync;
}

bool AntiAliasingPending() {
  std::lock_guard lock(g_start_mutex);
  return g_start_noted && RestartOnly("swap_post_effect") && Get("swap_post_effect") != g_start_aa;
}

bool HdTexturesPending() {
  const cod3::HdTextures now = cod3::GetHdTextures();
  if (now != cod3::HdTextures::kOn && now != cod3::HdTextures::kOff) return false;
  return GetBool("cod3_hd_textures") != (now == cod3::HdTextures::kOn);
}

// [acct2-bro Text lane] 2026-11-22 "NO LIMIT" in the chosen language
std::string FrameRateText(int fps) { return fps <= 0 ? Shown("NO LIMIT") : std::to_string(fps); }

std::mutex g_save_mutex;
std::function<void()> g_save;

void Save() {
  std::function<void()> save;
  {
    std::lock_guard lock(g_save_mutex);
    save = g_save;
  }
  if (save) save();
}

// ---------------------------------------------------------------------------
// Texts
// ---------------------------------------------------------------------------
constexpr const char* kLabelKeys[cod3::videomenu::kRowCount] = {
    "FEMENU_VIDEO_RESOLUTION", "FEMENU_VIDEO_MAXFPS",   "FEMENU_VIDEO_VSYNC",
    "FEMENU_VIDEO_AA",         "FEMENU_VIDEO_SHADOWS", "FEMENU_VIDEO_HDTEXTURES",
    "FEMENU_VIDEO_QTE",
    "FEMENU_VIDEO_DOF",        "FEMENU_VIDEO_MOTIONBLUR",  // [post-effects]
};
constexpr const char* kInstKeys[cod3::videomenu::kRowCount] = {
    "FEMENU_VIDEO_INST_RESOLUTION", "FEMENU_VIDEO_INST_MAXFPS",  "FEMENU_VIDEO_INST_VSYNC",
    "FEMENU_VIDEO_INST_AA",         "FEMENU_VIDEO_INST_SHADOWS", "FEMENU_VIDEO_INST_HDTEXTURES",
    "FEMENU_VIDEO_INST_QTE",
    "FEMENU_VIDEO_INST_DOF",        "FEMENU_VIDEO_INST_MOTIONBLUR",  // [post-effects]
};

constexpr const char* kLabelTexts[cod3::videomenu::kRowCount] = {
    "RESOLUTION",
    "FRAME RATE LIMIT",
    "V-SYNC",
    "ANTI-ALIASING",
    "SHADOWS",
    "HD TEXTURES",
    "BATTLE ACTIONS",
    "DEPTH OF FIELD",  // [post-effects]
    "MOTION BLUR",     // [post-effects]
};

// The end of a help text for a setting that waits for a restart.
std::string AfterRestart(bool pending) {  // [acct2-bro Text lane] 2026-11-22 in the chosen language
  return " " + (pending ? Say("COD3_PORT_VH_NEXT_START", "Changed: the game uses it from the next start.")
                        : Say("COD3_PORT_VH_AFTER_RESTART", "Applies after a restart."));
}
std::string AtOnce() { return " " + Say("COD3_PORT_VH_AT_ONCE", "Applies at once."); }

// The help text of a row, as it applies now.
std::string RowHelpText(int row) {
  std::string t;
  switch (row) {
    case cod3::videomenu::kResolution: {
      // [acct2-bro Text lane] 2026-11-22 each part through Say(): the chosen language's words, else this English
      t = Say("COD3_PORT_VH_RESOLUTION",
              "How many pixels the game is drawn with. Auto matches the monitor in fullscreen, or the "
              "window. Natively the game draws every pixel itself, up to about 1920x1080; above that "
              "the graphics card draws each of its pixels as several.");
      t += GetBool("cod3_live_resolution") ? AtOnce() : AfterRestart(false);
      const cod3::RenderPlan plan = cod3::GetRenderPlan();
      if (plan.game_width)
        t += " " + SayWith("COD3_PORT_VH_NOW_DRAWS", "Now: the game draws {0}.", cod3::DescribePlan(plan));
      break;
    }
    case cod3::videomenu::kFrameRate:
      t = Say("COD3_PORT_VH_FRAMERATE",  // [acct2-bro Text lane] 2026-11-22
              "The most frames per second the game may draw (com_maxfps). A limit, not a target: the "
              "game's speed doesn't change. Applies at once.");
      if (cod3::mp::IsMatchRunning())
        t += " " + Say("COD3_PORT_VH_NOT_IN_MATCH", "Can't be changed during a multiplayer match.");
      break;
    case cod3::videomenu::kVsync:
      t = Say("COD3_PORT_VH_VSYNC",  // [acct2-bro Text lane] 2026-11-22
              "Shows each new picture only when the monitor starts a new refresh, so there is no "
              "tearing. It doesn't limit the frame rate.");
      t += AfterRestart(VsyncPending());
      break;
    case cod3::videomenu::kAntiAliasing:
      t = Say("COD3_PORT_VH_AA",  // [acct2-bro Text lane] 2026-11-22
              "Smooths jagged edges after the picture is drawn. Extreme smooths more and softens the "
              "picture a little.");
      t += RestartOnly("swap_post_effect") ? AfterRestart(AntiAliasingPending()) : AtOnce();
      break;
    case cod3::videomenu::kShadows:
      t = Say("COD3_PORT_VH_SHADOWS",  // [acct2-bro Text lane] 2026-11-22
              "The shadows soldiers, vehicles and other moving things cast in sunlight. Off: none (at "
              "once). The level's own light and shade stay.");
      break;
    case cod3::videomenu::kHdTextures:
      t = Say("COD3_PORT_VH_HD",  // [acct2-bro Text lane] 2026-11-22
              "The game's own textures upscaled to twice the detail, made by the setup tool.");
      t += AfterRestart(HdTexturesPending());
      if (cod3::GetHdTextures() == cod3::HdTextures::kNoPack)
        t += " " + Say("COD3_PORT_VH_HD_NOT_INSTALLED",
                       "Not installed: tick \"Upscale the extracted textures\" in the setup tool.");
      break;
    case cod3::videomenu::kBattleActions:
#if !defined(COD3_MP)
      t = cod3::qte::MenuHelp();
#endif
      break;
    case cod3::videomenu::kDepthOfField:  // [post-effects]
      t = cod3::kMultiplayer  // [acct2-bro Text lane] 2026-11-22 in the chosen language
              ? Say("COD3_PORT_VH_DOF_MP",
                    "The background blurs where a map uses depth of field. Off: always sharp. Applies at once.")
              : Say("COD3_PORT_VH_DOF_SP",
                    "The background blurs while you aim down the sights, where a mission uses depth of field. "
                    "Off: always sharp. Applies at once.");
      break;
    case cod3::videomenu::kMotionBlur:  // [post-effects]
      t = Say("COD3_PORT_VH_MOTIONBLUR",  // [acct2-bro Text lane] 2026-11-22
              "The screen trails the game shows at some moments. Off: never. Applies at once.");
      break;
  }
  return t;
}

}  // namespace

// ===========================================================================
// Rows
// ===========================================================================
namespace cod3::videomenu {

const char* LabelKey(int row) { return row >= 0 && row < kRowCount ? kLabelKeys[row] : ""; }
const char* InstructionsKey(int row) { return row >= 0 && row < kRowCount ? kInstKeys[row] : ""; }
const char* LabelText(int row) { return row >= 0 && row < kRowCount ? kLabelTexts[row] : ""; }
std::string HelpText(int row) { return RowHelpText(row); }

std::string Value(int row) {
  switch (row) {
    case kResolution: {
      const std::string cur = Get("cod3_render_resolution");
      const int i = FindChoice(kResolutions, cur);
      if (i >= 0) return Shown(kResolutions[i].text);  // [acct2-bro Text lane] 2026-11-22
      uint32_t w = 0, h = 0;  // a size typed in the console ("1600x900", r_mode)
      if (ParseResolutionChoice(cur, &w, &h)) return std::to_string(w) + " X " + std::to_string(h);
      return Upper(cur);
    }
    case kFrameRate:
      return FrameRateText(FrameRate::Get().limit());
    case kVsync:
      return Shown(VsyncOn() ? "ON" : "OFF");  // [acct2-bro Text lane] 2026-11-22
    case kAntiAliasing: {
      const std::string cur = Get("swap_post_effect");
      const int i = FindChoice(kAAChoices, cur);
      return i >= 0 ? Shown(kAAChoices[i].text) : Upper(cur);  // [acct2-bro Text lane] 2026-11-22
    }
    case kShadows: {
      const std::string cur = Get("cod3_shadow_quality");
      const int i = FindChoice(kShadowQuality, cur.empty() ? "original" : cur);
      return i >= 0 ? Shown(kShadowQuality[i].text) : Upper(cur);  // [acct2-bro Text lane] 2026-11-22
    }
    case kHdTextures:
      return Shown(GetBool("cod3_hd_textures") ? "ON" : "OFF");  // [acct2-bro Text lane] 2026-11-22
    case kBattleActions:
#if !defined(COD3_MP)
      return qte::MenuValue();
#else
      return {};  // multiplayer has no battle actions
#endif
    case kDepthOfField:  // [post-effects]
      return Shown(post_effects::DepthOfFieldOn() ? "ON" : "OFF");  // [acct2-bro Text lane] 2026-11-22
    case kMotionBlur:  // [post-effects]
      return Shown(post_effects::MotionBlurOn() ? "ON" : "OFF");  // [acct2-bro Text lane] 2026-11-22
  }
  return {};
}

std::string Note(int row) {
  // [acct2-bro Text lane] 2026-11-22 each note in the chosen language (COD3_PORT_VN_* "@port" rows), else the English
  const std::string kAfterRestart = Say("COD3_PORT_VN_AFTER_RESTART", "APPLIES AFTER A RESTART");
  switch (row) {
    case kResolution:
      return GetBool("cod3_live_resolution") ? "" : kAfterRestart;
    case kFrameRate:
      return mp::IsMatchRunning() ? Say("COD3_PORT_VN_NOT_IN_MATCH", "NOT DURING A MULTIPLAYER MATCH") : "";
    case kVsync:
    case kHdTextures:
      return kAfterRestart;
    case kAntiAliasing:
      return RestartOnly("swap_post_effect") ? kAfterRestart : "";
    case kShadows:
      return "";
    case kBattleActions:
      return Say("COD3_PORT_VN_QTE", "VALVES, CRANES, CROWBARS, ROWING");
    case kDepthOfField:  // [post-effects]
      return kMultiplayer ? Say("COD3_PORT_VN_DOF_MP", "BACKGROUND BLUR WHERE A MAP USES IT")
                          : Say("COD3_PORT_VN_DOF_SP", "BACKGROUND BLUR WHILE AIMING");
    case kMotionBlur:  // [post-effects]
      return Say("COD3_PORT_VN_MOTIONBLUR", "SCREEN TRAILS AT SOME MOMENTS");
  }
  return {};
}

std::string Step(PPCContext& ctx, uint8_t* base, int row, bool right) {
  (void)ctx;
  (void)base;
  switch (row) {
    case kResolution: {
      std::vector<int> order;
      for (int i = 0; i < int(std::size(kResolutions)); ++i) order.push_back(i);
      const int at = FindChoice(kResolutions, Get("cod3_render_resolution"));
      const int next = StepIndex(order, at, right);
      if (next < 0 || next == at) return Value(row);
      rex::cvar::SetFlagByName("cod3_render_resolution", kResolutions[next].value);
      RequestRenderResolution();  // a live switch at a frame boundary (render_resolution.h)
      REXLOG_INFO("cod3: video options: render resolution {}", kResolutions[next].value);
      Save();
      return Value(row);
    }
    case kFrameRate: {
      // A limit only: the game's clock keeps its speed at any setting. Still
      // not while a multiplayer match runs.
      if (mp::IsMatchRunning()) return {};
      const int cur = FrameRate::Get().limit();
      const int n = int(std::size(kFrameRates));
      auto rank = [](int fps) { return fps <= 0 ? 1 << 30 : fps; };  // no limit is the highest
      int at = -1;
      for (int i = 0; i < n; ++i)
        if (kFrameRates[i] == cur) at = i;
      int next;
      if (at >= 0) {
        next = std::clamp(at + (right ? 1 : -1), 0, n - 1);
      } else {  // a value typed in the console: the next one either way
        next = right ? n - 1 : 0;
        for (int i = 0; i < n; ++i) {
          if (right && rank(kFrameRates[i]) > rank(cur)) { next = i; break; }
          if (!right && rank(kFrameRates[i]) < rank(cur)) next = i;
        }
      }
      const int fps = kFrameRates[next];
      if (fps == cur) return FrameRateText(cur);
      Engine::Get().SetMaxFps(fps);  // com_maxfps at the next frame, saved like seta
      rex::cvar::SetFlagByName("cod3_max_fps", std::to_string(fps));
      REXLOG_INFO("cod3: video options: com_maxfps {}", fps);
      Save();
      return FrameRateText(fps);  // the cvar changes at the next frame
    }
    case kVsync: {
      const bool on = !VsyncOn();
      if (Has(kVsyncVulkan)) rex::cvar::SetFlagByName(kVsyncVulkan, on ? "false" : "true");
      if (Has(kVsyncD3D12)) rex::cvar::SetFlagByName(kVsyncD3D12, on ? "false" : "true");
      REXLOG_INFO("cod3: video options: V-Sync {} (after a restart)", on ? "on" : "off");
      Save();
      return Value(row);
    }
    case kAntiAliasing: {
      if (!Has("swap_post_effect")) return Value(row);
      const std::vector<int> order = Allowed(kAAChoices, "swap_post_effect");
      const int at = FindChoice(kAAChoices, Get("swap_post_effect"));
      const int next = StepIndex(order, at, right);
      if (next < 0 || next == at) return Value(row);
      rex::cvar::SetFlagByName("swap_post_effect", kAAChoices[next].value);
      REXLOG_INFO("cod3: video options: swap_post_effect {}", kAAChoices[next].value);
      Save();
      return Value(row);
    }
    case kShadows: {
      const std::vector<int> order = Allowed(kShadowQuality, "cod3_shadow_quality");
      std::string cur = Get("cod3_shadow_quality");
      const int at = FindChoice(kShadowQuality, cur.empty() ? "original" : cur);
      const int next = StepIndex(order, at, right);
      if (next < 0 || next == at) return Value(row);
      // Both apply at once ("off" through its change callback).
      rex::cvar::SetFlagByName("cod3_shadow_quality", kShadowQuality[next].value);
      REXLOG_INFO("cod3: video options: shadow quality {}", kShadowQuality[next].value);
      Save();
      return Value(row);
    }
    case kHdTextures: {
      const bool on = !GetBool("cod3_hd_textures");
      rex::cvar::SetFlagByName("cod3_hd_textures", on ? "true" : "false");
      REXLOG_INFO("cod3: video options: HD textures {} (after a restart)", on ? "on" : "off");
      Save();
      return Value(row);
    }
    case kBattleActions:
#if !defined(COD3_MP)
      qte::MenuStep();  // two choices: either way flips
      Save();
#endif
      return Value(row);
    case kDepthOfField:  // [post-effects] two choices: either way flips; at once
    case kMotionBlur: {
      const char* name = row == kDepthOfField ? post_effects::kDepthOfField : post_effects::kMotionBlur;
      rex::cvar::SetFlagByName(name, GetBool(name) ? "false" : "true");  // post_effects.cpp logs it
      Save();
      return Value(row);
    }
  }
  return {};
}

void NoteStartValues() {
  std::lock_guard lock(g_start_mutex);
  if (g_start_noted) return;
  g_start_noted = true;
  g_start_vsync = VsyncOn();
  g_start_aa = Get("swap_post_effect");
}

void SetSaveHandler(std::function<void()> save) {
  {
    std::lock_guard lock(g_save_mutex);
    g_save = std::move(save);
  }
  // [acct2-main] 2026-11-14 Both apps call this once, in OnPostSetup: the settings file is read and the GPU plugin
  // loaded, and nothing has changed a setting yet. So the start values are noted here, not first when an options
  // page opens: a change made in F2 before then used to count as the start value.
  NoteStartValues();
}

}  // namespace cod3::videomenu
