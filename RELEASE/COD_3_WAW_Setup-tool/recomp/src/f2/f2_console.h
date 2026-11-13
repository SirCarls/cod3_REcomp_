// f2 - the CONSOLE page of the F2 window: the game's own console output and a line to type its commands into.
// [f2-menus]
//
// Call of Duty 3's F2 has the game's console on its CONSOLE page (recomp/src/ui/console_pages.h). The other games'
// ports give this page the lines their console print hook sees (ConsoleLog::Add, from the game's thread) and run
// what is typed through the game's own command buffer (the game's RunCommand, from its frame hook), so a command
// typed here does exactly what it does in the game's console.

#pragma once

#include <array>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

namespace f2 {

// The last lines the game printed: a ring of fixed slots, so adding a line doesn't allocate once the slots have grown
// to their longest line.
class ConsoleLog {
 public:
  static constexpr size_t kLines = 600;
  static ConsoleLog& Get();

  // Any thread (the game's print hook).
  void Add(int channel, std::string_view line);

  // UI thread: `fn(channel, text)` for every kept line, oldest first. Returns how many lines were ever added, so the
  // page can tell whether a new one came.
  uint64_t ForEach(const std::function<void(int, const std::string&)>& fn) const;
  void Clear();

 private:
  struct Line {
    int channel = 0;
    std::string text;
  };
  mutable std::mutex mutex_;
  std::array<Line, kLines> lines_{};
  uint64_t added_ = 0;  // lines ever added; the newest is at (added_ - 1) % kLines
};

// The page's own state (what's typed, the history), kept by the window between frames.
struct ConsolePage {
  char input[256] = {};
  std::vector<std::string> history;
  int history_pos = -1;
  uint64_t seen = 0;  // ConsoleLog's count at the last draw, to follow new lines
};

// Draws the page. `run` takes a typed command (the game queues it for its next frame); empty lines are ignored.
void DrawConsolePage(ConsolePage& page, const std::function<void(const std::string&)>& run);

}  // namespace f2
