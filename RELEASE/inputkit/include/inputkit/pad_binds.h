// inputkit - a controller's own binds, per player seat: the model shared by every game.
//
// [input-port] 2026-11-24, taken out of Call of Duty 3's game/keybinds/pad_model.cpp (acct2-main, owner 2026-11-05:
// a KEY BINDINGS / CONTROLLER page, per seat, "Allow for up to 3 binds on the same key", "Unbind the other"). Each
// game passes its buttons (an Xbox 360 pad's 16, an original Xbox pad's) and which of its actions a button may do.
//
// The rules:
//   * a button does up to three actions (kPadSlots);
//   * an action is on one button at a time: giving it to a button takes it from the button that had it;
//   * the game's own layout stays the base: a button nobody changed does what the layout gives it;
//   * buttons a game marks unchangeable (START: the pause menu) are never changed.
//
// Text form, per seat (CoD3's cod3_binds_pad_p1 .. _p4): "" = the layout; otherwise only the changed buttons,
// "a=jump,melee;x=;rs=melee" (an empty list = the button does nothing). Only button ids and action ids are ever
// read; anything else is dropped, never run. At most 2 KB are read.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "inputkit/binds.h"

namespace inputkit::padbinds {

inline constexpr int kPadSlots = 3;
inline constexpr int kMaxButtons = 24;
inline constexpr int kNone = -1;  // an empty slot

struct ButtonSpec {
  const char* id;     // "a", "lb", "dpad_up" (the text form)
  const char* label;  // "A", "LEFT BUMPER" (the menus)
  int game_key;       // the game's number for it (its bind table), or -1
  bool changeable;    // false: START
  // Why it can't be changed, for the note when a setting lists it ("START stays the pause menu's"); null: "<label>
  // can't be changed".
  const char* locked_note = nullptr;
};

struct Pad {
  const ButtonSpec* buttons;  // at most kMaxButtons
  int button_count;
  const binds::Table* actions;
  // Whether a button may do this action (CoD3: the on-foot actions that are game commands). Null: every action.
  bool (*allowed)(int action);
};

struct ButtonBinds {
  int act[kPadSlots] = {kNone, kNone, kNone};
  int Count() const;
  bool Has(int action) const;
  bool operator==(const ButtonBinds&) const = default;
};

// One seat's changes on top of the layout.
struct PadSet {
  bool custom[kMaxButtons] = {};
  ButtonBinds binds[kMaxButtons] = {};
  bool operator==(const PadSet&) const = default;
};

// What the layout gives each button (the game's bind table, read by the caller).
struct Layout {
  ButtonBinds binds[kMaxButtons] = {};
};

int ButtonById(const Pad& pad, std::string_view id);  // -1 when none
bool Allowed(const Pad& pad, int action);
void Compact(ButtonBinds& bb);  // empty slots to the end, order kept

ButtonBinds Effective(const PadSet& set, const Layout& layout, int button);
PadSet Parse(const Pad& pad, std::string_view text, std::vector<std::string>* notes);
std::string Serialize(const Pad& pad, const PadSet& set);
// Puts `action` (kNone = nothing) in `slot` of `button`; another button that has it loses it (the note says so).
std::string Assign(const Pad& pad, PadSet& set, const Layout& layout, int button, int slot, int action);
// Back to the layout: one button (an action the layout gives it is taken from any changed button that has it).
void Reset(const Pad& pad, PadSet& set, const Layout& layout, int button);
// kNone first, then every allowed action in the table's order.
std::vector<int> Choices(const Pad& pad);

}  // namespace inputkit::padbinds
