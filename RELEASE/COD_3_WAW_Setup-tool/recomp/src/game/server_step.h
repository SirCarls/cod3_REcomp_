// cod3 - [acct2-main] the campaign's game at a fixed rate (cod3_server_hz) and the moving things drawn
// between its updates (server_step.cpp).

#pragma once

#include <cstdint>

namespace cod3::server_step {

// The camera's offset while the player is carried by something the game moves in its updates (a
// vehicle): where the player was a part of an update ago, as the vehicle is drawn. False when the
// player walks (the movement steps smooth that) or nothing is to be smoothed. Game thread only.
bool PlayerViewOffset(uint8_t* base, float out[3]);

// [claude-9b-think] 2026-10-27 UNTESTED (cod3_server_smooth_ride) The camera's offset on a vehicle the player rides,
// in first person: from where the game placed it (cam: at the vehicle's pose of the last update) to the same place on
// the vehicle as it is drawn this frame. 1: out is set; 0: not riding, a camera mode the ride leaves (a turret's, third
// person: cod3_server_smooth_ride_cams) or the setting is off; -1: riding, nothing to move (the movement steps don't
// move a rider: their offset is left out).
// Game thread only, after 0x824505E8.
int RideViewOffset(uint8_t* base, const float cam[3], float out[3]);

// [claude-9b-think] 2026-10-27 UNTESTED True while the player rides a vehicle and cod3_server_smooth_ride is on.
bool PlayerRides(uint8_t* base);

}  // namespace cod3::server_step
