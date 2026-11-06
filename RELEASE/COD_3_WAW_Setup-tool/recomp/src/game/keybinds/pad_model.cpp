// cod3 - [acct2-main] the controller's own binds per seat (see pad_model.h).

#include "game/keybinds/pad_model.h"

#include <cctype>

namespace cod3::binds::pad {
namespace {

struct ButtonDef {
  const char* id;
  const char* label;
  int game_key;
};
// The game's key numbers: the pad poll's table (sub_8251DA00: 16 entries,
// X 211 ... LS 220 as entry + 207, START 27, BACK 9, the D-pad 154-157 in the
// keyboard arrows' order: UPARROW 154, DOWNARROW 155, LEFTARROW 156,
// RIGHTARROW 157).
constexpr ButtonDef kButtons[] = {
    {"a", "A", 212},
    {"b", "B", 213},
    {"x", "X", 211},
    {"y", "Y", 214},
    {"lb", "LEFT BUMPER", 218},
    {"rb", "RIGHT BUMPER", 217},
    {"lt", "LEFT TRIGGER", 216},
    {"rt", "RIGHT TRIGGER", 215},
    {"ls", "LEFT STICK CLICK", 220},
    {"rs", "RIGHT STICK CLICK", 219},
    {"dpad_up", "D-PAD UP", 154},
    {"dpad_down", "D-PAD DOWN", 155},
    {"dpad_left", "D-PAD LEFT", 156},
    {"dpad_right", "D-PAD RIGHT", 157},
    {"back", "BACK", 9},
    {"start", "START", 27},
};
static_assert(sizeof(kButtons) / sizeof(kButtons[0]) == static_cast<size_t>(kButtonCount));

constexpr size_t kMaxText = 2048;

int Index(Button b) {
  const int i = static_cast<int>(b);
  return i >= 0 && i < kButtonCount ? i : 0;
}

bool SameText(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  return true;
}

std::string_view Trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

// Pieces between `sep`, trimmed; empty pieces are kept only when `keep_empty`.
std::vector<std::string_view> Split(std::string_view s, char sep, bool keep_empty = false) {
  std::vector<std::string_view> out;
  while (true) {
    const size_t at = s.find(sep);
    const std::string_view piece = Trim(s.substr(0, at));
    if (!piece.empty() || keep_empty) out.push_back(piece);
    if (at == std::string_view::npos) break;
    s.remove_prefix(at + 1);
  }
  return out;
}

bool HasControlChars(std::string_view s) {
  for (char c : s)
    if (static_cast<unsigned char>(c) < 0x20 && c != '\t') return true;
  return false;
}

// Empty slots go to the end, the order of the others kept.
void Compact(ButtonBinds& bb) {
  ButtonBinds out;
  int n = 0;
  for (Action a : bb.act)
    if (a != Action::kCount) out.act[n++] = a;
  bb = out;
}

}  // namespace

const char* ButtonId(Button b) { return kButtons[Index(b)].id; }
const char* ButtonLabel(Button b) { return kButtons[Index(b)].label; }
int GameKey(Button b) { return kButtons[Index(b)].game_key; }
bool Changeable(Button b) { return b != Button::kStart && b != Button::kCount; }

Button ButtonById(std::string_view id) {
  id = Trim(id);
  for (int i = 0; i < kButtonCount; ++i)
    if (SameText(id, kButtons[i].id)) return static_cast<Button>(i);
  return Button::kCount;
}

bool PadAction(Action a) {
  if (a == Action::kCount || static_cast<int>(a) >= kActionCount) return false;
  const ActionDef& d = Def(a);
  if (d.group == Group::kVehicle) return false;  // vehicles read their own button tables
  switch (d.delivery) {
    case Delivery::kButton:
    case Delivery::kPlain:
    case Delivery::kOnce:
    case Delivery::kText:
    case Delivery::kAim:
      return true;
    default:
      return false;  // kAlias, kView (vehicles), kPort (a port setting, not a game command)
  }
}

const char* BindText(Action a) { return PadAction(a) ? Def(a).command : ""; }

int ButtonBinds::Count() const {
  int n = 0;
  for (Action a : act) n += a != Action::kCount;
  return n;
}

bool ButtonBinds::Has(Action a) const {
  if (a == Action::kCount) return false;
  for (Action x : act)
    if (x == a) return true;
  return false;
}

ButtonBinds Effective(const PadSet& set, const Layout& layout, Button b) {
  const int i = Index(b);
  return set.custom[i] ? set.binds[i] : layout.binds[i];
}

PadSet Parse(std::string_view text, std::vector<std::string>* notes) {
  auto note = [notes](std::string line) {
    if (notes) notes->push_back(std::move(line));
  };
  PadSet set;
  text = Trim(text);
  if (text.empty()) return set;
  if (text.size() > kMaxText) {
    note("the controller binds are longer than 2 KB; the rest is ignored");
    text = text.substr(0, kMaxText);
  }
  bool taken[kActionCount] = {};  // an action is on one button at a time: the first keeps it
  for (std::string_view entry : Split(text, ';')) {
    if (HasControlChars(entry)) {
      note("a controller bind entry with control characters was ignored");
      continue;
    }
    const size_t eq = entry.find('=');
    if (eq == std::string_view::npos) {
      note("'" + std::string(entry) + "' isn't button=actions; ignored");
      continue;
    }
    const Button b = ButtonById(entry.substr(0, eq));
    if (b == Button::kCount) {
      note("'" + std::string(Trim(entry.substr(0, eq))) + "' isn't a controller button; ignored");
      continue;
    }
    if (!Changeable(b)) {
      note("START stays the pause menu's; its entry is ignored");
      continue;
    }
    const int i = Index(b);
    if (set.custom[i]) {
      note(std::string(ButtonId(b)) + " is listed twice; the first one counts");
      continue;
    }
    set.custom[i] = true;
    int slot = 0;
    for (std::string_view id : Split(entry.substr(eq + 1), ',')) {
      const Action a = ActionById(id);
      if (!PadAction(a)) {
        note("'" + std::string(id) + "' isn't an action a controller button can do; ignored");
        continue;
      }
      if (taken[static_cast<int>(a)]) {
        note(std::string(Def(a).id) + " is on two buttons; the first one keeps it");
        continue;
      }
      if (slot >= kPadSlots) {
        note(std::string(ButtonId(b)) + ": three actions at most; " + Def(a).id + " ignored");
        continue;
      }
      taken[static_cast<int>(a)] = true;
      set.binds[i].act[slot++] = a;
    }
  }
  return set;
}

std::string Serialize(const PadSet& set) {
  std::string out;
  for (int i = 0; i < kButtonCount; ++i) {
    if (!set.custom[i]) continue;
    if (!out.empty()) out += ';';
    out += kButtons[i].id;
    out += '=';
    bool first = true;
    for (Action a : set.binds[i].act) {
      if (a == Action::kCount) continue;
      if (!first) out += ',';
      out += Def(a).id;
      first = false;
    }
  }
  return out;
}

std::string Assign(PadSet& set, const Layout& layout, Button b, int slot, Action action) {
  if (!Changeable(b) || slot < 0 || slot >= kPadSlots) return "";
  if (action != Action::kCount && !PadAction(action)) return "";
  const int i = Index(b);
  ButtonBinds mine = Effective(set, layout, b);
  if (mine.act[slot] == action) return "";
  std::string note;
  if (action != Action::kCount) {
    // Another button that has it loses it (owner: "Unbind the other").
    for (int j = 0; j < kButtonCount; ++j) {
      if (j == i) continue;
      const Button other = static_cast<Button>(j);
      ButtonBinds theirs = Effective(set, layout, other);
      if (!theirs.Has(action)) continue;
      for (Action& a : theirs.act)
        if (a == action) a = Action::kCount;
      Compact(theirs);
      set.custom[j] = true;
      set.binds[j] = theirs;
      note = std::string(Def(action).label) + " was on " + ButtonLabel(other) + "; " + ButtonLabel(other) +
             (theirs.Count() ? " keeps the rest" : " does nothing now");
    }
    // The same action in another slot of this button moves here.
    for (int s = 0; s < kPadSlots; ++s)
      if (s != slot && mine.act[s] == action) mine.act[s] = Action::kCount;
  }
  mine.act[slot] = action;
  Compact(mine);
  set.custom[i] = true;
  set.binds[i] = mine;
  return note;
}

void Reset(PadSet& set, const Layout& layout, Button b) {
  const int i = Index(b);
  set.custom[i] = false;
  set.binds[i] = {};
  for (Action action : layout.binds[i].act) {
    if (action == Action::kCount) continue;
    for (int j = 0; j < kButtonCount; ++j) {
      if (j == i || !set.custom[j] || !set.binds[j].Has(action)) continue;
      for (Action& a : set.binds[j].act)
        if (a == action) a = Action::kCount;
      Compact(set.binds[j]);
    }
  }
}

void ResetAll(PadSet& set) { set = {}; }

std::vector<Action> Choices() {
  std::vector<Action> out{Action::kCount};
  for (int a = 0; a < kActionCount; ++a)
    if (PadAction(static_cast<Action>(a))) out.push_back(static_cast<Action>(a));
  return out;
}

}  // namespace cod3::binds::pad
