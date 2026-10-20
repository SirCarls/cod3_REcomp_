// cod3mp: the plain rules behind game/mp/controls_mp.cpp (no game memory, no
// SDK), so they can be tested on their own.

#pragma once

#include <cctype>
#include <istream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>

namespace cod3::mpcontrols {

// The game's commands for the two grenades (both programs).
inline constexpr std::string_view kFragCommand = "+grenadeattack";
inline constexpr std::string_view kSpecialCommand = "+smokegrenade";

// A key's binding text runs `command`: it starts with it (any case),
// followed by nothing, a space or ';' ("+grenadeattack" or
// "+grenadeattack;wait").
inline bool BindingIsCommand(std::string_view binding, std::string_view command) {
  if (binding.size() < command.size()) return false;
  for (size_t i = 0; i < command.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(binding[i])) != command[i]) return false;
  return binding.size() == command.size() || binding[command.size()] == ';' || binding[command.size()] == ' ';
}

// Where the layout puts the grenades, from the right and left shoulders'
// bindings, against the keyboard's rows (named for the campaign's layout:
// right shoulder = frag, left shoulder = special/smoke).
enum class GrenadeKeys {
  kUnknown,    // not looked at yet
  kAsNamed,    // RB frag, LB special: the rows press the buttons they are named for
  kSwapped,    // RB special, LB frag: each row presses the other shoulder
  kElsewhere,  // anything else: the rows press the buttons they are named for
};

inline GrenadeKeys GrenadeKeysFor(std::string_view right_shoulder, std::string_view left_shoulder) {
  if (BindingIsCommand(right_shoulder, kFragCommand) && BindingIsCommand(left_shoulder, kSpecialCommand))
    return GrenadeKeys::kAsNamed;
  if (BindingIsCommand(right_shoulder, kSpecialCommand) && BindingIsCommand(left_shoulder, kFragCommand))
    return GrenadeKeys::kSwapped;
  return GrenadeKeys::kElsewhere;
}

// --- A console config's crosshair line ------------------------------------------
// The file Engine::SaveConfig writes: `seta <name> "<value>"` lines (`set`
// too), read as Engine::LoadConfig reads them.

inline constexpr std::string_view kCrosshairDvar = "cg_drawCrosshair";

// The line's dvar name when it is a seta/set line, "" otherwise.
inline std::string ConfigLineName(const std::string& line) {
  std::istringstream ss(line);
  std::string cmd, name;
  if (!(ss >> cmd >> name) || (cmd != "seta" && cmd != "set")) return {};
  return name;
}

// cg_drawCrosshair's value ("0" or "1"; the last such line wins, as in
// LoadConfig), or nullopt when there is none.
inline std::optional<std::string> CrosshairInConfig(std::istream& in) {
  std::optional<std::string> value;
  std::string line;
  while (std::getline(in, line)) {
    if (ConfigLineName(line) != kCrosshairDvar) continue;
    std::istringstream ss(line);
    std::string cmd, name, rest;
    ss >> cmd >> name;
    std::getline(ss, rest);
    const auto first = rest.find_first_not_of(" \t");
    rest = first == std::string::npos ? "" : rest.substr(first);
    if (rest.size() >= 2 && rest.front() == '"' && rest.back() == '"') rest = rest.substr(1, rest.size() - 2);
    if (rest == "0" || rest == "1") value = rest;
  }
  return value;
}

// The config's text with cg_drawCrosshair set to `value`: its first line
// replaced (any later ones dropped), or one added at the end; every other
// line kept. An empty config gets SaveConfig's header line first.
inline std::string ConfigWithCrosshair(std::istream& in, const std::string& value) {
  const std::string ours = "seta " + std::string(kCrosshairDvar) + " \"" + value + "\"";
  std::string out;
  bool any = false, replaced = false;
  std::string line;
  while (std::getline(in, line)) {
    any = true;
    if (ConfigLineName(line) == kCrosshairDvar) {
      if (replaced) continue;
      line = ours;
      replaced = true;
    }
    out += line;
    out += '\n';
  }
  if (!any) out += "// Call of Duty 3 static recomp - archived cvars (seta). Edit freely.\n";
  if (!replaced) out += ours + "\n";
  return out;
}

}  // namespace cod3::mpcontrols
