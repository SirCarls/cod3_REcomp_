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

// [acct2-ocr] 2026-11-07 (cod3_server_smooth_linked) After SmoothView (movement/hooks_sp.cpp), `moved`: whether it
// moved the camera. In the setting's camera modes, while the player rides no vehicle and SmoothView moved nothing,
// the camera's place is drawn between its places at the last two updates (a player linked to something the game moves
// in its updates, which the movement steps don't move). Game thread only, after CG_CalcViewValues.
void SmoothLinkedCamera(uint8_t* base, bool moved);

// [acct2-cod3-motion] 2026-11-07 (cod3_server_draw_subms) The part of a millisecond (-1 to 1) the game's clock hasn't
// counted this frame: the real time since the clock started, less the game's whole milliseconds; 0 when the setting is
// off. For drawing between fixed steps only (add it to a time on the game's clock); never for game time.
float DrawTimeFraction();

// [claude-9b-think] 2026-10-27 UNTESTED True while the player rides a vehicle and cod3_server_smooth_ride is on.
bool PlayerRides(uint8_t* base);

// [claude-9b-think] 2026-11-02 UNTESTED (cod3_move_hz_sync) The offset (ms, 0 or more) to add to the movement steps'
// ends (rules.h MoveStepEndAfter, on the usercmd clock) so that the game's updates end on step ends: each update then
// sees exactly one more step of cod3_move_hz (more when move_hz is a multiple of cod3_server_hz). 0 when the setting is
// off, the game isn't in fixed updates (cod3_server_hz 0, none run yet), or move_hz isn't a multiple of the update
// rate. Game thread only (the usercmd's Pmove).
int MoveStepPhase(uint8_t* base, int move_hz);

}  // namespace cod3::server_step
