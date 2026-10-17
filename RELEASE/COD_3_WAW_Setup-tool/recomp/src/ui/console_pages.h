// cod3 - F2 > Console: the ~ console itself, and this session's problems.
//
// The Console page is the drop-down console's own view (ui/console_view.h)
// in F2's window: the same output, input line, completion, history, channels
// and colors. The Problems page lists every warning and error the game, the
// port and the SDK logged this session (ui/problem_log.h), grouped with
// counts and their codes, newest first, and copies them for a bug report.

#pragma once

#include <memory>

namespace cod3::ui {

class ConsoleView;

// `view` is created on first use and lives as long as the F2 window.
void DrawConsolePage(std::shared_ptr<ConsoleView>& view);
void DrawProblemsPage();
// "PROBLEMS (12)###problems": the sub-tab's label with the number of kinds.
const char* ProblemsTabLabel();

}  // namespace cod3::ui
