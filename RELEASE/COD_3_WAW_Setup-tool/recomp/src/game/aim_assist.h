// cod3 - aim assist on/off (the game's "Sticky Aim"); see aim_assist.cpp.

#pragma once

#include <optional>

namespace cod3 {

// Whether aim assist is on right now; nullopt before the game has started.
std::optional<bool> AimAssistEnabled();
// Turns it on or off (from the F2 settings) and remembers the choice.
void SetAimAssist(bool on);

}  // namespace cod3
