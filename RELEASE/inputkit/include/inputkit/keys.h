// inputkit - keyboard and mouse key codes and their names, shared by every game in the project.
//
// [input-port] 2026-11-24, taken out of Call of Duty 3's game/keybinds/keys.{h,cpp} (owner: "make the controller
// module from our cod3 project work in the others"; "Same goes with how the mouse works"). CoD3's results are
// unchanged: its keys.h now names these.
//
// A KeyCode is a Windows virtual-key code for a keyboard key (1-255; the values of rex::ui::VirtualKey) or one of
// the mouse codes below. Games whose window gives other codes translate them first: SDL3 scancodes and mouse
// buttons with KeyFromSdlScancode / KeyFromSdlMouseButton (Big Red One, Finest Hour).
//
// Names are World at War's (Quake III's): B, SHIFT, CTRL, SPACE, UPARROW, MOUSE1, MWHEELUP ... Reading also takes
// the SDK's spellings (Shift, Control, Return, Up, LMB, MB4 ...) and SDL's (Left Shift, Return, Keypad 5 ...), in
// any case. No modifier combinations ("Shift+W"): World at War has none. A bare SHIFT, CTRL or ALT is an ordinary
// key (either side's).
//
// Plain code: no game, no SDK, no allocation except the std::string a name is returned in.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace inputkit {

using KeyCode = uint16_t;

inline constexpr KeyCode kNoKey = 0;
// Keyboard keys used by name in code (Windows virtual-key codes).
inline constexpr KeyCode kKeyBackspace = 0x08;
inline constexpr KeyCode kKeyTab = 0x09;
inline constexpr KeyCode kKeyEnter = 0x0D;
inline constexpr KeyCode kKeyShift = 0x10;
inline constexpr KeyCode kKeyCtrl = 0x11;
inline constexpr KeyCode kKeyAlt = 0x12;
inline constexpr KeyCode kKeyEscape = 0x1B;
inline constexpr KeyCode kKeySpace = 0x20;
inline constexpr KeyCode kKeyUp = 0x26;
inline constexpr KeyCode kKeyDown = 0x28;
inline constexpr KeyCode kKeyLeft = 0x25;
inline constexpr KeyCode kKeyRight = 0x27;
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

// A name -> its code (any case, any spelling above); kNoKey when unknown, empty, or a modifier combination.
KeyCode ParseKey(std::string_view name);

// The code's World at War name ("B", "SHIFT", "MOUSE4"); "" for none.
std::string KeyName(KeyCode code);

// As the games' prompts show it: the same, with the arrows as UP / DOWN / LEFT / RIGHT, ESCAPE as ESC and the
// keypad as NUM.
std::string PromptName(KeyCode code);

// True for a name with a modifier prefix ("Shift+Up", "Ctrl+W").
bool IsCombination(std::string_view name);

// Keys never bound to an action: ` / ~ (the console) and ESCAPE (the pause menu, and what cancels). A game's
// overlay keys (F2 ...) are checked by the game against its live settings.
bool IsAlwaysReserved(KeyCode code);

// SDL3's numbering (USB HID usage ids for scancodes; buttons 1 left, 2 middle, 3 right, 4 X1, 5 X2) -> a KeyCode;
// kNoKey for a key this table doesn't know. Taken as plain numbers so no SDL header is needed.
KeyCode KeyFromSdlScancode(int scancode);
KeyCode KeyFromSdlMouseButton(int button);

}  // namespace inputkit
