// cod3 - the game's own crosshair drawn sharp at the screen's resolution. [acct2-ocr] 2026-11-21 task 125
//
// Owner 11-21 10:55 ("for the default crosshair it draws at a super low res") and 14:29 ("re-drawing the crosshair is
// fine, but the logic needs to be PERFECT"). The game draws its cross as four quads of the 640 x 480 HUD space, each
// sampling one 8 x 8 picture (run acct2-ocr-xm60: 7.5-7.6 x 8 units, format 20, color FFFFFFFF): at 1920 x 1080 that
// is 18-23 pixels from 8 texels, magnified and filtered soft.
//
// Nothing of the game's logic is done again here: the game still works out and draws every piece itself (which
// pieces, where, how big, its color, its fade as the player moves or aims, its spread, split-screen views, vehicles
// and mounted guns). While the game's CG_DrawCrosshair runs (Drawing, set by its hooks in ui/hud_overlay.cpp), each
// quad it hands nglListAddQuad (QuadSwap, in game/ultrawide.cpp's hook) that samples a small picture magnified is,
// for that one call, pointed at a copy of the same picture made for the size it is drawn on screen: its outline where
// the game's own drawing (as the GPU samples it, clamped) is half its strength, antialiased over exactly one screen
// pixel, at 4 texels a pixel in exact 8:8:8:8 texels (ui/crosshair_texture_math.h). The quad is put back as it was
// after the list copies it. Each copy is made once
// (guest memory that the GPU reads, never written again) and found again by the picture's bytes and the size.
// cod3_crosshair_sharp off: the game's own picture.
#pragma once

#include <cstdint>

namespace cod3::crosshair_texture {

// Around the game's own CG_DrawCrosshair (the game's thread): quads handed to nglListAddQuad meanwhile are the
// crosshair's.
class Drawing {
 public:
  Drawing();
  ~Drawing();
  Drawing(const Drawing&) = delete;
  Drawing& operator=(const Drawing&) = delete;
};

// In nglListAddQuad's hook, before the list copies `quad` (r3): while Drawing, a quad sampling a picture this makes
// sharp is pointed at the sharp copy until this goes (after the copy). `hud_scale`: the factor ultrawide's mapping
// squeezes the HUD's x by (0 when it doesn't).
class QuadSwap {
 public:
  QuadSwap(uint8_t* base, uint32_t quad, float hud_scale);
  ~QuadSwap();
  QuadSwap(const QuadSwap&) = delete;
  QuadSwap& operator=(const QuadSwap&) = delete;

 private:
  uint8_t* base_ = nullptr;
  uint32_t quad_ = 0;
  uint32_t texture_ = 0;   // the game's, put back; 0: nothing changed
};

}  // namespace cod3::crosshair_texture
