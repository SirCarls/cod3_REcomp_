// cod3 - which game program this build is (program.h). The campaign (cod3)
// uses the campaign's table. The movement files are not in cod3mp's source
// list (recomp/mp/CMakeLists.txt) yet; when they are ported to it, this is
// the one place that picks kMultiplayer (program_mp.h) under COD3_MP.

#pragma once

#include "game/movement/program_sp.h"

namespace cod3::movement {

inline constexpr const Program& kProgram = kCampaign;

}  // namespace cod3::movement
