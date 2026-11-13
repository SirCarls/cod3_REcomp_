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
#include <cmath>  // [acct2-main]
#include <cstdio>  // [acct2-main]
#include <cstdint>
#include <cstring>  // [acct2-main]
#include <map>  // [acct2-main] the effect events below
#include <mutex>
#include <set>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>  // [acct2-main] fx_replay's own copy of the event name

#include "game/guest.h"

REXCVAR_DEFINE_BOOL(cod3_debug_effects, false, "cod3",
                    "Log effects the level doesn't have (each name once) and effect instances that weren't created "
                    "(the effect pool full); for barrels that burst without flames")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [acct2-main] 2026-11-05 dev test of Forest's red mortar flares (needs cod3_debug_effects). The latest
// "flare_light_loop" a level script played is played again, by the same string:
//   1: on each soldier whose footstep effect plays after it (once each, up to 16 soldiers). (Run 20's red glows were
//      the level's own flares coming down again after a reload, not these.)
//   2: on the level's own flare markers, once each, when one has come down below 400 (the script moves them from 600
//      to the ground over 5 s), to tell a flare that was played too early from one that can't show on a marker.
//   3: on the level's own flare markers every 8 s from 10 s after each has come down (6 times; time to set the view
//      up first), to see whether a flare started on the ground shows at all, and for how long.
//   4: as 1 (soldiers, up to 16 each new set of markers), from 10 s after the latest markers came down: whether the
//      flare shows on the ground where nothing covers it.
//   5: as 3, and each landed marker's server origin (+336, where its effects are drawn) swayed 8 units along x
//      twice a second: whether the flare only shows while its marker moves.
//   6: as 3, and each landed marker's server origin raised 64 units: whether the spot itself hides it.
//   7: as 6 with markers 1, 2, 3 raised 2, 8, 32 units: how far below the visible the spots are.
//   8: as 3, and the particle light at the landed markers set to what a point in no cell gets (below).
REXCVAR_DEFINE_INT32(cod3_debug_fx_replay, 0, "cod3",
                     "Dev test: play the level's own flare_light_loop again: 1 on the soldiers that step after it (up "
                     "to 16), 2 on the level's flare markers once they come down, 3 on them every 8 s from 10 s "
                     "after they came down, 4 as 1 from 10 s after they came down, 5 as 3 with the markers swaying, "
                     "6 as 3 with them 64 higher, 7 with them 2 / 8 / 32 higher, 8 as 3 with no-cell light at them "
                     "(needs cod3_debug_effects)")
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

namespace {
void WatchMarkers(PPCContext& ctx, uint8_t* base);  // [acct2-main] below; cod3_debug_fx_replay 3 polls it every frame
}  // namespace

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
  // [acct2-main] modes 3 and 4 need the markers watched after the soldiers stop walking (no footsteps to hook)
  if (REXCVAR_GET(cod3_debug_effects) && REXCVAR_GET(cod3_debug_fx_replay) >= 3) WatchMarkers(ctx, base);
}

// ---------------------------------------------------------------------------------------------------------------
// [acct2-main] 2026-11-05 the level scripts' effect events, same cvar (owner 17:40: the Forest mortar's "glowing
// reticle ... where the mortar will land" is missing; 18:27 "I believe it shows on the ground"). A level script plays
// an effect on an entity by name with EffectEventPlay (0x824B3270, the level interface's + 0xB8): r3 the entity's
// handle (its low 14 bits a slot of the entity table 0x82A2DCD0, 8 bytes a slot: the entity, then the slot's
// generation, which must match the handle's high bits), r4 -> the script string (its characters at + 12), r5 nonzero:
// refuse when the entity already plays one. It returns the request's handle, or -1 when it refused. Forest's
// mortar_flare_func1-3 (forest.dll 0x89035668 / 0x89035858 / 0x89035A70) spawn a script_origin 600 high over each
// blockade target, ask for "flare_light_loop" on it (Forest defines that event at run time, as Night Drop's red flare),
// then move it down onto the target over 5 s (0x824A9EC8). Logged the first 3 times per name (at most 256 names), so a
// run shows whether a script asked at all; nothing is changed:
//   "cod3: effects: event 'NAME' on entity N -> H"
namespace {

constexpr uint32_t kEntTable = 0x82A2DCD0;
std::map<std::string, uint32_t> g_events;

}  // namespace

REX_EXTERN(__imp__sub_824B3270);  // EffectEventPlay (not in config/names.toml)
namespace {
void ReplayFlare(PPCContext& ctx, uint8_t* base, uint32_t handle, const std::string& name);
uint32_t g_flare_str = 0;    // [acct2-main] the script string of the last flare_light_loop (not its stack slot)
uint32_t g_flare_name = 0;   // [acct2-main] our own string object for it (the level's may be freed): 12 bytes, the text
uint32_t FlareName(uint8_t* base) {  // [acct2-main] the string object fx_replay passes, made once in guest memory
  if (g_flare_name) return g_flare_name;
  auto* ks = rex::system::kernel_state();
  if (!ks || !ks->memory()) return 0;
  static const char kName[] = "flare_light_loop";
  const uint32_t obj = ks->memory()->SystemHeapAlloc(12 + sizeof(kName));
  if (!Valid(obj)) return 0;
  std::memset(base + obj, 0, 12);
  std::memcpy(base + obj + 12, kName, sizeof(kName));
  return g_flare_name = obj;
}
std::set<uint32_t> g_flare_replayed;  // [acct2-main] the soldiers (handles) given a flare, at most 16
struct FlareMarker {                  // [acct2-main] the level's own flare markers, the last 3 (a reload makes new ones)
  uint32_t handle, object;
  bool replayed;
  bool down = false;                  // came below 400 (cod3_debug_fx_replay 3)
  int later = 0;                      // replays done after it came down (3), at most 6
  std::chrono::steady_clock::time_point down_at;
  float rest[3] = {0, 0, 0};          // mode 5: its server origin (+336) when it came down
};
FlareMarker g_markers[3];
int g_marker_count = 0, g_marker_next = 0, g_marker_logs = 0;
std::chrono::steady_clock::time_point g_marker_logged;
bool g_markers_down = false;  // [acct2-main] mode 4: when the latest flare markers came down
std::chrono::steady_clock::time_point g_markers_down_at;
void WatchMarkers(PPCContext& ctx, uint8_t* base);
thread_local bool t_replaying = false;
}  // namespace

REX_HOOK_RAW(sub_824B3270) {
  if (!REXCVAR_GET(cod3_debug_effects) || t_replaying) {
    __imp__sub_824B3270(ctx, base);
    return;
  }
  const uint32_t handle = ctx.r3.u32;
  const uint32_t str_ref = ctx.r4.u32;
  std::string name = "?";
  if (Valid(str_ref)) {
    const uint32_t str = g::ReadU32(base, str_ref);
    if (Valid(str)) name = g::ReadString(base, str + 12, 64);
  }
  int32_t ent = -1;
  uint32_t gent = 0;
  const uint32_t slot = handle & 0x3FFF;
  if (slot < 0x1000 && (handle >> 14) == g::ReadU32(base, kEntTable + 4 + slot * 8)) {
    gent = g::ReadU32(base, kEntTable + slot * 8);
    if (Valid(gent)) ent = g::ReadS32(base, gent + kEntNumber);
    else gent = 0;
  }
  __imp__sub_824B3270(ctx, base);
  const int32_t result = static_cast<int32_t>(ctx.r3.u32);
  if (name == "flare_light_loop") g_flare_str = g::ReadU32(base, str_ref);  // [acct2-main] fx_replay: the latest
  if (name == "flare_light_loop" && gent) {                               // [acct2-main] the markers
    std::lock_guard<std::mutex> lock(g_mutex);
    g_markers[g_marker_next++ % 3] = FlareMarker{handle, gent, false};
    if (g_marker_count < 3) ++g_marker_count;
    g_marker_logs = 0;
  }
  ReplayFlare(ctx, base, handle, name);                                   // [acct2-main]
  if (name.rfind("step_", 0) == 0) WatchMarkers(ctx, base);              // [acct2-main]
  std::lock_guard<std::mutex> lock(g_mutex);
  auto it = g_events.find(name);
  if (it == g_events.end()) {
    if (g_events.size() >= 256) return;
    it = g_events.emplace(name, 0).first;
  }
  if (++it->second > (name == "flare_light_loop" ? 12u : 3u)) return;  // [acct2-main] flares: after reloads too
  // [acct2-main] 19:40 where the object is: its address, word 0, and the first float triples in its first 0x300 bytes
  // that look like a world position (to find the mortar targets' flares).
  std::string where;
  if (gent) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), " (object %08X, word0 %08X", gent, g::ReadU32(base, gent));
    where = buf;
    int hits = 0;
    for (uint32_t off = 0; off + 12 <= 0x300 && hits < 4; off += 4) {
      const float x = g::ReadF32(base, gent + off), y = g::ReadF32(base, gent + off + 4),
                  z = g::ReadF32(base, gent + off + 8);
      if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(z)) continue;
      if (std::fabs(x) > 30000 || std::fabs(y) > 30000 || std::fabs(z) > 10000) continue;
      if (std::fabs(x) < 100 && std::fabs(y) < 100) continue;
      if (x == std::floor(x) && y == std::floor(y) && std::fabs(x) > 1e4) continue;
      std::snprintf(buf, sizeof(buf), ", +%u (%.0f %.0f %.0f)", off, x, y, z);
      where += buf;
      ++hits;
      off += 8;
    }
    where += ")";
  }
  REXLOG_INFO("cod3: effects: event '{}' on entity {} -> {}{}", name, ent, result, where);
}

// [acct2-main] cod3_debug_fx_replay: after the hook above has logged a footstep, play the flare on that soldier.
namespace {
void ReplayFlare(PPCContext& ctx, uint8_t* base, uint32_t handle, const std::string& name) {
  const int mode = REXCVAR_GET(cod3_debug_fx_replay);
  if ((mode != 1 && mode != 4) || !g_flare_str || name.rfind("step_", 0) != 0) return;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (mode == 4 &&
        (!g_markers_down || std::chrono::steady_clock::now() - g_markers_down_at < std::chrono::seconds(10)))
      return;  // mode 4: soldiers only, from 10 s after the latest markers came down
    if (g_flare_replayed.size() >= 16 || !g_flare_replayed.insert(handle).second) return;
  }
  t_replaying = true;
  g::Call c(ctx, base);
  const uint32_t slot = (c.PushString("........") + 3) & ~3u;  // the string ref the call reads, on this call's stack
  g::WriteU32(base, slot, FlareName(base));
  const uint32_t r = static_cast<uint32_t>(c.Invoke(0x824B3270, {handle, slot, 0}));
  t_replaying = false;
  REXLOG_INFO("cod3: effects: replayed 'flare_light_loop' on the stepping soldier (handle {:08X}) -> {}", handle,
              static_cast<int32_t>(r));
}

// [acct2-main] Where the level's flare markers are (twice a second while soldiers step, 60 lines after each new set), and with
// cod3_debug_fx_replay 2 the flare played again on each once it has come down below 400.
// [acct2-main] run 47: the live effects (0x82418698 updates each in the list at 0x82AA4D54 whose +50 bit 1 is clear
// and whose +20 is 0: their particle instance at +28, its origin at +48, definition at +72 (name at +116), emitters
// at +84 (count, a halfword, at +90), each emitter's definition at +200). Run 47: the markers' flares and the soldiers'
// pass alike (+20 0, +50 0x0011). Run 48: every 4 s with cod3_debug_fx_replay 3 or more, the flare at each landed
// marker and one other flare: its instance's first 128 bytes and each of its first 3 emitters' first 256 bytes, at
// most 150 lines:
//   "cod3: effects: flare at marker N (0 another) instance I at (x y z), E emitters: words"
//   "cod3: effects:   emitter K at A +0..: words"       (two lines of 32 words each)
Clock::time_point g_fx_scanned;
int g_fx_scan_logs = 0;
int NearMarker(float x, float y, float z);  // below
std::string Words(uint8_t* base, uint32_t at, uint32_t n) {
  std::string out;
  char buf[16];
  for (uint32_t k = 0; k < n; ++k) {
    std::snprintf(buf, sizeof(buf), " %08X", g::ReadU32(base, at + 4 * k));
    out += buf;
  }
  return out;
}
void ScanFlareEffects(uint8_t* base) {
  constexpr uint32_t kEffectList = 0x82AA4D54;
  const auto now = Clock::now();
  if (g_fx_scan_logs >= 150 || now - g_fx_scanned < std::chrono::seconds(4)) return;
  g_fx_scanned = now;
  const uint32_t list = g::ReadU32(base, kEffectList), count = g::ReadU32(base, kEffectList + 8);
  bool other = false;
  for (uint32_t i = 0; Valid(list) && i < count && i < 4096 && g_fx_scan_logs < 150; ++i) {
    const uint32_t e = g::ReadU32(base, list + 4 * i);
    const uint32_t inst = Valid(e) ? g::ReadU32(base, e + 28) : 0;
    const uint32_t def = Valid(inst) ? g::ReadU32(base, inst + 72) : 0;
    if (!Valid(def)) continue;
    const std::string name = g::ReadString(base, def + 116, 48);
    if (name.find("flare") == std::string::npos) continue;
    const float x = g::ReadF32(base, inst + 48), y = g::ReadF32(base, inst + 52), z = g::ReadF32(base, inst + 56);
    const int near = NearMarker(x, y, z);
    if (!near && other) continue;
    if (!near) other = true;
    const uint32_t emitters = g::ReadU32(base, inst + 84);
    const uint32_t n = std::min<uint32_t>(g::ReadU16(base, inst + 90), 3);
    ++g_fx_scan_logs;
    REXLOG_INFO("cod3: effects: flare at marker {} instance {:08X} at ({:.0f} {:.0f} {:.0f}), {} emitters:{}", near, inst,
                x, y, z, g::ReadU16(base, inst + 90), Words(base, inst, 32));
    for (uint32_t k = 0; Valid(emitters) && k < n; ++k) {
      const uint32_t em = g::ReadU32(base, emitters + 4 * k);
      if (!Valid(em)) continue;
      g_fx_scan_logs += 2;
      REXLOG_INFO("cod3: effects:   emitter {} at {:08X} +0..:{}", k, em, Words(base, em, 32));
      REXLOG_INFO("cod3: effects:   emitter {} at {:08X} +128..:{}", k, em, Words(base, em + 128, 32));
    }
  }
}

void WatchMarkers(PPCContext& ctx, uint8_t* base) {
  uint32_t replay[3];
  int n_replay = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!g_marker_count) return;
    ScanFlareEffects(base);  // [acct2-main] run 47
    const auto now = std::chrono::steady_clock::now();
    const bool log = g_marker_logs < 60 && now - g_marker_logged >= std::chrono::milliseconds(500);
    if (log) {
      g_marker_logged = now;
      ++g_marker_logs;
    }
    std::string line;
    char buf[192];
    for (int i = 0; i < g_marker_count; ++i) {
      FlareMarker& m = g_markers[i];
      const uint32_t slot = m.handle & 0x3FFF;
      const bool alive = slot < 0x1000 && (m.handle >> 14) == g::ReadU32(base, kEntTable + 4 + slot * 8) &&
                         g::ReadU32(base, kEntTable + slot * 8) == m.object;
      const float z = alive ? g::ReadF32(base, m.object + 40) : 0.0f;
      if (alive) {
        // [acct2-main] + the drawn origin (+112) and the server origin (+336) the effects and coronas use
        std::snprintf(buf, sizeof(buf), " %d (%.0f %.0f %.0f) drawn (%.0f %.0f %.0f) server (%.0f %.0f %.0f)", i + 1,
                      g::ReadF32(base, m.object + 32), g::ReadF32(base, m.object + 36), z,
                      g::ReadF32(base, m.object + 112), g::ReadF32(base, m.object + 116),
                      g::ReadF32(base, m.object + 120), g::ReadF32(base, m.object + 336),
                      g::ReadF32(base, m.object + 340), g::ReadF32(base, m.object + 344));
      } else {
        std::snprintf(buf, sizeof(buf), " %d gone", i + 1);
      }
      line += buf;
      if (alive && !m.replayed && z < 400 && REXCVAR_GET(cod3_debug_fx_replay) == 2 && g_flare_str) {
        m.replayed = true;
        replay[n_replay++] = m.handle;
      }
      if (alive && z < 400 && !m.down) {  // mode 3: when it came down, then every 8 s from 10 s
        m.down = true;
        m.down_at = now;
        for (int k = 0; k < 3; ++k) m.rest[k] = g::ReadF32(base, m.object + 336 + 4 * k);
        REXLOG_INFO("cod3: effects: flare marker {} (handle {:08X}) is down", i + 1, m.handle);
        if (!g_markers_down || now - g_markers_down_at > std::chrono::seconds(5)) g_flare_replayed.clear();  // new set
        g_markers_down = true;
        g_markers_down_at = now;
      }
      if (alive && m.down && REXCVAR_GET(cod3_debug_fx_replay) == 5) {  // mode 5: sway it 8 units along x, 2 Hz
        const float t = std::chrono::duration<float>(now - m.down_at).count();
        g::WriteF32(base, m.object + 336, m.rest[0] + 8.0f * std::sin(t * 12.566f));
      }
      if (alive && m.down && REXCVAR_GET(cod3_debug_fx_replay) == 6)  // mode 6: 64 units higher
        g::WriteF32(base, m.object + 344, m.rest[2] + 64.0f);
      if (alive && m.down && REXCVAR_GET(cod3_debug_fx_replay) == 7)  // mode 7: markers 1-3 raised 2 / 8 / 32
        g::WriteF32(base, m.object + 344, m.rest[2] + static_cast<float>(2 << (2 * i)));
      if (alive && m.down && m.later < 6 && REXCVAR_GET(cod3_debug_fx_replay) >= 3 &&
          REXCVAR_GET(cod3_debug_fx_replay) != 4 && g_flare_str &&  // modes 3, 5, 6, 7
          now - m.down_at >= std::chrono::seconds(10 + 8 * m.later)) {
        ++m.later;
        replay[n_replay++] = m.handle;
      }
    }
    if (log) REXLOG_INFO("cod3: effects: flare markers:{}", line);
  }
  for (int i = 0; i < n_replay; ++i) {
    t_replaying = true;
    g::Call c(ctx, base);
    const uint32_t slot = (c.PushString("........") + 3) & ~3u;
    g::WriteU32(base, slot, FlareName(base));
    const uint32_t r = static_cast<uint32_t>(c.Invoke(0x824B3270, {replay[i], slot, 0}));
    t_replaying = false;
    REXLOG_INFO("cod3: effects: replayed 'flare_light_loop' on the flare marker (handle {:08X}) -> {}", replay[i],
                static_cast<int32_t>(r));
  }
}
}  // namespace

// [acct2-main] 2026-11-05 run 44: the light the particle system gets at a landed flare marker. ApsGameClient__vfunc_2
// (0x824244A8: r4 -> the point, r5 -> 3 vectors out) is its ask for the light at a point: the cell 0.01 above the
// point, else 50.01 above it (0x8241E330, R_CellForPoint 0x8241C0F8 under it); in a cell, the light grid's
// (0x82422030); in none, (0 0 0 0) (0 0 0 0) (1 1 1 1). With cod3_debug_fx_replay 3 or more, logged for points within
// 16 units of a landed marker (up to 96 above it), at most twice a second per marker, 120 lines:
//   "cod3: effects: light at marker N (x y z): (a b c d) (e f g h) (i j k l)"
// cod3_debug_fx_replay 8 (as 3) then gives those points the no-cell answer: whether that light hides the flare.
namespace {
Clock::time_point g_light_logged[3];
int g_light_logs = 0;
thread_local uint32_t g_light_sp = 0, g_light_lr = 0;  // [acct2-main] run 46: the asker's stack, return
std::set<uint64_t> g_light_chains;
uint64_t g_light_asks = 0;  // all of them, logged every 10 s ("cod3: effects: particle light asked N times")
Clock::time_point g_light_since;

// [acct2-main] run 45: which point is near a landed marker (0 when none), under g_mutex.
int NearMarker(float x, float y, float z) {
  for (int i = 0; i < g_marker_count; ++i) {
    const FlareMarker& m = g_markers[i];
    if (m.down && std::fabs(x - m.rest[0]) <= 16 && std::fabs(y - m.rest[1]) <= 16 && z >= m.rest[2] - 16 &&
        z <= m.rest[2] + 96)
      return i + 1;
  }
  return 0;
}
}  // namespace

REX_EXTERN(__imp__ApsGameClient__vfunc_2);
REX_HOOK_RAW(ApsGameClient__vfunc_2) {
  const uint32_t pt = ctx.r4.u32, out = ctx.r5.u32;
  g_light_sp = ctx.r1.u32;
  g_light_lr = static_cast<uint32_t>(ctx.lr);
  __imp__ApsGameClient__vfunc_2(ctx, base);
  const int mode = REXCVAR_GET(cod3_debug_fx_replay);
  if (!REXCVAR_GET(cod3_debug_effects) || mode < 3 || !Valid(pt) || !Valid(out)) return;
  const float x = g::ReadF32(base, pt), y = g::ReadF32(base, pt + 4), z = g::ReadF32(base, pt + 8);
  std::lock_guard<std::mutex> lock(g_mutex);
  ++g_light_asks;
  {  // [acct2-main] run 46: who asks (4 return addresses up the stack), each distinct chain once, at most 16
    uint32_t chain[4] = {g_light_lr, 0, 0, 0};
    uint32_t sp = g_light_sp;
    for (int k = 1; k < 4 && Valid(sp); ++k) {
      sp = g::ReadU32(base, sp);
      chain[k] = Valid(sp) ? g::ReadU32(base, sp - 8) : 0;
    }
    const uint64_t key = (uint64_t{chain[0]} << 32) | chain[1] ^ (uint64_t{chain[2]} << 16) ^ chain[3];
    if (g_light_chains.size() < 16 && g_light_chains.insert(key).second)
      REXLOG_INFO("cod3: effects: particle light asked from {:08X} {:08X} {:08X} {:08X} at ({:.0f} {:.0f} {:.0f})",
                  chain[0], chain[1], chain[2], chain[3], x, y, z);
  }
  if (g_marker_count && Clock::now() - g_light_since >= std::chrono::seconds(10)) {
    g_light_since = Clock::now();
    REXLOG_INFO("cod3: effects: particle light asked {} times", g_light_asks);
  }
  for (int i = 0; i < g_marker_count; ++i) {
    const FlareMarker& m = g_markers[i];
    if (!m.down || std::fabs(x - m.rest[0]) > 16 || std::fabs(y - m.rest[1]) > 16 || z < m.rest[2] - 16 ||
        z > m.rest[2] + 96)
      continue;
    const auto now = Clock::now();
    if (g_light_logs < 120 && now - g_light_logged[i] >= std::chrono::milliseconds(500)) {
      g_light_logged[i] = now;
      ++g_light_logs;
      float v[12];
      for (int k = 0; k < 12; ++k) v[k] = g::ReadF32(base, out + 4 * k);
      REXLOG_INFO("cod3: effects: light at marker {} ({:.1f} {:.1f} {:.1f}): ({:.3f} {:.3f} {:.3f} {:.3f}) ({:.3f} "
                  "{:.3f} {:.3f} {:.3f}) ({:.3f} {:.3f} {:.3f} {:.3f})",
                  i + 1, x, y, z, v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9], v[10], v[11]);
    }
    if (mode == 8)
      for (int k = 0; k < 12; ++k) g::WriteF32(base, out + 4 * k, k < 8 ? 0.0f : 1.0f);
    break;
  }
}

// [acct2-main] run 45: who asks for the cell of a point at a landed marker. R_CellForPoint (0x8241C0F8, r3 -> the
// point: the BSP's leaf, its cell or -1) and 0x8241E330 (r4 -> the point, r5 -> the cell out: the cell 0.01 above it,
// else 50.01 above; the light grid, or 0 in no cell). Each distinct caller (and its caller) with a point near a landed
// marker (as above) is logged once, at most 16:
//   "cod3: effects: cell (R_CellForPoint | 0x8241E330) at marker N (x y z) asked from LR (from LR2): result"
namespace {
std::set<uint64_t> g_cell_callers;
void CellAsked(uint8_t* base, const char* what, uint32_t pt, uint32_t lr, uint32_t sp, int32_t result) {
  if (!REXCVAR_GET(cod3_debug_effects) || REXCVAR_GET(cod3_debug_fx_replay) < 3 || !Valid(pt)) return;
  const float x = g::ReadF32(base, pt), y = g::ReadF32(base, pt + 4), z = g::ReadF32(base, pt + 8);
  std::lock_guard<std::mutex> lock(g_mutex);
  const int n = NearMarker(x, y, z);
  if (!n) return;
  const uint32_t back = g::ReadU32(base, sp);
  const uint32_t lr2 = Valid(back) ? g::ReadU32(base, back - 8) : 0;
  if (g_cell_callers.size() >= 16 || !g_cell_callers.insert((uint64_t{lr} << 32) | lr2).second) return;
  REXLOG_INFO("cod3: effects: cell ({}) at marker {} ({:.2f} {:.2f} {:.2f}) asked from {:08X} (from {:08X}): {}", what,
              n, x, y, z, lr, lr2, result);
}
}  // namespace

REX_EXTERN(__imp__sub_8241C0F8);
REX_HOOK_RAW(sub_8241C0F8) {
  const uint32_t pt = ctx.r3.u32, sp = ctx.r1.u32, lr = static_cast<uint32_t>(ctx.lr);
  __imp__sub_8241C0F8(ctx, base);
  CellAsked(base, "R_CellForPoint", pt, lr, sp, static_cast<int32_t>(ctx.r3.u32));
}

REX_EXTERN(__imp__sub_8241E330);
REX_HOOK_RAW(sub_8241E330) {
  const uint32_t pt = ctx.r4.u32, cell = ctx.r5.u32, sp = ctx.r1.u32, lr = static_cast<uint32_t>(ctx.lr);
  __imp__sub_8241E330(ctx, base);
  CellAsked(base, "0x8241E330", pt, lr, sp, Valid(cell) ? g::ReadS32(base, cell) : -2);
}
