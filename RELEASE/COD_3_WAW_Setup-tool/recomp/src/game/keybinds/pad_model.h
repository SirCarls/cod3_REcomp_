// cod3 - [acct2-main] the controller's own binds, per player seat (plain logic;
// the game side is game/keybinds/binds.cpp, the menus game/keybinds/menu_binds.h).
//
// Owner 2026-11-05 (dev/agents/acct2-main/KEYBINDS_MENU.md): a KEY BINDINGS /
// CONTROLLER page in the game's menus, both programs, split screen included:
//   * a row per button, left / right choosing what it does;
//   * per seat (players 1-4), not per profile;
//   * a button does up to three actions ("Allow for up to 3 binds on the same
//     key");
//   * an action is on one button at a time: giving it to a button takes it
//     from the button that had it, which then does nothing until it gets an
//     action ("Unbind the other").
//
// The game's own BUTTON LAYOUT (A-D, its xenon_<layout>.cfg) stays the base:
// a button nobody changed does what the layout gives it, and the game keeps
// writing that into its bind table. A changed button keeps its actions here,
// written over the layout's after every layout exec (binds.cpp).
//
// Stored per seat in cod3.toml / cod3mp.toml as cod3_binds_pad_p1 .. _p4:
// "" = the layout's buttons; otherwise only the changed buttons,
// "a=jump,melee;x=;rs=melee" (an empty list = the button does nothing). Only
// button ids and action ids are ever read; anything else is dropped, never run.
//
// START stays the pause menu's and is never changed (the menus need it).

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "game/keybinds/model.h"

namespace cod3::binds::pad {

enum class Button : uint8_t {
  kA, kB, kX, kY,
  kLB, kRB, kLT, kRT,
  kLS, kRS,              // the stick clicks
  kUp, kDown, kLeft, kRight,  // the D-pad
  kBack, kStart,
  kCount
};
inline constexpr int kButtonCount = static_cast<int>(Button::kCount);
inline constexpr int kPadSlots = 3;  // actions per button
inline constexpr int kSeats = 4;     // split-screen players

// "a", "lb", "dpad_up", "back" ... (cod3_binds_pad_p*), and the menus' names
// ("A", "LEFT BUMPER", "D-PAD UP").
const char* ButtonId(Button b);
const char* ButtonLabel(Button b);
Button ButtonById(std::string_view id);  // Button::kCount when none
// The game's key number for the button (its bind table; PLAN.md 2): A 212,
// B 213, X 211, Y 214, LB 218, RB 217, LT 216, RT 215, LS 220, RS 219, the
// D-pad 154-157, BACK 9, START 27.
int GameKey(Button b);
// START is never changed.
bool Changeable(Button b);

// The actions a controller button can do: the on-foot actions that are the
// game's own commands (not the Vehicle actions, which vehicles read through
// their own button tables, and not the port's settings).
bool PadAction(Action a);
// The game-command text a button's first action puts in the game's bind
// table ("+attack", "weapnext", "togglecrouch").
const char* BindText(Action a);

// One button's actions, in order; Action::kCount = empty.
struct ButtonBinds {
  Action act[kPadSlots] = {Action::kCount, Action::kCount, Action::kCount};
  int Count() const;
  bool Has(Action a) const;
  bool operator==(const ButtonBinds&) const = default;
};

// One seat's changes on top of the layout.
struct PadSet {
  bool custom[kButtonCount] = {};      // false: the layout's own binding
  ButtonBinds binds[kButtonCount] = {};
  bool operator==(const PadSet&) const = default;
};

// What the layout gives each button, as actions (the game's bind table read
// by the caller; ActionByCommand of each binding). A binding that isn't one
// of the actions is Action::kCount, shown by its text in the menus.
struct Layout {
  ButtonBinds binds[kButtonCount] = {};
};

// What the button does now: its changed actions, or the layout's.
ButtonBinds Effective(const PadSet& set, const Layout& layout, Button b);

// cod3_binds_pad_p* -> the changes; notes for anything dropped (may be null).
PadSet Parse(std::string_view text, std::vector<std::string>* notes);
// The changes -> the text ("" when none).
std::string Serialize(const PadSet& set);

// Puts `action` (Action::kCount = nothing) in slot `slot` of button `b`. The
// button becomes changed (its layout actions copied first). Another button
// that has `action` (changed, or by the layout) loses it; the note says so
// ("RELOAD was on X; X does nothing now."). Two slots of one button never
// hold the same action. Returns the note ("" when nothing else changed).
std::string Assign(PadSet& set, const Layout& layout, Button b, int slot, Action action);
// Back to the layout's binding: one button (an action the layout gives it is
// taken from any other button that has it, as Assign does), or all of them.
void Reset(PadSet& set, const Layout& layout, Button b);
void ResetAll(PadSet& set);

// The actions a button's left / right step through, in order: Action::kCount
// (NOTHING) first, then every PadAction in the table's order.
std::vector<Action> Choices();

}  // namespace cod3::binds::pad
