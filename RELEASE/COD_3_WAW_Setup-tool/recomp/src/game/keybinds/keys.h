// cod3 - keyboard and mouse keys for the port's own key binds
// (game/keybinds/binds.h): codes and the names cod3.toml, F2, the console
// and the prompts use.
//
// A KeyCode is a Windows virtual-key code for a keyboard key (the values of
// rex::ui::VirtualKey, 1-255) or one of the mouse codes below. Names are
// World at War's (Quake III's): B, SHIFT, CTRL, SPACE, UPARROW, MOUSE1,
// MWHEELUP ... Reading also takes the SDK's own spellings (Shift, Control,
// Return, Up, LMB, MB4 ...), in any case.
//
// No modifier combinations ("Shift+W"): World at War has none, and they were
// how Shift+arrows reached the binoculars. A bare SHIFT, CTRL or ALT is an
// ordinary key.
//
// Plain code: no game, no SDK (the unit test builds it on its own).

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace cod3::binds {

using KeyCode = uint16_t;

inline constexpr KeyCode kNoKey = 0;
// Keyboard keys used by name in code (Windows virtual-key codes).
inline constexpr KeyCode kKeyTab = 0x09;
inline constexpr KeyCode kKeyEnter = 0x0D;
inline constexpr KeyCode kKeyShift = 0x10;
inline constexpr KeyCode kKeyCtrl = 0x11;
inline constexpr KeyCode kKeyAlt = 0x12;
inline constexpr KeyCode kKeyEscape = 0x1B;
inline constexpr KeyCode kKeyB = 0x42;
inline constexpr KeyCode kKeyF1 = 0x70;
inline constexpr KeyCode kKeyBacktick = 0xC0;  // ` and ~ (the console key)
// Mouse buttons and the wheel (not virtual-key codes).
inline constexpr KeyCode kMouse1 = 0x101;  // left
inline constexpr KeyCode kMouse2 = 0x102;  // right
inline constexpr KeyCode kMouse3 = 0x103;  // middle
inline constexpr KeyCode kMouse4 = 0x104;  // back side button
inline constexpr KeyCode kMouse5 = 0x105;  // forward side button
inline constexpr KeyCode kWheelUp = 0x106;
inline constexpr KeyCode kWheelDown = 0x107;
inline constexpr size_t kKeyCodeCount = 0x108;

// A name -> its code (any case, either spelling); kNoKey when unknown, empty,
// or a modifier combination ("Shift+Up").
KeyCode ParseKey(std::string_view name);

// The code's World at War name ("B", "SHIFT", "MOUSE4"); "" for none.
std::string KeyName(KeyCode code);

// As the game's prompts show it: the same, with the arrows as UP / DOWN /
// LEFT / RIGHT and the keypad as NUM.
std::string PromptName(KeyCode code);

// True for a name with a modifier prefix ("Shift+Up", "Ctrl+W").
bool IsCombination(std::string_view name);

// Keys never bound to an action: ` / ~ (the console) and ESCAPE (the pause
// menu, and what cancels). The overlays' own keys (F2, F3 ...) are checked
// by the caller against their live settings.
bool IsAlwaysReserved(KeyCode code);

}  // namespace cod3::binds
