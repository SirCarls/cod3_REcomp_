// cod4 / cod4mp - debug screenshots: the game's own picture, saved and measured. [acct2-cod4]
//
// With cod4_debug_screenshots N above 0, every N seconds the frame the GPU emulation last sent to the screen (the
// SDK's Presenter::CaptureGuestOutput: never the desktop or another window) is written to
// userdata/logs/screenshots/auto-NNNN.ppm and the log gets its numbers: size, mean brightness, the share of pixels
// that aren't black, and the mean brightness of each cell of a 4 x 4 grid. A test run is judged by those numbers;
// the newest 30 pictures are kept. Off (0) by default. (The same as World at War's ../recomp_waw/src/waw_shots.*,
// under this program's names.)

#pragma once

#include <filesystem>
#include <functional>

namespace rex::ui {
struct RawImage;
}

namespace cod4::shots {

// Reads the game's latest picture; false when there is none yet.
using CaptureFn = std::function<bool(rex::ui::RawImage&)>;

// Once the GPU emulation is up: starts the thread when cod4_debug_screenshots is above 0.
void Start(const std::filesystem::path& folder, CaptureFn capture);

// At shutdown, before the presenter goes: ends the thread.
void Stop();

}  // namespace cod4::shots
