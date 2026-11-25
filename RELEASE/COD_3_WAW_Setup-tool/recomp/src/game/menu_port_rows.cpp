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
#include <utility>  // [acct2-bro Text lane] 2026-11-22 std::pair (ChoiceShown)

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "f2/f2_crosshair_cs2.h"   // [acct2-ocr] 2026-11-21 the CROSSHAIR page: each player's crosshair
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
    // [acct2-ocr] 2026-11-21 the CROSSHAIR page (the setting: the menu's player's, cod3_crosshair_p1 .. _p4)
    Own("COD3_PORT_XHAIR_STYLE", "STYLE", "cod3_crosshair_p1", kBoth),
    Own("COD3_PORT_XHAIR_DOT", "CENTER DOT", "cod3_crosshair_p1", kBoth),
    Own("COD3_PORT_XHAIR_LENGTH", "LENGTH", "cod3_crosshair_p1", kBoth),
    Own("COD3_PORT_XHAIR_THICKNESS", "THICKNESS", "cod3_crosshair_p1", kBoth),
    Own("COD3_PORT_XHAIR_GAP", "GAP", "cod3_crosshair_p1", kBoth),
    Own("COD3_PORT_XHAIR_OUTLINE", "OUTLINE", "cod3_crosshair_p1", kBoth),
    Own("COD3_PORT_XHAIR_COLOR", "COLOR", "cod3_crosshair_p1", kBoth),
    Own("COD3_PORT_XHAIR_ALPHA", "ALPHA", "cod3_crosshair_p1", kBoth),
    Own("COD3_PORT_XHAIR_T", "T STYLE", "cod3_crosshair_p1", kBoth),
};
static_assert(std::size(kInfo) == size_t(kRowCount));

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
// and -2 = the game's own.
// [rexglue-native] 2026-11-24 (owner 06:15: "our options for hold to press needs to be revisited and corrected.
// Please just logically fix it"): one scale in order, PRESS, 50 MS ... 200 MS, GAME'S (250 MS), 300 MS ... 1000 MS
// (players 2 to 4: PLAYER 1'S first). Before, GAME'S came before PRESS (right from 250 ms went down to 0) and 250 MS
// showed again further on as a value of its own; the game's own is now at its place on the scale, and a stored 250
// counts as the game's (the same hold).
constexpr int kUseHoldGame = 250;  // g_useholdtime with every controller layout
// Its place on the scale: -1 for PLAYER 1'S (players 2 to 4), else milliseconds (the game's own at 250).
int UseHoldPlace(int player, int v) {
  if (player > 0 && v == -1) return -1;
  return v < 0 ? kUseHoldGame : v;
}
// The setting's value for a place: the game's own is stored as such (-1; players 2 to 4: -2), not as 250.
int UseHoldValue(int player, int place) {
  if (place < 0) return -1;
  if (place == kUseHoldGame) return player > 0 ? -2 : -1;
  return place;
}
std::string UseHoldText(int player, int v) {
  // [acct2-bro Text lane] 2026-11-22 the values in the chosen language (COD3_PORT_V_* "@port" rows), else the English
  const int place = UseHoldPlace(player, v);
  if (place < 0) return Words("COD3_PORT_V_PLAYER1", "PLAYER 1'S");
  if (place == kUseHoldGame) return Words("COD3_PORT_V_GAMES", "GAME'S") + " (250 MS)";
  return place == 0 ? Words("COD3_PORT_V_PRESS", "PRESS") : fmt::format("{} MS", place);
}
bool StepUseHold(int player, bool right) {
  const char* name = use_hold::SettingName(player);
  if (!name) return false;
  const int v = use_hold::OwnSettingMs(player);
  const int place = UseHoldPlace(player, v);
  int next_place;
  if (right)
    next_place = place < 0 ? 0 : int(std::lround(NextStep(place, kUseHoldStep, true, 0, kUseHoldMax)));
  else
    next_place = place <= 0 ? (player > 0 ? -1 : 0)
                            : int(std::lround(NextStep(place, kUseHoldStep, false, 0, kUseHoldMax)));
  const int next = UseHoldValue(player, next_place);
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
  if (i < 0) return Words("COD3_PORT_V_GAMES", "GAME'S");  // [acct2-bro Text lane] 2026-11-22
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

// --- [acct2-ocr] 2026-11-21 the CROSSHAIR page ---------------------------------------------------------------------
// The menu's player's crosshair (f2/f2_crosshair_cs2.h): read as it is now (a change made here a moment ago
// included), changed by asking (written into the player's text setting at once, so the save below keeps it; F2 and
// the crosshair take it in on their next frame). CS2's ranges and steps; its colors' five (cl_crosshaircolor 0-4).
namespace xh = f2::xhair;

struct Word {
  const char* key;
  const char* english;
};
constexpr Word kOnWord{"COD3_PORT_V_ON", "ON"};
constexpr Word kOffWord{"COD3_PORT_V_OFF", "OFF"};
constexpr Word kGamesWord{"COD3_PORT_V_GAMES", "GAME'S"};
constexpr Word kStyleWords[xh::kStyleCount] = {
    {"COD3_PORT_XHAIR_S0", "DYNAMIC CROSS"},  {"COD3_PORT_XHAIR_S1", "DYNAMIC CIRCLE"},
    {"COD3_PORT_XHAIR_S2", "DYNAMIC CROSS (LEGACY)"}, {"COD3_PORT_XHAIR_S3", "STATIC CIRCLE"},
    {"COD3_PORT_XHAIR_S4", "STATIC CROSS"},   {"COD3_PORT_XHAIR_S5", "STATIC CROSS (SHOT FEEDBACK)"},
    {"COD3_PORT_XHAIR_S6", "DOT ONLY"},       {"COD3_PORT_XHAIR_S7", "DYNAMIC QUAD"},
    {"COD3_PORT_XHAIR_S8", "STATIC SQUARE"},  {"COD3_PORT_XHAIR_S9", "STATIC QUAD"}};
struct Preset {
  Word word;
  uint8_t r, g, b;
};
constexpr Preset kPresets[] = {{{"COD3_PORT_XHAIR_C_RED", "RED"}, 250, 50, 50},
                               {{"COD3_PORT_XHAIR_C_GREEN", "GREEN"}, 50, 250, 50},
                               {{"COD3_PORT_XHAIR_C_YELLOW", "YELLOW"}, 250, 250, 50},
                               {{"COD3_PORT_XHAIR_C_BLUE", "BLUE"}, 50, 50, 250},
                               {{"COD3_PORT_XHAIR_C_CYAN", "CYAN"}, 50, 250, 250}};
constexpr int kPresetCount = int(std::size(kPresets));
constexpr Word kCustomWord{"COD3_PORT_XHAIR_C_CUSTOM", "CUSTOM"};
constexpr Word kOwnColorsWord{"COD3_PORT_XHAIR_C_OWN", "OWN COLORS"};
constexpr Word kOutlineWords[3] = {kOffWord, {"COD3_PORT_XHAIR_O_FULL", "FULL"}, {"COD3_PORT_XHAIR_O_HALF", "HALF"}};
constexpr Word kPageWord{"COD3_PORT_XHAIR_PAGE", "CROSSHAIR"};
constexpr Word kTitleWord{"COD3_PORT_XHAIR_TITLE", "OPTIONS / CROSSHAIR"};
// The notes.
constexpr Word kNoteStyleOff{"COD3_PORT_XHAIR_N_STYLE_OFF", "THE GAME'S OWN CROSSHAIR. CHOOSE A STYLE FOR YOUR OWN"};
constexpr Word kNoteDynamic{"COD3_PORT_XHAIR_N_STYLE_DYNAMIC", "OPENS AND CLOSES WITH THE WEAPON'S SPREAD"};
constexpr Word kNoteStatic{"COD3_PORT_XHAIR_N_STYLE_STATIC", "KEEPS ITS SIZE WHILE MOVING AND FIRING"};
constexpr Word kNoteOff{"COD3_PORT_XHAIR_N_OFF", "FOR YOUR OWN CROSSHAIR, ONCE A STYLE IS CHOSEN"};
constexpr Word kNoteUnused{"COD3_PORT_XHAIR_N_UNUSED", "NOT USED BY THIS STYLE"};
constexpr Word kNoteDot{"COD3_PORT_XHAIR_N_DOT", "A DOT IN THE MIDDLE OF THE CROSSHAIR"};
constexpr Word kNoteLength{"COD3_PORT_XHAIR_N_LENGTH", "HOW LONG EACH LINE IS"};
constexpr Word kNoteThickness{"COD3_PORT_XHAIR_N_THICKNESS", "HOW THICK THE LINES AND THE DOT ARE"};
constexpr Word kNoteGap{"COD3_PORT_XHAIR_N_GAP", "THE SPACE BETWEEN THE MIDDLE AND THE LINES"};
constexpr Word kNoteOutline{"COD3_PORT_XHAIR_N_OUTLINE", "A DARK EDGE AROUND THE CROSSHAIR (HALF: TOP AND LEFT)"};
constexpr Word kNoteColorGame{"COD3_PORT_XHAIR_N_COLOR_GAME",
                              "THE GAME'S COLORS: RED OVER AN ENEMY, GREEN OVER A FRIENDLY"};
constexpr Word kNoteColor{"COD3_PORT_XHAIR_N_COLOR", "ONE COLOR, AS IN CS2 (CUSTOM: THE COLOR SET IN F2)"};
constexpr Word kNoteColorOwn{"COD3_PORT_XHAIR_N_COLOR_OWN", "YOUR OWN COLOR FOR EACH CASE, SET IN F2"};
constexpr Word kNoteAlpha{"COD3_PORT_XHAIR_N_ALPHA", "HOW SOLID THE CROSSHAIR IS (255: FULLY)"};
constexpr Word kNoteAlphaGame{"COD3_PORT_XHAIR_N_ALPHA_GAME", "THE GAME'S COLORS KEEP THE GAME'S OWN FADE"};
// [acct2-ocr] 2026-11-22 (owner 02:22: "the T style has dynamic modes within CS that should also be in cod3"): a
// dynamic style moves with T style on too, as CS2's
constexpr Word kNoteT{"COD3_PORT_XHAIR_N_T", "NO TOP LINE"};

// The sliders: CS2's ranges in pixels on its 1080-line screen (f2_crosshair_cs2.h), the arrows' steps.
struct XhairNumber {
  double min, max, step, typed;
  int decimals;
};
constexpr XhairNumber kLengthNumber{0.0, 255.0, 0.5, 0.5, 1};
constexpr XhairNumber kThicknessNumber{0.0, 32.0, 0.5, 0.5, 1};
constexpr XhairNumber kGapNumber{0.0, 128.0, 0.5, 0.5, 1};  // the legacy style from -10, as CS2's
constexpr double kGapLegacyMin = -10.0;
constexpr XhairNumber kAlphaNumber{0.0, 255.0, 5.0, 1.0, 0};

bool IsXhairRow(int row) { return row >= kXhairStyle && row <= kXhairT; }

std::string Say(const Word& w, bool english) { return english ? std::string(w.english) : Words(w.key, w.english); }

// COLOR's place: 0 GAME'S, 1-5 the presets, 6 CUSTOM (a color of F2's that isn't one of them), 7 OWN COLORS.
constexpr int kColorCustom = 1 + kPresetCount, kColorOwn = kColorCustom + 1;
int ColorPlace(const xh::Settings& s) {
  if (s.color_from == xh::kColorGame) return 0;
  if (s.color_from == xh::kColorOwnPerCase) return kColorOwn;
  for (int i = 0; i < kPresetCount; ++i)
    if (s.color.r == kPresets[i].r && s.color.g == kPresets[i].g && s.color.b == kPresets[i].b) return 1 + i;
  return kColorCustom;
}

double GapMin(const xh::Settings& s) { return s.style == xh::kDynamicCrossLegacy ? kGapLegacyMin : kGapNumber.min; }

bool XhairNumberOf(const xh::Settings& s, int row, XhairNumber* n, double* value) {
  switch (row) {
    case kXhairLength:
      *n = kLengthNumber;
      *value = s.length;
      break;
    case kXhairThickness:
      *n = kThicknessNumber;
      *value = s.thickness;
      break;
    case kXhairGap:
      *n = kGapNumber;
      n->min = GapMin(s);
      *value = s.gap;
      break;
    case kXhairAlpha:
      *n = kAlphaNumber;
      *value = s.color_from == xh::kColorOwnPerCase ? s.case_color[xh::kCaseNormal].a : s.color.a;
      break;
    default: return false;
  }
  *value = std::clamp(*value, n->min, n->max);
  return true;
}

// `row`'s number set to `v` (already in range); false when nothing changes.
bool SetXhairNumber(xh::Settings* s, int row, double v) {
  const float f = float(v);
  const uint8_t a = uint8_t(std::lround(std::clamp(v, 0.0, 255.0)));
  switch (row) {
    case kXhairLength: if (s->length == f) return false; s->length = f; return true;
    case kXhairThickness: if (s->thickness == f) return false; s->thickness = f; return true;
    case kXhairGap: if (s->gap == f) return false; s->gap = f; return true;
    case kXhairAlpha:
      if (s->color_from == xh::kColorOwnPerCase) {
        bool changed = false;
        for (xh::Rgba& c : s->case_color) {
          changed |= c.a != a;
          c.a = a;
        }
        return changed;
      }
      if (s->color.a == a) return false;
      s->color.a = a;
      return true;
    default: return false;
  }
}

std::string XhairValue(int player, int row, bool english) {
  const xh::Settings s = xh::PlayerCopy(player);
  switch (row) {
    case kXhairStyle:
      return !s.enabled ? Say(kGamesWord, english)
                        : Say(kStyleWords[std::clamp(s.style, 0, xh::kStyleCount - 1)], english);
    case kXhairDot: return Say(s.dot ? kOnWord : kOffWord, english);
    case kXhairT: return Say(s.t_style ? kOnWord : kOffWord, english);
    case kXhairOutline: return Say(kOutlineWords[std::clamp(s.outline, 0, 2)], english);
    case kXhairColor: {
      const int at = ColorPlace(s);
      if (at == 0) return Say(kGamesWord, english);
      if (at == kColorOwn) return Say(kOwnColorsWord, english);
      return Say(at == kColorCustom ? kCustomWord : kPresets[at - 1].word, english);
    }
    default: {
      XhairNumber n{};
      double v = 0;
      if (!XhairNumberOf(s, row, &n, &v)) return {};
      return fmt::format("{:.{}f}", v, n.decimals);
    }
  }
}

std::string XhairNote(int player, int row) {
  const xh::Settings s = xh::PlayerCopy(player);
  if (row == kXhairStyle) {
    if (!s.enabled) return Say(kNoteStyleOff, false);
    if (!xh::StyleIsDynamic(s.style)) return Say(kNoteStatic, false);
    return Say(kNoteDynamic, false);   // [acct2-ocr] 2026-11-22 with T style too, as CS2
  }
  if (!s.enabled) return Say(kNoteOff, false);
  const bool used = row == kXhairDot      ? xh::StyleUsesDot(s.style)
                    : row == kXhairLength ? xh::StyleUsesLength(s.style)
                    : row == kXhairGap    ? xh::StyleUsesGap(s.style)
                    : row == kXhairT      ? xh::StyleUsesT(s.style)
                                          : true;
  if (!used) return Say(kNoteUnused, false);
  switch (row) {
    case kXhairDot: return Say(kNoteDot, false);
    case kXhairLength: return Say(kNoteLength, false);
    case kXhairThickness: return Say(kNoteThickness, false);
    case kXhairGap: return Say(kNoteGap, false);
    case kXhairOutline: return Say(kNoteOutline, false);
    case kXhairColor: {
      const int at = ColorPlace(s);
      return Say(at == 0 ? kNoteColorGame : at == kColorOwn ? kNoteColorOwn : kNoteColor, false);
    }
    case kXhairAlpha: return Say(s.color_from == xh::kColorGame ? kNoteAlphaGame : kNoteAlpha, false);
    case kXhairT: return Say(kNoteT, false);
  }
  return {};
}

bool StepXhair(int player, int row, bool right) {
  xh::Settings s = xh::PlayerCopy(player);
  const std::string was = xh::Text(s);
  switch (row) {
    case kXhairStyle: {  // GAME'S, then CS2's ten, going round
      const int now = s.enabled ? std::clamp(s.style, 0, xh::kStyleCount - 1) : -1;
      const int last = xh::kStyleCount - 1;
      const int next = right ? (now + 1 > last ? -1 : now + 1) : (now - 1 < -1 ? last : now - 1);
      s.enabled = next >= 0;
      if (next >= 0) s.style = next;
      break;
    }
    case kXhairDot: s.dot = !s.dot; break;
    case kXhairT: s.t_style = !s.t_style; break;
    case kXhairOutline: s.outline = (std::clamp(s.outline, 0, 2) + (right ? 1 : 2)) % 3; break;
    case kXhairColor: {  // GAME'S, CS2's five, CUSTOM (only while it is the color), OWN COLORS, going round
      const int now = ColorPlace(s);
      int next = now;
      do {
        next = (next + (right ? 1 : kColorOwn)) % (kColorOwn + 1);
      } while (next == kColorCustom && now != kColorCustom);
      if (next == 0) {
        s.color_from = xh::kColorGame;
      } else if (next == kColorOwn) {
        s.color_from = xh::kColorOwnPerCase;
      } else {
        s.color_from = xh::kColorConstant;
        if (next != kColorCustom) {
          s.color.r = kPresets[next - 1].r;
          s.color.g = kPresets[next - 1].g;
          s.color.b = kPresets[next - 1].b;
        }
      }
      break;
    }
    default: {
      XhairNumber n{};
      double v = 0;
      if (!XhairNumberOf(s, row, &n, &v)) return false;
      SetXhairNumber(&s, row, NextStep(v, n.step, right, n.min, n.max));
      break;
    }
  }
  if (xh::Text(s) == was) return false;
  xh::AskSetPlayer(player, s);
  return true;
}

bool CanChange(int player, int row) {
  return ValidRow(row) && ValidPlayer(player) && Available(row) && !(kInfo[row].mouse && !KeyboardPlayer(player));
}

void Saved(int player, int row, const char* how) {
  protected_dvars::SaveSettings();
  REXLOG_INFO("cod3: Options: {} = {} (player {}{})", LabelText(row),
              IsXhairRow(row) ? XhairValue(player, row, true) : ValueFor(player, row), player + 1, how);
}

}  // namespace

bool Available(int row) {
  return ValidRow(row) && (kInfo[row].where & (cod3::kMultiplayer ? kMp : kSp)) != 0;
}

const char* LabelKey(int row) { return ValidRow(row) ? kInfo[row].key : kInfo[0].key; }

const char* CrosshairButtonKey() { return kPageWord.key; }  // [acct2-ocr] 2026-11-21
const char* CrosshairTitleKey() { return kTitleWord.key; }

std::string Words(const char* key, const char* english) {
  auto* ks = rex::system::kernel_state();
  uint8_t* base = ks && ks->memory() ? ks->memory()->virtual_membase() : nullptr;
  if (base && key) {
    if (const uint32_t text = text::PortText(base, key)) {
      const char* s = reinterpret_cast<const char*>(base + text);
      const size_t len = strnlen(s, 512);
      if (len > 0 && len < 512) return std::string(s, len);
    }
  }
  return english ? english : "";
}
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

// [acct2-bro Text lane] 2026-11-22 The rows' values and notes in the chosen language: each English text has a key
// whose "@port" row (game/text_port_words.inc, or a pack's own row) answers it through Words(), else the English
// stays. A value shown from a choice list: its word's key; names (XBOX, PLAYSTATION, ...) stay as they are.
namespace {
std::string OnOff(bool on) { return on ? Words(kOnWord.key, kOnWord.english) : Words(kOffWord.key, kOffWord.english); }
std::string ChoiceShown(const char* shown) {
  static constexpr std::pair<const char*, const char*> kChoiceKeys[] = {
      {"AUTO", "COD3_PORT_V_AUTO"}, {"ORIGINAL", "COD3_PORT_V_ORIGINAL"}, {"GOLD", "COD3_PORT_V_GOLD"},
      {"RAINBOW", "COD3_PORT_V_RAINBOW"}, {"COLOR CODES", "COD3_PORT_V_COLOR_CODES"}, {"CUSTOM", "COD3_PORT_V_CUSTOM"}};
  for (const auto& [english, key] : kChoiceKeys)
    if (std::strcmp(shown, english) == 0) return Words(key, english);
  return shown;
}
std::string GamesWord() { return Words(kGamesWord.key, kGamesWord.english); }
}  // namespace

std::string ValueFor(int player, int row) {
  if (!ValidRow(row) || !ValidPlayer(player)) return {};
  const Info& in = kInfo[row];
  switch (in.kind) {
    case Kind::kToggle:
      // [f2-menus] 2026-11-11 ctl 14:50 INVERT MOUSE: the mouse as it is (with the game's own Invert aim), as F2's box.
      if (std::string_view(in.cvar) == "cod3_invert_mouse") return OnOff(MouseLook::MouseInverted());
      return OnOff(GetBool(in.cvar));  // [acct2-bro Text lane] 2026-11-22
    case Kind::kChoice: {
      const std::string v = Get(in.cvar);
      // [acct2-bro Text lane] 2026-11-22 the choice's word in the chosen language
      for (int i = 0; i < in.choice_count; ++i)
        if (SameText(v, in.choices[i].value)) return ChoiceShown(in.choices[i].shown);
      return Upper(v);
    }
    case Kind::kNumber:
      return Shown(std::clamp(GetDouble(in.cvar, in.min), in.min, in.max), in);
    case Kind::kOwn:
      break;
  }
  if (IsXhairRow(row)) return XhairValue(player, row, false);  // [acct2-ocr] 2026-11-21
  switch (row) {
    case kUseHold: return UseHoldText(player, use_hold::OwnSettingMs(player));
    case kLeftHanded: return OnOff(GetBool(FlipSetting(player)));  // [acct2-bro Text lane] 2026-11-22
    case kVehicleFov: return fmt::format("{:.2f}", fov::VehicleScale(player));
    case kTextLanguage: return LanguageText(text::ChosenLanguage());
    case kVoices: {
      const int v = voice::Chosen();
      // [acct2-bro Text lane] 2026-11-22
      return v < 0 || v >= voice::kLanguages ? GamesWord() : Upper(voice::kNames[v]);
    }
    case kSplitHud: {
      const int v = SplitHud();
      return v == 0 ? GamesWord() : fmt::format("{}%", v);  // [acct2-bro Text lane] 2026-11-22
    }
  }
  return {};
}

// [acct2-bro Text lane] 2026-11-22 "{0}" in a note's text -> the number (a plain replace: a text is never used as a
// format string).
namespace {
std::string WithNumber(std::string text, const std::string& number) {
  const size_t at = text.find("{0}");
  if (at != std::string::npos) text.replace(at, 3, number);
  return text;
}
}  // namespace

std::string NoteFor(int player, int row) {
  if (!ValidRow(row) || !ValidPlayer(player)) return {};
  const Info& in = kInfo[row];
  // [acct2-bro Text lane] 2026-11-22 each note in the chosen language (COD3_PORT_<ROW>_N and COD3_PORT_N_* "@port"
  // rows), else the English
  if (in.mouse && !KeyboardPlayer(player)) return Words("COD3_PORT_N_KBM_PLAYERS", "THE KEYBOARD AND MOUSE PLAYER'S");
  if (in.kind != Kind::kOwn) return Words((std::string(in.key) + "_N").c_str(), in.note);
  if (IsXhairRow(row)) return XhairNote(player, row);  // [acct2-ocr] 2026-11-21
  switch (row) {
    case kUseHold:
      if (player > 0 && use_hold::OwnSettingMs(player) == -1)
        return Words("COD3_PORT_N_USE_HOLD_P1",
                     "PLAYER 1'S UNTIL CHANGED HERE: HOW LONG USE IS HELD (THE GAME'S: 250 MS)");
      return cod3::kMultiplayer ? Words("COD3_PORT_N_USE_HOLD_MP",
                                        "THIS PLAYER'S OWN: HOW LONG USE IS HELD (THE GAME'S: 250 MS)")
                                : Words("COD3_PORT_N_USE_HOLD_SP",
                                        "HOW LONG USE IS HELD FOR A WEAPON, A GUN OR A VEHICLE (THE GAME'S: 250 MS)");
    case kLeftHanded:
      return cod3::kMultiplayer ? Words("COD3_PORT_N_LEFT_HANDED_MP", "THIS PLAYER'S WEAPON IN THE LEFT HAND, MIRRORED")
                                : Words("COD3_PORT_N_LEFT_HANDED_SP", "THE WEAPON IN THE LEFT HAND, MIRRORED");
    case kVehicleFov:
      if (player > 0)
        return fov::OwnVehicleScale(player) > 0.0
                   ? WithNumber(Words("COD3_PORT_N_VEHICLE_FOV_OWN",
                                      "THIS PLAYER'S OWN, IN VEHICLES AND ON A TANK'S GUN (PLAYER 1'S: {0})"),
                                fmt::format("{:.2f}", fov::VehicleScale(0)))
                   : Words("COD3_PORT_N_VEHICLE_FOV_P1",
                           "PLAYER 1'S UNTIL CHANGED HERE: IN VEHICLES AND ON A TANK'S GUN");
      return Words("COD3_PORT_N_VEHICLE_FOV_SP",
                   "THE FIELD OF VIEW IN VEHICLES AND ON A TANK'S GUN (1.00: THE GAME'S)");
    case kTextLanguage: {
      const int i = text::ChosenLanguage();
      if (i >= 0 && !text::LanguageUsable(i)) return Words("COD3_PORT_N_TEXT_NOT_INSTALLED",
                                                           "NOT INSTALLED: TEXT/<LANGUAGE>.TXT IN THE GAME'S FOLDER");
      if (i >= 0 && text::LanguageAfterRestart(i)) return Words("COD3_PORT_N_TEXT_RESTART",
                                                                "APPLIES AFTER A RESTART (ITS SCRIPT IS DRAWN BY THE "
                                                                "PORT)");
      return Words("COD3_PORT_N_TEXT_NEXT",
                   "MENUS, HINTS, OBJECTIVES AND SUBTITLES, FROM THE NEXT TEXT THE GAME ASKS FOR");
    }
    case kVoices: {
      const int v = voice::Chosen();
      if (v >= 0 && v < voice::kLanguages && voice::Available()[size_t(v)] == 0)
        return Words("COD3_PORT_N_VOICES_NOT_INSTALLED", "NOT INSTALLED: THE SETUP TOOL ADDS ANOTHER DISC'S VOICES");
      return cod3::kMultiplayer ? Words("COD3_PORT_N_VOICES_MP", "THE SOLDIERS' LANGUAGE, HEARD FROM THE NEXT MAP")
                                : Words("COD3_PORT_N_VOICES_SP",
                                        "THE SOLDIERS' LANGUAGE (ANOTHER DISC'S: THE SETUP TOOL'S PLAY PAGE)");
    }
    case kSplitHud:
      return Words("COD3_PORT_N_SPLIT_HUD",
                   "HOW MUCH OF EACH VIEW THE HUD SPREADS OVER, FROM THE NEXT SPLIT (GAME'S: ABOUT 84%)");
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
        default:
          if (IsXhairRow(row)) changed = StepXhair(player, row, right);  // [acct2-ocr] 2026-11-21
          break;
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
  if (IsXhairRow(row)) {  // [acct2-ocr] 2026-11-21 LENGTH, THICKNESS, GAP, ALPHA
    XhairNumber x{};
    if (!XhairNumberOf(xh::PlayerCopy(player), row, &x, value)) return false;
    *n = menuslider::Number{x.min, x.max, x.typed, 1.0, x.decimals, ""};
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
  } else if (IsXhairRow(row)) {  // [acct2-ocr] 2026-11-21
    xh::Settings s = xh::PlayerCopy(player);
    XhairNumber x{};
    double now = 0;
    if (!XhairNumberOf(s, row, &x, &now)) return "";
    const double typed = std::clamp(x.min + std::round((value - x.min) / x.typed) * x.typed, x.min, x.max);
    changed = SetXhairNumber(&s, row, typed);
    if (changed) xh::AskSetPlayer(player, s);
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
  for (const Word& w : {kPageWord, kTitleWord})  // [acct2-ocr] 2026-11-21 the CROSSHAIR page's button and title
    if (std::strcmp(key, w.key) == 0) return FixedText(base, w.key, w.english);
  return 0;
}

}  // namespace cod3::portrows
