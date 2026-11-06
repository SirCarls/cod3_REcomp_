// cod3 - our own text layer for Russian, Chinese, Japanese and Korean. [acct2-ocr]
//
// The game's fonts are single-byte Latin (game/font_fill.h). For the languages
// that need other scripts, game/text_language.cpp hands the game its texts in
// UTF-8 and this draws them:
//   - the game lays a line out as it always does. While such a language is on,
//     its fonts' advances for the bytes 0x80-0xFF are set (in memory) so that a
//     UTF-8 character measures about as wide as we draw it: continuation bytes
//     (0x80-0xBF) 0, each lead byte the average advance of the characters it
//     starts (assets/text/fonts/overlay/<locale>.overlay.txt, made on the PC by
//     tools/overlay_fonts.py). Centering and wrapping fit; the originals come
//     back when the language changes;
//   - each line the game draws (nglListAddString: ultrawide.cpp's hook calls
//     OnString first) that holds such characters is noted with its place, size
//     and color in the game's 640x480 space, and the game draws it with alpha 0
//     (as the key caps do, button_prompts.cpp);
//   - HudOverlay draws the noted lines (Draw) with an ImGui font made from Noto
//     (OFL 1.1; a subset with only the characters our texts use, next to the
//     .overlay.txt), mapped like the key caps (the HUD area).
// The font is added to ImGui's atlas at start (the SDK uploads it once), so a
// change to or between these languages applies after a restart. Both programs.
#pragma once

#include <cstdint>
#include <filesystem>
#include <string_view>

#include <rex/ppc.h>

struct ImFont;
struct ImFontAtlas;

namespace cod3::text_overlay {

// Where the game's files are: assets/ (the overlay files are in text/fonts/overlay/).
void SetGameDataRoot(const std::filesystem::path& root);

// theme.cpp's ConfigureFonts: adds the chosen language's font when it needs this layer.
void AddFonts(ImFontAtlas* atlas);

// Whether this locale's overlay files are there (it can be chosen), and whether
// its font was added at start (its texts can be shown now).
bool Available(std::string_view locale);
bool Ready(std::string_view locale);

// [acct2-ocr] The locale's font file and its .overlay.txt em and ascent (px at the game's line height 40), for
// ui/text_native.*. False when the locale has none.
bool FontFile(std::string_view locale, std::filesystem::path* file, float* em, float* ascent);

// The font added at start (nullptr: none). For ui/film_text.* too.
ImFont* Font();

// nglListAddString (r3 font, r4 text, f1 x, f2 y, f4 x scale, f5 y scale, r8
// color), before the game draws it. Game thread.
void OnString(uint8_t* base, PPCContext& ctx);

// HudOverlay::OnDraw: the noted lines, over the HUD area (x, y, width, height in pixels).
void Draw(float x, float y, float width, float height);

}  // namespace cod3::text_overlay
