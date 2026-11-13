// waw - keys the PC way: key events handed to the game's own CL_KeyEvent, so the PC game's binds (`bind SPACE
// +gostand`, `bind MOUSE1 +attack`) work as on the PC. [f2-menus] 2026-11-09, dev/agents/f2-menus/BINDINGS-PLAN.md:
//   step 1: the path into the game and a scripted test (waw_debug_keys);
//   step 2: the window's keys, mouse buttons and wheel during gameplay (Listener), and the PC's default binds
//           (waw_pc_binds) given to the keys the game has no bind for.
// A key the game has a bind for goes to the game; any other key stays with the SDK's keyboard pad (keybind_*), so
// nothing is lost while a bind is missing, and the menus keep the keyboard pad.
//
// From the decomp (dev/waw-decomp/x360_sp): CL_KeyEvent 0x82141C50 (local client r3, controller r4, key r5, down r6,
// time r7), the game's key names table 0x82493060 (name, number pairs), Sys_Milliseconds 0x822A3048, Cbuf_AddText
// 0x82238978, the key states 0x82517954 (16 bytes a key: down, repeats, two binds' text; 0x1128 bytes a local
// client; Key_Bind_f 0x821433B8 reads 0x8251795C + key * 16), the key catchers 0x842F35E8 (0x10: a menu has the keys).

#pragma once

#include <cstdint>
#include <string>

#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/window_listener.h>

struct PPCContext;

namespace waw::keys {

// The game's number for a PC key name (SPACE, MOUSE1, ENTER, a, BUTTON_A ...), from its own names table; one
// character is its own number (letters lower-case, as the PC's keys are sent); "0xNN" is that number. -1: unknown.
int KeyNumber(const uint8_t* base, const std::string& name);

// A menu has the keys (the game's key catchers, as the PC's KEYCATCH_UI); false before the first frame.
bool MenuTakesKeys();

// Queues a press or a release of the PC key `name` (any thread); the next frame sends it.
void Queue(const std::string& name, bool down);

// Once a frame on the game's main thread (waw_hooks.cpp, waw_frame_start): gives the PC's default binds to unbound
// keys when gameplay starts, sends the queued key events to CL_KeyEvent in order, and runs waw_debug_keys. Costs a few
// atomic loads on a frame without any.
void RunFrame(PPCContext& ctx, uint8_t* base);

// The window's keys, mouse buttons and wheel, the PC way, during gameplay (the game reads mouse counts: waw_mouse.h),
// while the window has focus and the F2 window isn't using the input. Added in WawApp::OnPostSetup below the mouse
// look's listener and above the SDK's keyboard/mouse driver.
class Listener : public rex::ui::WindowInputListener {
 public:
  rex::ui::Window* window = nullptr;

  void OnKeyDown(rex::ui::KeyEvent& e) override;
  void OnKeyUp(rex::ui::KeyEvent& e) override;
  void OnMouseDown(rex::ui::MouseEvent& e) override;
  void OnMouseUp(rex::ui::MouseEvent& e) override;
  void OnMouseWheel(rex::ui::MouseEvent& e) override;
};

}  // namespace waw::keys
