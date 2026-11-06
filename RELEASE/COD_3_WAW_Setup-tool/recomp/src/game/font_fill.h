// cod3 - the glyphs the game's fonts lack, added to them in memory. [acct2-ocr]
//
// The game's three text fonts (i_helvetica_bold, i_helvetica_bold_italic,
// garamond: 224 glyphs each, Windows-1252 order from 0x20, one DXT5 atlas of
// 512x256 each) have no „ ‘ ’ “ – « » ª º Ã Õ (and no …), which French,
// German, Italian, Spanish and Portuguese need. The setup work on the player's
// PC makes them from each font's own glyphs and finds room for them in free
// 4x4 blocks of the font's own atlas (and in the cells of characters no text
// uses): assets/text/fonts/<font>.fill.txt (made from the disc's fonts, so never
// published; format in that file's header and in tools/font_fill_pack.py).
//
// The first time the game draws a string with a font (nglListAddString,
// campaign 0x82134F70, cod3mp 0x8214CE30: game/ultrawide.cpp hooks it and calls
// OnFontUsed first), before that string is drawn:
//   - the font is recognized by its line height, ascent and the record of 'W'
//     (a font no file matches, like the button pictures, is left alone);
//   - each new glyph's pixels are written into the atlas (DXT5 blocks, alpha
//     only, Xenos tiled, 16-bit swapped, as the disc has it), through the
//     texture's guest address so the GPU's texture cache sees the change;
//   - its glyph record (the FONT's +8 points at them: x, y, w, h, x_off, y_off,
//     advance) and UVs (+0x2C points at them; 0..1 of the atlas) are set; an
//     "alias" copies another glyph's record;
//   - the atlas is then sampled from its top mip level only (the new glyphs are
//     in that level alone), which also makes the GPU take the texture again.
// [acct2-ocr] 2026-11-05 The atlas TEX is the FONT's +4 (until then it was looked
// for at +0x0C..0x14 only and no font was ever filled). A font the game loads
// again (its first added glyph's record no longer ours) is filled again.
// Nothing on the disc or in the files on it changes. Both programs: the fonts
// and their layout are the same in cod3 and cod3mp, and nothing here depends on
// a code address.
#pragma once

#include <cstdint>
#include <filesystem>

namespace cod3::font_fill {

// Where the game's files are (Cod3App::OnConfigurePaths): assets/.
void SetGameDataRoot(const std::filesystem::path& root);

// True while the added glyphs can be used: the fill files are there and no font
// they name failed its check. The text pass (game/text_language.cpp) keeps
// stand-ins (" for «) when false.
bool Ready();

// Bumped when Ready() changes (the text pass converts its texts again).
uint32_t Generation();

// nglListAddString's font (r3): fills it the first time it is seen. Any thread.
void OnFontUsed(uint8_t* base, uint32_t font);

}  // namespace cod3::font_fill
