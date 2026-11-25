// inputkit - a game's keyboard and mouse actions and their keys: the bind model shared by every game.
//
// [input-port] 2026-11-24, taken out of Call of Duty 3's game/keybinds/model.cpp (the game-free half: parse,
// serialize, conflicts, F2's text boxes). Each game passes its own action table; CoD3 passes its 41 actions and gets
// exactly the results it had (dev/agents/keybinds/tests/test_binds.cpp).
//
// The rules (CoD3's, owner 2026-10-19 .. 2026-11-05):
//   * each action has up to two keys (kSlots);
//   * a key can be given to up to three actions (kMaxActionsPerKey, of any kind); a press does all of them;
//   * actions have a kind (CoD3: 0 on foot, 1 vehicle). Kinds only matter to the old "a key moves" rule
//     (AssignMoving, used by one-time moves of old settings) and to Parse's defaults (an action the text doesn't
//     list keeps its default keys unless an action of its own kind has them);
//   * ` / ~ and ESCAPE are never bound (keys.h IsAlwaysReserved).
//
// The text form (one setting, e.g. CoD3's cod3_binds_kbm_p1, Big Red One's input.binds_kbm):
//   "" = the defaults; otherwise "fire=MOUSE1;aim=MOUSE2;use=F;...;sprint=" (an empty list = unbound; an action the
//   text doesn't list keeps its default unless its key went elsewhere). Only action ids and key names are ever
//   read; anything else is dropped (with a note), never run. At most 4 KB are read.
//
// Keys are held in a KeyRow array, one row per action, in the table's order: the caller owns it (CoD3's KbmSet is
// one), so nothing here allocates per call except the strings it returns.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "inputkit/keys.h"

namespace inputkit::binds {

inline constexpr int kSlots = 2;
inline constexpr int kMaxActionsPerKey = 3;

using KeyRow = KeyCode[kSlots];

struct ActionSpec {
  const char* id;           // in the settings: "reload"
  const char* label;        // F2 and notes: "Reload"
  uint8_t kind;             // 0 on foot; games with vehicles: 1 vehicle
  KeyCode defaults[kSlots];
};

struct Table {
  const ActionSpec* actions;
  int count;
  const char* setting_name;  // for notes: "cod3_binds_kbm_p1"
};

// -1 when none (any case; surrounding spaces ignored).
int ActionById(const Table& t, std::string_view id);

void Defaults(const Table& t, KeyRow* keys);
bool Same(const Table& t, const KeyRow* a, const KeyRow* b);
bool IsDefault(const Table& t, const KeyRow* keys);

// Slot 1 moves to slot 0 when slot 0 is empty.
void Compact(KeyRow* keys, int action);

// The first action of `kind` with `key` and its slot; -1 when none.
int Find(const Table& t, const KeyRow* keys, KeyCode key, int* slot, uint8_t kind);
// Every action with `key` (any kind, in the table's order) and its slot; returns the count (at most `max`).
struct KeyUse {
  int action;
  int slot;
};
int FindAll(const Table& t, const KeyRow* keys, KeyCode key, KeyUse* out, int max);

// Text -> keys (see the top). Notes for everything dropped (may be null).
void Parse(const Table& t, std::string_view text, KeyRow* keys, std::vector<std::string>* notes);
// Keys -> text; "" when they are the defaults.
std::string Serialize(const Table& t, const KeyRow* keys);

// Puts `key` in `slot` of `action` (kNoKey clears it). The key keeps its other actions, up to kMaxActionsPerKey in
// all; the note says what else it does ("G also does Smoke grenade"). A key that has that many already is refused:
// nothing changes, *ok is false and the note says why. The action's other slot never keeps the same key.
std::string Assign(const Table& t, KeyRow* keys, int action, int slot, KeyCode key, bool* ok = nullptr);
// The older rule: a key another action of the same kind had is taken from it ("B was Binoculars; moved here").
std::string AssignMoving(const Table& t, KeyRow* keys, int action, int slot, KeyCode key);

// Both keys of an action as text ("B", "B, MOUSE4", "" when unbound) and back (F2's text box): AssignText sets both
// slots from "B" / "B, MOUSE4" / ""; false (nothing changed) when a name isn't a key or a key is reserved, with the
// reason in `note`.
std::string KeysText(const KeyRow* keys, int action);
bool AssignText(const Table& t, KeyRow* keys, int action, std::string_view text, std::string* note);

// Text helpers the games' own parsers share.
std::string_view Trim(std::string_view s);
bool SameText(std::string_view a, std::string_view b);  // ASCII, any case
std::vector<std::string_view> Split(std::string_view s, char sep);  // trimmed pieces, empty ones skipped
bool HasControlChars(std::string_view s);  // below 0x20, tab allowed

}  // namespace inputkit::binds
