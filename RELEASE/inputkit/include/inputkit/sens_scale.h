// inputkit - other games' mouse sensitivity scales: type in the number you use in another shooter and turn the same
// distance per inch of mouse here.
//
// [input-port] 2026-11-24 (owner: "Please add other games' senstivity scaling. Please do the top 10 shooters that
// differ in their equations. You should be able to find info on this from mouse-senstivity.com", then "please also
// add the modern-day call of duty sensitivity scale as an option in addition to the others and our current
// WAW/source model.")
//
// Every game here turns the view by sensitivity x degrees per count. What differs from game to game is the curve
// from the number on the slider to degrees per mouse count (the "yaw"):
//   linear      deg = yaw x s                     (Source, Valorant, Overwatch, modern Call of Duty, ...)
//   percent     deg = yaw x s, s in percent        (Fortnite: 7.0% is 7.0 here)
//   affine      deg = a x s + b                    (Battlefield 6: a fixed base turn under the slider)
//   exponential deg = a x 10^(s / 50)              (PUBG: each 50 on the slider is ten times the turn)
// The constants are the ones the sensitivity calculators publish (mouse-sensitivity.com's community tables, and the
// same numbers in the converters that copy them; see sens_scale.cpp for each one's source and how sure it is). They
// are facts about those games' math only: nothing of any other game's code is here.
//
// Our Call of Duty ports already turn by sensitivity x 0.022 degrees a count (the PC games' m_yaw; the Source
// engine's number too). So a game takes the player's number in the scale they picked, asks ToSource() for the
// equivalent number on its own 0.022 scale, and feeds that to its turn as before. Hip fire then matches the other
// game's turn exactly (the same centimeters for a full turn at the same DPI).
//
// Aiming down the sights: by default each game keeps its own zoom scaling (World at War's: the zoomed field of view
// over the normal one). The modern Call of Duty and Battlefield scales bring their own instead, "monitor distance"
// matching (MonitorDistanceRatio), as those games do by default (coefficient 1.33; modern Call of Duty's
// "Relative" aim-down-sights setting; its "Legacy" one is kGame). A game may let the player pick
// either for any scale (ZoomModel and the coefficient are separate settings).

#pragma once

#include <string_view>

namespace inputkit::sensscale {

// Kept in this order for good: games store the id string, never the number, but tests and the C interface count
// on it. Add new ones at the end.
enum class Scale : int {
  kSource = 0,    // our Call of Duty ports, Counter-Strike 2, Apex Legends, Quake (0.022) - the current model
  kCodModern,     // Call of Duty: Modern Warfare (2019) and later, Warzone, Black Ops 6 (0.0066)
  kValorant,      // 0.07
  kOverwatch,     // Overwatch 2, Destiny 2 (0.0066)
  kFortnite,      // percent slider (0.005555 per 1%)
  kSiege,         // Rainbow Six Siege (0.00572958 per slider step, multiplier unit 0.02)
  kPubg,          // PUBG: Battlegrounds (0.004444 x 10^(s/50))
  kBattlefield6,  // Battlefield 6 (0.00173623 x s + 0.00231498)
  kHaloInfinite,  // 0.0225
  kTarkov,        // Escape from Tarkov (0.125)
  kDeadlock,      // 0.044
  kRust,          // 0.1125
};
inline constexpr int kCount = 12;

// How aiming down the sights scales the turn.
enum class ZoomModel : int {
  kGame = 0,         // the game's own (the zoomed field of view over the normal one, in the CoD ports)
  kMonitorDistance,  // keep the same mouse distance to a point `coefficient` of the way to the screen's edge
};

struct Info {
  const char* id;      // the setting's text: "source", "cod_modern", ...
  const char* label;   // for a menu: "Valorant"
  const char* games;   // the games that share the scale, for a menu's help text
  double min, max;     // the slider's range in that game's own units
  double step;         // the slider's step
  ZoomModel zoom;      // the zoom model the scale's own games use by default
  double coefficient;  // and its coefficient (monitor distance only)
};

const Info& Describe(Scale s);
// "valorant" -> kValorant; anything unknown -> kSource (the current model), so an old or mistyped setting keeps the
// game's turn as it was.
Scale FromId(std::string_view id);
// Within 0..kCount-1, else kSource.
Scale FromIndex(int i);

// Degrees of turn for one mouse count at the slider value `value` (0 or above; a value under the scale's minimum
// turns as little as the scale allows, never backward).
double DegreesPerCount(Scale s, double value);
// The same turn as `value` in scale `s`, on our 0.022 scale. ToSource(kSource, v) == v.
double ToSource(Scale s, double value);
// The inverse: the number to type into scale `s` for the turn of `source` on the 0.022 scale (clamped to the
// scale's range).
double FromSource(Scale s, double source);
// From one scale to another (what a menu does when the player switches scale and wants to keep their turn).
double Convert(Scale from, Scale to, double value);

// The zoom multiplier that keeps the mouse distance to a point the same zoomed and unzoomed: the point `coefficient`
// half-heights of the screen from its center (Call of Duty's measure: 1.33 is the edge of a 4:3 picture, its and
// Battlefield's default; 0 is the limit at the center, the ratio of the tangents, "focal length" matching). Both
// fields of view VERTICAL, in degrees (VerticalFov turns a horizontal one into it). Returns 1 when either is out of
// range (not between 0 and 180).
double MonitorDistanceRatio(double hip_vfov_deg, double zoom_vfov_deg, double coefficient);
// A horizontal field of view over a picture `aspect` wide for each unit of height (4:3 is 1.3333) as a vertical one.
// The CoD ports' cg_fov is horizontal at 4:3: VerticalFov(cg_fov, 4.0 / 3.0).
double VerticalFov(double hfov_deg, double aspect);

// [input-port] 2026-11-24 aiming down the sights in the Call of Duty ports (owner's list: the ADS multiplier, and
// monitor distance for the modern Call of Duty and Battlefield scales). Call of Duty 4 and World at War scale the turn
// while zoomed by the ratio of the tangents of the half angles, tan(zoomed / 2) / tan(normal / 2), with cg_fov as the
// normal field of view (horizontal at 4:3); read in their generated code (CoD4 0x822E15A0, WaW 0x820F70F0). That is
// monitor distance 0 ("focal length"). A game hands AdsScale the ratio it computed and its cg_fov, and gives the game
// back what AdsScale returns in its place.
struct Ads {
  ZoomModel model = ZoomModel::kGame;  // kGame: keep the game's ratio
  double coefficient = 1.33;           // monitor distance only (half-heights of the screen, as MonitorDistanceRatio)
  double percent = 100.0;              // the zoomed turn on top of the model, 0-200; 100 changes nothing
};
inline constexpr double kAdsMinPercent = 0.0;
inline constexpr double kAdsMaxPercent = 200.0;
// The percentage comes in over the first part of the zoom: fully applied once the game's ratio is this far under 1
// (a 65 degree cg_fov zoomed to 59 already is). It depends on the zoom only, never on time or the frame rate.
inline constexpr double kAdsBlendSpan = 0.1;

// The zoomed field of view a tangent ratio stands for: tan(zoom / 2) = ratio x tan(hip / 2), in hip's unit
// (horizontal or vertical alike: the ratio is the same either way). Returns hip for a ratio that isn't above 0.
double ZoomFovFromTangentRatio(double hip_fov_deg, double ratio);
// What to give the game in place of its own zoom ratio `game_ratio` (a tangent ratio, 1 at the hip), its normal
// field of view `hip_hfov_deg` horizontal over a picture `aspect` wide for each unit of height. A ratio of 1 or
// more (the hip; aim-assist never raises it) comes back untouched, and so does anything out of range.
double AdsScale(const Ads& ads, double game_ratio, double hip_hfov_deg, double aspect);

}  // namespace inputkit::sensscale
