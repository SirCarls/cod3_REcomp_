// cod3 - no controller vibration while the keyboard and mouse are in use.
//
// The game vibrates a controller through one small function, its wrapper of
// XamInputSetState (user, XINPUT_VIBRATION*: two big-endian motor speeds;
// campaign sub_82344FA8, cod3mp sub_82314370, the same six instructions),
// each time its motor speeds change and the player's own Vibration option is
// on. The SDK gives the keyboard and mouse to guest user 0 together with the
// first controller (input/device_assignment.cpp, SlotAssignment), so a player
// on the keyboard and mouse would feel the controller lying next to them buzz.
//
// While the keyboard and mouse are in use (game/button_prompts.h,
// KeyboardMouseInUse: mnk_mode on and a key or mouse press since the last
// controller button, trigger or stick push), user 0 gets no vibration: the
// game's own speeds are kept, and sent once the controller is used again.
// The game's copy of the speeds and its Vibration option are never changed.
//
// Split screen: only user 0 (player 1's controller, the one the keyboard and
// mouse share) is ever held back. Users 1-3, players 2-4 on their own
// controllers, always get the game's vibration as it asks.
//
// cod3_rumble_with_keys = true lets the first controller vibrate anyway.

#pragma once

#include <cstdint>

struct PPCContext;

namespace cod3::rumble {

// Game thread, from the pad state hook (game/button_prompts.cpp) each time the
// game reads user 0's pads: stops user 0's vibration when the keyboard and
// mouse take over, and sends the game's current speeds when the controller
// comes back.
void OnPadRead(PPCContext& ctx, uint8_t* base);

}  // namespace cod3::rumble
