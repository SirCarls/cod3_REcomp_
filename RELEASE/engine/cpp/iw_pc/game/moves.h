// The kinds of move (bg_pmove.c's PM_*Move), Call of Duty 2 (see the Rust version,
// iw_pc::game::moves).
#pragma once

#include "iw_pc/game/pmove.h"
#include "iw_pc/game/trace.h"

namespace iw_pc::game::moves {

/// The move in the air (see the Rust version).
/// Original: PM_AirMove at 0x00515FA0 (CoD2MP_s.exe 1.3; Quake III's name; pml in eax, pm on the stack)
void air_move(pmove::Pmove& pm, pmove::MoveLocal& pml, const pmove::MoveSettings& settings, trace::MoveHost& host);

/// The move on the ground (see the Rust version).
/// Original: PM_WalkMove at 0x005160E0 (CoD2MP_s.exe 1.3; Quake III's name; pm, then pml, on the stack)
void walk_move(pmove::Pmove& pm, pmove::MoveLocal& pml, const pmove::MoveSettings& settings, trace::MoveHost& host);

/// The move of a dead player on the ground: the speed drops by 20.
/// Original: PM_DeadMove at 0x00516450 (CoD2MP_s.exe 1.3; Quake III's name; pml in eax, ps on the stack)
void dead_move(pmove::PlayerState& ps, const pmove::MoveLocal& pml);

/// Flying through walls (pm_type noclip; see the Rust version).
/// Original: PM_NoclipMove at 0x005164D0 (CoD2MP_s.exe 1.3; Quake III's name; pml in eax, pm in ecx)
void noclip_move(pmove::Pmove& pm, const pmove::MoveLocal& pml, const pmove::MoveSettings& settings);

/// Flying (pm_type UFO; see the Rust version).
/// Original: PM_UFOMove at 0x00516690 (CoD2MP_s.exe 1.3; our name; pm in eax, pml in ecx)
void ufo_move(pmove::Pmove& pm, const pmove::MoveLocal& pml, const pmove::MoveSettings& settings);

/// The move of a flying player (see the Rust version).
/// Original: PM_FlyMove at 0x00515E60 (CoD2MP_s.exe 1.3; Quake III's name; pml in eax, pm on the stack)
void fly_move(pmove::Pmove& pm, pmove::MoveLocal& pml, const pmove::MoveSettings& settings, trace::MoveHost& host);

/// The move on a ladder (see the Rust version).
/// Original: PM_LadderMove at 0x0051A360 (CoD2MP_s.exe 1.3; Return to Castle Wolfenstein's name; pml in eax, pm on the stack)
void ladder_move(pmove::Pmove& pm, pmove::MoveLocal& pml, const pmove::MoveSettings& settings, trace::MoveHost& host);

/// A ladder's surface (SURF_LADDER).
inline constexpr std::int32_t kSurfLadder = 0x8;

/// Off a ladder: the ladder bit cleared, kLadderFall set if it was on.
/// Original: PM_LeaveLadder at 0x0051A000 (CoD2MP_s.exe 1.3; our name; ps in eax)
void leave_ladder(pmove::PlayerState& ps);

/// Whether the player is on a ladder (see the Rust version).
/// Original: PM_CheckLadderMove at 0x0051A020 (CoD2MP_s.exe 1.3; Return to Castle Wolfenstein's name; pm, pml on the stack)
void check_ladder_move(pmove::Pmove& pm, pmove::MoveLocal& pml, trace::MoveHost& host);

}  // namespace iw_pc::game::moves
