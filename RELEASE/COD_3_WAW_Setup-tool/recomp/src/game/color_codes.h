// cod3 - World at War style ^0-^9 color codes and the ^: rainbow (owner 2026-11-03: the rainbow "similar to ^: from
// WAW"; "names and other user supplied text things can inherit those various color codes. ^0-9"; "rainbow code is
// "^:" with the colon"; "these user supplied entries need to be safe guarded too"; ^8/^9: "team colors" like World at
// War). Both programs. Header only, no allocation beyond the returned strings, no formatting, nothing run.
//
// A '^' followed by a digit picks a color for the text after it, as World at War's text renderer does (CoD WaW 1.7,
// both programs: the draw loop takes '^' + '0'..'9', skips "^^"); ^0-^7 are its table's colors, ^8 and ^9 the two
// teams' (WaW: g_TeamColor_Allies and g_TeamColor_Axis, CL_UpdateColor; their default values here). "^:" makes the
// text after it a rainbow (the owner's; World at War 1.7's own draw loop takes only digits).
// Call of Duty 3 itself only knows a code at the very start of a few HUD lines (its
// line draw 0x8242A1E0 colors the whole line by a leading ^1-^9 and drops a trailing ^7) and skips codes when it
// counts a line's printed length (0x824D2260, 0x82417778).

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace cod3::color_codes {

// 0xRRGGBBAA of ^0-^9 as World at War draws them in names, the HUD and menus (its console shows ^6 pink instead).
inline constexpr uint32_t kCodeRgba[10] = {
    0x000000FFu,  // ^0 black
    0xFF3333FFu,  // ^1 red
    0x00FF00FFu,  // ^2 green
    0xFFFF00FFu,  // ^3 yellow
    0x0000FFFFu,  // ^4 blue
    0x00FFFFFFu,  // ^5 cyan
    0xEEC062FFu,  // ^6 light orange
    0xFFFFFFFFu,  // ^7 white
    0x99A3B0FFu,  // ^8 the Allies' team color (WaW g_TeamColor_Allies 0.6 0.64 0.69)
    0xA69169FFu,  // ^9 the Axis' team color (WaW g_TeamColor_Axis 0.65 0.57 0.41)
};

// The six bright codes in rainbow order: ^1 red, ^6 orange, ^3 yellow, ^2 green, ^5 cyan, ^4 blue.
inline constexpr int kRainbowCodes[6] = {1, 6, 3, 2, 5, 4};

// The code after '^' that makes a rainbow, and its number in CodeOf's answers.
inline constexpr char kRainbowCode = ':';
inline constexpr int kRainbow = 10;

// A ^: rainbow moves on one color every this many seconds; each printed character is one color further.
inline constexpr double kRainbowStepSeconds = 0.5;

// The code a character after '^' stands for: 0-9, kRainbow for ':', -1 when it isn't one.
inline constexpr int CodeOf(char c) {
  return (c >= '0' && c <= '9') ? c - '0' : (c == kRainbowCode ? kRainbow : -1);
}

// Whether text[i] starts a color code: '^' and a digit (World at War's rule; "^^" is not a code) or "^:".
inline bool IsCode(std::string_view text, size_t i) {
  return i + 1 < text.size() && text[i] == '^' && CodeOf(text[i + 1]) >= 0;
}

// The text without its color codes (what is printed).
inline std::string Strip(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (size_t i = 0; i < text.size(); ++i) {
    if (IsCode(text, i)) {
      ++i;
      continue;
    }
    out.push_back(text[i]);
  }
  return out;
}

// Whether anything but spaces is printed.
inline bool HasVisible(std::string_view text) {
  for (size_t i = 0; i < text.size(); ++i) {
    if (IsCode(text, i)) {
      ++i;
      continue;
    }
    if (text[i] != ' ') return true;
  }
  return false;
}

// 0xRRGGBBAA of the n-th printed character of a ^: rainbow at `seconds` (any clock).
inline uint32_t RainbowRgba(double seconds, int64_t n) {
  constexpr int64_t kCount = int64_t(sizeof(kRainbowCodes) / sizeof(kRainbowCodes[0]));
  const double steps = std::isfinite(seconds) ? std::floor(seconds / kRainbowStepSeconds) : 0.0;
  const int64_t k = int64_t(std::fmod(steps, double(kCount))) + n % kCount;
  return kCodeRgba[kRainbowCodes[((k % kCount) + kCount) % kCount]];
}

// Calls f(run, code) for each stretch of text printed in one color, in order: code is -1 before the first code (the
// caller's own color), 0-9 or kRainbow. The codes themselves are not in any run. Bounds-checked for any bytes.
template <typename F>
inline void ForEachRun(std::string_view text, F&& f) {
  int code = -1;
  size_t start = 0;
  for (size_t i = 0; i < text.size(); ++i) {
    if (!IsCode(text, i)) continue;
    if (i > start) f(text.substr(start, i - start), code);
    code = CodeOf(text[i + 1]);
    start = i + 2;
    ++i;
  }
  if (start < text.size()) f(text.substr(start), code);
}

// A player name made safe before the game or anyone else sees it (both programs; the setup tool's
// game_settings._is_player_name and the SDK's player-name patch keep the same rule):
//  - printable ASCII only (32-126); no '"' or '\' (they split the game's info strings), '%' (a format code) or ';'
//    (it ends a console command);
//  - '^' only as a color code (^0-^9, ^:); any other '^' is dropped;
//  - spaces trimmed at both ends; at most max_bytes bytes, codes included (the game's own room for a name), never
//    cutting a code in half; codes with nothing printed after them dropped;
//  - empty when nothing visible is left: the caller then uses the game's own name, so no one is nameless or
//    invisible.
inline std::string SanitizeName(std::string_view raw, size_t max_bytes) {
  std::string s;
  s.reserve(raw.size() < size_t(64) ? raw.size() : size_t(64));
  for (size_t i = 0; i < raw.size() && s.size() < 256; ++i) {
    const char c = raw[i];
    const unsigned char u = static_cast<unsigned char>(c);
    if (u < 32 || u > 126 || c == '"' || c == '\\' || c == '%' || c == ';') continue;
    if (c == '^') {
      if (i + 1 < raw.size() && CodeOf(raw[i + 1]) >= 0) {
        s.push_back('^');
        s.push_back(raw[++i]);
      }
      continue;
    }
    s.push_back(c);
  }
  // Leading spaces (also those right after leading codes are kept: they are printed).
  size_t lead = 0;
  while (lead < s.size() && s[lead] == ' ') ++lead;
  s.erase(0, lead);
  if (s.size() > max_bytes) {
    s.resize(max_bytes);
    if (!s.empty() && s.back() == '^') s.pop_back();  // a code cut in half
  }
  // Trailing spaces and trailing codes, until neither is left. Every '^' still in s starts a code (the character
  // after a code's '^' is a digit or ':', never '^'), so a '^' second from the end is a code at the end.
  for (;;) {
    if (!s.empty() && s.back() == ' ') {
      s.pop_back();
    } else if (s.size() >= 2 && s[s.size() - 2] == '^') {
      s.resize(s.size() - 2);
    } else {
      break;
    }
  }
  if (!HasVisible(s)) return {};
  return s;
}

}  // namespace cod3::color_codes
