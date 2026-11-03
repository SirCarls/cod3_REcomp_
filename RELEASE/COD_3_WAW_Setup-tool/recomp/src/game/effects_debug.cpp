// cod3 - [claude-9b] 2026-10-27 why some explosive barrels burst without flames (plan item 7): a measurement, no
// change in behavior. dev/agents/claude-9b-think/FINDINGS-2026-10-27.md section 2. Campaign only for now (the cod3mp
// functions aren't mapped yet).
//
// A destroyed barrel's effect group (0x82533978) picks one of up to 5 effect names at random and registers it with
// FX_RegisterEffect (0x824293E8), which returns 0 for an effect the level didn't load: the play then does nothing, with
// only a developer print (Com_DPrintf). An effect instance comes from a pool of 128 (0x820EB270, the "aps" effects);
// when it is empty, or the instance can't start, it returns 0 and only the game's own message ring hears of it.
// With cod3_debug_effects:
//   "cod3: effects: 'NAME' isn't loaded (FX_RegisterEffect returned 0)"   once per name
//   "cod3: effects: an effect instance wasn't created for 'NAME' (pool empty or the start failed), N so far"
//                                                                          at most once a second
//
// [claude-9b-think] 2026-11-02 UNTESTED, same cvar: barrels without flames above 60 fps (owner: "there are no flames
// at high fps. It's like it's missing its script proc"). Static work found no frame-rate dependent step on the way:
//  - a barrel is a destructible: G_Damage (0x82564888) -> 0x82412300(def r3, entity r4, f1 damage, r8 means). Health
//    at entity +776; at < 1 it plays the definition's effect group (+0x1C) through the effect request queue
//    (0x82535668 -> 0x82534400, 64 a frame, drained in CL_Frame by 0x82534318 -> 0x82533978) and spawns its pieces
//    (+0x7C names, effects +0x20). The level's script proc (_destructibles::barrel_explode, chambois 0x890A5A50,
//    started by name through the level's lookup 0x89074D30) plays sounds, radius damage and the launch; it has no
//    timed window and no effect of its own;
//  - the effect objects count down a float lifetime by CL_Frame's dt (msec * 0.001, never 0 below 1000 fps), start
//    their effect on their first update, and their requests aren't dropped (a full queue is drained at once);
//  - the effect pool (0x82A2ACCC, 2 MB) "makes room" in FX_PlayFX (0x8241B2B0): at 80 % use it kills a killable,
//    lower-priority live effect (0x82418990 picks it; the game's "killing %08X %s to make way for %s" print is
//    empty). If something adds live effects with the frame rate, the barrel's long fire is the kind it kills.
// So this measures, in one run at 60 and one uncapped at the same barrels:
//   "cod3: effects: destructible ENT destroyed (def DEF, flags F0 -> F1, health H0 -> H1): N effect requests"
//   "cod3: effects: killed 'VICTIM' to make room for 'NEW' (pool P %), N so far"        at most 20, then every 100th
//   "cod3: effects: last 10 s: live effect objects (most M), pool use (most P %), K kills, R requests"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <set>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/guest.h"

REXCVAR_DEFINE_BOOL(cod3_debug_effects, false, "cod3",
                    "Log effects the level doesn't have (each name once) and effect instances that weren't created "
                    "(the effect pool full); for barrels that burst without flames")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

namespace g = cod3::guest;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kEffectDefName = 116;  // the effect definition (0x820EB270's r3) + 116: its name
constexpr size_t kMaxNames = 512;

std::mutex g_mutex;
std::set<std::string> g_missing;
uint64_t g_failed_instances = 0;
Clock::time_point g_last_instance_log{};

bool Valid(uint32_t p) { return p >= 0x10000; }

}  // namespace

REX_EXTERN(__imp__FX_RegisterEffect);
REX_HOOK_RAW(FX_RegisterEffect) {
  const bool debug = REXCVAR_GET(cod3_debug_effects);
  const uint32_t name_ptr = ctx.r3.u32;
  std::string name;
  if (debug && Valid(name_ptr)) name = g::ReadString(base, name_ptr, 128);
  __imp__FX_RegisterEffect(ctx, base);
  if (!debug || ctx.r3.u32 != 0) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_missing.size() >= kMaxNames || !g_missing.insert(name).second) return;
  REXLOG_INFO("cod3: effects: '{}' isn't loaded (FX_RegisterEffect returned 0)", name);
}

REX_EXTERN(__imp__sub_820EB270);
REX_HOOK_RAW(sub_820EB270) {
  const bool debug = REXCVAR_GET(cod3_debug_effects);
  const uint32_t def = ctx.r3.u32;
  __imp__sub_820EB270(ctx, base);
  if (!debug || ctx.r3.u32 != 0) return;
  const std::string name = Valid(def) ? g::ReadString(base, def + kEffectDefName, 64) : std::string("?");
  std::lock_guard<std::mutex> lock(g_mutex);
  ++g_failed_instances;
  const auto now = Clock::now();
  if (now - g_last_instance_log < std::chrono::seconds(1)) return;
  g_last_instance_log = now;
  REXLOG_INFO("cod3: effects: an effect instance wasn't created for '{}' (pool empty or the start failed), {} so far",
              name, g_failed_instances);
}

// [claude-9b-think] 2026-11-02 UNTESTED: destructibles, the pool's kills and a 10 s summary (see the top).
namespace {

constexpr uint32_t kFxPool = 0x82A2ACCC;        // the effect pool's manager (0x82418C80)
constexpr uint32_t kFxPoolVtable = 0x82001378;  // intact manager (list_memory.cpp checks it too)
constexpr uint32_t kPoolTotal = 4 + 1156, kPoolUsed = 4 + 1160;  // its heap (+4): size and bytes in use
constexpr uint32_t kEntNumber = 500, kEntHealth = 776;
constexpr uint32_t kDefFlags = 0x24;
constexpr uint32_t kDefDestroyed = 0x200000;    // set when its destroyed effect has been played
constexpr uint32_t kEffectsLive = 8192;         // the effect manager (0x82534318's r3): live effect objects

thread_local bool t_in_destructible = false;
thread_local uint32_t t_requests = 0;
thread_local uint32_t t_def = 0;  // [r2] the definition 0x82412300 is handling

// [r2] 2026-11-02 the definition's effects and damage-state models (0x82412300): +0x1C the destroyed effect group,
// +0x20 the pieces' effect group, +0x30 / +0x34 / +0x38 the damage-state models (0x8240DD50 swaps to one), +0x5C
// the targets (+0x60 handles, +0x6C names count, +0x70 effect names).
constexpr uint32_t kDefGroup = 0x1C, kDefPieceGroup = 0x20, kDefStates[3] = {0x30, 0x34, 0x38};
constexpr uint32_t kDefTargets = 0x5C, kDefTargetNameCount = 0x6C, kDefTargetNames = 0x70;
uint64_t g_state_swaps = 0;

// "a, b, c" of the definition's target effect names (at most 4).
std::string TargetNames(uint8_t* base, uint32_t def) {
  const uint32_t count = std::min<uint32_t>(g::ReadU32(base, def + kDefTargetNameCount), 4);
  const uint32_t names = g::ReadU32(base, def + kDefTargetNames);
  std::string out;
  for (uint32_t i = 0; Valid(names) && i < count; ++i) {
    const uint32_t name = g::ReadU32(base, names + 4 * i);
    if (!out.empty()) out += ", ";
    out += Valid(name) ? "'" + g::ReadString(base, name, 48) + "'" : std::string("-");
  }
  return out.empty() ? std::string("none") : out;
}
uint64_t g_destroyed = 0, g_kills = 0, g_kills_10s = 0, g_requests_10s = 0;
uint32_t g_live_max = 0, g_pool_max = 0;
Clock::time_point g_summary_since = Clock::now();

// The pool's use in percent (0 when unknown or its manager is damaged).
uint32_t PoolPercent(uint8_t* base) {
  const uint32_t pool = g::ReadU32(base, kFxPool);
  if (!Valid(pool) || g::ReadU32(base, pool) != kFxPoolVtable) return 0;
  const uint32_t total = g::ReadU32(base, pool + kPoolTotal), used = g::ReadU32(base, pool + kPoolUsed);
  return total ? uint32_t(uint64_t(used) * 100 / total) : 0;
}

std::string EffectName(uint8_t* base, uint32_t def) {
  return Valid(def) ? g::ReadString(base, def + kEffectDefName, 64) : std::string("?");
}

}  // namespace

// 0x82412300(definition r3, entity r4, f1 damage, r8 means): a destructible's damage (from G_Damage).
REX_EXTERN(__imp__sub_82412300);
REX_HOOK_RAW(sub_82412300) {
  if (!REXCVAR_GET(cod3_debug_effects)) {
    __imp__sub_82412300(ctx, base);
    return;
  }
  const uint32_t def = ctx.r3.u32, ent = ctx.r4.u32;
  const uint32_t flags0 = Valid(def) ? g::ReadU32(base, def + kDefFlags) : 0;
  const int32_t health0 = Valid(ent) ? g::ReadS32(base, ent + kEntHealth) : 0;
  t_in_destructible = true;
  t_requests = 0;
  t_def = def;
  __imp__sub_82412300(ctx, base);
  t_in_destructible = false;
  t_def = 0;
  const uint32_t flags1 = Valid(def) ? g::ReadU32(base, def + kDefFlags) : 0;
  const int32_t health1 = Valid(ent) ? g::ReadS32(base, ent + kEntHealth) : 0;
  if (!((health0 >= 1 && health1 < 1) || ((flags0 ^ flags1) & kDefDestroyed))) return;
  const uint64_t n = ++g_destroyed;
  if (n <= 20 || n % 100 == 0)
    REXLOG_INFO("cod3: effects: destructible {} destroyed (def {:08X}, flags {:08X} -> {:08X}, health {} -> {}): {} "
                "effect requests; {} so far; def: group {:08X}, pieces' group {:08X}, state models {:08X} {:08X} "
                "{:08X}, {} targets (effects {})",
                Valid(ent) ? g::ReadS32(base, ent + kEntNumber) : -1, def, flags0, flags1, health0, health1,
                t_requests, n, Valid(def) ? g::ReadU32(base, def + kDefGroup) : 0,
                Valid(def) ? g::ReadU32(base, def + kDefPieceGroup) : 0,
                Valid(def) ? g::ReadU32(base, def + kDefStates[0]) : 0,
                Valid(def) ? g::ReadU32(base, def + kDefStates[1]) : 0,
                Valid(def) ? g::ReadU32(base, def + kDefStates[2]) : 0,
                Valid(def) ? g::ReadU32(base, def + kDefTargets) : 0,
                Valid(def) ? TargetNames(base, def) : std::string("none"));
}

// [r2] 0x8240DD50(entity r3, model r4): a destructible's model swapped (from 0x82412300: its damage state at 25 /
// 50 / 75 % health; also from 0x8240E088). One line per swap inside 0x82412300 (at most 50, then every 100th).
REX_EXTERN(__imp__sub_8240DD50);
REX_HOOK_RAW(sub_8240DD50) {
  if (REXCVAR_GET(cod3_debug_effects) && t_in_destructible && Valid(t_def)) {
    const uint32_t ent = ctx.r3.u32, model = ctx.r4.u32;
    int state = -1;
    for (int i = 0; i < 3; ++i)
      if (model && model == g::ReadU32(base, t_def + kDefStates[i])) state = i;
    const uint64_t n = ++g_state_swaps;
    if (n <= 50 || n % 100 == 0)
      REXLOG_INFO("cod3: effects: destructible {} damaged (def {:08X}, health {}): model {:08X} ({}), {} so far",
                  Valid(ent) ? g::ReadS32(base, ent + kEntNumber) : -1, t_def,
                  Valid(ent) ? g::ReadS32(base, ent + kEntHealth) : 0, model,
                  state == 0 ? "state +0x30" : state == 1 ? "state +0x34" : state == 2 ? "state +0x38" : "other", n);
  }
  __imp__sub_8240DD50(ctx, base);
}

// 0x82535668(out, entity, effect group, ...): an effect request for an entity.
REX_EXTERN(__imp__sub_82535668);
REX_HOOK_RAW(sub_82535668) {
  if (REXCVAR_GET(cod3_debug_effects)) {
    ++g_requests_10s;
    if (t_in_destructible) ++t_requests;
  }
  __imp__sub_82535668(ctx, base);
}

// 0x82418990(new effect's definition r3): the live effect FX_PlayFX's "make room" (0x8241B2B0) kills, or 0.
REX_EXTERN(__imp__sub_82418990);
REX_HOOK_RAW(sub_82418990) {
  const uint32_t def = ctx.r3.u32;
  __imp__sub_82418990(ctx, base);
  const uint32_t victim = ctx.r3.u32;
  if (!REXCVAR_GET(cod3_debug_effects) || !Valid(victim)) return;
  ++g_kills_10s;
  const uint64_t n = ++g_kills;
  if (n > 20 && n % 100 != 0) return;
  const uint32_t instance = g::ReadU32(base, victim + 0x1C);
  const uint32_t victim_def = Valid(instance) ? g::ReadU32(base, instance + 72) : 0;
  REXLOG_INFO("cod3: effects: killed '{}' to make room for '{}' (pool {} %), {} so far", EffectName(base, victim_def),
              EffectName(base, def), PoolPercent(base), n);
}

// 0x82534318(manager, f1 = dt): CL_Frame's effect update, once a drawn frame. The 10 s summary.
REX_EXTERN(__imp__sub_82534318);
REX_HOOK_RAW(sub_82534318) {
  if (REXCVAR_GET(cod3_debug_effects)) {
    const uint32_t manager = ctx.r3.u32;
    if (Valid(manager)) g_live_max = std::max(g_live_max, g::ReadU32(base, manager + kEffectsLive));
    g_pool_max = std::max(g_pool_max, PoolPercent(base));
    const auto now = Clock::now();
    if (now - g_summary_since >= std::chrono::seconds(10)) {
      REXLOG_INFO("cod3: effects: last 10 s: live effect objects (most {}), pool use (most {} %), {} kills, {} requests",
                  g_live_max, g_pool_max, g_kills_10s, g_requests_10s);
      g_summary_since = now;
      g_live_max = g_pool_max = 0;
      g_kills_10s = g_requests_10s = 0;
    }
  }
  __imp__sub_82534318(ctx, base);
}
