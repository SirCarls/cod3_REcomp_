// cod4 / cod4mp - a sampling profile of one thread, for the frame-rate audit. [acct2-cod4]
//
// With cod4_debug_profile_thread set (a thread-name prefix, e.g. "Backend", the game's render thread), a helper
// thread waits cod4_debug_profile_start seconds, then interrupts that thread about 1,000 times a second for
// cod4_debug_profile_seconds and records the host code address it was running each time. The counts per module
// and offset go to <logs>/profile-<prefix>.txt; results/acct2-cod4-scripts/profmap.py turns them into functions
// (the recompiled sub_XXXXXXXX are named by their guest addresses). Linux only (a signal to the thread); elsewhere
// the setting only logs that it isn't there.

#pragma once

#include <filesystem>

namespace cod4::profile {

// Starts the helper thread if cod4_debug_profile_thread is set. log_dir: where the counts file goes.
void Start(const std::filesystem::path& log_dir);

// Stops it (at shutdown), without writing a partial profile.
void Stop();

}  // namespace cod4::profile
