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
//
// [acct2-main] 2026-10-25 Vehicle wheels (owner 2026-10-25: "vechicle's wheels seem bugged Tank
// treads included in this. It is tied to the 60fps psychics patch"). The same thing as the ragdolls:
// the physics vehicles (jeeps, tanks) are stepped inside the update, 0x82411AD8 -> 0x824113F8 for
// each one in the list 0x826A0260 (count 0x826A02DC), and after its step 0x824113F8 writes each
// wheel's pose into the entity's bones: up to 8 wheels, vehicle +36 + 4 * wheel their bones,
// 0x82405090 the wheel's matrix (its spin and suspension), 0x820C4118 through the body's transform
// (vehicle +628), then into the bone matrices (entity +0x1FC model, +0x70, +0x30 + 0x40 * bone); a
// vehicle with flag 0x20 (vehicle +640) has its own pose pass, 0x824067B8. The animation sets those
// bones every frame, so above cod3_physics_hz a frame without a step showed the wheels as the
// animation leaves them: the wheels and the tank treads on them jumped between the two. So in a frame
// without a step the wheels' pose is put back as the step left it (cod3_physics_vehicle_wheels, on):
// the same calls, nothing stepped and no time passes. The log counts the frames in which the
// animation had moved a vehicle's wheel bones since its step. The tread scrolling itself
// (0x8253B5F0, the vehicle's think) runs every frame with the frame's time and is not changed.

#include <algorithm>
#include <chrono>  // [acct2-main] the gap between updates (a level load)
#include <cstdint>
#include <cstring>  // [acct2-main] std::memcpy
#include <string>
#include <vector>

#include <fmt/format.h>

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

// [acct2-main] 2026-10-25: the vehicles' wheels between physics steps (see the top of the file).
REXCVAR_DEFINE_BOOL(cod3_physics_vehicle_wheels, true, "cod3/Game",
                    "Vehicles' wheels (and the tank treads on them) keep their pose in frames between "
                    "physics steps (above cod3_physics_hz frames a second). Off: those frames show the "
                    "wheels as the animation leaves them, so they jump between the two")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

// [acct2-main] 2026-10-25 diagnostic: the physics vehicles' wheel states in the log every 10 s.
REXCVAR_DEFINE_BOOL(cod3_physics_vehicle_log, false, "cod3/Game",
                    "Log the physics vehicles' wheel states every 10 s (a diagnostic for the wheel height)")
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

// [acct2-main] 2026-10-25 The physics vehicles' wheels (see the top of the file).
constexpr uint32_t kPhysVehicles = 0x826A0260;      // u32[]: the physics vehicles 0x82411AD8 steps
constexpr uint32_t kPhysVehicleCount = 0x826A02DC;  // s32: how many are in it
constexpr int kMaxPhysVehicles = int((kPhysVehicleCount - kPhysVehicles) / 4);
constexpr uint32_t kVehFlags = 640;          // vehicle +640: bit 0 in use; 0x20 its own pose pass (0x824067B8)
constexpr uint32_t kVehWheelBones = 36;      // vehicle +36: s32[8], each wheel's bone (below 0: none)
constexpr int kVehWheels = 8;
constexpr uint32_t kVehEntity = 624;         // vehicle +624: its entity
constexpr uint32_t kVehBody = 628;           // vehicle +628: its physics body
constexpr uint32_t kModelBoneCount = 171;    // model +171: u8, how many bones
constexpr uint32_t kVehWheelStates = 784;    // vehicle +784 + 4 * wheel: the wheel's state (0x82405090
                                             //   reads it; null until the vehicle's first step)
constexpr uint32_t kVehOther = 592;          // vehicle +592: read by both pose passes

struct StepWheels {
  uint32_t vehicle;
  uint64_t hash;
};
std::vector<StepWheels> g_step_wheels;
uint32_t g_window_wheel_frames = 0;  // frames without a step that had a physics vehicle
uint32_t g_window_wheel_moved = 0;   // ... in which the animation had moved its wheel bones
uint64_t g_wheel_moved_total = 0;
int g_window_vehicles = 0;           // the most physics vehicles in use in the window

int PhysVehicleCount(uint8_t* base) {
  return std::clamp(int(ReadU32(base, kPhysVehicleCount)), 0, kMaxPhysVehicles);
}

// The vehicle in use at list slot i, its model; false when it isn't there.
bool PhysVehicle(uint8_t* base, int i, uint32_t* vehicle, uint32_t* model) {
  const uint32_t v = ReadU32(base, kPhysVehicles + 4u * uint32_t(i));
  if (!IsGuestData(v) || !(ReadU32(base, v + kVehFlags) & 1)) return false;
  const uint32_t entity = ReadU32(base, v + kVehEntity);
  if (!IsGuestData(entity)) return false;
  *model = ReadU32(base, entity + kEntityModel);
  *vehicle = v;
  return IsGuestData(*model) && IsGuestData(ReadU32(base, *model + kModelBones));
}

// FNV-1a over the vehicle's wheel bone matrices.
uint64_t WheelHash(uint8_t* base, uint32_t vehicle, uint32_t model) {
  const uint32_t bones = ReadU32(base, model + kModelBones);
  const uint32_t count = ReadU8(base, model + kModelBoneCount);
  uint64_t h = 1469598103934665603ull;
  for (int w = 0; w < kVehWheels; ++w) {
    const int32_t bone = cod3::guest::ReadS32(base, vehicle + kVehWheelBones + 4u * uint32_t(w));
    if (bone < 0 || uint32_t(bone) >= count) continue;
    const uint32_t m = bones + kBoneFirst + kBoneSize * uint32_t(bone);
    for (uint32_t k = 0; k < kBoneSize; ++k) {
      h ^= ReadU8(base, m + k);
      h *= 1099511628211ull;
    }
  }
  return h;
}

// After the frame's steps: what each vehicle's wheel bones are now.
void RememberStepWheels(uint8_t* base) {
  g_step_wheels.clear();
  const int n = PhysVehicleCount(base);
  for (int i = 0; i < n; ++i) {
    uint32_t vehicle, model;
    if (PhysVehicle(base, i, &vehicle, &model)) g_step_wheels.push_back({vehicle, WheelHash(base, vehicle, model)});
  }
}

// A frame without a step: has the animation moved a vehicle's wheel bones since its step? True when
// there is a physics vehicle at all.
bool CheckStepWheels(uint8_t* base) {
  const int n = PhysVehicleCount(base);
  int vehicles = 0;
  bool moved = false;
  for (int i = 0; i < n; ++i) {
    uint32_t vehicle, model;
    if (!PhysVehicle(base, i, &vehicle, &model)) continue;
    ++vehicles;
    const uint64_t now = WheelHash(base, vehicle, model);
    for (const StepWheels& s : g_step_wheels)
      if (s.vehicle == vehicle && s.hash != now) moved = true;
  }
  g_window_vehicles = std::max(g_window_vehicles, vehicles);
  if (vehicles > 0) ++g_window_wheel_frames;
  if (moved) {
    ++g_window_wheel_moved;
    ++g_wheel_moved_total;
  }
  return vehicles > 0;
}

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
// [acct2-main] A wheel's matrix (r3 = the vehicle, r4 = the wheel, r5 = 64 bytes out), a matrix
// through a transform (r3 = out, r4 = the transform, r5 = in), and the flag-0x20 vehicles' pose pass.
REX_EXTERN(sub_82405090);
REX_EXTERN(sub_820C4118);
REX_EXTERN(sub_824067B8);

namespace {

// [acct2-main] What 0x824113F8 does after a vehicle's step, for every physics vehicle: its wheels'
// pose into the bones. Nothing moves and no time passes. The 64-byte matrix sits on the guest stack,
// above a frame made for the calls. Only vehicles that have had a step since the last remembered one
// (g_step_wheels), and only wheels whose state exists: a vehicle that has just appeared has no wheel
// states before its first step (2.18.3 read them anyway and crashed at the start of four levels).
bool SteppedVehicle(uint32_t vehicle) {
  for (const StepWheels& s : g_step_wheels)
    if (s.vehicle == vehicle) return true;
  return false;
}

void PutBackWheelPoses(PPCContext& ctx, uint8_t* base) {
  const uint64_t saved_r1 = ctx.r1.u64;
  const uint32_t old_r1 = ctx.r1.u32;
  const uint32_t frame = (old_r1 - 0x100u) & ~0xFu;
  const uint32_t matrix = frame + 0x80u;  // 16-aligned, between the new stack pointer and the old one
  cod3::guest::WriteU32(base, frame, old_r1);  // the back chain, as stwu leaves it
  ctx.r1.u64 = frame;
  const int n = PhysVehicleCount(base);
  for (int i = 0; i < n; ++i) {
    uint32_t vehicle, model;
    if (!PhysVehicle(base, i, &vehicle, &model) || !SteppedVehicle(vehicle)) continue;
    if (!IsGuestData(ReadU32(base, vehicle + kVehBody)) || !IsGuestData(ReadU32(base, vehicle + kVehOther)))
      continue;
    if (ReadU32(base, vehicle + kVehFlags) & 0x20) {
      // 0x824067B8 reads the states of wheels 0, 2 and 4.
      if (!IsGuestData(ReadU32(base, vehicle + kVehWheelStates)) ||
          !IsGuestData(ReadU32(base, vehicle + kVehWheelStates + 8)) ||
          !IsGuestData(ReadU32(base, vehicle + kVehWheelStates + 16)))
        continue;
      ctx.r3.s64 = int32_t(vehicle);
      sub_824067B8(ctx, base);
      continue;
    }
    for (int w = 0; w < kVehWheels; ++w) {
      const int32_t bone = cod3::guest::ReadS32(base, vehicle + kVehWheelBones + 4u * uint32_t(w));
      if (bone < 0 || !IsGuestData(ReadU32(base, vehicle + kVehWheelStates + 4u * uint32_t(w)))) continue;
      const uint32_t m = ReadU32(base, ReadU32(base, vehicle + kVehEntity) + kEntityModel);
      if (!IsGuestData(m) || ReadU8(base, m + kModelBoneCount) < uint32_t(bone)) continue;  // as the game checks
      ctx.r3.s64 = int32_t(vehicle);
      ctx.r4.s64 = w;
      ctx.r5.s64 = int32_t(matrix);
      sub_82405090(ctx, base);
      ctx.r3.s64 = int32_t(matrix);
      ctx.r4.s64 = int32_t(ReadU32(base, vehicle + kVehBody));
      ctx.r5.s64 = int32_t(matrix);
      sub_820C4118(ctx, base);
      const uint32_t bones = ReadU32(base, m + kModelBones);
      if (!IsGuestData(bones)) continue;
      std::memcpy(base + bones + kBoneFirst + kBoneSize * uint32_t(bone), base + matrix, kBoneSize);
    }
  }
  ctx.r1.u64 = saved_r1;
}

}  // namespace

// [acct2-main] diagnostic: the physics vehicles' wheel states (cod3_physics_vehicle_log).
void LogVehicles(uint8_t* base) {
  if (!REXCVAR_GET(cod3_physics_vehicle_log)) return;
  const int n = PhysVehicleCount(base);
  for (int i = 0; i < n && i < 4; ++i) {
    const uint32_t v = ReadU32(base, kPhysVehicles + 4u * uint32_t(i));
    if (!IsGuestData(v)) continue;
    std::string text = fmt::format("cod3: physics: vehicle {} {:08X} flags {:X}:", i, v, ReadU32(base, v + kVehFlags));
    for (int w = 0; w < kVehWheels; ++w) {
      const uint32_t st = ReadU32(base, v + kVehWheelStates + 4u * uint32_t(w));
      if (!IsGuestData(st)) continue;
      text += fmt::format(" | w{} bone {}", w, cod3::guest::ReadS32(base, v + kVehWheelBones + 4u * uint32_t(w)));
      for (uint32_t off = 128; off <= 160; off += 4)
        text += fmt::format(" {:.3g}", cod3::guest::ReadF32(base, st + off));
    }
    REXLOG_INFO("{}", text);
  }
}

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
    g_step_wheels.clear();  // [acct2-main]
  }
  const double frame_time = ctx.f1.f64;
  {
    // [acct2-main] Over a second without an update: a level was loaded (or the game paused). The
    // vehicles remembered from the last step may be gone, and their memory used for new ones that
    // haven't had a step yet, so they are forgotten until the next step.
    static std::chrono::steady_clock::time_point last_update;
    const auto now = std::chrono::steady_clock::now();
    if (now - last_update > std::chrono::seconds(1)) g_step_wheels.clear();
    last_update = now;
  }
  if (hz <= 0 || !(frame_time > 0.0)) {  // off, or a frame with no time: as the game does
    __imp__sub_824139B8(ctx, base);
    {  // [acct2-main] the diagnostic every 10 s in this mode too
      static double logged_time = 0.0;
      logged_time += frame_time > 0.0 ? frame_time : 0.0;
      if (logged_time >= 10.0) {
        logged_time = 0.0;
        LogVehicles(base);
      }
    }
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
    RememberStepWheels(base);  // [acct2-main]
  } else {
    // No step this frame: only the poses, with no time, as the update does them: the vehicles'
    // wheels ([acct2-main]), then the ragdolls (the update's own order).
    const bool ragdolls = CheckRagdollPoses(base) && REXCVAR_GET(cod3_physics_ragdoll_pose);
    const bool wheels = CheckStepWheels(base) && REXCVAR_GET(cod3_physics_vehicle_wheels);
    if (ragdolls || wheels) {
      ctx.r3.s64 = int32_t(kPhysScratch);
      ctx.r4.s64 = int32_t(kPhysScratchSize);
      sub_8217E9B8(ctx, base);
      cod3::guest::WriteU32(base, kPhysScratchInUse, kPhysScratch);
      if (wheels) PutBackWheelPoses(ctx, base);
      if (ragdolls) {
        ctx.f1.f64 = 0.0;
        sub_8240FE08(ctx, base);
      }
      cod3::guest::WriteU32(base, kPhysScratchInUse, 0);
      sub_8217EA38(ctx, base);
    }
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
    if (g_window_wheel_frames > 0)  // [acct2-main]
      REXLOG_INFO("cod3: physics: vehicle wheels: {} frames without a step had a physics vehicle (up to {}; "
                  "{} frames and {} steps in {:.1f} s); in {} of them the animation had moved its wheel bones "
                  "since its step ({} so far): {} (cod3_physics_vehicle_wheels)",
                  g_window_wheel_frames, g_window_vehicles, g_window_frames, g_window_steps, g_window_time,
                  g_window_wheel_moved, g_wheel_moved_total,
                  REXCVAR_GET(cod3_physics_vehicle_wheels) ? "the wheels' pose put back" : "left as they were");
    LogVehicles(base);  // [acct2-main] diagnostic
    g_window_time = 0.0;
    g_window_frames = g_window_steps = 0;
    g_window_ragdoll_frames = g_window_ragdoll_moved = 0;
    g_window_wheel_frames = g_window_wheel_moved = 0;  // [acct2-main]
    g_window_vehicles = 0;
  }
}
