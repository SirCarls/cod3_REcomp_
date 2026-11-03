// cod3 - [claude-9b-think] 2026-11-02 UNTESTED cod3_debug_ai_shots (campaign only, default off). Runtime evidence
// for "the AI misses almost always above 60 fps" (owner, ob21). Logs, changes nothing.
//
// ob21: census 3 saw no hit-or-miss decision (0x823DBAC8) against the player in any window while the AI fired ~5
// aimed shots a second at him. Static reading since (default.xex, generated code): 0x823ED9B8 (ActorCmd_Shoot /
// ShootAt), after 0x8251EA58 allows the shot, picks one of two aim models by [0x829B62CC] (a setting at
// 0x829B62C0 + 12):
//   == 1: 0x823DB830(actor, f1 accuracy, ..., r8 its eye out, r9 the aim point out): no hit-or-miss roll. If
//         level.time - actor +268 < 1000 ms it aims at the point stored at actor +272..+280, else at the target's
//         eye (0x823D5308) lowered by a constant; then 0x823DA738 (accuracy by distance) and 0x824A27D8 shape the
//         shot around that point;
//   else: 0x823DBAC8 (the hit-or-miss roll census 3 counted).
// ob22 (owner 16:20): [0x829B62CC] = 1; every AI shot is 0x823DB830 -> Bullet_Fire from sub_823ED9B8+0x328, called
// from level scripts through ActorCmd_Shoot (sub_824B8FF8+0x70 <- level code <- sub_824A6150+0x158).
//
// With cod3_debug_ai_shots on, for each Bullet_Fire (0x82567A58) by anyone but the player with the player within
// 2000 units of the muzzle, the first 30 of a level and then every 50th are logged:
//   - the guest call stack (the return address into Bullet_Fire's caller, then the frames' saved ones, named as the
//     crash log names them: watchdog::GuestStack);
//   - the shooter (entity, number +500) and its distance to his eye;
//   - what decided it in this update for this shooter: 0x823DB830 (the point it aimed at, its distance from his eye
//     and chest, the stored point's age) or 0x823DBAC8 (hit / miss, chance);
//   - the angle between the shot and the lines from the muzzle to his eye and to his chest (3/4 of the way from his
//     feet to his eye): the aim direction (weaponParms +0) and the traced one (with the spread);
//   - the bullet trace (0x82550498 from Bullet_Fire_Extended at 0x82565530; results +0 the end point).
// Every 10 s: calls a second of 0x823DB830 and 0x823DBAC8 (any target / the player), the AI bullets within 2000 of
// him and how many hurt him, Bullet_Fire's callers, and [0x829B62CC].
//
// [r1] 2026-11-02 UNTESTED, after ob22:
//  - the trace result: Bullet_Fire_Extended reads +0..+8 as the end point (twice, for the impact) and +16..+24 as
//    the normal; ob22's "+32 fraction / +48 entity" read 0.008-0.097 / #0 on almost every shot, so those two are no
//    longer used. "Hit him" is now G_Damage on the player during the followed shot (ai_census.cpp's hook). The raw
//    words +32..+60 are printed with the first 3 logged shots of a level, to settle the layout later;
//  - a 10 s line for the shots AT THE PLAYER only (0x823DB830's target is his entity): count, how many used the
//    stored point, the mean and the most the aimed point was from his eye, the stored point's mean age when used,
//    the mean aim error to his chest, the traces that ended within 16 units of his feet-to-eye line, and the shots
//    that hurt him;
//  - the stored point's writer. Static search (stores to an actor's +268 and +272..+280 together) finds
//    sub_824AE908(r3 entity handle, r4 a position, ..., f1): it resolves the handle (0x82A2DAD0 + 512 + 8 x number,
//    serial check), takes the entity's actor (entity +532) and, in one branch, writes actor +268 = a time read as
//    [x +156] (level.time if x is the level, unconfirmed) and actor +272..+280 = one of up to N candidate points
//    (a 16-byte array at r1+992, picked by index). It has no direct caller in default.xex (an indirect call:
//    likely the level scripts' engine table). Its callees are in the path-node code (0x8246xxxx-0x8247xxxx). What
//    the candidates are built from, and how often it runs, is not known statically. So it is hooked: every call is
//    counted (inside a usercmd or not), and a call that changes the actor's +268 / +272..+280 is a "write", logged
//    (first 20 a level, then every 50th, for an actor whose target is the player or a point within 300 units of
//    him) with: the stack (first 5), r4's distance from his feet and eye, the new time against level.time, the new
//    point's distance from r4, his feet and his eye, and whether it ran inside a usercmd. If writes happen inside
//    usercmds, or use a position that leads or lags him by the frame rate, that is the input to fix.
//
// [r2] 2026-11-02 UNTESTED cod3_debug_ai_spawns (default off; independent of cod3_debug_ai_shots). Owner (Mace):
// "spawn rates seem to scale up like crazy on higher fps", while census v1 sees Broc_Spawn and SpawnActor at the same
// rates at 60 and ~200 fps. Every 10 s:
//  - the sentients (level +8, 33 of 336 bytes, sampled once a game update), the player left out: alive by team (the
//    mean and the most), and a second, arrived (a slot newly in use, or holding a new entity: whatever spawned it)
//    and left (died or removed) by team. The team is sentient +4: 0x823C11F8(mask) finds a sentient by
//    (1 << +4) & mask. 1 = axis and 2 = allies are assumed (as in the later engines); the player's own value is
//    printed to check it;
//  - G_Spawn (0x82543568) calls a second, how many inside a usercmd, and its callers (the classname is set by the
//    caller after it returns, so the callers stand in for it);
//  - drone tasks started a second (0x8251A208, the DroneSplineTask constructor), inside a usercmd or not, and the
//    ones alive: started ones whose +0 is still its vtable 0x820693D0 (destruction puts the base's 0x82067850 back,
//    or the memory is reused). The drone spawner 0x82521C70 itself is stepped by server_step.cpp;
//  - every AI shot (Bullet_Fire by anyone but the player) by the shooter's team and the target's (0x823DB830's
//    target this update: axis, allied AI, the player, other, unknown), with how many traces ended within 16 units of
//    the target's body (its origin to 64 up; the player's feet to eye) and how many hurt it (G_Damage on it).
//
// [r3] 2026-11-02 UNTESTED cod3_debug_ai_accuracy (default off). ob23: the hit rate on the player follows usercmds per
// update (1.0: 13%, ~1.5: 7%, ~3.2: 1%) while the aimed point is as good or better uncapped. Static reading of the
// shot (default.xex, generated code):
//  - 0x823ED9B8 builds weaponParms with 0x823D7390: its axis from actor +316..+348, its muzzle and direction from
//    0x823D13B0, which caches tag_flash's world matrix (0x8254EC98, the entity's DObj) at actor +528..+548 with
//    level.time at actor +524 (once per update). Then, aim model 1: 0x823DB830 gives the aim point and two radii
//    (r5 the radius, r6 the most); 0x8252E968 turns (weaponParms right x radius) by a random angle about forward;
//    forward = normalize(aim point + that offset - muzzle). So the shot's angle off its aim point is the miss radius
//    over the distance, by design: not the gun's pose (the muzzle only moves the start). ob23's 30-47 units at
//    526-1044 is that radius.
//  - 0x823DB830's radius: most = max((1 - accuracy) x distance x (1 - actor +236) x k, floor) x a team factor (allies
//    x [r31+168] when 0x824A27D8 matches; axis x [0x82062C0C] while byte 0x82A2AD77 is set and the player's
//    flags2 (ps +208) lacks 0x100000); radius = most x (1 - u^3 x v), u = f + (1 - f) x rand, v = rand, f =
//    0x823DA738's accuracy fraction. Against the player 0x823DA738 uses actor +244, the weapon's near/far band, his
//    sideways speed (0x823D8828: his eye now against the 2-entry history 0x82A2A0A0 that Pmove fills through
//    0x823D7E18, over level.time - the sample's time, scaled by ps +84), how many of eye / middle / feet are visible
//    (0x823D8018, three sight traces), his stance (ps +44 & 1 / & 2) and the difficulty.
//  - the fire gate: 0x823ED9B8 returns at once when the sentient already shot at this level.time (sentient +72), and
//    without firing when 0x8251EA58 (shooter, enemy, 0.75 x actor +0x8AC) says no (it calls 0x8251DD00 on both
//    origins raised by +264: a line check, no clock). How often an actor fires is the level and anim scripts' calls.
// With cod3_debug_ai_accuracy, every 10 s:
//  - the shot commands: already shot this update / refused by 0x8251EA58 / fired / other, all and by actors with the
//    player as enemy, and those actors' fire rate per game second;
//  - every 0x823DB830 at the player: accuracy, distance, actor +236 / +244, the radius and the most, 0x823DA738's
//    fraction, 0x823D8828's sideways speed with the history's elapsed time and the units he moved since, the
//    visible points, stance and flags2 counts, ps +84;
//  - the shots at the player: the angle between the shot and its aim point against the radius's angle, the muzzle's
//    offset from the shooter's origin, and how many used a muzzle cached this update;
//  - the player: horizontal speed a game second per update while moving, jump heights (from ps +116 / +120), and
//    0x823D7E18's samples (in an update that already had one; taken again by cod3_fix_ai_player_history).

#include "game/ai_shots.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/guest.h"
#include "game/movement/step_view.h"  // [r4b] StepsRun
#include "game/watchdog.h"

REXCVAR_DEFINE_BOOL(cod3_debug_ai_shots, false, "cod3/Debug",
                    "Log AI shots near the player (first 30 a level, then every 50th): the game's call stack, what "
                    "decided the shot, its aim error to his eye and chest, and what its trace hit; and every 10 s "
                    "how often each aim function ran")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// [r2]
REXCVAR_DEFINE_BOOL(cod3_debug_ai_spawns, false, "cod3/Debug",
                    "Every 10 s (campaign): the AI alive by team and the most, arrivals and deaths a second by team, "
                    "G_Spawn calls and drone tasks started a second, the drone tasks alive, and the AI's shots by "
                    "shooter and target team with how many ended at the target")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// [r3]
REXCVAR_DEFINE_BOOL(cod3_debug_ai_accuracy, false, "cod3/Debug",
                    "Every 10 s (campaign): what became of the AI's shot commands and their fire rate at the player, "
                    "what made up each miss radius at the player, the shots' angle off their aim point, and the "
                    "player's speed, jump heights and movement samples for the AI")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DECLARE(bool, cod3_fix_ai_player_history);  // ai_aim.cpp

namespace {

namespace g = cod3::guest;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kGentityTable = 0x82A2AB40, kActiveLocalClient = 0x82A2A1C8;  // as ai_census.cpp
constexpr uint32_t kLevelTime = 0x82A4E790 + 156;                                 // level.time (ms)
constexpr uint32_t kEntOrigin = 336, kEntNumber = 500, kEntActor = 532;
constexpr uint32_t kCgBase = 0x82A98D90, kCgSize = 5840, kCgViewOrigin = 4480;    // the player's eye
constexpr uint32_t kWpForward = 0, kWpMuzzle = 0x24;                               // Bullet_Fire's weaponParms
constexpr uint32_t kAimModel = 0x829B62C0 + 12;  // 0x823ED9B8: 1 -> 0x823DB830, else 0x823DBAC8
constexpr uint32_t kActorSentient = 4, kSentientTarget = 0x68, kActorChance = 0x108;
constexpr uint32_t kActorStoredTime = 268, kActorStoredPoint = 272;  // 0x823DB830's stored aim point
constexpr uint32_t kTraceFromBullet = 0x82565534;  // 0x82550498's return address in Bullet_Fire_Extended
constexpr uint32_t kTraceEnd = 0, kTraceRaw = 32;  // [r1] +0 the end point; +32.. printed raw
constexpr uint32_t kHandleTable = 0x82A2DAD0 + 512;  // [r1] sub_824AE908's entity handles: {pointer, serial}
constexpr float kNear = 2000.0f, kNearTrace = 16.0f, kNearWrite = 300.0f;
constexpr float kDegrees = 57.2957795f;

struct V3 {
  float x = 0, y = 0, z = 0;
};
V3 ReadV3(uint8_t* base, uint32_t a) { return {g::ReadF32(base, a), g::ReadF32(base, a + 4), g::ReadF32(base, a + 8)}; }
V3 Sub(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 Lerp(V3 a, V3 b, float t) { return {a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, a.z + (b.z - a.z) * t}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
float Length(V3 a) { return std::sqrt(Dot(a, a)); }
bool Same(V3 a, V3 b) { return a.x == b.x && a.y == b.y && a.z == b.z; }

// The angle in degrees between two directions (-1 when either is about 0).
float AngleDeg(V3 a, V3 b) {
  const float la = Length(a), lb = Length(b);
  if (!(la > 1e-4f && lb > 1e-4f)) return -1.0f;
  return std::acos(std::clamp(Dot(a, b) / (la * lb), -1.0f, 1.0f)) * kDegrees;
}

uint32_t PlayerEntity(uint8_t* base) {
  const uint32_t table = g::ReadU32(base, kGentityTable);
  const uint32_t client = g::ReadU32(base, kActiveLocalClient);
  if (table < 0x10000 || client > 3) return 0;
  return g::ReadU32(base, table + 4 * (client + 1));
}

// His feet (entity origin) and eye (the client game's view origin; 60 up from his feet when that is far away).
bool PlayerBody(uint8_t* base, uint32_t player, V3* feet, V3* eye) {
  const uint32_t client = g::ReadU32(base, kActiveLocalClient);
  if (!player || client > 3) return false;
  *feet = ReadV3(base, player + kEntOrigin);
  *eye = ReadV3(base, kCgBase + client * kCgSize + kCgViewOrigin);
  const V3 d = Sub(*eye, *feet);
  if (!(Dot(d, d) < 200.0f * 200.0f)) *eye = {feet->x, feet->y, feet->z + 60.0f};
  return true;
}

V3 Chest(V3 feet, V3 eye) { return Lerp(feet, eye, 0.75f); }

float DistanceToBody(V3 p, V3 feet, V3 eye) {
  const V3 axis = Sub(eye, feet), rel = Sub(p, feet);
  const float len2 = Dot(axis, axis);
  const float t = len2 > 1e-3f ? std::clamp(Dot(rel, axis) / len2, 0.0f, 1.0f) : 0.0f;
  return Length(Sub(p, Lerp(feet, eye, t)));
}

// An actor's entity (its sentient's +0) and its target's entity (its sentient's +0x68 sentient's +0).
uint32_t ActorEntity(uint8_t* base, uint32_t actor) {
  if (actor < 0x10000) return 0;
  const uint32_t sentient = g::ReadU32(base, actor + kActorSentient);
  return sentient >= 0x10000 ? g::ReadU32(base, sentient) : 0;
}
uint32_t TargetEntity(uint8_t* base, uint32_t actor) {
  if (actor < 0x10000) return 0;
  const uint32_t sentient = g::ReadU32(base, actor + kActorSentient);
  if (sentient < 0x10000) return 0;
  const uint32_t target = g::ReadU32(base, sentient + kSentientTarget);
  return target >= 0x10000 ? g::ReadU32(base, target) : 0;
}

// [r1] sub_824AE908's handle -> entity -> actor (0 when the handle is stale or has no actor).
uint32_t ActorFromHandle(uint8_t* base, uint32_t handle) {
  const uint32_t number = handle & 0x3FFF;
  if (number >= 4096) return 0;
  const uint32_t slot = kHandleTable + number * 8;
  if (g::ReadU32(base, slot + 4) != (handle >> 14)) return 0;
  const uint32_t entity = g::ReadU32(base, slot);
  return entity >= 0x10000 ? g::ReadU32(base, entity + kEntActor) : 0;
}

bool On() { return REXCVAR_GET(cod3_debug_ai_shots); }

thread_local int t_in_usercmd = 0;  // [r1] inside ClientThink_real

// The last aim decision on this thread (the game thread runs the AI).
struct Decision {
  int model = 0;  // 1: 0x823DB830 (aim point), 2: 0x823DBAC8 (hit or miss)
  uint32_t shooter = 0, target = 0;
  int32_t level_time = 0;
  float accuracy = 0;
  int32_t stored_age = 0;  // model 1: level.time - actor +268 before the call (< 1000: the stored point is used)
  bool has_point = false;
  V3 point;                // model 1: the point it aims at
  uint32_t result = 0;     // model 2
  float chance = 0;
};
thread_local Decision t_decision;

struct Shot {
  bool active = false, log = false, traced = false, hurt_him = false;
  uint64_t number = 0;
  uint32_t attacker = 0, lr = 0, player = 0;
  int32_t level_time = 0;
  float distance = 0;
  V3 muzzle, forward, feet, eye, start, end, trace_end;
  uint32_t raw[8] = {};
  std::string stack;
};
thread_local Shot t_shot;

std::atomic<uint64_t> g_aim_point_any{0}, g_aim_point_player{0}, g_hit_or_miss_any{0}, g_hit_or_miss_player{0};
std::atomic<uint64_t> g_bullets_near{0}, g_bullets_hurt_him{0}, g_level_shots{0}, g_level_writes{0};
std::atomic<int32_t> g_last_level_time{0}, g_last_write_level_time{0};
std::mutex g_callers_mutex;
std::vector<std::pair<uint32_t, uint64_t>> g_callers;  // Bullet_Fire's return addresses this window
Clock::time_point g_since = Clock::now();               // main thread

// [r1] Shots at the player this window (main thread: the AI and ClientThink run there).
struct AtPlayer {
  uint64_t count = 0, stored_used = 0, with_point = 0, traced = 0, trace_near = 0, hurt = 0;
  double offset_sum = 0, offset_max = 0, age_sum = 0, aim_err_sum = 0;
};
AtPlayer g_at;
// [r1] sub_824AE908 this window.
struct Writer {
  uint64_t calls = 0, calls_in_usercmd = 0, writes = 0, writes_in_usercmd = 0, writes_player = 0;
  uint64_t time_is_level = 0;  // the new +268 equals level.time
  // [r4] 2026-11-02 the calls for an actor whose target is the player: what sub_824AE908 was given and answered
  // (static read: r4 the actor's eye, r5 the point it looks for, f1 the spread of the extra points, r7 how many
  // extra points; it answers 1 and writes +268 / +272 when one of the points is seen).
  uint64_t pl_calls = 0, pl_yes = 0, pl_too_near = 0, pl_too_far = 0, pl_samples[4] = {};  // samples 0, 3, 5, other
  double pl_f1_sum = 0, pl_eye_up_sum = 0, pl_eye_up_min = 1e9, pl_dist_sum = 0;
  double pl_point_up_sum = 0, pl_point_side_sum = 0, pl_point_eye_sum = 0, pl_point_eye_max = 0;
  // [r6] 2026-11-02 UNTESTED the same calls by the helper's try (0 first, 1 second, 2 other), with the player's
  // collision box and contents at the call (what the sight test's entity trace hits: entity +240 / +256 / +324).
  struct Try {
    uint64_t calls = 0, seen = 0, box_empty = 0, not_in_mask = 0;
    double up_sum = 0, f1_sum = 0, box_h_sum = 0;
  } pl_try[3];
};
Writer g_writer;

// [r5] 2026-11-02 UNTESTED the combat script's sight helper (mace2 DLL sub_891DF240): its two calls of 0x824AE908
// (through the DLL's engine table, slot +2020), told apart by their return addresses (mace2 only; other levels' DLLs
// land in "other"). [r6] ob31: r5's gate hooks counted 0 (the static slot -> function mapping was wrong; only +2020
// is known), so they are gone; [r6] logs the table's words as the engine filled them instead.
//   0x891DF5C0  the first try: the target's +1468 point lowered by [0x89001D0C], f1 [0x89001D08], r7 0/5 extra points
//   0x891DF5FC  the second try (only after a first that saw nothing): the raw +1468 point, f1 [0x89001474], 0/3
// ob31's player calls are (eye - 20, f1 12) and (eye, f1 8) in pairs: the "56 / 10" means were their average.
namespace gates {
constexpr uint32_t kFirstTry = 0x891DF5C0, kSecondTry = 0x891DF5FC;
struct Counts {
  uint64_t first = 0, first_seen = 0, second = 0, second_seen = 0, other = 0, other_seen = 0;
};
Counts g_counts;  // the game thread only (the AI and the scripts run there)
// [r6] The DLL's engine table (0x89265130, the slots the helper and the combat script use) and the helper's constants.
constexpr uint32_t kDllTable = 0x89265130;
constexpr uint32_t kSlots[] = {284, 1468, 2020, 2024, 3892, 4052, 4100, 4156};
constexpr uint32_t kConsts[] = {0x89001D08, 0x89001D0C, 0x89001474, 0x89001400};
uint32_t g_table_logged_for = 0;  // slot +2020's word when the table was last logged (a new load logs it again)
}  // namespace gates

// [r6] The player's entity: his collision box (mins +240, maxs +256: ClientThink_real copies the pmove_t's box there
// after Pmove, 0x8256C7F0) and contents (+324), as the sight test's entity trace (0x825157C0) reads them; its mask.
constexpr uint32_t kEntMins = 240, kEntMaxs = 256, kEntContents = 324;
constexpr uint32_t kSightTraceMask = 0x0280F021;  // 0x824AE908's trace (r14)

void NoteCaller(uint32_t lr) {
  std::lock_guard lock(g_callers_mutex);
  for (auto& c : g_callers)
    if (c.first == lr) {
      ++c.second;
      return;
    }
  if (g_callers.size() < 16) g_callers.emplace_back(lr, 1);
}

std::string DescribeDecision(uint8_t* base, const Shot& s) {
  const Decision& d = t_decision;
  if (d.model == 0 || d.shooter != s.attacker || d.level_time != s.level_time)
    return "no 0x823DB830 / 0x823DBAC8 for this shooter in this update";
  std::string target = !d.target ? "none" : d.target == s.player ? "the player"
                                        : fmt::format("#{}", g::ReadU32(base, d.target + kEntNumber));
  if (d.model == 2)
    return fmt::format("hit-or-miss 0x823DBAC8 at {}: {} (chance {:.3f}, accuracy {:.2f})", target,
                       d.result ? "hit" : "miss", d.chance, d.accuracy);
  if (!d.has_point) return fmt::format("aim point 0x823DB830 at {} (no point given back)", target);
  return fmt::format("aim point 0x823DB830 at {} (accuracy {:.2f}): the point {:.0f} units from his eye, {:.0f} from "
                     "his chest; the stored point's age {} ms ({})",
                     target, d.accuracy, Length(Sub(d.point, s.eye)), Length(Sub(d.point, Chest(s.feet, s.eye))),
                     d.stored_age, d.stored_age < 1000 ? "used" : "not used: his eye, lowered");
}

// [r1] One followed shot into the at-the-player window.
void CountAtPlayer(const Shot& s) {
  const Decision& d = t_decision;
  if (d.model != 1 || d.shooter != s.attacker || d.level_time != s.level_time || d.target != s.player) return;
  AtPlayer& a = g_at;
  ++a.count;
  if (d.has_point) {
    ++a.with_point;
    const double off = Length(Sub(d.point, s.eye));
    a.offset_sum += off;
    a.offset_max = std::max(a.offset_max, off);
  }
  if (d.stored_age < 1000) {
    ++a.stored_used;
    a.age_sum += d.stored_age;
  }
  const float err = AngleDeg(s.forward, Sub(Chest(s.feet, s.eye), s.muzzle));
  if (err >= 0.0f) a.aim_err_sum += err;
  if (s.traced) {
    ++a.traced;
    if (DistanceToBody(s.trace_end, s.feet, s.eye) <= kNearTrace) ++a.trace_near;
  }
  if (s.hurt_him) ++a.hurt;
}

void EachUsercmd(uint8_t* base) {
  if (!On()) return;
  const auto now = Clock::now();
  const double seconds = std::chrono::duration<double>(now - g_since).count();
  if (seconds < 10.0) return;
  g_since = now;
  std::string callers;
  {
    std::lock_guard lock(g_callers_mutex);
    std::sort(g_callers.begin(), g_callers.end(), [](auto& a, auto& b) { return a.second > b.second; });
    for (const auto& c : g_callers)
      callers += fmt::format("{}{} x{}", callers.empty() ? "" : ", ", cod3::watchdog::GuestCodeName(c.first), c.second);
    g_callers.clear();
  }
  auto rate = [seconds](std::atomic<uint64_t>& c) { return double(c.exchange(0)) / seconds; };
  const double ap_any = rate(g_aim_point_any), ap_player = rate(g_aim_point_player);
  const double hm_any = rate(g_hit_or_miss_any), hm_player = rate(g_hit_or_miss_player);
  const double close_rate = rate(g_bullets_near), hurt_rate = rate(g_bullets_hurt_him);  // ("near": a Windows macro)
  REXLOG_INFO("cod3: ai shots, last 10 s, a second: aim point 0x823DB830 {:.2f} (at the player {:.2f}), hit-or-miss "
              "0x823DBAC8 {:.2f} (at the player {:.2f}); AI bullets within 2000 of him {:.2f}, that hurt him "
              "{:.2f}; aim model [0x829B62CC] = {}; Bullet_Fire's callers: {}",
              ap_any, ap_player, hm_any, hm_player, close_rate, hurt_rate, g::ReadS32(base, kAimModel),
              callers.empty() ? "none" : callers);
  // [r1] The shots at the player, and the stored point's writer.
  const AtPlayer a = g_at;
  g_at = AtPlayer{};
  REXLOG_INFO("cod3: ai shots at the player, last 10 s: {} shots; the stored point used {} ({:.0f}%, mean age {:.0f} "
              "ms); the aimed point from his eye: mean {:.0f}, most {:.0f} units; mean aim error to his chest {:.1f} "
              "deg; traces ending within 16 units of him {} of {}; hurt him {}",
              a.count, a.stored_used, a.count ? 100.0 * a.stored_used / a.count : 0.0,
              a.stored_used ? a.age_sum / a.stored_used : 0.0, a.with_point ? a.offset_sum / a.with_point : 0.0,
              a.offset_max, a.count ? a.aim_err_sum / a.count : 0.0, a.trace_near, a.traced, a.hurt);
  const Writer w = g_writer;
  g_writer = Writer{};
  REXLOG_INFO("cod3: ai stored point, last 10 s: sub_824AE908 {} calls ({} inside a usercmd), {} wrote an actor's "
              "+268 / +272 ({} inside a usercmd, {} for an actor targeting the player, {} with +268 = level.time)",
              w.calls, w.calls_in_usercmd, w.writes, w.writes_in_usercmd, w.writes_player, w.time_is_level);
  // [r4] 2026-11-02 the same calls for actors targeting the player: given what, answered what.
  const double n = w.pl_calls ? double(w.pl_calls) : 1.0;
  REXLOG_INFO("cod3: ai sight at the player, last 10 s: sub_824AE908 {} calls, seen {} ({:.0f}%), too near {}, too "
              "far {}; extra points 0/3/5/other {}/{}/{}/{}; f1 mean {:.2f}; the actor's eye (r4) above his feet mean "
              "{:.1f}, least {:.1f}; eye to point mean {:.0f}; the point (r5) above the player's feet mean {:.1f}, "
              "sideways from them mean {:.1f}, from his eye mean {:.1f}, most {:.1f}",
              w.pl_calls, w.pl_yes, 100.0 * w.pl_yes / n, w.pl_too_near, w.pl_too_far, w.pl_samples[0],
              w.pl_samples[1], w.pl_samples[2], w.pl_samples[3], w.pl_f1_sum / n, w.pl_eye_up_sum / n,
              w.pl_calls ? w.pl_eye_up_min : 0.0, w.pl_dist_sum / n, w.pl_point_up_sum / n, w.pl_point_side_sum / n,
              w.pl_point_eye_sum / n, w.pl_point_eye_max);
  // [r5] 2026-11-02 the sight helper's two tries (any actor, any target).
  const gates::Counts c = gates::g_counts;
  gates::g_counts = gates::Counts{};
  REXLOG_INFO("cod3: ai sight helper, last 10 s: first try {} (seen {}); second try {} (seen {}); other sight calls "
              "{} (seen {})",
              c.first, c.first_seen, c.second, c.second_seen, c.other, c.other_seen);
  // [r6] 2026-11-02 UNTESTED the player calls by try, with his box and contents at each call. Expected if the box is
  // the cause: uncapped (cod3_move_keep_box 0) "box empty" near all calls and seen 0; at 60 fps or with the fix, few.
  const auto per = [](const Writer::Try& t, double Writer::Try::*sum) { return t.calls ? t.*sum / t.calls : 0.0; };
  REXLOG_INFO("cod3: ai sight at the player by try, last 10 s: first {} (seen {}), point {:.1f} above his feet, f1 "
              "{:.2f}, his box {:.1f} high, empty in {}, contents outside the trace's mask in {}; second {} (seen {}), "
              "{:.1f}, f1 {:.2f}, box {:.1f}, empty {}, contents {}; other {} (seen {})",
              w.pl_try[0].calls, w.pl_try[0].seen, per(w.pl_try[0], &Writer::Try::up_sum),
              per(w.pl_try[0], &Writer::Try::f1_sum), per(w.pl_try[0], &Writer::Try::box_h_sum), w.pl_try[0].box_empty,
              w.pl_try[0].not_in_mask, w.pl_try[1].calls, w.pl_try[1].seen, per(w.pl_try[1], &Writer::Try::up_sum),
              per(w.pl_try[1], &Writer::Try::f1_sum), per(w.pl_try[1], &Writer::Try::box_h_sum), w.pl_try[1].box_empty,
              w.pl_try[1].not_in_mask, w.pl_try[2].calls, w.pl_try[2].seen);
  // [r6] Once per load while mace2's helper runs: the DLL's engine table as the engine filled it (the static
  // mapping proved wrong) and the helper's constants, read from memory.
  if (c.first + c.second > 0) {
    const uint32_t anchor = g::ReadU32(base, gates::kDllTable + 2020);
    if (anchor != gates::g_table_logged_for) {
      gates::g_table_logged_for = anchor;
      std::string slots, consts;
      for (uint32_t s : gates::kSlots)
        slots += fmt::format("{}+{} 0x{:08X}", slots.empty() ? "" : ", ", s, g::ReadU32(base, gates::kDllTable + s));
      for (uint32_t a : gates::kConsts)
        consts += fmt::format("{}[0x{:08X}] {:.3f}", consts.empty() ? "" : ", ", a, g::ReadF32(base, a));
      REXLOG_INFO("cod3: ai sight helper (mace2): engine table at 0x{:08X}: {}; constants {}", gates::kDllTable, slots,
                  consts);
    }
  }
}

// ---- [r2] cod3_debug_ai_spawns ----

bool SpawnsOn() { return REXCVAR_GET(cod3_debug_ai_spawns); }

constexpr uint32_t kLevelSentients = 0x82A4E790 + 8;  // as ai_census.cpp: 33 sentients of 336 bytes
constexpr uint32_t kSentientCount = 33, kSentientSize = 336, kSentientInUse = 156;
constexpr uint32_t kSentientTeam = 4;  // 0x823C11F8(mask): (1 << sentient +4) & mask
constexpr uint32_t kDroneTaskVtable = 0x820693D0;  // 0x8251A208 stores it over the base's 0x82067850
constexpr float kActorHeight = 64.0f;
constexpr size_t kDroneTasksKept = 1024;

enum Side { kAxis, kAllies, kOther, kSides };
const char* const kSideNames[kSides] = {"axis", "allies", "other"};
Side SideOfTeam(int32_t team) { return team == 1 ? kAxis : team == 2 ? kAllies : kOther; }

enum TargetSide { kToAxis, kToAllies, kToPlayer, kToOther, kToUnknown, kTargetSides };
const char* const kTargetNames[kTargetSides] = {"axis", "allied AI", "the player", "other", "unknown"};

// The team (sentient +4) of an entity's sentient in use, -1 when it has none.
int32_t TeamOf(uint8_t* base, uint32_t entity) {
  const uint32_t first = g::ReadU32(base, kLevelSentients);
  if (!entity || first < 0x10000) return -1;
  for (uint32_t i = 0; i < kSentientCount; ++i) {
    const uint32_t s = first + i * kSentientSize;
    if (g::ReadS32(base, s + kSentientInUse) >= 0 && g::ReadU32(base, s) == entity)
      return g::ReadS32(base, s + kSentientTeam);
  }
  return -1;
}

struct TeamShots {
  uint64_t shots = 0, traced = 0, ended_at = 0, hurt = 0;
};

// This window (main thread: the AI, the scheduler and ClientThink run there).
struct Spawns {
  uint64_t updates = 0;
  uint64_t alive_sum[kSides] = {}, alive_most[kSides] = {}, arrived[kSides] = {}, left[kSides] = {};
  int32_t player_team = -1;
  TeamShots shots[kSides][kTargetSides] = {};
};
Spawns g_spawns;
Clock::time_point g_spawns_since = Clock::now();

// Each sentient slot as of the last update.
struct Slot {
  uint32_t entity = 0;
  int32_t team = -1;
};
Slot g_slots[kSentientCount];
bool g_slots_valid = false;
int32_t g_slots_level_time = 0;

std::atomic<uint64_t> g_g_spawns{0}, g_g_spawns_usercmd{0}, g_drone_starts{0}, g_drone_starts_usercmd{0};
std::vector<std::pair<uint32_t, uint64_t>> g_g_spawn_callers;  // under g_callers_mutex
std::vector<uint32_t> g_drone_tasks;                            // under g_callers_mutex

// The shot in Bullet_Fire on this thread.
struct AnyShot {
  bool active = false, traced = false, hurt = false;
  uint32_t attacker = 0, target = 0;  // entities (target 0: not known)
  Side shooter_side = kOther;
  TargetSide target_side = kToUnknown;
  V3 trace_end;
};
thread_local AnyShot t_any;

bool StartAnyShot(PPCContext& ctx, uint8_t* base) {
  const uint32_t attacker = ctx.r3.u32;
  const uint32_t player = PlayerEntity(base);
  if (!attacker || attacker == player) return false;
  AnyShot& a = t_any;
  a = AnyShot{};
  a.active = true;
  a.attacker = attacker;
  a.shooter_side = SideOfTeam(TeamOf(base, attacker));
  const Decision& d = t_decision;
  if (d.model != 0 && d.shooter == attacker && d.level_time == g::ReadS32(base, kLevelTime) && d.target) {
    a.target = d.target;
    if (player && d.target == player) {
      a.target_side = kToPlayer;
    } else {
      const int32_t team = TeamOf(base, d.target);
      a.target_side = team == 1 ? kToAxis : team == 2 ? kToAllies : kToOther;
    }
  }
  return true;
}

void EndAnyShot(uint8_t* base) {
  AnyShot& a = t_any;
  a.active = false;
  TeamShots& t = g_spawns.shots[a.shooter_side][a.target_side];
  ++t.shots;
  if (a.hurt) ++t.hurt;
  if (!a.traced) return;
  ++t.traced;
  if (!a.target) return;
  V3 feet, eye;
  if (a.target_side == kToPlayer) {
    if (!PlayerBody(base, a.target, &feet, &eye)) return;
  } else {
    feet = ReadV3(base, a.target + kEntOrigin);
    eye = {feet.x, feet.y, feet.z + kActorHeight};
  }
  if (DistanceToBody(a.trace_end, feet, eye) <= kNearTrace) ++t.ended_at;
}

void NoteSpawnCaller(uint32_t lr) {
  std::lock_guard lock(g_callers_mutex);
  for (auto& c : g_g_spawn_callers)
    if (c.first == lr) {
      ++c.second;
      return;
    }
  if (g_g_spawn_callers.size() < 16) g_g_spawn_callers.emplace_back(lr, 1);
}

void SpawnsEachUpdate(uint8_t* base) {
  if (!SpawnsOn()) {
    g_slots_valid = false;
    return;
  }
  const uint32_t first = g::ReadU32(base, kLevelSentients);
  if (first < 0x10000) {
    g_slots_valid = false;
    return;
  }
  const uint32_t player = PlayerEntity(base);
  const int32_t level_time = g::ReadS32(base, kLevelTime);
  // After a level load or restart (level.time went back), the slots are taken as they are: nothing counted.
  const bool count = g_slots_valid && level_time >= g_slots_level_time;
  g_slots_level_time = level_time;
  g_slots_valid = true;
  Spawns& sp = g_spawns;
  ++sp.updates;
  uint64_t alive[kSides] = {};
  for (uint32_t i = 0; i < kSentientCount; ++i) {
    const uint32_t s = first + i * kSentientSize;
    const bool in_use = g::ReadS32(base, s + kSentientInUse) >= 0;
    const uint32_t entity = in_use ? g::ReadU32(base, s) : 0;
    const int32_t team = in_use ? g::ReadS32(base, s + kSentientTeam) : -1;
    Slot& old = g_slots[i];
    if (entity && entity == player) {
      sp.player_team = team;
    } else {
      if (entity) ++alive[SideOfTeam(team)];
      if (count && old.entity != entity) {
        if (old.entity && old.entity != player) ++sp.left[SideOfTeam(old.team)];
        if (entity) ++sp.arrived[SideOfTeam(team)];
      }
    }
    old.entity = entity;
    old.team = team;
  }
  for (int k = 0; k < kSides; ++k) {
    sp.alive_sum[k] += alive[k];
    sp.alive_most[k] = std::max(sp.alive_most[k], alive[k]);
  }
}

void SpawnsEachUsercmd(uint8_t* base) {
  if (!SpawnsOn()) return;
  const auto now = Clock::now();
  const double seconds = std::chrono::duration<double>(now - g_spawns_since).count();
  if (seconds < 10.0) return;
  g_spawns_since = now;
  const Spawns sp = g_spawns;
  g_spawns = Spawns{};
  std::string callers;
  size_t drones_alive = 0;
  {
    std::lock_guard lock(g_callers_mutex);
    std::sort(g_g_spawn_callers.begin(), g_g_spawn_callers.end(),
              [](auto& a, auto& b) { return a.second > b.second; });
    for (const auto& c : g_g_spawn_callers)
      callers += fmt::format("{}{} x{}", callers.empty() ? "" : ", ", cod3::watchdog::GuestCodeName(c.first), c.second);
    g_g_spawn_callers.clear();
    g_drone_tasks.erase(std::remove_if(g_drone_tasks.begin(), g_drone_tasks.end(),
                                       [base](uint32_t t) { return g::ReadU32(base, t) != kDroneTaskVtable; }),
                        g_drone_tasks.end());
    drones_alive = g_drone_tasks.size();
  }
  auto rate = [seconds](std::atomic<uint64_t>& c) { return double(c.exchange(0)) / seconds; };
  const double gs = rate(g_g_spawns), gs_cmd = rate(g_g_spawns_usercmd);
  const double ds = rate(g_drone_starts), ds_cmd = rate(g_drone_starts_usercmd);
  const double n = sp.updates ? double(sp.updates) : 1.0;
  REXLOG_INFO("cod3: ai spawns, last 10 s ({:.1f} game updates a second): alive (mean / most) axis {:.1f} / {}, "
              "allies {:.1f} / {}, other {:.1f} / {}; a second, arrived axis {:.2f}, allies {:.2f}, other {:.2f}; "
              "left (died or removed) axis {:.2f}, allies {:.2f}, other {:.2f}; the player's team value {} (taken as "
              "1 axis, 2 allies)",
              sp.updates / seconds, sp.alive_sum[kAxis] / n, sp.alive_most[kAxis], sp.alive_sum[kAllies] / n,
              sp.alive_most[kAllies], sp.alive_sum[kOther] / n, sp.alive_most[kOther], sp.arrived[kAxis] / seconds,
              sp.arrived[kAllies] / seconds, sp.arrived[kOther] / seconds, sp.left[kAxis] / seconds,
              sp.left[kAllies] / seconds, sp.left[kOther] / seconds, sp.player_team);
  REXLOG_INFO("cod3: ai spawns, last 10 s, a second: G_Spawn {:.2f} ({:.2f} inside a usercmd; callers: {}); drone "
              "tasks started (0x8251A208) {:.2f} ({:.2f} inside a usercmd); drone tasks alive {}",
              gs, gs_cmd, callers.empty() ? "none" : callers, ds, ds_cmd, drones_alive);
  std::string shots;
  for (int s = 0; s < kSides; ++s)
    for (int t = 0; t < kTargetSides; ++t) {
      const TeamShots& c = sp.shots[s][t];
      if (!c.shots) continue;
      shots += fmt::format("{}{} at {}: {} ({:.2f}/s), traced {}, ended within 16 units of it {}, hurt it {}",
                           shots.empty() ? "" : "; ", kSideNames[s], kTargetNames[t], c.shots, c.shots / seconds,
                           c.traced, c.ended_at, c.hurt);
    }
  REXLOG_INFO("cod3: ai shots by team, last 10 s: {}", shots.empty() ? "none" : shots);
}

// ---- [r3] cod3_debug_ai_accuracy ----

bool AccOn() { return REXCVAR_GET(cod3_debug_ai_accuracy); }

constexpr uint32_t kEntClient = 528;  // the player's gclient: its playerState at +0
constexpr uint32_t kPsPmFlags = 44, kPsSpeedInt = 84, kPsJumpTime = 116, kPsJumpOriginZ = 120, kPsFlags2 = 208;
constexpr uint32_t kActor236 = 236, kActorPlayerAccuracy = 244;
constexpr uint32_t kActorMuzzleTime = 524;   // 0x823D13B0: level.time of the cached tag_flash matrix
constexpr uint32_t kSentientLastShot = 72;   // 0x823ED9B8: level.time of the sentient's last shot
constexpr uint32_t kHistoryInterval = 0x829B0B3C;  // ms between 0x823D7E18's samples (as ai_aim.cpp)
constexpr uint32_t kGateFromShoot = 0x823EDAC4;    // 0x8251EA58's return address in 0x823ED9B8
constexpr uint32_t kFractionFromAim = 0x823DBA0C;  // 0x823DA738's in 0x823DB830
constexpr uint32_t kSidewaysFromFraction = 0x823DA818;  // 0x823D8828's in 0x823DA738
constexpr uint32_t kHistoryFromSideways = 0x823D884C;   // 0x823D7D58's in 0x823D8828
constexpr uint32_t kSightFromFraction[3] = {0x823DA934, 0x823DA968, 0x823DA9A4};  // 0x823D8018's in 0x823DA738

// A mean with its least and most.
struct Stat {
  double sum = 0, least = 0, most = 0;
  uint64_t n = 0;
  void Add(double v) {
    least = n ? std::min(least, v) : v;
    most = n ? std::max(most, v) : v;
    sum += v;
    ++n;
  }
  double Mean() const { return n ? sum / double(n) : 0.0; }
};

// One 0x823DB830 at the player, on this thread (the game thread).
struct AccShot {
  bool in_aim = false, has_fraction = false, has_sideways = false, has_history = false, has_radius = false;
  uint32_t shooter = 0;
  int32_t level_time = 0, elapsed = 0;
  float fraction = 0, sideways = 0, moved = 0, radius = 0;
  int sight_calls = 0, sight_seen = 0;
};
thread_local AccShot t_acc;
thread_local int t_gate = -1;         // 0x8251EA58's answer inside the current 0x823ED9B8 (-1: not asked)
thread_local uint64_t t_bullets = 0;  // Bullet_Fire calls on this thread

// The shot command around 0x823ED9B8.
struct ShootCmd {
  bool at_player = false, already = false, given_point = false;
  uint64_t bullets = 0;
};
thread_local ShootCmd t_cmd;

// This window (main thread).
struct Accuracy {
  uint64_t cmds = 0, cmds_player = 0, already = 0, already_player = 0, refused = 0, refused_player = 0;
  uint64_t fired = 0, fired_player = 0, fired_given_point = 0, other = 0;
  double actor_seconds = 0;  // actors with the player as enemy x game seconds
  uint64_t aims = 0, crouch = 0, prone = 0, flags2 = 0;
  Stat accuracy, distance, a236, a244, most, radius, fraction, sideways, elapsed, moved, seen, ps_speed;
  Stat off_angle, radius_angle, muzzle_across, muzzle_up;
  uint64_t muzzle_shots = 0, muzzle_cached = 0;
  Stat speed, jump;
  uint64_t samples = 0, samples_same = 0, refreshes = 0;
  uint64_t step_hist[4] = {};  // [r4b] movement steps between two game updates: 0, 1, 2, 3 or more
};
Accuracy g_acc;
Clock::time_point g_acc_since = Clock::now();

// The player between updates.
struct Track {
  bool valid = false, jumping = false;
  int32_t time = 0, jump_time = 0, jump_start = 0;
  V3 origin;
  float jump_z0 = 0, jump_top = 0;
  // [r4] The last kSpeedSpan updates (time, origin), for the speed over ~100 ms.
  int32_t span_time[7] = {};
  V3 span_origin[7];
  int span_count = 0, span_next = 0;
  uint64_t steps_seen = 0;  // [r4b] cod3::movement::StepsRun() at the last update
};
Track g_track;
// [r4] 2026-11-02 UNTESTED: the speed is taken over 6 updates (~100 ms), not one. The player moves
// in cod3_move_hz steps on the usercmd clock, which aren't in phase with the game updates above 60
// fps: an update interval then holds 0, 1 or 2 movement steps. Over one interval, "while moving"
// (> 10 units/s) left out the 0-step ones and kept the 2-step ones, so the mean read high (ob25:
// ~15% uncapped) and the most read about twice the walking speed (372-385). Over 6 intervals the
// error is one step in 6 at most, and no moving window has 0 steps.
constexpr int kSpeedSpan = 7;  // samples: 6 intervals

// After 0x823DB830 at the player.
void NoteAim(uint8_t* base, uint32_t actor, uint32_t player, float accuracy, uint32_t eye_out, uint32_t point_out,
             uint32_t radius_out, uint32_t most_out) {
  Accuracy& a = g_acc;
  AccShot& s = t_acc;
  ++a.aims;
  a.accuracy.Add(accuracy);
  if (eye_out >= 0x10000 && point_out >= 0x10000)
    a.distance.Add(Length(Sub(ReadV3(base, point_out), ReadV3(base, eye_out))));
  a.a236.Add(g::ReadF32(base, actor + kActor236));
  a.a244.Add(g::ReadF32(base, actor + kActorPlayerAccuracy));
  if (most_out >= 0x10000) a.most.Add(g::ReadF32(base, most_out));
  if (radius_out >= 0x10000) {
    s.has_radius = true;
    s.radius = g::ReadF32(base, radius_out);
    a.radius.Add(s.radius);
  }
  if (s.has_fraction) a.fraction.Add(s.fraction);
  if (s.has_sideways) a.sideways.Add(s.sideways);
  if (s.has_history) {
    a.elapsed.Add(s.elapsed);
    a.moved.Add(s.moved);
  }
  if (s.sight_calls) a.seen.Add(s.sight_seen);
  const uint32_t client = g::ReadU32(base, player + kEntClient);
  if (client >= 0x10000) {
    const uint32_t pm_flags = g::ReadU32(base, client + kPsPmFlags);
    if (pm_flags & 1) ++a.prone;
    if (pm_flags & 2) ++a.crouch;
    if (g::ReadU32(base, client + kPsFlags2) & 0x100000) ++a.flags2;
    a.ps_speed.Add(g::ReadS32(base, client + kPsSpeedInt));
  }
}

// At Bullet_Fire: a shot at the player decided by 0x823DB830 in this update.
void NoteShotAtPlayer(PPCContext& ctx, uint8_t* base) {
  const uint32_t attacker = ctx.r3.u32, wp = ctx.r6.u32;
  const uint32_t player = PlayerEntity(base);
  if (!attacker || !player || attacker == player || wp < 0x10000) return;
  const int32_t now = g::ReadS32(base, kLevelTime);
  const Decision& d = t_decision;
  if (d.model != 1 || d.shooter != attacker || d.level_time != now || d.target != player || !d.has_point) return;
  Accuracy& a = g_acc;
  const V3 muzzle = ReadV3(base, wp + kWpMuzzle);
  const V3 to_point = Sub(d.point, muzzle);
  const float angle = AngleDeg(ReadV3(base, wp + kWpForward), to_point);
  if (angle >= 0.0f) a.off_angle.Add(angle);
  const AccShot& s = t_acc;
  const float distance = Length(to_point);
  if (s.has_radius && s.shooter == attacker && s.level_time == now && distance > 1.0f)
    a.radius_angle.Add(std::atan(s.radius / distance) * kDegrees);
  const V3 off = Sub(muzzle, ReadV3(base, attacker + kEntOrigin));
  a.muzzle_across.Add(std::sqrt(off.x * off.x + off.y * off.y));
  a.muzzle_up.Add(off.z);
  ++a.muzzle_shots;
  const uint32_t actor = g::ReadU32(base, attacker + kEntActor);
  if (actor >= 0x10000 && g::ReadS32(base, actor + kActorMuzzleTime) == now) ++a.muzzle_cached;
}

void AccEachUpdate(uint8_t* base) {
  Track& t = g_track;
  const uint32_t player = AccOn() ? PlayerEntity(base) : 0;
  if (!player) {
    t.valid = t.jumping = false;
    return;
  }
  Accuracy& a = g_acc;
  const int32_t now = g::ReadS32(base, kLevelTime);
  const V3 origin = ReadV3(base, player + kEntOrigin);
  const int32_t dt = now - t.time;
  // [r4] A level load or a long pause (level.time back, or a gap) starts the window again.
  if (!t.valid || dt <= 0 || dt > 100) t.span_count = 0;
  t.span_time[t.span_next] = now;
  t.span_origin[t.span_next] = origin;
  t.span_next = (t.span_next + 1) % kSpeedSpan;
  if (t.span_count < kSpeedSpan) ++t.span_count;
  if (t.span_count == kSpeedSpan) {
    const int oldest = t.span_next;  // the slot written kSpeedSpan - 1 updates ago
    const int32_t span = now - t.span_time[oldest];
    if (span > 0) {
      const float dx = origin.x - t.span_origin[oldest].x, dy = origin.y - t.span_origin[oldest].y;
      const double speed = std::sqrt(dx * dx + dy * dy) * 1000.0 / span;
      // Teleports and level-start pushes (more than 1000 units/s) aren't walking.
      if (speed > 10.0 && speed < 1000.0) a.speed.Add(speed);
    }
  }
  // [r4b] 2026-11-02 UNTESTED The movement steps (cod3_move_hz) run since the last update: 1 each at any frame rate
  // when they end on the updates' ends (cod3_move_hz_sync), 0 or 2 now and then above 60 fps when they don't. The CoD4
  // style's 8 ms steps count too: 2 or 3 an update (125 a second against 60), more spread above 60 fps.
  const uint64_t steps_now = cod3::movement::StepsRun();
  if (t.valid && dt > 0 && dt <= 100) ++a.step_hist[std::min<uint64_t>(steps_now - t.steps_seen, 3)];
  t.steps_seen = steps_now;
  if (t.valid && dt > 0 && dt <= 100) {
    // Actors with the player as enemy, for their fire rate.
    const uint32_t first = g::ReadU32(base, kLevelSentients);
    if (first >= 0x10000) {
      const uint32_t end = first + kSentientCount * kSentientSize;
      uint32_t count = 0;
      for (uint32_t s = first; s < end; s += kSentientSize) {
        if (g::ReadS32(base, s + kSentientInUse) < 0 || g::ReadU32(base, s) == player) continue;
        const uint32_t target = g::ReadU32(base, s + kSentientTarget);
        if (target >= first && target < end && g::ReadU32(base, target) == player) ++count;
      }
      a.actor_seconds += double(count) * dt / 1000.0;
    }
  }
  // A jump: ps +116 (jump time) changes to a new non-zero value, from the height at ps +120.
  const uint32_t client = g::ReadU32(base, player + kEntClient);
  if (client >= 0x10000) {
    const int32_t jump_time = g::ReadS32(base, client + kPsJumpTime);
    if (t.valid && jump_time != t.jump_time && jump_time != 0) {
      if (t.jumping) a.jump.Add(t.jump_top);
      t.jumping = true;
      t.jump_z0 = g::ReadF32(base, client + kPsJumpOriginZ);
      t.jump_top = 0.0f;
      t.jump_start = now;
    }
    t.jump_time = jump_time;
    if (t.jumping) {
      t.jump_top = std::max(t.jump_top, origin.z - t.jump_z0);
      if (dt < 0 || now - t.jump_start > 1500) {
        a.jump.Add(t.jump_top);
        t.jumping = false;
      }
    }
  }
  t.valid = true;
  t.time = now;
  t.origin = origin;
}

void AccEachUsercmd(uint8_t* base) {
  if (!AccOn()) return;
  const auto now = Clock::now();
  const double seconds = std::chrono::duration<double>(now - g_acc_since).count();
  if (seconds < 10.0) return;
  g_acc_since = now;
  const Accuracy a = g_acc;
  g_acc = Accuracy{};
  const double actor_s = a.actor_seconds > 0.0 ? a.actor_seconds : 1.0;
  REXLOG_INFO("cod3: ai fire, last 10 s: shot commands {} ({} by actors with the player as enemy); already shot this "
              "update {} ({}); refused by 0x8251EA58 {} ({}); fired {} ({}; {} at a given point); other {}; actors "
              "with the player as enemy: {:.1f} on average, each fired {:.2f} a game second ({:.2f} commands)",
              a.cmds, a.cmds_player, a.already, a.already_player, a.refused, a.refused_player, a.fired, a.fired_player,
              a.fired_given_point, a.other, a.actor_seconds / seconds, a.fired_player / actor_s,
              a.cmds_player / actor_s);
  REXLOG_INFO("cod3: ai miss radius at the player, last 10 s: {} aims; accuracy {:.2f}; distance {:.0f}; actor +236 "
              "{:.2f}, +244 {:.2f}; most radius {:.1f} (most {:.1f}); radius {:.1f} (most {:.1f}); 0x823DA738 fraction "
              "{:.3f} ({:.3f}..{:.3f}, {}); 0x823D8828 sideways {:.3f} (most {:.3f}, {}); history {:.0f} ms ({:.0f}.."
              "{:.0f}), he moved {:.1f} units since ({:.0f} units/s); visible points {:.2f} of 3; crouched {}, prone {}, "
              "flags2 0x100000 {}; ps +84 {:.0f}",
              a.aims, a.accuracy.Mean(), a.distance.Mean(), a.a236.Mean(), a.a244.Mean(), a.most.Mean(), a.most.most,
              a.radius.Mean(), a.radius.most, a.fraction.Mean(), a.fraction.least, a.fraction.most, a.fraction.n,
              a.sideways.Mean(), a.sideways.most, a.sideways.n, a.elapsed.Mean(), a.elapsed.least, a.elapsed.most,
              a.moved.Mean(), a.elapsed.Mean() > 0.0 ? a.moved.Mean() * 1000.0 / a.elapsed.Mean() : 0.0,
              a.seen.Mean(), a.crouch, a.prone, a.flags2, a.ps_speed.Mean());
  REXLOG_INFO("cod3: ai shots at the player, last 10 s: {} shots; off their aim point {:.2f} deg (most {:.2f}), the "
              "radius as an angle {:.2f} deg; muzzle from the shooter's origin {:.1f} across (most {:.1f}), {:.1f} up; "
              "muzzle cached this update {} of {}",
              a.off_angle.n, a.off_angle.Mean(), a.off_angle.most, a.radius_angle.Mean(), a.muzzle_across.Mean(),
              a.muzzle_across.most, a.muzzle_up.Mean(), a.muzzle_cached, a.muzzle_shots);
  REXLOG_INFO("cod3: player movement, last 10 s: horizontal speed while moving (over ~100 ms) {:.0f} units a game second (most "
              "{:.0f}, {} updates); jumps {}, height {:.1f} (least {:.1f}, most {:.1f}); 0x823D7E18 samples {} ({} in an "
              "update that had one, {} taken again; cod3_fix_ai_player_history {}); sample interval {} ms; movement "
              "steps between two updates 0: {}, 1: {}, 2: {}, 3+: {}",
              a.speed.Mean(), a.speed.most, a.speed.n, a.jump.n, a.jump.Mean(), a.jump.least, a.jump.most, a.samples,
              a.samples_same, a.refreshes, REXCVAR_GET(cod3_fix_ai_player_history) ? "on" : "off",
              g::ReadS32(base, kHistoryInterval), a.step_hist[0], a.step_hist[1], a.step_hist[2], a.step_hist[3]);
}

}  // namespace

namespace cod3::ai_shots {

BulletScope::BulletScope(PPCContext& ctx, uint8_t* base) : base_(base), active_(false), any_(false) {
  ++t_bullets;                                // [r3]
  if (AccOn()) NoteShotAtPlayer(ctx, base);  // [r3]
  if (SpawnsOn() && !t_any.active) any_ = StartAnyShot(ctx, base);  // [r2]
  if (!On() || t_shot.active) return;
  const uint32_t attacker = ctx.r3.u32, wp = ctx.r6.u32;
  const uint32_t player = PlayerEntity(base);
  if (!attacker || !player || attacker == player || wp < 0x10000) return;
  V3 feet, eye;
  if (!PlayerBody(base, player, &feet, &eye)) return;
  const V3 muzzle = ReadV3(base, wp + kWpMuzzle);
  const float distance = Length(Sub(eye, muzzle));
  if (!(distance <= kNear)) return;
  Shot& s = t_shot;
  s = Shot{};
  s.active = active_ = true;
  s.attacker = attacker;
  s.player = player;
  s.lr = uint32_t(ctx.lr);
  s.level_time = g::ReadS32(base, kLevelTime);
  s.distance = distance;
  s.muzzle = muzzle;
  s.forward = ReadV3(base, wp + kWpForward);
  s.feet = feet;
  s.eye = eye;
  g_bullets_near.fetch_add(1, std::memory_order_relaxed);
  NoteCaller(s.lr);
  // Numbered per level (level.time goes back at a level load or restart).
  if (s.level_time < g_last_level_time.exchange(s.level_time)) g_level_shots.store(0);
  s.number = g_level_shots.fetch_add(1) + 1;
  s.log = s.number <= 30 || s.number % 50 == 0;
  if (s.log)
    s.stack = cod3::watchdog::GuestCodeName(s.lr) + " <- " + cod3::watchdog::GuestStack(ctx, base);
}

BulletScope::~BulletScope() {
  if (any_) EndAnyShot(base_);  // [r2]
  if (!active_) return;
  Shot& s = t_shot;
  s.active = false;
  uint8_t* base = base_;
  if (s.hurt_him) g_bullets_hurt_him.fetch_add(1, std::memory_order_relaxed);
  CountAtPlayer(s);  // [r1]
  if (!s.log) return;
  const V3 chest = Chest(s.feet, s.eye);
  const V3 shot = Sub(s.end, s.start);
  std::string trace = "no trace seen (not Bullet_Fire_Extended's first: a shotgun?)";
  if (s.traced) {
    trace = fmt::format("its end {:.0f} units from him", DistanceToBody(s.trace_end, s.feet, s.eye));
    if (s.number <= 3)  // [r1] the result's words +32..+60, raw, to settle its layout
      trace += fmt::format(" (result +32..+60: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X})", s.raw[0],
                           s.raw[1], s.raw[2], s.raw[3], s.raw[4], s.raw[5], s.raw[6], s.raw[7]);
  }
  REXLOG_INFO("cod3: ai shot {} (level {} ms): shooter {:08X} #{} {:.0f} units from his eye; {}; aim error {:.1f} deg "
              "to his eye, {:.1f} to his chest; with the spread {:.1f} / {:.1f}; trace: {}; hurt him: {}",
              s.number, s.level_time, s.attacker, g::ReadU32(base, s.attacker + kEntNumber), s.distance,
              DescribeDecision(base, s), AngleDeg(s.forward, Sub(s.eye, s.muzzle)),
              AngleDeg(s.forward, Sub(chest, s.muzzle)), s.traced ? AngleDeg(shot, Sub(s.eye, s.start)) : -1.0f,
              s.traced ? AngleDeg(shot, Sub(chest, s.start)) : -1.0f, trace, s.hurt_him ? "yes" : "no");
  REXLOG_INFO("cod3: ai shot {}: guest stack {}", s.number, s.stack);
}

void OnHitOrMiss(uint8_t* base, uint32_t actor, uint32_t result) {
  if (!On() && !SpawnsOn()) return;  // [r2] the shot's target for cod3_debug_ai_spawns too
  Decision d;
  d.model = 2;
  d.shooter = ActorEntity(base, actor);
  d.target = TargetEntity(base, actor);
  d.level_time = g::ReadS32(base, kLevelTime);
  d.result = result;
  d.chance = actor >= 0x10000 ? g::ReadF32(base, actor + kActorChance) : 0.0f;
  t_decision = d;
  g_hit_or_miss_any.fetch_add(1, std::memory_order_relaxed);
  if (d.target && d.target == PlayerEntity(base)) g_hit_or_miss_player.fetch_add(1, std::memory_order_relaxed);
}

UsercmdScope::UsercmdScope(uint8_t* base) {
  EachUsercmd(base);
  SpawnsEachUsercmd(base);  // [r2]
  AccEachUsercmd(base);     // [r3]
  ++t_in_usercmd;
}

UsercmdScope::~UsercmdScope() { --t_in_usercmd; }

void OnDamage(uint8_t* /*base*/, uint32_t target) {
  Shot& s = t_shot;
  if (s.active && target && target == s.player) s.hurt_him = true;
  AnyShot& a = t_any;  // [r2]
  if (a.active && target && target == a.target) a.hurt = true;
}

void EachUpdate(uint8_t* base) {
  SpawnsEachUpdate(base);  // [r2]
  AccEachUpdate(base);     // [r3]
}

// [r3]
ShootScope::ShootScope(PPCContext& ctx, uint8_t* base) : active_(AccOn()) {
  if (!active_) return;
  const uint32_t actor = ctx.r3.u32;
  ShootCmd& c = t_cmd;
  c = ShootCmd{};
  const uint32_t player = PlayerEntity(base);
  c.at_player = player && TargetEntity(base, actor) == player;
  const uint32_t sentient = actor >= 0x10000 ? g::ReadU32(base, actor + kActorSentient) : 0;
  c.already = sentient >= 0x10000 && g::ReadS32(base, sentient + kSentientLastShot) == g::ReadS32(base, kLevelTime);
  c.given_point = ctx.r5.u32 != 0;
  c.bullets = t_bullets;
  t_gate = -1;
}

ShootScope::~ShootScope() {
  if (!active_) return;
  const ShootCmd& c = t_cmd;
  Accuracy& a = g_acc;
  ++a.cmds;
  if (c.at_player) ++a.cmds_player;
  if (c.already) {
    ++a.already;
    if (c.at_player) ++a.already_player;
  } else if (t_gate == 0) {
    ++a.refused;
    if (c.at_player) ++a.refused_player;
  } else if (t_bullets != c.bullets) {
    ++a.fired;
    if (c.at_player) ++a.fired_player;
    if (c.given_point) ++a.fired_given_point;
  } else {
    ++a.other;
  }
  t_gate = -1;
}

void OnPlayerSample(uint8_t* /*base*/, bool same_update, bool refreshed) {
  if (!AccOn()) return;
  Accuracy& a = g_acc;
  ++a.samples;
  if (same_update) ++a.samples_same;
  if (refreshed) ++a.refreshes;
}

}  // namespace cod3::ai_shots

// 0x823DB830(actor r3, accuracy f1, ..., r8 its eye out, r9 the aim point out): aim model 1.
REX_EXTERN(__imp__sub_823DB830);
REX_HOOK_RAW(sub_823DB830) {
  if (!On() && !SpawnsOn() && !AccOn()) {  // [r2] [r3] the shot's target for cod3_debug_ai_spawns / _accuracy too
    __imp__sub_823DB830(ctx, base);
    return;
  }
  const uint32_t actor = ctx.r3.u32, point_out = ctx.r9.u32;
  const float accuracy = float(ctx.f1.f64);
  const int32_t level_time = g::ReadS32(base, kLevelTime);
  const int32_t stored = actor >= 0x10000 ? g::ReadS32(base, actor + kActorStoredTime) : 0;
  // [r3] At the player: what made up the radius (0x823DA738, 0x823D8828, 0x823D7D58, 0x823D8018 inside this call).
  const uint32_t radius_out = ctx.r5.u32, most_out = ctx.r6.u32, eye_out = ctx.r8.u32;
  const uint32_t player = AccOn() ? PlayerEntity(base) : 0;
  const bool at_player = player && TargetEntity(base, actor) == player;
  if (at_player) {
    t_acc = AccShot{};
    t_acc.in_aim = true;
    t_acc.shooter = ActorEntity(base, actor);
    t_acc.level_time = level_time;
  }
  __imp__sub_823DB830(ctx, base);
  if (at_player) {
    t_acc.in_aim = false;
    NoteAim(base, actor, player, accuracy, eye_out, point_out, radius_out, most_out);
  }
  Decision d;
  d.model = 1;
  d.shooter = ActorEntity(base, actor);
  d.target = TargetEntity(base, actor);
  d.level_time = level_time;
  d.accuracy = accuracy;
  d.stored_age = level_time - stored;
  d.has_point = point_out >= 0x10000;
  if (d.has_point) d.point = ReadV3(base, point_out);
  t_decision = d;
  g_aim_point_any.fetch_add(1, std::memory_order_relaxed);
  if (d.target && d.target == PlayerEntity(base)) g_aim_point_player.fetch_add(1, std::memory_order_relaxed);
}

// 0x82550498(results r3, start r4, end r5, ...): the bullet's trace; only Bullet_Fire_Extended's first call, inside
// a followed shot.
REX_EXTERN(__imp__sub_82550498);
REX_HOOK_RAW(sub_82550498) {
  Shot& s = t_shot;
  AnyShot& a = t_any;  // [r2] any AI shot, for cod3_debug_ai_spawns
  const bool from_bullet = uint32_t(ctx.lr) == kTraceFromBullet;
  const bool follow = s.active && !s.traced && from_bullet, any = a.active && !a.traced && from_bullet;
  if (!follow && !any) {
    __imp__sub_82550498(ctx, base);
    return;
  }
  const uint32_t results = ctx.r3.u32;
  if (follow) {
    s.start = ReadV3(base, ctx.r4.u32);
    s.end = ReadV3(base, ctx.r5.u32);
  }
  __imp__sub_82550498(ctx, base);
  if (results < 0x10000) return;
  if (any) {
    a.traced = true;
    a.trace_end = ReadV3(base, results + kTraceEnd);
  }
  if (!follow) return;
  s.traced = true;
  s.trace_end = ReadV3(base, results + kTraceEnd);
  for (int i = 0; i < 8; ++i) s.raw[i] = g::ReadU32(base, results + kTraceRaw + 4 * i);  // [r1]
}

// [r1] sub_824AE908(r3 entity handle, r4 a position, ...): the only function found that writes an actor's stored
// aim time and point (+268, +272..+280) together. Counted, and its writes logged.
REX_EXTERN(__imp__sub_824AE908);
REX_HOOK_RAW(sub_824AE908) {
  if (!On()) {
    __imp__sub_824AE908(ctx, base);
    return;
  }
  const uint32_t handle = ctx.r3.u32, lr = uint32_t(ctx.lr);
  const uint32_t actor = ActorFromHandle(base, handle);
  const V3 arg = ctx.r4.u32 >= 0x10000 ? ReadV3(base, ctx.r4.u32) : V3{};
  const V3 look = ctx.r5.u32 >= 0x10000 ? ReadV3(base, ctx.r5.u32) : V3{};  // [r4] the point looked for
  const uint32_t extra = ctx.r7.u32;                                          // [r4] how many extra points
  const bool in_usercmd = t_in_usercmd > 0;
  const float f1 = float(ctx.f1.f64);
  const int32_t time0 = actor ? g::ReadS32(base, actor + kActorStoredTime) : 0;
  const V3 point0 = actor ? ReadV3(base, actor + kActorStoredPoint) : V3{};
  // The stack is walked before the call (the frames are gone after it), only for the first writes of a level and
  // at most 200 times in all (most calls write nothing).
  static std::atomic<int> stack_budget{200};
  std::string stack;
  const uint64_t seen = g_level_writes.load(std::memory_order_relaxed);
  if (actor && (seen < 5 || seen % 50 == 49) && stack_budget.fetch_sub(1) > 0)
    stack = cod3::watchdog::GuestCodeName(lr) + " <- " + cod3::watchdog::GuestStack(ctx, base);
  __imp__sub_824AE908(ctx, base);
  const bool answered = (ctx.r3.u32 & 0xFF) != 0;  // [r4] its answer: a point was seen
  {  // [r5] which caller: the helper's first or second try, or another
    gates::Counts& c = gates::g_counts;
    if (lr == gates::kFirstTry) {
      ++c.first;
      if (answered) ++c.first_seen;
    } else if (lr == gates::kSecondTry) {
      ++c.second;
      if (answered) ++c.second_seen;
    } else {
      ++c.other;
      if (answered) ++c.other_seen;
    }
  }
  Writer& w = g_writer;
  ++w.calls;
  if (in_usercmd) ++w.calls_in_usercmd;
  if (!actor) return;
  // [r4] 2026-11-02 for an actor targeting the player: the inputs and the answer. The distance gates are the
  // function's own: the squared distance eye -> point below the constant at 0x8207C614, or above level + 3040
  // (0x82A4F370), answers 0 at once (static read of sub_824AE908's start).
  {
    const uint32_t me = PlayerEntity(base);
    V3 feet, eye;
    if (me && TargetEntity(base, actor) == me && PlayerBody(base, me, &feet, &eye)) {
      ++w.pl_calls;
      if (answered) ++w.pl_yes;
      const V3 d = Sub(look, arg);
      const float d2 = Dot(d, d);
      if (d2 < g::ReadF32(base, 0x8207C614)) ++w.pl_too_near;
      else if (d2 > g::ReadF32(base, 0x82A4F370)) ++w.pl_too_far;
      ++w.pl_samples[extra == 0 ? 0 : extra == 3 ? 1 : extra == 5 ? 2 : 3];
      w.pl_f1_sum += f1;
      const uint32_t self = ActorEntity(base, actor);
      if (self) {
        const double up = arg.z - g::ReadF32(base, self + kEntOrigin + 8);
        w.pl_eye_up_sum += up;
        w.pl_eye_up_min = std::min(w.pl_eye_up_min, up);
      }
      w.pl_dist_sum += std::sqrt(d2);
      w.pl_point_up_sum += look.z - feet.z;
      w.pl_point_side_sum += std::sqrt((look.x - feet.x) * (look.x - feet.x) + (look.y - feet.y) * (look.y - feet.y));
      const double to_eye = Length(Sub(look, eye));
      w.pl_point_eye_sum += to_eye;
      w.pl_point_eye_max = std::max(w.pl_point_eye_max, to_eye);
      // [r6] 2026-11-02 UNTESTED by try, and his collision box / contents as the call found them
      Writer::Try& t = w.pl_try[lr == gates::kFirstTry ? 0 : lr == gates::kSecondTry ? 1 : 2];
      ++t.calls;
      if (answered) ++t.seen;
      t.up_sum += look.z - feet.z;
      t.f1_sum += f1;
      const float box_h = g::ReadF32(base, me + kEntMaxs + 8) - g::ReadF32(base, me + kEntMins + 8);
      t.box_h_sum += box_h;
      if (!(box_h >= 1.0f)) ++t.box_empty;
      if (!(g::ReadU32(base, me + kEntContents) & kSightTraceMask)) ++t.not_in_mask;
    }
  }
  const int32_t time1 = g::ReadS32(base, actor + kActorStoredTime);
  const V3 point1 = ReadV3(base, actor + kActorStoredPoint);
  if (time1 == time0 && Same(point1, point0)) return;
  const int32_t level_time = g::ReadS32(base, kLevelTime);
  const uint32_t player = PlayerEntity(base);
  const bool targets_player = player && TargetEntity(base, actor) == player;
  ++w.writes;
  if (in_usercmd) ++w.writes_in_usercmd;
  if (targets_player) ++w.writes_player;
  if (time1 == level_time) ++w.time_is_level;
  V3 feet, eye;
  if (!PlayerBody(base, player, &feet, &eye)) return;
  if (!targets_player && DistanceToBody(point1, feet, eye) > kNearWrite) return;
  if (level_time < g_last_write_level_time.exchange(level_time)) g_level_writes.store(0);
  const uint64_t n = g_level_writes.fetch_add(1) + 1;
  if (n > 20 && n % 50 != 0) return;
  REXLOG_INFO("cod3: ai stored point {} (level {} ms): actor of #{} (targets {}), {} usercmd; r4 {:.0f} units from his "
              "feet, {:.0f} from his eye; +268 {} -> {} ({}); the point {:.0f} units from r4, {:.0f} from his feet, "
              "{:.0f} from his eye; f1 {:.3f}{}{}",
              n, level_time, handle & 0x3FFF, targets_player ? "the player" : "another", in_usercmd ? "inside a" : "outside",
              Length(Sub(arg, feet)), Length(Sub(arg, eye)), time0, time1,
              time1 == level_time ? "level.time" : fmt::format("level.time {:+}", time1 - level_time),
              Length(Sub(point1, arg)), Length(Sub(point1, feet)), Length(Sub(point1, eye)), f1,
              stack.empty() ? "" : "; stack ", stack);
}

// [r2] G_Spawn (0x82543568): an entity allocated, by anyone. Counted, with its callers (cod3_debug_ai_spawns).
REX_EXTERN(__imp__G_Spawn);
REX_HOOK_RAW(G_Spawn) {
  if (SpawnsOn()) {
    g_g_spawns.fetch_add(1, std::memory_order_relaxed);
    if (t_in_usercmd > 0) g_g_spawns_usercmd.fetch_add(1, std::memory_order_relaxed);
    NoteSpawnCaller(uint32_t(ctx.lr));
  }
  __imp__G_Spawn(ctx, base);
}

// [r2] 0x8251A208(this r3, ...): the DroneSplineTask constructor (its vtable 0x820693D0 over the base's). Counted, and
// the task kept to see later whether it is still alive (cod3_debug_ai_spawns).
REX_EXTERN(__imp__sub_8251A208);
REX_HOOK_RAW(sub_8251A208) {
  const uint32_t task = ctx.r3.u32;
  __imp__sub_8251A208(ctx, base);
  if (!SpawnsOn()) return;
  g_drone_starts.fetch_add(1, std::memory_order_relaxed);
  if (t_in_usercmd > 0) g_drone_starts_usercmd.fetch_add(1, std::memory_order_relaxed);
  if (task < 0x10000) return;
  std::lock_guard lock(g_callers_mutex);
  if (g_drone_tasks.size() < kDroneTasksKept &&
      std::find(g_drone_tasks.begin(), g_drone_tasks.end(), task) == g_drone_tasks.end())
    g_drone_tasks.push_back(task);
}

// [r3] 0x823DA738 (accuracy fraction) from 0x823DB830 at the player: its result.
REX_EXTERN(__imp__sub_823DA738);
REX_HOOK_RAW(sub_823DA738) {
  const bool mine = t_acc.in_aim && uint32_t(ctx.lr) == kFractionFromAim;
  __imp__sub_823DA738(ctx, base);
  if (!mine) return;
  t_acc.has_fraction = true;
  t_acc.fraction = float(ctx.f1.f64);
}

// [r3] 0x823D8828(actor, the player's sentient) from 0x823DA738 there: his sideways speed as the AI reads it.
REX_EXTERN(__imp__sub_823D8828);
REX_HOOK_RAW(sub_823D8828) {
  const bool mine = t_acc.in_aim && uint32_t(ctx.lr) == kSidewaysFromFraction;
  __imp__sub_823D8828(ctx, base);
  if (!mine) return;
  t_acc.has_sideways = true;
  t_acc.sideways = float(ctx.f1.f64);
}

// [r3] 0x823D7D58(sentient, r4 position out, r5 time out) from 0x823D8828 there: the history sample it compares with,
// its age, and how far (horizontally) his origin is from it now.
REX_EXTERN(__imp__sub_823D7D58);
REX_HOOK_RAW(sub_823D7D58) {
  const bool mine = t_acc.in_aim && uint32_t(ctx.lr) == kHistoryFromSideways;
  const uint32_t position_out = ctx.r4.u32, time_out = ctx.r5.u32;
  __imp__sub_823D7D58(ctx, base);
  if (!mine || position_out < 0x10000 || time_out < 0x10000) return;
  const uint32_t player = PlayerEntity(base);
  if (!player) return;
  const V3 sample = ReadV3(base, position_out), now = ReadV3(base, player + kEntOrigin);
  t_acc.has_history = true;
  t_acc.elapsed = g::ReadS32(base, kLevelTime) - g::ReadS32(base, time_out);
  t_acc.moved = std::sqrt((now.x - sample.x) * (now.x - sample.x) + (now.y - sample.y) * (now.y - sample.y));
}

// [r3] 0x823D8018 (a sight check) from 0x823DA738 there: how many of his eye, middle and feet were seen.
REX_EXTERN(__imp__sub_823D8018);
REX_HOOK_RAW(sub_823D8018) {
  const uint32_t lr = uint32_t(ctx.lr);
  const bool mine = t_acc.in_aim &&
                    (lr == kSightFromFraction[0] || lr == kSightFromFraction[1] || lr == kSightFromFraction[2]);
  __imp__sub_823D8018(ctx, base);
  if (!mine) return;
  ++t_acc.sight_calls;
  if (ctx.f1.f64 != 0.0) ++t_acc.sight_seen;
}

// [r3] 0x8251EA58 (may the shot be fired: a line check) from 0x823ED9B8: its answer.
REX_EXTERN(__imp__sub_8251EA58);
REX_HOOK_RAW(sub_8251EA58) {
  const bool mine = uint32_t(ctx.lr) == kGateFromShoot && AccOn();
  __imp__sub_8251EA58(ctx, base);
  if (mine) t_gate = (ctx.r3.u32 & 0xFF) != 0 ? 1 : 0;
}

// [r6] 2026-11-02 r5's four gate hooks (sub_824D33A8, sub_824DA0B8, sub_82482C30, sub_824AF658) are gone: ob31
// counted 0 calls from the helper in each (the slot -> function mapping was wrong).
