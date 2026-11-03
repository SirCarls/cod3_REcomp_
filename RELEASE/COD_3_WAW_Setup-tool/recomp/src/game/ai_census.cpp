// cod3 - [claude-9b-think] 2026-11-02 UNTESTED cod3_debug_ai_census (campaign). Owner 11-02: "AI not shooting above
// 60 fps", "spawn rates tied to fps in almost every level". Counts only, nothing changes.
//
// What static work found (default.xex, the generated code):
//  - Script threads run once per game update: SV_Frame calls the scheduler 0x824BB6F8(f1 = msec * 0.001), which runs
//    ScriptThread_Run. Their waits count down there: Broc_Wait's AeThreadWaitState (0x824999C0) subtracts the frame
//    time block's seconds (0x825F4E08 + 12), which the server step sets to the update's time while the update runs
//    (RunSystems); Broc_WaitFrames' AeThreadWaitFramesState (0x824981F8) subtracts 1 per check, i.e. per update. So
//    level and AI scripts are at 60 updates a second at any frame rate: the cause is outside them.
//  - The AI's bullet: ActorCmd_Shoot / ShootAt (0x824B8FF8, 0x824B9088, level slot commands next to ActorCmd_Melee)
//    -> 0x823ED9B8 -> Bullet_Fire 0x82567A58. Spawns: Broc_Spawn (level slot 0x264) and SpawnActor 0x823ED058.
//  - What runs once per drawn frame and touches game state: the player's usercmd (ClientThink_real: his move, his
//    triggers, his weapon), the client game, the frame's systems when cod3_server_step_systems is off.
//
// This logs, every 10 s while on: game updates, drawn frames and usercmds a second, and per second the AI's shoot
// commands, the AI's bullets (Bullet_Fire with an attacker other than the player), the player's bullets, script
// spawns and actor spawns. A run at 60 fps and one uncapped at the same spot (the same checkpoint, 60 s each) show
// which of them scales with the frame rate: that one's caller is where the fix goes.
//
// [claude-9b-think] 2026-11-02 v2. ob19's census: shoot commands and spawns don't follow the frame rate, only the
// usercmds do. So the question is whom the AI aims at and what it knows of the player, which may change once per
// usercmd. A second line, same cvar, same 10 s:
//  - a second: AI bullets, those aimed at the player (the shot's direction within 4 degrees of a point on the
//    player's feet-to-eye line, from the muzzle) and those that hit him (G_Damage on the player from inside an AI
//    Bullet_Fire); actor damage to the player (G_Damage 0x82564888 with the player as target, another attacker:
//    times and points); traces that end at the player (within 24 units of his feet-to-eye line, starting away from
//    him) from actor code (0x823C0000..0x823FFFFF) and from other code (SV_Trace 0x82516E28); and actor-updates with
//    the player as enemy (once per update, each sentient whose target, sentient +0x68, is the player's; with any
//    enemy in brackets);
//  - per game update (the scheduler 0x824BB6F8, once an update): the player's links (SV_LinkEntity with his
//    entity: ClientThink_real links him after each usercmd), the actor push queries (0x823D0FE8, from
//    ClientThink_real: CM_AreaEntities around the player, then 0x8254E318 / 0x8254E618), the entity touch pass's
//    queries (0x82549240, from 0x825505A0 and 0x82562118: a G_TouchTriggers candidate, not confirmed) and the
//    other CM_AreaEntities calls.
// At 60 fps each per-update number is the console's; one that grows about 3.3 times at 200 fps runs per usercmd.
// If "traces from actor code" stays near 0 in both runs, the AI's sight uses another trace than SV_Trace.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/ai_shots.h"  // [claude-9b-think] 2026-11-02 cod3_debug_ai_shots
#include "game/guest.h"

REXCVAR_DEFINE_BOOL(cod3_debug_ai_census, false, "cod3",
                    "Log every 10 s how many AI shoot commands, AI and player bullets, script spawns and actor "
                    "spawns happen a second, with the game updates, frames and usercmds a second (to compare 60 fps "
                    "with an uncapped run); and the AI's aim at the player, its hits and damage, traces ending at "
                    "the player, actors with the player as enemy, and what runs per game update")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

namespace g = cod3::guest;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kGentityTable = 0x82A2AB40;       // *(it) + 4 * (client + 1): the player's entity
constexpr uint32_t kActiveLocalClient = 0x82A2A1C8;  // the player index the game uses
constexpr uint32_t kLevel = 0x82A4E790;
constexpr uint32_t kLevelTime = kLevel + 156;        // level.time (ms)
// [v2] The sentients: level +8 points at 33 of 336 bytes (0x823C11F8 / 0x823C1170 walk them): +0 the entity, +156
// >= 0 while in use, +0x68 the sentient it targets (0x823ED9B8 aims at that one's entity).
constexpr uint32_t kLevelSentients = kLevel + 8;
constexpr uint32_t kSentientCount = 33, kSentientSize = 336;
constexpr uint32_t kSentientInUse = 156, kSentientTarget = 0x68;
constexpr uint32_t kEntOrigin = 336;                 // gentity_t: the server's current origin (server_step.cpp)
constexpr uint32_t kCgBase = 0x82A98D90, kCgSize = 5840, kCgViewOrigin = 4480;  // the player's eye (refdef)
// Bullet_Fire(attacker r3, spread f1 / f2, damage r5, weaponParms* r6, weapon entity r7); weaponParms +0 the unit
// forward, +0x24 the muzzle (0x823ED9B8 builds them for an actor).
constexpr uint32_t kWpForward = 0, kWpMuzzle = 0x24;
constexpr float kAimCos = 0.99756405f;  // cos 4 degrees
constexpr float kNearPlayer = 24.0f;
constexpr uint32_t kActorCodeStart = 0x823C0000, kActorCodeEnd = 0x82400000;
// CM_AreaEntities' callers (return addresses).
constexpr uint32_t kAreaFromActorPush = 0x823D109C;  // in 0x823D0FE8
constexpr uint32_t kAreaFromTouchPass = 0x82549428;  // in 0x82549240
constexpr uint32_t kAreaFromItself = 0x82476104;     // its own recursion

struct Census {
  std::atomic<uint64_t> shoot_cmds{0}, ai_bullets{0}, player_bullets{0}, script_spawns{0}, actor_spawns{0};
  std::atomic<uint64_t> usercmds{0};
  // v2
  std::atomic<uint64_t> ai_aimed{0}, ai_hits{0}, damage_times{0}, damage_points{0};
  std::atomic<uint64_t> traces_actor{0}, traces_other{0}, enemy_player{0}, enemy_any{0};
  std::atomic<uint64_t> updates{0}, player_links{0}, area_push{0}, area_touch{0}, area_other{0};
};
Census g_census;
Clock::time_point g_since = Clock::now();
uint32_t g_level_time_since = 0;
thread_local int t_in_ai_bullet = 0;

uint32_t PlayerEntity(uint8_t* base) {
  const uint32_t table = g::ReadU32(base, kGentityTable);
  const uint32_t client = g::ReadU32(base, kActiveLocalClient);
  if (table < 0x10000 || client > 3) return 0;
  return g::ReadU32(base, table + 4 * (client + 1));
}

bool On() { return REXCVAR_GET(cod3_debug_ai_census); }

struct V3 {
  float x, y, z;
};
V3 ReadV3(uint8_t* base, uint32_t a) { return {g::ReadF32(base, a), g::ReadF32(base, a + 4), g::ReadF32(base, a + 8)}; }
V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 Lerp(V3 a, V3 b, float t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

// The player's feet (his entity's origin) and eye (the client game's view origin; 60 units up when that is far
// from him: a camera of a cutscene or a vehicle).
bool PlayerBody(uint8_t* base, uint32_t player, V3* feet, V3* eye) {
  if (!player) return false;
  const uint32_t client = g::ReadU32(base, kActiveLocalClient);
  if (client > 3) return false;
  *feet = ReadV3(base, player + kEntOrigin);
  *eye = ReadV3(base, kCgBase + client * kCgSize + kCgViewOrigin);
  const V3 d = Sub(*eye, *feet);
  if (!(Dot(d, d) < 200.0f * 200.0f)) *eye = {feet->x, feet->y, feet->z + 60.0f};
  return true;
}

// The distance from `p` to the player's feet-to-eye line.
float DistanceToBody(V3 p, V3 feet, V3 eye) {
  const V3 axis = Sub(eye, feet), rel = Sub(p, feet);
  const float len2 = Dot(axis, axis);
  const float t = len2 > 1e-3f ? std::clamp(Dot(rel, axis) / len2, 0.0f, 1.0f) : 0.0f;
  const V3 d = Sub(p, Lerp(feet, eye, t));
  return std::sqrt(Dot(d, d));
}

// The shot from `wp` (weaponParms) points within 4 degrees of a point of the player's feet-to-eye line.
bool AimedAtPlayer(uint8_t* base, uint32_t wp, V3 feet, V3 eye) {
  V3 forward = ReadV3(base, wp + kWpForward);
  const float flen = std::sqrt(Dot(forward, forward));
  if (!(flen > 0.5f && flen < 2.0f)) return false;
  forward = {forward.x / flen, forward.y / flen, forward.z / flen};
  const V3 muzzle = ReadV3(base, wp + kWpMuzzle);
  for (int i = 0; i <= 4; ++i) {
    const V3 v = Sub(Lerp(feet, eye, float(i) * 0.25f), muzzle);
    const float len = std::sqrt(Dot(v, v));
    if (len < 1.0f || Dot(forward, v) >= kAimCos * len) return true;
  }
  return false;
}

// Once every 10 s, from the usercmd hook (once per drawn frame).
void MaybeLog(uint8_t* base) {
  const auto now = Clock::now();
  const double seconds = std::chrono::duration<double>(now - g_since).count();
  if (seconds < 10.0) return;
  const uint32_t level_time = g::ReadU32(base, kLevelTime);
  const double game_seconds = double(level_time - g_level_time_since) * 0.001;
  auto take = [](std::atomic<uint64_t>& c) { return double(c.exchange(0)); };
  const double cmds = take(g_census.usercmds), shoot = take(g_census.shoot_cmds), ai = take(g_census.ai_bullets);
  const double pl = take(g_census.player_bullets), sspawn = take(g_census.script_spawns);
  const double aspawn = take(g_census.actor_spawns);
  const double aimed = take(g_census.ai_aimed), hits = take(g_census.ai_hits);
  const double dmg_times = take(g_census.damage_times), dmg_points = take(g_census.damage_points);
  const double tr_actor = take(g_census.traces_actor), tr_other = take(g_census.traces_other);
  const double en_player = take(g_census.enemy_player), en_any = take(g_census.enemy_any);
  const double updates = take(g_census.updates), links = take(g_census.player_links);
  const double push = take(g_census.area_push), touch = take(g_census.area_touch), other = take(g_census.area_other);
  REXLOG_INFO("cod3: ai census over {:.1f} s (game time {:.1f} s): {:.0f} usercmds a second; a second: AI shoot "
              "commands {:.1f}, AI bullets {:.1f}, player bullets {:.1f}, script spawns {:.2f}, actor spawns {:.2f}",
              seconds, game_seconds, cmds / seconds, shoot / seconds, ai / seconds, pl / seconds, sspawn / seconds,
              aspawn / seconds);
  const double per_update = updates > 0.0 ? 1.0 / updates : 0.0;
  REXLOG_INFO("cod3: ai census 2: {:.0f} game updates, {:.2f} usercmds an update; a second: AI bullets {:.1f} (aimed "
              "at the player {:.1f}, hit him {:.1f}), actor damage to the player {:.2f} times / {:.1f} points, traces "
              "ending at the player {:.1f} from actor code / {:.1f} other, actor-updates with the player as enemy "
              "{:.1f} (any enemy {:.1f}); an update: player links {:.2f}, actor push queries (0x823D0FE8) {:.2f}, "
              "touch pass queries (0x82549240) {:.2f}, other area queries {:.2f}",
              updates, cmds * per_update, ai / seconds, aimed / seconds, hits / seconds, dmg_times / seconds,
              dmg_points / seconds, tr_actor / seconds, tr_other / seconds, en_player / seconds, en_any / seconds,
              links * per_update, push * per_update, touch * per_update, other * per_update);
  g_since = now;
  g_level_time_since = level_time;
}

}  // namespace

// ActorCmd_Shoot / ShootAt (both end in 0x823ED9B8).
REX_EXTERN(__imp__sub_824B8FF8);
REX_HOOK_RAW(sub_824B8FF8) {
  if (On()) g_census.shoot_cmds.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_824B8FF8(ctx, base);
}
REX_EXTERN(__imp__sub_824B9088);
REX_HOOK_RAW(sub_824B9088) {
  if (On()) g_census.shoot_cmds.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_824B9088(ctx, base);
}

// Bullet_Fire(attacker r3, spread f1 / f2, damage r5, weaponParms* r6, weapon entity r7).
REX_EXTERN(__imp__sub_82567A58);
REX_HOOK_RAW(sub_82567A58) {
  // [claude-9b-think] 2026-11-02 cod3_debug_ai_shots: the shot's stack, aim and trace (ai_shots.cpp), around the
  // whole call whatever this census does.
  const cod3::ai_shots::BulletScope shot_scope(ctx, base);
  if (!On()) {
    __imp__sub_82567A58(ctx, base);
    return;
  }
  const uint32_t attacker = ctx.r3.u32, wp = ctx.r6.u32;
  const uint32_t player = PlayerEntity(base);
  if (attacker && attacker == player) {
    g_census.player_bullets.fetch_add(1, std::memory_order_relaxed);
    __imp__sub_82567A58(ctx, base);
    return;
  }
  g_census.ai_bullets.fetch_add(1, std::memory_order_relaxed);
  V3 feet{}, eye{};
  if (wp && PlayerBody(base, player, &feet, &eye) && AimedAtPlayer(base, wp, feet, eye))
    g_census.ai_aimed.fetch_add(1, std::memory_order_relaxed);
  ++t_in_ai_bullet;
  __imp__sub_82567A58(ctx, base);
  --t_in_ai_bullet;
}

// [v2] G_Damage(target r3, inflictor r4, attacker r5, dir r6, point r7, damage r8, flags r9, means r10): it counts
// the player's damage by others itself (0x829C2920).
REX_EXTERN(__imp__sub_82564888);
REX_HOOK_RAW(sub_82564888) {
  cod3::ai_shots::OnDamage(base, ctx.r3.u32);  // [claude-9b-think] 2026-11-02 [r1] did the followed shot hurt him
  if (On()) {
    const uint32_t target = ctx.r3.u32, attacker = ctx.r5.u32;
    const uint32_t player = PlayerEntity(base);
    if (target && target == player && attacker && attacker != player) {
      g_census.damage_times.fetch_add(1, std::memory_order_relaxed);
      g_census.damage_points.fetch_add(uint64_t(std::max<int32_t>(ctx.r8.s32, 0)), std::memory_order_relaxed);
      if (t_in_ai_bullet > 0) g_census.ai_hits.fetch_add(1, std::memory_order_relaxed);
    }
  }
  __imp__sub_82564888(ctx, base);
}

// [v2] SV_Trace(results r3, start r4, mins r5, maxs r6, end r7, ...) (G_RunItem's call): a trace that ends at the
// player and starts away from him.
REX_EXTERN(__imp__SV_Trace);
REX_HOOK_RAW(SV_Trace) {
  if (On() && ctx.r4.u32 && ctx.r7.u32) {
    V3 feet{}, eye{};
    if (PlayerBody(base, PlayerEntity(base), &feet, &eye) &&
        DistanceToBody(ReadV3(base, ctx.r7.u32), feet, eye) <= kNearPlayer &&
        DistanceToBody(ReadV3(base, ctx.r4.u32), feet, eye) > kNearPlayer) {
      const uint32_t lr = uint32_t(ctx.lr);
      (lr >= kActorCodeStart && lr < kActorCodeEnd ? g_census.traces_actor : g_census.traces_other)
          .fetch_add(1, std::memory_order_relaxed);
    }
  }
  __imp__SV_Trace(ctx, base);
}

// [v2] SV_LinkEntity(entity r3): the player's links.
REX_EXTERN(__imp__SV_LinkEntity);
REX_HOOK_RAW(SV_LinkEntity) {
  if (On() && ctx.r3.u32 && ctx.r3.u32 == PlayerEntity(base))
    g_census.player_links.fetch_add(1, std::memory_order_relaxed);
  __imp__SV_LinkEntity(ctx, base);
}

// [v2] CM_AreaEntities, by caller.
REX_EXTERN(__imp__CM_AreaEntities);
REX_HOOK_RAW(CM_AreaEntities) {
  if (On()) {
    const uint32_t lr = uint32_t(ctx.lr);
    if (lr == kAreaFromActorPush)
      g_census.area_push.fetch_add(1, std::memory_order_relaxed);
    else if (lr == kAreaFromTouchPass)
      g_census.area_touch.fetch_add(1, std::memory_order_relaxed);
    else if (lr != kAreaFromItself)
      g_census.area_other.fetch_add(1, std::memory_order_relaxed);
  }
  __imp__CM_AreaEntities(ctx, base);
}

// [v2] The script scheduler (SV_Frame, once a game update): the update counted, and the sentients that target the
// player.
REX_EXTERN(__imp__sub_824BB6F8);
REX_HOOK_RAW(sub_824BB6F8) {
  cod3::ai_shots::EachUpdate(base);  // [claude-9b-think] 2026-11-02 [r2] cod3_debug_ai_spawns: the sentients by team
  if (On()) {
    g_census.updates.fetch_add(1, std::memory_order_relaxed);
    const uint32_t player = PlayerEntity(base);
    const uint32_t first = g::ReadU32(base, kLevelSentients);
    const uint32_t end = first + kSentientCount * kSentientSize;
    if (player && first >= 0x10000) {
      uint64_t any = 0, at_player = 0;
      for (uint32_t s = first; s < end; s += kSentientSize) {
        if (g::ReadS32(base, s + kSentientInUse) < 0) continue;
        const uint32_t entity = g::ReadU32(base, s);
        if (!entity || entity == player) continue;
        const uint32_t target = g::ReadU32(base, s + kSentientTarget);
        if (target < first || target >= end) continue;  // none (0), or not a sentient
        ++any;
        if (g::ReadU32(base, target) == player) ++at_player;
      }
      g_census.enemy_any.fetch_add(any, std::memory_order_relaxed);
      g_census.enemy_player.fetch_add(at_player, std::memory_order_relaxed);
    }
  }
  __imp__sub_824BB6F8(ctx, base);
}

// Broc_Spawn: a level script's spawn.
REX_EXTERN(__imp__Broc_Spawn);
REX_HOOK_RAW(Broc_Spawn) {
  if (On()) g_census.script_spawns.fetch_add(1, std::memory_order_relaxed);
  __imp__Broc_Spawn(ctx, base);
}

// SpawnActor: an AI from a spawner.
REX_EXTERN(__imp__sub_823ED058);
REX_HOOK_RAW(sub_823ED058) {
  if (On()) g_census.actor_spawns.fetch_add(1, std::memory_order_relaxed);
  __imp__sub_823ED058(ctx, base);
}

// ClientThink_real: one usercmd (one a drawn frame).
REX_EXTERN(__imp__ClientThink_real);
REX_HOOK_RAW(ClientThink_real) {
  // [claude-9b-think] 2026-11-02 cod3_debug_ai_shots: its 10 s line; [r1] and what runs inside a usercmd.
  const cod3::ai_shots::UsercmdScope usercmd_scope(base);
  if (On()) {
    g_census.usercmds.fetch_add(1, std::memory_order_relaxed);
    MaybeLog(base);
  }
  __imp__ClientThink_real(ctx, base);
}
