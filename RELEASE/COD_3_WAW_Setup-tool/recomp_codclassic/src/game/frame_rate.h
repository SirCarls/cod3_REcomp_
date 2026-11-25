// codclassic - the frame limit (frame_rate.cpp). [acct2-uo]

#pragma once

namespace codclassic::frame_rate {

// Before each Com_Frame on the game's main thread: waits until the next frame is due under
// codclassic_max_fps (no wait when it is 0).
void BeginFrame();

}  // namespace codclassic::frame_rate
