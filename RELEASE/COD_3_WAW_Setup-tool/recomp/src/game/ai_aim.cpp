// cod3 - [claude-9b-think] 2026-11-02 UNTESTED: the AI's shots at the player above 60 fps (campaign only).
//
// ob20 (owner 15:03, both rates in one run): hits on the player ~0.4-0.5 a second at 60 fps, ~0 uncapped, with the
// AI aiming about as often. How an actor's shot at the player is decided (default.xex):
//   0x823ED9B8 (ActorCmd_Shoot / ShootAt, f1 = the command's accuracy; once per level.time per sentient)
//     -> 0x8251EA58(0.75 x actor +0x8AC, [0x82A2A1A8], actor, enemy): may it fire at all
//     -> 0x823DBAC8: chance = 0x823DA738(distance) x f1 (min 0.02 for the first factor), kept at actor +0x108;
//        hit when rand()/32768 < chance. A hit aims at 0x823D5308(enemy) (origin +0x150 plus the view height
//        client +0xBC, plus lean) through 0x823C7DD8 (a random side offset, and a height offset from client
//        +1084); a miss goes through 0x823D7028 (the miss offset) and counts at actor +0x100.
//   0x823DA738(distance, actor, target): the actor's accuracy (actor +0xF4 % against a player, +0xF8 otherwise)
//     with the weapon's near/far band (+0x854/+0x858); against a player, mixed by 0x823D8828 (the player's sideways
//     speed: his position now against a 2-entry history 0x82A2A0A8 / times 0x82A2A0C0, index 0x82A2A0A4); then x
//     0.1 / 0.5 / 0.8 / 1 for how many of his eye, middle and feet are visible (0x823D8018), x the stance table
//     (client +0x2C), x the difficulty (0x829B1B7C).
// Which inputs follow the usercmds rather than the updates:
//   - the history: 0x823D7E18, called from Pmove (0x82496978, after its moves) for every usercmd that moves, adds
//     a sample when level.time is more than [0x829B0B3C] ms past the last one, with level.time as its time. At
//     60 fps the one usercmd between two updates records where the player is when the next update runs. Above
//     60 fps the first usercmd after an update records it and the 2-3 after it don't: the sample is one update
//     early, so the speed the AI reads spans up to two updates of movement in the time of one (about 1.7x).
//     In 0x823DA738 that speed only weighs beyond the weapon's far range (inside it both terms are equal), so this
//     alone shouldn't take hits to 0;
//   - the rest (distance, visibility, stance, view height) read the player's latest state in both cases.
// Not found statically: what makes the hits drop. So this:
//   - cod3_fix_ai_player_history (default on): while the newest sample has this update's level.time, every later
//     Pmove of the same update records it again (the game's own code, into the same slot), so it holds where the
//     player is when the next update runs, as at 60 fps. At 60 fps (one usercmd an update) it never acts. Same
//     for every actor; MP has no actors (cod3mp isn't touched);
//   - with cod3_debug_ai_census, a third line every 10 s: decisions at the player, hits decided, the mean chance
//     and how many were <= 0, and of the shots decided as hits / misses, how many lowered his health. If decided
//     hits stay but don't lower his health uncapped, the bullet's trace against the player is the cause, not the
//     chance.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/ai_shots.h"  // [claude-9b-think] 2026-11-02 cod3_debug_ai_shots
#include "game/guest.h"

REXCVAR_DECLARE(bool, cod3_debug_ai_census);  // ai_census.cpp

// [claude-9b] 2026-11-02 sync: off by default in the project folder for now (not confirmed on its own; the AI fix
// that was confirmed in runs is cod3_move_keep_box, movement/hooks_sp.cpp).
REXCVAR_DEFINE_BOOL(cod3_fix_ai_player_history, false, "cod3/Game",
                    "Above 60 fps the AI reads the player's movement over the same time as at 60 fps (its sample of "
                    "where he was is taken when the next game update starts); off: the game's own sample, one "
                    "update early above 60 fps")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

namespace g = cod3::guest;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kLevelTime = 0x82A4E790 + 156;  // level.time (ms)
constexpr uint32_t kHistoryLast = 0x82A2A0A0;      // level.time of the newest sample
constexpr uint32_t kHistoryIndex = 0x82A2A0A4;     // the newest sample's slot (0 / 1)
constexpr uint32_t kHistoryInterval = 0x829B0B3C;  // ms between samples
constexpr uint32_t kGentityTable = 0x82A2AB40, kActiveLocalClient = 0x82A2A1C8;
constexpr uint32_t kEntHealth = 776;
constexpr uint32_t kActorChance = 0x108;           // 0x823DBAC8 keeps the chance it drew against here
constexpr uint32_t kActorSentient = 4, kSentientTarget = 0x68;

std::atomic<uint64_t> g_refreshes{0};
uint64_t g_decisions = 0, g_hits_decided = 0, g_chance_zero = 0, g_hit_hurt = 0, g_miss_hurt = 0;
double g_chance_sum = 0.0;
Clock::time_point g_since = Clock::now();
int t_decision = -1;  // main thread: 0x823DBAC8's answer within the current 0x823ED9B8, -1 none

uint32_t PlayerEntity(uint8_t* base) {
  const uint32_t table = g::ReadU32(base, kGentityTable);
  const uint32_t client = g::ReadU32(base, kActiveLocalClient);
  if (table < 0x10000 || client > 3) return 0;
  return g::ReadU32(base, table + 4 * (client + 1));
}

// The actor's target is the player (its sentient's target sentient's entity).
bool TargetsPlayer(uint8_t* base, uint32_t actor, uint32_t player) {
  if (actor < 0x10000 || !player) return false;
  const uint32_t sentient = g::ReadU32(base, actor + kActorSentient);
  if (sentient < 0x10000) return false;
  const uint32_t target = g::ReadU32(base, sentient + kSentientTarget);
  return target >= 0x10000 && g::ReadU32(base, target) == player;
}

void MaybeLog() {
  const auto now = Clock::now();
  const double seconds = std::chrono::duration<double>(now - g_since).count();
  if (seconds < 10.0) return;
  REXLOG_INFO("cod3: ai census 3: a second: decisions at the player {:.2f}, decided hits {:.2f} (mean chance {:.3f}, "
              "{:.2f} at or below 0), shots that hurt him: decided hits {:.2f}, decided misses {:.2f}; history "
              "samples taken again {:.1f}",
              g_decisions / seconds, g_hits_decided / seconds, g_decisions ? g_chance_sum / g_decisions : 0.0,
              g_chance_zero / seconds, g_hit_hurt / seconds, g_miss_hurt / seconds,
              double(g_refreshes.exchange(0)) / seconds);
  g_decisions = g_hits_decided = g_chance_zero = g_hit_hurt = g_miss_hurt = 0;
  g_chance_sum = 0.0;
  g_since = now;
}

}  // namespace

// 0x823D7E18(pmove): Pmove's sample of the player's position for the AI.
REX_EXTERN(__imp__sub_823D7E18);
REX_HOOK_RAW(sub_823D7E18) {
  // [claude-9b-think] 2026-11-02 [r3] same_update / refreshed for cod3_debug_ai_accuracy (ai_shots.cpp).
  const int32_t now = g::ReadS32(base, kLevelTime);
  const bool same_update = now != 0 && g::ReadS32(base, kHistoryLast) == now;
  bool refreshed = false;
  if (REXCVAR_GET(cod3_fix_ai_player_history)) {
    const int32_t interval = g::ReadS32(base, kHistoryInterval);
    const uint32_t index = g::ReadU32(base, kHistoryIndex);
    if (same_update && interval >= 0 && index <= 1) {
      // The newest sample is this update's: let the game's code take it again into the same slot (it steps the
      // index once and stores level.time as the sample's time and the newest time).
      g::WriteU32(base, kHistoryIndex, index ^ 1u);
      g::WriteU32(base, kHistoryLast, uint32_t(now - interval - 1));
      g_refreshes.fetch_add(1, std::memory_order_relaxed);
      refreshed = true;
    }
  }
  __imp__sub_823D7E18(ctx, base);
  cod3::ai_shots::OnPlayerSample(base, same_update, refreshed);
}

// 0x823DBAC8(f1 accuracy, actor): hit (1) or miss (0) for the actor's shot at its target.
REX_EXTERN(__imp__sub_823DBAC8);
REX_HOOK_RAW(sub_823DBAC8) {
  const uint32_t actor = ctx.r3.u32;
  __imp__sub_823DBAC8(ctx, base);
  cod3::ai_shots::OnHitOrMiss(base, actor, ctx.r3.u32);  // [claude-9b-think] 2026-11-02 cod3_debug_ai_shots
  if (!REXCVAR_GET(cod3_debug_ai_census) || !TargetsPlayer(base, actor, PlayerEntity(base))) return;
  t_decision = ctx.r3.u32 != 0 ? 1 : 0;
  const float chance = g::ReadF32(base, actor + kActorChance);
  ++g_decisions;
  if (t_decision) ++g_hits_decided;
  g_chance_sum += chance;
  if (!(chance > 0.0f)) ++g_chance_zero;
}

// 0x823ED9B8(actor, f1): the actor's shot (decision, aim, Bullet_Fire): did the player's health go down in it.
REX_EXTERN(__imp__sub_823ED9B8);
REX_HOOK_RAW(sub_823ED9B8) {
  // [claude-9b-think] 2026-11-02 [r3] cod3_debug_ai_accuracy: the command's outcome (ai_shots.cpp).
  const cod3::ai_shots::ShootScope shoot_scope(ctx, base);
  if (!REXCVAR_GET(cod3_debug_ai_census)) {
    __imp__sub_823ED9B8(ctx, base);
    return;
  }
  const uint32_t player = PlayerEntity(base);
  const int32_t health0 = player ? g::ReadS32(base, player + kEntHealth) : 0;
  t_decision = -1;
  __imp__sub_823ED9B8(ctx, base);
  const int32_t health1 = player ? g::ReadS32(base, player + kEntHealth) : 0;
  if (health1 < health0) {
    if (t_decision == 1) ++g_hit_hurt;
    else if (t_decision == 0) ++g_miss_hurt;
  }
  t_decision = -1;
  MaybeLog();
}
