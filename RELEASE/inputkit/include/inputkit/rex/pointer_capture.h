// inputkit - holding the mouse pointer for mouse look, in the ports built on the ReXGlue SDK (World at War, Call of
// Duty 4, Call of Duty 2; header only, so inputkit itself needs no SDK). [input-port] 2026-11-24
//
// The three ports had the same code (waw_mouse.cpp, cod4_mouse.cpp, cod2_pc_input.cpp): while the game wants the
// mouse (each game decides: mouse look on, a level playing, the window focused, no menu or F2 window), the pointer is
// hidden and held (relative mode, else put back at the window's center after each move) and its motion goes to the
// game; otherwise it is let go as it was. Each game's window listener calls Update with its own answer and Motion
// for each move event.

#pragma once

#include <cstdint>

#include <rex/logging.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>

namespace inputkit::rexui {

class PointerCapture {
 public:
  // Holds or lets go of the pointer. True when that changed: the caller drops motion not yet given to the game,
  // since motion from before the switch belongs to the menu, not to the view. `game` names it in the log ("waw").
  bool Update(rex::ui::Window* window, bool want, const char* game) {
    if (!window || want == held_) return false;
    if (want) {
      prev_cursor_ = window->GetCursorVisibility();
      window->SetCursorVisibility(rex::ui::Window::CursorVisibility::kHidden);
      window->CaptureMouse();
      relative_ = window->SetRelativeMouseMode(true);
      if (!relative_) window->WarpMouseToCenter(prev_x_, prev_y_);
      held_ = true;
      REXLOG_INFO("{}: mouse look holds the pointer ({})", game, relative_ ? "relative mode" : "re-centering");
    } else {
      if (relative_) window->SetRelativeMouseMode(false);
      window->ReleaseMouse();
      window->SetCursorVisibility(prev_cursor_);
      held_ = false;
      relative_ = false;
    }
    return true;
  }

  // A move event while held: the motion in *dx, *dy (counts), the event marked handled so the SDK's keyboard/mouse
  // driver doesn't turn the right stick as well, and true. False (nothing changed) while not held.
  bool Motion(rex::ui::Window* window, rex::ui::MouseEvent& e, float* dx, float* dy) {
    if (!held_) return false;
    e.set_handled(true);
    if (relative_) {
      *dx = float(e.dx());
      *dy = float(e.dy());
      return true;
    }
    // No pointer lock: measured from the window's center, and the cursor put back there.
    *dx = float(e.x() - prev_x_);
    *dy = float(e.y() - prev_y_);
    int32_t cx = 0, cy = 0;
    if (window && window->WarpMouseToCenter(cx, cy)) {
      prev_x_ = cx;
      prev_y_ = cy;
    } else {
      prev_x_ = e.x();
      prev_y_ = e.y();
    }
    return true;
  }

  bool held() const { return held_; }

 private:
  bool held_ = false;
  bool relative_ = false;  // pointer lock (else the cursor is put back at the window's center)
  int32_t prev_x_ = 0;
  int32_t prev_y_ = 0;
  rex::ui::Window::CursorVisibility prev_cursor_ = rex::ui::Window::CursorVisibility::kVisible;
};

}  // namespace inputkit::rexui
