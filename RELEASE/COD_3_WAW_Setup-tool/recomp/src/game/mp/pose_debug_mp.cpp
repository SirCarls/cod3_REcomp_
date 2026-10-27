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

#include <algorithm>
#include <chrono>
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

void Report(Clock::time_point now) {
  const double seconds = std::chrono::duration<double>(now - g_window_start).count();
  std::vector<std::pair<uint32_t, Track>> list(g_tracks.begin(), g_tracks.end());
  std::sort(list.begin(), list.end(), [](const auto& a, const auto& b) {
    return a.second.aba != b.second.aba ? a.second.aba > b.second.aba : a.second.draws > b.second.draws;
  });
  REXLOG_INFO("cod3mp: pose: {} models drawn over {:.2f} s", list.size(), seconds);
  for (size_t i = 0; i < list.size() && i < 5; ++i) {
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
  if (now - g_window_start >= std::chrono::seconds(1)) Report(now);
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
  if (REXCVAR_GET(cod3_debug_pose_mp)) OnDraw(base, ctx.r3.u32);
  __imp__sub_82446458(ctx, base);
}

// [claude-9b] 2026-10-27 The characters' draw (cod3mp 0x8243ECA0, campaign 0x82427D18: r3 the model, r4 the entity):
// players' bodies don't go through the node draw (run c9b-test-split-1: only props and one 16-bone model were seen).
REX_EXTERN(__imp__sub_8243ECA0);
REX_HOOK_RAW(sub_8243ECA0) {
  if (REXCVAR_GET(cod3_debug_pose_mp)) OnDrawModel(base, ctx.r3.u32, ctx.r4.u32);
  __imp__sub_8243ECA0(ctx, base);
}
