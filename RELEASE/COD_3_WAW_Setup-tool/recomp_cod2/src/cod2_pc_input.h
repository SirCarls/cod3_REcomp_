// cod2 / cod2mp - the PC copy's keys and mouse. [f2-menus]
//
// Owner 2026-11-10 15:57: "cod2 should copy what the PC copy has." (asked: all of it; options and dvars first, then
// keys and mouse, then movement feel). This is the keys and mouse.
//
// The Xbox 360 programs kept the PC game's input code next to the controller's:
//   - the mouse: CL_MouseMove (campaign 0x8242B4D0, multiplayer 0x8241A738; called once a usercmd by CL_CreateCmd)
//     turns the view by the counts in cl.mouseDx[2] / mouseDy[2] (the slot cl.mouseIndex picks), through the game's
//     own sensitivity, m_yaw, m_pitch, m_filter, cl_freelook and the zoom's scale. On the 360 nothing fills the
//     counts. With cod2_mouse_look on, the window holds the pointer while a level plays and its motion is added to
//     the current slot just before the game reads it (as Call of Duty 4's port does, ../recomp_cod4/src/cod4_mouse.h);
//   - the keys: every command the PC copy binds a key to is still a console command here (+forward, +leanleft,
//     gocrouch, goprone, toggleads, weapnext, +melee, +holdbreath, ... in both programs' strings). With cod2_pc_keys
//     on, a key in cod2_pc_binds sends its command to the game's console when it goes down, and a "+" command's
//     "-" twin when it comes up, the way the PC game's own binds do. So the keys do what they do on the PC (lean on
//     Q and E, crouch and prone on their own keys, aim toggles) instead of pressing a controller's button.
//
// Both only while a level plays with no menu or console open (the game's own key catchers are 0 and it made a
// usercmd in the last second and a half) and the F2 window is closed. Everywhere else the keys go on to the SDK's
// keyboard pad as before, so menus, the Press START screen and the pause menu work as they did; Escape is never
// taken (it stays START: the pause menu).
//
// The default keys are the PC copy's default.cfg (read with dev/agents/f2-menus/tools/cod2_pc_binds.py), with one
// change: there Shift is melee and hold breath in one (+melee_breath). Here Shift is sprint (game/sprint.cpp) and
// hold breath, and melee is V, as in the Call of Duty 4 and World at War ports.
//
// The turn: the 360 program multiplies the mouse's turn by its wide picture's share of the field of view (1.2414 at
// cg_fov 65), so the same sensitivity turns a quarter more than on the PC. cod2_mouse_match_pc (on) takes that share
// back out of the counts: from the hip, sensitivity x 0.022 degrees a count, as the PC copy.
//
// Multiplayer: written from the decompile only. cod2mp doesn't start a level in this port yet (it shows "the
// current profile is not allowed to play on Xbox Live"), so nothing of this has run there; the counts are only
// written while the game's slot index reads 0 or 1.
//
// Read from the game's code (dev/waw-decomp/cod2_360/{sp,mp}/decomp): the campaign's counts are globals (mouseDx
// 0x828E9EB8, mouseDy 0x828E9EC0, mouseIndex 0x828E9EC8; the view's pitch 0x828E9F08 and yaw 0x828E9F0C; the key
// catchers 0x828E0884: 1 the console, 8 a menu); multiplayer reaches the same through the pointer at 0x824D37E0
// (+0x85D8, +0x85E0, +0x85E8; pitch +0x8620, yaw +0x8624; key catchers +4).

#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

namespace cod2::pc_input {

// The window's keys and mouse, above the SDK's keyboard/mouse driver (Cod2App::OnPostSetup): a key or button it
// takes doesn't reach the driver, so it doesn't press a controller button as well.
class Listener : public rex::ui::WindowInputListener {
 public:
  rex::ui::Window* window = nullptr;

  void OnKeyDown(rex::ui::KeyEvent& e) override;
  void OnKeyUp(rex::ui::KeyEvent& e) override;
  void OnMouseDown(rex::ui::MouseEvent& e) override;
  void OnMouseUp(rex::ui::MouseEvent& e) override;
  void OnMouseMove(rex::ui::MouseEvent& e) override;
  void OnMouseWheel(rex::ui::MouseEvent& e) override;

 private:
  void UpdateCapture();

  bool captured_ = false;
  bool relative_ = false;  // pointer lock (else the cursor is put back at the window's center)
  int32_t prev_x_ = 0;
  int32_t prev_y_ = 0;
  rex::ui::Window::CursorVisibility prev_cursor_ = rex::ui::Window::CursorVisibility::kVisible;
};

// The game's main thread, once a controller read (game/test_start.cpp's hook): notes whether a level is playing,
// lets the held keys go when it stops, keeps the game's mouse dvars at the port's settings, runs the test script.
void OnGameFrame(const uint8_t* base);

// [f2-menus] 2026-11-11 00:15 True while a level runs (the game is making usercmds), menus open or not. Any thread.
bool LevelRuns();

// The commands of the keys pressed and let go since the last call, for the game's command buffer. True when there
// are any.
bool TakeCommands(std::vector<std::string>& out);

}  // namespace cod2::pc_input
