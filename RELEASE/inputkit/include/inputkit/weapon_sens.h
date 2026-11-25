// inputkit - sensitivity per weapon group, from the hip and aiming down the sights: the settings text shared by every
// game.
//
// [input-port] 2026-11-24, taken out of Call of Duty 3's game/aim_sensitivity.cpp (claude-9b, owner 2026-10-20:
// "allow for ADS sensitivity scaling per weapon class. (percentage based from 0-200%)", "also regular sensitivity
// not just ADS"). Each game names its own groups (CoD3: rifle shotgun sniper smg lmg mg pistol launcher mounted
// other) and reads the weapon in hand its own way; the text, the limits and the blend are the same everywhere:
//   "group=hip/aiming" items separated by spaces or commas, percentages 0-200; "group=N" sets both; a group left
//   out is 100/100 (the game's own feel). Anything else is skipped.
// The multiplier blends hip into aiming as the weapon comes up to the sights (0 at the hip, 1 at the sights).
//
// The game thread asks for the multiplier every usercmd: a game parses again only when the setting's text changed,
// and compares that text without copying it first (CoD3's aim_sensitivity.cpp Lookup).

#pragma once

#include <array>
#include <string>
#include <string_view>

namespace inputkit::weaponsens {

inline constexpr int kMaxGroups = 16;
inline constexpr int kMinPercent = 0;
inline constexpr int kMaxPercent = 200;

struct Groups {
  const char* const* keys;  // "rifle", "shotgun" ...
  int count;                // at most kMaxGroups
};

struct Table {
  std::array<std::array<int, 2>, kMaxGroups> pct;  // [group][0 hip, 1 aiming]
  Table() {
    for (auto& g : pct) g = {100, 100};
  }
  bool AllDefault(const Groups& groups) const;
  // 1 at 100/100; `aim` 0 (hip) .. 1 (at the sights).
  double Multiplier(int group, float aim) const;
};

Table Parse(const Groups& groups, std::string_view text);
std::string Format(const Groups& groups, const Table& t);
int ClampPercent(long v);

}  // namespace inputkit::weaponsens
