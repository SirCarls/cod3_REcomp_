// cod4 / cod4mp - aim with the mouse through the game's own mouse look. [acct2-cod4]
//
// The 360 programs kept IW3's PC mouse look: CL_MouseMove (campaign 0x822CC838, called once a usercmd from
// 0x822CDE70) turns the view by the counts that CL_GetMouseMovement (0x822CAFB0) reads from clientActive_t (r3):
// mouseDx[2] at +51520, mouseDy[2] at +51528, mouseIndex at +51536 (with m_filter averaging the two slots). On the
// 360 nothing fills those counts. With cod4_mouse_look on, the window holds the pointer during gameplay and its motion
// is added to the current slot just before the game reads it, so the game's own sensitivity, m_yaw, m_pitch,
// m_filter and its zoomed-in scale apply, as on the PC. (Call of Duty 3 writes the view angles itself,
// game/mouse_look.cpp; CoD4 has the PC path to use.)
//
// Multiplayer runs the same code on a larger clientActive_t: CL_GetMouseMovement 0x822D14C8 (mouseDx[2] at +152768,
// mouseDy[2] at +152776, mouseIndex at +152784), CL_MouseMove 0x822D3D48.

#pragma once

#include <cstdint>

#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

namespace cod4::mouse {

// cod4_mouse_look and the keyboard pad (mnk_mode) both on.
bool Enabled();

// The window's mouse: holds the pointer (relative mode, else re-centering) while the game reads mouse counts and the
// window has focus, and hands the motion to the game. Added in Cod4App::OnPostSetup, above the SDK's keyboard/mouse
// driver, which then doesn't see the motion (so mnk_mouse can't turn the view a second time).
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

}  // namespace cod4::mouse
