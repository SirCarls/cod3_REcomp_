// inputkit - sensitivity per weapon group (see weapon_sens.h). The text rules are Call of Duty 3's
// aim_sensitivity.cpp Parse / Format.

#include "inputkit/weapon_sens.h"

#include <algorithm>
#include <cstdlib>

namespace inputkit::weaponsens {

int ClampPercent(long v) { return int(std::clamp<long>(v, kMinPercent, kMaxPercent)); }

bool Table::AllDefault(const Groups& groups) const {
  for (int g = 0; g < groups.count && g < kMaxGroups; ++g)
    if (pct[g][0] != 100 || pct[g][1] != 100) return false;
  return true;
}

double Table::Multiplier(int group, float aim) const {
  if (group < 0 || group >= kMaxGroups) return 1.0;
  const double hip = pct[group][0], ads = pct[group][1];
  return (hip + (ads - hip) * aim) / 100.0;
}

Table Parse(const Groups& groups, std::string_view text) {
  Table t;
  size_t i = 0;
  auto separator = [](char c) { return c == ' ' || c == ',' || c == '\t' || c == '\n' || c == '\r'; };
  while (i < text.size()) {
    while (i < text.size() && separator(text[i])) ++i;
    size_t end = i;
    while (end < text.size() && !separator(text[end])) ++end;
    const std::string item(text.substr(i, end - i));
    i = end;
    const auto eq = item.find('=');
    if (eq == std::string::npos) continue;
    const std::string key = item.substr(0, eq);
    int g = -1;
    for (int k = 0; k < groups.count && k < kMaxGroups; ++k)
      if (key == groups.keys[k]) g = k;
    if (g < 0) continue;
    const char* v = item.c_str() + eq + 1;
    char* stop = nullptr;
    const long hip = std::strtol(v, &stop, 10);
    if (stop == v) continue;
    long ads = hip;
    if (*stop == '/') {
      const char* a = stop + 1;
      ads = std::strtol(a, &stop, 10);
      if (stop == a) ads = hip;
    }
    t.pct[g] = {ClampPercent(hip), ClampPercent(ads)};
  }
  return t;
}

std::string Format(const Groups& groups, const Table& t) {
  std::string out;
  for (int g = 0; g < groups.count && g < kMaxGroups; ++g) {
    const auto& p = t.pct[g];
    if (p[0] == 100 && p[1] == 100) continue;
    if (!out.empty()) out += ' ';
    out += groups.keys[g];
    out += '=' + std::to_string(p[0]);
    if (p[1] != p[0]) out += '/' + std::to_string(p[1]);
  }
  return out;
}

}  // namespace inputkit::weaponsens
