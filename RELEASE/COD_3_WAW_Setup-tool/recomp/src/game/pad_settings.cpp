// cod3 - [acct2-ocr] 2026-11-07 the controller's settings, per player (see pad_settings.h).

#include "game/pad_settings.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <string>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/logging.h>

#include "game/addresses.h"  // controls::kSettings, kLocalClients (cod3mp: addresses_mp.h)
#include "game/guest.h"
#include "game/program.h"
#include "game/protected_dvars.h"  // SaveSettings: the in-game menus' save path
#include "ui/theme.h"

REXCVAR_DEFINE_INT32(cod3_pad_sensitivity_max, 50, "cod3/Controller",
                     "Highest Horizontal and Vertical Sensitivity on Options > Controls, both rows, every player "
                     "(the game's own pages: 37 sideways, 50 up/down). Each step turns the stick 0.04 x the game's "
                     "m_yaw / m_pitch more")
    .range(cod3::pad::kGameHorizontalMax, cod3::pad::kSensitivityCeiling);

// Player 1's; cod3mp also has split screen's _p2 .. _p4.
#define COD3_PAD_P1(name, lo, hi, help) REXCVAR_DEFINE_INT32(name, -1, "cod3/Controller", help).range(lo, hi)
#if defined(COD3_MP)
#define COD3_PAD_SETTING(name, lo, hi, help)                                                                  \
  COD3_PAD_P1(name, lo, hi, help);                                                                           \
  COD3_PAD_P1(name##_p2, lo, hi, "Split screen: player 2's (as " #name ")");                                 \
  COD3_PAD_P1(name##_p3, lo, hi, "Split screen: player 3's (as " #name ")");                                 \
  COD3_PAD_P1(name##_p4, lo, hi, "Split screen: player 4's (as " #name ")")
#else
#define COD3_PAD_SETTING(name, lo, hi, help) COD3_PAD_P1(name, lo, hi, help)
#endif

COD3_PAD_SETTING(cod3_pad_invert, -1, 1,
                 "Options > Controls > Invert Aim of player 1 (the controller): 0 off, 1 on; -1 leaves it to the "
                 "Options page");
COD3_PAD_SETTING(cod3_pad_vibration, -1, 1,
                 "Options > Controls > Vibration of player 1: 0 off, 1 on; -1 leaves it to the Options page");
COD3_PAD_SETTING(cod3_pad_horizontal_sensitivity, -1, 100,
                 "Options > Controls > Horizontal Sensitivity of player 1: 0 up to cod3_pad_sensitivity_max; -1 "
                 "leaves it to the Options page");
COD3_PAD_SETTING(cod3_pad_vertical_sensitivity, -1, 100,
                 "Options > Controls > Vertical Sensitivity of player 1: 0 up to cod3_pad_sensitivity_max; -1 "
                 "leaves it to the Options page");

#undef COD3_PAD_SETTING
#undef COD3_PAD_P1

namespace cod3::pad {
namespace {

using guest::ReadS32;
using guest::ReadU32;
using guest::ReadU8;

// The campaign's one block of control settings (addr::controls::kSettings); cod3mp's four controller profiles
// (0x82C6B730 + profile * 6984, game/mp/controls_mp.cpp).
constexpr uint32_t kSettings = cod3::addr::controls::kSettings;  // cod3mp's: game/mp/addresses_mp.h
constexpr uint32_t kProfileStride = 6984;
constexpr uint32_t kProfiles = 4;
// cod3mp's local clients (addresses_mp.h kLocalClients): +180 the controller profile, +184 in use (0: not).
constexpr uint32_t kMpLocalClients = cod3::addr::keybinds::kLocalClients;  // read only in cod3mp (0x82C7A270 there)
constexpr uint32_t kMpLocalClientStride = 3208;
constexpr uint32_t kMpClientProfile = 180;
constexpr uint32_t kMpClientState = 184;

// By Setting: its name stem and where it is in the control settings.
struct Info {
  const char* name;
  uint32_t offset;
  bool byte;
};
constexpr std::array<Info, kSettingCount> kInfo = {{
    {"cod3_pad_invert", 65, true},
    {"cod3_pad_vibration", 66, true},
    {"cod3_pad_horizontal_sensitivity", 68, false},
    {"cod3_pad_vertical_sensitivity", 72, false},
}};

// "cod3_pad_invert", "cod3_pad_invert_p2" ...
const std::array<std::array<std::string, kMaxPlayers>, kSettingCount>& Names() {
  static const auto names = [] {
    std::array<std::array<std::string, kMaxPlayers>, kSettingCount> n;
    for (int s = 0; s < kSettingCount; ++s)
      for (int p = 0; p < kMaxPlayers; ++p)
        n[s][p] = std::string(kInfo[s].name) + (p ? "_p" + std::to_string(p + 1) : std::string());
    return n;
  }();
  return names;
}

bool Valid(int player, int s) { return player >= 0 && player < Players() && s >= 0 && s < kSettingCount; }

// The control settings of `player` now; 0 when that player has none (no such split-screen player).
uint32_t Block(uint8_t* base, int player) {
  if (!cod3::kMultiplayer) return player == 0 ? kSettings : 0;
  auto client = [&](int p, uint32_t field) {
    return ReadU32(base, kMpLocalClients + uint32_t(p) * kMpLocalClientStride + field);
  };
  // Players 2 to 4 only while their client is in use; never a profile a player before them has.
  if (player > 0 && client(player, kMpClientState) == 0) return 0;
  const uint32_t profile = client(player, kMpClientProfile);
  if (profile >= kProfiles) return 0;
  for (int q = 0; q < player; ++q)
    if ((q == 0 || client(q, kMpClientState) != 0) && client(q, kMpClientProfile) == profile) return 0;
  return kSettings + profile * kProfileStride;
}

int ReadRow(uint8_t* base, uint32_t block, int s) {
  const uint32_t at = block + kInfo[s].offset;
  return kInfo[s].byte ? int(ReadU8(base, at)) : ReadS32(base, at);
}

void WriteRow(uint8_t* base, uint32_t block, int s, int value) {
  const uint32_t at = block + kInfo[s].offset;
  if (kInfo[s].byte)
    guest::WriteU8(base, at, uint8_t(value));
  else
    guest::WriteU32(base, at, uint32_t(value));
}

std::array<std::array<std::atomic<int>, kSettingCount>, kMaxPlayers> g_live;
std::array<std::array<int, kSettingCount>, kMaxPlayers> g_logged;
std::atomic<bool> g_started{false};  // the live values are known

}  // namespace

int Players() { return cod3::kMultiplayer ? kMaxPlayers : 1; }

int SensitivityMax() {
  return std::clamp(int(REXCVAR_GET(cod3_pad_sensitivity_max)), kGameHorizontalMax, kSensitivityCeiling);
}

const char* SettingName(int player, int setting) {
  return Valid(player, setting) ? Names()[setting][player].c_str() : "";
}

int Most(int setting) { return setting == kHorizontal || setting == kVertical ? SensitivityMax() : 1; }

int Wanted(int player, int setting) {
  if (!Valid(player, setting)) return -1;
  const std::string v = rex::cvar::GetFlagByName(Names()[setting][player].c_str());
  return v.empty() ? -1 : std::atoi(v.c_str());
}

void SetWanted(int player, int setting, int value) {
  if (!Valid(player, setting)) return;
  const int v = value < 0 ? -1 : std::clamp(value, 0, Most(setting));
  if (Wanted(player, setting) == v) return;
  rex::cvar::SetFlagByName(Names()[setting][player].c_str(), std::to_string(v));
}

int Live(int player, int setting) {
  if (!g_started.load(std::memory_order_acquire) || !Valid(player, setting)) return -1;
  return g_live[player][setting].load(std::memory_order_relaxed);
}

void ResetAll() {
  for (int p = 0; p < Players(); ++p)
    for (int s = 0; s < kSettingCount; ++s) SetWanted(p, s, -1);
  rex::cvar::SetFlagByName("cod3_pad_sensitivity_max", std::to_string(kGameVerticalMax));
}

void Sync(PPCContext& /*ctx*/, uint8_t* base) {
  if (!g_started.load(std::memory_order_relaxed)) {
    for (auto& p : g_logged) p.fill(-2);
    for (auto& p : g_live)
      for (auto& v : p) v.store(-1, std::memory_order_relaxed);
    g_started.store(true, std::memory_order_release);
  }
  for (int p = 0; p < Players(); ++p) {
    const uint32_t block = Block(base, p);
    for (int s = 0; s < kSettingCount; ++s) {
      if (!block) {
        g_live[p][s].store(-1, std::memory_order_relaxed);
        continue;
      }
      const int wanted = Wanted(p, s);
      if (wanted >= 0) {
        const int value = std::clamp(wanted, 0, Most(s));
        if (ReadRow(base, block, s) != value) {
          WriteRow(base, block, s, value);
          if (g_logged[p][s] != value) {
            g_logged[p][s] = value;
            REXLOG_INFO("cod3: {} {}: player {}'s Options > Controls row set from the setting", Names()[s][p],
                        value, p + 1);
          }
        }
      }
      g_live[p][s].store(ReadRow(base, block, s), std::memory_order_relaxed);
    }
  }
}

void FollowPage(int player, int setting, int value) {
  if (!Valid(player, setting) || Wanted(player, setting) < 0) return;
  const int v = std::clamp(value, 0, Most(setting));
  if (Wanted(player, setting) == v) return;
  SetWanted(player, setting, v);
  protected_dvars::SaveSettings();
  REXLOG_INFO("cod3: {} {} (changed on player {}'s Options page)", Names()[setting][player], v, player + 1);
}

bool DrawF2Section() {
  bool changed = false;
  ui::Section("Sensitivity, invert and vibration");
  static int player = 0;
  if (Players() > 1) {
    const char* players[] = {"Player 1", "Player 2", "Player 3", "Player 4"};
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("Player 4").x * 2.2f);
    ImGui::Combo("##pad_player", &player, players, kMaxPlayers);
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("Split screen: each player has their own, kept with the controller profile that player "
                        "signs in with.");
  } else {
    player = 0;
  }
  int cap = SensitivityMax();
  if (ImGui::SliderInt("Highest sensitivity (both rows, every player)", &cap, kGameHorizontalMax,
                       kSensitivityCeiling, "%d", ImGuiSliderFlags_AlwaysClamp)) {
    rex::cvar::SetFlagByName("cod3_pad_sensitivity_max", std::to_string(cap));
    changed = true;
  }
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("How far Options > Controls lets Horizontal and Vertical Sensitivity go. The game's own pages "
                      "stopped Horizontal at 37 and Vertical at 50. cod3_pad_sensitivity_max");

  const bool here = Live(player, kHorizontal) >= 0;
  if (!here) ImGui::TextDisabled("Player %d isn't playing now: these apply when they join.", player + 1);
  // A sensitivity: a checkbox (set here, or the Options page's) and its slider.
  auto number_row = [&](int s, const char* label) {
    ImGui::PushID(s);
    const int wanted = Wanted(player, s);
    const int live = Live(player, s);
    bool own = wanted >= 0;
    if (ImGui::Checkbox("##own", &own)) {
      // On: start from the player's row (a player not playing now: player 1's, else the game's middle, 25).
      const int start = live >= 0 ? live : Live(0, s) >= 0 ? Live(0, s) : 25;
      SetWanted(player, s, own ? start : -1);
      changed = true;
    }
    if (ImGui::IsItemHovered())
      ImGui::SetTooltip("On: this value, for player %d (%s). Off: the Options page's.", player + 1,
                        SettingName(player, s));
    ImGui::SameLine();
    int v = own ? wanted : live >= 0 ? live : Live(0, s) >= 0 ? Live(0, s) : 25;
    if (!own) ImGui::BeginDisabled();
    if (ImGui::SliderInt(label, &v, 0, Most(s), "%d", ImGuiSliderFlags_AlwaysClamp) && own) {
      SetWanted(player, s, v);
      changed = true;
    }
    if (!own) ImGui::EndDisabled();
    ImGui::PopID();
  };
  // On / off: the Options page's, off or on.
  auto toggle_row = [&](int s, const char* label) {
    ImGui::PushID(s);
    int choice = std::clamp(Wanted(player, s), -1, 1) + 1;  // 0 the Options page's, 1 off, 2 on
    const int live = Live(player, s);
    char page[48];
    std::snprintf(page, sizeof(page), "Options page (%s)", live < 0 ? "-" : live ? "on" : "off");
    const char* items[] = {page, "Off", "On"};
    ImGui::SetNextItemWidth(ImGui::CalcTextSize("Options page (off)").x * 1.6f);
    if (ImGui::Combo(label, &choice, items, 3)) {
      SetWanted(player, s, choice - 1);
      changed = true;
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", SettingName(player, s));
    ImGui::PopID();
  };
  number_row(kHorizontal, "Horizontal sensitivity");
  number_row(kVertical, "Vertical sensitivity");
  toggle_row(kInvert, "Invert aim");
  toggle_row(kVibration, "Vibration");
  ImGui::PushTextWrapPos(0.0f);
  ImGui::TextDisabled("The game's own Options > Controls rows, for each player. Each sensitivity step turns the stick "
                      "a little faster (0.04 x the game's m_yaw / m_pitch). Set here, a value is written into the "
                      "player's own rows and a change made on the Options page comes back here; off, the page "
                      "decides as before.");
  ImGui::PopTextWrapPos();
  return changed;
}

}  // namespace cod3::pad
