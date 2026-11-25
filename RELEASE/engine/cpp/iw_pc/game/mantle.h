// Climbing over obstacles (mantling), Call of Duty 2 (see the Rust version, iw_pc::game::mantle).
#pragma once

#include "iw_pc/game/pmove.h"

namespace iw_pc::game::mantle {

/// The mantle's dvars (kept in pmove.h, part of the movement's).
using MantleSettings = pmove::MantleSettings;

/// Keeps the view within mantle_view_yawcap of the climb's direction (see the Rust version).
/// Original: Mantle_CapView at 0x00512B80 (CoD2MP_s.exe 1.3; Call of Duty 4's name; ps in esi)
void mantle_cap_view(pmove::PlayerState& ps, const MantleSettings& settings);

}  // namespace iw_pc::game::mantle
