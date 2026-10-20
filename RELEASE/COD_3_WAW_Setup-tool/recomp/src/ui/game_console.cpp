// cod3 - World at War style drop-down console. See game_console.h.

#include "util.h"
#include "ui/game_console.h"
#include "ui/theme.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "game/addresses.h"
#include "game/decomp_trace.h"
#include "game/diagnostics.h"
#include "game/hd_textures.h"
#include "game/program.h"
#if !defined(COD3_MP)  // [movement] campaign only for now (cod3mp: with the peer-to-peer glue, later)
#include "game/protected_dvars.h"
#endif
#if !defined(COD3_MP)  // [keybinds] campaign only for now
#include "game/keybinds/binds.h"
#endif
#include "game/screenshot.h"
#include "ui/console_view.h"
#include "ui/dvar_domain.h"
#include "ui/dvar_help.h"
#include "ui/problem_log.h"
#include "ui/waw_dvars.h"

namespace cod3::ui {

namespace {

constexpr const char* kVersionTag = "Call of Duty 3 - static recomp";

const char* const kLocalCommands[] = {"clear", "cls",    "condump",   "cvarlist", "dvarlist", "dvardump",
                                      "find",  "fov",    "cheats",    "sv_cheats", "help",    "quit",
                                      "cod3_report", "problems", "screenshot", "cod3_hd_compare",
                                      "cmdlist",     "dvarexport"};

// A file name for condump: the name part only (no folders, no drive, no
// ".."), printable characters, ending in .txt. Anything else gives the default.
std::string SafeDumpName(const std::string& wanted) {
  std::string name;
  for (char c : wanted) {
    const unsigned char u = static_cast<unsigned char>(c);
    if (std::isalnum(u) || c == '_' || c == '-' || c == '.') name += c;
  }
  if (name.empty() || name.find("..") != std::string::npos || name.front() == '.' || name.size() > 64)
    return "condump.txt";
  // Windows device names (con.txt is the console there, not a file).
  std::string stem = name.substr(0, name.find('.'));
  for (auto& c : stem) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  if (stem == "con" || stem == "prn" || stem == "aux" || stem == "nul" ||
      (stem.size() == 4 && (stem.rfind("com", 0) == 0 || stem.rfind("lpt", 0) == 0) &&
       std::isdigit(static_cast<unsigned char>(stem[3]))))
    return "condump.txt";
  if (name.size() < 4 || name.compare(name.size() - 4, 4, ".txt") != 0) name += ".txt";
  return name;
}

bool StartsWithNoCase(std::string_view s, std::string_view prefix) {
  if (prefix.size() > s.size()) return false;
  for (size_t i = 0; i < prefix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(s[i])) !=
        std::tolower(static_cast<unsigned char>(prefix[i])))
      return false;
  }
  return true;
}

bool EqualsNoCase(std::string_view a, std::string_view b) {
  return a.size() == b.size() && StartsWithNoCase(a, b);
}

std::string ToLower(std::string s) {
  for (auto& c : s) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return s;
}

// Q3 tokenizer subset: whitespace separated, "quoted strings" kept whole.
std::vector<std::string> Tokenize(std::string_view s) {
  std::vector<std::string> out;
  size_t i = 0;
  while (i < s.size()) {
    while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
    if (i >= s.size()) break;
    std::string tok;
    if (s[i] == '"') {
      ++i;
      while (i < s.size() && s[i] != '"') tok += s[i++];
      if (i < s.size()) ++i;
    } else {
      while (i < s.size() && !std::isspace(static_cast<unsigned char>(s[i]))) tok += s[i++];
    }
    out.push_back(std::move(tok));
  }
  return out;
}

// Back to one command line, quoting what Tokenize would otherwise split.
std::string JoinCommand(const std::vector<std::string>& args) {
  std::string out;
  for (size_t i = 0; i < args.size(); ++i) {
    if (i) out += ' ';
    const bool quote = args[i].empty() || args[i].find_first_of(" \t;") != std::string::npos;
    out += quote ? "\"" + args[i] + "\"" : args[i];
  }
  return out;
}

std::string JoinFrom(const std::vector<std::string>& args, size_t first) {
  std::string out;
  for (size_t i = first; i < args.size(); ++i) out += (i > first ? " " : "") + args[i];
  return out;
}

// Split "a; b; c" on semicolons that are not inside quotes, and on line
// breaks anywhere: the game's command buffer ends a command at a line break
// even inside quotes, so each piece must pass the cheat guard on its own
// (a --cod3_autoexec value can hold line breaks).
std::vector<std::string> SplitCommands(std::string_view line) {
  std::vector<std::string> out;
  std::string cur;
  bool quoted = false;
  for (char c : line) {
    if (c == '\n' || c == '\r') {
      out.push_back(cur);
      cur.clear();
      quoted = false;
      continue;
    }
    if (c == '"') quoted = !quoted;
    if (c == ';' && !quoted) {
      out.push_back(cur);
      cur.clear();
    } else {
      cur += c;
    }
  }
  out.push_back(cur);
  out.erase(std::remove_if(out.begin(), out.end(),
                           [](const std::string& s) {
                             return s.find_first_not_of(" \t") == std::string::npos;
                           }),
            out.end());
  return out;
}

}  // namespace

// ===========================================================================
// GameConsole (model)
// ===========================================================================
GameConsole& GameConsole::Get() {
  static GameConsole console;
  return console;
}

const std::vector<const char*>& GameConsole::LocalCommands() {
  static const std::vector<const char*> names(std::begin(kLocalCommands), std::end(kLocalCommands));
  return names;
}

void GameConsole::Print(ConsoleChannel channel, std::string_view text) {
  std::lock_guard lock(mutex_);
  size_t pos = 0;
  while (pos <= text.size()) {
    size_t nl = text.find('\n', pos);
    std::string_view piece = text.substr(pos, nl == std::string_view::npos ? std::string_view::npos
                                                                           : nl - pos);
    std::string clean;
    clean.reserve(piece.size());
    for (char c : piece) {
      if (c == '\r') continue;
      clean += (static_cast<unsigned char>(c) < 0x20 && c != '\t') ? ' ' : c;
    }
    if (last_line_open_ && !lines_.empty()) {
      lines_.back().text += clean;
    } else if (!(nl == std::string_view::npos && clean.empty())) {
      lines_.push_back({channel, std::move(clean)});
    }
    if (nl == std::string_view::npos) {
      last_line_open_ = !piece.empty();
      break;
    }
    last_line_open_ = false;
    pos = nl + 1;
    if (pos == text.size()) break;
  }
  while (lines_.size() > kMaxLines) lines_.pop_front();
  ++generation_;
}

void GameConsole::Clear() {
  std::lock_guard lock(mutex_);
  lines_.clear();
  last_line_open_ = false;
  ++generation_;
}

void GameConsole::CopyLines(std::vector<ConsoleLine>& out) {
  std::lock_guard lock(mutex_);
  out.assign(lines_.begin(), lines_.end());
}

bool GameConsole::Dump(const std::filesystem::path& path) {
  std::vector<ConsoleLine> copy;
  CopyLines(copy);
  std::ofstream out(path, std::ios::trunc);
  if (!out) return false;
  for (auto& l : copy) {
    std::string t;
    for (size_t i = 0; i < l.text.size(); ++i) {
      if (l.text[i] == '^' && i + 1 < l.text.size() && std::isdigit(static_cast<unsigned char>(l.text[i + 1]))) {
        ++i;
        continue;
      }
      t += l.text[i];
    }
    out << t << "\n";
  }
  return true;
}

bool GameConsole::IsCheatDvar(const std::string& name) {
  const EngineSnapshot snap = Engine::Get().Snapshot();
  for (const CvarInfo& c : snap.cvars)
    if (EqualsNoCase(c.name, name)) return (c.flags & cod3::addr::cvar_flag::kCheat) != 0;
  return false;
}

void GameConsole::Submit(std::string_view line, cheats::Source source) {
  std::string trimmed(line);
  trimmed.erase(0, trimmed.find_first_not_of(" \t"));
  while (!trimmed.empty() && std::isspace(static_cast<unsigned char>(trimmed.back()))) trimmed.pop_back();
  if (trimmed.empty()) return;

  // Q3 consoles accept a leading slash / backslash.
  if (trimmed[0] == '/' || trimmed[0] == '\\') trimmed.erase(0, 1);

  Print(ConsoleChannel::kEcho, "]" + trimmed + "\n");
  if (source == cheats::Source::kConsole && (history_.empty() || history_.back() != trimmed)) {
    history_.push_back(trimmed);
    while (history_.size() > kMaxHistory) history_.pop_front();
  }

  auto& engine = Engine::Get();
  for (auto& cmd : SplitCommands(trimmed)) {
    auto args = Tokenize(cmd);
    if (args.empty()) continue;
    // The decomp trace build's trace_mark / trace_status / trace_write (game/decomp_trace.h).
    if (cod3::trace::kEnabled && cod3::trace::HandleConsoleCommand(ToLower(args[0]), args)) continue;
#if !defined(COD3_MP)  // [movement]
    // server_sided_protected dvars (game/protected_dvars.h), in every form
    // (set, seta, reset, toggle, "<name> <value>" ...): the port's own; refused
    // while a match's server sets them.
    {
      bool changed = false;
      if (protected_dvars::HandleConsoleLine(
              args, source,
              [this](bool error, const std::string& text) {
                Print(error ? ConsoleChannel::kError : ConsoleChannel::kSystem, text);
              },
              &changed)) {
        if (changed && on_settings_changed_) on_settings_changed_();
        continue;
      }
    }
#endif
#if !defined(COD3_MP)  // [keybinds]
    // "bind B +binoculars", "unbind B", "bind B": keyboard and mouse keys of
    // the port's own actions (game/keybinds/binds.h), saved like F2's keys.
    // Controller buttons (L3, UPARROW ...) stay the game's own binds.
    {
      bool changed = false;
      const bool handled = binds::HandleConsoleCommand(
          args, source,
          [this](bool error, const std::string& text) {
            Print(error ? ConsoleChannel::kError : ConsoleChannel::kSystem, text);
          },
          &changed);
      if (changed && on_settings_changed_) on_settings_changed_();
      if (handled) continue;
    }
#endif
    if (HandleLocal(ToLower(args[0]), args, source)) continue;
    if (HandleWaw(cmd, args)) continue;
    // [native-crt] cod3_profile (the function profiler; it only reads) from
    // this console and from cod3_autoexec too, not only from the F10 log
    // console: "cod3_profile XThread82387D30 10". Typed or start-up script only.
    // [native-lzo] The same for cod3_native_lzo_report (it only writes its
    // counters to the log).
    // [native-math] And cod3_native_math_report (the same) and
    // cod3_native_math_test N (compares at most 2,000,000 more arguments per
    // function with the game's code; the game pauses a few seconds at most).
    // [native-printf] And cod3_native_printf_report and cod3_native_printf_test
    // N (compares at most 1,000,000 more random formats with the game's code).
    // [native-strtol] And cod3_native_strtol_report and cod3_native_strtol_test N.
    // [native-fmod-mix] And cod3_native_resampler_report and
    // cod3_native_fmod_mix_report (they only write their counters to the log).
    if ((ToLower(args[0]) == "cod3_profile" || ToLower(args[0]) == "cod3_native_lzo_report" ||
         ToLower(args[0]) == "cod3_native_math_report" ||
         ToLower(args[0]) == "cod3_native_math_test" ||
         ToLower(args[0]) == "cod3_native_printf_report" ||
         ToLower(args[0]) == "cod3_native_printf_test" ||
         ToLower(args[0]) == "cod3_native_strtol_report" ||
         ToLower(args[0]) == "cod3_native_strtol_test" ||
         ToLower(args[0]) == "cod3_native_resampler_report" ||
         ToLower(args[0]) == "cod3_native_fmod_mix_report") &&
        (source == cheats::Source::kConsole || source == cheats::Source::kAutoexec)) {
      rex::cvar::InvokeCommand(ToLower(args[0]), args.size() > 1 ? JoinFrom(args, 1) : std::string());
      continue;
    }
    // native-render: the port's own settings by their own names too
    // ("cod3_render_percent 75", also from cod3_autoexec), like the F10 log
    // console; F2 shows the new value.
    if (args.size() >= 2 && args[0].rfind("cod3_", 0) == 0) {
      if (const auto* info = rex::cvar::GetFlagInfo(args[0]); info && info->type != rex::cvar::FlagType::Command) {
        std::string value = JoinFrom(args, 1);
        // A choice by its number in the domain list, as World at War takes them.
        const auto& choices = info->constraints.allowed_values;
        if (!choices.empty() && std::find(choices.begin(), choices.end(), value) == choices.end() &&
            !value.empty() && value.size() <= 3 && std::all_of(value.begin(), value.end(), [](char c) {
              return std::isdigit(static_cast<unsigned char>(c)) != 0;
            })) {
          if (const size_t index = std::stoul(value); index < choices.size()) value = choices[index];
        }
        if (rex::cvar::SetFlagByName(args[0], value)) {
          Print(ConsoleChannel::kSystem, args[0] + " \"" + rex::cvar::GetFlagByName(args[0]) + "\"\n");
          if (on_settings_changed_) on_settings_changed_();
        } else {
          // World at War's answer to a value outside the domain.
          Print(ConsoleChannel::kError, "^1'" + value + "' is not a valid value for dvar '" + args[0] +
                                            "' (or it is only read at start)\n");
          for (const auto& l : domain::Describe(domain::ForPortSetting(*info)))
            Print(ConsoleChannel::kGame, "  " + l + "\n");
        }
        continue;
      }
    }
    // A port setting on its own ("cod3_render_percent", "fullscreen"): its
    // value, default and domain, World at War style. The game's own names
    // come first (the game answers those itself).
    if (args.size() == 1) {
      const auto* info = rex::cvar::GetFlagInfo(args[0]);
      if (info && info->type != rex::cvar::FlagType::Command) {
        const EngineSnapshot snap = engine.Snapshot();
        const bool game_name =
            std::any_of(snap.cvars.begin(), snap.cvars.end(), [&](const CvarInfo& c) { return EqualsNoCase(c.name, args[0]); }) ||
            std::any_of(snap.commands.begin(), snap.commands.end(), [&](const std::string& c) { return EqualsNoCase(c, args[0]); });
        if (!game_name) {
          for (const auto& l : domain::InfoLines(info->name, domain::PortValueText(*info, rex::cvar::GetFlagByName(args[0])),
                                                 domain::PortValueText(*info, info->default_value),
                                                 domain::ForPortSetting(*info)))
            Print(ConsoleChannel::kGame, l + "\n");
          if (!info->description.empty()) Print(ConsoleChannel::kDeveloper, "  " + info->description + "\n");
          continue;
        }
      }
    }
    // The cheat guard (game/cheat_guard.h): cheat commands and cheat-protected
    // dvars only while cheats are on, host-only commands only for the host.
    // The game checks again on its side; this gives the plain message.
    const bool set_style = args.size() >= 2 &&
                           (EqualsNoCase(args[0], "set") || EqualsNoCase(args[0], "seta") ||
                            EqualsNoCase(args[0], "sets") || EqualsNoCase(args[0], "setu") ||
                            EqualsNoCase(args[0], "reset") || EqualsNoCase(args[0], "toggle") ||
                            EqualsNoCase(args[0], "togglep") || EqualsNoCase(args[0], "setfromcvar"));
    const std::string& dvar = args[set_style ? 1 : 0];
    const bool cheat_dvar = IsCheatDvar(dvar);
    if (const cheats::Verdict v = cheats::Check(args, source, cheat_dvar); !v.allowed) {
      Print(ConsoleChannel::kError, v.message);
      REXLOG_INFO("cod3: refused the console line '{}' (cheat guard)", cmd);
      continue;
    }
    if (!engine.GameReady()) {
      Print(ConsoleChannel::kError, "^3The game has not finished starting yet.\n");
      continue;
    }
    // seta: also remember it in our config so it survives restarts. Never a
    // cheat-protected dvar: the game resets those at each level while cheats
    // are off, and the config must not carry cheats.
    if (EqualsNoCase(args[0], "seta") && args.size() >= 3) {
      if (cheat_dvar) {
        Print(ConsoleChannel::kSystem, "^3" + args[1] + " is cheat protected and isn't saved to the config.\n");
      } else {
        std::string value = args[2];
        for (size_t i = 3; i < args.size(); ++i) value += " " + args[i];
        engine.SetArchived(args[1], value);
      }
    }
    engine.QueueCommand(cmd);
  }
  engine.RequestSnapshot();
}

bool GameConsole::HandleWaw(std::string& cmd, std::vector<std::string>& args) {
  auto& engine = Engine::Get();
  const bool set_style = args.size() >= 2 && (EqualsNoCase(args[0], "set") || EqualsNoCase(args[0], "seta") ||
                                              EqualsNoCase(args[0], "sets") || EqualsNoCase(args[0], "setu"));
  const size_t name_at = set_style ? 1 : 0;
  const waw::Dvar* d = waw::Find(args[name_at]);

  // "sensitivity" / "cg_gun_x" on its own: its value, default and domain,
  // World at War style (ui/dvar_domain.h), then what it is in this port.
  if (d && !set_style && args.size() == 1) {
    const EngineSnapshot snap = engine.Snapshot();
    const std::string value = waw::Value(*d, snap);
    for (const auto& line : domain::InfoLines(d->name, value.empty() ? "?" : value, waw::DefaultValue(*d, snap),
                                              domain::ForWaw(*d, snap)))
      Print(ConsoleChannel::kGame, line + "\n");
    const char* where = d->kind == waw::Kind::kGame ? "Call of Duty 3's " : "the port's setting ";
    Print(ConsoleChannel::kDeveloper, std::string("  ") + d->help + " (" + where + d->target + ")\n");
    return true;
  }
  // Port settings are the PC port's own: set here, not in the game.
  if (d && d->kind == waw::Kind::kPort && args.size() > name_at + 1) {
    const waw::SetResult r = waw::Set(*d, JoinFrom(args, name_at + 1));
    Print(r.ok ? ConsoleChannel::kSystem : ConsoleChannel::kError, r.message);
    if (r.changed_port_setting && on_settings_changed_) on_settings_changed_();
    return true;
  }
  // Game aliases go to the game under CoD3's name (cheat protection applies).
  std::vector<std::string> rewritten;
  if (waw::RewriteGameAlias(args, &rewritten)) {
    args = std::move(rewritten);
    cmd = JoinCommand(args);
    return false;
  }
  // A World at War dvar this game has nothing for.
  const std::string& name = args[name_at];
  if (waw::IsWawOnly(name)) {
    const EngineSnapshot snap = engine.Snapshot();
    const bool known = std::any_of(snap.cvars.begin(), snap.cvars.end(),
                                   [&](const CvarInfo& c) { return EqualsNoCase(c.name, name); });
    if (!known) {
      Print(ConsoleChannel::kSystem, "^3" + name +
                                         "^7 is a World at War dvar; Call of Duty 3 has nothing for it.\n");
      return true;
    }
  }
  return false;
}

// help <name>: what one dvar, setting or command is; help <words>: the
// dvars whose name or help line has every word (ui/dvar_help.h).
void GameConsole::PrintHelp(const std::string& words) {
  const EngineSnapshot snap = Engine::Get().Snapshot();
  const bool one_word = words.find(' ') == std::string::npos;
  if (one_word) {
    const CvarInfo* live = nullptr;
    for (const CvarInfo& c : snap.cvars)
      if (EqualsNoCase(c.name, words)) live = &c;
    const dvar_help::Entry* e = dvar_help::Find(words);
    if (e || live) {
      const std::string name = e ? e->name : live->name;
      Print(ConsoleChannel::kGame, "^3" + name + "^7: " + (e ? e->text : "(no help line for it yet)") + "\n");
      std::string where = e ? std::string(dvar_help::CategoryTitle(e->category)) + "; " +
                                  dvar_help::ProgramsText(e->programs)
                            : std::string();
      if (live) {
        where += std::string(where.empty() ? "" : "; ") + "value \"" + live->value + "^7\" default \"" + live->reset +
                 "^7\"";
        const std::string flags = ConsoleFlagString(live->flags);
        if (flags.find_first_not_of(' ') != std::string::npos) where += " flags [" + flags + "]";
        if (live->flags & cod3::addr::cvar_flag::kCheat) where += " (cheat protected)";
      } else {
        where += "; not registered right now";
      }
      Print(ConsoleChannel::kDeveloper, "  " + where + "\n");
      return;
    }
    if (const waw::Dvar* d = waw::Find(words)) {
      const char* kind = d->kind == waw::Kind::kGame ? "Call of Duty 3's " : "the port's setting ";
      Print(ConsoleChannel::kGame, "^3" + std::string(d->name) + "^7: " + d->help + " (World at War name for " +
                                       kind + d->target + ")\n");
      return;
    }
    if (const auto* info = rex::cvar::GetFlagInfo(words); info && info->type != rex::cvar::FlagType::Command) {
      Print(ConsoleChannel::kGame, "^3" + std::string(info->name) + "^7: " + info->description +
                                       " (a port setting; F2 has most of them)\n");
      return;
    }
    const bool command = std::any_of(snap.commands.begin(), snap.commands.end(),
                                     [&](const std::string& c) { return EqualsNoCase(c, words); }) ||
                         std::any_of(std::begin(kLocalCommands), std::end(kLocalCommands),
                                     [&](const char* c) { return EqualsNoCase(c, words); });
    if (command) {
      Print(ConsoleChannel::kGame, words + " is a command; help (no word) lists the console's own.\n");
      return;
    }
  }
  const auto found = dvar_help::Search(words);
  constexpr size_t kMax = 40;
  for (size_t i = 0; i < found.size() && i < kMax; ++i)
    Print(ConsoleChannel::kGame, "  ^3" + std::string(found[i]->name) + "^7  " + found[i]->text + "\n");
  if (found.size() > kMax)
    Print(ConsoleChannel::kDeveloper, "  ... and " + std::to_string(found.size() - kMax) + " more (more words narrow it)\n");
  Print(ConsoleChannel::kSystem, found.empty() ? "No dvar's name or help has \"" + words + "\".\n"
                                               : std::to_string(found.size()) + " matches\n");
}

bool GameConsole::HandleLocal(const std::string& cmd, const std::vector<std::string>& args,
                              cheats::Source source) {
  auto& engine = Engine::Get();
  if (cmd == "clear" || cmd == "cls") {
    Clear();
    return true;
  }
  if (cmd == "help" && args.size() > 1) {
    PrintHelp(JoinFrom(args, 1));
    return true;
  }
  if (cmd == "help") {
    Print(ConsoleChannel::kSystem,
          "Console commands: clear, condump <file>, cvarlist / dvarlist [filter], find <text>, "
          "help <dvar> (what it does), help <words> (the dvars whose name or help has them), "
          "cmdlist [filter] (the game's commands), dvarexport [file] (every dvar and command as a "
          "table in userdata/), "
          "fov [value], sv_cheats [0|1] (cheats is the same command), cod3_report (a report file "
          "for bug reports), problems [count] (this session's warnings and errors, as in F2 > "
          "Console > Problems), screenshot (the game's picture, into "
          "userdata/logs/screenshots), cod3_hd_compare [seconds] (the same view with the HD "
          "textures and without them: stand still), quit\n"
          "A dvar or setting name on its own shows its value, default and domain, World at War "
          "style (\"cg_fov\" is:\"65\" default:\"65\", Domain is any number from 65 to 160), and "
          "what it does. The list of every dvar: docs/DVARS.md.\n"
          "Cheats: cheat commands (give, god, noclip, notarget, ufo, setviewpos, jumptonode) and "
          "cheat-protected dvars (C in cvarlist, e.g. timescale) work only while sv_cheats is 1, "
          "and every mission starts with sv_cheats 0. sv_cheats 1 puts a CHEATS USED mark on "
          "screen until the mission is loaded again; sv_cheats 0 sets the cheat-protected dvars "
          "back to their defaults and turns god / notarget / noclip / ufo off. In a multiplayer "
          "match only the host can set sv_cheats, and everyone is told.\n"
          "Logging: cod3_log_detail normal | verbose | debug, cod3_perf_log <seconds>, "
          "cod3_perf_overlay 1, cod3_debug_screenshots <seconds> (with debug detail: the "
          "game's picture every so often, 0 = none)\n"
          "World at War dvar names work too (W in cvarlist): CoD3's own under another name "
          "(cg_gun_x = cg_gunX) or the port's settings (sensitivity, m_yaw, r_mode 1920x1080, "
          "r_fullscreen, r_vsync, snd_volume).\n"
          "Everything else runs through the game's command buffer, e.g. map island, god, "
          "noclip, give <weapon>, cg_fov 90, cg_fovScale 1.2, com_maxfps 144 (0 = no limit), "
          "cg_drawFPS 1, cmdlist, exec <cfg>\n");
    return true;
  }
  // cheats / sv_cheats: one command under two names, and the only way the
  // port turns cheats on or off (game/cheat_guard.h). "set sv_cheats 1",
  // "seta ...", "toggle sv_cheats" land here too (the game's dvar is
  // read-only, so they would do nothing, silently).
  {
    const bool set_style = args.size() >= 2 &&
                           (cmd == "set" || cmd == "seta" || cmd == "sets" || cmd == "setu" ||
                            cmd == "toggle" || cmd == "togglep" || cmd == "reset");
    const bool on_sv_cheats = set_style && cheats::IsSvCheatsName(args[1]);
    if (cmd == "cheats" || cmd == "sv_cheats" || on_sv_cheats) {
      std::vector<std::string> request = {"sv_cheats"};
      if (on_sv_cheats) {
        if (cmd == "reset") request.push_back("0");
        else if (cmd == "toggle" || cmd == "togglep") request.push_back(cheats::Enabled() ? "0" : "1");
        else if (args.size() >= 3) request.push_back(args[2]);
      } else if (args.size() >= 2) {
        request.push_back(args[1]);
      }
      const cheats::Verdict v = cheats::CheatsCommand(request, source);
      Print(v.allowed ? ConsoleChannel::kSystem : ConsoleChannel::kError, v.message);
      return true;
    }
  }
  // problems [count]: the newest kinds of warnings and errors (ui/problem_log.h).
  if (cmd == "problems") {
    const long wanted = args.size() > 1 ? std::strtol(args[1].c_str(), nullptr, 10) : 20;
    const size_t count = static_cast<size_t>(std::clamp(wanted, 1L, static_cast<long>(ProblemLog::kMaxGroups)));
    Print(ConsoleChannel::kSystem, ProblemLog::Get().AsText(count));
    return true;
  }
  if (cmd == "cod3_report") {
    const auto path = diag::WriteReport();
    if (!path.empty())
      Print(ConsoleChannel::kSystem, "Wrote " + cod3::PathToUtf8(path) + "\n");
    else
      Print(ConsoleChannel::kError, "^1cod3_report: could not write the report\n");
    return true;
  }
  // screenshot: the game's own picture (game/screenshot.h); never the
  // console, F2, the desktop or another window. No file name is taken.
  if (cmd == "screenshot") {
    const std::string file = shot::Request(shot::Kind::kAsked, "asked for in the console");
    if (!file.empty())
      Print(ConsoleChannel::kSystem,
            "Saving userdata/logs/screenshots/" + file + " (the game's picture only)\n");
    else if (shot::Available())
      Print(ConsoleChannel::kError, "^1screenshot: one was just taken; try again in a moment\n");
    else
      Print(ConsoleChannel::kError, "^1screenshot: not available (no GPU emulation)\n");
    return true;
  }
  // cod3_hd_compare [seconds]: the same view with and without the HD
  // textures, a moment apart (game/screenshot.h).
  if (cmd == "cod3_hd_compare") {
    const HdTextures hd = GetHdTextures();
    if (cod3::kMultiplayer || hd::IsMultiplayerMode()) {
      // Campaign only: in multiplayer no texture changes at all.
      Print(ConsoleChannel::kError, "^1cod3_hd_compare: campaign only (not in multiplayer)\n");
    } else if (hd != HdTextures::kOn && hd != HdTextures::kCommandLine) {
      Print(ConsoleChannel::kError,
            std::string("^1cod3_hd_compare: the HD textures aren't in use in this session (") +
                DescribeHdTextures(hd) + "; F2 > Video > HD textures, then restart)\n");
    } else {
      const double seconds = args.size() > 1 ? std::strtod(args[1].c_str(), nullptr) : 2.0;
      const std::string stem = shot::RequestHdCompare(seconds > 0 ? seconds : 2.0);
      if (stem.empty())
        Print(ConsoleChannel::kError, "^1cod3_hd_compare: one is already under way\n");
      else
        Print(ConsoleChannel::kSystem,
              "Stand still: saving userdata/logs/screenshots/" + stem +
                  "-on.png, then the HD textures go off for a moment for " + stem + "-off.png\n");
    }
    return true;
  }
  if (cmd == "condump") {
    // Always a file right inside userdata/ (SafeDumpName).
    const std::string name = SafeDumpName(args.size() > 1 ? args[1] : "condump.txt");
    auto path = user_dir_ / name;
    std::error_code ec;
    std::filesystem::create_directories(user_dir_, ec);
    if (Dump(path))
      Print(ConsoleChannel::kSystem, "Dumped console text to " + cod3::PathToUtf8(path) + "\n");
    else
      Print(ConsoleChannel::kError, "^1condump: could not write " + cod3::PathToUtf8(path) + "\n");
    return true;
  }
  if (cmd == "cvarlist" || cmd == "dvarlist" || cmd == "dvardump" || cmd == "find") {
    if (cmd == "find" && args.size() < 2) {
      Print(ConsoleChannel::kSystem, "usage: find <text>\n");
      return true;
    }
    std::string filter = args.size() > 1 ? ToLower(args[1]) : "";
    auto snap = engine.Snapshot();
    int count = 0;
    for (auto& v : snap.cvars) {
      if (!filter.empty() && ToLower(v.name).find(filter) == std::string::npos) continue;
      Print(ConsoleChannel::kGame, ConsoleFlagString(v.flags) + " " + v.name + " \"" + v.value + "^7\"\n");
      ++count;
    }
    // World at War names for CoD3 dvars and port settings.
    for (const auto& w : waw::All()) {
      if (!filter.empty() && ToLower(w.name).find(filter) == std::string::npos) continue;
      Print(ConsoleChannel::kGame, std::string("  W  ") + w.name + " \"" + waw::Value(w, snap) + "^7\"  -> " +
                                       w.target + "\n");
      ++count;
    }
    if (cmd == "find") {
      for (auto& c : snap.commands) {
        if (ToLower(c).find(filter) == std::string::npos) continue;
        Print(ConsoleChannel::kGame, "    cmd  " + c + "\n");
        ++count;
      }
      int waw_only = 0;
      std::string names;
      for (auto n : waw::WawOnlyNames()) {
        if (ToLower(std::string(n)).find(filter) == std::string::npos) continue;
        if (waw_only < 40) names += (waw_only ? ", " : "") + std::string(n);
        ++waw_only;
      }
      if (waw_only)
        Print(ConsoleChannel::kDeveloper,
              "World at War only (" + std::to_string(waw_only) + ", nothing in Call of Duty 3): " + names +
                  (waw_only > 40 ? ", ..." : "") + "\n");
    }
    Print(ConsoleChannel::kSystem, std::to_string(count) + " matches\n");
    return true;
  }
  // cmdlist [filter]: the game's own cmdlist prints nothing in this build.
  if (cmd == "cmdlist") {
    const std::string filter = args.size() > 1 ? ToLower(args[1]) : "";
    const EngineSnapshot snap = engine.Snapshot();
    int count = 0;
    for (const auto& c : snap.commands) {
      if (!filter.empty() && ToLower(c).find(filter) == std::string::npos) continue;
      Print(ConsoleChannel::kGame, "    " + c + "\n");
      ++count;
    }
    std::string local;
    for (const char* c : kLocalCommands)
      if (filter.empty() || ToLower(c).find(filter) != std::string::npos) local += std::string(local.empty() ? "" : ", ") + c;
    if (!local.empty()) Print(ConsoleChannel::kDeveloper, "The console's own: " + local + "\n");
    Print(ConsoleChannel::kSystem, std::to_string(count) + " commands\n");
    return true;
  }
  // dvarexport [file]: every dvar (name, flags, value, default) and command as
  // a tab-separated table, a file right inside userdata/ (SafeDumpName), for
  // the dvar reference (docs/DVARS.md).
  if (cmd == "dvarexport") {
    const std::string name = SafeDumpName(args.size() > 1 ? args[1] : "dvars.txt");
    const auto path = user_dir_ / name;
    const EngineSnapshot snap = engine.Snapshot();
    std::error_code ec;
    std::filesystem::create_directories(user_dir_, ec);
    std::ofstream out(path, std::ios::trunc);
    auto clean = [](std::string v) {
      for (auto& c : v)
        if (c == '\t' || c == '\n' || c == '\r') c = ' ';
      return v;
    };
    if (out) {
      out << "# " << (cod3::kMultiplayer ? "multiplayer" : "campaign") << "\tkind\tname\tflags\tvalue\tdefault\n";
      for (const auto& v : snap.cvars) {
        char flags[16];
        std::snprintf(flags, sizeof(flags), "0x%X", v.flags);
        out << "dvar\t" << clean(v.name) << "\t" << flags << "\t" << clean(v.value) << "\t" << clean(v.reset) << "\n";
      }
      for (const auto& c : snap.commands) out << "cmd\t" << clean(c) << "\t\t\t\n";
    }
    if (out)
      Print(ConsoleChannel::kSystem, "Wrote " + std::to_string(snap.cvars.size()) + " dvars and " +
                                         std::to_string(snap.commands.size()) + " commands to " +
                                         cod3::PathToUtf8(path) + "\n");
    else
      Print(ConsoleChannel::kError, "^1dvarexport: could not write " + cod3::PathToUtf8(path) + "\n");
    return true;
  }
  if (cmd == "fov") {
    if (args.size() < 2) {
      char buf[96];
      std::snprintf(buf, sizeof(buf), "cg_fov is %g, cg_fovScale is %g (effective %g)\n",
                    engine.fov(), engine.fov_scale(), engine.fov() * engine.fov_scale());
      Print(ConsoleChannel::kSystem, buf);
    } else {
      engine.SetFov(std::strtof(args[1].c_str(), nullptr));
    }
    return true;
  }
  // The game's own quit expects the Xbox dashboard; on PC it closes the window.
  if (cmd == "quit" || cmd == "exit") {
    if (on_quit_) {
      Print(ConsoleChannel::kSystem, "Quitting...\n");
      on_quit_();
    }
    return true;
  }
  return false;
}

// ===========================================================================
// GameConsoleDialog (the drop-down window around the shared ConsoleView)
// ===========================================================================
GameConsoleDialog::GameConsoleDialog(rex::ui::ImGuiDrawer* drawer, bool full)
    : ImGuiDialog(drawer), view_(std::make_unique<ConsoleView>()), full_(full) {
  GameConsole::SetOpen(true);
}

GameConsoleDialog::~GameConsoleDialog() {
  GameConsole::SetOpen(false);
}

void GameConsoleDialog::OnDraw(ImGuiIO& io) {
  // The console keeps the SDK's small fixed-width font (columns line up);
  // the F2 window and the HUD use the theme's font (ui/theme.h).
  ImFont* mono = MonoFont();
  ImGui::PushFont(mono, mono->LegacySize);
  DrawConsole(io);
  ImGui::PopFont();
}

void GameConsoleDialog::DrawConsole(ImGuiIO& io) {
  view_->Refresh();
  open_anim_ = std::min(1.0f, open_anim_ + io.DeltaTime * 10.0f);
  const float width = io.DisplaySize.x;
  const float line_h = ImGui::GetFrameHeightWithSpacing();
  const float output_h = full_ ? std::floor(io.DisplaySize.y * 0.45f) : 0.0f;
  const float height = output_h + line_h + ImGui::GetStyle().WindowPadding.y * 2.0f + 6.0f;
  const float y = -height * (1.0f - open_anim_);

  ImGui::SetNextWindowPos(ImVec2(0, y), ImGuiCond_Always);
  ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_Always);
  ImGui::PushStyleColor(ImGuiCol_WindowBg, ImVec4(0.06f, 0.06f, 0.05f, 0.90f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.55f, 0.47f, 0.25f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0.12f, 0.12f, 0.10f, 1.0f));
  ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
  ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 1.0f);
  const ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                                 ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse;
  const bool shown = ImGui::Begin("##cod3_console", nullptr, flags);
  if (shown) {
    if (full_) {
      view_->DrawOutput(output_h);
      ImGui::Separator();
    }
    view_->DrawInputLine(kVersionTag, /*keep_focus=*/true);
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(3);
  if (shown) view_->DrawSuggestions(ImVec2(0, y + height + 2.0f), false, std::max(width * 0.6f, view_->input_max().x));
}

}  // namespace cod3::ui
