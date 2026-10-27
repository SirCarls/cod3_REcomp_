// cod3 - [claude-9b] 2026-10-27 light coronas (lanterns, lamps) fade at the same speed at any frame
// rate (plan item 23, owner: lantern lights "a permanent fix that doesn't cost frame time"; owner 2026-10-27 03:57:
// effects run fast above 60 fps). Both programs. dev/agents/claude-9b-think/FINDINGS-2026-10-27.md section 3.
//
// The corona pass (campaign 0x8245C208, cod3mp 0x82476FD0; r3: the corona system) runs once per drawn frame over 99
// lists (pointers at r3 + 4); each list has a record count at +0x40 and its 0x30-byte records at +0x44. A record's fade
// (+0x24, float) steps 0.1 toward shown or hidden every call, so above 60 fps lights faded in and out faster (2.4 times
// at 144 fps).
//
// With cod3_fix_corona_fade the fades are saved before the pass and, after it, each change is scaled by the frame's
// share of a 60 Hz frame (real time since the pass last ran for that corona system, at most one 60 Hz step): 60 fps and
// below stay as the game has it, above 60 a fade takes as long as at 60. No guest constant is written, nothing is
// drawn twice; a few hundred float reads a frame.
//
// [claude-9b] 2026-10-27 cod3_corona_trace (dev/agents/claude-9b-think/FINDINGS-CORONA-TRACE.md): whether a corona is
// hidden comes from a ray (CG_Trace, campaign 0x824460C0, cod3mp 0x824604A8: the world and characters) from the eye to
// it instead of one pixel of the GPU's depth buffer read back (0x82420F48 / mp 0x82438DF0, called from the pass at
// 0x8245CACC / mp 0x82477848), and the aim-down-sights blur's focus (0x824267B8 / mp 0x8243DEA0, 10 depth samples at
// the screen's middle) from one ray along the view. Those two are the game's only CPU reads of a GPU resolve, so with
// both the CPU never waits on the GPU for a readback. Rays are few: at most 4 per 1/60 s, each corona's kept for
// 150 ms or until the eye moves 16 units. The fade above keeps the edges soft.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>

#include "game/guest.h"

REXCVAR_DEFINE_BOOL(cod3_fix_corona_fade, true, "cod3/Video",
                    "Light coronas (lanterns, lamps) fade in and out at the same speed at any frame rate; off: one "
                    "step per drawn frame, as the game does (faster above 60 fps)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_corona_trace, false, "cod3/Video",
                    "Light coronas hidden by a ray against the world and characters, and the aim-down-sights blur "
                    "focused by a ray, instead of reading the GPU's depth buffer back (no wait on the GPU)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

namespace g = cod3::guest;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kLists = 4;          // the corona system + 4: the list pointers
constexpr int kListCount = 99;
constexpr uint32_t kListRecords = 0x40;  // a list: its record count,
constexpr uint32_t kListArray = 0x44;    //   its records
constexpr uint32_t kRecordSize = 0x30;
constexpr uint32_t kRecordFade = 0x24;   // a record: its fade (float, 0 hidden .. 1 shown)
constexpr size_t kMaxRecords = 8192;
constexpr double kStepSeconds = 1.0 / 60.0;
constexpr int kSystems = 4;              // corona systems seen (one per view at most)

struct Saved {
  uint32_t addr;
  float fade;
};
std::vector<Saved> g_saved;

struct System {
  uint32_t arg = 0;
  Clock::time_point last{};
};
System g_systems[kSystems];
int g_next_system = 0;

bool Valid(uint32_t p) { return p >= 0x10000 && (p & 3) == 0; }

// The share of a 60 Hz step this pass gets: real time since this system's last pass, at most 1 (the first pass: 1).
double StepShare(uint32_t arg) {
  const auto now = Clock::now();
  System* s = nullptr;
  for (System& c : g_systems) {
    if (c.arg == arg) s = &c;
  }
  if (!s) {
    s = &g_systems[g_next_system];
    g_next_system = (g_next_system + 1) % kSystems;
    s->arg = arg;
    s->last = now;
    return 1.0;
  }
  const double dt = std::chrono::duration<double>(now - s->last).count();
  s->last = now;
  return std::clamp(dt / kStepSeconds, 0.0, 1.0);
}

void Save(uint8_t* base, uint32_t system) {
  g_saved.clear();
  for (int i = 0; i < kListCount; ++i) {
    const uint32_t list = g::ReadU32(base, system + kLists + 4 * i);
    if (!Valid(list)) continue;
    const uint32_t count = g::ReadU32(base, list + kListRecords);
    const uint32_t records = g::ReadU32(base, list + kListArray);
    if (count == 0 || !Valid(records)) continue;
    for (uint32_t j = 0; j < count && g_saved.size() < kMaxRecords; ++j) {
      const uint32_t addr = records + j * kRecordSize + kRecordFade;
      g_saved.push_back({addr, g::ReadF32(base, addr)});
    }
  }
}

void Scale(uint8_t* base, double share) {
  for (const Saved& s : g_saved) {
    const float now = g::ReadF32(base, s.addr);
    if (now != s.fade) g::WriteF32(base, s.addr, static_cast<float>(s.fade + (now - s.fade) * share));
  }
}

}  // namespace

#if defined(COD3_MP)
REX_EXTERN(__imp__sub_82476FD0);
REX_HOOK_RAW(sub_82476FD0) {
#else
REX_EXTERN(__imp__sub_8245C208);
REX_HOOK_RAW(sub_8245C208) {
#endif
  const uint32_t system = ctx.r3.u32;
  const bool fix = REXCVAR_GET(cod3_fix_corona_fade) && Valid(system);
  const double share = fix ? StepShare(system) : 1.0;
  const bool scale = fix && share < 1.0;
  if (scale) Save(base, system);
#if defined(COD3_MP)
  __imp__sub_82476FD0(ctx, base);
#else
  __imp__sub_8245C208(ctx, base);
#endif
  if (scale) Scale(base, share);
}

// ---------------------------------------------------------------------------------------------------------------
// [claude-9b] 2026-10-27 cod3_corona_trace.
namespace {

#if defined(COD3_MP)
constexpr uint32_t kDepthReadReturn = 0x8247784C;  // the corona pass's call of the depth read, its return
constexpr uint32_t kCgTrace = 0x824604A8;
constexpr uint32_t kTraceFilterVtable = 0x82065DCC;
constexpr uint32_t kEntityList = 0x82BCC7FC;       // *(it) + 4 (client + 1): the player's entity
constexpr uint32_t kEntityNumber = 484;
constexpr uint32_t kActiveClient = 0x82BCBE9C;
constexpr uint32_t kCg = 0x82C7D490, kCgSize = 6368, kCgViewOrigin = 4960, kCgViewAxis = 4976;
constexpr uint32_t kDofSamples = 0x82BCC94C;       // float[10]
#else
constexpr uint32_t kDepthReadReturn = 0x8245CAD0;
constexpr uint32_t kCgTrace = 0x824460C0;
constexpr uint32_t kTraceFilterVtable = 0x82067848;
constexpr uint32_t kEntityList = 0x82A2AB40;
constexpr uint32_t kEntityNumber = 500;
constexpr uint32_t kActiveClient = 0x82A2A1C8;
constexpr uint32_t kCg = 0x82A98D90, kCgSize = 5840, kCgViewOrigin = 4480, kCgViewAxis = 4496;
constexpr uint32_t kDofSamples = 0x82A2AC08;
#endif
constexpr uint32_t kTraceMask = 0x02000001;  // world solid and characters
constexpr uint32_t kTraceFraction = 0x20;
constexpr int kDofSampleCount = 10;
constexpr float kFar = 8192.0f;
constexpr float kNoHit = 3.4e38f;
constexpr double kRayRate = 240.0;            // rays a second (4 per 1/60 s)
constexpr double kRayBurst = 8.0;
constexpr double kKeepSeconds = 0.15;
constexpr float kKeepMove = 16.0f;
constexpr float kShortOfCorona = 4.0f;        // the ray stops short of the corona's own lamp

struct Seen {
  bool visible = false;
  Clock::time_point when{};
  float eye[3] = {};
};
std::unordered_map<uint32_t, Seen> g_seen;   // by record (main thread)
double g_ray_tokens = kRayBurst;
Clock::time_point g_ray_refill = Clock::now();

void ReadVec3(uint8_t* base, uint32_t a, float v[3]) {
  for (int i = 0; i < 3; ++i) v[i] = g::ReadF32(base, a + 4 * i);
}

int32_t PlayerNumber(uint8_t* base) {
  const uint32_t client = g::ReadU32(base, kActiveClient);
  const uint32_t list = g::ReadU32(base, kEntityList);
  if (client > 3 || !Valid(list)) return -1;
  const uint32_t ent = g::ReadU32(base, list + 4 * (client + 1));
  return Valid(ent) ? g::ReadS32(base, ent + kEntityNumber) : -1;
}

// CG_Trace's fraction from `start` to `end` (a point trace; the player left out); -1 when it can't run.
float RayFraction(PPCContext& parent, uint8_t* base, const float start[3], const float end[3]) {
  g::Call call(parent, base);
  auto& c = call.ctx();
  c.r1.u32 = (c.r1.u32 - 0x100) & ~0xFu;  // scratch: [r1, r1 + 0x100), above the callee's frames
  const uint32_t s = c.r1.u32;
  const uint32_t trace = s, at_start = s + 0x50, at_end = s + 0x60, zero = s + 0x70, filter = s + 0x80;
  for (uint32_t a = s; a < s + 0xA0; a += 4) g::WriteU32(base, a, 0);
  for (int i = 0; i < 3; ++i) {
    g::WriteF32(base, at_start + 4 * i, start[i]);
    g::WriteF32(base, at_end + 4 * i, end[i]);
  }
  const int32_t player = PlayerNumber(base);
  g::WriteU32(base, filter + 0x00, kTraceFilterVtable);
  g::WriteU32(base, filter + 0x04, static_cast<uint32_t>(player));
  g::WriteU32(base, filter + 0x08, static_cast<uint32_t>(player));
  g::WriteU32(base, filter + 0x14, kTraceMask);
  call.Invoke(kCgTrace, {trace, at_start, zero, zero, at_end, filter});
  const float f = g::ReadF32(base, trace + kTraceFraction);
  return std::isfinite(f) ? std::clamp(f, 0.0f, 1.0f) : -1.0f;
}

bool TakeRay() {
  const auto now = Clock::now();
  g_ray_tokens = std::min(kRayBurst, g_ray_tokens + std::chrono::duration<double>(now - g_ray_refill).count() * kRayRate);
  g_ray_refill = now;
  if (g_ray_tokens < 1.0) return false;
  g_ray_tokens -= 1.0;
  return true;
}

// The camera position the pass measured from (v127; f30 = the corona's distance from it), in the guest's element order
// (the recompiler keeps a vector's 16 bytes reversed: element i is f32[3 - i]); false when it doesn't match f30.
bool PassEye(const PPCContext& ctx, const float pos[3], float eye[3]) {
  const float want = static_cast<float>(ctx.f30.f64);
  for (int order = 0; order < 2; ++order) {
    for (int i = 0; i < 3; ++i) eye[i] = order == 0 ? ctx.v127.f32[3 - i] : ctx.v127.f32[i];
    const float dx = pos[0] - eye[0], dy = pos[1] - eye[1], dz = pos[2] - eye[2];
    if (std::fabs(std::sqrt(dx * dx + dy * dy + dz * dz) - want) < 1.0f) return true;
  }
  return false;
}

// Whether the corona `record` is in sight (true: answered in *visible).
bool CoronaInSight(PPCContext& ctx, uint8_t* base, uint32_t record, bool* visible) {
  if (!Valid(record)) return false;
  float pos[3], eye[3];
  ReadVec3(base, record, pos);
  if (!PassEye(ctx, pos, eye)) return false;
  Seen& seen = g_seen[record];
  const auto now = Clock::now();
  const float mx = eye[0] - seen.eye[0], my = eye[1] - seen.eye[1], mz = eye[2] - seen.eye[2];
  const bool stale = seen.when == Clock::time_point{} ||
                     std::chrono::duration<double>(now - seen.when).count() > kKeepSeconds ||
                     mx * mx + my * my + mz * mz > kKeepMove * kKeepMove;
  if (stale && TakeRay()) {
    const float dx = pos[0] - eye[0], dy = pos[1] - eye[1], dz = pos[2] - eye[2];
    const float len = std::sqrt(dx * dx + dy * dy + dz * dz);
    const float k = len > kShortOfCorona ? (len - kShortOfCorona) / len : 0.0f;
    const float end[3] = {eye[0] + dx * k, eye[1] + dy * k, eye[2] + dz * k};
    const float f = RayFraction(ctx, base, eye, end);
    if (f < 0.0f) return false;
    seen.visible = f >= 0.999f;
    seen.when = now;
    for (int i = 0; i < 3; ++i) seen.eye[i] = eye[i];
  }
  *visible = seen.visible;  // never traced yet: hidden until its ray (it fades in)
  if (g_seen.size() > 4096) g_seen.clear();
  return true;
}

}  // namespace

// The depth read (r1 f1 f2: the screen point; returns f1 the scene's depth there): from the corona pass, a ray.
#if defined(COD3_MP)
REX_EXTERN(__imp__sub_82438DF0);
REX_HOOK_RAW(sub_82438DF0) {
#else
REX_EXTERN(__imp__sub_82420F48);
REX_HOOK_RAW(sub_82420F48) {
#endif
  if (REXCVAR_GET(cod3_corona_trace) && static_cast<uint32_t>(ctx.lr) == kDepthReadReturn) {
    bool visible = false;
    if (CoronaInSight(ctx, base, ctx.r30.u32, &visible)) {
      ctx.f1.f64 = visible ? double(kNoHit) : 0.0;
      return;
    }
  }
#if defined(COD3_MP)
  __imp__sub_82438DF0(ctx, base);
#else
  __imp__sub_82420F48(ctx, base);
#endif
}

// The aim-down-sights blur's depth samples (10 around the screen's middle): one ray along the view instead.
#if defined(COD3_MP)
REX_EXTERN(__imp__sub_8243DEA0);
REX_HOOK_RAW(sub_8243DEA0) {
#else
REX_EXTERN(__imp__sub_824267B8);
REX_HOOK_RAW(sub_824267B8) {
#endif
  if (REXCVAR_GET(cod3_corona_trace)) {
    const uint32_t client = g::ReadU32(base, kActiveClient);
    if (client <= 3) {
      const uint32_t cg = kCg + client * kCgSize;
      float eye[3], fwd[3];
      ReadVec3(base, cg + kCgViewOrigin, eye);
      ReadVec3(base, cg + kCgViewAxis, fwd);
      const float end[3] = {eye[0] + fwd[0] * kFar, eye[1] + fwd[1] * kFar, eye[2] + fwd[2] * kFar};
      const float f = RayFraction(ctx, base, eye, end);
      if (f >= 0.0f) {
        const float depth = f < 1.0f ? f * kFar : kNoHit;
        for (int i = 0; i < kDofSampleCount; ++i) g::WriteF32(base, kDofSamples + 4 * i, depth);
        return;
      }
    }
  }
#if defined(COD3_MP)
  __imp__sub_8243DEA0(ctx, base);
#else
  __imp__sub_824267B8(ctx, base);
#endif
}
