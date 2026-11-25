// inputkit - the mouse's motion from the window to the game, and the PC turn it makes: shared by every game.
//
// [input-port] 2026-11-24, taken out of Call of Duty 3's game/mouse_look.{h,cpp} (claude-9b, owner 2026-10-29 "the
// way you turn with kbm now has some judder"; on by default since 2026-11-02, the owner confirmed it).
//
// MotionQueue: the window thread hands the mouse's counts over in batches, as often as it runs its event loop; the
// game takes them once per command (usercmd) it builds. Taken whole, some frames above the event loop's rate turned
// by nothing and the next by a whole batch: an uneven turn at high frame rates. With `resample` on, each batch is
// spread over the time since the batch before (at most 8 ms) and a take gets the motion up to "now less the longest
// recent gap between batches" (at most 8 ms): every frame turns by its own share. Every count is applied in the end;
// the cost is up to that gap of delay. With it off, a take gets everything that arrived.
// [input-port] 2026-11-24 (owner: "every game needs to be able to function properly regardless of FPS"): the wait was
// the average gap; when the event loop's batches come unevenly (1 to 6 ms apart), a frame reading just before a late
// batch found nothing due and the next found double, so above ~144 fps a steady hand turned unevenly from frame to
// frame (7-59% spread, tests/test_frame_rate.cpp). Waiting for the longest recent gap (forgotten slowly, toward the
// average) keeps every frame's share steady at 20 to 1000 fps (under 9%), for 1-2 ms more delay at most.
//
// Every game uses it the same way: the window's mouse listener calls Add, the game's own read (CoD3's
// CL_AdjustAngles, the IW games' CL_GetMouseMovement, Big Red One's pad poll) calls Take, and Clear when the mouse
// is taken or let go. A game with several local players takes only for the player the mouse drives, and must decide
// that before it takes (CoD3's split screen lost player 1's motion to players 2-4 until 2026-11-24).
//
// Turn: the World at War / Quake III mouse look, for games that turn the view themselves (CoD3, Big Red One):
//     yaw   -= dx * sensitivity * m_yaw
//     pitch += dy * sensitivity * m_pitch    (x -1 when inverted)
// with the sensitivity scaled by (FOV / base FOV) x the zoom sensitivity while zoomed in, and by the game's own
// multipliers (weapon groups, the controller rows). Games whose code has the PC's mouse look (World at War, CoD2,
// CoD4) give it the counts instead and let the game turn.
//
// Thread-safe (one mutex); no allocation after construction.

#pragma once

#include <array>
#include <cstdint>
#include <mutex>

namespace inputkit::mouse {

class MotionQueue {
 public:
  // Window thread: counts that arrived at `now_ns` (a monotonic clock, e.g. xplat_monotonic_ns).
  void Add(float dx, float dy, int64_t now_ns, bool resample);
  // Game thread: the motion due by `now_ns` (everything, for batches added with resample off).
  void Take(int64_t now_ns, float* dx, float* dy);
  // Either thread: drop what isn't taken yet.
  void Clear();
  // The batches' typical spacing (an average; 0 before two batches), for logs.
  double BatchIntervalNs() const;

 private:
  struct Batch {
    int64_t start_ns, end_ns;
    float dx, dy;
    float taken;  // the share already taken
  };
  static constexpr int64_t kMaxSpanNs = 8'000'000;
  static constexpr size_t kRing = 256;  // never read for a while: the newest are kept

  mutable std::mutex mutex_;
  float whole_dx_ = 0.0f, whole_dy_ = 0.0f;  // added with resample off
  std::array<Batch, kRing> ring_{};
  size_t head_ = 0, size_ = 0;  // the oldest batch, and how many
  int64_t last_batch_ns_ = 0;
  double batch_interval_ns_ = 0.0;
  double longest_gap_ns_ = 0.0;  // the longest recent time between batches, slowly forgotten
};

struct Turn {
  double sensitivity = 5.0;     // World at War's scale (its default 5)
  double m_yaw = 0.022;         // degrees a count at sensitivity 1
  double m_pitch = 0.022;
  double zoom_ratio = 1.0;      // FOV / base FOV now (1: not zoomed in); below 0.999 counts as zoomed
  double zoom_sensitivity = 1.0;  // on top of the ratio while zoomed
  double multiplier = 1.0;      // the game's own: weapon groups, rows ...
  double scale_x = 1.0, scale_y = 1.0;
  bool invert = false;
};

// The view's change for these counts: *yaw_delta to add to yaw, *pitch_delta to add to pitch (degrees).
void TurnFromCounts(const Turn& t, float dx, float dy, float* yaw_delta, float* pitch_delta);

// FOV / base FOV for Turn::zoom_ratio: 1 when either is unknown, else clamped to 0.05 .. 1 (CoD3's SetViewFov).
double ZoomRatio(double fov, double base_fov);

}  // namespace inputkit::mouse
