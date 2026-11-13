// cod3mp - the touches after a move, 60 a game second per player, whatever the frame rate.
//
// [acct2-ocr] 2026-11-05, owner 19:05: "Ok please start working on MP" (after the campaign's fix in
// game/movement/hooks_sp.cpp, owner 18:24 / 18:33: edge detection and rubbing against the AI were frame
// dependent). The multiplayer program's ClientThink_real (0x82592CD0) runs once per usercmd, so once per drawn
// frame of the player who sent it, and after Pmove and the entity link it calls the same two functions as the
// campaign's:
//   0x823B6BA0 (returns to 0x825933F4; r3 the player's entity, r4 its client, r5 = r1+144) = campaign 0x823D0FE8:
//              the player's box against the area's triggers and entities, calling their touch functions;
//   0x82565B80 (returns to 0x8259346C; r3 the entity, r4 how many, r5 the list) = campaign 0x825459B8. Pmove's
//              pmove is at r1+176 there, so the count is pm+132 and the list pm+136 (the campaign: pm at r1+144):
//              the touch functions of the entities Pmove ran into (32-bit entity handles; the function skips a
//              handle whose entity is gone, and touches each once).
// So a player at 250 fps touched triggers, and whatever he rubbed against, 250 times a second, at 30 fps 30 times.
// cod3mp moves a player once per usercmd (no fixed movement steps, unlike the campaign's cod3_move_hz), so here the
// count is the 1/60 s marks of the game clock the usercmd's time passed since that player's last usercmd: none:
// both are left out, and the entities Pmove ran into are kept for the next run (at most 16 in a list, so a short
// bump still touches once); N: both run N times (at most 6) with the same arguments. At 60 fps a usercmd passes one
// mark, as before. It is the host's code: it applies to every player in a match this program hosts.
// Everything below is cod3mp-only (inside COD3_MP, so tools/mp_aliases.py treats the hooks as this program's own).

#include <algorithm>
#include <array>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/guest.h"

#if defined(COD3_MP)

REXCVAR_DEFINE_BOOL(cod3_move_touch_per_step, true, "cod3",
                    "The players' touches after a move (triggers, the entities they run into or rub against) run 60 "
                    "times a game second, whatever each player's frame rate (as at 60 fps); off: once per usercmd "
                    "(at 250 fps 250 times a second). Host only: applies to every player in a match hosted here");
REXCVAR_DEFINE_BOOL(cod3_debug_move_touch, false, "cod3",
                    "Log the touches after the move every 2 game seconds: usercmds, the calls asked and run, the "
                    "touch lists kept for later and their entities added to the next run");

namespace {
namespace g = cod3::guest;

constexpr uint32_t kAreaTouchReturn = 0x825933F4;  // ClientThink_real, after its 0x823B6BA0 call
constexpr uint32_t kTouchListReturn = 0x8259346C;  // ClientThink_real, after its 0x82565B80 call
constexpr uint32_t kClientOfEntity = 512;          // gentity_t -> gclient_t, whose playerState comes first
constexpr uint32_t kCommandTime = 32;              // playerState commandTime (program_mp.h): Pmove left the usercmd's time
constexpr int kTickHz = 60;
constexpr int kMaxRuns = 6;
constexpr int kMaxList = 16;  // entities kept for the next run, and the most a list grows to with them

bool ValidGuest(uint32_t p) { return p >= 0x10000 && (p & 3) == 0; }

// Per player (keyed by the client, entity +512): the usercmd time last counted, the runs of the usercmd being run
// (both calls of one usercmd use the same count) and the entities kept from usercmds that ran none.
struct PlayerPace {
  uint32_t client = 0;
  bool have_time = false;
  int32_t last_time = 0;
  bool cached = false;
  int32_t cached_time = 0;
  int cached_runs = 1;
  int pending = 0;
  std::array<uint32_t, kMaxList> kept = {};
};
std::array<PlayerPace, 64> g_players;

PlayerPace& PlayerFor(uint32_t client) {
  PlayerPace* empty = nullptr;
  for (PlayerPace& p : g_players) {
    if (p.client == client) return p;
    if (p.client == 0 && empty == nullptr) empty = &p;
  }
  PlayerPace& p = empty != nullptr ? *empty : g_players[(client >> 4) % g_players.size()];
  p = PlayerPace{};
  p.client = client;
  return p;
}

// How many times this usercmd's touches run: the 1/60 s marks between the player's last usercmd time and this one.
int RunsFor(PlayerPace& p, int32_t time, bool consume) {
  if (!p.cached || p.cached_time != time) {
    int runs = 1;
    if (p.have_time && time >= p.last_time) {
      const int64_t from = static_cast<int64_t>(p.last_time) * kTickHz / 1000;
      const int64_t to = static_cast<int64_t>(time) * kTickHz / 1000;
      runs = static_cast<int>(std::min<int64_t>(to - from, kMaxRuns));
    }
    p.have_time = true;
    p.last_time = time;
    p.cached = true;
    p.cached_time = time;
    p.cached_runs = runs;
  }
  const int runs = p.cached_runs;
  if (consume) p.cached = false;
  return runs;
}

// Runs `call` n times, with the caller's argument registers each time (a call leaves them changed).
template <typename F>
void RunTimes(PPCContext& ctx, int n, F&& call) {
  const PPCRegister args[8] = {ctx.r3, ctx.r4, ctx.r5, ctx.r6, ctx.r7, ctx.r8, ctx.r9, ctx.r10};
  for (int i = 0; i < n; ++i) {
    if (i > 0) {
      ctx.r3 = args[0]; ctx.r4 = args[1]; ctx.r5 = args[2]; ctx.r6 = args[3];
      ctx.r7 = args[4]; ctx.r8 = args[5]; ctx.r9 = args[6]; ctx.r10 = args[7];
    }
    call();
  }
}

// cod3_debug_move_touch: counts since the last line (all players).
struct TouchLog {
  bool have = false;
  int32_t start = 0;
  uint64_t cmds = 0, ticks = 0, area_asked = 0, area_run = 0, list_asked = 0, list_run = 0, list_full = 0;
  uint64_t lists_kept = 0, ents_added = 0;
};
TouchLog g_log;

void LogTick(int32_t time) {
  if (!REXCVAR_GET(cod3_debug_move_touch)) {
    g_log.have = false;
    return;
  }
  TouchLog& l = g_log;
  if (!l.have || time < l.start) {
    l = TouchLog{};
    l.have = true;
    l.start = time;
    return;
  }
  if (time - l.start < 2000) return;
  REXLOG_INFO("cod3mp: touches after the move over {} game ms (cod3_move_touch_per_step {}): {} usercmds, {} ticks; "
              "area touches (0x823B6BA0) {} asked, {} run; touch lists (0x82565B80) {} asked, {} run, {} with "
              "entities; {} lists kept for the next run, {} entities added to a run",
              time - l.start, REXCVAR_GET(cod3_move_touch_per_step) ? "on" : "off", l.cmds, l.ticks, l.area_asked,
              l.area_run, l.list_asked, l.list_run, l.list_full, l.lists_kept, l.ents_added);
  const int32_t start = time;
  l = TouchLog{};
  l.have = true;
  l.start = start;
}

// The usercmd's time: Pmove has run, so the playerState's commandTime is it.
int32_t UsercmdTime(uint8_t* base, uint32_t client) { return g::ReadS32(base, client + kCommandTime); }
}  // namespace

REX_EXTERN(__imp__sub_823B6BA0);
REX_HOOK_RAW(sub_823B6BA0) {
  const uint32_t ent = ctx.r3.u32;
  const uint32_t client = ValidGuest(ent) ? g::ReadU32(base, ent + kClientOfEntity) : 0;
  if (static_cast<uint32_t>(ctx.lr) != kAreaTouchReturn || !ValidGuest(client)) {
    __imp__sub_823B6BA0(ctx, base);
    return;
  }
  const int32_t time = UsercmdTime(base, client);
  const int runs = RunsFor(PlayerFor(client), time, false);
  const int n = REXCVAR_GET(cod3_move_touch_per_step) ? runs : 1;
  LogTick(time);
  if (g_log.have) {
    ++g_log.cmds;
    g_log.ticks += static_cast<uint64_t>(runs);
    ++g_log.area_asked;
    g_log.area_run += static_cast<uint64_t>(n);
  }
  RunTimes(ctx, n, [&] { __imp__sub_823B6BA0(ctx, base); });
}

REX_EXTERN(__imp__sub_82565B80);
REX_HOOK_RAW(sub_82565B80) {
  const uint32_t ent = ctx.r3.u32;
  const uint32_t client = ValidGuest(ent) ? g::ReadU32(base, ent + kClientOfEntity) : 0;
  const uint32_t list = ctx.r5.u32;  // pm + 136, ClientThink_real's stack (room for 32)
  if (static_cast<uint32_t>(ctx.lr) != kTouchListReturn || !ValidGuest(client) || !ValidGuest(list)) {
    __imp__sub_82565B80(ctx, base);
    return;
  }
  const int32_t time = UsercmdTime(base, client);
  PlayerPace& p = PlayerFor(client);
  const int runs = RunsFor(p, time, true);
  int32_t count = static_cast<int32_t>(ctx.r4.u32);
  if (count < 0 || count > 64) count = 0;  // not a list we know: leave it to the game
  if (g_log.have) {
    ++g_log.list_asked;
    if (count > 0) ++g_log.list_full;
  }
  if (!REXCVAR_GET(cod3_move_touch_per_step)) {
    p.pending = 0;
    if (g_log.have) ++g_log.list_run;
    __imp__sub_82565B80(ctx, base);
    return;
  }
  if (runs == 0) {
    // No 1/60 s mark in this usercmd: keep what Pmove ran into for the next run (each handle once).
    bool kept_any = false;
    for (int32_t i = 0; i < count && p.pending < kMaxList; ++i) {
      const uint32_t h = g::ReadU32(base, list + 4 * static_cast<uint32_t>(i));
      if (std::find(p.kept.begin(), p.kept.begin() + p.pending, h) == p.kept.begin() + p.pending) {
        p.kept[static_cast<size_t>(p.pending++)] = h;
        kept_any = true;
      }
    }
    if (kept_any && g_log.have) ++g_log.lists_kept;
    return;
  }
  // A run: add the kept entities this list doesn't have (up to kMaxList in all), then run it `runs` times.
  if (p.pending > 0) {
    int32_t added = 0;
    for (int k = 0; k < p.pending && count < kMaxList; ++k) {
      const uint32_t h = p.kept[static_cast<size_t>(k)];
      bool have = false;
      for (int32_t i = 0; i < count && !have; ++i) have = g::ReadU32(base, list + 4 * static_cast<uint32_t>(i)) == h;
      if (have) continue;
      g::WriteU32(base, list + 4 * static_cast<uint32_t>(count), h);
      ++count;
      ++added;
    }
    p.pending = 0;
    ctx.r4.u64 = static_cast<uint32_t>(count);
    if (g_log.have) g_log.ents_added += static_cast<uint64_t>(added);
  }
  if (g_log.have) g_log.list_run += static_cast<uint64_t>(runs);
  RunTimes(ctx, runs, [&] { __imp__sub_82565B80(ctx, base); });
}

#endif  // COD3_MP
