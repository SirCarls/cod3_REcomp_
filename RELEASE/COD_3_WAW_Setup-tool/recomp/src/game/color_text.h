// cod3 - World at War's ^0-^9 / ^: color codes inside any line the game draws. [acct2-ocr]
//
// Owner 2026-11-04 13:35: "The user name isn't showing the color via those codes." Player names keep their codes
// (game/color_codes.h SanitizeName, F2's name rows), but Call of Duty 3's own text drawing only colors a whole HUD
// line by a code at its very start (0x8242A1E0); a code in the middle of a line ("Killed by ^1Doug^5las") prints
// in the line's one color. So every line that reaches nglListAddString (ultrawide.cpp's hook, both programs) with
// a code in it is drawn here as runs instead: one game call per stretch of one color, each placed after the last
// by the font's own advances (FONT +0x38 records, advance at +24) times the line's x scale (f4, after ultrawide's
// mapping), the codes themselves not drawn; ^: gives each character the next rainbow color (color_codes.h). The
// line's alpha is kept, so fades still fade. A line without codes goes to the game unchanged.
//
// The run texts live in a guest ring (256 KB, never freed): the game may keep a string's pointer until the frame
// is drawn, and one frame's coded lines need a few KB.
//
// cod3_color_codes_in_text (default on) turns it off; cod3_debug_color_codes logs each coded line once.
// Unverified until a run: r8's byte order (taken as 0xAARRGGBB, as text_overlay reads it) and that the game's
// advance per glyph is exactly advance * f4.
#pragma once

#include <cstdint>

#include <rex/ppc.h>

namespace cod3::color_text {

using DrawFn = void (*)(PPCContext& ctx, uint8_t* base);

// In nglListAddString's hook, after its own changes to ctx and before the game's function: true when the line
// had color codes and was drawn here (call `draw` no more), false to draw it as it is.
bool DrawRuns(uint8_t* base, PPCContext& ctx, DrawFn draw);

}  // namespace cod3::color_text
