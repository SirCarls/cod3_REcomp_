// cod3 - PC controls for the game's own menus.
//
// The console menus (title screen, main menu, options, pause menu, message
// boxes) stay exactly the game's: the same pages, drawn by the game. On top
// of the controller they now take the mouse and the keyboard like a PC game:
//  * mouse: the item the pointer moves onto is selected (the game's own
//    highlight follows it), a left click activates it (A), a right click goes
//    back (B), the wheel moves up and down the list. On option rows (a label
//    with a value to its right) a click on the value side steps the value
//    left or right, depending on the half clicked.
//  * keyboard: arrow keys = D-pad, Enter = A (Start on the "Press Start"
//    screen), Escape and Backspace = B. In play Escape still opens the pause
//    menu (Escape is the Start bind there).
//  * mouse and keys together: a key, the wheel or the pad takes the
//    highlight; a pointer resting on an item doesn't take it back until the
//    mouse moves a few pixels or onto another item. An arrow key is a D-pad
//    press while down and its release when up, so a held arrow repeats with
//    the game's own repeat and stops when let go; every other press (wheel,
//    click, Enter) is released at once. (Before, the game never got the
//    release and repeated the direction for ever: the endless scrolling.)
// Each event goes through the menu system's own button handler, the same
// call a controller press ends in (addresses.h, namespace pc_menu), so every
// menu, sound and side effect is the game's. The OS cursor shows while a menu
// is up and hides in play. F2 stays the fallback for every setting.
//
// Where the pointer is: the window's pixels go to the picture's rectangle the
// way the presenter places it (any window size, bars, ultrawide), then to the
// game's 640x480 menu space. An item is where its letters are drawn: the
// label's position, alignment and scale (selected or not) and the font's own
// glyph sizes; a message box's buttons are the texts its button list draws.
// Each item can be pointed at across its row (half way to the next row);
// items side by side split their line half way. The footer prompts ("[ENTER]
// SELECT [ESC] BACK") are buttons as well: a click on one presses it.
//
// Also here: the "Select Storage Device" item of the pause menu becomes QUIT
// TO DESKTOP (under QUIT TO MENU) and the storage buttons of the game's
// message boxes are removed (the port saves into its userdata folder; there
// is no device to pick). The main menu's XBOX LIVE MARKETPLACE becomes QUIT
// TO TITLE SCREEN (what B does there: back to "Press Start") and a sixth row
// QUIT TO DESKTOP is added under it. QUIT TO DESKTOP asks in the game's own
// Yes / No message box, then saves the settings and ends the program like the
// window's close button.

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace rex::ui {
class Window;
}

namespace cod3::pcmenu {

// UI thread: start listening to the window's mouse and keyboard. overlay_open
// says when the console or the settings window has the input (then nothing
// is taken from them).
void Attach(rex::ui::Window* window, size_t z_order, std::function<bool()> overlay_open);
void Detach(rex::ui::Window* window);

// True while a game menu takes input (set by the game thread every frame a
// menu page is up). False when cod3_pc_menus is off.
bool MenuActive();

// UI thread, every frame while the game doesn't hold the mouse: shows the OS
// cursor over menus and overlays, hides it (until it moves) otherwise.
void UpdateCursor(rex::ui::Window* window);

// Cod3App: what QUIT TO DESKTOP (main menu or pause menu) does once
// confirmed (called on the game thread; save the settings and end the
// program).
void SetQuitToDesktopHandler(std::function<void()> quit);

// game/button_prompts.cpp, as the game draws a line of a multi-line text
// (game thread): footer prompts drawn by a menu become clickable.
void NoteLineDrawn(uint8_t* base, uint32_t line, double y_scale);

}  // namespace cod3::pcmenu
