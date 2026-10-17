// cod3 - F2 > Console pages. See console_pages.h.

#include "ui/console_pages.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <vector>

#include <imgui.h>

#include "ui/console_view.h"
#include "ui/problem_log.h"
#include "ui/theme.h"

namespace cod3::ui {

namespace {

// "21:14:03" into `buf` (no allocation: it runs for each row shown, every frame).
const char* ClockText(std::chrono::system_clock::time_point t, char (&buf)[16]) {
  const std::time_t tt = std::chrono::system_clock::to_time_t(t);
  std::tm tm{};
#ifdef _WIN32
  localtime_s(&tm, &tt);
#else
  localtime_r(&tt, &tm);
#endif
  if (!std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm)) std::snprintf(buf, sizeof(buf), "--:--:--");
  return buf;
}

// The Problems page's copy of the list, refreshed when the log changed (at
// most four times a second) and only while the page is shown.
struct ProblemsPageState {
  std::vector<Problem> list;
  std::vector<int> shown;  // positions in `list` that pass the filters
  uint64_t generation = ~0ull;
  double last_refresh = -1.0;
  int severity_filter = 0;  // 0 all, 1 errors, 2 warnings
  ImGuiTextFilter text_filter;
  bool filters_changed = true;
  int selected = -1;  // position in `list`
};
ProblemsPageState& PageState() {
  static ProblemsPageState state;
  return state;
}

double NowSeconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

bool Passes(const ProblemsPageState& s, const Problem& p) {
  if (s.severity_filter == 1 && p.severity != Problem::Severity::kError) return false;
  if (s.severity_filter == 2 && p.severity != Problem::Severity::kWarning) return false;
  if (!s.text_filter.IsActive()) return true;
  return s.text_filter.PassFilter(p.text.c_str()) || s.text_filter.PassFilter(p.code.c_str()) ||
         s.text_filter.PassFilter(p.source.c_str());
}

void RefreshProblems(ProblemsPageState& s) {
  auto& log = ProblemLog::Get();
  const double now = NowSeconds();
  const uint64_t gen = log.generation();
  const bool changed = gen != s.generation && now - s.last_refresh >= 0.25;
  if (changed) {
    // Keep the selection on the same group (its first line and source).
    std::string selected_text, selected_source;
    if (s.selected >= 0 && s.selected < static_cast<int>(s.list.size())) {
      selected_text = s.list[s.selected].text;
      selected_source = s.list[s.selected].source;
    }
    log.CopyNewestFirst(s.list);
    s.generation = gen;
    s.last_refresh = now;
    s.selected = -1;
    for (int i = 0; i < static_cast<int>(s.list.size()) && !selected_text.empty(); ++i)
      if (s.list[i].text == selected_text && s.list[i].source == selected_source) s.selected = i;
  }
  if (changed || s.filters_changed) {
    s.shown.clear();
    for (int i = 0; i < static_cast<int>(s.list.size()); ++i)
      if (Passes(s, s.list[i])) s.shown.push_back(i);
    s.filters_changed = false;
  }
}

}  // namespace

const char* ProblemsTabLabel() {
  static char label[48];
  std::snprintf(label, sizeof(label), "Problems (%zu)###problems", ProblemLog::Get().group_count());
  return label;
}

void DrawConsolePage(std::shared_ptr<ConsoleView>& view) {
  if (!view) {
    view = std::make_shared<ConsoleView>();
    view->FocusInput();
  }
  view->Refresh();
  // The ~ console's fixed-width font, at a whole multiple of its size so it
  // stays sharp, about the size of F2's text.
  ImFont* mono = MonoFont();
  const float base = mono->LegacySize > 0 ? mono->LegacySize : 10.0f;
  const float scale = std::max(1.0f, std::floor(ImGui::GetStyle().FontSizeBase / base + 0.25f));
  ImGui::PushFont(mono, base * scale);
  // The ~ console's look: a near-black panel with a brass edge.
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.06f, 0.06f, 0.05f, 0.95f));
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.55f, 0.47f, 0.25f, 1.0f));
  const float panel_h = std::max(ImGui::GetFrameHeightWithSpacing() * 4.0f, ImGui::GetContentRegionAvail().y);
  ImGui::BeginChild("##console_panel", ImVec2(0, panel_h), ImGuiChildFlags_Borders,
                    ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
  const float input_h = ImGui::GetFrameHeightWithSpacing();
  const float output_h =
      std::max(input_h, ImGui::GetContentRegionAvail().y - input_h - ImGui::GetStyle().ItemSpacing.y * 2.0f);
  view->DrawOutput(output_h);
  ImGui::Separator();
  view->DrawInputLine(nullptr, false);
  const float panel_w = ImGui::GetWindowWidth();
  ImGui::EndChild();
  ImGui::PopStyleColor(2);
  // The completion box above the input line, while typing.
  if (view->input_active())
    view->DrawSuggestions(ImVec2(view->input_min().x, view->input_min().y - 4.0f), true, panel_w);
  ImGui::PopFont();
}

void DrawProblemsPage() {
  auto& s = PageState();
  auto& log = ProblemLog::Get();
  RefreshProblems(s);
  const Palette& palette = CurrentPalette();

  if (ImGui::Button("Copy all")) ImGui::SetClipboardText(log.AsText().c_str());
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Copies every row as text, for a bug report.");
  ImGui::SameLine();
  if (ImGui::Button("Copy selected")) {
    if (s.selected >= 0 && s.selected < static_cast<int>(s.list.size()))
      ImGui::SetClipboardText(ProblemLog::FormatLine(s.list[s.selected]).c_str());
  }
  ImGui::SameLine();
  if (ImGui::Button("Clear")) {
    log.Clear();
    s.selected = -1;
  }
  if (ImGui::IsItemHovered()) ImGui::SetTooltip("Empties the list; new problems still come in.");
  ImGui::SameLine();
  ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7.0f);
  static const char* const kSeverities[] = {"All", "Errors", "Warnings"};
  if (ImGui::Combo("##severity", &s.severity_filter, kSeverities, 3)) s.filters_changed = true;
  ImGui::SameLine();
  if (s.text_filter.Draw("Filter", std::max(80.0f, ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize("Filter").x - 12.0f)))
    s.filters_changed = true;

  const uint64_t errors = log.error_lines(), warnings = log.warning_lines(), dropped = log.dropped_lines();
  ImGui::TextColored(palette.muted,
                     "This session: %llu errors and %llu warnings from the game, the port and the SDK, in %zu "
                     "kinds, newest first. Hover a row for details, right-click to copy it.%s",
                     static_cast<unsigned long long>(errors), static_cast<unsigned long long>(warnings), s.list.size(),
                     dropped ? " Some later kinds weren't kept (the list is full)." : "");
  if (s.list.empty()) {
    ImGui::TextDisabled("No warnings or errors so far.");
    return;
  }

  const ImGuiTableFlags flags = ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_BordersInnerV |
                                ImGuiTableFlags_Resizable | ImGuiTableFlags_SizingFixedFit;
  if (!ImGui::BeginTable("##problems", 6, flags, ImVec2(0, ImGui::GetContentRegionAvail().y))) return;
  ImGui::TableSetupScrollFreeze(0, 1);
  ImGui::TableSetupColumn("Last seen");
  ImGui::TableSetupColumn("Kind");
  ImGui::TableSetupColumn("Count");
  ImGui::TableSetupColumn("From");
  ImGui::TableSetupColumn("Code");
  ImGui::TableSetupColumn("Message", ImGuiTableColumnFlags_WidthStretch);
  ImGui::TableHeadersRow();

  ImGuiListClipper clipper;
  clipper.Begin(static_cast<int>(s.shown.size()));
  while (clipper.Step()) {
    for (int row = clipper.DisplayStart; row < clipper.DisplayEnd; ++row) {
      const int index = s.shown[row];
      const Problem& p = s.list[index];
      const bool is_error = p.severity == Problem::Severity::kError;
      ImGui::TableNextRow();
      ImGui::TableNextColumn();
      ImGui::PushID(index);
      char when[16];
      ClockText(p.last_seen, when);
      if (ImGui::Selectable(when, s.selected == index,
                            ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
        s.selected = index;
      if (ImGui::IsItemHovered() && ImGui::BeginTooltip()) {
        // The details: when it started, and other lines of the same kind.
        ImGui::PushTextWrapPos(ImGui::GetFontSize() * 40.0f);
        char first[16];
        ImGui::Text("%s x%llu, first at %s, last at %s", is_error ? "Error" : "Warning",
                    static_cast<unsigned long long>(p.count), ClockText(p.first_seen, first), when);
        if (!p.code.empty()) ImGui::Text("Code: %s", p.code.c_str());
        ImGui::TextUnformatted(p.text.c_str());
        if (!p.examples.empty()) {
          ImGui::TextDisabled("Other lines of this kind:");
          for (const auto& e : p.examples) ImGui::TextDisabled("%s", e.c_str());
        }
        ImGui::TextDisabled("Right-click to copy.");
        ImGui::PopTextWrapPos();
        ImGui::EndTooltip();
      }
      if (ImGui::BeginPopupContextItem("##row_menu")) {
        s.selected = index;
        if (ImGui::MenuItem("Copy line")) ImGui::SetClipboardText(ProblemLog::FormatLine(p).c_str());
        if (ImGui::MenuItem("Copy message")) ImGui::SetClipboardText(p.text.c_str());
        if (!p.code.empty() && ImGui::MenuItem("Copy code")) ImGui::SetClipboardText(p.code.c_str());
        ImGui::EndPopup();
      }
      ImGui::TableNextColumn();
      ImGui::TextColored(is_error ? palette.bad : palette.warn, "%s", is_error ? "error" : "warning");
      ImGui::TableNextColumn();
      ImGui::Text("%llu", static_cast<unsigned long long>(p.count));
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(p.source.c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(p.code.empty() ? "" : p.code.c_str());
      ImGui::TableNextColumn();
      ImGui::TextUnformatted(p.text.c_str());
      ImGui::PopID();
    }
  }
  ImGui::EndTable();
}

}  // namespace cod3::ui
