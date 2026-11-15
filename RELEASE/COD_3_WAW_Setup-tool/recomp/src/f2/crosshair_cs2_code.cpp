// cod3 - Counter-Strike 2 crosshair share codes (crosshair_cs2_code.h). [acct2-decomp] 2026-11-07.
// [acct2-ocr] 2026-11-14 copied unchanged from dev/agents/acct2-decomp/cs2-crosshair/ for f2/f2_crosshair_cs2.
#include "crosshair_cs2_code.h"

#include <cstdio>

namespace cod3::crosshair_cs2 {
namespace {

constexpr char kAlphabet[] = "ABCDEFGHJKLMNOPQRSTUVWXYZabcdefhijkmnopqrstuvwxyz23456789";  // 57
constexpr size_t kBodyChars = 44;
constexpr uint8_t kVersion = 1;

int AlphabetIndex(char ch) {
  for (int i = 0; i < 57; ++i)
    if (kAlphabet[i] == ch) return i;
  return -1;
}

uint8_t Checksum(const std::array<uint8_t, 32>& b) {
  unsigned sum = 0;
  for (size_t i = 1; i < b.size(); ++i) sum += b[i];
  return static_cast<uint8_t>(sum & 0xFF);
}

}  // namespace

std::array<uint8_t, 32> ToBytes(const Code& c) {
  std::array<uint8_t, 32> b{};
  b[1] = kVersion;
  b[2] = static_cast<uint8_t>(c.screen_height & 0xFF);  // little-endian (checked on the reference example: 768)
  b[3] = static_cast<uint8_t>(c.screen_height >> 8);
  b[4] = static_cast<uint8_t>((c.style & 0x0F) | (c.follow_recoil ? 0x10 : 0) | ((c.reserved_style_bit5 & 1) << 5) |
                              (c.center_dot ? 0x40 : 0) | (c.t_style ? 0x80 : 0));
  for (int i = 0; i < 4; ++i) b[5 + i] = c.color[i];
  for (int i = 0; i < 4; ++i) b[9 + i] = c.outline_color[i];
  b[13] = c.thickness;
  b[14] = c.outline_mode;
  b[15] = static_cast<uint8_t>(c.gap);
  b[16] = c.length;
  b[17] = c.dynamic_spread_limit;
  const uint32_t split = (uint32_t(c.split_distance & 0x7F)) | (uint32_t(c.inner_split_alpha & 0x7F) << 7) |
                         (uint32_t(c.outer_split_alpha & 0x7F) << 14) | (uint32_t(c.split_size_ratio & 0x7F) << 21) |
                         (uint32_t(c.scope_dot_use_crosshair_color ? 1 : 0) << 28) |
                         (uint32_t(c.reserved_split_bits & 0x7) << 29);
  for (int i = 0; i < 4; ++i) b[18 + i] = static_cast<uint8_t>(split >> (8 * i));  // little-endian
  b[22] = c.scope_dot_scale;
  for (size_t i = 0; i < c.reserved_tail.size(); ++i) b[23 + i] = c.reserved_tail[i];
  b[0] = Checksum(b);
  return b;
}

Code FromBytes(const std::array<uint8_t, 32>& b) {
  Code c;
  c.screen_height = static_cast<uint16_t>(b[2] | (b[3] << 8));
  c.style = b[4] & 0x0F;
  c.follow_recoil = (b[4] & 0x10) != 0;
  c.reserved_style_bit5 = (b[4] >> 5) & 1;
  c.center_dot = (b[4] & 0x40) != 0;
  c.t_style = (b[4] & 0x80) != 0;
  for (int i = 0; i < 4; ++i) c.color[i] = b[5 + i];
  for (int i = 0; i < 4; ++i) c.outline_color[i] = b[9 + i];
  c.thickness = b[13];
  c.outline_mode = b[14];
  c.gap = static_cast<int8_t>(b[15]);
  c.length = b[16];
  c.dynamic_spread_limit = b[17];
  uint32_t split = 0;
  for (int i = 0; i < 4; ++i) split |= uint32_t(b[18 + i]) << (8 * i);
  c.split_distance = split & 0x7F;
  c.inner_split_alpha = (split >> 7) & 0x7F;
  c.outer_split_alpha = (split >> 14) & 0x7F;
  c.split_size_ratio = (split >> 21) & 0x7F;
  c.scope_dot_use_crosshair_color = ((split >> 28) & 1) != 0;
  c.reserved_split_bits = (split >> 29) & 0x7;
  c.scope_dot_scale = b[22];
  for (size_t i = 0; i < c.reserved_tail.size(); ++i) c.reserved_tail[i] = b[23 + i];
  return c;
}

std::optional<Code> Decode(std::string_view text, std::string* why) {
  auto fail = [&](const char* reason) -> std::optional<Code> {
    if (why) *why = reason;
    return std::nullopt;
  };
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n'))
    text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n'))
    text.remove_suffix(1);
  if (text.substr(0, 5) == "CSGO-") return fail("an old CSGO- code: CS2 no longer takes these; make a new one in CS2");
  if (text.substr(0, 2) != "CS") return fail("a CS2 crosshair code starts with CS");
  text.remove_prefix(2);
  if (text.size() != kBodyChars) return fail("a CS2 crosshair code is CS and 44 more characters");
  // The number, built most significant character first (the code keeps the least significant first).
  std::array<uint8_t, 32> b{};
  for (size_t k = kBodyChars; k-- > 0;) {
    const int d = AlphabetIndex(text[k]);
    if (d < 0) return fail("a character that isn't in a crosshair code");
    unsigned carry = static_cast<unsigned>(d);
    for (size_t i = b.size(); i-- > 0;) {
      const unsigned v = b[i] * 57u + carry;
      b[i] = static_cast<uint8_t>(v & 0xFF);
      carry = v >> 8;
    }
    if (carry) return fail("the number is too large for a crosshair code");
  }
  if (b[0] != Checksum(b)) return fail("the code's checksum doesn't match (a typo?)");
  if (b[1] != kVersion) return fail("a crosshair code version this game doesn't know yet");
  if (why) why->clear();
  return FromBytes(b);
}

std::string Encode(const Code& c) {
  std::array<uint8_t, 32> b = ToBytes(c);
  std::string body;
  body.reserve(kBodyChars);
  for (size_t k = 0; k < kBodyChars; ++k) {  // repeated division by 57, least significant digit first
    unsigned rem = 0;
    for (size_t i = 0; i < b.size(); ++i) {
      const unsigned v = (rem << 8) | b[i];
      b[i] = static_cast<uint8_t>(v / 57);
      rem = v % 57;
    }
    body.push_back(kAlphabet[rem]);
  }
  return "CS" + body;
}

std::string ToConsoleLines(const Code& c) {
  char buf[2048];
  std::snprintf(buf, sizeof(buf),
                "cl_crosshairstyle \"%u\"\ncl_crosshair_recoil \"%u\"\ncl_crosshairdot \"%u\"\ncl_crosshair_t \"%u\"\n"
                "cl_crosshaircolor_r \"%u\"\ncl_crosshaircolor_g \"%u\"\ncl_crosshaircolor_b \"%u\"\n"
                "cl_crosshaircolor_a \"%u\"\ncl_crosshairoutline_r \"%u\"\ncl_crosshairoutline_g \"%u\"\n"
                "cl_crosshairoutline_b \"%u\"\ncl_crosshairoutline_a \"%u\"\ncl_crosshair_thickness \"%u\"\n"
                "cl_crosshair_drawoutline \"%u\"\ncl_crosshair_gap \"%d\"\ncl_crosshair_length \"%u\"\n"
                "cl_crosshair_dynamic_spread_limit \"%u\"\ncl_crosshair_dynamic_splitdist \"%u\"\n"
                "cl_crosshair_dynamic_splitalpha_innermod \"%g\"\ncl_crosshair_dynamic_splitalpha_outermod \"%g\"\n"
                "cl_crosshair_dynamic_maxdist_splitratio \"%g\"\ncl_crosshair_screen_height \"%u\"\n"
                "cl_ironsight_usecrosshaircolor \"%u\"\ncl_ironsight_dot_scale \"%g\"\n",
                c.style, c.follow_recoil, c.center_dot, c.t_style, c.color[0], c.color[1], c.color[2], c.color[3],
                c.outline_color[0], c.outline_color[1], c.outline_color[2], c.outline_color[3], c.thickness,
                c.outline_mode, c.gap, c.length, c.dynamic_spread_limit, c.split_distance,
                c.inner_split_alpha / 100.0, c.outer_split_alpha / 100.0 + 0.3, c.split_size_ratio / 100.0,
                c.screen_height, c.scope_dot_use_crosshair_color, (c.scope_dot_scale + 10) / 100.0);
  return buf;
}

}  // namespace cod3::crosshair_cs2
