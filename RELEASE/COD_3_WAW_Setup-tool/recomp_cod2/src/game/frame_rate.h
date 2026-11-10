// cod2 - the frame rate: the guest vblank and the frame limit (frame_rate.cpp).
// [acct2-cod2]

#pragma once

namespace cod2::frame_rate {

// Runs the guest vblank at 1000 Hz unless the player set guest_vblank_rate
// (command line, environment or cod2.toml). Called by Cod2App before and
// after the runtime's set-up; the SDK reads the rate every millisecond.
void ApplyGpuDefaults();

// Before each Com_Frame on the game's main thread: waits until the next frame
// is due under cod2_max_fps (no wait when it is 0).
void BeginFrame();

}  // namespace cod2::frame_rate
