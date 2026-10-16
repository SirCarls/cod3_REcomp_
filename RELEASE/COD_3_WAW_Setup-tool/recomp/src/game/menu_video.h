// cod3 - the port's video settings in the game's own Options menus.
//
// Front end: Options gets a fifth item, VIDEO (the panel's unused fifth row,
// SP_OP_editprofile.panel text_option_05 with its row band and line, which
// the game hides). It opens the Options > Gameplay page in "video mode"
// (game/menu_fov.cpp): the same page, title "OPTIONS / VIDEO", its six rows
// re-labeled RESOLUTION, FRAME RATE LIMIT, V-SYNC, ANTI-ALIASING, SHADOWS and
// HD TEXTURES, with values, arrows, help text and left/right stepping that
// drive the port's settings. GAMEPLAY opens the page as before.
//
// In game: Pause > Options gets a VIDEO OPTIONS row above Reset to Defaults;
// it switches that page to the same six rows (B goes back to the options).
//
// The values come from the settings themselves every time they are shown
// and while a page is open, so F2, the ~ console and the menus always agree.
// What each row drives:
//   RESOLUTION        cod3_render_resolution (the same choices as F2 > Video;
//                     a live switch with cod3_live_resolution, the default)
//   FRAME RATE LIMIT  com_maxfps (a limit; never changes the game's speed),
//                     not while a multiplayer match runs
//   V-SYNC            vulkan_allow_present_mode_immediate /
//                     d3d12_allow_variable_refresh_rate_and_tearing (after a restart)
//   ANTI-ALIASING     swap_post_effect: off, FXAA, FXAA extreme
//   SHADOWS           cod3_shadow_quality: original, off (game/shadows.h)
//   HD TEXTURES       cod3_hd_textures (after a restart)
// All of them only change what is drawn and how often; none changes how the
// game plays.

#pragma once

#include <cstdint>
#include <functional>
#include <string>

struct PPCContext;

namespace cod3::videomenu {

// The rows, in the order the pages show them.
enum Row : int {
  kResolution,
  kFrameRate,
  kVsync,
  kAntiAliasing,
  kShadows,
  kHdTextures,
  kRowCount
};

// String keys (resolved by Localize below through the game's Localize hook).
const char* LabelKey(int row);         // "RESOLUTION" ...
const char* InstructionsKey(int row);  // the row's help text (front end)
const char* TitleKey();                // "OPTIONS / VIDEO" (page title)
const char* ItemKey();                 // the front end Options item "VIDEO"
const char* ItemInstructionsKey();     // its help text
const char* PauseItemKey();            // the in-game row "VIDEO OPTIONS"

// The value a row shows now ("1280 X 720", "ON" ...), read from the setting.
std::string Value(int row);

// A short note for the in-game page, where there is no help text: when the
// row's change waits for a restart, or can't be made now ("" = none).
std::string Note(int row);

// One step left or right on a row (game thread); returns the new value's
// text, or "" when the row can't change now.
std::string Step(PPCContext& ctx, uint8_t* base, int row, bool right);

// Remembers the settings the running game started with, to tell which rows
// wait for a restart. Called when the options pages are made (at start).
void NoteStartValues();

// The game's Localize hook (game/menu_fov.cpp) asks here first: a guest
// string for one of the keys above, else 0. Help texts are made when asked,
// so they say what applies now.
uint32_t Localize(uint8_t* base, const char* key);

// Cod3App: how to save the port's settings file after a change from a menu
// (it saves on the UI thread).
void SetSaveHandler(std::function<void()> save);

// game/menu_fov.cpp: the next time the Options > Gameplay page `page` opens
// it shows the video rows (true) or its own (false).
void SetGameplayPageMode(uint32_t page, bool video);

// game/menu_fov.cpp: Pause > Options opens with the game's options (the
// pause menu's Options item calls it before opening the page).
void ResetPauseOptions();

// The quit rows (game/pc_menu.cpp): the pause menu's "QUIT TO MENU", "QUIT
// TO DESKTOP" (also the main menu's), the main menu's "QUIT TO TITLE
// SCREEN", and the question QUIT TO DESKTOP asks (keys resolved by Localize
// like the ones above).
const char* QuitToMenuKey();
const char* QuitToDesktopKey();
const char* QuitToTitleKey();
const char* QuitToDesktopQuestionKey();

}  // namespace cod3::videomenu
