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
//
// [rexglue-native thread2] 2026-11-24 cod3_corona_depth_probe (work folder drafts/MACE-LAMP-DEPTH.md): the ray sees
// world brushes and entities with a box or brush model only, so a static model or a vehicle whose collision is its
// model never hid a corona (The Mace: a lamp glowing through the tank). In native mode the corona is now asked of
// the native scene's own depth, the game's original test: ngl_side projects it with the frame's own camera and
// ngl_vk reads the depth texel under it (ngl_side.h AskCoronaDepth). The answer is 1 or 2 frames old, hidden by the
// fade; the ray still answers when there is none yet (a corona's first frames, off the picture, no probes).

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>  // [acct2-bro] 2026-11-15 rn98a

#include "game/guest.h"
#include "game/build_address.h"  // [waw-decomp] 2026-11-23: A(), this build's address of a USA address (discmap)
#include "game/ngl_side.h"  // [rexglue-native thread2] 2026-11-24: the native depth probes (AskCoronaDepth)

REXCVAR_DEFINE_BOOL(cod3_fix_corona_fade, true, "cod3/Video",
                    "Light coronas (lanterns, lamps) fade in and out at the same speed at any frame rate; off: one "
                    "step per drawn frame, as the game does (faster above 60 fps)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_corona_trace, false, "cod3/Video",
                    "Light coronas hidden by a ray against the world and characters, and the aim-down-sights blur "
                    "focused by a ray, instead of reading the GPU's depth buffer back (no wait on the GPU)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [rexglue-native thread2] 2026-11-24
REXCVAR_DEFINE_BOOL(cod3_corona_depth_probe, true, "cod3/Video",
                    "In native mode, light coronas hidden by the native picture's own depth at their place, as the "
                    "game tests them (models and vehicles hide them too), with the ray only until an answer comes; "
                    "off: the ray alone")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// [acct2-bro] 2026-11-15 rn98a (debug, off by default): why the coronas' trace (on in native mode) falls back to
// the depth read; native mode never writes the depth the read looks at (0 = far: a corona shows through a wall).
REXCVAR_DEFINE_BOOL(cod3_debug_corona_trace, false, "cod3/Debug",
                    "Log the coronas' trace every 2 s: passes, records, depth reads, answers, fallbacks")
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
// [rexglue-native thread2] 2026-11-24 rule 14 (owner 04:37: "every game needs to be able to function propery regardless
// of FPS"): below 60 fps a pass may take up to this many 60 Hz steps (10 fps), so a lantern fades in the same time at
// 20 or 30 as at 60; it was at most one step a pass, 3x slower at 20 (FPS-RULE14.md A3).
constexpr double kMaxShare = 6.0;
constexpr int kSystems = 8;              // corona systems seen, by view ([acct2-bro] 2026-11-15 b97: 4 -> 8)

// [acct2-bro] 2026-11-15 b97 (owner 18:06, split screen): cod3mp's pass is handed one corona system for every view (the
// caller 0x824488F4 loads r3 from one global), so a system's step and a corona's ray answer are kept by view: the
// active local client (the view being drawn; the campaign's is 0) with the system or the record.
#if defined(COD3_MP)
constexpr uint32_t kViewClient = 0x82BCBE9C;
#else
constexpr uint32_t kViewClient = 0x82A2A1C8;
#endif

struct Saved {
  uint32_t addr;
  float fade;
};
std::vector<Saved> g_saved;

struct System {
  uint32_t arg = 0;
  uint32_t view = 0;  // [acct2-bro] 2026-11-15 b97: the local client it was stepped for
  Clock::time_point last{};
};
System g_systems[kSystems];
int g_next_system = 0;

bool Valid(uint32_t p) { return p >= 0x10000 && (p & 3) == 0; }

// [acct2-bro] 2026-11-15 rn98a: the debug counts (main thread), logged and cleared every 2 s
struct TraceCounts {
  uint64_t passes = 0, records = 0;            // corona passes, records in their lists
  uint64_t reads = 0, reads_other = 0;         // depth reads from the pass, from elsewhere
  uint64_t seen = 0, hidden = 0;               // answered by a ray (kept or fresh)
  uint64_t fresh = 0, blocked = 0;             // fresh rays, of those blocked
  uint64_t bad_record = 0, bad_eye = 0, bad_ray = 0, off = 0;  // fell back to the depth read
  uint64_t fell_far = 0;                       // fallbacks whose read said 0 (far: shown)
  uint64_t shared = 0;                         // [acct2-bro] b97: answers for a record another view asked last
  uint64_t probed = 0, probe_hidden = 0;       // [rexglue-native thread2] 2026-11-24: answered by the depth probe
  uint32_t views = 0;                          // b97: the views (bits) that ran a pass
  uint32_t first_system = 0;                   // b97: the first system seen
  uint64_t other_system = 0;                   // b97: passes for another system than that one
  float sample_pos[3] = {}, sample_f30 = 0, sample_v[4] = {}, sample_read = -1;
  uint32_t sample_record = 0, sample_lr = 0;
  Clock::time_point last = Clock::now();
};
TraceCounts g_tc;
void LogTraceCounts() {
  if (!REXCVAR_GET(cod3_debug_corona_trace)) return;
  const auto now = Clock::now();
  if (std::chrono::duration<double>(now - g_tc.last).count() < 2.0) return;
  REXLOG_INFO("cod3: coronas: rn98a trace {}: passes {} records {}; depth reads {} (+{} elsewhere): depth probe {} "
              "(hidden {}); shown {} hidden "
              "{} (fresh rays {}, blocked {}); fell back: record {} eye {} ray {} off {} (read 0, far: {}); views "
              "{:x}, passes for another system {}, records asked by another view last {}; sample "
              "record {:08X} at {:.1f} {:.1f} {:.1f} f30 {:.2f} v127 {:.1f} {:.1f} {:.1f} {:.1f} lr {:08X} read {:.4g}",
              REXCVAR_GET(cod3_corona_trace) ? "on" : "off", g_tc.passes, g_tc.records, g_tc.reads, g_tc.reads_other,
              g_tc.probed, g_tc.probe_hidden, g_tc.seen, g_tc.hidden, g_tc.fresh, g_tc.blocked, g_tc.bad_record, g_tc.bad_eye, g_tc.bad_ray, g_tc.off,
              g_tc.fell_far, g_tc.views, g_tc.other_system, g_tc.shared, g_tc.sample_record, g_tc.sample_pos[0],
              g_tc.sample_pos[1], g_tc.sample_pos[2],
              g_tc.sample_f30, g_tc.sample_v[0], g_tc.sample_v[1], g_tc.sample_v[2], g_tc.sample_v[3], g_tc.sample_lr,
              g_tc.sample_read);
  g_tc = TraceCounts{};
  g_tc.last = now;
}

// The share of a 60 Hz step this pass gets: real time since this system's last pass, at most 1 (the first pass: 1).
// [acct2-bro] 2026-11-15 b97: by view too (split screen: each view's pass is a 60 Hz step of its own, as the game's)
double StepShare(uint32_t arg, uint32_t view) {
  const auto now = Clock::now();
  System* s = nullptr;
  for (System& c : g_systems) {
    if (c.arg == arg && c.view == view) s = &c;
  }
  if (!s) {
    s = &g_systems[g_next_system];
    g_next_system = (g_next_system + 1) % kSystems;
    s->arg = arg;
    s->view = view;
    s->last = now;
    return 1.0;
  }
  const double dt = std::chrono::duration<double>(now - s->last).count();
  s->last = now;
  return std::clamp(dt / kStepSeconds, 0.0, kMaxShare);  // [rexglue-native thread2] 2026-11-24: was at most 1
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
    if (now == s.fade) continue;
    // [rexglue-native thread2] 2026-11-24: above 1 (below 60 fps) the pass's one step is stretched, kept to 0..1
    const double scaled = s.fade + (now - s.fade) * share;
    g::WriteF32(base, s.addr, static_cast<float>(share > 1.0 ? std::clamp(scaled, 0.0, 1.0) : scaled));
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
  if (REXCVAR_GET(cod3_debug_corona_trace) && Valid(system)) {  // [acct2-bro] 2026-11-15 rn98a
    ++g_tc.passes;
    const uint32_t view = g::ReadU32(base, kViewClient);  // [acct2-bro] b97
    if (view < 32) g_tc.views |= 1u << view;
    if (g_tc.first_system == 0) g_tc.first_system = system;
    if (system != g_tc.first_system) ++g_tc.other_system;
    for (int i = 0; i < kListCount; ++i) {
      const uint32_t list = g::ReadU32(base, system + kLists + 4 * i);
      if (Valid(list)) g_tc.records += g::ReadU32(base, list + kListRecords);
    }
  }
  const bool fix = REXCVAR_GET(cod3_fix_corona_fade) && Valid(system);
  const double share = fix ? StepShare(system, g::ReadU32(base, kViewClient)) : 1.0;  // [acct2-bro] b97: by view
  const bool scale = fix && share != 1.0;  // [rexglue-native thread2] 2026-11-24: below 60 fps too (was < 1)
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
// [waw-decomp] 2026-11-23: A() gives the build's own address (tools/discmap); USA: the same.
constexpr uint32_t kDepthReadReturn = A(0x8247784C);  // the corona pass's call of the depth read, its return
constexpr uint32_t kCgTrace = A(0x824604A8);
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
// [rexglue-native thread2] 2026-11-24: a depth probe's answer is used while it is this fresh (answers come 1 or 2
// drawn frames after the ask: 100 ms at a 20 fps cap); older, or none, and the ray answers
constexpr double kProbeKeepSeconds = 0.25;

struct Seen {
  bool visible = false;
  Clock::time_point when{};
  float eye[3] = {};
};
std::unordered_map<uint64_t, Seen> g_seen;  // by view and record (main thread; [acct2-bro] 2026-11-15 b97: by view)
std::unordered_map<uint32_t, uint32_t> g_last_view;  // [acct2-bro] b97, debug only: the view that asked for it last
static_assert(kActiveClient == kViewClient, "the same active local client");
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
  if (!Valid(record)) {
    ++g_tc.bad_record;  // [acct2-bro] 2026-11-15 rn98a
    return false;
  }
  float pos[3], eye[3];
  ReadVec3(base, record, pos);
  g_tc.sample_record = record;  // rn98a: the last one seen
  for (int i = 0; i < 3; ++i) g_tc.sample_pos[i] = pos[i];
  g_tc.sample_f30 = static_cast<float>(ctx.f30.f64);
  for (int i = 0; i < 4; ++i) g_tc.sample_v[i] = ctx.v127.f32[i];
  if (!PassEye(ctx, pos, eye)) {
    ++g_tc.bad_eye;  // rn98a
    return false;
  }
  // [acct2-bro] 2026-11-15 b97: one view's answer is never another's (split screen: the same record, another eye)
  const uint32_t view = g::ReadU32(base, kActiveClient);
  if (REXCVAR_GET(cod3_debug_corona_trace)) {
    auto [it, added] = g_last_view.try_emplace(record, view);
    if (!added && it->second != view) ++g_tc.shared;
    it->second = view;
    if (g_last_view.size() > 4096) g_last_view.clear();
  }
  Seen& seen = g_seen[(uint64_t(view) << 32) | record];
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
    if (f < 0.0f) {
      ++g_tc.bad_ray;  // rn98a
      return false;
    }
    seen.visible = f >= 0.999f;
    ++g_tc.fresh;                       // rn98a
    if (!seen.visible) ++g_tc.blocked;  // rn98a
    seen.when = now;
    for (int i = 0; i < 3; ++i) seen.eye[i] = eye[i];
  }
  *visible = seen.visible;  // never traced yet: hidden until its ray (it fades in)
  if (g_seen.size() > 4096) g_seen.clear();
  return true;
}

// [rexglue-native thread2] 2026-11-24: whether the native scene's depth says the corona `record` is in sight (true:
// answered in *visible). Every call asks again for this frame (ngl_side places it with this frame's camera); the
// answer is the latest that came back, kProbeKeepSeconds at most. False (the ray answers): no native probes, the
// record or the eye can't be read, or no fresh answer yet.
bool CoronaByDepth(PPCContext& ctx, uint8_t* base, uint32_t record, bool* visible) {
  if (!Valid(record)) return false;
  float pos[3], eye[3];
  ReadVec3(base, record, pos);
  if (!PassEye(ctx, pos, eye)) return false;
  const uint32_t view = g::ReadU32(base, kActiveClient);  // one view's answer is never another's (as g_seen's)
  const uint64_t key = (uint64_t(view) << 32) | record;
  if (!cod3::ngl_side::AskCoronaDepth(key, pos, eye)) return false;
  return cod3::ngl_side::CoronaDepthAnswer(key, kProbeKeepSeconds, visible);
}

// [rexglue-native thread2] 2026-11-24 (debug, cod3_debug_corona_trace): a corona the depth probe hides, with what
// decided it and the ray's verdict beside it, so each hidden corona can be checked by numbers (drafts/
// MACE-LAMP-DEPTH.md, "Proving the hidden ones"): the occluder point (the probed depth unprojected through the same
// view-projection) must be a surface between the eye and the corona. At most one line per record every 2 s and
// kMostProbeLines a run.
constexpr int kMostProbeLines = 40;
constexpr double kProbeLineSeconds = 2.0;
std::unordered_map<uint32_t, Clock::time_point> g_probe_line_last;  // by record (main thread)
int g_probe_lines = 0;

void LogProbeHides(PPCContext& ctx, uint8_t* base, uint32_t record) {
  if (g_probe_lines >= kMostProbeLines) return;
  const auto now = Clock::now();
  auto [last, added] = g_probe_line_last.try_emplace(record, now);
  if (!added && std::chrono::duration<double>(now - last->second).count() < kProbeLineSeconds) return;
  last->second = now;
  float pos[3], eye[3];
  ReadVec3(base, record, pos);
  if (!PassEye(ctx, pos, eye)) return;
  ++g_probe_lines;
  const uint32_t view = g::ReadU32(base, kActiveClient);
  cod3::ngl_side::CoronaDepthDetail detail;
  const bool have = cod3::ngl_side::CoronaDepthDetails((uint64_t(view) << 32) | record, &detail);
  const float dx = pos[0] - eye[0], dy = pos[1] - eye[1], dz = pos[2] - eye[2];
  const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
  // the occluder: its distance from the eye, and how far along the eye-to-corona line it lies (its projection on it)
  float occluder_distance = -1.0f, along = -1.0f;
  if (have && detail.occluder_ok) {
    const float ox = detail.occluder[0] - eye[0], oy = detail.occluder[1] - eye[1], oz = detail.occluder[2] - eye[2];
    occluder_distance = std::sqrt(ox * ox + oy * oy + oz * oz);
    along = distance > 0.0f ? (ox * dx + oy * dy + oz * dz) / distance : 0.0f;
  }
  // the ray as the old path casts it (from the eye to kShortOfCorona short of the corona)
  const float k = distance > kShortOfCorona ? (distance - kShortOfCorona) / distance : 0.0f;
  const float end[3] = {eye[0] + dx * k, eye[1] + dy * k, eye[2] + dz * k};
  const float f = RayFraction(ctx, base, eye, end);
  const bool ray_shows = f >= 0.999f;
  REXLOG_INFO("cod3: coronas: probe-hides{} record {:08X} view {} at {:.1f} {:.1f} {:.1f} eye {:.1f} {:.1f} {:.1f} "
              "distance {:.1f}; depth {:.7f} near_depth {:.7f} ndc {:.4f} {:.4f}; occluder {} {:.1f} {:.1f} {:.1f} "
              "distance {:.1f} along {:.1f} (corona - 8: {:.1f}); ray fraction {:.4f} ({})",
              ray_shows ? " ray-would-show" : "", record, view, pos[0], pos[1], pos[2], eye[0], eye[1], eye[2],
              distance, have ? detail.depth : -1.0f, have ? detail.near_depth : -1.0f, have ? detail.ndc[0] : 0.0f,
              have ? detail.ndc[1] : 0.0f, have && detail.occluder_ok ? "at" : "(none)", detail.occluder[0],
              detail.occluder[1], detail.occluder[2], occluder_distance, along, distance - 8.0f, f,
              f < 0.0f ? "no ray" : ray_shows ? "shown" : "blocked");
}

}  // namespace

// The depth read (r1 f1 f2: the screen point; returns f1 the scene's depth there): from the corona pass, a ray.
// [rexglue-native thread2] 2026-11-24: or, in native mode, the native scene's depth there (CoronaByDepth) first.
#if defined(COD3_MP)
REX_EXTERN(__imp__sub_82438DF0);
REX_HOOK_RAW(sub_82438DF0) {
#else
REX_EXTERN(__imp__sub_82420F48);
REX_HOOK_RAW(sub_82420F48) {
#endif
  const bool from_pass = static_cast<uint32_t>(ctx.lr) == kDepthReadReturn;  // [acct2-bro] 2026-11-15 rn98a
  if (REXCVAR_GET(cod3_debug_corona_trace)) {
    from_pass ? ++g_tc.reads : ++g_tc.reads_other;
    if (!from_pass) g_tc.sample_lr = static_cast<uint32_t>(ctx.lr);
    if (from_pass && !REXCVAR_GET(cod3_corona_trace)) ++g_tc.off;
    LogTraceCounts();
  }
  if (REXCVAR_GET(cod3_corona_trace) && from_pass) {
    bool visible = false;
    if (REXCVAR_GET(cod3_corona_depth_probe) && CoronaByDepth(ctx, base, ctx.r30.u32, &visible)) {
      // (debug) before the answer is written: the log's ray runs the game's trace from this context, as the old path
      if (!visible && REXCVAR_GET(cod3_debug_corona_trace)) LogProbeHides(ctx, base, ctx.r30.u32);
      ctx.f1.f64 = visible ? double(kNoHit) : 0.0;  // the same two answers as the ray's
      ++g_tc.probed;
      if (!visible) ++g_tc.probe_hidden;
      return;
    }
    if (CoronaInSight(ctx, base, ctx.r30.u32, &visible)) {
      ctx.f1.f64 = visible ? double(kNoHit) : 0.0;
      visible ? ++g_tc.seen : ++g_tc.hidden;  // rn98a
      return;
    }
  }
#if defined(COD3_MP)
  __imp__sub_82438DF0(ctx, base);
#else
  __imp__sub_82420F48(ctx, base);
#endif
  if (REXCVAR_GET(cod3_debug_corona_trace) && from_pass) {  // rn98a: what the fallback read
    g_tc.sample_read = static_cast<float>(ctx.f1.f64);
    if (ctx.f1.f64 == 0.0) ++g_tc.fell_far;
  }
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
