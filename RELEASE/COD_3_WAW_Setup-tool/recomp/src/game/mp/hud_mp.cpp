// cod3mp: the campaign's custom crosshair (ui/hud_overlay.cpp) with one more
// rule for split screen.
//
// The custom crosshair is drawn at the middle of the screen, which is player
// 1's view only while one player plays. CG_DrawCrosshair runs once per local
// player's view, with the game's active local client set to that player
// (codmp_xenonf.xex 0x8245DD5C: cg + client * 6368); when any other player's
// view has drawn a crosshair lately, every player keeps the game's own.
// mp/config/codmp_campaign.toml calls these instead of the campaign's hooks.

#include <atomic>
#include <chrono>
#include <cstdint>

#include <rex/hook.h>

#include "game/addresses.h"
#include "game/guest.h"

bool Cod3CrosshairHook(PPCRegister& f29, PPCRegister& r1, uint8_t* base);
bool Cod3MountedCrosshairHook(PPCRegister& r1, uint8_t* base);

namespace {

using Clock = std::chrono::steady_clock;

// When a view other than player 1's last drew a crosshair.
std::atomic<int64_t> g_other_view_ns{0};
constexpr int64_t kSplitScreenHoldNs = 1'000'000'000;

bool OnlyPlayerOne(uint8_t* base) {
  const int64_t now = Clock::now().time_since_epoch().count();
  if (cod3::guest::ReadU32(base, cod3::addr::kActiveLocalClient) != 0) {
    g_other_view_ns.store(now, std::memory_order_relaxed);
    return false;
  }
  const int64_t other = g_other_view_ns.load(std::memory_order_relaxed);
  return other == 0 || now - other > kSplitScreenHoldNs;
}

}  // namespace

bool Cod3MpCrosshairHook(PPCRegister& f29, PPCRegister& r1, uint8_t* base) {
  return OnlyPlayerOne(base) && Cod3CrosshairHook(f29, r1, base);
}

bool Cod3MpMountedCrosshairHook(PPCRegister& r1, uint8_t* base) {
  return OnlyPlayerOne(base) && Cod3MountedCrosshairHook(r1, base);
}
