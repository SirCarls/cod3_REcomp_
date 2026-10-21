// cod3 - weapon offsets with Counter-Strike 2's commands (see viewmodel.h).

#include "game/viewmodel.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/addresses.h"
#include "game/guest.h"
#include "game/program.h"

REXCVAR_DEFINE_DOUBLE(cod3_viewmodel_offset_x, 0.0, "cod3/Game",
                      "Weapon to the right, in game units (Counter-Strike 2's viewmodel_offset_x; 0 = the "
                      "game's own place)")
    .range(-2.5, 2.5)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(cod3_viewmodel_offset_y, 0.0, "cod3/Game",
                      "Weapon away from you, in game units (Counter-Strike 2's viewmodel_offset_y)")
    .range(-2.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(cod3_viewmodel_offset_z, 0.0, "cod3/Game",
                      "Weapon up, in game units (Counter-Strike 2's viewmodel_offset_z)")
    .range(-2.0, 2.0)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(cod3_viewmodel_presetpos, 0, "cod3/Game",
                     "Counter-Strike 2's viewmodel_presetpos: 0 the three offsets, 1 Desktop, 2 Couch, "
                     "3 Classic")
    .range(0, 3)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_debug_viewmodel, false, "cod3",
                    "Log the weapon offset, how far the weapon is up to the sights and the gun's origin "
                    "twice a second while an offset is set");

namespace {

using cod3::guest::ReadF32;
using cod3::guest::ReadU32;
using cod3::guest::WriteF32;

// cgame's vmCvar_t copies of the gun offset dvars (symbols.toml, the cgame
// cvarTable at 0x825CDBF0); the float value is at +8. sub_82445118 ends with
// origin += cg_gunX * forward + cg_gunY * left + cg_gunZ * up, the camera's
// axes at cg + 4496 (cg = 0x82A98D90 + client x 5840), the gun's origin at
// its refEntity (r3) + 60. cod3mp: the same in sub_8245E3A8 (axes at
// cg + 4976, cg = 0x82C7D490 + client x 6368).
constexpr uint32_t kGunX = COD3_ADDR(0x82A0E5A0, 0x82B9F830);
constexpr uint32_t kGunY = COD3_ADDR(0x82A0CBB0, 0x82B9DDB0);
constexpr uint32_t kGunZ = COD3_ADDR(0x82A0D8A0, 0x82B9EA10);
constexpr uint32_t kVmCvarValue = 8;

// The player's state as the gun placement reaches it: the active local
// client, the entity list *0x82A2AB40 at [client + 1], the entity's gclient
// at +528 (its view height, +188, is read there too). How far the weapon is
// up to the sights (0 at the hip, 1 at the sights) is at +140: CG_DrawCrosshair
// reads it there (f29) for its own fade, which the custom crosshair follows.
// (+80, given for it in dev/agents/movement/NOTES.md, stays 0 while aiming.)
// cod3mp reads the same chain in both functions: the list *0x82BCC7FC, the
// client at +512, the view height +220 and the aim fraction +172
// (CG_DrawCrosshair sub_8245D9F8: lfs f29,172(r11)).
constexpr uint32_t kActiveLocalClient = cod3::addr::kActiveLocalClient;
constexpr uint32_t kEntityList = COD3_ADDR(0x82A2AB40, 0x82BCC7FC);
constexpr uint32_t kEntityClient = cod3::kMultiplayer ? 512 : 528;
constexpr uint32_t kWeaponPosFrac = cod3::kMultiplayer ? 172 : 140;

// A guest pointer that can be read: not null and not the GPU register window.
bool Readable(uint32_t address) {
  return address >= 0x10000 && (address < 0x7F000000 || address >= 0x80000000) && address < 0xFFFF0000;
}

// How far the weapon is up to the sights, 0 to 1 (0 when it can't be found).
float AimFraction(uint8_t* base) {
  const uint32_t client = ReadU32(base, kActiveLocalClient);
  if (client > 3) return 0.0f;
  const uint32_t list = ReadU32(base, kEntityList);
  if (!Readable(list)) return 0.0f;
  const uint32_t entity = ReadU32(base, list + 4 * (client + 1));
  if (!Readable(entity)) return 0.0f;
  const uint32_t gclient = ReadU32(base, entity + kEntityClient);
  if (!Readable(gclient)) return 0.0f;
  const float frac = ReadF32(base, gclient + kWeaponPosFrac);
  return std::isfinite(frac) ? std::clamp(frac, 0.0f, 1.0f) : 0.0f;
}

cod3::viewmodel::Offsets Current() {
  cod3::viewmodel::Offsets o;
  if (cod3::viewmodel::PresetOffsets(REXCVAR_GET(cod3_viewmodel_presetpos), &o)) return o;
  o.x = REXCVAR_GET(cod3_viewmodel_offset_x);
  o.y = REXCVAR_GET(cod3_viewmodel_offset_y);
  o.z = REXCVAR_GET(cod3_viewmodel_offset_z);
  // Only finite values within CS2's ranges reach the game.
  auto clean = [](double v, double lo, double hi) { return std::isfinite(v) ? std::clamp(v, lo, hi) : 0.0; };
  o.x = clean(o.x, -2.5, 2.5);
  o.y = clean(o.y, -2.0, 2.0);
  o.z = clean(o.z, -2.0, 2.0);
  return o;
}

// One log line per new position (main thread only).
cod3::viewmodel::Offsets g_logged;
bool g_logged_once = false;

void LogWhenChanged(const cod3::viewmodel::Offsets& o) {
  if (g_logged_once && o.x == g_logged.x && o.y == g_logged.y && o.z == g_logged.z) return;
  g_logged = o;
  g_logged_once = true;
  const int preset = REXCVAR_GET(cod3_viewmodel_presetpos);
  REXLOG_INFO("cod3: weapon offset x {:g} y {:g} z {:g} (Counter-Strike 2 axes; {}), faded out at the sights",
              o.x, o.y, o.z, cod3::viewmodel::PresetName(preset));
}

}  // namespace

// The gun's place each frame (r3 = its refEntity), called from CG_AddViewWeapon
// through sub_8244F1D8 on the game's main thread (cod3mp: sub_8245E3A8 from
// sub_82468D58; tools/mp_aliases.py renames the hook). The offsets ride on the
// game's own cg_gunX / cg_gunY / cg_gunZ for this one call: CS2's x (right)
// is cg_gunY's left with the sign turned, y (away) is cg_gunX, z is cg_gunZ.
REX_EXTERN(__imp__sub_82445118);
REX_HOOK_RAW(sub_82445118) {
  const cod3::viewmodel::Offsets o = Current();
  if (o.x == 0.0 && o.y == 0.0 && o.z == 0.0) {
    if (g_logged_once) LogWhenChanged(o);
    __imp__sub_82445118(ctx, base);
    return;
  }
  LogWhenChanged(o);
  const float aim = AimFraction(base);
  const float weight = 1.0f - aim;
  const uint32_t x = kGunX + kVmCvarValue, y = kGunY + kVmCvarValue, z = kGunZ + kVmCvarValue;
  const float game_x = ReadF32(base, x), game_y = ReadF32(base, y), game_z = ReadF32(base, z);
  WriteF32(base, x, game_x + float(o.y) * weight);
  WriteF32(base, y, game_y - float(o.x) * weight);
  WriteF32(base, z, game_z + float(o.z) * weight);
  const uint32_t gun = ctx.r3.u32;  // its refEntity (the call may change r3)
  __imp__sub_82445118(ctx, base);
  WriteF32(base, x, game_x);
  WriteF32(base, y, game_y);
  WriteF32(base, z, game_z);
  if (REXCVAR_GET(cod3_debug_viewmodel)) {
    static auto next = std::chrono::steady_clock::time_point{};
    const auto now = std::chrono::steady_clock::now();
    if (now >= next) {
      next = now + std::chrono::milliseconds(500);
      REXLOG_INFO("cod3: weapon offset debug: at the sights {:.3f}, offset weight {:.3f}, gun origin {:.2f} {:.2f} "
                  "{:.2f}", aim, weight, ReadF32(base, gun + 60), ReadF32(base, gun + 64), ReadF32(base, gun + 68));
    }
  }
}
