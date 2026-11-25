// The view: leaning (see the Rust version, iw_pc::game::view).
#pragma once

#include <cstdint>

#include "iw_pc/game/pmove.h"
#include "iw_pc/game/trace.h"

namespace iw_pc::game::view {

/// The lean after msec, kept to the room a trace leaves (see the Rust version); `handler` names the
/// trace function the original gets.
/// Original: PM_UpdateLean at 0x00518FF0 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in esi, cmd in eax, then (msec, capsuleTrace) on the stack)
void update_lean(pmove::PlayerState& ps, float msec, const std::uint8_t* cmd, std::uint8_t handler, trace::MoveHost& host);

/// The view's angles from the command, kept within what the player may look at (see the Rust
/// version), then the lean.
/// Original: PM_UpdateViewAngles at 0x00519340 (CoD2MP_s.exe 1.3; Quake III's name; ps in eax, then (msec, cmd, handler) on the stack)
void update_view_angles(pmove::PlayerState& ps, float msec, const std::uint8_t* cmd, std::uint8_t handler,
                        const pmove::MoveSettings& settings, trace::MoveHost& host);

}  // namespace iw_pc::game::view
