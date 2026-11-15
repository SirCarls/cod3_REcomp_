// cod3 - Counter-Strike 2 crosshair share codes, read and written exactly as CS2 does ([acct2-decomp] 2026-11-07;
// owner 15:23 "How can we allow for modern CS2 crosshair codes to work?", 15:29 "Ok we need this to be 1:1 with CS2").
//
// A code is "CS" + 44 characters, no dashes (CS2 since its 2026-09-30 update; format "cs2-v1"). The characters are
// one 256-bit number in base 57, least significant first, over the alphabet below (no I, l, g, 0, 1); the number is
// 32 bytes, big-endian. Byte 0 is a checksum (the sum of bytes 1..31, mod 256), byte 1 the version (1). Layout as
// the community's reference library (akiver/csgo-sharecode, doc/code_layout.md) documents it; checked against its
// example code, every field. Fields keep their raw stored values so a code read and written again is the same code.
// [acct2-ocr] 2026-11-14 copied unchanged from dev/agents/acct2-decomp/cs2-crosshair/ (acct2-decomp's, 11-07) for
// f2/f2_crosshair_cs2 (CS2's crosshair in every game's F2 window).
#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace cod3::crosshair_cs2 {

// cl_crosshairstyle, as CS2 names them.
enum Style : uint8_t {
  kDynamicCross = 0,
  kDynamicCircle = 1,
  kDynamicCrossClassic = 2,  // the old CS:GO classic dynamic, with the split
  kStaticCircle = 3,
  kStaticCross = 4,
  kStaticCrossShotFeedback = 5,
  kDotOnly = 6,
  kDynamicQuad = 7,  // CS2's default
  kStaticSquare = 8,
  kStaticQuadrant = 9,
};

// cl_crosshair_drawoutline
enum OutlineMode : uint8_t { kOutlineNone = 0, kOutlineFull = 1, kOutlineHalf = 2 };

struct Code {
  uint16_t screen_height = 0;  // cl_crosshair_screen_height: the screen height the pixel sizes are for
  uint8_t style = kDynamicQuad;
  bool follow_recoil = false;  // cl_crosshair_recoil
  bool center_dot = false;     // cl_crosshairdot
  bool t_style = false;        // cl_crosshair_t
  uint8_t color[4] = {0, 255, 0, 255};          // cl_crosshaircolor_r/_g/_b/_a
  uint8_t outline_color[4] = {0, 0, 0, 255};    // cl_crosshairoutline_r/_g/_b/_a
  uint8_t thickness = 1;                        // cl_crosshair_thickness, pixels
  uint8_t outline_mode = kOutlineNone;          // cl_crosshair_drawoutline
  int8_t gap = 0;                               // cl_crosshair_gap, pixels
  uint8_t length = 5;                           // cl_crosshair_length, pixels
  uint8_t dynamic_spread_limit = 0;             // cl_crosshair_dynamic_spread_limit
  uint8_t split_distance = 0;                   // cl_crosshair_dynamic_splitdist (7 bits)
  uint8_t inner_split_alpha = 0;                // cl_crosshair_dynamic_splitalpha_innermod = this / 100 (7 bits)
  uint8_t outer_split_alpha = 0;                // cl_crosshair_dynamic_splitalpha_outermod = this / 100 + 0.3 (7 bits)
  uint8_t split_size_ratio = 0;                 // cl_crosshair_dynamic_maxdist_splitratio = this / 100 (7 bits)
  bool scope_dot_use_crosshair_color = false;   // cl_ironsight_usecrosshaircolor
  uint8_t scope_dot_scale = 90;                 // cl_ironsight_dot_scale = (this + 10) / 100
  // Bits and bytes the format doesn't use yet, kept so a code round-trips: byte 4 bit 5, the split field's bits
  // 29-31, bytes 23-31.
  uint8_t reserved_style_bit5 = 0;
  uint8_t reserved_split_bits = 0;
  std::array<uint8_t, 9> reserved_tail{};

  float InnerSplitAlpha() const { return inner_split_alpha / 100.0f; }
  float OuterSplitAlpha() const { return outer_split_alpha / 100.0f + 0.3f; }
  float SplitSizeRatio() const { return split_size_ratio / 100.0f; }
  float ScopeDotScale() const { return (scope_dot_scale + 10) / 100.0f; }
};

// The 32 bytes <-> the fields.
std::array<uint8_t, 32> ToBytes(const Code& c);  // byte 0 (checksum) and byte 1 (version) filled in
Code FromBytes(const std::array<uint8_t, 32>& b);

// "CS..." <-> the fields. Decode accepts surrounding spaces; it fails (with a reason) on anything else: an old
// "CSGO-" code (CS2 itself no longer takes them), a wrong length or character, a bad checksum, an unknown version.
std::optional<Code> Decode(std::string_view text, std::string* why = nullptr);
std::string Encode(const Code& c);

// The console lines CS2 would set for this code (cl_crosshair... "value"), one per line, in CS2's names.
std::string ToConsoleLines(const Code& c);

}  // namespace cod3::crosshair_cs2
