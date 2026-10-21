// The SDK's own spelling of key names, for the keybind_* settings (F2 >
// Controls > Key bindings, the controller rows).
//
// The SDK reads those rows' names exactly as written (Space, Up, Shift+Up,
// Numpad0; rex/ui/keybinds.h ParseVirtualKey), while F2 shows and takes them
// in capitals (owner 2026-10-20 20:24: "make sure the keybind touppers for the
// user"). Spelled() turns what was typed back into the SDK's spelling; World
// at War's names (the action rows' ENTER, UPARROW, MOUSE1, KP_0 ...) become
// the SDK's name for the same key. Inline (with keys.cpp for World at War's
// names), so dev/agents/keybinds/tests/test_sdk_spelling.cpp checks it
// against the SDK itself.
#pragma once

#include <cctype>
#include <string>
#include <string_view>

#include <rex/ui/keybinds.h>
#include <rex/ui/virtual_key.h>

#include "game/keybinds/keys.h"

namespace cod3::binds::sdk {

inline bool SameName(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(a[i])) != std::tolower(static_cast<unsigned char>(b[i])))
      return false;
  return true;
}

// One key name: the SDK's spelling of it, or the name as typed when the SDK
// has no name for that key.
inline std::string SpelledKey(std::string_view name) {
  if (name.empty() || rex::ui::ParseVirtualKey(name) != rex::ui::VirtualKey::kNone) return std::string(name);
  for (const char* known : {"Return", "NumpadEnter"})  // two names for one key: the one typed
    if (SameName(name, known)) return known;
  for (int vk = 1; vk < 256; ++vk) {
    const std::string known = rex::ui::VirtualKeyToString(static_cast<rex::ui::VirtualKey>(vk));
    if (!known.empty() && SameName(name, known)) return known;
  }
  // World at War's names (keys.h): the SDK's name for the same key.
  const KeyCode code = ParseKey(name);
  switch (code) {
    case kKeyEnter: return "Return";  // ENTER, KP_ENTER (the SDK has two names for it)
    case kMouse1: return "LMB";
    case kMouse2: return "RMB";
    case kMouse3: return "MMB";
    case kMouse4: return "MB4";
    case kMouse5: return "MB5";
    default: break;
  }
  if (code != kNoKey && code < 256) {
    const std::string known = rex::ui::VirtualKeyToString(static_cast<rex::ui::VirtualKey>(code));
    if (!known.empty()) return known;
  }
  return std::string(name);  // MWHEELUP ...: no key the SDK reads
}

// A modifier in front of a '+' (Shift+Up): the SDK's spellings Shift, Ctrl,
// Control, Alt; anything else as a key name.
inline std::string SpelledModifier(std::string_view name) {
  for (const char* known : {"Shift", "Ctrl", "Control", "Alt"})
    if (SameName(name, known)) return known;
  return SpelledKey(name);
}

// "SHIFT+UP, MB4" -> "Shift+Up, MB4": each name between commas and plus signs;
// the spaces and separators stay as typed. Spelled(Spelled(x)) == Spelled(x).
inline std::string Spelled(std::string_view typed) {
  std::string out;
  size_t start = 0;
  while (start <= typed.size()) {
    size_t end = typed.find_first_of(",+", start);
    if (end == std::string_view::npos) end = typed.size();
    const std::string_view part = typed.substr(start, end - start);
    const size_t lead = part.find_first_not_of(" \t");
    if (lead == std::string_view::npos) {
      out += part;
    } else {
      const size_t trail = part.find_last_not_of(" \t");
      const std::string_view name = part.substr(lead, trail - lead + 1);
      out += part.substr(0, lead);
      out += (end < typed.size() && typed[end] == '+') ? SpelledModifier(name) : SpelledKey(name);
      out += part.substr(trail + 1);
    }
    if (end < typed.size()) out += typed[end];
    start = end + 1;
  }
  return out;
}

}  // namespace cod3::binds::sdk
