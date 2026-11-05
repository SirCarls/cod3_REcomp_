// cod3 - the language the game's voices speak (setting cod3_voice_language),
// from any Call of Duty 3 disc.
//
// The game code is the same on every disc and knows five voice languages
// (English, German, French, Spanish, Italian: game/voice_paths.h), but each
// disc carries only its own banks. The setup tool copies another disc's voice
// files into assets/ next to the game's own (c3w/voices.py, the Play page's
// "Add voices from another disc"); this setting then makes the game register
// those banks in place of its own:
//   - the levels' and the battle chatter's sound banks, when a level loads
//     (0x82478B80, hooked): the bank of the chosen language where assets/ has
//     it, else the game's own;
//   - the movies' sound tracks (movies/<name>-xx.wma): 0x824EED20 picks one by
//     the console language (XGetLanguage, called at 0x824EEDCC); the mid-asm
//     hook after that call (recomp/config/default.toml) answers with the
//     chosen language. The game falls back to English when a movie lacks it.
// The game's text and menus stay in the game's own language (0x82A2A150 is
// never changed). "disc" leaves everything as the game does it.
//
// A change: the films follow at once. The soldiers follow in the middle of the
// level ([acct2-decomp] 2026-11-04): on the game thread, each bank whose files
// differ for the new voice waits until nothing of the slots that change has
// played for two frames, then those slots are released, registered again and
// loaded again in the background, the way the game does it (no sound is cut;
// lines asked for in the moment between are not heard). The battle chatter
// changes within seconds. A bank that keeps playing (a level's own sounds in an
// English disc's whole bank), or whose new files need more sound memory than
// the old ones, waits for the next level load (starting or loading a mission):
// the banks of the archives that level opens are registered for the new
// voice, and every bank still registered for the old one is released,
// registered again and loaded again while the level loads (0x824631D0,
// hooked).
//
// Campaign only: the multiplayer program registers its banks in its own code
// (0x824964E8, language 0x82BCBE24: docs/X360_FAMILY.md).
#pragma once

#include <array>
#include <filesystem>

namespace cod3::voice {

// Where the game's files are (Cod3App::OnConfigurePaths): assets/.
void SetGameDataRoot(const std::filesystem::path& root);

// The chosen language, 0-4 (en de fr es it), or -1 for the game's own.
int Chosen();

// How many campaign speech banks of each language assets/ has (what the
// setting can choose from). Scanned at most every few seconds.
std::array<int, 5> Available();

// F2 > Game > Sound: the Voices row. Returns true when the player changed it
// (the caller saves the settings at once: the game restarts between missions).
bool DrawSettingsRow();

}  // namespace cod3::voice
