// cod2 - the player's movement in fixed steps (cod2_move_hz). [acct2-cod2]
//
// The game moves the player once per usercmd by that command's milliseconds:
// Pmove 0x82469C18 runs PmoveSingle 0x82469290 in pieces of at most 66 ms until
// the playerState's commandTime reaches the command's serverTime. One command
// is made per drawn frame, so jump heights, speeds and how the player meets
// stairs and walls change with the frame rate (the PC game's "125/250/333 fps
// jumps"); the Xbox 360 hid it at its 60 fps. Pmove has two callers, the
// server's client think and the client's prediction (the global pmove
// 0x82870288), so one hook here keeps both in agreement.
//
//   cod2_move_hz  0-250, default 60 (the 360's own rate; owner's choice open):
//                 steps of 1/N s that end at fixed times of the game's clock
//                 (16, 17, 17 ms at 60), one PmoveSingle per step: none, one
//                 or several in a command. The game's own loop runs each step:
//                 Pmove is called with commandTime at the step's start and
//                 serverTime at its end. Afterwards both are the command's time
//                 again, so the rest of the game sees what it always did; the
//                 next command goes on from the last step's end. A command
//                 with no step end in it still turns the view
//                 (PM_UpdateViewAngles 0x82465D10). 0: the game's own steps.
//
// Above the step rate the player's position would change in steps, so the view
// is placed between the last two steps (one step behind at most; the view
// angles are never delayed): after the client's prediction
// (CG_PredictPlayerState_Internal 0x824460C8) the predicted origin becomes the
// last step's start plus the share of that step the newest command's time has
// reached, and the true origin goes back before the next prediction, so the
// game's own movement never sees the shown one. Not after a step that moved
// more than 64 units beyond what the player's speed covers in it (a teleport,
// a spawn); a long fall is still smoothed.
//
// The design is Call of Duty 3's cod3_move_hz (recomp/src/game/movement/
// hooks_sp.cpp: RunHzSteps and its CG_CalcViewValues hook). Still to come for
// CoD2: cod2mp's Pmove. The checks of every position and mover: motion_audit.cpp.
//
// The pmove_t (from Pmove's code): +0 the playerState (its +0 commandTime, +12
// pm_flags), +4 the usercmd (its +0 serverTime), +52 the previous usercmd (12
// words, copied after each step by the game's loop), +264 the byte
// PM_UpdateViewAngles takes last. PM_UpdateViewAngles is called as
// (r3 playerState, f1 msec, r5 usercmd, r6 that byte). The playerState's
// origin is at +0x14 (PmoveSingle keeps +0x14..+0x28, origin and velocity, as
// the previous ones).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/motion_audit.h"

REXCVAR_DEFINE_INT32(cod2_move_hz, 60, "cod2/Game",
                     "The player's movement in fixed steps of 1/N s, so jumps, speeds and collisions are "
                     "the same at any frame rate (60: the Xbox 360's); 0: one step a frame, as the game")
    .range(0, 250);
REXCVAR_DEFINE_INT32(cod2_debug_movement, 0, "cod2/Debug",
                     "Tests: every this many seconds, log the movement steps per usercmd (0: off)")
    .range(0, 600);

#if !defined(COD2_MP)

namespace {

constexpr uint32_t kPredictionPmove = 0x82870288u;  // the client's prediction's pmove_t
constexpr uint32_t kPmCmd = 4;                       // pmove_t: the usercmd
constexpr uint32_t kPmViewByte = 264;                // pmove_t: PM_UpdateViewAngles' last argument
constexpr uint32_t kPsCommandTime = 0;               // playerState_t
constexpr uint32_t kPsOrigin = 0x14;                 // playerState_t: vec3
constexpr uint32_t kPsVelocity = 0x20;               // playerState_t: vec3 (units a second)
constexpr int kMaxSpan = 1000;                       // Pmove keeps at most this much time
constexpr float kMaxStepMove = 64.0f;                // more in one step, beyond the velocity's share: a teleport

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, 4);
}

float LoadF32(const uint8_t* base, uint32_t address) {
  const uint32_t v = Load32(base, address);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

void StoreF32(uint8_t* base, uint32_t address, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  Store32(base, address, v);
}

void ReadOrigin(const uint8_t* base, uint32_t ps, float o[3]) {
  for (int i = 0; i < 3; ++i) o[i] = LoadF32(base, ps + kPsOrigin + 4 * i);
}

void WriteOrigin(uint8_t* base, uint32_t ps, const float o[3]) {
  for (int i = 0; i < 3; ++i) StoreF32(base, ps + kPsOrigin + 4 * i, o[i]);
}

// The prediction's last step, for the view between steps (game thread only).
struct LastStep {
  bool valid = false;
  uint32_t ps = 0;     // the predicted playerState
  float from[3] = {};  // its origin before the last step
  float to[3] = {};    // and after it
  int step_end = 0;    // that step's end (game ms)
  int step_len = 1;
  int now = 0;         // the newest command's time
  bool shown = false;  // the shown origin is in the playerState now
  float true_origin[3] = {};
};
LastStep g_last;

// Step k ends at floor(k * 1000 / hz) ms of the game's clock.
int StepEnd(int64_t k, int hz) { return static_cast<int>(k * 1000 / hz); }

// The last step end at or before t.
int64_t StepAtOrBefore(int t, int hz) {
  int64_t k = static_cast<int64_t>(t) * hz / 1000;
  while (StepEnd(k + 1, hz) <= t) ++k;
  while (k > 0 && StepEnd(k, hz) > t) --k;
  return k;
}

// The test's counters (cod2_debug_movement), per caller: 0 the server, 1 the prediction.
struct Counts {
  uint64_t commands = 0, steps = 0, none = 0, several = 0;
};
Counts g_counts[2];
int64_t g_log_start_ns = 0;

// How often the view was placed between steps after a prediction, and why not
// (cod2_debug_movement): the player had moved after the last step (something
// after Pmove changed the predicted origin, e.g. the prediction's error decay),
// the newest command's time was outside the last step, or there was no step
// to use (none yet, or a jump of more than 64 units).
struct ShowCounts {
  uint64_t shown = 0, moved = 0, outside = 0, none = 0;
  float most_moved = 0.0f;  // the largest distance between the origin and the last step's end
};
ShowCounts g_show;

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

void MaybeLog(int hz) {
  const int seconds = REXCVAR_GET(cod2_debug_movement);
  if (seconds <= 0) return;
  const int64_t now = NowNs();
  if (g_log_start_ns == 0) {
    g_log_start_ns = now;
    return;
  }
  if (now - g_log_start_ns < int64_t(seconds) * 1000000000LL) return;
  const double s = double(now - g_log_start_ns) / 1e9;
  const Counts& sv = g_counts[0];
  const Counts& cl = g_counts[1];
  REXLOG_INFO(
      "cod2: movement: {:.1f} s at 1/{} s steps; server {} usercmds, {} steps ({} with none, {} with "
      "more than one); prediction {} usercmds, {} steps ({} with none, {} with more than one); view "
      "between steps {} times, not {} (moved after the step {}, up to {:.2f} units; outside the step {}; "
      "no step {})",
      s, hz, sv.commands, sv.steps, sv.none, sv.several, cl.commands, cl.steps, cl.none, cl.several,
      g_show.shown, g_show.moved + g_show.outside + g_show.none, g_show.moved, g_show.most_moved,
      g_show.outside, g_show.none);
  g_counts[0] = {};
  g_counts[1] = {};
  g_show = {};
  g_log_start_ns = now;
}

}  // namespace

REX_EXTERN(__imp__sub_82469C18);  // Pmove
REX_EXTERN(__imp__sub_82465D10);  // PM_UpdateViewAngles

REX_HOOK_RAW(sub_82469C18) {
  const int hz = std::clamp(REXCVAR_GET(cod2_move_hz), 0, 250);
  const uint32_t pm = ctx.r3.u32;
  const uint32_t ps = Load32(base, pm);
  const uint32_t cmd = pm + kPmCmd;
  const int final_time = static_cast<int>(Load32(base, cmd));
  const int command_time = static_cast<int>(Load32(base, ps + kPsCommandTime));
  if (hz <= 0 || ps == 0 || final_time <= command_time || command_time < 0) {
    // The game's own steps, or nothing to move (an older command).
    __imp__sub_82469C18(ctx, base);
    return;
  }

  // Go on from the last step end at or before the command's start: the
  // previous command left the player there.
  int start = StepEnd(StepAtOrBefore(command_time, hz), hz);
  if (final_time - start > kMaxSpan) start = final_time - kMaxSpan;

  // A call changes the argument registers; each step gets the caller's again.
  const PPCRegister r3 = ctx.r3, r4 = ctx.r4, r5 = ctx.r5, r6 = ctx.r6;
  auto restore = [&] {
    ctx.r3 = r3;
    ctx.r4 = r4;
    ctx.r5 = r5;
    ctx.r6 = r6;
  };

  const bool prediction = pm == kPredictionPmove;
  float before[3] = {};
  int steps = 0, t = start, last_len = 1;
  for (int64_t k = StepAtOrBefore(t, hz) + 1;; ++k) {
    const int end = StepEnd(k, hz);
    if (end > final_time) break;
    if (end <= t) continue;
    if (prediction) ReadOrigin(base, ps, before);
    Store32(base, ps + kPsCommandTime, static_cast<uint32_t>(t));
    Store32(base, cmd, static_cast<uint32_t>(end));
    restore();
    __imp__sub_82469C18(ctx, base);  // the game's loop runs exactly this step
    last_len = end - t;
    t = end;
    ++steps;
  }
  if (prediction) {
    if (steps > 0) {
      for (int i = 0; i < 3; ++i) g_last.from[i] = before[i];
      ReadOrigin(base, ps, g_last.to);
      const float dx = g_last.to[0] - before[0], dy = g_last.to[1] - before[1],
                  dz = g_last.to[2] - before[2];
      // A step may move as far as the player's speed takes him (a long fall
      // reaches thousands of units a second), plus 64 units; more is a
      // teleport or a spawn, which is shown at once.
      float v[3];
      for (int i = 0; i < 3; ++i) v[i] = LoadF32(base, ps + kPsVelocity + 4 * i);
      const float speed = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
      const float allowed = kMaxStepMove + 2.0f * speed * float(std::max(last_len, 1)) / 1000.0f;
      g_last.valid = dx * dx + dy * dy + dz * dz < allowed * allowed;
      g_last.step_end = t;
      g_last.step_len = std::max(last_len, 1);
    }
    g_last.ps = ps;
    g_last.now = final_time;
  }
  if (steps == 0) {
    // No step ends in this command: Pmove with no time (its flag handling
    // still runs, its loop none), then the view turns to the command's angles.
    Store32(base, ps + kPsCommandTime, static_cast<uint32_t>(start));
    Store32(base, cmd, static_cast<uint32_t>(start));
    restore();
    __imp__sub_82469C18(ctx, base);
    ctx.r3.u64 = ps;
    ctx.f1.f64 = static_cast<double>(static_cast<float>(final_time - command_time));
    ctx.r5.u64 = cmd;
    ctx.r6.u64 = base[pm + kPmViewByte];
    __imp__sub_82465D10(ctx, base);
  }
  // The command's time again, as the game's own loop leaves it.
  Store32(base, ps + kPsCommandTime, static_cast<uint32_t>(final_time));
  Store32(base, cmd, static_cast<uint32_t>(final_time));
  restore();

  Counts& c = g_counts[prediction ? 1 : 0];
  ++c.commands;
  c.steps += static_cast<uint64_t>(steps);
  if (steps == 0) ++c.none;
  if (steps > 1) ++c.several;
  MaybeLog(hz);
}

namespace {

// After the client's prediction: the view's origin between the last two steps
// (see the top of this file). A prediction that runs no step keeps the last
// one; it applies only while the player is still where that step ended.
void ShowBetweenSteps(uint8_t* base) {
  if (REXCVAR_GET(cod2_move_hz) <= 0) return;
  if (!g_last.valid || g_last.ps == 0) {
    ++g_show.none;
    return;
  }
  const int into = g_last.now - g_last.step_end;
  if (into < 0 || into >= g_last.step_len) {
    ++g_show.outside;
    return;
  }
  float origin[3];
  ReadOrigin(base, g_last.ps, origin);
  // Only while the player is where that step left him (nothing else moved him).
  for (int i = 0; i < 3; ++i) {
    if (origin[i] != g_last.to[i]) {
      const float dx = origin[0] - g_last.to[0], dy = origin[1] - g_last.to[1],
                  dz = origin[2] - g_last.to[2];
      g_show.most_moved = std::max(g_show.most_moved, std::sqrt(dx * dx + dy * dy + dz * dz));
      ++g_show.moved;
      return;
    }
  }
  ++g_show.shown;
  const float share = float(into) / float(g_last.step_len);
  float shown[3];
  for (int i = 0; i < 3; ++i) shown[i] = g_last.from[i] + (g_last.to[i] - g_last.from[i]) * share;
  for (int i = 0; i < 3; ++i) g_last.true_origin[i] = origin[i];
  WriteOrigin(base, g_last.ps, shown);
  g_last.shown = true;
}

}  // namespace

// The client's prediction: the true origin back before it runs, the shown one
// after it, and the test's count of how the view moved (motion_audit.cpp).
REX_EXTERN(__imp__sub_824460C8);  // CG_PredictPlayerState_Internal
REX_HOOK_RAW(sub_824460C8) {
  if (g_last.shown) {
    WriteOrigin(base, g_last.ps, g_last.true_origin);
    g_last.shown = false;
  }
  __imp__sub_824460C8(ctx, base);
  ShowBetweenSteps(base);
  cod2::motion_audit::OnPlayerPredicted(base);
}

#endif  // !COD2_MP
