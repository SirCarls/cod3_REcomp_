// cod3 - the in-engine function profiler: which functions one thread spends
// its CPU time in, found by sampling, with no outside tools (owner,
// 2026-10-13: "I want this to be in-engine").
//
// The thread profiler (game/thread_profiler.h) says which thread limits the
// frame rate; this says what that thread is doing. On the owner's PC the
// game's main thread runs at 86-95% of a core in fights, and the window
// thread at 93-97% all the time (2026-10-15 reports): this finds out why.
//
// `cod3_profile <thread> <seconds>` in the F10 log console (the SDK's: it
// runs commands, and shows the result line too) starts a capture 3 s later,
// so the console can be closed first (an open console repaints the window
// continuously and would be measured too). <thread> is main (the game's main
// thread, the default), gpu (GPU
// emulation), window (window, input and presenting), or the start of a
// thread's name as the thread table shows it ("XThread8212A780" for a job
// worker). The report goes to userdata/logs/profile-<thread>-<time>.txt.
//
// How it samples, about once per millisecond of the thread's CPU time (time
// it spends waiting isn't sampled, so the percentages are of its work):
//   - Linux: a timer on the thread's own CPU clock sends it SIGPROF; the
//     handler only stores the interrupted instruction address (and the
//     caller candidates, below). CPU-time
//     timers are only as fine as the kernel's tick (HZ, 100 to 1000 a second
//     by kernel), so a kernel with 250 gives at most 250 samples a second;
//     how busy the thread was comes from its CPU clock.
//   - Windows: a helper thread suspends it, reads its instruction address and
//     resumes it, when its CPU cycle count has moved since the last look.
// Names come from the game's and the libraries' symbol tables (Linux; the
// recompiled functions keep readable names such as Com_Frame or
// sub_82536DD0), and for the recompiled code also from the guest-to-host
// function table, so every game function is reported with its Xbox 360
// address (both platforms).
//
// Code without a name of its own (the C library's memcpy, a graphics
// driver) is reported by who called it: each sample also keeps a few words
// from the thread's stack that point into code, and the first one right after
// a call instruction is taken as the return address. That is a good guess,
// not a certainty (a function with a large stack frame can hold an old one).
// The C library's memory and string routines are named even without its
// debug symbols (see AddCLibraryNames), and its debug symbols are read when
// the distribution's package for them is installed (libc6-dbg on Ubuntu).
//
// On Linux each sample also notes whether the thread held the SDK's global
// lock (glibc keeps the owner's thread id in the mutex), and the report lists
// what the thread did while holding it, with callers: what the other threads
// that wait for the lock (the GPU emulation, mostly) are waiting behind.
//
// Cost: nothing while not capturing; about 1% of the sampled thread while
// capturing.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace cod3 {

class FunctionProfiler {
 public:
  // Starts a capture of `thread` (see above) for `seconds` (1-120) after
  // `delay_seconds`, on its own thread. Returns a line for the console: what
  // will happen, or why it can't (a capture already running, no such
  // thread, a platform without support).
  static std::string Start(const std::string& thread, int seconds, int delay_seconds);

  // True while a capture runs (or waits for its delay).
  static bool Busy();

  // This session's profiles, a few lines each (the modules and the hottest
  // functions), for cod3_report. The full reports stay in userdata/logs; the
  // newest 30 are kept.
  static std::vector<std::string> SessionSummaries();
};

}  // namespace cod3
