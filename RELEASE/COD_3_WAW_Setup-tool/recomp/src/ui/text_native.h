// cod3 - Russian, Chinese, Japanese and Korean drawn through the game's own text path. [acct2-ocr]
//
// Owner 2026-11-05 06:23: the Big Red One chat draws its menus natively (bro/src/ngl/ngl_font.c); the same
// here. The game's text path is single-byte: a FONT has 224 glyph records (first code 0x20) and one atlas
// texture (dev/agents/acct2-ocr/NOTES.md 06:40 has the layouts). A line holding our characters is drawn as
// one game call (nglListAddString) per run of characters that sit on the same "page": a FONT of ours, made
// from the game font's own header, whose records, UVs and 512x256 DXT5 atlas hold up to 223 of our glyphs
// (rendered from the Noto subset in assets/text/fonts/overlay/, OFL 1.1), and the run re-coded as that page's
// bytes. The game then lays out, sorts (z), fades, scales for widescreen and draws them like its own text.
// Pages and glyphs are made when first needed; a language change needs no restart.
//
// Measuring (campaign): 0x82134B90 (font, text, f1 x scale, f2 y scale, r5 int* width, r6 int* height) is
// answered here for such lines, from the same advances, so the game centers and wraps them exactly.
//
// cod3_text_native (default on); off: ui/text_overlay.* draws these lines on top as before.
#pragma once

#include <cstdint>
#include <string_view>

#include <rex/ppc.h>

namespace cod3::text_native {

// Whether lines in this locale are drawn here (the setting is on and the locale's font file is there).
bool Active(std::string_view locale);

using DrawFn = void (*)(PPCContext& ctx, uint8_t* base);
// In nglListAddString's hook (game/ultrawide.cpp), after its own changes to ctx: true when the line held our
// characters and was drawn here as runs through `draw` (call it no more), false to draw it as it is.
bool DrawLine(uint8_t* base, PPCContext& ctx, DrawFn draw);

}  // namespace cod3::text_native
