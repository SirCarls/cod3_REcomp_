// cod3 - [claude-9b] the game's physics in fixed steps (cod3_physics_hz, 60 a
// second by default), whatever the frame rate. Campaign only for now.
//
// The campaign runs its whole game frame once per drawn frame: Com_Frame ->
// SV_Frame (0x82518E40) -> 0x825173F8 -> G_RunFrame (0x8256D3E0), with that
// frame's milliseconds (no fixed server rate as in Quake 3). G_RunFrame gives
// the physics update 0x824139B8 the frame's time in seconds (f1 = msec *
// 0.001; it caps it at 0.1 s). That one call runs every physics object
// (0x82413910 -> 0x82411B80 each), the ragdolls (below), the physics world's
// step (0x8217E998, the phys_* library) and the work that follows from them.
// It is G_RunFrame's only caller of it, and nothing on the client side steps
// the physics.
//
// Above 60 fps those steps get small, and objects behave in ways the console
// never showed: a piece of window glass and boxes hit hard enough to kill the
// player, and a tank can't push a door open (owner, 2026-10-15 and 10-21).
// Player movement is not affected (it measured the same at 60, 144 and an
// uncapped rate: bench c9b-1021 physics_fps).
//
// This hook keeps the frames' time and runs the update in whole steps of
// 1 / cod3_physics_hz seconds: none, one or several in a frame, at most 0.1 s
// worth (the game's own cap; time past it is dropped, as the game drops it).
// A step is taken once three quarters of one is due and the difference is
// carried, so at the rate itself every frame gets exactly one step even though
// the game's milliseconds alternate (16, 17, 17 at 60 fps). Between steps the
// physics objects keep their last position, so above the rate they move in
// 60 Hz steps on screen. 0 turns it off: one step a frame with the frame's
// time, as the game does.
//
// Ragdolls (owner 2026-10-23: "Please add ragdolls to this. When killing an
// enemy in the campaign it will sometimes create a 'ghost' fading in-out still
// playing their scripted animations"). A dead enemy's ragdoll is part of the
// same update, so it already steps at cod3_physics_hz with everything else:
//   - its owners are the list 0x8263F940 (count 0x8263FF44); an owner's +0x568
//     is its ragdoll (made by 0x8240FCB0 from a pool of 16), owner +0 its entity;
//   - before the world step, 0x82409FA8 keeps each owner's last animated pose
//     (0x82406118: the ragdoll starts with the animation's speed);
//   - after it, 0x8240FE08 -> 0x8240FD78 -> 0x8240DA08 writes the ragdoll's
//     pose into the entity's bones (entity +0x1FC: its model; model +0x70: the
//     bone matrices, 0x40 bytes each from +0x30): the 10 joints from their
//     bodies (0x8240D4C0, the root moving the entity too), then the blend from
//     the animation into the ragdoll (0x82405940, over ragdoll +0x2194 seconds)
//     or, once that is done, the whole ragdoll pose (0x82405F68).
// The animation still sets the same bones every frame, so the ragdoll's pose
// only stays on screen in a frame whose update wrote it after the animation.
// With fixed steps, a frame without a step showed the animation instead: above
// the rate the picture alternated between the falling body and the enemy's
// last animation, the "ghost". So in a frame without a step the pose part runs
// alone: 0x8240FE08 with no time, inside the physics scratch memory the update
// sets up for it (0x8217E9B8 / 0x8217EA38). Every time it is given only adds
// to timers (the blend, settling, the ragdoll's end), so no time means no
// change but the pose: the bodies don't move, the ragdoll is put back on the
// bones (in 60 Hz steps, like the other physics objects), and a blend still in
// progress follows the animation smoothly. cod3_physics_ragdoll_pose false
// leaves those frames as 2.16.1 had them; either way the log counts the frames
// in which the animation had moved a ragdoll's bones since its step.

#include <algorithm>
#include <cstdint>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/guest.h"

REXCVAR_DEFINE_INT32(cod3_physics_hz, 60, "cod3/Game",
                     "Physics steps a second, whatever the frame rate (window glass, boxes, doors, "
                     "debris, ragdolls). 0 = one step a frame with the frame's time, as the game "
                     "does, which changes how objects behave above 60 fps")
    .range(0, 240)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_BOOL(cod3_physics_ragdoll_pose, true, "cod3/Game",
                    "Ragdolls keep their pose in frames between physics steps (above "
                    "cod3_physics_hz frames a second). Off: those frames show the enemy's "
                    "animation instead, a flickering \"ghost\" of a dead enemy")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

using cod3::guest::ReadU32;
using cod3::guest::ReadU8;

constexpr double kMaxFrameTime = 0.1;  // seconds: the update's own cap (0x82001930)

// The ragdolls (see the top of the file).
constexpr uint32_t kRagdollOwners = 0x8263F940;      // u32[]: the objects that can have a ragdoll
constexpr uint32_t kRagdollOwnerCount = 0x8263FF44;  // s32: how many are in it
constexpr int kMaxRagdollOwners = int((kRagdollOwnerCount - kRagdollOwners) / 4);
constexpr uint32_t kOwnerRagdoll = 0x568;      // owner +0x568: its ragdoll (0: none)
constexpr uint32_t kEntityModel = 0x1FC;       // entity +0x1FC: its model
constexpr uint32_t kModelBones = 0x70;         // model +0x70: the bone matrices
constexpr uint32_t kBoneFirst = 0x30;          // the first bone matrix, from there
constexpr uint32_t kBoneSize = 0x40;           // one bone matrix
constexpr uint32_t kRagdollJointBone = 0x1EA4;  // ragdoll +0x1EA4 + 0x50 * joint: its bone
constexpr uint32_t kRagdollJointSize = 0x50;
constexpr int kRagdollJoints = 10;
constexpr uint32_t kMaxBone = 0xFF;  // a model has fewer bones than this (u8 counts)

// The physics scratch memory the update sets up around its work (0x824139B8).
constexpr uint32_t kPhysScratch = 0x82AB9C28;
constexpr uint32_t kPhysScratchSize = 0x129800;
constexpr uint32_t kPhysScratchInUse = 0x82A2AD18;  // u32: kPhysScratch while it's set up

double g_due = 0.0;   // seconds of physics time owed (can be a little below 0)
int g_logged_hz = -1;  // the rate last written to the log

// Counts for the log lines every 10 s of game time.
double g_window_time = 0.0;
uint32_t g_window_frames = 0, g_window_steps = 0;
uint32_t g_window_ragdoll_frames = 0;  // frames without a step that had a ragdoll
uint32_t g_window_ragdoll_moved = 0;   // ... in which the animation had moved its bones
uint64_t g_ragdoll_moved_total = 0;

// Each ragdoll's bones as its last step left them (a hash of its 10 joints' matrices).
struct StepPose {
  uint32_t ragdoll;
  uint64_t hash;
};
std::vector<StepPose> g_step_poses;

bool IsGuestData(uint32_t a) { return a >= 0x40000000u && (a & 3) == 0; }

// The owner's entity and ragdoll, or false when it has none (or something isn't set up).
bool OwnerRagdoll(uint8_t* base, int i, uint32_t* entity, uint32_t* ragdoll) {
  const uint32_t owner = ReadU32(base, kRagdollOwners + 4u * uint32_t(i));
  if (!IsGuestData(owner)) return false;
  *ragdoll = ReadU32(base, owner + kOwnerRagdoll);
  *entity = ReadU32(base, owner);
  return IsGuestData(*ragdoll) && IsGuestData(*entity);
}

int RagdollOwnerCount(uint8_t* base) {
  return std::clamp(int(ReadU32(base, kRagdollOwnerCount)), 0, kMaxRagdollOwners);
}

// FNV-1a over the bone matrices the ragdoll's joints write; 0 when the model isn't there.
uint64_t PoseHash(uint8_t* base, uint32_t entity, uint32_t ragdoll) {
  const uint32_t model = ReadU32(base, entity + kEntityModel);
  if (!IsGuestData(model)) return 0;
  const uint32_t bones = ReadU32(base, model + kModelBones);
  if (!IsGuestData(bones)) return 0;
  uint64_t h = 1469598103934665603ull;
  for (int j = 0; j < kRagdollJoints; ++j) {
    const uint32_t bone = ReadU32(base, ragdoll + kRagdollJointBone + kRagdollJointSize * uint32_t(j)) & 0x3FFFFFF;
    if (bone > kMaxBone) continue;
    const uint32_t m = bones + kBoneFirst + kBoneSize * bone;
    for (uint32_t k = 0; k < kBoneSize; ++k) {
      h ^= ReadU8(base, m + k);
      h *= 1099511628211ull;
    }
  }
  return h;
}

// After the frame's steps: what each ragdoll's bones are now.
void RememberStepPoses(uint8_t* base) {
  g_step_poses.clear();
  const int n = RagdollOwnerCount(base);
  for (int i = 0; i < n; ++i) {
    uint32_t entity, ragdoll;
    if (OwnerRagdoll(base, i, &entity, &ragdoll)) g_step_poses.push_back({ragdoll, PoseHash(base, entity, ragdoll)});
  }
}

// A frame without a step: has the animation moved a ragdoll's bones since its step? (The log's
// count of the "ghost".) True when there is a ragdoll at all.
bool CheckRagdollPoses(uint8_t* base) {
  const int n = RagdollOwnerCount(base);
  bool any = false, moved = false;
  for (int i = 0; i < n; ++i) {
    uint32_t entity, ragdoll;
    if (!OwnerRagdoll(base, i, &entity, &ragdoll)) continue;
    any = true;
    const uint64_t now = PoseHash(base, entity, ragdoll);
    for (const StepPose& p : g_step_poses)
      if (p.ragdoll == ragdoll && p.hash != now) moved = true;
  }
  if (any) ++g_window_ragdoll_frames;
  if (moved) {
    ++g_window_ragdoll_moved;
    ++g_ragdoll_moved_total;
  }
  return any;
}

uint32_t g_ragdolls_started = 0;

}  // namespace

// A ragdoll starts (r3 = its owner): logged, so a "ghost" report can be matched with one.
REX_EXTERN(__imp__sub_8240FCB0);
REX_HOOK_RAW(sub_8240FCB0) {
  const uint32_t owner = ctx.r3.u32;
  const bool had = IsGuestData(owner) && ReadU32(base, owner + kOwnerRagdoll) != 0;
  __imp__sub_8240FCB0(ctx, base);
  if (had || !IsGuestData(owner)) return;
  const uint32_t ragdoll = ReadU32(base, owner + kOwnerRagdoll);
  ++g_ragdolls_started;
  if (g_ragdolls_started <= 20 || g_ragdolls_started % 100 == 0)
    REXLOG_INFO("cod3: physics: ragdoll {} {} (entity {:08X}, ragdoll {:08X}; {} owners in use)", g_ragdolls_started,
                ragdoll ? "started" : "not started (none free)", ReadU32(base, owner), ragdoll,
                RagdollOwnerCount(base));
}

// The ragdolls' pose from their bodies, after the world step (f1 = the step's time).
REX_EXTERN(sub_8240FE08);
// The physics scratch memory for one update: r3 = the buffer, r4 = its size; and its release.
REX_EXTERN(sub_8217E9B8);
REX_EXTERN(sub_8217EA38);

// The physics update: f1 = the frame's time in seconds.
REX_EXTERN(__imp__sub_824139B8);
REX_HOOK_RAW(sub_824139B8) {
  const int hz = REXCVAR_GET(cod3_physics_hz);
  if (hz != g_logged_hz) {
    g_logged_hz = hz;
    if (hz > 0)
      REXLOG_INFO("cod3: physics: fixed steps of 1/{} s, whatever the frame rate (cod3_physics_hz)", hz);
    else
      REXLOG_INFO("cod3: physics: one step a frame with the frame's time, as the game does "
                  "(cod3_physics_hz 0)");
    g_due = 0.0;
    g_step_poses.clear();
  }
  const double frame_time = ctx.f1.f64;
  if (hz <= 0 || !(frame_time > 0.0)) {  // off, or a frame with no time: as the game does
    __imp__sub_824139B8(ctx, base);
    return;
  }

  const double step = double(float(1.0 / hz));  // the value the guest's single float holds
  const int max_steps = std::max(1, int(kMaxFrameTime * hz + 0.5));
  g_due += frame_time;
  int steps = 0;
  while (g_due >= 0.75 * step && steps < max_steps) {
    ctx.f1.f64 = step;
    __imp__sub_824139B8(ctx, base);
    g_due -= step;
    ++steps;
  }
  if (steps == max_steps && g_due > step) g_due = 0.0;  // past the cap: dropped, as the game does

  if (steps > 0) {
    RememberStepPoses(base);
  } else if (CheckRagdollPoses(base) && REXCVAR_GET(cod3_physics_ragdoll_pose)) {
    // No step this frame: only the ragdolls' pose, with no time, as the update does it.
    ctx.r3.s64 = int32_t(kPhysScratch);
    ctx.r4.s64 = int32_t(kPhysScratchSize);
    sub_8217E9B8(ctx, base);
    cod3::guest::WriteU32(base, kPhysScratchInUse, kPhysScratch);
    ctx.f1.f64 = 0.0;
    sub_8240FE08(ctx, base);
    cod3::guest::WriteU32(base, kPhysScratchInUse, 0);
    sub_8217EA38(ctx, base);
  }
  ctx.f1.f64 = frame_time;

  g_window_time += frame_time;
  ++g_window_frames;
  g_window_steps += uint32_t(steps);
  if (g_window_time >= 10.0) {
    REXLOG_DEBUG("cod3: physics: {} steps in {} frames over {:.1f} s ({:.1f} steps a second)",
                 g_window_steps, g_window_frames, g_window_time, g_window_steps / g_window_time);
    if (g_window_ragdoll_frames > 0)
      REXLOG_INFO("cod3: physics: ragdolls: {} frames without a step had one ({} frames and {} steps in "
                  "{:.1f} s); in {} of them the animation had moved a ragdoll's bones since its step ({} so "
                  "far): {} (cod3_physics_ragdoll_pose)",
                  g_window_ragdoll_frames, g_window_frames, g_window_steps, g_window_time,
                  g_window_ragdoll_moved, g_ragdoll_moved_total,
                  REXCVAR_GET(cod3_physics_ragdoll_pose) ? "the ragdoll's pose put back" : "left as they were");
    g_window_time = 0.0;
    g_window_frames = g_window_steps = 0;
    g_window_ragdoll_frames = g_window_ragdoll_moved = 0;
  }
}
