// cod3 - frame rate limit (com_maxfps) and the frame rate counter (cg_drawFPS).
//
// com_maxfps is a limit, not a target: the game runs as fast as it can and
// never faster than com_maxfps (0 = no limit). CoD3's own Com_Frame enforces
// it in whole milliseconds with a busy loop, so 144 gave 166 fps and one CPU
// core spun while waiting. A mid-asm hook at the top of Com_Frame (see
// config/default.toml) paces frames on the host clock instead: it sleeps
// most of the wait, keeps exact rates such as 144, and reads com_maxfps
// every frame so a change from the console or the settings applies at once.
//
// The guest's vblank interrupts would otherwise be a second, hidden limit
// (frames wait for the next vblank and drop from 60 straight to 30), so
// Cod3App runs them at 1000 Hz; the video mode the game sees stays 60 Hz.
//
// Windows: Sleep() wakes on the system timer's tick, 15.6 ms unless a program
// asks for 1 ms (timeBeginPeriod), and Windows 11 drops that request while it
// thinks the window is hidden. SetUpHostTiming asks for 1 ms and opts out of
// that (power throttling), and the limiter waits with a high-resolution
// waitable timer, which wakes within a fraction of a millisecond either way.
// The same deadlines as on Linux, so the pacing and the limit don't change.

#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

namespace rex::ui {
class Window;
}

namespace cod3 {

// Once at start (Cod3App::OnPreSetup). Windows: 1 ms timer resolution, kept
// when Windows thinks the window is hidden. Linux and macOS: nothing to do,
// their sleeps are precise.
void SetUpHostTiming();

// UI thread, every frame of the overlay: once the game has run for a moment,
// logs one line on how frames are paced and shown (the frame rate limit,
// V-Sync and the present mode, the display's refresh rate, the timer), so a
// player's log says why the frame rate is where it is.
void LogFramePacingOnce(rex::ui::Window* window);

struct FrameStats {
  float fps = 0.0f;       // frames per second over the last update period
  float frame_ms = 0.0f;  // average frame time
  float min_ms = 0.0f;    // fastest frame in the period
  float max_ms = 0.0f;    // slowest frame in the period
  float busy_ms = 0.0f;   // average time the game's main thread worked per frame
                          // (frame_ms minus the wait for com_maxfps)
  int limit = 0;          // com_maxfps (0 = no limit)
  bool valid = false;     // false until the game has run a full period
};

class FrameRate {
 public:
  static FrameRate& Get();

  // Guest main thread, once per Com_Frame: waits until the next frame may
  // start under `max_fps` (0 or less = no limit), then records the frame.
  void BeginFrame(int max_fps);

  // Any thread.
  FrameStats Stats();
  int limit() const { return limit_.load(std::memory_order_relaxed); }

 private:
  using Clock = std::chrono::steady_clock;
  FrameRate() = default;
  static void WaitUntil(Clock::time_point deadline);
  void Record(Clock::time_point now, double busy_s);

  // Pacing (guest main thread only).
  Clock::time_point next_frame_{};
  int paced_fps_ = 0;
  Clock::time_point wait_end_{};  // when the previous frame's wait ended

  // Counting (guest main thread writes, anyone reads the published stats).
  Clock::time_point last_frame_{};
  Clock::time_point period_start_{};
  int period_frames_ = 0;
  double period_min_s_ = 0.0;
  double period_max_s_ = 0.0;
  double period_busy_s_ = 0.0;

  std::atomic<int> limit_{0};
  std::mutex mutex_;
  FrameStats published_;
};

}  // namespace cod3
