// cod3mp - [waw-decomp] 2026-11-16 the plain rules of command_guard_mp.cpp, with no game in them, so
// tests/command_guard_rules_test.cpp can check them with the system's compiler.
//
// Owner, 2026-11-16 14:35: "make sure cod3's can't inject scripts for MP". Read as: nothing that is not part of the
// game as shipped, or of the port's reviewed code, gets a command to run in a multiplayer match. Two places in the
// 2006 code put text into the game's command buffer that doesn't come from the game's own files:
//  - the map command a match start or restart runs ("map <name>" / "map_restart <name>", made by 0x823FD238), whose
//    name is a row of the game's map table; the rows of a map pack come from that pack's own mp/dlmaps.txt, so a
//    pack copied in by hand could put a ';' and any other command after the name (IsPlainMapName);
//  - `exec <file>`: the game runs the lines of a config file as console commands, past the port's cheat guard. At
//    every start it asks for autoexec.cfg, bro.cfg, language.cfg and safemode.cfg, which no known disc ships
//    (c3w/disc_md5), so a file dropped into the game folder would run (CheckExec).

#pragma once

#include <string>
#include <string_view>

namespace cod3::mp::command_guard {

// The longest map or config file name the rules take (the game's own are under 20 characters).
inline constexpr size_t kMaxName = 63;

// A map's file name as the map command takes it (mp_merv, mpmaynne, rouen ...): letters, digits and '_' only, 1 to
// kMaxName characters. Anything else (a ';', a quote, a space, a line break) could end the map command and start
// another one.
inline bool IsPlainMapName(std::string_view name) {
  if (name.empty() || name.size() > kMaxName) return false;
  for (const char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
    if (!ok) return false;
  }
  return true;
}

enum class ExecVerdict {
  kAllow,
  kRefuseStartupFile,  // a file the game looks for at every start that no disc ships
  kRefusePath,         // not a plain file name in the game's config folder
  kRefuseInMatch,      // during a match only the game's own config files run
};

// The file name as `exec` opens it: lower case, '\' read as '/', a leading "config/" dropped, ".cfg" added when the
// name has no extension (the game's COM_DefaultExtension). Empty when it isn't a plain file name: a folder, a drive,
// "..", or a character other than a-z, 0-9, '_', '-' and '.'.
inline std::string NormalizeExecName(std::string_view raw) {
  std::string name;
  for (const char c : raw) {
    char low = (c >= 'A' && c <= 'Z') ? static_cast<char>(c - 'A' + 'a') : c;
    if (low == '\\') low = '/';
    name += low;
  }
  if (name.rfind("config/", 0) == 0) name.erase(0, 7);
  if (name.empty() || name.size() > kMaxName || name.find("..") != std::string::npos) return {};
  for (const char c : name) {
    const bool ok = (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.';
    if (!ok) return {};  // '/', ':', spaces, quotes, ';' and anything else
  }
  if (name.front() == '.') return {};
  if (name.find('.') == std::string::npos) name += ".cfg";
  return name;
}

// The game's own config files, by the names its code execs them with (the same in every build of the program):
// default.cfg, the controller layouts xenon_<x>.cfg and mp_xenon_<x>.cfg (Controls_ExecLayoutFiles) and the stick
// layouts ts_<x>.cfg.
inline bool IsGameConfigName(const std::string& name) {
  auto ends_cfg = [&] { return name.size() > 4 && name.compare(name.size() - 4, 4, ".cfg") == 0; };
  if (name == "default.cfg") return true;
  if (!ends_cfg()) return false;
  return name.rfind("xenon_", 0) == 0 || name.rfind("mp_xenon_", 0) == 0 || name.rfind("ts_", 0) == 0;
}

// Whether `exec <raw>` may run. No name at all is let through: the game prints its own usage line.
inline ExecVerdict CheckExec(std::string_view raw, bool in_match) {
  if (raw.empty()) return ExecVerdict::kAllow;
  const std::string name = NormalizeExecName(raw);
  if (name.empty()) return ExecVerdict::kRefusePath;
  for (const char* probe : {"autoexec.cfg", "bro.cfg", "language.cfg", "safemode.cfg"})
    if (name == probe) return ExecVerdict::kRefuseStartupFile;
  if (in_match && !IsGameConfigName(name)) return ExecVerdict::kRefuseInMatch;
  return ExecVerdict::kAllow;
}

}  // namespace cod3::mp::command_guard
