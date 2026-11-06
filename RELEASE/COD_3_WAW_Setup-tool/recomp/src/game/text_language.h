// cod3 - the language of the game's text (setting cod3_text_language),
// switchable while the game runs, whatever the console language or the disc.
// [acct2-ocr]
//
// Every text the game shows from its string tables ends in one lookup by key
// hash, 0x82527FD0 (r3 the tables, r4 the hash: h * 33 + each lowercase
// character of the key). Localize (0x8252CB98) gets there for named keys, the
// level scripts (objectives, subtitles, hints) with the hashes they carry.
// In the campaign that lookup is hooked once, in game/button_prompts.cpp; its
// [acct2-ocr] line asks GuestTextFor() first, and the game's own text is used when
// we have none. In cod3mp it is hooked at the end of text_language.cpp.
//
// Where the texts come from: one file per language in the game's data folder,
// assets/text/<locale>.txt (UTF-8). A row is "KEY<tab>TEXT", as in
// config/prompt_text_kbm.tsv: KEY is the key hash as 8 hex digits or the key's
// name; TEXT keeps the game's tokens ("[ACTION]", "~button", "^1" colors) and
// writes a new line as "\n". Lines starting with '#' are comments. The files are
// made on the player's PC (official texts from the player's own discs, the rest
// our translations); nothing from a disc is published.
//
// The game's fonts are single-byte (Windows-1252 order, 224 glyphs from 0x20).
// Latin languages are converted to that. The characters the fonts lack
// („ ‘ ’ “ – « » ª º Ã Õ) are added to them by game/font_fill.*; without its files
// they get plain stand-ins (" for «). … is always written as three dots. Russian, Chinese, Japanese and Korean stay UTF-8 and are drawn by our
// own text layer (ui/text_overlay.*); its font is added at start, so a change to
// or between them applies after a restart (until then the game's own text stays).
//
// Kinds [acct2-ocr]: a pack's rows are grouped under "@menus", "@prompts",
// "@objectives", "@subtitles", "@loading" and "@credits" lines (rows before any
// such line are menus). Each kind has a setting (cod3_text_<kind>, default on;
// F2 > Sound > Text by kind); a kind that is off keeps the game's own English.
// Forced text (cod3_text_film_cards: the films' burned-in title cards and
// legal screens, rows under "@films", keys FILMCARD_<FILM>_<n>) is shown
// whatever those settings; its layer is still to come.
//
// A change: texts the game asks for from then on are in the new language
// (prompts, hints, message boxes, a menu page opened again). Texts a page or a
// level already holds stay until the game asks again: the next menu page, the
// next level load. Both programs: cod3mp's lookup is 0x82548B68 (44 of its 45
// instructions are the campaign's; Localize 0x8254D180 -> 0x8254B2B8 -> it), hooked
// in text_language.cpp, since nothing else in cod3mp hooks it.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>

namespace cod3::text {

// Where the game's files are (Cod3App::OnConfigurePaths): assets/.
void SetGameDataRoot(const std::filesystem::path& root);

// The chosen locale ("fr-FR"), or "" for the game's own text.
std::string Chosen();

// Bumped each time the chosen text language changes or its file is read
// again; code that caches the game's words keeps its cache per value.
uint32_t Generation();

// The guest address of our text for a key hash in the chosen language, or 0
// when there is none (the game's own text stays). The address stays valid for
// the rest of the run. Any thread.
uint32_t GuestTextFor(uint8_t* base, uint32_t key_hash);

// [acct2-ocr] The string tables' lookup by hash (SP 0x82527FD0, MP 0x82548B68)
// answers an entry record whose +8 is the text, not the text: its callers read
// lwz r3,8(r3) ("NO STRING" when that is 0). A guest copy of `original_entry`
// (0 when the game has none) with +8 set to our text for the key, or 0 when
// there is none. Cached per key; stays valid for the rest of the run.
uint32_t GuestEntryFor(uint8_t* base, uint32_t key_hash, uint32_t original_entry);

// [acct2-ocr] The port's own menu texts (the FEMENU_* and COD3_* keys the recomp's
// menus add, and the MPFRONTEND_* items it rewords, answered by its Localize
// hooks in game/menu_fov.cpp and game/mp/menus_mp.cpp):
// the guest address of the chosen language's text for `key` when the pack has
// it in its "@port" rows, else 0 (the port's English stays). Any thread.
uint32_t PortText(uint8_t* base, const char* key);

// The game's key hash of a key name (0x820A0118).
uint32_t KeyHash(std::string_view key);

// [acct2-ocr] For the films' layer (ui/film_text.h): a FILMCARD_<FILM>_<n> row of
// the chosen language in UTF-8, or "" when the film's English stays (the game's
// own text chosen, no such row, cod3_text_film_cards off, or a script whose font
// wasn't added at start). Any thread.
std::string FilmText(std::string_view key);

// assets/ (SetGameDataRoot), for the other text files there.
std::filesystem::path GameDataRoot();

// A button-verb text ("{b} drücken": {b} is where the button goes) is handed
// to the game without {b} ("drücken"); the game then writes its button after
// the verb ("drücken ~a"). For an [ACTION] word's sentence (r3 after
// 0x824DD9A8, button_prompts.cpp): the sentence with the button where the
// chosen language's text put {b} ("~a drücken"), or "" when nothing moves.
std::string PlaceButton(std::string_view sentence);

// Chinese and Japanese have no spaces, and the game breaks lines only at
// spaces: a space between two CJK characters where a line may break (not
// before closing punctuation or small kana, not after an opening bracket).
// ui/text_overlay.* doesn't draw these spaces and takes their width off the
// characters' advances, so the layout doesn't grow. Applied to the zh-* and
// ja-JP texts when they are read.
std::string WithLineBreaks(std::string_view utf8);
bool IsCjk(char32_t cp);

// [acct2-ocr] A UTF-8 text of ours in the chosen language (button_prompts.cpp's keyboard-and-mouse rows) as the
// chosen language's own rows are converted: the fonts' bytes, or UTF-8 for the text layer's scripts (Chinese and
// Japanese with WithLineBreaks); "" when the game's own text is chosen or the text layer has no font for it.
std::string ForChosenFonts(std::string_view utf8);

// [acct2-ocr] 2026-11-05 Texts the game draws without asking its string tables (the boot "Loading ...", the levels'
// intro cards, the name tags, the exe's names): a pack's rows under "@raw" or "@raw:<kind>" are "ENGLISH<tab>TEXT",
// ENGLISH the whole line as the English game draws it (UTF-8, "\n" for a new line). For the line at guest address
// `text` (nglListAddString's r4, ultrawide.cpp; the line measure, ui/text_native.cpp): the guest address of ours
// when the whole line is such an English text (a "^N" color code in front stays in front), else 0. A row
// "Pvt. *<tab>Рядовой *" answers a rank and one capitalized name no row has ("Pvt. Hale" -> "Рядовой Hale"). The address
// stays valid for the rest of the run. Any thread.
uint32_t RawTextFor(uint8_t* base, uint32_t text);

// UTF-8 -> the game fonts' single-byte order. Characters the fonts can't show
// become a stand-in (with_fill: the glyphs font_fill adds are kept) or '?'.
std::string ToGameFontBytes(std::string_view utf8, bool with_fill);

// F2 > Sound, under Voices: the Text row. Returns true when the player changed
// it (the caller saves the settings at once).
bool DrawSettingsRow();

}  // namespace cod3::text
