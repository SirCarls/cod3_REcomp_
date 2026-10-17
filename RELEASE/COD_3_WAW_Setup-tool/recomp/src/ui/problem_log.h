// cod3 - every warning and error logged this session, grouped, for F2 >
// Console > Problems and the console's `problems` command.
//
// A log sink (spdlog, added to every logger through rex::AddSink) sees each
// line the game, the port and the SDK log. It keeps the warnings and errors
// (and the game's own "Warning: ..." / "ERROR: ..." prints, which reach the
// log as "[game] ..." info lines), grouped by message with the numbers and
// quoted names left out, so 14 "NtCreateFile FAILED: path='d:\_thai\' ->
// 0xc000000f" lines for 14 folders are one row with a count. Each group
// remembers the code its lines carry, named where it is a known one:
//   NTSTATUS / X_STATUS (0xC000000F STATUS_NO_SUCH_FILE), HRESULT
//   (0x80070005 E_ACCESSDENIED, 0x887A0005 DXGI_ERROR_DEVICE_REMOVED),
//   Win32 errors inside an HRESULT, Vulkan results (ErrorDeviceLost),
//   names the SDK prints (X_STATUS_..., VK_ERROR_...), and the game's
//   Com_Error code (ERR_DROP ...).
//
// Bounded: at most kMaxGroups groups (later new kinds are only counted),
// each line cut to kMaxText characters. Lines at info level cost a prefix
// check and nothing else.

#pragma once

#include <chrono>
#include <cstdint>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace cod3::ui {

struct Problem {
  enum class Severity : uint8_t { kWarning, kError };
  Severity severity = Severity::kWarning;
  std::string source;  // "game", "port", or the SDK's log category ("gpu", "krnl", ...)
  std::string code;    // "0xC000000F STATUS_NO_SUCH_FILE", or empty
  std::string text;    // the newest line of the group
  std::vector<std::string> examples;  // up to kMaxExamples other lines of the group
  uint64_t count = 0;
  std::chrono::system_clock::time_point first_seen;
  std::chrono::system_clock::time_point last_seen;
  uint64_t last_order = 0;  // larger = newer
};

class ProblemLog {
 public:
  static constexpr size_t kMaxGroups = 400;
  static constexpr size_t kMaxText = 600;
  static constexpr size_t kMaxExamples = 3;

  static ProblemLog& Get();

  // Adds the log sink, once. Call early (Cod3App::OnPreSetup): lines logged
  // before it are not seen.
  void Install();

  // Called by the sink for every log line (any thread).
  void OnLogLine(int spdlog_level, std::string_view category, std::string_view text,
                 std::chrono::system_clock::time_point when);

  // Changes whenever something is added or cleared.
  uint64_t generation() const;
  size_t group_count() const;
  uint64_t error_lines() const;
  uint64_t warning_lines() const;
  uint64_t dropped_lines() const;  // lines of kinds that came after kMaxGroups

  // Newest first.
  void CopyNewestFirst(std::vector<Problem>& out) const;
  void Clear();

  // One line per group, newest first, for the clipboard and the console:
  // "[21:14:03] warning x14 krnl 0xC000000F STATUS_NO_SUCH_FILE: [NtCreateFile] ..."
  static std::string FormatLine(const Problem& p);
  std::string AsText(size_t max_groups = 0) const;

 private:
  ProblemLog() = default;
  // `code` empty: found in the text (FindErrorCode) when the group has none yet.
  void Add(Problem::Severity severity, std::string_view source, std::string_view text,
           std::chrono::system_clock::time_point when, std::string code);

  mutable std::mutex mutex_;
  std::vector<Problem> groups_;
  std::unordered_map<std::string, size_t> index_;  // grouping key -> position in groups_
  uint64_t order_ = 0;
  uint64_t generation_ = 0;
  uint64_t errors_ = 0;
  uint64_t warnings_ = 0;
  uint64_t dropped_ = 0;
  size_t last_game_group_ = SIZE_MAX;  // the group of the last "[game]" line, for repeat counts
  std::string last_com_error_;         // Com_Error's text, so its "[game] ERROR:" echo isn't counted twice
  bool installed_ = false;
};

// The name of a known status / error number (0xC000000F -> "STATUS_NO_SUCH_FILE"),
// or empty.
std::string_view StatusCodeName(uint32_t code);
// The code a log line carries, formatted ("0xC000000F STATUS_NO_SUCH_FILE",
// "0x80070002 HRESULT for Win32 error 2 ERROR_FILE_NOT_FOUND", "ErrorDeviceLost"),
// or empty.
std::string FindErrorCode(std::string_view text);

}  // namespace cod3::ui
