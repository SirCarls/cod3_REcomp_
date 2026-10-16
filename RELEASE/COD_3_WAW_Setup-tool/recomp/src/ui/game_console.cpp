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
#include "game/diagnostics.h"
#include "ui/waw_dvars.h"

namespace cod3::ui {

namespace {

constexpr const char* kPrompt = "CoD3 >";
constexpr const char* kVersionTag = "Call of Duty 3 - static recomp";
constexpr int kMaxInlineSuggestions = 24;

const char* kLocalCommands[] = {"clear", "cls",  "condump", "cvarlist", "dvarlist",  "dvardump",   "find",
                                "fov",   "cheats", "sv_cheats", "help",   "quit",      "cod3_report"};

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

ImVec4 Q3Color(char code) {
  switch (code) {
    case '0': return ImVec4(0.35f, 0.35f, 0.35f, 1.0f);
    case '1': return ImVec4(1.00f, 0.32f, 0.32f, 1.0f);
    case '2': return ImVec4(0.40f, 1.00f, 0.40f, 1.0f);
    case '3': return ImVec4(1.00f, 0.92f, 0.36f, 1.0f);
    case '4': return ImVec4(0.42f, 0.52f, 1.00f, 1.0f);
    case '5': return ImVec4(0.38f, 0.95f, 1.00f, 1.0f);
    case '6': return ImVec4(1.00f, 0.45f, 1.00f, 1.0f);
    case '8': return ImVec4(1.00f, 0.62f, 0.20f, 1.0f);
    case '9': return ImVec4(0.60f, 0.60f, 0.60f, 1.0f);
    default: return ImVec4(0.92f, 0.92f, 0.92f, 1.0f);
  }
}

ImVec4 ChannelColor(ConsoleChannel ch) {
  switch (ch) {
    case ConsoleChannel::kDeveloper: return ImVec4(0.62f, 0.62f, 0.62f, 1.0f);
    case ConsoleChannel::kError: return ImVec4(1.00f, 0.36f, 0.30f, 1.0f);
    case ConsoleChannel::kEcho: return ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
    case ConsoleChannel::kSystem: return ImVec4(1.00f, 0.80f, 0.35f, 1.0f);
    default: return ImVec4(0.92f, 0.92f, 0.92f, 1.0f);
  }
}

// Draws text with ^N color codes on a single line.
void DrawColored(std::string_view text, ImVec4 base_color) {
  ImVec4 color = base_color;
  size_t start = 0;
  bool first = true;
  auto flush = [&](size_t end) {
    if (end <= start) return;
    if (!first) ImGui::SameLine(0.0f, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(text.data() + start, text.data() + end);
    ImGui::PopStyleColor();
    first = false;
  };
  for (size_t i = 0; i + 1 < text.size(); ++i) {
    if (text[i] == '^' && text[i + 1] >= '0' && text[i + 1] <= '9') {
      flush(i);
      color = text[i + 1] == '7' ? base_color : Q3Color(text[i + 1]);
      start = i + 2;
      ++i;
    }
  }
  flush(text.size());
  if (first) ImGui::TextUnformatted("");
}

std::string FlagString(uint32_t f) {
  namespace cf = cod3::addr::cvar_flag;
  std::string s;
  s += (f & cf::kServerInfo) ? 'S' : ' ';
  s += (f & cf::kUserInfo) ? 'U' : ' ';
  s += (f & cf::kRom) ? 'R' : ' ';
  s += (f & cf::kInit) ? 'I' : ' ';
  s += (f & cf::kArchive) ? 'A' : ' ';
  s += (f & cf::kLatch) ? 'L' : ' ';
  s += (f & cf::kCheat) ? 'C' : ' ';
  s += (f & cf::kTemp) ? 'T' : ' ';
  return s;
}

double NowSeconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

// ===========================================================================
// GameConsole (model)
// ===========================================================================
GameConsole& GameConsole::Get() {
  static GameConsole console;
  return console;
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
    if (HandleLocal(ToLower(args[0]), args, source)) continue;
    if (HandleWaw(cmd, args)) continue;
    // native-render: the port's own settings by their own names too
    // ("cod3_render_percent 75", also from cod3_autoexec), like the F10 log
    // console; F2 shows the new value.
    if (args.size() >= 2 && args[0].rfind("cod3_", 0) == 0) {
      if (const auto* info = rex::cvar::GetFlagInfo(args[0]); info && info->type != rex::cvar::FlagType::Command) {
        const std::string value = JoinFrom(args, 1);
        if (rex::cvar::SetFlagByName(args[0], value)) {
          Print(ConsoleChannel::kSystem, args[0] + " \"" + rex::cvar::GetFlagByName(args[0]) + "\"\n");
          if (on_settings_changed_) on_settings_changed_();
        } else {
          Print(ConsoleChannel::kError, "^1" + args[0] + ": \"" + value + "\" wasn't taken (not an allowed value, or a start-only setting)\n");
        }
        continue;
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

  // "sensitivity" / "cg_gun_x" on its own: show it, World at War style.
  if (d && !set_style && args.size() == 1) {
    const std::string value = waw::Value(*d, engine.Snapshot());
    const char* where = d->kind == waw::Kind::kGame ? "Call of Duty 3: " : "port setting ";
    Print(ConsoleChannel::kGame, "\"" + std::string(d->name) + "\" is:\"" +
                                     (value.empty() ? std::string("?") : value) + "^7\" (" + where +
                                     d->target + ")\n");
    Print(ConsoleChannel::kDeveloper, std::string("  ") + d->help + "\n");
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

bool GameConsole::HandleLocal(const std::string& cmd, const std::vector<std::string>& args,
                              cheats::Source source) {
  auto& engine = Engine::Get();
  if (cmd == "clear" || cmd == "cls") {
    Clear();
    return true;
  }
  if (cmd == "help") {
    Print(ConsoleChannel::kSystem,
          "Console commands: clear, condump <file>, cvarlist / dvarlist [filter], find <text>, "
          "fov [value], sv_cheats [0|1] (cheats is the same command), cod3_report (a report file "
          "for bug reports), quit\n"
          "Cheats: cheat commands (give, god, noclip, notarget, ufo, setviewpos, jumptonode) and "
          "cheat-protected dvars (C in cvarlist, e.g. timescale) work only while sv_cheats is 1, "
          "and every mission starts with sv_cheats 0. sv_cheats 1 puts a CHEATS USED mark on "
          "screen until the mission is loaded again; sv_cheats 0 sets the cheat-protected dvars "
          "back to their defaults and turns god / notarget / noclip / ufo off. In a multiplayer "
          "match only the host can set sv_cheats, and everyone is told.\n"
          "Logging: cod3_log_detail normal | verbose | debug, cod3_perf_log <seconds>, "
          "cod3_perf_overlay 1\n"
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
  if (cmd == "cod3_report") {
    const auto path = diag::WriteReport();
    if (!path.empty())
      Print(ConsoleChannel::kSystem, "Wrote " + cod3::PathToUtf8(path) + "\n");
    else
      Print(ConsoleChannel::kError, "^1cod3_report: could not write the report\n");
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
      Print(ConsoleChannel::kGame, FlagString(v.flags) + " " + v.name + " \"" + v.value + "^7\"\n");
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
// GameConsoleDialog (view)
// ===========================================================================
GameConsoleDialog::GameConsoleDialog(rex::ui::ImGuiDrawer* drawer, bool full)
    : ImGuiDialog(drawer), full_(full) {
  GameConsole::SetOpen(true);
  Engine::Get().RequestSnapshot();
}

GameConsoleDialog::~GameConsoleDialog() {
  GameConsole::SetOpen(false);
}

void GameConsoleDialog::UpdateSuggestions(std::string_view text) {
  suggestions_.clear();
  suggestion_sel_ = -1;
  size_t start = text.find_first_not_of(" /\\");
  if (start == std::string_view::npos) return;
  text.remove_prefix(start);
  // Only the command / cvar name (first token) is completed.
  size_t space = text.find(' ');
  std::string_view word = text.substr(0, space);
  if (word.empty()) return;
  const bool exact_only = space != std::string_view::npos;

  for (auto& v : snapshot_.cvars) {
    if (exact_only ? EqualsNoCase(v.name, word) : StartsWithNoCase(v.name, word))
      suggestions_.push_back({v.name, v.value, v.reset, v.flags, true});
  }
  for (auto& c : snapshot_.commands) {
    if (exact_only ? EqualsNoCase(c, word) : StartsWithNoCase(c, word))
      suggestions_.push_back({c, {}, {}, 0, false});
  }
  for (const char* c : kLocalCommands) {
    if (exact_only ? EqualsNoCase(c, word) : StartsWithNoCase(c, word))
      suggestions_.push_back({c, {}, {}, 0, false});
  }
  for (const auto& w : waw::All()) {
    if (exact_only ? EqualsNoCase(w.name, word) : StartsWithNoCase(w.name, word))
      suggestions_.push_back({w.name, waw::Value(w, snapshot_), {}, 0, true});
  }
  std::sort(suggestions_.begin(), suggestions_.end(), [](const Suggestion& a, const Suggestion& b) {
    return ToLower(a.name) < ToLower(b.name);
  });
  suggestions_.erase(std::unique(suggestions_.begin(), suggestions_.end(),
                                 [](const Suggestion& a, const Suggestion& b) {
                                   return EqualsNoCase(a.name, b.name);
                                 }),
                     suggestions_.end());
}

int GameConsoleDialog::InputCallback(ImGuiInputTextCallbackData* data) {
  auto* self = static_cast<GameConsoleDialog*>(data->UserData);
  switch (data->EventFlag) {
    case ImGuiInputTextFlags_CallbackAlways: {
      std::string_view text(data->Buf, static_cast<size_t>(data->BufTextLen));
      // Completions update typed_prefix_ themselves, so a mismatch here means
      // the user edited the line.
      if (text != self->typed_prefix_) {
        self->typed_prefix_ = std::string(text);
        self->UpdateSuggestions(text);
      }
      break;
    }
    case ImGuiInputTextFlags_CallbackCompletion: {
      auto& sug = self->suggestions_;
      if (sug.empty()) break;
      std::string replacement;
      if (sug.size() == 1) {
        replacement = sug[0].name + " ";
      } else {
        // Longest common prefix first; once it stops growing, cycle matches.
        std::string lcp = sug[0].name;
        for (auto& s : sug) {
          size_t j = 0;
          while (j < lcp.size() && j < s.name.size() &&
                 std::tolower(static_cast<unsigned char>(lcp[j])) ==
                     std::tolower(static_cast<unsigned char>(s.name[j])))
            ++j;
          lcp.resize(j);
        }
        std::string current(data->Buf, static_cast<size_t>(data->BufTextLen));
        size_t lead = current.find_first_not_of(" /\\");
        std::string word = lead == std::string::npos ? "" : current.substr(lead);
        if (lcp.size() > word.size()) {
          replacement = lcp;
        } else {
          self->suggestion_sel_ = (self->suggestion_sel_ + 1) % static_cast<int>(sug.size());
          replacement = sug[self->suggestion_sel_].name;
        }
      }
      data->DeleteChars(0, data->BufTextLen);
      data->InsertChars(0, replacement.c_str());
      if (sug.size() == 1) {
        self->suggestion_sel_ = -1;
        self->typed_prefix_ = replacement;
        self->UpdateSuggestions(replacement);
      } else if (self->suggestion_sel_ < 0) {
        self->typed_prefix_ = replacement;
        self->UpdateSuggestions(replacement);
      } else {
        self->typed_prefix_ = replacement;
      }
      break;
    }
    case ImGuiInputTextFlags_CallbackHistory: {
      auto& hist = GameConsole::Get().history();
      if (hist.empty()) break;
      int prev = self->history_pos_;
      if (data->EventKey == ImGuiKey_UpArrow) {
        self->history_pos_ =
            self->history_pos_ < 0 ? static_cast<int>(hist.size()) - 1 : std::max(0, self->history_pos_ - 1);
      } else if (data->EventKey == ImGuiKey_DownArrow) {
        if (self->history_pos_ >= 0 && ++self->history_pos_ >= static_cast<int>(hist.size()))
          self->history_pos_ = -1;
      }
      if (prev != self->history_pos_) {
        const std::string& h = self->history_pos_ >= 0 ? hist[self->history_pos_] : std::string();
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(0, h.c_str());
        self->suggestion_sel_ = -1;
        self->typed_prefix_ = h;
        self->UpdateSuggestions(h);
      }
      break;
    }
    default:
      break;
  }
  return 0;
}

void GameConsoleDialog::DrawOutput(float height) {
  auto& model = GameConsole::Get();
  if (model.generation() != lines_gen_) {
    lines_gen_ = model.generation();
    model.CopyLines(lines_);
    scroll_to_bottom_ = true;
  }
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
  ImGui::BeginChild("##cod3_output", ImVec2(0, height), false,
                    ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNav);
  ImGuiListClipper clipper;
  clipper.Begin(static_cast<int>(lines_.size()));
  while (clipper.Step()) {
    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
      DrawColored(lines_[i].text, ChannelColor(lines_[i].channel));
  }
  if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) ImGui::SetScrollY(ImGui::GetScrollY() - height * 0.8f);
  if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) ImGui::SetScrollY(ImGui::GetScrollY() + height * 0.8f);
  if (scroll_to_bottom_) {
    ImGui::SetScrollHereY(1.0f);
    scroll_to_bottom_ = false;
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

void GameConsoleDialog::DrawSuggestions(float x, float y, float width) {
  if (suggestions_.empty()) return;
  ImGui::SetNextWindowPos(ImVec2(x, y), ImGuiCond_Always);
  ImGui::SetNextWindowSizeConstraints(ImVec2(width * 0.5f, 0), ImVec2(width, 600));
  ImGui::SetNextWindowBgAlpha(0.88f);
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoFocusOnAppearing | ImGuiWindowFlags_NoNav |
                           ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_AlwaysAutoResize;
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.55f, 0.47f, 0.25f, 1.0f));
  if (ImGui::Begin("##cod3_suggest", nullptr, flags)) {
    const ImVec4 name_col(1.00f, 1.00f, 1.00f, 1.0f);
    const ImVec4 value_col(1.00f, 0.92f, 0.36f, 1.0f);
    const ImVec4 dim_col(0.60f, 0.60f, 0.60f, 1.0f);
    if (suggestions_.size() == 1 && suggestions_[0].is_cvar) {
      // World at War shows the full dvar info for a single match.
      auto& s = suggestions_[0];
      ImGui::TextColored(name_col, "%s", s.name.c_str());
      ImGui::SameLine(0, 24);
      DrawColored(s.value, value_col);
      ImGui::TextColored(dim_col, "  default: %s", s.reset.c_str());
      std::string f = FlagString(s.flags);
      ImGui::TextColored(dim_col, "  flags: [%s]  (S=serverinfo U=userinfo R=read-only I=init "
                                  "A=archived L=latched C=cheat T=temp)", f.c_str());
    } else if (static_cast<int>(suggestions_.size()) > kMaxInlineSuggestions) {
      ImGui::TextColored(dim_col, "%d matches (too many to show here, press Shift+~ for the full console)",
                         static_cast<int>(suggestions_.size()));
    } else {
      float name_w = 0;
      for (auto& s : suggestions_) name_w = std::max(name_w, ImGui::CalcTextSize(s.name.c_str()).x);
      for (int i = 0; i < static_cast<int>(suggestions_.size()); ++i) {
        auto& s = suggestions_[i];
        if (i == suggestion_sel_) {
          ImVec2 p = ImGui::GetCursorScreenPos();
          ImGui::GetWindowDrawList()->AddRectFilled(
              p, ImVec2(p.x + width - 24, p.y + ImGui::GetTextLineHeight()),
              ImGui::GetColorU32(ImVec4(0.45f, 0.38f, 0.16f, 0.9f)));
        }
        ImGui::TextColored(s.is_cvar ? name_col : ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "%s", s.name.c_str());
        if (s.is_cvar) {
          ImGui::SameLine(name_w + 32);
          DrawColored(s.value, value_col);
        }
      }
    }
  }
  ImGui::End();
  ImGui::PopStyleColor();
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
  // Keep autocomplete data fresh while open (twice a second).
  double now = NowSeconds();
  if (now - last_snapshot_request_ > 0.5) {
    Engine::Get().RequestSnapshot();
    last_snapshot_request_ = now;
  }
  auto& engine = Engine::Get();
  if (engine.SnapshotGeneration() != snapshot_gen_) {
    snapshot_ = engine.Snapshot();
    snapshot_gen_ = snapshot_.generation;
    if (suggestion_sel_ < 0) UpdateSuggestions(typed_prefix_);
  }

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
  ImGuiWindowFlags flags = ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                           ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                           ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoCollapse;
  if (ImGui::Begin("##cod3_console", nullptr, flags)) {
    if (full_) {
      DrawOutput(output_h);
      ImGui::Separator();
    }

    ImGui::AlignTextToFramePadding();
    ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.30f, 1.0f), "%s", kPrompt);
    ImGui::SameLine();
    const float tag_w = ImGui::CalcTextSize(kVersionTag).x;
    ImGui::SetNextItemWidth(std::max(100.0f, ImGui::GetContentRegionAvail().x - tag_w - 16.0f));
    if (focus_input_) {
      ImGui::SetKeyboardFocusHere();
      focus_input_ = false;
    }
    ImGuiInputTextFlags in_flags = ImGuiInputTextFlags_EnterReturnsTrue |
                                   ImGuiInputTextFlags_CallbackAlways |
                                   ImGuiInputTextFlags_CallbackCompletion |
                                   ImGuiInputTextFlags_CallbackHistory;
    ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
    bool submitted = ImGui::InputText("##cod3_input", input_, sizeof(input_), in_flags,
                                      &GameConsoleDialog::InputCallback, this);
    ImGui::PopStyleColor();
    const ImVec2 input_max = ImGui::GetItemRectMax();
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.50f, 1.0f), "%s", kVersionTag);

    if (submitted) {
      GameConsole::Get().Submit(input_);
      input_[0] = '\0';
      typed_prefix_.clear();
      suggestions_.clear();
      suggestion_sel_ = -1;
      history_pos_ = -1;
      scroll_to_bottom_ = true;
      ImGui::SetKeyboardFocusHere(-1);
    }
    if (ImGui::IsKeyPressed(ImGuiKey_L) && io.KeyCtrl) GameConsole::Get().Clear();
    // Keep keyboard focus in the input while the console is open.
    if (!ImGui::IsAnyItemActive() && !ImGui::IsAnyItemHovered()) focus_input_ = true;

    ImGui::End();
    ImGui::PopStyleVar(2);
    ImGui::PopStyleColor(3);
    DrawSuggestions(0, y + height + 2.0f, std::max(width * 0.6f, input_max.x));
    return;
  }
  ImGui::End();
  ImGui::PopStyleVar(2);
  ImGui::PopStyleColor(3);
}

}  // namespace cod3::ui
