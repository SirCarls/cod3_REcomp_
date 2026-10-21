// cod3 - the movement settings in the game's own Options menu.
//
// In game: Pause > Options gets a MOVEMENT OPTIONS row under VIDEO OPTIONS;
// it switches the page to six rows (game/menu_fov.cpp shows them the way it
// shows the video rows): SPRINT, SPRINT BUTTON, MOVEMENT STYLE, COD4 SPRINT,
// COD4 SPEEDS, COD4 JUMP TRICKS. Left and right change a row;
// B goes back to the options.
//
// The rows are game/movement/settings.h's, the same as F2 > Game > Movement:
// the values are read from the settings every time they are shown, so both
// always agree. A row the server sets in a match (or a CoD4 part while the
// style is Call of Duty 3) doesn't change, and the line under the rows says why.

#pragma once

#include <cstdint>
#include <string>

struct PPCContext;

namespace cod3::movementmenu {

// The rows, in the order the page shows them (settings.h's Row).
inline constexpr int kRows = 6;
int RowCount();

// String keys (resolved by Localize below through the game's Localize hook).
const char* LabelKey(int row);  // "SPRINT" ...
const char* TitleKey();         // "OPTIONS / MOVEMENT"
const char* PauseItemKey();     // the in-game row "MOVEMENT OPTIONS"

// The value a row shows now ("ON", "COD4" ...).
std::string Value(int row);
// The line under the rows for the selected row: why it can't change now
// ("" = it can).
std::string Note(int row);
// One step left or right (game thread); returns the new value's text, or ""
// when the row can't change now.
std::string Step(PPCContext& ctx, uint8_t* base, int row, bool right);

// The game's Localize hook (game/menu_fov.cpp) asks here: a guest string for
// one of the keys above, else 0.
uint32_t Localize(uint8_t* base, const char* key);

}  // namespace cod3::movementmenu
