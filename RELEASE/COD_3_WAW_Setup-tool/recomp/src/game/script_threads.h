// cod3 - the game's script threads on host fibers (see script_threads.cpp).

#pragma once

#include <string>

namespace cod3 {

// One line of counters for the log: threads begun, resumed, finished,
// destroyed and suspended right now. Call on the game's main thread.
std::string ScriptThreadSummary();

// For the hang watchdog, from any thread: which fiber runs and whether a
// switch is under way, and the counters so far.
std::string ScriptThreadWatchLine();

}  // namespace cod3
