// Stances (bg_pmove.c's duck check): standing, crouching and lying down, the box and the view height
// each gives, and the view height's way from one to the next. Call of Duty 2's versions (see the
// Rust version, iw_pc::game::duck).
#pragma once

#include <cstdint>
#include <span>
#include <utility>

#include "iw_pc/game/pmove.h"
#include "iw_pc/game/trace.h"
#include "iw_pc/x87.h"

namespace iw_pc::game::duck {

/// The view height of the dead (DEAD_VIEWHEIGHT).
inline constexpr std::int32_t kDeadViewHeight = 8;
/// A refused stance change's events (Call of Duty 4's EV_STANCE_FORCE_*; Call of Duty 2's numbers).
inline constexpr std::int32_t kEvStanceForceStand = 0x8C, kEvStanceForceCrouch = 0x8D, kEvStanceForceProne = 0x8E;
/// eFlags bits of the stance (EF_CROUCHING, our EF_PRONE).
inline constexpr std::int32_t kEFlagsCrouching = 0x4, kEFlagsProne = 0x8;
/// The box's height crouching and lying down.
inline constexpr float kCrouchMaxsZ = 50.0f, kProneMaxsZ = 30.0f;
/// The bit an animation number flips when the same one starts again (ANIM_TOGGLEBIT).
inline constexpr std::int32_t kAnimToggleBit = 0x200;

/// A point of a view height's way (Call of Duty 4's viewLerpWaypoint_s).
struct ViewLerpWaypoint {
    std::int32_t fraction;
    float view_height;
    std::int32_t offset;
};

/// The game's ways (0x005DB630, 0x005DB6A0, 0x005DB710, 0x005DB7B8), each ending in fraction -1.
extern const ViewLerpWaypoint kStandToCrouch[9], kCrouchToStand[9], kCrouchToProne[11], kProneToCrouch[8];

/// How long the view height takes to reach `target`: 400 to lying down, 400 up to crouching (200
/// down), else 200.
/// Original: PM_GetViewHeightLerpTime at 0x00517330 (CoD2MP_s.exe 1.3; Call of Duty 4's name; the target in eax, down on the stack)
std::int32_t view_height_lerp_time(std::int32_t target, std::int32_t down);

/// The view height `fraction` percent along a way, and its position offset (see the Rust version).
/// Original: PM_ViewHeightTableLerp at 0x00517360 (CoD2MP_s.exe 1.3; Call of Duty 4's name; fraction in eax, table in edi, the offset out in ebx; the height on the x87 stack)
std::pair<x87::F24, float> view_height_table_lerp(std::int32_t fraction, std::span<const ViewLerpWaypoint> table);

/// The view height on its way to viewHeightTarget (see the Rust version).
/// Original: PM_ViewHeightAdjust at 0x005174D0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; pm, pml on the stack)
void view_height_adjust(pmove::Pmove& pm, pmove::MoveLocal& pml, const pmove::MoveSettings& settings,
                        trace::MoveHost& host);

/// The stance and the box the move has (see the Rust version).
/// Original: PM_CheckDuck at 0x005179B0 (CoD2MP_s.exe 1.3; Quake III's name; pm in edi, pml on the stack)
void check_duck(pmove::Pmove& pm, pmove::MoveLocal& pml, const pmove::MoveSettings& settings, trace::MoveHost& host);

}  // namespace iw_pc::game::duck
