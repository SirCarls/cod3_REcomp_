// inputkit - the mouse's motion and turn (see mouse.h). The resampling is Call of Duty 3's mouse_look.cpp
// (cod3_mouse_resample), the turn its ApplyToGuest.

#include "inputkit/mouse.h"

#include <algorithm>
#include <cmath>

namespace inputkit::mouse {

void MotionQueue::Add(float dx, float dy, int64_t now, bool resample) {
  std::lock_guard lock(mutex_);
  if (!resample) {
    whole_dx_ += dx;
    whole_dy_ += dy;
    return;
  }
  // This batch covers the time since the one before (at most 8 ms).
  const int64_t since = last_batch_ns_ ? now - last_batch_ns_ : kMaxSpanNs;
  if (last_batch_ns_ && since > 0 && since < 4 * kMaxSpanNs) {
    batch_interval_ns_ += (double(since) - batch_interval_ns_) * 0.1;
    // The longest recent gap, drifting back toward the average (about 50 batches) so one stall doesn't stay.
    longest_gap_ns_ = std::max(double(since), longest_gap_ns_ + (batch_interval_ns_ - longest_gap_ns_) * 0.02);
  }
  last_batch_ns_ = now;
  const int64_t span = std::clamp<int64_t>(since, 1, kMaxSpanNs);
  if (size_ == kRing) {  // never read for a while: keep the newest
    head_ = (head_ + 1) % kRing;
    --size_;
  }
  ring_[(head_ + size_) % kRing] = Batch{now - span, now, dx, dy, 0.0f};
  ++size_;
}

void MotionQueue::Take(int64_t now, float* out_dx, float* out_dy) {
  std::lock_guard lock(mutex_);
  float dx = whole_dx_, dy = whole_dy_;
  whole_dx_ = whole_dy_ = 0.0f;
  // The motion of the batches up to now less the longest recent gap between batches (so the next batch is never
  // due before it arrives), each batch spread evenly over its own time.
  const int64_t lag = int64_t(std::clamp(longest_gap_ns_, 0.0, double(kMaxSpanNs)));
  const int64_t until = now - lag;
  while (size_) {
    Batch& b = ring_[head_];
    const float upto = b.end_ns <= until     ? 1.0f
                       : b.start_ns >= until ? 0.0f
                                             : float(double(until - b.start_ns) /
                                                     double(std::max<int64_t>(b.end_ns - b.start_ns, 1)));
    if (upto > b.taken) {
      dx += b.dx * (upto - b.taken);
      dy += b.dy * (upto - b.taken);
      b.taken = upto;
    }
    if (b.taken < 1.0f) break;
    head_ = (head_ + 1) % kRing;
    --size_;
  }
  *out_dx = dx;
  *out_dy = dy;
}

void MotionQueue::Clear() {
  std::lock_guard lock(mutex_);
  whole_dx_ = whole_dy_ = 0.0f;
  head_ = size_ = 0;
}

double MotionQueue::BatchIntervalNs() const {
  std::lock_guard lock(mutex_);
  return batch_interval_ns_;
}

void TurnFromCounts(const Turn& t, float dx, float dy, float* yaw_delta, float* pitch_delta) {
  double sens = t.sensitivity;
  if (t.zoom_ratio < 0.999) sens *= t.zoom_ratio * t.zoom_sensitivity;
  sens *= t.multiplier;
  const double invert = t.invert ? -1.0 : 1.0;
  *yaw_delta = -static_cast<float>(dx * sens * t.scale_x * t.m_yaw);
  *pitch_delta = static_cast<float>(dy * sens * t.scale_y * t.m_pitch * invert);
}

double ZoomRatio(double fov, double base_fov) {
  const double ratio = (base_fov > 1.0 && fov > 0.0) ? fov / base_fov : 1.0;
  return std::clamp(ratio, 0.05, 1.0);
}

}  // namespace inputkit::mouse
