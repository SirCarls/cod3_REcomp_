// Player movement (bg_pmove.c), Call of Duty 2 (see the Rust version, iw_pc::game::pmove):
// the player state and the move's scratch state as their bytes, the settings as values.
#pragma once

#include <array>
#include <cstdint>
#include <cstring>
#include <vector>

#include "iw_pc/game/trace.h"
#include "iw_pc/x87.h"

namespace iw_pc::game::pmove {

inline constexpr std::size_t kPlayerStateSize = 0x26A8;
inline constexpr std::size_t kMoveLocalSize = 0x6C;

namespace ps {
inline constexpr std::size_t kPmType = 0x04, kBobCycle = 0x08, kPmFlags = 0x0C, kPmTime = 0x10, kOrigin = 0x14, kVelocity = 0x20,
                             kOldVelocity = 0x2C, kGravity = 0x48, kLeanf = 0x4C, kSpeed = 0x50, kGroundEntityNum = 0x60,
                             kLadderVec = 0x64, kJumpTime = 0x70, kJumpOriginZ = 0x74, kLegsTimer = 0x78,
                             kTorsoTimer = 0x80, kMovementDir = 0x9C, kEFlags = 0xA0, kEventSequence = 0xA4,
                             kEvents = 0xA8, kEventParms = 0xB8, kViewAngles = 0xE8,
                             kClientNum = 0xCC, kViewHeightTarget = 0xF4, kProneCheckSize = 0x578,
                             kProneDirection = 0x584, kProneTorsoHeight = 0x5A8, kProneTorsoPitch = 0x5AC,
                             kProneWaistPitch = 0x5B0;
}
namespace pml {
inline constexpr std::size_t kForward = 0x00, kRight = 0x0C, kFrameTime = 0x24, kMsec = 0x28, kWalking = 0x2C, kGroundPlane = 0x30,
                             kGroundTrace = 0x38, kGroundNormal = 0x3C, kGroundSurfaceFlags = 0x48, kImpactSpeed = 0x5C,
                             kPreviousOrigin = 0x60;
}
namespace pm_type {
inline constexpr std::int32_t kNoclip = 2, kUfo = 3, kSpectator = 4, kDead = 6;
}
namespace pm_flags {
inline constexpr std::int32_t kProne = 0x1, kLadder = 0x20, kSlowed = 0x100, kSlide = 0x200, kTimeKnockback = 0x400, kJumping = 0x80000;
}
inline constexpr std::int32_t kSurfSlick = 0x2;
/// The entity numbers of "no entity" (ENTITYNUM_NONE) and of the world (ENTITYNUM_WORLD).
inline constexpr std::int32_t kEntityNone = 0x3FF, kEntityWorld = 0x3FE;
/// The view heights of crouching and lying down (viewHeightTarget).
inline constexpr std::int32_t kCrouchViewHeight = 40, kProneViewHeight = 11;
/// Bytes of pmove_t the ports know so far; of a player command; the most touched entities (MAXTOUCH).
inline constexpr std::size_t kPmoveSize = 0xE8, kUsercmdSize = 28;
inline constexpr std::int32_t kMaxTouch = 32;
/// Offsets in pmove_t (the first word points at the player state).
namespace pm {
inline constexpr std::size_t kCmd = 0x04, kOldCmd = 0x20, kTraceMask = 0x3C, kNumTouch = 0x40, kTouchEnts = 0x44,
                             kMins = 0xC4, kMaxs = 0xD0, kXySpeed = 0xDC, kHandler = 0xE4;
}
/// eFlags bits with which a prone player's movement direction doesn't follow his body (names not known yet).
inline constexpr std::int32_t kEFlagsProneFree = 0x300;

/// A structure as its bytes, with little-endian word access.
struct Bytes {
    std::vector<std::uint8_t> bytes;
    explicit Bytes(std::size_t size) : bytes(size, 0) {}
    std::int32_t integer(std::size_t at) const {
        std::int32_t v;
        std::memcpy(&v, bytes.data() + at, 4);
        return v;
    }
    void set_integer(std::size_t at, std::int32_t v) { std::memcpy(bytes.data() + at, &v, 4); }
    float real(std::size_t at) const {
        float v;
        std::memcpy(&v, bytes.data() + at, 4);
        return v;
    }
    void set_real(std::size_t at, float v) { std::memcpy(bytes.data() + at, &v, 4); }
    std::array<float, 3> vec3(std::size_t at) const { return {real(at), real(at + 4), real(at + 8)}; }
    void set_vec3(std::size_t at, const std::array<float, 3>& v) {
        for (std::size_t i = 0; i < 3; ++i) set_real(at + 4 * i, v[i]);
    }
};

struct PlayerState : Bytes {
    PlayerState() : Bytes(kPlayerStateSize) {}
    std::int32_t pm_type() const { return integer(ps::kPmType); }
    std::int32_t pm_flags() const { return integer(ps::kPmFlags); }
    std::array<float, 3> velocity() const { return {real(ps::kVelocity), real(ps::kVelocity + 4), real(ps::kVelocity + 8)}; }
    void set_velocity(const std::array<float, 3>& v) {
        for (std::size_t i = 0; i < 3; ++i) set_real(ps::kVelocity + 4 * i, v[i]);
    }
};

struct MoveLocal : Bytes {
    MoveLocal() : Bytes(kMoveLocalSize) {}
    float frame_time() const { return real(pml::kFrameTime); }
    bool walking() const { return integer(pml::kWalking) != 0; }
};

/// A move (pmove_t): the player's state and the move's own fields as their bytes (the first word,
/// the player state's pointer, unused here).
struct Pmove : Bytes {
    PlayerState ps;
    Pmove() : Bytes(kPmoveSize) {}
    const std::uint8_t* cmd() const { return bytes.data() + pm::kCmd; }
};

/// The movement's dvars.
struct MoveSettings {
    float stop_speed = 100.0f;
    float friction = 5.5f;
    bool jump_slowdown_enable = true;
    float inertia_max = 50.0f;
    float inertia_angle = 0.0f;
    bool inertia_debug = false;
    float spectate_speed_scale = 1.0f;
    float jump_height = 39.0f;
    float jump_step_size = 18.0f;
    float footsteps_threshold = 0.0f;
};

/// Original: Jump_ReduceFriction at 0x0052FD30 (CoD2MP_s.exe 1.3; ps in eax, the result on the x87 stack)
x87::F24 jump_reduce_friction(PlayerState& ps, const MoveSettings& settings);

/// Original: PM_Friction at 0x00515490 (CoD2MP_s.exe 1.3; ps in esi, pml in edi)
void friction(PlayerState& ps, const MoveLocal& pml, const MoveSettings& settings);

/// Original: PM_InertiaTurnTooSharp at 0x005155A0 (CoD2MP_s.exe 1.3; our name; ps in eax, dir in ecx, amount on the stack)
bool inertia_turn_too_sharp(const PlayerState& ps, const std::array<float, 3>& dir, float amount, const MoveSettings& settings);

/// Original: PM_ClampInertia at 0x00515680 (CoD2MP_s.exe 1.3; our name; ps in edx, then (amount, dir) on the stack)
float clamp_inertia(const PlayerState& ps, float amount, const std::array<float, 3>& dir, const MoveSettings& settings);

/// Original: PM_Accelerate at 0x005156E0 (CoD2MP_s.exe 1.3; wishdir in eax, ps in edi, pml in ebx, then (wishspeed, accel) on the stack)
void accelerate(PlayerState& ps, const MoveLocal& pml, const std::array<float, 3>& wish_dir, float wish_speed, float accel,
                const MoveSettings& settings);

/// How much of the speed a move of (forward, right, up) gives (see the Rust version).
/// Original: PM_MoveScale at 0x00515810 (CoD2MP_s.exe 1.3; our name, Call of Duty 4's; ps in ecx, then (forward, right, up) on the stack)
x87::F24 move_scale(const PlayerState& ps, float forward, float right, float up, const MoveSettings& settings);

/// The same for a command's forward and right bytes.
/// Original: PM_CmdScale at 0x005158E0 (CoD2MP_s.exe 1.3; cmd in esi, ps in edi)
x87::F24 cmd_scale(const PlayerState& ps, const std::uint8_t* cmd, const MoveSettings& settings);

/// The move's timers run down by the command's milliseconds (see the Rust version).
/// Original: PM_DropTimers at 0x00518F90 (CoD2MP_s.exe 1.3; ps in eax, pml in esi)
void drop_timers(PlayerState& ps, const MoveLocal& pml);

/// movementDir (-90 to 90 whole degrees) from the prone direction, the ladder or the last move
/// (see the Rust version).
/// Original: PM_SetMovementDir at 0x00515C30 (CoD2MP_s.exe 1.3; Quake III's name; pm in eax, pml in ecx)
void set_movement_dir(Pmove& pm, const MoveLocal& pml);

/// v clipped against a plane: v + n * -(b - |b| * 0.001), b = v . n.
/// Original: PM_ClipVelocity at 0x00515430 (CoD2MP_s.exe 1.3; Quake III's name; in in ecx, normal in eax, out in edx)
std::array<float, 3> clip_velocity(const std::array<float, 3>& v, const std::array<float, 3>& normal);

/// Records a touched entity (not the world, not twice, at most kMaxTouch).
/// Original: PM_AddTouchEnt at 0x005153F0 (CoD2MP_s.exe 1.3; Quake III's name; pm in eax, the number in edi)
void add_touch_ent(Pmove& pm, std::int32_t number);

/// The step a jumping player may still climb (see the Rust version).
/// Original: Jump_GetStepHeight at 0x0052FBD0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in edx, origin in ecx, the step out in edi; al)
bool jump_get_step_height(const PlayerState& ps, const std::array<float, 3>& origin, float& step, const MoveSettings& settings);

/// A jump's upward speed kept to what reaches the jump's height (see the Rust version).
/// Original: Jump_ClampVelocity at 0x0052FD90 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in ecx, origin in eax)
void jump_clamp_velocity(PlayerState& ps, const std::array<float, 3>& origin, const MoveSettings& settings);

/// Whether the move makes footstep sounds (see the Rust version).
/// Original: PM_ShouldMakeFootsteps at 0x005184E0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; pm in esi; eax)
bool should_make_footsteps(const Pmove& pm, const MoveSettings& settings);

/// A trace for the moving player; started inside another body, that body counts as touched, the
/// move stops colliding with bodies and the trace is made again without them (see the Rust version).
/// Original: PM_playerTrace at 0x00515310 (CoD2MP_s.exe 1.3; Call of Duty 4's name; pm in esi, results in ebx, then (start, mins, maxs, end, passEntityNum, contentMask) on the stack)
trace::Trace player_trace(Pmove& pm, const std::array<float, 3>& start, const std::array<float, 3>& mins,
                          const std::array<float, 3>& maxs, const std::array<float, 3>& end, std::int32_t pass_entity,
                          std::int32_t content_mask, trace::MoveHost& host);

/// Events kept in a player state (MAX_PS_EVENTS); surfaces without footsteps (SURF_NOSTEPS); the first
/// footstep events (running, walking, prone; each + the surface type); a ladder's own surface type.
inline constexpr std::int32_t kMaxPsEvents = 4, kSurfNoSteps = 0x2000, kEvFootstepRun = 1, kEvFootstepWalk = 0x18,
                              kEvFootstepProne = 0x2F, kLadderSurfaceType = 0x15;

/// An event the player's own game predicts (0 adds none), with parameter 0.
/// Original: PM_AddEvent at 0x005153B0 (CoD2MP_s.exe 1.3; Quake III's name; ps in eax, the event in ecx)
void add_event(PlayerState& ps, std::int32_t event);

/// The footstep event for the ground the move stands on (see the Rust version).
/// Original: PM_FootstepType at 0x00516960 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in edx, pml in eax)
std::int32_t footstep_type(const PlayerState& ps, const MoveLocal& pml);

/// A footstep when the bob cycle passes half way (see the Rust version).
/// Original: PM_FootstepEvent at 0x00518380 (CoD2MP_s.exe 1.3; Call of Duty 4's name; old bob cycle in ecx, new in edx, footstep in eax, then (pm, pml) on the stack)
void footstep_event(Pmove& pm, const MoveLocal& pml, std::int32_t old_bob, std::int32_t new_bob, bool footstep,
                    trace::MoveHost& host);

}  // namespace iw_pc::game::pmove
