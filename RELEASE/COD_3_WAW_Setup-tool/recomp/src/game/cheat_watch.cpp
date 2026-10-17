// cod3 - "CHEATS USED" watermark (see cheat_watch.h).

#include "game/cheat_watch.h"

#include <cstring>
#include <string_view>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/addresses.h"
#include "game/cheat_guard.h"
#include "game/diagnostics.h"
#include "game/engine.h"
#include "game/fov.h"
#include "game/game_api.h"
#include "game/settings_sync.h"
#include "game/guest.h"
#if !defined(COD3_MP)  // [movement] campaign only for now (cod3mp: with the peer-to-peer glue, later)
#include "game/protected_dvars.h"
#endif
#include "game/watchdog.h"
#include "ui/game_console.h"

REXCVAR_DEFINE_BOOL(cod3_debug_cheats, false, "cod3",
                    "Log console dvar changes and cheat commands as the watermark sees them");

namespace cod3 {

namespace a = cod3::addr;
using guest::ReadString;
using guest::ReadU32;

CheatWatch& CheatWatch::Get() {
  static CheatWatch instance;
  return instance;
}

void CheatWatch::Trigger(const std::string& why) {
  history_.insert(why);
  if (!active_.exchange(true)) {
    REXLOG_INFO("cod3: cheat used ({}); watermark on", why);
    ui::GameConsole::Get().Print(
        ui::ConsoleChannel::kSystem,
        "^1Cheats used (" + why + ").^7 A CHEATS USED mark stays on screen until the mission is "
        "loaded again (a level start turns cheats off: sv_cheats 0, cheat-protected dvars back to "
        "their defaults, god/notarget/noclip/ufo off).\n");
  }
}

void CheatWatch::SetToggle(const char* name, bool on, uint32_t ent) {
  std::lock_guard lock(mutex_);
  if (ent) player_ent_ = ent;
  if (on) {
    toggles_.insert(name);
    Trigger(name);
  } else {
    toggles_.erase(name);
  }
}

void CheatWatch::OneShot(const char* name) {
  std::lock_guard lock(mutex_);
  one_shots_.insert(name);
  Trigger(name);
}

void CheatWatch::CheatDvar(const std::string& name, uint32_t cvar, bool at_default) {
  std::lock_guard lock(mutex_);
  if (at_default) {
    dvars_.erase(name);
    return;
  }
  dvars_[name] = cvar;
  Trigger(name);
}

void CheatWatch::LevelStart() {
  std::lock_guard lock(mutex_);
  // A new level gives a new player: toggles are off and given items gone.
  toggles_.clear();
  one_shots_.clear();
  player_ent_ = 0;
}

void CheatWatch::ResetCheats(PPCContext& ctx, uint8_t* base) {
  namespace c = addr::cheats;
  std::map<std::string, uint32_t> dvars;
  uint32_t ent = 0;
  {
    std::lock_guard lock(mutex_);
    dvars = dvars_;
    ent = player_ent_;
  }
  // Forced, but to the default: the policy allows exactly that. And out of
  // the console config, so it doesn't come back at the next start.
  for (const auto& [name, var] : dvars) {
    if (!var) continue;
    const std::string reset = game::CvarResetString(base, var);
    if (game::CvarString(base, var) == reset) continue;
    Engine::CvarSet(ctx, base, name.c_str(), reset.c_str(), /*force=*/true);
    Engine::Get().DropArchived(name);
    REXLOG_INFO("cod3: cheats off: {} back to '{}'", name, reset);
  }
  if (ent) {
    guest::WriteU32(base, ent + c::kEntFlags, ReadU32(base, ent + c::kEntFlags) & ~(c::kFlagGod | c::kFlagNoTarget));
    if (const uint32_t client = ReadU32(base, ent + c::kEntClient)) {
      guest::WriteU32(base, client + c::kClientNoclip, 0);
      guest::WriteU32(base, client + c::kClientUfo, 0);
    }
  }
  std::lock_guard lock(mutex_);
  toggles_.clear();
}

void CheatWatch::Update(uint8_t* base) {
  if (!Active()) return;
  std::lock_guard lock(mutex_);
  // Cheat dvars set back to their default by any route (the console,
  // `reset`, a config) no longer count.
  for (auto it = dvars_.begin(); it != dvars_.end();) {
    const uint32_t var = it->second;
    const bool at_default = !var || ReadString(base, ReadU32(base, var + a::cvar::kString), 256) ==
                                        ReadString(base, ReadU32(base, var + a::cvar::kResetString), 256);
    it = at_default ? dvars_.erase(it) : std::next(it);
  }
  const uint32_t sv_cheats = ReadU32(base, a::kSvCheatsPtr);
  const bool cheats_on = sv_cheats && guest::ReadS32(base, sv_cheats + a::cvar::kInteger) != 0;
  if (!cheats_on && dvars_.empty() && toggles_.empty() && one_shots_.empty()) {
    active_ = false;
    history_.clear();
    REXLOG_INFO("cod3: cheats reset; watermark off");
    ui::GameConsole::Get().Print(ui::ConsoleChannel::kSystem,
                                 "Cheats are all reset; the CHEATS USED mark is gone.\n");
  }
}

std::string CheatWatch::Reasons() {
  std::lock_guard lock(mutex_);
  std::string out;
  for (const auto& r : history_) out += (out.empty() ? "" : ", ") + r;
  return out;
}

}  // namespace cod3

// ---------------------------------------------------------------------------
// Hooks
// ---------------------------------------------------------------------------
namespace {
namespace c = cod3::addr::cheats;
using cod3::guest::ReadU32;

thread_local int g_cheats_ok = -1;   // CheatsOk's last answer on this thread (-1: not asked)

bool ToggleState(uint8_t* base, uint32_t ent, int which) {
  if (!ent) return false;
  if (which < 2) return (ReadU32(base, ent + c::kEntFlags) & (which == 0 ? c::kFlagGod : c::kFlagNoTarget)) != 0;
  const uint32_t client = ReadU32(base, ent + c::kEntClient);
  return client && ReadU32(base, client + (which == 2 ? c::kClientNoclip : c::kClientUfo)) != 0;
}

void Toggle(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*), int which,
            const char* name) {
  const uint32_t ent = ctx.r3.u32;
  g_cheats_ok = -1;
  original(ctx, base);
  if (g_cheats_ok == 1) cod3::CheatWatch::Get().SetToggle(name, ToggleState(base, ent, which), ent);
}

void OneShot(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*), const char* name,
             bool own_cheat_check) {
  // give runs CheatsOk; setviewpos / jumptonode test sv_cheats themselves.
  bool cheats_on = false;
  if (own_cheat_check) {
    const uint32_t vm = c::kCheatsVmCvar;
    cheats_on = cod3::guest::ReadS32(base, vm + c::kVmCvarInteger) != 0;
  }
  g_cheats_ok = -1;
  original(ctx, base);
  if (own_cheat_check ? cheats_on : g_cheats_ok == 1) cod3::CheatWatch::Get().OneShot(name);
}
}  // namespace

// CheatsOk(ent): sv_cheats on and the player alive.
REX_EXTERN(__imp__CheatsOk);
REX_HOOK_RAW(CheatsOk) {
  __imp__CheatsOk(ctx, base);
  g_cheats_ok = ctx.r3.u32 != 0 ? 1 : 0;
}

REX_EXTERN(__imp__Cmd_God_f);
REX_HOOK_RAW(Cmd_God_f) { Toggle(ctx, base, __imp__Cmd_God_f, 0, "god"); }
REX_EXTERN(__imp__Cmd_Notarget_f);
REX_HOOK_RAW(Cmd_Notarget_f) { Toggle(ctx, base, __imp__Cmd_Notarget_f, 1, "notarget"); }
REX_EXTERN(__imp__Cmd_Noclip_f);
REX_HOOK_RAW(Cmd_Noclip_f) { Toggle(ctx, base, __imp__Cmd_Noclip_f, 2, "noclip"); }
REX_EXTERN(__imp__Cmd_Ufo_f);
REX_HOOK_RAW(Cmd_Ufo_f) { Toggle(ctx, base, __imp__Cmd_Ufo_f, 3, "ufo"); }

REX_EXTERN(__imp__Cmd_Give_f);
REX_HOOK_RAW(Cmd_Give_f) { OneShot(ctx, base, __imp__Cmd_Give_f, "give", false); }
REX_EXTERN(__imp__Cmd_SetViewpos_f);
REX_HOOK_RAW(Cmd_SetViewpos_f) { OneShot(ctx, base, __imp__Cmd_SetViewpos_f, "setviewpos", true); }
REX_EXTERN(__imp__Cmd_JumpToNode_f);
REX_HOOK_RAW(Cmd_JumpToNode_f) { OneShot(ctx, base, __imp__Cmd_JumpToNode_f, "jumptonode", true); }

// Cvar_Set2(name, value, force): force = 0 is the console's route (and the
// one that refuses cheat-protected dvars while sv_cheats is 0).
REX_EXTERN(__imp__Cvar_Set2);
REX_HOOK_RAW(Cvar_Set2) {
#if !defined(COD3_MP)  // [movement]
  // A server_sided_protected dvar is the port's, never a game dvar: a
  // config, exec, a script or a menu can't write it (game/protected_dvars.h).
  if (ctx.r3.u32 >= 0x10000) {
    const char* name = reinterpret_cast<const char*>(base + ctx.r3.u32);
    if (cod3::protected_dvars::RefuseGuestWrite(std::string_view(name, strnlen(name, 65)))) {
      ctx.r3.u64 = 0;
      return;
    }
  }
#endif
  // No value means "the dvar's default" (Cvar_Reset, the console's
  // "reset <dvar>"), but this game's Cvar_Set2 reads the value before it
  // checks for that, so a reset crashed the game (the Xbox had no console to
  // type one). Hand it the default instead, as Quake 3 does; nothing for a
  // dvar that doesn't exist.
  if (ctx.r4.u32 == 0) {
    const uint32_t var = cod3::game::CvarFindGuest(ctx, base, ctx.r3.u32);
    const uint32_t reset_value = var ? ReadU32(base, var + cod3::addr::cvar::kResetString) : 0;
    if (!reset_value) {
      ctx.r3.u64 = 0;
      return;
    }
    ctx.r4.u64 = reset_value;
  }
  // The console game turns cheats on at every level start (SV_Map_f, see
  // addresses.h); the Xbox had no console to use them from. Here every level
  // starts with cheats off, and only "sv_cheats 1" in the console turns them
  // on (game/cheat_guard.h).
  const bool level_start_cheats = ctx.lr == cod3::addr::cheats::kSvMapSetsCheatsReturn;
  if (level_start_cheats) ctx.r4.u64 = cod3::addr::cheats::kStringZero;
  const bool from_console = ctx.r5.u32 == 0;
  // The value asked for, to tell a refused write from a no-op below.
  const std::string wanted = from_console ? cod3::guest::ReadString(base, ctx.r4.u32, 256) : std::string();
  __imp__Cvar_Set2(ctx, base);
  const uint32_t var = ctx.r3.u32;
  if (level_start_cheats) {
    cod3::cheats::Sync(base);
    cod3::CheatWatch::Get().ResetCheats(ctx, base);
    REXLOG_INFO("cod3: level start: cheats off (sv_cheats 0; the console game turned them on here)");
    return;
  }
  if (!from_console || !var) return;
  namespace cv = cod3::addr::cvar;
  if (REXCVAR_GET(cod3_debug_cheats)) {
    REXLOG_INFO("cod3: console set {} = '{}' (flags {:#x}, default '{}')",
                cod3::guest::ReadString(base, ReadU32(base, var + cv::kName), 128),
                cod3::guest::ReadString(base, ReadU32(base, var + cv::kString), 128),
                ReadU32(base, var + cv::kFlags),
                cod3::guest::ReadString(base, ReadU32(base, var + cv::kResetString), 128));
  }
  if (!(ReadU32(base, var + cv::kFlags) & cod3::addr::cvar_flag::kCheat)) return;
  const std::string name = cod3::guest::ReadString(base, ReadU32(base, var + cv::kName), 128);
  const std::string value = cod3::guest::ReadString(base, ReadU32(base, var + cv::kString), 256);
  const bool at_default = value == cod3::guest::ReadString(base, ReadU32(base, var + cv::kResetString), 256);
  // The game refuses the write silently while cheats are off (a Com_DPrintf):
  // say so in the console (game/cheat_guard.h).
  if (value != wanted) cod3::cheats::NoteRefusedWrite(base, var);
  cod3::CheatWatch::Get().CheatDvar(name, var, at_default);
}

// Level start: SV_SpawnServer(mapname, ...), map_restart, map_restart_internal.
// (SV_SpawnServer also times the load for the log: game/diagnostics.h.)
REX_EXTERN(__imp__SV_SpawnServer);
REX_HOOK_RAW(SV_SpawnServer) {
  cod3::watchdog::SetGameContext(&ctx);
  cod3::watchdog::Mark("SV_SpawnServer (a level load starts)");
  cod3::CheatWatch::Get().LevelStart();
  cod3::fov::OnLevelStart(ctx, base);  // the player's cg_fov / cg_fovScale again (game/fov.h)
  cod3::diag::LevelLoadBegin(cod3::guest::ReadString(base, ctx.r3.u32, 64));
  __imp__SV_SpawnServer(ctx, base);
  cod3::diag::LevelLoadEnd();
  cod3::settings::NoteLevelStart();  // the player's settings over the profile's (game/settings_sync.h)
  cod3::watchdog::Mark("SV_SpawnServer returned (the level is loaded)");
}
REX_EXTERN(__imp__SV_MapRestart_f);
REX_HOOK_RAW(SV_MapRestart_f) {
  cod3::CheatWatch::Get().LevelStart();
  cod3::fov::OnLevelStart(ctx, base);
  __imp__SV_MapRestart_f(ctx, base);
}
REX_EXTERN(__imp__SV_MapRestartInternal_f);
REX_HOOK_RAW(SV_MapRestartInternal_f) {
  cod3::CheatWatch::Get().LevelStart();
  cod3::fov::OnLevelStart(ctx, base);
  __imp__SV_MapRestartInternal_f(ctx, base);
}
