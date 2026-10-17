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
// never changed). "disc" leaves everything as the game does it. A change
// applies from the next level load (the game restarts between missions).
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
