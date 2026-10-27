// cod3 - the player's movement settings and the rows that show them.
//
// The player's own settings (cod3.toml, category cod3/Movement):
//   cod3_sprint          on/off   sprint in the campaign (multiplayer always has it)
//   cod3_sprint_toggle   hold/toggle  how the sprint button works
//   cod3_cod4_movement   off/on   the player's movement style: Call of Duty 3 / Call of Duty 4
//   cod3_cod4_sprint, cod3_cod4_speeds, cod3_cod4_jumps
//                        on/off   the three parts of the CoD4 style (only with cod4)
//   cod3_move_hz         0..240   movement steps a second at any frame rate (60) [acct2-main]
// The match's settings are server_sided_protected (game/protected_dvars.h):
// alternate_movement and alternate_movement_sprint/_speeds/_jumps.
//
// The same rows (labels, values, steps, locks) are shown by F2 > Game >
// Movement and by the game's Options menus (game/movement/menu_movement.h),
// read from the settings each time they are drawn, so both always agree.

#pragma once

#include <string>

#include "game/movement/rules.h"
#include "game/movement/sprint_input.h"

namespace cod3::movement {

// --- the player's own settings (any thread) --------------------------------------
bool SprintSetting();        // cod3_sprint
SprintMode SprintButtonMode();  // cod3_sprint_toggle
Preference OwnPreference();  // cod3_cod4_movement + cod3_cod4_*
bool DebugMovement();        // cod3_debug_movement
int MoveStepHz();            // cod3_move_hz (0 = one move a frame) [acct2-main]

// The rules for this machine's player now: the campaign is its own server,
// so these are what its movement follows (alternate_movement 2 by default:
// the player's own choice).
MoveRules LocalRules();

// --- the rows (F2 and the game's menus) ------------------------------------------
enum Row : int {
  kRowSprint,        // SPRINT                 ON / OFF
  kRowSprintButton,  // SPRINT BUTTON          HOLD / TOGGLE
  kRowStyle,         // MOVEMENT STYLE         COD3 / COD4
  kRowCod4Sprint,    // COD4 SPRINT            ON / OFF
  kRowCod4Speeds,    // COD4 SPEEDS            ON / OFF
  kRowCod4Jumps,     // COD4 JUMP TRICKS       ON / OFF
  kRowCount
};
const char* RowLabel(int row);        // upper case, as the game's menus
const char* RowHelp(int row);         // one sentence
std::string RowValue(int row);        // the value shown now ("ON", "COD4" ...)
// Why the row can't change now ("" = it can): a match whose server sets it,
// or a part while the style is Call of Duty 3.
std::string RowLock(int row);
// One step left or right. Returns false when the row is locked. Saves the
// settings (through the protected dvars' save handler).
bool StepRow(int row, bool right);

// --- F2 > Game (ui/hud_overlay.cpp) ----------------------------------------------
// Draws the Movement section; true when a setting changed (F2 then saves).
bool DrawF2Section();

}  // namespace cod3::movement
