// cod3 - the in-engine thread profiler: which thread limits the frame rate,
// found inside the game, with no outside tools (owner, 2026-10-13).
//
// Once a second, on its own thread ("cod3 profiler"), it reads for every
// thread of the game's process:
//   - its CPU time from the OS (Linux /proc/self/task/<id>/schedstat,
//     Windows GetThreadTimes), as % of one core;
//   - its name from the OS (the SDK names its threads: "GPU Commands (...)",
//     "Main XThread (...)", "XThread<id>" for the game's other threads);
//   - how long it waited, and on what (rex/thread/thread_stats.h, SDK patch
//     "cpu thread timing"): the game's own events and threads, sleeps,
//     contended spinlocks (spinning looks like work to the OS) and the SDK's
//     global lock.
//
// With the frame costs (the emulated GPU's counters, rex/graphics/gpu_stats.h,
// and the game's own frame times, game/frame_rate.h) it names what limits the
// frame rate: the frame limit, all CPU cores busy, the graphics card, the GPU
// emulation thread, the game's main thread, or the main thread waiting on the
// game's other threads.
//
// It runs by itself from the start and writes to the log (game/diagnostics.h
// detail levels):
//   - always: what this PC has (game/machine_profile.h), once the game runs;
//     the per-thread table of any second with a frame of 250 ms or more
//     (at most one every 10 seconds), to explain hitches; and the table when
//     the game's main thread is stuck (no watchdog mark for
//     cod3_watchdog_seconds, game/watchdog.h), then every minute, three times
//     at most, to say what every other thread does meanwhile;
//   - verbose: "limited by ..." every 10 s (cod3_perf_log N: every N s), the
//     table every minute, and hitch tables from 100 ms;
//   - debug: "limited by ..." every 2 s.
// The overlay and `cod3_report` can show the same (Report(), Verdict(),
// Table()).
//
// Cost: one pass over the process's threads a second (two small reads per
// thread on Linux, one OS query per thread on Windows; about 1-2 ms for 70
// threads), on its own thread, so no game thread ever waits for it.

#pragma once

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace cod3 {

// What the frame cost, per frame, over the last second.
struct FrameCosts {
  bool valid = false;
  float frame_ms = 0;       // wall time per presented frame
  float game_ms = 0;        // the game's main thread, limiter sleep excluded
  float gpu_thread_ms = 0;  // the GPU emulation thread, busy (0 = SDK doesn't count it)
  float gpu_wait_ms = 0;    //   of which waiting for the graphics card
  float submit_ms = 0;      //   of which handing commands to the driver
  float shader_ms = 0;      //   of which translating shaders while a draw waited
  int max_fps = 0;          // com_maxfps (0 = no limit)
  // The game's own frames (Com_Frame) per second. Not the same clock as
  // frame_ms (presented frames): the frame limit applies to these.
  float game_fps = 0;
  float slowest_frame_ms = 0;  // the slowest new game frame since the last sample (0 = none)
};

struct ThreadRow {
  uint32_t id = 0;
  std::string name;  // as the OS has it
  std::string role;  // what the thread is, when known ("GPU emulation", ...)
  float cpu_pct = 0;  // CPU time, % of one core
  // Waits, % of the period: the game's own events/threads, sleep, spinlock,
  // the SDK's global lock (see rex/thread/thread_stats.h).
  float guest_wait_pct = 0, sleep_pct = 0, spin_pct = 0, lock_pct = 0;
  // Who held the global lock while this thread waited for it, busiest
  // first: "Main XThread 11%, rex::thread::Ti 5%" (Linux; SDK thread stats 2).
  std::string lock_holders;
};

struct ThreadReport {
  bool valid = false;
  FrameCosts costs;             // what the verdict was made from
  std::vector<ThreadRow> rows;  // busiest first
  float process_cpu_pct = 0;    // all threads together, % of one core
  int busy_threads = 0;         // threads above 85% of a core
  std::string limited_by;       // "the GPU emulation thread", ...
  std::string why;              // the numbers behind it
};

class ThreadProfiler {
 public:
  // The first call starts the profiler's thread (the game starts it at load).
  static ThreadProfiler& Get();

  // Optional: the game's main thread can say who it is (from any of its
  // hooks; cheap). Without it, the thread is found by its SDK name.
  void NoteGameMainThread();

  ThreadReport Report();

  // "limited by X (why)", for the log line and the overlay.
  static std::string Verdict(const ThreadReport& report);
  // The busiest threads, one per line, for the report and the log.
  static std::string Table(const ThreadReport& report, size_t max_rows);
  // "A 52%, B 50%, C 32%": the busiest few, for one line.
  static std::string Busiest(const ThreadReport& report, size_t count);

 private:
  ThreadProfiler() = default;
  void Start();
  void WorkerLoop();
  void Compute(const FrameCosts& costs);  // profiler thread

  struct Previous {
    uint64_t cpu_ns = 0;
    uint64_t wait_us[4] = {};
    std::unordered_map<uint32_t, uint64_t> holder_us;  // global lock wait by holder
  };

  std::mutex mutex_;  // report_, worker_started_
  bool worker_started_ = false;
  ThreadReport report_;
  // Profiler thread only:
  std::unordered_map<uint32_t, Previous> previous_;
  uint64_t previous_time_us_ = 0;
  std::atomic<uint32_t> main_thread_id_{0};
  uint32_t ui_thread_id_ = 0;
};

}  // namespace cod3
