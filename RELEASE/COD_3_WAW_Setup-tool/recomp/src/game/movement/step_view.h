// cod3 - [claude-9b] 2026-10-27 The drawn frame's values between the movement steps (movement/hooks_sp.cpp).

#pragma once

#include <cstdint>

namespace cod3::movement {

// Puts back the playerState values (the aim, the lean) written for the client game's frame between the last movement
// step's values (cod3_move_aim_smooth), each unless the game changed it since. Called after the client game's frame
// and before any game code runs (server_step.cpp). Game thread only.
void EndDrawnStepValues(uint8_t* base);

}  // namespace cod3::movement
