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
// [menus-brightness] The arrows and gauges drawn on an option row are part of
// it: a click on the left / right arrow steps the value that way, a click
// between the arrows steps it toward the half clicked, and a click on a
// gauge (volume, sensitivity, brightness) moves it to the place clicked and
// follows the pointer while the button is held. A page's own slider (Options
// > SCREEN) works the same way. They are found by the names of the pictures
// the menu draws, so the port's own rows work too.
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

struct PPCContext;

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

// [menus-brightness] Game thread, inside a page's left / right handler: true
// while the mouse / keyboard layer presses `code` (pm::kCode*) or holds it as
// an arrow key, so a page can tell those from the pad's.
bool FromPcInput(uint32_t code);

// --- [mp-menus] For a program's own menu hooks (game/mp/menus_mp.cpp) -----
// All on the game thread.

// After a menu system's ProcessInput: the mouse and keyboard for its page
// (what the ProcessInput hook in pc_menu.cpp does).
void OnProcessInput(PPCContext& ctx, uint8_t* base, uint32_t system);

// Around a menu system's draw: the footer prompts, arrows and gauges drawn
// inside belong to it. Begin returns the system drawing before (draws nest);
// pass it to End.
uint32_t BeginSystemDraw(uint32_t system);
void EndSystemDraw(uint32_t outer);

// A picture a menu drew (arrows and gauges are noted for the mouse).
void NotePictureDrawn(uint8_t* base, uint32_t quad);

// QUIT TO DESKTOP confirmed: the app's quit handler (save and end).
void QuitToDesktopNow();

// Panel elements, the way the game finds and copies them: a text or a
// picture of a panel by name (0 when it has none), a copy of an element, a
// copy kept `dy` below its source (position, opacity, shown), an item's label
// key, and (cod3_menu_trace) a log of a panel's elements.
uint32_t FindPanelText(PPCContext& ctx, uint8_t* base, uint32_t panel, const char* name);
uint32_t FindPanelPicture(PPCContext& ctx, uint8_t* base, uint32_t panel, const char* name);
uint32_t CopyPanelElement(PPCContext& ctx, uint8_t* base, uint32_t source, bool picture);
void FollowPanelElement(uint8_t* base, uint32_t source, uint32_t copy, bool picture, float dy);
void SetItemLabelKey(PPCContext& ctx, uint8_t* base, uint32_t entry, const char* key);
void DumpMenuPanel(uint8_t* base, uint32_t panel, const char* what);

}  // namespace cod3::pcmenu
