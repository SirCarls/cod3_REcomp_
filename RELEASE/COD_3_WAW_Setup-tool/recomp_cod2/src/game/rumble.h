// cod2 / cod2mp - no controller vibration while the keyboard and mouse are in use. [input-port]
//
// [input-port] 2026-11-24 (owner: "make the controller module from our cod3 project work in the others"): Call of
// Duty 3's game/rumble.h, for Call of Duty 2.
//
// The game vibrates a controller through one small function, its wrapper of XamInputSetState (user r3,
// XINPUT_VIBRATION* r4: two big-endian motor speeds; campaign sub_8228A6B0, multiplayer sub_822897C0, the same six
// instructions, read from the generated code: r5 = r4, r4 = r6 = r7 = r8 = 0, then XamInputSetState). Call of Duty
// 3's wrapper is the same code. The SDK gives the keyboard and mouse to guest user 0 together with the first
// controller, so a player on the keyboard and mouse would feel the controller lying next to them buzz.
//
// The logic is shared with World at War and Call of Duty 4 (inputkit/rex/rumble_hold.h). While the keyboard and
// mouse are in use (inputkit/activity.h: a key, a mouse button or 24 counts of mouse motion
// since the controller's last button, trigger or stick push), user 0 gets no vibration. The game's own speeds are
// kept, and sent once the controller is used again. The game's copy of the speeds and its own vibration option are
// never changed. Users 1-3 (players on their own controllers) always get the game's vibration as it asks.
//
// cod2_rumble_with_keys = true lets the first controller vibrate anyway.
//
// Frame rate: the hold-back follows the last device used, not frames (inputkit/activity.h). A change takes effect
// at the next controller read, which the game makes every frame.

#pragma once

#include <cstdint>

struct PPCContext;

namespace cod2::rumble {

// Window thread (cod2_pc_input.cpp's listener), for every key or mouse event, taken or not.
void NoteKeyboardMousePress();
void NoteMouseMotion(float dx, float dy);

// Game thread, from the controller read hook (game/test_start.cpp) after each read of user 0: `result` is
// XInputGetState's answer (0 = connected), `state` the guest XINPUT_STATE it filled. Notes which device was used,
// stops user 0's vibration when the keyboard and mouse take over, and sends the game's current speeds when the
// controller comes back.
void OnPadRead(PPCContext& ctx, uint8_t* base, uint32_t result, uint32_t state);

}  // namespace cod2::rumble
