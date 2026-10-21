// cod3 - the AIM page of the game's own Options menu: sensitivity per weapon
// class and the custom crosshair (owner 2026-10-20: "will require submenu
// in-game and f2", "crosshair menu hasn't been added in-game yet", "One AIM
// page", "you can scale the menu down a little or completely re-organize
// them per function").
//
// In game: Pause > Options ends with one line of page buttons, VIDEO,
// MOVEMENT and AIM side by side, and RESET DEFAULTS under it
// (game/menu_fov.cpp places them; left and right move along the line). AIM
// switches the page to nine rows, shown the way the video and movement rows
// are, but with each value right after its label and a live preview of the
// crosshair at the top right (owner: "the menu selections should hug the left
// side and the display for it on the top right"):
//   CUSTOM CROSSHAIR     cod3_crosshair: the port's crosshair instead of the game's
//   CROSSHAIR STYLE      cod3_crosshair_style
//   CROSSHAIR LENGTH     cod3_crosshair_size
//   CROSSHAIR GAP        cod3_crosshair_gap
//   CROSSHAIR THICKNESS  cod3_crosshair_thickness
//   CROSSHAIR COLOR      AUTO, the game's own colors (cod3_crosshair_constant_color
//                        off), or one color (on, cod3_crosshair_color)
//   -- a gap --
//   SENSITIVITY FOR      which weapon class the next two rows set (not a
//                        setting; it opens at the class in the player's hands)
//     FROM THE HIP       that class's turning speed from the hip, 0-200%
//     AIMING DOWN SIGHTS the same while aiming down the sights
// The values are the same settings as F2 > Controls > Sensitivity per weapon
// and F2 > Crosshair, read every time they are shown, so all places agree.
// A change is saved at once. The outline, the dot and exact colors stay in F2.
// The five rows with numbers are sliders: arrow, gauge, arrow and a number box
// to type in (game/menu_slider.h).
//
// The campaign program for now (player 1's settings); cod3mp's Options pages
// are built differently (game/mp/options_mp.cpp) and get these rows per
// player next.

#pragma once

#include <cstdint>
#include <string>

#include "game/menu_slider.h"

struct PPCContext;

namespace cod3::aimmenu {

// Two groups, a gap apart (owner 2026-10-20 21:04: "the aim menu makes it seem
// like each weapon class has their own crosshair settings"): the crosshair,
// one for every weapon, next to its preview; then the sensitivity, which is
// per weapon class.
enum Row : int {
  kCrosshair,
  kStyle,
  kLength,
  kGap,
  kThickness,
  kColor,
  kWeaponClass,  // first row of the second group
  kHip,
  kAiming,
  kRowCount
};
inline constexpr int kRows = kRowCount;
inline constexpr int kSecondGroup = kWeaponClass;

// String keys (resolved by Localize below through the game's Localize hook).
const char* LabelKey(int row);   // "WEAPON CLASS" ...
const char* TitleKey();          // "OPTIONS / AIM"
const char* PauseItemKey();      // the page button "AIM"
// Shorter labels for the other two page buttons once the three share a line.
const char* VideoButtonKey();     // "VIDEO"
const char* MovementButtonKey();  // "MOVEMENT"

// The value a row shows now ("RIFLES", "100%", "ON" ...).
std::string Value(int row);
// The line under the rows for the selected row ("" = none).
std::string Note(int row);
// One step left or right (game thread); returns the new value's text, or ""
// when the row can't change now.
std::string Step(PPCContext& ctx, uint8_t* base, int row, bool right);

// The slider rows (game/menu_slider.h; owner 2026-10-20 21:09: "the sliders
// need to reflect the text value (this needs to be a textbox too)"): CROSSHAIR
// LENGTH, GAP and THICKNESS, FROM THE HIP and AIMING DOWN SIGHTS show a gauge
// and a number box. True for those rows, with the setting's range and its
// value now.
bool NumberOf(int row, menuslider::Number* n, double* value);
// A number typed into one of them (already within the range, on a step):
// sets and saves it; returns the value's text.
std::string SetNumber(int row, double value);

// game/menu_fov.cpp, every frame the AIM page draws: the crosshair preview's
// box at the page's top right, in the menu's 640x480 panel space (drawn by
// ui/hud_overlay.h's ShowCrosshairPreview, lined up with the menu at any
// window size or shape).
void ShowPreview(float x0, float y0, float x1, float y1);

// Game thread, as AIM opens: WEAPON CLASS starts at the class in the
// player's hands (when it can be read; else where it was left).
void OnOpen(uint8_t* base);

// The game's Localize hook (game/menu_fov.cpp) asks here: a guest string for
// one of the keys above, else 0.
uint32_t Localize(uint8_t* base, const char* key);

}  // namespace cod3::aimmenu
