// f2 - Counter-Strike 2's crosshair for every game's F2 window (f2_crosshair_cs2.h). [acct2-ocr] 2026-11-14.

#include "f2/f2_crosshair_cs2.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <tuple>

#include <imgui.h>

#include <rex/cvar.h>

#include "f2/crosshair_cs2_code.h"  // [acct2-decomp] CS2's share codes
#include "ui/slider_box.h"          // Call of Duty 3's slider with a number box

namespace f2::xhair {

namespace {

constexpr float kPi = 3.14159265358979f;
constexpr int kCodeGapLimit = 3840;  // a CS2 code's gap, either way (libclient.so 0x1bc5120, 0x1bc4cc0)

// ------------------------------------------------------------------------------------------------- names

const char* const kStyleNames[kStyleCount] = {
    "Dynamic Cross", "Dynamic Circle", "Dynamic Cross (Legacy)", "Static Circle", "Static Cross",
    "Static Cross (Shot Feedback)", "Dot Only", "Dynamic Quad", "Static Square", "Static Quad"};

const char* const kGrenadeNames[kGrenadeCount] = {"Flash", "Explosive", "Fire", "Smoke", "Decoy"};

// [acct2-ocr] 2026-11-14 the special crosshairs' rows (owner 17:06 / 17:19).
const char* const kSpecialNames[kSpecialCount] = {"Weapons with a crosshair of their own", "Turrets and mounted guns",
                                                  "Vehicles", "Scopes", "Grenade countdown"};
const char* const kSpecialHelp[kSpecialCount] = {
    "Weapons the game draws a crosshair of their own for, such as World at War's flamethrower.",
    "Mounted machine guns, anti-aircraft guns and gunner seats.",
    "Driving or riding a vehicle, and the vehicle's aiming marks on the HUD.",
    "A scope's picture while aiming, also a scoped turret's. None leaves the zoomed view bare.",
    "A cooked grenade's countdown, the marks round the crosshair. Mine: six marks in this crosshair's look."};
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
// T style only applies to the styles that offer it (CS2: "removes the top bar on cross styles"). [acct2-ocr]
// 2026-11-22 (owner 02:22: "the T style has dynamic modes within CS that should also be in cod3"): a dynamic style
// moves with the spread with T style on too, as CS2's own code draws it (0x1bc3820 leaves out only the top bar), and
// shows its dynamic rows; acct2-bro's 11-15 rule (still with T style) is gone.
bool TOn(const Settings& s) { return s.t_style && UsesT(s.style); }
bool MovesWithSpread(const Settings& s) { return StyleIsDynamic(s.style); }

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
  // [acct2-ocr] 2026-11-21 each player's shot feedback kick (CS2's, 0x1bc43d0) and the least spread of the last ten
  // seconds (for View::spread_rest when the game doesn't say), in twenty half-second buckets. kick[kPlayers]: the
  // preview's.
  struct Kick {
    float kick = 0;
    double at = -1;          // the frame it was last run down
    double shot = -1;        // the last shot counted
    float preview_timer = 0;  // the preview's next shot (CS2's: 0.2 to 0.6 s apart)
    uint32_t seed = 0x2545F491u;
  };
  std::array<Kick, kPlayers + 1> kick{};
  struct Rest {
    double start = -1;
    int head = 0;
    std::array<float, 20> least{};
  };
  std::array<Rest, kPlayers> rest{};
  // [acct2-ocr] 2026-11-21 For the game's own Options pages, on the game's thread (PlayerCopy, AskSetPlayer): each
  // player's crosshair as last published here, and a change asked there until Sync has taken it in.
  std::mutex copy_mutex;
  std::array<Settings, kPlayers> copy{};
  std::array<bool, kPlayers> asked{};
  std::array<bool, kPlayers> asked_written{};  // written into the text setting (or to be, once bound)
  std::array<uint32_t, kPlayers> ask_seq{};    // counts the asks
  std::array<uint32_t, kPlayers> done_seq{};   // the last ask whose write has returned (a read after sees it, or newer)
  std::array<std::string, kPlayers> asked_text;
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
    // [acct2-ocr] which of the four arcs are drawn (CS2's shader draws all four, whatever T style is; a mask of the
    // port's own)
    const int bit = quarter >= 0.0f ? (quarter < 0.5f ? 1 : 2) : (quarter > -1.5f ? 8 : 4);
    if (!(sh.arcs & bit)) {
      *fill_cov = 0.0f;
      *outline_cov = 0.0f;
      return;
    }
    f *= SmoothStep(hs + 0.5f / L, hs - 0.5f / L, ad);
    o *= SmoothStep(hs + (og + 0.5f) / L, hs + (og - 0.5f) / L, ad);
  }
  if (sh.kind == 3) {  // [acct2-ocr] 2026-11-15 one arc centered at sh.angle (the grenade countdown's marks)
    float d = a - sh.angle;
    while (d > kPi) d -= 2.0f * kPi;
    while (d < -kPi) d += 2.0f * kPi;
    const float ad = std::fabs(d), L = std::max(len, 1.0f), hs = sh.span * 0.5f;
    const float og = Mix(sh.grow0, sh.grow1, h);
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
  add("arc", b(s.grenade_arc));  // [acct2-ocr] 2026-11-22
  add("arc_bounces", std::to_string(s.grenade_arc_bounces));
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
    else if (key == "gap") s.gap = f(-kCodeGapLimit, kCodeGapLimit);  // [acct2-ocr] 11-15: a code's range
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
    else if (key == "screen_height") s.screen_height = in(0, 65535);  // [acct2-ocr] 11-15: a code's range
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
    else if (key == "arc") s.grenade_arc = on;  // [acct2-ocr] 2026-11-22
    else if (key == "arc_bounces") s.grenade_arc_bounces = in(0, 3);
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
  // [acct2-ocr] 2026-11-15: as CS2's console import (libclient.so 0x1bd4840: the decoder 0x1bc5120, then 0x1bc5a20
  // makes the settings). An old CSGO- code doesn't carry the iron sight dot's two settings: CS2 keeps the player's.
  cs2::LegacyFill fill;
  fill.scope_dot_use_crosshair_color = s->ironsight_use_color;
  fill.scope_dot_scale = s->ironsight_dot_scale;
  const std::optional<cs2::Code> c = cs2::Decode(code, why, fill);
  if (!c) return false;
  // CS2 draws a style over 9 as 9 and an outline mode over 2 as 2 (0x1bc5860); only an old code can carry them.
  s->style = std::min<int>(c->style, kStyleCount - 1);
  s->dot = c->center_dot;
  s->t_style = c->t_style;
  s->color = {c->color[0], c->color[1], c->color[2], c->color[3]};
  s->outline_color = {c->outline_color[0], c->outline_color[1], c->outline_color[2], c->outline_color[3]};
  s->thickness = float(std::min<int>(c->thickness, 32));
  s->outline = std::min<int>(c->outline_mode, 2);
  s->gap = float(std::clamp<int>(c->gap, -kCodeGapLimit, kCodeGapLimit));
  s->length = float(c->length);
  s->spread_limit = c->dynamic_spread_limit;
  s->split_distance = c->split_distance;
  // An old code can carry split alphas and a ratio a little over 1 (CS2 converts them unchecked); kept to CS2's
  // own ranges here.
  s->split_inner_alpha = std::clamp(c->InnerSplitAlpha(), 0.0f, 1.0f);
  s->split_outer_alpha = std::clamp(c->OuterSplitAlpha(), 0.3f, 1.0f);
  s->split_ratio = std::clamp(c->SplitSizeRatio(), 0.0f, 1.0f);
  s->ironsight_use_color = c->scope_dot_use_crosshair_color;
  s->ironsight_dot_scale = std::clamp(c->ScopeDotScale(), 0.1f, 2.0f);
  // The sizes are pixels on a screen this tall. CS2 then rescales them to its own window (0x1bc5bb0: each size times
  // the window's height over this, rounded, at least 1); here they scale with each view's height when drawn. An old
  // code can have none: CS2 then takes the sizes as they are for its window; here, for the height set now.
  if (c->screen_height > 0) s->screen_height = c->screen_height;
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
  // [acct2-ocr] 2026-11-15: as CS2's encoder (libclient.so 0x1bc4cc0) makes a code of its settings: the same limits,
  // and the alphas, the ratio and the dot's scale rounded from the same float arithmetic.
  auto hundredths = [](float v) { return uint8_t(int(std::roundf(v / 0.01f)) & 0x7F); };
  cs2::Code c;
  c.screen_height = uint16_t(std::clamp(s.screen_height, 0, 65535));
  c.style = uint8_t(std::clamp(s.style, 0, 9));
  c.follow_recoil = s.follow_recoil;
  c.center_dot = s.dot;
  c.t_style = s.t_style;
  const uint8_t col[4] = {s.color.r, s.color.g, s.color.b, s.color.a};
  const uint8_t oc[4] = {s.outline_color.r, s.outline_color.g, s.outline_color.b, s.outline_color.a};
  std::memcpy(c.color, col, 4);
  std::memcpy(c.outline_color, oc, 4);
  c.thickness = uint8_t(std::clamp(Round(s.thickness), 0, 32));
  c.outline_mode = uint8_t(std::clamp(s.outline, 0, 2));
  c.gap = int16_t(std::clamp(Round(s.gap), -kCodeGapLimit, kCodeGapLimit));
  c.length = uint8_t(std::clamp(Round(s.length), 0, 255));
  c.dynamic_spread_limit = uint8_t(std::clamp(s.spread_limit, 0, 255));
  c.split_distance = uint8_t(std::clamp(s.split_distance, 0, 127));
  c.inner_split_alpha = hundredths(std::clamp(s.split_inner_alpha, 0.0f, 1.0f));
  c.outer_split_alpha = hundredths(std::clamp(s.split_outer_alpha, 0.3f, 1.0f) - 0.3f);
  c.split_size_ratio = hundredths(std::clamp(s.split_ratio, 0.0f, 1.0f));
  c.scope_dot_use_crosshair_color = s.ironsight_use_color;
  c.scope_dot_scale = uint8_t(int(std::roundf((std::clamp(s.ironsight_dot_scale, 0.1f, 2.0f) - 0.1f) / 0.01f)));
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
  std::lock_guard lock(st.copy_mutex);  // [acct2-ocr] 2026-11-21 and the copy the game's thread reads
  for (int p = 0; p < kPlayers; ++p) {
    if (!st.asked[p]) st.copy[p] = st.players[p];
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
  // [acct2-ocr] 2026-11-21 a change the game's Options pages asked for before the names were bound: written now
  std::array<std::string, kPlayers> late;
  std::array<uint32_t, kPlayers> late_seq{};
  {
    std::lock_guard lock(st.copy_mutex);
    for (int p = 0; p < kPlayers; ++p)
      if (st.asked[p] && !st.asked_written[p] && HasSetting(st.names[p])) {
        late[p] = st.asked_text[p];
        late_seq[p] = st.ask_seq[p];
        st.asked_written[p] = true;
      }
  }
  for (int p = 0; p < kPlayers; ++p) {
    if (!late_seq[p]) continue;
    rex::cvar::SetFlagByName(st.names[p], late[p]);
    std::lock_guard lock(st.copy_mutex);
    st.done_seq[p] = std::max(st.done_seq[p], late_seq[p]);
  }
  for (int p = 0; p < kPlayers; ++p) {
    uint32_t done = 0;  // [acct2-ocr] 2026-11-21 the asks written before this read: the read has them (or newer)
    {
      std::lock_guard lock(st.copy_mutex);
      done = st.done_seq[p];
    }
    const std::string text = HasSetting(st.names[p]) ? rex::cvar::GetFlagByName(st.names[p]) : std::string();
    {
      std::lock_guard lock(st.copy_mutex);
      if (st.asked[p] && st.ask_seq[p] == done) st.asked[p] = false;
    }
    // [acct2-ocr] 2026-11-21 player 1's text changed elsewhere while the game runs (the game's Options pages, the
    // console): the old names follow it below, so the two agree.
    const bool elsewhere = p == 0 && st.read[p] && !text.empty() && text != st.last[p];
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
    if (elsewhere && st.set_legacy) {
      Write(0);
      continue;
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

// [acct2-ocr] 2026-11-21 the game's Options pages (game/menu_port_rows.h)
Settings PlayerCopy(int player) {
  Store& st = S();
  std::lock_guard lock(st.copy_mutex);
  return st.copy[size_t(std::clamp(player, 0, kPlayers - 1))];
}

void AskSetPlayer(int player, const Settings& s) {
  Store& st = S();
  const size_t p = size_t(std::clamp(player, 0, kPlayers - 1));
  const std::string text = Text(s);
  bool write = false;
  uint32_t seq = 0;
  {
    std::lock_guard lock(st.copy_mutex);
    st.copy[p] = s;
    st.asked[p] = true;
    st.asked_text[p] = text;
    seq = ++st.ask_seq[p];
    write = st.asked_written[p] = st.bound && HasSetting(st.names[p]);
  }
  if (!write) return;
  rex::cvar::SetFlagByName(st.names[p], text);  // (no setting's callback runs into this file)
  std::lock_guard lock(st.copy_mutex);
  st.done_seq[p] = std::max(st.done_seq[p], seq);
}

bool StyleUsesLength(int style) { return UsesLength(style); }
bool StyleUsesGap(int style) { return UsesGap(style); }
bool StyleUsesT(int style) { return UsesT(style); }
bool StyleUsesDot(int style) { return UsesDot(style); }

void SetCaps(const Caps& caps) { S().caps = caps; }

// [acct2-ocr] 2026-11-22 the grenade arc's block (a host's veto), set by the game's code, read by the page
namespace {
std::atomic<const char*> g_arc_blocked{nullptr};
}  // namespace
void SetGrenadeArcBlocked(const char* why) { g_arc_blocked.store(why, std::memory_order_relaxed); }
const char* GrenadeArcBlocked() { return g_arc_blocked.load(std::memory_order_relaxed); }
const Caps& GetCaps() { return S().caps; }

// ------------------------------------------------------------------------------------------------- public: placing

namespace {

// [acct2-ocr] 2026-11-21 CS2's placement, read from libclient.so (CS2 of 11-19; owner 11-21 10:43 "These settings
// really need to look and feel official"): the main builder 0x1bd3560, the styles' builders 0x1bc3670-0x1bc4bc0, the
// shape adders 0x1bc1d50 (rectangles), 0x1bc1e50 (rings), 0x1bc1f50 (four arcs) and the draw-time rescale 0x1bc5cf0.
// Its numbers, as it has them:
constexpr float kSpreadCapBase = 64.0f;  // the dynamic spread's soft limit: 64 + cl_crosshair_dynamic_spread_limit
constexpr float kSpreadKnee = 0.75f;     //   it eases from three quarters of that on
constexpr float kFadeAt = 0.25f, kFadeMost = 0.8f;  // the dynamic styles fade to 0.2 at 0.25 above resting inaccuracy
constexpr float kQuadRingSlack = 10.0f;  // the dynamic quad is a ring within 10 pixels of the resting spread...
constexpr float kQuadRingMost = 100.0f;  //   ... and under 100, else four arcs
constexpr float kQuadArcsAt = 0.2f;      //   the arcs go from 45 degrees at rest to 5 at 0.2 above it
constexpr float kQuadSpanMost = 45.0f, kQuadSpanFall = -40.0f;
constexpr float kStaticQuadSpan = 90.0f;  // the static quad's arcs: 90 degrees times its split ratio
constexpr float kDegToRad = 0.0174533f;
constexpr float kKickMost = 25.0f, kKickFall = 42.0f, kKickScreen = 1200.0f;  // shot feedback (0x1bc43d0)
constexpr float kKickPreview = 15.0f;  // a shot in CS2's preview, 0.2 to 0.6 s apart
// For each shot a CS2 gun adds its recoil number / 3.5 (0x1bc45d0); Call of Duty's guns have none, so a rifle's is
// taken (23, the number CS2's M4A4 has).
constexpr float kKickShot = 23.0f / 3.5f;

// CS2's V_roundf (halves away from 0) and cvtss2si (to the nearest, halves to even).
float RoundF(float v) { return std::round(v); }
int Nearest(float v) { return int(std::lrint(v)); }

// How far the spread is from rest to where CS2's inaccuracy `at` above the weapon's resting one would put it, 0 to 1.
// CS2 works these from its weapons' inaccuracy (radians); its spread in pixels is inaccuracy * 320 / tan(fov / 2) *
// height / 480 (its classic style's formula, 0x1bc3e40), so at its 90 degrees an inaccuracy `at` is at * height * 2/3
// pixels. Call of Duty gives its spread, so the pixels stand in for the inaccuracy.
float Above(float spread, float rest, float at, float height) {
  const float span = at * height * (320.0f / 480.0f) - rest;
  if (!(span > 0.0f)) return spread > rest ? 1.0f : 0.0f;
  return std::clamp((spread - rest) / span, 0.0f, 1.0f);
}

}  // namespace

std::vector<Shape> Place(const Settings& s, const View& v, double now, int player) {
  (void)now;
  (void)player;
  std::vector<Shape> out;
  const Caps& caps = S().caps;

  // [acct2-ocr] 2026-11-21 The screen's height in whole pixels and the sizes for it, as CS2 rescales them at draw time
  // (0x1bc5cf0) from cl_crosshair_screen_height's: each that isn't 0 becomes V_roundf(size * height / its height), at
  // least 1 (the gap keeps its sign); the same height, or none, leaves them as they are.
  const float H = float(std::max(1L, std::lround(v.height)));
  float gap = s.gap, th = s.thickness, len = s.length, limit = float(s.spread_limit),
        split = float(s.split_distance);
  {
    const float h = float(s.screen_height);
    if (h > 0.0f && h != H) {
      const float k = H / h;
      const float g = RoundF(gap * k);
      if (gap > 0.0f)
        gap = std::max(1.0f, g);
      else if (gap < 0.0f)
        gap = std::min(-1.0f, g);
      for (float* x : {&len, &th, &limit, &split})
        if (*x > 0.0f) *x = std::max(1.0f, RoundF(*x * k));
    }
  }
  const float ref = float(s.screen_height > 0 ? s.screen_height : 1080);
  const float scale = std::max(0.25f, H / ref);  // the port's own pieces (the marks below the crosshair)

  float fill[4];
  FillColor(s, v, fill);
  float outline[4] = {0, 0, 0, 0};
  const bool outlined = s.outline == kOutlineFull || s.outline == kOutlineHalf;
  if (outlined) Color4(s.outline_color, std::clamp(v.rgba[3], 0.0f, 1.0f), outline);
  // How far the outline reaches past a shape, on the top / left and the bottom / right: CS2's full (1, 1) and half
  // (1, 0); the port lets it be wider (outline_px).
  const float reach = outlined ? float(std::max(1, Round(s.outline_thickness))) : 0.0f;
  const float grow0 = reach, grow1 = s.outline == kOutlineHalf ? 0.0f : reach;

  // CS2's whole thickness (cvtss2si, at least 1) and its halves: c pixels before the middle line, a after it. An odd
  // thickness's middle is the middle of the pixel before the line cx, an even one's the line cx itself.
  int t = Nearest(th);
  if (t <= 0) t = 1;
  const int c = (t + 1) >> 1, a = t - c;
  const bool odd = c != a;
  const float off = odd ? 0.5f : 0.0f;  // a ring's middle there

  // The middle in whole pixels. CS2 cuts the view's middle down (roundss 0xb; with Follow recoil the kicked point's
  // next pixel up), which on a screen an even number of pixels across is the screen's middle line. [acct2-ocr] 11-21
  // (owner 11:16 "make sure the crosshair center is actually the center of the screen", 11:18 "please account for
  // splitscreen too") here the line is chosen for the thickness, so that the crosshair's own middle is its view's
  // middle wherever the pixels allow it: the same as CS2's on an even screen (an odd thickness then sits half a pixel
  // up and left, as in CS2, there being no middle pixel), and on an odd one (a split-screen view 525 lines tall, a
  // scaled window) on its middle pixel or within half a pixel, where CS2's cut can be a whole pixel off. Each
  // split-screen view is centered on its own middle. Ties go up and left, as CS2's; 1/64 takes up the float error of
  // a scaled window's middle.
  float at_x = v.cx, at_y = v.cy;
  if (s.follow_recoil && caps.recoil) {
    at_x += v.recoil_x;
    at_y += v.recoil_y;
  }
  constexpr float kTie = 1.0f / 64.0f;
  auto line_for = [&](float at) {  // the line cx (or cy) that puts the crosshair's middle nearest `at`
    return odd ? std::floor(at - kTie) + 1.0f : std::ceil(at - 0.5f - kTie);
  };
  const float cx = line_for(at_x), cy = line_for(at_y);

  // The spread in pixels, as CS2 rounds it (its +0xb0), and at rest (+0xac). A game that gives none sits at its gap,
  // at rest.
  const bool moving = MovesWithSpread(s) && caps.spread;
  float spread, rest;
  if (moving) {
    const float px = H / 480.0f * s.open_scale;
    const float now_units = std::max(0.0f, v.spread);
    spread = RoundF(now_units * px);
    rest = v.spread_rest >= 0.0f ? RoundF(std::min(v.spread_rest, now_units) * px) : spread;
  } else {
    spread = rest = std::max(0.0f, gap);
  }
  // CS2's fade for the dynamic styles (+0xb8): 1 at rest down to 0.2 at 0.25 above the resting inaccuracy.
  const float fade = moving ? 1.0f - kFadeMost * Above(spread, rest, kFadeAt, H) : 1.0f;
  // The dynamic styles' gap (+0xb4): the spread, eased to a stop at 64 + the limit from three quarters of that on, at
  // least the thickness with the dot, cut to a whole pixel.
  float dyn = spread;
  {
    const float cap = kSpreadCapBase + limit, knee = kSpreadKnee * cap;
    if (dyn > knee) dyn = cap - (cap - knee) * std::exp(-(1.0f / (cap - knee)) * (dyn - knee));
    dyn = std::min(dyn, cap);
    dyn = std::max(dyn, s.dot ? th : 0.0f);
    dyn = std::trunc(dyn);
  }

  // CS2's alphas are bytes: the dynamic styles' V_roundf(alpha * fade), the legacy split's (int)(alpha * its mod).
  auto with_alpha = [](const float c[4], float a, float o[4]) {
    o[0] = c[0];
    o[1] = c[1];
    o[2] = c[2];
    o[3] = a;
  };
  float fill_dyn[4], outline_dyn[4];
  with_alpha(fill, RoundF(fill[3] * 255.0f * fade) / 255.0f, fill_dyn);
  with_alpha(outline, RoundF(outline[3] * 255.0f * fade) / 255.0f, outline_dyn);

  // A rectangle as CS2 adds one (0x1bc1d50): [x0, x1) x [y0, y1), kept as the shader takes it, inclusive.
  auto rect = [&](float x0, float y0, float x1, float y1, const float f[4], const float o[4]) {
    if (!(x1 > x0) || !(y1 > y0)) return;
    Shape sh;
    sh.kind = 0;
    sh.x0 = x0;
    sh.y0 = y0;
    sh.x1 = x1 - 1.0f;
    sh.y1 = y1 - 1.0f;
    sh.grow0 = grow0;
    sh.grow1 = grow1;
    for (int i = 0; i < 4; ++i) {
      sh.fill[i] = f[i];
      sh.outline[i] = o[i];
    }
    out.push_back(sh);
  };
  // A ring (kind 1) or four arcs (kind 2) around (x, y), outer radius R, T thick, as CS2's builders give them. CS2
  // stores x - 0.5 and its shader measures from the pixel's corner; Coverage here measures from the pixel's middle,
  // which is the same circle around x.
  auto ring = [&](int kind, float x, float y, float R, float T, float span, const float f[4], const float o[4]) {
    Shape sh;
    sh.kind = kind;
    sh.x0 = x;
    sh.y0 = y;
    sh.x1 = R;
    sh.y1 = T;
    sh.span = span;
    sh.grow0 = grow0;
    sh.grow1 = grow1;
    for (int i = 0; i < 4; ++i) {
      sh.fill[i] = f[i];
      sh.outline[i] = o[i];
    }
    out.push_back(sh);
  };

  // CS2's cross (0x1bc3820): four bars `g` out from the middle (no less than 0), `l` long; an odd thickness's right
  // and bottom bars start a pixel sooner, so they are as far from its middle pixel as the left and top ones. T style
  // leaves out the top one.
  auto cross = [&](const float f[4], const float o[4], float g, float l) {
    if (!(l > 0.0f) || !(th > 0.0f)) return;
    g = std::max(g, 0.0f);
    const float k = odd ? -1.0f : 0.0f;
    const float left = std::floor(cx - g);
    rect(left - l, cy - float(c), left, cy + float(a), f, o);
    const float right = std::ceil(cx + g) + k;
    rect(right, cy - float(c), right + l, cy + float(a), f, o);
    if (!TOn(s)) {
      const float top = std::floor(cy - g - l);
      rect(cx - float(c), top, cx + float(a), top + l, f, o);
    }
    const float bottom = std::ceil(cy + g) + k;
    rect(cx - float(c), bottom, cx + float(a), bottom + l, f, o);
  };
  // CS2's dot (0x1bc3670, and Dot Only's): t x t on the bars' middle.
  auto dot = [&](const float f[4], const float o[4]) {
    if (th > 0.0f) rect(cx - float(c), cy - float(c), cx + float(a), cy + float(a), f, o);
  };

  // While a grenade is out: CS2 keeps the crosshair only when asked to.
  const bool grenade_out = caps.grenades && v.grenade >= 0 && v.grenade < kGrenadeCount &&
                           (caps.grenade_kinds >> v.grenade & 1u);
  const bool draw_main = !grenade_out || s.grenade_keep_mine;
  const bool watching_hidden = caps.spectating && v.watching && (!s.observer || (v.watching_bot && !s.observed_bot));

  if (draw_main && !watching_hidden) {
    if (s.dot) dot(fill, outline);  // before the style, as CS2 draws it
    switch (s.style) {
      case kDynamicCross:  // 0x1bc3a90: the cross at the dynamic gap, faded
        cross(fill_dyn, outline_dyn, dyn, len);
        break;
      case kDynamicCircle:  // 0x1bc3b80: a ring out to the dynamic gap plus the thickness, a pixel thinner
        if (dyn >= 0.0f && th > 0.0f)
          ring(1, cx - off, cy - off, th + dyn, std::max(1.0f, th - 1.0f), 0.0f, fill_dyn, outline_dyn);
        break;
      case kDynamicCrossLegacy: {
        // 0x1bc3e40, CS:GO's classic dynamic: the spread in the 480-line screen's units (CS2: (inaccuracy + spread) *
        // 320 / tan(fov / 2)); the bars sit that far out past the gap. Past the split distance each bar splits:
        // its inner (1 - ratio) stays at the split distance and its outer part goes on with the spread, each with
        // its own alpha.
        const float units = moving ? std::max(0.0f, v.spread) * s.open_scale : 0.0f;
        const float yres = H / 480.0f;
        const float D = RoundF(yres * units);
        float outer_gap, inner_gap;
        if (D > 0.0f) {
          outer_gap = D + gap;
          inner_gap = gap + RoundF(std::min(split, units) * yres);
        } else {
          outer_gap = std::trunc(gap);  // CS2 without a player's spread: 4 + gap - 4
          inner_gap = gap;
        }
        if (units > split) {
          const float inner_len = std::ceil((1.0f - s.split_ratio) * len), outer_len = std::floor(len * s.split_ratio);
          float fo[4], oo[4], fi[4], oi[4];
          with_alpha(fill, std::trunc(fill[3] * 255.0f * s.split_outer_alpha) / 255.0f, fo);
          with_alpha(outline, std::trunc(outline[3] * 255.0f * s.split_outer_alpha) / 255.0f, oo);
          with_alpha(fill, std::trunc(fill[3] * 255.0f * s.split_inner_alpha) / 255.0f, fi);
          with_alpha(outline, std::trunc(outline[3] * 255.0f * s.split_inner_alpha) / 255.0f, oi);
          cross(fo, oo, outer_gap + inner_len, outer_len);
          cross(fi, oi, std::max(inner_gap, 0.0f), inner_len);
        } else {
          cross(fill, outline, outer_gap, len);
        }
        break;
      }
      case kStaticCircle:  // 0x1bc41e0: a ring out to the gap (at least 1) plus the thickness, a pixel thinner
        if (th > 0.0f)
          ring(1, cx - off, cy - off, std::max(1.0f, gap) + th, std::max(1.0f, th - 1.0f), 0.0f, fill, outline);
        break;
      case kStaticCross:  // 0x1bd3e00
        cross(fill, outline, std::max(gap, 0.0f), len);
        break;
      case kStaticCrossShotFeedback:  // 0x1bc43d0: the gap grows by the kick, kick / 1200 of the screen's height
        cross(fill, outline, RoundF(std::max(gap, 0.0f) + H * std::max(v.shot_kick, 0.0f) / kKickScreen), len);
        break;
      case kDotOnly:  // 0x1bd4378: the dot, unless it is drawn already
        if (!s.dot) dot(fill, outline);
        break;
      case kDynamicQuad: {
        // 0x1bc46c0: near rest a ring round the dynamic gap; further out four arcs on the diagonals, 45 degrees each
        // at rest narrowing to 5; and the cross at the gap, unfaded.
        if (spread > 0.0f) {
          const float o = (odd && th >= 1.0f) ? 0.5f : 0.0f;
          if (spread < kQuadRingMost && spread <= kQuadRingSlack + rest) {
            const float T = std::max(1.0f, th - 1.0f);
            ring(1, cx - o, cy - o, dyn + T, T, 0.0f, fill_dyn, outline_dyn);
          } else {
            const float deg = kQuadSpanMost + Above(spread, rest, kQuadArcsAt, H) * kQuadSpanFall;
            ring(2, cx - o, cy - o, th + dyn, th, deg * kDegToRad, fill_dyn, outline_dyn);
          }
        }
        cross(fill, outline, std::max(gap, 0.0f), len);
        break;
      }
      case kStaticSquare:
        // 0x1bc49c0: a square frame the gap out, the thickness wide; with the dot an odd thickness's frame is a pixel
        // wider to the left and top, so the dot is in its middle.
        if (th > 0.0f) {
          const float k = (odd && s.dot) ? -1.0f : 0.0f;
          const float g = std::max(gap, 0.0f);
          const float right = std::floor(cx + g), left = std::floor(cx - g - th + k);
          const float top = std::floor(cy - g - th + k), bottom = std::floor(cy + g);
          rect(left, top, right, top + th, fill, outline);
          rect(left, bottom, right, bottom + th, fill, outline);
          rect(left, top, left + th, bottom + th, fill, outline);
          rect(right, top, right + th, bottom + th, fill, outline);
        }
        break;
      case kStaticQuad: {
        // 0x1bc4bc0: four arcs on the diagonals out to the gap (at least 1) plus the thickness, each 90 degrees times
        // the split ratio
        const float o = (odd && th >= 1.0f) ? 0.5f : 0.0f;
        const float span = kStaticQuadSpan * std::clamp(s.split_ratio, 0.0f, 1.0f) * kDegToRad;
        ring(2, cx - o, cy - o, std::max(1.0f, gap) + th, th, span, fill, outline);
        break;
      }
      default:
        break;
    }
  }

  const size_t main_shapes = out.size();  // [acct2-ocr] the crosshair itself (the countdown goes round it)
  const int ti = t, ci = c, ai = a;
  const int gi = Round(gap), li = Round(len);
  const int cxi = int(cx), cyi = int(cy);
  // Pixels i whose middles are within `half` of the crosshair's middle line, across: [lo, hi] inclusive.
  auto span_of = [&](float middle, float half, int* lo, int* hi) {
    *lo = int(std::ceil(middle - half - 0.5f));
    *hi = int(std::floor(middle + half - 0.5f));
  };

  // The iron sight dot while the sights are up.
  if (s.ads_dot && caps.sights && v.sights >= 0.5f) {
    const int d = std::max(2, Round(3.0f * scale * s.ironsight_dot_scale));
    float dot_rgba[4] = {1.0f, 0.25f, 0.25f, 1.0f};
    if (s.ironsight_use_color) {
      FillColor(s, v, dot_rgba);
      dot_rgba[3] = s.color_from == kColorConstant ? s.color.a / 255.0f : 1.0f;
    }
    Shape sh;
    int x0, x1, y0, y1;
    span_of(cx - off, float(d) * 0.5f, &x0, &x1);
    span_of(cy - off, float(d) * 0.5f, &y0, &y1);
    sh.x0 = float(x0);
    sh.y0 = float(y0);
    sh.x1 = float(x1);
    sh.y1 = float(y1);
    for (int i = 0; i < 4; ++i) sh.fill[i] = dot_rgba[i];
    out.push_back(sh);
  }

  // Friendly warning: the port's own mark (a ring with a bar) above the crosshair while a friendly is under it.
  const bool friendly = v.over_friendly || CaseOf(v.rgba, caps.dead_color) == kCaseFriendly;
  if (s.friendly_warning && friendly && draw_main) {
    const int r = std::max(3, Round(5.0f * scale));
    const int w = std::max(1, Round(1.5f * scale));
    const int my = cyi - std::max(gi + li, 0) - r - Round(6.0f * scale);
    float col[4];
    Color4(s.case_color[kCaseFriendly], 1.0f, col);
    Shape sh;
    sh.kind = 1;
    sh.x0 = cx - off;
    sh.y0 = float(my) + 0.5f;
    sh.x1 = float(r);
    sh.y1 = float(w);
    for (int i = 0; i < 4; ++i) sh.fill[i] = col[i];
    out.push_back(sh);
    Shape bar;
    int x0, x1;
    span_of(cx - off, float(r - w), &x0, &x1);
    bar.x0 = float(x0);
    bar.x1 = float(x1);
    bar.y0 = float(my - w / 2);
    bar.y1 = float(my - w / 2 + w - 1);
    for (int i = 0; i < 4; ++i) bar.fill[i] = col[i];
    out.push_back(bar);
  }

  // The grenade crosshair: a line-up mark under the middle once the grenade has been out for its delay.
  if (grenade_out && s.grenade_on[v.grenade] && v.grenade_seconds >= s.grenade_delay[v.grenade]) {
    const int step = std::max(4, li > 0 ? li : Round(6.0f * scale));
    const int top = cyi + std::max(gi, 0) + 1;
    rect(float(cxi - ci), float(top), float(cxi + ai), float(top + 3 * step + 1), fill, outline);
    for (int k = 1; k <= 3; ++k)
      rect(float(cxi - ci - step / 2), float(top + k * step - ci), float(cxi + ai + step / 2),
           float(top + k * step + ai), fill, outline);
  }

  // What is drawn without the crosshair (View::cross false: the game drew none this frame) starts here.
  const size_t extras_start = out.size();
  // [acct2-ocr] 2026-11-22 (owner 02:22: "It needs to be 1:1 with features") the scope as CS2 draws a sniper's: two
  // black lines across the view through the middle, Scope line width (1-6, for the settings' screen height) thick,
  // with Scopes Mine (in place of the game's picture) or Both (over it), as far in as the game's picture has faded in.
  const int scope_mode = std::clamp(s.special[kSpecialScope], 0, kSpecialModeCount - 1);
  if (caps.scope && v.scoped && (caps.special >> kSpecialScope & 1u) &&
      (scope_mode == kMine || scope_mode == kBoth)) {
    const int lw = std::max(1, Round(float(std::clamp(s.sniper_width, 1, 6)) * H / ref));
    const float vw = v.width > 0.0f ? v.width : H * 16.0f / 9.0f;
    const float black[4] = {0.0f, 0.0f, 0.0f, std::clamp(v.scope, 0.0f, 1.0f)};
    const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    const int after = lw / 2, before = lw - after;  // pixels before and after the middle line, as a thickness's
    rect(float(Round(cx - vw * 0.5f)), float(cyi - before), float(Round(cx + vw * 0.5f)), float(cyi + after), black,
         clear);
    rect(float(cxi - before), float(Round(cy - H * 0.5f)), float(cxi + after), float(Round(cy + H * 0.5f)), black,
         clear);
  }

  // The sniper's inaccuracy while scoped: a thin ring the size of the spread.
  if (caps.scope && v.scoped && s.sniper_show_inaccuracy && caps.spread) {
    const float r = std::max(2.0f, std::max(0.0f, v.spread) * H / 480.0f);
    Shape sh;
    sh.kind = 1;
    sh.x0 = cx - off;
    sh.y0 = cy - off;
    sh.x1 = r;
    sh.y1 = float(std::max(1, s.sniper_width));
    for (int i = 0; i < 3; ++i) sh.fill[i] = 0.0f;
    sh.fill[3] = 0.5f;
    out.push_back(sh);
  }

  // [acct2-ocr] 2026-11-15 (owner 20:21: "There is a grenade countdown crosshair in-game already") a cooked grenade's
  // time left as six marks round the crosshair, as the game's own countdown shows it: one goes at each sixth of the
  // fuse (Call of Duty 3's 0.83, 0.67, 0.5, 0.33 and 0.17 of it left), the rest clockwise from the top, in this
  // crosshair's color, thickness and outline, just outside what the crosshair reaches.
  if (v.countdown >= 0.0f) {
    const float mx = cx - off, my = cy - off;
    float most = float(std::max(ti, 2));
    for (size_t i = 0; i < main_shapes; ++i) {
      const Shape& m = out[i];
      if (m.kind == 0)
        most = std::max({most, std::fabs(m.x0 - mx), std::fabs(m.x1 + 1.0f - mx), std::fabs(m.y0 - my),
                         std::fabs(m.y1 + 1.0f - my)});
      else
        most = std::max(most, m.x1);
    }
    const int ring_t = std::max(1, ti);
    const float radius = most + float(std::max(2, Round(3.0f * scale))) + float(ring_t);
    int marks = 6;
    for (float left : {0.83f, 0.67f, 0.5f, 0.33f, 0.17f})
      if (v.countdown < left) --marks;
    for (int k = 0; k < marks; ++k) {
      Shape sh;
      sh.kind = 3;
      sh.x0 = mx;
      sh.y0 = my;
      sh.x1 = radius;
      sh.y1 = float(ring_t);
      sh.span = kPi / 3.0f - 0.24f;  // a sixth of the circle less a small gap
      sh.angle = float(k) * kPi / 3.0f;
      sh.grow0 = grow0;
      sh.grow1 = grow1;
      for (int i = 0; i < 4; ++i) {
        sh.fill[i] = fill[i];
        sh.outline[i] = outline[i];
      }
      out.push_back(sh);
    }
    // [acct2-ocr] 2026-11-22 without the crosshair: the scope's lines and ring (when scoped) and the marks
    if (!v.cross) out.erase(out.begin(), out.begin() + std::ptrdiff_t(extras_start));
  } else if (!v.cross) {
    out.erase(out.begin(), out.begin() + std::ptrdiff_t(extras_start));  // [acct2-ocr] 11-22 the scope's, if any
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

// [acct2-ocr] 2026-11-21 CS2's shot feedback kick (0x1bc43d0), once a frame: a shot adds to it, it is kept to 0-25,
// then it runs down at 42 a second. `shot_at` is the last shot's time (< 0: none). The preview fires as CS2's does: a
// shot of 15 every 0.2 to 0.6 seconds.
float RunKick(Store::Kick* k, double now, double shot_at, bool preview) {
  const float dt = k->at < 0.0 ? 0.0f : float(std::clamp(now - k->at, 0.0, 0.25));
  k->at = now;
  if (preview) {
    k->preview_timer -= dt;
    if (k->preview_timer < 0.0f) {
      k->kick += kKickPreview;
      k->seed = k->seed * 1664525u + 1013904223u;
      k->preview_timer = 0.2f + 0.4f * float(k->seed >> 8) / float(1u << 24);
    }
  } else if (shot_at >= 0.0 && shot_at > k->shot + 1e-3) {
    if (now - shot_at < 0.25) k->kick += kKickShot;  // a shot of this frame or the last few, not an old one
    k->shot = shot_at;
  }
  k->kick = k->kick < 0.0f ? 0.0f : std::min(kKickMost, k->kick);
  if (k->kick > 0.0f)
    k->kick -= kKickFall * dt;
  else
    k->kick = 0.0f;
  return k->kick;
}

// [acct2-ocr] 2026-11-21 the least spread of the last ten seconds, in half-second buckets: the resting spread for a
// game that doesn't give it (Call of Duty's spread rests at its least while the player stands still).
float TrackRest(Store::Rest* r, double now, float spread) {
  if (r->start < 0.0 || now < r->start || now - r->start > 10.0) {
    r->start = now;
    r->head = 0;
    r->least.fill(spread);
  }
  while (now - r->start >= 0.5) {
    r->start += 0.5;
    r->head = (r->head + 1) % int(r->least.size());
    r->least[size_t(r->head)] = spread;
  }
  r->least[size_t(r->head)] = std::min(r->least[size_t(r->head)], spread);
  return *std::min_element(r->least.begin(), r->least.end());
}

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
  // [acct2-ocr] 2026-11-21 what CS2 takes from its weapons: the resting spread and shot feedback's kick
  if (!preview && player >= 0 && player < kPlayers) {
    const size_t p = size_t(player);
    if (v.spread_rest < 0.0f && st.caps.spread && v.spread > 0.0f)
      v.spread_rest = TrackRest(&st.rest[p], now, v.spread);
    if (v.shot_kick < 0.0f) {
      const double shot = v.seconds_since_shot >= 0.0 ? now - v.seconds_since_shot : -1.0;
      v.shot_kick = RunKick(&st.kick[p], now, shot, false);
    }
  } else if (preview && v.shot_kick < 0.0f) {
    v.shot_kick = RunKick(&st.kick[kPlayers], now, -1.0, true);
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

// The preview's view: middle (cx, cy), `height` the view it is sized for; a dynamic style opens and closes, the
// grenade countdown runs while it is this player's.
View PreviewView(const Settings& s, float cx, float cy, float height) {
  View v;
  v.cx = cx;
  v.cy = cy;
  v.height = height;
  const double t = ImGui::GetTime();
  if (MovesWithSpread(s)) v.spread = float(3.0 + 3.0 * std::sin(t * 2.2));  // [acct2-ocr] 11-22 with T style too
  v.spread_rest = 0.0f;  // [acct2-ocr] 11-21 the least of that swing; shot feedback fires as CS2's preview does
  // [acct2-ocr] 2026-11-15 the grenade countdown while it is this player's (Mine or Both): a four-second fuse, a
  // second's rest between
  if ((S().caps.special >> kSpecialCountdown & 1u) &&
      (s.special[kSpecialCountdown] == kMine || s.special[kSpecialCountdown] == kBoth)) {
    const double cycle = std::fmod(t, 5.0);
    if (cycle < 4.0) v.countdown = float(1.0 - cycle / 4.0);
  }
  return v;
}

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
  View v = PreviewView(s, std::floor(p0.x + side * 0.5f), std::floor(p0.y + side * 0.5f), io.DisplaySize.y);
  v.rgba[3] = s.enabled ? 1.0f : 0.35f;
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

bool DrawPreviewAt(ImDrawList* dl, int player, float cx, float cy, float height) {
  if (!dl || player < 0 || player >= kPlayers || !(height > 0.0f)) return false;
  const Settings& s = Player(player);
  View v = PreviewView(s, cx, cy, height);
  v.rgba[3] = s.enabled ? 1.0f : 0.35f;  // dimmed while it is off, as F2's preview
  DrawSettings(dl, s, v, player, true);
  return true;
}

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
    const bool enter =
        ImGui::InputTextWithHint("##code", "CS2 crosshair code (CS... or an old CSGO-...)", code_text,
                                 sizeof(code_text), ImGuiInputTextFlags_EnterReturnsTrue);
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
  if (MovesWithSpread(s) && !caps.spread)
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
  // The dynamic rows for the dynamic styles (with T style too, [acct2-ocr] 11-22), greyed together, with one reason,
  // while the game gives no spread
  if (MovesWithSpread(s)) {
    Greyed g(caps.spread, "These need the game's spread, which this game's hook doesn't give yet.");
    if (UsesSpreadLimit(s.style)) edit |= SliderI("Dynamic spread limit", &s.spread_limit, 0, 255);
    if (s.style == kDynamicCrossLegacy) {
      edit |= SliderI("Split distance", &s.split_distance, 0, 127);
      edit |= SliderF("Inner split alpha", &s.split_inner_alpha, 0.0f, 1.0f, "%.2f");
      edit |= SliderF("Outer split alpha", &s.split_outer_alpha, 0.3f, 1.0f, "%.2f");
      edit |= SliderF("Split size ratio", &s.split_ratio, 0.0f, 1.0f, "%.2f");
    }
    edit |= SliderF("How far it opens", &s.open_scale, 0.0f, 3.0f, "%.2f x");
    if (caps.spread) Help("1 is exactly the game's own spread; lower keeps the crosshair tighter.");
  }
  if (s.style == kStaticQuad) edit |= SliderF("Static quad split ratio", &s.split_ratio, 0.0f, 1.0f, "%.2f");
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
    if (s.observer) {  // [acct2-bro] 2026-11-15: only means something with the box before it on
      ImGui::SameLine();
      edit |= ImGui::Checkbox("...also a bot", &s.observed_bot);
    }
  }

  Section("Grenades");
  {
    Greyed g(caps.grenades, "Needs to know which grenade is out, which this game's hook doesn't give yet.");
    edit |= ImGui::Checkbox("Keep my crosshair while a grenade is out", &s.grenade_keep_mine);
    for (int i = 0; i < kGrenadeCount; ++i) {
      ImGui::PushID(i);
      // [acct2-ocr] 2026-11-22 a kind the game has no grenade of stays greyed (Call of Duty 3: frag and smoke only)
      Greyed kind(!caps.grenades || (caps.grenade_kinds >> i & 1u), "This game has no grenade of this kind.");
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
    {  // [acct2-ocr] 2026-11-22 these two act on the game taking the scope down after a shot
      Greyed shots(!caps.scope || caps.scope_shots, "This game keeps the scope up through a shot.");
      edit |= ImGui::Checkbox("Delay unscope after a shot", &s.sniper_delay_unscope);
      edit |= ImGui::Checkbox("Zoom back in after a shot", &s.sniper_auto_rezoom);
    }
    edit |= ImGui::Checkbox("Show inaccuracy in the scope", &s.sniper_show_inaccuracy);
    edit |= SliderI("Scope line width", &s.sniper_width, 1, 6);
    if (caps.scope && (caps.special >> kSpecialScope & 1u))
      Help("The lines are drawn with Scopes set to Mine (in place of the game's picture) or Both (over it).");
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

  // [acct2-ocr] 2026-11-22 (owner 04:31: Titanfall 2's grenade arc) not part of the crosshair, so not greyed with it
  Section("Grenade arc");
  {
    const char* blocked = GrenadeArcBlocked();
    Greyed g(caps.grenade_arc && blocked == nullptr,
             blocked != nullptr ? blocked : "Needs the game's throw, which this game's hook doesn't give yet.");
    edit |= ImGui::Checkbox("Show where my grenade will land", &s.grenade_arc);
    if (s.grenade_arc) edit |= SliderI("Bounces to follow", &s.grenade_arc_bounces, 0, 3);
    Help("While you hold a grenade: its path as the game will throw it from where you look now, its bounces, and a "
         "ring where it comes to rest or goes off (a cooked grenade's fuse counts). In this crosshair's colours.");
  }

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
