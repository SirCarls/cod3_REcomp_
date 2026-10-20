// cod3 - the console's view: scroll-back output, input line, completion and
// history.
//
// One implementation for both places the console is shown: the drop-down ~
// console (GameConsoleDialog, ui/game_console.h) and F2 > Console
// (ui/console_pages.h). Both type into the same GameConsole, so they share
// the output, the history and every command; a line typed in one shows in
// the other.
//
// Tab completes (longest common part first, then cycles), Up / Down walk the
// history (or the matches while they are shown), PgUp / PgDn scroll the
// output, Ctrl+L clears it, right-click on it copies it. The completion box
// is World at War's: for one match the dvar's value, default, domain
// (ui/dvar_domain.h), flags and help line (ui/dvar_help.h); for several, each
// name with its value and domain, and the highlighted one's help line.
//
// The view exists only while it is shown. It recomputes the matches only when
// the typed text or the game's dvar list changes, never per frame.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <imgui.h>

#include "game/engine.h"
#include "ui/game_console.h"

struct ImGuiInputTextCallbackData;

namespace cod3::ui {

namespace waw {
struct Dvar;
}

// "S U R I A L C T" flag letters (serverinfo, userinfo, read-only, init,
// archived, latched, cheat, temp), as cvarlist shows them.
std::string ConsoleFlagString(uint32_t flags);
ImVec4 ConsoleChannelColor(ConsoleChannel channel);
// Text with ^N color codes, on one line.
void DrawConsoleText(std::string_view text, ImVec4 base_color);

class ConsoleView {
 public:
  ConsoleView();

  // Each frame the view is drawn, first: keeps the completion data fresh
  // (the game's dvars and commands, asked for twice a second).
  void Refresh();
  // The scroll-back output, `height` pixels tall.
  void DrawOutput(float height);
  // The prompt and the input line, with `trailing` (may be null) at its
  // right. keep_focus: the input takes the keyboard back whenever nothing
  // else has it (the drop-down console); otherwise only after FocusInput()
  // and after a line is entered.
  void DrawInputLine(const char* trailing, bool keep_focus);
  // The completion box, at `pos`: its top-left corner, or with `above` its
  // bottom-left corner. Drawn over everything (an ImGui tooltip window).
  void DrawSuggestions(ImVec2 pos, bool above, float width);

  void FocusInput() { focus_input_ = true; }
  bool input_active() const { return input_active_; }
  ImVec2 input_min() const { return input_min_; }
  ImVec2 input_max() const { return input_max_; }

 private:
  struct Suggestion {
    enum class Type : uint8_t { kGameDvar, kCommand, kWaw, kPortSetting };
    Type type = Type::kCommand;
    std::string name;
    std::string value;   // empty for commands
    std::string reset;
    std::string domain;  // a few words (domain::Brief), for the list
    uint32_t flags = 0;  // game dvars
    const waw::Dvar* waw = nullptr;
  };

  static int InputCallback(ImGuiInputTextCallbackData* data);
  void UpdateSuggestions(std::string_view text);
  void DescribeSingleMatch();
  void DrawSingleMatch(ImVec4 name_col, ImVec4 value_col, ImVec4 dim_col);

  bool focus_input_ = true;
  bool input_active_ = false;
  bool scroll_to_bottom_ = true;
  char input_[512] = {};
  int history_pos_ = -1;
  std::string typed_prefix_;
  std::vector<Suggestion> suggestions_;
  int suggestion_sel_ = -1;
  // The one match's details: World at War's domain lines and a description.
  std::vector<std::string> single_domain_;
  std::string single_help_;
  std::string single_flags_;  // game dvars: ConsoleFlagString
  uint64_t snapshot_gen_ = 0;
  EngineSnapshot snapshot_;
  double last_snapshot_request_ = 0.0;
  std::vector<ConsoleLine> lines_;
  uint64_t lines_gen_ = ~0ull;
  std::vector<std::string> port_settings_;  // cod3_* settings, settable by name
  ImVec2 input_min_{0, 0}, input_max_{0, 0};
};

}  // namespace cod3::ui
