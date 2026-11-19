// cod3 - Counter-Strike 2 crosshair share codes, read and written exactly as CS2 does ([acct2-decomp] 2026-11-07;
// owner 15:23 "How can we allow for modern CS2 crosshair codes to work?", 15:29 "Ok we need this to be 1:1 with CS2").
//
// [acct2-ocr] 2026-11-15 rewritten from CS2's own code (libclient.so: the decoder 0x1bc5120, the encoder 0x1bc4cc0, the
// code-to-settings step 0x1bc5a20; owner 21:11 "make the codes actually work right"). The layout before was the
// community library's, and it differed from CS2's in four places: the outline mode is in byte 13's top two bits (not
// byte 14), the gap is a 16-bit number in bytes 14-15 (not one byte at 15), the style has five bits (not four) and
// Follow recoil is bit 5 of byte 4 (not bit 4). So a CS2 code with an outline or a gap came in wrong, and ours went out
// wrong. Old "CSGO-" codes are now taken as CS2 takes them.
//
// A code is "CS" + 44 characters. The characters are one 256-bit number in base 57, least significant first, over the
// alphabet below (no I, l, g, 0, 1); the number is 32 bytes, big-endian:
//   0      checksum: the sum of bytes 1..31, mod 256
//   1      version: 1
//   2-3    cl_crosshair_screen_height, little-endian (0 is refused; under 240 becomes 240)
//   4      bits 0-4 the style (over 9 becomes 9), 5 Follow recoil, 6 the center dot, 7 the T style
//   5-8    the color, R G B A
//   9-12   the outline's color, R G B A
//   13     bits 0-5 the thickness (over 32 becomes 32), 6-7 the outline mode (over 2 becomes 2)
//   14-15  the gap, signed, little-endian, -3840 to 3840
//   16     the length; 17 the dynamic spread limit
//   18-21  little-endian: bits 0-6 the split distance, 7-13 the inner split alpha x 100 (over 100 becomes 100),
//          14-20 (the outer split alpha - 0.3) x 100 (over 70 becomes 70), 21-27 the split ratio x 100 (over 100
//          becomes 100), 28 the iron sight dot uses the crosshair's color, 29-31 unused
//   22     (the iron sight dot's scale - 0.1) x 100 (over 190 becomes 190)
//   23-31  unused
// The sizes (length, thickness, gap, the spread limit and the split distance) are whole pixels on a screen
// cl_crosshair_screen_height lines tall.
//
// An old code is "CSGO-" + five groups of five characters with dashes: 25 characters, one number in base 57 again, 18
// bytes big-endian. Byte 0 is the sum of bytes 1..15; byte 1 the version, which must be 3 or more (CS:GO's own codes,
// version 1, are refused, as CS2 refuses them). CS2 turns one into a new code (FromLegacyBytes below says how, field by
// field); the iron sight dot's two settings, which an old code doesn't carry, come from the player's own.
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
  uint8_t style = kDynamicQuad;  // 5 bits (an old code can give up to 15, as in CS2)
  bool follow_recoil = false;    // cl_crosshair_recoil
  bool center_dot = false;       // cl_crosshairdot
  bool t_style = false;          // cl_crosshair_t
  uint8_t color[4] = {0, 255, 0, 255};        // cl_crosshaircolor_r/_g/_b/_a
  uint8_t outline_color[4] = {0, 0, 0, 255};  // cl_crosshairoutline_r/_g/_b/_a
  uint8_t thickness = 1;                      // cl_crosshair_thickness, pixels (6 bits)
  uint8_t outline_mode = kOutlineNone;        // cl_crosshair_drawoutline (2 bits)
  int16_t gap = 0;                            // cl_crosshair_gap, pixels
  uint8_t length = 5;                         // cl_crosshair_length, pixels
  uint8_t dynamic_spread_limit = 0;           // cl_crosshair_dynamic_spread_limit
  uint8_t split_distance = 0;                 // cl_crosshair_dynamic_splitdist (7 bits)
  uint8_t inner_split_alpha = 0;              // cl_crosshair_dynamic_splitalpha_innermod = this / 100 (7 bits)
  uint8_t outer_split_alpha = 0;              // cl_crosshair_dynamic_splitalpha_outermod = this / 100 + 0.3 (7 bits)
  uint8_t split_size_ratio = 0;               // cl_crosshair_dynamic_maxdist_splitratio = this / 100 (7 bits)
  bool scope_dot_use_crosshair_color = false;  // cl_ironsight_usecrosshaircolor
  uint8_t scope_dot_scale = 90;                // cl_ironsight_dot_scale = this / 100 + 0.1
  // Bits and bytes the format doesn't use yet, kept so a code read and written again is the same code: the split
  // field's bits 29-31, bytes 23-31.
  uint8_t reserved_split_bits = 0;
  std::array<uint8_t, 9> reserved_tail{};
  uint8_t legacy_version = 0;  // the version of the old "CSGO-" code this came from (3 or more); 0: a "CS" code

  // The settings CS2 makes of the fields (0x1bc5a20), in CS2's float arithmetic.
  float InnerSplitAlpha() const { return float(inner_split_alpha) * 0.01f; }
  float OuterSplitAlpha() const { return float(outer_split_alpha) * 0.01f + 0.3f; }
  float SplitSizeRatio() const { return float(split_size_ratio) * 0.01f; }
  float ScopeDotScale() const { return float(scope_dot_scale) * 0.01f + 0.1f; }
};

// The settings an old code doesn't carry, which CS2 takes from the player's own (cl_ironsight_usecrosshaircolor,
// cl_ironsight_dot_scale).
struct LegacyFill {
  bool scope_dot_use_crosshair_color = false;
  float scope_dot_scale = 1.0f;
};

// The 32 bytes <-> the fields. ToBytes fills in byte 0 (the checksum) and byte 1 (the version); FromBytes takes the
// bytes as they are (Decode applies CS2's limits first).
std::array<uint8_t, 32> ToBytes(const Code& c);
Code FromBytes(const std::array<uint8_t, 32>& b);
// CS2's limits on a code read in (0x1bc5120): the screen height at least 240, the style at most 9, the thickness at
// most 32, the outline mode at most 2, the gap -3840 to 3840, the inner split alpha at most 100, the outer at most 70,
// the ratio at most 100, the iron sight dot at most 190. The checksum byte is left as it was read.
void ApplyLimits(std::array<uint8_t, 32>* b);
// An old code's 18 bytes -> the new 32 (CS2's conversion, which applies no limits).
std::array<uint8_t, 32> FromLegacyBytes(const std::array<uint8_t, 18>& old, const LegacyFill& fill);

// "CS..." or "CSGO-..." -> the fields, as CS2 reads them. Decode accepts surrounding spaces (CS2 is given the code
// alone); it fails, with a reason, on anything CS2 refuses: a wrong length, prefix or character, a number too large, a
// bad checksum, an unknown version, a screen height of 0, an old CS:GO code (version 1 or 2).
std::optional<Code> Decode(std::string_view text, std::string* why = nullptr, const LegacyFill& fill = {});
// The fields -> "CS" + 44 characters. The fields go in as they are (masked to their bits), so a code read in comes out
// the same; the limits on a player's settings are ExportCode's (f2_crosshair_cs2.cpp), as CS2's encoder's.
std::string Encode(const Code& c);

// The console lines CS2 would set for this code (cl_crosshair... "value"), one per line, in CS2's names.
std::string ToConsoleLines(const Code& c);

}  // namespace cod3::crosshair_cs2
