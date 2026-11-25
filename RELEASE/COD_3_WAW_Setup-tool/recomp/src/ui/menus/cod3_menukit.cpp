// cod3 - [waw-decomp/menus2] 2026-11-23 Native menus: the CoD3 adapter and the takeover of Pause > Options (see
// cod3_menukit.h). Its rows are f2-menus' row providers of 2026-11-16 (dev notes: native-menus P0, section 1) on
// menukit's RowProvider: each one asks the functions the game's own Options pages and F2 already use.

#include "ui/menus/cod3_menukit.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/filesystem.h>
#include <rex/logging.h>

#include "game/addresses.h"
#include "game/engine.h"
#include "game/fov.h"
#include "game/guest.h"
#include "game/menu_controller.h"
#include "game/menu_port_rows.h"
#include "game/menu_slider.h"
#include "game/menu_video.h"
#include "game/options_pages.h"
#include "game/pc_menu.h"
#include "game/program.h"
#if !defined(COD3_MP)
#include "game/menu_aim.h"                // the campaign's AIM page (cod3mp doesn't build it)
#include "game/movement/menu_movement.h"  // the campaign's MOVEMENT page
#endif
#include "menukit/menukit.h"
#include "ui/menus/cod3_menukit_shown.h"

// The menus' data, built in from recomp/menus/cod3/ (CMakeLists.txt, cod3_menus_data.inc.in).
#include "cod3_menus_data.inc"

REXCVAR_DEFINE_BOOL(cod3_native_menus, false, "cod3",
                    "Native menus (being built): the game's Pause > Options page drawn and run by the port's own menu "
                    "code (src/menukit), in both programs and for each split-screen player; the game still opens "
                    "and closes it. Off: the game's own page, as before")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_STRING(cod3_menu_style, "modern", "cod3",
                      "The native menus' look (cod3_native_menus): modern (left-aligned, the pages listed on the "
                      "left) or original (the game's own layout, one page at a time, X / Y for the next page)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DECLARE(bool, cod3_menu_trace);

namespace cod3::nativemenus {
namespace {

namespace pm = cod3::addr::pc_menu;
using cod3::guest::ReadS32;
using cod3::guest::ReadU16;
using cod3::guest::ReadU32;
using cod3::guest::ReadU8;

// The game's pages ours replaces (their vtables): the campaign's AdvancedOptionsMenu (Pause > Options); cod3mp's
// InGameOptionsMenu and AARInGameOptionsMenu (game/mp/options_mp.cpp).
#if defined(COD3_MP)
constexpr uint32_t kTakenPages[] = {0x820716DC, 0x820717E4};
#else
constexpr uint32_t kTakenPages[] = {0x82071B24};
#endif

bool Plausible(uint32_t a) { return a >= 0x10000u && a < 0xC0000000u && (a & 3) == 0; }

// What a provider needs of the game to change a row through the game (video, aim, movement): the calling hook's.
struct GameRef {
  PPCContext* ctx = nullptr;
  uint8_t* base = nullptr;
  uint32_t page = 0;  // the game's page ours stands in for (the game: rows change through it)
};

GameRef* RefOf(const menukit::Context& c) { return static_cast<GameRef*>(c.game); }

// A provider's names: the menu file's word for each row and the row code's own number for it.
struct Named {
  const char* name;
  int row;
};

template <size_t N>
int Lookup(const Named (&table)[N], std::string_view name) {
  for (const Named& n : table)
    if (name == n.name) return n.row;
  return -1;
}

void CopyNumber(const menuslider::Number& from, double value, menukit::RowView* out) {
  out->number.min = std::min(from.min, from.max);
  out->number.max = std::max(from.min, from.max);
  out->number.step = from.step;
  out->number_value = value;
}

std::string Two(double v) {
  char text[32];
  std::snprintf(text, sizeof(text), "%.2f", v);
  return text;
}

// Game thread: guest memory, for the text below (set by every call from the game's hooks).
uint8_t* g_base = nullptr;

// A text key in the chosen language, asked of the port's own Localize answers (the ones the game's Localize hook
// asks: game/menu_port_rows.h, menu_video.h, menu_controller.h, menu_aim.h, movement/menu_movement.h; a pack's row
// or the port's own words in the chosen language), else its English. Game thread.
std::string Words(const char* key, const char* english) {
  const std::string fallback = english ? english : (key ? key : "");
  if (!key || !*key || !g_base) return fallback;
  uint32_t text = cod3::portrows::Localize(g_base, key);
  if (!text) text = cod3::controllermenu::Localize(g_base, key);
#if !defined(COD3_MP)  // these are the campaign's (cod3mp's video rows are answered through portrows and options_pages)
  if (!text) text = cod3::videomenu::Localize(g_base, key);
  if (!text) text = cod3::aimmenu::Localize(g_base, key);
  if (!text) text = cod3::movementmenu::Localize(g_base, key);
#endif
  if (!text) return fallback;
  std::string out = cod3::guest::ReadString(g_base, text, 256);
  return out.empty() ? fallback : out;
}

// --- port: game/menu_port_rows.h ---------------------------------------------------------------------------------
namespace pr = cod3::portrows;
constexpr Named kPortNames[] = {
    {"skip_cutscenes", pr::kSkipCutscenes},   {"hold_to_skip", pr::kSkipHold},
    {"cutscene_bars", pr::kCutsceneBars},     {"use_hold_time", pr::kUseHold},
    {"left_handed_weapon", pr::kLeftHanded},  {"vehicle_fov_scale", pr::kVehicleFov},
    {"text_language", pr::kTextLanguage},     {"voices", pr::kVoices},
    {"master_volume", pr::kVolume},           {"keys_in_prompts", pr::kKeyPrompts},
    {"keys_as_key_caps", pr::kKeyCaps},       {"button_pictures", pr::kButtonStyle},
    {"button_colors", pr::kButtonColors},     {"raw_mouse", pr::kRawMouse},
    {"invert_mouse", pr::kInvertMouse},       {"mouse_sensitivity", pr::kMouseSensitivity},
    {"mouse_ads_scale", pr::kMouseAds},       {"even_mouse_turning", pr::kMouseSmoothing},
    {"colors_in_names", pr::kColorCodes},     {"show_version", pr::kShowVersion},
    {"split_screen_hud", pr::kSplitHud},
};

class PortRows final : public menukit::RowProvider {
 public:
  std::string_view Prefix() const override { return "port"; }
  bool View(std::string_view name, const menukit::Context& c, menukit::RowView* out) const override {
    const int row = Lookup(kPortNames, name);
    if (row < 0 || !pr::Available(row)) return false;
    out->label = Words(pr::LabelKey(row), pr::LabelText(row));
    out->value = pr::ValueFor(c.player, row);
    out->note = pr::NoteFor(c.player, row);
    menuslider::Number n;
    double v = 0.0;
    out->kind = pr::NumberOfFor(c.player, row, &n, &v) ? menukit::Kind::kSlider : menukit::Kind::kChoice;
    if (out->kind == menukit::Kind::kSlider) CopyNumber(n, v, out);
    return true;
  }
  bool Step(std::string_view name, const menukit::Context& c, int direction) override {
    const int row = Lookup(kPortNames, name);
    return row >= 0 && pr::Available(row) && !pr::StepFor(c.player, row, direction > 0).empty();
  }
  bool SetNumber(std::string_view name, const menukit::Context& c, double value) override {
    const int row = Lookup(kPortNames, name);
    return row >= 0 && pr::Available(row) && !pr::SetNumberFor(c.player, row, value).empty();
  }
};

// --- video: game/menu_video.h (menu_video_rows.cpp, both programs) -----------------------------------------------
namespace vm = cod3::videomenu;
constexpr Named kVideoNames[] = {
    {"resolution", vm::kResolution},         {"frame_rate_limit", vm::kFrameRate},
    {"vsync", vm::kVsync},                   {"anti_aliasing", vm::kAntiAliasing},
    {"shadows", vm::kShadows},               {"hd_textures", vm::kHdTextures},
    {"battle_actions", vm::kBattleActions},  {"depth_of_field", vm::kDepthOfField},
    {"motion_blur", vm::kMotionBlur},
};

class VideoRows final : public menukit::RowProvider {
 public:
  std::string_view Prefix() const override { return "video"; }
  bool View(std::string_view name, const menukit::Context&, menukit::RowView* out) const override {
    const int row = Lookup(kVideoNames, name);
    if (row < 0) return false;
    out->label = Words(vm::LabelKey(row), vm::LabelText(row));
    out->value = vm::Value(row);
    out->note = vm::Note(row);
    out->kind = menukit::Kind::kChoice;
    return true;
  }
  bool Step(std::string_view name, const menukit::Context& c, int direction) override {
    const int row = Lookup(kVideoNames, name);
    GameRef* g = RefOf(c);
    return row >= 0 && g && g->ctx && g->base && !vm::Step(*g->ctx, g->base, row, direction > 0).empty();
  }
};

// --- controller: game/menu_controller.h --------------------------------------------------------------------------
namespace cm = cod3::controllermenu;
constexpr Named kControllerNames[] = {
    {"sensitivity_limit", cm::kLimit},
    {"stick_dead_zone", cm::kDeadZone},
    {"vibrate_with_keyboard", cm::kRumbleKeys},  // the first controller's: the campaign's only
};

bool ControllerHas(int row) { return row >= 0 && (!cod3::kMultiplayer || row < cm::kMultiplayerRows); }

class ControllerRows final : public menukit::RowProvider {
 public:
  std::string_view Prefix() const override { return "controller"; }
  bool View(std::string_view name, const menukit::Context& c, menukit::RowView* out) const override {
    const int row = Lookup(kControllerNames, name);
    if (!ControllerHas(row)) return false;
    out->label = Words(cm::LabelKey(row), cm::LabelText(row));
    out->value = cm::ValueFor(c.player, row);
    out->note = cm::NoteFor(c.player, row);
    menuslider::Number n;
    double v = 0.0;
    out->kind = cm::NumberOfFor(c.player, row, &n, &v) ? menukit::Kind::kSlider : menukit::Kind::kChoice;
    if (out->kind == menukit::Kind::kSlider) CopyNumber(n, v, out);
    return true;
  }
  bool Step(std::string_view name, const menukit::Context& c, int direction) override {
    const int row = Lookup(kControllerNames, name);
    return ControllerHas(row) && !cm::StepFor(c.player, row, direction > 0).empty();
  }
  bool SetNumber(std::string_view name, const menukit::Context& c, double value) override {
    const int row = Lookup(kControllerNames, name);
    return ControllerHas(row) && !cm::SetNumberFor(c.player, row, value).empty();
  }
};

// --- fov: game/fov.h. Steps of 5 and 0.05 kept together in the effective range (fov::FitFov / FitScale), as F2
// and the game's own FOV rows do (f2-menus 2026-11-17); player 1 through Engine (queued, saved), the others' own
// split-screen values.
class FovRows final : public menukit::RowProvider {
 public:
  std::string_view Prefix() const override { return "fov"; }
  bool View(std::string_view name, const menukit::Context& c, menukit::RowView* out) const override {
    const cod3::fov::Settings s = cod3::fov::ViewSettings(c.player);
    out->kind = menukit::Kind::kSlider;
    if (name == "field_of_view") {
      out->label = Words("COD3_NATIVE_FIELD_OF_VIEW", "FIELD OF VIEW");
      out->value = std::to_string(static_cast<int>(s.fov + 0.5f));
      out->number = {cod3::fov::kMinFov, cod3::fov::kMaxFov, 5.0};
      out->number_value = s.fov;
      return true;
    }
    if (name == "fov_scale") {
      out->label = Words("COD3_NATIVE_FOV_SCALE", "FOV SCALE");
      out->value = Two(s.scale);
      out->number = {cod3::fov::kMinScale, cod3::fov::kMaxScale, 0.05};
      out->number_value = s.scale;
      return true;
    }
    return false;
  }
  bool Step(std::string_view name, const menukit::Context& c, int direction) override {
    const cod3::fov::Settings s = cod3::fov::ViewSettings(c.player);
    if (name == "field_of_view") return SetNumber(name, c, s.fov + (direction > 0 ? 5.0 : -5.0));
    if (name == "fov_scale") return SetNumber(name, c, s.scale + (direction > 0 ? 0.05 : -0.05));
    return false;
  }
  bool SetNumber(std::string_view name, const menukit::Context& c, double v) override {
    const cod3::fov::Settings s = cod3::fov::ViewSettings(c.player);
    cod3::fov::Settings f = s;
    if (name == "field_of_view")
      f = cod3::fov::FitFov(static_cast<float>(v), s.scale);
    else if (name == "fov_scale")
      f = cod3::fov::FitScale(s.fov, static_cast<float>(v));
    else
      return false;
    if (std::fabs(f.fov - s.fov) < 0.01f && std::fabs(f.scale - s.scale) < 0.0001f) return false;  // at an end
    if (c.player <= 0) {
      cod3::Engine::Get().SetFovSettings(f.fov, f.scale);
    } else {
      cod3::fov::SetSplitFov(c.player, f.fov);
      cod3::fov::SetSplitScale(c.player, f.scale);
    }
    return true;
  }
};

#if !defined(COD3_MP)
// --- aim: game/menu_aim.h (the campaign's) -----------------------------------------------------------------------
namespace am = cod3::aimmenu;
constexpr Named kAimNames[] = {
    {"crosshair", am::kCrosshair},          {"crosshair_style", am::kStyle},
    {"crosshair_length", am::kLength},      {"crosshair_gap", am::kGap},
    {"crosshair_thickness", am::kThickness}, {"crosshair_color", am::kColor},
    {"mouse_sensitivity", am::kMouse},      {"sensitivity_for", am::kWeaponClass},
    {"from_the_hip", am::kHip},             {"aiming_down_sights", am::kAiming},
};

class AimRows final : public menukit::RowProvider {
 public:
  std::string_view Prefix() const override { return "aim"; }
  bool View(std::string_view name, const menukit::Context&, menukit::RowView* out) const override {
    const int row = Lookup(kAimNames, name);
    if (row < 0) return false;
    out->label = Words(am::LabelKey(row), am::LabelKey(row));  // its keys are the English words
    out->value = am::Value(row);
    out->note = am::Note(row);
    menuslider::Number n;
    double v = 0.0;
    out->kind = am::NumberOf(row, &n, &v) ? menukit::Kind::kSlider : menukit::Kind::kChoice;
    if (out->kind == menukit::Kind::kSlider) CopyNumber(n, v, out);
    return true;
  }
  bool Step(std::string_view name, const menukit::Context& c, int direction) override {
    const int row = Lookup(kAimNames, name);
    GameRef* g = RefOf(c);
    return row >= 0 && g && g->ctx && g->base && !am::Step(*g->ctx, g->base, row, direction > 0).empty();
  }
  bool SetNumber(std::string_view name, const menukit::Context&, double value) override {
    const int row = Lookup(kAimNames, name);
    return row >= 0 && !am::SetNumber(row, value).empty();
  }
};

// --- movement: game/movement/menu_movement.h (the campaign's) ----------------------------------------------------
namespace mv = cod3::movementmenu;
constexpr Named kMovementNames[] = {
    {"sprint", 0},      {"sprint_button", 1}, {"movement_style", 2},
    {"cod4_sprint", 3}, {"cod4_speeds", 4},   {"cod4_jump_tricks", 5},
};

bool MovementHas(int row) { return row >= 0 && row < mv::RowCount(); }

class MovementRows final : public menukit::RowProvider {
 public:
  std::string_view Prefix() const override { return "movement"; }
  bool View(std::string_view name, const menukit::Context&, menukit::RowView* out) const override {
    const int row = Lookup(kMovementNames, name);
    if (!MovementHas(row)) return false;
    out->label = Words(mv::LabelKey(row), mv::LabelKey(row));  // "SPRINT" ...
    out->value = mv::Value(row);
    out->note = mv::Note(row);
    out->kind = menukit::Kind::kChoice;
    return true;
  }
  bool Step(std::string_view name, const menukit::Context& c, int direction) override {
    const int row = Lookup(kMovementNames, name);
    GameRef* g = RefOf(c);
    return MovementHas(row) && g && g->ctx && g->base && !mv::Step(*g->ctx, g->base, row, direction > 0).empty();
  }
};

// --- game: the campaign page's own options (AdvancedOptionsMenu items 0-3, game/addresses.h adv), changed through
// the page's own handlers: its selection set to the item (vtbl +164, as the game's own moves do), then its OnLeft /
// OnRight (adv::kToggle; menu_fov.cpp's hook keeps the port's sensitivity cap). The value, the game's step sound and
// the page's saving (ControlsPage_Apply, "save changes?" on Back) stay the game's. The values are the page's own
// copies (+166 invert aim and +167 vibration, bytes, menu_fov.cpp; the sensitivities' numbers at +152 / +156).
struct GameItem {
  const char* name;
  int item;
  const char* key;
  const char* english;
  uint32_t value;  // offset in the page
  bool flag;       // a byte on / off, else a number (s32)
};
constexpr GameItem kGameItems[] = {
    {"invert_aim", 0, "COD3_NATIVE_INVERT_AIM", "INVERT AIM", 166, true},
    {"vibration", 1, "COD3_NATIVE_VIBRATION", "VIBRATION", 167, true},
    {"horizontal_sensitivity", 2, "COD3_NATIVE_HORIZONTAL_SENSITIVITY", "HORIZONTAL SENSITIVITY", 152, false},
    {"vertical_sensitivity", 3, "COD3_NATIVE_VERTICAL_SENSITIVITY", "VERTICAL SENSITIVITY", 156, false},
};

const GameItem* FindGameItem(std::string_view name) {
  for (const GameItem& g : kGameItems)
    if (name == g.name) return &g;
  return nullptr;
}

class GameRows final : public menukit::RowProvider {
 public:
  std::string_view Prefix() const override { return "game"; }
  bool View(std::string_view name, const menukit::Context& c, menukit::RowView* out) const override {
    const GameItem* g = FindGameItem(name);
    if (!g) return false;
    out->label = Words(g->key, g->english);
    out->kind = menukit::Kind::kChoice;
    const GameRef* ref = RefOf(c);
    if (!ref || !ref->base || !Plausible(ref->page)) {  // the data check at start: the row exists
      out->value.clear();
      return true;
    }
    if (g->flag)
      out->value = Words(ReadU8(ref->base, ref->page + g->value) ? "COD3_NATIVE_ON" : "COD3_NATIVE_OFF",
                         ReadU8(ref->base, ref->page + g->value) ? "ON" : "OFF");
    else
      out->value = std::to_string(ReadS32(ref->base, ref->page + g->value));
    return true;
  }
  bool Step(std::string_view name, const menukit::Context& c, int direction) override {
    const GameItem* g = FindGameItem(name);
    GameRef* ref = RefOf(c);
    if (!g || !ref || !ref->ctx || !ref->base || !Plausible(ref->page) ||
        ReadU32(ref->base, ref->page) != cod3::addr::fe::adv::kVtable)
      return false;
    const uint32_t before = ReadU32(ref->base, ref->page + (g->value & ~3u));
    {
      cod3::guest::Call call(*ref->ctx, ref->base);
      call.Invoke(cod3::guest::VirtualSlot(ref->base, ref->page, pm::kPageSetSelection),
                  {ref->page, uint64_t(g->item), 1});
    }
    {
      cod3::guest::Call call(*ref->ctx, ref->base);
      call.Invoke(cod3::addr::fe::adv::kToggle, {ref->page, direction > 0 ? 1u : 0u});
    }
    return ReadU32(ref->base, ref->page + (g->value & ~3u)) != before;
  }
};
#endif  // !COD3_MP

// --- the adapter ---------------------------------------------------------------------------------------------------

// English for the menu file's page keys that options_pages.cpp doesn't have.
struct English {
  const char* key;
  const char* text;
};
constexpr English kEnglish[] = {
    {"FEMENU_OPTIONS", "OPTIONS"},
    {"COD3_OPTIONS_CONTROLLER", "CONTROLLER"},
    {"COD3_OPTIONS_CONTROLLER_TITLE", "OPTIONS / CONTROLLER"},
    {"COD3_OPTIONS_MOUSE", "MOUSE"},
    {"COD3_OPTIONS_MOUSE_TITLE", "OPTIONS / MOUSE"},
    {"COD3_OPTIONS_MOVEMENT", "MOVEMENT"},
    {"COD3_OPTIONS_MOVEMENT_TITLE", "OPTIONS / MOVEMENT"},
};

class Cod3Adapter final : public menukit::Adapter {
 public:
  std::string_view Game() const override { return "cod3"; }
  bool HasTag(std::string_view tag) const override {
    return cod3::kMultiplayer ? (tag == "cod3mp" || tag == "mp") : (tag == "cod3" || tag == "sp");
  }
  std::string Text(std::string_view key) const override {
    const std::string k(key);
    const char* english = cod3::options_pages::Text(k.c_str());
    for (const English& e : kEnglish)
      if (!english && k == e.key) english = e.text;
    std::string fallback;
    if (!english) {  // "COD3_OPTIONS_AUDIO" -> "AUDIO": the key's last words
      const size_t cut = k.rfind("OPTIONS_");
      fallback = cut == std::string::npos ? k : k.substr(cut + 8);
      std::replace(fallback.begin(), fallback.end(), '_', ' ');
      english = fallback.c_str();
    }
    return Words(k.c_str(), english);
  }
  menukit::RowProvider* Provider(std::string_view prefix) override {
    for (menukit::RowProvider* p : providers_)
      if (p->Prefix() == prefix) return p;
    return nullptr;
  }
  void Play(menukit::Sound sound, const menukit::Context& c) override;

 private:
  PortRows port_;
  VideoRows video_;
  ControllerRows controller_;
  FovRows fov_;
#if !defined(COD3_MP)
  AimRows aim_;
  MovementRows movement_;
  GameRows game_;
  std::vector<menukit::RowProvider*> providers_ = {&port_,   &video_,    &controller_, &fov_,
                                                   &aim_,    &movement_, &game_};
#else
  std::vector<menukit::RowProvider*> providers_ = {&port_, &video_, &controller_, &fov_};
#endif
};

// --- the takeover's state (game thread only) -----------------------------------------------------------------------

struct Open {
  uint32_t page = 0;              // the game's page ours stands in for
  menukit::Navigator navigator;
  menukit::Look look = menukit::Look::kModern;
  int player = 0;                 // the menu's local player (cod3mp: the in-game system's local client)
  std::vector<menukit::DrawItem> items;  // the last layout (the pointer's hit test)
  float part[4] = {0.0f, 0.0f, 1.0f, 1.0f};
  // A direction held on the keyboard (pc_menu.cpp holds an arrow key until it is let go): repeated after kRepeatAfter,
  // every kRepeatEvery, by the clock (the same at any frame rate).
  uint32_t held_code = 0;
  int64_t held_since = 0, held_last = 0;
};

constexpr int64_t kRepeatAfterNs = 400'000'000;
constexpr int64_t kRepeatEveryNs = 90'000'000;

Cod3Adapter& TheAdapter() {
  static Cod3Adapter adapter;
  return adapter;
}

std::unordered_map<uint32_t, Open>& Opens() {  // by menu system
  static std::unordered_map<uint32_t, Open> opens;
  return opens;
}

// The menu file, read once (game thread, the first time a page is taken): the copy beside the program
// (menus/cod3/pause_options.toml, put there by the build, so it can be edited without a build) when it reads
// well, else the one built in (recomp/menus/cod3/pause_options.toml when the program was built).
bool ReadMenu(std::string_view text, std::string_view source, menukit::Menu* menu) {
  std::vector<std::string> problems;
  const bool usable = menukit::ParseMenu(text, source, TheAdapter(), menu, &problems);
  for (const std::string& p : problems) REXLOG_WARN("cod3: native menus: {}", p);
  return usable;
}

const menukit::Menu* TheMenu() {
  static bool read = false;
  static menukit::Menu menu;
  static bool usable = false;
  if (!read) {
    read = true;
    const std::filesystem::path beside =
        rex::filesystem::GetExecutableFolder() / "menus" / "cod3" / "pause_options.toml";
    std::ifstream file(beside, std::ios::binary);
    std::string source = "built in";
    if (file) {
      std::stringstream text;
      text << file.rdbuf();
      usable = ReadMenu(text.str(), "menus/cod3/pause_options.toml", &menu);
      if (usable) source = "menus/cod3/pause_options.toml";
    }
    if (!usable) {
      menu = menukit::Menu{};
      usable = ReadMenu(kPauseOptionsToml, "menus/cod3/pause_options.toml (built in)", &menu);
    }
    size_t rows = 0;
    for (const menukit::Page& page : menu.pages) rows += page.rows.size();
    REXLOG_INFO("cod3: native menus: Pause > Options from {}: {} pages, {} rows{}", source, menu.pages.size(),
                rows, usable ? "" : " (nothing usable: the game's own page stays)");
  }
  return usable ? &menu : nullptr;
}

menukit::Look ChosenLook() {
  return REXCVAR_GET(cod3_menu_style) == "original" ? menukit::Look::kOriginal : menukit::Look::kModern;
}

bool Taken(uint8_t* base, uint32_t page) {
  if (!Plausible(page)) return false;
  const uint32_t vtable = ReadU32(base, page);
  return std::find(std::begin(kTakenPages), std::end(kTakenPages), vtable) != std::end(kTakenPages);
}

int PlayerOf(uint8_t* base, uint32_t system) {
#if defined(COD3_MP)
  const uint32_t client = ReadU32(base, system + 48);  // InGameMenuSystem: its local client (pc_menu.cpp, mpui)
  return client < 4 ? static_cast<int>(client) : 0;
#else
  (void)base, (void)system;
  return 0;
#endif
}

// The player's view as a share of the picture (split screen), from the game's own views (refdef x, y, width,
// height of each local player's cg; the same reading as game/mp/hud_mp.cpp SetView). The whole picture otherwise.
void ViewOf(uint8_t* base, int player, float part[4]) {
  part[0] = part[1] = 0.0f;
  part[2] = part[3] = 1.0f;
#if defined(COD3_MP)
  constexpr uint32_t kCgArray = 0x82C7D490, kCgStride = 6368, kRefdefX = 4928;  // game/mp/hud_mp.cpp
  int32_t right = 0, bottom = 0, rect[4][4] = {};
  for (int i = 0; i < 4; ++i) {
    const uint32_t at = kCgArray + kCgStride * uint32_t(i) + kRefdefX;
    for (int k = 0; k < 4; ++k) rect[i][k] = ReadS32(base, at + 4 * uint32_t(k));
    const int32_t* r = rect[i];
    if (r[2] <= 0 || r[3] <= 0 || r[0] < 0 || r[1] < 0 || r[0] + r[2] > 16384 || r[1] + r[3] > 16384) continue;
    right = std::max(right, r[0] + r[2]);
    bottom = std::max(bottom, r[1] + r[3]);
  }
  const int32_t* r = rect[std::clamp(player, 0, 3)];
  if (right <= 0 || bottom <= 0 || r[2] <= 0 || r[3] <= 0) return;
  part[0] = float(r[0]) / float(right);
  part[1] = float(r[1]) / float(bottom);
  part[2] = float(r[2]) / float(right);
  part[3] = float(r[3]) / float(bottom);
#else
  (void)base, (void)player;
#endif
}

// The open page of `system` when the game shows the page ours stands in for, else nullptr (and it is forgotten).
Open* Owned(PPCContext& ctx, uint8_t* base, uint32_t system) {
  if (!REXCVAR_GET(cod3_native_menus) || !Plausible(system)) {
    if (!Opens().empty()) Opens().clear();
    return nullptr;
  }
  const uint32_t page = cod3::pcmenu::SystemPage(ctx, base, system);
  auto it = Opens().find(system);
  if (!Taken(base, page)) {
    if (it != Opens().end()) Opens().erase(it);
    return nullptr;
  }
  const menukit::Menu* menu = TheMenu();
  if (!menu) return nullptr;
  if (it == Opens().end()) it = Opens().emplace(system, Open{}).first;
  Open& o = it->second;
  const menukit::Look look = ChosenLook();
  if (o.page != page || o.look != look || !o.navigator.IsOpen()) {
    o.page = page;
    o.look = look;
    o.player = PlayerOf(base, system);
    GameRef ref{&ctx, base, page};
    o.navigator.Open(menu, look, TheAdapter(), menukit::Context{o.player, &ref});
    REXLOG_INFO("cod3: native menus: Pause > Options taken ({} look) for player {}",
                look == menukit::Look::kModern ? "modern" : "original", o.player + 1);
  }
  return &o;
}

// The game's step sound (cod3mp): an option entry of the page under ours (FEComboBox, vtable 0x820709EC, byte +40
// set) plays it with its own code, 0x82509EB0 (f2-menus 2026-11-17, run 15:03: the sound and the arrow flash). The
// campaign's page steps its values itself and its sound call is not known yet: silent there.
uint32_t g_sound_page = 0;  // game thread: the page of the call that plays

void Cod3Adapter::Play(menukit::Sound sound, const menukit::Context& c) {
#if defined(COD3_MP)
  GameRef* g = RefOf(c);
  if (sound != menukit::Sound::kStep || !g || !g->ctx || !g->base || !Plausible(g_sound_page)) return;
  uint8_t* base = g->base;
  const uint32_t list = ReadU32(base, g_sound_page + 4);
  const int count = int16_t(ReadU16(base, g_sound_page + 38));
  if (!Plausible(list) || count <= 0 || count > 64) return;
  for (int i = 0; i < count; ++i) {
    const uint32_t item = ReadU32(base, list + 4 * uint32_t(i));
    if (!Plausible(item) || ReadU32(base, item) != 0x820709ECu || !ReadU8(base, item + 40)) continue;
    cod3::guest::Call call(*g->ctx, base);
    call.Invoke(0x82509EB0u, {item});
    return;
  }
#else
  (void)sound, (void)c;
#endif
}

void Trace(const char* what, uint32_t code, const Open& o) {
  if (!REXCVAR_GET(cod3_menu_trace)) return;
  const menukit::Menu* menu = o.navigator.menu();
  std::string row = "-";
  if (menu && o.navigator.page() < static_cast<int>(menu->pages.size())) {
    const menukit::Page& page = menu->pages[size_t(o.navigator.page())];
    if (o.navigator.row() >= 0 && o.navigator.row() < static_cast<int>(page.rows.size()))
      row = page.rows[size_t(o.navigator.row())].id;
    REXLOG_INFO("cod3: native menus: {} {} -> page {} ({}), row {}{} (player {})", what, code,
                o.navigator.page() + 1, page.id, row, o.navigator.in_list() ? " (page list)" : "", o.player + 1);
  }
}

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// The game's menu button codes (game/addresses.h pc_menu) as menukit's inputs; false for the ones left to the game.
bool InputOf(uint32_t code, menukit::Input* input) {
  switch (code) {
    case pm::kCodeUp: *input = menukit::Input::kUp; return true;
    case pm::kCodeDown: *input = menukit::Input::kDown; return true;
    case pm::kCodeLeft: *input = menukit::Input::kLeft; return true;
    case pm::kCodeRight: *input = menukit::Input::kRight; return true;
    case pm::kCodeA: *input = menukit::Input::kAccept; return true;
    case pm::kCodeB:
    case pm::kCodeBack: *input = menukit::Input::kBack; return true;
    case pm::kCodeX: *input = menukit::Input::kPagePrev; return true;
    case pm::kCodeY: *input = menukit::Input::kPageNext; return true;
    default: return false;
  }
}

bool IsDirection(uint32_t code) {
  return code == pm::kCodeUp || code == pm::kCodeDown || code == pm::kCodeLeft || code == pm::kCodeRight;
}

}  // namespace

bool TakeButton(PPCContext& ctx, uint8_t* base, uint32_t system, uint32_t code, int32_t player) {
  g_base = base;
  (void)player;
  Open* o = Owned(ctx, base, system);
  if (!o) return false;
  menukit::Input input;
  if (!InputOf(code, &input)) return false;  // Start and anything else: the game's (Start closes the pause menu)
  GameRef ref{&ctx, base, o->page};
  g_sound_page = o->page;
  const menukit::Outcome outcome = o->navigator.Press(input, TheAdapter(), menukit::Context{o->player, &ref});
  g_sound_page = 0;
  if (outcome == menukit::Outcome::kLeave) {
    Trace("leaves on", code, *o);
    Opens().erase(system);  // the game's own Back takes the page away
    return false;
  }
  Trace("button", code, *o);
  if (IsDirection(code) && cod3::pcmenu::FromPcInput(code)) {  // an arrow key held: repeated in DrawSystem
    o->held_code = code;
    o->held_since = o->held_last = NowNs();
  } else {
    o->held_code = 0;
  }
  ctx.r3.u64 = 1;  // taken (every press is ours while the page is up: the game's page under it stays still)
  return true;
}

bool TakePointer(PPCContext& ctx, uint8_t* base, uint32_t system, float x, float y, bool over, bool moved,
                 bool click) {
  g_base = base;
  Open* o = Owned(ctx, base, system);
  if (!o) return false;
  if (!over || (!moved && !click)) return true;
  // The whole picture's 640x480 to the player's view's own.
  const float w = std::max(o->part[2], 0.01f), h = std::max(o->part[3], 0.01f);
  const float vx = (x / 640.0f - o->part[0]) / w * 640.0f, vy = (y / 480.0f - o->part[1]) / h * 480.0f;
  const menukit::DrawItem* hit = menukit::HitTest(o->items, vx, vy);
  if (!hit) return true;
  GameRef ref{&ctx, base, o->page};
  const menukit::Context c{o->player, &ref};
  g_sound_page = o->page;
  if (click) {
    int side = 0;
    float fraction = -1.0f;
    const float mid = (hit->box.x0 + hit->box.x1) * 0.5f;
    if (hit->what == menukit::What::kGauge)
      fraction = (vx - hit->box.x0) / std::max(hit->box.x1 - hit->box.x0, 1.0f);
    else if (hit->what == menukit::What::kValue || hit->what == menukit::What::kArrows)
      side = vx < mid ? -1 : 1;
    o->navigator.Click(hit->index, hit->category, side, fraction, TheAdapter(), c);
    Trace("click on", static_cast<uint32_t>(hit->index), *o);
  } else {
    o->navigator.Hover(hit->index, hit->category, TheAdapter(), c);
  }
  g_sound_page = 0;
  return true;
}

bool DrawSystem(PPCContext& ctx, uint8_t* base, uint32_t system) {
  g_base = base;
  Open* o = Owned(ctx, base, system);
  if (!o) return false;
  GameRef ref{&ctx, base, o->page};
  if (o->held_code) {  // a held arrow key: one more step now and then, until pc_menu.cpp lets it go
    const int64_t now = NowNs();
    menukit::Input input;
    if (!cod3::pcmenu::FromPcInput(o->held_code) || !InputOf(o->held_code, &input)) {
      o->held_code = 0;
    } else if (now - o->held_since >= kRepeatAfterNs && now - o->held_last >= kRepeatEveryNs) {
      o->held_last = now;
      g_sound_page = o->page;
      o->navigator.Press(input, TheAdapter(), menukit::Context{o->player, &ref});
      g_sound_page = 0;
      Trace("repeat", o->held_code, *o);
    }
  }
  o->items = menukit::LayOut(o->navigator, TheAdapter(), menukit::Context{o->player, &ref}, menukit::Box{});
  ViewOf(base, o->player, o->part);
  Shown shown;
  shown.system = system;
  shown.look = o->look;
  shown.items = o->items;
  std::copy(std::begin(o->part), std::end(o->part), std::begin(shown.part));
  shown.ns = NowNs();
  shown.prompts = o->look == menukit::Look::kModern
                      ? Words("COD3_NATIVE_MENU_PROMPTS", "A  SELECT      B  BACK      X / Y  PAGE")
                      : Words("COD3_NATIVE_MENU_PROMPTS_ORIGINAL", "B  BACK      X / Y  PAGE");
  Publish(std::move(shown));
  return true;
}

}  // namespace cod3::nativemenus
