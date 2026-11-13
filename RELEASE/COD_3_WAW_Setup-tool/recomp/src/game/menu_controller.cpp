// cod3 - [acct2-ocr] 2026-11-07 the CONTROLLER page's rows (see menu_controller.h).

#include "game/menu_controller.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

#include <fmt/format.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "game/engine.h"
#include "game/pad_settings.h"
#include "game/program.h"
#include "game/protected_dvars.h"  // SaveSettings: the in-game menus' save path

namespace cod3::controllermenu {
namespace {

struct Text {
  const char* key;
  const char* text;
};
constexpr Text kTexts[] = {
    {"FEMENU_COP_PAD_TITLE", "OPTIONS / CONTROLLER"},
    {"FEMENU_COP_PAD_ITEM", "CONTROLLER"},
    {"FEMENU_COP_PAD_LIMIT", "SENSITIVITY LIMIT"},
    {"FEMENU_COP_PAD_DEADZONE", "STICK DEAD ZONE"},
    {"FEMENU_COP_PAD_RUMBLEKEYS", "VIBRATE WITH KEYBOARD"},
};
// The row labels' keys, in Row's order.
constexpr const char* kLabelKeys[kRows] = {"FEMENU_COP_PAD_LIMIT", "FEMENU_COP_PAD_DEADZONE",
                                           "FEMENU_COP_PAD_RUMBLEKEYS"};
constexpr const char* kDeadZoneSetting[pad::kMaxPlayers] = {"cod3_deadzone_p1", "cod3_deadzone_p2",
                                                            "cod3_deadzone_p3", "cod3_deadzone_p4"};
constexpr int kDeadZoneMax = 100;   // of the stick's 128, as F2's slider
constexpr int kJoyThresholdGame = 25;  // the game's own joy_threshold

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
int GetInt(const char* name, int fallback) {
  const std::string v = Get(name);
  return v.empty() ? fallback : std::atoi(v.c_str());
}
bool GetBool(const char* name) {
  const std::string v = Get(name);
  return v == "true" || v == "1";
}

bool ValidPlayer(int player) { return player >= 0 && player < pad::Players(); }

// The game's joy_threshold, every controller's dead zone (as F2 reads it: the console's last snapshot, else
// the saved value, else the game's own 25).
int SharedDeadZone() {
  static std::mutex m;
  static uint64_t generation = ~0ull;
  static int value = -1;
  auto& engine = Engine::Get();
  std::lock_guard lock(m);
  const uint64_t gen = engine.SnapshotGeneration();
  if (gen != generation) {
    generation = gen;
    const EngineSnapshot snap = engine.Snapshot();
    for (const auto& c : snap.cvars)
      if (c.name == "joy_threshold") value = std::atoi(c.value.c_str());
  }
  if (value < 0) {
    const auto archived = engine.ArchivedCvars();
    if (auto it = archived.find("joy_threshold"); it != archived.end()) value = std::atoi(it->second.c_str());
  }
  return std::clamp(value < 0 ? kJoyThresholdGame : value, 0, kDeadZoneMax);
}

// The player's own dead zone (-1: the shared one) and the one in use.
int OwnDeadZone(int player) {
  return ValidPlayer(player) ? std::clamp(GetInt(kDeadZoneSetting[player], -1), -1, kDeadZoneMax) : -1;
}
int DeadZoneInUse(int player) {
  const int own = OwnDeadZone(player);
  return own >= 0 ? own : SharedDeadZone();
}

// Sets the player's own dead zone; the shared value hands it back (-1), so nothing differs from the game's.
bool SetDeadZone(int player, int units) {
  if (!ValidPlayer(player)) return false;
  units = std::clamp(units, 0, kDeadZoneMax);
  const int setting = units == SharedDeadZone() ? -1 : units;
  if (setting == OwnDeadZone(player)) return false;
  return rex::cvar::SetFlagByName(kDeadZoneSetting[player], std::to_string(setting));
}

bool SetLimit(int value) {
  value = std::clamp(value, pad::kGameHorizontalMax, pad::kSensitivityCeiling);
  if (value == pad::SensitivityMax()) return false;
  return rex::cvar::SetFlagByName("cod3_pad_sensitivity_max", std::to_string(value));
}

bool RowShown(int row) { return row >= 0 && row < (cod3::kMultiplayer ? kMultiplayerRows : kRows); }

void Saved(int player, int row, const char* how) {
  protected_dvars::SaveSettings();
  REXLOG_INFO("cod3: Options > Controller: {} = {} (player {}{})", LabelText(row), ValueFor(player, row), player + 1,
              how);
}

}  // namespace

const char* LabelKey(int row) { return row >= 0 && row < kRows ? kLabelKeys[row] : kLabelKeys[0]; }

const char* LabelText(int row) {
  const char* key = LabelKey(row);
  for (const Text& t : kTexts)
    if (std::strcmp(key, t.key) == 0) return t.text;
  return "";
}

const char* TitleKey() { return "FEMENU_COP_PAD_TITLE"; }
const char* ButtonKey() { return "FEMENU_COP_PAD_ITEM"; }

std::string ValueFor(int player, int row) {
  if (!RowShown(row)) return {};
  switch (row) {
    case kLimit: return std::to_string(pad::SensitivityMax());
    case kDeadZone: return std::to_string(DeadZoneInUse(player));
    case kRumbleKeys: return GetBool("cod3_rumble_with_keys") ? "ON" : "OFF";
  }
  return {};
}

std::string NoteFor(int player, int row) {
  if (!RowShown(row)) return {};
  switch (row) {
    case kLimit:
      return fmt::format("HOW FAR HORIZ. AND VERT. SENS. GO (THE GAME'S: {} AND {})", pad::kGameHorizontalMax,
                         pad::kGameVerticalMax);
    case kDeadZone:
      return OwnDeadZone(player) >= 0
                 ? fmt::format("THIS PLAYER'S OWN, OF 128 (EVERY CONTROLLER'S: {})", SharedDeadZone())
                 : std::string("OF 128: EVERY CONTROLLER'S (THE GAME'S JOY_THRESHOLD)");
    case kRumbleKeys: return "THE FIRST CONTROLLER, WHILE THE KEYBOARD AND MOUSE ARE IN USE";
  }
  return {};
}

std::string StepFor(int player, int row, bool right) {
  if (!RowShown(row) || !ValidPlayer(player)) return "";
  bool changed = false;
  switch (row) {
    case kLimit: changed = SetLimit(pad::SensitivityMax() + (right ? 1 : -1)); break;
    case kDeadZone: changed = SetDeadZone(player, DeadZoneInUse(player) + (right ? 1 : -1)); break;
    case kRumbleKeys:
      changed = rex::cvar::SetFlagByName("cod3_rumble_with_keys", GetBool("cod3_rumble_with_keys") ? "false" : "true");
      break;
  }
  if (!changed) return "";
  Saved(player, row, "");
  return ValueFor(player, row);
}

bool NumberOfFor(int player, int row, menuslider::Number* n, double* value) {
  if (!RowShown(row) || !n || !value) return false;
  switch (row) {
    case kLimit:
      *n = menuslider::Number{double(pad::kGameHorizontalMax), double(pad::kSensitivityCeiling), 1.0, 1.0, 0, ""};
      *value = pad::SensitivityMax();
      return true;
    case kDeadZone:
      *n = menuslider::Number{0.0, double(kDeadZoneMax), 1.0, 1.0, 0, ""};
      *value = DeadZoneInUse(player);
      return true;
  }
  return false;
}

std::string SetNumberFor(int player, int row, double value) {
  if (!RowShown(row) || !ValidPlayer(player) || !std::isfinite(value)) return "";
  bool changed = false;
  switch (row) {
    case kLimit: changed = SetLimit(int(std::lround(value))); break;
    case kDeadZone: changed = SetDeadZone(player, int(std::lround(value))); break;
    default: return "";
  }
  if (!changed) return "";
  Saved(player, row, ", typed");
  return ValueFor(player, row);
}

std::string Value(int row) { return ValueFor(0, row); }
std::string Note(int row) { return NoteFor(0, row); }
std::string Step(PPCContext& /*ctx*/, uint8_t* /*base*/, int row, bool right) { return StepFor(0, row, right); }
bool NumberOf(int row, menuslider::Number* n, double* value) { return NumberOfFor(0, row, n, value); }
std::string SetNumber(int row, double value) { return SetNumberFor(0, row, value); }

uint32_t Localize(uint8_t* base, const char* key) {
  if (!key || std::strncmp(key, "FEMENU_COP_PAD_", 15) != 0) return 0;
  for (const Text& t : kTexts)
    if (std::strcmp(key, t.key) == 0) return FixedText(base, t.key, t.text);
  return 0;
}

}  // namespace cod3::controllermenu
