// inputkit - which the player used last (include/inputkit/activity.h).

#include "inputkit/activity.h"

#include <cmath>
#include <cstdlib>

namespace inputkit::activity {

void Tracker::NotePress(int64_t now_ms) {
  presses_.fetch_add(1, std::memory_order_relaxed);
  press_ms_.store(now_ms, std::memory_order_relaxed);
  device_.store(Device::kKeyboardMouse, std::memory_order_relaxed);
}

void Tracker::KeyboardMousePress(int64_t now_ms) { NotePress(now_ms); }

void Tracker::MouseMotion(float dx, float dy, int64_t now_ms) {
  const float counts = std::fabs(dx) + std::fabs(dy);
  if (!(counts > 0.0f)) return;  // also drops NaN
  motion_ms_.store(now_ms, std::memory_order_relaxed);
  // Added up rather than judged per event: the window hands motion over in batches of any size, so a per-event
  // test would depend on its event loop's rate.
  float sum = motion_sum_.load(std::memory_order_relaxed);
  while (!motion_sum_.compare_exchange_weak(sum, sum + counts, std::memory_order_relaxed)) {
  }
  if (sum < kMouseUseCounts && sum + counts >= kMouseUseCounts) NotePress(now_ms);
}

bool Tracker::PadRead(const PadState& pad, int64_t now_ms) {
  std::lock_guard lock(mutex_);
  bool used = (pad.buttons & ~last_.buttons) != 0;
  for (int i = 0; i < 2; ++i)
    used |= pad.triggers[i] > kTriggerThreshold && last_.triggers[i] <= kTriggerThreshold;
  // std::abs on int: -32768 as int16_t would overflow its own type.
  auto far = [](int16_t v) { return std::abs(static_cast<int>(v)) > kStickThreshold; };
  for (int i = 0; i < 2; ++i) used |= far(pad.axes[i]) && !far(last_.axes[i]);
  const bool mouse_moving = now_ms - motion_ms_.load(std::memory_order_relaxed) < kExplainMs;
  for (int i = 2; i < 4; ++i) used |= !mouse_moving && far(pad.axes[i]) && !far(last_.axes[i]);
  last_ = pad;

  const uint32_t presses = presses_.load(std::memory_order_relaxed);
  const bool explained =
      presses != last_presses_ || now_ms - press_ms_.load(std::memory_order_relaxed) < kExplainMs;
  last_presses_ = presses;
  if (!used || explained) return false;
  motion_sum_.store(0.0f, std::memory_order_relaxed);
  return device_.exchange(Device::kController, std::memory_order_relaxed) != Device::kController;
}

void Tracker::Reset() {
  std::lock_guard lock(mutex_);
  device_.store(Device::kController, std::memory_order_relaxed);
  presses_.store(0, std::memory_order_relaxed);
  press_ms_.store(INT64_MIN / 2, std::memory_order_relaxed);
  motion_ms_.store(INT64_MIN / 2, std::memory_order_relaxed);
  motion_sum_.store(0.0f, std::memory_order_relaxed);
  last_ = PadState{};
  last_presses_ = 0;
}

}  // namespace inputkit::activity
