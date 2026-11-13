// cod3 - [acct2-main] the KEY BINDINGS pages as rows (see menu_binds.h).

#include "game/keybinds/menu_binds.h"

#include <algorithm>
#include <cctype>
#include <vector>

namespace cod3::binds::menu {
namespace {

constexpr const char* kMoveCvars[kMoveRows] = {"keybind_lstick_up", "keybind_lstick_down", "keybind_lstick_left",
                                               "keybind_lstick_right"};
constexpr const char* kMoveLabels[kMoveRows] = {"FORWARD", "BACK", "STRAFE LEFT", "STRAFE RIGHT"};

// Controller page: the changeable buttons (every button but START, which is
// the last), then RESET TO LAYOUT.
constexpr int kPadButtonRows = pad::kButtonCount - 1;
static_assert(static_cast<int>(pad::Button::kStart) == pad::kButtonCount - 1);
// Keyboard page: AIM, the movement rows, every action, RESET KEYS.
constexpr int kFirstMove = 1;
constexpr int kFirstAction = kFirstMove + kMoveRows;
constexpr int kKeyboardRows = kFirstAction + kActionCount + 1;

pad::Button ButtonOf(int row) { return static_cast<pad::Button>(row); }
Action ActionOf(int row) { return static_cast<Action>(row - kFirstAction); }

std::string Mark(const std::string& text, bool on) { return on ? "[" + text + "]" : text; }

std::string KeyText(KeyCode k) { return k == kNoKey ? "--" : KeyName(k); }

std::string ActionText(Action a) { return a == Action::kCount ? "--" : Upper(Def(a).label); }

// The slot the cursor shows on `row` (0 when the cursor is on another row).
int SlotOn(const Cursor& c, int row) { return c.row == row ? c.slot : 0; }

void Follow(Cursor& c, int row) {
  if (c.row != row) {
    c.row = row;
    c.slot = 0;
    c.capturing = false;
  }
}

}  // namespace

const char* MoveRowCvar(int i) { return i >= 0 && i < kMoveRows ? kMoveCvars[i] : ""; }

std::string Upper(std::string_view text) {
  std::string out(text);
  for (char& ch : out) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
  return out;
}

int RowCount(Page page) { return page == Page::kController ? kPadButtonRows + 1 : kKeyboardRows; }

RowKind Kind(Page page, int row) {
  if (row == RowCount(page) - 1) return RowKind::kReset;
  if (page == Page::kController) return RowKind::kButton;
  if (row == 0) return RowKind::kAim;
  if (row < kFirstAction) return RowKind::kMove;
  return RowKind::kAction;
}

std::string Label(Page page, int row) {
  if (row < 0 || row >= RowCount(page)) return "";
  switch (Kind(page, row)) {
    case RowKind::kButton: return pad::ButtonLabel(ButtonOf(row));
    case RowKind::kAim: return "AIM DOWN THE SIGHTS";
    case RowKind::kMove: return kMoveLabels[row - kFirstMove];
    case RowKind::kAction: return Upper(Def(ActionOf(row)).label);
    case RowKind::kReset: return page == Page::kController ? "RESET TO LAYOUT" : "RESET KEYS";
  }
  return "";
}

std::string Value(Page page, int row, const State& s, const Cursor& c) {
  if (row < 0 || row >= RowCount(page)) return "";
  const int slot = SlotOn(c, row);
  const bool here = c.row == row;
  switch (Kind(page, row)) {
    case RowKind::kButton: {
      const pad::Button b = ButtonOf(row);
      const pad::ButtonBinds bb = pad::Effective(s.pad, s.layout, b);
      const int i = static_cast<int>(b);
      std::string out;
      for (int k = 0; k < pad::kPadSlots; ++k) {
        std::string text = ActionText(bb.act[k]);
        // A layout binding that isn't one of the actions, shown as the game has it.
        if (k == 0 && !s.pad.custom[i] && bb.act[0] == Action::kCount && !s.layout_text[i].empty())
          text = Upper(s.layout_text[i]);
        if (k) out += "  ";
        out += std::to_string(k + 1) + ": " + Mark(text, here && k == slot);
      }
      return out;
    }
    case RowKind::kAim: return s.aim_toggle ? "TOGGLE" : "HOLD";
    case RowKind::kMove:
    case RowKind::kAction: {
      std::string out;
      for (int k = 0; k < kSlots; ++k) {
        const KeyCode key = Kind(page, row) == RowKind::kMove ? s.move[row - kFirstMove][k]
                                                                : s.kbm.key[static_cast<int>(ActionOf(row))][k];
        std::string text = here && k == slot && c.capturing ? "PRESS A KEY - ESC CANCELS" : KeyText(key);
        if (k) out += "    ";
        out += Mark(text, here && k == slot);
      }
      return out;
    }
    case RowKind::kReset: return "";
  }
  return "";
}

std::string Help(Page page, int row, const State& s) {
  if (row < 0 || row >= RowCount(page)) return "";
  switch (Kind(page, row)) {
    case RowKind::kButton:
      return "Up to three actions for " + std::string(pad::ButtonLabel(ButtonOf(row))) +
             ". A: the next slot. LEFT / RIGHT: change it. X: clear it. An action is on one button at a time.";
    case RowKind::kAim: return "Hold the aim key to aim, or press it once to aim and again to stop.";
    case RowKind::kMove: return "Walking and steering. A: press the new key. LEFT / RIGHT: key 1 or 2.";
    case RowKind::kAction: {
      const Action a = ActionOf(row);
      std::string h = Def(a).help;
      if (Def(a).group == Group::kVehicle) h += " Counts only while you drive.";
      return h + " A: press the new key. LEFT / RIGHT: key 1 or 2. BACKSPACE: clear it.";
    }
    case RowKind::kReset:
      return page == Page::kController
                 ? "Every button back to " + std::string(s.multiplayer ? "the layout" : "the BUTTON LAYOUT") + "'s own."
                 : "Every key back to its default.";
  }
  return "";
}

std::string Title(Page page, int row, const State& s, bool split) {
  std::string t = page == Page::kController ? "KEY BINDINGS / CONTROLLER" : "KEY BINDINGS / KEYBOARD & MOUSE";
  if (page == Page::kKeyboard && row >= 0 && row < RowCount(page)) {
    std::string group;
    switch (Kind(page, row)) {
      case RowKind::kAim: group = "AIM"; break;
      case RowKind::kMove: group = "MOVEMENT"; break;
      case RowKind::kAction: group = Upper(GroupName(Def(ActionOf(row)).group)); break;
      default: break;
    }
    if (!group.empty())
      t += ": " + group + " (" + std::to_string(row + 1) + "/" + std::to_string(RowCount(page)) + ")";
  }
  if (split) t += " - PLAYER " + std::to_string(s.seat + 1);
  return t;
}

Result Step(Page page, int row, bool right, State& s, Cursor& c) {
  Result r;
  if (row < 0 || row >= RowCount(page)) return r;
  Follow(c, row);
  switch (Kind(page, row)) {
    case RowKind::kButton: {
      const pad::Button b = ButtonOf(row);
      const std::vector<Action> choices = pad::Choices();
      const Action now = pad::Effective(s.pad, s.layout, b).act[c.slot];
      int at = 0;
      for (size_t i = 0; i < choices.size(); ++i)
        if (choices[i] == now) at = static_cast<int>(i);
      const int n = static_cast<int>(choices.size());
      const Action next = choices[(at + (right ? 1 : n - 1)) % n];
      const pad::PadSet before = s.pad;
      r.note = pad::Assign(s.pad, s.layout, b, c.slot, next);
      r.changed = !(s.pad == before);
      // The action lands in the first free slot when the ones before are empty.
      const pad::ButtonBinds bb = pad::Effective(s.pad, s.layout, b);
      for (int k = 0; k < pad::kPadSlots; ++k)
        if (next != Action::kCount && bb.act[k] == next) c.slot = k;
      return r;
    }
    case RowKind::kAim:
      s.aim_toggle = !s.aim_toggle;
      r.changed = true;
      return r;
    case RowKind::kMove:
    case RowKind::kAction:
      c.capturing = false;
      c.slot = right ? std::min(c.slot + 1, kSlots - 1) : std::max(c.slot - 1, 0);
      return r;
    case RowKind::kReset: return r;
  }
  return r;
}

Result Select(Page page, int row, State& s, Cursor& c) {
  Result r;
  if (row < 0 || row >= RowCount(page)) return r;
  Follow(c, row);
  switch (Kind(page, row)) {
    case RowKind::kButton: {
      // The next slot, up to the first empty one.
      const int count = pad::Effective(s.pad, s.layout, ButtonOf(row)).Count();
      const int last = std::min(count, pad::kPadSlots - 1);
      c.slot = c.slot >= last ? 0 : c.slot + 1;
      return r;
    }
    case RowKind::kAim:
      s.aim_toggle = !s.aim_toggle;
      r.changed = true;
      return r;
    case RowKind::kMove:
    case RowKind::kAction:
      c.capturing = true;
      return r;
    case RowKind::kReset:
      r.ask_reset = true;
      return r;
  }
  return r;
}

Result Clear(Page page, int row, State& s, Cursor& c) {
  Result r;
  if (row < 0 || row >= RowCount(page)) return r;
  Follow(c, row);
  c.capturing = false;
  switch (Kind(page, row)) {
    case RowKind::kButton: {
      const pad::PadSet before = s.pad;
      r.note = pad::Assign(s.pad, s.layout, ButtonOf(row), c.slot, Action::kCount);
      r.changed = !(s.pad == before);
      return r;
    }
    case RowKind::kMove: {
      KeyCode* keys = s.move[row - kFirstMove];
      r.changed = keys[c.slot] != kNoKey;
      keys[c.slot] = kNoKey;
      if (c.slot == 0 && keys[1] != kNoKey) std::swap(keys[0], keys[1]);
      return r;
    }
    case RowKind::kAction: {
      const KbmSet before = s.kbm;
      Assign(s.kbm, ActionOf(row), c.slot, kNoKey);
      r.changed = !(s.kbm == before);
      return r;
    }
    default: return r;
  }
}

Result Captured(KeyCode key, State& s, Cursor& c, const std::function<std::string(KeyCode)>& reserved) {
  Result r;
  if (!c.capturing || c.row < 0) return r;
  if (key == kNoKey) {
    c.capturing = false;
    return r;
  }
  std::string why = IsAlwaysReserved(key) ? "the console or the pause menu" : "";
  if (why.empty() && reserved) why = reserved(key);
  if (!why.empty()) {
    r.note = KeyName(key) + " can't be bound (" + why + "). Press another key, or ESC.";
    return r;  // still waiting
  }
  c.capturing = false;
  const int row = c.row;
  if (Kind(Page::kKeyboard, row) == RowKind::kMove) {
    const int m = row - kFirstMove;
    for (int i = 0; i < kMoveRows; ++i)
      for (int k = 0; k < kSlots; ++k)
        if (s.move[i][k] == key && !(i == m && k == c.slot)) {
          s.move[i][k] = kNoKey;
          if (i != m) r.note = KeyName(key) + " was " + kMoveLabels[i] + "; moved here.";
        }
    s.move[m][c.slot] = key;
    for (int i = 0; i < kMoveRows; ++i)  // an empty first key takes the second
      if (s.move[i][0] == kNoKey && s.move[i][1] != kNoKey) std::swap(s.move[i][0], s.move[i][1]);
    r.changed = true;
    return r;
  }
  if (Kind(Page::kKeyboard, row) != RowKind::kAction) return r;
  bool ok = true;
  const KbmSet before = s.kbm;
  const std::string note = Assign(s.kbm, ActionOf(row), c.slot, key, &ok);
  if (!note.empty()) r.note = note + ".";
  r.changed = !(s.kbm == before);
  // The key lands in the first free slot when the one before is empty.
  for (int k = 0; k < kSlots; ++k)
    if (s.kbm.key[static_cast<int>(ActionOf(row))][k] == key) c.slot = k;
  return r;
}

Result ResetAll(Page page, State& s) {
  Result r;
  if (page == Page::kController) {
    const pad::PadSet before = s.pad;
    pad::ResetAll(s.pad);
    r.changed = !(s.pad == before);
    r.note = "Every button does what the layout gives it.";
  } else {
    s.kbm = Defaults();
    s.aim_toggle = false;
    r.changed = true;  // the movement rows' defaults are the caller's (it resets them too)
    r.note = "Every key is back to its default.";
  }
  return r;
}

int FirstVisible(int selected, int visible, int count, int first) {
  if (visible <= 0 || count <= 0) return 0;
  if (selected < first) first = selected;
  if (selected >= first + visible) first = selected - visible + 1;
  return std::clamp(first, 0, std::max(0, count - visible));
}

}  // namespace cod3::binds::menu
