// cod3 - [waw-decomp/menus2] 2026-11-23 Native menus: what the game thread laid out, for the overlay to draw
// (cod3_menukit.cpp publishes, cod3_menukit_draw.cpp draws). Internal to src/ui/menus/.
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "menukit/menukit.h"

namespace cod3::nativemenus {

// One menu system's page as laid out this frame, in the menus' 640x480 space of its player's view.
struct Shown {
  uint32_t system = 0;
  menukit::Look look = menukit::Look::kModern;
  std::vector<menukit::DrawItem> items;
  float part[4] = {0.0f, 0.0f, 1.0f, 1.0f};  // the player's view: x, y, width, height as shares of the picture
  int64_t ns = 0;                            // when (steady clock)
  std::string prompts;                       // the buttons line, in the chosen language
};

// Game thread: replaces that system's last layout.
void Publish(Shown shown);

}  // namespace cod3::nativemenus
