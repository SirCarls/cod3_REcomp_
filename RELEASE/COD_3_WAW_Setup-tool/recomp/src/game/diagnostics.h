// cod3 - detailed logging and reports, for testing on the player's own PC.
//
// cod3_log_detail (F2 > Performance > Logging, or the console):
//   normal   what the game always logged;
//   verbose  + slow frames (each frame well over the average, with what the
//            emulated GPU did in it), level load times, a cost line every
//            10 seconds, the settings at start and on every change;
//   debug    + the SDK's own debug messages, a cost line every 2 seconds and
//            every new resolve copied back to the CPU.
// Every session starts with a header (build, OS, CPU threads, the settings
// that matter for speed and looks), whatever the detail.
//
// `cod3_report` in the ~ console (or F2's "Write a report") writes
// userdata/logs/report-<date>-<time>.txt: that header, every setting that
// isn't at its default, the last minute of per-frame costs, the slowest
// frames and the level load times. One file to attach to a bug report.
//
// Logs and reports stay on the player's computer; they hold the GPU name
// (from the SDK's own log) but no paths beyond the game folder's own.

#pragma once

#include <cstdint>
#include <filesystem>
#include <string>

namespace cod3::diag {

enum class Detail { kNormal = 0, kVerbose = 1, kDebug = 2 };
Detail GetDetail();
inline bool Verbose() { return GetDetail() >= Detail::kVerbose; }

// Once at start (after the settings are loaded): applies the detail to the
// SDK's log settings, logs the session header, and follows later changes.
void Start(const std::filesystem::path& logs_dir);

// Guest main thread, every frame (game/frame_rate.cpp): frame time and the
// game thread's share of it, for the slow frame log.
void OnFrame(double frame_s, double busy_s);

// Guest main thread: a level load (SV_SpawnServer) starts / its briefing
// film starts playing on after the load / the load ends.
void LevelLoadBegin(const std::string& map);
void LevelLoadMovie();
void LevelLoadEnd();

// Any thread: the per-second cost report (game/perf_stats.h) for the history.
void RecordPerfLine(const std::string& line);

// Writes the report; returns its path (empty on failure).
std::filesystem::path WriteReport();

// [hw-scaling] The folder the log and the reports go to (userdata/logs);
// the function profiler writes there too (game/function_profiler.h).
std::filesystem::path LogsDir();

// The settings summary used by the header and the report.
std::string SettingsSummary();

// [claude-9b] 2026-10-27 (owner 18:31 / 18:33): the setup tool version this game was built with ("2.22.0"), from
// the "tool_version" the setup tool writes into cod3_waw.json next to assets/ and userdata/ at every build; empty
// when the game wasn't built by the setup tool. Read once. In the log's session line, the report and the HUD label.
std::string ToolVersion();

}  // namespace cod3::diag
