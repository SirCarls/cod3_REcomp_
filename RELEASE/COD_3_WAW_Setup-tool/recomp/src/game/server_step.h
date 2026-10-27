// cod3 - [acct2-main] the campaign's game at a fixed rate (cod3_server_hz) and the moving things drawn
// between its updates (server_step.cpp).

#pragma once

#include <cstdint>

namespace cod3::server_step {

// The camera's offset while the player is carried by something the game moves in its updates (a
// vehicle): where the player was a part of an update ago, as the vehicle is drawn. False when the
// player walks (the movement steps smooth that) or nothing is to be smoothed. Game thread only.
bool PlayerViewOffset(uint8_t* base, float out[3]);

}  // namespace cod3::server_step
