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
#include "game/viewmodel_flip.h"  // [claude-9b] the weapon in the left hand

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
// [claude-9b] 2026-10-27 (owner 09:56: "aiming down sights seems like it's jittery with the 60hz patch"; 10:13:
// "the ADS jutter needs fixing ASAP"): one line a drawn frame while aiming or moving, for this many frames: what the
// gun is placed from and where it ends up, beside the camera, to see which of them moves in steps.
REXCVAR_DEFINE_INT32(cod3_debug_ads_trace, 0, "cod3",
                     "Log the gun, the camera and the player's position in every drawn frame while aiming "
                     "or moving, for this many frames (0: off; campaign)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

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

// [claude-9b] 2026-10-27 cod3_debug_ads_trace: after the gun is placed (r3 = its refEntity: axes +20, origin +60).
// The camera is cg's refdef (cg = 0x82A98D90 + client x 5840: fov +4472, origin +4480, axes +4496) after the
// movement's smoothing (CG_CalcViewValues' hook runs before the gun is placed); the player's playerState origin
// and velocity are the gclient's +0 and +16; +80 and +140 feed the gun's offset (0x82440AA8), as do cg +4596 / +4600.
void AdsTrace(uint8_t* base, uint32_t gun, uint32_t client) {
  static int logged = 0, last_setting = 0;
  static auto last = std::chrono::steady_clock::time_point{};
  const int setting = REXCVAR_GET(cod3_debug_ads_trace);
  if (setting != last_setting) {
    logged = 0;
    last_setting = setting;
  }
  if (cod3::kMultiplayer || setting <= 0 || logged >= setting || client > 3 || !Readable(gun)) return;
  const uint32_t list = ReadU32(base, kEntityList);
  if (!Readable(list)) return;
  const uint32_t entity = ReadU32(base, list + 4 * (client + 1));
  if (!Readable(entity)) return;
  const uint32_t ps = ReadU32(base, entity + kEntityClient);
  if (!Readable(ps)) return;
  const float aim = ReadF32(base, ps + kWeaponPosFrac);
  const float vx = ReadF32(base, ps + 16), vy = ReadF32(base, ps + 20);
  if (!(aim > 0.01f) && vx * vx + vy * vy < 1.0f) return;  // neither aiming nor moving
  const uint32_t cg = 0x82A98D90u + client * 5840u;
  const auto now = std::chrono::steady_clock::now();
  const long long dt_us =
      last.time_since_epoch().count() ? std::chrono::duration_cast<std::chrono::microseconds>(now - last).count() : 0;
  last = now;
  ++logged;
  REXLOG_INFO("cod3: ads trace {} dt {} aim {:.4f} f80 {:.4f} ps {:.3f} {:.3f} {:.3f} vel {:.1f} {:.1f} cam {:.3f} {:.3f} "
              "{:.3f} fwd {:.5f} {:.5f} {:.5f} fov {:.3f} {:.3f} gun {:.3f} {:.3f} {:.3f} gfwd {:.5f} {:.5f} {:.5f} "
              "c4596 {:.4f} c4600 {:.4f} bob {} phase {:.4f} speed {:.2f} yaw {:.4f} pitch {:.4f}",
              logged, dt_us, aim, ReadF32(base, ps + 80), ReadF32(base, ps + 0), ReadF32(base, ps + 4),
              ReadF32(base, ps + 8), vx, vy, ReadF32(base, cg + 4480), ReadF32(base, cg + 4484), ReadF32(base, cg + 4488),
              ReadF32(base, cg + 4496), ReadF32(base, cg + 4500), ReadF32(base, cg + 4504), ReadF32(base, cg + 4472),
              ReadF32(base, cg + 4476), ReadF32(base, gun + 60), ReadF32(base, gun + 64), ReadF32(base, gun + 68),
              ReadF32(base, gun + 20), ReadF32(base, gun + 24), ReadF32(base, gun + 28), ReadF32(base, cg + 4596),
              ReadF32(base, cg + 4600), ReadU32(base, ps + 40) & 0xFFu, ReadF32(base, cg + 5804),
              ReadF32(base, cg + 5808), ReadF32(base, cg + 4548), ReadF32(base, cg + 4544));
}

// [claude-9b] 2026-10-27 The gun mirrored to the left hand: its left axis (refEntity + 32) turned around, and its
// scene marked for the turned culling (game/viewmodel_flip.cpp).
constexpr uint32_t kGunLeftAxis = 32;

void Mirror(uint8_t* base, uint32_t gun, uint32_t client) {
  if (!Readable(gun)) return;
  for (int i = 0; i < 3; ++i) {
    const uint32_t a = gun + kGunLeftAxis + 4 * i;
    WriteF32(base, a, -ReadF32(base, a));
  }
  cod3::viewmodel::MarkMirrored(client);
}

}  // namespace

// The gun's place each frame (r3 = its refEntity), called from CG_AddViewWeapon
// through sub_8244F1D8 on the game's main thread (cod3mp: sub_8245E3A8 from
// sub_82468D58; tools/mp_aliases.py renames the hook). The offsets ride on the
// game's own cg_gunX / cg_gunY / cg_gunZ for this one call: CS2's x (right)
// is cg_gunY's left with the sign turned, y (away) is cg_gunX, z is cg_gunZ.
// [claude-9b] 2026-10-27 With the weapon in the left hand (game/viewmodel_flip.cpp), the gun's left axis
// (its refEntity + 32, the axes written by this call) is turned around after the call, and CS2's x is turned too, so
// "Right" moves the weapon outward on either side.
REX_EXTERN(__imp__sub_82445118);
REX_HOOK_RAW(sub_82445118) {
  const uint32_t client = ReadU32(base, kActiveLocalClient);
  const bool flip = client < 4 && cod3::viewmodel::FlipFor(client) && !cod3::viewmodel::OnMountedGun(base, client) &&
                    cod3::viewmodel::OwnViewModelScene(base);
  cod3::viewmodel::Offsets o = Current();
  if (flip) o.x = -o.x;
  const uint32_t gun = ctx.r3.u32;  // its refEntity (the call may change r3)
  if (o.x == 0.0 && o.y == 0.0 && o.z == 0.0) {
    if (g_logged_once) LogWhenChanged(o);
    __imp__sub_82445118(ctx, base);
    if (flip) Mirror(base, gun, client);
    AdsTrace(base, gun, client);  // [claude-9b]
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
  __imp__sub_82445118(ctx, base);
  WriteF32(base, x, game_x);
  WriteF32(base, y, game_y);
  WriteF32(base, z, game_z);
  if (flip) Mirror(base, gun, client);
  AdsTrace(base, gun, client);  // [claude-9b]
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
