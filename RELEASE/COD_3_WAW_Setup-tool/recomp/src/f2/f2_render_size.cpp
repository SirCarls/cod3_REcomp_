// f2 - the size the IW3-engine games draw their 3D scene at (see f2_render_size.h). [f2-menus]

#include "f2/f2_render_size.h"

#include <cstdio>
#include <mutex>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include "f2/f2_rows.h"
#include "f2/f2_window.h"

REXCVAR_DEFINE_STRING(game_draw_size, "console", "Display",
    "The size the game draws its 3D scene at: console (1024x600, the Xbox 360's, stretched to the screen) or "
    "screen (the screen's size the game uses, 1280x720: nothing stretched). Applies after a restart");
REXCVAR_DEFINE_BOOL(game_draw_msaa, true, "Display",
    "The game's own 2x MSAA. Left out by itself when the 3D size with it doesn't fit the game's own EDRAM layout "
    "(1280x720 doesn't). Applies after a restart");

namespace f2 {

const char* const kGameDrawSizeSettings[2] = {"game_draw_size", "game_draw_msaa"};

namespace {

std::mutex g_note_mutex;
std::string g_note;

uint32_t Load32(const uint8_t* base, uint32_t address) {
  const uint8_t* p = base + address;
  return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  uint8_t* p = base + address;
  p[0] = uint8_t(value >> 24);
  p[1] = uint8_t(value >> 16);
  p[2] = uint8_t(value >> 8);
  p[3] = uint8_t(value);
}

// The record's MSAA field as the present parameters read it (0x823F8AD8 in World at War's campaign): 4 = 4x,
// 2 or 3 = 2x, below 2 = none.
uint32_t Samples(uint32_t msaa_field) { return msaa_field >= 4 ? 4 : msaa_field >= 2 ? 2 : 1; }

// EDRAM tiles of one 32-bit surface: a tile is 80 x 16 samples; 2x MSAA doubles the rows, 4x the columns too.
uint32_t SurfaceTiles(uint32_t width, uint32_t height, uint32_t samples) {
  const uint32_t sx = samples >= 4 ? 2 : 1, sy = samples >= 2 ? 2 : 1;
  return ((width * sx + 79) / 80) * ((height * sy + 15) / 16);
}

// The game lays out its EDRAM for its own 3D size: its color and depth surfaces sit where 1024 x 600 with 2x MSAA
// (975 tiles each) leaves them, whatever the emulated EDRAM holds. A bigger surface runs into the next one: in
// Call of Duty 4, 1280 x 720 with 2x MSAA (1440 tiles) drew the lower half of the level in one flat colour
// (f2-menus cod4res1 d2/d3, 2026-11-09). So a size is used only when each surface takes no more tiles than the
// game's own; 1280 x 720 without MSAA takes 720.
bool Fits(uint32_t w, uint32_t h, uint32_t samples, uint32_t own_tiles) {
  return SurfaceTiles(w, h, samples) <= own_tiles;
}

std::string Describe(uint32_t w, uint32_t h, uint32_t samples) {
  char text[64];
  std::snprintf(text, sizeof(text), "%u x %u%s", w, h,
                samples >= 4 ? " with 4x MSAA" : samples >= 2 ? " with 2x MSAA" : "");
  return text;
}

}  // namespace

void ApplyGameDrawSize(PPCContext& ctx, uint8_t* base, const char* game) {
  const uint32_t record = ctx.r31.u32;
  const uint32_t game_w = Load32(base, record + 0x08), game_h = ctx.r10.u32;
  const uint32_t screen_w = Load32(base, record + 0x10), screen_h = Load32(base, record + 0x14);
  const uint32_t game_msaa = Load32(base, record + 0x18);
  const uint32_t tiles = SurfaceTiles(game_w, game_h, Samples(game_msaa));

  const std::string choice = REXCVAR_GET(game_draw_size);
  uint32_t w = game_w, h = game_h, msaa = game_msaa;
  std::string why;
  if (choice == "screen" && screen_w >= 640 && screen_h >= 480) {
    w = screen_w;
    h = screen_h;
  } else if (choice != "console" && !choice.empty()) {
    why = " (game_draw_size \"" + choice + "\" isn't console or screen)";
  }
  if (!REXCVAR_GET(game_draw_msaa) && Samples(msaa) > 1) {
    msaa = 0;
    why += " (MSAA off: game_draw_msaa)";
  }
  if (!Fits(w, h, Samples(msaa), tiles) && Samples(msaa) > 1) {
    msaa = 0;
    why += " (MSAA off: with it the surfaces don't fit the game's EDRAM layout)";
  }
  if (!Fits(w, h, Samples(msaa), tiles)) {
    w = game_w;
    h = game_h;
    msaa = game_msaa;
    why += " (the game's own size: the other doesn't fit the game's EDRAM layout)";
  }

  Store32(base, record + 0x08, w);
  ctx.r10.u64 = h;  // the instruction the hook runs before stores it at +0x0C
  if (msaa != game_msaa) Store32(base, record + 0x18, msaa);

  const std::string chosen = Describe(w, h, Samples(msaa));
  REXLOG_INFO("{}: the game draws its 3D scene at {} (its own choice {}), the screen at {} x {}; {} EDRAM tiles a "
              "surface (the game's layout has {}){}",
              game, chosen, Describe(game_w, game_h, Samples(game_msaa)), screen_w, screen_h,
              SurfaceTiles(w, h, Samples(msaa)), tiles, why);
  std::lock_guard<std::mutex> lock(g_note_mutex);
  g_note = chosen;
}

std::string GameDrawSizeNote() {
  std::lock_guard<std::mutex> lock(g_note_mutex);
  return g_note;
}

void DrawGameDrawSizeRows(PageContext& c) {
  {
    const char* v[] = {"console", "screen"};
    const char* n[] = {"The console's (1024 x 600, stretched)", "The screen's (every pixel drawn)"};
    c.dirty |= Choice("Game's 3D size", "game_draw_size", v, n, 2, Applies::kAfterRestart, &c.restart);
    Help("The size the game draws its 3D world at before the menus and HUD go on top. The Xbox 360 drew 1024 x 600 "
         "and stretched it to the screen (1280 x 720); \"the screen's\" draws every pixel itself, a little slower. "
         "Render scale multiplies either. Applies after a restart.");
  }
  c.dirty |= Check("Game's anti-aliasing (2x MSAA)", "game_draw_msaa", Applies::kAfterRestart, &c.restart);
  Help("The game's own 2x multisampling. The game's graphics memory layout only has room for it at the console's "
       "size, so \"the screen's\" size goes without it (render scale smooths edges instead). Applies after a "
       "restart.");
  const std::string note = GameDrawSizeNote();
  if (!note.empty()) Help(("Drawing now: " + note + ".").c_str());
}

}  // namespace f2

// The [[midasm_hook]] named in World at War's and Call of Duty 4's config/default.toml (campaign and multiplayer).
void iw3_game_draw_size_hook(PPCContext& ctx, uint8_t* base) { f2::ApplyGameDrawSize(ctx, base, "f2"); }
