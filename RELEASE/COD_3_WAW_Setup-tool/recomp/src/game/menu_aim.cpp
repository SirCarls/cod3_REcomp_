// cod3 - the AIM page of the game's own Options menu (see menu_aim.h).

#include "game/menu_aim.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <fmt/format.h>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "game/aim_sensitivity.h"
#include "game/protected_dvars.h"  // SaveSettings: the menus' save path in the campaign
#include "ui/hud_overlay.h"  // the crosshair preview

namespace cod3::aimmenu {

namespace as = cod3::aimsens;

namespace {

// Player 1's settings: the campaign has one player.
constexpr int kPlayer = 0;

struct Text {
  const char* key;
  const char* text;
};
constexpr Text kTexts[] = {
    {"FEMENU_COP_AIM_TITLE", "OPTIONS / AIM"},
    {"FEMENU_COP_AIM_ITEM", "AIM"},
    {"FEMENU_COP_AIM_VIDEO_BUTTON", "VIDEO"},
    {"FEMENU_COP_AIM_MOVEMENT_BUTTON", "MOVEMENT"},
    {"FEMENU_COP_AIM_CLASS", "SENSITIVITY FOR"},
    {"FEMENU_COP_AIM_HIP", "  FROM THE HIP"},
    {"FEMENU_COP_AIM_ADS", "  AIMING DOWN SIGHTS"},
    {"FEMENU_COP_AIM_XHAIR", "CUSTOM CROSSHAIR"},
    {"FEMENU_COP_AIM_XHAIR_STYLE", "CROSSHAIR STYLE"},
    {"FEMENU_COP_AIM_XHAIR_LENGTH", "CROSSHAIR LENGTH"},
    {"FEMENU_COP_AIM_XHAIR_GAP", "CROSSHAIR GAP"},
    {"FEMENU_COP_AIM_XHAIR_THICKNESS", "CROSSHAIR THICKNESS"},
    {"FEMENU_COP_AIM_XHAIR_COLOR", "CROSSHAIR COLOR"},
};
// The row labels' keys, in Row's order.
constexpr const char* kLabelKeys[kRows] = {
    "FEMENU_COP_AIM_XHAIR",     "FEMENU_COP_AIM_XHAIR_STYLE",     "FEMENU_COP_AIM_XHAIR_LENGTH",
    "FEMENU_COP_AIM_XHAIR_GAP", "FEMENU_COP_AIM_XHAIR_THICKNESS", "FEMENU_COP_AIM_XHAIR_COLOR",
    "FEMENU_COP_AIM_CLASS",     "FEMENU_COP_AIM_HIP",             "FEMENU_COP_AIM_ADS",
};

// The weapon classes as the menu shows them (aim_sensitivity.h's groups, in
// its order). The value column between the arrows holds about nine letters.
constexpr std::array<const char*, as::kGroupCount> kClassNames = {
    "RIFLES", "SHOTGUNS", "SNIPERS",  "SMGS",    "LMGS",
    "MGS",    "PISTOLS",  "LAUNCHER", "MOUNTED", "OTHER"};

constexpr std::array<const char*, 5> kStyleNames = {"CROSS", "CROSS+DOT", "DOT", "CIRCLE", "T"};

// The crosshair's sizes: the same ranges and steps as F2 > Crosshair's sliders.
struct Size {
  const char* setting;
  double lo, hi, step;
};
constexpr Size kLengthSize = {"cod3_crosshair_size", 0.0, 32.0, 1.0};
constexpr Size kGapSize = {"cod3_crosshair_gap", -4.0, 32.0, 1.0};
constexpr Size kThicknessSize = {"cod3_crosshair_thickness", 0.5, 8.0, 0.5};

// CROSSHAIR COLOR's choices: AUTO (the game's own colors), then one color each.
struct Color {
  const char* name;
  int r, g, b;
};
constexpr std::array<Color, 7> kColors = {{{"AUTO", 0, 0, 0},
                                           {"GREEN", 0, 255, 0},
                                           {"RED", 255, 0, 0},
                                           {"YELLOW", 255, 255, 0},
                                           {"CYAN", 0, 255, 255},
                                           {"MAGENTA", 255, 0, 255},
                                           {"WHITE", 255, 255, 255}}};

// The class the next two rows set; kept while the game runs.
std::atomic<int> g_class{as::kRifle};

// Guest copies of the texts, made once (the game keeps the pointers).
std::mutex g_text_mutex;
std::unordered_map<std::string, uint32_t> g_guest_texts;

uint32_t FixedText(uint8_t* base, const char* key, const char* text) {
  std::lock_guard lock(g_text_mutex);
  uint32_t& addr = g_guest_texts[key];
  if (!addr) {
    const uint32_t len = static_cast<uint32_t>(std::strlen(text)) + 1;
    addr = rex::system::kernel_state()->memory()->SystemHeapAlloc(len);
    if (addr) std::memcpy(base + addr, text, len);
  }
  return addr;
}

std::string Get(const char* name) { return rex::cvar::GetFlagByName(name); }
bool GetBool(const char* name) {
  const std::string v = Get(name);
  return v == "true" || v == "1";
}
double GetDouble(const char* name) {
  const std::string v = Get(name);
  char* end = nullptr;
  const double d = std::strtod(v.c_str(), &end);
  return end != v.c_str() && std::isfinite(d) ? d : 0.0;
}
bool Set(const char* name, const std::string& value) { return rex::cvar::SetFlagByName(name, value); }

std::string Percent(int pct) { return fmt::format("{}%", pct); }
std::string Number(double v) { return fmt::format("{:.1f}", v); }

// The crosshair color as R G B A, as cod3_crosshair_color holds it.
std::array<int, 4> CurrentColor() {
  std::array<int, 4> c = {0, 255, 0, 255};
  int r, g, b, a;
  if (std::sscanf(Get("cod3_crosshair_color").c_str(), "%d %d %d %d", &r, &g, &b, &a) == 4)
    c = {std::clamp(r, 0, 255), std::clamp(g, 0, 255), std::clamp(b, 0, 255), std::clamp(a, 0, 255)};
  return c;
}

// The choice CROSSHAIR COLOR shows: 0 the game's own, 1.. a named color, -1
// a color of the player's own from F2.
int ColorChoice() {
  if (!GetBool("cod3_crosshair_constant_color")) return 0;
  const auto c = CurrentColor();
  for (int i = 1; i < int(kColors.size()); ++i)
    if (c[0] == kColors[i].r && c[1] == kColors[i].g && c[2] == kColors[i].b) return i;
  return -1;
}

bool SetColorChoice(int choice) {
  if (choice <= 0) return Set("cod3_crosshair_constant_color", "false");
  const auto c = CurrentColor();
  const Color& k = kColors[choice];
  return Set("cod3_crosshair_color", fmt::format("{} {} {} {}", k.r, k.g, k.b, c[3])) &&
         Set("cod3_crosshair_constant_color", "true");
}

bool StepSize(const Size& s, bool right) {
  const double now = GetDouble(s.setting);
  // From a value between steps (typed in F2), the first step lands on the grid.
  const double grid = right ? std::floor(now / s.step + 1e-6) + 1.0 : std::ceil(now / s.step - 1e-6) - 1.0;
  const double next = std::clamp(grid * s.step, s.lo, s.hi);
  if (std::abs(next - now) < 1e-9) return false;
  return Set(s.setting, fmt::format("{:.2f}", next));
}

bool CrosshairRow(int row) { return row >= kCrosshair && row <= kColor; }

// The row's label as shown, for the log.
const char* LabelText(int row) {
  const char* key = row >= 0 && row < kRows ? kLabelKeys[row] : "";
  for (const Text& t : kTexts)
    if (std::strcmp(key, t.key) == 0) return t.text;
  return "?";
}

}  // namespace

const char* LabelKey(int row) { return row >= 0 && row < kRows ? kLabelKeys[row] : kLabelKeys[0]; }
const char* TitleKey() { return "FEMENU_COP_AIM_TITLE"; }
const char* PauseItemKey() { return "FEMENU_COP_AIM_ITEM"; }
const char* VideoButtonKey() { return "FEMENU_COP_AIM_VIDEO_BUTTON"; }
const char* MovementButtonKey() { return "FEMENU_COP_AIM_MOVEMENT_BUTTON"; }

std::string Value(int row) {
  const int group = std::clamp(g_class.load(), 0, as::kGroupCount - 1);
  switch (row) {
    case kWeaponClass: return kClassNames[group];
    case kHip: return Percent(as::Percent(kPlayer, group, false));
    case kAiming: return Percent(as::Percent(kPlayer, group, true));
    case kCrosshair: return GetBool("cod3_crosshair") ? "ON" : "OFF";
    case kStyle: {
      const int s = std::atoi(Get("cod3_crosshair_style").c_str());
      return kStyleNames[std::clamp(s, 0, int(kStyleNames.size()) - 1)];
    }
    case kLength: return Number(GetDouble(kLengthSize.setting));
    case kGap: return Number(GetDouble(kGapSize.setting));
    case kThickness: return Number(GetDouble(kThicknessSize.setting));
    case kColor: {
      const int c = ColorChoice();
      return c < 0 ? "CUSTOM" : kColors[c].name;
    }
  }
  return {};
}

std::string Note(int row) {
  switch (row) {
    case kWeaponClass: return "EACH WEAPON CLASS HAS ITS OWN SENSITIVITY";
    case kHip:
    case kAiming: return "MOUSE AND STICK, 100% IS THE GAME'S OWN";
  }
  if (CrosshairRow(row) && row != kCrosshair && !GetBool("cod3_crosshair")) return "TURN CUSTOM CROSSHAIR ON TO SEE IT";
  switch (row) {
    case kCrosshair: return "ONE CROSSHAIR FOR EVERY WEAPON";
    case kLength:
    case kGap:
    case kThickness: return "PIXELS ON A 1080P SCREEN";
    case kColor: {
      const int c = ColorChoice();
      return c == 0 ? "AUTO: RED OVER AN ENEMY, GREEN OVER A FRIEND" : c < 0 ? "YOUR OWN COLOR FROM F2" : "";
    }
  }
  return {};
}

std::string Step(PPCContext& ctx, uint8_t* base, int row, bool right) {
  (void)ctx;
  (void)base;
  const int group = std::clamp(g_class.load(), 0, as::kGroupCount - 1);
  bool changed = false;
  switch (row) {
    case kWeaponClass:
      // Not a setting: which class the next rows show; goes round.
      g_class.store((group + (right ? 1 : as::kGroupCount - 1)) % as::kGroupCount);
      return Value(row);
    case kHip:
    case kAiming: {
      const bool aiming = row == kAiming;
      const int now = as::Percent(kPlayer, group, aiming);
      const int next = std::clamp(now + (right ? as::kMenuStep : -as::kMenuStep), as::kMinPercent, as::kMaxPercent);
      if (next == now) return "";
      as::SetPercent(kPlayer, group, aiming, next);
      changed = true;
      break;
    }
    case kCrosshair: changed = Set("cod3_crosshair", GetBool("cod3_crosshair") ? "false" : "true"); break;
    case kStyle: {
      const int n = int(kStyleNames.size());
      const int s = std::clamp(std::atoi(Get("cod3_crosshair_style").c_str()), 0, n - 1);
      changed = Set("cod3_crosshair_style", std::to_string((s + (right ? 1 : n - 1)) % n));
      break;
    }
    case kLength: changed = StepSize(kLengthSize, right); break;
    case kGap: changed = StepSize(kGapSize, right); break;
    case kThickness: changed = StepSize(kThicknessSize, right); break;
    case kColor: {
      const int n = int(kColors.size());
      const int c = ColorChoice();
      // A color of the player's own steps to the list's ends.
      const int next = c < 0 ? (right ? 1 : 0) : (c + (right ? 1 : n - 1)) % n;
      changed = SetColorChoice(next);
      break;
    }
    default: return "";
  }
  if (!changed) return "";
  protected_dvars::SaveSettings();
  const std::string v = Value(row);
  REXLOG_INFO("cod3: Options > Aim: {} = {}{}", LabelText(row), v,
              row == kHip || row == kAiming ? std::string(" (") + kClassNames[group] + ")" : std::string());
  return v;
}

bool NumberOf(int row, menuslider::Number* n, double* value) {
  // The sizes take any tenth when typed (F2's sliders do too); the arrows
  // keep their steps. The percentages take any whole number.
  auto size = [&](const Size& s) {
    *n = menuslider::Number{s.lo, s.hi, 0.1, 1.0, 1, ""};
    *value = std::clamp(GetDouble(s.setting), s.lo, s.hi);
    return true;
  };
  const int group = std::clamp(g_class.load(), 0, as::kGroupCount - 1);
  switch (row) {
    case kLength: return size(kLengthSize);
    case kGap: return size(kGapSize);
    case kThickness: return size(kThicknessSize);
    case kHip:
    case kAiming:
      *n = menuslider::Number{double(as::kMinPercent), double(as::kMaxPercent), 1.0, 1.0, 0, "%"};
      *value = as::Percent(kPlayer, group, row == kAiming);
      return true;
  }
  return false;
}

std::string SetNumber(int row, double value) {
  const int group = std::clamp(g_class.load(), 0, as::kGroupCount - 1);
  bool changed = false;
  switch (row) {
    case kLength: changed = Set(kLengthSize.setting, fmt::format("{:.2f}", value)); break;
    case kGap: changed = Set(kGapSize.setting, fmt::format("{:.2f}", value)); break;
    case kThickness: changed = Set(kThicknessSize.setting, fmt::format("{:.2f}", value)); break;
    case kHip:
    case kAiming:
      as::SetPercent(kPlayer, group, row == kAiming, int(std::lround(value)));
      changed = true;
      break;
    default: return "";
  }
  if (!changed) return "";
  protected_dvars::SaveSettings();
  const std::string v = Value(row);
  REXLOG_INFO("cod3: Options > Aim: {} = {} (typed){}", LabelText(row), v,
              row == kHip || row == kAiming ? std::string(" (") + kClassNames[group] + ")" : std::string());
  return v;
}

void ShowPreview(float x0, float y0, float x1, float y1) { ui::ShowCrosshairPreview(x0, y0, x1, y1); }

void OnOpen(uint8_t* base) {
  // The class in the player's hands, when it can be read.
  const int held = as::HeldGroup(base, kPlayer, nullptr);
  if (held >= 0 && held < as::kGroupCount && held != as::kOther) g_class.store(held);
}

uint32_t Localize(uint8_t* base, const char* key) {
  if (!key || std::strncmp(key, "FEMENU_COP_AIM_", 15) != 0) return 0;
  for (const Text& t : kTexts)
    if (std::strcmp(key, t.key) == 0) return FixedText(base, t.key, t.text);
  return 0;
}

}  // namespace cod3::aimmenu
