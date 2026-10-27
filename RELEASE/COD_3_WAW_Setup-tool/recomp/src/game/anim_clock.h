// cod3 - [claude-9b] animation clocks that ran with the frame rate (anim_clock.cpp), both programs.
#pragma once

#include <cstdint>

namespace cod3::anim_clock {

// After BeginNextFrame (0x82130808, cod3mp 0x82143838) copied the vblank count into its snapshot: the fallback
// animation clock in real time (cod3_fix_ui_clock), and the debug log (cod3_debug_anim_clock).
void AfterBeginNextFrame(uint8_t* base);

}  // namespace cod3::anim_clock
