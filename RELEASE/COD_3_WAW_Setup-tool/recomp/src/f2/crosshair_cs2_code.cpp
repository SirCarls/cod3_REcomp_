// cod3 - Counter-Strike 2 crosshair share codes (crosshair_cs2_code.h). [acct2-decomp] 2026-11-07.
// [acct2-ocr] 2026-11-14 copied from dev/agents/acct2-decomp/cs2-crosshair/ for f2/f2_crosshair_cs2; 2026-11-15
// rewritten from CS2's own code (libclient.so 0x1bc5120, 0x1bc4cc0, 0x1bc5a20): the layout, the limits, old codes.
#include "crosshair_cs2_code.h"

#include <algorithm>
#include <cmath>
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

uint8_t Checksum(const uint8_t* b, size_t from, size_t to) {  // the sum of b[from..to), mod 256
  unsigned sum = 0;
  for (size_t i = from; i < to; ++i) sum += b[i];
  return static_cast<uint8_t>(sum & 0xFF);
}

// The characters (most significant last) -> N bytes, big-endian; false if the number doesn't fit or a character isn't
// in the alphabet (CS2: strchr on the alphabet, then the carry left over).
template <size_t N>
bool FromBase57(std::string_view chars, std::array<uint8_t, N>* out, const char** why) {
  std::array<uint8_t, N> b{};
  for (size_t k = chars.size(); k-- > 0;) {
    const int d = AlphabetIndex(chars[k]);
    if (d < 0) {
      *why = "a character that isn't in a crosshair code";
      return false;
    }
    unsigned carry = static_cast<unsigned>(d);
    for (size_t i = N; i-- > 0;) {
      const unsigned v = b[i] * 57u + carry;
      b[i] = static_cast<uint8_t>(v & 0xFF);
      carry = v >> 8;
    }
    if (carry) {
      *why = "the number is too large for a crosshair code";
      return false;
    }
  }
  *out = b;
  return true;
}

// CS2 rounds with V_roundf (tier0); round half away from zero, as roundf. The arguments are formed in float, as CS2's.
int RoundF(float v) { return static_cast<int>(std::roundf(v)); }

}  // namespace

std::array<uint8_t, 32> ToBytes(const Code& c) {
  std::array<uint8_t, 32> b{};
  b[1] = kVersion;
  b[2] = static_cast<uint8_t>(c.screen_height & 0xFF);
  b[3] = static_cast<uint8_t>(c.screen_height >> 8);
  b[4] = static_cast<uint8_t>((c.style & 0x1F) | (c.follow_recoil ? 0x20 : 0) | (c.center_dot ? 0x40 : 0) |
                              (c.t_style ? 0x80 : 0));
  for (int i = 0; i < 4; ++i) b[5 + i] = c.color[i];
  for (int i = 0; i < 4; ++i) b[9 + i] = c.outline_color[i];
  b[13] = static_cast<uint8_t>((c.thickness & 0x3F) | ((c.outline_mode & 0x3) << 6));
  const uint16_t gap = static_cast<uint16_t>(c.gap);
  b[14] = static_cast<uint8_t>(gap & 0xFF);
  b[15] = static_cast<uint8_t>(gap >> 8);
  b[16] = c.length;
  b[17] = c.dynamic_spread_limit;
  const uint32_t split = (uint32_t(c.split_distance & 0x7F)) | (uint32_t(c.inner_split_alpha & 0x7F) << 7) |
                         (uint32_t(c.outer_split_alpha & 0x7F) << 14) | (uint32_t(c.split_size_ratio & 0x7F) << 21) |
                         (uint32_t(c.scope_dot_use_crosshair_color ? 1 : 0) << 28) |
                         (uint32_t(c.reserved_split_bits & 0x7) << 29);
  for (int i = 0; i < 4; ++i) b[18 + i] = static_cast<uint8_t>(split >> (8 * i));
  b[22] = c.scope_dot_scale;
  for (size_t i = 0; i < c.reserved_tail.size(); ++i) b[23 + i] = c.reserved_tail[i];
  b[0] = Checksum(b.data(), 1, b.size());
  return b;
}

Code FromBytes(const std::array<uint8_t, 32>& b) {
  Code c;
  c.screen_height = static_cast<uint16_t>(b[2] | (b[3] << 8));
  c.style = b[4] & 0x1F;
  c.follow_recoil = (b[4] >> 5) & 1;
  c.center_dot = (b[4] >> 6) & 1;
  c.t_style = (b[4] >> 7) & 1;
  for (int i = 0; i < 4; ++i) c.color[i] = b[5 + i];
  for (int i = 0; i < 4; ++i) c.outline_color[i] = b[9 + i];
  c.thickness = b[13] & 0x3F;
  c.outline_mode = b[13] >> 6;
  c.gap = static_cast<int16_t>(static_cast<uint16_t>(b[14] | (b[15] << 8)));
  c.length = b[16];
  c.dynamic_spread_limit = b[17];
  uint32_t split = 0;
  for (int i = 0; i < 4; ++i) split |= uint32_t(b[18 + i]) << (8 * i);
  c.split_distance = split & 0x7F;
  c.inner_split_alpha = (split >> 7) & 0x7F;
  c.outer_split_alpha = (split >> 14) & 0x7F;
  c.split_size_ratio = (split >> 21) & 0x7F;
  c.scope_dot_use_crosshair_color = ((split >> 28) & 1) != 0;
  c.reserved_split_bits = static_cast<uint8_t>((split >> 29) & 0x7);
  c.scope_dot_scale = b[22];
  for (size_t i = 0; i < c.reserved_tail.size(); ++i) c.reserved_tail[i] = b[23 + i];
  return c;
}

void ApplyLimits(std::array<uint8_t, 32>* bytes) {
  auto& b = *bytes;
  const uint16_t h = std::max<uint16_t>(static_cast<uint16_t>(b[2] | (b[3] << 8)), 240);
  b[2] = static_cast<uint8_t>(h & 0xFF);
  b[3] = static_cast<uint8_t>(h >> 8);
  b[4] = static_cast<uint8_t>((b[4] & 0xE0) | std::min(b[4] & 0x1F, 9));
  b[13] = static_cast<uint8_t>((std::min(b[13] >> 6, 2) << 6) | std::min(b[13] & 0x3F, 32));
  const int gap = std::clamp<int>(static_cast<int16_t>(static_cast<uint16_t>(b[14] | (b[15] << 8))), -3840, 3840);
  b[14] = static_cast<uint8_t>(static_cast<uint16_t>(gap) & 0xFF);
  b[15] = static_cast<uint8_t>(static_cast<uint16_t>(gap) >> 8);
  uint32_t split = 0;
  for (int i = 0; i < 4; ++i) split |= uint32_t(b[18 + i]) << (8 * i);
  const uint32_t inner = std::min<uint32_t>((split >> 7) & 0x7F, 100);
  const uint32_t outer = std::min<uint32_t>((split >> 14) & 0x7F, 70);
  const uint32_t ratio = std::min<uint32_t>((split >> 21) & 0x7F, 100);
  split = (split & 0xF000007Fu) | (inner << 7) | (outer << 14) | (ratio << 21);
  for (int i = 0; i < 4; ++i) b[18 + i] = static_cast<uint8_t>(split >> (8 * i));
  b[22] = std::min<uint8_t>(b[22], 190);
}

std::array<uint8_t, 32> FromLegacyBytes(const std::array<uint8_t, 18>& o, const LegacyFill& fill) {
  // CS2 0x1bc5633..0x1bc5858, into a zeroed buffer. Old byte -> new field:
  //   1 the version (3: the outline is on or off, in byte 2's bit 5; 4 and up: the mode in byte 13's bits 4-5)
  //   2 bits 0-3 the style, 4 Follow recoil, 5 the outline (version 3), 6 the center dot, 7 the T style
  //   3-6 the color; the outline is black with the color's alpha
  //   7 the gap (0-255, as CS2 reads it); 8 the length; 9 the dynamic spread limit
  //   10 bits 0-6 the split distance; 10 bit 7 and 11 bits 0-3 the inner split alpha in 20ths; 11 bits 4-7 the
  //      outer's, above 0.3, in 20ths; 12 bits 0-6 the split ratio x 100; 12 bit 7 and 13 bits 0-3 the thickness
  //   14-15 the screen height
  std::array<uint8_t, 32> b{};
  const int outline = o[1] == 3 ? (o[2] >> 5) & 1 : (o[13] >> 4) & 3;
  b[2] = o[14];
  b[3] = o[15];
  b[4] = static_cast<uint8_t>((o[2] & 0xCF) | ((o[2] << 1) & 0x20));
  for (int i = 0; i < 4; ++i) b[5 + i] = o[3 + i];
  b[12] = o[6];  // the outline's alpha; its color stays black
  const int thickness = ((o[12] | (o[13] << 8)) >> 7) & 0x1F;
  b[13] = static_cast<uint8_t>((outline << 6) | thickness);
  b[14] = o[7];
  b[16] = o[8];
  b[17] = o[9];
  const int inner20 = ((o[10] | (o[11] << 8)) >> 7) & 0x1F;
  const int outer20 = o[11] >> 4;
  const uint32_t inner = uint32_t(RoundF(float(inner20) * 0.05f / 0.01f)) & 0x7F;
  const uint32_t outer = uint32_t(RoundF(float(outer20) * 0.05f / 0.01f)) & 0x7F;
  const uint32_t split = uint32_t(o[10] & 0x7F) | (inner << 7) | (outer << 14) | (uint32_t(o[12] & 0x7F) << 21) |
                         (uint32_t(fill.scope_dot_use_crosshair_color ? 1 : 0) << 28);
  for (int i = 0; i < 4; ++i) b[18 + i] = static_cast<uint8_t>(split >> (8 * i));
  const float scale = std::min(std::max(fill.scope_dot_scale, 0.1f), 2.0f);
  b[22] = static_cast<uint8_t>(RoundF((scale - 0.1f) / 0.01f));
  b[1] = kVersion;
  b[0] = Checksum(b.data(), 1, b.size());
  return b;
}

std::optional<Code> Decode(std::string_view text, std::string* why, const LegacyFill& fill) {
  const char* reason = nullptr;
  auto fail = [&](const char* r) -> std::optional<Code> {
    if (why) *why = r;
    return std::nullopt;
  };
  while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' || text.front() == '\n'))
    text.remove_prefix(1);
  while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' || text.back() == '\n'))
    text.remove_suffix(1);
  if (text.empty()) return fail("no code");

  // An old code: "CSGO-" and five groups of five characters (CS2 tries this first for 34 characters; when it fails,
  // the code is refused).
  if (text.size() == 34 && text.substr(0, 5) == "CSGO-") {
    static constexpr size_t kDashes[] = {10, 16, 22, 28}, kGroups[] = {5, 11, 17, 23, 29};
    for (size_t at : kDashes)
      if (text[at] != '-') return fail("an old CSGO- code has dashes between groups of five characters");
    std::string chars;
    for (size_t at : kGroups) chars.append(text.substr(at, 5));
    std::array<uint8_t, 18> old{};
    if (!FromBase57(chars, &old, &reason)) return fail(reason);
    if (old[0] != Checksum(old.data(), 1, 16)) return fail("the code's checksum doesn't match (a typo?)");
    if (old[1] <= 2)
      return fail("a CS:GO crosshair code: CS2 doesn't take these (it takes old CSGO- codes made in CS2)");
    Code c = FromBytes(FromLegacyBytes(old, fill));
    c.legacy_version = old[1];
    if (why) why->clear();
    return c;
  }
  if (text.size() == 34 && text.substr(0, 4) == "CSGO") return fail("an old crosshair code starts with CSGO-");

  if (text.size() != 2 + kBodyChars) return fail("a CS2 crosshair code is CS and 44 more characters");
  if (text.substr(0, 2) != "CS") return fail("a CS2 crosshair code starts with CS");
  std::array<uint8_t, 32> b{};
  if (!FromBase57(text.substr(2), &b, &reason)) return fail(reason);
  if (b[1] != kVersion) return fail("a crosshair code version this game doesn't know yet");
  if ((b[2] | b[3]) == 0) return fail("the code has no screen height");
  if (b[0] != Checksum(b.data(), 1, b.size())) return fail("the code's checksum doesn't match (a typo?)");
  ApplyLimits(&b);
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
                c.outline_mode, int(c.gap), c.length, c.dynamic_spread_limit, c.split_distance,
                double(c.InnerSplitAlpha()), double(c.OuterSplitAlpha()), double(c.SplitSizeRatio()),
                c.screen_height, c.scope_dot_use_crosshair_color, double(c.ScopeDotScale()));
  return buf;
}

}  // namespace cod3::crosshair_cs2
