// cod3 - the "server_sided_protected" dvar category.
//
// A dvar in this category is the match's, not the player's: the server's
// host (its admin) sets it, and while a player is in a match the server's
// value applies on that player's machine. Nothing on the player's machine
// can change it during the match, neither for the server nor for that
// player's own game. Outside a match (the campaign, the menus) it is a
// normal setting. This is separate from sv_cheats and cheat protection
// (game/cheat_guard.h): a protected dvar is never a cheat and is not shown in
// the CHEATS USED mark or the cheats announcement column.
//
// Each dvar keeps two values:
//   - the player's own value: a port setting (cod3.toml), changed only when
//     no match runs, or by the host for the host's own match;
//   - the server's value: memory only, set from the server's settings while
//     this machine is a guest in a match, dropped when the player leaves or
//     quits. The own value was never touched, so it is back by construction.
// All port code reads the effective value through Get().
//
// The list is kEntries in protected_dvars.cpp (one place); adding a dvar is
// one entry there plus its port setting. docs/MOVEMENT.md explains the rule,
// every path that could change a value and what each says.
//
// Paths and what stops them during a match (on a guest's machine):
//   the port's console (set, seta, sets, setu, reset, toggle, togglep,
//     setfromcvar, "<name> <value>", World at War aliases), cod3_autoexec:
//     HandleConsoleLine (ui/game_console.cpp);
//   config files, exec, guest scripts and the game's menus: they reach the
//     game's Cvar_Set2, where RefuseGuestWrite refuses the name always (a
//     protected dvar is never a guest dvar) (game/cheat_watch.cpp);
//   F2 and the game's Options menus: the rows ask IsLocked and show the
//     server's value, locked;
//   any port code, the command line and cod3.toml: the port settings refuse
//     a change while locked (their validator), and the command line and the
//     file are read only at start, before any match;
//   another player's commands: refused always (cheat_guard.cpp).
// The server never takes these values from a client: they travel server ->
// client only, in the server's systeminfo string (ServerInfoPairs /
// ApplyServerInfo, cod3mp's glue).

#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

#include "game/cheat_guard.h"

namespace cod3::protected_dvars {

enum class Id : int {
  kAlternateMovement,        // 0 CoD3 for everyone, 1 CoD4 for everyone, 2 each player's choice
  kAlternateMovementSprint,  // under 1: the CoD4 parts for everyone
  kAlternateMovementSpeeds,
  kAlternateMovementMantle,
  kAlternateMovementJumps,
  kCount
};
inline constexpr int kCount = static_cast<int>(Id::kCount);

struct Entry {
  Id id;
  const char* name;
  int min, max;          // an integer domain (0..1 for a switch)
  int default_value;     // this program's default for the player's own value
  int match_default;     // in a match whose server didn't send a value
  const char* help;      // one sentence
};
const Entry& Info(Id id);
const std::vector<Entry>& All();
// Case-insensitive; also a World at War alias whose target is protected.
bool Find(std::string_view name, Id* out);

// --- values (any thread) -------------------------------------------------------
int Get(Id id);   // the effective value
int Own(Id id);   // the player's own (saved) value
bool ServerSet(Id id);  // the server's value applies now
// Bumps whenever an effective value or the match state may have changed.
uint32_t Generation();

// --- the match ---------------------------------------------------------------------
struct Match {
  bool running = false;  // a multiplayer match runs on this machine
  bool host = true;      // this machine hosts it (or there is no match)
  bool test = false;     // cod3_test_fake_match
};
Match CurrentMatch();
// A match runs and this machine is a guest: the values are the server's.
bool IsLocked();
// The text locked rows and refusals show.
std::string LockedReason(Id id);

// --- changing the player's own value (any thread) ----------------------------------
struct Verdict {
  bool allowed = true;
  bool changed = false;
  std::string message;  // for the console, ends with '\n' (empty: nothing to say)
};
// The only setter of own values: the console, F2 and the game's menus. Strict:
// an integer within the entry's domain.
Verdict RequestSet(Id id, std::string_view value, cheats::Source source);
Verdict RequestReset(Id id, cheats::Source source);

// A console line naming a protected dvar, in any form (see the top of this
// file). Returns false when the line isn't about one; otherwise prints what
// to say through `print(error, text)` and sets `*changed` when an own value
// changed (the caller saves the settings).
bool HandleConsoleLine(const std::vector<std::string>& args, cheats::Source source,
                       const std::function<void(bool error, const std::string& text)>& print, bool* changed);

// The game's Cvar_Set2 (config files, exec, scripts, menus): true = refuse,
// with a message and a log line (rate limited). Always for a protected name.
bool RefuseGuestWrite(std::string_view name);

// --- the multiplayer glue (cod3mp; the test match below uses the same calls) -------
// A guest's machine: the server's systeminfo string arrived or changed. Takes
// only the listed keys, each checked against its domain (anything else is
// dropped with a log line). Returns false for a malformed string.
bool ApplyServerInfo(std::string_view systeminfo);
// The guest left the match (disconnect, the match ended, quit): the server's
// values are dropped and the own values apply again.
void LeaveMatch();
// The host: the pairs to add to its systeminfo ("\alternate_movement\1...").
std::string ServerInfoPairs();
// The host: how a change is told to every player (cod3mp sends the game's own
// "print" server command; a normal server message, not the cheats column).
// Without one (the campaign) the line goes to this machine's console only.
void SetServerMessageSender(std::function<void(const std::string& text)> send);

// --- per frame (game thread) --------------------------------------------------------
// Runs the test match (cod3_test_fake_match) and tells about changes.
void Tick();

// The hook for settings saved from the game's menus or the console (Cod3App).
void SetSaveHandler(std::function<void()> save);
void SaveSettings();

}  // namespace cod3::protected_dvars
