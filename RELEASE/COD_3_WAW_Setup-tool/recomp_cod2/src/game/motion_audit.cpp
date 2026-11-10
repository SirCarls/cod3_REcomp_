// cod2 - tests: how what is drawn moves from frame to frame. [acct2-cod2]
//
// Above 60 fps a moving thing must move a little in every drawn frame. If it
// moves only in some (the frames that bring a new server snapshot or a new
// movement step) it is seen to step; if it goes forward, back and forward
// again it jolts; a very long move in one frame is a teleport. This counts all
// three, for test runs at 60 and 250 fps (off by default):
//
//   cod2_debug_entity_steps  every N seconds two log lines:
//     "entity steps": per entity type (the byte at centity +0xA0, the game's
//       eType), the entities drawn, their frames, the frames they moved in,
//       "held" frames (no move between frames with moves, a pause of at most
//       120 ms), "reversals" (a move of more than 120 degrees against the one
//       before it), "uneven" (a speed more than twice or less than half the
//       frame before's) and jumps over 32 units, with the worst entity's
//       address. Read after CG_CalcEntityLerpPositions 0x82453DF8 (r3 = the
//       centity): the drawn origin at +0x148, as the game evaluated it for
//       this frame's time (cg.time 0x8272695C).
//       Also the frames drawn with no next snapshot (cg.nextSnap 0x82711DA8 =
//       0: interpolated entities stand still) and those whose time was past
//       the next snapshot's (its +4).
//     "player steps": the same for the player's view origin after the
//       client's prediction (the predicted playerState, origin +0x14), per
//       pm_type (+4), the stance bits of pm_flags (+12: 1 prone, 2 crouched,
//       4 mantling, 8 the next one, 0x20 on a ladder: Pmove then calls
//       PM_LadderMove 0x82468A70) and whether the player is on a turret
//       (eFlags +0x7C & 0x300, set by the turret's use: 0x100 prone, 0x200
//       crouched, 0x300 standing), plus the view's yaw (+0xC8) turning back
//       and forth. The line also gives cg_fov's value, so each
//       run's field of view is on record (owner 2026-11-07: the high-fps runs
//       use 100).
//   cod2_debug_entity_steps_exact  a frame counts as moving when the origin
//     changed at all, not only by more than 0.01 units. At 250 fps a slow
//     thing (under 2.5 units a second) moves less than 0.01 a frame and would
//     be counted held; a really held frame repeats the origin exactly.
//   Each count of held frames also says how many of them were "under the
//     float step": the move expected from the thing's recent speed (an
//     average over about 10 frames, held ones included) was under about half
//     the spacing of float values at that position (|coordinate| x 2^-24:
//     0.0024 units at 40,000), which rounding swallows. Those are a slow
//     mover far from the map's center, not a missed update.
//   And each count says how many held frames were "ended by a float-step
//     move": the move that ended the hold changed no axis by more than two
//     float steps at that position. A real stall ends with a normal-sized move
//     (the speed times the held time). Up to two entity holds per type and
//     period that end with a bigger move are logged as "hold example" lines
//     (type, frames held, the move, the float step, the speed before, the
//     position, and the server times of the snapshot drawn from when the hold
//     began and when it ended, with the next one's, to see whether holds line
//     up with snapshots; build 19).
//
// Only the campaign for now; cod2mp's addresses differ.

#include "game/motion_audit.h"

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <unordered_map>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(cod2_debug_entity_steps, 0, "cod2/Debug",
                     "Tests: every this many seconds, log how entities and the player's view moved from "
                     "frame to frame (held frames, reversals, jumps; 0: off)")
    .range(0, 600);
REXCVAR_DEFINE_BOOL(cod2_debug_entity_steps_exact, false, "cod2/Debug",
                    "Tests: in the motion audit's counts (entities and the player's view), any change of the "
                    "origin is a move, not only over 0.01 units, so slow movers at high frame rates "
                    "aren't counted as held");

#if !defined(COD2_MP)

namespace {

constexpr uint32_t kCgTime = 0x8272695Cu;            // cg.time (ms), the time the frame draws
constexpr uint32_t kCgSnap = 0x82711DA4u;            // cg.snap (the snapshot drawn from)
constexpr uint32_t kCgNextSnap = 0x82711DA8u;        // cg.nextSnap (0: none to move toward)
constexpr uint32_t kPredictedEntity = 0x8272EF28u;   // cg.predictedPlayerEntity (the player's own)
constexpr uint32_t kPredictionPmove = 0x82870288u;   // its +0 is the predicted playerState
constexpr uint32_t kFovDvar = 0x827F6DD0u;           // the cg_fov dvar (CG_RegisterDvars 0x8244C588)
constexpr uint32_t kDvarValue = 8;                   // a float dvar's current value
constexpr uint32_t kCentOrigin = 0x148;              // centity: the drawn origin
constexpr uint32_t kCentType = 0xA0;                 // centity: the entity type (a byte)
constexpr uint32_t kPsType = 4, kPsFlags = 12, kPsOrigin = 0x14, kPsEFlags = 0x7C, kPsYaw = 0xC8;
constexpr uint32_t kStanceBits = 0x2F;              // pm_flags: prone, crouched, mantling, 8, ladder
constexpr uint32_t kPmfLadder = 0x20;
constexpr uint32_t kEfTurret = 0x300;               // eFlags: on a turret (and its stance)
constexpr int kTypes = 17;                           // 0-15, and 16 for anything above
constexpr float kMoved = 0.01f;                      // units: less is standing still
constexpr float kTurned = 0.01f;                     // degrees
constexpr float kJump = 32.0f;                       // units in one frame
constexpr float kFloatStep = 1.0f / 16777216.0f;     // 2^-24: about half a float's step, relative
constexpr int kMaxPauseMs = 120;                     // a longer pause is a stop, not a held frame
constexpr int kMaxGapMs = 250;                       // not drawn for longer: start again

uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}

float LoadF32(const uint8_t* base, uint32_t address) {
  const uint32_t v = Load32(base, address);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

struct Counts {
  uint64_t tracks = 0, frames = 0, moving = 0, held = 0, holds = 0, reversals = 0, uneven = 0,
           jumps = 0, yaw_reversals = 0, float_held = 0, step_ended_held = 0;
};

// One drawn thing followed from frame to frame (game thread only).
struct Track {
  int32_t pending_time = 0;  // the newest frame's time and origin (the last call that frame)
  float pending[3] = {};
  float pending_yaw = 0.0f;
  bool has_pending = false;
  bool has_last = false;     // the frame before
  int32_t last_time = 0;
  float last[3] = {};
  float last_move[3] = {};
  float last_dist = 0.0f, last_speed = 0.0f;
  float speed_avg = 0.0f;    // units per ms, averaged over about 10 frames (held ones too)
  bool moved_last = false;
  bool can_hold = false;     // it moved, so a frame without a move is held
  int held_run = 0;
  int held_run_float = 0;    // of held_run: frames whose expected move was under the float step
  int32_t hold_start = 0;
  int32_t hold_snap = 0;     // cg.snap's server time when the hold began
  float last_yaw = 0.0f, last_turn = 0.0f;
  uint32_t period = 0;       // the log period it was last counted in
  uint64_t period_held = 0, period_reversals = 0, period_jumps = 0;
  int type = 0;
};

uint32_t g_period = 1;
constexpr int kExamplesPerType = 2;
Counts g_types[kTypes];
int g_examples[kTypes] = {};  // "hold example" lines this period, per entity type
std::unordered_map<uint32_t, Track> g_entities;  // by centity address
std::unordered_map<uint32_t, Counts> g_player;   // by pm_type << 16 | turret bits << 8 | stance bits
Track g_view;
int32_t g_frame_time = 0;
uint64_t g_frames = 0;
// Frames drawn with no next snapshot (entities held at the current one), and
// frames whose time was past the next snapshot's (moved beyond it).
uint64_t g_no_next = 0, g_past_next = 0;
// The server times of cg.snap and cg.nextSnap at the last frame (0: none).
int32_t g_snap_time = 0, g_next_snap_time = 0;
float g_fov = 0.0f;  // cg_fov's value at the last frame (0: not registered yet)
int64_t g_log_start_ns = 0;

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

// The step from the track's last frame to its pending one, counted in c.
void Advance(Track& t, Counts& c, bool with_yaw) {
  const float yaw = t.pending_yaw;
  if (t.period != g_period) {
    t.period = g_period;
    t.period_held = t.period_reversals = t.period_jumps = 0;
    ++c.tracks;
  }
  const int32_t dt = t.pending_time - t.last_time;
  if (!t.has_last || dt <= 0 || dt > kMaxGapMs) {
    t.has_last = true;
    t.last_time = t.pending_time;
    std::memcpy(t.last, t.pending, sizeof t.last);
    t.moved_last = t.can_hold = false;
    t.held_run = t.held_run_float = 0;
    t.speed_avg = 0.0f;
    t.last_yaw = yaw;
    t.last_turn = 0.0f;
    return;
  }
  ++c.frames;
  float d[3];
  for (int i = 0; i < 3; ++i) d[i] = t.pending[i] - t.last[i];
  const float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
  const float moved = REXCVAR_GET(cod2_debug_entity_steps_exact) ? 0.0f : kMoved;
  if (dist > moved) {
    ++c.moving;
    if (t.held_run > 0 && t.can_hold) {
      c.held += uint64_t(t.held_run);
      c.float_held += uint64_t(t.held_run_float);
      ++c.holds;
      t.period_held += uint64_t(t.held_run);
      // The move that ended the hold: at most two float steps on every axis is
      // rounding (a slow mover's origin can only change by whole steps).
      float worst_axis = 0.0f, worst_step = 0.0f;
      bool tiny = true;
      for (int i = 0; i < 3; ++i) {
        const float step = std::fabs(
            std::nextafter(t.pending[i], std::numeric_limits<float>::infinity()) - t.pending[i]);
        if (std::fabs(d[i]) > 2.0f * step) tiny = false;
        if (std::fabs(d[i]) > worst_axis) {
          worst_axis = std::fabs(d[i]);
          worst_step = step;
        }
      }
      if (tiny) {
        c.step_ended_held += uint64_t(t.held_run);
      } else if (!with_yaw && t.type >= 0 && t.type < kTypes && g_examples[t.type] < kExamplesPerType) {
        ++g_examples[t.type];
        REXLOG_INFO("cod2: entity steps: hold example: type {}, {} frames held ({} ms), then a move of {:.4f} "
                    "units (largest axis {:.4f}, float step there {:.5f}), speed before {:.2f} units/s, at "
                    "({:.1f}, {:.1f}, {:.1f}); held from {} to {} ms, snapshot {} ms when it began, {} ms "
                    "now (next {} ms)",
                    t.type, t.held_run, t.pending_time - t.hold_start, dist, worst_axis, worst_step,
                    t.last_speed * 1000.0f, t.pending[0], t.pending[1], t.pending[2], t.hold_start,
                    t.pending_time, t.hold_snap, g_snap_time, g_next_snap_time);
      }
    }
    t.held_run = t.held_run_float = 0;
    const float speed = dist / float(dt);
    if (t.moved_last && dist > 0.05f && t.last_dist > 0.05f) {
      const float dot = d[0] * t.last_move[0] + d[1] * t.last_move[1] + d[2] * t.last_move[2];
      if (dot < -0.5f * dist * t.last_dist) {
        ++c.reversals;
        ++t.period_reversals;
      }
      if (speed > 2.0f * t.last_speed || 2.0f * speed < t.last_speed) ++c.uneven;
    }
    if (dist > kJump) {
      ++c.jumps;
      ++t.period_jumps;
    }
    std::memcpy(t.last_move, d, sizeof d);
    t.last_dist = dist;
    t.last_speed = speed;
    t.moved_last = t.can_hold = true;
  } else {
    if (t.can_hold) {
      if (t.held_run == 0) {
        t.hold_start = t.last_time;
        t.hold_snap = g_snap_time;
      }
      ++t.held_run;
      // About half the float spacing at this position: a smaller move rounds away.
      const float big = std::fmax(std::fabs(t.pending[0]), std::fmax(std::fabs(t.pending[1]),
                                                                     std::fabs(t.pending[2])));
      if (t.speed_avg * float(dt) < big * kFloatStep) ++t.held_run_float;
      if (t.pending_time - t.hold_start > kMaxPauseMs) {  // it stopped
        t.can_hold = false;
        t.held_run = t.held_run_float = 0;
      }
    }
    t.moved_last = false;
  }
  if (with_yaw) {
    float turn = yaw - t.last_yaw;
    if (turn > 180.0f) turn -= 360.0f;
    if (turn < -180.0f) turn += 360.0f;
    if (std::fabs(turn) > kTurned && std::fabs(t.last_turn) > kTurned && (turn > 0) != (t.last_turn > 0))
      ++c.yaw_reversals;
    t.last_turn = turn;
    t.last_yaw = yaw;
  }
  t.speed_avg = 0.9f * t.speed_avg + 0.1f * (dist / float(dt));
  t.last_time = t.pending_time;
  std::memcpy(t.last, t.pending, sizeof t.last);
}

std::string CountsText(const Counts& c) {
  std::string s = std::to_string(c.frames) + " frames, " + std::to_string(c.moving) + " moving, " +
                  std::to_string(c.held) + " held (" + std::to_string(c.holds) + " holds), " +
                  std::to_string(c.reversals) + " reversals, " + std::to_string(c.uneven) +
                  " uneven, " + std::to_string(c.jumps) + " jumps";
  return s;
}

void MaybeLog() {
  const int seconds = REXCVAR_GET(cod2_debug_entity_steps);
  if (seconds <= 0) return;
  const int64_t now = NowNs();
  if (g_log_start_ns == 0) {
    g_log_start_ns = now;
    return;
  }
  if (now - g_log_start_ns < int64_t(seconds) * 1000000000LL) return;
  const double s = double(now - g_log_start_ns) / 1e9;

  // The worst entity of each type (most held frames and reversals).
  uint32_t worst[kTypes] = {};
  uint64_t worst_score[kTypes] = {};
  const Track* worst_track[kTypes] = {};
  for (const auto& [address, t] : g_entities) {
    if (t.period != g_period) continue;
    const uint64_t score = t.period_held + 4 * t.period_reversals + 4 * t.period_jumps;
    if (score > worst_score[t.type]) {
      worst_score[t.type] = score;
      worst[t.type] = address;
      worst_track[t.type] = &t;
    }
  }
  std::string types;
  for (int i = 0; i < kTypes; ++i) {
    const Counts& c = g_types[i];
    if (c.tracks == 0) continue;
    types += "; type " + std::to_string(i) + ": " + std::to_string(c.tracks) + " drawn, " +
             CountsText(c) + ", " + std::to_string(c.float_held) + " held under the float step, " +
             std::to_string(c.step_ended_held) + " ended by a float-step move";
    if (worst_track[i]) {
      char text[96];
      std::snprintf(text, sizeof text, ", worst 0x%08X (%llu held, %llu reversals, %llu jumps)",
                    worst[i], static_cast<unsigned long long>(worst_track[i]->period_held),
                    static_cast<unsigned long long>(worst_track[i]->period_reversals),
                    static_cast<unsigned long long>(worst_track[i]->period_jumps));
      types += text;
    }
  }
  REXLOG_INFO("cod2: entity steps: {:.1f} s ({}), {} frames ({} with no next snapshot, {} past it){}", s,
              REXCVAR_GET(cod2_debug_entity_steps_exact) ? "moving: any change" : "moving: over 0.01 units",
              g_frames, g_no_next, g_past_next, types.empty() ? std::string("; nothing drawn") : types);

  std::string player;
  for (const auto& [key, c] : g_player) {
    if (c.frames == 0) continue;
    char head[96];
    const uint32_t stance = key & 0xFFu, turret = (key >> 8) & 0xFFu;
    std::snprintf(head, sizeof head, "; pm_type %u stance 0x%X%s%s: ", key >> 16, stance,
                  (stance & kPmfLadder) ? " (ladder)" : "",
                  turret ? (turret == 1 ? " (turret, prone)" : turret == 2 ? " (turret, crouched)" : " (turret)")
                         : "");
    player += head + CountsText(c) + ", " + std::to_string(c.yaw_reversals) + " yaw reversals, " +
              std::to_string(c.float_held) + " held under the float step, " +
              std::to_string(c.step_ended_held) + " ended by a float-step move";
  }
  REXLOG_INFO("cod2: player steps: {:.1f} s, cg_fov {:.1f}{}", s, g_fov,
              player.empty() ? std::string("; no prediction") : player);

  for (Counts& c : g_types) c = {};
  g_player.clear();
  g_frames = 0;
  g_no_next = g_past_next = 0;
  std::memset(g_examples, 0, sizeof g_examples);
  ++g_period;
  g_log_start_ns = now;
  // Forget what was not drawn in the last period.
  for (auto it = g_entities.begin(); it != g_entities.end();) {
    if (g_period - it->second.period > 2)
      it = g_entities.erase(it);
    else
      ++it;
  }
}

// Once a frame (a new cg.time): the frame count, and the log when it is due.
void NoteFrame(const uint8_t* base, int32_t time) {
  if (time != g_frame_time) {
    g_frame_time = time;
    ++g_frames;
    // The snapshots this frame draws between (a snapshot's +4 is its server time).
    const uint32_t next = Load32(base, kCgNextSnap);
    const uint32_t snap = Load32(base, kCgSnap);
    g_snap_time = snap != 0 ? static_cast<int32_t>(Load32(base, snap + 4)) : 0;
    g_next_snap_time = next != 0 ? static_cast<int32_t>(Load32(base, next + 4)) : 0;
    if (next == 0) {
      ++g_no_next;
    } else if (Load32(base, kCgSnap) != 0 && time > static_cast<int32_t>(Load32(base, next + 4))) {
      ++g_past_next;
    }
    const uint32_t fov = Load32(base, kFovDvar);
    if (fov != 0) g_fov = LoadF32(base, fov + kDvarValue);
    MaybeLog();
  }
}

}  // namespace

namespace cod2::motion_audit {

void OnPlayerPredicted(const uint8_t* base) {
  if (REXCVAR_GET(cod2_debug_entity_steps) <= 0) return;
  const uint32_t ps = Load32(base, kPredictionPmove);
  if (ps == 0) return;
  const int32_t time = static_cast<int32_t>(Load32(base, kCgTime));
  NoteFrame(base, time);
  // The frame before is complete now (a second prediction in the same frame
  // replaces the first).
  if (g_view.has_pending && g_view.pending_time != time)
    Advance(g_view, g_player[uint32_t(g_view.type)], true);
  g_view.has_pending = true;
  g_view.pending_time = time;
  for (int i = 0; i < 3; ++i) g_view.pending[i] = LoadF32(base, ps + kPsOrigin + 4 * i);
  g_view.pending_yaw = LoadF32(base, ps + kPsYaw);
  g_view.type = static_cast<int>(((Load32(base, ps + kPsType) & 0xFFu) << 16) |
                                 (Load32(base, ps + kPsEFlags) & kEfTurret) |
                                 (Load32(base, ps + kPsFlags) & kStanceBits));
}

}  // namespace cod2::motion_audit

REX_EXTERN(__imp__sub_82453DF8);  // CG_CalcEntityLerpPositions
REX_HOOK_RAW(sub_82453DF8) {
  const uint32_t cent = ctx.r3.u32;
  __imp__sub_82453DF8(ctx, base);
  if (REXCVAR_GET(cod2_debug_entity_steps) <= 0 || cent == kPredictedEntity || cent == 0) return;
  const int32_t time = static_cast<int32_t>(Load32(base, kCgTime));
  NoteFrame(base, time);
  Track& t = g_entities[cent];
  if (t.has_pending && t.pending_time != time) Advance(t, g_types[t.type], false);
  t.has_pending = true;
  t.pending_time = time;
  for (int i = 0; i < 3; ++i) t.pending[i] = LoadF32(base, cent + kCentOrigin + 4 * i);
  const int type = base[cent + kCentType];
  t.type = type < kTypes - 1 ? type : kTypes - 1;
}

#endif  // !COD2_MP
