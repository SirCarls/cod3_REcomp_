// cod3mp: [claude-9b] 2026-10-27 how rival players' poses change from one draw to the next (owner 2026-10-27 04:23:
// "some animations from npc or rival players seem off sometimes"), a measurement; no change in behavior. The campaign
// has its own (cod3_debug_pose, game/server_step.cpp); cod3mp draws other players from the client's own entities,
// posed on the client (dev/agents/claude-9b-think/FINDINGS-NPCPOSE.md Q5).
//
// The renderer's draw of a scene node (cod3mp 0x82446458, campaign 0x8242E450; r3: the node, node + 0x5C the model,
// node + 0x60 the entity) is hooked; the model's bones are [model + 0x70] + 0x30, 0x40 bytes each, model + 0xAB of
// them. With cod3_debug_pose_mp each draw hashes them, per model, and once a second the log says how many models were
// drawn and, for the five that alternated most, how often the pose went A-B-A (back to the one two draws before: a
// model drawn in two poses by turns) and how often it changed at all (a pose that only changes on some draws steps
// instead of moving smoothly). In split screen every view draws each model, so "changed" halves.
//
// [acct2-main] 2026-11-05 Owner 15:27 / 15:33: the other players "glide without walking" (4-player split screen).
// With cod3_debug_pose_motion_mp, once a second, for each drawn model of 20 bones or more (soldiers, players): how far
// its body went (entity + 0x150, the origin the game links; across, units a second), how far its bones went in model
// space (each bone record's +0x30 origin: the three that went furthest and the mean), and how often its pose changed.
// Walking legs travel about as far as the body or further; a body that moves while its bones stay put glides.
// Measurement only.
//
// [acct2-main] 2026-11-05 Owner 15:34: "player arms when driving in first person still has that jittering effect".
// With cod3_debug_ride_mp N, for N drawn frames while the local player rides a vehicle (-N: on foot too): the camera
// (cg + 4960, axes cg + 4976), and for each model drawn within 256 units of it (the vehicle, the player's body, the
// first-person weapon and arms) how far its bones moved on the screen since the last frame (in the camera's frame: a
// part that rides with the camera stays put), split into what its placement did (the refEntity's origin and axes, as
// RE_AddRefEntityToScene or the first-person weapon's 0x82433440 got them) and what its pose did (the bones in the
// model's own space, as the draws 0x8243ECA0 / 0x82446458 read them). Measurement only.

#include <algorithm>
#include <chrono>
#include <cmath>  // [acct2-main]
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/guest.h"

REXCVAR_DEFINE_BOOL(cod3_debug_pose_mp, false, "cod3",
                    "Log once a second how often drawn models' poses go back and forth (A-B-A) or change between "
                    "draws (rival players' animation)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [acct2-main] 2026-11-05 (see the note at the top)
REXCVAR_DEFINE_BOOL(cod3_debug_pose_motion_mp, false, "cod3",
                    "Log once a second how far each drawn soldier's body and bones moved (players that glide "
                    "without walking)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [acct2-main] 2026-11-05 (see the note at the top)
REXCVAR_DEFINE_INT32(cod3_debug_ride_mp, 0, "cod3",
                     "While the local player rides a vehicle, log this many drawn frames: how far each model "
                     "near the camera (the vehicle, the player's arms and body, the weapon) moved on the screen, "
                     "and whether its placement or its pose moved it; -N: on foot too; 0: off")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

namespace g = cod3::guest;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kNodeModel = 0x5C;
constexpr uint32_t kNodeEntity = 0x60;
constexpr uint32_t kModelBones = 0x70;
constexpr uint32_t kBonesFirst = 0x30;
constexpr uint32_t kBoneSize = 0x40;
constexpr uint32_t kModelBoneCount = 0xAB;
constexpr uint32_t kMaxBones = 160;
constexpr size_t kMaxModels = 2048;

struct Track {
  uint64_t hash[2] = {};  // the last draw's, the one before
  uint32_t draws = 0, aba = 0, changed = 0;
  uint32_t entity = 0, bones = 0;
};

std::mutex g_mutex;
std::unordered_map<uint32_t, Track> g_tracks;  // by model
Clock::time_point g_window_start = Clock::now();

bool Valid(uint32_t p) { return p >= 0x10000 && (p & 3) == 0; }

uint64_t PoseHash(uint8_t* base, uint32_t model, uint32_t* bones_out) {
  const uint32_t bones = g::ReadU32(base, model + kModelBones);
  const uint32_t count = g::ReadU8(base, model + kModelBoneCount);
  *bones_out = count;
  if (!Valid(bones) || count == 0 || count > kMaxBones) return 0;
  const uint8_t* p = g::Ptr(base, bones + kBonesFirst);
  uint64_t h = 1469598103934665603ull;
  for (uint32_t i = 0, n = count * kBoneSize; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
  return h;
}

// [acct2-main] 2026-11-05 cod3_debug_pose_motion_mp
constexpr uint32_t kEntityOrigin = 0x150;     // the linked origin (0x823EC930 copies a player's there)
constexpr uint32_t kEntityList = 0x82BCC7FC;  // -> the entity pointers, entity n at [n + 1]
constexpr uint32_t kBoneOrigin = 0x30;        // a bone record's fourth row
constexpr uint32_t kMinCharacterBones = 20;
constexpr float kMaxBoneStep = 64.0f;   // a bone that jumps further at once (a new level of detail, a spawn): left out
constexpr float kMaxBodyStep = 256.0f;  // a body that does (a spawn, a teleport): not counted

struct Motion {
  std::vector<float> last;    // each bone's origin at the last draw, xyz
  std::vector<float> travel;  // each bone's distance this second
  float x = 0, y = 0, body = 0;
  bool have_bones = false, have_body = false;
  uint32_t draws = 0, changes = 0, entity = 0, bones = 0;
};
std::unordered_map<uint32_t, Motion> g_motion;  // by model

int EntityNumber(uint8_t* base, uint32_t entity) {
  const uint32_t list = g::ReadU32(base, kEntityList);
  if (!Valid(list) || !Valid(entity)) return -1;
  for (uint32_t n = 0; n < 64; ++n)
    if (g::ReadU32(base, list + (n + 1) * 4) == entity) return static_cast<int>(n);
  return -1;
}

void NoteMotion(uint8_t* base, uint32_t model, uint32_t entity, uint32_t bone_buffer, uint32_t count) {
  Motion& m = g_motion[model];
  if (m.bones != count || m.entity != entity) {
    m = Motion{};
    m.bones = count;
    m.entity = entity;
    m.last.assign(count * 3, 0.0f);
    m.travel.assign(count, 0.0f);
  }
  ++m.draws;
  bool changed = false;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t a = bone_buffer + kBonesFirst + i * kBoneSize + kBoneOrigin;
    const float x = g::ReadF32(base, a), y = g::ReadF32(base, a + 4), z = g::ReadF32(base, a + 8);
    float* l = &m.last[i * 3];
    if (m.have_bones) {
      const float dx = x - l[0], dy = y - l[1], dz = z - l[2];
      const float d = std::sqrt(dx * dx + dy * dy + dz * dz);
      if (d > 0.0f) changed = true;
      if (d < kMaxBoneStep) m.travel[i] += d;
    }
    l[0] = x;
    l[1] = y;
    l[2] = z;
  }
  m.have_bones = true;
  if (changed) ++m.changes;
  if (!Valid(entity)) return;
  const float x = g::ReadF32(base, entity + kEntityOrigin), y = g::ReadF32(base, entity + kEntityOrigin + 4);
  if (m.have_body) {
    const float d = std::sqrt((x - m.x) * (x - m.x) + (y - m.y) * (y - m.y));
    if (d < kMaxBodyStep) m.body += d;
  }
  m.x = x;
  m.y = y;
  m.have_body = true;
}

void ReportMotion(uint8_t* base, double seconds) {
  std::vector<std::pair<uint32_t, Motion*>> list;
  for (auto& [model, m] : g_motion)
    if (m.draws > 0) list.push_back({model, &m});
  std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) { return a.second->entity < b.second->entity; });
  const double s = std::max(seconds, 0.001);
  REXLOG_INFO("cod3mp: pose motion: {} soldiers drawn over {:.2f} s", list.size(), seconds);
  for (const auto& [model, mp] : list) {
    const Motion& m = *mp;
    std::vector<uint32_t> order(m.travel.size());
    double sum = 0;
    for (uint32_t i = 0; i < order.size(); ++i) {
      order[i] = i;
      sum += m.travel[i];
    }
    const size_t k = std::min<size_t>(3, order.size());
    std::partial_sort(order.begin(), order.begin() + k, order.end(),
                      [&](uint32_t a, uint32_t b) { return m.travel[a] > m.travel[b]; });
    uint32_t top[3] = {0, 0, 0};
    for (size_t i = 0; i < k; ++i) top[i] = order[i];
    auto t = [&](int j) { return m.travel.empty() ? 0.0 : m.travel[top[j]] / s; };
    REXLOG_INFO("cod3mp: pose motion:   entity {} ({:#x}, model {:#x}, {} bones): body {:.0f} u/s across; bones "
                "{:.0f} {:.0f} {:.0f} u/s (#{} #{} #{}), mean {:.1f}; pose changed {} times in {} draws",
                EntityNumber(base, m.entity), m.entity, model, m.bones, m.body / s, t(0), t(1), t(2), top[0], top[1],
                top[2], m.travel.empty() ? 0.0 : sum / double(m.travel.size()) / s, m.changes, m.draws);
  }
  for (auto it = g_motion.begin(); it != g_motion.end();) {
    if (it->second.draws == 0) {
      it = g_motion.erase(it);
      continue;
    }
    Motion& m = it->second;
    std::fill(m.travel.begin(), m.travel.end(), 0.0f);
    m.body = 0;
    m.draws = m.changes = 0;
    ++it;
  }
}

// [acct2-main] 2026-11-05 cod3_debug_ride_mp
constexpr uint32_t kActiveLocalClient = 0x82BCBE9C;  // the view being built (0x82469968 reads it)
constexpr uint32_t kCgBase = 0x82C7D490;
constexpr uint32_t kCgSize = 0x18E0;
constexpr uint32_t kCgViewOrigin = 0x1360;  // cg + 4960
constexpr uint32_t kCgViewAxis = 0x1370;    // cg + 4976: forward, left, up
constexpr uint32_t kEntFlags = 0x10;
constexpr uint32_t kEntOnVehicle = 0x00100000;  // riding (0x82469968 places the body by the seat then)
constexpr uint32_t kEntClient = 0x200;          // the gclient / playerState
constexpr uint32_t kPsSeatA = 0x514, kPsSeatB = 0x518;  // read by 0x82469968 for a rider's view
constexpr uint32_t kRefFx = 0x04, kRefAxis = 0x14, kRefOrigin = 0x3C, kRefModel = 0x54, kRefEntity = 0x58;
constexpr uint32_t kModelFirstBone = 0x68;  // the characters' draw starts at this bone record
constexpr float kRideNear = 256.0f;
constexpr auto kNewFrameGap = std::chrono::microseconds(3000);

struct RidePlace {
  uint64_t frame = 0;
  float o[3] = {}, ax[9] = {};
  uint32_t fx = 0, entity = 0;
};
struct RideShake {
  uint64_t frame = 0;
  uint32_t count = 0;
  std::vector<float> cam, pose;  // the last draw's bones: in the camera's frame, in the model's
  float org[3] = {};             // the last draw's placement origin in the camera's frame
  float ax[9] = {};              // and its axes
};
struct RideView {
  float o[3] = {}, ax[9] = {};
};
uint64_t g_ride_frame = 0;
int g_ride_client = -1;
Clock::time_point g_ride_last_add;
Clock::time_point g_ride_frame_start;
double g_ride_dt_ms = 0;
int32_t g_ride_setting = 0;
int32_t g_ride_left = 0;
bool g_ride_frame_on = false;  // this frame is being traced
RideView g_ride_view;
uint64_t g_ride_view_frame[4] = {}, g_ride_view_prev[4] = {};  // each view's traced frame, and the one before
std::unordered_map<uint32_t, RidePlace> g_ride_place;  // by model, this frame
std::unordered_map<uint64_t, RideShake> g_ride_shake;  // by view and model, across frames

void ReadVec3(uint8_t* base, uint32_t a, float* v) {
  for (int i = 0; i < 3; ++i) v[i] = g::ReadF32(base, a + 4 * i);
}
float Dot3(const float* a, const float* b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
// A world point into the camera's frame (forward, left, up).
void ToCamera(const RideView& v, const float* w, float* c) {
  const float d[3] = {w[0] - v.o[0], w[1] - v.o[1], w[2] - v.o[2]};
  for (int i = 0; i < 3; ++i) c[i] = Dot3(d, v.ax + 3 * i);
}
float Dist3(const float* a, const float* b) {
  const float d[3] = {a[0] - b[0], a[1] - b[1], a[2] - b[2]};
  return std::sqrt(Dot3(d, d));
}

// Called with g_mutex held, at each refEntity the frame adds: a new frame starts with another view or after a gap.
void RideFrameCheck(uint8_t* base) {
  const auto now = Clock::now();
  const uint32_t client = g::ReadU32(base, kActiveLocalClient);
  if (client > 3) return;
  const bool new_frame = int(client) != g_ride_client || now - g_ride_last_add > kNewFrameGap;
  g_ride_last_add = now;
  if (!new_frame) return;
  g_ride_dt_ms = std::chrono::duration<double, std::milli>(now - g_ride_frame_start).count();
  g_ride_frame_start = now;
  g_ride_client = int(client);
  ++g_ride_frame;
  g_ride_place.clear();
  g_ride_frame_on = false;
  const int32_t setting = REXCVAR_GET(cod3_debug_ride_mp);
  if (setting != g_ride_setting) {
    g_ride_setting = setting;
    g_ride_left = setting < 0 ? -setting : setting;
    g_ride_shake.clear();
  }
  if (g_ride_left <= 0) return;
  const uint32_t list = g::ReadU32(base, kEntityList);
  if (!Valid(list)) return;
  const uint32_t player = g::ReadU32(base, list + (client + 1) * 4);
  if (!Valid(player)) return;
  const bool riding = (g::ReadU32(base, player + kEntFlags) & kEntOnVehicle) != 0;
  if (!riding && setting > 0) return;
  const uint32_t cg = kCgBase + client * kCgSize;
  ReadVec3(base, cg + kCgViewOrigin, g_ride_view.o);
  for (int i = 0; i < 3; ++i) ReadVec3(base, cg + kCgViewAxis + 12 * i, g_ride_view.ax + 3 * i);
  const uint32_t ps = g::ReadU32(base, player + kEntClient);
  const uint32_t seat_a = Valid(ps) ? g::ReadU32(base, ps + kPsSeatA) : 0;
  const uint32_t seat_b = Valid(ps) ? g::ReadU32(base, ps + kPsSeatB) : 0;
  float body[3];
  ReadVec3(base, player + kEntityOrigin, body);
  float body_c[3];
  ToCamera(g_ride_view, body, body_c);
  REXLOG_INFO("cod3mp: ride: frame {} +{:.2f} ms, view {}: camera {:.2f} {:.2f} {:.2f} forward {:.4f} {:.4f} {:.4f}; "
              "riding {}, seat {} {}; the player's origin in the camera's frame {:.2f} {:.2f} {:.2f}",
              g_ride_frame, g_ride_dt_ms, client, g_ride_view.o[0], g_ride_view.o[1], g_ride_view.o[2],
              g_ride_view.ax[0], g_ride_view.ax[1], g_ride_view.ax[2], riding ? 1 : 0, seat_a, seat_b, body_c[0],
              body_c[1], body_c[2]);
  g_ride_frame_on = true;
  g_ride_view_prev[client] = g_ride_view_frame[client];
  g_ride_view_frame[client] = g_ride_frame;
  if (--g_ride_left == 0)
    REXLOG_INFO("cod3mp: ride: that was the last of the {} frames (cod3_debug_ride_mp)",
                setting < 0 ? -setting : setting);
}

void RideNotePlace(uint8_t* base, uint32_t ref) {
  if (!g_ride_frame_on || !Valid(ref)) return;
  const uint32_t model = g::ReadU32(base, ref + kRefModel);
  if (!Valid(model)) return;
  RidePlace p;
  ReadVec3(base, ref + kRefOrigin, p.o);
  if (!std::isfinite(p.o[0]) || Dist3(p.o, g_ride_view.o) > kRideNear) return;
  for (int i = 0; i < 3; ++i) ReadVec3(base, ref + kRefAxis + 12 * i, p.ax + 3 * i);
  p.frame = g_ride_frame;
  p.fx = g::ReadU32(base, ref + kRefFx);
  p.entity = g::ReadU32(base, ref + kRefEntity);
  if (g_ride_place.size() < 256) g_ride_place[model] = p;
}

// At the characters' draw: the bones as drawn, against this frame's placement and camera.
void RideNoteBones(uint8_t* base, uint32_t model) {
  if (!g_ride_frame_on) return;
  const auto it = g_ride_place.find(model);
  if (it == g_ride_place.end() || it->second.frame != g_ride_frame) return;
  const RidePlace& p = it->second;
  RideShake& s = g_ride_shake[(uint64_t(g_ride_client) << 32) | model];
  if (s.frame == g_ride_frame) return;  // drawn twice in the frame: once is enough
  const uint32_t buffer = g::ReadU32(base, model + kModelBones);
  const uint32_t first = g::ReadU8(base, model + kModelFirstBone);
  const uint32_t count = g::ReadU8(base, model + kModelBoneCount);
  if (!Valid(buffer) || count == 0 || count > kMaxBones) return;
  std::vector<float> cam(count * 3), pose(count * 3);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t a = buffer + kBonesFirst + (first + i) * kBoneSize + kBoneOrigin;
    float m[3], w[3];
    ReadVec3(base, a, m);
    for (int k = 0; k < 3; ++k) w[k] = p.o[k] + m[0] * p.ax[k] + m[1] * p.ax[3 + k] + m[2] * p.ax[6 + k];
    ToCamera(g_ride_view, w, &cam[i * 3]);
    for (int k = 0; k < 3; ++k) pose[i * 3 + k] = m[k];
  }
  float org[3], ax[9];
  ToCamera(g_ride_view, p.o, org);
  for (int i = 0; i < 3; ++i)
    for (int k = 0; k < 3; ++k) ax[3 * i + k] = Dot3(p.ax + 3 * i, g_ride_view.ax + 3 * k);
  const bool follows = s.frame != 0 && s.frame == g_ride_view_prev[g_ride_client] && s.count == count;
  if (follows) {
    std::vector<float> moved(count), posed(count);
    uint32_t worst = 0;
    for (uint32_t i = 0; i < count; ++i) {
      moved[i] = Dist3(&cam[i * 3], &s.cam[i * 3]);
      posed[i] = Dist3(&pose[i * 3], &s.pose[i * 3]);
      if (moved[i] > moved[worst]) worst = i;
    }
    const float worst_moved = moved[worst], worst_posed = posed[worst];
    std::vector<float> sorted = moved;
    std::nth_element(sorted.begin(), sorted.begin() + count / 2, sorted.end());
    float turn = 0;  // the placement's axes against the camera's, largest change of a component
    for (int i = 0; i < 9; ++i) turn = std::max(turn, std::fabs(ax[i] - s.ax[i]));
    const float dist = std::sqrt(Dot3(org, org));
    REXLOG_INFO("cod3mp: ride:   frame {} model {:#x} (entity {}, fx {:#x}, {} bones, {:.0f} u away): on the screen "
                "the bones moved {:.3f} (median) to {:.3f} u (#{}; its pose moved it {:.3f}); the placement moved "
                "{:.3f} u and turned {:.5f}; at {:.2f} {:.2f} {:.2f}",
                g_ride_frame, model, EntityNumber(base, p.entity), p.fx, count, dist, sorted[count / 2], worst_moved,
                worst, worst_posed, Dist3(org, s.org), turn, cam[worst * 3], cam[worst * 3 + 1], cam[worst * 3 + 2]);
  }
  s.frame = g_ride_frame;
  s.count = count;
  s.cam.swap(cam);
  s.pose.swap(pose);
  std::copy(org, org + 3, s.org);
  std::copy(ax, ax + 9, s.ax);
  if (g_ride_shake.size() > kMaxModels) g_ride_shake.clear();
}

void Report(uint8_t* base, Clock::time_point now) {
  const double seconds = std::chrono::duration<double>(now - g_window_start).count();
  std::vector<std::pair<uint32_t, Track>> list(g_tracks.begin(), g_tracks.end());
  std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
    return a.second.aba != b.second.aba ? a.second.aba > b.second.aba : a.second.draws > b.second.draws;
  });
  if (REXCVAR_GET(cod3_debug_pose_motion_mp)) ReportMotion(base, seconds);  // [acct2-main]
  if (REXCVAR_GET(cod3_debug_pose_mp)) REXLOG_INFO("cod3mp: pose: {} models drawn over {:.2f} s", list.size(), seconds);
  for (size_t i = 0; i < list.size() && i < 5 && REXCVAR_GET(cod3_debug_pose_mp); ++i) {
    const Track& t = list[i].second;
    REXLOG_INFO("cod3mp: pose:   model {:#x} (entity {:#x}, {} bones): {} draws, A-B-A {}, changed {}", list[i].first,
                t.entity, t.bones, t.draws, t.aba, t.changed);
  }
  g_tracks.clear();
  g_window_start = now;
}

void OnDrawModel(uint8_t* base, uint32_t model, uint32_t entity) {
  if (!Valid(model)) return;
  uint32_t bones = 0;
  const uint64_t h = PoseHash(base, model, &bones);
  if (h == 0) return;
  std::lock_guard<std::mutex> lock(g_mutex);
  const auto now = Clock::now();
  if (now - g_window_start >= std::chrono::seconds(1)) Report(base, now);
  // [acct2-main] cod3_debug_pose_motion_mp
  if (REXCVAR_GET(cod3_debug_pose_motion_mp) && bones >= kMinCharacterBones && g_motion.size() < kMaxModels)
    NoteMotion(base, model, entity, g::ReadU32(base, model + kModelBones), bones);
  if (!REXCVAR_GET(cod3_debug_pose_mp)) return;
  if (g_tracks.size() >= kMaxModels && g_tracks.find(model) == g_tracks.end()) return;
  Track& t = g_tracks[model];
  if (t.draws > 0 && h != t.hash[0]) ++t.changed;
  if (t.draws > 1 && h == t.hash[1] && h != t.hash[0]) ++t.aba;
  t.hash[1] = t.hash[0];
  t.hash[0] = h;
  ++t.draws;
  t.entity = entity;
  t.bones = bones;
}

void OnDraw(uint8_t* base, uint32_t node) {
  if (!Valid(node)) return;
  OnDrawModel(base, g::ReadU32(base, node + kNodeModel), g::ReadU32(base, node + kNodeEntity));
}

}  // namespace

REX_EXTERN(__imp__sub_82446458);
REX_HOOK_RAW(sub_82446458) {
  if (REXCVAR_GET(cod3_debug_pose_mp) || REXCVAR_GET(cod3_debug_pose_motion_mp)) OnDraw(base, ctx.r3.u32);
  if ((g_ride_setting != 0 || REXCVAR_GET(cod3_debug_ride_mp) != 0) && Valid(ctx.r3.u32)) {  // [acct2-main]
    std::lock_guard<std::mutex> lock(g_mutex);
    RideNoteBones(base, g::ReadU32(base, ctx.r3.u32 + kNodeModel));
  }
  __imp__sub_82446458(ctx, base);
}

// [claude-9b] 2026-10-27 The characters' draw (cod3mp 0x8243ECA0, campaign 0x82427D18: r3 the model, r4 the entity):
// players' bodies don't go through the node draw (run c9b-test-split-1: only props and one 16-bone model were seen).
REX_EXTERN(__imp__sub_8243ECA0);
REX_HOOK_RAW(sub_8243ECA0) {
  if (REXCVAR_GET(cod3_debug_pose_mp) || REXCVAR_GET(cod3_debug_pose_motion_mp))
    OnDrawModel(base, ctx.r3.u32, ctx.r4.u32);
  if (g_ride_setting != 0 || REXCVAR_GET(cod3_debug_ride_mp) != 0) {  // [acct2-main] cod3_debug_ride_mp
    std::lock_guard<std::mutex> lock(g_mutex);
    RideNoteBones(base, ctx.r3.u32);
  }
  __imp__sub_8243ECA0(ctx, base);
}

// [acct2-main] 2026-11-05 cod3_debug_ride_mp: RE_AddRefEntityToScene(refEntity), each model into this frame's scene
// with its placement (the campaign's is hooked in game/server_step.cpp; this file is multiplayer only).
REX_EXTERN(__imp__RE_AddRefEntityToScene);
REX_HOOK_RAW(RE_AddRefEntityToScene) {
  if (g_ride_setting != 0 || REXCVAR_GET(cod3_debug_ride_mp) != 0) {
    std::lock_guard<std::mutex> lock(g_mutex);
    RideFrameCheck(base);
    RideNotePlace(base, ctx.r3.u32);
  }
  __imp__RE_AddRefEntityToScene(ctx, base);
}

// [acct2-main] 2026-11-05 cod3_debug_ride_mp: 0x82433440(refEntity), the first-person weapon and arms into the scene
// (its own list; only the view weapon's draw 0x82468D58 calls it; campaign 0x8241B6F0). The same refEntity layout.
REX_EXTERN(__imp__sub_82433440);
REX_HOOK_RAW(sub_82433440) {
  if (g_ride_setting != 0 || REXCVAR_GET(cod3_debug_ride_mp) != 0) {
    std::lock_guard<std::mutex> lock(g_mutex);
    RideFrameCheck(base);
    RideNotePlace(base, ctx.r3.u32);
    // Posed just before this call (0x82468D58 advances its animation by the frame's time, then places it): read the
    // bones now; the renderer's draw of it may not go through the two draws hooked below.
    if (Valid(ctx.r3.u32)) RideNoteBones(base, g::ReadU32(base, ctx.r3.u32 + kRefModel));
  }
  __imp__sub_82433440(ctx, base);
}

