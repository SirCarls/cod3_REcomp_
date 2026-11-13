// f2 - the rows of the F2 settings window (see f2_rows.h). [f2-menus]
//
// Written after Call of Duty 3's CvarCombo, CvarCheckbox, Help, ResetButton and ResetSettings
// (recomp/src/ui/hud_overlay.cpp), so a row behaves the same in every game.

#include "f2/f2_rows.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>

#include "ui/slider_box.h"  // Call of Duty 3's slider with a number box (header only, Dear ImGui alone)

namespace f2 {

namespace {

// True when a change to `name` only takes effect at the next start: the row says so, or the setting is registered
// as needing a restart.
bool NeedsRestart(const char* name, Applies applies) {
  if (applies == Applies::kAfterRestart) return true;
  const auto* info = rex::cvar::GetFlagInfo(name);
  return info && info->lifecycle != rex::cvar::Lifecycle::kHotReload;
}

void Changed(const char* name, Applies applies, bool* restart) {
  if (restart && NeedsRestart(name, applies)) *restart = true;
}

// A row for a setting this build doesn't have: its label, greyed out, so the page keeps its shape.
void Missing(const char* label) {
  ImGui::BeginDisabled();
  ImGui::TextUnformatted(label);
  ImGui::SameLine();
  ImGui::TextDisabled("(not in this build)");
  ImGui::EndDisabled();
}

}  // namespace

bool Has(const char* name) { return rex::cvar::GetFlagInfo(name) != nullptr; }

std::string Get(const char* name) { return rex::cvar::GetFlagByName(name); }

bool GetBool(const char* name) {
  const std::string v = Get(name);
  return v == "true" || v == "1";
}

int GetInt(const char* name, int fallback) {
  const std::string v = Get(name);
  return v.empty() ? fallback : std::atoi(v.c_str());
}

double GetDouble(const char* name, double fallback) {
  const std::string v = Get(name);
  return v.empty() ? fallback : std::strtod(v.c_str(), nullptr);
}

bool Check(const char* label, const char* name, Applies applies, bool* restart) {
  if (!Has(name)) {
    Missing(label);
    return false;
  }
  bool v = GetBool(name);
  if (!ImGui::Checkbox(label, &v)) return false;
  rex::cvar::SetFlagByName(name, v ? "true" : "false");
  Changed(name, applies, restart);
  return true;
}

bool Choice(const char* label, const char* name, const char* const* values, const char* const* names, int count,
            Applies applies, bool* restart) {
  if (!Has(name)) {
    Missing(label);
    return false;
  }
  // Only the values this build accepts (a setting with a list of allowed values).
  const auto* info = rex::cvar::GetFlagInfo(name);
  std::vector<int> shown;
  for (int i = 0; i < count; ++i) {
    if (info && info->constraints.HasAllowedValues()) {
      const auto& allowed = info->constraints.allowed_values;
      if (std::find(allowed.begin(), allowed.end(), values[i]) == allowed.end()) continue;
    }
    shown.push_back(i);
  }
  if (shown.empty()) return false;

  const std::string current = Get(name);
  int index = -1;
  for (size_t j = 0; j < shown.size(); ++j)
    if (current == values[shown[j]]) index = static_cast<int>(j);
  // A value set elsewhere (the console, the setup tool, the command line) that isn't one of the choices is shown
  // as it is instead of as the first choice.
  const std::string other = "Other: " + current;
  const char* preview = index >= 0 ? names[shown[index]] : other.c_str();
  bool changed = false;
  if (ImGui::BeginCombo(label, preview)) {
    for (size_t j = 0; j < shown.size(); ++j) {
      const bool selected = static_cast<int>(j) == index;
      if (ImGui::Selectable(names[shown[j]], selected) && !selected) {
        rex::cvar::SetFlagByName(name, values[shown[j]]);
        Changed(name, applies, restart);
        changed = true;
      }
      if (selected) ImGui::SetItemDefaultFocus();
    }
    ImGui::EndCombo();
  }
  return changed;
}

bool SliderInt(const char* label, const char* name, int min, int max, const char* format, Applies applies,
               bool* restart) {
  if (!Has(name)) {
    Missing(label);
    return false;
  }
  int v = std::clamp(GetInt(name, min), min, max);
  if (!cod3::ui::SliderIntBox(label, &v, min, max, format)) return false;
  rex::cvar::SetFlagByName(name, std::to_string(v));
  Changed(name, applies, restart);
  return true;
}

bool SliderDouble(const char* label, const char* name, double min, double max, const char* format, Applies applies,
                  bool* restart) {
  if (!Has(name)) {
    Missing(label);
    return false;
  }
  float v = static_cast<float>(std::clamp(GetDouble(name, min), min, max));
  if (!cod3::ui::SliderFloatBox(label, &v, static_cast<float>(min), static_cast<float>(max), format)) return false;
  char text[32];
  std::snprintf(text, sizeof(text), "%.4f", static_cast<double>(v));
  rex::cvar::SetFlagByName(name, text);
  Changed(name, applies, restart);
  return true;
}

bool TextRow(const char* label, const char* name, bool* restart) {
  if (!Has(name)) {
    Missing(label);
    return false;
  }
  // Each row keeps its own buffer while it is being typed into; otherwise it shows the setting's value.
  ImGui::PushID(name);
  char buffer[128];
  const std::string current = Get(name);
  std::snprintf(buffer, sizeof(buffer), "%s", current.c_str());
  ImGui::InputText(label, buffer, sizeof(buffer));
  bool changed = false;
  if (ImGui::IsItemDeactivatedAfterEdit() && current != buffer) {
    rex::cvar::SetFlagByName(name, buffer);
    Changed(name, Applies::kAtOnce, restart);
    changed = true;
  }
  ImGui::PopID();
  return changed;
}

void Help(const char* text) {
  ImGui::PushTextWrapPos(0.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextUnformatted(text);
  ImGui::PopStyleColor();
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
}

bool ResetButton(const char* id, const char* what) {
  char label[128], title[96];
  std::snprintf(label, sizeof(label), "Reset %s to defaults##%s", what, id);
  std::snprintf(title, sizeof(title), "Reset to defaults##%s", id);
  const float w = ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
  ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - w));
  if (ImGui::SmallButton(label)) ImGui::OpenPopup(title);
  bool confirmed = false;
  if (ImGui::BeginPopupModal(title, nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
    ImGui::Text("Put the %s back to their defaults?", what);
    ImGui::TextDisabled("Nothing else changes.");
    if (ImGui::Button("Reset")) {
      confirmed = true;
      ImGui::CloseCurrentPopup();
    }
    ImGui::SameLine();
    if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
    ImGui::EndPopup();
  }
  return confirmed;
}

bool ResetSettings(std::initializer_list<const char*> names) {
  bool restart = false;
  for (const char* name : names) {
    const auto* info = rex::cvar::GetFlagInfo(name);
    if (!info) continue;
    const std::string def = info->default_value;  // copied: the entry can move when the registry grows
    const bool hot = info->lifecycle == rex::cvar::Lifecycle::kHotReload;
    const std::string before = Get(name);
    if (before != def) rex::cvar::SetFlagByName(name, def);
    if (Get(name) != before && !hot) restart = true;
  }
  return restart;
}

}  // namespace f2
