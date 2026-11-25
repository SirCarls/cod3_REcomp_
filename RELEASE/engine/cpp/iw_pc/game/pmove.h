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
inline constexpr std::size_t kMoveLocalSize = 0x7C;  // all of pml_t (PmoveSingle zeroes 0x1F words)

namespace ps {
inline constexpr std::size_t kPmType = 0x04, kBobCycle = 0x08, kPmFlags = 0x0C, kPmTime = 0x10, kOrigin = 0x14, kVelocity = 0x20,
                             kOldVelocity = 0x2C, kGravity = 0x48, kLeanf = 0x4C, kSpeed = 0x50, kGroundEntityNum = 0x60,
                             kLadderVec = 0x64, kJumpTime = 0x70, kJumpOriginZ = 0x74, kLegsTimer = 0x78,
                             kTorsoTimer = 0x80, kDamageTimer = 0x90, kMovementDir = 0x9C, kEFlags = 0xA0, kEventSequence = 0xA4,
                             kEvents = 0xA8, kEventParms = 0xB8, kViewAngles = 0xE8,
                             kClientNum = 0xCC, kWeapon = 0xD4, kWeaponState = 0xD8, kFoliageSoundTime = 0x44, kViewHeightTarget = 0xF4, kViewHeightCurrent = 0xF8,
                             kViewHeightLerpTime = 0xFC, kDeltaAngles = 0x54, kDeltaYaw = 0x58, kMantleYaw = 0x5BC,
                             kDeadYaw = 0x130, kViewAngleClampBase = 0x10C, kViewAngleClampRange = 0x114,
                             kPronePitchBase = 0x58C, kOffhandIndex = 0xD0, kWeaponTime = 0x34, kWeaponDelay = 0x38,
                             kWeaponPosFrac = 0xDC, kAdsDelayTime = 0xE0, kDamageCount = 0x128, kWeapAnim = 0x5D0,
                             kGrenadeTimeLeft = 0x3C, kAmmo = 0x144, kAmmoClip = 0x344, kWeapons = 0x544,
                             kPrimaryWeapons = 0x555, kHoldBreathScale = 0x5B4, kHoldBreathTimer = 0x5B8,
                             kViewHeightLerpTarget = 0x100, kViewHeightLerpDown = 0x104, kProneCheckSize = 0x578,
                             kProneDirection = 0x584, kProneTorsoHeight = 0x5A8, kProneTorsoPitch = 0x5AC,
                             kProneWaistPitch = 0x5B0, kAimSpreadScale = 0x5D4;
/// The torso's animation and how long it lasts (torsoAnimDuration); the view height's position offset
/// (viewHeightLerpPosAdj); the standing box (mins, maxs); the prone body's ground pitch (proneDirectionPitch).
inline constexpr std::size_t kTorsoAnim = 0x84, kTorsoAnimDuration = 0x8C, kViewHeightLerpPosAdj = 0x108,
                             kMins = 0x56C, kMaxs = 0x578, kProneDirectionPitch = 0x588;
}
namespace pml {
inline constexpr std::size_t kForward = 0x00, kRight = 0x0C, kUp = 0x18, kFrameTime = 0x24, kMsec = 0x28, kWalking = 0x2C, kGroundPlane = 0x30,
                             kAlmostGroundPlane = 0x34, kGroundTrace = 0x38, kGroundNormal = 0x3C, kGroundSurfaceFlags = 0x48, kImpactSpeed = 0x5C,
                             kPreviousOrigin = 0x60, kPreviousVelocity = 0x6C;
}
namespace pm_type {
inline constexpr std::int32_t kNoclip = 2, kUfo = 3, kSpectator = 4, kIntermission = 5, kDead = 6;
}
namespace pm_flags {
inline constexpr std::int32_t kProne = 0x1, kLadder = 0x20, kSlowed = 0x100, kSlide = 0x200, kTimeKnockback = 0x400, kJumping = 0x80000;
/// Crouching (PMF_DUCKED); aiming down the sights (our name) and a bit that goes with it (its name not known).
inline constexpr std::int32_t kDucked = 0x2, kAds = 0x40, kAdsHold = 0x800;
/// Bits with which a player can't jump (0x4 and 0x1000; their names not known yet).
inline constexpr std::int32_t kNoJump = 0x1004;
/// A bit with which a player can't lean (our name).
inline constexpr std::int32_t kNoLean = 0x8000;
/// Climbing over an obstacle (Call of Duty 4's PMF_MANTLE); the view turned for a prone body (our name).
inline constexpr std::int32_t kMantle = 0x4, kProneViewTurned = 0x10000;
/// Fallen off a ladder: no ladder until walking again (our name).
inline constexpr std::int32_t kLadderFall = 0x40000;
/// The weapon in hand is the offhand's; looking through the binoculars (our names).
inline constexpr std::int32_t kOffhand = 0x10, kBinoculars = 0x20000;
/// Holding the breath (PMF_BREATH).
inline constexpr std::int32_t kHoldBreath = 0x4000;
}
/// Surfaces a fall on does no damage (SURF_NODAMAGE), without friction (SURF_SLICK).
inline constexpr std::int32_t kSurfNoDamage = 0x1, kSurfSlick = 0x2;
/// The entity numbers of "no entity" (ENTITYNUM_NONE) and of the world (ENTITYNUM_WORLD).
inline constexpr std::int32_t kEntityNone = 0x3FF, kEntityWorld = 0x3FE;
/// The view heights of crouching, lying down and standing (viewHeightTarget).
inline constexpr std::int32_t kCrouchViewHeight = 40, kProneViewHeight = 11, kStandViewHeight = 60;
/// The stances effective_stance gives (Call of Duty 4's PM_EFF_STANCE_*).
namespace stance {
inline constexpr std::int32_t kStand = 0, kProne = 1, kCrouch = 2;
}
/// Bytes of pmove_t the ports know so far; of a player command; the most touched entities (MAXTOUCH).
inline constexpr std::size_t kPmoveSize = 0xE8, kUsercmdSize = 28;
inline constexpr std::int32_t kMaxTouch = 32;
/// Offsets in pmove_t (the first word points at the player state).
namespace pm {
inline constexpr std::size_t kCmd = 0x04, kOldCmd = 0x20, kTraceMask = 0x3C, kNumTouch = 0x40, kTouchEnts = 0x44,
                             kMins = 0xC4, kMaxs = 0xD0, kXySpeed = 0xDC, kHandler = 0xE4;
/// Set when the move lay down or got up from lying down (proneChange).
inline constexpr std::size_t kProneChange = 0xE0;
}
/// Offsets in a player command (usercmd_t).
namespace cmd {
inline constexpr std::size_t kServerTime = 0x00, kButtons = 0x04, kAngles = 0x0C, kForwardMove = 0x18, kRightMove = 0x19,
                             kWeapon = 0x08;
}
/// Command buttons the movement tests: jump; one that moves at 0.4 of the speed on the ground (its
/// name not known yet).
namespace button {
inline constexpr std::int32_t kJump = 0x400, kSlow = 0x800;
/// Leaning left and right; in a fly move, down and up.
inline constexpr std::int32_t kLeanLeft = 0x40, kLeanRight = 0x80;
/// Lying down and crouching, held; one with which a refused stance change makes no event (our name).
inline constexpr std::int32_t kProne = 0x100, kCrouch = 0x200, kNoStanceEvent = 0x2000;
/// Aiming down the sights, held (our name); firing (BUTTON_ATTACK); holding the breath (BUTTON_HOLDBREATH).
inline constexpr std::int32_t kAds = 0x1000, kAttack = 0x1, kHoldBreath = 0x8000;
}
/// eFlags bits with which a prone player's movement direction doesn't follow his body (names not known yet).
inline constexpr std::int32_t kEFlagsProneFree = 0x300;
/// The same bits as the movement reads them elsewhere: on a mounted weapon (our name).
inline constexpr std::int32_t kEFlagsMounted = kEFlagsProneFree;
/// The eFlags bit of a weapon aimed down its sights (our name).
inline constexpr std::int32_t kEFlagsAds = 0x40000;
/// The event of the aim leaving the sights (our name).
inline constexpr std::int32_t kEvAdsExit = 0x95;

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
    /// Keeps trace as the ground's (groundTrace).
    void set_ground_trace(const trace::Trace& t) {
        std::memcpy(bytes.data() + pml::kGroundTrace, t.bytes.data(), t.bytes.size());
    }
};

/// A move (pmove_t): the player's state and the move's own fields as their bytes (the first word,
/// the player state's pointer, unused here).
struct Pmove : Bytes {
    PlayerState ps;
    Pmove() : Bytes(kPmoveSize) {}
    const std::uint8_t* cmd() const { return bytes.data() + pm::kCmd; }
    std::int32_t server_time() const { return integer(pm::kCmd + cmd::kServerTime); }
};

/// The climbing's (mantling's) dvars (see iw_pc::game::mantle).
struct MantleSettings {
    bool enable = true;          // mantle_enable
    float view_yaw_cap = 60.0f;  // mantle_view_yawcap
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
    float back_speed_scale = 0.7f;
    float strafe_speed_scale = 0.8f;
    float dmg_timer_max_time = 750.0f;
    float dmg_timer_min_scale = 0.0f;
    float jump_spread_add = 64.0f;
    float jump_ladder_push_vel = 128.0f;
    float view_pitch_up = 85.0f;     // player_view_pitch_up
    float view_pitch_down = 85.0f;   // player_view_pitch_down
    float ladder_yaw_cap = 100.0f;   // bg_ladder_yawcap
    float prone_yaw_cap = 85.0f;     // bg_prone_yawcap
    MantleSettings mantle;
    float fall_damage_min_height = 256.0f;  // bg_fallDamageMinHeight
    float fall_damage_max_height = 480.0f;  // bg_fallDamageMaxHeight (at least 1)
    bool scope_exit_on_damage = false;             // player_scopeExitOnDamage
    std::int32_t ads_exit_delay = 0;               // player_adsExitDelay (0 to 1000)
    float aim_spread_move_speed_threshold = 11.0f; // bg_aimSpreadMoveSpeedThreshold (0 to 300)
    float breath_hold_time = 4.5f;   // player_breath_hold_time
    float breath_gasp_time = 1.0f;   // player_breath_gasp_time
    float breath_gasp_scale = 4.5f;  // player_breath_gasp_scale
    float breath_hold_lerp = 4.0f;   // player_breath_hold_lerp
    float breath_gasp_lerp = 6.0f;   // player_breath_gasp_lerp
    float foliage_min_speed = 40.0f;        // bg_foliagesnd_minspeed
    float foliage_max_speed = 180.0f;       // bg_foliagesnd_maxspeed
    std::int32_t foliage_slow_interval = 1500;  // bg_foliagesnd_slowinterval (the defaults: Call of Duty 4's)
    std::int32_t foliage_fast_interval = 500;   // bg_foliagesnd_fastinterval
    std::int32_t foliage_reset_interval = 500;  // bg_foliagesnd_resetinterval
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

/// The stance the movement goes by: crouching (view height target 40), lying down (11), standing.
/// Original: PM_GetEffectiveStance at 0x00515470 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in eax)
std::int32_t effective_stance(const PlayerState& ps);

/// How far the view height's change has gone, 0 to 1 (see the Rust version).
/// Original: PM_GetViewHeightLerp at 0x005173F0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; pm in edi, from in edx, to in ecx; the result on the x87 stack)
x87::F24 view_height_lerp(const Pmove& pm, std::int32_t from, std::int32_t to);

/// The ground speed's slowdown from damage (see the Rust version).
/// Original: PM_DamageScale_Walk at 0x00515BE0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; the timer on the stack, the result on the x87 stack)
x87::F24 damage_scale_walk(std::int32_t damage_timer, const MoveSettings& settings);

/// How much of the speed a command's moves give on the ground (see the Rust version).
/// Original: PM_CmdScale_Walk at 0x00515990 (CoD2MP_s.exe 1.3; Call of Duty 4's name; pm in eax, cmd in ebx; the result on the x87 stack)
x87::F24 cmd_scale_walk(const Pmove& pm, const std::uint8_t* cmd, const MoveSettings& settings, const trace::MoveHost& host);

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

/// An event the player's own game predicts with its parameter (0 adds none; both kept as bytes).
/// Original: BG_AddPredictableEventToPlayerstate at 0x00514320 (CoD2MP_s.exe 1.3; Quake III's name; ps in eax, the event in ecx, the parameter on the stack)
void add_predictable_event(std::int32_t event, std::int32_t parm, PlayerState& ps);

/// The contents of foliage (CONTENTS_FOLIAGE); the event of moving through it (EV_FOLIAGE_SOUND).
inline constexpr std::int32_t kContentsFoliage = 0x2, kEvFoliageSound = 0x8B;

/// The sound of moving through foliage (see the Rust version).
/// Original: PM_FoliageSounds at 0x00518E10 (CoD2MP_s.exe 1.3; Call of Duty 4's name; pm in eax)
void foliage_sounds(Pmove& pm, const MoveSettings& settings, trace::MoveHost& host);

/// pm_flags::kSlowed for this move: holding the breath aiming down the sights (see the Rust version).
/// Original: PM_UpdateAdsSlowdown at 0x00519F90 (CoD2MP_s.exe 1.3; our name; pm in ecx)
void update_ads_slowdown(Pmove& pm);

/// The aim leaves the sights: kEvAdsExit and pm_flags::kAds cleared.
/// Original: PM_ExitAimDownSight at 0x004F27E0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in eax)
void exit_aim_down_sight(PlayerState& ps);

/// The footstep event for the ground the move stands on (see the Rust version).
/// Original: PM_FootstepType at 0x00516960 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in edx, pml in eax)
std::int32_t footstep_type(const PlayerState& ps, const MoveLocal& pml);

/// A footstep when the bob cycle passes half way (see the Rust version).
/// Original: PM_FootstepEvent at 0x00518380 (CoD2MP_s.exe 1.3; Call of Duty 4's name; old bob cycle in ecx, new in edx, footstep in eax, then (pm, pml) on the stack)
void footstep_event(Pmove& pm, const MoveLocal& pml, std::int32_t old_bob, std::int32_t new_bob, bool footstep,
                    trace::MoveHost& host);

}  // namespace iw_pc::game::pmove
