// The ground under the player (bg_pmove.c's ground trace): the trace down that finds it, the way
// out of a solid, leaving it, and landing on it. Call of Duty 2's versions (see the Rust version,
// iw_pc::game::ground).
#pragma once

#include <cstdint>

#include "iw_pc/game/pmove.h"
#include "iw_pc/game/trace.h"

namespace iw_pc::game::ground {

/// The first landing events (each + the ground's surface type): a hard landing, and one that hurts
/// (our names).
inline constexpr std::int32_t kEvLanding = 0x5D, kEvLandingPain = 0x74;

/// What crash_land warns of when the fall damage's heights are the wrong way round (the game's text).
inline constexpr const char* kFallDamageHeightsWarning =
    "bg_fallDamageMaxHeight must be greater than bg_fallDamageMinHeight\n";

/// The event of a fall of 8 to 12 units: a running footstep on the surface type, or 0.
/// Original: PM_MediumLandingForSurface at 0x005169C0 (CoD2MP_s.exe 1.3; our name; pml in eax)
std::int32_t medium_landing_for_surface(const pmove::MoveLocal& pml);

/// The event of a fall of more than 12 units: kEvLanding + the surface type.
/// Original: PM_HardLandingForSurface at 0x005169E0 (CoD2MP_s.exe 1.3; our name; pml in eax)
std::int32_t hard_landing_for_surface(const pmove::MoveLocal& pml);

/// Landing: the fall's height, damage, slowdown, animation and events (see the Rust version).
/// Original: PM_CrashLand at 0x00516A20 (CoD2MP_s.exe 1.3; Quake III's name; ps in esi, pml on the stack)
void crash_land(pmove::PlayerState& ps, const pmove::MoveLocal& pml, const pmove::MoveSettings& settings,
                trace::MoveHost& host);

/// The way out of a solid: 26 steps of a unit, the first free one, then down (see the Rust version).
/// Original: PM_CorrectAllSolid at 0x00516DA0 (CoD2MP_s.exe 1.3; Quake III's name; pm in ecx, trace in eax, pml on the stack)
bool correct_all_solid(pmove::Pmove& pm, pmove::MoveLocal& pml, trace::Trace& trace, trace::MoveHost& host);

/// No ground below: off it; how near it is (see the Rust version).
/// Original: PM_GroundTraceMissed at 0x00516F10 (CoD2MP_s.exe 1.3; Quake III's name; pm in eax, pml in ebx)
void ground_trace_missed(pmove::Pmove& pm, pmove::MoveLocal& pml, trace::MoveHost& host);

/// The ground under the player (see the Rust version).
/// Original: PM_GroundTrace at 0x00517080 (CoD2MP_s.exe 1.3; Quake III's name; pm, pml on the stack)
void ground_trace(pmove::Pmove& pm, pmove::MoveLocal& pml, const pmove::MoveSettings& settings, trace::MoveHost& host);

}  // namespace iw_pc::game::ground
