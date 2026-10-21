// cod3 - F2 > Game > Server: the match's rules, for the host only (see
// server_tab.h).

#include "ui/server_tab.h"

#include <algorithm>
#include <string>

#include <imgui.h>

#include "game/protected_dvars.h"
#include "ui/slider_box.h"
#include "ui/theme.h"

namespace cod3::ui::server_tab {
namespace {

namespace pd = protected_dvars;

void HelpLine(const std::string& text) {
  ImGui::PushTextWrapPos(0.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextUnformatted(text.c_str());
  ImGui::PopStyleColor();
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
}

// The dvar's name, then its one-sentence help.
std::string Help(pd::Id id) {
  const pd::Entry& e = pd::Info(id);
  return std::string(e.name) + ": " + e.help;
}

bool Set(pd::Id id, int value) {
  return pd::RequestSet(id, std::to_string(value), cheats::Source::kSettings).changed;
}

// A row with a few named values, as a list.
bool ChoiceRow(pd::Id id, const char* label, const char* const* names, int count) {
  const int now = pd::Get(id);
  int v = now;
  bool changed = false;
  ImGui::PushID(pd::Info(id).name);
  if (ImGui::Combo(label, &v, names, count) && v != now) changed = Set(id, v);
  ImGui::PopID();
  HelpLine(Help(id));
  return changed;
}

// A percentage, a slider with a number box. The value is set once the drag
// or the typing ends, so a match hears of one change, not of every step.
bool PercentRow(pd::Id id, const char* label) {
  static int pending = -1;
  static pd::Id pending_id = pd::Id::kCount;
  const pd::Entry& e = pd::Info(id);
  int v = pending >= 0 && pending_id == id ? pending : pd::Get(id);
  ImGui::PushID(e.name);
  if (SliderIntBox(label, &v, e.min, e.max, "%d%%")) {
    pending = v;
    pending_id = id;
  }
  ImGui::PopID();
  bool changed = false;
  if (pending >= 0 && pending_id == id && !ImGui::IsAnyItemActive()) {
    changed = Set(id, pending);
    pending = -1;
  }
  HelpLine(Help(id));
  return changed;
}

bool ResetAll() {
  bool changed = false;
  for (const pd::Entry& e : pd::All()) changed |= pd::RequestReset(e.id, cheats::Source::kSettings).changed;
  return changed;
}

}  // namespace

bool Shown() { return !pd::IsLocked(); }

bool Draw() {
  bool changed = false;
  // Nothing here applies to a guest: the tab goes away the moment this
  // machine joins someone else's match.
  if (!Shown()) return false;

  // "Reset match rules to defaults", with a confirmation, at the right.
  {
    const char* label = "Reset match rules to defaults##reset_server";
    const float w = ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - w));
    if (ImGui::SmallButton(label)) ImGui::OpenPopup("Reset to defaults##server");
    if (ImGui::BeginPopupModal("Reset to defaults##server", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextUnformatted("Put every match rule back to its default?");
      ImGui::TextDisabled("Your own settings don't change.");
      if (ImGui::Button("Reset")) {
        changed |= ResetAll();
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
  }
  const pd::Match match = pd::CurrentMatch();
  ImGui::TextUnformatted(match.running ? "You host this match: these rules apply to every player in it."
                                       : "The rules of a match you host. In the campaign you are always the host.");
  HelpLine("Players can't change them during your match; each player's own settings come back when they leave. "
           "Players who join a match don't see this tab.");

  Section("Movement");
  static const char* kModes[] = {"0: Call of Duty 3 for everyone", "1: Call of Duty 4 for everyone",
                                 "2: Each player's own style"};
  changed |= ChoiceRow(pd::Id::kAlternateMovement, "Movement rule", kModes, 3);
  static const char* kOnOff[] = {"0: Off", "1: On"};
  // The three parts apply under 1 only; they stay editable so a host can set
  // them up before switching.
  ImGui::Indent();
  changed |= ChoiceRow(pd::Id::kAlternateMovementSprint, "CoD4 sprint", kOnOff, 2);
  changed |= ChoiceRow(pd::Id::kAlternateMovementSpeeds, "CoD4 speeds", kOnOff, 2);
  changed |= ChoiceRow(pd::Id::kAlternateMovementJumps, "CoD4 jump tricks", kOnOff, 2);
  ImGui::Unindent();

  Section("Aim assist");
  static const char* kRule[] = {"0: Off for everyone", "1: Each player's own choice"};
  changed |= ChoiceRow(pd::Id::kStickyAim, "Sticky Aim rule", kRule, 2);
  changed |= PercentRow(pd::Id::kAimSlowdown, "Sticky Aim strength");
  return changed;
}

}  // namespace cod3::ui::server_tab
