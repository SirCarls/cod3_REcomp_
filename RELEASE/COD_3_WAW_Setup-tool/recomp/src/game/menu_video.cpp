// cod3 - the port's video settings in the game's own Options menus (see
// menu_video.h). This file holds the settings rows and the front end's
// Options page (its new VIDEO item); the pages that show the rows (Options >
// Gameplay in video mode, the pause menu's options) are in menu_fov.cpp,
// next to the field of view rows on the same pages.

#include "game/menu_video.h"

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "game/addresses.h"
#include "game/engine.h"
#include "game/frame_rate.h"
#include "game/guest.h"
#include "game/hd_textures.h"
#include "game/multiplayer.h"
#include "game/qte_actions.h"
#include "game/render_resolution.h"
#include "game/shadows.h"

namespace {

namespace fe = cod3::addr::fe;
namespace opt = cod3::addr::fe::options;
using cod3::guest::Call;
using cod3::guest::ReadF32;
using cod3::guest::ReadU16;
using cod3::guest::ReadU32;
using cod3::guest::VirtualSlot;
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

bool VsyncOn() {
  if (Has(kVsyncVulkan)) return !GetBool(kVsyncVulkan);
  if (Has(kVsyncD3D12)) return !GetBool(kVsyncD3D12);
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

std::string FrameRateText(int fps) { return fps <= 0 ? "NO LIMIT" : std::to_string(fps); }

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
struct Text {
  const char* key;
  const char* text;  // nullptr: made when asked (HelpText)
};

constexpr const char* kTitleKey = "FEMENU_OP_VIDEO_TITLE";
constexpr const char* kItemKey = "FEMENU_OP_VIDEO";
constexpr const char* kItemInstKey = "FEMENU_OP_INST_VIDEO";
constexpr const char* kPauseItemKey = "FEMENU_COP_VIDEO_OPTIONS";
constexpr const char* kQuitToMenuKey = "FEMENU_PAUSE_QUIT_TO_MENU";
constexpr const char* kQuitToDesktopKey = "FEMENU_PAUSE_QUIT_TO_DESKTOP";
constexpr const char* kQuitToTitleKey = "FEMENU_MAIN_QUIT_TO_TITLE";
constexpr const char* kQuitQuestionKey = "FEMENU_QUIT_TO_DESKTOP_QUESTION";
constexpr const char* kBrightnessKey = "FEMENU_COP_BRIGHTNESS";  // [menus-brightness]
constexpr const char* kLabelKeys[cod3::videomenu::kRowCount] = {
    "FEMENU_VIDEO_RESOLUTION", "FEMENU_VIDEO_MAXFPS",   "FEMENU_VIDEO_VSYNC",
    "FEMENU_VIDEO_AA",         "FEMENU_VIDEO_SHADOWS", "FEMENU_VIDEO_HDTEXTURES",
    "FEMENU_VIDEO_QTE",
};
constexpr const char* kInstKeys[cod3::videomenu::kRowCount] = {
    "FEMENU_VIDEO_INST_RESOLUTION", "FEMENU_VIDEO_INST_MAXFPS",  "FEMENU_VIDEO_INST_VSYNC",
    "FEMENU_VIDEO_INST_AA",         "FEMENU_VIDEO_INST_SHADOWS", "FEMENU_VIDEO_INST_HDTEXTURES",
    "FEMENU_VIDEO_INST_QTE",
};

constexpr Text kTexts[] = {
    {kTitleKey, "OPTIONS / VIDEO"},
    {kItemKey, "VIDEO"},
    {kItemInstKey,
     "Resolution, frame rate limit, V-Sync, anti-aliasing, shadows and HD textures. The same "
     "settings are in F2."},
    {kPauseItemKey, "VIDEO OPTIONS"},
    {kQuitToMenuKey, "QUIT TO MENU"},
    {kQuitToDesktopKey, "QUIT TO DESKTOP"},
    {kQuitToTitleKey, "QUIT TO TITLE SCREEN"},
    {kQuitQuestionKey, "Quit the game and go back to the desktop?"},
    {kBrightnessKey, "BRIGHTNESS"},  // [menus-brightness]
    {kLabelKeys[0], "RESOLUTION"},
    {kLabelKeys[1], "FRAME RATE LIMIT"},
    {kLabelKeys[2], "V-SYNC"},
    {kLabelKeys[3], "ANTI-ALIASING"},
    {kLabelKeys[4], "SHADOWS"},
    {kLabelKeys[5], "HD TEXTURES"},
    {kLabelKeys[6], "BATTLE ACTIONS"},
};

// The end of a help text for a setting that waits for a restart.
std::string AfterRestart(bool pending) {
  return pending ? " Changed: the game uses it from the next start." : " Applies after a restart.";
}

// The help text of a row, as it applies now.
std::string HelpText(int row) {
  std::string t;
  switch (row) {
    case cod3::videomenu::kResolution: {
      t = "How many pixels the game is drawn with. Auto matches the monitor in fullscreen, or the "
          "window. Natively the game draws every pixel itself, up to about 1920x1080; above that "
          "the graphics card draws each of its pixels as several.";
      t += GetBool("cod3_live_resolution") ? " Applies at once." : " Applies after a restart.";
      const cod3::RenderPlan plan = cod3::GetRenderPlan();
      if (plan.game_width) t += " Now: the game draws " + cod3::DescribePlan(plan) + ".";
      break;
    }
    case cod3::videomenu::kFrameRate:
      t = "The most frames per second the game may draw (com_maxfps). A limit, not a target: the "
          "game's speed doesn't change. Applies at once.";
      if (cod3::mp::IsMatchRunning()) t += " Can't be changed during a multiplayer match.";
      break;
    case cod3::videomenu::kVsync:
      t = "Shows each new picture only when the monitor starts a new refresh, so there is no "
          "tearing. It doesn't limit the frame rate.";
      t += AfterRestart(VsyncPending());
      break;
    case cod3::videomenu::kAntiAliasing:
      t = "Smooths jagged edges after the picture is drawn. Extreme smooths more and softens the "
          "picture a little.";
      t += RestartOnly("swap_post_effect") ? AfterRestart(AntiAliasingPending()) : " Applies at once.";
      break;
    case cod3::videomenu::kShadows:
      t = "The shadows soldiers, vehicles and other moving things cast in sunlight. Off: none (at "
          "once). The level's own light and shade stay.";
      break;
    case cod3::videomenu::kHdTextures:
      t = "The game's own textures upscaled to twice the detail, made by the setup tool.";
      t += AfterRestart(HdTexturesPending());
      if (cod3::GetHdTextures() == cod3::HdTextures::kNoPack)
        t += " Not installed: tick \"Upscale the extracted textures\" in the setup tool.";
      break;
    case cod3::videomenu::kBattleActions:
      t = cod3::qte::MenuHelp();
      break;
  }
  return t;
}

// Guest copies of the texts: fixed ones once, help texts in a buffer of
// their own that is rewritten whenever the game asks for the key.
constexpr uint32_t kHelpBufferSize = 512;
std::mutex g_text_mutex;
std::unordered_map<std::string, uint32_t> g_guest_texts;

uint32_t GuestAlloc(uint32_t size) {
  auto* mem = rex::system::kernel_state()->memory();
  return mem->SystemHeapAlloc(size);
}

uint32_t FixedText(uint8_t* base, const char* key, const char* text) {
  std::lock_guard lock(g_text_mutex);
  uint32_t& addr = g_guest_texts[key];
  if (!addr) {
    const uint32_t len = uint32_t(std::strlen(text)) + 1;
    addr = GuestAlloc(len);
    if (addr) std::memcpy(base + addr, text, len);
  }
  return addr;
}

uint32_t HelpTextBuffer(uint8_t* base, const char* key, const std::string& text) {
  std::lock_guard lock(g_text_mutex);
  uint32_t& addr = g_guest_texts[key];
  if (!addr) addr = GuestAlloc(kHelpBufferSize);
  if (!addr) return 0;
  const size_t len = std::min<size_t>(text.size(), kHelpBufferSize - 1);
  std::memcpy(base + addr, text.data(), len);
  base[addr + len] = 0;
  return addr;
}

// ---------------------------------------------------------------------------
// Guest UI calls (the same as menu_fov.cpp's)
// ---------------------------------------------------------------------------
bool Valid(uint32_t p) { return p >= 0x10000 && p < 0xC0000000u && (p & 3) == 0; }

uint64_t VCall(PPCContext& ctx, uint8_t* base, uint32_t obj, uint32_t slot, uint64_t a1 = 0,
               uint64_t a2 = 0, uint64_t a3 = 0) {
  if (!obj) return 0;
  Call c(ctx, base);
  return c.Invoke(VirtualSlot(base, obj, slot), {obj, a1, a2, a3});
}

uint32_t FindElement(PPCContext& ctx, uint8_t* base, uint32_t panel, const char* name) {
  Call c(ctx, base);
  const uint32_t s = c.PushString(name);
  return static_cast<uint32_t>(c.Invoke(fe::kPanelFindElement, {panel, s}));
}

uint32_t FindQuad(PPCContext& ctx, uint8_t* base, uint32_t panel, const char* name) {
  Call c(ctx, base);
  const uint32_t s = c.PushString(name);
  const uint32_t quad = static_cast<uint32_t>(c.Invoke(fe::kPanelFindQuad, {panel, s}));
  if (!Valid(quad)) return 0;
  const uint32_t str = ReadU32(base, quad + fe::kQuadName);
  if (!str || cod3::guest::ReadString(base, str + 12, 64) != name) return 0;
  return quad;
}

void SetTextKey(PPCContext& ctx, uint8_t* base, uint32_t elem, const char* key) {
  if (!Valid(elem)) return;
  Call c(ctx, base);
  const uint32_t s = c.PushString(key);
  c.Invoke(VirtualSlot(base, elem, fe::kElemSetTextKey), {elem, s});
}

void SetInstructions(PPCContext& ctx, uint8_t* base, uint32_t box, const char* key) {
  if (!Valid(box)) return;
  Call c(ctx, base);
  const uint64_t width = c.Invoke(VirtualSlot(base, box, fe::kWrapGetWidth), {box});
  const uint32_t s = c.PushString(key);
  const float scale = ReadF32(base, fe::kTextScaleDefault);
  c.Invoke(VirtualSlot(base, box, fe::kWrapSetTextKey), {box, s, width}, {scale});
}

int16_t Selected(uint8_t* base, uint32_t page) {
  return static_cast<int16_t>(ReadU16(base, page + fe::kSelected));
}

void SetItemCount(uint8_t* base, uint32_t page, int count) {  // big-endian s16
  base[page + fe::kItemCount] = uint8_t(count >> 8);
  base[page + fe::kItemCount + 1] = uint8_t(count);
}

// ---------------------------------------------------------------------------
// Front end Options page: the VIDEO item
// ---------------------------------------------------------------------------
std::mutex g_pages_mutex;
std::unordered_set<uint32_t> g_options_pages;  // pages that have the VIDEO item
uint32_t g_back_from_video = 0;  // the Options page whose VIDEO page is open (g_pages_mutex)

bool HasVideoItem(uint8_t* base, uint32_t page) {
  if (!Valid(page) || ReadU32(base, page) != opt::kVtable) return false;
  std::lock_guard lock(g_pages_mutex);
  return g_options_pages.count(page) != 0;
}

// The big title and the help text for the VIDEO item.
void ShowVideoItem(PPCContext& ctx, uint8_t* base, uint32_t page) {
  SetTextKey(ctx, base, ReadU32(base, page + opt::kSelectedText), kItemKey);
  SetInstructions(ctx, base, ReadU32(base, page + opt::kInstructions), kItemInstKey);
}

// The Options > Gameplay page of the menu system that owns `page`.
uint32_t GameplayPage(uint8_t* base, uint32_t page) {
  const uint32_t system = ReadU32(base, page + fe::kPageSystem);
  if (!Valid(system)) return 0;
  const uint32_t pages = ReadU32(base, system + fe::kSysPages);
  if (!Valid(pages)) return 0;
  const uint32_t gameplay = ReadU32(base, pages + 4 * opt::kGameplayPage);
  return Valid(gameplay) ? gameplay : 0;
}

}  // namespace

// ===========================================================================
// Rows
// ===========================================================================
namespace cod3::videomenu {

const char* LabelKey(int row) { return row >= 0 && row < kRowCount ? kLabelKeys[row] : ""; }
const char* InstructionsKey(int row) { return row >= 0 && row < kRowCount ? kInstKeys[row] : ""; }
const char* TitleKey() { return kTitleKey; }
const char* ItemKey() { return kItemKey; }
const char* ItemInstructionsKey() { return kItemInstKey; }
const char* PauseItemKey() { return kPauseItemKey; }
const char* QuitToMenuKey() { return kQuitToMenuKey; }
const char* QuitToDesktopKey() { return kQuitToDesktopKey; }
const char* QuitToTitleKey() { return kQuitToTitleKey; }
const char* QuitToDesktopQuestionKey() { return kQuitQuestionKey; }

std::string Value(int row) {
  switch (row) {
    case kResolution: {
      const std::string cur = Get("cod3_render_resolution");
      const int i = FindChoice(kResolutions, cur);
      if (i >= 0) return kResolutions[i].text;
      uint32_t w = 0, h = 0;  // a size typed in the console ("1600x900", r_mode)
      if (ParseResolutionChoice(cur, &w, &h)) return std::to_string(w) + " X " + std::to_string(h);
      return Upper(cur);
    }
    case kFrameRate:
      return FrameRateText(FrameRate::Get().limit());
    case kVsync:
      return VsyncOn() ? "ON" : "OFF";
    case kAntiAliasing: {
      const std::string cur = Get("swap_post_effect");
      const int i = FindChoice(kAAChoices, cur);
      return i >= 0 ? kAAChoices[i].text : Upper(cur);
    }
    case kShadows: {
      const std::string cur = Get("cod3_shadow_quality");
      const int i = FindChoice(kShadowQuality, cur.empty() ? "original" : cur);
      return i >= 0 ? kShadowQuality[i].text : Upper(cur);
    }
    case kHdTextures:
      return GetBool("cod3_hd_textures") ? "ON" : "OFF";
    case kBattleActions:
      return qte::MenuValue();
  }
  return {};
}

std::string Note(int row) {
  constexpr const char* kAfterRestart = "APPLIES AFTER A RESTART";
  switch (row) {
    case kResolution:
      return GetBool("cod3_live_resolution") ? "" : kAfterRestart;
    case kFrameRate:
      return mp::IsMatchRunning() ? "NOT DURING A MULTIPLAYER MATCH" : "";
    case kVsync:
    case kHdTextures:
      return kAfterRestart;
    case kAntiAliasing:
      return RestartOnly("swap_post_effect") ? kAfterRestart : "";
    case kShadows:
      return "";
    case kBattleActions:
      return "VALVES, CRANES, CROWBARS, ROWING";
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
      qte::MenuStep();  // two choices: either way flips
      Save();
      return Value(row);
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

uint32_t Localize(uint8_t* base, const char* key) {
  if (!key || std::strncmp(key, "FEMENU_", 7) != 0) return 0;
  for (const Text& t : kTexts)
    if (std::strcmp(key, t.key) == 0) return FixedText(base, t.key, t.text);
  for (int row = 0; row < kRowCount; ++row)
    if (std::strcmp(key, kInstKeys[row]) == 0) return HelpTextBuffer(base, kInstKeys[row], HelpText(row));
  return 0;
}

void SetSaveHandler(std::function<void()> save) {
  std::lock_guard lock(g_save_mutex);
  g_save = std::move(save);
}

// ---------------------------------------------------------------------------
// [menus-brightness] The game's brightness (addresses.h, brightness)
// ---------------------------------------------------------------------------
namespace {
namespace br = cod3::addr::brightness;

float BrightnessMax(uint8_t* base) {
  const float max = ReadF32(base, br::kMax);
  return std::isfinite(max) && max > br::kMin ? max : 2.0f;
}

void SetBrightnessValue(uint8_t* base, float value) {
  value = std::clamp(value, br::kMin, BrightnessMax(base));
  cod3::guest::WriteF32(base, br::kValue, value);
}
}  // namespace

float Brightness(uint8_t* base) {
  const float v = ReadF32(base, br::kValue);
  return std::isfinite(v) ? v : 1.0f;
}

float BrightnessFraction(uint8_t* base) {
  const float range = BrightnessMax(base) - br::kMin;
  return std::clamp((Brightness(base) - br::kMin) / range, 0.0f, 1.0f);
}

void SetBrightnessFraction(uint8_t* base, float fraction) {
  const float range = BrightnessMax(base) - br::kMin;
  SetBrightnessValue(base, br::kMin + std::clamp(fraction, 0.0f, 1.0f) * range);
}

void StepBrightness(uint8_t* base, bool right, const char* from) {
  float step = ReadF32(base, br::kStep);
  if (!(step > 0.0f && step < 1.0f)) step = 0.05f;
  SetBrightnessValue(base, Brightness(base) + (right ? step : -step));
  REXLOG_INFO("cod3: brightness {:.2f} ({})", Brightness(base), from);
}

float ExchangeProfileBrightness(uint8_t* base, float value) {
  const float old = ReadF32(base, br::kProfile + br::kProfileValue);
  cod3::guest::WriteF32(base, br::kProfile + br::kProfileValue, value);
  return old;
}

const char* BrightnessKey() { return kBrightnessKey; }

}  // namespace cod3::videomenu

// ===========================================================================
// Front end Options page (ProfileEditMenu): a fifth item, VIDEO
// ===========================================================================

// Init(this, panel): the game adds GAMEPLAY, CONTROLS, SOUND and SCREEN and
// hides the fifth row's band and line. VIDEO goes on that row: the page's
// item array gets a fifth slot (a bigger array in place of the page's, like
// the in-game options in menu_fov.cpp), the item is added on text_option_05
// and the row's band and line are shown again.
REX_EXTERN(__imp__sub_824E2948);
REX_HOOK_RAW(sub_824E2948) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_824E2948(ctx, base);
  cod3::videomenu::NoteStartValues();
  if (!Valid(self) || ReadU32(base, self) != opt::kVtable) return;
  // The other language layout moves the rows; there the settings stay in F2.
  if (ReadU32(base, fe::kLanguageMode) != 0) return;
  if (static_cast<int16_t>(ReadU16(base, self + fe::kItemCount)) != opt::kItemCount) return;
  const uint32_t panel = ReadU32(base, self + opt::kPanel);
  if (!Valid(panel)) return;
  const uint32_t label = FindElement(ctx, base, panel, "text_option_05");
  const uint32_t band = FindQuad(ctx, base, panel, "bkg_row_05");
  const uint32_t line = FindQuad(ctx, base, panel, "bkg_line_04");
  const uint32_t old_items = ReadU32(base, self + fe::kItems);
  if (!Valid(label) || !Valid(old_items)) {
    REXLOG_WARN("cod3: Options panel has no fifth row; the VIDEO item is not added");
    return;
  }

  uint32_t items = 0;
  {
    Call c(ctx, base);
    items = static_cast<uint32_t>(c.Invoke(fe::kOperatorNew, {4u * (opt::kItemCount + 1)}));
  }
  if (!Valid(items)) return;
  for (int i = 0; i < opt::kItemCount + 1; ++i)
    cod3::guest::WriteU32(base, items + 4 * i, i < opt::kItemCount ? ReadU32(base, old_items + 4 * i) : 0);
  cod3::guest::WriteU32(base, self + fe::kItems, items);
  {
    Call c(ctx, base);
    c.Invoke(fe::kOperatorDelete, {old_items});
  }

  // this->AddItem(4, text_option_05, 0); items[4]->SetLabelKey("VIDEO")
  VCall(ctx, base, self, fe::kPageAddItem, opt::kVideoItem, label, 0);
  const uint32_t item = ReadU32(base, items + 4 * opt::kVideoItem);
  if (!Valid(item)) {
    REXLOG_WARN("cod3: Options page refused the VIDEO item");
    return;  // the count stays 4: the page never reads the fifth slot
  }
  {
    Call c(ctx, base);
    const uint32_t s = c.PushString(kItemKey);
    c.Invoke(VirtualSlot(base, item, fe::kItemSetLabelKey), {item, s});
  }
  for (uint32_t e : {label, band, line})
    if (Valid(e)) VCall(ctx, base, e, fe::kElemSetVisible, 1);
  SetItemCount(base, self, opt::kItemCount + 1);

  std::lock_guard lock(g_pages_mutex);
  g_options_pages.insert(self);
}

REX_EXTERN(__imp__ProfileEditMenu__dtor);
REX_HOOK_RAW(ProfileEditMenu__dtor) {
  {
    std::lock_guard lock(g_pages_mutex);
    g_options_pages.erase(ctx.r3.u32);
  }
  __imp__ProfileEditMenu__dtor(ctx, base);
}

// The item handler (this, item): GAMEPLAY opens the gameplay page as itself,
// VIDEO opens it in video mode the same way (FEMenuSystem "go to page 3").
REX_EXTERN(__imp__ProfileEditMenu__vfunc_19);
REX_HOOK_RAW(ProfileEditMenu__vfunc_19) {
  const uint32_t self = ctx.r3.u32;
  const int item = static_cast<int32_t>(ctx.r4.u32);
  if (HasVideoItem(base, self) && (item == 0 || item == opt::kVideoItem)) {
    const uint32_t gameplay = GameplayPage(base, self);
    if (gameplay) cod3::videomenu::SetGameplayPageMode(gameplay, item == opt::kVideoItem);
    if (item == opt::kVideoItem) {
      if (!gameplay) return;
      {
        std::lock_guard lock(g_pages_mutex);
        g_back_from_video = self;
      }
      const uint32_t system = ReadU32(base, self + fe::kPageSystem);
      VCall(ctx, base, system, fe::kSysGoTo, opt::kGameplayPage, 0);
      return;
    }
  }
  __imp__ProfileEditMenu__vfunc_19(ctx, base);
}

// Opening the page and moving the highlight set the big title and the help
// text from two four-entry tables; for VIDEO they are ours. Coming back from
// the VIDEO page, VIDEO stays highlighted (the game starts at the top).
REX_EXTERN(__imp__ProfileEditMenu__vfunc_20);
REX_HOOK_RAW(ProfileEditMenu__vfunc_20) {
  const uint32_t self = ctx.r3.u32;
  __imp__ProfileEditMenu__vfunc_20(ctx, base);
  if (!HasVideoItem(base, self)) return;
  bool back_from_video = false;
  {
    std::lock_guard lock(g_pages_mutex);
    back_from_video = g_back_from_video == self;
    g_back_from_video = 0;
  }
  if (back_from_video && Selected(base, self) != opt::kVideoItem)
    VCall(ctx, base, self, fe::kPageSetSelection, opt::kVideoItem, 1);
  if (Selected(base, self) == opt::kVideoItem) ShowVideoItem(ctx, base, self);
}

REX_EXTERN(__imp__sub_824C3248);
REX_HOOK_RAW(sub_824C3248) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_824C3248(ctx, base);
  if (HasVideoItem(base, self) && Selected(base, self) == opt::kVideoItem) ShowVideoItem(ctx, base, self);
}

REX_EXTERN(__imp__sub_824C3190);
REX_HOOK_RAW(sub_824C3190) {
  const uint32_t self = ctx.r3.u32;
  __imp__sub_824C3190(ctx, base);
  if (HasVideoItem(base, self) && Selected(base, self) == opt::kVideoItem) ShowVideoItem(ctx, base, self);
}
