// cod3 - [acct2-main] the KEY BINDINGS pages of the game's own menus, as rows
// any menu can show (plain logic; no game calls).
//
// Owner 2026-11-05 (dev/agents/acct2-main/KEYBINDS_MENU.md): "an in-game menu
// for keybinds. One for controller and one for mouse and keyboard", in the
// campaign and multiplayer, split screen and the main menu. Each program's
// menus (game/menu_fov.cpp, game/mp/options_mp.cpp) place these rows on
// their own panels: a label, a value, a help line, and left / right / A /
// clear on the highlighted row. The rows and what they do are here, once.
//
// CONTROLLER (a seat's pad_model.h set): a row per button but START, the
// value its three slots ("1: RELOAD  2: MELEE  3: --"); A moves the
// highlighted slot, left / right step it through the actions; then RESET
// TO LAYOUT.
//
// KEYBOARD & MOUSE (model.h's set; the keyboard player's only): AIM (HOLD /
// TOGGLE), the four movement rows (the keyboard's left-stick rows, keybind_*),
// every action, then RESET KEYS. The value shows both keys ("F    MOUSE4");
// left / right pick key 1 or 2, A arms a capture (the next key, mouse button
// or wheel notch: Captured), clear empties the chosen key.
//
// The pages are long (about 45 rows): FirstVisible gives the window of rows a
// panel of `visible` rows shows, and Title names the group of the
// highlighted row ("KEY BINDINGS / KEYBOARD & MOUSE: COMBAT (3/12)").

#pragma once

#include <cstdint>
#include <functional>
#include <string>

#include "game/keybinds/keys.h"
#include "game/keybinds/model.h"
#include "game/keybinds/pad_model.h"

namespace cod3::binds::menu {

enum class Page : uint8_t { kController, kKeyboard };

// The four movement rows on the keyboard page (keybind_lstick_up ... : the
// SDK's keyboard rows, whose keys the caller reads and writes).
inline constexpr int kMoveRows = 4;
const char* MoveRowCvar(int i);  // "keybind_lstick_up" ...

// What the pages show and change. The caller fills it from the settings
// (and the seat's layout from the game's bind table) and stores what changed.
struct State {
  pad::PadSet pad;              // the seat's changes
  pad::Layout layout;           // the seat's layout, as actions
  std::string layout_text[pad::kButtonCount];  // a layout binding that isn't an action, as the game has it
  KbmSet kbm;
  bool aim_toggle = false;      // cod3_aim_toggle_kbm_p1
  KeyCode move[kMoveRows][kSlots] = {};  // the movement rows' keys
  bool multiplayer = false;
  int seat = 0;                 // 0-3
};

// Per page, kept by the menu while it is open.
struct Cursor {
  int slot = 0;          // the highlighted slot of the highlighted row
  int row = -1;          // the row `slot` is for (moving to another row starts at slot 0)
  bool capturing = false;  // keyboard page: waiting for a key for `row`, `slot`
  int first = 0;         // the first visible row (FirstVisible)
};

enum class RowKind : uint8_t { kButton, kAim, kMove, kAction, kReset };

int RowCount(Page page);
RowKind Kind(Page page, int row);

std::string Label(Page page, int row);
// The highlighted slot is marked [ ] when `row` is the cursor's row.
std::string Value(Page page, int row, const State& s, const Cursor& c);
std::string Help(Page page, int row, const State& s);
// "KEY BINDINGS / CONTROLLER" (+ " - PLAYER 2" in multiplayer split screen
// when `split`), or the keyboard page's with the highlighted row's group.
std::string Title(Page page, int row, const State& s, bool split);

struct Result {
  std::string note;        // what to show on the note line ("" = nothing to say)
  bool changed = false;    // State changed: store it
  bool ask_reset = false;  // a RESET row was chosen: ask Yes / No, then ResetAll
};

// Left / right on the row.
Result Step(Page page, int row, bool right, State& s, Cursor& c);
// A / ENTER / a click on the row.
Result Select(Page page, int row, State& s, Cursor& c);
// BACKSPACE / DELETE / the pad's X: empties the highlighted slot.
Result Clear(Page page, int row, State& s, Cursor& c);
// The key taken while capturing (kNoKey = cancelled: ESCAPE, B, closing).
// `reserved` says why a key can't be bound ("" = it can; F2, F3 ... are the
// caller's).
Result Captured(KeyCode key, State& s, Cursor& c, const std::function<std::string(KeyCode)>& reserved);
// After the Yes of ask_reset.
Result ResetAll(Page page, State& s);

// The first row a panel of `visible` rows shows so that `selected` is in
// view, starting from the window it showed before (`first`).
int FirstVisible(int selected, int visible, int count, int first);

// The text's capitals, as the game's menus show names.
std::string Upper(std::string_view text);

}  // namespace cod3::binds::menu
