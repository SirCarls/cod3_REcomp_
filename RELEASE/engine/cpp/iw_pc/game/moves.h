// The kinds of move (bg_pmove.c's PM_*Move), Call of Duty 2 (see the Rust version,
// iw_pc::game::moves).
#pragma once

#include "iw_pc/game/pmove.h"
#include "iw_pc/game/trace.h"

namespace iw_pc::game::moves {

/// The move in the air (see the Rust version).
/// Original: PM_AirMove at 0x00515FA0 (CoD2MP_s.exe 1.3; Quake III's name; pml in eax, pm on the stack)
void air_move(pmove::Pmove& pm, pmove::MoveLocal& pml, const pmove::MoveSettings& settings, trace::MoveHost& host);

}  // namespace iw_pc::game::moves
