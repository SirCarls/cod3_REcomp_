// cod2 / cod2mp - the turn while aiming down the sights: the game's own zoom scaling, monitor distance, and a
// percentage on top (inputkit::sensscale::AdsScale). [input-port] 2026-11-24
//
// CoD2 scales the mouse turn linearly in degrees, unlike CoD4 and World at War: CG_CalcFov (SP 0x8243F868, MP
// 0x8242B818) stores the picture's horizontal fov over cg_fov, and the cgame copies that, times a weapon factor when
// it is set, into the float CL_MouseMove multiplies the turn by (SP: 0x8245A780 into 0x828E9EE8, read by 0x8242B4D0;
// MP: CG_DrawActive 0x8244B280 into clientActive +0x8608, read by 0x8241A738). This hooks those two writers: after
// the game stores its value, a non-default setting multiplies it by the player's choice. The zoom is measured as the
// tangent ratio of the vertical fovs CG_CalcFov also stores, so the monitor-distance model is the same one CoD4 and
// World at War use. It depends on the zoom only, never on time or the frame rate.

#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "inputkit/sens_scale.h"

REXCVAR_DEFINE_INT32(cod2_ads_model, 0, "cod2/Controls",
                     "The turn while aiming down the sights: 0 the game's own (linear in the field of view), 1 "
                     "monitor distance (cod2_ads_coefficient; modern Call of Duty's and Battlefield's default)")
    .range(0, 1);
REXCVAR_DEFINE_DOUBLE(cod2_ads_coefficient, 1.33, "cod2/Controls",
                      "Monitor distance: where on the screen the zoomed turn matches the normal one, in half-heights "
                      "from the center (1.33: the edge of a 4:3 picture; 0: the center)")
    .range(0.0, 5.0);
REXCVAR_DEFINE_DOUBLE(cod2_ads_percent, 100.0, "cod2/Controls",
                      "The turn while aiming down the sights, in percent of what cod2_ads_model gives (100: as is)")
    .range(0.0, 200.0);

// The second macro level makes the address name expand before the SDK's macros paste it (as game/rumble.cpp does).
#if defined(COD2_MP)
#define COD2_ADS_WRITER sub_8244B280  // CG_DrawActive: the cgame's per-frame copy into the client
#else
#define COD2_ADS_WRITER sub_8245A780  // the cgame's per-frame copy into the client
#endif
#define COD2_ADS_IMP_(name) __imp__##name
#define COD2_ADS_IMP(name) COD2_ADS_IMP_(name)
#define COD2_ADS_EXTERN(name) REX_EXTERN(COD2_ADS_IMP(name))
#define COD2_ADS_HOOK_RAW(name) REX_HOOK_RAW(name)

COD2_ADS_EXTERN(COD2_ADS_WRITER);

namespace {

constexpr uint32_t kDvarValue = 8;  // dvar_t: the current value (CG_CalcFov reads cg_fov there)
constexpr double kPi = 3.14159265358979323846;

#if defined(COD2_MP)
constexpr uint32_t kCgPointer = 0x824D4160;      // cg_t* (CG_CalcFov's stores go through it)
constexpr uint32_t kCgFovX = 0x26300;            // cg_t: the picture's horizontal fov, degrees
constexpr uint32_t kCgFovY = 0x26304;            // cg_t: its vertical fov
constexpr uint32_t kCgRatio = 0x28378;           // cg_t: fov x / cg_fov, the game's turn scale
constexpr uint32_t kAspectPointer = 0x824D4164;  // its float +0x5E94: the picture's width over 4:3's
constexpr uint32_t kAspectOffset = 0x5E94;
constexpr uint32_t kClientPointer = 0x824D37E0;  // clientActive_t*
constexpr uint32_t kClientScale = 0x8608;        // clientActive_t: the float CL_MouseMove multiplies by
constexpr uint32_t kCgFovDvar = 0x82D6CA38;      // dvar_t* cg_fov
constexpr uint32_t kCgFovScaleDvar = 0x8277234C;  // dvar_t* cg_fovScale (the hip fov is cg_fov times it)
#else
constexpr uint32_t kFovX = 0x8272F1C8;   // the picture's horizontal fov, degrees (CG_CalcFov)
constexpr uint32_t kFovY = 0x8272F1CC;   // its vertical fov
constexpr uint32_t kRatio = 0x8272F248;  // fov x / cg_fov, the game's turn scale
constexpr uint32_t kAspect = 0x82709710;  // float: the picture's width over 4:3's
constexpr uint32_t kScale = 0x828E9EE8;  // the float CL_MouseMove multiplies by
constexpr uint32_t kCgFovDvar = 0x827F6DD0;  // dvar_t* cg_fov
#endif

uint32_t LoadBe32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, sizeof(v));
  return __builtin_bswap32(v);
}

float LoadFloat(const uint8_t* base, uint32_t address) {
  const uint32_t bits = LoadBe32(base, address);
  float f;
  std::memcpy(&f, &bits, sizeof(f));
  return f;
}

void StoreFloat(uint8_t* base, uint32_t address, float f) {
  uint32_t bits;
  std::memcpy(&bits, &f, sizeof(bits));
  bits = __builtin_bswap32(bits);
  std::memcpy(base + address, &bits, sizeof(bits));
}

float DvarFloat(const uint8_t* base, uint32_t dvar_pointer) {
  const uint32_t dvar = LoadBe32(base, dvar_pointer);
  return dvar == 0 ? 0.0f : LoadFloat(base, dvar + kDvarValue);  // 0 before the game registered it
}

double Tan(double degrees) { return std::tan(degrees * kPi / 360.0); }  // of half the angle

struct View {
  uint32_t scale = 0;   // where the client's turn scale is
  double cg_fov = 0.0;  // the raw dvar (the game divides by it)
  double hip = 0.0;     // the unzoomed horizontal fov at 4:3
  double fov_y = 0.0;   // the picture's vertical fov now
  double ratio = 0.0;   // the game's fov x / cg_fov now
  double aspect = 0.0;  // the picture's width over 4:3's
};

bool ReadView(const uint8_t* base, View& v) {
  v.cg_fov = DvarFloat(base, kCgFovDvar);
#if defined(COD2_MP)
  const uint32_t cg = LoadBe32(base, kCgPointer);
  const uint32_t client = LoadBe32(base, kClientPointer);
  const uint32_t aspect = LoadBe32(base, kAspectPointer);
  if (cg == 0 || client == 0 || aspect == 0) return false;
  const double fov_scale = LoadBe32(base, kCgFovScaleDvar) == 0 ? 1.0 : DvarFloat(base, kCgFovScaleDvar);
  v.hip = v.cg_fov * fov_scale;
  v.fov_y = LoadFloat(base, cg + kCgFovY);
  v.ratio = LoadFloat(base, cg + kCgRatio);
  v.aspect = LoadFloat(base, aspect + kAspectOffset);
  v.scale = client + kClientScale;
  (void)kCgFovX;
#else
  v.hip = v.cg_fov;
  v.fov_y = LoadFloat(base, kFovY);
  v.ratio = LoadFloat(base, kRatio);
  v.aspect = LoadFloat(base, kAspect);
  v.scale = kScale;
  (void)kFovX;
#endif
  // Anything out of these ranges is a value read before the game set it up, or a wrong address: leave the game's.
  const auto in = [](double x, double lo, double hi) { return std::isfinite(x) && x > lo && x < hi; };
  return in(v.cg_fov, 1.0, 179.0) && in(v.hip, 1.0, 179.0) && in(v.fov_y, 0.0, 179.0) && in(v.ratio, 0.0, 10.0) &&
         in(v.aspect, 0.1, 10.0);
}

// The multiplier on the game's own value. The zoom is the tangent ratio of the zoomed and unzoomed vertical fovs
// (CG_CalcFov's vertical is 2 atan(0.75 tan(horizontal / 2))), the same measure CoD4's and World at War's games use,
// so AdsScale's models mean the same in all three. Returns 1 at the hip.
double Multiplier(const inputkit::sensscale::Ads& ads, const View& v, double* zoom) {
  const double t = std::tan(v.fov_y * kPi / 360.0) / (0.75 * Tan(v.hip));
  *zoom = t;
  if (!std::isfinite(t) || t <= 0.0 || t >= 1.0 || v.ratio <= 0.0) return 1.0;
  const double mine = inputkit::sensscale::AdsScale(ads, t, v.hip, 4.0 / 3.0);
  if (ads.model == inputkit::sensscale::ZoomModel::kGame) return mine / t;  // the percentage on the game's own
  // Monitor distance: the hip's own scale (fov x / cg_fov at the hip; 1 at 4:3, more on a wider picture) times
  // the matched zoom, over what the game stores now.
  const double hip_x = 360.0 / kPi * std::atan(v.aspect * 0.75 * Tan(v.hip));
  return hip_x / v.cg_fov * mine / v.ratio;
}

// One line on the first zoomed frame after the settings change (and the first of the session), so a test run can
// read the scale in and out. The writer runs on the game thread only.
void LogOnce(const inputkit::sensscale::Ads& ads, double zoom, double in, double out, double cg_fov) {
  static bool logged = false;
  static int last_model = -1;
  static double last_coefficient = 0.0, last_percent = 0.0;
  const int model = ads.model == inputkit::sensscale::ZoomModel::kMonitorDistance ? 1 : 0;
  if (logged && model == last_model && ads.coefficient == last_coefficient && ads.percent == last_percent) return;
  logged = true;
  last_model = model;
  last_coefficient = ads.coefficient;
  last_percent = ads.percent;
  REXLOG_INFO("cod2: ads model {} coefficient {:.2f} percent {:.0f}: zoom {:.4f}, turn scale in {:.4f} out {:.4f} "
              "(cg_fov {:.1f})",
              model, ads.coefficient, ads.percent, zoom, in, out, cg_fov);
}

void Apply(uint8_t* base) {
  inputkit::sensscale::Ads ads;
  ads.model = REXCVAR_GET(cod2_ads_model) == 1 ? inputkit::sensscale::ZoomModel::kMonitorDistance
                                               : inputkit::sensscale::ZoomModel::kGame;
  ads.coefficient = REXCVAR_GET(cod2_ads_coefficient);
  ads.percent = REXCVAR_GET(cod2_ads_percent);
  View v;
  if (!ReadView(base, v)) return;
  double zoom = 1.0;
  const double m = Multiplier(ads, v, &zoom);
  const bool zoomed = zoom > 0.0 && zoom < 1.0;
  const float in = LoadFloat(base, v.scale);
  if (ads.model == inputkit::sensscale::ZoomModel::kGame && ads.percent == 100.0) {
    if (zoomed) LogOnce(ads, zoom, in, in, v.cg_fov);  // the defaults: the game's own, untouched
    return;
  }
  if (m == 1.0 || !std::isfinite(m)) return;
  const float out = float(in * m);
  StoreFloat(base, v.scale, out);
  if (zoomed) LogOnce(ads, zoom, in, out, v.cg_fov);
}

}  // namespace

COD2_ADS_HOOK_RAW(COD2_ADS_WRITER) {
  COD2_ADS_IMP(COD2_ADS_WRITER)(ctx, base);
  Apply(base);
}
