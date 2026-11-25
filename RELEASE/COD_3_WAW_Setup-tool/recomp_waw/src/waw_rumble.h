// waw - no controller vibration while the keyboard and mouse are in use. [input-port]
//
// [input-port] 2026-11-24 (owner: "make the controller module from our cod3 project work in the others"): Call of
// Duty 3's game/rumble.h, for World at War, through the code the ReXGlue ports share (inputkit/rex/rumble_hold.h).
//
// The game's XInput wrappers, read from the generated code: XamInputGetState's at 0x8237C120 (user r3, state r4;
// flags 1), called every frame; XamInputSetState's at 0x8237C130 (user r3, XINPUT_VIBRATION* r4), which checks the
// controller's capabilities and then sends the speeds it reads through r4. While the keyboard and mouse are in use,
// controller 1 gets zero speeds (a copy of ours; the game's own are kept and sent once the controller is used
// again). Players on their own controllers always feel the game's vibration. waw_rumble_with_keys = true lets
// controller 1 vibrate anyway.
//
// Frame rate: what is held back follows the last device used, not frames (inputkit/activity.h).

#pragma once

#include <rex/ui/window_listener.h>

namespace waw::rumble {

// A window listener that only watches the keys and the mouse (it never takes an event): added above the game's own.
rex::ui::WindowInputListener* Watcher();

}  // namespace waw::rumble
