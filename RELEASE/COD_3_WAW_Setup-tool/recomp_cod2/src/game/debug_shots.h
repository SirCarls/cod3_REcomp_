// cod2 - pictures of the game for test runs. [acct2-cod2]
//
// With cod2_debug_shot_seconds above 0, every that many seconds the game's
// own picture (the frame the GPU emulation last sent out, read back with the
// SDK's Presenter::CaptureGuestOutput; never the desktop or another window)
// is measured, and a line goes to the log: its size, mean brightness, the
// share of pixels that aren't near black, how many coarse colors it has and a
// fingerprint, so a run can be judged from the log alone. When the picture
// changed, it is also saved as userdata/logs/screenshots/auto-<n>.ppm (at most
// 50 files a run). Capturing runs on a thread of its own.

#pragma once

#include <filesystem>
#include <functional>

namespace rex::ui {
struct RawImage;
}

namespace cod2::shots {

using CaptureFn = std::function<bool(rex::ui::RawImage&)>;

// Starts the thread when cod2_debug_shot_seconds is above 0.
void Start(const std::filesystem::path& folder, CaptureFn capture);

// Stops the thread (before the GPU emulation goes).
void Stop();

}  // namespace cod2::shots
