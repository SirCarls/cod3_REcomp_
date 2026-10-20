// cod3 - the keyboard and mouse actions and their keys (see model.h).

#include "game/keybinds/model.h"

#include <cctype>

namespace cod3::binds {

namespace {

constexpr KeyCode K(char c) { return static_cast<KeyCode>(c); }
constexpr KeyCode kKeySpace = 0x20;

using D = Delivery;
using G = Group;

// In Action's order (model.h). The game's commands are the same in both
// programs (default.xex and codmp_xenonf.xex register the same names).
constexpr ActionDef kDefs[] = {
    // --- Combat ----------------------------------------------------------------
    {"fire", "Fire", "Fires the weapon in your hands while held.", G::kCombat, D::kButton, "+attack", {nullptr},
     {kMouse1, kNoKey}},
    {"aim", "Aim down the sights",
     "Aims down the sights while held, or on and off with each press when Aim toggle (below) is on.",
     G::kCombat, D::kAim, "+speed", {"toggle cl_run", nullptr}, {kMouse2, kNoKey}},
    {"use", "Use / interact",
     "Picks up a weapon, opens a door, mounts a gun, plants a charge (tap or hold, as the hint says); "
     "never reloads.",
     G::kCombat, D::kButton, "+activate", {"+activatereload", "+activatemelee", "+activatemoveup", nullptr},
     {K('F'), kNoKey}},
    {"reload", "Reload", "Reloads; never uses anything.", G::kCombat, D::kButton, "+reload",
     {"+activatereload", nullptr}, {K('R'), kNoKey}},
    {"usereload", "Use or reload (the controller's X)",
     "Tap to reload, hold to use: the controller's X button on one key.", G::kCombat, D::kButton,
     "+activatereload", {nullptr}, {kNoKey, kNoKey}},
    {"melee", "Melee", "Hits with your weapon.", G::kCombat, D::kButton, "+melee", {"+activatemelee", nullptr},
     {K('V'), kMouse3}},
    {"usemelee", "Use or melee", "Uses what is in front of you, hits otherwise (a controller layout's mix).",
     G::kCombat, D::kButton, "+activatemelee", {nullptr}, {kNoKey, kNoKey}},
    {"frag", "Frag grenade",
     "Hold to cook, let go to throw; also throws a grenade back when the hint says so.", G::kCombat,
     D::kButton, "+grenadeattack", {nullptr}, {K('G'), kNoKey}},
    {"smoke", "Smoke grenade",
     "Hold and let go to throw a smoke grenade (in multiplayer: your class's special grenade).", G::kCombat,
     D::kButton, "+smokegrenade", {nullptr}, {K('4'), kNoKey}},
    {"binoculars", "Binoculars",
     "Hold to look through the binoculars; pressed again while they are up, it steadies them.", G::kCombat,
     D::kButton, "+binoculars", {nullptr}, {kKeyB, kNoKey}},
    {"sprintbreath", "Sprint / hold breath",
     "World at War's Shift: holds your breath while aiming down the sights or through the binoculars, "
     "sprints otherwise (when Sprint is on, F2 > Game > Movement).",
     G::kCombat, D::kButton, "+sprintbreath", {"+holdbreath", "+sprint", nullptr}, {kKeyShift, kNoKey}},
    {"holdbreath", "Hold breath", "Holds your breath (steadies a scope or the binoculars) and nothing else.",
     G::kCombat, D::kButton, "+holdbreath", {"+sprintbreath", nullptr}, {kNoKey, kNoKey}},
    {"sprint", "Sprint", "Sprints (when Sprint is on, F2 > Game > Movement) and nothing else.", G::kCombat,
     D::kButton, "+sprint", {"+sprintbreath", nullptr}, {kNoKey, kNoKey}},
    {"leanleft", "Lean left", "Leans left while held.", G::kCombat, D::kButton, "+leanleft",
     {"+leanleftswitchnext", nullptr}, {K('Q'), kNoKey}},
    {"leanright", "Lean right", "Leans right while held.", G::kCombat, D::kButton, "+leanright",
     {"+leanrightswitchnext", nullptr}, {K('E'), kNoKey}},
    {"leanleftnext", "Lean left / switch weapon",
     "Leans left while aiming down the sights, switches weapons otherwise (a controller layout's mix).",
     G::kCombat, D::kButton, "+leanleftswitchnext", {nullptr}, {kNoKey, kNoKey}},
    {"leanrightnext", "Lean right / switch weapon",
     "Leans right while aiming down the sights, switches weapons otherwise (a controller layout's mix).",
     G::kCombat, D::kButton, "+leanrightswitchnext", {nullptr}, {kNoKey, kNoKey}},
    // --- Stance ----------------------------------------------------------------
    {"jump", "Jump / stand up", "Jumps; from crouch or prone, stands up.", G::kStance, D::kButton, "+gostand",
     {"+moveup", "+activatemoveup", nullptr}, {kKeySpace, kNoKey}},
    {"moveup", "Raise stance / jump", "Prone to crouch, crouch to standing, and jumps when standing.",
     G::kStance, D::kButton, "+moveup", {"+gostand", nullptr}, {kNoKey, kNoKey}},
    {"usemoveup", "Use or raise stance", "Uses what is in front of you, raises your stance otherwise.",
     G::kStance, D::kButton, "+activatemoveup", {nullptr}, {kNoKey, kNoKey}},
    {"stance", "Change stance (the controller's B)", "Tap: crouch, or stand up again; hold: go prone.",
     G::kStance, D::kPlain, "+stance", {nullptr}, {kNoKey, kNoKey}},
    {"crouch", "Crouch", "Crouches; pressed again, stands up.", G::kStance, D::kOnce, "togglecrouch",
     {"+stance", "lowerstance", "+movedown", nullptr}, {K('C'), kNoKey}},
    {"crouchhold", "Crouch while held", "Crouches while held, stands up when let go.", G::kStance, D::kButton,
     "+movedown", {"+stance", nullptr}, {kNoKey, kNoKey}},
    {"prone", "Prone", "Goes prone; pressed again, stands up.", G::kStance, D::kOnce, "toggleprone",
     {"+stance", "goprone", "+prone", nullptr}, {kKeyCtrl, kNoKey}},
    {"pronehold", "Prone while held", "Goes prone while held, gets up when let go.", G::kStance, D::kButton,
     "+prone", {"+stance", nullptr}, {kNoKey, kNoKey}},
    {"lowerstance", "Lower stance", "Standing to crouch, crouch to prone.", G::kStance, D::kOnce,
     "lowerstance", {"+stance", nullptr}, {kNoKey, kNoKey}},
    {"raisestance", "Raise stance", "Prone to crouch, crouch to standing.", G::kStance, D::kOnce, "raisestance",
     {"+gostand", "+moveup", nullptr}, {kNoKey, kNoKey}},
    // --- Weapons ---------------------------------------------------------------
    {"weapnext", "Next weapon", "Switches to your next weapon.", G::kWeapons, D::kText, "weapnext", {nullptr},
     {kWheelDown, kNoKey}},
    {"weapprev", "Previous weapon", "Switches to your previous weapon.", G::kWeapons, D::kText, "weapprev",
     {"weapnext", nullptr}, {kWheelUp, kNoKey}},
    {"weapalt", "Weapon's other mode", "Your weapon's other mode, where it has one.", G::kWeapons, D::kText,
     "weapalt", {nullptr}, {kNoKey, kNoKey}},
    {"slot1", "Weapon 1", "Your first weapon.", G::kWeapons, D::kText, "weaponslot 1", {"weapnext", nullptr},
     {K('1'), kNoKey}},
    {"slot2", "Weapon 2", "Your second weapon.", G::kWeapons, D::kText, "weaponslot 2", {"weapnext", nullptr},
     {K('2'), kNoKey}},
    // --- General ---------------------------------------------------------------
    {"scores", "Objectives / scores",
     "Shows the objectives (campaign) or the scores (multiplayer) while held. TAB does the same through "
     "the controller's BACK row.",
     G::kGeneral, D::kText, "+scores", {nullptr}, {kNoKey, kNoKey}},
};
static_assert(sizeof(kDefs) / sizeof(kDefs[0]) == static_cast<size_t>(kActionCount));

constexpr const char* kGroupNames[] = {"Combat", "Stance", "Weapons", "General"};
static_assert(sizeof(kGroupNames) / sizeof(kGroupNames[0]) == static_cast<size_t>(Group::kCount));

// The controller rows (keybind_*) and the action their button is in the
// campaign's layout A (xenon_a.cfg).
struct RowAction {
  const char* row;
  Action action;
};
constexpr RowAction kRowActions[] = {
    {"keybind_right_trigger", Action::kFire},   {"keybind_left_trigger", Action::kAim},
    {"keybind_a", Action::kJump},               {"keybind_b", Action::kStance},
    {"keybind_x", Action::kUseReload},          {"keybind_y", Action::kWeapNext},
    {"keybind_right_shoulder", Action::kFrag},  {"keybind_left_shoulder", Action::kSmoke},
    {"keybind_rstick_press", Action::kMelee},   {"keybind_lstick_press", Action::kBinoculars},
    {"keybind_dpad_up", Action::kHoldBreath},
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

Action ActionForRow(std::string_view row) {
  for (const RowAction& r : kRowActions)
    if (row == r.row) return r.action;
  return Action::kCount;
}

}  // namespace

const char* GroupName(Group group) {
  const int i = static_cast<int>(group);
  return i >= 0 && i < static_cast<int>(Group::kCount) ? kGroupNames[i] : "";
}

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

const char* RowForAction(Action action) {
  for (const RowAction& r : kRowActions)
    if (r.action == action) return r.row;
  return nullptr;
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
// The one-time moves from keybind_*
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

Migration MigrateRows(const KbmSet& current, const std::vector<OldBind>& rows, bool to_actions) {
  Migration m;
  m.set = current;
  // A row's keys as one text to compare: "r, F" and "R,F" are the same row.
  auto normal = [](std::string_view value) {
    std::string out;
    for (std::string_view name : Split(value, ',')) {
      if (!out.empty()) out += ',';
      for (char c : name) out += static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    }
    return out;
  };
  // A row the player changed: in cod3.toml and not its default.
  auto changed = [&normal](const OldBind& r) { return r.from_file && normal(r.value) != normal(r.default_value); };
  // The row's keys that an action can have (in order, no repeats).
  auto keys_of = [&m](const OldBind& r) {
    std::vector<KeyCode> keys;
    for (std::string_view name : Split(r.value, ',')) {
      const KeyCode key = ParseKey(name);
      if (IsCombination(name)) {
        m.log.push_back(r.name + ": " + std::string(name) + " left out (key combinations aren't used by actions)");
      } else if (key == kNoKey || IsAlwaysReserved(key)) {
        if (key == kNoKey) m.log.push_back(r.name + ": '" + std::string(name) + "' isn't a key the actions know");
      } else {
        bool dup = false;
        for (KeyCode k : keys) dup |= k == key;
        if (!dup) keys.push_back(key);
      }
    }
    return keys;
  };
  // 1. Rows that are one action: that action gets the player's keys.
  for (const OldBind& r : rows) {
    if (!to_actions || !changed(r)) continue;
    const Action action = ActionForRow(r.name);
    if (action == Action::kCount) continue;
    std::vector<KeyCode> keys = keys_of(r);
    if (keys.empty()) {
      m.log.push_back(r.name + " \"" + std::string(Trim(r.value)) + "\": no key to give " + Def(action).id +
                      ", which keeps " + (KeysText(m.set, action).empty() ? "none" : KeysText(m.set, action)));
      continue;
    }
    if (keys.size() > static_cast<size_t>(kSlots)) {
      std::string extra;
      for (size_t i = kSlots; i < keys.size(); ++i) extra += (extra.empty() ? "" : ", ") + KeyName(keys[i]);
      m.log.push_back(r.name + ": two keys at most for " + Def(action).id + "; " + extra +
                      " left out (in play they keep the action they have by default, if any)");
      keys.resize(kSlots);
    }
    KbmSet next = m.set;
    for (int s = 0; s < kSlots; ++s) next.key[Index(action)][s] = kNoKey;
    for (size_t i = 0; i < keys.size(); ++i) {
      const std::string n = Assign(next, action, static_cast<int>(i), keys[i]);
      if (!n.empty()) m.log.push_back(r.name + ": " + n);
    }
    m.set = next;
    m.log.push_back(r.name + " \"" + std::string(Trim(r.value)) + "\" (yours) -> " + Def(action).id + " " +
                    KeysText(m.set, action));
  }
  // 2. Every other row the player changed keeps its keys in play too (every
  // changed row when `to_actions` is false).
  for (const OldBind& r : rows) {
    if (!changed(r) || (to_actions && ActionForRow(r.name) != Action::kCount)) continue;
    for (KeyCode key : keys_of(r)) {
      int slot = 0;
      const Action had = Find(m.set, key, &slot);
      if (had == Action::kCount) continue;
      m.set.key[Index(had)][slot] = kNoKey;
      Compact(m.set, Index(had));
      const std::string left = KeysText(m.set, had);
      m.log.push_back(KeyName(key) + " stays with " + r.name + " (yours); " + Def(had).label +
                      (left.empty() ? " has no key now" : " keeps " + left));
    }
  }
  m.set_changed = !(m.set == current);
  return m;
}

}  // namespace cod3::binds
