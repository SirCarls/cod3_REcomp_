// cod3mp: the match state the cheat policy asks for (game/multiplayer.h),
// answered by the multiplayer program. The campaign's game/multiplayer.cpp
// (its launch of cod3mp) isn't part of this program.
//
//  - a match runs while this machine runs a server (sv_running) or its
//    client plays in one (the client state below);
//  - who hosts the match isn't known yet: every console runs its own server
//    and only exchanges player state with the others (docs/MP_PROTOCOL.md
//    section 1), so sv_running is set on every machine of a system link
//    match. Until the Demonware session's host flag is mapped, and a way to
//    tell every player exists, no machine counts as the host during a match:
//    nobody can turn cheats on in one (game/cheat_guard.cpp), the game's
//    own sv_cheats 1 at each map load is turned off again
//    (game/cheat_watch.cpp, the Cvar_Set2 hook after SV_SpawnServer), and
//    so is any other sv_cheats 1 seen during a match (Tick);
//  - nothing from another machine is ever run here.

#include "game/mp/session.h"

#include <atomic>
#include <filesystem>
#include <string>

#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "game/addresses.h"
#include "game/cheat_guard.h"
#include "game/cheat_watch.h"
#include "game/engine.h"
#include "game/game_api.h"
#include "game/guest.h"
#include "game/multiplayer.h"
#include "ui/announcement_feed.h"
#include "ui/game_console.h"

namespace cod3::mp {

namespace {

// The client's connection state (codmp_xenonf.xex): 2 while it plays in a
// match: CL_SendCmd (0x8252CB70), CL_CreateNewCommands and
// CL_ForwardToServer_f only work then; CL_Disconnect sets 0.
constexpr uint32_t kClientState = 0x82C2DAC8;
constexpr uint32_t kClientInMatch = 2;

uint8_t* GuestBase() {
  auto* ks = rex::system::kernel_state();
  return ks && ks->memory() ? ks->memory()->virtual_membase() : nullptr;
}

bool ServerRunsHere(uint8_t* base) {
  // sv_running is registered by Com_Init; 0 until then.
  const uint32_t var = guest::ReadU32(base, addr::crossref::kSvRunningPtr);
  return var != 0 && game::CvarInteger(base, var) != 0;
}

bool ClientInMatch(uint8_t* base) { return guest::ReadU32(base, kClientState) == kClientInMatch; }

bool InMatch(uint8_t* base) { return ServerRunsHere(base) || ClientInMatch(base); }

bool g_in_match = false;
bool g_cheats = false;
std::atomic<bool> g_turning_off{false};  // a task to turn cheats off is queued

// Cheats on during a match. The console checks `sv_cheats 1` when it is
// typed and the game thread applies it at its next frame, so one typed
// while a level loads (no match yet) lands in the match. No machine is the
// host yet: turn them off again, on the game's thread, with every cheat
// dvar and toggle reset.
void TurnCheatsOff() {
  if (g_turning_off.exchange(true)) return;
  Engine::Get().QueueGuestTask([](PPCContext& ctx, uint8_t* base) {
    g_turning_off = false;
    cheats::Sync(base);
    if (!cheats::Enabled() || !InMatch(base)) return;
    Engine::CvarSet(ctx, base, "sv_cheats", "0", /*force=*/true);
    cheats::Sync(base);
    CheatWatch::Get().ResetCheats(ctx, base);
    REXLOG_WARN("cod3mp: sv_cheats was 1 during a match; turned off again (no machine is the host yet)");
    hud::AnnouncementFeed::Push("Cheats stay off in multiplayer matches");
  });
}

}  // namespace

// The campaign's launch of this program; here there is nothing to launch.
void SetGameDataRoot(const std::filesystem::path&) {}
bool MultiplayerAvailable() { return true; }

bool IsMatchRunning() {
  uint8_t* base = GuestBase();
  return base && InMatch(base);
}

// No machine is the host during a match yet (see the top of this file).
bool IsHost() { return !IsMatchRunning(); }

// Shown on this machine only (feed, console, log): there is no channel to the
// other players yet.
void Announce(const std::string& text) {
  REXLOG_INFO("cod3mp: announcement: {}", text);
  hud::AnnouncementFeed::Push(text);
  ui::GameConsole::Get().Print(ui::ConsoleChannel::kSystem, text + "\n");
}

void Tick() {
  uint8_t* base = GuestBase();
  if (!base) return;
  const bool in_match = InMatch(base);
  cheats::Sync(base);
  const bool on = cheats::Enabled();
  if (in_match != g_in_match) {
    if (in_match) {
      REXLOG_INFO("cod3mp: match started (server here {}, client in a match {}; sv_cheats {}); "
                  "cheats stay off in matches until cod3mp knows the session's host",
                  ServerRunsHere(base), ClientInMatch(base), on ? 1 : 0);
    } else {
      REXLOG_INFO("cod3mp: match ended");
    }
  } else if (in_match && on != g_cheats) {
    REXLOG_WARN("cod3mp: sv_cheats became {} during the match", on ? 1 : 0);
  }
  if (in_match && on) TurnCheatsOff();
  g_in_match = in_match;
  g_cheats = on;
}

}  // namespace cod3::mp
