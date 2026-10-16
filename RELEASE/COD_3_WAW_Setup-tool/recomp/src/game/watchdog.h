// cod3 - a hang watchdog, and on Windows a last-chance crash logger.
//
// Hang watchdog: the game's main thread marks a few points as it passes them
// (every frame, each step of the level loader, each screen update, script
// threads starting and resuming, SV_SpawnServer). A host thread looks once a
// second; when no mark has come for cod3_watchdog_seconds (default 10) it
// logs, and flushes:
//   - the last point the game thread passed;
//   - its guest registers (lr = the return address of its last call, r1, r3)
//     and, when lr is a known kernel call site, which call that is (a wait,
//     a file read, a XAM message ...);
//   - the script-thread fibers: which one runs, and whether a switch to or
//     from one is under way.
// It repeats every minute while the thread stays stuck (a few times at most)
// and says when it moves again. cod3_watchdog_seconds 0 turns it off. Costs
// one wake-up a second and a counter per mark.
//
// Windows: an exception nothing handles (an access violation, an illegal
// instruction ...) and abort() are logged with the code, the address and its
// module, the host call stack and the game thread's state, and the log is
// flushed before the process ends. (Under Proton a crash otherwise leaves the
// log without a word: owner's log, 2026-10-12 and 10-13.)

#pragma once

#include <cstdint>

struct PPCContext;

namespace cod3::watchdog {

// Once at start-up, after logging is set up.
void Start();

// Game thread: passed a marked point. `where` must be a string literal.
// Calls from any other thread are ignored.
void Mark(const char* where);

// Game thread, from Com_Frame: this thread is the game thread.
void MarkFrame();

// Game thread: its guest context (for lr, r1 and r3 in a report).
void SetGameContext(const PPCContext* ctx);

// [hw-scaling] How many marks the game thread has passed so far; any thread
// may read it. The thread profiler (game/thread_profiler.h) uses it to log
// what every thread does while the game thread is stuck.
uint64_t MarkCount();

}  // namespace cod3::watchdog
