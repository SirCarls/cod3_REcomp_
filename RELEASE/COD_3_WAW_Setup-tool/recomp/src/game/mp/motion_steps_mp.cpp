// cod3mp - [acct2-cod3-motion] 2026-11-07 How the drawn entities and the camera move from one drawn frame to the next in
// multiplayer (owner 20:33: "You will likely need to look at MP as well. Check player movement in all positions for
// similar behaviour"). The multiplayer twin of the campaign's cod3_debug_entity_steps (game/server_step.cpp), for
// cod3mp's client game: CG_AddCEntity 0x8246B5B8 (the campaign's 0x8244CFB0, the same switch by type), each entity's
// scene node at entity +0x214 (0x8248FA50 makes it; the campaign's +0x224) with its origin at node +0x44 and its
// forward axis at node +0x1C, and the camera of each local player (cg 0x82C7D490 + client x 6368: origin +4960,
// forward +4976; hooks_mp.cpp's view namespace). cod3mp runs its game once a drawn frame (cod3mp_server_hz 0), so
// there is no "frame with an update" here: a thing that moves in steps shows as still frames between moves and a
// large spread of its speed from frame to frame. Per second: by entity type (16 = the camera) the moving ones, their
// still frames, the spread of their speed (100% = all over the place), reversals (moved back against the last
// frame's move: a jolt) and jumps (far more than its usual move in one frame); with N > 1 also the N worst. Split
// screen draws each entity once per local player: every local player's view is counted on its own. Counting only.

#if defined(COD3_MP)

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/guest.h"

REXCVAR_DEFINE_INT32(cod3mp_debug_entity_steps, 0, "cod3",
                     "Multiplayer: log once a second, by entity type (16 = each local player's camera), how the drawn "
                     "entities moved from frame to frame: still frames while moving, the spread of their speed, jolts "
                     "back and forth and jumps; N > 1: also the N worst. 0: off")
    .range(0, 64)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

namespace g = cod3::guest;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kActiveClient = 0x82BCBE9C;  // the local client whose view is being built
constexpr uint32_t kCg = 0x82C7D490, kCgSize = 6368, kCgViewOrigin = 4960, kCgViewAxis = 4976;
constexpr uint32_t kEntFlags = 16;
constexpr uint32_t kEntSceneNode = 0x214;
constexpr uint32_t kNodeOrigin = 0x44, kNodeAxis = 0x1C;
constexpr uint8_t kCameraType = 16;
constexpr float kMoving = 5.0f;     // units a second
constexpr float kReverse = 0.05f;   // the smallest move a reversal is counted for

bool ValidGuest(uint32_t p) { return p >= 0x10000 && (p & 3) == 0; }
float Len(const float v[3]) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

struct Steps {
  uint8_t type = 0;
  uint64_t frame = 0;
  float origin[3] = {}, fwd[3] = {}, move[3] = {};
  bool have_move = false;
  double rate = 0.0;  // its usual units a millisecond (a slow average)
  uint32_t samples = 0;
  // this second
  uint32_t n = 0, still = 0, reversals = 0, jumps = 0;
  double ms = 0.0, dist = 0.0, turn = 0.0, v_sum = 0.0, v_sq = 0.0;
  float max_jump = 0.0f;
};
std::unordered_map<uint64_t, Steps> g_steps;  // by (entity address or local player) x 4 + local player
uint64_t g_frame[4] = {};                     // drawn frames of each local player
double g_frame_ms[4] = {};                    // their last frame's real milliseconds
Clock::time_point g_frame_at[4] = {};
Clock::time_point g_since = Clock::now();

uint32_t LocalClient(uint8_t* base) {
  const uint32_t c = g::ReadU32(base, kActiveClient);
  return c < 4 ? c : 0;
}

void Sample(uint64_t key, uint8_t type, uint32_t local, const float o[3], const float f[3]) {
  for (int i = 0; i < 3; ++i)
    if (!std::isfinite(o[i]) || !std::isfinite(f[i])) return;
  Steps& s = g_steps[key];
  if (s.type != type) {
    s = Steps{};
    s.type = type;
  }
  const double ms = g_frame_ms[local];
  if (s.frame != 0 && s.frame + 1 == g_frame[local] && ms > 0.0) {
    const float m[3] = {o[0] - s.origin[0], o[1] - s.origin[1], o[2] - s.origin[2]};
    const float d = Len(m);
    float turn = 0.0f;
    const float fl = Len(f), sl = Len(s.fwd);
    if (fl > 0.0f && sl > 0.0f) {
      const float c = (f[0] * s.fwd[0] + f[1] * s.fwd[1] + f[2] * s.fwd[2]) / (fl * sl);
      turn = std::acos(std::clamp(c, -1.0f, 1.0f)) * 57.29578f;
    }
    if (d > 32.0f && d > (s.samples >= 30 ? 3.0 * s.rate * ms + 16.0 : 128.0)) {  // a jump: a teleport or a cut
      ++s.jumps;
      s.max_jump = std::max(s.max_jump, d);
      s.have_move = false;
    } else {
      ++s.n;
      s.ms += ms;
      s.dist += d;
      s.turn += turn;
      const double v = d / ms;
      s.v_sum += v;
      s.v_sq += v * v;
      if (d < 0.001f) ++s.still;
      if (s.have_move) {
        const float last = Len(s.move);
        const float dot = m[0] * s.move[0] + m[1] * s.move[1] + m[2] * s.move[2];
        if (d > kReverse && last > kReverse && dot < -0.5f * d * last) ++s.reversals;
      }
      s.rate = s.samples ? 0.95 * s.rate + 0.05 * v : v;
      ++s.samples;
      for (int i = 0; i < 3; ++i) s.move[i] = m[i];
      s.have_move = true;
    }
  } else {
    s.have_move = false;
  }
  for (int i = 0; i < 3; ++i) {
    s.origin[i] = o[i];
    s.fwd[i] = f[i];
  }
  s.frame = g_frame[local];
}

float Speed(const Steps& s) { return s.ms > 0.0 ? float(s.dist * 1000.0 / s.ms) : 0.0f; }
// The spread of its speed from frame to frame, against its mean (0 even, 1 or more: all over the place).
float Spread(const Steps& s) {
  if (s.n < 2) return 0.0f;
  const double mean = s.v_sum / s.n;
  const double var = std::max(0.0, s.v_sq / s.n - mean * mean);
  return mean > 0.0 ? float(std::sqrt(var) / mean) : 0.0f;
}

void Log() {
  const int setting = REXCVAR_GET(cod3mp_debug_entity_steps);
  struct Sum {
    uint32_t drawn = 0, moving = 0, still = 0, frames = 0, reversals = 0, jumps = 0, uneven = 0;
    double speed = 0.0, spread = 0.0;
    float max_jump = 0.0f;
  };
  Sum t[kCameraType + 1];
  struct Worst {
    float score;
    uint64_t key;
  };
  std::vector<Worst> worst;
  uint64_t newest = 0;
  for (uint64_t f : g_frame) newest = std::max(newest, f);
  for (auto it = g_steps.begin(); it != g_steps.end();) {
    Steps& s = it->second;
    if (s.frame + 2000 < newest) {
      it = g_steps.erase(it);
      continue;
    }
    if (s.n + s.jumps > 0) {
      Sum& ts = t[std::min<uint8_t>(s.type, kCameraType)];
      ++ts.drawn;
      const float v = Speed(s), spread = Spread(s);
      if (v > kMoving) {
        ++ts.moving;
        ts.still += s.still;
        ts.frames += s.n;
        ts.speed += v;
        ts.spread += spread;
        if (spread > 0.5f) ++ts.uneven;
      }
      ts.reversals += s.reversals;
      ts.jumps += s.jumps;
      ts.max_jump = std::max(ts.max_jump, s.max_jump);
      const float score = (v > kMoving ? spread * 10.0f + 10.0f * float(s.still) / float(std::max(s.n, 1u)) : 0.0f) +
                          2.0f * float(s.reversals) + 20.0f * float(s.jumps);
      if (setting > 1 && score > 2.0f) worst.push_back(Worst{score, it->first});
    }
    ++it;
  }
  REXLOG_INFO("cod3mp: entity steps: local players' frames {} {} {} {}", g_frame[0], g_frame[1], g_frame[2],
              g_frame[3]);
  for (int k = 0; k <= kCameraType; ++k) {
    const Sum& ts = t[k];
    if (!ts.drawn) continue;
    REXLOG_INFO("cod3mp: entity steps: type {} drawn {} moving {} uneven {}; speed {:.1f} u/s, spread {:.0f}%; still in "
                "{} of {} frames; reversals {} jumps {} (max {:.0f})",
                k, ts.drawn, ts.moving, ts.uneven, ts.moving ? ts.speed / ts.moving : 0.0,
                ts.moving ? ts.spread / ts.moving * 100.0 : 0.0, ts.still, ts.frames, ts.reversals, ts.jumps,
                ts.max_jump);
  }
  std::sort(worst.begin(), worst.end(), [](const Worst& a, const Worst& b) { return a.score > b.score; });
  for (size_t i = 0; i < worst.size() && i < size_t(setting); ++i) {
    const Steps& s = g_steps[worst[i].key];
    REXLOG_INFO("cod3mp: entity steps: worst {} key {:x} type {} local {} frames {} speed {:.1f} u/s spread {:.0f}% "
                "turn {:.1f} deg/s still {} rev {} jmp {} max {:.0f} o {:.0f} {:.0f} {:.0f}",
                i + 1, worst[i].key >> 2, s.type, worst[i].key & 3, s.n, Speed(s), Spread(s) * 100.0f,
                s.ms > 0.0 ? s.turn * 1000.0 / s.ms : 0.0, s.still, s.reversals, s.jumps, s.max_jump, s.origin[0],
                s.origin[1], s.origin[2]);
  }
  for (auto& kv : g_steps) {
    Steps& s = kv.second;
    s.n = s.still = s.reversals = s.jumps = 0;
    s.ms = s.dist = s.turn = s.v_sum = s.v_sq = 0.0;
    s.max_jump = 0.0f;
  }
}

}  // namespace

// CG_DrawActiveFrame (0x8246C748): one local player's view. Before it: a new frame of that player; after it: its camera.
REX_EXTERN(__imp__sub_8246C748);
REX_HOOK_RAW(sub_8246C748) {
  const bool on = REXCVAR_GET(cod3mp_debug_entity_steps) > 0;
  uint32_t local = 0;
  if (on) {
    local = LocalClient(base);
    const auto now = Clock::now();
    g_frame_ms[local] = g_frame[local] ? std::chrono::duration<double, std::milli>(now - g_frame_at[local]).count() : 0.0;
    g_frame_at[local] = now;
    ++g_frame[local];
    if (now - g_since >= std::chrono::seconds(1)) {
      Log();
      g_since = now;
    }
  } else if (!g_steps.empty()) {
    g_steps.clear();
  }
  __imp__sub_8246C748(ctx, base);
  if (!on) return;
  const uint32_t cg = kCg + local * kCgSize;
  float o[3], f[3];
  for (int i = 0; i < 3; ++i) {
    o[i] = g::ReadF32(base, cg + kCgViewOrigin + 4 * i);
    f[i] = g::ReadF32(base, cg + kCgViewAxis + 4 * i);
  }
  Sample(uint64_t(local) * 4 + local, kCameraType, local, o, f);
}

// CG_AddCEntity (0x8246B5B8): one entity into the scene; where its scene node went.
REX_EXTERN(__imp__sub_8246B5B8);
REX_HOOK_RAW(sub_8246B5B8) {
  const uint32_t ent = ctx.r3.u32;
  __imp__sub_8246B5B8(ctx, base);
  if (REXCVAR_GET(cod3mp_debug_entity_steps) <= 0 || !ValidGuest(ent)) return;
  if (g::ReadU32(base, ent + kEntFlags) & 0x80) return;  // not drawn (the draws' own test)
  const uint32_t node = g::ReadU32(base, ent + kEntSceneNode);
  if (!ValidGuest(node)) return;
  const uint32_t local = LocalClient(base);
  float o[3], f[3];
  for (int i = 0; i < 3; ++i) {
    o[i] = g::ReadF32(base, node + kNodeOrigin + 4 * i);
    f[i] = g::ReadF32(base, node + kNodeAxis + 4 * i);
  }
  Sample(uint64_t(ent) * 4 + local, g::ReadU8(base, ent) & 15, local, o, f);
}

#endif  // defined(COD3_MP)
