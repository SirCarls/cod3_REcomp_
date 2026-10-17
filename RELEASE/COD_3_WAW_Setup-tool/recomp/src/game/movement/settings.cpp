// cod3 - the player's movement settings, their rows, and F2's Movement
// section (see settings.h).

#include "game/movement/settings.h"

#include <algorithm>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "game/movement/program_current.h"
#include "game/protected_dvars.h"
#include "ui/theme.h"

REXCVAR_DEFINE_BOOL(cod3_sprint, true, "cod3/Movement",
                    "Sprint in the campaign (the Sprint / hold breath key, Shift by default; the pad's L3 while "
                    "pushing forward; +sprint). Off: as on the console, no sprint");
REXCVAR_DEFINE_BOOL(cod3_sprint_toggle, false, "cod3/Movement",
                    "Sprint button: false = hold to sprint, true = press to sprint (ends when you stop "
                    "moving forward, aim, fire or press again)");
REXCVAR_DEFINE_BOOL(cod3_cod4_movement, false, "cod3/Movement",
                    "Your movement style: false = Call of Duty 3 (as on the console), true = Call of Duty 4 "
                    "(with the cod3_cod4_* parts). In a match the server's alternate_movement decides");
REXCVAR_DEFINE_BOOL(cod3_cod4_sprint, true, "cod3/Movement",
                    "CoD4 style part: CoD4 sprint (4 seconds, then it recharges; aim or fire stops it; "
                    "forward only)");
REXCVAR_DEFINE_BOOL(cod3_cod4_speeds, true, "cod3/Movement",
                    "CoD4 style part: CoD4 strafe (0.8) and back (0.7) speeds, and CoD4 PC's movement at "
                    "125 fps (its air strafing and jump height) at any frame rate");
REXCVAR_DEFINE_BOOL(cod3_cod4_mantle, true, "cod3/Movement",
                    "CoD4 style part: climb onto ledges up to about chest height (jump at them)");
REXCVAR_DEFINE_BOOL(cod3_cod4_jumps, true, "cod3/Movement",
                    "CoD4 style part: CoD4 PC bounces off steep slopes (CoD4's air strafing is part of "
                    "cod3_cod4_speeds)");
REXCVAR_DEFINE_BOOL(cod3_debug_movement, false, "cod3",
                    "Log the player's movement 4 times a second (position, speed, stamina, sprint, rules)");


namespace cod3::movement {

namespace pd = protected_dvars;

namespace {

bool PartFlag(int row, Preference& pref, bool** out) {
  switch (row) {
    case kRowCod4Sprint: *out = &pref.parts.sprint; return true;
    case kRowCod4Speeds: *out = &pref.parts.speeds; return true;
    case kRowCod4Mantle: *out = &pref.parts.mantle; return true;
    case kRowCod4Jumps: *out = &pref.parts.jumps; return true;
  }
  return false;
}

const char* PartCvar(int row) {
  switch (row) {
    case kRowCod4Sprint: return "cod3_cod4_sprint";
    case kRowCod4Speeds: return "cod3_cod4_speeds";
    case kRowCod4Mantle: return "cod3_cod4_mantle";
    case kRowCod4Jumps: return "cod3_cod4_jumps";
  }
  return nullptr;
}

// In a match whose rule isn't "each player's own", the server decides the
// style and the parts (alternate_movement 0 or 1).
bool ServerDecidesStyle() {
  return pd::IsLocked() && pd::Get(pd::Id::kAlternateMovement) != static_cast<int>(MatchMode::kPlayersChoice);
}

bool SetCvar(const char* name, const char* value) {
  if (!rex::cvar::SetFlagByName(name, value)) return false;
  REXLOG_INFO("cod3: {} = {}", name, value);
  return true;
}

}  // namespace

// ---------------------------------------------------------------------------
// The settings
// ---------------------------------------------------------------------------
bool SprintSetting() { return kProgram.sprint_is_a_setting ? REXCVAR_GET(cod3_sprint) : true; }

SprintMode SprintButtonMode() { return REXCVAR_GET(cod3_sprint_toggle) ? SprintMode::kToggle : SprintMode::kHold; }

Preference OwnPreference() {
  Preference p;
  p.cod4 = REXCVAR_GET(cod3_cod4_movement);
  p.parts.sprint = REXCVAR_GET(cod3_cod4_sprint);
  p.parts.speeds = REXCVAR_GET(cod3_cod4_speeds);
  p.parts.mantle = REXCVAR_GET(cod3_cod4_mantle);
  p.parts.jumps = REXCVAR_GET(cod3_cod4_jumps);
  return p;
}

bool DebugMovement() { return REXCVAR_GET(cod3_debug_movement); }

MoveRules LocalRules() {
  Parts host;
  host.sprint = pd::Get(pd::Id::kAlternateMovementSprint) != 0;
  host.speeds = pd::Get(pd::Id::kAlternateMovementSpeeds) != 0;
  host.mantle = pd::Get(pd::Id::kAlternateMovementMantle) != 0;
  host.jumps = pd::Get(pd::Id::kAlternateMovementJumps) != 0;
  return EffectiveRules(pd::Get(pd::Id::kAlternateMovement), host, OwnPreference(), SprintSetting());
}

// ---------------------------------------------------------------------------
// The rows
// ---------------------------------------------------------------------------
const char* RowLabel(int row) {
  switch (row) {
    case kRowSprint: return "SPRINT";
    case kRowSprintButton: return "SPRINT BUTTON";
    case kRowStyle: return "MOVEMENT STYLE";
    case kRowCod4Sprint: return "COD4 SPRINT";
    case kRowCod4Speeds: return "COD4 SPEEDS";
    case kRowCod4Mantle: return "COD4 MANTLING";
    case kRowCod4Jumps: return "COD4 JUMP TRICKS";
  }
  return "";
}

const char* RowHelp(int row) {
  switch (row) {
    case kRowSprint:
      return "Sprint in the campaign: Shift (keyboard), or L3 while pushing the left stick forward. Off is the console game.";
    case kRowSprintButton:
      return "Hold to sprint, or press once: a toggle ends when you stop moving forward, aim, fire or press again.";
    case kRowStyle:
      return "COD3 moves as on the console (Call of Duty 3); COD4 uses the Call of Duty 4 parts below.";
    case kRowCod4Sprint:
      return "With the CoD4 style: 4 seconds of sprint that recharge, forward only; aiming or firing stops it.";
    case kRowCod4Speeds:
      return "With the CoD4 style: strafing at 0.8 and walking backward at 0.7 of the walk speed, and CoD4 PC's "
             "air strafing and jump height at 125 fps, whatever your frame rate.";
    case kRowCod4Mantle:
      return "With the CoD4 style: jump at a ledge up to chest height to climb onto it.";
    case kRowCod4Jumps:
      return "With the CoD4 style: bounce off steep slopes as on CoD4 PC (CoD4's air strafing comes with CoD4 "
             "speeds).";
  }
  return "";
}

std::string RowValue(int row) {
  const MoveRules effective = LocalRules();
  switch (row) {
    case kRowSprint: return SprintSetting() ? "ON" : "OFF";
    case kRowSprintButton: return SprintButtonMode() == SprintMode::kToggle ? "TOGGLE" : "HOLD";
    case kRowStyle:
      // Short: "CALL OF DUTY 3" is wider than the menu's value box (rebase run
      // 2026-10-16: it ran into the right arrow).
      if (ServerDecidesStyle()) return effective.cod4 ? "COD4" : "COD3";
      return OwnPreference().cod4 ? "COD4" : "COD3";
    default: break;
  }
  Preference pref = OwnPreference();
  if (ServerDecidesStyle()) pref = Preference{effective.cod4, effective.parts};
  bool* on = nullptr;
  if (!PartFlag(row, pref, &on)) return "";
  return *on ? "ON" : "OFF";
}

std::string RowLock(int row) {
  if (row == kRowSprint && !kProgram.sprint_is_a_setting) return "Multiplayer always has sprint.";
  if (row >= kRowStyle && ServerDecidesStyle()) return "Set by the server in this match.";
  if (row >= kRowCod4Sprint && row <= kRowCod4Jumps && !OwnPreference().cod4)
    return "Only with the Call of Duty 4 movement style.";
  return "";
}

bool StepRow(int row, bool right) {
  (void)right;  // every row has two values: either way flips it
  if (!RowLock(row).empty()) return false;
  bool ok = false;
  switch (row) {
    case kRowSprint: ok = SetCvar("cod3_sprint", SprintSetting() ? "false" : "true"); break;
    case kRowSprintButton:
      ok = SetCvar("cod3_sprint_toggle", SprintButtonMode() == SprintMode::kToggle ? "false" : "true");
      break;
    case kRowStyle: ok = SetCvar("cod3_cod4_movement", OwnPreference().cod4 ? "false" : "true"); break;
    default: {
      Preference pref = OwnPreference();
      bool* on = nullptr;
      if (PartFlag(row, pref, &on)) ok = SetCvar(PartCvar(row), *on ? "false" : "true");
      break;
    }
  }
  if (ok) pd::SaveSettings();
  return ok;
}

// ---------------------------------------------------------------------------
// F2 > Game > Movement
// ---------------------------------------------------------------------------
namespace {

void HelpLine(const char* text) {
  ImGui::PushTextWrapPos(0.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, ImGui::GetStyleColorVec4(ImGuiCol_TextDisabled));
  ImGui::TextUnformatted(text);
  ImGui::PopStyleColor();
  ImGui::PopTextWrapPos();
  ImGui::Spacing();
}

// A row with two values as a combo (the same words as the game's menu).
bool TwoValueRow(int row, const char* label, const char* off_text, const char* on_text, bool on) {
  const std::string lock = RowLock(row);
  if (!lock.empty()) ImGui::BeginDisabled();
  int value = on ? 1 : 0;
  const char* names[] = {off_text, on_text};
  bool changed = false;
  if (ImGui::Combo(label, &value, names, 2) && (value == 1) != on) changed = StepRow(row, true);
  if (!lock.empty()) ImGui::EndDisabled();
  HelpLine(RowHelp(row));
  if (!lock.empty()) ImGui::TextDisabled("%s", lock.c_str());
  return changed;
}

bool ProtectedRow(pd::Id id, const char* label, const char* const* names, int count) {
  const pd::Entry& e = pd::Info(id);
  const bool locked = pd::IsLocked();
  int value = pd::Get(id);
  bool changed = false;
  if (locked) ImGui::BeginDisabled();
  if (ImGui::Combo(label, &value, names, count)) {
    const pd::Verdict v = pd::RequestSet(id, std::to_string(value), cheats::Source::kSettings);
    changed = v.changed;
  }
  if (locked) ImGui::EndDisabled();
  HelpLine(e.help);
  if (locked) ImGui::TextDisabled("%s", pd::LockedReason(id).c_str());
  return changed;
}

}  // namespace

// Puts the movement settings back to their defaults (the match rule only
// when no match's server sets it).
static bool ResetMovementSettings() {
  bool changed = false;
  for (const char* name : {"cod3_sprint", "cod3_sprint_toggle", "cod3_cod4_movement", "cod3_cod4_sprint",
                           "cod3_cod4_speeds", "cod3_cod4_mantle", "cod3_cod4_jumps"}) {
    const auto* info = rex::cvar::GetFlagInfo(name);
    if (!info || rex::cvar::GetFlagByName(name) == info->default_value) continue;
    changed |= rex::cvar::SetFlagByName(name, info->default_value);
  }
  for (const pd::Entry& e : pd::All()) changed |= pd::RequestReset(e.id, cheats::Source::kSettings).changed;
  if (changed) REXLOG_INFO("cod3: movement settings back to their defaults (F2)");
  return changed;
}

bool DrawF2Section() {
  bool changed = false;
  ui::Section("Movement");
  {
    // "Reset movement settings to defaults", with a confirmation, at the right.
    const char* label = "Reset movement settings to defaults##reset_movement";
    const float w = ImGui::CalcTextSize(label, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - w));
    if (ImGui::SmallButton(label)) ImGui::OpenPopup("Reset to defaults##movement");
    if (ImGui::BeginPopupModal("Reset to defaults##movement", nullptr, ImGuiWindowFlags_AlwaysAutoResize)) {
      ImGui::TextUnformatted("Put the movement settings back to their defaults?");
      ImGui::TextDisabled("Nothing else changes.");
      if (ImGui::Button("Reset")) {
        changed |= ResetMovementSettings();
        ImGui::CloseCurrentPopup();
      }
      ImGui::SameLine();
      if (ImGui::Button("Cancel")) ImGui::CloseCurrentPopup();
      ImGui::EndPopup();
    }
  }
  const MoveRules effective = LocalRules();
  ImGui::Text("Now: %s", Describe(effective).c_str());
  ImGui::Spacing();

  if (kProgram.sprint_is_a_setting)
    changed |= TwoValueRow(kRowSprint, "Sprint", "Off", "On", SprintSetting());
  changed |= TwoValueRow(kRowSprintButton, "Sprint button", "Hold", "Toggle",
                         SprintButtonMode() == SprintMode::kToggle);
  const bool show_server = ServerDecidesStyle();
  changed |= TwoValueRow(kRowStyle, "Movement style", "CoD3 (as on the console)", "CoD4",
                         show_server ? effective.cod4 : OwnPreference().cod4);
  ImGui::Indent();
  const Preference own = OwnPreference();
  const Parts shown = show_server ? effective.parts : own.parts;
  changed |= TwoValueRow(kRowCod4Sprint, "CoD4 sprint", "Off", "On", shown.sprint);
  changed |= TwoValueRow(kRowCod4Speeds, "CoD4 speeds", "Off", "On", shown.speeds);
  changed |= TwoValueRow(kRowCod4Mantle, "CoD4 mantling", "Off", "On", shown.mantle);
  changed |= TwoValueRow(kRowCod4Jumps, "CoD4 jump tricks", "Off", "On", shown.jumps);
  ImGui::Unindent();

  if (ImGui::CollapsingHeader("Match rule (advanced, server_sided_protected)")) {
    HelpLine("Set by a match's host and sent to every player; nobody else can change them during the match, "
             "and your own values come back when you leave. In the campaign you are the host: 2 (each "
             "player's own style) makes the Movement style row above apply.");
    static const char* kModes[] = {"0: Call of Duty 3 for everyone", "1: Call of Duty 4 for everyone",
                                   "2: Each player's own style"};
    changed |= ProtectedRow(pd::Id::kAlternateMovement, "alternate_movement", kModes, 3);
    static const char* kOnOff[] = {"0: Off", "1: On"};
    changed |= ProtectedRow(pd::Id::kAlternateMovementSprint, "alternate_movement_sprint", kOnOff, 2);
    changed |= ProtectedRow(pd::Id::kAlternateMovementSpeeds, "alternate_movement_speeds", kOnOff, 2);
    changed |= ProtectedRow(pd::Id::kAlternateMovementMantle, "alternate_movement_mantle", kOnOff, 2);
    changed |= ProtectedRow(pd::Id::kAlternateMovementJumps, "alternate_movement_jumps", kOnOff, 2);
  }
  return changed;
}

}  // namespace cod3::movement
