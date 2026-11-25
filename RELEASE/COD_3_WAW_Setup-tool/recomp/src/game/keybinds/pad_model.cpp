// cod3 - [acct2-main] the controller's own binds per seat (see pad_model.h).

#include "game/keybinds/pad_model.h"

#include <array>

#include "inputkit/pad_binds.h"

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

// [input-port] 2026-11-24: the rules (parse, serialize, "Unbind the other",
// reset) are the shared inputkit library's (inputkit/src/pad_binds.cpp, the
// code that was here); CoD3 passes it these buttons and PadAction, and
// changes its Action-typed binds to the library's numbers and back (16
// buttons, only on a menu step or a settings read).
namespace ikp = inputkit::padbinds;

constexpr std::array<ikp::ButtonSpec, kButtonCount> MakeSpecs() {
  std::array<ikp::ButtonSpec, kButtonCount> out{};
  for (int i = 0; i < kButtonCount; ++i) {
    const bool start = i == static_cast<int>(Button::kStart);
    out[i] = {kButtons[i].id, kButtons[i].label, kButtons[i].game_key, !start,
              start ? "START stays the pause menu's" : nullptr};
  }
  return out;
}
constexpr std::array<ikp::ButtonSpec, kButtonCount> kSpecs = MakeSpecs();
static_assert(kPadSlots == ikp::kPadSlots && kButtonCount <= ikp::kMaxButtons);

bool AllowedAction(int a) { return PadAction(static_cast<Action>(a)); }

const ikp::Pad& ThePad() {
  static const ikp::Pad pad{kSpecs.data(), kButtonCount, &ActionTable(), AllowedAction};
  return pad;
}

int Index(Button b) {
  const int i = static_cast<int>(b);
  return i >= 0 && i < kButtonCount ? i : 0;
}

ikp::ButtonBinds ToIk(const ButtonBinds& bb) {
  ikp::ButtonBinds out;
  for (int s = 0; s < kPadSlots; ++s) out.act[s] = bb.act[s] == Action::kCount ? ikp::kNone : static_cast<int>(bb.act[s]);
  return out;
}

ButtonBinds FromIk(const ikp::ButtonBinds& bb) {
  ButtonBinds out;
  for (int s = 0; s < kPadSlots; ++s)
    out.act[s] = bb.act[s] == ikp::kNone ? Action::kCount : static_cast<Action>(bb.act[s]);
  return out;
}

ikp::PadSet ToIk(const PadSet& set) {
  ikp::PadSet out;
  for (int i = 0; i < kButtonCount; ++i) {
    out.custom[i] = set.custom[i];
    out.binds[i] = ToIk(set.binds[i]);
  }
  return out;
}

PadSet FromIk(const ikp::PadSet& set) {
  PadSet out;
  for (int i = 0; i < kButtonCount; ++i) {
    out.custom[i] = set.custom[i];
    out.binds[i] = FromIk(set.binds[i]);
  }
  return out;
}

ikp::Layout ToIk(const Layout& layout) {
  ikp::Layout out;
  for (int i = 0; i < kButtonCount; ++i) out.binds[i] = ToIk(layout.binds[i]);
  return out;
}

}  // namespace

const char* ButtonId(Button b) { return kButtons[Index(b)].id; }
const char* ButtonLabel(Button b) { return kButtons[Index(b)].label; }
int GameKey(Button b) { return kButtons[Index(b)].game_key; }
bool Changeable(Button b) { return b != Button::kStart && b != Button::kCount; }

Button ButtonById(std::string_view id) {
  const int i = ikp::ButtonById(ThePad(), id);
  return i < 0 ? Button::kCount : static_cast<Button>(i);
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
  return FromIk(ikp::Parse(ThePad(), text, notes));
}

std::string Serialize(const PadSet& set) { return ikp::Serialize(ThePad(), ToIk(set)); }

std::string Assign(PadSet& set, const Layout& layout, Button b, int slot, Action action) {
  ikp::PadSet next = ToIk(set);
  const std::string note = ikp::Assign(ThePad(), next, ToIk(layout), static_cast<int>(b), slot,
                                       action == Action::kCount ? ikp::kNone : static_cast<int>(action));
  set = FromIk(next);
  return note;
}

void Reset(PadSet& set, const Layout& layout, Button b) {
  if (b == Button::kCount) return;
  ikp::PadSet next = ToIk(set);
  ikp::Reset(ThePad(), next, ToIk(layout), Index(b));
  set = FromIk(next);
}

void ResetAll(PadSet& set) { set = {}; }

std::vector<Action> Choices() {
  std::vector<Action> out;
  for (int a : ikp::Choices(ThePad())) out.push_back(a == ikp::kNone ? Action::kCount : static_cast<Action>(a));
  return out;
}

}  // namespace cod3::binds::pad
