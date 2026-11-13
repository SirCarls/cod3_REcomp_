// f2 - mouse acceleration, the sensitivity randomizer and the sensitivity scales, inside the game only. [f2-menus]
//
// Moved out of f2_common.cpp on 2026-11-11 03:45 (owner 03:05: "This will be needed within cod3 as well. Same goes
// for raw accel"), unchanged but for the rows, which are drawn with the caller's own widgets (f2_accel.h): nothing
// of the F2 window is used here, so Call of Duty 3 (recomp/) builds this file as the other ports do.

#include "f2/f2_accel.h"

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <mutex>
#include <random>
#include <string>
#include <utility>

#include <imgui.h>

#include <rex/cvar.h>

// [f2-menus] 2026-11-11 04:46 (owner 04:40: "Please help with BRo") Big Red One builds this file too, and it is not on
// the rexglue SDK: its own rex/cvar.h (bro/src/f2/shim) has the setting macros, and it has no rex/logging.h, so every
// log line is made here as plain text and handed to LogLine: the SDK's log where there is one, else the port's own
// f2_accel_log (Big Red One: bro_f2.cpp). The file keeps to C++17 for the same reason.
// [rexglue-native] 2026-11-11 (release 2.41.0, BRO's build): the SDK's rex/logging.h can be on Big Red One's
// include path too (the setup tool builds BRO next to CoD3's SDK) without the SDK's spdlog/fmt settings, so BRO's
// own rex/cvar.h (it defines REXCVAR_SHIM_DEFINE) keeps it on the plain-text path below.
#if !defined(REXCVAR_SHIM_DEFINE) && __has_include(<rex/logging.h>)
#include <rex/logging.h>
#define F2_ACCEL_SDK_LOG 1
#else
extern "C" void f2_accel_log(const char* text);
#endif

// [f2-menus] 2026-11-11 02:55 (owner 02:14: "We also want to replicate raw accel for the users who want it, but ONLY
// in-game. Raw accell is system-wide and can trigger anti-cheat issues with other games. We want ours to be in-game
// ONLY."). Raw Accel's curves (github.com/RawAccelOfficial/rawaccel, MIT licence; its common/accel-*.hpp and
// rawaccel.hpp read for the formulas, its defaults kept) applied to the counts this port has already read for its
// own mouse look: no driver, nothing outside the game's process, off until the player turns it on.
REXCVAR_DEFINE_BOOL(f2_accel, false, "f2/Mouse",
                    "Mouse acceleration inside this game only (Raw Accel's curves, no driver): the faster the mouse "
                    "moves, the more each count turns the view");
REXCVAR_DEFINE_INT32(f2_accel_mode, 0, "f2/Mouse",
                     "Acceleration curve: 0 linear, 1 classic, 2 power, 3 natural, 4 jump, 5 synchronous (Raw Accel's)")
    .range(0, 5);
REXCVAR_DEFINE_BOOL(f2_accel_gain, true, "f2/Mouse",
                    "The curve shapes the gain (Raw Accel's default, smoother); off: it shapes the sensitivity (legacy)");
REXCVAR_DEFINE_DOUBLE(f2_accel_acceleration, 0.005, "f2/Mouse", "Linear and classic: the acceleration").range(0.0, 10.0);
REXCVAR_DEFINE_DOUBLE(f2_accel_exponent_classic, 2.0, "f2/Mouse", "Classic: the exponent (2 is linear)").range(1.01, 10.0);
REXCVAR_DEFINE_DOUBLE(f2_accel_exponent_power, 0.05, "f2/Mouse", "Power: the exponent").range(0.001, 10.0);
REXCVAR_DEFINE_DOUBLE(f2_accel_scale, 1.0, "f2/Mouse", "Power: the scale").range(0.0001, 100.0);
REXCVAR_DEFINE_DOUBLE(f2_accel_output_offset, 0.0, "f2/Mouse",
                      "Power: the sensitivity the curve starts from (0 = from nothing)")
    .range(0.0, 10.0);
REXCVAR_DEFINE_DOUBLE(f2_accel_input_offset, 0.0, "f2/Mouse",
                      "Linear, classic and natural: the speed (counts a millisecond) below which nothing is added")
    .range(0.0, 200.0);
REXCVAR_DEFINE_INT32(f2_accel_cap_mode, 0, "f2/Mouse",
                     "Linear, classic and power: what caps the curve: 0 the output cap, 1 the input cap, 2 both")
    .range(0, 2);
REXCVAR_DEFINE_DOUBLE(f2_accel_cap_input, 15.0, "f2/Mouse",
                      "The input cap, a speed in counts a millisecond; jump: the speed of the step")
    .range(0.0, 500.0);
REXCVAR_DEFINE_DOUBLE(f2_accel_cap_output, 1.5, "f2/Mouse",
                      "The output cap, a sensitivity multiple (0 = no cap); jump: the multiple after the step")
    .range(0.0, 20.0);
REXCVAR_DEFINE_DOUBLE(f2_accel_decay_rate, 0.1, "f2/Mouse", "Natural: how fast the curve nears its limit").range(0.0001, 10.0);
REXCVAR_DEFINE_DOUBLE(f2_accel_limit, 1.5, "f2/Mouse", "Natural: the sensitivity multiple the curve nears").range(0.0, 20.0);
REXCVAR_DEFINE_DOUBLE(f2_accel_smooth, 0.5, "f2/Mouse",
                      "Jump and synchronous: how soft the change is (0 = at once)")
    .range(0.0, 1.0);
REXCVAR_DEFINE_DOUBLE(f2_accel_sync_speed, 5.0, "f2/Mouse",
                      "Synchronous: the speed (counts a millisecond) at which the sensitivity is unchanged")
    .range(0.01, 500.0);
REXCVAR_DEFINE_DOUBLE(f2_accel_motivity, 1.5, "f2/Mouse",
                      "Synchronous: the sensitivity goes from 1 / this to this")
    .range(1.01, 20.0);
REXCVAR_DEFINE_DOUBLE(f2_accel_gamma, 1.0, "f2/Mouse", "Synchronous: how fast the change comes").range(0.01, 20.0);
REXCVAR_DEFINE_DOUBLE(f2_accel_sens, 1.0, "f2/Mouse", "Sensitivity multiplier after the curve").range(0.01, 20.0);
REXCVAR_DEFINE_DOUBLE(f2_accel_yx_ratio, 1.0, "f2/Mouse", "Up / down sensitivity as a multiple of left / right").range(0.01, 20.0);
REXCVAR_DEFINE_INT32(f2_accel_dpi, 0, "f2/Mouse",
                     "The mouse's DPI, so speeds mean the same on any mouse (as Raw Accel: counts are scaled to 1000 "
                     "DPI); 0 = not set, counts as they come")
    .range(0, 64000);
REXCVAR_DEFINE_BOOL(f2_accel_whole, true, "f2/Mouse",
                    "The curve takes the whole motion's speed; off: side to side and up / down each by its own");
// [f2-menus] 2026-11-11 03:10 (owner 03:02: "this will be needed as well. It's similar to rawaccel
// https://github.com/Whisperrr/SensitivityRandomizer"). A sensitivity that wanders around a baseline over time, for
// aim training, with that program's settings (type, baseline, min, max, spread, smoothing, timestep, runtime, pause,
// a live readout). Its repository states no licence and its README doesn't say how its curve is made, so the
// generator below is this project's own; like the acceleration it acts only on this game's own mouse look.
REXCVAR_DEFINE_BOOL(f2_sens_random, false, "f2/Mouse",
                    "Sensitivity randomizer inside this game only: the mouse sensitivity wanders around a baseline "
                    "over time (aim training)");
REXCVAR_DEFINE_INT32(f2_sens_random_type, 1, "f2/Mouse",
                     "Sensitivity randomizer: 0 steps (a new value every timestep), 1 smooth (always moving)")
    .range(0, 1);
REXCVAR_DEFINE_DOUBLE(f2_sens_random_baseline, 1.0, "f2/Mouse",
                      "Sensitivity randomizer: the multiple it wanders around (1 = your own sensitivity)")
    .range(0.1, 10.0);
REXCVAR_DEFINE_DOUBLE(f2_sens_random_min, 0.5, "f2/Mouse", "Sensitivity randomizer: the smallest multiple").range(0.05, 10.0);
REXCVAR_DEFINE_DOUBLE(f2_sens_random_max, 2.0, "f2/Mouse", "Sensitivity randomizer: the largest multiple").range(0.05, 20.0);
REXCVAR_DEFINE_DOUBLE(f2_sens_random_spread, 0.6, "f2/Mouse",
                      "Sensitivity randomizer: how far from the baseline it usually goes (0.1 = barely)")
    .range(0.0, 3.0);
REXCVAR_DEFINE_INT32(f2_sens_random_smoothing, 2, "f2/Mouse",
                     "Sensitivity randomizer: 0 no smoothing to 4 the slowest, smoothest changes")
    .range(0, 4);
REXCVAR_DEFINE_DOUBLE(f2_sens_random_timestep, 10.0, "f2/Mouse",
                      "Sensitivity randomizer, steps: seconds between two values")
    .range(0.25, 600.0);
REXCVAR_DEFINE_DOUBLE(f2_sens_random_runtime, 30.0, "f2/Mouse",
                      "Sensitivity randomizer: minutes it runs after it is turned on, then the baseline (0 = no end)")
    .range(0.0, 600.0);
REXCVAR_DEFINE_BOOL(f2_sens_random_paused, false, "f2/Mouse",
                    "Sensitivity randomizer: paused (the multiple and the time left stay where they are)");
REXCVAR_DEFINE_BOOL(f2_sens_random_show, true, "f2/Mouse",
                    "Sensitivity randomizer: show the live multiple and the time left in the top left corner");
REXCVAR_DEFINE_INT32(f2_sens_random_seed, 0, "f2/Mouse",
                     "Sensitivity randomizer: a number that makes the same sequence every time (tests); 0 = a new one "
                     "each time")
    .range(0, 2000000000);
REXCVAR_DEFINE_INT32(f2_sens_random_log, 0, "f2/Mouse",
                     "Debug: log the randomizer's multiple every N seconds, with the smallest and largest so far "
                     "(0 = never)")
    .range(0, 3600);

// [f2-menus] 2026-11-11 03:20 (owner 03:09: "We also need to support different sensitivity equations.
// Overwatch/moderncod, valorant, and any others you feel are worth adding."). The sensitivity typed as another game
// counts it: each game turns a fixed number of degrees for one mouse count at sensitivity 1 (its "yaw"), so the same
// turn here is that sensitivity x its yaw / this game's yaw. The yaws: Quake, Source, Counter-Strike 2, Apex and the
// older Call of Duty PC games 0.022; Valorant 0.07; Overwatch 2, the modern Call of Duty games and Destiny 2 0.0066;
// Rainbow Six Siege 0.00572957795; Fortnite 0.005555 (its percent); Battlefield V / 2042 0.0055555; Deadlock 0.044.
REXCVAR_DEFINE_INT32(f2_sens_scale, 0, "f2/Mouse",
                     "Type the mouse sensitivity as another game counts it: 0 this game's own, 1 Quake / Source / "
                     "Counter-Strike 2 / Apex, 2 Valorant, 3 Overwatch 2 / modern Call of Duty / Destiny 2, 4 Rainbow "
                     "Six Siege, 5 Fortnite, 6 Battlefield V / 2042, 7 Deadlock, 8 centimetres for a full turn (with "
                     "f2_sens_dpi)")
    .range(0, 8);
REXCVAR_DEFINE_DOUBLE(f2_sens_value, 1.0, "f2/Mouse",
                      "The sensitivity on f2_sens_scale's scale, or the centimetres for a full turn")
    .range(0.001, 1000.0);
REXCVAR_DEFINE_INT32(f2_sens_dpi, 800, "f2/Mouse", "The mouse's DPI, for centimetres for a full turn").range(50, 64000);

REXCVAR_DEFINE_BOOL(f2_accel_debug, false, "f2/Mouse",
                    "Debug: log once a second the speed the curve saw and the multiple it gave (tests)");

namespace f2 {

// ------------------------------------------------------------------------------------------------- mouse acceleration
// [f2-menus] 2026-11-11 02:55 Raw Accel's curves inside the game (see the settings above). x is the speed in counts a
// millisecond (after the DPI scaling); each curve gives the multiple the counts are turned by. The formulas are
// Raw Accel's (MIT): classic and linear from accel-classic.hpp, power from accel-power.hpp, natural from
// accel-natural.hpp, jump from accel-jump.hpp, synchronous from accel-synchronous.hpp; the steps around them
// (speed from counts and time, the time clamp 0.0625 to 100 ms, whole or by component, sensitivity, Y/X) from
// rawaccel.hpp. Left out: its look-up table mode, rotation, snap, speed clamps, smoothing and the domain / range /
// direction weights.
namespace accel {
namespace {

void LogLine(const char* text) {
#if defined(F2_ACCEL_SDK_LOG)
  REXLOG_INFO("{}", text);
#else
  f2_accel_log(text);
#endif
}

std::atomic<bool> g_supported{false};
float g_corner_top = 0.0f;  // where other corner lines ended this frame (SetCornerTop); UI thread

bool HasSetting(const char* name) { return rex::cvar::GetFlagInfo(name) != nullptr; }

struct Args {
  int mode = 0;  // 0 linear, 1 classic, 2 power, 3 natural, 4 jump, 5 synchronous
  bool gain = true;
  double input_offset = 0, output_offset = 0, acceleration = 0.005, decay_rate = 0.1, gamma = 1, motivity = 1.5;
  double exponent_classic = 2, scale = 1, exponent_power = 0.05, limit = 1.5, sync_speed = 5, smooth = 0.5;
  double cap_x = 15, cap_y = 1.5;
  int cap_mode = 0;  // 0 out, 1 in, 2 both
};

Args Read() {
  Args a;
  a.mode = REXCVAR_GET(f2_accel_mode);
  a.gain = REXCVAR_GET(f2_accel_gain);
  a.input_offset = REXCVAR_GET(f2_accel_input_offset);
  a.output_offset = REXCVAR_GET(f2_accel_output_offset);
  a.acceleration = REXCVAR_GET(f2_accel_acceleration);
  a.decay_rate = REXCVAR_GET(f2_accel_decay_rate);
  a.gamma = REXCVAR_GET(f2_accel_gamma);
  a.motivity = REXCVAR_GET(f2_accel_motivity);
  a.exponent_classic = REXCVAR_GET(f2_accel_exponent_classic);
  a.scale = REXCVAR_GET(f2_accel_scale);
  a.exponent_power = REXCVAR_GET(f2_accel_exponent_power);
  a.limit = REXCVAR_GET(f2_accel_limit);
  a.sync_speed = REXCVAR_GET(f2_accel_sync_speed);
  a.smooth = REXCVAR_GET(f2_accel_smooth);
  a.cap_x = REXCVAR_GET(f2_accel_cap_input);
  a.cap_y = REXCVAR_GET(f2_accel_cap_output);
  a.cap_mode = REXCVAR_GET(f2_accel_cap_mode);
  return a;
}

// accel-classic.hpp. Linear is classic with exponent 2.
double Classic(double x, const Args& a, double p) {
  p = std::max(p, 1.01);
  const double o = std::max(a.input_offset, 0.0);
  double accel = std::max(a.acceleration, 0.0);
  double raised = std::pow(accel, p - 1.0);
  auto base = [&](double v) { return raised * std::pow(v - o, p) / v; };
  double sign = 1.0;
  if (!a.gain) {  // legacy: the sensitivity itself is capped
    double cap = DBL_MAX;
    if (a.cap_mode == 2 && a.cap_x > o && a.cap_y > 0.0) {
      cap = std::abs(a.cap_y - 1.0);
      if (a.cap_y < 1.0) sign = -1.0;
      raised = a.cap_x * cap / std::pow(a.cap_x - o, p);
    } else if (a.cap_mode == 1 && a.cap_x > o) {
      cap = base(a.cap_x);
    } else if (a.cap_mode == 0 && a.cap_y > 0.0) {
      cap = std::abs(a.cap_y - 1.0);
      if (a.cap_y < 1.0) sign = -1.0;
    }
    if (x <= o) return 1.0;
    return 1.0 + sign * std::min(base(x), cap);
  }
  // gain: past the cap the output goes on in a straight line
  double cap_x = DBL_MAX, cap_gain = 0.0;
  bool capped = false;
  if (a.cap_mode == 2 && a.cap_x > o && a.cap_y > 0.0) {
    cap_gain = std::abs(a.cap_y - 1.0);
    if (a.cap_y < 1.0) sign = -1.0;
    cap_x = a.cap_x;
    accel = std::pow(cap_gain / p, 1.0 / (p - 1.0)) / (cap_x - o);
    raised = std::pow(accel, p - 1.0);
    capped = true;
  } else if (a.cap_mode == 1 && a.cap_x > o) {
    cap_x = a.cap_x;
    cap_gain = p * std::pow(accel * (cap_x - o), p - 1.0);
    capped = true;
  } else if (a.cap_mode == 0 && a.cap_y > 0.0) {
    cap_gain = std::abs(a.cap_y - 1.0);
    if (a.cap_y < 1.0) sign = -1.0;
    if (cap_gain <= 0.0 || accel <= 0.0) return 1.0;  // a cap of 1: nothing is added
    cap_x = o + std::pow(cap_gain / p, 1.0 / (p - 1.0)) / accel;
    capped = true;
  }
  if (x <= o) return 1.0;
  if (!capped || x < cap_x) return 1.0 + sign * base(x);
  const double constant = (base(cap_x) - cap_gain) * cap_x;
  return 1.0 + sign * (constant / x + cap_gain);
}

// accel-power.hpp. The result is the multiple itself (nothing is added to 1).
double Power(double x, const Args& a) {
  const double n = std::max(a.exponent_power, 0.001);
  const double y0 = std::max(a.output_offset, 0.0);
  const bool io = a.cap_mode == 2 && a.cap_x > 0.0 && a.cap_y > 0.0;
  double s = std::max(a.scale, 0.0001);
  if (io) s = (a.gain ? std::pow(a.cap_y / (n + 1.0), 1.0 / n) : std::pow(a.cap_y, 1.0 / n)) / a.cap_x;
  double x0 = 0.0, constant = 0.0;
  if (!(io && !a.gain) && y0 > 0.0) {
    x0 = std::pow(y0 / (n + 1.0), 1.0 / n) / s;
    constant = x0 * y0 * n / (n + 1.0);
  }
  auto base = [&](double v) { return v <= x0 ? y0 : std::pow(s * v, n) + constant / v; };
  if (x <= 0.0) return y0;
  if (!a.gain) {
    double cap = DBL_MAX;
    if (io) cap = a.cap_y;
    else if (a.cap_mode == 1 && a.cap_x > 0.0) cap = base(a.cap_x);
    else if (a.cap_mode == 0 && a.cap_y > 0.0) cap = a.cap_y;
    return std::min(base(x), cap);
  }
  double cap_x = DBL_MAX, cap_y = 0.0;
  bool capped = false;
  if (io) {
    cap_x = a.cap_x;
    cap_y = a.cap_y;
    capped = true;
  } else if (a.cap_mode == 1 && a.cap_x > 0.0) {
    if (a.cap_x <= x0) return y0;  // capped below where the curve starts: flat
    cap_x = a.cap_x;
    cap_y = (n + 1.0) * std::pow(s * cap_x, n);
    capped = true;
  } else if (a.cap_mode == 0 && a.cap_y > 0.0) {
    cap_x = std::pow(a.cap_y / (n + 1.0), 1.0 / n) / s;
    cap_y = a.cap_y;
    capped = true;
  }
  if (!capped || x < cap_x) return base(x);
  return cap_y + (base(cap_x) - cap_y) * cap_x / x;
}

// accel-natural.hpp.
double Natural(double x, const Args& a) {
  const double o = std::max(a.input_offset, 0.0);
  const double limit = a.limit - 1.0;
  if (x <= o || limit == 0.0 || a.decay_rate <= 0.0) return 1.0;
  const double accel = a.decay_rate / std::abs(limit);
  const double decay = std::exp(accel * (o - x));
  if (!a.gain) return 1.0 + limit * (x - o) * (1.0 - decay) / x;
  return 1.0 + limit * ((decay - 1.0) / accel + (x - o)) / x;
}

// ln(1 + e^z) without overflow.
double Log1pExp(double z) { return z > 30.0 ? z : std::log1p(std::exp(z)); }

// accel-jump.hpp. cap_x is the speed of the step, cap_y the multiple after it.
double Jump(double x, const Args& a) {
  const double step = a.cap_x, height = a.cap_y - 1.0;
  const double rate = a.smooth * step < 1.0 ? 0.0 : 2.0 * 3.14159265358979323846 / (a.smooth * step);
  if (!a.gain) {
    if (rate > 0.0) return 1.0 + height / (1.0 + std::exp(rate * (step - x)));
    return x < step ? 1.0 : 1.0 + height;
  }
  if (x <= 0.0) return 1.0;
  if (rate > 0.0) {
    const double integral = height * (x + Log1pExp(rate * (step - x)) / rate);
    const double constant = -height * Log1pExp(rate * step) / rate;
    return 1.0 + (integral + constant) / x;
  }
  return x < step ? 1.0 : 1.0 + height * (x - step) / x;
}

// accel-synchronous.hpp, its sensitivity form.
double SynchronousLegacy(double x, const Args& a) {
  const double m = std::max(a.motivity, 1.01), s = std::max(a.sync_speed, 0.01), g = std::max(a.gamma, 0.01);
  if (x <= 0.0) return 1.0 / m;
  const double sharpness = a.smooth <= 0.0 ? 16.0 : 0.5 / a.smooth;
  if (sharpness >= 16.0) return std::clamp(std::pow(x / s, g), 1.0 / m, m);
  if (x == s) return 1.0;
  const double log_ratio = std::log(x / s);
  const double l = g * std::abs(log_ratio) / std::log(m);
  const double e = std::copysign(std::pow(std::tanh(std::pow(l, sharpness)), 1.0 / sharpness), log_ratio);
  return std::pow(m, e);
}

// Its gain form: the average of the sensitivity form from 0 to x. Raw Accel keeps a table of the running integral,
// eight points an octave from 1/8 to 512 counts a millisecond; so does this, one a thread, made again when a
// setting changes.
double SynchronousGain(double x, const Args& a) {
  constexpr int kFirst = -3, kLast = 9, kPerOctave = 8, kPoints = (kLast - kFirst) * kPerOctave + 1;
  struct Table {
    double motivity = 0, sync_speed = 0, gamma = 0, smooth = -1;
    double at[kPoints] = {}, integral[kPoints] = {};
  };
  thread_local Table table;
  if (table.motivity != a.motivity || table.sync_speed != a.sync_speed || table.gamma != a.gamma ||
      table.smooth != a.smooth) {
    table.motivity = a.motivity;
    table.sync_speed = a.sync_speed;
    table.gamma = a.gamma;
    table.smooth = a.smooth;
    double from = 0.0, sum = 0.0;
    for (int i = 0; i < kPoints; ++i) {
      const double to = std::pow(2.0, kFirst + double(i) / kPerOctave);
      const int pieces = i == 0 ? 32 : 4;  // midpoints of each piece
      const double width = (to - from) / pieces;
      for (int k = 0; k < pieces; ++k) sum += SynchronousLegacy(from + (k + 0.5) * width, a) * width;
      table.at[i] = to;
      table.integral[i] = sum;
      from = to;
    }
  }
  if (x <= table.at[0]) return table.integral[0] / table.at[0];
  if (x >= table.at[kPoints - 1])
    return (table.integral[kPoints - 1] + (x - table.at[kPoints - 1]) * SynchronousLegacy(x, a)) / x;
  int i = static_cast<int>((std::log2(x) - kFirst) * kPerOctave);
  i = std::clamp(i, 0, kPoints - 2);
  while (i > 0 && table.at[i] > x) --i;
  while (i < kPoints - 2 && table.at[i + 1] < x) ++i;
  const double share = (x - table.at[i]) / (table.at[i + 1] - table.at[i]);
  return (table.integral[i] + share * (table.integral[i + 1] - table.integral[i])) / x;
}

double Curve(double x, const Args& a) {
  double m = 1.0;
  switch (a.mode) {
    case 0: m = Classic(x, a, 2.0); break;
    case 1: m = Classic(x, a, a.exponent_classic); break;
    case 2: m = Power(x, a); break;
    case 3: m = Natural(x, a); break;
    case 4: m = Jump(x, a); break;
    case 5: m = a.gain ? SynchronousGain(x, a) : SynchronousLegacy(x, a); break;
    default: break;
  }
  if (!std::isfinite(m)) return 1.0;      // a setting the formula can't take: no acceleration
  return std::clamp(m, 0.0, 1000.0);
}

// ---- the sensitivity randomizer (see the f2_sens_random settings). Tick, once a UI frame, reads the settings;
// the run itself moves in fixed steps of real time, from Tick and from every mouse event (AdvanceTo), so the game's
// frame rate has no part in it. Steps: every timestep a new value, baseline x e^(spread x a normal number), drawn
// again while it falls outside min..max, reached over a short blend (smoothing x 0.25 s). Smooth: the logarithm of
// the multiple is a mean-reverting random walk (it wanders, and is pulled back toward the baseline) whose usual
// distance from the baseline is the spread and whose pace is set by the smoothing (it takes about 0.5, 1, 2, 4 or
// 8 seconds to forget where it was), passed through a second, short filter so it has no corners.
std::atomic<float> g_random_multiple{1.0f};

struct Randomizer {
  bool on = false, finished = false;
  std::mt19937 rng;
  std::normal_distribution<double> normal{0.0, 1.0};
  double elapsed = 0.0;      // seconds it has run (pauses don't count)
  double walk = 0.0, value = 0.0;  // smooth: the walk and its filtered value, as logarithms around the baseline
  double from = 1.0, to = 1.0, since_step = 0.0;  // steps
  double multiple = 1.0, low = 1.0, high = 1.0;
  std::chrono::steady_clock::time_point last{}, last_log{};
  double pending = 0.0;      // real time not yet turned into steps
  int64_t steps = 0;         // steps of kRandomStep it has run
};
Randomizer g_randomizer;
// [f2-menus] 2026-11-11 03:40 the run is moved from the UI thread and from the thread mouse events come on: the
// mutex guards g_randomizer and the settings Tick last read for it.
std::mutex g_random_mutex;
std::atomic<bool> g_random_on{false};

double RandomTarget(Randomizer& r, double baseline, double low, double high, double spread) {
  for (int i = 0; i < 32; ++i) {
    const double v = baseline * std::exp(spread * r.normal(r.rng));
    if (v >= low && v <= high) return v;
  }
  return std::clamp(baseline, low, high);
}

struct RandomParams {
  int type = 1, smoothing = 2;
  double baseline = 1, low = 0.5, high = 2, spread = 0.6, timestep = 10;
  bool operator==(const RandomParams& o) const {
    return type == o.type && smoothing == o.smoothing && baseline == o.baseline && low == o.low && high == o.high &&
           spread == o.spread && timestep == o.timestep;
  }
};

RandomParams ReadRandomParams() {
  RandomParams p;
  p.type = REXCVAR_GET(f2_sens_random_type);
  p.smoothing = std::clamp(REXCVAR_GET(f2_sens_random_smoothing), 0, 4);
  p.baseline = REXCVAR_GET(f2_sens_random_baseline);
  p.low = REXCVAR_GET(f2_sens_random_min);
  p.high = REXCVAR_GET(f2_sens_random_max);
  if (p.low > p.high) std::swap(p.low, p.high);
  p.spread = REXCVAR_GET(f2_sens_random_spread);
  p.timestep = REXCVAR_GET(f2_sens_random_timestep);
  return p;
}

void StartRandomizer(Randomizer& r, const RandomParams& p, uint32_t seed) {
  r = Randomizer{};
  r.on = true;
  r.rng.seed(seed);
  r.from = r.to = r.multiple = r.low = r.high = std::clamp(p.baseline, p.low, p.high);
  r.since_step = 1e9;  // the first step comes at once
}

// `dt` seconds further: the next multiple.
void Advance(Randomizer& r, const RandomParams& p, double dt) {
  if (p.type == 0) {
    r.since_step += dt;
    if (r.since_step >= p.timestep) {
      r.from = r.multiple;
      r.to = RandomTarget(r, p.baseline, p.low, p.high, p.spread);
      r.since_step = 0.0;
    }
    const double blend = p.smoothing * 0.25;
    double share = blend > 0.0 ? std::clamp(r.since_step / blend, 0.0, 1.0) : 1.0;
    share = share * share * (3.0 - 2.0 * share);
    r.multiple = r.from + (r.to - r.from) * share;
  } else {
    // [f2-menus] 03:40 the exact step of such a walk, so its usual distance from the baseline is the spread
    // whatever the length of the step.
    const double memory = 0.5 * std::pow(2.0, p.smoothing);  // seconds
    const double keep = std::exp(-dt / memory);
    r.walk = r.walk * keep + p.spread * std::sqrt(1.0 - keep * keep) * r.normal(r.rng);
    r.value += (r.walk - r.value) * (1.0 - std::exp(-dt / (memory * 0.25)));
    // [f2-menus] 03:20 the limits bend the curve instead of cutting it flat: toward min and max the logarithm is
    // squeezed (tanh), so it nears them and turns back smoothly.
    double value = r.value;
    if (p.baseline > p.low && p.baseline < p.high) {
      const double room = value >= 0.0 ? std::log(p.high / p.baseline) : std::log(p.baseline / p.low);
      value = room * std::tanh(value / room);
    }
    r.multiple = std::clamp(p.baseline * std::exp(value), p.low, p.high);
  }
  r.low = std::min(r.low, r.multiple);
  r.high = std::max(r.high, r.multiple);
}

// [f2-menus] 2026-11-11 03:40 (owner 03:24: "FPS cannot affect mouse input."): the run moves in fixed steps of 5 ms
// of real time, whoever asks (a UI frame or a mouse event), so the multiple at a moment follows from the seed and the
// time alone, not from how many frames the game draws; a mouse event gets the multiple of its own moment, not of
// the last frame. Under g_random_mutex. The settings are the ones Tick last read.
constexpr double kRandomStep = 0.005;
RandomParams g_random_params;
bool g_random_paused = false;
double g_random_runtime = 0.0;  // seconds, 0 = no end
int g_random_log = 0;           // a log line every so many seconds of the run

void LogRandomizer(const Randomizer& r, const char* state) {
  char text[200];
  std::snprintf(text, sizeof(text), "f2: sensitivity randomizer: x%.4f (so far %.4f to %.4f) after %.1f s%s",
                r.multiple, r.low, r.high, r.elapsed, state);
  LogLine(text);
}

void AdvanceTo(Randomizer& r, std::chrono::steady_clock::time_point now) {
  const double dt = std::chrono::duration<double>(now - r.last).count();
  if (dt <= 0.0) return;
  r.last = now;
  if (g_random_paused || r.finished) return;
  const RandomParams& p = g_random_params;
  r.pending += std::min(dt, 600.0);
  while (r.pending >= kRandomStep) {
    r.pending -= kRandomStep;
    ++r.steps;
    r.elapsed = double(r.steps) * kRandomStep;
    if (g_random_runtime > 0.0 && r.elapsed >= g_random_runtime) {
      r.finished = true;
      r.pending = 0.0;
      r.multiple = std::clamp(p.baseline, p.low, p.high);
      LogLine("f2: sensitivity randomizer: its runtime is over, back at the baseline");
      break;
    }
    Advance(r, p, kRandomStep);
    if (g_random_log > 0 && r.steps % (int64_t(g_random_log) * 200) == 0) LogRandomizer(r, "");
  }
  g_random_multiple.store(static_cast<float>(r.multiple), std::memory_order_relaxed);
}

// The graph of a run: the same generator run ahead for `seconds` with a fixed start, `count` points. It shows what
// the settings give (as Sensitivity Randomizer's visualize.py draws its list), not the values a real run will take.
void SampleRun(const RandomParams& p, double seconds, float* out, int count) {
  Randomizer r;
  StartRandomizer(r, p, 20261111u);
  const double dt = 0.05;
  const int steps = std::max(1, static_cast<int>(seconds / count / dt));
  for (int i = 0; i < count; ++i) {
    for (int k = 0; k < steps; ++k) Advance(r, p, dt);
    out[i] = static_cast<float>(r.multiple);
  }
}

// The last two minutes of the running one, a point every half second (UI thread only).
constexpr int kHistory = 240;
float g_history[kHistory] = {};
int g_history_next = 0, g_history_count = 0;
double g_history_wait = 0.0;

void RandomizerTick() {
  const bool on = REXCVAR_GET(f2_sens_random);
  const auto now = std::chrono::steady_clock::now();
  const double runtime = REXCVAR_GET(f2_sens_random_runtime) * 60.0;
  const bool paused = REXCVAR_GET(f2_sens_random_paused);
  double multiple = 1.0, elapsed = 0.0;
  bool finished = false;
  {
    std::lock_guard lock(g_random_mutex);
    Randomizer& r = g_randomizer;
    if (!on) {
      if (r.on) LogLine("f2: sensitivity randomizer off");
      r.on = false;
      g_random_on.store(false, std::memory_order_relaxed);
      g_random_multiple.store(1.0f, std::memory_order_relaxed);
      return;
    }
    const RandomParams p = ReadRandomParams();
    if (!r.on) {  // turned on: a fresh run
      const int seed = REXCVAR_GET(f2_sens_random_seed);
      StartRandomizer(r, p, seed > 0 ? static_cast<uint32_t>(seed) : std::random_device{}());
      r.last = r.last_log = now;
      g_history_next = g_history_count = 0;
      g_history_wait = 0.0;
      char line[240];
      std::snprintf(line, sizeof(line),
                    "f2: sensitivity randomizer on: %s, baseline %g, %g to %g, spread %g, smoothing %d, timestep %g s, "
                    "runtime %g min, seed %d",
                    p.type == 0 ? "steps" : "smooth", p.baseline, p.low, p.high, p.spread, p.smoothing, p.timestep,
                    double(REXCVAR_GET(f2_sens_random_runtime)), seed);
      LogLine(line);
    }
    g_random_params = p;
    g_random_paused = paused;
    g_random_runtime = runtime;
    g_random_log = std::max(0, REXCVAR_GET(f2_sens_random_log));
    g_random_on.store(true, std::memory_order_relaxed);
    const double before = r.elapsed;
    AdvanceTo(r, now);
    g_history_wait += r.elapsed - before;
    if (g_history_wait >= 0.5) {
      g_history_wait = 0.0;
      g_history[g_history_next] = static_cast<float>(r.multiple);
      g_history_next = (g_history_next + 1) % kHistory;
      g_history_count = std::min(g_history_count + 1, kHistory);
    }
    // While it runs AdvanceTo writes the log line, at whole seconds of the run; held still, this does.
    if (!paused && !r.finished) {
      r.last_log = now;
    } else if (g_random_log > 0 && now - r.last_log >= std::chrono::seconds(g_random_log)) {
      r.last_log = now;
      LogRandomizer(r, r.finished ? ", finished" : ", paused");
    }
    multiple = r.multiple;
    elapsed = r.elapsed;
    finished = r.finished;
  }

  const double left = runtime > 0.0 ? std::max(0.0, runtime - elapsed) : -1.0;
  if (REXCVAR_GET(f2_sens_random_show)) {
    char text[96];
    const char* state = finished ? "  finished" : paused ? "  PAUSED" : "";
    if (left >= 0.0)
      std::snprintf(text, sizeof(text), "sensitivity x%.2f  %d:%02d left%s", multiple, static_cast<int>(left) / 60,
                    static_cast<int>(left) % 60, state);
    else
      std::snprintf(text, sizeof(text), "sensitivity x%.2f%s", multiple, state);
    const ImGuiIO& io = ImGui::GetIO();
    const float margin = std::floor(12.0f * std::max(1.0f, io.DisplaySize.y / 1080.0f));
    const ImVec2 pos(margin, std::floor(std::max(margin, g_corner_top)));
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    dl->AddText(ImVec2(pos.x + 1.0f, pos.y + 1.0f), IM_COL32(0, 0, 0, 200), text);
    dl->AddText(pos, IM_COL32(230, 230, 230, 255), text);
  }
}

// ---- the sensitivity scales (see f2_sens_scale). The port names its own sensitivity setting once
// (SetGameSensitivity); a scale sets that setting to the value that turns as far a count. UI thread only.
struct Scale {
  const char* name;
  double yaw;  // degrees a count at sensitivity 1
};
constexpr Scale kScales[] = {
    {"This game's own numbers", 0.0},
    {"Quake / Source / Counter-Strike 2 / Apex", 0.022},
    {"Valorant", 0.07},
    {"Overwatch 2 / modern Call of Duty / Destiny 2", 0.0066},
    {"Rainbow Six Siege", 0.00572957795},
    {"Fortnite (its percent)", 0.005555},
    {"Battlefield V / 2042", 0.0055555},
    {"Deadlock", 0.044},
    {"Centimetres for a full turn", 0.0},
};
constexpr int kScaleCount = int(sizeof(kScales) / sizeof(kScales[0]));
constexpr int kScaleCentimetres = kScaleCount - 1;

struct GameSensitivity {
  std::string setting, own_switch;
  double yaw = 0.022, low = 0.01, high = 100.0;
};
GameSensitivity g_game_sensitivity;
std::string g_scale_note;
int g_scale_applied = -1, g_scale_dpi_applied = -1;
double g_scale_value_applied = -1.0;

// Degrees a count for `value` on `scale` (0 = this game's own, nothing to do).
double WantedDegrees(int scale, double value, int dpi) {
  if (scale <= 0 || scale >= kScaleCount || value <= 0.0) return 0.0;
  if (scale == kScaleCentimetres) return dpi > 0 ? 360.0 * 2.54 / (double(dpi) * value) : 0.0;
  return kScales[scale].yaw * value;
}

void ScaleTick() {
  const int scale = REXCVAR_GET(f2_sens_scale);
  const double value = REXCVAR_GET(f2_sens_value);
  const int dpi = REXCVAR_GET(f2_sens_dpi);
  if (scale == g_scale_applied && value == g_scale_value_applied && dpi == g_scale_dpi_applied) return;
  g_scale_applied = scale;
  g_scale_value_applied = value;
  g_scale_dpi_applied = dpi;
  g_scale_note.clear();
  const GameSensitivity& g = g_game_sensitivity;
  const double wanted = WantedDegrees(scale, value, dpi);
  if (wanted <= 0.0 || g.setting.empty() || !HasSetting(g.setting.c_str()) || g.yaw <= 0.0) return;
  const double own = std::clamp(wanted / g.yaw, g.low, g.high);
  char number[40];
  std::snprintf(number, sizeof(number), "%.6g", own);
  rex::cvar::SetFlagByName(g.setting, number);
  if (!g.own_switch.empty() && HasSetting(g.own_switch.c_str())) rex::cvar::SetFlagByName(g.own_switch, "true");
  char text[200];
  std::snprintf(text, sizeof(text), "= %s in this game's own numbers: %.5f degrees a count, %.1f cm for a full turn at %d DPI%s",
                number, own * g.yaw, 360.0 * 2.54 / (double(dpi) * own * g.yaw), dpi,
                own * g.yaw != wanted && std::abs(own * g.yaw - wanted) > 1e-9 ? " (at this game's limit)" : "");
  g_scale_note = text;
  char line[420];
  std::snprintf(line, sizeof(line), "f2: sensitivity scale %d (%s), %g: %s %s", scale, kScales[scale].name, value,
                g.setting.c_str(), text);
  LogLine(line);
}

}  // namespace

void SetSupported(bool supported) { g_supported.store(supported, std::memory_order_relaxed); }
void SetCornerTop(float y) { g_corner_top = y; }

// [f2-menus] 2026-11-11 04:12 for the windows' Reset buttons.
int Settings(const char* const** names) {
  static const char* const kNames[] = {
      "f2_sens_scale", "f2_sens_value", "f2_sens_dpi",
      "f2_accel", "f2_accel_mode", "f2_accel_gain", "f2_accel_acceleration", "f2_accel_exponent_classic",
      "f2_accel_exponent_power", "f2_accel_scale", "f2_accel_output_offset", "f2_accel_input_offset",
      "f2_accel_cap_mode", "f2_accel_cap_input", "f2_accel_cap_output", "f2_accel_decay_rate", "f2_accel_limit",
      "f2_accel_smooth", "f2_accel_sync_speed", "f2_accel_motivity", "f2_accel_gamma", "f2_accel_sens",
      "f2_accel_yx_ratio", "f2_accel_dpi", "f2_accel_whole",
      "f2_sens_random", "f2_sens_random_type", "f2_sens_random_baseline", "f2_sens_random_min", "f2_sens_random_max",
      "f2_sens_random_spread", "f2_sens_random_smoothing", "f2_sens_random_timestep", "f2_sens_random_runtime",
      "f2_sens_random_paused", "f2_sens_random_show",
  };
  if (names) *names = kNames;
  return static_cast<int>(sizeof(kNames) / sizeof(kNames[0]));
}

void SetGameSensitivity(const char* setting, const char* own_switch, double yaw, double low, double high) {
  g_game_sensitivity.setting = setting ? setting : "";
  g_game_sensitivity.own_switch = own_switch ? own_switch : "";
  g_game_sensitivity.yaw = yaw;
  g_game_sensitivity.low = low;
  g_game_sensitivity.high = high;
}

void Tick() {
  RandomizerTick();
  ScaleTick();
}

double Multiplier(double speed) { return Curve(speed, Read()); }

void Apply(float* dx, float* dy, double ms) { ApplyAged(dx, dy, ms, 0.0); }

void ApplyAged(float* dx, float* dy, double ms, double age_ms) {
  if (!dx || !dy) return;
  const bool curve = REXCVAR_GET(f2_accel);
  // [f2-menus] 03:40 the randomizer's multiple of the counts' own moment (see AdvanceTo); 1 while it is off.
  // 2026-11-11 Counts read late (a port that takes its mouse events once a frame) say how old they are, so the
  // multiple doesn't depend on when in the frame, or how often, they are read. Time only runs forward here: counts
  // older than the last step taken get the multiple as it is.
  double random = 1.0;
  if (g_random_on.load(std::memory_order_relaxed)) {
    std::lock_guard lock(g_random_mutex);
    if (g_randomizer.on) {
      auto at = std::chrono::steady_clock::now();
      if (age_ms > 0.0)
        at -= std::chrono::duration_cast<std::chrono::steady_clock::duration>(
            std::chrono::duration<double, std::milli>(std::min(age_ms, 1000.0)));
      AdvanceTo(g_randomizer, at);
      random = g_randomizer.multiple;
    }
  }
  if (!curve && random == 1.0) return;
  double x = *dx, y = *dy;
  if (x == 0.0 && y == 0.0) return;
  double scale_x = 1.0, scale_y = 1.0, speed = 0.0, sens = 1.0, yx = 1.0;
  // The time the counts took: Raw Accel's clamp. An unknown time counts as the longest, the slowest speed.
  // [f2-menus] 2026-11-11 03:30 (owner 03:24: "FPS cannot affect mouse input."): the speed comes from the mouse's
  // own events, never from the game's frames. When events that were held up arrive in a burst (the time since the
  // one before is far below the source's usual spacing), each counts as one usual spacing, so a hitch in delivery
  // can't make the curve see a huge speed. The usual spacing is the smallest seen lately, per calling thread.
  thread_local double poll_ms = 0.0;
  if (ms >= 0.1) poll_ms = poll_ms > 0.0 ? std::min(poll_ms * 1.01, ms) : ms;
  double time = ms > 0.0 ? ms : 100.0;
  if (ms > 0.0 && poll_ms > 0.0 && ms < 0.3 * poll_ms) time = poll_ms;
  time = std::clamp(time, 0.0625, 100.0);
  Args a;
  if (curve) {
    a = Read();
    const int dpi = REXCVAR_GET(f2_accel_dpi);
    const double dpi_factor = dpi > 0 ? 1000.0 / dpi : 1.0;
    const double per_ms = dpi_factor / time;
    if (REXCVAR_GET(f2_accel_whole)) {
      speed = std::sqrt(x * x + y * y) * per_ms;
      scale_x = scale_y = Curve(speed, a);
    } else {
      speed = std::max(std::abs(x), std::abs(y)) * per_ms;
      scale_x = Curve(std::abs(x) * per_ms, a);
      scale_y = Curve(std::abs(y) * per_ms, a);
    }
    sens = REXCVAR_GET(f2_accel_sens) * dpi_factor;
    yx = REXCVAR_GET(f2_accel_yx_ratio);
  }
  x *= scale_x * sens * random;
  y *= scale_y * sens * yx * random;
  if (REXCVAR_GET(f2_accel_debug)) {
    thread_local std::chrono::steady_clock::time_point last{};
    const auto now = std::chrono::steady_clock::now();
    if (now - last >= std::chrono::seconds(1)) {
      last = now;
      char text[240], line[320];
      std::snprintf(text, sizeof(text),
                    "counts %.2f,%.2f in %.2f ms = %.4f counts a ms -> x%.4f,%.4f -> %.3f,%.3f (randomizer x%.4f)",
                    double(*dx), double(*dy), time, speed, scale_x, scale_y, x, y, random);
      if (curve)
        std::snprintf(line, sizeof(line), "f2: accel: mode %d %s: %s", a.mode, a.gain ? "gain" : "legacy", text);
      else
        std::snprintf(line, sizeof(line), "f2: accel: off: %s", text);
      LogLine(line);
    }
  }
  *dx = static_cast<float>(x);
  *dy = static_cast<float>(y);
}

static bool DrawScaleRows(const Widgets& w, bool* restart) {
  bool dirty = false;
  if (g_game_sensitivity.setting.empty() || !HasSetting(g_game_sensitivity.setting.c_str())) return dirty;
  w.section("Sensitivity as another game counts it");
  {
    static const char* const kValues[kScaleCount] = {"0", "1", "2", "3", "4", "5", "6", "7", "8"};
    const char* names[kScaleCount];
    for (int i = 0; i < kScaleCount; ++i) names[i] = kScales[i].name;
    dirty |= w.choice("Scale", "f2_sens_scale", kValues, names, kScaleCount, restart);
  }
  const int scale = REXCVAR_GET(f2_sens_scale);
  if (scale == kScaleCentimetres) {
    dirty |= w.slider_double("Centimetres for a full turn", "f2_sens_value", 5.0, 150.0, "%.1f", restart);
    dirty |= w.slider_int("Mouse DPI", "f2_sens_dpi", 100, 6400, "%d", restart);
  } else if (scale > 0) {
    dirty |= w.slider_double("Sensitivity there", "f2_sens_value", 0.01, 20.0, "%.3f", restart);
    dirty |= w.slider_int("Mouse DPI (for the centimetres shown)", "f2_sens_dpi", 100, 6400, "%d", restart);
  }
  if (!g_scale_note.empty()) ImGui::TextDisabled("%s", g_scale_note.c_str());
  w.help("Type your sensitivity from another game, or how far the mouse travels for a full turn, and this game turns "
       "exactly as far for the same hand motion (from the hip). It sets this game's own mouse sensitivity; moving "
       "that row afterwards keeps your new value.");
  return dirty;
}

static bool DrawRandomizerRows(const Widgets& w, bool* restart) {
  bool dirty = false;
  w.section("Sensitivity randomizer (inside this game only)");
  dirty |= w.check("Sensitivity randomizer", "f2_sens_random", restart);
  w.help("For aim training: your mouse sensitivity wanders around a baseline while you play, so your aim learns to "
       "follow the target and not one fixed sensitivity. Like Sensitivity Randomizer, but only inside this game's "
       "own mouse look: no driver, nothing changes for Windows or any other game. Meant for practice, now and then.");
  if (!REXCVAR_GET(f2_sens_random)) return dirty;
  {
    const char* v[] = {"1", "0"};
    const char* n[] = {"Smooth (always moving)", "Steps (a new value every timestep)"};
    dirty |= w.choice("Type", "f2_sens_random_type", v, n, 2, restart);
  }
  auto slider = [&](const char* label, const char* name, double min, double max, const char* format) {
    dirty |= w.slider_double(label, name, min, max, format, restart);
  };
  slider("Baseline", "f2_sens_random_baseline", 0.25, 4.0, "%.2f");
  slider("Smallest", "f2_sens_random_min", 0.1, 4.0, "%.2f");
  slider("Largest", "f2_sens_random_max", 0.25, 8.0, "%.2f");
  slider("Spread", "f2_sens_random_spread", 0.0, 2.0, "%.2f");
  w.help("How far from the baseline it usually goes. 0.1 barely moves; 0.6 often reaches half and double.");
  dirty |= w.slider_int("Smoothing", "f2_sens_random_smoothing", 0, 4, "%d", restart);
  if (REXCVAR_GET(f2_sens_random_type) == 0) slider("Timestep (seconds)", "f2_sens_random_timestep", 0.25, 120.0, "%.2f");
  slider("Runtime (minutes, 0 = no end)", "f2_sens_random_runtime", 0.0, 240.0, "%.0f");
  dirty |= w.check("Paused", "f2_sens_random_paused", restart);
  dirty |= w.check("Show the multiple and the time left", "f2_sens_random_show", restart);
  {
    // [f2-menus] 03:15 (owner 03:03: "that graph like graphic would be nice too."): what these settings give over
    // five minutes, and what the running one did in its last two.
    constexpr int kPoints = 240;
    static float sample[kPoints];
    static RandomParams drawn;
    static bool have = false;
    const RandomParams p = ReadRandomParams();
    if (!have || !(drawn == p)) {
      SampleRun(p, 300.0, sample, kPoints);
      drawn = p;
      have = true;
    }
    const float bottom = static_cast<float>(std::min(p.low, p.baseline)) * 0.9f;
    const float top = static_cast<float>(std::max(p.high, p.baseline)) * 1.05f;
    const ImVec2 size(0.0f, ImGui::GetFontSize() * 6.0f);
    char label[96];
    std::snprintf(label, sizeof(label), "a sample of 5 minutes with these settings (x%.2f to x%.2f)", p.low, p.high);
    ImGui::PlotLines("##f2_sens_random_sample", sample, kPoints, 0, label, bottom, top, size);
    if (g_history_count > 1) {
      std::snprintf(label, sizeof(label), "this run, the last %d seconds, now x%.2f", g_history_count / 2,
                    double(g_random_multiple.load(std::memory_order_relaxed)));
      const int offset = g_history_count == kHistory ? g_history_next : 0;
      ImGui::PlotLines("##f2_sens_random_history", g_history, g_history_count, offset, label, bottom, top, size);
    } else {
      ImGui::TextDisabled("Now: x%.2f", double(g_random_multiple.load(std::memory_order_relaxed)));
    }
  }
  w.help("The upper graph is an example of what these settings do, not the values to come. Turn the randomizer off "
       "and on again to start a new run.");
  return dirty;
}

static bool DrawCurveRows(const Widgets& w, bool* restart) {
  bool dirty = false;
  w.section("Mouse acceleration (inside this game only)");
  dirty |= w.check("Mouse acceleration", "f2_accel", restart);
  w.help("The faster you move the mouse, the further each bit of motion turns the view, by a curve you choose. These "
       "are Raw Accel's curves, but they act only on this game's own mouse look, inside the game: no driver is "
       "installed, nothing changes for Windows or for any other game, so there is nothing for another game's "
       "anti-cheat to find. It works with the game's own mouse look (not with the mouse played as a controller "
       "stick).");
  if (!REXCVAR_GET(f2_accel)) return dirty;
  {
    const char* v[] = {"0", "1", "2", "3", "4", "5"};
    const char* n[] = {"Linear", "Classic", "Power", "Natural", "Jump", "Synchronous"};
    dirty |= w.choice("Curve", "f2_accel_mode", v, n, 6, restart);
  }
  const int mode = REXCVAR_GET(f2_accel_mode);
  dirty |= w.check("Shape the gain (smoother)", "f2_accel_gain", restart);
  w.help("On: the curve says how much each further bit of speed adds (Raw Accel's \"Gain\"). Off: the curve is the "
       "sensitivity itself (its older form).");
  auto slider = [&](const char* label, const char* name, double min, double max, const char* format) {
    dirty |= w.slider_double(label, name, min, max, format, restart);
  };
  if (mode == 0 || mode == 1) {
    slider("Acceleration", "f2_accel_acceleration", 0.0, 0.2, "%.4f");
    if (mode == 1) slider("Exponent", "f2_accel_exponent_classic", 1.01, 6.0, "%.2f");
  }
  if (mode == 2) {
    slider("Scale", "f2_accel_scale", 0.01, 10.0, "%.3f");
    slider("Exponent", "f2_accel_exponent_power", 0.01, 2.0, "%.3f");
    slider("Output offset", "f2_accel_output_offset", 0.0, 3.0, "%.2f");
  }
  if (mode == 3) {
    slider("Decay rate", "f2_accel_decay_rate", 0.001, 1.0, "%.3f");
    slider("Limit", "f2_accel_limit", 0.1, 6.0, "%.2f");
  }
  if (mode == 0 || mode == 1 || mode == 3) slider("Input offset (counts a ms)", "f2_accel_input_offset", 0.0, 50.0, "%.1f");
  if (mode == 0 || mode == 1 || mode == 2) {
    const char* v[] = {"0", "1", "2"};
    const char* n[] = {"Output", "Input", "Both"};
    dirty |= w.choice("Cap by", "f2_accel_cap_mode", v, n, 3, restart);
    const int cap_mode = REXCVAR_GET(f2_accel_cap_mode);
    if (cap_mode != 0) slider("Cap: input (counts a ms)", "f2_accel_cap_input", 0.0, 100.0, "%.1f");
    if (cap_mode != 1) slider("Cap: output (0 = none)", "f2_accel_cap_output", 0.0, 6.0, "%.2f");
  }
  if (mode == 4) {
    slider("Step at (counts a ms)", "f2_accel_cap_input", 0.0, 100.0, "%.1f");
    slider("Sensitivity after the step", "f2_accel_cap_output", 0.1, 6.0, "%.2f");
    slider("Smooth", "f2_accel_smooth", 0.0, 1.0, "%.2f");
  }
  if (mode == 5) {
    slider("Synchronous speed (counts a ms)", "f2_accel_sync_speed", 0.1, 100.0, "%.1f");
    slider("Motivity", "f2_accel_motivity", 1.01, 6.0, "%.2f");
    slider("Gamma", "f2_accel_gamma", 0.1, 6.0, "%.2f");
    slider("Smooth", "f2_accel_smooth", 0.0, 1.0, "%.2f");
  }
  {
    // The curve: the sensitivity multiple from standing still to 60 counts a millisecond.
    constexpr int kSamples = 96;
    constexpr float kTop = 60.0f;
    float values[kSamples];
    const Args args = Read();
    float low = 1.0f, high = 1.0f;
    for (int i = 0; i < kSamples; ++i) {
      values[i] = static_cast<float>(Curve(double(kTop) * (i + 0.5) / kSamples, args));
      low = std::min(low, values[i]);
      high = std::max(high, values[i]);
    }
    char overlay[64];
    std::snprintf(overlay, sizeof(overlay), "x%.2f to x%.2f, 0 to %.0f counts a ms", low, high, kTop);
    ImGui::PlotLines("##f2_accel_curve", values, kSamples, 0, overlay, std::min(low, 0.0f), high * 1.1f,
                     ImVec2(0.0f, ImGui::GetFontSize() * 6.0f));
  }
  slider("Sensitivity multiplier", "f2_accel_sens", 0.1, 5.0, "%.2f");
  slider("Up / down against side to side", "f2_accel_yx_ratio", 0.1, 3.0, "%.2f");
  dirty |= w.slider_int("Mouse DPI (0 = not set)", "f2_accel_dpi", 0, 32000, "%d", restart);
  w.help("With the DPI set, speeds are counted as on a 1000 DPI mouse, as Raw Accel does, so its settings can be "
       "copied as they are; the sensitivity changes by 1000 / DPI with it. Leave it at 0 to keep your sensitivity.");
  dirty |= w.check("One curve for the whole motion", "f2_accel_whole", restart);
  w.help("Off: side to side and up / down are each sped up by their own speed.");
  return dirty;
}

bool DrawRows(const Widgets& w, bool* restart) {
  if (!g_supported.load(std::memory_order_relaxed)) return false;
  bool dirty = DrawScaleRows(w, restart);
  dirty |= DrawCurveRows(w, restart);
  dirty |= DrawRandomizerRows(w, restart);
  return dirty;
}

}  // namespace accel

}  // namespace f2
