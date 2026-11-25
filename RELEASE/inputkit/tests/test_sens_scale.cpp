// inputkit - other games' sensitivity scales (sens_scale.h) against the calculators' own worked examples, and the
// math's round trips. [input-port] 2026-11-24
#include "inputkit/inputkit.h"
#include "inputkit/sens_scale.h"

#include <cmath>
#include <cstdio>

using namespace inputkit::sensscale;

namespace {

int checks = 0, failures = 0;

void near(double got, double want, double tol, const char* what) {
  ++checks;
  if (!(std::fabs(got - want) <= tol)) {
    ++failures;
    std::printf("FAILED: %s: got %.6f, want %.6f (+/- %g)\n", what, got, want, tol);
  }
}

void is(bool ok, const char* what) {
  ++checks;
  if (!ok) {
    ++failures;
    std::printf("FAILED: %s\n", what);
  }
}

// cm for a full turn: 360 / (DPI x degrees per count) inches.
double Cm360(Scale s, double value, double dpi) { return 360.0 / (dpi * DegreesPerCount(s, value)) * 2.54; }

}  // namespace

int main() {
  // The current model is untouched: our number is the number.
  near(ToSource(Scale::kSource, 5.0), 5.0, 0.0, "source to source");
  near(FromSource(Scale::kSource, 2.75), 2.75, 0.0, "source from source");

  // The calculators' worked examples (recharge.com: CS2 2.0 is Valorant 0.628; west-games.com: CS2 2.0 at 800 DPI
  // is about 8% in Fortnite, 7.98% with its 0.5515 yaw, so 7.92% with the 0.5555 the others give).
  near(Convert(Scale::kSource, Scale::kValorant, 2.0), 0.6286, 0.0005, "CS2 2.0 -> Valorant");
  near(Convert(Scale::kSource, Scale::kFortnite, 2.0), 7.92, 0.01, "CS2 2.0 -> Fortnite %");
  // Overwatch's and modern Call of Duty's 0.0066 is 10/3 of Source's 0.022, and 0.07 / 0.0066 from Valorant.
  near(Convert(Scale::kSource, Scale::kCodModern, 1.5), 5.0, 1e-9, "Source 1.5 -> modern CoD 5.0");
  near(Convert(Scale::kValorant, Scale::kOverwatch, 0.5), 5.3030, 0.0001, "Valorant 0.5 -> Overwatch");
  // 1.0 in Source at 800 DPI is 51.95 cm a turn; the same in Deadlock is 0.5.
  near(Cm360(Scale::kSource, 1.0, 800), 51.95, 0.01, "Source 1.0 at 800 DPI, cm/360");
  near(Convert(Scale::kSource, Scale::kDeadlock, 1.0), 0.5, 1e-12, "Source 1.0 -> Deadlock");

  // The curves. Battlefield 6's default 20: 0.00173623 x 20 + 0.00231498.
  near(DegreesPerCount(Scale::kBattlefield6, 20.0), 0.03703958, 1e-8, "Battlefield 6 at 20");
  near(DegreesPerCount(Scale::kBattlefield6, 0.0), 0.00231498, 1e-12, "Battlefield 6 at 0 still turns");
  // PUBG: 50 on the slider is ten times 0, and 100 ten times 50.
  near(DegreesPerCount(Scale::kPubg, 50.0) / DegreesPerCount(Scale::kPubg, 0.0), 10.0, 1e-9, "PUBG 0 -> 50");
  near(DegreesPerCount(Scale::kPubg, 100.0) / DegreesPerCount(Scale::kPubg, 50.0), 10.0, 1e-9, "PUBG 50 -> 100");
  near(DegreesPerCount(Scale::kSiege, 10.0), 0.0572958, 1e-7, "Siege 10");

  // Every scale back and forth, across its range: the same number comes back.
  for (int i = 0; i < kCount; ++i) {
    const Scale s = FromIndex(i);
    const Info& info = Describe(s);
    for (int k = 0; k <= 10; ++k) {
      const double v = info.min + (info.max - info.min) * k / 10.0;
      char what[96];
      // Through the 0.022 scale unclamped (ToSource never clamps; Convert clamps to the target slider's range).
      std::snprintf(what, sizeof what, "%s %.3f round trip", info.id, v);
      near(FromSource(s, ToSource(s, v)), v, 1e-9 * (1.0 + v), what);
      std::snprintf(what, sizeof what, "%s %.3f turns forward", info.id, v);
      is(DegreesPerCount(s, v) >= 0.0, what);
    }
    is(FromId(info.id) == s, "id finds its scale");
    is(info.min < info.max && info.step > 0.0, "a usable range");
  }
  // Out of range: clamped to the scale's slider, never negative, never NaN.
  near(FromSource(Scale::kValorant, 1e9), Describe(Scale::kValorant).max, 0.0, "clamped high");
  near(FromSource(Scale::kBattlefield6, 0.0), 0.0, 0.0, "under Battlefield's base turn: its bottom");
  near(ToSource(Scale::kValorant, -3.0), 0.0, 0.0, "a negative value turns nothing");
  near(ToSource(Scale::kValorant, std::nan("")), 0.0, 0.0, "NaN turns nothing");
  is(FromId("no_such_game") == Scale::kSource, "unknown id keeps the current model");
  is(FromIndex(99) == Scale::kSource && FromIndex(-1) == Scale::kSource, "bad index keeps the current model");

  // Monitor distance matching.
  near(MonitorDistanceRatio(70.0, 70.0, 1.33), 1.0, 1e-12, "no zoom, no change");
  near(MonitorDistanceRatio(80.0, 40.0, 0.0), std::tan(20.0 * M_PI / 180.0) / std::tan(40.0 * M_PI / 180.0), 1e-12,
       "coefficient 0 is the ratio of the tangents");
  near(MonitorDistanceRatio(80.0, 40.0, 1e-9), MonitorDistanceRatio(80.0, 40.0, 0.0), 1e-9, "0 is the limit");
  {
    // The bigger the coefficient, the closer to the edge it matches and the faster zoomed (for a zoom in).
    const double a = MonitorDistanceRatio(80.0, 40.0, 0.5), b = MonitorDistanceRatio(80.0, 40.0, 1.33),
                 c = MonitorDistanceRatio(80.0, 40.0, 3.0);
    is(a < b && b < c && c < 1.0, "the coefficient moves the match outward");
    // At coefficient 1.33 with vertical fields of view, the match is at a 4:3 picture's edge: the angle there,
    // hip and zoomed, is the horizontal 4:3 field of view's half.
    const double vh = VerticalFov(80.0, 4.0 / 3.0), vz = VerticalFov(40.0, 4.0 / 3.0);
    near(MonitorDistanceRatio(vh, vz, 4.0 / 3.0), (40.0 / 2.0) / (80.0 / 2.0), 1e-9,
         "1.33 matches the 4:3 edge: the horizontal fields' ratio");
  }
  near(VerticalFov(90.0, 16.0 / 9.0), 58.7155, 0.0001, "90 horizontal at 16:9 is 58.7 vertical");
  near(MonitorDistanceRatio(0.0, 40.0, 1.33), 1.0, 0.0, "bad field of view: no change");

  // [input-port] aiming down the sights (AdsScale). CoD4 / WaW hand over tan(zoom/2) / tan(hip/2).
  {
    auto tr = [](double hip, double zoom) {
      return std::tan(zoom * M_PI / 360.0) / std::tan(hip * M_PI / 360.0);
    };
    near(ZoomFovFromTangentRatio(65.0, tr(65.0, 55.0)), 55.0, 1e-9, "the zoomed field of view back from the ratio");
    near(ZoomFovFromTangentRatio(65.0, 1.0), 65.0, 1e-9, "ratio 1 is the hip");
    Ads game;  // the game's own, 100%
    for (double r : {1.0, 0.99, 0.81, 0.3, 0.05}) near(AdsScale(game, r, 65.0, 4.0 / 3.0), r, 0.0, "kGame at 100%: as is");
    Ads md0{ZoomModel::kMonitorDistance, 0.0, 100.0};
    near(AdsScale(md0, tr(65.0, 20.0), 65.0, 4.0 / 3.0), tr(65.0, 20.0), 1e-12,
         "monitor distance 0 is what CoD4 / WaW already do");
    Ads md{ZoomModel::kMonitorDistance, 1.33, 100.0};
    const double want = MonitorDistanceRatio(VerticalFov(65.0, 4.0 / 3.0), VerticalFov(20.0, 4.0 / 3.0), 1.33);
    near(AdsScale(md, tr(65.0, 20.0), 65.0, 4.0 / 3.0), want, 1e-12, "monitor distance 1.33 from the game's ratio");
    is(want > tr(65.0, 20.0), "1.33 turns faster zoomed than the tangent ratio");
    near(AdsScale(md, 1.0, 65.0, 4.0 / 3.0), 1.0, 0.0, "the hip is never changed");
    near(AdsScale(md, 0.999999, 65.0, 4.0 / 3.0), 0.999999, 1e-5, "no jump as the zoom starts");
    double last = 1.0;
    bool falls = true;
    for (double z = 64.0; z >= 5.0; z -= 1.0) {
      const double v = AdsScale(md, tr(65.0, z), 65.0, 4.0 / 3.0);
      falls = falls && v < last;
      last = v;
    }
    is(falls, "monitor distance: more zoom, slower turn, all the way");
    Ads half{ZoomModel::kGame, 1.33, 50.0};
    near(AdsScale(half, 0.3, 65.0, 4.0 / 3.0), 0.15, 1e-12, "50% at full zoom halves it");
    near(AdsScale(half, 0.95, 65.0, 4.0 / 3.0), 0.95 * 0.75, 1e-12, "half way into the blend: half the change");
    Ads big{ZoomModel::kGame, 1.33, 500.0};
    near(AdsScale(big, 0.3, 65.0, 4.0 / 3.0), 0.6, 1e-12, "the percentage stops at 200");
    near(AdsScale(md, 0.5, 0.0, 4.0 / 3.0), 0.5, 0.0, "a bad cg_fov: the game's own");
  }

  // The C interface says the same.
  near(ik_sens_convert(0, 2, 2.0), Convert(Scale::kSource, Scale::kValorant, 2.0), 0.0, "C convert");
  is(ik_sens_scale_from_id("cod_modern") == 1 && ik_sens_scale_from_id(nullptr) == 0, "C ids");
  {
    double coef = 0.0;
    is(ik_sens_scale_zoom(1, &coef) == IK_ZOOM_MONITOR_DISTANCE && coef == 1.33, "C modern CoD zoom");
  }
  is(IK_SENS_SCALE_COUNT == kCount, "C count");

  std::printf("%d checks, %d failed\n", checks, failures);
  return failures ? 1 : 0;
}
