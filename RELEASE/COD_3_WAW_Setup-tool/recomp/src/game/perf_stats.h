// cod3 - what each frame costs, for finding where the time goes.
//
// Once a second the monitor takes the GPU emulation's running totals
// (rex/graphics/gpu_stats.h, SDK patch 0012) and the game thread's frame
// times (frame_rate.h) and turns them into per-frame averages:
//
//   frame   - time per frame; game = the time the game's main thread worked
//             (the rest is the com_maxfps wait).
//   draws, resolves, transfers - the emulated GPU's work. Every resolve copies
//             a picture out of the EDRAM; every transfer moves one between
//             render targets.
//   GPU waits - how often, and how long, the emulator stopped to wait for the
//             graphics card. Each wait leaves the card's queue empty.
//   readback - resolves copied back for the game's CPU to read.
//   queries - occlusion queries (light flares).
//   upload  - guest memory sent to the GPU's copy; faults = CPU writes caught
//             by the write watch on memory the GPU uses.
//   GPU thread - [hw-scaling] the GPU emulation's one thread: busy time per
//             frame, of which submitting and translating shaders while a draw
//             waited (SDK "cpu thread timing" patch; 0 without it).
//   presents - [hw-scaling] pictures the window presented a second; well
//             above the frame rate means it repaints on its own (a CPU core
//             and graphics card time for nothing).
//   limited by - [hw-scaling] what limits the frame rate, from
//             game/thread_profiler.h.
//
// Shown with cod3_perf_overlay (F2 > Performance) and written to the log
// every cod3_perf_log seconds.

#pragma once

#include <cstdint>
#include <mutex>
#include <string>

namespace cod3 {

struct PerfReport {
  bool valid = false;
  float fps = 0, frame_ms = 0, game_ms = 0;
  // Per frame:
  float draws = 0, resolves = 0, transfers = 0, submissions = 0;
  float gpu_waits = 0, gpu_wait_ms = 0;
  float readbacks = 0, readback_kb = 0;
  float queries = 0;
  float upload_kb = 0, texture_loads = 0, watch_faults = 0;
  // [hw-scaling] (game/thread_profiler.h)
  float gpu_thread_ms = 0, submit_ms = 0, shader_ms = 0, shaders = 0;
  // [hw-scaling] pictures the window presented a second: the game's frames
  // plus repaints for the UI alone (0 = SDK doesn't count them).
  float presents_per_s = 0;
  std::string limited_by;  // "limited by ... (...)", empty until known
};

class PerfMonitor {
 public:
  static PerfMonitor& Get();

  // UI thread, every presented frame: refreshes the report once a second
  // and logs it when cod3_perf_log asks.
  void Tick();

  PerfReport Report();
  static std::string Describe(const PerfReport& report);

 private:
  PerfMonitor() = default;
  std::mutex mutex_;
  PerfReport report_;
};

}  // namespace cod3
