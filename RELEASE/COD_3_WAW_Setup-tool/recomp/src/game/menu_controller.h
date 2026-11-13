// cod3 - [acct2-ocr] 2026-11-07 the CONTROLLER page of the game's own Options menus (owner 22:22: "Please add a
// controller tab in every settings menu. These are controller related settings."; 22:24: "Make sure these
// controller options account for split-screen").
//
// Its rows (the same settings as F2 > Controls > Controller and the ~ console):
//   SENSITIVITY LIMIT      cod3_pad_sensitivity_max: how far the Horizontal and Vertical Sensitivity rows go,
//                          every player (game/pad_settings.h)
//   STICK DEAD ZONE        the player's own dead zone, cod3_deadzone_p1 .. _p4 (game/keybinds/pad_binds.cpp);
//                          at the game's own joy_threshold (every controller's) it is -1 again
//   VIBRATE WITH KEYBOARD  cod3_rumble_with_keys (game/rumble.cpp): the first controller's, so the campaign's
//                          page only
// The game's own Horizontal / Vertical Sensitivity, Invert Aim and Vibration stay where the game has them: the
// campaign's Options list, cod3mp's Pause > OPTIONS > CONTROLLER page (game/options_pages.cpp), which also gets
// the first two rows here for the player whose menu it is. A change applies and is saved at once.

#pragma once

#include <cstdint>
#include <string>

#include "game/menu_slider.h"

struct PPCContext;

namespace cod3::controllermenu {

enum Row : int { kLimit, kDeadZone, kRumbleKeys, kRowCount };
inline constexpr int kRows = kRowCount;
inline constexpr int kMultiplayerRows = 2;  // cod3mp: SENSITIVITY LIMIT and STICK DEAD ZONE

const char* LabelKey(int row);   // "FEMENU_COP_PAD_LIMIT" ...
const char* LabelText(int row);  // its English ("SENSITIVITY LIMIT")
const char* TitleKey();          // "OPTIONS / CONTROLLER"
const char* ButtonKey();         // the page button "CONTROLLER"

// For `player` (0-3; the campaign has player 1, 0).
std::string ValueFor(int player, int row);
std::string NoteFor(int player, int row);
// One step left or right (game thread); the new value's text, or "" when it can't change.
std::string StepFor(int player, int row, bool right);
// The rows with numbers (game/menu_slider.h): SENSITIVITY LIMIT and STICK DEAD ZONE.
bool NumberOfFor(int player, int row, menuslider::Number* n, double* value);
std::string SetNumberFor(int player, int row, double value);

// Player 1's, as the campaign's pages take them (game/menu_fov.cpp's SubPage).
std::string Value(int row);
std::string Note(int row);
std::string Step(PPCContext& ctx, uint8_t* base, int row, bool right);
bool NumberOf(int row, menuslider::Number* n, double* value);
std::string SetNumber(int row, double value);

// The game's Localize hook asks here: a guest string for one of the keys above, else 0.
uint32_t Localize(uint8_t* base, const char* key);

}  // namespace cod3::controllermenu
