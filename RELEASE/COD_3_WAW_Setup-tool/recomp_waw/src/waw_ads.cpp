// waw - the turn while aiming down the sights: the game's own zoom scaling, monitor distance, and a percentage on
// top (inputkit::sensscale::AdsScale). [input-port] 2026-11-24
//
// The cgame computes tan(zoomed fov / 2) / tan(cg_fov / 2) every frame (0x820F70F0), times an aim-assist factor
// (0x82101880), and hands it to the client's setter 0x822C43E0 (local client r3, the ratio f1), which stores it in
// clientActive_t +0x27168 where CL_MouseMove 0x822C91A0 multiplies the mouse turn by it. With waw_ads_model or
// waw_ads_percent changed, the setter stores the player's choice instead. It depends on the zoom only, never on time
// or the frame rate.

#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "generated/default/waw_pch.h"
#include "inputkit/sens_scale.h"

REXCVAR_DEFINE_INT32(waw_ads_model, 0, "waw",
                     "The turn while aiming down the sights: 0 the game's own (the tangents of the zoomed and normal "
                     "fields of view), 1 monitor distance (waw_ads_coefficient; modern Call of Duty's and "
                     "Battlefield's default)")
    .range(0, 1);
REXCVAR_DEFINE_DOUBLE(waw_ads_coefficient, 1.33, "waw",
                      "Monitor distance: where on the screen the zoomed turn matches the normal one, in half-heights "
                      "from the center (1.33: the edge of a 4:3 picture; 0: the center, the game's own)")
    .range(0.0, 5.0);
REXCVAR_DEFINE_DOUBLE(waw_ads_percent, 100.0, "waw",
                      "The turn while aiming down the sights, in percent of what waw_ads_model gives (100: as is)")
    .range(0.0, 200.0);

REX_EXTERN(__imp__sub_822C43E0);  // the client's zoom-ratio setter (local client r3, ratio f1)

namespace {

constexpr uint32_t kCgFovDvar = 0x844C9C04;  // dvar_t* cg_fov (0x820F70F0 takes the tangent of its value)
constexpr uint32_t kDvarValue = 12;          // dvar_t: the current value

uint32_t LoadBe32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, sizeof(v));
  return __builtin_bswap32(v);
}

float CgFov(const uint8_t* base) {
  const uint32_t dvar = LoadBe32(base, kCgFovDvar);
  if (dvar == 0) return 0.0f;  // before the cgame registered it
  const uint32_t bits = LoadBe32(base, dvar + kDvarValue);
  float hip;
  std::memcpy(&hip, &bits, sizeof(hip));
  return hip;
}

// One line on the first zoomed call after the settings change (and the first one of the session), so a test run can
// read the ratio in and out instead of inferring it from the turn. The setter runs on the game thread only.
void LogOnce(const inputkit::sensscale::Ads& ads, double in, double out, float hip) {
  static bool logged = false;
  static int last_model = -1;
  static double last_coefficient = 0.0, last_percent = 0.0;
  const int model = ads.model == inputkit::sensscale::ZoomModel::kMonitorDistance ? 1 : 0;
  if (logged && model == last_model && ads.coefficient == last_coefficient && ads.percent == last_percent) return;
  logged = true;
  last_model = model;
  last_coefficient = ads.coefficient;
  last_percent = ads.percent;
  REXLOG_INFO("waw: ads model {} coefficient {:.2f} percent {:.0f}: zoom ratio in {:.4f} out {:.4f} (cg_fov {:.1f})",
              model, ads.coefficient, ads.percent, in, out, hip);
}

double Adjust(const uint8_t* base, double game) {
  inputkit::sensscale::Ads ads;
  ads.model = REXCVAR_GET(waw_ads_model) == 1 ? inputkit::sensscale::ZoomModel::kMonitorDistance
                                              : inputkit::sensscale::ZoomModel::kGame;
  ads.coefficient = REXCVAR_GET(waw_ads_coefficient);
  ads.percent = REXCVAR_GET(waw_ads_percent);
  const bool zoomed = game > 0.0 && game < 1.0;
  if (ads.model == inputkit::sensscale::ZoomModel::kGame && ads.percent == 100.0) {
    if (zoomed) LogOnce(ads, game, game, CgFov(base));
    return game;
  }
  const float hip = CgFov(base);
  if (hip == 0.0f) return game;
  // cg_fov is horizontal at 4:3 in the IW engine.
  const double out = inputkit::sensscale::AdsScale(ads, game, hip, 4.0 / 3.0);
  if (zoomed) LogOnce(ads, game, out, hip);
  return out;
}

}  // namespace

REX_HOOK_RAW(sub_822C43E0) {
  ctx.f1.f64 = Adjust(base, ctx.f1.f64);
  __imp__sub_822C43E0(ctx, base);
}
