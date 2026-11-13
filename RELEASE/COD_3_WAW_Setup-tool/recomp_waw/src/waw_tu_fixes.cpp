// waw - World at War's title update 7 fixes, redone as our own hooks on the disc program. [waw-decomp]
//
// Owner, 2026-11-09 17:28: "Basically we need to replicate the patches without actually needing them. Also without
// breaking all our progress." So the port keeps running the disc's own code (every other hook address stays valid),
// and each fix the update made is written here again, in our own words, behind a setting of its own. No title update
// file is needed, loaded or shipped. How each fix was found (the update studied on the PC, never in the cloud) is in
// dev/agents/waw-decomp/TITLE-UPDATES.md.
//
// Each function here is named by a [[midasm_hook]] block in config/default.toml, which says where the codegen calls
// it. Addresses are the disc program's (default.xex: campaign, co-op and zombies).

#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "generated/default/waw_pch.h"

REXCVAR_DEFINE_BOOL(waw_fix_rifle_grenade_switch, true, "waw",
    "Title update 7's rifle grenade fix, redone: when a weapon change ends on a grenade-type weapon (the rifle "
    "grenade), the game doesn't start a second change back to the other weapon");
REXCVAR_DEFINE_BOOL(waw_fix_vehicle_push_while_mantling, true, "waw",
    "Title update 7's mantling fix, redone: a vehicle doesn't push a player who is climbing over something "
    "(the update's vehicle_push_during_mantle, on by default there)");
REXCVAR_DEFINE_INT32(waw_fix_standing_but_looks_crouched, 2, "waw",
    "Title update 7's fix for a player who is standing but whose legs still look crouched (or prone), redone: "
    "while the player is in the air, legs whose animation doesn't fit the stance are set again. 0 off, 1 the whole "
    "time in the air, 2 only just before landing (the update's bg_bugfixStandingButLooksCrouched, 2 by default there)");

REXCVAR_DEFINE_BOOL(waw_fix_dpad_item_firing, true, "waw",
    "Title update 7's fix for its bug 42531, redone: holding fire doesn't fire a weapon the game treats as a d-pad item "
    "(the update's g_testForDpadItem, on by default there)");

// The game's own functions the fixes call (the disc program's addresses, the decomp's names).
DECLARE_REX_FUNC(sub_820E19F8);  // PM_GetEffectiveStance(ps): 0 standing, 1 prone, 2 crouched
DECLARE_REX_FUNC(sub_820E3EF0);  // PM_Footsteps_NotMoving(pm, stance): the idle legs for the stance
DECLARE_REX_FUNC(sub_820D89C8);  // BG_AnimScriptAnimation(ps, aiState, movetype, force): < 0 when nothing played
DECLARE_REX_FUNC(sub_820D6358);  // BG_SetConditionValue(clientNum, condition, value)

namespace {

// Guest memory, big-endian, at `base + address` (as the translated code reads it).
uint32_t TuReadGuestU32(const uint8_t* base, uint32_t address) {
  uint32_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap32(value);
}

// WeaponDef (the Xbox 360 disc build's layout, read from the code below): +0x144 is weapType, and
// WEAPTYPE_GRENADE is 1 (the update compares with 1 at the same place).
constexpr uint32_t kWeaponDefType = 0x144;
constexpr uint32_t kWeaponTypeGrenade = 1;

// gentity_s +0x180 is the entity's client (0 for non-players); the client starts with its playerState_s, whose
// pm_flags (+0xC) has PMF_MANTLE = 0x4 while the player is mantling (the update tests the same fields).
constexpr uint32_t kEntityClient = 0x180;
constexpr uint32_t kClientPmFlags = 0xC;
constexpr uint32_t kPmfMantle = 0x4;

float TuReadGuestFloat(const uint8_t* base, uint32_t address) {
  const uint32_t bits = TuReadGuestU32(base, address);
  float value;
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}

// Calls one of the game's translated functions from inside a hook, with up to four integer arguments, and returns its
// r3. The callee gets a frame below the hooked function's own, and every register is put back afterwards (as
// QueueConsoleCommand in waw_hooks.cpp does it); what the callee writes to memory stays.
template <typename Function>
uint32_t TuCallGuest(PPCContext& ctx, uint8_t* base, Function function, uint32_t r3, uint32_t r4 = 0,
                     uint32_t r5 = 0, uint32_t r6 = 0) {
  constexpr uint32_t kRoom = 0x100;  // stack left free between the hooked function's frame and the callee's
  const PPCContext saved = ctx;
  ctx.r1.u64 = (ctx.r1.u32 - kRoom) & ~0xFu;
  ctx.r3.u64 = r3;
  ctx.r4.u64 = r4;
  ctx.r5.u64 = r5;
  ctx.r6.u64 = r6;
  function(ctx, base);
  const uint32_t result = ctx.r3.u32;
  ctx = saved;
  return result;
}

// --- D-pad items ------------------------------------------------------------------------------------------------------
// The weapon definitions, one pointer per weapon index (0x824CE208, as PM_Weapon_ShouldBeFiring 0x820EDB80 reads them),
// the player's current weapon index (ps +0x104), and the definition's d-pad item word (+0x5F0): the weapon-change code
// (PM_BeginWeaponChange 0x820EC808, PM_Weapon_CheckForChangeWeapon 0x820F0190, PM_Weapon_FinishWeaponChange
// 0x820F0D18) treats weapons with it set apart, and the update's fix tests the same word.
constexpr uint32_t kWeaponDefs = 0x824CE208;
constexpr uint32_t kPlayerWeapon = 0x104;
constexpr uint32_t kWeaponDefDpadItem = 0x5F0;

// --- Standing but looks crouched -------------------------------------------------------------------------------------
// pmove_t, playerState_s and pml_t fields (the disc build's layout, as PM_Footsteps 0x820E8408 and the update's fix
// read them; pml_t is Call of Duty 4's, whose msec at +0x28 is what PM_Footsteps uses too). playerState_s's pm_flags
// is kClientPmFlags above.
constexpr uint32_t kPmovePlayerState = 0x0;        // pm->ps
constexpr uint32_t kPmoveForwardMove = 0x1A;       // pm->cmd.forwardmove, a signed byte
constexpr uint32_t kPmoveRightMove = 0x1B;         // pm->cmd.rightmove, a signed byte
constexpr uint32_t kPmoveHorizontalSpeed = 0x114;  // pm->xyspeed, a float PM_Footsteps sets first
constexpr uint32_t kPmlWalking = 0x2C;             // pml->walking
constexpr uint32_t kPmlAlmostGroundPlane = 0x34;   // pml->almostGroundPlane: ground just below the player
constexpr uint32_t kPlayerPmType = 0x4;            // ps->pm_type
constexpr uint32_t kPlayerClientNum = 0xF8;        // ps->clientNum
constexpr uint32_t kPmTypeNormal = 0;
constexpr uint32_t kPmfLadder = 0x8;
constexpr uint32_t kPmfBackwardsRun = 0x20;

// PM_GetEffectiveStance's answers; PM_Footsteps adds 3 while the player runs backwards, which picks the second half of
// the movetype table below. Together that is the "stance index", 0 to 5.
constexpr uint32_t kStanceProne = 1;
constexpr uint32_t kStanceCrouched = 2;
constexpr uint32_t kStanceBackwards = 3;

// Each client's animation state, bgs->clientinfo[]: bgs is the running thread's own (the second word of the table r13
// points to), the array starts 0x87A28 into it, one clientInfo_t of 0x58C bytes per client (PM_Footsteps_NotMoving
// 0x820E3EF0 finds it the same way, for client numbers under 4 only). The animation conditions start at +0x478, two
// words each (BG_SetConditionValue 0x820D6358). Condition 3, the movetype, is a bit mask holding the bit of the
// movement animation last played (BG_AnimScriptAnimation 0x820D89C8 sets it); condition 8 is the strafing direction.
constexpr uint32_t kThreadSlotBgs = 0x4;
constexpr uint32_t kBgsClientInfo = 0x87A28;
constexpr uint32_t kClientInfoSize = 0x58C;
constexpr uint32_t kClientInfoConditions = 0x478;
constexpr uint32_t kConditionSize = 8;
constexpr uint32_t kConditionMovetype = 3;
constexpr uint32_t kConditionStrafing = 8;
constexpr uint32_t kAnimatedClients = 4;

// Movetypes (the animation scripts' movement types, by the game's numbers): the idle of each stance, and the table
// PM_Footsteps picks the moving ones from, 0x82008CB8: per stance index two entries, running then walking, each the
// movetype and its stumbling variant (two words).
constexpr uint32_t kMovetypeIdle = 1;
constexpr uint32_t kMovetypeIdleCrouched = 2;
constexpr uint32_t kMovetypeIdleProne = 3;
constexpr uint32_t kMovingMovetypes = 0x82008CB8;
constexpr uint32_t kMovingEntrySize = 8;
constexpr uint32_t kMovingEntriesPerStance = 2;
constexpr uint32_t kMovingWordsPerStance = 4;
constexpr uint32_t kPaceWalking = 1;

// Two of the game's settings PM_Footsteps reads, by the pointers their registration stores (0x820DE034, 0x820DE118);
// a dvar_t's value is at +0xC in this build. player_moveThreshhold: the speed under which the legs idle.
// player_strafeAnimCosAngle: how far from straight ahead or behind the stick may point before the legs strafe.
constexpr uint32_t kDvarMoveThreshold = 0x824CBDEC;
constexpr uint32_t kDvarStrafeCosAngle = 0x824CBDE8;
constexpr uint32_t kDvarValue = 0xC;

// Set while this fix has PM_Footsteps_NotMoving pick the idle legs. The update gives that function a third argument,
// 0 from its fix, so it skips the flinch (the short animation after being hit, player_dmgtimer_flinchTime) and plays
// the idle; waw_fix_stance_idle_skip_flinch_hook does the same in the disc's function.
thread_local bool t_stance_fix_idle = false;

struct StanceFixIdleScope {
  StanceFixIdleScope() { t_stance_fix_idle = true; }
  ~StanceFixIdleScope() { t_stance_fix_idle = false; }
};

// The movetype the client's legs last played: the lowest bit set in its movetype mask (as the update's fix reads it),
// or 0 when none is.
uint32_t CurrentMovetype(const uint8_t* base, uint32_t client_info) {
  const uint32_t mask = client_info + kClientInfoConditions + kConditionMovetype * kConditionSize;
  for (uint32_t bit = 0; bit < 64; ++bit) {
    if (TuReadGuestU32(base, mask + (bit / 32) * 4) & (1u << (bit % 32))) return bit;
  }
  return 0;
}

// Whether that movetype fits the stance index: the stance's idle, or one of its four moving movetypes (running,
// walking and their stumbling variants).
bool MovetypeFitsStance(const uint8_t* base, uint32_t movetype, uint32_t stance_index) {
  uint32_t idle = kMovetypeIdle;
  if (stance_index == kStanceProne || stance_index == kStanceProne + kStanceBackwards) idle = kMovetypeIdleProne;
  if (stance_index == kStanceCrouched || stance_index == kStanceCrouched + kStanceBackwards) {
    idle = kMovetypeIdleCrouched;
  }
  if (movetype == idle) return true;
  const uint32_t words = kMovingMovetypes + stance_index * kMovingEntriesPerStance * kMovingEntrySize;
  for (uint32_t word = 0; word < kMovingWordsPerStance; ++word) {
    if (TuReadGuestU32(base, words + word * 4) == movetype) return true;
  }
  return false;
}

// The strafing direction PM_Footsteps sets before a moving animation: 0 while the stick points mostly ahead or behind
// (within player_strafeAnimCosAngle of it), otherwise 2 to the right and 1 to the left.
uint32_t StrafeDirection(const uint8_t* base, uint32_t pm) {
  const float forward = static_cast<int8_t>(base[pm + kPmoveForwardMove]);
  const float right = static_cast<int8_t>(base[pm + kPmoveRightMove]);
  const float length = std::sqrt(forward * forward + right * right);
  const float ahead = length != 0.0f ? forward / length : forward;
  const float cos_angle = TuReadGuestFloat(base, TuReadGuestU32(base, kDvarStrafeCosAngle) + kDvarValue);
  if (ahead > cos_angle || ahead < -cos_angle) return 0;
  return right > 0.0f ? 2 : 1;
}

// The update's fix, when the player is moving: the stance index's walking legs, picked as PM_Footsteps picks them (the
// strafing direction first, then the movetype; the plain idle when the scripts have no such animation). The update's
// last-stand crawl doesn't come in: PM_Footsteps takes this path only while that state (ps +0x944) is 0.
void PlayWalkingLegs(PPCContext& ctx, uint8_t* base, uint32_t pm, uint32_t ps, uint32_t stance_index) {
  TuCallGuest(ctx, base, sub_820D6358, TuReadGuestU32(base, ps + kPlayerClientNum), kConditionStrafing,
              StrafeDirection(base, pm));
  const uint32_t entry = stance_index * kMovingEntriesPerStance + kPaceWalking;
  const uint32_t movetype = TuReadGuestU32(base, kMovingMovetypes + entry * kMovingEntrySize);
  const int32_t played = static_cast<int32_t>(TuCallGuest(ctx, base, sub_820D89C8, ps, 0, movetype, 0));
  if (played < 0) TuCallGuest(ctx, base, sub_820D89C8, ps, 0, kMovetypeIdle, 0);
}

// The first few times the fix acts go into the log, then every hundredth, so a test can see it work.
void LogStanceFix(uint32_t movetype, uint32_t stance_index) {
  static std::atomic<uint32_t> count{0};
  const uint32_t n = count.fetch_add(1, std::memory_order_relaxed) + 1;
  if (n <= 8 || n % 100 == 0) {
    REXLOG_INFO("waw: TU7 stance fix #{}: legs movetype {} didn't fit stance index {}; set again", n, movetype,
                stance_index);
  }
}

}  // namespace

// PM_Weapon_FinishWeaponChange 0x820F0D18, at 0x820F0E4C (`cmplwi cr6, r11, 0`). r27 is the new weapon's
// WeaponDef and r11 says whether the follow-up block runs: when the weapon taken out is one whose def allows it
// and the old weapon is an alternate-mode one (+0x154 == 3), that block calls PM_BeginWeaponChange again, toward
// the old weapon's alternate (+0x63C). Switching onto a rifle grenade could set that off and bounce the player back
// off it. The update clears r11 here when the new weapon is a grenade type and its setting (rifleGrenadeSwitchFix,
// on by default) is on; this does the same with our own setting.
void waw_fix_rifle_grenade_switch_hook(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(waw_fix_rifle_grenade_switch)) return;
  const uint32_t weapon_def = ctx.r27.u32;
  if (weapon_def == 0) return;
  if (TuReadGuestU32(base, weapon_def + kWeaponDefType) == kWeaponTypeGrenade) ctx.r11.u64 = 0;
}

// The vehicle push 0x821F1F68 (registered next to the mantle settings; r4 is the entity being pushed), at its first
// instruction, before anything else runs. The update returns from it at once when its setting is on and the pushed
// entity is a player who is mantling: a vehicle shoving a player mid-climb could put them inside the geometry. This
// returns true for the same case, so the codegen leaves the function with every register as it came in (the
// update's early exit returns the same way).
bool waw_fix_vehicle_push_while_mantling_hook(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(waw_fix_vehicle_push_while_mantling)) return false;
  const uint32_t entity = ctx.r4.u32;
  if (entity == 0) return false;
  const uint32_t client = TuReadGuestU32(base, entity + kEntityClient);
  if (client == 0) return false;
  return (TuReadGuestU32(base, client + kClientPmFlags) & kPmfMantle) != 0;
}

// PM_Footsteps 0x820E8408, at 0x820E84E8 (r27 = pm, r25 = pml): the player is in the air (no ground entity, pm_type
// not 1, ps +0x944 zero) and PM_Footstep_LadderMove 0x820E8168 has just run; PM_Footsteps returns next. Legs keep the
// movement animation they last played until the game plays another, and in the air it plays none, so a player who
// stood up (or crouched) since could land with legs of the other stance: "standing but looks crouched". The update made
// the ladder function say whether the player was on a ladder and, when not, calls a new function here (0x820F48E0 in
// the update) that this redoes: with its setting on (2: only while the ground is just below and the player isn't
// walking), not mantling, in normal play, when the legs' movetype isn't the stance's idle or one of its moving ones, it
// plays the stance's idle (PM_Footsteps_NotMoving, under player_moveThreshhold) or its walking legs.
void waw_fix_standing_but_looks_crouched_hook(PPCContext& ctx, uint8_t* base) {
  const int32_t mode = REXCVAR_GET(waw_fix_standing_but_looks_crouched);
  if (mode <= 0) return;
  const uint32_t pm = ctx.r27.u32;
  const uint32_t pml = ctx.r25.u32;
  const uint32_t ps = TuReadGuestU32(base, pm + kPmovePlayerState);
  const uint32_t pm_flags = TuReadGuestU32(base, ps + kClientPmFlags);
  if (pm_flags & kPmfLadder) return;  // the ladder function played the climbing legs
  if (mode == 2) {
    if (TuReadGuestU32(base, pml + kPmlAlmostGroundPlane) == 0) return;
    if (TuReadGuestU32(base, pml + kPmlWalking) != 0) return;
  }
  if (pm_flags & kPmfMantle) return;
  if (TuReadGuestU32(base, ps + kPlayerPmType) != kPmTypeNormal) return;

  const uint32_t client_num = TuReadGuestU32(base, ps + kPlayerClientNum);
  const uint32_t bgs = TuReadGuestU32(base, TuReadGuestU32(base, ctx.r13.u32) + kThreadSlotBgs);
  if (bgs == 0 || client_num >= kAnimatedClients) return;
  const uint32_t client_info = bgs + kBgsClientInfo + client_num * kClientInfoSize;

  // The stance PM_Footsteps worked out at 0x820E84B0 (it was in r4, which the ladder call took for pml).
  const uint32_t stance = TuCallGuest(ctx, base, sub_820E19F8, ps);
  const uint32_t stance_index = stance + ((pm_flags & kPmfBackwardsRun) ? kStanceBackwards : 0);
  const uint32_t movetype = CurrentMovetype(base, client_info);
  if (MovetypeFitsStance(base, movetype, stance_index)) return;

  LogStanceFix(movetype, stance_index);
  const float speed = TuReadGuestFloat(base, pm + kPmoveHorizontalSpeed);
  const float idle_under = TuReadGuestFloat(base, TuReadGuestU32(base, kDvarMoveThreshold) + kDvarValue);
  if (speed < idle_under) {
    StanceFixIdleScope idle_scope;
    TuCallGuest(ctx, base, sub_820E3EF0, pm, stance);
  } else {
    PlayWalkingLegs(ctx, base, pm, ps, stance_index);
  }
}

// PM_Footsteps_NotMoving 0x820E3EF0, at 0x820E407C and 0x820E40E0: the second instruction of each of its two checks for
// a recent hit (player_dmgtimer_flinchTime), which would play the flinch (movetype 32 + direction). While the fix above
// asks for the idle legs, this returns true and the config goes on at the idle instead: 0x820E40B0 (the stance's idle
// movetype is in r5 already) and 0x820E4104 (the standing idle), where the update's third argument sends it.
bool waw_fix_stance_idle_skip_flinch_hook(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  (void)base;
  return t_stance_fix_idle;
}

// PM_Weapon_ShouldBeFiring 0x820EDB80, at 0x820EDC00 (r6 = ps, r5 = whether the player should be firing; every path
// through the function passes here, after its check for a player in the air). The update added one more test at this
// point, behind its g_testForDpadItem ("Turn off fix for devtrack entry 42531", on by default): when the current
// weapon's definition has the d-pad item word set, the answer is no. This clears r5 the same way.
void waw_fix_dpad_item_firing_hook(PPCContext& ctx, uint8_t* base) {
  if (!REXCVAR_GET(waw_fix_dpad_item_firing)) return;
  const uint32_t ps = ctx.r6.u32;
  const uint32_t weapon = TuReadGuestU32(base, ps + kPlayerWeapon);
  const uint32_t weapon_def = TuReadGuestU32(base, kWeaponDefs + weapon * 4);
  if (weapon_def != 0 && TuReadGuestU32(base, weapon_def + kWeaponDefDpadItem) != 0) ctx.r5.u64 = 0;
}
