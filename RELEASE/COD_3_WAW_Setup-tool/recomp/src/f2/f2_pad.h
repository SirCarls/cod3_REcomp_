// f2 - the F2 window with a controller: Back + Start held for a second opens and closes it, and while it is open a
// controller moves around it. [f2-menus]
//
// Owner 2026-11-09 20:05 (the F2 plan's second question): "Back + Start held 1 s", so a split-screen player without
// a keyboard can reach the settings.
//
// The controllers are read here straight from SDL 3, which the SDK already uses for them and has already opened,
// and not through the SDK's input system: while the window is open the game gets no input (the input system's
// active callback says no, and its SDL driver then answers with an idle pad), but the window still has to see the
// controller that is moving around it. Nothing here goes to the game.
//
// UI thread only (the SDK's window loop, where SDL's events are pumped and Dear ImGui draws).

#pragma once

#include <array>
#include <chrono>
#include <cstdint>

#include "f2/f2_hold.h"

struct ImGuiIO;

namespace f2 {

class PadShortcut {
 public:
  // Once a UI frame. True on the frame Back + Start have been held together for a second on one controller.
  // `enabled` false (the game's setting turned it off) never fires.
  bool Poll(bool enabled);

  // How far the current hold is (0-1), for a small hint while it fills.
  float Progress() const;

  // While the window is open, every connected controller moves around it (Dear ImGui's gamepad navigation: the
  // D-pad and left stick move, A chooses, B goes back, LB and RB change the page). When it closes, every button is
  // let go, so nothing stays pressed in the window.
  void FeedNavigation(ImGuiIO& io, bool window_open);

  // -1 (LB) or +1 (RB) on the frame a shoulder button was pressed while the window is open; 0 otherwise.
  int TakePageStep();

 private:
  static constexpr int kMaxPads = 8;
  struct Pad {
    uint32_t id = 0;  // SDL_JoystickID; 0 = an empty slot
    HoldTimer hold;
  };

  void RefreshPadList(std::chrono::steady_clock::time_point now);

  std::array<Pad, kMaxPads> pads_{};
  std::chrono::steady_clock::time_point next_refresh_{};
  float progress_ = 0.0f;
  // What was last sent to Dear ImGui, so only changes are sent; all up when the window is closed.
  std::array<float, 20> sent_{};
  bool shoulder_left_ = false, shoulder_right_ = false;
  int page_step_ = 0;
};

}  // namespace f2
