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
//   BATTLE ACTIONS    cod3_qte_style: spinner or original (game/qte_actions.h);
//                     the in-game page only (the front end's has six rows)
//   DEPTH OF FIELD    [post-effects] cod3_depth_of_field: on (the game's own) or
//                     off (game/post_effects.h); the in-game page only
//   MOTION BLUR       [post-effects] cod3_motion_blur: on or off; the in-game
//                     page only
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
  // Stick battle actions: spinner or the game's arrow (game/qte_actions.h).
  // Only the in-game page has room for this seventh row; the front end's
  // page shows the first six.
  kBattleActions,
  // [post-effects] The game's depth of field and motion blur on or off
  // (game/post_effects.h); the in-game page only, like kBattleActions.
  kDepthOfField,
  kMotionBlur,
  kRowCount
};

// String keys (resolved by Localize below through the game's Localize hook).
// [menus-categories] The rows (the enum above, these two keys, LabelText,
// HelpText, Value, Note, Step, NoteStartValues, SetSaveHandler) are
// menu_video_rows.cpp's, built into both programs; the rest is the campaign's
// (menu_video.cpp).
const char* LabelKey(int row);         // "RESOLUTION" ...
const char* InstructionsKey(int row);  // the row's help text (front end)
const char* LabelText(int row);        // the label's English text, "RESOLUTION" ...
std::string HelpText(int row);         // the row's help text as it applies now
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

// Cod3App (and [menus-categories] Cod3MpApp): how to save the port's
// settings file after a change from a menu (it saves on the UI thread).
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

// [menus-brightness] The game's own brightness (addresses.h, brightness): the
// one value the main menu's Options > SCREEN page changes. The BRIGHTNESS row
// of Pause > Options > VIDEO OPTIONS (menu_fov.cpp) shows and changes the
// same value, so the last
// change made anywhere wins and both places show it. A change shows at the
// next frame (the game rebuilds its gamma ramp whenever the value changes)
// and is saved with the player's options like the game saves them.
float Brightness(uint8_t* base);                  // 0.5 .. 2.0, 1.0 by default
float BrightnessFraction(uint8_t* base);          // 0 .. 1 along that range
void SetBrightnessFraction(uint8_t* base, float fraction);
// One step left or right, the SCREEN page's D-pad step (0.05); `from` names
// the place for the log.
void StepBrightness(uint8_t* base, bool right, const char* from);
// The brightness in the player's options (what is saved): sets it to `value`
// and returns the one it had (menu_fov.cpp: the pause menu's Options swap
// their values with the player's options, the way the game does).
float ExchangeProfileBrightness(uint8_t* base, float value);
const char* BrightnessKey();                      // the row's label "BRIGHTNESS"

// [menus-brightness] pc_menu.cpp, a click or drag on a gauge: sets the
// slider of `page` (row `item`, or -1 for a page's own slider) to `fraction`
// of its range directly. True for the game's brightness (the SCREEN page and
// the pause menu's BRIGHTNESS row); false for any other gauge, which
// pc_menu.cpp steps with left / right instead. Game thread.
bool SetSlider(uint8_t* base, uint32_t page, int item, float fraction);

}  // namespace cod3::videomenu
