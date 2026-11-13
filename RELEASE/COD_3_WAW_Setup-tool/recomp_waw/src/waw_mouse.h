// waw - aim with the mouse through the game's own mouse look. [f2-menus]
//
// The owner, 2026-11-09 22:00: "also work on mouse for all the iw3 games." Call of Duty 4 has it
// (recomp_cod4/src/cod4_mouse.*, acct2-cod4's); this is the same for World at War, whose Xbox 360 program kept the
// same PC code (our decomp, dev/waw-decomp/x360_sp): CL_MouseMove (0x822C91A0, once a usercmd) turns the view by the
// counts that CL_GetMouseMovement (0x822C9068) reads from clientActive_t (r3): mouseDx[2] at +0x27128 (160040),
// mouseDy[2] at +0x27130 (160048), mouseIndex at +0x27138 (160056), m_filter averaging the two slots. On the 360
// nothing fills those counts. With waw_mouse_look on, the window holds the pointer during gameplay and its motion is
// added to the current slot just before the game reads it, so the game's own sensitivity, m_yaw, m_pitch, m_filter,
// cl_mouseAccel and its zoomed-in scale apply, as on the PC.

#pragma once

#include <cstdint>

#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

namespace waw::mouse {

// waw_mouse_look and the keyboard pad (mnk_mode) both on.
bool Enabled();

// The game read mouse counts in the last 1.5 s: gameplay (a menu or a load stops the reads). waw_keys.h uses it too.
bool InGameplay();

// The keyboard pad's keys as a PC shooter's (only where nobody chose a value), before the input drivers start.
void ApplyInputDefaults();

// The window's mouse: holds the pointer (relative mode, else re-centering) while the game reads mouse counts, the
// window has focus and the F2 window isn't using the input, and hands the motion to the game. Added in
// WawApp::OnPostSetup above the SDK's keyboard/mouse driver, which then doesn't see the motion (so mnk_mouse can't turn
// the view a second time).
class Listener : public rex::ui::WindowInputListener {
 public:
  rex::ui::Window* window = nullptr;

  void OnMouseMove(rex::ui::MouseEvent& e) override;
  void OnMouseDown(rex::ui::MouseEvent& e) override;

 private:
  void UpdateCapture();

  bool captured_ = false;
  bool relative_ = false;  // pointer lock (else the cursor is put back at the window's center)
  int32_t prev_x_ = 0;
  int32_t prev_y_ = 0;
  rex::ui::Window::CursorVisibility prev_cursor_ = rex::ui::Window::CursorVisibility::kVisible;
};

}  // namespace waw::mouse
