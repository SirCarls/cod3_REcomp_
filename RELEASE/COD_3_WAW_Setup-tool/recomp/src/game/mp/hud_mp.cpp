// cod3mp: the campaign's custom crosshair (ui/hud_overlay.cpp) in every
// split-screen player's view.
//
// CG_DrawCrosshair runs once per local player's view, with the game's active
// local client set to that player (codmp_xenonf.xex 0x8245DD5C: cg + client *
// 6368). [claude-9b] Each call says which view it is and where that view is on
// the picture (the client's refdef: x, y, width, height at cg + 4928, the
// rectangle CG_DrawCrosshair's own position math reads at sub_82457560), so the
// custom crosshair is drawn at the middle of each player's view (owner
// 2026-10-26: "custom crosshairs per player, placed where the original would
// be"); a player who leaves stops calling it, and the views left are resized by
// the game, so the crosshairs follow. Before: with more than one player every
// player kept the game's own crosshair.
// mp/config/codmp_campaign.toml calls these instead of the campaign's hooks.

#include <algorithm>
#include <cmath>
#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>

#include "game/addresses.h"
#include "game/guest.h"
#include "game/ultrawide.h"
#include "ui/hud_overlay.h"

bool Cod3CrosshairHook(PPCRegister& f29, PPCRegister& r1, uint8_t* base);
bool Cod3MountedCrosshairHook(PPCRegister& r1, uint8_t* base);

namespace {

// [claude-9b] cg_t in codmp_xenonf.xex: one per local player, refdef_t at +4928 starting
// {int x, y, width, height} (CG_DrawCrosshair's position call 0x82457560 reads the width and
// height at +4936 / +4940; the view setup clears 96 bytes from +4928 and fills the four
// through 0x8252D870). MP only, so not in addresses.h's table.
constexpr uint32_t kCgArray = 0x82C7D490;  // lis -32056 / addi -11120
constexpr uint32_t kCgStride = 6368;
constexpr uint32_t kRefdefX = 4928;
constexpr int kLocalPlayers = 4;

// Tell the overlay which player's view this crosshair is for and where that view is: as a
// share of the screen, which is what all the local players' views cover together.
void SetView(uint8_t* base) {
  using cod3::guest::ReadS32;
  const uint32_t client = cod3::guest::ReadU32(base, cod3::addr::kActiveLocalClient);
  if (client >= uint32_t(kLocalPlayers)) return;  // not a local player: player 1's whole picture
  int32_t right = 0, bottom = 0;
  int32_t rect[kLocalPlayers][4] = {};
  for (int i = 0; i < kLocalPlayers; ++i) {
    const uint32_t at = kCgArray + kCgStride * uint32_t(i) + kRefdefX;
    for (int k = 0; k < 4; ++k) rect[i][k] = ReadS32(base, at + 4 * k);
    const int32_t* r = rect[i];
    // Views never set up (all zero) or not making sense are left out.
    if (r[2] <= 0 || r[3] <= 0 || r[0] < 0 || r[1] < 0 || r[0] + r[2] > 16384 || r[1] + r[3] > 16384) continue;
    right = std::max(right, r[0] + r[2]);
    bottom = std::max(bottom, r[1] + r[3]);
  }
  const int32_t* r = rect[client];
  if (right <= 0 || bottom <= 0 || r[2] <= 0 || r[3] <= 0) return;
  // A view's place and shape in the log when it changes (a player joining or leaving): the game's
  // projection shape from its field of view (refdef +16 / +20 are fov_x / fov_y in degrees, not
  // tangents; [claude-9b] 2026-10-27, FINDINGS-SPLITSCREEN-VIEW: the old line divided the degrees and
  // looked like a stretch) against the view's shape on the picture (its share of the 640x480 screen
  // times the picture's shape). They are equal at 16:9; at another picture shape the port's
  // projection is in ultrawide.cpp's "3D view" line (owner 2026-10-26: "a slight horizontal stretch").
  static int32_t logged[kLocalPlayers][4] = {};
  if (std::equal(r, r + 4, logged[client]) == false) {
    std::copy(r, r + 4, logged[client]);
    const uint32_t at = kCgArray + kCgStride * client + kRefdefX;
    constexpr double kHalfDegree = 3.14159265358979323846 / 360.0;
    const double fov_x = cod3::guest::ReadF32(base, at + 16), fov_y = cod3::guest::ReadF32(base, at + 20);
    const double tan_y = std::tan(fov_y * kHalfDegree);
    const double game = tan_y > 0.0 ? std::tan(fov_x * kHalfDegree) / tan_y : 0.0;
    const double shown = double(r[2]) / double(r[3]) * 0.75 * cod3::DisplayAspect();
    REXLOG_INFO("cod3mp: player {}'s view {}x{} at {},{} of {}x{}; fov {:.1f} x {:.1f}, the game's projection "
                "shape {:.4f}, shape on the picture {:.4f}",
                client + 1, r[2], r[3], r[0], r[1], right, bottom, fov_x, fov_y, game, shown);
  }
  cod3::ui::SetGameCrosshairView(int(client), float(r[0]) / float(right), float(r[1]) / float(bottom),
                                 float(r[2]) / float(right), float(r[3]) / float(bottom));
}

}  // namespace

bool Cod3MpCrosshairHook(PPCRegister& f29, PPCRegister& r1, uint8_t* base) {
  SetView(base);
  return Cod3CrosshairHook(f29, r1, base);
}

bool Cod3MpMountedCrosshairHook(PPCRegister& r1, uint8_t* base) {
  SetView(base);
  return Cod3MountedCrosshairHook(r1, base);
}
