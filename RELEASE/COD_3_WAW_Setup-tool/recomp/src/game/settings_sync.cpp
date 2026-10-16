// cod3 - one setting, two places (see settings_sync.h).

#include "game/settings_sync.h"

#include <atomic>
#include <chrono>
#include <string>

#include <rex/logging.h>

#include "game/addresses.h"
#include "game/engine.h"
#include "game/game_api.h"

namespace cod3::settings {
namespace {
namespace a = cod3::addr;

constexpr const char* kCrosshairDvar = "cg_drawCrosshair";

std::atomic<bool> g_resync{true};  // apply the saved choice at the next frame
uint32_t g_cvar = 0;               // cvar_t* of cg_drawCrosshair, 0 until registered
int g_last_dvar = -1;              // on/off as last seen (-1: not yet)
int g_last_byte = -1;

uint32_t CrosshairByte() { return a::controls::kSettings + a::controls::kCrosshair; }

// Both places to `on`.
void Apply(PPCContext& ctx, uint8_t* base, int on) {
  if ((game::CvarInteger(base, g_cvar) != 0) != (on != 0))
    Engine::CvarSet(ctx, base, kCrosshairDvar, on ? "1" : "0", /*force=*/true);
  base[CrosshairByte()] = uint8_t(on ? 1 : 0);
  g_last_dvar = g_last_byte = on ? 1 : 0;
}
}  // namespace

void NoteLevelStart() { g_resync = true; }

void SyncCrosshair(PPCContext& ctx, uint8_t* base) {
  if (!g_cvar) {
    // The client game registers it when it first starts; look twice a second
    // until then (a guest call, so not every frame).
    static std::chrono::steady_clock::time_point next_look;
    const auto now = std::chrono::steady_clock::now();
    if (now < next_look) return;
    next_look = now + std::chrono::milliseconds(500);
    g_cvar = Engine::CvarFind(ctx, base, kCrosshairDvar);
    if (!g_cvar) return;
    REXLOG_INFO("cod3: crosshair setting: cg_drawCrosshair {}, Options > Gameplay > Crosshair {}",
                game::CvarInteger(base, g_cvar), base[CrosshairByte()]);
  }
  const int dvar = game::CvarInteger(base, g_cvar) != 0 ? 1 : 0;
  const int byte = base[CrosshairByte()] != 0 ? 1 : 0;
  if (g_resync.exchange(false)) {
    // The player's last choice, from either place, is in the console config;
    // the profile may hold an older one.
    const auto archived = Engine::Get().ArchivedCvars();
    if (const auto it = archived.find(kCrosshairDvar); it != archived.end()) {
      Apply(ctx, base, it->second != "0");
    } else {
      g_last_dvar = dvar;  // nothing chosen yet: both stay as the game has them
      g_last_byte = byte;
    }
    return;
  }
  const char* from = nullptr;
  int on = 0;
  if (dvar != g_last_dvar) {
    from = "cg_drawCrosshair; Options > Gameplay follows";  // the console, a config, a script
    on = dvar;
  } else if (byte != g_last_byte) {
    from = "Options > Gameplay; cg_drawCrosshair follows";
    on = byte;
  }
  if (!from) return;
  Apply(ctx, base, on);
  Engine::Get().SetArchived(kCrosshairDvar, on ? "1" : "0");
  REXLOG_INFO("cod3: crosshair {} ({})", on ? "on" : "off", from);
}

}  // namespace cod3::settings
