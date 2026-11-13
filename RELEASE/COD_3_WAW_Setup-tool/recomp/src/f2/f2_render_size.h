// f2 - the size the IW3-engine games (Call of Duty 4, World at War) draw their 3D scene at. [f2-menus]
//
// Both games pick their sizes once, at start, in the function that reads the console's video mode (World at War
// campaign 0x823F9088, Call of Duty 4 campaign 0x82135318, multiplayer 0x8216CCD0; the same code in all three). It
// fills a record (r31) that the renderer and the present parameters are made from:
//   +0x00 byte  widescreen display              +0x01 byte  high-definition display
//   +0x08 u32   the 3D scene's width            +0x0C u32   its height
//               (1024 x 600 on a widescreen display, 896 x 680 on 4:3)
//   +0x10 u32   the screen's width              +0x14 u32   its height
//               (the display clamped to 640 x 480 .. 1280 x 720)
//   +0x18 u32   MSAA: 4 = 4x, 2 or 3 = 2x, below 2 = none (the games use 2)
// The 3D scene is drawn at the scene size with 2x MSAA (1024 x 600 x 2 just fits the console's 10 MB EDRAM) and
// stretched to the screen size; menus and the HUD are drawn at the screen size.
//
// game_draw_size "screen" draws the 3D scene at the screen size instead, so nothing is stretched. The hook
// (iw3_game_draw_size_hook, named by a [[midasm_hook]] block in each game's config/default.toml) runs at the store of
// the scene height (`stw r10,12(r31)`): r10 holds the height and +0x08 already has the width.

#pragma once

#include <cstdint>
#include <string>

struct PPCContext;

namespace f2 {

struct PageContext;

// The settings DrawGameDrawSizeRows shows (for a page's reset button).
extern const char* const kGameDrawSizeSettings[2];

// F2 rows: "Game's 3D size" (game_draw_size) and "Game's anti-aliasing" (game_draw_msaa). Both apply after a restart.
void DrawGameDrawSizeRows(PageContext& c);

// The hook's work (see above). `game` names the program in the log line.
void ApplyGameDrawSize(PPCContext& ctx, uint8_t* base, const char* game);

// What the last call chose, for F2: "1280 x 720 with 2x MSAA" etc. Empty before the game has picked its sizes.
std::string GameDrawSizeNote();

}  // namespace f2
