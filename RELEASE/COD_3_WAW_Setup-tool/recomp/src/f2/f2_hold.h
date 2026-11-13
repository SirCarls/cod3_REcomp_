// f2 - "held for a second": the rule for opening the F2 window with a controller. [f2-menus]
//
// Owner 2026-11-09 20:05: Back + Start held 1 s opens the F2 window (the F2 plan's second question), so a
// split-screen player without a keyboard can reach it. Plain logic with no SDK or SDL in it, so its tests build on
// their own (dev/agents/f2-menus/tests/f2_hold_test.cpp).
//
// The rules (the hold to skip's, recomp/src/game/cutscene_skip.h, where they apply):
//  - the hold is timed in real time, never in frames, so it lasts the same at any frame rate;
//  - it fires once; the buttons must be let go before it can fire again (holding on doesn't open and close the
//    window over and over);
//  - letting go of either button early starts the count again.

#pragma once

#include <chrono>

namespace f2 {

class HoldTimer {
 public:
  using Clock = std::chrono::steady_clock;

  HoldTimer() = default;  // a second
  explicit HoldTimer(std::chrono::milliseconds hold) : hold_(hold) {}

  // Once a frame: whether the buttons are all down now. True on the one frame the hold completes.
  bool Update(bool held, Clock::time_point now) {
    if (!held) {
      holding_ = false;
      fired_ = false;
      return false;
    }
    if (!holding_) {
      holding_ = true;
      since_ = now;
    }
    if (fired_ || now - since_ < hold_) return false;
    fired_ = true;
    return true;
  }

  // 0 to 1: how far the current hold is (for a small hint while it fills); 0 when nothing is held or it fired.
  float Progress(Clock::time_point now) const {
    if (!holding_ || fired_) return 0.0f;
    const float done = std::chrono::duration<float>(now - since_).count() /
                       std::chrono::duration<float>(hold_).count();
    return done < 0.0f ? 0.0f : (done > 1.0f ? 1.0f : done);
  }

 private:
  std::chrono::milliseconds hold_{1000};
  bool holding_ = false;
  bool fired_ = false;
  Clock::time_point since_{};
};

}  // namespace f2
