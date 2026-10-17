// cod3 - keyboard and mouse keys in the game's button prompts.
//
// The game names controller buttons in its texts: menu footers ("(A) SELECT
// (B) BACK"), message boxes, in-game hints ("Press and hold (X) to swap for
// Kar98k"), battle actions ("Press (A) to swing."). They are pictures from
// the game's button font (addresses.h, namespace prompts). While the keyboard
// and mouse are in use (mnk_mode on and no controller button pressed since
// the last key or mouse press) each picture becomes the key that presses
// that button, in brackets and in the text's own font: "[ENTER] SELECT
// [ESC] BACK", "Press and hold [R] to swap for Kar98k".
//  * In play the key is the first key of the button's keybind_* setting
//    (keybind_x = "R,F" shows [R]), so rebinding changes the prompts. A
//    button the game binds to one of the port's own keyboard actions
//    (game/keybinds/binds.h) shows that action's key instead: the left stick
//    click, +binoculars, shows [B].
//  * In the menus (cod3_pc_menus, game/pc_menu.h) A is [ENTER], B [ESC],
//    START [ENTER] and the D-pad the arrow keys, the keys the PC menus take.
//  * Stick actions read "Use [WASD]" / "Use the mouse" (English), and the
//    game's "Pull" / "Click" verbs its own "Press"; the title says "PRESS
//    ENTER".
// A controller button press brings the game's own pictures back; a key or
// mouse press shows the keys again. Texts already on screen follow at once
// (they are laid out again). Only player 1 can show keys (the keyboard is
// guest user 0); players 2-4 always see controller buttons.
// cod3_glyph_style picks how controller buttons look: xbox (the game's own
// pictures), playstation or gamecube (button names, "[CROSS]", until
// pictures of our own exist; one data table, kButtons). cod3_key_prompts
// turns the keys off. Nothing else changes: no timing, no input, only what
// the prompts show.

#pragma once

#include <cstddef>

namespace rex::ui {
class Window;
}

namespace cod3::prompts {

// UI thread: watch the window's keyboard and mouse (never takes an event).
void Attach(rex::ui::Window* window, size_t z_order);
void Detach(rex::ui::Window* window);

}  // namespace cod3::prompts
