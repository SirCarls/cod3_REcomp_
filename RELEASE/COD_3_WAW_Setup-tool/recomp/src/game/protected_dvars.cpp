// cod3 - the server_sided_protected dvar category (see protected_dvars.h).

#include "game/protected_dvars.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <charconv>
#include <chrono>
#include <map>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "game/engine.h"
#include "game/movement/program_current.h"
#include "game/movement/rules.h"
#include "game/multiplayer.h"
#include "ui/dvar_domain.h"
#include "ui/game_console.h"
#include "ui/waw_dvars.h"

namespace {
// A change to an own value while the values are the server's is refused
// here too (the port settings' validator), whatever path it took (F2, the
// console, a menu, other port code). The value must be a whole integer: the
// settings' own parser would take "1x" as 1 (cod3.toml and the command line
// are untrusted input).
bool OwnWriteAllowed(std::string_view name, std::string_view value);
}  // namespace

// The players' own values: port settings, saved in cod3.toml like the rest.
REXCVAR_DEFINE_INT32(alternate_movement, cod3::movement::kProgram.default_alternate_movement, "cod3/Movement",
                     "Match movement rule (server_sided_protected): 0 everyone Call of Duty 3, 1 everyone "
                     "Call of Duty 4, 2 each player's own style. The host's in a match")
    .range(0, 2)
    .validator([](std::string_view v) { return OwnWriteAllowed("alternate_movement", v); });
REXCVAR_DEFINE_INT32(alternate_movement_sprint, 1, "cod3/Movement",
                     "Under alternate_movement 1: CoD4 sprint for everyone (server_sided_protected)")
    .range(0, 1)
    .validator([](std::string_view v) { return OwnWriteAllowed("alternate_movement_sprint", v); });
REXCVAR_DEFINE_INT32(alternate_movement_speeds, 1, "cod3/Movement",
                     "Under alternate_movement 1: CoD4 speeds for everyone (server_sided_protected)")
    .range(0, 1)
    .validator([](std::string_view v) { return OwnWriteAllowed("alternate_movement_speeds", v); });
REXCVAR_DEFINE_INT32(alternate_movement_mantle, 1, "cod3/Movement",
                     "Under alternate_movement 1: mantling for everyone (server_sided_protected)")
    .range(0, 1)
    .validator([](std::string_view v) { return OwnWriteAllowed("alternate_movement_mantle", v); });
REXCVAR_DEFINE_INT32(alternate_movement_jumps, 1, "cod3/Movement",
                     "Under alternate_movement 1: PC jump tricks for everyone (server_sided_protected)")
    .range(0, 1)
    .validator([](std::string_view v) { return OwnWriteAllowed("alternate_movement_jumps", v); });

// TEST ONLY: play this machine as a guest in a pretend match, with these
// server values, so every protection path can be tried in the campaign.
// "<seconds>:<name>=<value>;...;<seconds>:end" (seconds after the first
// movement step of a level), e.g.
// "0:alternate_movement=1;0:alternate_movement_mantle=0;120:end". Read once
// at start (the command line or cod3.toml), and never used while a real
// match runs.
REXCVAR_DEFINE_STRING(cod3_test_fake_match, "", "cod3",
                      "TEST ONLY: a pretend match with these server values (see protected_dvars.cpp)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

namespace cod3::protected_dvars {

namespace {

using cheats::Source;
using ui::ConsoleChannel;
using ui::GameConsole;

const std::vector<Entry>& Entries() {
  static const std::vector<Entry> list = {
      {Id::kAlternateMovement, "alternate_movement", 0, 2, movement::kProgram.default_alternate_movement, 0,
       "How everyone in a match moves: 0 Call of Duty 3 (the console), 1 Call of Duty 4 (with the "
       "alternate_movement_* parts), 2 each player's own Movement style. Set by the host; in the campaign "
       "you are the host."},
      {Id::kAlternateMovementSprint, "alternate_movement_sprint", 0, 1, 1, 1,
       "With alternate_movement 1: Call of Duty 4 sprint (4 seconds, aim or fire stops it) for everyone."},
      {Id::kAlternateMovementSpeeds, "alternate_movement_speeds", 0, 1, 1, 1,
       "With alternate_movement 1: Call of Duty 4 strafe, back and sprint speeds for everyone."},
      {Id::kAlternateMovementMantle, "alternate_movement_mantle", 0, 1, 1, 1,
       "With alternate_movement 1: climbing onto ledges up to chest height for everyone."},
      {Id::kAlternateMovementJumps, "alternate_movement_jumps", 0, 1, 1, 1,
       "With alternate_movement 1: Call of Duty 4 PC bounces off steep slopes for everyone."},
  };
  return list;
}

int32_t& OwnStorage(Id id) {
  switch (id) {
    case Id::kAlternateMovement: return REXCVAR_GET(alternate_movement);
    case Id::kAlternateMovementSprint: return REXCVAR_GET(alternate_movement_sprint);
    case Id::kAlternateMovementSpeeds: return REXCVAR_GET(alternate_movement_speeds);
    case Id::kAlternateMovementMantle: return REXCVAR_GET(alternate_movement_mantle);
    default: return REXCVAR_GET(alternate_movement_jumps);
  }
}

std::array<std::atomic<int>, kCount> g_server{};
std::array<std::atomic<bool>, kCount> g_server_set{};
std::atomic<bool> g_test_running{false};   // the pretend match
std::atomic<uint32_t> g_generation{1};

std::mutex g_mutex;  // guards the members below
std::function<void(const std::string&)> g_send;
std::function<void()> g_save;

bool EqualsNoCase(std::string_view x, std::string_view y) {
  if (x.size() != y.size()) return false;
  for (size_t i = 0; i < x.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(x[i])) != std::tolower(static_cast<unsigned char>(y[i])))
      return false;
  return true;
}

std::string Lower(std::string_view s) {
  std::string out(s);
  for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return out;
}

// Strict: an optional '-' and digits, nothing else, within the domain.
bool ParseValue(const Entry& e, std::string_view text, int* out) {
  if (text.empty() || text.size() > 6) return false;
  int v = 0;
  const auto [ptr, ec] = std::from_chars(text.data(), text.data() + text.size(), v);
  if (ec != std::errc() || ptr != text.data() + text.size()) return false;
  if (v < e.min || v > e.max) return false;
  *out = v;
  return true;
}

// Once a second per name and reason at most (a script or a key could repeat
// a refused write every frame).
bool RateLimited(const std::string& what) {
  using Clock = std::chrono::steady_clock;
  static std::mutex mutex;
  static std::map<std::string, Clock::time_point> last;
  const auto now = Clock::now();
  std::lock_guard lock(mutex);
  auto it = last.find(what);
  if (it != last.end() && now - it->second < std::chrono::seconds(1)) return true;
  if (last.size() > 256) last.clear();
  last[what] = now;
  return false;
}

void Refused(const std::string& name, const std::string& why_for_log, const std::string& message) {
  if (RateLimited(name + "|" + why_for_log)) return;
  REXLOG_INFO("cod3: protected: refused a change of {} ({})", name, why_for_log);
  GameConsole::Get().Print(ConsoleChannel::kError, message);
}

const char* SourceName(Source s) {
  switch (s) {
    case Source::kConsole: return "the console";
    case Source::kAutoexec: return "cod3_autoexec";
    case Source::kConfig: return "a config file";
    case Source::kSettings: return "the settings";
    case Source::kNetwork: return "another player";
  }
  return "?";
}

std::string ValueText(Id id, int v) {
  if (id == Id::kAlternateMovement) return std::to_string(v) + " (" + movement::DescribeMode(v) + ")";
  return std::to_string(v);
}

ui::domain::Domain DomainFor(const Entry& e) {
  ui::domain::Domain d;
  if (e.id == Id::kAlternateMovement) {
    d.kind = ui::domain::Domain::Kind::kEnum;
    d.choices = {"Call of Duty 3 for everyone", "Call of Duty 4 for everyone", "each player's own style"};
  } else {
    d.kind = ui::domain::Domain::Kind::kBool;
  }
  return d;
}

void Bump() { g_generation.fetch_add(1, std::memory_order_relaxed); }

void TellHostChange(Id id, int value) {
  // The host's change applies at the next server frame; everyone hears of
  // it as a normal server message (cod3mp sends it), the campaign's player
  // in the console.
  const Entry& e = Info(id);
  const std::string text = id == Id::kAlternateMovement
                               ? std::string("Movement: ") + movement::DescribeMode(value) + " (set by the host)."
                               : std::string("Movement: ") + e.name + " " + std::to_string(value) + " (set by the host).";
  std::function<void(const std::string&)> send;
  {
    std::lock_guard lock(g_mutex);
    send = g_send;
  }
  if (send && mp::IsMatchRunning() && mp::IsHost()) send(text);
}

}  // namespace

// ---------------------------------------------------------------------------
// The list and the values
// ---------------------------------------------------------------------------
const std::vector<Entry>& All() { return Entries(); }

const Entry& Info(Id id) {
  const auto& list = Entries();
  const int i = std::clamp(static_cast<int>(id), 0, kCount - 1);
  return list[static_cast<size_t>(i)];
}

bool Find(std::string_view name, Id* out) {
  for (const Entry& e : Entries()) {
    if (EqualsNoCase(name, e.name)) {
      *out = e.id;
      return true;
    }
  }
  // A World at War name for one of them (none today; kept so a future alias
  // can't slip past the protection).
  if (const ui::waw::Dvar* d = ui::waw::Find(name); d && d->target) {
    for (const Entry& e : Entries()) {
      if (EqualsNoCase(d->target, e.name)) {
        *out = e.id;
        return true;
      }
    }
  }
  return false;
}

Match CurrentMatch() {
  Match m;
  // A real match always wins over the test's pretend one.
  if (!mp::IsMatchRunning() && g_test_running.load(std::memory_order_relaxed)) {
    m.running = true;
    m.host = false;
    m.test = true;
    return m;
  }
  m.running = mp::IsMatchRunning();
  m.host = !m.running || mp::IsHost();
  return m;
}

bool IsLocked() {
  const Match m = CurrentMatch();
  return m.running && !m.host;
}

int Own(Id id) {
  const Entry& e = Info(id);
  return std::clamp(static_cast<int>(OwnStorage(id)), e.min, e.max);
}

bool ServerSet(Id id) {
  return IsLocked() && g_server_set[static_cast<size_t>(id)].load(std::memory_order_relaxed);
}

int Get(Id id) {
  if (!IsLocked()) return Own(id);
  const size_t i = static_cast<size_t>(id);
  // A server that didn't send a value (an older port): the match default,
  // never the player's own value.
  return g_server_set[i].load(std::memory_order_relaxed) ? g_server[i].load(std::memory_order_relaxed)
                                                         : Info(id).match_default;
}

uint32_t Generation() { return g_generation.load(std::memory_order_relaxed); }

std::string LockedReason(Id id) {
  const Entry& e = Info(id);
  return std::string(e.name) + " is set by the server in this match (" + ValueText(id, Get(id)) +
         "); your own value, " + std::to_string(Own(id)) + ", comes back when you leave.";
}

// ---------------------------------------------------------------------------
// Changing an own value
// ---------------------------------------------------------------------------
Verdict RequestSet(Id id, std::string_view value, Source source) {
  Verdict v;
  const Entry& e = Info(id);
  if (source == Source::kNetwork) {
    v.allowed = false;
    v.message = std::string("^1") + e.name + ": not allowed from another player.\n";
    REXLOG_INFO("cod3: protected: refused {} from another player", e.name);
    return v;
  }
  if (IsLocked()) {
    v.allowed = false;
    v.message = "^1" + LockedReason(id) + "\n";
    REXLOG_INFO("cod3: protected: refused a change of {} to '{}' from {} (set by the server in this match)",
                e.name, value, SourceName(source));
    return v;
  }
  int parsed = 0;
  if (!ParseValue(e, value, &parsed)) {
    v.allowed = false;
    v.message = "^1'" + std::string(value) + "' is not a valid value for dvar '" + e.name + "'\n";
    for (const auto& l : ui::domain::Describe(DomainFor(e))) v.message += "  " + l + "\n";
    return v;
  }
  if (parsed != Own(id)) {
    if (!rex::cvar::SetFlagByName(e.name, std::to_string(parsed))) {
      v.allowed = false;
      v.message = std::string("^1") + e.name + " can't be changed now.\n";
      return v;
    }
    v.changed = true;
    Bump();
    REXLOG_INFO("cod3: {} = {} ({})", e.name, parsed, SourceName(source));
    TellHostChange(id, parsed);
  }
  v.message = std::string(e.name) + " \"" + std::to_string(parsed) + "\"" +
              (id == Id::kAlternateMovement ? std::string(": ") + movement::DescribeMode(parsed) : std::string()) +
              "\n";
  return v;
}

Verdict RequestReset(Id id, Source source) { return RequestSet(id, std::to_string(Info(id).default_value), source); }

bool HandleConsoleLine(const std::vector<std::string>& args, Source source,
                       const std::function<void(bool, const std::string&)>& print, bool* changed) {
  *changed = false;
  if (args.empty()) return false;
  const std::string cmd = Lower(args[0]);
  static constexpr const char* kSetStyle[] = {"set", "seta", "sets", "setu", "reset", "toggle", "togglep",
                                              "setfromcvar"};
  const bool set_style = std::any_of(std::begin(kSetStyle), std::end(kSetStyle),
                                     [&](const char* s) { return cmd == s; });
  const size_t name_at = set_style ? 1 : 0;
  if (name_at >= args.size()) return false;
  Id id;
  if (!Find(args[name_at], &id)) return false;
  const Entry& e = Info(id);

  // The name alone: value, default and domain, World at War style.
  if (!set_style && args.size() == 1) {
    for (const auto& l : ui::domain::InfoLines(e.name, std::to_string(Get(id)), std::to_string(e.default_value),
                                               DomainFor(e)))
      print(false, l + "\n");
    print(false, std::string("  ") + e.help + " (server_sided_protected: the server's value in a match)\n");
    if (IsLocked()) print(false, "  " + LockedReason(id) + "\n");
    return true;
  }

  std::string value;
  if (cmd == "reset") {
    value = std::to_string(e.default_value);
  } else if (cmd == "toggle" || cmd == "togglep") {
    // With values: the next one after the current; without: 0 <-> 1.
    const std::string now = std::to_string(Own(id));
    if (args.size() > 2) {
      value = args[2];
      for (size_t i = 2; i < args.size(); ++i)
        if (args[i] == now) value = args[i + 1 < args.size() ? i + 1 : 2];
    } else {
      value = Own(id) != 0 ? "0" : "1";
    }
  } else if (cmd == "setfromcvar") {
    if (args.size() < 3) {
      print(true, "usage: setfromcvar <dvar> <source dvar>\n");
      return true;
    }
    Id other;
    if (Find(args[2], &other)) {
      value = std::to_string(Get(other));
    } else {
      const EngineSnapshot snap = Engine::Get().Snapshot();
      for (const CvarInfo& c : snap.cvars)
        if (EqualsNoCase(c.name, args[2])) value = c.value;
      if (value.empty()) value = rex::cvar::GetFlagByName(args[2]);
    }
  } else {
    const size_t value_at = name_at + 1;
    if (value_at >= args.size()) {
      print(true, "usage: " + args[0] + " " + e.name + " <value>\n");
      return true;
    }
    value = args[value_at];
  }
  const Verdict v = RequestSet(id, value, source);
  if (!v.message.empty()) print(!v.allowed, v.message);
  *changed = v.changed;
  return true;
}

bool RefuseGuestWrite(std::string_view name) {
  // Called for every game dvar write: a cheap look at the names first (no
  // allocation; World at War aliases only exist in the port's console).
  if (name.size() < 18 || name.size() > 64 || (name[0] != 'a' && name[0] != 'A')) return false;
  const Entry* found = nullptr;
  for (const Entry& e : Entries())
    if (EqualsNoCase(name, e.name)) found = &e;
  if (!found) return false;
  const Id id = found->id;
  const Entry& e = *found;
  const std::string message =
      IsLocked() ? "^1" + LockedReason(id) + "\n"
                 : std::string("^1") + e.name + " is a port setting: set it in the ~ console (" + e.name +
                       " <value>), in F2 > Game > Movement or in the game's Options; a config or a script "
                       "can't.\n";
  Refused(e.name, "a game dvar write (config, exec, script or menu)", message);
  return true;
}

// ---------------------------------------------------------------------------
// The multiplayer glue
// ---------------------------------------------------------------------------
bool ApplyServerInfo(std::string_view info) {
  std::array<int, kCount> values{};
  std::array<bool, kCount> set{};
  const bool ok = movement::ForEachInfoPair(info, [&](std::string_view key, std::string_view value) {
    for (const Entry& e : Entries()) {
      if (!EqualsNoCase(key, e.name)) continue;
      int v = 0;
      if (ParseValue(e, value, &v)) {
        values[static_cast<size_t>(e.id)] = v;
        set[static_cast<size_t>(e.id)] = true;
      } else {
        REXLOG_WARN("cod3: protected: the server sent {} = '{}', not a valid value; dropped (the match default "
                    "applies)",
                    e.name, std::string(value).substr(0, 32));
      }
    }
  });
  if (!ok) {
    REXLOG_WARN("cod3: protected: the server's settings string is malformed; nothing taken from it");
    return false;
  }
  bool any_change = false;
  for (int i = 0; i < kCount; ++i) {
    const size_t k = static_cast<size_t>(i);
    if (set[k] == g_server_set[k].load() && (!set[k] || values[k] == g_server[k].load())) continue;
    any_change = true;
    g_server[k].store(values[k]);
    g_server_set[k].store(set[k]);
    if (set[k])
      REXLOG_INFO("cod3: protected: the server sets {} = {}", Entries()[k].name, values[k]);
  }
  if (any_change) {
    Bump();
    GameConsole::Get().Print(ConsoleChannel::kSystem,
                             std::string("Movement in this match: ") +
                                 movement::DescribeMode(Get(Id::kAlternateMovement)) + " (set by the server).\n");
  }
  return true;
}

void LeaveMatch() {
  bool had = false;
  for (int i = 0; i < kCount; ++i) had = g_server_set[static_cast<size_t>(i)].exchange(false) || had;
  g_test_running.store(false);
  Bump();
  if (had) {
    REXLOG_INFO("cod3: protected: left the match; your own values apply again (alternate_movement {})",
                Own(Id::kAlternateMovement));
    GameConsole::Get().Print(ConsoleChannel::kSystem,
                             "Left the match: your own movement settings apply again.\n");
  }
}

std::string ServerInfoPairs() {
  // The host's own values are the match's. (Matches are PC only, owner
  // 2026-10-16: no Xbox 360 player rule.)
  std::string out;
  for (const Entry& e : Entries()) out += std::string("\\") + e.name + "\\" + std::to_string(Own(e.id));
  return out;
}

void SetServerMessageSender(std::function<void(const std::string&)> send) {
  std::lock_guard lock(g_mutex);
  g_send = std::move(send);
}

void SetSaveHandler(std::function<void()> save) {
  std::lock_guard lock(g_mutex);
  g_save = std::move(save);
}

void SaveSettings() {
  std::function<void()> save;
  {
    std::lock_guard lock(g_mutex);
    save = g_save;
  }
  if (save) save();
}

// ---------------------------------------------------------------------------
// The test match
// ---------------------------------------------------------------------------
void Tick() {
  struct Step {
    int seconds = 0;
    bool end = false;
    std::string pair;  // "\name\value"
  };
  static bool parsed = false;
  static std::vector<Step> steps;
  static size_t next = 0;
  static std::chrono::steady_clock::time_point start;
  if (!parsed) {
    parsed = true;
    start = std::chrono::steady_clock::now();
    // cod3.toml and the command line are read without the settings' range
    // check: a value outside the domain is put back to the default once.
    for (const Entry& e : Entries()) {
      int32_t& own = OwnStorage(e.id);
      if (own >= e.min && own <= e.max) continue;
      REXLOG_WARN("cod3: protected: {} = {} from the settings is not {}..{}; back to {}", e.name, own, e.min,
                  e.max, e.default_value);
      own = e.default_value;
    }
    const std::string& script = REXCVAR_GET(cod3_test_fake_match);
    size_t at = 0;
    while (at < script.size() && steps.size() < 64) {
      size_t end = script.find(';', at);
      if (end == std::string::npos) end = script.size();
      const std::string entry = script.substr(at, end - at);
      at = end + 1;
      const size_t colon = entry.find(':');
      if (colon == std::string::npos) continue;
      Step s;
      s.seconds = std::max(0, std::atoi(entry.substr(0, colon).c_str()));
      const std::string what = entry.substr(colon + 1);
      if (what == "end") {
        s.end = true;
      } else if (const size_t eq = what.find('='); eq != std::string::npos) {
        s.pair = "\\" + what.substr(0, eq) + "\\" + what.substr(eq + 1);
      } else {
        continue;
      }
      steps.push_back(std::move(s));
    }
    if (!steps.empty()) REXLOG_INFO("cod3: TEST: a pretend match with {} steps (cod3_test_fake_match)", steps.size());
  }
  if (next >= steps.size()) return;
  // Never during a real match: the test can't stand in for a real server.
  if (mp::IsMatchRunning()) {
    if (g_test_running.exchange(false)) REXLOG_WARN("cod3: TEST: a real match runs; the pretend match is over");
    next = steps.size();
    return;
  }
  const auto elapsed = std::chrono::steady_clock::now() - start;
  std::string info;
  bool end = false;
  while (next < steps.size() && elapsed >= std::chrono::seconds(steps[next].seconds)) {
    if (steps[next].end) end = true;
    else info += steps[next].pair;
    ++next;
  }
  if (!info.empty()) {
    if (!g_test_running.exchange(true)) {
      REXLOG_INFO("cod3: TEST: this machine is now a guest in a pretend match");
      Bump();
    }
    // The pretend server sends everything it has set so far, as a real one
    // resends its whole systeminfo string.
    static std::string sent;
    sent += info;
    ApplyServerInfo(sent);
  }
  if (end) {
    REXLOG_INFO("cod3: TEST: the pretend match ends");
    LeaveMatch();
  }
}

}  // namespace cod3::protected_dvars

namespace {
bool OwnWriteAllowed(std::string_view name, std::string_view value) {
  const bool whole = !value.empty() && value.size() <= 6 &&
                     std::all_of(value.begin(), value.end(), [](char c) { return c >= '0' && c <= '9'; });
  if (!whole) {
    REXLOG_WARN("cod3: protected: '{}' is not a valid value for {}; kept the old one", value.substr(0, 32), name);
    return false;
  }
  if (!cod3::protected_dvars::IsLocked()) return true;
  REXLOG_INFO("cod3: protected: refused a change of {} (set by the server in this match)", name);
  return false;
}
}  // namespace
