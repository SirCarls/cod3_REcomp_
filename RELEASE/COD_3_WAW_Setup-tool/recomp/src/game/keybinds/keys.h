// cod3 - keyboard and mouse keys for the port's own key binds
// (game/keybinds/binds.h): codes and the names cod3.toml, F2, the console
// and the prompts use.
//
// [input-port] 2026-11-24: the codes, the names and their rules moved to the
// shared inputkit library (inputkit/include/inputkit/keys.h), which every
// game in the project uses; these are the same functions and values under
// CoD3's names, so nothing here changed for CoD3. Reading also takes SDL's
// spellings now ("Left Shift", "Keypad 5"), for Big Red One's settings.
//
// A KeyCode is a Windows virtual-key code for a keyboard key (the values of
// rex::ui::VirtualKey, 1-255) or one of the mouse codes. Names are World at
// War's (Quake III's): B, SHIFT, CTRL, SPACE, UPARROW, MOUSE1, MWHEELUP ...
// No modifier combinations ("Shift+W"); a bare SHIFT, CTRL or ALT is an
// ordinary key.
//
// Plain code: no game, no SDK (the unit test builds it on its own).

#pragma once

#include "inputkit/keys.h"

namespace cod3::binds {

using inputkit::KeyCode;

using inputkit::kNoKey;
using inputkit::kKeyTab;
using inputkit::kKeyEnter;
using inputkit::kKeyShift;
using inputkit::kKeyCtrl;
using inputkit::kKeyAlt;
using inputkit::kKeyEscape;
using inputkit::kKeyB;
using inputkit::kKeyF1;
using inputkit::kKeyBacktick;
using inputkit::kMouse1;
using inputkit::kMouse2;
using inputkit::kMouse3;
using inputkit::kMouse4;
using inputkit::kMouse5;
using inputkit::kWheelUp;
using inputkit::kWheelDown;
using inputkit::kKeyCodeCount;

using inputkit::ParseKey;          // a name -> its code; kNoKey when unknown or a combination
using inputkit::KeyName;           // "B", "SHIFT", "MOUSE4"; "" for none
using inputkit::PromptName;        // as the prompts show it (UP, NUM5 ...)
using inputkit::IsCombination;     // "Shift+Up"
using inputkit::IsAlwaysReserved;  // ` / ~ and ESCAPE

}  // namespace cod3::binds
