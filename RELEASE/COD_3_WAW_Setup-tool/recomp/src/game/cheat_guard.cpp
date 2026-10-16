// cod3 - cheat policy for every source (see cheat_guard.h).

#include "game/cheat_guard.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <map>
#include <mutex>

#include <rex/hook.h>
#include <rex/logging.h>

#include "game/addresses.h"
#include "game/cheat_watch.h"
#include "game/engine.h"
#include "game/game_api.h"
#include "game/guest.h"
#include "game/multiplayer.h"
#include "ui/announcement_feed.h"
#include "ui/game_console.h"

namespace cod3::cheats {

namespace {

namespace a = cod3::addr;
using guest::ReadU32;

std::atomic<bool> g_enabled{false};

bool EqualsNoCase(std::string_view x, std::string_view y) {
  if (x.size() != y.size()) return false;
  for (size_t i = 0; i < x.size(); ++i)
    if (std::tolower(static_cast<unsigned char>(x[i])) != std::tolower(static_cast<unsigned char>(y[i])))
      return false;
  return true;
}

// The game's cheat commands: the client commands that ask CheatsOk (god,
// notarget, noclip, ufo, give, take, invinc) and the two that test sv_cheats
// themselves (setviewpos, jumptonode), from the command table at 0x825F49F0
// (game/addresses.h, namespace cheats; cheat_watch.cpp hooks them).
constexpr const char* kCheatCommands[] = {"give",       "take",  "god",        "notarget", "noclip",
                                          "ufo",        "invinc", "setviewpos", "jumptonode"};

// Commands that start, restart or end the match, or turn cheats on: the
// host's business in multiplayer.
constexpr const char* kHostOnlyCommands[] = {"map",          "spmap",     "devmap",        "map_restart",
                                             "fast_restart", "reboot",    "load_mp_exe",   "cheats",
                                             "sv_cheats",    "loadgame",  "quickstart",    "frontend",
                                             "cvar_restart", "writeconfig", "writedefaults"};

// set-style commands: the dvar is the second word.
constexpr const char* kSetStyle[] = {"set", "seta", "sets", "setu", "reset", "toggle", "togglep",
                                     "setfromcvar"};

const char* SourceName(Source s) {
  switch (s) {
    case Source::kConsole: return "the console";
    case Source::kAutoexec: return "cod3_autoexec";
    case Source::kConfig: return "the console config";
    case Source::kSettings: return "the settings";
    case Source::kNetwork: return "another player";
  }
  return "?";
}

std::string HowToEnable() {
  if (mp::IsMatchRunning()) return "only the host can turn them on (sv_cheats 1).";
  return "turn them on with sv_cheats 1 first (the CHEATS USED mark then stays on screen until "
         "the mission is loaded again).";
}

// Turns cheats on or off on the game's main thread, once the policy allowed
// it, and tells everyone.
void ApplySvCheats(PPCContext& ctx, uint8_t* base, bool on, Source source) {
  Engine::CvarSet(ctx, base, "sv_cheats", on ? "1" : "0", /*force=*/true);
  Sync(base);
  const bool now = Enabled();
  if (now != on) {
    REXLOG_WARN("cod3: sv_cheats {} asked for from {}, but the game kept {}", on ? 1 : 0,
                SourceName(source), now ? 1 : 0);
    return;
  }
  REXLOG_INFO("cod3: sv_cheats {} ({})", on ? 1 : 0, SourceName(source));
  if (on)
    CheatWatch::Get().OneShot("sv_cheats 1");  // the CHEATS USED mark stays until the mission loads again
  else
    CheatWatch::Get().ResetCheats(ctx, base);  // cheat dvars to their defaults, god/noclip/... off
  const std::string text = std::string(on ? "Cheats enabled" : "Cheats disabled") +
                           (mp::IsMatchRunning() ? " by the host" : "");
  if (mp::IsMatchRunning())
    mp::Announce(text);  // reaches every player's feed (and this one's)
  else
    hud::AnnouncementFeed::Push(text);
}

}  // namespace

// ---------------------------------------------------------------------------
// State
// ---------------------------------------------------------------------------
bool Enabled() { return g_enabled.load(std::memory_order_relaxed); }

void Sync(uint8_t* base) {
  const uint32_t var = ReadU32(base, a::kSvCheatsPtr);
  g_enabled.store(var != 0 && game::CvarInteger(base, var) != 0, std::memory_order_relaxed);
}

// ---------------------------------------------------------------------------
// Policy
// ---------------------------------------------------------------------------
bool IsCheatCommand(std::string_view name) {
  for (const char* c : kCheatCommands)
    if (EqualsNoCase(name, c)) return true;
  return false;
}

bool IsHostOnlyCommand(std::string_view name) {
  for (const char* c : kHostOnlyCommands)
    if (EqualsNoCase(name, c)) return true;
  return false;
}

bool IsSvCheatsName(std::string_view name) { return EqualsNoCase(name, "sv_cheats"); }

Verdict Check(const std::vector<std::string>& args, Source source, bool cheat_dvar) {
  Verdict v;
  if (args.empty()) return v;
  const std::string& cmd = args[0];
  const bool guest = mp::IsMatchRunning() && !mp::IsHost();

  // Another player's commands never run as console commands here: only the
  // match's own messages do (the multiplayer code calls Check for each
  // console-like request it gets, so a cheat or a host-only command from a
  // guest is refused before it reaches the game).
  if (source == Source::kNetwork && (IsHostOnlyCommand(cmd) || IsCheatCommand(cmd))) {
    v.allowed = false;
    v.message = "^1" + cmd + ": not allowed from another player.\n";
    return v;
  }
  if (guest && IsHostOnlyCommand(cmd)) {
    v.allowed = false;
    v.message = "^1" + cmd + ": only the host can do that in a multiplayer match.\n";
    return v;
  }
  if (IsCheatCommand(cmd) && !Enabled()) {
    v.allowed = false;
    v.message = "^1" + cmd + ": cheats are off; " + HowToEnable() + "\n";
    return v;
  }
  // The dvar named in a set-style command, or the "<dvar> <value>" form.
  size_t name_at = 0;
  for (const char* s : kSetStyle)
    if (EqualsNoCase(cmd, s)) name_at = 1;
  if (name_at >= args.size()) return v;
  const std::string& name = args[name_at];
  if (IsSvCheatsName(name)) {
    // Routed to CheatsCommand by the console; anywhere else it's refused
    // (the dvar is read-only in the game as well).
    v.allowed = false;
    v.message = "^1sv_cheats is set with the sv_cheats (or cheats) command.\n";
    return v;
  }
  const bool sets_value = args.size() > name_at + 1 || name_at == 1;  // reset / toggle change it too
  if (cheat_dvar && sets_value && !Enabled()) {
    v.allowed = false;
    v.message = "^1" + name + " is cheat protected; " + HowToEnable() + "\n";
    return v;
  }
  return v;
}

// ---------------------------------------------------------------------------
// cheats / sv_cheats
// ---------------------------------------------------------------------------
Verdict CheatsCommand(const std::vector<std::string>& args, Source source) {
  Verdict v;
  if (args.size() < 2) {
    v.message = "\"sv_cheats\" is:\"" + std::string(Enabled() ? "1" : "0") +
                "^7\" default:\"0\"\n  Cheats on or off (cheats is the same command). Single player: "
                "cheat commands and cheat-protected dvars work while it is 1, with a CHEATS USED "
                "mark. Multiplayer: only the host may change it, and everyone is told.\n";
    return v;
  }
  if (source == Source::kNetwork) {
    v.allowed = false;
    v.message = "^1sv_cheats: not allowed from another player.\n";
    return v;
  }
  if (source == Source::kConfig) {
    v.allowed = false;
    v.message = "^1sv_cheats is never read from a config file; use the console.\n";
    return v;
  }
  if (mp::IsMatchRunning() && !mp::IsHost()) {
    v.allowed = false;
    v.message = "^1sv_cheats: only the host can turn cheats on or off in a multiplayer match.\n";
    return v;
  }
  // Any value other than 0 / off / false turns them on, like the game's dvar.
  const std::string& value = args[1];
  const bool on = !(value == "0" || EqualsNoCase(value, "off") || EqualsNoCase(value, "false"));
  Engine::Get().QueueGuestTask(
      [on, source](PPCContext& ctx, uint8_t* base) { ApplySvCheats(ctx, base, on, source); });
  v.message = on ? "sv_cheats 1: cheats on. The CHEATS USED mark stays on screen until the mission is "
                   "loaded again (see help).\n"
                 : "sv_cheats 0: cheats off; cheat-protected dvars back to their defaults, god / "
                   "notarget / noclip / ufo off.\n";
  return v;
}

// ---------------------------------------------------------------------------
// Forced writes
// ---------------------------------------------------------------------------
const std::vector<ForcedDvar>& ForcedDvars() {
  // Keep this list short; each entry names the one piece of port code that
  // needs it and why force (not a plain write) is required.
  static const std::vector<ForcedDvar> list = {
      {"sv_cheats",
       "the cheats / sv_cheats command (CheatsCommand above, after the single player / host "
       "check): the dvar is read-only in the game, so only a forced write changes it"},
      {"com_maxfps",
       "the starting frame rate limit from the settings (Engine::FirstFrame, SetMaxFps): written "
       "before the game registers the dvar and whatever its flags; it is a limit, not a cheat"},
      {"timescale",
       "the level intro skip (game/cutscene_skip.h): runs the scripted intro on fast game time "
       "behind a black screen and puts the previous value back, like the game's own cutscene "
       "code; no advantage in play"},
  };
  return list;
}

bool MayForce(uint8_t* base, uint32_t cvar, std::string_view name, std::string_view value) {
  // In a multiplayer match game speed is the host's alone: no forced
  // timescale there (the intro skip is single player only).
  const bool match_speed = mp::IsMatchRunning() && EqualsNoCase(name, "timescale");
  for (const ForcedDvar& d : ForcedDvars())
    if (EqualsNoCase(name, d.name) && !match_speed) return true;
  // Back to the default is never a cheat (ResetArchivedCvars, the Cvar_Get hook).
  if (cvar) {
    if (game::CvarResetString(base, cvar) == value) return true;
  }
  return false;
}

void NoteRefusedWrite(uint8_t* base, uint32_t cvar) {
  if (!cvar || Enabled()) return;
  if (!(game::CvarFlags(base, cvar) & a::cvar_flag::kCheat)) return;
  // The same dvar at most once a second: a script or a bound key could set
  // it every frame.
  using Clock = std::chrono::steady_clock;
  static std::mutex mutex;
  static std::map<std::string, Clock::time_point> last;
  const std::string name = game::CvarName(base, cvar);
  const auto now = Clock::now();
  {
    std::lock_guard lock(mutex);
    auto it = last.find(name);
    if (it != last.end() && now - it->second < std::chrono::seconds(1)) return;
    last[name] = now;
    if (last.size() > 256) last.clear();
  }
  ui::GameConsole::Get().Print(ui::ConsoleChannel::kError,
                               "^1" + name + " is cheat protected; " + HowToEnable() + "\n");
}

}  // namespace cod3::cheats

// ---------------------------------------------------------------------------
// Hook: Cvar_Get(name, value, flags). A dvar set before the game registered
// it (a config file, or a console line typed early) is user-created until
// then and keeps its value when the game registers it. If the game registers
// it cheat-protected while cheats are off, that value is a cheat that slipped
// in before the check: back to the default, with a log line. Nothing changes
// while cheats are on, or for dvars the game created itself.
// ---------------------------------------------------------------------------
REX_EXTERN(__imp__Cvar_Get);
REX_HOOK_RAW(Cvar_Get) {
  namespace a = cod3::addr;
  namespace game = cod3::game;
  // Was the name there already, as a user-created dvar? (The name is the
  // hook's own argument, already in guest memory.)
  const uint32_t before = game::CvarFindGuest(ctx, base, ctx.r3.u32);
  const bool user_created = before && (game::CvarFlags(base, before) & a::cvar_flag::kUserCreated);
  __imp__Cvar_Get(ctx, base);
  const uint32_t var = ctx.r3.u32;
  if (!user_created || !var) return;
  if (!(game::CvarFlags(base, var) & a::cvar_flag::kCheat)) return;
  cod3::cheats::Sync(base);
  if (cod3::cheats::Enabled()) return;
  const std::string value = game::CvarString(base, var);
  const std::string reset = game::CvarResetString(base, var);
  if (value == reset) return;
  const std::string name = game::CvarName(base, var);
  // Forced, but to the default: MayForce allows exactly that. And out of the
  // console config, so it doesn't come back at the next start.
  cod3::Engine::CvarSet(ctx, base, name.c_str(), reset.c_str(), /*force=*/true);
  cod3::Engine::Get().DropArchived(name);
  REXLOG_WARN("cod3: {} was set to '{}' before the game registered it as cheat protected; back to "
              "'{}' (cheats are off)",
              name, value, reset);
  cod3::ui::GameConsole::Get().Print(
      cod3::ui::ConsoleChannel::kError,
      "^1" + name + " is cheat protected; its value '" + value + "' from a config was dropped.\n");
}
