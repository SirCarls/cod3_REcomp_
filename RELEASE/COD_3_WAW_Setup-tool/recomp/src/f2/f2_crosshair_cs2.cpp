// f2 - Counter-Strike 2's crosshair for every game's F2 window (f2_crosshair_cs2.h). [acct2-ocr] 2026-11-14.

#include "f2/f2_crosshair_cs2.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <tuple>

#include <imgui.h>

#include <rex/cvar.h>

#include "f2/crosshair_cs2_code.h"  // [acct2-decomp] CS2's share codes
#include "ui/slider_box.h"          // Call of Duty 3's slider with a number box

namespace f2::xhair {

namespace {

constexpr float kPi = 3.14159265358979f;

// ------------------------------------------------------------------------------------------------- names

const char* const kStyleNames[kStyleCount] = {
    "Dynamic Cross", "Dynamic Circle", "Dynamic Cross (Legacy)", "Static Circle", "Static Cross",
    "Static Cross (Shot Feedback)", "Dot Only", "Dynamic Quad", "Static Square", "Static Quad"};

const char* const kGrenadeNames[kGrenadeCount] = {"Flash", "Explosive", "Fire", "Smoke", "Decoy"};

// [acct2-ocr] 2026-11-14 the special crosshairs' rows (owner 17:06 / 17:19).
const char* const kSpecialNames[kSpecialCount] = {"Weapons with a crosshair of their own", "Turrets and mounted guns",
                                                  "Vehicles", "Scopes", "Aiming mode"};
const char* const kSpecialHelp[kSpecialCount] = {
    "Weapons the game draws a crosshair of their own for, such as World at War's flamethrower.",
    "Mounted machine guns, anti-aircraft guns and gunner seats.",
    "Driving or riding a vehicle, and the vehicle's aiming marks on the HUD.",
    "A scope's picture while aiming, also a scoped turret's. None leaves the zoomed view bare.",
    "Call of Duty 3's mark at the point you aim at (most likely the mortar's)."};
const char* const kSpecialModeNames[kSpecialModeCount] = {"The game's own", "Mine", "Both", "None"};

// Which rows each style uses (CS2's settingsmenu_crosshair script, acct2-decomp's NOTES 11-07 15:56).
bool UsesLength(int st) {
  return st == kDynamicCross || st == kDynamicCrossLegacy || st == kStaticCross || st == kStaticCrossShotFeedback ||
         st == kDynamicQuad;
}
bool UsesGap(int st) { return st != kDynamicCircle && st != kDotOnly; }
bool UsesT(int st) { return UsesLength(st); }
bool UsesDot(int st) { return st != kDotOnly; }
bool UsesSpreadLimit(int st) { return st == kDynamicCross || st == kDynamicCircle || st == kDynamicQuad; }

// ------------------------------------------------------------------------------------------------- text

std::string ColorText(const Rgba& c) {
  char b[32];
  std::snprintf(b, sizeof(b), "%u,%u,%u,%u", c.r, c.g, c.b, c.a);
  return b;
}

// "r g b a" or "r,g,b,a", 0-255 each (alpha 255 when left out); false, and `out` left alone, when it can't be read.
bool ParseColor(std::string_view text, Rgba* out) {
  std::string t(text);
  for (char& ch : t)
    if (ch == ',') ch = ' ';
  int v[4] = {255, 255, 255, 255};
  if (std::sscanf(t.c_str(), "%d %d %d %d", &v[0], &v[1], &v[2], &v[3]) < 3) return false;
  for (int& x : v) x = std::clamp(x, 0, 255);
  *out = {uint8_t(v[0]), uint8_t(v[1]), uint8_t(v[2]), uint8_t(v[3])};
  return true;
}

std::string Num(double v) {
  char b[32];
  std::snprintf(b, sizeof(b), "%g", v);
  return b;
}

// ------------------------------------------------------------------------------------------------- storage

struct Store {
  bool bound = false;
  std::array<std::string, kPlayers> names;
  std::array<std::string, kPlayers> last;
  std::array<bool, kPlayers> read{};
  std::array<bool, kPlayers> follows{};  // an empty text setting: the same as player 1
  std::function<Legacy()> get_legacy;
  std::function<void(const Legacy&)> set_legacy;
  Legacy legacy_seen;  // the old settings as last read or written
  std::array<Settings, kPlayers> players;
  std::array<std::atomic<bool>, kPlayers> enabled{};  // for the game's hook, on its own thread
  std::array<std::array<std::atomic<int>, kSpecialCount>, kPlayers> special{};  // [acct2-ocr] the same, per kind
  Caps caps;
  // Shot feedback without the game's shots: a jump in the spread.
  std::array<float, kPlayers> last_spread{};
  std::array<double, kPlayers> shot_at{-1, -1, -1, -1};
};

Store& S() {
  static Store s;
  return s;
}

// ------------------------------------------------------------------------------------------------- shading

float SmoothStep(float e0, float e1, float x) {
  // GLSL's smoothstep, also for e0 > e1 (the shader uses it both ways).
  const float d = e1 - e0;
  float t = d == 0.0f ? (x < e0 ? 0.0f : 1.0f) : (x - e0) / d;
  t = std::clamp(t, 0.0f, 1.0f);
  return t * t * (3.0f - 2.0f * t);
}

float Mix(float a, float b, float t) { return a + (b - a) * t; }

void Coverage(const Shape& sh, int px, int py, float* fill_cov, float* outline_cov) {
  if (sh.kind == 0) {
    const bool in = px >= sh.x0 && py >= sh.y0 && px <= sh.x1 && py <= sh.y1;
    const bool out = px >= sh.x0 - sh.grow0 && py >= sh.y0 - sh.grow0 && px <= sh.x1 + sh.grow1 &&
                     py <= sh.y1 + sh.grow1;
    *fill_cov = in ? 1.0f : 0.0f;
    *outline_cov = out ? 1.0f : 0.0f;
    return;
  }
  // Rings and arcs: the pixel's middle against the circle, every edge smoothed over one pixel.
  const float dx = float(px) + 0.5f - sh.x0, dy = float(py) + 0.5f - sh.y0;
  const float len = std::sqrt(dx * dx + dy * dy);
  const float a = std::atan2(dx, -dy);  // 0 at the top, +pi/2 to the right
  const float h = std::max(SmoothStep(0.0f, kPi / 2, a), SmoothStep(-kPi / 2, -kPi, a));
  const float go = Mix(sh.grow0, sh.grow1, h), gi = Mix(sh.grow1, sh.grow0, h);
  const float R = sh.x1, T = sh.y1;
  float f = SmoothStep(R + 0.5f, R - 0.5f, len) * SmoothStep(R - T - 0.5f, R - T + 0.5f, len);
  float o = SmoothStep(R + go + 0.5f, R + go - 0.5f, len) * SmoothStep(R - T - gi - 0.5f, R - T - gi + 0.5f, len);
  if (sh.kind == 2) {
    const float quarter = std::floor(a * 2.0f / kPi);  // 0 top right, 1 bottom right, -1 top left, -2 bottom left
    const float q = a - quarter * (kPi / 2) - kPi / 4;
    const float ad = std::fabs(q);
    const float L = std::max(len, 1.0f);
    const float hs = sh.span * 0.5f;
    const float og = Mix(sh.grow0, sh.grow1, h);
    // [acct2-ocr] which of the four arcs are drawn (CS2's shader draws all four; the T style leaves out the top two)
    const int bit = quarter >= 0.0f ? (quarter < 0.5f ? 1 : 2) : (quarter > -1.5f ? 8 : 4);
    if (!(sh.arcs & bit)) {
      *fill_cov = 0.0f;
      *outline_cov = 0.0f;
      return;
    }
    f *= SmoothStep(hs + 0.5f / L, hs - 0.5f / L, ad);
    o *= SmoothStep(hs + (og + 0.5f) / L, hs + (og - 0.5f) / L, ad);
  }
  *fill_cov = f;
  *outline_cov = o;
}

// ------------------------------------------------------------------------------------------------- placing

int Round(float v) { return int(std::lround(v)); }

void Color4(const Rgba& c, float alpha, float out[4]) {
  out[0] = c.r / 255.0f;
  out[1] = c.g / 255.0f;
  out[2] = c.b / 255.0f;
  out[3] = c.a / 255.0f * std::clamp(alpha, 0.0f, 1.0f);
}

// The case the game's crosshair color shows (its white, red over an enemy, green over a friendly, black over a
// dead body).
int CaseOf(const float rgba[4], bool dead_color) {
  if (rgba[0] > 0.9f && rgba[1] < 0.6f && rgba[2] < 0.6f) return kCaseEnemy;
  if (rgba[1] > 0.9f && rgba[0] < 0.6f && rgba[2] < 0.6f) return kCaseFriendly;
  if (dead_color && rgba[0] < 0.1f && rgba[1] < 0.1f && rgba[2] < 0.1f) return kCaseDead;
  return kCaseNormal;
}

void FillColor(const Settings& s, const View& v, float out[4]) {
  const float fade = std::clamp(v.rgba[3], 0.0f, 1.0f);
  if (s.color_from == kColorConstant) {
    Color4(s.color, fade, out);
  } else if (s.color_from == kColorOwnPerCase) {
    Color4(s.case_color[CaseOf(v.rgba, S().caps.dead_color)], fade, out);
  } else {
    for (int i = 0; i < 4; ++i) out[i] = std::clamp(v.rgba[i], 0.0f, 1.0f);
  }
}

}  // namespace

// ------------------------------------------------------------------------------------------------- public: names

const char* StyleName(int style) { return style >= 0 && style < kStyleCount ? kStyleNames[style] : "?"; }

const char* SpecialName(int kind) { return kind >= 0 && kind < kSpecialCount ? kSpecialNames[kind] : "?"; }

bool StyleIsDynamic(int style) {
  return style == kDynamicCross || style == kDynamicCircle || style == kDynamicCrossLegacy || style == kDotOnly ||
         style == kDynamicQuad;
}

// ------------------------------------------------------------------------------------------------- public: text

std::string Text(const Settings& s) {
  std::string t = "v=1";
  auto add = [&](const char* k, const std::string& v) {
    t += ' ';
    t += k;
    t += '=';
    t += v;
  };
  auto b = [](bool v) { return std::string(v ? "1" : "0"); };
  add("on", b(s.enabled));
  add("style", std::to_string(s.style));
  add("length", Num(s.length));
  add("thickness", Num(s.thickness));
  add("gap", Num(s.gap));
  add("dot", b(s.dot));
  add("t", b(s.t_style));
  add("color", ColorText(s.color));
  add("outline", std::to_string(s.outline));
  add("outline_color", ColorText(s.outline_color));
  add("outline_px", Num(s.outline_thickness));
  add("spread_limit", std::to_string(s.spread_limit));
  add("split_distance", std::to_string(s.split_distance));
  add("split_inner", Num(s.split_inner_alpha));
  add("split_outer", Num(s.split_outer_alpha));
  add("split_ratio", Num(s.split_ratio));
  add("recoil", b(s.follow_recoil));
  add("ads_dot", b(s.ads_dot));
  add("ads_dot_color", b(s.ironsight_use_color));
  add("ads_dot_scale", Num(s.ironsight_dot_scale));
  add("screen_height", std::to_string(s.screen_height));
  add("friendly_warning", b(s.friendly_warning));
  add("observer", b(s.observer));
  add("observed_bot", b(s.observed_bot));
  add("grenade_keep", b(s.grenade_keep_mine));
  std::string g;
  for (int i = 0; i < kGrenadeCount; ++i) {
    if (i) g += ',';
    g += (s.grenade_on[i] ? "1:" : "0:") + Num(s.grenade_delay[i]);
  }
  add("grenades", g);
  add("sniper_unscope_delay", b(s.sniper_delay_unscope));
  add("sniper_inaccuracy", b(s.sniper_show_inaccuracy));
  add("sniper_rezoom", b(s.sniper_auto_rezoom));
  add("sniper_width", std::to_string(s.sniper_width));
  add("color_from", std::to_string(s.color_from));
  add("color_normal", ColorText(s.case_color[kCaseNormal]));
  add("color_enemy", ColorText(s.case_color[kCaseEnemy]));
  add("color_friendly", ColorText(s.case_color[kCaseFriendly]));
  add("color_dead", ColorText(s.case_color[kCaseDead]));
  add("open_scale", Num(s.open_scale));
  {
    std::string sp;
    for (int i = 0; i < kSpecialCount; ++i) {
      if (i) sp += ',';
      sp += std::to_string(s.special[i]);
    }
    add("special", sp);
  }
  if (s.legacy_hash) {
    char b[24];
    std::snprintf(b, sizeof(b), "%016llx", static_cast<unsigned long long>(s.legacy_hash));
    add("legacy", b);
  }
  return t;
}

Settings FromText(std::string_view text) {
  Settings s;
  size_t i = 0;
  while (i < text.size()) {
    while (i < text.size() && text[i] == ' ') ++i;
    size_t end = text.find(' ', i);
    if (end == std::string_view::npos) end = text.size();
    const std::string_view pair = text.substr(i, end - i);
    i = end;
    const size_t eq = pair.find('=');
    if (eq == std::string_view::npos) continue;
    const std::string key(pair.substr(0, eq));
    const std::string val(pair.substr(eq + 1));
    const double d = std::strtod(val.c_str(), nullptr);
    const int n = int(std::lround(d));
    const bool on = val == "1" || val == "true";
    auto f = [&](float lo, float hi) { return float(std::clamp(d, double(lo), double(hi))); };
    auto in = [&](int lo, int hi) { return std::clamp(n, lo, hi); };
    if (key == "on") s.enabled = on;
    else if (key == "style") s.style = in(0, kStyleCount - 1);
    else if (key == "length") s.length = f(0, 255);
    else if (key == "thickness") s.thickness = f(0, 32);
    else if (key == "gap") s.gap = f(-10, 128);
    else if (key == "dot") s.dot = on;
    else if (key == "t") s.t_style = on;
    else if (key == "color") ParseColor(val, &s.color);
    else if (key == "outline") s.outline = in(0, 2);
    else if (key == "outline_color") ParseColor(val, &s.outline_color);
    else if (key == "outline_px") s.outline_thickness = f(1, 4);
    else if (key == "spread_limit") s.spread_limit = in(0, 255);
    else if (key == "split_distance") s.split_distance = in(0, 127);
    else if (key == "split_inner") s.split_inner_alpha = f(0, 1);
    else if (key == "split_outer") s.split_outer_alpha = f(0.3f, 1);
    else if (key == "split_ratio") s.split_ratio = f(0, 1);
    else if (key == "recoil") s.follow_recoil = on;
    else if (key == "ads_dot") s.ads_dot = on;
    else if (key == "ads_dot_color") s.ironsight_use_color = on;
    else if (key == "ads_dot_scale") s.ironsight_dot_scale = f(0.1f, 2);
    else if (key == "screen_height") s.screen_height = in(0, 8640);
    else if (key == "friendly_warning") s.friendly_warning = on;
    else if (key == "observer") s.observer = on;
    else if (key == "observed_bot") s.observed_bot = on;
    else if (key == "grenade_keep") s.grenade_keep_mine = on;
    else if (key == "grenades") {
      size_t j = 0;
      for (int k = 0; k < kGrenadeCount && j < val.size(); ++k) {
        size_t comma = val.find(',', j);
        if (comma == std::string::npos) comma = val.size();
        const std::string item = val.substr(j, comma - j);
        const size_t colon = item.find(':');
        s.grenade_on[k] = !item.empty() && item[0] == '1';
        if (colon != std::string::npos)
          s.grenade_delay[k] = float(std::clamp(std::strtod(item.c_str() + colon + 1, nullptr), 0.0, 5.0));
        j = comma + 1;
      }
    } else if (key == "sniper_unscope_delay") s.sniper_delay_unscope = on;
    else if (key == "sniper_inaccuracy") s.sniper_show_inaccuracy = on;
    else if (key == "sniper_rezoom") s.sniper_auto_rezoom = on;
    else if (key == "sniper_width") s.sniper_width = in(1, 6);
    else if (key == "color_from") s.color_from = in(0, 2);
    else if (key == "color_normal") ParseColor(val, &s.case_color[kCaseNormal]);
    else if (key == "color_enemy") ParseColor(val, &s.case_color[kCaseEnemy]);
    else if (key == "color_friendly") ParseColor(val, &s.case_color[kCaseFriendly]);
    else if (key == "color_dead") ParseColor(val, &s.case_color[kCaseDead]);
    else if (key == "open_scale") s.open_scale = f(0, 3);
    else if (key == "special") {  // "0,1,0,2,0": one SpecialMode per kind, in kind order
      size_t j = 0;
      for (int k = 0; k < kSpecialCount && j < val.size(); ++k) {
        size_t comma = val.find(',', j);
        if (comma == std::string::npos) comma = val.size();
        const int m = std::atoi(val.substr(j, comma - j).c_str());
        s.special[k] = m >= 0 && m < kSpecialModeCount ? m : kGameOwn;
        j = comma + 1;
      }
    }
    else if (key == "legacy") s.legacy_hash = std::strtoull(val.c_str(), nullptr, 16);
  }
  return s;
}

// ------------------------------------------------------------------------------------------------- public: codes

bool ImportCode(std::string_view code, Settings* s, std::string* why) {
  namespace cs2 = cod3::crosshair_cs2;
  const std::optional<cs2::Code> c = cs2::Decode(code, why);
  if (!c) return false;
  s->style = std::min<int>(c->style, kStyleCount - 1);
  s->dot = c->center_dot;
  s->t_style = c->t_style;
  s->color = {c->color[0], c->color[1], c->color[2], c->color[3]};
  s->outline_color = {c->outline_color[0], c->outline_color[1], c->outline_color[2], c->outline_color[3]};
  s->thickness = float(std::min<int>(c->thickness, 32));
  s->outline = std::min<int>(c->outline_mode, 2);
  s->gap = float(std::max<int>(c->gap, -10));
  s->length = float(c->length);
  s->spread_limit = c->dynamic_spread_limit;
  s->split_distance = c->split_distance;
  s->split_inner_alpha = std::min(1.0f, c->InnerSplitAlpha());
  s->split_outer_alpha = std::clamp(c->OuterSplitAlpha(), 0.3f, 1.0f);
  s->split_ratio = std::min(1.0f, c->SplitSizeRatio());
  s->ironsight_use_color = c->scope_dot_use_crosshair_color;
  s->ironsight_dot_scale = std::clamp(c->ScopeDotScale(), 0.1f, 2.0f);
  s->screen_height = c->screen_height ? c->screen_height : 1080;
  // As CS2: "Crosshair settings were imported, disabling cl_crosshair_recoil".
  s->follow_recoil = false;
  // A CS2 crosshair is one color with a one-pixel outline: shown as in CS2.
  s->color_from = kColorConstant;
  s->outline_thickness = 1.0f;
  s->enabled = true;
  if (why) why->clear();
  return true;
}

std::string ExportCode(const Settings& s) {
  namespace cs2 = cod3::crosshair_cs2;
  cs2::Code c;
  c.screen_height = uint16_t(std::clamp(s.screen_height, 0, 65535));
  c.style = uint8_t(std::clamp(s.style, 0, kStyleCount - 1));
  c.follow_recoil = s.follow_recoil;
  c.center_dot = s.dot;
  c.t_style = s.t_style;
  const uint8_t col[4] = {s.color.r, s.color.g, s.color.b, s.color.a};
  const uint8_t oc[4] = {s.outline_color.r, s.outline_color.g, s.outline_color.b, s.outline_color.a};
  std::memcpy(c.color, col, 4);
  std::memcpy(c.outline_color, oc, 4);
  c.thickness = uint8_t(std::clamp(Round(s.thickness), 0, 255));
  c.outline_mode = uint8_t(std::clamp(s.outline, 0, 2));
  c.gap = int8_t(std::clamp(Round(s.gap), -128, 127));
  c.length = uint8_t(std::clamp(Round(s.length), 0, 255));
  c.dynamic_spread_limit = uint8_t(std::clamp(s.spread_limit, 0, 255));
  c.split_distance = uint8_t(std::clamp(s.split_distance, 0, 127));
  c.inner_split_alpha = uint8_t(std::clamp(Round(s.split_inner_alpha * 100.0f), 0, 127));
  c.outer_split_alpha = uint8_t(std::clamp(Round((s.split_outer_alpha - 0.3f) * 100.0f), 0, 127));
  c.split_size_ratio = uint8_t(std::clamp(Round(s.split_ratio * 100.0f), 0, 127));
  c.scope_dot_use_crosshair_color = s.ironsight_use_color;
  c.scope_dot_scale = uint8_t(std::clamp(Round(s.ironsight_dot_scale * 100.0f) - 10, 0, 255));
  return cs2::Encode(c);
}

// ------------------------------------------------------------------------------------------------- public: legacy

Settings FromLegacy(const Legacy& l) {
  Settings s;
  s.enabled = l.enabled;
  s.length = float(l.size);
  s.thickness = float(l.thickness);
  s.gap = float(l.gap);
  s.dot = l.dot;
  const bool dyn = l.dynamic == 1, split = l.dynamic == 2;
  switch (l.shape) {
    case 1:  // cross with a dot
      s.dot = true;
      [[fallthrough]];
    case 0:  // cross
    default:
      s.style = split ? kDynamicCrossLegacy : dyn ? kDynamicCross : kStaticCross;
      break;
    case 2:
      s.style = kDotOnly;
      break;
    case 3:  // the old circle's line ran through gap + size / 2; a CS2 circle starts at the gap
      s.style = (dyn || split) ? kDynamicCircle : kStaticCircle;
      s.gap = float(std::max(0.0, l.gap + l.size * 0.5 - l.thickness * 0.5));
      break;
    case 4:
      s.style = split ? kDynamicCrossLegacy : dyn ? kDynamicCross : kStaticCross;
      s.t_style = true;
      break;
  }
  s.outline = l.outline ? kOutlineFull : kOutlineNone;
  s.outline_thickness = float(std::clamp(l.outline_thickness, 1.0, 4.0));
  s.color_from = l.constant_color ? kColorConstant : l.own_colors ? kColorOwnPerCase : kColorGame;
  ParseColor(l.color, &s.color);
  for (int i = 0; i < kCaseCount; ++i) ParseColor(l.colors[i], &s.case_color[i]);
  s.open_scale = float(std::clamp(l.dynamic_scale, 0.0, 3.0));
  s.split_distance = std::clamp(int(std::lround(l.split_distance)), 0, 127);
  s.split_ratio = float(std::clamp(l.split_ratio, 0.0, 1.0));
  s.split_inner_alpha = float(std::clamp(l.split_alpha_inner, 0.0, 1.0));
  s.split_outer_alpha = float(std::clamp(l.split_alpha_outer, 0.3, 1.0));
  return s;
}

Legacy ToLegacy(const Settings& s) {
  Legacy l;
  const bool circle = s.style == kDynamicCircle || s.style == kStaticCircle;
  l.enabled = s.enabled;
  l.shape = s.style == kDotOnly ? 2 : circle ? 3 : s.t_style ? 4 : s.dot ? 1 : 0;
  l.dynamic = s.style == kDynamicCrossLegacy ? 2 : (StyleIsDynamic(s.style) && s.style != kDotOnly) ? 1 : 0;
  l.size = s.length;
  l.thickness = s.thickness;
  l.gap = circle ? s.gap - s.length * 0.5 + s.thickness * 0.5 : s.gap;
  l.outline = s.outline != kOutlineNone;
  l.outline_thickness = s.outline_thickness;
  l.dot = s.dot;
  l.constant_color = s.color_from == kColorConstant;
  l.own_colors = s.color_from == kColorOwnPerCase;
  auto text = [](const Rgba& c) {
    char b[32];
    std::snprintf(b, sizeof(b), "%u %u %u %u", c.r, c.g, c.b, c.a);
    return std::string(b);
  };
  l.color = text(s.color);
  for (int i = 0; i < kCaseCount; ++i) l.colors[i] = text(s.case_color[i]);
  l.dynamic_scale = s.open_scale;
  l.split_distance = s.split_distance;
  l.split_ratio = s.split_ratio;
  l.split_alpha_inner = s.split_inner_alpha;
  l.split_alpha_outer = s.split_outer_alpha;
  return l;
}

// ------------------------------------------------------------------------------------------------- public: storage

namespace {

bool Differs(double a, double b) { return std::fabs(a - b) > 1e-4; }

bool ColorDiffers(const std::string& a, const std::string& b) {
  Rgba x{}, y{};
  const bool ok_a = ParseColor(a, &x), ok_b = ParseColor(b, &y);
  return ok_a != ok_b || !(x == y);
}

bool LegacyDiffers(const Legacy& a, const Legacy& b) {
  if (a.enabled != b.enabled || a.shape != b.shape || a.outline != b.outline || a.dot != b.dot ||
      a.constant_color != b.constant_color || a.own_colors != b.own_colors || a.dynamic != b.dynamic)
    return true;
  if (Differs(a.size, b.size) || Differs(a.thickness, b.thickness) || Differs(a.gap, b.gap) ||
      Differs(a.outline_thickness, b.outline_thickness) || Differs(a.dynamic_scale, b.dynamic_scale) ||
      Differs(a.split_distance, b.split_distance) || Differs(a.split_ratio, b.split_ratio) ||
      Differs(a.split_alpha_inner, b.split_alpha_inner) || Differs(a.split_alpha_outer, b.split_alpha_outer))
    return true;
  if (ColorDiffers(a.color, b.color)) return true;
  for (int i = 0; i < kCaseCount; ++i)
    if (ColorDiffers(a.colors[i], b.colors[i])) return true;
  return false;
}

// FNV-1a over the old settings' values (numbers to 4 places, colors as read), for Settings::legacy_hash.
uint64_t LegacyHash(const Legacy& l) {
  char b[512];
  Rgba c[1 + kCaseCount];
  ParseColor(l.color, &c[0]);
  for (int i = 0; i < kCaseCount; ++i) ParseColor(l.colors[i], &c[1 + i]);
  int n = std::snprintf(b, sizeof(b), "%d %d %d %d %d %d %d %d %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f %.4f", l.enabled,
                        l.shape, l.outline, l.dot, l.constant_color, l.own_colors, l.dynamic, 0, l.size, l.thickness,
                        l.gap, l.outline_thickness, l.dynamic_scale, l.split_distance, l.split_ratio,
                        l.split_alpha_inner, l.split_alpha_outer);
  for (const Rgba& k : c)
    n += std::snprintf(b + n, sizeof(b) - size_t(n), " %u,%u,%u,%u", k.r, k.g, k.b, k.a);
  uint64_t h = 1469598103934665603ull;
  for (int i = 0; i < n && i < int(sizeof(b)); ++i) h = (h ^ uint8_t(b[i])) * 1099511628211ull;
  return h ? h : 1;
}

// Player 1's crosshair after the old settings went from `was` to `now` elsewhere: only what changed there.
Settings MergeLegacy(Settings p, const Legacy& was, const Legacy& now) {
  const Settings n = FromLegacy(now);
  if (now.enabled != was.enabled) p.enabled = n.enabled;
  const bool shape = now.shape != was.shape || now.dynamic != was.dynamic;
  if (shape) {
    p.style = n.style;
    p.t_style = n.t_style;
    p.dot = n.dot;
  }
  if (Differs(now.size, was.size)) p.length = n.length;
  if (Differs(now.thickness, was.thickness)) p.thickness = n.thickness;
  if (shape || Differs(now.gap, was.gap)) p.gap = n.gap;
  if (now.outline != was.outline) p.outline = n.outline;
  if (Differs(now.outline_thickness, was.outline_thickness)) p.outline_thickness = n.outline_thickness;
  if (now.dot != was.dot) p.dot = n.dot;
  if (now.constant_color != was.constant_color || now.own_colors != was.own_colors) p.color_from = n.color_from;
  if (ColorDiffers(now.color, was.color)) p.color = n.color;
  for (int i = 0; i < kCaseCount; ++i)
    if (ColorDiffers(now.colors[i], was.colors[i])) p.case_color[i] = n.case_color[i];
  if (Differs(now.dynamic_scale, was.dynamic_scale)) p.open_scale = n.open_scale;
  if (Differs(now.split_distance, was.split_distance)) p.split_distance = n.split_distance;
  if (Differs(now.split_ratio, was.split_ratio)) p.split_ratio = n.split_ratio;
  if (Differs(now.split_alpha_inner, was.split_alpha_inner)) p.split_inner_alpha = n.split_inner_alpha;
  if (Differs(now.split_alpha_outer, was.split_alpha_outer)) p.split_outer_alpha = n.split_outer_alpha;
  return p;
}

bool HasSetting(const std::string& name) { return !name.empty() && rex::cvar::GetFlagInfo(name) != nullptr; }

void PublishEnabled() {
  Store& st = S();
  for (int p = 0; p < kPlayers; ++p) {
    st.enabled[p].store(st.players[p].enabled, std::memory_order_relaxed);
    for (int k = 0; k < kSpecialCount; ++k) st.special[p][k].store(st.players[p].special[k], std::memory_order_relaxed);
  }
}

// Writes player `p`'s crosshair into its text setting, and player 1's into the old names too.
void Write(int p) {
  Store& st = S();
  if (p == 0 && st.bound && st.set_legacy) {
    st.set_legacy(ToLegacy(st.players[0]));
    if (st.get_legacy) {
      st.legacy_seen = st.get_legacy();
      st.players[0].legacy_hash = LegacyHash(st.legacy_seen);
    }
  }
  const std::string text = Text(st.players[p]);
  st.last[p] = text;
  st.read[p] = true;
  st.follows[p] = false;
  if (st.bound && HasSetting(st.names[p])) rex::cvar::SetFlagByName(st.names[p], text);
}

// A player back to "the same as player 1" (an empty text setting).
void FollowPlayer1(int player) {
  Store& st = S();
  if (player <= 0 || player >= kPlayers) return;
  st.follows[player] = true;
  st.players[player] = st.players[0];
  st.last[player].clear();
  if (st.bound && HasSetting(st.names[player])) rex::cvar::SetFlagByName(st.names[player], "");
  PublishEnabled();
}

}  // namespace

void Bind(const std::array<const char*, kPlayers>& names, std::function<Legacy()> get_legacy,
          std::function<void(const Legacy&)> set_legacy) {
  Store& st = S();
  for (int p = 0; p < kPlayers; ++p) {
    st.names[p] = names[p] ? names[p] : "";
    st.read[p] = false;
  }
  st.get_legacy = std::move(get_legacy);
  st.set_legacy = std::move(set_legacy);
  st.legacy_seen = st.get_legacy ? st.get_legacy() : Legacy{};
  st.bound = true;
  // The old names changed while the game was closed (an in-game menu saved them, the file was edited): player 1's
  // crosshair then takes in what differs from it, at the first Sync.
  const std::string first = HasSetting(st.names[0]) ? rex::cvar::GetFlagByName(st.names[0]) : std::string();
  if (!first.empty() && st.get_legacy) {
    const Settings p = FromText(first);
    if (p.legacy_hash != 0 && p.legacy_hash != LegacyHash(st.legacy_seen)) st.legacy_seen = ToLegacy(p);
  }
  Sync();
}

void Sync() {
  Store& st = S();
  if (!st.bound) return;
  for (int p = 0; p < kPlayers; ++p) {
    const std::string text = HasSetting(st.names[p]) ? rex::cvar::GetFlagByName(st.names[p]) : std::string();
    if (text.empty()) {
      // Player 1 from the old settings (read every time, so the old names still work from the console or an
      // in-game menu); players 2 to 4 the same as player 1 until they are given their own.
      st.follows[p] = p > 0;
      if (p == 0) {
        st.legacy_seen = st.get_legacy ? st.get_legacy() : Legacy{};
        st.players[0] = st.get_legacy ? FromLegacy(st.legacy_seen) : Settings{};
      } else {
        st.players[p] = st.players[0];
      }
      st.last[p].clear();
      st.read[p] = true;
      continue;
    }
    st.follows[p] = false;
    if (!st.read[p] || text != st.last[p]) {
      st.players[p] = FromText(text);
      st.last[p] = text;
      st.read[p] = true;
    }
    if (p == 0 && st.get_legacy) {
      // The old names changed elsewhere (Call of Duty 3's AIM page, the console): into player 1's crosshair.
      const Legacy now = st.get_legacy();
      if (LegacyDiffers(now, st.legacy_seen)) {
        st.players[0] = MergeLegacy(st.players[0], st.legacy_seen, now);
        st.legacy_seen = now;
        Write(0);
      }
    }
  }
  PublishEnabled();
}

const Settings& Player(int player) { return S().players[std::clamp(player, 0, kPlayers - 1)]; }

void SetPlayer(int player, const Settings& s) {
  Store& st = S();
  player = std::clamp(player, 0, kPlayers - 1);
  st.players[player] = s;
  Write(player);
  if (player == 0)
    for (int p = 1; p < kPlayers; ++p)
      if (st.follows[p]) st.players[p] = s;
  PublishEnabled();
}

bool Enabled(int player) {
  if (player < 0 || player >= kPlayers) return false;
  return S().enabled[player].load(std::memory_order_relaxed);
}

Plan PlanFor(int player, int kind) {
  Plan plan;  // the game's own
  if (!Enabled(player)) return plan;
  if (kind < 0 || kind >= kSpecialCount) return {false, true};  // the hip-fire crosshair: this player's instead
  switch (S().special[player][kind].load(std::memory_order_relaxed)) {
    case kMine: return {false, true};
    case kBoth: return {true, true};
    case kNone: return {false, false};
    default: return plan;
  }
}

void SetCaps(const Caps& caps) { S().caps = caps; }
const Caps& GetCaps() { return S().caps; }

// ------------------------------------------------------------------------------------------------- public: placing

std::vector<Shape> Place(const Settings& s, const View& v, double now, int player) {
  (void)now;
  (void)player;
  std::vector<Shape> out;
  const Caps& caps = S().caps;
  const float ref = float(s.screen_height > 0 ? s.screen_height : 1080);
  const float scale = std::max(0.25f, v.height / ref);

  float fill[4];
  FillColor(s, v, fill);
  float outline[4] = {0, 0, 0, 0};
  float grow = 0.0f;
  if (s.outline != kOutlineNone) {
    Color4(s.outline_color, std::clamp(v.rgba[3], 0.0f, 1.0f), outline);
    grow = float(std::max(1, Round(s.outline_thickness)));
  }
  const float grow0 = grow, grow1 = s.outline == kOutlineHalf ? 0.0f : grow;

  int cx = int(std::floor(v.cx)), cy = int(std::floor(v.cy));
  if (s.follow_recoil && caps.recoil) {
    cx += Round(v.recoil_x);
    cy += Round(v.recoil_y);
  }
  const int t = std::max(1, Round(s.thickness * scale));
  const int len = std::max(0, Round(s.length * scale));
  const int gap = Round(s.gap * scale);
  const int h0 = t / 2;

  // The game's spread for the dynamic styles, in pixels of this view.
  int spread = 0;
  if (StyleIsDynamic(s.style) && caps.spread) {
    float px = std::max(0.0f, v.spread) * v.height / 480.0f * s.open_scale;
    if (UsesSpreadLimit(s.style) && s.spread_limit > 0) px = std::min(px, float(s.spread_limit) * scale);
    spread = Round(px);
  }

  auto rect = [&](int x0, int y0, int x1, int y1, float alpha) {
    if (x1 < x0 || y1 < y0) return;
    Shape sh;
    sh.kind = 0;
    sh.x0 = float(x0);
    sh.y0 = float(y0);
    sh.x1 = float(x1);
    sh.y1 = float(y1);
    sh.grow0 = grow0;
    sh.grow1 = grow1;
    for (int i = 0; i < 4; ++i) {
      sh.fill[i] = fill[i];
      sh.outline[i] = outline[i];
    }
    sh.fill[3] *= alpha;
    sh.outline[3] *= alpha;
    out.push_back(sh);
  };
  // The four lines, `d` pixels out from the middle, `l` long.
  auto bars = [&](int d, int l, float alpha) {
    if (l <= 0) return;
    rect(cx + d, cy - h0, cx + d + l - 1, cy - h0 + t - 1, alpha);  // right
    rect(cx - d - l, cy - h0, cx - d - 1, cy - h0 + t - 1, alpha);  // left
    rect(cx - h0, cy + d, cx - h0 + t - 1, cy + d + l - 1, alpha);  // down
    if (!s.t_style) rect(cx - h0, cy - d - l, cx - h0 + t - 1, cy - d - 1, alpha);  // up
  };
  const float rc = (t % 2) ? 0.5f : 0.0f;  // a ring's middle where the lines' middle is
  auto ring = [&](int kind, float inner, float span) {
    Shape sh;
    sh.kind = kind;
    if (kind == 2 && s.t_style) sh.arcs = 2 | 4;  // the bottom two
    sh.x0 = float(cx) + rc;
    sh.y0 = float(cy) + rc;
    sh.y1 = float(t);
    sh.x1 = std::max(inner, 0.0f) + float(t);
    sh.span = span;
    sh.grow0 = grow0;
    sh.grow1 = grow1;
    for (int i = 0; i < 4; ++i) {
      sh.fill[i] = fill[i];
      sh.outline[i] = outline[i];
    }
    out.push_back(sh);
  };

  // While a grenade is out: CS2 keeps the crosshair only when asked to.
  const bool grenade_out = caps.grenades && v.grenade >= 0 && v.grenade < kGrenadeCount;
  const bool draw_main = !grenade_out || s.grenade_keep_mine;
  const bool watching_hidden = caps.spectating && v.watching && (!s.observer || (v.watching_bot && !s.observed_bot));

  if (draw_main && !watching_hidden) {
    switch (s.style) {
      case kDynamicCross:
        bars(gap + spread, len, 1.0f);
        break;
      case kDynamicCrossLegacy: {
        // CS:GO's Classic Dynamic: past the split distance the inner part of each line stays and the outer part
        // goes on with the spread, each with its own alpha.
        const int split_at = Round(float(s.split_distance) * scale);
        if (spread > split_at) {
          const int inner = Round(float(len) * (1.0f - s.split_ratio));
          const int outer = len - inner;
          bars(gap + split_at, inner, s.split_inner_alpha);
          bars(gap + spread + inner, outer, s.split_outer_alpha);
        } else {
          bars(gap + spread, len, 1.0f);
        }
        break;
      }
      case kStaticCross:
        bars(gap, len, 1.0f);
        break;
      case kStaticCrossShotFeedback: {
        // The lines jump out at a shot and come back within 0.15 s.
        int push = 0;
        if (v.seconds_since_shot >= 0.0 && v.seconds_since_shot < 0.15)
          push = Round(4.0f * scale * float(1.0 - v.seconds_since_shot / 0.15));
        bars(gap + push, len, 1.0f);
        break;
      }
      case kDotOnly:
        break;  // the dot below
      case kDynamicCircle:
        ring(1, std::max(float(Round(3.0f * scale)), float(spread)), 0.0f);
        break;
      case kStaticCircle:
        ring(1, float(gap), 0.0f);
        break;
      case kDynamicQuad: {
        // Four arcs on the diagonals, `length` long along the circle.
        const float inner = float(std::max(0, gap + spread));
        const float mid = inner + float(t) * 0.5f;
        if (len > 0) ring(2, inner, std::min(kPi / 2, float(len) / std::max(1.0f, mid)));
        break;
      }
      case kStaticQuad:
        ring(2, float(std::max(0, gap)), (kPi / 2) * (1.0f - std::clamp(s.split_ratio, 0.0f, 1.0f)));
        break;
      case kStaticSquare: {
        const int hs = std::max(0, gap) + t;
        rect(cx - hs, cy - hs, cx + hs - 1, cy - hs + t - 1, 1.0f);              // top
        rect(cx - hs, cy + hs - t, cx + hs - 1, cy + hs - 1, 1.0f);              // bottom
        rect(cx - hs, cy - hs + t, cx - hs + t - 1, cy + hs - t - 1, 1.0f);      // left
        rect(cx + hs - t, cy - hs + t, cx + hs - 1, cy + hs - t - 1, 1.0f);      // right
        break;
      }
      default:
        bars(gap, len, 1.0f);
        break;
    }
    if (s.dot || s.style == kDotOnly) rect(cx - h0, cy - h0, cx - h0 + t - 1, cy - h0 + t - 1, 1.0f);
  }

  // The iron sight dot while the sights are up.
  if (s.ads_dot && caps.sights && v.sights >= 0.5f) {
    const int d = std::max(2, Round(3.0f * scale * s.ironsight_dot_scale));
    float dot[4] = {1.0f, 0.25f, 0.25f, 1.0f};
    if (s.ironsight_use_color) {
      FillColor(s, v, dot);
      dot[3] = s.color_from == kColorConstant ? s.color.a / 255.0f : 1.0f;
    }
    Shape sh;
    sh.x0 = float(cx - d / 2);
    sh.y0 = float(cy - d / 2);
    sh.x1 = float(cx - d / 2 + d - 1);
    sh.y1 = float(cy - d / 2 + d - 1);
    for (int i = 0; i < 4; ++i) sh.fill[i] = dot[i];
    out.push_back(sh);
  }

  // Friendly warning: the port's own mark (a ring with a bar) above the crosshair while a friendly is under it.
  const bool friendly = v.over_friendly || CaseOf(v.rgba, caps.dead_color) == kCaseFriendly;
  if (s.friendly_warning && friendly && draw_main) {
    const int r = std::max(3, Round(5.0f * scale));
    const int w = std::max(1, Round(1.5f * scale));
    const int my = cy - std::max(gap + len, 0) - r - Round(6.0f * scale);
    float col[4];
    Color4(s.case_color[kCaseFriendly], 1.0f, col);
    Shape sh;
    sh.kind = 1;
    sh.x0 = float(cx) + rc;
    sh.y0 = float(my) + 0.5f;
    sh.x1 = float(r);
    sh.y1 = float(w);
    for (int i = 0; i < 4; ++i) sh.fill[i] = col[i];
    out.push_back(sh);
    Shape bar;
    bar.x0 = float(cx - r + w);
    bar.x1 = float(cx + r - w - 1);
    bar.y0 = float(my - w / 2);
    bar.y1 = float(my - w / 2 + w - 1);
    for (int i = 0; i < 4; ++i) bar.fill[i] = col[i];
    out.push_back(bar);
  }

  // The grenade crosshair: a line-up mark under the middle once the grenade has been out for its delay.
  if (grenade_out && s.grenade_on[v.grenade] && v.grenade_seconds >= s.grenade_delay[v.grenade]) {
    const int step = std::max(4, len > 0 ? len : Round(6.0f * scale));
    const int top = cy + std::max(gap, 0) + 1;
    rect(cx - h0, top, cx - h0 + t - 1, top + 3 * step, 1.0f);
    for (int k = 1; k <= 3; ++k) rect(cx - h0 - step / 2, top + k * step - h0, cx - h0 + t - 1 + step / 2,
                                      top + k * step - h0 + t - 1, 1.0f);
  }

  // The sniper's inaccuracy while scoped: a thin ring the size of the spread.
  if (caps.scope && v.scoped && s.sniper_show_inaccuracy && caps.spread) {
    const float r = std::max(2.0f, std::max(0.0f, v.spread) * v.height / 480.0f);
    Shape sh;
    sh.kind = 1;
    sh.x0 = float(cx) + 0.5f;
    sh.y0 = float(cy) + 0.5f;
    sh.x1 = r;
    sh.y1 = float(std::max(1, s.sniper_width));
    for (int i = 0; i < 3; ++i) sh.fill[i] = 0.0f;
    sh.fill[3] = 0.5f;
    out.push_back(sh);
  }
  return out;
}

void Shade(const std::vector<Shape>& shapes, int px, int py, float out[4]) {
  float F[4] = {0, 0, 0, 0}, O[4] = {0, 0, 0, 0};
  for (const Shape& sh : shapes) {
    float fc = 0, oc = 0;
    Coverage(sh, px, py, &fc, &oc);
    const float fa = fc * sh.fill[3];
    if (fa >= F[3]) {
      F[0] = sh.fill[0];
      F[1] = sh.fill[1];
      F[2] = sh.fill[2];
      F[3] = fa;
    }
    const float oa = oc * sh.outline[3];
    if (oa >= O[3]) {
      O[0] = sh.outline[0];
      O[1] = sh.outline[1];
      O[2] = sh.outline[2];
      O[3] = oa;
    }
  }
  // CS2's mix (SHADER.md "Mixing"): the outline counts with weight O * (1 - F)^2.
  const float k = (1.0f - F[3]) * O[3];
  for (int i = 0; i < 3; ++i) out[i] = O[i] * k * (1.0f - F[3]) + F[i] * F[3];
  out[3] = k * (1.0f - F[3]) + F[3];
}

void Rasterize(const std::vector<Shape>& shapes, std::vector<Run>* runs) {
  runs->clear();
  if (shapes.empty()) return;
  float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f;
  for (const Shape& sh : shapes) {
    const float g = std::max(sh.grow0, sh.grow1) + 2.0f;
    if (sh.kind == 0) {
      x0 = std::min(x0, sh.x0 - g);
      y0 = std::min(y0, sh.y0 - g);
      x1 = std::max(x1, sh.x1 + g);
      y1 = std::max(y1, sh.y1 + g);
    } else {
      x0 = std::min(x0, sh.x0 - sh.x1 - g);
      y0 = std::min(y0, sh.y0 - sh.x1 - g);
      x1 = std::max(x1, sh.x0 + sh.x1 + g);
      y1 = std::max(y1, sh.y0 + sh.x1 + g);
    }
  }
  // At most 1024 pixels each way around the middle of the box.
  const int bx0 = int(std::floor(x0)), by0 = int(std::floor(y0));
  int bx1 = int(std::ceil(x1)), by1 = int(std::ceil(y1));
  bx1 = std::min(bx1, bx0 + 1023);
  by1 = std::min(by1, by0 + 1023);

  // Runs of one color along each row, then runs that repeat on the next row become one rectangle.
  std::map<std::tuple<int, int, uint32_t>, size_t> open;  // (x, w, color) -> index in *runs of a rectangle open
  std::map<std::tuple<int, int, uint32_t>, size_t> next;
  for (int y = by0; y <= by1; ++y) {
    next.clear();
    bool in_run = false;  // (a run can start left of the window: x below 0)
    int start = 0;
    uint32_t color = 0;
    auto flush = [&](int end_x) {
      if (!in_run) return;
      const auto key = std::make_tuple(start, end_x - start, color);
      auto it = open.find(key);
      if (it != open.end() && (*runs)[it->second].y + (*runs)[it->second].h == y) {
        (*runs)[it->second].h += 1;
        next[key] = it->second;
      } else {
        runs->push_back({start, y, end_x - start, 1, color});
        next[key] = runs->size() - 1;
      }
      in_run = false;
    };
    for (int x = bx0; x <= bx1 + 1; ++x) {
      uint32_t c = 0;
      if (x <= bx1) {
        float px[4];
        Shade(shapes, x, y, px);
        if (px[3] >= 0.5f / 255.0f) {
          const float inv = 1.0f / px[3];
          const uint32_t r = uint32_t(std::clamp(Round(px[0] * inv * 255.0f), 0, 255));
          const uint32_t g = uint32_t(std::clamp(Round(px[1] * inv * 255.0f), 0, 255));
          const uint32_t b = uint32_t(std::clamp(Round(px[2] * inv * 255.0f), 0, 255));
          const uint32_t a = uint32_t(std::clamp(Round(px[3] * 255.0f), 0, 255));
          c = (a << 24) | (b << 16) | (g << 8) | r;  // IM_COL32
        }
      }
      if (in_run && c != color) flush(x);
      if (c != 0 && !in_run) {
        in_run = true;
        start = x;
        color = c;
      }
    }
    open.swap(next);
  }
}

// ------------------------------------------------------------------------------------------------- public: drawing

namespace {

void DrawSettings(ImDrawList* dl, const Settings& s, View v, int player, bool preview) {
  const ImGuiIO& io = ImGui::GetIO();
  const float fx = io.DisplayFramebufferScale.x > 0.01f ? io.DisplayFramebufferScale.x : 1.0f;
  const float fy = io.DisplayFramebufferScale.y > 0.01f ? io.DisplayFramebufferScale.y : 1.0f;
  v.cx *= fx;
  v.cy *= fy;
  v.height *= fy;
  v.recoil_x *= fx;
  v.recoil_y *= fy;
  const double now = ImGui::GetTime();
  Store& st = S();
  if (!preview && !st.caps.shots && st.caps.spread && player >= 0 && player < kPlayers) {
    if (v.spread > st.last_spread[player] + 0.75f) st.shot_at[player] = now;
    st.last_spread[player] = v.spread;
    if (v.seconds_since_shot < 0.0 && st.shot_at[player] >= 0.0) v.seconds_since_shot = now - st.shot_at[player];
  }
  std::vector<Shape> shapes = Place(s, v, now, player);
  static thread_local std::vector<Run> runs;
  Rasterize(shapes, &runs);
  for (const Run& r : runs)
    dl->AddRectFilled(ImVec2(float(r.x) / fx, float(r.y) / fy), ImVec2(float(r.x + r.w) / fx, float(r.y + r.h) / fy),
                      r.color);
}

void Help(const char* text) {
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextDisabled("%s", text);
  ImGui::PopTextWrapPos();
}

void Section(const char* text) {
  ImGui::Spacing();
  ImGui::SeparatorText(text);
}

// A row the game can't feed yet: greyed, with the reason.
struct Greyed {
  bool on;
  const char* why;
  Greyed(bool available, const char* reason) : on(!available), why(reason) {
    if (on) ImGui::BeginDisabled();
  }
  ~Greyed() {
    if (!on) return;
    ImGui::EndDisabled();
    Help(why);
  }
};

bool EditColor(const char* label, Rgba* c) {
  float f[4] = {c->r / 255.0f, c->g / 255.0f, c->b / 255.0f, c->a / 255.0f};
  if (!ImGui::ColorEdit4(label, f, ImGuiColorEditFlags_AlphaBar)) return false;
  *c = {uint8_t(Round(f[0] * 255)), uint8_t(Round(f[1] * 255)), uint8_t(Round(f[2] * 255)), uint8_t(Round(f[3] * 255))};
  return true;
}

bool SliderF(const char* label, float* v, float lo, float hi, const char* fmt = "%.0f") {
  return cod3::ui::SliderFloatBox(label, v, lo, hi, fmt);
}
bool SliderI(const char* label, int* v, int lo, int hi) { return cod3::ui::SliderIntBox(label, v, lo, hi); }

// The preview: the crosshair at its size in the game, in a `side` pixels square box, half dark and half light (so
// an outline and see-through colors show), opening and closing as in the game for a dynamic style when the game
// gives its spread; the game's own color shows as over nothing (white). Dimmed while it is off.
void DrawPreviewBox(const Settings& s, int player, float side) {
  const ImGuiIO& io = ImGui::GetIO();
  const ImVec2 p0 = ImGui::GetCursorScreenPos();
  const ImVec2 p1(p0.x + side, p0.y + side);
  ImGui::Dummy(ImVec2(side, side));
  ImDrawList* dl = ImGui::GetWindowDrawList();
  dl->PushClipRect(p0, p1, true);
  dl->AddRectFilled(p0, ImVec2(std::floor(p0.x + side * 0.5f), p1.y), IM_COL32(38, 40, 44, 255));
  dl->AddRectFilled(ImVec2(std::floor(p0.x + side * 0.5f), p0.y), p1, IM_COL32(176, 180, 186, 255));
  View v;
  v.cx = std::floor(p0.x + side * 0.5f);
  v.cy = std::floor(p0.y + side * 0.5f);
  v.height = io.DisplaySize.y;  // its size on the whole screen
  v.rgba[3] = s.enabled ? 1.0f : 0.35f;
  const double t = ImGui::GetTime();
  if (StyleIsDynamic(s.style)) v.spread = float(3.0 + 3.0 * std::sin(t * 2.2));
  if (s.style == kStaticCrossShotFeedback) v.seconds_since_shot = std::fmod(t, 0.8);
  DrawSettings(dl, s, v, player, true);
  dl->PopClipRect();
  dl->AddRect(p0, p1, ImGui::GetColorU32(ImGuiCol_Border));
}

}  // namespace

void DrawView(ImDrawList* dl, int player, const View& view) {
  if (player < 0 || player >= kPlayers) return;
  const Settings& s = Player(player);
  if (!s.enabled) return;
  DrawSettings(dl, s, view, player, false);
}

void DrawPreview(int player, float side) { DrawPreviewBox(Player(player), player, side); }

bool DrawPage(int* selected, const std::function<bool()>& game_rows) {
  Store& st = S();
  const Caps& caps = st.caps;
  const int players = std::clamp(caps.players, 1, kPlayers);
  int p = std::clamp(selected ? *selected : 0, 0, players - 1);
  bool changed = false;

  // Two columns, as Call of Duty 3's page (acct2-decomp, owner 2026-11-05 06:05: "when scrolling that menu the
  // preview should stay on the right side of the menu while staying centered"): the rows scroll on the left, the
  // preview sits centered in a column on the right that never scrolls.
  const ImGuiStyle& style = ImGui::GetStyle();
  const float side = std::floor(std::max(ImGui::GetFrameHeight() * 6.0f, ImGui::GetIO().DisplaySize.y * 0.12f));
  static constexpr const char* kCaption[] = {"Preview, at its size", "in the game", "(this crosshair is off)"};
  float caption_w = 0.0f;
  for (const char* line : kCaption) caption_w = std::max(caption_w, ImGui::CalcTextSize(line).x);
  const float column_w = std::floor(std::max(side, caption_w) + style.WindowPadding.x * 2.0f);
  const float page_w = ImGui::GetContentRegionAvail().x;
  ImGui::BeginChild("##xhair_rows", ImVec2(std::floor(std::max(page_w * 0.5f, page_w - column_w - style.ItemSpacing.x)),
                                           0.0f));

  if (players > 1) {
    ImGui::TextUnformatted("Crosshair of");
    for (int i = 0; i < players; ++i) {
      char label[32];
      std::snprintf(label, sizeof(label), "Player %d##xhair_player", i + 1);
      ImGui::SameLine();
      if (ImGui::RadioButton(label, p == i)) p = i;
    }
    if (selected) *selected = p;
    Help("Each split-screen player has a crosshair of their own, drawn in their part of the screen.");
  }
  if (game_rows) changed |= game_rows();

  ImGui::PushID(p);
  Settings s = Player(p);
  bool edit = false;

  if (p > 0) {
    if (st.follows[p]) {
      Help("This player uses player 1's crosshair until you change something here.");
    } else if (ImGui::SmallButton("Use player 1's crosshair again")) {
      FollowPlayer1(p);
      s = Player(p);
      changed = true;
    }
  }

  edit |= ImGui::Checkbox("Use my own crosshair", &s.enabled);
  Help("Replaces the game's hip-fire crosshair with this one, shown exactly when the game would show its own: not "
       "in menus or films.");

  // CS2's share codes (also while the crosshair is off: a code turns it on).
  {
    static char code_text[96] = "";
    static std::string message;
    ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.55f, 420.0f));
    const bool enter = ImGui::InputTextWithHint("##code", "CS2 crosshair code (CS...)", code_text, sizeof(code_text),
                                                ImGuiInputTextFlags_EnterReturnsTrue);
    ImGui::SameLine();
    if (ImGui::Button("Import") || enter) {
      std::string why;
      if (ImportCode(code_text, &s, &why)) {
        edit = true;
        message = "Imported, as in CS2 (and, as CS2 does on an import, Follow recoil is off).";
      } else {
        message = "Not imported: " + why + ".";
      }
    }
    ImGui::SameLine();
    if (ImGui::Button("Copy my code")) {
      const std::string code = ExportCode(s);
      ImGui::SetClipboardText(code.c_str());
      message = "Copied: " + code;
    }
    if (!message.empty()) Help(message.c_str());
  }

  ImGui::BeginDisabled(!s.enabled);  // the rows grey while this crosshair is off, as the other rows of F2
  Section("Style");
  ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.6f, 320.0f));
  edit |= ImGui::Combo("Style", &s.style, kStyleNames, kStyleCount);
  if (StyleIsDynamic(s.style) && !caps.spread)
    Help("This game doesn't tell its spread yet, so a dynamic style stays still here.");
  edit |= SliderF("Thickness", &s.thickness, 0.0f, 32.0f, "%.1f");
  if (UsesLength(s.style)) edit |= SliderF("Length", &s.length, 0.0f, 255.0f, "%.1f");
  if (UsesGap(s.style))
    edit |= SliderF("Gap", &s.gap, s.style == kDynamicCrossLegacy ? -10.0f : 0.0f, 128.0f, "%.1f");
  if (UsesDot(s.style)) edit |= ImGui::Checkbox("Center dot", &s.dot);
  if (UsesT(s.style)) {
    ImGui::SameLine();
    edit |= ImGui::Checkbox("T style (no top line)", &s.t_style);
  }
  if (UsesSpreadLimit(s.style)) {
    Greyed g(caps.spread, "Needs the game's spread, which this game's hook doesn't give yet.");
    edit |= SliderI("Dynamic spread limit", &s.spread_limit, 0, 255);
  }
  if (s.style == kDynamicCrossLegacy) {
    edit |= SliderI("Split distance", &s.split_distance, 0, 127);
    edit |= SliderF("Inner split alpha", &s.split_inner_alpha, 0.0f, 1.0f, "%.2f");
    edit |= SliderF("Outer split alpha", &s.split_outer_alpha, 0.3f, 1.0f, "%.2f");
    edit |= SliderF("Split size ratio", &s.split_ratio, 0.0f, 1.0f, "%.2f");
  }
  if (s.style == kStaticQuad) edit |= SliderF("Static quad split ratio", &s.split_ratio, 0.0f, 1.0f, "%.2f");
  if (StyleIsDynamic(s.style)) {
    Greyed g(caps.spread, "Needs the game's spread.");
    edit |= SliderF("How far it opens", &s.open_scale, 0.0f, 3.0f, "%.2f x");
    if (caps.spread) Help("1 is exactly the game's own spread; lower keeps the crosshair tighter.");
  }
  edit |= SliderI("Sizes are for a screen this tall", &s.screen_height, 240, 4320);
  Help("Sizes are pixels on a screen this many lines tall; they grow and shrink with yours, and with a split-screen "
       "player's part of it. A CS2 code brings its own.");

  Section("Color");
  {
    const char* from[] = {"The game's (it changes over an enemy or a friendly)", "My own for each case",
                          "One color, as in CS2"};
    ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.6f, 320.0f));
    edit |= ImGui::Combo("Color", &s.color_from, from, 3);
    if (s.color_from == kColorConstant) {
      edit |= EditColor("Crosshair color", &s.color);
    } else if (s.color_from == kColorOwnPerCase) {
      edit |= EditColor("Over nothing", &s.case_color[kCaseNormal]);
      edit |= EditColor("Over an enemy", &s.case_color[kCaseEnemy]);
      edit |= EditColor("Over a friendly", &s.case_color[kCaseFriendly]);
      if (caps.dead_color) edit |= EditColor("Over a dead body", &s.case_color[kCaseDead]);
      Help("Your colors in place of the game's; the crosshair still changes when the game's does.");
    }
  }

  Section("Outline");
  {
    const char* modes[] = {"None", "Full", "Half (top and left)"};
    ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.6f, 320.0f));
    edit |= ImGui::Combo("Outline", &s.outline, modes, 3);
    if (s.outline != kOutlineNone) {
      edit |= EditColor("Outline color", &s.outline_color);
      edit |= SliderF("Outline width (pixels)", &s.outline_thickness, 1.0f, 4.0f, "%.0f");
      Help("CS2's outline is one pixel.");
    }
  }

  Section("More from CS2");
  {
    Greyed g(caps.recoil, "Needs the view's kick, which this game's hook doesn't give yet.");
    edit |= ImGui::Checkbox("Follow recoil", &s.follow_recoil);
  }
  {
    Greyed g(caps.sights, "Needs how far the sights are up, which this game's hook doesn't give yet.");
    edit |= ImGui::Checkbox("Iron sight dot while aiming down the sights", &s.ads_dot);
    if (s.ads_dot) {
      edit |= ImGui::Checkbox("Dot in the crosshair's color", &s.ironsight_use_color);
      edit |= SliderF("Dot scale", &s.ironsight_dot_scale, 0.1f, 2.0f, "%.2f");
    }
  }
  edit |= ImGui::Checkbox("Friendly warning", &s.friendly_warning);
  Help("A mark over the crosshair while a friendly is under it (when the game turns its crosshair green).");
  {
    Greyed g(caps.spectating, "Needs to know when you watch another player, which this game's hook doesn't give yet.");
    edit |= ImGui::Checkbox("Show the crosshair when watching another player", &s.observer);
    ImGui::SameLine();
    edit |= ImGui::Checkbox("...also a bot", &s.observed_bot);
  }

  Section("Grenades");
  {
    Greyed g(caps.grenades, "Needs to know which grenade is out, which this game's hook doesn't give yet.");
    edit |= ImGui::Checkbox("Keep my crosshair while a grenade is out", &s.grenade_keep_mine);
    for (int i = 0; i < kGrenadeCount; ++i) {
      ImGui::PushID(i);
      char label[48];
      std::snprintf(label, sizeof(label), "%s grenade crosshair", kGrenadeNames[i]);
      edit |= ImGui::Checkbox(label, &s.grenade_on[i]);
      if (s.grenade_on[i]) {
        ImGui::SameLine();
        ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.5f, 220.0f));
        edit |= SliderF("delay (s)##delay", &s.grenade_delay[i], 0.0f, 5.0f, "%.1f");
      }
      ImGui::PopID();
    }
  }

  Section("Sniper");
  {
    Greyed g(caps.scope, "Needs the game's scope, which this game's hook doesn't give yet.");
    edit |= ImGui::Checkbox("Delay unscope after a shot", &s.sniper_delay_unscope);
    edit |= ImGui::Checkbox("Show inaccuracy in the scope", &s.sniper_show_inaccuracy);
    edit |= ImGui::Checkbox("Zoom back in after a shot", &s.sniper_auto_rezoom);
    edit |= SliderI("Scope line width", &s.sniper_width, 1, 6);
  }

  // [acct2-ocr] 2026-11-14 (owner 17:06 / 17:19) the crosshairs the game draws for something other than an ordinary
  // weapon: one row for each kind this game has, the game's own unless chosen.
  if (caps.special != 0 || caps.special_later != 0) {
    Section("Special crosshairs");
    Help("What this player sees where the game draws a crosshair of its own kind. The game's own: as the game draws "
         "it. Mine: this crosshair instead. Both: the game's and this one over it. None: no crosshair there.");
    for (int k = 0; k < kSpecialCount; ++k) {
      const bool told = (caps.special >> k) & 1u, later = (caps.special_later >> k) & 1u;
      if (!told && !later) continue;
      ImGui::PushID(100 + k);
      const char* name = caps.special_label[k] ? caps.special_label[k] : kSpecialNames[k];
      Greyed g(told, "This game's hooks don't tell this one apart yet, so the game's own stays.");
      int mode = told ? std::clamp(s.special[k], 0, kSpecialModeCount - 1) : int(kGameOwn);
      ImGui::SetNextItemWidth(std::min(ImGui::GetContentRegionAvail().x * 0.35f, 200.0f));
      if (ImGui::Combo(name, &mode, kSpecialModeNames, kSpecialModeCount) && told) {
        s.special[k] = mode;
        edit = true;
      }
      if (told) Help(kSpecialHelp[k]);
      ImGui::PopID();
    }
  }
  ImGui::EndDisabled();

  ImGui::Spacing();
  if (players > 1 && ImGui::Button("Give every player this crosshair")) {
    for (int i = 0; i < kPlayers; ++i)
      if (i != p) SetPlayer(i, s);
    changed = true;
  }
  if (players > 1) ImGui::SameLine();
  if (ImGui::Button("Reset this crosshair")) ImGui::OpenPopup("##xhair_reset");
  if (ImGui::BeginPopup("##xhair_reset")) {
    ImGui::TextUnformatted("Put this player's crosshair back to the defaults?");
    if (ImGui::Button("Reset")) {
      s = Settings{};
      edit = true;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }

  if (edit) {
    SetPlayer(p, s);
    changed = true;
  }
  ImGui::PopID();
  ImGui::EndChild();

  // The preview's column.
  ImGui::SameLine();
  ImGui::BeginChild("##xhair_preview", ImVec2(0.0f, 0.0f), ImGuiChildFlags_None,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  {
    const Settings& shown = Player(p);
    const int lines = shown.enabled ? 2 : 3;
    const ImVec2 area = ImGui::GetContentRegionAvail();
    const ImVec2 start = ImGui::GetCursorPos();
    const float block_h = side + style.ItemSpacing.y + float(lines) * ImGui::GetTextLineHeightWithSpacing();
    ImGui::SetCursorPos(ImVec2(start.x + std::floor(std::max(0.0f, (area.x - side) * 0.5f)),
                               start.y + std::floor(std::max(0.0f, (area.y - block_h) * 0.5f))));
    DrawPreviewBox(shown, p, side);
    for (int i = 0; i < lines; ++i) {
      const float w = ImGui::CalcTextSize(kCaption[i]).x;
      ImGui::SetCursorPosX(start.x + std::floor(std::max(0.0f, (area.x - w) * 0.5f)));
      ImGui::TextDisabled("%s", kCaption[i]);
    }
  }
  ImGui::EndChild();
  return changed;
}

}  // namespace f2::xhair
