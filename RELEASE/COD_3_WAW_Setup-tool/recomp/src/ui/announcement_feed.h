// cod3 - the announcement column of the HUD.
//
// Short notices everyone in a match should see ("Cheats enabled by the
// host"), in their own column at the right of the screen, under the frame
// rate counter, styled like the game's own obituary feed (one line each,
// newest at the bottom, fading out after a few seconds; ui/theme.h colors).
// It works in single player too, so cod3::mp::Announce() (game/multiplayer.h)
// just pushes here on every machine once a line has been shared.
//
// The column is kept inside the 16:9 HUD area on a wider picture
// (game/ultrawide.h), like the rest of the port's HUD text.

#pragma once

#include <string>

struct ImGuiIO;

namespace cod3::hud {

class AnnouncementFeed {
 public:
  // Any thread. Lines are plain text (^N color codes are shown as written).
  static void Push(std::string text);
  // UI thread, once per frame, from the HUD overlay (hud_overlay.cpp).
  static void Draw(ImGuiIO& io);
  // Drops every line (tests, level change).
  static void Clear();
};

}  // namespace cod3::hud
