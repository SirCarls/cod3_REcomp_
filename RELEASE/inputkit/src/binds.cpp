// inputkit - the bind model (see binds.h). The rules and notes are Call of Duty 3's model.cpp, word for word.

#include "inputkit/binds.h"

#include <cctype>
#include <memory>

namespace inputkit::binds {

namespace {

constexpr size_t kMaxText = 4096;

bool Valid(const Table& t, int action) { return action >= 0 && action < t.count; }

}  // namespace

std::string_view Trim(std::string_view s) {
  while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
  while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
  return s;
}

bool SameText(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  return true;
}

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

int ActionById(const Table& t, std::string_view id) {
  id = Trim(id);
  for (int i = 0; i < t.count; ++i)
    if (SameText(id, t.actions[i].id)) return i;
  return -1;
}

void Defaults(const Table& t, KeyRow* keys) {
  for (int a = 0; a < t.count; ++a)
    for (int s = 0; s < kSlots; ++s) keys[a][s] = t.actions[a].defaults[s];
}

bool Same(const Table& t, const KeyRow* a, const KeyRow* b) {
  for (int i = 0; i < t.count; ++i)
    for (int s = 0; s < kSlots; ++s)
      if (a[i][s] != b[i][s]) return false;
  return true;
}

bool IsDefault(const Table& t, const KeyRow* keys) {
  for (int a = 0; a < t.count; ++a)
    for (int s = 0; s < kSlots; ++s)
      if (keys[a][s] != t.actions[a].defaults[s]) return false;
  return true;
}

void Compact(KeyRow* keys, int a) {
  if (keys[a][0] == kNoKey && keys[a][1] != kNoKey) {
    keys[a][0] = keys[a][1];
    keys[a][1] = kNoKey;
  }
}

int Find(const Table& t, const KeyRow* keys, KeyCode key, int* slot, uint8_t kind) {
  if (key == kNoKey) return -1;
  for (int a = 0; a < t.count; ++a) {
    if (t.actions[a].kind != kind) continue;
    for (int s = 0; s < kSlots; ++s)
      if (keys[a][s] == key) {
        if (slot) *slot = s;
        return a;
      }
  }
  return -1;
}

int FindAll(const Table& t, const KeyRow* keys, KeyCode key, KeyUse* out, int max) {
  int n = 0;
  if (key == kNoKey) return 0;
  for (int a = 0; a < t.count && n < max; ++a)
    for (int s = 0; s < kSlots; ++s)
      if (keys[a][s] == key) {
        out[n++] = {a, s};
        break;
      }
  return n;
}

void Parse(const Table& t, std::string_view text, KeyRow* keys, std::vector<std::string>* notes) {
  auto note = [notes](std::string line) {
    if (notes) notes->push_back(std::move(line));
  };
  text = Trim(text);
  if (text.empty()) {
    Defaults(t, keys);
    return;
  }
  if (text.size() > kMaxText) {
    note(std::string(t.setting_name) + " is longer than 4 KB; the rest is ignored");
    text = text.substr(0, kMaxText);
  }
  for (int a = 0; a < t.count; ++a)
    for (int s = 0; s < kSlots; ++s) keys[a][s] = kNoKey;
  std::vector<bool> mentioned(size_t(t.count), false);
  std::vector<KeyUse> uses(size_t(t.count));
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
    const int a = ActionById(t, entry.substr(0, eq));
    if (a < 0) {
      note("'" + std::string(Trim(entry.substr(0, eq))) + "' isn't an action; ignored");
      continue;
    }
    if (mentioned[size_t(a)]) {
      note(std::string(t.actions[a].id) + " is listed twice; the first one counts");
      continue;
    }
    mentioned[size_t(a)] = true;
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
      const int used = FindAll(t, keys, key, uses.data(), t.count);
      bool here = false;
      for (int i = 0; i < used; ++i) here |= uses[size_t(i)].action == a;
      if (here) continue;  // listed twice for the same action
      if (used >= kMaxActionsPerKey) {
        note(KeyName(key) + " is listed for more than " + std::to_string(kMaxActionsPerKey) +
             " actions; the first " + std::to_string(kMaxActionsPerKey) + " keep it");
        continue;
      }
      if (slot >= kSlots) {
        note(std::string(t.actions[a].id) + ": two keys at most; " + KeyName(key) + " ignored");
        continue;
      }
      keys[a][slot++] = key;
    }
  }
  // Actions the text doesn't list (added later) keep their defaults, unless an action of their kind has the key.
  for (int a = 0; a < t.count; ++a) {
    if (mentioned[size_t(a)]) continue;
    int slot = 0;
    for (KeyCode key : t.actions[a].defaults)
      if (key != kNoKey && Find(t, keys, key, nullptr, t.actions[a].kind) < 0) keys[a][slot++] = key;
  }
}

std::string Serialize(const Table& t, const KeyRow* keys) {
  if (IsDefault(t, keys)) return "";
  std::string out;
  for (int a = 0; a < t.count; ++a) {
    if (a) out += ';';
    out += t.actions[a].id;
    out += '=';
    bool first = true;
    for (int s = 0; s < kSlots; ++s) {
      const KeyCode key = keys[a][s];
      if (key == kNoKey) continue;
      if (!first) out += ',';
      out += KeyName(key);
      first = false;
    }
  }
  return out;
}

std::string AssignMoving(const Table& t, KeyRow* keys, int a, int slot, KeyCode key) {
  if (!Valid(t, a) || slot < 0 || slot >= kSlots) return "";
  std::string note;
  if (key != kNoKey) {
    int had_slot = 0;
    const int had = Find(t, keys, key, &had_slot, t.actions[a].kind);
    if (had == a && had_slot == slot) return "";
    if (had >= 0) {
      keys[had][had_slot] = kNoKey;
      if (had != a) {
        Compact(keys, had);
        note = KeyName(key) + " was " + t.actions[had].label + "; moved here";
      }
    }
  }
  keys[a][slot] = key;
  Compact(keys, a);
  return note;
}

std::string Assign(const Table& t, KeyRow* keys, int a, int slot, KeyCode key, bool* ok) {
  if (ok) *ok = true;
  if (!Valid(t, a) || slot < 0 || slot >= kSlots) return "";
  if (key == kNoKey || keys[a][slot] == key) {
    keys[a][slot] = key;
    Compact(keys, a);
    return "";
  }
  std::vector<KeyUse> uses(size_t(t.count));
  const int used = FindAll(t, keys, key, uses.data(), t.count);
  std::string others;
  int count = 0;
  for (int i = 0; i < used; ++i) {
    if (uses[size_t(i)].action == a) continue;  // its other slot: moves here
    ++count;
    others += std::string(others.empty() ? "" : ", ") + t.actions[uses[size_t(i)].action].label;
  }
  if (count >= kMaxActionsPerKey) {
    if (ok) *ok = false;
    return KeyName(key) + " already does " + std::to_string(count) + " things (" + others + "); clear one first";
  }
  for (int s = 0; s < kSlots; ++s)
    if (s != slot && keys[a][s] == key) keys[a][s] = kNoKey;
  keys[a][slot] = key;
  Compact(keys, a);
  return others.empty() ? "" : KeyName(key) + " also does " + others;
}

std::string KeysText(const KeyRow* keys, int a) {
  std::string out;
  for (int s = 0; s < kSlots; ++s) {
    const KeyCode key = keys[a][s];
    if (key == kNoKey) continue;
    if (!out.empty()) out += ", ";
    out += KeyName(key);
  }
  return out;
}

bool AssignText(const Table& t, KeyRow* keys, int a, std::string_view text, std::string* note) {
  if (!Valid(t, a)) return false;
  std::vector<KeyCode> wanted;
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
    for (KeyCode k : wanted) dup |= k == key;
    if (!dup) wanted.push_back(key);
  }
  if (wanted.size() > static_cast<size_t>(kSlots)) {
    if (note) *note = "Two keys at most.";
    return false;
  }
  const std::unique_ptr<KeyRow[]> next(new KeyRow[size_t(t.count)]);
  KeyRow* rows = next.get();
  for (int i = 0; i < t.count; ++i)
    for (int s = 0; s < kSlots; ++s) rows[i][s] = keys[i][s];
  for (int s = 0; s < kSlots; ++s) rows[a][s] = kNoKey;
  std::string moved;
  for (size_t i = 0; i < wanted.size(); ++i) {
    bool fits = true;
    const std::string n = Assign(t, rows, a, static_cast<int>(i), wanted[i], &fits);
    if (!fits) {
      if (note) *note = n + ".";
      return false;
    }
    if (!n.empty()) moved += (moved.empty() ? "" : " ") + n + ".";
  }
  for (int i = 0; i < t.count; ++i)
    for (int s = 0; s < kSlots; ++s) keys[i][s] = rows[i][s];
  if (note) *note = moved;
  return true;
}

}  // namespace inputkit::binds
