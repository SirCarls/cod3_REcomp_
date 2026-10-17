// cod3 - screenshots for debugging and bug reports.
//
// Only the game's own picture is saved: the frame the emulated GPU last sent
// to the screen, at the game's render resolution, read back from the GPU
// emulation (the SDK's Presenter::CaptureGuestOutput). Never the desktop,
// another window, or the port's own windows over the game (F2, the console):
// nothing is read from the screen or the window system.
//
// Files go to userdata/logs/screenshots/ (PNG):
//   shot-<date>-<time>.png    asked for: `screenshot` in the ~ console, or
//                             F2 > Performance > Logging > Take a screenshot;
//   auto-<date>-<time>.png    taken by the debug log (cod3_log_detail debug):
//                             every cod3_debug_screenshots seconds (60; 0 =
//                             none) while the picture changes, and when the
//                             game's main thread gets stuck (game/watchdog.h;
//                             at most one a minute);
//   report-<date>-<time>.png  next to a report written at debug detail;
//   hd-compare-<date>-<time>-on.png and -off.png
//                             `cod3_hd_compare` in the console, or F2 > Performance
//                             > Logging > Compare HD textures: the same view
//                             with the HD textures, then without them.
// Only the newest 100 auto- files are kept (older auto- files are removed);
// the others are never removed by the game. Each file is named in the log.
//
// Capturing and writing run on a thread of their own, so the game never
// waits for them.

#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace rex::ui {
struct RawImage;
}

namespace cod3::shot {

// Reads the game's latest picture (the presenter's guest output); false when
// there is none yet.
using CaptureFn = std::function<bool(rex::ui::RawImage&)>;

// Once, when the GPU emulation is up: where the files go
// (userdata/logs/screenshots) and how to read the picture. Starts the thread.
void Start(const std::filesystem::path& folder, CaptureFn capture);

// At shutdown, before the GPU emulation goes: waits for a capture under way,
// then no more are taken.
void Stop();

// Whether screenshots can be taken (Start ran, Stop didn't).
bool Available();

enum class Kind {
  kAsked,   // shot-*.png: the player asked
  kAuto,    // auto-*.png: the debug log
  kReport,  // report-*.png: with a report (name given)
  kCompare, // hd-compare-*-on.png / -off.png (RequestHdCompare)
};

// Any thread: queues a screenshot. `reason` goes in the log line. For
// kReport, `name` is the file name without ".png" (the report's own name).
// Returns the file name it will have (under userdata/logs/screenshots), or
// an empty string when none was queued (not available, or one asked for
// less than half a second ago).
std::string Request(Kind kind, std::string_view reason, std::string_view name = {});

// Any thread: the same view twice, with the HD textures and without them:
// a screenshot, then the SDK's live switch texture_replacement_enabled off
// for `seconds_off` (the game draws its own textures again from the next
// frame), a second screenshot, and the switch back on. Returns the files'
// common name (hd-compare-<date>-<time>), or an empty string when none was
// queued. The caller checks that the HD textures are in use.
std::string RequestHdCompare(double seconds_off);

// Whether the debug log takes screenshots now (cod3_log_detail debug and
// cod3_debug_screenshots above 0).
bool AutoEnabled();

}  // namespace cod3::shot
