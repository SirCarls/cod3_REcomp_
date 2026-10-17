// cod3 - the numbers behind the stick battle actions (QTEs).
//
// Shared by the port's input for those actions (game/qte_actions.cpp), the
// spinner that shows them (ui/qte_spinner.cpp) and a standalone test
// (wip/qte/qte_math_test.cpp). Plain inline functions, no game memory.
//
// Where the game's own receivers (InteractInputRcvr*, default.xex) matter,
// they are mirrored here with the game's constants, so the port can predict
// what a stick position does, draw it, and fake an action for tests:
//   * IN_JoyMove 0x82522138 + 0x82519D98: the stick scale and dead zone,
//   * StickToggle step 0x824F6498: a flip is a pass beyond +70, then -70,
//   * Rowboat step 0x824FC450: catch with the stick up, sweep clockwise,
//   * StickSwirl step 0x82508238 (sampler 0x8250C6D8, smoother 0x82509C78):
//     the turning rate the game measures.

#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace cod3::qte::math {

inline constexpr float kPi = 3.14159265358979f;
inline constexpr float kTwoPi = 2.0f * kPi;

// ---------------------------------------------------------------------------
// The game's stick scale
// ---------------------------------------------------------------------------
// IN_JoyMove keeps the high byte of each axis (-128..127) and applies a dead
// zone per axis (joy_threshold, 25 of 128): smaller values are 0, larger ones
// are stretched back to 0..128. Every receiver compares against that scale.
inline constexpr float kGameAxisMax = 128.0f;
inline constexpr float kDeadZone = 25.0f;

// A raw deflection (-1..1) on the game's scale (-128..128).
inline float GameAxis(float raw) {
  const float v = std::clamp(raw, -1.0f, 1.0f) * kGameAxisMax;
  const float magnitude = std::fabs(v);
  if (magnitude < kDeadZone) return 0.0f;
  return std::copysign((magnitude - kDeadZone) / (kGameAxisMax - kDeadZone) * kGameAxisMax, v);
}

// The raw deflection (0..1) that reaches `game` on the game's scale.
inline constexpr float RawForGame(float game) {
  return (game * (kGameAxisMax - kDeadZone) / kGameAxisMax + kDeadZone) / kGameAxisMax;
}

// ---------------------------------------------------------------------------
// Toggles (StickToggleHoriz / StickToggleVert)
// ---------------------------------------------------------------------------
// A flip counts when the axis passes +70 after the last flip was at -70, or
// the other way round: about 64% of a full push.
inline constexpr float kToggleFlipGame = 70.0f;
inline constexpr float kToggleFlipRaw = RawForGame(kToggleFlipGame);

// ---------------------------------------------------------------------------
// Rowboat
// ---------------------------------------------------------------------------
// The catch: the stick must point up (y at least 15) and its length grows the
// phase from 0 to 0.25 between 15 and 100. The stroke: the phase follows the
// stick's compass angle, 0.25 at 12 o'clock to 1.0 at 6 o'clock (clockwise
// through 3 o'clock); past 0.98 the stroke is done and the phase starts over.
// Letting go (length below about 5.5) drops the phase to 0; pushing left past
// -70 in a stroke is a slip.
inline constexpr float kRowCatchMinY = 15.0f;
inline constexpr float kRowCatchRange = 85.0f;
inline constexpr float kRowStrokeStart = 0.25f;
inline constexpr float kRowCatchMax = 0.26f;
inline constexpr float kRowReleaseLength2 = 30.0f;  // squared length on the game's scale
inline constexpr float kRowSlipX = -70.0f;
inline constexpr float kRowWrapDegrees = 225.0f;    // larger angles count as 12 o'clock
inline constexpr float kRowDonePhase = 0.98f;

// Compass angle of a stick, in degrees: 0 = up, 90 = right, clockwise, 0..360.
inline float CompassDegrees(float x, float y_up) {
  if (x == 0.0f && y_up == 0.0f) return 0.0f;
  float deg = std::atan2(x, y_up) * (180.0f / kPi);
  return deg < 0.0f ? deg + 360.0f : deg;
}

// The receiver's state machine, one game frame at a time (game scale, y up).
struct RowboatModel {
  float phase = 0.0f;
  bool slipped = false;      // this frame: pushed left in a stroke
  bool stroke_done = false;  // this frame: a full stroke

  void Step(float x, float y_up) {
    slipped = stroke_done = false;
    if (phase < kRowStrokeStart) {
      if (y_up < kRowCatchMinY) {
        phase = 0.0f;
        return;
      }
      const float length = std::sqrt(x * x + y_up * y_up);
      phase = std::clamp((length - kRowCatchMinY) / kRowCatchRange * kRowStrokeStart, 0.0f, kRowCatchMax);
      return;
    }
    if (x * x + y_up * y_up <= kRowReleaseLength2) {
      phase = 0.0f;
      return;
    }
    if (x < kRowSlipX) {
      slipped = true;
      return;
    }
    float angle = CompassDegrees(x, y_up);
    if (angle > kRowWrapDegrees) angle = 0.0f;
    phase = std::min(kRowStrokeStart + angle / 180.0f * (1.0f - kRowStrokeStart), 1.0f);
    if (phase > kRowDonePhase) {
      stroke_done = true;
      phase = 0.0f;
    }
  }
};

// ---------------------------------------------------------------------------
// The virtual stick the mouse makes (the cursor is in spinner radii, y down)
// ---------------------------------------------------------------------------
struct Stick {
  float x = 0.0f;
  float y_up = 0.0f;  // up is positive, like XInput
};

// Inside this part of the radius the cursor is "centered" (no direction).
inline constexpr float kCursorCenter = 0.25f;
// How far the cursor may leave the ring (in radii).
inline constexpr float kCursorLimit = 1.3f;

// Swirl: a full push toward the cursor (the game only uses the direction).
inline Stick SwirlStick(float cx, float cy) {
  const float length = std::sqrt(cx * cx + cy * cy);
  if (length < kCursorCenter) return {};
  return {cx / length, -cy / length};
}

// Toggles: the cursor's position along the axis, a full push at the ring.
inline Stick ToggleStick(float cx, float cy, bool vertical) {
  if (vertical) return {0.0f, std::clamp(-cy, -1.0f, 1.0f)};
  return {std::clamp(cx, -1.0f, 1.0f), 0.0f};
}

// Rowing: a full push toward the cursor on the right half of the ring (the
// stroke, 12 to 6 o'clock clockwise); the left half and the middle are the
// way back and leave the stick centered. A circle drawn with the mouse then
// rows stroke after stroke, and the game's slip (a push left in a stroke)
// can't happen by accident.
inline Stick RowStick(float cx, float cy) {
  const float length = std::sqrt(cx * cx + cy * cy);
  if (length < kCursorCenter || cx < 0.0f) return {};
  return {cx / length, -cy / length};
}

// ---------------------------------------------------------------------------
// Frame rate
// ---------------------------------------------------------------------------
// The swirl receiver adds up (1 - cos dθ) per frame and then multiplies by
// the frame time, so at N frames a second the same turning of the stick
// gives 60²/N² of the console's progress (the console drew 60). This puts
// the console's number back above 60; at 60 and below nothing changes.
inline constexpr float kConsoleFrameRate = 60.0f;
inline constexpr float kMaxSwirlFactor = 400.0f;  // 1200 frames a second

inline float SwirlFrameRateFactor(float frames_per_second) {
  if (!(frames_per_second > kConsoleFrameRate)) return 1.0f;
  const float ratio = frames_per_second / kConsoleFrameRate;
  return std::min(ratio * ratio, kMaxSwirlFactor);
}

// The swirl turret (sub_8250F648, the cranes) turns by
// factor * clamp(m_yaw * msec * R, ±0.08) a frame, which is 60/N of the
// console's turn a second. Given the game's step, the console's step for one
// of N frames: the step times N/60, never more than the console's largest
// step spread over the frames (`turret_factor` and `step_limit` are the
// game's).
inline float TurretStep(float game_step, float frames_per_second, float turret_factor,
                        float step_limit) {
  if (!(frames_per_second > kConsoleFrameRate)) return game_step;
  const float ratio = std::min(frames_per_second / kConsoleFrameRate, 20.0f);
  const float largest = std::fabs(turret_factor) * step_limit / ratio;
  return std::clamp(game_step * ratio, -largest, largest);
}

// Frames a second from the game's frame times: the average of the last 32.
class FrameRateMeter {
 public:
  void Add(float seconds) {
    if (!(seconds > 0.0f) || seconds > 1.0f) return;  // paused or a hitch: not a frame time
    sum_ += seconds - times_[next_];
    times_[next_] = seconds;
    next_ = (next_ + 1) % kCount;
    if (count_ < kCount) ++count_;
  }
  float FramesPerSecond() const {
    return count_ > 0 && sum_ > 0.0f ? float(count_) / sum_ : kConsoleFrameRate;
  }
  void Reset() { *this = FrameRateMeter{}; }

 private:
  static constexpr int kCount = 32;
  float times_[kCount] = {};
  float sum_ = 0.0f;
  int next_ = 0;
  int count_ = 0;
};

// ---------------------------------------------------------------------------
// Swirl progress, as the game measures it (for the demo and the test)
// ---------------------------------------------------------------------------
// Each frame the sampler compares the stick's direction with the last one:
// (1 - cos dθ), signed by the turning direction (counterclockwise positive).
// It is added up over 0.1 s windows; each window's sum per second goes into
// a ring of five, and their average R gives the output
//     dt * 1000 * m_yaw * R      (negated when the action wants clockwise,
//                                 never below 0).
class SwirlMeter {
 public:
  // `x`, `y_up` on any scale (only the direction counts); returns the output
  // before the port's frame-rate factor.
  float Step(float x, float y_up, float dt, bool clockwise, float m_yaw) {
    float term = 0.0f;
    const float length = std::sqrt(x * x + y_up * y_up);
    if (length > 0.0f) {
      const float ux = x / length, uy = y_up / length;
      if (have_last_) {
        const float dot = std::clamp(last_x_ * ux + last_y_ * uy, -1.0f, 1.0f);
        const float cross = last_x_ * uy - last_y_ * ux;
        term = (1.0f - dot) * (cross < 0.0f ? -1.0f : 1.0f);
      }
      last_x_ = ux, last_y_ = uy, have_last_ = true;
    }
    if (window_time_ > kWindow) {
      ring_[next_] = window_sum_ / window_time_;
      next_ = (next_ + 1) % kRing;
      window_time_ = window_sum_ = 0.0f;
    } else {
      window_time_ += dt;
      window_sum_ += term;
    }
    float r = 0.0f;
    for (float v : ring_) r += v;
    r /= float(kRing);
    float out = dt * 1000.0f * m_yaw * r;
    if (clockwise) out = -out;
    return std::max(out, 0.0f);
  }
  void Reset() { *this = SwirlMeter{}; }

 private:
  static constexpr float kWindow = 0.1f;
  static constexpr int kRing = 5;
  float ring_[kRing] = {};
  int next_ = 0;
  float window_time_ = 0.0f, window_sum_ = 0.0f;
  float last_x_ = 0.0f, last_y_ = 0.0f;
  bool have_last_ = false;
};

// ---------------------------------------------------------------------------
// Toggle rate, as the game measures it (for the demo and the test)
// ---------------------------------------------------------------------------
// Flips are counted in 1 s windows (up to five kept); the rate is the flips
// over the windows' time (at least 1 s) times the action's inputRateFactor.
// It holds for 0.5 s after the last flip, then falls to 0 by 0.8 s.
class ToggleMeter {
 public:
  // `axis` on the game's scale; returns the rate.
  float Step(float axis, float dt, float rate_factor) {
    bool flip = false;
    if (side_ != -1 && axis > kToggleFlipGame) {
      side_ = -1;
      flip = true;
    } else if (side_ != 1 && axis < -kToggleFlipGame) {
      side_ = 1;
      flip = true;
    }
    times_[current_] += dt;
    since_flip_ += dt;
    if (times_[current_] > 1.0f) {
      const float over = times_[current_] - 1.0f;
      times_[current_] = 1.0f;
      if (filled_ < kWindows) ++filled_;
      current_ = (current_ + 1) % kWindows;
      counts_[current_] = 0;
      times_[current_] = over;
    }
    if (flip) {
      ++counts_[current_];
      since_flip_ = 0.0f;
    }
    float time = 0.0f;
    int flips = 0;
    for (int i = 0; i < kWindows; ++i) {
      time += times_[i];
      flips += counts_[i];
    }
    float rate = float(flips) / std::max(time, 1.0f) * rate_factor;
    if (since_flip_ > 0.5f) rate *= std::max(0.0f, 1.0f - (since_flip_ - 0.5f) / 0.3f);
    return rate;
  }
  int side() const { return side_; }
  void Reset() { *this = ToggleMeter{}; }

 private:
  static constexpr int kWindows = 5;
  float times_[kWindows] = {};
  int counts_[kWindows] = {};
  int current_ = 0;
  int filled_ = 0;
  int side_ = 0;
  float since_flip_ = 0.0f;
};

// ---------------------------------------------------------------------------
// The guide (how fast the spinner's dot goes)
// ---------------------------------------------------------------------------
// A ScaleAnimSpeed action plays its animation at full speed when
// animSpeedInputRateFactor * output reaches animSpeedMax. At the console's
// 60 frames that needs 1 - cos(ω/60) = max / (1000 * m_yaw * factor), so the
// dot circles at that ω (radians a second), kept between 0.8 and 2.5 turns a
// second; 1.5 turns when the action has no such numbers.
inline constexpr float kGuideFallbackTurns = 1.5f;

inline float SwirlGuideRadiansPerSecond(float anim_rate_factor, float anim_speed_max, float m_yaw) {
  float omega = kGuideFallbackTurns * kTwoPi;
  if (anim_rate_factor > 0.0f && anim_speed_max > 0.0f && m_yaw > 0.0f) {
    const float c = 1.0f - anim_speed_max / (1000.0f * m_yaw * anim_rate_factor);
    if (c > -1.0f && c < 1.0f) omega = kConsoleFrameRate * std::acos(c);
  }
  return std::clamp(omega, 0.8f * kTwoPi, 2.5f * kTwoPi);
}

// Toggles: flips a second the dot shows.
inline constexpr float kToggleGuideFlips = 2.5f;
// Rowing: seconds for the stroke (12 to 6 o'clock) and the way back. The
// boat's oar follows the phase at most one stroke a second (0x82505638).
inline constexpr float kRowGuideStroke = 1.0f;
inline constexpr float kRowGuideReturn = 0.6f;

// Wraps an angle to (-π, π].
inline float WrapAngle(float a) {
  if (!std::isfinite(a)) return 0.0f;
  const float r = std::remainder(a, kTwoPi);
  return r <= -kPi ? r + kTwoPi : r;
}

}  // namespace cod3::qte::math
