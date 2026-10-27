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
// A controller button press brings the controller's buttons back; a key or
// mouse press shows the keys again. Texts already on screen follow at once
// (they are laid out again). Only the player on the first controller can
// show keys (the keyboard and mouse are guest user 0); the others always see
// their controllers' buttons.
// cod3_glyph_style picks how controller buttons look: auto (the default;
// [mp-menus] each player's own controller, as the SDK names it, SDK patch
// 0067: an Xbox pad or one it can't name shows the game's own pictures, a
// PlayStation pad "[CROSS]", a Nintendo pad the label in that place, "[B]"
// for the bottom button), or one style for everyone: xbox (the game's own
// pictures), playstation, nintendo or gamecube (button names until pictures
// of our own exist; one data table, kButtons). cod3_key_prompts turns the
// keys off. Nothing else changes: no timing, no input, only what the
// prompts show.

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace rex::ui {
class Window;
}
namespace rex::input {
class InputSystem;
}

namespace cod3::prompts {

// UI thread: watch the window's keyboard and mouse (never takes an event).
void Attach(rex::ui::Window* window, size_t z_order);
void Detach(rex::ui::Window* window);
// [mp-menus] A keyboard or mouse use another listener made up (the PC
// menus' test script, cod3_test_menu_input): the prompts show keys.
void NoteKeyboardUse();
// [mp-menus] The app's input system (after setup; nullptr at shutdown): which
// kind of controller each player holds, for cod3_glyph_style auto.
void SetInputSystem(rex::input::InputSystem* input);
// [rumble] Whether the keyboard and mouse are in use now: mnk_mode on, and a
// key or mouse press since the last controller button (what the first
// controller's prompts follow, whatever cod3_key_prompts says). Any thread.
bool KeyboardMouseInUse();

// [acct2-decomp] 2026-10-25 (owner 23:42/23:43: the keys should look like the
// "SPACE Skip" key cap; that prompt should follow the controller too):

// What the first player's prompts show right now: keys, or a controller's
// buttons in one style (the same choice the game's prompts make: mnk_mode,
// cod3_key_prompts, the last device used, cod3_glyph_style). Any thread.
enum class Look : uint8_t { kKeys, kXbox, kPlayStation, kNintendo, kGameCube };
Look FirstPlayerLook();
// A read of the first controller's state outside the game's own pad reads
// (the cutscene skip reads it while the game's pad reads may not run): a
// button pressed there with no key or mouse press brings the controller's
// buttons back, as in play. Any thread.
void NotePadState(uint16_t buttons, uint8_t left_trigger, uint8_t right_trigger, const int16_t axes[4]);
// The keys the game's prompts show, as key caps: while keys are shown, each
// key name a text line draws ("[ENTER]", "[WASD]") is drawn invisibly by the
// game and noted here, in the game's 640x480 menu space, for the overlay to
// draw as a key cap like "SPACE Skip"'s (ui/hud_overlay.cpp). Entries are
// those drawn in the last `max_age_ms`. UI thread.
struct KeyCap {
  float x0, y0, x1, y1;  // menu space
  std::string name;      // without the brackets
  float alpha;           // the text's opacity as the game draws it (fades)
};
std::vector<KeyCap> CurrentKeyCaps(int max_age_ms);

}  // namespace cod3::prompts
