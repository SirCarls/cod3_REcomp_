// [acct2-bro] 2026-11-22 cod3 - F2 > Game > Camos (Camo lane; what and why: camo_tab.h, game/ngl_camo.h).
//
// One player at a time (split-screen players 1-4 in multiplayer, player 1 in the campaign): an "All guns" camo, then
// a row for every gun that player's screen has drawn in first person this session (named as the game's own gun
// textures name them: springfield, m1garand; c519), each "Same as all guns" or its own camo. A camo list of 650
// is long, so each choice opens a list with a filter box. Nothing here touches the game: the rows write the
// settings, which are saved with F2's others.

#include "ui/camo_tab.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>

#include "game/ngl_camo.h"

namespace cod3::ui::camo_tab {
namespace {

namespace camo = cod3::ngl_camo;

int g_player = 1;
char g_filter[64] = "";

std::string Lower(std::string s) {
  for (char& c : s) c = char(std::tolower(uint8_t(c)));
  return s;
}

// The name shown for a camo id ("" = none, "=" = same as all guns).
std::string Shown(const std::string& id, const char* none) {
  if (id.empty()) return none;
  for (const camo::Entry& e : camo::List())
    if (e.id == id) return e.name;
  return id + " (not in the imported list)";
}

// A camo picker: a combo whose list has a filter box. Returns true when the choice changed.
bool Picker(const char* label, int player, const std::string& weapon, const char* none) {
  const std::string now = camo::Choice(player, weapon);
  bool changed = false;
  ImGui::PushID(label);
  if (ImGui::BeginCombo(label, Shown(now, none).c_str(), ImGuiComboFlags_HeightLarge)) {
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputTextWithHint("##filter", "Filter (part of a pack or camo name)", g_filter, sizeof g_filter);
    const std::string want = Lower(g_filter);
    if (ImGui::Selectable(none, now.empty())) {
      camo::SetChoice(player, weapon, "");
      changed = true;
    }
    int shown = 0;
    for (const camo::Entry& e : camo::List()) {
      if (!want.empty() && Lower(e.name).find(want) == std::string::npos) continue;
      if (++shown > 400) {  // a long list stays quick; the filter narrows it
        ImGui::TextDisabled("... more: type in the filter");
        break;
      }
      ImGui::PushID(e.id.c_str());
      if (ImGui::Selectable(e.name.c_str(), e.id == now)) {
        camo::SetChoice(player, weapon, e.id);
        changed = true;
      }
      ImGui::PopID();
    }
    ImGui::EndCombo();
  }
  ImGui::PopID();
  return changed;
}

}  // namespace

bool Draw() {
  bool dirty = false;
  const std::string on_text = rex::cvar::GetFlagByName("cod3_camo");
  bool on = on_text == "true" || on_text == "1";
  if (ImGui::Checkbox("Camos on the guns", &on)) {
    rex::cvar::SetFlagByName("cod3_camo", on ? "true" : "false");
    dirty = true;
  }
  ImGui::TextWrapped("Black Ops style camos from your own camo pack, on your first-person guns, with the native "
                     "renderer. Only you see them: nothing is sent to other players and the game itself is unchanged. "
                     "A change shows at the next frame, also in the middle of a match.");
  const auto& list = camo::List();
  if (list.empty()) {
    ImGui::TextDisabled("No camos imported yet: the setup tool imports a pack into data/cod3/camos.");
    if (ImGui::Button("Look again")) camo::Rescan();
    return dirty;
  }
  ImGui::Text("%d camos imported.", int(list.size()));
  ImGui::SameLine();
  if (ImGui::SmallButton("Look again")) camo::Rescan();
  const std::string strength_text = rex::cvar::GetFlagByName("cod3_camo_strength");
  int strength = strength_text.empty() ? 100 : std::atoi(strength_text.c_str());
  if (ImGui::SliderInt("Strength", &strength, 0, 100, "%d%%")) {
    rex::cvar::SetFlagByName("cod3_camo_strength", std::to_string(std::clamp(strength, 0, 100)));
    dirty = true;
  }
  // [acct2-bro] 2026-11-22 x509: how fast the moving ("Animated") camos crawl; by real time, the same at any frame rate
  const std::string crawl_text = rex::cvar::GetFlagByName("cod3_camo_crawl");
  int crawl = crawl_text.empty() ? 100 : std::atoi(crawl_text.c_str());
  if (ImGui::SliderInt("Crawl speed", &crawl, 0, 400, "%d%%")) {
    rex::cvar::SetFlagByName("cod3_camo_crawl", std::to_string(std::clamp(crawl, 0, 400)));
    dirty = true;
  }
#if defined(COD3_MP)
  ImGui::SliderInt("Player", &g_player, 1, 4);
  ImGui::SameLine();
  ImGui::TextDisabled("(split screen: each player has their own)");
#else
  g_player = 1;
#endif
  ImGui::Separator();
  dirty |= Picker("All guns", g_player, "*", "None");
  const std::vector<std::string> guns = camo::GunsSeen();
  if (guns.empty()) {
    ImGui::TextDisabled("Each gun gets its own row here once you have held it.");
  }
  for (const std::string& gun : guns) dirty |= Picker(gun.c_str(), g_player, gun, "Same as all guns");
  return dirty;
}

}  // namespace cod3::ui::camo_tab
