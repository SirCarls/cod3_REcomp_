// cod3 - sensitivity per weapon class (see aim_sensitivity.h).

#include "game/aim_sensitivity.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <sstream>
#include <string>

#include <imgui.h>
#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/addresses.h"
#include "game/guest.h"
#include "game/program.h"
#include "ui/slider_box.h"
#include "ui/theme.h"

REXCVAR_DEFINE_STRING(cod3_weapon_sensitivity, "", "cod3/Aim",
                      "Sensitivity per weapon group for player 1, from the hip and aiming, in percent "
                      "(0-200, 100 = the game's own): \"group=hip/aiming\" items, e.g. \"smg=80/60 "
                      "sniper=100/40\". Groups: rifle shotgun sniper smg lmg mg pistol launcher mounted "
                      "other; one left out is 100/100");
REXCVAR_DEFINE_STRING(cod3_weapon_sensitivity_p2, "", "cod3/Aim",
                      "The same for multiplayer's split-screen player 2");
REXCVAR_DEFINE_STRING(cod3_weapon_sensitivity_p3, "", "cod3/Aim",
                      "The same for multiplayer's split-screen player 3");
REXCVAR_DEFINE_STRING(cod3_weapon_sensitivity_p4, "", "cod3/Aim",
                      "The same for multiplayer's split-screen player 4");
REXCVAR_DEFINE_BOOL(cod3_debug_weapon_sensitivity, false, "cod3",
                    "Log the weapon group in hand, how far it is up to the sights and the turn's "
                    "multiplier twice a second while the stick turns the view");

namespace cod3::aimsens {
namespace {

using guest::ReadF32;
using guest::ReadU32;
using guest::WriteF32;

constexpr std::array<const char*, kGroupCount> kKeys = {
    "rifle", "shotgun", "sniper", "smg", "lmg", "mg", "pistol", "launcher", "mounted", "other"};
constexpr std::array<const char*, kGroupCount> kLabels = {
    "Rifles",           "Shotguns",      "Sniper rifles", "Submachine guns", "Light machine guns",
    "Machine guns",     "Pistols",       "Launchers",     "Mounted guns",    "Other (grenades, binoculars ...)"};

// The weapon in a local client's hands, read as the game's CG_DrawCrosshair
// and gun placement (campaign sub_82445118, cod3mp sub_8245E3A8) read it: the
// list *0x82A2AB40 at [client + 1], that entry's client data at +528, its
// held weapon at +132 and how far it is up to the sights at +140; the weapon
// table *0x82A2A2E0 [weapon], whose definition has the game's weaponClass at
// +0xB0 and ammoType at +0xC0 (the game's weapon field table at 0x825CFBD0:
// "weaponClass" +0xB0 type 9, "ammoType" +0xC0 type 10, "adsOverlayShader"
// +0x650 type 0, a string; the same offsets in cod3mp's table at 0x825D7AAC).
// cod3mp: the list *0x82BCC7FC, client data +512, weapon +164, sights +172,
// the weapon table *0x82BCBF9C.
constexpr uint32_t kEntityList = COD3_ADDR(0x82A2AB40, 0x82BCC7FC);
constexpr uint32_t kWeaponDefs = COD3_ADDR(0x82A2A2E0, 0x82BCBF9C);
constexpr uint32_t kEntityClient = cod3::kMultiplayer ? 512 : 528;
constexpr uint32_t kHeldWeapon = cod3::kMultiplayer ? 164 : 132;
constexpr uint32_t kWeaponPosFrac = cod3::kMultiplayer ? 172 : 140;
constexpr uint32_t kDefClass = 0xB0;
constexpr uint32_t kDefAmmoType = 0xC0;
constexpr uint32_t kDefAdsOverlay = 0x650;  // the scope picture over the screen at the sights
constexpr uint32_t kMaxWeaponIndex = 1024;  // a sanity bound, far above the game's table

// The game's weaponClass names (its enum list at 0x825CFB70, both programs)
// and ammoType's first name ("spread", the shotguns' ammunition, 0x825CFBB4).
enum GameClass : uint32_t {
  kClassRifle = 0, kClassMg = 1, kClassSmg = 2, kClassLmg = 3, kClassPistol = 4, kClassGrenade = 5,
  kClassRocketLauncher = 6, kClassTurret = 7, kClassSpotter = 8, kClassNonPlayer = 9, kClassSniper = 10,
};
constexpr uint32_t kAmmoSpread = 0;

bool Readable(uint32_t address) {
  return address >= 0x10000 && (address < 0x7F000000 || address >= 0x80000000) && address < 0xFFFF0000;
}

// A scoped rifle is a sniper rifle here: the game files most of them as
// weaponClass "rifle" with a scope overlay (Forest's first rifle), and only a
// few as "sniper".
int GroupOf(uint32_t game_class, uint32_t ammo, bool scope) {
  switch (game_class) {
    case kClassRifle: return ammo == kAmmoSpread ? kShotgun : scope ? kSniper : kRifle;
    case kClassSniper: return kSniper;
    case kClassSmg: return kSmg;
    case kClassLmg: return kLmg;
    case kClassMg: return kMg;
    case kClassPistol: return kPistol;
    case kClassRocketLauncher: return kLauncher;
    case kClassTurret: return kMounted;
    default: return kOther;
  }
}

// --- the settings ---------------------------------------------------------------
struct Table {
  std::array<std::array<int, 2>, kGroupCount> pct;  // [group][0 hip, 1 aiming]
  Table() {
    for (auto& g : pct) g = {100, 100};
  }
  bool AllDefault() const {
    for (const auto& g : pct)
      if (g[0] != 100 || g[1] != 100) return false;
    return true;
  }
};

std::string Text(int player) {
  switch (player) {
    case 1: return REXCVAR_GET(cod3_weapon_sensitivity_p2);
    case 2: return REXCVAR_GET(cod3_weapon_sensitivity_p3);
    case 3: return REXCVAR_GET(cod3_weapon_sensitivity_p4);
    default: return REXCVAR_GET(cod3_weapon_sensitivity);
  }
}

int Clamp(long v) { return int(std::clamp<long>(v, kMinPercent, kMaxPercent)); }

// "group=hip/aiming" or "group=both" items, separated by spaces or commas;
// anything else is skipped.
Table Parse(const std::string& text) {
  Table t;
  std::string norm = text;
  std::replace(norm.begin(), norm.end(), ',', ' ');
  std::istringstream in(norm);
  std::string item;
  while (in >> item) {
    const auto eq = item.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = item.substr(0, eq);
    int g = -1;
    for (int i = 0; i < kGroupCount; ++i)
      if (key == kKeys[i]) g = i;
    if (g < 0) continue;
    const char* v = item.c_str() + eq + 1;
    char* end = nullptr;
    const long hip = std::strtol(v, &end, 10);
    if (end == v) continue;
    long ads = hip;
    if (*end == '/') {
      const char* a = end + 1;
      ads = std::strtol(a, &end, 10);
      if (end == a) ads = hip;
    }
    t.pct[g] = {Clamp(hip), Clamp(ads)};
  }
  return t;
}

std::string Format(const Table& t) {
  std::string out;
  for (int g = 0; g < kGroupCount; ++g) {
    const auto& p = t.pct[g];
    if (p[0] == 100 && p[1] == 100) continue;
    if (!out.empty()) out += ' ';
    out += kKeys[g];
    out += '=' + std::to_string(p[0]);
    if (p[1] != p[0]) out += '/' + std::to_string(p[1]);
  }
  return out;
}

std::mutex g_mutex;
std::array<std::string, kPlayers> g_text;
std::array<Table, kPlayers> g_table;
std::array<bool, kPlayers> g_parsed{};

// The player's table now (parsed again when the setting's text changed).
Table Get(int player) {
  player = std::clamp(player, 0, kPlayers - 1);
  const std::string text = Text(player);
  std::lock_guard lock(g_mutex);
  if (!g_parsed[player] || text != g_text[player]) {
    g_text[player] = text;
    g_table[player] = Parse(text);
    g_parsed[player] = true;
    const std::string shown = Format(g_table[player]);
    REXLOG_INFO("cod3: weapon sensitivity, player {}: {}", player + 1,
                shown.empty() ? std::string("100% everywhere (the game's own)")
                              : shown + " (hip/aiming %; the rest 100%)");
  }
  return g_table[player];
}

// What the guest thread saw last, per local client (F2's "now" line).
struct Seen {
  std::atomic<int> group{kOther};
  std::atomic<float> aim{0.0f};
  std::atomic<float> multiplier{1.0f};
  std::atomic<bool> valid{false};
};
std::array<Seen, kPlayers> g_seen;

uint32_t AnglesOf(uint32_t client) {
  return addr::kClientActive + client * addr::kClientActiveStride + addr::kViewAnglesOffset;
}

float AngleDelta(float to, float from) {
  float d = std::remainder(to - from, 360.0f);  // -180 .. 180
  return std::isfinite(d) ? d : 0.0f;
}

}  // namespace

const char* GroupKey(int group) { return group >= 0 && group < kGroupCount ? kKeys[group] : "other"; }
const char* GroupLabel(int group) { return group >= 0 && group < kGroupCount ? kLabels[group] : kLabels[kOther]; }

const char* SettingName(int player) {
  switch (player) {
    case 1: return "cod3_weapon_sensitivity_p2";
    case 2: return "cod3_weapon_sensitivity_p3";
    case 3: return "cod3_weapon_sensitivity_p4";
    default: return "cod3_weapon_sensitivity";
  }
}

int Percent(int player, int group, bool aiming) {
  if (group < 0 || group >= kGroupCount) return 100;
  return Get(player).pct[group][aiming ? 1 : 0];
}

void SetPercent(int player, int group, bool aiming, int percent) {
  if (group < 0 || group >= kGroupCount) return;
  player = std::clamp(player, 0, kPlayers - 1);
  Table t = Get(player);
  t.pct[group][aiming ? 1 : 0] = Clamp(percent);
  rex::cvar::SetFlagByName(SettingName(player), Format(t));
}

void ResetPlayer(int player) {
  rex::cvar::SetFlagByName(SettingName(std::clamp(player, 0, kPlayers - 1)), "");
}

int HeldGroup(uint8_t* base, uint32_t client, float* aim) {
  if (aim) *aim = 0.0f;
  if (client >= uint32_t(kPlayers)) return kOther;
  const uint32_t list = ReadU32(base, kEntityList);
  if (!Readable(list)) return kOther;
  const uint32_t entity = ReadU32(base, list + 4 * (client + 1));
  if (!Readable(entity)) return kOther;
  const uint32_t data = ReadU32(base, entity + kEntityClient);
  if (!Readable(data)) return kOther;
  if (aim) {
    const float f = ReadF32(base, data + kWeaponPosFrac);
    *aim = std::isfinite(f) ? std::clamp(f, 0.0f, 1.0f) : 0.0f;
  }
  const uint32_t weapon = ReadU32(base, data + kHeldWeapon);
  if (weapon == 0 || weapon >= kMaxWeaponIndex) return kOther;
  const uint32_t defs = ReadU32(base, kWeaponDefs);
  if (!Readable(defs)) return kOther;
  const uint32_t def = ReadU32(base, defs + 4 * weapon);
  if (!Readable(def)) return kOther;
  const uint32_t overlay = ReadU32(base, def + kDefAdsOverlay);
  const bool scope = Readable(overlay) && guest::ReadU8(base, overlay) != 0;
  return GroupOf(ReadU32(base, def + kDefClass), ReadU32(base, def + kDefAmmoType), scope);
}

double Multiplier(uint8_t* base, uint32_t client) {
  if (client >= uint32_t(kPlayers)) return 1.0;
  const Table t = Get(int(client));
  float aim = 0.0f;
  const int group = HeldGroup(base, client, &aim);
  const double hip = t.pct[group][0], ads = t.pct[group][1];
  const double m = (hip + (ads - hip) * aim) / 100.0;
  Seen& s = g_seen[client];
  s.group.store(group, std::memory_order_relaxed);
  s.aim.store(aim, std::memory_order_relaxed);
  s.multiplier.store(float(m), std::memory_order_relaxed);
  s.valid.store(true, std::memory_order_relaxed);
  return m;
}

namespace {

// The stick's turn (guest thread only): CL_MouseMove below. The game's own
// turn this frame (its sensitivity rows, the zoom, its turn limits) is
// finished first, then multiplied, so 50% turns half as far as the game would
// at any stick deflection, the limit included, like the mouse. Sticky Aim
// isn't in this turn (it moves the player state's delta_angles in the
// player's movement code), so it is never scaled here.
struct StickTurn {
  uint32_t client = 0;
  bool valid = false;
  float pitch = 0.0f, yaw = 0.0f;
};

StickTurn BeginStickTurn(uint8_t* base) {
  StickTurn t;
  const uint32_t client = ReadU32(base, addr::kActiveLocalClient);
  if (client >= uint32_t(kPlayers)) return t;
  const uint32_t angles = AnglesOf(client);
  t = {client, true, ReadF32(base, angles + 0), ReadF32(base, angles + 4)};
  return t;
}

void EndStickTurn(uint8_t* base, const StickTurn& t) {
  if (!t.valid || ReadU32(base, addr::kActiveLocalClient) != t.client) return;
  const uint32_t angles = AnglesOf(t.client);
  const float dp = AngleDelta(ReadF32(base, angles + 0), t.pitch);
  const float dy = AngleDelta(ReadF32(base, angles + 4), t.yaw);
  if (dp == 0.0f && dy == 0.0f) return;  // the stick didn't turn the view
  const double m = Multiplier(base, t.client);
  if (std::abs(m - 1.0) > 1e-6) {
    const float pitch = t.pitch + float(dp * m), yaw = t.yaw + float(dy * m);
    if (std::isfinite(pitch) && std::isfinite(yaw)) {
      WriteF32(base, angles + 0, pitch);
      WriteF32(base, angles + 4, yaw);
    }
  }
  if (!REXCVAR_GET(cod3_debug_weapon_sensitivity)) return;
  static auto next = std::chrono::steady_clock::time_point{};
  const auto now = std::chrono::steady_clock::now();
  if (now < next) return;
  next = now + std::chrono::milliseconds(500);
  const Seen& s = g_seen[t.client];
  REXLOG_INFO("cod3: weapon sensitivity debug: player {}, {}, at the sights {:.3f}, x{:.3f}; the stick's turn "
              "this frame yaw {:.4f} -> {:.4f}, pitch {:.4f} -> {:.4f}",
              t.client + 1, kKeys[s.group.load()], s.aim.load(), m, dy, dy * m, dp, dp * m);
}

}  // namespace

namespace {
// One short, muted line (the values above it are what the eye should find).
void HelpLine(const char* text) {
  ImGui::PushTextWrapPos(0.0f);
  ImGui::PushStyleColor(ImGuiCol_Text, ui::CurrentPalette().muted);
  ImGui::TextUnformatted(text);
  ImGui::PopStyleColor();
  ImGui::PopTextWrapPos();
}
}  // namespace

bool DrawF2Section() {
  bool changed = false;
  ui::Section("Sensitivity per weapon");
  static int player = 0;
  if (!cod3::kMultiplayer) player = 0;
  // Reset sits on the right, away from the values.
  {
    const char* reset = "Reset to 100%##weapon_sens";
    const float w = ImGui::CalcTextSize(reset, nullptr, true).x + ImGui::GetStyle().FramePadding.x * 2.0f;
    if (cod3::kMultiplayer) {
      const char* players[] = {"Player 1", "Player 2", "Player 3", "Player 4"};
      ImGui::SetNextItemWidth(ImGui::CalcTextSize("Player 4").x * 2.2f);
      ImGui::Combo("##weapon_sens_player", &player, players, kPlayers);
      if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Split screen: each player has their own. Player 1 is the keyboard and mouse, or the "
                          "first controller.");
      ImGui::SameLine();
    }
    ImGui::SetCursorPosX(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - w));
    if (ImGui::SmallButton(reset)) {
      ResetPlayer(player);
      changed = true;
    }
  }
  const Seen& s = g_seen[player];
  const int held = s.valid.load(std::memory_order_relaxed) ? s.group.load(std::memory_order_relaxed) : -1;
  const ui::Palette& pal = ui::CurrentPalette();
  if (ImGui::BeginTable("##weapon_sens", 3, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_RowBg)) {
    ImGui::TableSetupColumn("Weapon", ImGuiTableColumnFlags_WidthStretch, 0.9f);
    ImGui::TableSetupColumn("Hip", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableSetupColumn("Aiming", ImGuiTableColumnFlags_WidthStretch, 1.0f);
    ImGui::TableHeadersRow();
    for (int g = 0; g < kGroupCount; ++g) {
      ImGui::TableNextRow();
      const bool in_hand = g == held;
      if (in_hand) {
        // The weapon in hand now: its row is the one to look at.
        ImVec4 band = pal.accent;
        band.w = 0.22f;
        ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg1, ImGui::GetColorU32(band));
      }
      ImGui::TableSetColumnIndex(0);
      ImGui::AlignTextToFramePadding();
      if (in_hand) ImGui::PushStyleColor(ImGuiCol_Text, pal.accent_text);
      ImGui::TextUnformatted(g == kOther ? "Other" : kLabels[g]);
      if (in_hand) ImGui::PopStyleColor();
      if (g == kOther && ImGui::IsItemHovered()) ImGui::SetTooltip("Grenades, binoculars and anything else.");
      for (int col = 0; col < 2; ++col) {
        ImGui::TableSetColumnIndex(col + 1);
        ImGui::PushID(g * 2 + col);
        int v = Percent(player, g, col == 1);
        if (ui::SliderIntBox("##pct", &v, kMinPercent, kMaxPercent, "%d%%", ImGui::GetContentRegionAvail().x)) {
          SetPercent(player, g, col == 1, v);
          changed = true;
        }
        ImGui::PopID();
      }
    }
    ImGui::EndTable();
  }
  HelpLine("100% is the game's own feel. Hip blends into Aiming as the weapon comes up; aiming down the sights "
           "itself never changes. Mouse and controller.");
  if (ImGui::IsItemHovered())
    ImGui::SetTooltip("In the ~ console: cod3_weapon_sensitivity \"smg=80/60 sniper=100/40\"\n"
                      "(rifle shotgun sniper smg lmg mg pistol launcher mounted other; hip/aiming).\n"
                      "Aim assist (the game's Sticky Aim) is never touched by these percentages.");
  ImGui::Spacing();
  return changed;
}

}  // namespace cod3::aimsens

// CL_MouseMove (campaign 0x8250FF48, cod3mp 0x8252B578): the game turns the
// view by the stick here, for the local client whose usercmd is being built.
REX_EXTERN(__imp__CL_MouseMove);
REX_HOOK_RAW(CL_MouseMove) {
  const cod3::aimsens::StickTurn turn = cod3::aimsens::BeginStickTurn(base);
  __imp__CL_MouseMove(ctx, base);
  cod3::aimsens::EndStickTurn(base, turn);
}
