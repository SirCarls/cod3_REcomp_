// cod3 - the turret and gun of the ridden vehicle between game updates (game/ride_bones.h).
//
// [claude-9b-think] 2026-10-27 UNTESTED. Stage 1 of FINDINGS-ANIM-BLEND. FINDINGS-TANK-HULL: in a tank's turret view
// the camera turns smoothly between updates, but the turret and gun move only in updates, a 60 Hz sawtooth.
//
// The bones. An animated model (entity +0x1FC) keeps its bones in one buffer, [model +0x70] + 0x30, 0x40 bytes a
// bone (four vec4 rows: the X, Y and Z axes and the origin, in model space; w unused), byte model +0xAB of them. Only
// game updates write them: the animation queue (0x824F38E8 -> 0x8216E680, inline in G_RunFrame's tail 0x82563FC0, no
// jobs) and the vehicles' own passes in the physics update. 0x8253F438 is G_RunFrame's last call. The renderer reads
// them only while the frame's lists are built: 0x8242E450 -> the vehicle draw 0x8242CB68 (each record times the node
// matrix, the result copied) or the skinned draw 0x8242D080 (a memcpy into the frame's allocator). The render job
// (0x82130980 -> 0x8214F770) reads those copies, never the buffer. The turret view's camera reads tag_turret's matrix
// through the same bones (0x82446598 -> 0x8254EC98), inside CG_DrawActiveFrame.
//
// Here, for the vehicle the player rides only:
//  - Snapshot at each update's end (0x8253F438's hook, after the game's code): all its records, raw, the last three.
//  - Which bones: those the update changed that kept their place relative to an anchor: tag_turret, tag_barrel,
//    tag_gunner_turret or tag_gunner_barrel. The bone indices come from the game's own lookup (0x820CEBB8 gives the
//    name's token, as the turret camera gets it, then 0x8241D820(model, token)). That is the turret, the gun and
//    whatever is mounted on them. The hull, and the wheels the physics pass writes, are left alone.
//  - Blend at CG_DrawActiveFrame's entry, before the camera reads tag_turret. Rows and origin are lerped `back` of
//    the way from the update's pose to the one before. The rows are then made orthogonal again, each keeping its
//    lerped length: a turn of a few degrees an update, where a row lerp is what FINDINGS-ANIM-BLEND allows. The w
//    lanes are left as they are.
//  - Restore in PutBackDrawn, before any game code. Each bone's update bytes go back only where the buffer still
//    holds what was written (PutBackOne's rule), so a writer in between keeps its value.
//
// Guards (no blend for that update, all bones or none, so the turret never tears):
//  - the two snapshots aren't of two updates in a row (0x82A2ABC0, the animation's update counter, +1);
//  - a different model, bone buffer, bone count or level of detail;
//  - an anchor that turned more than 45 degrees or moved more than 8 units in one update;
//  - a bone that turned more than 45 degrees or moved more than 64 units;
//  - A-B-A: the pose went back to the one before last.
// And no blend that frame when a bone has changed since the update.
//
// cod3_blend_ride_bones 2 does all of it but the writes, for the log. cod3_debug_ride_bones counts every step, and
// whether the renderer drew the model only between the blend and the restore, and on the frame's own thread.
#include "game/ride_bones.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <thread>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(cod3_blend_ride_bones, 0, "cod3/Game",
                     "The turret and gun of the vehicle you ride drawn between the game's updates (cod3_server_hz), "
                     "as its hull and the camera are: 0 off (the last update's pose until the next), 1 on, 2 verify "
                     "only (everything but the writes; see cod3_debug_ride_bones)")
    .range(0, 2)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(cod3_debug_ride_bones, false, "cod3",
                    "Log once a second what cod3_blend_ride_bones found and did: the ridden vehicle's anchors and "
                    "blended bones, the guards, A-B-A skips, the restores, and when and where the renderer drew it")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace cod3::ride_bones {
namespace {

namespace g = cod3::guest;

constexpr uint32_t kEntNumber = 500;
constexpr uint32_t kEntModel = 0x1FC;
constexpr uint32_t kModelBones = 0x70;
constexpr uint32_t kModelBoneCount = 0xAB;
constexpr uint32_t kModelLod = 0xB8;       // +0xB8, +0xBC, +0xC0: the levels of detail
constexpr uint32_t kModelArmed = 0xC4;     // bit 0: to be drawn (the draw clears it)
constexpr uint32_t kNodeModel = 0x5C;      // 0x8242E450's scene node: its model
constexpr uint32_t kRecords = 0x30;
constexpr uint32_t kRecordSize = 0x40;
constexpr uint32_t kMaxBones = 255;
constexpr uint32_t kAnimUpdateCounter = 0x82A2ABC0;
constexpr uint32_t kNameToken = 0x820CEBB8;   // (const char*) -> the name's token (as 0x82446598 gets tag_turret's)
constexpr uint32_t kBoneOfName = 0x8241D820;  // (model, token) -> the bone's index, or -1
constexpr int kAnchors = 4;
constexpr const char* kAnchorNames[kAnchors] = {"tag_turret", "tag_barrel", "tag_gunner_turret", "tag_gunner_barrel"};
constexpr float kMaxTurnDeg = 45.0f;
constexpr float kMaxAnchorMove = 8.0f;
constexpr float kMaxBoneMove = 64.0f;
constexpr float kRigidRow = 2e-3f;    // the rows relative to an anchor the same within this
constexpr float kRigidOrigin = 0.1f;  // the origin relative to an anchor the same within this (units)

bool ValidGuest(uint32_t p) { return p >= 0x10000 && (p & 3) == 0; }

float LoadF(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  v = __builtin_bswap32(v);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
void StoreF(uint8_t* p, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}

// A bone record: three axis rows, then the origin (row vectors: a point p goes to p * rows + origin).
struct Mat {
  float r[3][3];
  float o[3];
};
Mat Load(const uint8_t* rec) {
  Mat m;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) m.r[i][j] = LoadF(rec + 16 * i + 4 * j);
  for (int j = 0; j < 3; ++j) m.o[j] = LoadF(rec + 48 + 4 * j);
  return m;
}
float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
void Cross(const float a[3], const float b[3], float out[3]) {
  out[0] = a[1] * b[2] - a[2] * b[1];
  out[1] = a[2] * b[0] - a[0] * b[2];
  out[2] = a[0] * b[1] - a[1] * b[0];
}
float Len(const float a[3]) { return std::sqrt(Dot(a, a)); }

// How far (degrees) a's axes turn to b's: the trace of b's rows times a's, the rows' lengths divided out.
float TurnDeg(const Mat& a, const Mat& b) {
  float tr = 0.0f;
  for (int i = 0; i < 3; ++i) {
    const float la = Len(a.r[i]), lb = Len(b.r[i]);
    if (la < 1e-6f || lb < 1e-6f) return 180.0f;
    tr += Dot(a.r[i], b.r[i]) / (la * lb);
  }
  return std::acos(std::clamp((tr - 1.0f) * 0.5f, -1.0f, 1.0f)) * 57.29578f;
}
float MoveUnits(const Mat& a, const Mat& b) {
  const float d[3] = {b.o[0] - a.o[0], b.o[1] - a.o[1], b.o[2] - a.o[2]};
  return Len(d);
}

// b relative to the anchor a (b = rel * a): b's rows in a's axes, b's origin from a's in a's axes.
Mat Relative(const Mat& a, const Mat& b) {
  Mat rel;
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j) rel.r[i][j] = Dot(b.r[i], a.r[j]);
  const float d[3] = {b.o[0] - a.o[0], b.o[1] - a.o[1], b.o[2] - a.o[2]};
  for (int j = 0; j < 3; ++j) rel.o[j] = Dot(d, a.r[j]);
  return rel;
}
bool SameRelative(const Mat& x, const Mat& y) {
  for (int i = 0; i < 3; ++i)
    for (int j = 0; j < 3; ++j)
      if (std::fabs(x.r[i][j] - y.r[i][j]) > kRigidRow) return false;
  for (int j = 0; j < 3; ++j)
    if (std::fabs(x.o[j] - y.o[j]) > kRigidOrigin) return false;
  return true;
}

// `back` of the way from cur to prev: the rows and the origin lerped, the rows made orthogonal again (with cur's
// handedness), each with its lerped length. False for a degenerate record (then nothing is written for it).
bool BlendRecord(const Mat& prev, const Mat& cur, float back, Mat& out) {
  float v[3][3], len[3];
  for (int i = 0; i < 3; ++i) {
    for (int j = 0; j < 3; ++j) v[i][j] = cur.r[i][j] + (prev.r[i][j] - cur.r[i][j]) * back;
    len[i] = Len(cur.r[i]) + (Len(prev.r[i]) - Len(cur.r[i])) * back;
  }
  float x[3], y[3], z[3], c[3];
  const float lx = Len(v[0]);
  if (lx < 1e-6f) return false;
  for (int j = 0; j < 3; ++j) x[j] = v[0][j] / lx;
  const float d = Dot(v[1], x);
  for (int j = 0; j < 3; ++j) y[j] = v[1][j] - d * x[j];
  const float ly = Len(y);
  if (ly < 1e-6f) return false;
  for (int j = 0; j < 3; ++j) y[j] /= ly;
  Cross(x, y, z);
  Cross(cur.r[0], cur.r[1], c);
  const float hand = Dot(c, cur.r[2]) < 0.0f ? -1.0f : 1.0f;
  for (int j = 0; j < 3; ++j) {
    out.r[0][j] = x[j] * len[0];
    out.r[1][j] = y[j] * len[1];
    out.r[2][j] = z[j] * hand * len[2];
    out.o[j] = cur.o[j] + (prev.o[j] - cur.o[j]) * back;
  }
  return true;
}

struct Snap {
  uint32_t counter = 0;  // the animation's update counter when it was taken
  int32_t lod[3] = {};
  uint32_t hash = 0;
  std::vector<uint8_t> raw;  // the records as the update left them, in guest byte order
};

struct State {
  uint32_t ent = 0, model = 0, bones = 0, n = 0;
  int32_t number = -1;
  int anchor[kAnchors] = {-1, -1, -1, -1};
  bool anchors_looked_up = false;
  Snap snap[3];  // [0] the last update's, [1] the one before, [2] the one before that
  int have = 0;
  bool pair = false;            // snap[1] -> snap[0] is blended
  std::vector<uint16_t> group;  // the bones blended
  float turn_deg = 0.0f;        // the pair's largest anchor turn
};
State g_state;

struct Written {
  uint32_t at;  // the record's guest address
  uint8_t before[kRecordSize];
  uint8_t after[kRecordSize];
};
std::vector<Written> g_written;

// The log's counts (game thread; the draw counts below are atomics: the renderer's thread is to be confirmed).
struct Counts {
  uint64_t updates = 0, pairs = 0, still = 0, not_consecutive = 0, aba = 0, no_anchor = 0;
  uint64_t guard_anchor_turn = 0, guard_anchor_move = 0, guard_bone_turn = 0, guard_bone_move = 0;
  uint64_t group_bones = 0, changed_not_group = 0;
  uint64_t frames = 0, blended = 0, verified = 0, no_pair = 0, back0 = 0, changed_since_update = 0;
  uint64_t restored = 0, kept = 0, undrawn = 0;
  float max_turn = 0.0f;
};
Counts g_counts;
std::chrono::steady_clock::time_point g_log_next{};

std::atomic<uint32_t> g_track_model{0};     // the ridden vehicle's model (0: none)
std::atomic<bool> g_window{false};          // a blend (verify only: its stand-in) is in the buffer
std::atomic<uint64_t> g_frame{0};           // Blend's calls
std::atomic<uint64_t> g_restored_frame{0};  // the frame whose blend the last Restore put back
std::atomic<std::thread::id> g_frame_thread{};
std::atomic<uint64_t> g_draws_in{0}, g_draws_late{0}, g_draws_other{0}, g_draws_window{0};

uint32_t Hash(const std::vector<uint8_t>& raw) {
  uint32_t h = 2166136261u;
  for (uint8_t b : raw) h = (h ^ b) * 16777619u;
  return h ? h : 1;
}

void Reset() {
  g_state = State{};
  g_track_model = 0;
}

// The anchors' bone indices, by the game's own lookup (the turret camera's two calls). Each call to 0x820CEBB8 may
// hold a reference to the name in the game's string table, as the camera's does: four per vehicle ridden.
void LookUpAnchors(PPCContext& ctx, uint8_t* base, State& s) {
  s.anchors_looked_up = true;
  g::Call call(ctx, base);
  for (int a = 0; a < kAnchors; ++a) {
    const uint32_t name = call.PushString(kAnchorNames[a]);
    const uint32_t token = uint32_t(call.Invoke(kNameToken, {name}));
    if (!token) continue;
    const int32_t index = int32_t(uint32_t(call.Invoke(kBoneOfName, {s.model, token})));
    if (index >= 0 && uint32_t(index) < s.n) s.anchor[a] = index;
  }
  if (REXCVAR_GET(cod3_debug_ride_bones))
    REXLOG_INFO("cod3: ride bones: vehicle {} (model {:08x}, {} bones): tag_turret {}, tag_barrel {}, "
                "tag_gunner_turret {}, tag_gunner_barrel {} (-1: none)",
                s.number, s.model, s.n, s.anchor[0], s.anchor[1], s.anchor[2], s.anchor[3]);
}

void MaybeLog() {
  if (!REXCVAR_GET(cod3_debug_ride_bones)) return;
  const auto now = std::chrono::steady_clock::now();
  if (now < g_log_next) return;
  const bool first = g_log_next == std::chrono::steady_clock::time_point{};
  g_log_next = now + std::chrono::seconds(1);
  const uint64_t in = g_draws_in.exchange(0), late = g_draws_late.exchange(0), other = g_draws_other.exchange(0);
  if (!first) {
    const Counts& c = g_counts;
    const State& s = g_state;
    REXLOG_INFO("cod3: ride bones: mode {} vehicle {} (model {:08x}, {} bones; anchors {} {} {} {}): updates {} "
                "(pairs {}, still {}, not in a row {}, A-B-A {}, no anchor {}); guards: anchor turn {} move {}, bone "
                "turn {} move {}; {:.1f} bones a pair (changed, not on the turret or gun: {}), largest turn {:.2f} deg "
                "an update; frames {}: blended {}, verify only {}, no pair {}, back 0 {}, changed since the update {}; "
                "restored {} bones, kept {}; the model drawn inside the blend {}, after its restore {}, on another "
                "thread {}; blends not drawn {}",
                REXCVAR_GET(cod3_blend_ride_bones), s.number, s.model, s.n, s.anchor[0], s.anchor[1], s.anchor[2],
                s.anchor[3], c.updates, c.pairs, c.still, c.not_consecutive, c.aba, c.no_anchor, c.guard_anchor_turn,
                c.guard_anchor_move, c.guard_bone_turn, c.guard_bone_move,
                c.pairs ? double(c.group_bones) / double(c.pairs) : 0.0, c.changed_not_group, c.max_turn, c.frames,
                c.blended, c.verified, c.no_pair, c.back0, c.changed_since_update, c.restored, c.kept, in, late,
                other, c.undrawn);
  }
  g_counts = Counts{};
}

}  // namespace

void AfterUpdate(PPCContext& ctx, uint8_t* base, uint32_t vehicle) {
  if (REXCVAR_GET(cod3_blend_ride_bones) == 0 || !ValidGuest(vehicle)) {
    if (g_state.ent) Reset();
    return;
  }
  const uint32_t model = g::ReadU32(base, vehicle + kEntModel);
  const uint32_t bones = ValidGuest(model) ? g::ReadU32(base, model + kModelBones) : 0;
  const uint32_t n = ValidGuest(model) ? g::ReadU8(base, model + kModelBoneCount) : 0;
  const int32_t number = g::ReadS32(base, vehicle + kEntNumber);
  if (!ValidGuest(bones) || n == 0 || n > kMaxBones) {
    if (g_state.ent) Reset();
    return;
  }
  State& s = g_state;
  if (s.ent != vehicle || s.model != model || s.bones != bones || s.n != n || s.number != number) {
    Reset();
    s.ent = vehicle;
    s.model = model;
    s.bones = bones;
    s.n = n;
    s.number = number;
  }
  if (!s.anchors_looked_up) LookUpAnchors(ctx, base, s);
  g_track_model = model;
  ++g_counts.updates;
  // The snapshots move down one; the new one is this update's.
  std::swap(s.snap[2], s.snap[1]);
  std::swap(s.snap[1], s.snap[0]);
  Snap& cur = s.snap[0];
  cur.counter = g::ReadU32(base, kAnimUpdateCounter);
  for (int i = 0; i < 3; ++i) cur.lod[i] = g::ReadS32(base, model + kModelLod + 4 * i);
  cur.raw.assign(base + bones + kRecords, base + bones + kRecords + n * kRecordSize);
  cur.hash = Hash(cur.raw);
  s.have = std::min(s.have + 1, 3);
  s.pair = false;
  s.group.clear();
  s.turn_deg = 0.0f;
  if (s.have < 2) return;
  const Snap& prev = s.snap[1];
  if (cur.counter != prev.counter + 1 || std::memcmp(cur.lod, prev.lod, sizeof(cur.lod)) != 0) {
    ++g_counts.not_consecutive;
    return;
  }
  if (cur.hash == prev.hash) {
    ++g_counts.still;  // nothing moved in this update
    return;
  }
  if (s.have >= 3 && cur.hash == s.snap[2].hash) {
    ++g_counts.aba;  // back to the pose before last: two writers, not a motion (NPCPOSE candidate 1)
    return;
  }
  Mat a_prev[kAnchors], a_cur[kAnchors];
  bool any_anchor = false;
  for (int a = 0; a < kAnchors; ++a) {
    if (s.anchor[a] < 0) continue;
    any_anchor = true;
    a_prev[a] = Load(prev.raw.data() + s.anchor[a] * kRecordSize);
    a_cur[a] = Load(cur.raw.data() + s.anchor[a] * kRecordSize);
    const float turn = TurnDeg(a_prev[a], a_cur[a]);
    if (turn > kMaxTurnDeg) {
      ++g_counts.guard_anchor_turn;
      return;
    }
    if (MoveUnits(a_prev[a], a_cur[a]) > kMaxAnchorMove) {
      ++g_counts.guard_anchor_move;
      return;
    }
    s.turn_deg = std::max(s.turn_deg, turn);
  }
  if (!any_anchor) {
    ++g_counts.no_anchor;
    return;
  }
  for (uint32_t b = 0; b < s.n; ++b) {
    const uint8_t* rp = prev.raw.data() + b * kRecordSize;
    const uint8_t* rc = cur.raw.data() + b * kRecordSize;
    if (std::memcmp(rp, rc, kRecordSize) == 0) continue;  // not moved
    const Mat mp = Load(rp), mc = Load(rc);
    bool rigid = false;
    for (int a = 0; a < kAnchors && !rigid; ++a)
      rigid = s.anchor[a] >= 0 && SameRelative(Relative(a_prev[a], mp), Relative(a_cur[a], mc));
    if (!rigid) {
      ++g_counts.changed_not_group;
      continue;
    }
    if (TurnDeg(mp, mc) > kMaxTurnDeg) {
      ++g_counts.guard_bone_turn;
      s.group.clear();
      return;
    }
    if (MoveUnits(mp, mc) > kMaxBoneMove) {
      ++g_counts.guard_bone_move;
      s.group.clear();
      return;
    }
    s.group.push_back(uint16_t(b));
  }
  s.pair = !s.group.empty();
  ++g_counts.pairs;
  g_counts.group_bones += s.group.size();
  g_counts.max_turn = std::max(g_counts.max_turn, s.turn_deg);
}

void Blend(uint8_t* base, uint32_t vehicle, float back) {
  Restore(base);  // a blend nothing put back (it would fail the check below and stay)
  ++g_frame;
  g_frame_thread = std::this_thread::get_id();
  MaybeLog();
  const int mode = REXCVAR_GET(cod3_blend_ride_bones);
  State& s = g_state;
  if (mode == 0 || !ValidGuest(vehicle) || vehicle != s.ent) return;
  ++g_counts.frames;
  if (!s.pair) {
    ++g_counts.no_pair;
    return;
  }
  if (back <= 0.0f) {
    ++g_counts.back0;
    return;
  }
  back = std::min(back, 1.0f);
  if (g::ReadU32(base, vehicle + kEntModel) != s.model || g::ReadU32(base, s.model + kModelBones) != s.bones) {
    Reset();
    return;
  }
  const Snap& cur = s.snap[0];
  const Snap& prev = s.snap[1];
  for (uint16_t b : s.group) {
    if (std::memcmp(base + s.bones + kRecords + b * kRecordSize, cur.raw.data() + b * kRecordSize, kRecordSize) != 0) {
      ++g_counts.changed_since_update;  // a writer after the update: leave the frame as it is
      return;
    }
  }
  g_draws_window = 0;
  g_window = true;
  if (mode == 2) {
    ++g_counts.verified;
    return;
  }
  g_written.reserve(s.group.size());
  for (uint16_t b : s.group) {
    Written w;
    w.at = s.bones + kRecords + b * kRecordSize;
    std::memcpy(w.before, base + w.at, kRecordSize);
    Mat out;
    if (!BlendRecord(Load(prev.raw.data() + b * kRecordSize), Load(cur.raw.data() + b * kRecordSize), back, out))
      continue;
    std::memcpy(w.after, w.before, kRecordSize);  // the w lanes stay
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j) StoreF(w.after + 16 * i + 4 * j, out.r[i][j]);
    for (int j = 0; j < 3; ++j) StoreF(w.after + 48 + 4 * j, out.o[j]);
    std::memcpy(base + w.at, w.after, kRecordSize);
    g_written.push_back(w);
  }
  ++g_counts.blended;
}

void Restore(uint8_t* base) {
  if (!g_window.load()) return;
  for (const Written& w : g_written) {
    if (std::memcmp(base + w.at, w.after, kRecordSize) == 0) {
      std::memcpy(base + w.at, w.before, kRecordSize);
      ++g_counts.restored;
    } else {
      ++g_counts.kept;  // written since the blend: that writer's value stays
    }
  }
  g_written.clear();
  if (g_draws_window.load() == 0) ++g_counts.undrawn;
  g_restored_frame = g_frame.load();
  g_window = false;
}

void AtDraw(uint8_t* base, uint32_t node) {
  const uint32_t track = g_track_model.load(std::memory_order_relaxed);
  if (!track || !ValidGuest(node)) return;
  const uint32_t model = g::ReadU32(base, node + kNodeModel);
  if (model != track || !(g::ReadU32(base, model + kModelArmed) & 1)) return;  // not this model, or not drawn now
  if (g_window.load()) {
    ++g_draws_in;
    ++g_draws_window;
  } else if (g_restored_frame.load() == g_frame.load()) {
    ++g_draws_late;  // after this frame's blend was put back: a reader outside the window
  }
  if (std::this_thread::get_id() != g_frame_thread.load()) ++g_draws_other;
}

void Forget() { Reset(); }

}  // namespace cod3::ride_bones
