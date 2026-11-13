// cod3 - [acct2-ocr] 2026-11-07 the port's settings as rows of the in-game Options pages (see menu_port_rows.h).

#include "game/menu_port_rows.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "game/fov.h"              // VehicleScale: the vehicle field of view, per player in cod3mp
#include "game/keybinds/binds.h"   // KeyboardClient: whose the keyboard and mouse are (cod3mp)
#include "game/mouse_look.h"       // [f2-menus] ctl 14:50 INVERT MOUSE as the mouse is
#include "game/program.h"
#include "game/protected_dvars.h"  // SaveSettings: the in-game menus' save path
#include "game/text_language.h"    // the Text setting's languages; PortText
#include "game/use_hold.h"         // the Use hold per player
#include "game/voice_language.h"
#include "game/voice_paths.h"      // kCodes, kNames

namespace cod3::portrows {
namespace {

enum class Kind : uint8_t {
  kToggle,  // ON / OFF: a bool setting
  kChoice,  // one of `choices`: a string setting
  kNumber,  // a number in [min, max]: a slider row
  kOwn,     // values and steps of its own (below)
};
constexpr uint8_t kSp = 1, kMp = 2, kBoth = kSp | kMp;

struct Choice {
  const char* value;  // the setting's
  const char* shown;  // the row's
};
constexpr Choice kButtonStyles[] = {{"auto", "AUTO"},         {"xbox", "XBOX"},         {"playstation", "PLAYSTATION"},
                                    {"playstation2", "PLAYSTATION 2"}, {"nintendo", "NINTENDO"}, {"gamecube", "GAMECUBE"}};
constexpr Choice kButtonColorModes[] = {{"original", "ORIGINAL"}, {"gold", "GOLD"},  {"rainbow", "RAINBOW"},
                                        {"rainbow_codes", "COLOR CODES"}, {"custom", "CUSTOM"}};

struct Info {
  const char* key;
  const char* label;
  const char* cvar;   // player 1's setting
  Kind kind;
  uint8_t where;      // kSp, kMp or both
  bool mouse;         // the keyboard and mouse's: other players' menus show it, don't change it
  const char* note;   // the line under the rows (kOwn rows: NoteFor's own)
  // kNumber (and VEHICLE FOV SCALE): the range, the arrows' step, the typed values' step, how the box shows it
  double min, max, step, typed, shown;
  int decimals;
  const char* suffix;
  bool integer;  // an INT32 setting
  const Choice* choices;
  int choice_count;
};

constexpr Info Toggle(const char* key, const char* label, const char* cvar, uint8_t where, bool p1, const char* note) {
  return {key, label, cvar, Kind::kToggle, where, p1, note, 0, 0, 0, 0, 1, 0, "", false, nullptr, 0};
}
constexpr Info NumberRow(const char* key, const char* label, const char* cvar, uint8_t where, bool p1, const char* note,
                      double min, double max, double step, double typed, double shown, int decimals,
                      const char* suffix, bool integer) {
  return {key, label, cvar, Kind::kNumber, where, p1, note, min, max, step, typed, shown, decimals, suffix, integer,
          nullptr, 0};
}
template <size_t N>
constexpr Info Choices(const char* key, const char* label, const char* cvar, uint8_t where, const char* note,
                       const Choice (&choices)[N]) {
  return {key, label, cvar, Kind::kChoice, where, false, note, 0, 0, 0, 0, 1, 0, "", false, choices, int(N)};
}
constexpr Info Own(const char* key, const char* label, const char* cvar, uint8_t where) {
  return {key, label, cvar, Kind::kOwn, where, false, "", 0, 0, 0, 0, 1, 0, "", false, nullptr, 0};
}

// In Row's order.
constexpr Info kInfo[kRowCount] = {
    Toggle("COD3_PORT_SKIP_CUTSCENES", "SKIP CUTSCENES", "cod3_skip_movies", kSp, false,
           "LOGOS, BRIEFINGS AND LEVEL INTROS END AS SOON AS THE GAME ALLOWS"),
    NumberRow("COD3_PORT_SKIP_HOLD", "HOLD TO SKIP", "cod3_skip_hold_ms", kSp, false,
           "HOW LONG SKIP IS HELD ON A BRIEFING (0: A PRESS SKIPS)", 0, 5000, 100, 100, 0.001, 1, " S", true),
    Toggle("COD3_PORT_CUTSCENE_BARS", "CUTSCENE BARS", "cod3_letterbox", kSp, false,
           "THE BLACK BARS OF THE SCENES INSIDE A MISSION (OFF: THE WHOLE PICTURE)"),
    Own("COD3_PORT_USE_HOLD", "USE HOLD TIME", "cod3_use_hold_ms", kBoth),
    Own("COD3_PORT_LEFT_HANDED", "LEFT-HANDED WEAPON", "cod3_viewmodel_flip", kBoth),
    Own("COD3_PORT_VEHICLE_FOV", "VEHICLE FOV SCALE", "cod3_vehicle_fov_scale", kBoth),
    Own("COD3_PORT_TEXT_LANGUAGE", "TEXT LANGUAGE", "cod3_text_language", kBoth),
    Own("COD3_PORT_VOICES", "VOICES", "cod3_voice_language", kBoth),
    NumberRow("COD3_PORT_VOLUME", "MASTER VOLUME", "cod3_volume", kBoth, false,
           "EVERYTHING THE GAME PLAYS (100%: AS THE GAME MIXES IT)", 0.0, 2.0, 0.05, 0.01, 100.0, 0, "%", false),
    Toggle("COD3_PORT_KEY_PROMPTS", "KEYS IN PROMPTS", "cod3_key_prompts", kBoth, false,
           "THE BOUND KEYS INSTEAD OF BUTTONS WHILE THE KEYBOARD AND MOUSE ARE IN USE"),
    Toggle("COD3_PORT_KEY_CAPS", "KEYS AS KEY CAPS", "cod3_key_caps", kBoth, false,
           "THE PROMPTS' KEYS DRAWN AS KEY CAPS, MOUSE BUTTONS AS A MOUSE"),
    Choices("COD3_PORT_BUTTON_STYLE", "BUTTON PICTURES", "cod3_glyph_style", kBoth,
            "AUTO: EACH PLAYER'S OWN CONTROLLER'S BUTTONS", kButtonStyles),
    Choices("COD3_PORT_BUTTON_COLORS", "BUTTON COLORS", "cod3_glyph_color_mode", kBoth,
            "THE PROMPTS' BUTTONS AND KEYS (CUSTOM: F2'S COLORS)", kButtonColorModes),
    Toggle("COD3_PORT_RAW_MOUSE", "RAW MOUSE", "cod3_raw_mouse", kBoth, true,
           "AIM WITH THE MOUSE ITSELF (OFF: THE MOUSE MOVES THE RIGHT STICK)"),
    Toggle("COD3_PORT_INVERT_MOUSE", "INVERT MOUSE", "cod3_invert_mouse", kBoth, true,
           "THE MOUSE'S UP AND DOWN (THE STICKS HAVE INVERT AIM)"),
    NumberRow("COD3_PORT_MOUSE_SENS", "MOUSE SENSITIVITY", "cod3_sensitivity", kMp, true,
           "WORLD AT WAR'S SCALE (5 IS ITS DEFAULT)", 0.1, 30.0, 0.25, 0.05, 1.0, 2, "", false),
    NumberRow("COD3_PORT_MOUSE_ADS", "MOUSE ADS SCALE", "cod3_zoom_sensitivity", kBoth, true,
           "THE MOUSE WHILE AIMING DOWN THE SIGHTS, ON TOP OF THE ZOOM", 0.05, 4.0, 0.05, 0.05, 1.0, 2, "", false),
    Toggle("COD3_PORT_MOUSE_SMOOTH", "EVEN MOUSE TURNING", "cod3_mouse_resample", kBoth, true,
           "THE MOUSE'S MOTION SPREAD OVER THE FRAMES AT HIGH FRAME RATES"),
    Toggle("COD3_PORT_COLOR_CODES", "COLORS IN NAMES", "cod3_color_codes_in_text", kBoth, false,
           "COLOR CODES ANYWHERE IN A NAME, AS IN WORLD AT WAR (OFF: THE GAME'S)"),
    Toggle("COD3_PORT_SHOW_VERSION", "SHOW VERSION", "cod3_show_version", kBoth, false,
           "THE SETUP TOOL'S VERSION, SMALL IN THE TOP RIGHT CORNER"),
    Own("COD3_PORT_SPLIT_HUD", "SPLIT-SCREEN HUD", "cod3_hud_safe_area", kMp),
};

// The weapon hand of players 1 to 4 (game/viewmodel_flip.cpp).
constexpr const char* kFlipSetting[4] = {"cod3_viewmodel_flip", "cod3_viewmodel_flip_2", "cod3_viewmodel_flip_3",
                                         "cod3_viewmodel_flip_4"};
constexpr int kUseHoldStep = 50;     // ms
constexpr int kUseHoldMax = 1000;    // the setting's range: -1 (the game's; players 2-4 also -2) to 1000
constexpr int kSplitHudLow = 50;     // cod3_hud_safe_area: 0 (the game's), else 50 to 93 percent
constexpr int kSplitHudHigh = 93;
constexpr double kVehicleMin = 0.5, kVehicleMax = 2.0, kVehicleStep = 0.05;

// Guest copies of the label texts, made once (the game keeps the pointers).
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
int GetInt(const char* name, int fallback) {
  const std::string v = Get(name);
  return v.empty() ? fallback : std::atoi(v.c_str());
}
double GetDouble(const char* name, double fallback) {
  const std::string v = Get(name);
  if (v.empty()) return fallback;
  const double d = std::atof(v.c_str());
  return std::isfinite(d) ? d : fallback;
}
bool GetBool(const char* name) {
  const std::string v = Get(name);
  return v == "true" || v == "1";
}
bool SameText(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(uint8_t(a[i])) != std::tolower(uint8_t(b[i]))) return false;
  return true;
}
std::string Upper(std::string s) {
  for (char& c : s) c = char(std::toupper(uint8_t(c)));
  return s;
}

int Players() { return cod3::kMultiplayer ? 4 : 1; }
bool ValidPlayer(int player) { return player >= 0 && player < Players(); }
bool ValidRow(int row) { return row >= 0 && row < kRowCount; }

// A setting's text for a number: whole for an INT32 setting, else up to four decimals without trailing zeros.
std::string NumberText(double v, bool integer) {
  if (integer) return std::to_string(int(std::lround(v)));
  std::string s = fmt::format("{:.4f}", v);
  while (!s.empty() && s.back() == '0') s.pop_back();
  if (!s.empty() && s.back() == '.') s.pop_back();
  return s.empty() ? "0" : s;
}

// As the slider's box shows it: "1.0 S", "100%", "1.25".
std::string Shown(double v, const Info& in) {
  return fmt::format("{:.{}f}{}", v * in.shown, in.decimals, in.suffix);
}

// The next multiple of `step` past `value` (a value set in F2 or the console can lie between two), within the range.
double NextStep(double value, double step, bool right, double min, double max) {
  const double steps = value / step;
  const double next = right ? std::floor(steps + 1e-6) + 1.0 : std::ceil(steps - 1e-6) - 1.0;
  return std::clamp(std::round(next * step * 1e6) / 1e6, min, max);
}

bool SetText(const char* name, const std::string& value) {
  if (Get(name) == value) return false;
  return rex::cvar::SetFlagByName(name, value);
}

// --- the rows of their own -------------------------------------------------------------------------------------------

// USE HOLD TIME, the player's own (game/use_hold.h; owner 23:40 "it's per person"). Player 1: -1 the game's own
// (g_useholdtime, 250 ms), 0 a press, else milliseconds. cod3mp's players 2 to 4 also -1 = player 1's (the start)
// and -2 = the game's own: PLAYER 1'S, GAME'S, PRESS, 50 MS ... 1000 MS.
std::string UseHoldText(int player, int v) {
  if (player > 0 && v == -1) return "PLAYER 1'S";
  if (v < 0) return "GAME'S";
  return v == 0 ? "PRESS" : fmt::format("{} MS", v);
}
bool StepUseHold(int player, bool right) {
  const char* name = use_hold::SettingName(player);
  if (!name) return false;
  const int v = use_hold::OwnSettingMs(player);
  // The order of the special values: player 1 GAME'S (-1), PRESS; players 2-4 PLAYER 1'S (-1), GAME'S (-2), PRESS.
  const int game = player > 0 ? -2 : -1;
  int next = v;
  if (v < 0) {
    if (player > 0 && v == -1)
      next = right ? -2 : -1;
    else
      next = right ? 0 : (player > 0 ? -1 : game);
  } else if (v == 0) {
    next = right ? kUseHoldStep : game;
  } else {
    next = int(std::lround(NextStep(v, kUseHoldStep, right, 0, kUseHoldMax)));
  }
  return next != v && rex::cvar::SetFlagByName(name, std::to_string(next));
}

// Whether `player` has the keyboard and mouse: the campaign's player 1; in cod3mp the local client on controller 0
// (binds::KeyboardClient).
bool KeyboardPlayer(int player) {
#if defined(COD3_MP)
  auto* ks = rex::system::kernel_state();
  uint8_t* base = ks && ks->memory() ? ks->memory()->virtual_membase() : nullptr;
  return base ? int(binds::KeyboardClient(base)) == player : player == 0;
#else
  return player == 0;
#endif
}

// LEFT-HANDED WEAPON: the player's own.
const char* FlipSetting(int player) { return kFlipSetting[std::clamp(player, 0, 3)]; }

// VEHICLE FOV SCALE: player 1's cod3_vehicle_fov_scale; cod3mp's players 2 to 4 their own (-1: player 1's). Stepping
// a player 2-4's onto player 1's value hands it back, as STICK DEAD ZONE does.
bool SetVehicle(int player, double v) {
  v = std::clamp(v, kVehicleMin, kVehicleMax);
  if (player == 0) {
    if (std::fabs(v - fov::VehicleScale(0)) < 1e-6) return false;
    fov::SetVehicleScale(0, v);
    return true;
  }
  const double own = fov::OwnVehicleScale(player);
  const bool back = std::fabs(v - fov::VehicleScale(0)) < 1e-6;
  if (back ? own < 0.0 : std::fabs(v - own) < 1e-6) return false;
  fov::SetVehicleScale(player, back ? -1.0 : v);
  return true;
}

// TEXT LANGUAGE: the game's own (-1) or one of the Text setting's languages; short names for the row.
struct ShortName {
  const char* code;
  const char* name;
};
constexpr ShortName kShortNames[] = {
    {"en-US", "ENGLISH (US)"},      {"en-GB", "ENGLISH (UK)"},       {"fr-FR", "FRANCAIS"},
    {"fr-CA", "FRANCAIS (CANADA)"}, {"fr-BE", "FRANCAIS (BELGIQUE)"}, {"de-DE", "DEUTSCH"},
    {"it-IT", "ITALIANO"},          {"es-ES", "ESPANOL"},            {"es-419", "ESPANOL (LATAM)"},
    {"pt-BR", "PORTUGUES (BRASIL)"}, {"pt-PT", "PORTUGUES"},         {"ru-RU", "RUSSIAN"},
    {"zh-Hans", "CHINESE (SIMPLIFIED)"}, {"zh-Hant-TW", "CHINESE (TAIWAN)"}, {"zh-Hant-HK", "CHINESE (HONG KONG)"},
    {"ja-JP", "JAPANESE"},          {"ko-KR", "KOREAN"},
};
std::string LanguageText(int i) {
  if (i < 0) return "GAME'S";
  const char* code = text::LanguageCode(i);
  for (const ShortName& s : kShortNames)
    if (std::strcmp(code, s.code) == 0) return s.name;
  return Upper(text::LanguageName(i));
}
// The next language that can be chosen, the game's own included, going round.
int NextLanguage(int from, bool right) {
  const int n = text::LanguageCount();
  int i = from;
  for (int k = 0; k <= n; ++k) {
    i = right ? (i + 1 > n - 1 ? -1 : i + 1) : (i - 1 < -1 ? n - 1 : i - 1);
    if (i < 0 || text::LanguageUsable(i)) return i;
  }
  return from;
}

// VOICES: the game's own (-1) or an installed one (game/voice_language.h).
int NextVoice(int from, bool right) {
  const auto have = voice::Available();
  int i = from;
  for (int k = 0; k <= voice::kLanguages; ++k) {
    i = right ? (i + 1 > voice::kLanguages - 1 ? -1 : i + 1) : (i - 1 < -1 ? voice::kLanguages - 1 : i - 1);
    if (i < 0 || have[size_t(i)] > 0) return i;
  }
  return from;
}

// SPLIT-SCREEN HUD: 0 the game's (about 84 percent), else 50 to 93 percent of each view.
int SplitHud() { return std::clamp(GetInt("cod3_hud_safe_area", 0), 0, kSplitHudHigh); }
bool StepSplitHud(bool right) {
  const int v = SplitHud();
  int next = v;
  if (right)
    next = v < kSplitHudLow ? kSplitHudLow : std::min(v + 1, kSplitHudHigh);
  else
    next = v <= kSplitHudLow ? 0 : v - 1;
  return next != v && rex::cvar::SetFlagByName("cod3_hud_safe_area", std::to_string(next));
}

bool CanChange(int player, int row) {
  return ValidRow(row) && ValidPlayer(player) && Available(row) && !(kInfo[row].mouse && !KeyboardPlayer(player));
}

void Saved(int player, int row, const char* how) {
  protected_dvars::SaveSettings();
  REXLOG_INFO("cod3: Options: {} = {} (player {}{})", LabelText(row), ValueFor(player, row), player + 1, how);
}

}  // namespace

bool Available(int row) {
  return ValidRow(row) && (kInfo[row].where & (cod3::kMultiplayer ? kMp : kSp)) != 0;
}

const char* LabelKey(int row) { return ValidRow(row) ? kInfo[row].key : kInfo[0].key; }
const char* LabelText(int row) { return ValidRow(row) ? kInfo[row].label : ""; }

std::string LabelShown(uint8_t* base, int row) {
  if (!ValidRow(row)) return {};
  if (base) {
    if (const uint32_t text = text::PortText(base, kInfo[row].key)) {
      const char* s = reinterpret_cast<const char*>(base + text);
      const size_t len = strnlen(s, 128);
      if (len > 0 && len < 128) return std::string(s, len);
    }
  }
  return kInfo[row].label;
}

std::string ValueFor(int player, int row) {
  if (!ValidRow(row) || !ValidPlayer(player)) return {};
  const Info& in = kInfo[row];
  switch (in.kind) {
    case Kind::kToggle:
      // [f2-menus] 2026-11-11 ctl 14:50 INVERT MOUSE: the mouse as it is (with the game's own Invert aim), as F2's box.
      if (std::string_view(in.cvar) == "cod3_invert_mouse") return MouseLook::MouseInverted() ? "ON" : "OFF";
      return GetBool(in.cvar) ? "ON" : "OFF";
    case Kind::kChoice: {
      const std::string v = Get(in.cvar);
      for (int i = 0; i < in.choice_count; ++i)
        if (SameText(v, in.choices[i].value)) return in.choices[i].shown;
      return Upper(v);
    }
    case Kind::kNumber:
      return Shown(std::clamp(GetDouble(in.cvar, in.min), in.min, in.max), in);
    case Kind::kOwn:
      break;
  }
  switch (row) {
    case kUseHold: return UseHoldText(player, use_hold::OwnSettingMs(player));
    case kLeftHanded: return GetBool(FlipSetting(player)) ? "ON" : "OFF";
    case kVehicleFov: return fmt::format("{:.2f}", fov::VehicleScale(player));
    case kTextLanguage: return LanguageText(text::ChosenLanguage());
    case kVoices: {
      const int v = voice::Chosen();
      return v < 0 || v >= voice::kLanguages ? std::string("GAME'S") : Upper(voice::kNames[v]);
    }
    case kSplitHud: {
      const int v = SplitHud();
      return v == 0 ? std::string("GAME'S") : fmt::format("{}%", v);
    }
  }
  return {};
}

std::string NoteFor(int player, int row) {
  if (!ValidRow(row) || !ValidPlayer(player)) return {};
  const Info& in = kInfo[row];
  if (in.mouse && !KeyboardPlayer(player)) return "THE KEYBOARD AND MOUSE PLAYER'S";
  if (in.kind != Kind::kOwn) return in.note;
  switch (row) {
    case kUseHold:
      if (player > 0 && use_hold::OwnSettingMs(player) == -1)
        return "PLAYER 1'S UNTIL CHANGED HERE: HOW LONG USE IS HELD (THE GAME'S: 250 MS)";
      return cod3::kMultiplayer ? "THIS PLAYER'S OWN: HOW LONG USE IS HELD (THE GAME'S: 250 MS)"
                                : "HOW LONG USE IS HELD FOR A WEAPON, A GUN OR A VEHICLE (THE GAME'S: 250 MS)";
    case kLeftHanded:
      return cod3::kMultiplayer ? "THIS PLAYER'S WEAPON IN THE LEFT HAND, MIRRORED"
                                : "THE WEAPON IN THE LEFT HAND, MIRRORED";
    case kVehicleFov:
      if (player > 0)
        return fov::OwnVehicleScale(player) > 0.0
                   ? fmt::format("THIS PLAYER'S OWN, IN VEHICLES AND ON A TANK'S GUN (PLAYER 1'S: {:.2f})",
                                 fov::VehicleScale(0))
                   : std::string("PLAYER 1'S UNTIL CHANGED HERE: IN VEHICLES AND ON A TANK'S GUN");
      return "THE FIELD OF VIEW IN VEHICLES AND ON A TANK'S GUN (1.00: THE GAME'S)";
    case kTextLanguage: {
      const int i = text::ChosenLanguage();
      if (i >= 0 && !text::LanguageUsable(i)) return "NOT INSTALLED: TEXT/<LANGUAGE>.TXT IN THE GAME'S FOLDER";
      if (i >= 0 && text::LanguageAfterRestart(i)) return "APPLIES AFTER A RESTART (ITS SCRIPT IS DRAWN BY THE PORT)";
      return "MENUS, HINTS, OBJECTIVES AND SUBTITLES, FROM THE NEXT TEXT THE GAME ASKS FOR";
    }
    case kVoices: {
      const int v = voice::Chosen();
      if (v >= 0 && v < voice::kLanguages && voice::Available()[size_t(v)] == 0)
        return "NOT INSTALLED: THE SETUP TOOL ADDS ANOTHER DISC'S VOICES";
      return cod3::kMultiplayer ? "THE SOLDIERS' LANGUAGE, HEARD FROM THE NEXT MAP"
                                : "THE SOLDIERS' LANGUAGE (ANOTHER DISC'S: THE SETUP TOOL'S PLAY PAGE)";
    }
    case kSplitHud:
      return "HOW MUCH OF EACH VIEW THE HUD SPREADS OVER, FROM THE NEXT SPLIT (GAME'S: ABOUT 84%)";
  }
  return {};
}

std::string StepFor(int player, int row, bool right) {
  if (!CanChange(player, row)) return "";
  const Info& in = kInfo[row];
  bool changed = false;
  switch (in.kind) {
    case Kind::kToggle:
      // [f2-menus] 2026-11-11 ctl 14:50 INVERT MOUSE flips the mouse as it is (F2's Invert vertical does the same).
      if (std::string_view(in.cvar) == "cod3_invert_mouse") {
        const bool was = MouseLook::MouseInverted();
        MouseLook::SetMouseInverted(!was);
        changed = MouseLook::MouseInverted() != was;
        break;
      }
      changed = rex::cvar::SetFlagByName(in.cvar, GetBool(in.cvar) ? "false" : "true");
      // [f2-menus] 2026-11-11 ctl 14:50 RAW MOUSE is F2's Mouse aim: off hands the mouse to the right stick (mnk_mouse),
      // on takes it back, so the view never turns twice (off used to leave the mouse no aim at all).
      if (changed && std::string_view(in.cvar) == "cod3_raw_mouse")
        rex::cvar::SetFlagByName("mnk_mouse", GetBool("cod3_raw_mouse") ? "false" : "true");
      break;
    case Kind::kChoice: {
      const std::string v = Get(in.cvar);
      int at = -1;
      for (int i = 0; i < in.choice_count; ++i)
        if (SameText(v, in.choices[i].value)) at = i;
      const int next = at < 0 ? 0 : (at + (right ? 1 : in.choice_count - 1)) % in.choice_count;
      changed = SetText(in.cvar, in.choices[next].value);
      break;
    }
    case Kind::kNumber: {
      const double v = std::clamp(GetDouble(in.cvar, in.min), in.min, in.max);
      const double next = NextStep(v, in.step, right, in.min, in.max);
      changed = SetText(in.cvar, NumberText(next, in.integer));
      break;
    }
    case Kind::kOwn:
      switch (row) {
        case kUseHold: changed = StepUseHold(player, right); break;
        case kLeftHanded:
          changed = rex::cvar::SetFlagByName(FlipSetting(player), GetBool(FlipSetting(player)) ? "false" : "true");
          break;
        case kVehicleFov:
          changed = SetVehicle(player,
                               NextStep(fov::VehicleScale(player), kVehicleStep, right, kVehicleMin, kVehicleMax));
          break;
        case kTextLanguage: {
          const int now = text::ChosenLanguage();
          changed = text::ChooseLanguage(NextLanguage(now, right));
          break;
        }
        case kVoices: {
          const int now = voice::Chosen();
          const int next = NextVoice(now, right);
          if (next != now)
            changed = rex::cvar::SetFlagByName("cod3_voice_language", next < 0 ? "disc" : voice::kCodes[next]);
          break;
        }
        case kSplitHud: changed = StepSplitHud(right); break;
      }
      break;
  }
  if (!changed) return "";
  Saved(player, row, "");
  return ValueFor(player, row);
}

bool NumberOfFor(int player, int row, menuslider::Number* n, double* value) {
  if (!ValidRow(row) || !ValidPlayer(player) || !n || !value) return false;
  const Info& in = kInfo[row];
  if (in.kind == Kind::kNumber) {
    *n = menuslider::Number{in.min, in.max, in.typed, in.shown, in.decimals, in.suffix};
    *value = std::clamp(GetDouble(in.cvar, in.min), in.min, in.max);
    return true;
  }
  if (row == kVehicleFov) {
    *n = menuslider::Number{kVehicleMin, kVehicleMax, kVehicleStep, 1.0, 2, ""};
    *value = fov::VehicleScale(player);
    return true;
  }
  return false;
}

std::string SetNumberFor(int player, int row, double value) {
  if (!CanChange(player, row) || !std::isfinite(value)) return "";
  const Info& in = kInfo[row];
  bool changed = false;
  if (in.kind == Kind::kNumber) {
    const double typed = in.typed > 0.0 ? in.min + std::round((value - in.min) / in.typed) * in.typed : value;
    changed = SetText(in.cvar, NumberText(std::clamp(typed, in.min, in.max), in.integer));
  } else if (row == kVehicleFov) {
    changed = SetVehicle(player, kVehicleMin + std::round((value - kVehicleMin) / kVehicleStep) * kVehicleStep);
  } else {
    return "";
  }
  if (!changed) return "";
  Saved(player, row, ", typed");
  return ValueFor(player, row);
}

uint32_t Localize(uint8_t* base, const char* key) {
  if (!key || std::strncmp(key, "COD3_PORT_", 10) != 0) return 0;
  for (const Info& in : kInfo)
    if (std::strcmp(key, in.key) == 0) return FixedText(base, in.key, in.label);
  return 0;
}

}  // namespace cod3::portrows
