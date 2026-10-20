// cod3 - the console's view, shared by the ~ console and F2. See console_view.h.

#include "ui/console_view.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cmath>

#include <rex/cvar.h>

#include "game/addresses.h"
#include "ui/dvar_domain.h"
#include "ui/dvar_help.h"
#include "ui/waw_dvars.h"

namespace cod3::ui {

namespace {

constexpr const char* kPrompt = "CoD3 >";
constexpr int kMaxInlineSuggestions = 24;

bool StartsWithNoCase(std::string_view s, std::string_view prefix) {
  if (prefix.size() > s.size()) return false;
  for (size_t i = 0; i < prefix.size(); ++i) {
    if (std::tolower(static_cast<unsigned char>(s[i])) != std::tolower(static_cast<unsigned char>(prefix[i])))
      return false;
  }
  return true;
}

bool EqualsNoCase(std::string_view a, std::string_view b) { return a.size() == b.size() && StartsWithNoCase(a, b); }

bool LessNoCase(std::string_view a, std::string_view b) {
  return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(), [](char x, char y) {
    return std::tolower(static_cast<unsigned char>(x)) < std::tolower(static_cast<unsigned char>(y));
  });
}

ImVec4 Q3Color(char code) {
  switch (code) {
    case '0': return ImVec4(0.35f, 0.35f, 0.35f, 1.0f);
    case '1': return ImVec4(1.00f, 0.32f, 0.32f, 1.0f);
    case '2': return ImVec4(0.40f, 1.00f, 0.40f, 1.0f);
    case '3': return ImVec4(1.00f, 0.92f, 0.36f, 1.0f);
    case '4': return ImVec4(0.42f, 0.52f, 1.00f, 1.0f);
    case '5': return ImVec4(0.38f, 0.95f, 1.00f, 1.0f);
    case '6': return ImVec4(1.00f, 0.45f, 1.00f, 1.0f);
    case '8': return ImVec4(1.00f, 0.62f, 0.20f, 1.0f);
    case '9': return ImVec4(0.60f, 0.60f, 0.60f, 1.0f);
    default: return ImVec4(0.92f, 0.92f, 0.92f, 1.0f);
  }
}

double NowSeconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

// The text without ^N color codes, for the clipboard.
std::string WithoutColors(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '^' && i + 1 < text.size() && std::isdigit(static_cast<unsigned char>(text[i + 1]))) {
      ++i;
      continue;
    }
    out += text[i];
  }
  return out;
}

}  // namespace

std::string ConsoleFlagString(uint32_t f) {
  namespace cf = cod3::addr::cvar_flag;
  std::string s;
  s += (f & cf::kServerInfo) ? 'S' : ' ';
  s += (f & cf::kUserInfo) ? 'U' : ' ';
  s += (f & cf::kRom) ? 'R' : ' ';
  s += (f & cf::kInit) ? 'I' : ' ';
  s += (f & cf::kArchive) ? 'A' : ' ';
  s += (f & cf::kLatch) ? 'L' : ' ';
  s += (f & cf::kCheat) ? 'C' : ' ';
  s += (f & cf::kTemp) ? 'T' : ' ';
  return s;
}

ImVec4 ConsoleChannelColor(ConsoleChannel ch) {
  switch (ch) {
    case ConsoleChannel::kDeveloper: return ImVec4(0.62f, 0.62f, 0.62f, 1.0f);
    case ConsoleChannel::kError: return ImVec4(1.00f, 0.36f, 0.30f, 1.0f);
    case ConsoleChannel::kEcho: return ImVec4(0.75f, 0.75f, 0.75f, 1.0f);
    case ConsoleChannel::kSystem: return ImVec4(1.00f, 0.80f, 0.35f, 1.0f);
    default: return ImVec4(0.92f, 0.92f, 0.92f, 1.0f);
  }
}

void DrawConsoleText(std::string_view text, ImVec4 base_color) {
  ImVec4 color = base_color;
  size_t start = 0;
  bool first = true;
  auto flush = [&](size_t end) {
    if (end <= start) return;
    if (!first) ImGui::SameLine(0.0f, 0.0f);
    ImGui::PushStyleColor(ImGuiCol_Text, color);
    ImGui::TextUnformatted(text.data() + start, text.data() + end);
    ImGui::PopStyleColor();
    first = false;
  };
  for (size_t i = 0; i + 1 < text.size(); ++i) {
    if (text[i] == '^' && text[i + 1] >= '0' && text[i + 1] <= '9') {
      flush(i);
      color = text[i + 1] == '7' ? base_color : Q3Color(text[i + 1]);
      start = i + 2;
      ++i;
    }
  }
  flush(text.size());
  if (first) ImGui::TextUnformatted("");
}

// ===========================================================================
// ConsoleView
// ===========================================================================
ConsoleView::ConsoleView() {
  Engine::Get().RequestSnapshot();
  // The port's own settings complete too (they are set by name: "cod3_render_percent 75").
  for (auto& name : rex::cvar::ListFlags()) {
    if (name.rfind("cod3_", 0) != 0) continue;
    const auto* info = rex::cvar::GetFlagInfo(name);
    if (info && info->type != rex::cvar::FlagType::Command) port_settings_.push_back(name);
  }
}

void ConsoleView::Refresh() {
  // Keep the completion data fresh while shown (twice a second).
  const double now = NowSeconds();
  auto& engine = Engine::Get();
  if (now - last_snapshot_request_ > 0.5) {
    engine.RequestSnapshot();
    last_snapshot_request_ = now;
  }
  if (engine.SnapshotGeneration() != snapshot_gen_) {
    snapshot_ = engine.Snapshot();
    snapshot_gen_ = snapshot_.generation;
    if (suggestion_sel_ < 0) UpdateSuggestions(typed_prefix_);
  }
}

void ConsoleView::UpdateSuggestions(std::string_view text) {
  suggestions_.clear();
  suggestion_sel_ = -1;
  single_domain_.clear();
  single_help_.clear();
  const size_t start = text.find_first_not_of(" /\\");
  if (start == std::string_view::npos) return;
  text.remove_prefix(start);
  // Only the command / dvar name (first word) is completed.
  const size_t space = text.find(' ');
  const std::string_view word = text.substr(0, space);
  if (word.empty()) return;
  const bool exact_only = space != std::string_view::npos;
  auto matches = [&](std::string_view name) {
    return exact_only ? EqualsNoCase(name, word) : StartsWithNoCase(name, word);
  };
  using Type = Suggestion::Type;

  for (const auto& v : snapshot_.cvars) {
    if (!matches(v.name)) continue;
    Suggestion s;
    s.type = Type::kGameDvar;
    s.name = v.name;
    s.value = v.value;
    s.reset = v.reset;
    s.flags = v.flags;
    suggestions_.push_back(std::move(s));
  }
  for (const auto& c : snapshot_.commands) {
    if (!matches(c)) continue;
    Suggestion s;
    s.name = c;
    suggestions_.push_back(std::move(s));
  }
  for (const char* c : GameConsole::LocalCommands()) {
    if (!matches(c)) continue;
    Suggestion s;
    s.name = c;
    suggestions_.push_back(std::move(s));
  }
  for (const auto& w : waw::All()) {
    if (!matches(w.name)) continue;
    Suggestion s;
    s.type = Type::kWaw;
    s.name = w.name;
    s.value = waw::Value(w, snapshot_);
    s.reset = waw::DefaultValue(w, snapshot_);
    s.waw = &w;
    suggestions_.push_back(std::move(s));
  }
  for (const auto& name : port_settings_) {
    if (!matches(name)) continue;
    const auto* info = rex::cvar::GetFlagInfo(name);
    if (!info) continue;
    Suggestion s;
    s.type = Type::kPortSetting;
    s.name = name;
    s.value = domain::PortValueText(*info, rex::cvar::GetFlagByName(name));
    s.reset = domain::PortValueText(*info, info->default_value);
    suggestions_.push_back(std::move(s));
  }
  std::stable_sort(suggestions_.begin(), suggestions_.end(),
                   [](const Suggestion& a, const Suggestion& b) { return LessNoCase(a.name, b.name); });
  // A name known twice shows once, the game's first (the stable sort keeps
  // the order they were added in).
  suggestions_.erase(std::unique(suggestions_.begin(), suggestions_.end(),
                                 [](const Suggestion& a, const Suggestion& b) { return EqualsNoCase(a.name, b.name); }),
                     suggestions_.end());

  // The list shows each match's domain; a long list isn't shown at all.
  if (static_cast<int>(suggestions_.size()) > kMaxInlineSuggestions) return;
  for (auto& s : suggestions_) {
    switch (s.type) {
      case Type::kGameDvar: s.domain = domain::Brief(domain::ForGameDvar(s.name, s.reset)); break;
      case Type::kWaw: s.domain = domain::Brief(domain::ForWaw(*s.waw, snapshot_)); break;
      case Type::kPortSetting:
        if (const auto* info = rex::cvar::GetFlagInfo(s.name)) s.domain = domain::Brief(domain::ForPortSetting(*info));
        break;
      case Type::kCommand: break;
    }
  }
  if (suggestions_.size() == 1) DescribeSingleMatch();
}

void ConsoleView::DescribeSingleMatch() {
  const Suggestion& s = suggestions_[0];
  using Type = Suggestion::Type;
  domain::Domain d;
  switch (s.type) {
    case Type::kGameDvar:
      d = domain::ForGameDvar(s.name, s.reset);
      single_flags_ = ConsoleFlagString(s.flags);
      if (const dvar_help::Entry* e = dvar_help::Find(s.name)) single_help_ = e->text;
      break;
    case Type::kWaw:
      d = domain::ForWaw(*s.waw, snapshot_);
      single_help_ = std::string(s.waw->help) +
                     (s.waw->kind == waw::Kind::kGame ? " (Call of Duty 3's " : " (the port's ") + s.waw->target + ")";
      break;
    case Type::kPortSetting:
      if (const auto* info = rex::cvar::GetFlagInfo(s.name)) {
        d = domain::ForPortSetting(*info);
        single_help_ = info->description;
      }
      break;
    case Type::kCommand: return;
  }
  single_domain_ = domain::Describe(d);
}

int ConsoleView::InputCallback(ImGuiInputTextCallbackData* data) {
  auto* self = static_cast<ConsoleView*>(data->UserData);
  switch (data->EventFlag) {
    case ImGuiInputTextFlags_CallbackAlways: {
      std::string_view text(data->Buf, static_cast<size_t>(data->BufTextLen));
      // Completions update typed_prefix_ themselves, so a mismatch here means
      // the user edited the line.
      if (text != self->typed_prefix_) {
        self->typed_prefix_ = std::string(text);
        self->UpdateSuggestions(text);
      }
      break;
    }
    case ImGuiInputTextFlags_CallbackCompletion: {
      auto& sug = self->suggestions_;
      if (sug.empty()) break;
      std::string replacement;
      if (sug.size() == 1) {
        replacement = sug[0].name + " ";
      } else {
        // Longest common prefix first; once it stops growing, cycle matches.
        std::string lcp = sug[0].name;
        for (auto& s : sug) {
          size_t j = 0;
          while (j < lcp.size() && j < s.name.size() &&
                 std::tolower(static_cast<unsigned char>(lcp[j])) == std::tolower(static_cast<unsigned char>(s.name[j])))
            ++j;
          lcp.resize(j);
        }
        std::string current(data->Buf, static_cast<size_t>(data->BufTextLen));
        size_t lead = current.find_first_not_of(" /\\");
        std::string word = lead == std::string::npos ? "" : current.substr(lead);
        if (lcp.size() > word.size()) {
          replacement = lcp;
        } else {
          self->suggestion_sel_ = (self->suggestion_sel_ + 1) % static_cast<int>(sug.size());
          replacement = sug[self->suggestion_sel_].name;
        }
      }
      data->DeleteChars(0, data->BufTextLen);
      data->InsertChars(0, replacement.c_str());
      if (sug.size() == 1 || self->suggestion_sel_ < 0) {
        self->typed_prefix_ = replacement;
        self->UpdateSuggestions(replacement);
      } else {
        self->typed_prefix_ = replacement;
      }
      break;
    }
    case ImGuiInputTextFlags_CallbackHistory: {
      auto& hist = GameConsole::Get().history();
      if (hist.empty()) break;
      const int prev = self->history_pos_;
      if (data->EventKey == ImGuiKey_UpArrow) {
        self->history_pos_ =
            self->history_pos_ < 0 ? static_cast<int>(hist.size()) - 1 : std::max(0, self->history_pos_ - 1);
      } else if (data->EventKey == ImGuiKey_DownArrow) {
        if (self->history_pos_ >= 0 && ++self->history_pos_ >= static_cast<int>(hist.size())) self->history_pos_ = -1;
      }
      if (prev != self->history_pos_) {
        const std::string h = self->history_pos_ >= 0 ? hist[self->history_pos_] : std::string();
        data->DeleteChars(0, data->BufTextLen);
        data->InsertChars(0, h.c_str());
        self->typed_prefix_ = h;
        self->UpdateSuggestions(h);
      }
      break;
    }
    default:
      break;
  }
  return 0;
}

void ConsoleView::DrawOutput(float height) {
  auto& model = GameConsole::Get();
  if (model.generation() != lines_gen_) {
    lines_gen_ = model.generation();
    model.CopyLines(lines_);
    scroll_to_bottom_ = true;
  }
  ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
  ImGui::BeginChild("##cod3_output", ImVec2(0, height), false,
                    ImGuiWindowFlags_HorizontalScrollbar | ImGuiWindowFlags_NoNav);
  ImGuiListClipper clipper;
  clipper.Begin(static_cast<int>(lines_.size()));
  while (clipper.Step()) {
    for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i)
      DrawConsoleText(lines_[i].text, ConsoleChannelColor(lines_[i].channel));
  }
  if (ImGui::IsKeyPressed(ImGuiKey_PageUp)) ImGui::SetScrollY(ImGui::GetScrollY() - height * 0.8f);
  if (ImGui::IsKeyPressed(ImGuiKey_PageDown)) ImGui::SetScrollY(ImGui::GetScrollY() + height * 0.8f);
  if (scroll_to_bottom_) {
    ImGui::SetScrollHereY(1.0f);
    scroll_to_bottom_ = false;
  }
  // Right-click: copy the output (without color codes) or clear it.
  if (ImGui::BeginPopupContextWindow("##cod3_output_menu")) {
    if (ImGui::MenuItem("Copy all")) {
      std::string all;
      for (const auto& l : lines_) all += WithoutColors(l.text) + "\n";
      ImGui::SetClipboardText(all.c_str());
    }
    if (ImGui::MenuItem("Clear", "Ctrl+L")) model.Clear();
    ImGui::EndPopup();
  }
  ImGui::EndChild();
  ImGui::PopStyleColor();
}

void ConsoleView::DrawInputLine(const char* trailing, bool keep_focus) {
  ImGuiIO& io = ImGui::GetIO();
  ImGui::AlignTextToFramePadding();
  ImGui::TextColored(ImVec4(1.0f, 0.82f, 0.30f, 1.0f), "%s", kPrompt);
  ImGui::SameLine();
  const float trailing_w = trailing ? ImGui::CalcTextSize(trailing).x + 16.0f : 0.0f;
  ImGui::SetNextItemWidth(std::max(100.0f, ImGui::GetContentRegionAvail().x - trailing_w));
  if (focus_input_) {
    ImGui::SetKeyboardFocusHere();
    focus_input_ = false;
  }
  const ImGuiInputTextFlags flags = ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_CallbackAlways |
                                    ImGuiInputTextFlags_CallbackCompletion | ImGuiInputTextFlags_CallbackHistory;
  ImGui::PushStyleColor(ImGuiCol_FrameBg, ImVec4(0, 0, 0, 0));
  const bool submitted =
      ImGui::InputText("##cod3_input", input_, sizeof(input_), flags, &ConsoleView::InputCallback, this);
  ImGui::PopStyleColor();
  input_active_ = ImGui::IsItemActive();
  input_min_ = ImGui::GetItemRectMin();
  input_max_ = ImGui::GetItemRectMax();
  if (trailing) {
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.55f, 0.55f, 0.50f, 1.0f), "%s", trailing);
  }

  if (submitted) {
    GameConsole::Get().Submit(input_);
    input_[0] = '\0';
    typed_prefix_.clear();
    suggestions_.clear();
    single_domain_.clear();
    single_help_.clear();
    suggestion_sel_ = -1;
    history_pos_ = -1;
    scroll_to_bottom_ = true;
    ImGui::SetKeyboardFocusHere(-1);
    input_active_ = true;
  }
  if ((keep_focus || input_active_) && io.KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_L)) GameConsole::Get().Clear();
  // The drop-down console keeps the keyboard in its input line.
  if (keep_focus && !ImGui::IsAnyItemActive() && !ImGui::IsAnyItemHovered()) focus_input_ = true;
}

void ConsoleView::DrawSingleMatch(ImVec4 name_col, ImVec4 value_col, ImVec4 dim_col) {
  // World at War's box for one dvar: its value, default and domain.
  const Suggestion& s = suggestions_[0];
  ImGui::TextColored(name_col, "%s", s.name.c_str());
  ImGui::SameLine(0, 24);
  DrawConsoleText(s.value, value_col);
  ImGui::TextColored(dim_col, "  default");
  ImGui::SameLine(0, 12);
  DrawConsoleText(s.reset, dim_col);
  const ImVec4 domain_col(0.85f, 0.80f, 0.62f, 1.0f);
  for (const auto& line : single_domain_) ImGui::TextColored(domain_col, "  %s", line.c_str());
  if (s.type == Suggestion::Type::kGameDvar) {
    ImGui::TextColored(dim_col,
                       "  flags: [%s]  (S=serverinfo U=userinfo R=read-only I=init A=archived L=latched C=cheat "
                       "T=temp)",
                       single_flags_.c_str());
  }
  if (!single_help_.empty()) {
    ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + 640.0f);
    ImGui::TextColored(dim_col, "  %s", single_help_.c_str());
    ImGui::PopTextWrapPos();
  }
}

void ConsoleView::DrawSuggestions(ImVec2 pos, bool above, float width) {
  if (suggestions_.empty()) return;
  ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(0.0f, above ? 1.0f : 0.0f));
  ImGui::SetNextWindowSizeConstraints(ImVec2(width * 0.5f, 0), ImVec2(width, 600));
  ImGui::SetNextWindowBgAlpha(0.92f);
  ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0.55f, 0.47f, 0.25f, 1.0f));
  ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0.06f, 0.06f, 0.05f, 1.0f));
  // A tooltip window: drawn over every other window (F2 too), takes no input.
  if (ImGui::BeginTooltip()) {
    const ImVec4 name_col(1.00f, 1.00f, 1.00f, 1.0f);
    const ImVec4 value_col(1.00f, 0.92f, 0.36f, 1.0f);
    const ImVec4 dim_col(0.60f, 0.60f, 0.60f, 1.0f);
    if (suggestions_.size() == 1 && suggestions_[0].type != Suggestion::Type::kCommand) {
      DrawSingleMatch(name_col, value_col, dim_col);
    } else if (static_cast<int>(suggestions_.size()) > kMaxInlineSuggestions) {
      ImGui::TextColored(dim_col, "%d matches (too many to show here, keep typing; cvarlist <text> lists them)",
                         static_cast<int>(suggestions_.size()));
    } else {
      float name_w = 0, value_w = 0;
      for (auto& s : suggestions_) {
        name_w = std::max(name_w, ImGui::CalcTextSize(s.name.c_str()).x);
        value_w = std::max(value_w, ImGui::CalcTextSize(s.value.c_str()).x);
      }
      value_w = std::min(value_w, 220.0f);
      const float origin = ImGui::GetCursorPosX();
      for (int i = 0; i < static_cast<int>(suggestions_.size()); ++i) {
        auto& s = suggestions_[i];
        if (i == suggestion_sel_) {
          ImVec2 p = ImGui::GetCursorScreenPos();
          ImGui::GetWindowDrawList()->AddRectFilled(p, ImVec2(p.x + width - 24, p.y + ImGui::GetTextLineHeight()),
                                                    ImGui::GetColorU32(ImVec4(0.45f, 0.38f, 0.16f, 0.9f)));
        }
        const bool has_value = s.type != Suggestion::Type::kCommand;
        ImGui::TextColored(has_value ? name_col : ImVec4(0.55f, 0.85f, 1.0f, 1.0f), "%s", s.name.c_str());
        if (has_value) {
          ImGui::SameLine(origin + name_w + 32);
          DrawConsoleText(s.value, value_col);
          if (!s.domain.empty()) {
            ImGui::SameLine(origin + name_w + 32 + value_w + 24);
            ImGui::TextColored(dim_col, "%s", s.domain.c_str());
          }
        }
      }
      // The highlighted dvar's help line under the list (ui/dvar_help.h).
      if (suggestion_sel_ >= 0 && suggestion_sel_ < static_cast<int>(suggestions_.size()) &&
          suggestions_[suggestion_sel_].type == Suggestion::Type::kGameDvar) {
        if (const dvar_help::Entry* e = dvar_help::Find(suggestions_[suggestion_sel_].name)) {
          ImGui::Separator();
          ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width - 32.0f);
          ImGui::TextColored(dim_col, "%s", e->text);
          ImGui::PopTextWrapPos();
        }
      }
    }
    ImGui::EndTooltip();
  }
  ImGui::PopStyleColor(2);
}

}  // namespace cod3::ui
