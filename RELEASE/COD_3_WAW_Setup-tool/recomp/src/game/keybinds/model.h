// cod3 - the port's keyboard and mouse actions and their keys (plain logic;
// the game side is game/keybinds/binds.cpp).
//
// First step of the key binds plan (dev/agents/keybinds/PLAN.md): the actions that
// used to be reached only through controller buttons the keyboard pretended
// to press (the left stick click, the D-pad) are actions of their own. Each
// action is one of the game's own commands, called directly with a key id of
// its own (binds.h), never a controller button:
//   binoculars     +binoculars    (the pad's L3 in layouts A, B, D; B in C)  B
//   sprintbreath   +sprintbreath  World at War's Shift: hold breath while   SHIFT
//                                 aiming or through the binoculars, sprint
//                                 otherwise (game/movement)
//   holdbreath     +holdbreath    hold breath only (the pad's D-pad up)      -
//   sprint         +sprint        sprint only (game/movement)                -
// Up to two keys per action; a key belongs to one action.
//
// Stored in cod3.toml as cod3_binds_kbm_p1: "" = these defaults, otherwise
// every action: "binoculars=B;sprintbreath=SHIFT;holdbreath=;sprint=" (an
// empty list = unbound). Only action ids and key names are ever read; any
// other text is dropped, never run.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "game/keybinds/keys.h"

namespace cod3::binds {

enum class Action : uint8_t { kBinoculars, kSprintBreath, kHoldBreath, kSprint, kCount };
inline constexpr int kActionCount = static_cast<int>(Action::kCount);
inline constexpr int kSlots = 2;

struct ActionDef {
  const char* id;              // in cod3.toml: "binoculars"
  const char* label;           // F2 and messages: "Binoculars"
  const char* help;            // F2: one sentence
  const char* command;         // the game's command: "+binoculars"
  const char* equivalents[3];  // other game bindings that count as this action
                               // (prompts; the controller button used inside an
                               // interaction), nullptr-terminated
  KeyCode defaults[kSlots];    // World at War style
};

const ActionDef& Def(Action action);
// By id or by the game's command (case-insensitive); Action::kCount when none.
Action ActionById(std::string_view id);
Action ActionByCommand(std::string_view command);

struct KbmSet {
  KeyCode key[kActionCount][kSlots] = {};
  bool operator==(const KbmSet&) const = default;
};

KbmSet Defaults();

// cod3_binds_kbm_p1 -> the keys. "" = the defaults. Unknown ids or keys,
// reserved keys, combinations and keys already taken are dropped, each with a
// note in `notes` (may be null). At most 4 KB are read.
KbmSet Parse(std::string_view text, std::vector<std::string>* notes);
// The keys -> cod3_binds_kbm_p1: "" when they are the defaults.
std::string Serialize(const KbmSet& set);

// The action and slot that have `key`; Action::kCount when none.
Action Find(const KbmSet& set, KeyCode key, int* slot = nullptr);

// Puts `key` in `slot` of `action` (kNoKey clears the slot). A key another
// action had is taken from it; the note says so ("B was Binoculars; moved
// here"). Returns "" when nothing else changed.
std::string Assign(KbmSet& set, Action action, int slot, KeyCode key);

// Both keys of an action as text ("B", "B, MOUSE4", "" when unbound) and back
// (F2's text box). AssignText sets both slots from "B" / "B, MOUSE4" / "";
// false (and nothing changed) when a name isn't a key or a key is reserved,
// with the reason in `note`.
std::string KeysText(const KbmSet& set, Action action);
bool AssignText(KbmSet& set, Action action, std::string_view text, std::string* note);

// --- one-time move of the old controller-button keys (PLAN.md section 10) ------
// The keyboard used to press controller buttons through keybind_* settings:
// keybind_lstick_press = "Shift" (L3: binoculars), keybind_dpad_up =
// "Shift+Up" (D-pad up: hold breath) ... The campaign doesn't use those any
// more (app_defaults.h). A player's own keys there move to the actions; the
// old defaults and every Shift+key combination go.
struct OldBind {
  std::string name;   // "keybind_lstick_press"
  std::string value;  // its value now
  bool from_file;     // the player's cod3.toml set it (else nothing to move)
};
struct Migration {
  KbmSet set;                 // the keys after the move
  bool set_changed = false;   // `set` differs from what came in
  // keybind_* -> its new value; "" means back to its (campaign) default.
  std::vector<std::pair<std::string, std::string>> new_values;
  std::vector<std::string> log;  // one line each, for the log
};
// `others`: every other keybind_* value, so a key still pressing another
// controller button (keybind_x "R,F") stays there.
Migration Migrate(const KbmSet& current, const std::vector<OldBind>& binds,
                  const std::vector<OldBind>& others);

}  // namespace cod3::binds
