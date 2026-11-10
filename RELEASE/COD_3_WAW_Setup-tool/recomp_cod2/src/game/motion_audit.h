// cod2 - tests: how what is drawn moves from frame to frame. [acct2-cod2]
#pragma once

#include <cstdint>

namespace cod2::motion_audit {

// The player's view after the client's prediction (and the fixed step's
// smoothing): called by movement.cpp's CG_PredictPlayerState_Internal hook.
void OnPlayerPredicted(const uint8_t* base);

}  // namespace cod2::motion_audit
