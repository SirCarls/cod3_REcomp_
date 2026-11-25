// inputkit - the mouse's turn at any frame rate (project rule, owner 2026-11-24: "every game needs to be able to
// function properly regardless of FPS"). A steady hand, 1000 counts a second, reaches the queue the way a window's
// event loop hands it over (batches holding the motion since the batch before, evenly or unevenly spaced), and the
// game reads it at 20 to 1000 frames a second. At every rate: the same total turn, the same turn a second, and with
// resampling on an even turn from frame to frame. The dead zone keeps no state, so how often it runs can't matter.
#include "inputkit/mouse.h"
#include "inputkit/stick.h"

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

using namespace inputkit::mouse;

namespace {

int checks = 0, failures = 0;

void check(bool ok, const char* what, double fps) {
  ++checks;
  if (!ok) {
    ++failures;
    std::printf("FAILED at %.0f fps: %s\n", fps, what);
  }
}

struct Result {
  double total_deg;     // after the motion stops and the queue drains
  double mean_deg_s;    // turn a second over the middle second
  double spread;        // frame-to-frame spread of that rate (standard deviation / mean)
};

// The batches' spacing: even (1-4 ms in turn) or uneven (0.3-6 ms, as an event loop that now and then stalls).
Result Run(double fps, bool resample, bool uneven) {
  MotionQueue q;
  const Turn turn;
  const int64_t frame = int64_t(1e9 / fps), motion_end = 2'000'000'000;
  int64_t last_batch = 0, next_batch = 1'000'000, next_frame = frame;
  uint64_t k = 0;
  double total = 0;
  std::vector<double> rates;
  for (int64_t now = 0; now <= motion_end + 50'000'000; now += 100'000) {  // 0.1 ms steps
    if (now >= next_batch && now <= motion_end) {
      q.Add(float((now - last_batch) / 1e6), 0.0f, now, resample);  // 1 count a millisecond
      last_batch = now;
      k = k * 6364136223846793005ULL + 1442695040888963407ULL;
      next_batch = now + (uneven ? 300'000 + int64_t((k >> 33) % 5'700'000) : 1'000'000 * int64_t(1 + k % 4));
    }
    if (now >= next_frame) {
      float dx, dy, yaw, pitch;
      q.Take(now, &dx, &dy);
      TurnFromCounts(turn, dx, dy, &yaw, &pitch);
      total += -yaw;
      if (now > 500'000'000 && now < 1'500'000'000) rates.push_back(-yaw / (frame / 1e9));
      next_frame += frame;
    }
  }
  double mean = 0, var = 0;
  for (double r : rates) mean += r;
  mean /= double(rates.size());
  for (double r : rates) var += (r - mean) * (r - mean);
  return {total, mean, std::sqrt(var / double(rates.size())) / mean};
}

}  // namespace

int main() {
  const Turn turn;
  // Every batch's counts (1 a ms; the last one, ending with the motion, may lie past the last batch boundary).
  const double want_total = 2000.0 * turn.sensitivity * turn.m_yaw, want_rate = want_total / 2.0;
  for (bool uneven : {false, true}) {
    for (bool resample : {true, false}) {
      for (double fps : {20.0, 30.0, 60.0, 144.0, 240.0, 360.0, 500.0, 1000.0}) {
        const Result r = Run(fps, resample, uneven);
        std::printf("%-6s resample=%d %5.0f fps: total %.2f deg, %.2f deg/s, frame-to-frame spread %.1f%%\n",
                    uneven ? "uneven" : "even", resample, fps, r.total_deg, r.mean_deg_s, r.spread * 100);
        check(std::fabs(r.total_deg - want_total) < 0.01 * want_total, "the total turn", fps);
        check(std::fabs(r.mean_deg_s - want_rate) < 0.01 * want_rate, "the turn a second", fps);
        // Resampling is what keeps high frame rates even; without it a frame gets whatever batch came in.
        if (resample) check(r.spread < 0.10, "an even turn from frame to frame", fps);
      }
    }
  }
  int x = 20000, y = 0;
  inputkit_radial_dead_zone(&x, &y, 0.2f);
  for (int i = 0; i < 1000; ++i) {
    int x2 = 20000, y2 = 0;
    inputkit_radial_dead_zone(&x2, &y2, 0.2f);
    if (x2 != x || y2 != y) {
      check(false, "the dead zone gives the same stick however often it runs", 0);
      break;
    }
  }
  ++checks;
  std::printf("inputkit frame rate: %d checks, %d failed\n", checks, failures);
  return failures != 0;
}
