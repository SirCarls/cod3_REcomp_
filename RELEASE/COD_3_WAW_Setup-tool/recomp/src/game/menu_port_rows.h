// cod3 - [acct2-ocr] 2026-11-07 The port's own settings as rows of the game's in-game Options pages (owner 23:17:
// "keep going. Option menus need more options. We can edit a lot"), in both programs:
//  * the campaign's Pause > Options pages (game/menu_fov.cpp): GAMEPLAY has nine of these rows after BATTLE
//    ACTIONS, CONTROLLER seven after its own three (kCampaignGameplay and kCampaignController below; ten rows fill
//    a page, as AIM's ten do);
//  * cod3mp's Pause > OPTIONS categories (game/options_pages.cpp, game/mp/options_mp.cpp): more rows on GAMEPLAY
//    and AIM, and two more categories, AUDIO and INTERFACE.
// Each row is one of the port's settings, the same one F2 and the ~ console have (docs/F2_MENU.md), applied and
// saved at once like F2 (protected_dvars::SaveSettings).
//
// Split screen (owner 22:24 "Make sure these controller options account for split-screen", 22:48 "This per player
// thing needs to be for all the other decomps too"): a row that is a player's own follows the menu's player in
// cod3mp: LEFT-HANDED WEAPON (cod3_viewmodel_flip, _2 .. _4), VEHICLE FOV SCALE (cod3_vehicle_fov_scale and
// cod3mp's cod3_vehicle_fov_scale_p2 .. _p4, game/fov.h) and USE HOLD TIME (cod3_use_hold_ms and cod3mp's
// cod3_use_hold_ms_p2 .. _p4, game/use_hold.h; owner 23:40 "it's per person"). The mouse rows are the keyboard and
// mouse player's (the local client on controller 0): the other players' menus show them but don't change them. The
// rest is the machine's (one sound, one text language, the prompts' pictures: "auto" is each player's own
// controller). A row a program doesn't have (the campaign's cutscene rows, cod3mp's split-screen HUD) isn't on the
// other's pages.
//
// The rows' texts: labels by key (COD3_PORT_*), answered by the Localize hooks (a pack's "@port" row in the chosen
// language first, game/text_language.h PortText; else the English here); values and notes in English.

#pragma once

#include <cstdint>
#include <string>

#include "game/menu_slider.h"

namespace cod3::portrows {

enum Row : int {
  kSkipCutscenes,     // SKIP CUTSCENES        cod3_skip_movies (campaign)
  kSkipHold,          // HOLD TO SKIP          cod3_skip_hold_ms (campaign)
  kCutsceneBars,      // CUTSCENE BARS         cod3_letterbox (campaign)
  kUseHold,           // USE HOLD TIME         cod3_use_hold_ms (cod3mp: _p2 .. _p4, the player's own)
  kLeftHanded,        // LEFT-HANDED WEAPON    cod3_viewmodel_flip, _2 .. _4 (the player's own)
  kVehicleFov,        // VEHICLE FOV SCALE     cod3_vehicle_fov_scale (cod3mp: _p2 .. _p4, the player's own)
  kTextLanguage,      // TEXT LANGUAGE         cod3_text_language (game/text_language.h)
  kVoices,            // VOICES                cod3_voice_language (game/voice_language.h)
  kVolume,            // MASTER VOLUME         cod3_volume
  kKeyPrompts,        // KEYS IN PROMPTS       cod3_key_prompts
  kKeyCaps,           // KEYS AS KEY CAPS      cod3_key_caps
  kButtonStyle,       // BUTTON PICTURES       cod3_glyph_style
  kButtonColors,      // BUTTON COLORS         cod3_glyph_color_mode
  kRawMouse,          // RAW MOUSE             cod3_raw_mouse (the keyboard and mouse player's)
  kInvertMouse,       // INVERT MOUSE          cod3_invert_mouse (the keyboard and mouse player's)
  kMouseSensitivity,  // MOUSE SENSITIVITY     cod3_sensitivity (cod3mp; the campaign's is on AIM)
  kMouseAds,          // MOUSE ADS SCALE       cod3_zoom_sensitivity (the keyboard and mouse player's)
  kMouseSmoothing,    // EVEN MOUSE TURNING    cod3_mouse_resample (the keyboard and mouse player's)
  kColorCodes,        // COLORS IN NAMES       cod3_color_codes_in_text
  kShowVersion,       // SHOW VERSION          cod3_show_version
  kSplitHud,          // SPLIT-SCREEN HUD      cod3_hud_safe_area (cod3mp)
  kRowCount
};

// The campaign's pages (game/menu_fov.cpp): GAMEPLAY's rows after BATTLE ACTIONS, CONTROLLER's after
// game/menu_controller.h's three.
inline constexpr int kCampaignGameplay[] = {kSkipCutscenes, kSkipHold,    kCutsceneBars, kUseHold, kLeftHanded,
                                            kVehicleFov,    kTextLanguage, kVoices,      kVolume};
inline constexpr int kCampaignController[] = {kKeyPrompts, kKeyCaps,  kButtonStyle,   kRawMouse,
                                              kInvertMouse, kMouseAds, kMouseSmoothing};

// Whether this program has the row.
bool Available(int row);

const char* LabelKey(int row);   // "COD3_PORT_SKIP_CUTSCENES" ...
const char* LabelText(int row);  // its English ("SKIP CUTSCENES")
// The label as cod3mp's rows show it (they are set as text): the chosen language's when its pack has a "@port"
// row for the key, else the English.
std::string LabelShown(uint8_t* base, int row);

// For `player` (0-3; the campaign has player 1, 0).
std::string ValueFor(int player, int row);
std::string NoteFor(int player, int row);
// One step left or right (game thread); the new value's text, or "" when it doesn't change.
std::string StepFor(int player, int row, bool right);
// The rows with numbers (sliders, game/menu_slider.h): HOLD TO SKIP, VEHICLE FOV SCALE, MASTER VOLUME, MOUSE
// SENSITIVITY, MOUSE ADS SCALE.
bool NumberOfFor(int player, int row, menuslider::Number* n, double* value);
std::string SetNumberFor(int player, int row, double value);

// The game's Localize hook asks here: a guest string for one of the keys above, else 0.
uint32_t Localize(uint8_t* base, const char* key);

}  // namespace cod3::portrows
