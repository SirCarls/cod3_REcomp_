// cod3 - one place that decides what may touch cheats.
//
// The game's own rule (Cvar_Set2 at 0x8252B850, confirmed in the generated
// code): a write with force = 0 is refused for a read-only, write-protected
// or, while sv_cheats is 0, cheat-protected (CVAR_CHEAT) dvar; force = 1
// skips every check. sv_cheats itself is read-only (flags 0x48), so only a
// forced write changes it: the game's `devmap` / `map`, and the port. The
// cheat commands (give, god, noclip, notarget, ufo, setviewpos, jumptonode)
// test the cheats dvar themselves (CheatsOk).
//
// The port adds, for every source (its console, cod3_autoexec, the console
// config userdata/cod3_config.cfg, the F2 settings and, later, the network):
//   - `cheats` and `sv_cheats` are the same command and the only way the
//     port turns cheats on or off (CheatsCommand). In a multiplayer match only
//     the host may use it, and the change is announced to everyone
//     (game/multiplayer.h: IsMatchRunning, IsHost, Announce).
//   - A cheat command or a cheat-protected dvar is refused with a plain
//     message while cheats are off (Check), instead of the game's silent
//     refusal (its "is cheat protected" is a Com_DPrintf, shown only with
//     developer 1).
//   - A cheat-protected dvar coming from a config file is dropped with a log
//     line (Engine::FirstFrame), and one that was set before the game
//     registered it is put back to its default (the Cvar_Get hook).
//   - The port itself writes dvars with force only from a short allow-list
//     (MayForce), each with a reason in cheat_guard.cpp.
// Nothing here changes the game while sv_cheats is 1 in single player:
// cheats work as before and the CHEATS USED mark shows (cheat_watch.h).

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

struct PPCContext;

namespace cod3::cheats {

// Where a request comes from (for the message and the policy).
enum class Source {
  kConsole,   // typed in the port's console
  kAutoexec,  // --cod3_autoexec / cod3.toml
  kConfig,    // userdata/cod3_config.cfg
  kSettings,  // the F2 window
  kNetwork,   // another player (multiplayer, later)
};

// The answer to a request. `message` is ready for the console (ends with
// '\n') when the request is refused.
struct Verdict {
  bool allowed = true;
  std::string message;
};

// --- state (any thread) ------------------------------------------------------
// sv_cheats as the game sees it, mirrored once per game frame (Sync).
bool Enabled();
// Guest main thread, once per frame (Engine::OnGuestFrame).
void Sync(uint8_t* base);

// --- policy (any thread) -----------------------------------------------------
// The game's cheat commands (CheatsOk or their own sv_cheats test).
bool IsCheatCommand(std::string_view name);
// Commands only the host of a multiplayer match may run (they start, restart
// or end the match, or turn cheats on): map, spmap, devmap, map_restart,
// fast_restart, reboot, load_mp_exe, cheats / sv_cheats ...
bool IsHostOnlyCommand(std::string_view name);
// Whether a tokenized command line may run from `source` now. Looks at the
// command name and, for set / seta / toggle / reset / "<dvar> <value>", the
// dvar's flags (`cheat_dvar` says whether the dvar named in it is
// cheat-protected, when the caller knows; the game checks again anyway).
Verdict Check(const std::vector<std::string>& args, Source source, bool cheat_dvar);
// Whether `name` is sv_cheats (any case), the dvar no config may carry.
bool IsSvCheatsName(std::string_view name);

// --- the cheats command (any thread; the change runs on the guest thread) ----
// `cheats [0|1]` / `sv_cheats [0|1]`: with no value, reports the state; else
// asks for the change (queued on the game's main thread). Returns what to
// print now.
Verdict CheatsCommand(const std::vector<std::string>& args, Source source);

// --- forced writes (guest main thread) ---------------------------------------
// Whether the port may write `name` = `value` with force (Engine::CvarSet):
// the allow-list in cheat_guard.cpp, or a value equal to the dvar's reset
// string (a reset is never a cheat). `cvar` is the cvar_t* or 0.
bool MayForce(uint8_t* base, uint32_t cvar, std::string_view name, std::string_view value);

// A cheat-protected dvar write from the console (Cvar_Set2 with force = 0)
// that the game refused while cheats are off: tells the player in the
// console (rate limited). Called by the Cvar_Set2 hook in cheat_watch.cpp.
void NoteRefusedWrite(uint8_t* base, uint32_t cvar);

// The dvars the port writes with force, with the reason (for the notes and
// the `cheats` help).
struct ForcedDvar {
  const char* name;
  const char* why;
};
const std::vector<ForcedDvar>& ForcedDvars();

}  // namespace cod3::cheats
