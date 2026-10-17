// cod3 - the keyboard and mouse actions and their keys (see model.h).

#include "game/keybinds/model.h"

#include <cctype>

namespace cod3::binds {

namespace {

constexpr ActionDef kDefs[kActionCount] = {
    {"binoculars", "Binoculars",
     "Hold to look through the binoculars; pressed again while they are up, it steadies them.",
     "+binoculars", {nullptr}, {kKeyB, kNoKey}},
    {"sprintbreath", "Sprint / hold breath",
     "World at War's Shift: holds your breath while aiming down the sights or through the binoculars, "
     "sprints otherwise (when Sprint is on, F2 > Game > Movement).",
     "+sprintbreath", {"+holdbreath", "+sprint", nullptr}, {kKeyShift, kNoKey}},
    {"holdbreath", "Hold breath",
     "Holds your breath (steadies a scope or the binoculars) and nothing else.",
     "+holdbreath", {"+sprintbreath", nullptr}, {kNoKey, kNoKey}},
    {"sprint", "Sprint",
     "Sprints (when Sprint is on, F2 > Game > Movement) and nothing else.",
     "+sprint", {"+sprintbreath", nullptr}, {kNoKey, kNoKey}},
};

constexpr size_t kMaxText = 4096;

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

// Splits at `sep`, trimmed pieces, empty ones skipped.
std::vector<std::string_view> Split(std::string_view s, char sep) {
  std::vector<std::string_view> out;
  while (!s.empty()) {
    const size_t at = s.find(sep);
    const std::string_view piece = Trim(s.substr(0, at));
    if (!piece.empty()) out.push_back(piece);
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

// Slot 1 moves to slot 0 when slot 0 is empty.
void Compact(KbmSet& set, int a) {
  if (set.key[a][0] == kNoKey && set.key[a][1] != kNoKey) {
    set.key[a][0] = set.key[a][1];
    set.key[a][1] = kNoKey;
  }
}

int Index(Action a) { return static_cast<int>(a); }

}  // namespace

const ActionDef& Def(Action action) {
  const int i = Index(action);
  return kDefs[i >= 0 && i < kActionCount ? i : 0];
}

Action ActionById(std::string_view id) {
  id = Trim(id);
  for (int i = 0; i < kActionCount; ++i)
    if (SameText(id, kDefs[i].id)) return static_cast<Action>(i);
  return Action::kCount;
}

Action ActionByCommand(std::string_view command) {
  command = Trim(command);
  for (int i = 0; i < kActionCount; ++i)
    if (SameText(command, kDefs[i].command)) return static_cast<Action>(i);
  return Action::kCount;
}

KbmSet Defaults() {
  KbmSet set;
  for (int a = 0; a < kActionCount; ++a)
    for (int s = 0; s < kSlots; ++s) set.key[a][s] = kDefs[a].defaults[s];
  return set;
}

Action Find(const KbmSet& set, KeyCode key, int* slot) {
  if (key == kNoKey) return Action::kCount;
  for (int a = 0; a < kActionCount; ++a)
    for (int s = 0; s < kSlots; ++s)
      if (set.key[a][s] == key) {
        if (slot) *slot = s;
        return static_cast<Action>(a);
      }
  return Action::kCount;
}

KbmSet Parse(std::string_view text, std::vector<std::string>* notes) {
  auto note = [notes](std::string line) {
    if (notes) notes->push_back(std::move(line));
  };
  text = Trim(text);
  if (text.empty()) return Defaults();
  if (text.size() > kMaxText) {
    note("cod3_binds_kbm_p1 is longer than 4 KB; the rest is ignored");
    text = text.substr(0, kMaxText);
  }
  KbmSet set;
  bool mentioned[kActionCount] = {};
  for (std::string_view entry : Split(text, ';')) {
    if (HasControlChars(entry)) {
      note("a key bind entry with control characters was ignored");
      continue;
    }
    const size_t eq = entry.find('=');
    if (eq == std::string_view::npos) {
      note("'" + std::string(entry) + "' isn't action=keys; ignored");
      continue;
    }
    const Action action = ActionById(entry.substr(0, eq));
    if (action == Action::kCount) {
      note("'" + std::string(Trim(entry.substr(0, eq))) + "' isn't an action; ignored");
      continue;
    }
    const int a = Index(action);
    if (mentioned[a]) {
      note(std::string(kDefs[a].id) + " is listed twice; the first one counts");
      continue;
    }
    mentioned[a] = true;
    int slot = 0;
    for (std::string_view name : Split(entry.substr(eq + 1), ',')) {
      if (IsCombination(name)) {
        note("'" + std::string(name) + "' (a key combination) isn't used; bind the key on its own");
        continue;
      }
      const KeyCode key = ParseKey(name);
      if (key == kNoKey) {
        note("'" + std::string(name) + "' isn't a key name; ignored");
        continue;
      }
      if (IsAlwaysReserved(key)) {
        note(KeyName(key) + " can't be bound (the console or the pause menu); ignored");
        continue;
      }
      if (Find(set, key) != Action::kCount) {
        note(KeyName(key) + " is listed for two actions; the first one keeps it");
        continue;
      }
      if (slot >= kSlots) {
        note(std::string(kDefs[a].id) + ": two keys at most; " + KeyName(key) + " ignored");
        continue;
      }
      set.key[a][slot++] = key;
    }
  }
  // Actions the text doesn't list (added later) keep their defaults, unless
  // the key went to another action.
  for (int a = 0; a < kActionCount; ++a) {
    if (mentioned[a]) continue;
    int slot = 0;
    for (KeyCode key : kDefs[a].defaults)
      if (key != kNoKey && Find(set, key) == Action::kCount) set.key[a][slot++] = key;
  }
  return set;
}

std::string Serialize(const KbmSet& set) {
  if (set == Defaults()) return "";
  std::string out;
  for (int a = 0; a < kActionCount; ++a) {
    if (a) out += ';';
    out += kDefs[a].id;
    out += '=';
    bool first = true;
    for (KeyCode key : set.key[a]) {
      if (key == kNoKey) continue;
      if (!first) out += ',';
      out += KeyName(key);
      first = false;
    }
  }
  return out;
}

std::string Assign(KbmSet& set, Action action, int slot, KeyCode key) {
  const int a = Index(action);
  if (a < 0 || a >= kActionCount || slot < 0 || slot >= kSlots) return "";
  std::string note;
  if (key != kNoKey) {
    int had_slot = 0;
    const Action had = Find(set, key, &had_slot);
    if (had == action && had_slot == slot) return "";
    if (had != Action::kCount) {
      set.key[Index(had)][had_slot] = kNoKey;
      if (had != action) {
        Compact(set, Index(had));
        note = KeyName(key) + " was " + kDefs[Index(had)].label + "; moved here";
      }
    }
  }
  set.key[a][slot] = key;
  Compact(set, a);
  return note;
}

std::string KeysText(const KbmSet& set, Action action) {
  const int a = Index(action);
  std::string out;
  for (KeyCode key : set.key[a]) {
    if (key == kNoKey) continue;
    if (!out.empty()) out += ", ";
    out += KeyName(key);
  }
  return out;
}

bool AssignText(KbmSet& set, Action action, std::string_view text, std::string* note) {
  std::vector<KeyCode> keys;
  for (std::string_view name : Split(text, ',')) {
    if (IsCombination(name)) {
      if (note) *note = "Key combinations (" + std::string(name) + ") aren't used: bind the key on its own.";
      return false;
    }
    const KeyCode key = ParseKey(name);
    if (key == kNoKey) {
      if (note) *note = "'" + std::string(name) + "' isn't a key name (B, SHIFT, MOUSE4, MWHEELUP ...).";
      return false;
    }
    if (IsAlwaysReserved(key)) {
      if (note) *note = KeyName(key) + " can't be bound (the console or the pause menu).";
      return false;
    }
    bool dup = false;
    for (KeyCode k : keys) dup |= k == key;
    if (!dup) keys.push_back(key);
  }
  if (keys.size() > static_cast<size_t>(kSlots)) {
    if (note) *note = "Two keys at most.";
    return false;
  }
  KbmSet next = set;
  const int a = Index(action);
  for (int s = 0; s < kSlots; ++s) next.key[a][s] = kNoKey;
  std::string moved;
  for (size_t i = 0; i < keys.size(); ++i) {
    const std::string n = Assign(next, action, static_cast<int>(i), keys[i]);
    if (!n.empty()) moved += (moved.empty() ? "" : " ") + n + ".";
  }
  set = next;
  if (note) *note = moved;
  return true;
}

// ---------------------------------------------------------------------------
// The one-time move from keybind_*
// ---------------------------------------------------------------------------
namespace {

struct OldDefault {
  const char* name;
  const char* values[2];  // the values that were defaults (the port's, the SDK's)
};
constexpr OldDefault kOldDefaults[] = {
    {"keybind_lstick_press", {"Shift", "F"}},  // the port's default, the SDK's
    {"keybind_dpad_up", {"Shift+Up", nullptr}},
    {"keybind_dpad_down", {"Shift+Down", nullptr}},
    {"keybind_dpad_left", {"Shift+Left", nullptr}},
    {"keybind_dpad_right", {"Shift+Right", nullptr}},
};

bool IsOldDefault(std::string_view name, std::string_view value) {
  value = Trim(value);
  if (value.empty()) return true;
  for (const OldDefault& d : kOldDefaults) {
    if (name != d.name) continue;
    for (const char* v : d.values)
      if (v && SameText(value, v)) return true;
  }
  return false;
}

}  // namespace

Migration Migrate(const KbmSet& current, const std::vector<OldBind>& binds, const std::vector<OldBind>& others) {
  Migration m;
  m.set = current;
  // Keys still pressing other controller buttons through keybind_*.
  auto other_owner = [&others](KeyCode key) -> std::string {
    for (const OldBind& o : others)
      for (std::string_view name : Split(o.value, ','))
        if (!IsCombination(name) && ParseKey(name) == key) return o.name;
    return "";
  };
  for (const OldBind& b : binds) {
    if (!b.from_file) continue;
    const std::string_view value = Trim(b.value);
    const bool lstick = b.name == "keybind_lstick_press";
    const bool dpad_up = b.name == "keybind_dpad_up";
    if (IsOldDefault(b.name, value)) {
      m.new_values.emplace_back(b.name, "");
      if (!value.empty()) {
        std::string why = lstick ? "the binoculars are their own key now (" + KeysText(m.set, Action::kBinoculars) +
                                       "), Shift is sprint / hold breath"
                                 : "Shift+arrows press nothing now";
        m.log.push_back(b.name + " \"" + std::string(value) + "\" (the old default) removed: " + why);
      }
      continue;
    }
    if (lstick || dpad_up) {
      // The player's own keys for that button go to the action the campaign's
      // layouts gave it: L3 = binoculars, D-pad up = hold breath.
      const Action action = lstick ? Action::kBinoculars : Action::kHoldBreath;
      std::vector<KeyCode> keys;
      for (std::string_view name : Split(value, ',')) {
        const KeyCode key = ParseKey(name);
        if (IsCombination(name)) {
          m.log.push_back(b.name + ": " + std::string(name) + " dropped (key combinations aren't used)");
        } else if (key == kNoKey || IsAlwaysReserved(key)) {
          m.log.push_back(b.name + ": '" + std::string(name) + "' dropped (not a key that can be bound)");
        } else if (key == kKeyShift) {
          m.log.push_back(b.name + ": SHIFT stays " + Def(Action::kSprintBreath).label);
        } else if (const std::string owner = other_owner(key); !owner.empty()) {
          m.log.push_back(b.name + ": " + KeyName(key) + " stays with " + owner);
        } else if (keys.size() < static_cast<size_t>(kSlots)) {
          bool dup = false;
          for (KeyCode k : keys) dup |= k == key;
          if (!dup) keys.push_back(key);
        }
      }
      if (!keys.empty()) {
        KbmSet next = m.set;
        for (int s = 0; s < kSlots; ++s) next.key[Index(action)][s] = kNoKey;
        for (size_t i = 0; i < keys.size(); ++i) {
          const std::string n = Assign(next, action, static_cast<int>(i), keys[i]);
          if (!n.empty()) m.log.push_back(b.name + ": " + n);
        }
        m.set = next;
        m.log.push_back(b.name + " \"" + std::string(value) + "\" -> " + Def(action).id + " " +
                        KeysText(m.set, action));
      } else {
        m.log.push_back(b.name + " \"" + std::string(value) + "\" removed (no key left to move)");
      }
      m.new_values.emplace_back(b.name, "");
      continue;
    }
    // The other D-pad directions: the campaign's layouts give them nothing in
    // play, so a player's own keys keep pressing them; only the Shift+arrow
    // combinations go.
    std::string kept;
    bool dropped = false;
    for (std::string_view name : Split(value, ',')) {
      if (IsCombination(name)) {
        dropped = true;
        continue;
      }
      if (!kept.empty()) kept += ',';
      kept += name;
    }
    if (!dropped) continue;
    m.new_values.emplace_back(b.name, kept);
    m.log.push_back(b.name + " \"" + std::string(value) + "\" -> \"" + kept + "\" (key combinations removed)");
  }
  m.set_changed = !(m.set == current);
  return m;
}

}  // namespace cod3::binds
