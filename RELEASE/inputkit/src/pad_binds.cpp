// inputkit - a controller's own binds per seat (see pad_binds.h). The rules are Call of Duty 3's pad_model.cpp.

#include "inputkit/pad_binds.h"

namespace inputkit::padbinds {

namespace {

constexpr size_t kMaxText = 2048;

bool ValidButton(const Pad& pad, int b) { return b >= 0 && b < pad.button_count && b < kMaxButtons; }

}  // namespace

int ButtonBinds::Count() const {
  int n = 0;
  for (int a : act) n += a != kNone;
  return n;
}

bool ButtonBinds::Has(int action) const {
  if (action == kNone) return false;
  for (int x : act)
    if (x == action) return true;
  return false;
}

void Compact(ButtonBinds& bb) {
  ButtonBinds out;
  int n = 0;
  for (int a : bb.act)
    if (a != kNone) out.act[n++] = a;
  bb = out;
}

int ButtonById(const Pad& pad, std::string_view id) {
  id = binds::Trim(id);
  for (int i = 0; i < pad.button_count && i < kMaxButtons; ++i)
    if (binds::SameText(id, pad.buttons[i].id)) return i;
  return -1;
}

bool Allowed(const Pad& pad, int action) {
  if (action < 0 || action >= pad.actions->count) return false;
  return pad.allowed ? pad.allowed(action) : true;
}

ButtonBinds Effective(const PadSet& set, const Layout& layout, int b) {
  if (b < 0 || b >= kMaxButtons) return {};
  return set.custom[b] ? set.binds[b] : layout.binds[b];
}

PadSet Parse(const Pad& pad, std::string_view text, std::vector<std::string>* notes) {
  auto note = [notes](std::string line) {
    if (notes) notes->push_back(std::move(line));
  };
  PadSet set;
  text = binds::Trim(text);
  if (text.empty()) return set;
  if (text.size() > kMaxText) {
    note("the controller binds are longer than 2 KB; the rest is ignored");
    text = text.substr(0, kMaxText);
  }
  std::vector<bool> taken(size_t(pad.actions->count), false);  // an action is on one button: the first keeps it
  for (std::string_view entry : binds::Split(text, ';')) {
    if (binds::HasControlChars(entry)) {
      note("a controller bind entry with control characters was ignored");
      continue;
    }
    const size_t eq = entry.find('=');
    if (eq == std::string_view::npos) {
      note("'" + std::string(entry) + "' isn't button=actions; ignored");
      continue;
    }
    const int b = ButtonById(pad, entry.substr(0, eq));
    if (b < 0) {
      note("'" + std::string(binds::Trim(entry.substr(0, eq))) + "' isn't a controller button; ignored");
      continue;
    }
    if (!pad.buttons[b].changeable) {
      note((pad.buttons[b].locked_note ? std::string(pad.buttons[b].locked_note)
                                       : std::string(pad.buttons[b].label) + " can't be changed") +
           "; its entry is ignored");
      continue;
    }
    if (set.custom[b]) {
      note(std::string(pad.buttons[b].id) + " is listed twice; the first one counts");
      continue;
    }
    set.custom[b] = true;
    int slot = 0;
    for (std::string_view id : binds::Split(entry.substr(eq + 1), ',')) {
      const int a = binds::ActionById(*pad.actions, id);
      if (!Allowed(pad, a)) {
        note("'" + std::string(id) + "' isn't an action a controller button can do; ignored");
        continue;
      }
      if (taken[size_t(a)]) {
        note(std::string(pad.actions->actions[a].id) + " is on two buttons; the first one keeps it");
        continue;
      }
      if (slot >= kPadSlots) {
        note(std::string(pad.buttons[b].id) + ": three actions at most; " + pad.actions->actions[a].id + " ignored");
        continue;
      }
      taken[size_t(a)] = true;
      set.binds[b].act[slot++] = a;
    }
  }
  return set;
}

std::string Serialize(const Pad& pad, const PadSet& set) {
  std::string out;
  for (int i = 0; i < pad.button_count && i < kMaxButtons; ++i) {
    if (!set.custom[i]) continue;
    if (!out.empty()) out += ';';
    out += pad.buttons[i].id;
    out += '=';
    bool first = true;
    for (int a : set.binds[i].act) {
      if (a == kNone) continue;
      if (!first) out += ',';
      out += pad.actions->actions[a].id;
      first = false;
    }
  }
  return out;
}

std::string Assign(const Pad& pad, PadSet& set, const Layout& layout, int b, int slot, int action) {
  if (!ValidButton(pad, b) || !pad.buttons[b].changeable || slot < 0 || slot >= kPadSlots) return "";
  if (action != kNone && !Allowed(pad, action)) return "";
  ButtonBinds mine = Effective(set, layout, b);
  if (mine.act[slot] == action) return "";
  std::string note;
  if (action != kNone) {
    // Another button that has it loses it (owner: "Unbind the other").
    for (int j = 0; j < pad.button_count && j < kMaxButtons; ++j) {
      if (j == b) continue;
      ButtonBinds theirs = Effective(set, layout, j);
      if (!theirs.Has(action)) continue;
      for (int& a : theirs.act)
        if (a == action) a = kNone;
      Compact(theirs);
      set.custom[j] = true;
      set.binds[j] = theirs;
      const std::string other = pad.buttons[j].label;
      note = std::string(pad.actions->actions[action].label) + " was on " + other + "; " + other +
             (theirs.Count() ? " keeps the rest" : " does nothing now");
    }
    // The same action in another slot of this button moves here.
    for (int s = 0; s < kPadSlots; ++s)
      if (s != slot && mine.act[s] == action) mine.act[s] = kNone;
  }
  mine.act[slot] = action;
  Compact(mine);
  set.custom[b] = true;
  set.binds[b] = mine;
  return note;
}

void Reset(const Pad& pad, PadSet& set, const Layout& layout, int b) {
  if (!ValidButton(pad, b)) return;
  set.custom[b] = false;
  set.binds[b] = {};
  for (int action : layout.binds[b].act) {
    if (action == kNone) continue;
    for (int j = 0; j < pad.button_count && j < kMaxButtons; ++j) {
      if (j == b || !set.custom[j] || !set.binds[j].Has(action)) continue;
      for (int& a : set.binds[j].act)
        if (a == action) a = kNone;
      Compact(set.binds[j]);
    }
  }
}

std::vector<int> Choices(const Pad& pad) {
  std::vector<int> out{kNone};
  for (int a = 0; a < pad.actions->count; ++a)
    if (Allowed(pad, a)) out.push_back(a);
  return out;
}

}  // namespace inputkit::padbinds
