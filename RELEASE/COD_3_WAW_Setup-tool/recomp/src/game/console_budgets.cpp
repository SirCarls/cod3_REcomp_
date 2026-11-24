// cod3 - [acct2-decomp] 2026-11-22 what is drawn at a distance: the Xbox 360's budgets, raised (see
// console_budgets.h).
//
// Both are values the game reads as it goes; nothing here changes what the game simulates, its speed or its clock.
//  - Detail: RE_RenderScene (campaign 0x82430710) turns r_lodscale into the scene's detail scale (smaller: the
//    detailed models stay longer; addresses.h crossref::kRLodScalePtr, ultrawide.cpp's split-screen note). The view
//    gets r_lodscale / cod3_model_detail_distance while the game draws it, then r_lodscale is put back, so the
//    console, a config and ultrawide.cpp's split-screen detail (cod3mp_split_lod, which doubles it inside) all still
//    see the game's value.
//  - Grass: the game keeps the nearest grass patches each frame (grassmanager::FrustumCullGPatch, 0x823F10A8, from
//    0x823F26A8; cod3mp 0x8240C138), up to the count its set-up stores (0x823F6A68 stores 88, or 500 with
//    capture_movie; cod3mp 0x82411848). The keep code checks the count against 2048 (0x823F2778); the native
//    renderer draws at most 1024 patches a frame (ngl_side.cpp kGrassPatches), so the setting stops there.

#include "game/console_budgets.h"

#include <algorithm>
#include <atomic>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/guest.h"

REXCVAR_DEFINE_DOUBLE(cod3_model_detail_distance, 2.0, "cod3/Display",
                      "How far models keep their detailed versions, times the Xbox 360's distances: 1 = the "
                      "console's, 2 = twice as far. The game's r_lodscale is divided by this for each 3D view")
    .range(1.0, 4.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(cod3_grass_patches, 500, "cod3/Display",
                     "Grass patches drawn around you, the nearest first: the Xbox 360 kept 88 (the game keeps 500 "
                     "itself when it records a movie). 88 = the console's; up to 1024. From the next level")
    .range(88, 1024);

namespace cod3::budgets {
namespace {

using cod3::guest::ReadF32;
using cod3::guest::ReadU32;
using cod3::guest::WriteF32;
using cod3::guest::WriteU32;

#if defined(COD3_MP)
constexpr uint32_t kLodScaleDvar = 0x82BA333C;  // cvar_t* r_lodscale (ultrawide.cpp kMpLodScaleDvar)
constexpr uint32_t kGrassBudget = 0x825D5414;   // u32 (dev/waw-decomp/cod3/mp/globals.tsv: 0x8240C138, 0x82411848)
#else
constexpr uint32_t kLodScaleDvar = 0x82A11B5C;  // cvar_t* r_lodscale (addresses.h crossref::kRLodScalePtr)
constexpr uint32_t kGrassBudget = 0x825CC590;   // u32 (dev/waw-decomp/cod3/globals.tsv: 0x823F26A8, 0x823F6A68)
#endif
constexpr uint32_t kCvarValue = 0x1C;  // cvar_t's float (addresses.h cvar::kValue)
constexpr uint32_t kConsoleGrass = 88;   // what the set-up stores without capture_movie
constexpr uint32_t kGameGrassMost = 2048;  // the keep code's own check
constexpr uint32_t kNativeGrassMost = 1024;  // ngl_side.cpp kGrassPatches

bool ValidPointer(uint32_t address) { return address >= 0x10000 && (address & 3) == 0; }

}  // namespace

uint32_t DetailBegin(uint8_t* base, float* saved) {
  const double distance = REXCVAR_GET(cod3_model_detail_distance);
  if (!(distance > 1.0)) return 0;
  const uint32_t dvar = ReadU32(base, kLodScaleDvar);
  if (!ValidPointer(dvar)) return 0;  // not registered yet (before the renderer starts)
  *saved = ReadF32(base, dvar + kCvarValue);
  if (!(*saved > 0.01f && *saved < 16.0f)) return 0;  // not a value the game would use: leave it alone
  const float scale = static_cast<float>(*saved / std::min(distance, 4.0));
  WriteF32(base, dvar + kCvarValue, scale);
  static std::atomic<float> logged{0.0f};
  if (logged.exchange(scale, std::memory_order_relaxed) != scale)
    REXLOG_INFO("cod3: budgets: models keep their detail {:.2f} times as far (r_lodscale {:.2f}, {:.2f} for each 3D "
                "view)",
                distance, *saved, scale);
  return dvar;
}

void DetailEnd(uint8_t* base, uint32_t dvar, float saved) {
  if (dvar) WriteF32(base, dvar + kCvarValue, saved);
}

}  // namespace cod3::budgets

// The grass set-up: the game's own count first, then the setting's (never below what the game chose, so a movie
// recording keeps its 500).
REX_EXTERN(__imp__sub_823F6A68);
REX_HOOK_RAW(sub_823F6A68) {
  __imp__sub_823F6A68(ctx, base);
  namespace b = cod3::budgets;
  const uint32_t game = cod3::guest::ReadU32(base, b::kGrassBudget);
  const uint32_t most = std::min(b::kGameGrassMost, b::kNativeGrassMost);
  const uint32_t wanted = std::clamp<uint32_t>(static_cast<uint32_t>(REXCVAR_GET(cod3_grass_patches)),
                                               b::kConsoleGrass, most);
  const uint32_t count = std::max(game, wanted);
  if (game > 0 && game <= b::kGameGrassMost && count != game) cod3::guest::WriteU32(base, b::kGrassBudget, count);
  static std::atomic<uint32_t> logged{0};
  if (logged.exchange(count, std::memory_order_relaxed) != count)
    REXLOG_INFO("cod3: budgets: grass patches {} (the game's set-up chose {}; the Xbox 360 kept {})", count, game,
                b::kConsoleGrass);
}
