// inputkit - other games' mouse sensitivity scales (include/inputkit/sens_scale.h). [input-port] 2026-11-24
//
// Where each constant comes from. The owner named mouse-sensitivity.com; its pages turn automated readers away, so
// the numbers were read from converters that publish its community's yaw table, and kept only where two or more
// agree (2026-11-24):
//   - sens-converter.com's table ("effective degrees per count"): every value below, including the three curves
//     (Battlefield 6's 0.00173623 x s + 0.00231498, PUBG's 0.004444 x 10^(s/50)) and Fortnite's 0.005555.
//   - recharge.com's converter article: the same 0.022 / 0.07 / 0.0066 / 0.00573 / 0.005555 / 0.125, and modern Call
//     of Duty's Monitor Distance Coefficient default of 1.33.
//   - senslab.pro's Battlefield 6 page: the same two constants, with the slider 0-200, default 20.
//   - Ubisoft's own "FOV and input sensitivity" article for Siege: the slider times MouseSensitivityMultiplierUnit
//     (0.02 in GameSettings.ini; slider 50 is a factor of 1); the 0.00572958 degrees per step is at that 0.02.
// How sure: the linear ones are the well-known values (0.022 Source and Quake, 0.07 Valorant, 0.0066 Overwatch and
// modern Call of Duty). One source gives Fortnite 0.005515 instead of 0.005555 (0.7% apart); PUBG's curve is quoted
// at its default 80 field of view in third person.
// Plain math: a number in, a number out, no state, no allocation (safe on the game thread every usercmd).

#include "inputkit/sens_scale.h"

#include <algorithm>
#include <cmath>

namespace inputkit::sensscale {

namespace {

constexpr double kSourceYaw = 0.022;
constexpr double kPi = 3.14159265358979323846;

// Battlefield 6: a fixed base turn under the slider.
constexpr double kBf6Slope = 0.00173623, kBf6Base = 0.00231498;
// PUBG: ten times the turn for each 50 on the slider.
constexpr double kPubgScale = 0.004444, kPubgDecade = 50.0;

enum class Curve { kLinear, kAffine, kExponential };

struct Entry {
  Info info;
  Curve curve;
  double yaw;  // linear: degrees per count per slider unit; affine: the slope; exponential: the scale
};

// In Scale's order.
const Entry kEntries[kCount] = {
    {{"source", "World at War / Source (current)",
      "the CoD ports' own scale: Call of Duty 2, 4 and World at War on PC, Counter-Strike 2, Apex Legends, "
      "Team Fortress 2, Quake",
      0.001, 100.0, 0.01, ZoomModel::kGame, 0.0},
     Curve::kLinear, kSourceYaw},
    {{"cod_modern", "Modern Call of Duty",
      "Call of Duty: Modern Warfare (2019) and later, Warzone, Black Ops 6; aiming down the sights as its "
      "\"Relative\" setting (monitor distance, coefficient 1.33)",
      0.01, 100.0, 0.01, ZoomModel::kMonitorDistance, 1.33},
     Curve::kLinear, 0.0066},
    {{"valorant", "Valorant", "Valorant", 0.001, 10.0, 0.001, ZoomModel::kGame, 0.0}, Curve::kLinear, 0.07},
    {{"overwatch", "Overwatch 2 / Destiny 2", "Overwatch 2, Destiny 2", 0.01, 100.0, 0.01, ZoomModel::kGame, 0.0},
     Curve::kLinear, 0.0066},
    {{"fortnite", "Fortnite (percent)", "Fortnite: the X/Y percentage as shown (7.0% is 7.0)", 0.1, 100.0, 0.1,
      ZoomModel::kGame, 0.0},
     Curve::kLinear, 0.005555},
    {{"siege", "Rainbow Six Siege",
      "Rainbow Six Siege: the horizontal slider, with GameSettings.ini's MouseSensitivityMultiplierUnit at its "
      "0.02 default",
      1.0, 100.0, 1.0, ZoomModel::kGame, 0.0},
     Curve::kLinear, 0.00572958},
    {{"pubg", "PUBG: Battlegrounds", "PUBG: Battlegrounds: the general slider (third person, 80 field of view)", 1.0,
      100.0, 1.0, ZoomModel::kGame, 0.0},
     Curve::kExponential, kPubgScale},
    {{"battlefield6", "Battlefield 6",
      "Battlefield 6: infantry mouse aim sensitivity (0-200); aiming as Uniform Soldier Aiming (monitor distance, "
      "coefficient 1.33)",
      0.0, 200.0, 1.0, ZoomModel::kMonitorDistance, 1.33},
     Curve::kAffine, kBf6Slope},
    {{"halo_infinite", "Halo Infinite", "Halo Infinite", 0.1, 20.0, 0.1, ZoomModel::kGame, 0.0}, Curve::kLinear,
     0.0225},
    {{"tarkov", "Escape from Tarkov", "Escape from Tarkov (before its armor and weight slowdowns)", 0.01, 5.0, 0.01,
      ZoomModel::kGame, 0.0},
     Curve::kLinear, 0.125},
    {{"deadlock", "Deadlock", "Deadlock", 0.01, 50.0, 0.01, ZoomModel::kGame, 0.0}, Curve::kLinear, 0.044},
    {{"rust", "Rust", "Rust (at its default 75 field of view)", 0.01, 5.0, 0.01, ZoomModel::kGame, 0.0},
     Curve::kLinear, 0.1125},
};

const Entry& Get(Scale s) {
  const int i = static_cast<int>(s);
  return kEntries[(i >= 0 && i < kCount) ? i : 0];
}

bool Finite(double v) { return std::isfinite(v); }

}  // namespace

const Info& Describe(Scale s) { return Get(s).info; }

Scale FromIndex(int i) { return (i >= 0 && i < kCount) ? static_cast<Scale>(i) : Scale::kSource; }

Scale FromId(std::string_view id) {
  for (int i = 0; i < kCount; ++i)
    if (id == kEntries[i].info.id) return static_cast<Scale>(i);
  return Scale::kSource;
}

double DegreesPerCount(Scale s, double value) {
  const Entry& e = Get(s);
  if (!Finite(value) || value < 0.0) value = 0.0;
  switch (e.curve) {
    case Curve::kAffine:
      return e.yaw * value + kBf6Base;
    case Curve::kExponential:
      return e.yaw * std::pow(10.0, value / kPubgDecade);
    case Curve::kLinear:
    default:
      return e.yaw * value;
  }
}

double ToSource(Scale s, double value) {
  if (s == Scale::kSource) return (Finite(value) && value > 0.0) ? value : 0.0;  // exactly, no rounding trip
  return DegreesPerCount(s, value) / kSourceYaw;
}

double FromSource(Scale s, double source) {
  const Entry& e = Get(s);
  if (!Finite(source) || source < 0.0) source = 0.0;
  const double deg = source * kSourceYaw;
  double v;
  switch (e.curve) {
    case Curve::kAffine:
      v = (deg - kBf6Base) / e.yaw;  // under the base turn: the slider's bottom
      break;
    case Curve::kExponential:
      v = deg > 0.0 ? kPubgDecade * std::log10(deg / e.yaw) : e.info.min;
      break;
    case Curve::kLinear:
    default:
      v = deg / e.yaw;
      break;
  }
  if (s == Scale::kSource) v = source;  // exactly
  return std::clamp(v, e.info.min, e.info.max);
}

double Convert(Scale from, Scale to, double value) {
  if (from == to) return value;
  return FromSource(to, ToSource(from, value));
}

double MonitorDistanceRatio(double hip_vfov_deg, double zoom_vfov_deg, double coefficient) {
  if (!Finite(hip_vfov_deg) || !Finite(zoom_vfov_deg) || hip_vfov_deg <= 0.0 || hip_vfov_deg >= 180.0 ||
      zoom_vfov_deg <= 0.0 || zoom_vfov_deg >= 180.0)
    return 1.0;
  const double th = std::tan(hip_vfov_deg * kPi / 360.0), tz = std::tan(zoom_vfov_deg * kPi / 360.0);
  if (!Finite(coefficient) || coefficient <= 0.0) return tz / th;  // the limit at the center
  return std::atan(coefficient * tz) / std::atan(coefficient * th);
}

double VerticalFov(double hfov_deg, double aspect) {
  if (!Finite(hfov_deg) || !Finite(aspect) || aspect <= 0.0 || hfov_deg <= 0.0 || hfov_deg >= 180.0) return hfov_deg;
  return 2.0 * std::atan(std::tan(hfov_deg * kPi / 360.0) / aspect) * 180.0 / kPi;
}

double ZoomFovFromTangentRatio(double hip_fov_deg, double ratio) {
  if (!Finite(hip_fov_deg) || !Finite(ratio) || ratio <= 0.0 || hip_fov_deg <= 0.0 || hip_fov_deg >= 180.0)
    return hip_fov_deg;
  return 2.0 * std::atan(ratio * std::tan(hip_fov_deg * kPi / 360.0)) * 180.0 / kPi;
}

double AdsScale(const Ads& ads, double game_ratio, double hip_hfov_deg, double aspect) {
  // The hip, or numbers no game sends: leave the game's own value.
  if (!Finite(game_ratio) || game_ratio <= 0.0 || game_ratio >= 1.0) return game_ratio;
  if (!Finite(hip_hfov_deg) || hip_hfov_deg <= 0.0 || hip_hfov_deg >= 180.0) return game_ratio;
  double ratio = game_ratio;
  if (ads.model == ZoomModel::kMonitorDistance) {
    const double zoom = ZoomFovFromTangentRatio(hip_hfov_deg, game_ratio);
    ratio = MonitorDistanceRatio(VerticalFov(hip_hfov_deg, aspect), VerticalFov(zoom, aspect), ads.coefficient);
  }
  const double pct = Finite(ads.percent) ? std::clamp(ads.percent, kAdsMinPercent, kAdsMaxPercent) : 100.0;
  // How far into the zoom: 0 at the hip, 1 from kAdsBlendSpan under it on (no jump as the sights come up).
  const double into = std::clamp((1.0 - game_ratio) / kAdsBlendSpan, 0.0, 1.0);
  return ratio * (1.0 + (pct / 100.0 - 1.0) * into);
}

}  // namespace inputkit::sensscale
