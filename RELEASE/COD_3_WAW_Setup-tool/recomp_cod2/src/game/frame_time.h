// cod2 - where a frame's time goes (cod2_debug_frame_time, frame_time.cpp).
// [acct2-cod2]

#pragma once

#include <cstdint>

namespace cod2::frame_time {

// Com_Frame's start and end, from its hook (game/frame_rate.cpp): the whole
// frame's part of the test and its log line. FrameStart returns 0 while the
// test is off; FrameEnd takes what it returned.
int64_t FrameStart();
void FrameEnd(int64_t start);

}  // namespace cod2::frame_time
