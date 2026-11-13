// f2 - the CONSOLE page (see f2_console.h). [f2-menus]

#include "f2/f2_console.h"

#include <algorithm>
#include <cstring>

#include <imgui.h>

#include "f2/f2_rows.h"
#include "f2/f2_theme.h"

namespace f2 {

ConsoleLog& ConsoleLog::Get() {
  static ConsoleLog log;
  return log;
}

void ConsoleLog::Add(int channel, std::string_view line) {
  std::lock_guard<std::mutex> lock(mutex_);
  Line& slot = lines_[added_ % kLines];
  slot.channel = channel;
  slot.text.assign(line.data(), line.size());
  ++added_;
}

uint64_t ConsoleLog::ForEach(const std::function<void(int, const std::string&)>& fn) const {
  std::lock_guard<std::mutex> lock(mutex_);
  const uint64_t kept = std::min<uint64_t>(added_, kLines);
  for (uint64_t i = added_ - kept; i < added_; ++i) {
    const Line& line = lines_[i % kLines];
    fn(line.channel, line.text);
  }
  return added_;
}

void ConsoleLog::Clear() {
  std::lock_guard<std::mutex> lock(mutex_);
  for (Line& line : lines_) line.text.clear();
  added_ = 0;
}

namespace {

// Up and Down walk through what was typed before, as in the game's console.
int HistoryCallback(ImGuiInputTextCallbackData* data) {
  auto* page = static_cast<ConsolePage*>(data->UserData);
  if (data->EventFlag != ImGuiInputTextFlags_CallbackHistory || page->history.empty()) return 0;
  const int last = static_cast<int>(page->history.size()) - 1;
  if (data->EventKey == ImGuiKey_UpArrow)
    page->history_pos = page->history_pos < 0 ? last : std::max(0, page->history_pos - 1);
  else if (data->EventKey == ImGuiKey_DownArrow && page->history_pos >= 0)
    page->history_pos = page->history_pos >= last ? -1 : page->history_pos + 1;
  data->DeleteChars(0, data->BufTextLen);
  if (page->history_pos >= 0) data->InsertChars(0, page->history[page->history_pos].c_str());
  return 0;
}

}  // namespace

void DrawConsolePage(ConsolePage& page, const std::function<void(const std::string&)>& run) {
  Help("The game's own console: what it prints, and a line to type its commands into (the same commands as its "
       "console, such as cg_drawFPS 1 or devmap mak). A command runs at the game's next frame.");
  const float input_height = ImGui::GetFrameHeightWithSpacing();
  ImGui::BeginChild("##console_lines", ImVec2(0, -input_height), ImGuiChildFlags_Borders,
                    ImGuiWindowFlags_HorizontalScrollbar);
  ImGui::PushFont(MonoFont(), 0.0f);
  const bool at_bottom = ImGui::GetScrollY() >= ImGui::GetScrollMaxY() - 1.0f;
  const uint64_t added = ConsoleLog::Get().ForEach([](int channel, const std::string& text) {
    // Channel numbers are the game's; the error channel's lines in the warning color.
    (void)channel;
    const bool error = text.find("ERROR") != std::string::npos || text.find("Error") != std::string::npos;
    if (error) ImGui::PushStyleColor(ImGuiCol_Text, CurrentColors().bad);
    ImGui::TextUnformatted(text.c_str());
    if (error) ImGui::PopStyleColor();
  });
  // Follow new lines while the view is at the bottom (scrolled up, it stays where the player put it).
  if (added != page.seen && at_bottom) ImGui::SetScrollHereY(1.0f);
  page.seen = added;
  ImGui::PopFont();
  ImGui::EndChild();

  ImGui::SetNextItemWidth(-1.0f);
  const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackHistory;
  if (ImGui::InputTextWithHint("##console_input", "Type a console command and press Enter", page.input,
                               sizeof(page.input), flags, HistoryCallback, &page)) {
    std::string command(page.input);
    while (!command.empty() && (command.back() == ' ' || command.back() == '\t')) command.pop_back();
    if (!command.empty()) {
      run(command);
      page.history.erase(std::remove(page.history.begin(), page.history.end(), command), page.history.end());
      page.history.push_back(command);
      if (page.history.size() > 64) page.history.erase(page.history.begin());
    }
    page.history_pos = -1;
    page.input[0] = '\0';
    ImGui::SetKeyboardFocusHere(-1);  // keep typing
  }
}

}  // namespace f2
