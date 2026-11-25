// Jumping (Call of Duty 4's bg_jump.cpp; Call of Duty 2's is among its movement code; see the Rust
// version, iw_pc::game::jump). The jump's first functions (jump_reduce_friction,
// jump_get_step_height, jump_clamp_velocity) are in pmove.h.
#pragma once

#include <cstdint>

#include "iw_pc/game/pmove.h"
#include "iw_pc/game/trace.h"
#include "iw_pc/x87.h"

namespace iw_pc::game::jump {

/// The first of the jump events, one for each surface type (our name; the ladder's surface type
/// off a ladder).
inline constexpr std::int32_t kEvJump = 0x46;

/// Starts the landing's slowdown when none runs (pm_time 0): kJumping and 1.8 s of pm_time.
/// Original: Jump_ActivateSlowdown at 0x0052FC40 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in eax)
void jump_activate_slowdown(pmove::PlayerState& ps);

/// The landing's slowdown of the velocity (see the Rust version).
/// Original: Jump_ApplySlowdown at 0x0052FC60 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in ecx)
void jump_apply_slowdown(pmove::PlayerState& ps, const pmove::MoveSettings& settings);

/// How much a jump soon after landing loses: 1, 2.5 from 1.7 s of pm_time, else pm_time * 0.00088235294 + 1.
/// Original: Jump_GetLandFactor at 0x0052FDF0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps on the stack, the result on the x87 stack)
x87::F24 jump_get_land_factor(const pmove::PlayerState& ps, const pmove::MoveSettings& settings);

/// Starts a jump `height` high (see the Rust version).
/// Original: Jump_Start at 0x0052FE30 (CoD2MP_s.exe 1.3; Call of Duty 4's name; pm in edi, pml in esi, height on the stack)
void jump_start(pmove::Pmove& pm, pmove::MoveLocal& pml, float height, const pmove::MoveSettings& settings);

/// The jump's event for the ladder or the ground's surface type.
/// Original: Jump_AddSurfaceEvent at 0x0052FFB0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in eax, pml on the stack)
void jump_add_surface_event(pmove::PlayerState& ps, const pmove::MoveLocal& pml);

/// A jump off a ladder (see the Rust version).
/// Original: Jump_PushOffLadder at 0x0052FED0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in edi, pml in ebx)
void jump_push_off_ladder(pmove::PlayerState& ps, const pmove::MoveLocal& pml, const pmove::MoveSettings& settings);

/// Whether the command jumps, and if so the jump (see the Rust version).
/// Original: Jump_Check at 0x00530020 (CoD2MP_s.exe 1.3; Call of Duty 4's name; pml in eax, pm on the stack; al)
bool jump_check(pmove::Pmove& pm, pmove::MoveLocal& pml, const pmove::MoveSettings& settings, trace::MoveHost& host);

}  // namespace iw_pc::game::jump
