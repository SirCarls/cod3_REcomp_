// cod3 - a test-only tour of the F2 settings window (ui/hud_overlay.cpp). [rexglue-native-f2] 2026-11-11 f2t1
//
// A test run can't open F2: the window opens from the F2 key only, and test windows never get the keyboard, so
// nobody could take a picture of it. With the test setting cod3_debug_f2_tour set, the window opens by itself a while
// after the start and shows each page in turn, a screen at a time on a long page, writing one log line for each view:
//   cod3: f2 tour: VIDEO/Resolution part 1 of 2 (pid 12345)
// so a script outside the game can take a picture of the game's window at each line (rexglue-native/f2/f2shots.sh).
// When it is done it selects the pages that were selected before, scrolls every page back to its top and closes the
// window. It changes no setting and never saves; it only clicks through the window as a person would.
// Off (empty, the default): nothing happens and nothing is logged.
#pragma once

#include <functional>

namespace cod3::ui::f2tour {

// Once a UI frame, from the HUD overlay's frame callback (HudOverlay::on_frame, which runs inside the Dear ImGui frame
// before the F2 window draws). `toggle` opens or closes the F2 window (the app's ToggleSettings, deferred so that no
// window is made or destroyed while the windows draw); `open` says whether the F2 window exists now.
void Tick(const std::function<void()>& toggle, bool open);

}  // namespace cod3::ui::f2tour
