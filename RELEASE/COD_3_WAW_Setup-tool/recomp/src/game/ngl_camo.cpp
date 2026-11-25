// [acct2-bro] 2026-11-22 cod3 - the camo layer's files, textures and choices (Camo lane). What and why: ngl_camo.h.
//
// The .c3camo file is c3w/camo_pack.py's (version 1, little endian): a 64-byte header ("C3CAMO1\0", version, header
// bytes, map count, flags, scroll u, scroll v, scale, reserved, data bytes, data crc32, 3 reserved, header crc32 of
// bytes 0-59 and the table), a table of 32 bytes a map (kind, format, width, height, levels, offset, bytes, reserved)
// and the maps' levels, largest first, packed. ParseCamo keeps the tool's read_camo rules exactly.
// [acct2-bro] 2026-11-23 c3camo v2: the same file with version 2 allows format 5, BC7 blocks as stored (a file is
// version 2 only when a map is BC7; a newer version is refused by name). A BC7 map goes to the GPU as its blocks
// (ngl_vk_texture_blocks, when ngl_vk has it and the device has BC textures on, sampled as UNORM like the RGBA8
// path), else it is decoded here (DecodeBc7Block, a total function on 16-byte blocks) and made as RGBA8 as before.
//
// A map is used as RGBA8 at most kSide (1024) on a side with a full mip chain: the reading thread decodes BC1-BC3,
// halves the picture until it fits (a box filter: the gun's pattern doesn't need more, and many pack maps are 4096 with
// one level) and makes every smaller level. The drawing thread turns one waiting map a frame into an ngl_vk texture.
// ngl_vk can't free one texture, so at most kMostTextures are made between two Forget()s (a new level); past that a
// new choice draws no camo until the next level (logged once).

#include "game/ngl_camo.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <thread>
#include <unordered_map>
#include <utility>

#if defined(NGL_CAMO_TEST)
// The test program (x503/camo503_test.cpp) builds this file alone: no cvars, logging or ngl_vk.
#include "camo503_stubs.h"
#if defined(NGL_CAMO_TEST_BLOCKS)
// [acct2-bro] 2026-11-23 c3camo v2: the BC7 test program's stand-ins (the older test programs build without them)
#include "camo_v2_stubs.h"
#endif
#else
#include <rex/cvar.h>
#include <rex/logging.h>
#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
#if defined(COD3_NGL_VK)
extern "C" {
#include "ngl_vk.h"
}
#endif
#endif

#if !defined(NGL_CAMO_TEST)
REXCVAR_DEFINE_BOOL(cod3_camo, true, "cod3/Display",
                    "With cod3_native_vulkan, the camos chosen in F2 (WEAPONS > Camos) on the first-person guns. Off: "
                    "every gun exactly as the game draws it (the kill switch)");
REXCVAR_DEFINE_STRING(cod3_camo_choices, "", "cod3/Display",
                      "The camo of each player's guns, F2's choices: 'p1 springfield=camo_0123456789ab; p1 *=...' "
                      "(player 1-4, the gun as its own textures name it, or * for every gun, a camo of "
                      "data/cod3/camos/index.tsv)");  // [acct2-bro] 2026-11-22 c519: gun names from textures
REXCVAR_DEFINE_INT32(cod3_camo_strength, 100, "cod3/Display",
                     "How strongly a chosen camo covers the gun, in percent (100 = fully, as in Black Ops)")
    .range(0, 100);
// [acct2-bro] 2026-11-22 x509: moving camos (owner 03:18 "camos that move/crawl"). Black Ops 1 sets its moving
// Pack-a-Punch material's uvScroll at run time (the zone's literal is 0 0 0 1, pap507), so the pack carries no speed:
// the importer gives moving sets a default and this scales it.
REXCVAR_DEFINE_INT32(cod3_camo_crawl, 100, "cod3/Display",
                     "How fast moving camos crawl, in percent of their own speed (0 = still, 100 = as imported)")
    .range(0, 400);
// [acct2-bro] 2026-11-23 c3camo v2: a debug switch (not in F2): BC7 camos decoded on the CPU even where the GPU
// takes their blocks, to compare the two (the pictures must match)
REXCVAR_DEFINE_BOOL(cod3_camo_cpu_blocks, false, "cod3/Debug",
                    "Debug: BC7 camos decoded on the CPU and made as RGBA8 even when the GPU could take their blocks");
#endif

namespace cod3::ngl_camo {
namespace {

// ---- limits (the tool's, camo_pack.py; the game checks them again) ------------------------------------------------
constexpr uint32_t kMaxSide = 4096;
constexpr uint64_t kMaxMapBytes = 16ull << 20;
constexpr uint64_t kMaxFileBytes = 64ull << 20;
constexpr float kDarkestMean = 4.0f / 255.0f;  // [acct2-bro] 2026-11-22 c519: a colour map darker on average is refused
constexpr uint32_t kMaxLevels = 13;
constexpr uint32_t kSide = 1024;           // the size a map is used at, at most
constexpr uint32_t kMostTextures = 48;     // textures made between two Forget()s (16 camos of 3 maps)
constexpr float kScrollLimit = 16.0f, kScaleMin = 1.0f / 16.0f, kScaleMax = 64.0f;
constexpr uint32_t kHeader = 64, kEntry = 32;
enum : uint32_t { kColor = 1, kSpecular = 2, kEnv = 3 };
enum : uint32_t { kRgba8 = 1, kBc1 = 2, kBc2 = 3, kBc3 = 4, kBc7 = 5 };  // kBc7: c3camo v2 (version 2 only)
constexpr uint32_t kVersion = 1, kVersionBc7 = 2;                       // [acct2-bro] 2026-11-23 c3camo v2

uint32_t Crc32(const uint8_t* p, size_t n, uint32_t crc = 0) {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[i] = c;
    }
    return t;
  }();
  crc = ~crc;
  for (size_t i = 0; i < n; ++i) crc = table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

uint32_t U32(const uint8_t* p) {
  return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24;
}
float F32(const uint8_t* p) {
  const uint32_t u = U32(p);
  float f;
  std::memcpy(&f, &u, 4);
  return f;
}

uint64_t LevelBytes(uint32_t fmt, uint32_t w, uint32_t h) {
  const uint64_t bw = std::max<uint32_t>(1, (w + 3) / 4), bh = std::max<uint32_t>(1, (h + 3) / 4);
  switch (fmt) {
    case kRgba8: return uint64_t(w) * h * 4;
    case kBc1: return bw * bh * 8;
    case kBc2:
    case kBc3:
    case kBc7: return bw * bh * 16;
    default: return 0;
  }
}
uint32_t FullLevels(uint32_t w, uint32_t h) {
  uint32_t n = 1;
  for (uint32_t s = std::max(w, h); s > 1; s >>= 1) ++n;
  return n;
}

struct MapView {        // one map of a checked file: its largest level's bytes stay inside the file's buffer
  uint32_t kind = 0, fmt = 0, width = 0, height = 0;
  const uint8_t* base = nullptr;  // the largest level
  uint64_t base_bytes = 0;
  uint32_t levels = 1;            // [acct2-bro] 2026-11-23 c3camo v2: the levels stored, packed after base
};
struct CamoView {
  std::vector<MapView> maps;
  float scroll_u = 0, scroll_v = 0, scale = 1;
};

}  // namespace

// The tool's read_camo, rule for rule. Exposed (not in the header) for the test program.
bool ParseCamo(const uint8_t* d, size_t size, CamoView* out, std::string* why) {
  auto fail = [&](const char* w) { if (why) *why = w; return false; };
  if (size < kHeader || std::memcmp(d, "C3CAMO1\0", 8) != 0) return fail("not a camo file");
  const uint32_t version = U32(d + 8), head = U32(d + 12), count = U32(d + 16), flags = U32(d + 20);
  const float su = F32(d + 24), sv = F32(d + 28), scale = F32(d + 32);
  const uint32_t data_bytes = U32(d + 40), data_crc = U32(d + 44), head_crc = U32(d + 60);
  if (version > kVersionBc7 && head == kHeader) return fail("a newer camo format");  // c3camo v2
  if ((version != kVersion && version != kVersionBc7) || head != kHeader || flags != 0 || U32(d + 48) ||
      U32(d + 52) || U32(d + 56))
    return fail("a camo file of another version");
  if (count < 1 || count > 3) return fail("a wrong map count");
  const uint64_t table_end = kHeader + uint64_t(kEntry) * count;
  if (size < table_end) return fail("cut off in its table");
  uint32_t crc = Crc32(d, 60);
  crc = Crc32(d + kHeader, size_t(table_end - kHeader), crc);
  if (crc != head_crc) return fail("its header checksum is wrong");
  if (size - table_end != data_bytes) return fail("its size doesn't match its header");
  if (!std::isfinite(su) || !std::isfinite(sv) || !std::isfinite(scale)) return fail("a crawl setting isn't a number");
  if (std::fabs(su) > kScrollLimit || std::fabs(sv) > kScrollLimit) return fail("a crawl speed over the limit");
  if (!(scale >= kScaleMin && scale <= kScaleMax)) return fail("a scale out of range");
  CamoView v;
  v.scroll_u = su, v.scroll_v = sv, v.scale = scale;
  uint64_t expect = 0;
  uint32_t kinds = 0;
  for (uint32_t i = 0; i < count; ++i) {
    const uint8_t* e = d + kHeader + kEntry * i;
    const uint32_t kind = U32(e), fmt = U32(e + 4), w = U32(e + 8), h = U32(e + 12), levels = U32(e + 16);
    const uint32_t offset = U32(e + 20), bytes = U32(e + 24);
    if (kind < kColor || kind > kEnv || (kinds & (1u << kind)) || fmt < kRgba8 || fmt > kBc7 || U32(e + 28) ||
        (fmt == kBc7 && version != kVersionBc7))  // c3camo v2: BC7 only in a version 2 file
      return fail("a map of an unknown kind or format");
    kinds |= 1u << kind;
    if (w < 1 || w > kMaxSide || h < 1 || h > kMaxSide || LevelBytes(fmt, w, h) > kMaxMapBytes)
      return fail("a map over the caps");
    if (levels < 1 || levels > std::min(kMaxLevels, FullLevels(w, h))) return fail("a wrong number of levels");
    uint64_t chain = 0;
    for (uint32_t l = 0; l < levels; ++l) chain += LevelBytes(fmt, std::max(1u, w >> l), std::max(1u, h >> l));
    if (offset != expect || bytes != chain || uint64_t(offset) + bytes > data_bytes)
      return fail("a map's place in the file is wrong");
    expect += bytes;
    MapView m;
    m.kind = kind, m.fmt = fmt, m.width = w, m.height = h;
    m.base = d + table_end + offset;
    m.base_bytes = LevelBytes(fmt, w, h);
    m.levels = levels;
    v.maps.push_back(m);
  }
  if (expect != data_bytes) return fail("bytes after the last map");
  if (Crc32(d + table_end, data_bytes) != data_crc) return fail("its data checksum is wrong");
  *out = std::move(v);
  return true;
}

// ---- [acct2-bro] 2026-11-23 c3camo v2: one BC7 block (16 bytes) to RGBA8, for the CPU fallback -------------------
// A total function on any 16 bytes, as BC1-BC3 are: every field is read with its own bit width from the block's
// own 128 bits (a read past bit 128 gives 0), every table index is masked to the table's size, nothing is
// allocated, and the reserved mode (a first byte of 0) gives transparent black, as the format's specification asks.
// The tables and bit layout are the public BC7 format's (Direct3D 11 "BC7 Format Mode Reference"); c3w/bc7.py
// has the same, checked against Pillow's decoder, and the test program checks this one against bc7.py's values.
namespace bc7 {
constexpr uint8_t kWeights2[4] = {0, 21, 43, 64};
constexpr uint8_t kWeights3[8] = {0, 9, 18, 27, 37, 46, 55, 64};
constexpr uint8_t kWeights4[16] = {0, 4, 9, 13, 17, 21, 26, 30, 34, 38, 43, 47, 51, 55, 60, 64};
// mode: subsets, partition bits, rotation bits, index selection bits, colour bits, alpha bits, endpoint P-bits,
// shared P-bits, index bits, second index bits
struct Mode { uint8_t ns, pb, rb, isb, cb, ab, epb, spb, ib, ib2; };
constexpr Mode kModes[8] = {
    {3, 4, 0, 0, 4, 0, 1, 0, 3, 0}, {2, 6, 0, 0, 6, 0, 0, 1, 3, 0}, {3, 6, 0, 0, 5, 0, 0, 0, 2, 0},
    {2, 6, 0, 0, 7, 0, 1, 0, 2, 0}, {1, 0, 2, 1, 5, 6, 0, 0, 2, 3}, {1, 0, 2, 0, 7, 8, 0, 0, 2, 2},
    {1, 0, 0, 0, 7, 7, 1, 0, 4, 0}, {2, 6, 0, 0, 5, 5, 1, 0, 2, 0}};
constexpr uint16_t kPart2[64] = {
    0xCCCC, 0x8888, 0xEEEE, 0xECC8, 0xC880, 0xFEEC, 0xFEC8, 0xEC80, 0xC800, 0xFFEC, 0xFE80, 0xE800, 0xFFE8,
    0xFF00, 0xFFF0, 0xF000, 0xF710, 0x008E, 0x7100, 0x08CE, 0x008C, 0x7310, 0x3100, 0x8CCE, 0x088C, 0x3110,
    0x6666, 0x366C, 0x17E8, 0x0FF0, 0x718E, 0x399C, 0xAAAA, 0xF0F0, 0x5A5A, 0x33CC, 0x3C3C, 0x55AA, 0x9696,
    0xA55A, 0x73CE, 0x13C8, 0x324C, 0x3BDC, 0x6996, 0xC33C, 0x9966, 0x0660, 0x0272, 0x04E4, 0x4E40, 0x2720,
    0xC936, 0x936C, 0x39C6, 0x639C, 0x9336, 0x9CC6, 0x817E, 0xE718, 0xCCF0, 0x0FCC, 0x7744, 0xEE22};
constexpr const char* kPart3[64] = {
    "0011001102212222", "0001001122112221", "0000200122112211", "0222002200110111", "0000000011221122",
    "0011001100220022", "0022002211111111", "0011001122112211", "0000000011112222", "0000111111112222",
    "0000111122222222", "0012001200120012", "0112011201120112", "0122012201220122", "0011011211221222",
    "0011200122002220", "0001001101121122", "0111001120012200", "0000112211221122", "0022002200221111",
    "0111011102220222", "0001000122212221", "0000001101220122", "0000110022102210", "0122012200110000",
    "0012001211222222", "0110122112210110", "0000011012211221", "0022110211020022", "0110011020022222",
    "0011012201220011", "0000200022112221", "0000000211221222", "0222002200120011", "0011001200220222",
    "0120012001200120", "0000111122220000", "0120120120120120", "0120201212010120", "0011220011220011",
    "0011112222000011", "0101010122222222", "0000000021212121", "0022112200221122", "0022001100220011",
    "0220122102201221", "0101222222220101", "0000212121212121", "0101010101012222", "0222011102220111",
    "0002111200021112", "0000211221122112", "0222011101110222", "0002111211120002", "0110011001102222",
    "0000000021122112", "0110011022222222", "0022001100110022", "0022112211220022", "0000000000002112",
    "0002000100020001", "0222122202221222", "0101222222222222", "0111201122012220"};
constexpr uint8_t kAnchor2[64] = {15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 15,
                                  15, 2,  8,  2,  2,  8,  8,  15, 2,  8,  2,  2,  8,  8,  2,  2,
                                  15, 15, 6,  8,  2,  8,  15, 15, 2,  8,  2,  2,  2,  15, 15, 6,
                                  6,  2,  6,  8,  15, 15, 2,  2,  15, 15, 15, 15, 15, 2,  2,  15};
constexpr uint8_t kAnchor3a[64] = {3, 3,  15, 15, 8, 3,  15, 15, 8,  8,  6,  6,  6,  5,  3, 3,
                                   3, 3,  8,  15, 3, 3,  6,  10, 5,  8,  8,  6,  8,  5,  15, 15,
                                   8, 15, 3,  5,  6, 10, 8,  15, 15, 3,  15, 5,  15, 15, 15, 15,
                                   3, 15, 5,  5,  5, 8,  5,  10, 5,  10, 8,  13, 15, 12, 3, 3};
constexpr uint8_t kAnchor3b[64] = {15, 8,  8,  3,  15, 15, 3,  8,  15, 15, 15, 15, 15, 15, 15, 8,
                                   15, 8,  15, 3,  15, 8,  15, 8,  3,  15, 6,  10, 15, 15, 10, 8,
                                   15, 3,  15, 10, 10, 8,  9,  10, 6,  15, 8,  15, 3,  6,  6,  8,
                                   15, 3,  15, 15, 15, 15, 15, 15, 15, 15, 15, 15, 3,  15, 15, 8};

struct Bits {  // the block's 128 bits, read low to high
  uint64_t lo = 0, hi = 0;
  uint32_t pos = 0;
  uint32_t Take(uint32_t n) {  // n <= 8
    uint32_t v = 0;
    if (n && pos < 128) {
      const uint64_t mask = (uint64_t(1) << n) - 1;
      if (pos >= 64) v = uint32_t((hi >> (pos - 64)) & mask);
      else if (pos + n <= 64) v = uint32_t((lo >> pos) & mask);
      else v = uint32_t(((lo >> pos) | (hi << (64 - pos))) & mask);
    }
    pos += n;
    return v;
  }
};
inline uint32_t Expand(uint32_t v, uint32_t n) {  // n bits (1..8) to 8
  v = (v << (8 - n)) & 0xFF;
  return v | (v >> n);
}
inline const uint8_t* Weights(uint32_t bits) { return bits == 2 ? kWeights2 : bits == 3 ? kWeights3 : kWeights4; }
}  // namespace bc7

// One block to 16 pixels, R G B A, row by row; true when the block was of the reserved mode (all zero then).
bool DecodeBc7Block(const uint8_t* blk, uint8_t px[16][4]) {
  bc7::Bits b;
  for (int i = 7; i >= 0; --i) b.lo = (b.lo << 8) | blk[i], b.hi = (b.hi << 8) | blk[8 + i];
  const uint32_t first = blk[0];
  if (first == 0) {
    std::memset(px, 0, 16 * 4);
    return true;
  }
  uint32_t mode = 0;
  while (!((first >> mode) & 1)) ++mode;  // first != 0: mode <= 7
  const bc7::Mode& m = bc7::kModes[mode & 7];
  b.pos = mode + 1;
  const uint32_t part = b.Take(m.pb) & 63, rot = b.Take(m.rb) & 3, sel = b.Take(m.isb) & 1;
  const uint32_t ne = 2u * m.ns;  // 2, 4 or 6 endpoints
  uint32_t ep[6][4] = {};
  for (uint32_t c = 0; c < 3; ++c)
    for (uint32_t e = 0; e < ne; ++e) ep[e][c] = b.Take(m.cb);
  for (uint32_t e = 0; e < ne; ++e) ep[e][3] = m.ab ? b.Take(m.ab) : 255;
  uint32_t cbits = m.cb, abits = m.ab;
  if (m.epb) {
    for (uint32_t e = 0; e < ne; ++e) {
      const uint32_t p = b.Take(1);
      for (uint32_t c = 0; c < (m.ab ? 4u : 3u); ++c) ep[e][c] = (ep[e][c] << 1) | p;
    }
    ++cbits;
    if (m.ab) ++abits;
  } else if (m.spb) {
    for (uint32_t s = 0; s < m.ns; ++s) {
      const uint32_t p = b.Take(1);
      for (uint32_t e = 2 * s; e < 2 * s + 2; ++e)
        for (uint32_t c = 0; c < 3; ++c) ep[e][c] = (ep[e][c] << 1) | p;
    }
    ++cbits;
  }
  for (uint32_t e = 0; e < ne; ++e) {
    for (uint32_t c = 0; c < 3; ++c) ep[e][c] = bc7::Expand(ep[e][c], cbits);
    ep[e][3] = m.ab ? bc7::Expand(ep[e][3], abits) : 255;
  }
  uint32_t subset[16] = {};
  uint32_t anchor1 = 16, anchor2 = 16;  // 16: none
  if (m.ns == 2) {
    for (uint32_t i = 0; i < 16; ++i) subset[i] = (bc7::kPart2[part] >> i) & 1;
    anchor1 = bc7::kAnchor2[part] & 15;
  } else if (m.ns == 3) {
    for (uint32_t i = 0; i < 16; ++i) subset[i] = uint32_t(bc7::kPart3[part][i] - '0') % 3;
    anchor1 = bc7::kAnchor3a[part] & 15, anchor2 = bc7::kAnchor3b[part] & 15;
  }
  uint32_t idx[16], idx2[16] = {};
  for (uint32_t i = 0; i < 16; ++i) {
    const bool anchor = i == 0 || i == anchor1 || i == anchor2;
    idx[i] = b.Take(anchor ? m.ib - 1u : m.ib) & ((1u << m.ib) - 1);
  }
  if (m.ib2)
    for (uint32_t i = 0; i < 16; ++i) idx2[i] = b.Take(i == 0 ? m.ib2 - 1u : m.ib2) & ((1u << m.ib2) - 1);
  const uint8_t* w1 = bc7::Weights(m.ib);
  const uint8_t* w2 = bc7::Weights(m.ib2 ? m.ib2 : 2);
  for (uint32_t i = 0; i < 16; ++i) {
    const uint32_t s = subset[i] % m.ns;
    const uint32_t* e0 = ep[2 * s];
    const uint32_t* e1 = ep[2 * s + 1];
    uint32_t wc, wa;
    if (!m.ib2) {
      wc = wa = w1[idx[i]];
    } else if (sel) {  // mode 4 with the selection bit: colour by the 3-bit indices, alpha by the 2-bit ones
      wc = w2[idx2[i]], wa = w1[idx[i]];
    } else {
      wc = w1[idx[i]], wa = w2[idx2[i]];
    }
    uint8_t out[4];
    for (uint32_t c = 0; c < 3; ++c) out[c] = uint8_t(((64 - wc) * e0[c] + wc * e1[c] + 32) >> 6);
    out[3] = uint8_t(((64 - wa) * e0[3] + wa * e1[3] + 32) >> 6);
    if (rot) std::swap(out[3], out[rot - 1]);
    std::memcpy(px[i], out, 4);
  }
  return false;
}

namespace {

// ---- decoding: BC1/BC2/BC3 (any bit pattern is a valid block) to RGBA8 ----------------------------------------------
void Color565(uint16_t c, uint8_t out[3]) {
  out[0] = uint8_t(((c >> 11) & 31) * 255 / 31);
  out[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
  out[2] = uint8_t((c & 31) * 255 / 31);
}
void ColorBlock(const uint8_t* b, bool bc1, uint8_t px[16][4]) {
  const uint16_t c0 = uint16_t(b[0] | b[1] << 8), c1 = uint16_t(b[2] | b[3] << 8);
  uint8_t pal[4][4] = {};
  Color565(c0, pal[0]);
  Color565(c1, pal[1]);
  pal[0][3] = pal[1][3] = pal[2][3] = 255;
  pal[3][3] = 255;
  for (int k = 0; k < 3; ++k) {
    if (!bc1 || c0 > c1) {
      pal[2][k] = uint8_t((2 * pal[0][k] + pal[1][k]) / 3);
      pal[3][k] = uint8_t((pal[0][k] + 2 * pal[1][k]) / 3);
    } else {
      pal[2][k] = uint8_t((pal[0][k] + pal[1][k]) / 2);
      pal[3][k] = 0;
    }
  }
  if (bc1 && c0 <= c1) pal[3][3] = 0;  // BC1's transparent black
  const uint32_t idx = U32(b + 4);
  for (int i = 0; i < 16; ++i) std::memcpy(px[i], pal[(idx >> (2 * i)) & 3], 4);
}
void AlphaBc3(const uint8_t* b, uint8_t px[16][4]) {
  uint8_t a[8] = {b[0], b[1]};
  if (a[0] > a[1]) {
    for (int i = 1; i < 7; ++i) a[i + 1] = uint8_t(((7 - i) * a[0] + i * a[1]) / 7);
  } else {
    for (int i = 1; i < 5; ++i) a[i + 1] = uint8_t(((5 - i) * a[0] + i * a[1]) / 5);
    a[6] = 0, a[7] = 255;
  }
  uint64_t bits = 0;
  for (int i = 0; i < 6; ++i) bits |= uint64_t(b[2 + i]) << (8 * i);
  for (int i = 0; i < 16; ++i) px[i][3] = a[(bits >> (3 * i)) & 7];
}
void AlphaBc2(const uint8_t* b, uint8_t px[16][4]) {
  for (int i = 0; i < 16; ++i) px[i][3] = uint8_t(((b[i / 2] >> (4 * (i & 1))) & 15) * 17);
}

// The largest level of a map as RGBA8 (w x h x 4).
std::vector<uint8_t> DecodeBase(const MapView& m) {
  const uint32_t w = m.width, h = m.height;
  std::vector<uint8_t> out(size_t(w) * h * 4);
  if (m.fmt == kRgba8) {
    std::memcpy(out.data(), m.base, out.size());
    return out;
  }
  const uint32_t bw = std::max(1u, (w + 3) / 4), bh = std::max(1u, (h + 3) / 4);
  const uint32_t step = m.fmt == kBc1 ? 8 : 16;
  uint8_t px[16][4];
  for (uint32_t by = 0; by < bh; ++by) {
    for (uint32_t bx = 0; bx < bw; ++bx) {
      const uint8_t* blk = m.base + (size_t(by) * bw + bx) * step;
      if (m.fmt == kBc7) {  // c3camo v2
        DecodeBc7Block(blk, px);
      } else {
        ColorBlock(m.fmt == kBc1 ? blk : blk + 8, m.fmt == kBc1, px);
      }
      if (m.fmt == kBc2) AlphaBc2(blk, px);
      if (m.fmt == kBc3) AlphaBc3(blk, px);
      for (int i = 0; i < 16; ++i) {
        const uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
        if (x < w && y < h) std::memcpy(&out[(size_t(y) * w + x) * 4], px[i], 4);
      }
    }
  }
  return out;
}

// Half the size (a 2x2 box; an odd side keeps its last row or column's weight right by clamping).
std::vector<uint8_t> Half(const std::vector<uint8_t>& src, uint32_t w, uint32_t h, uint32_t* nw, uint32_t* nh) {
  *nw = std::max(1u, w / 2), *nh = std::max(1u, h / 2);
  std::vector<uint8_t> out(size_t(*nw) * *nh * 4);
  for (uint32_t y = 0; y < *nh; ++y) {
    const uint32_t y0 = std::min(h - 1, y * 2), y1 = std::min(h - 1, y * 2 + 1);
    for (uint32_t x = 0; x < *nw; ++x) {
      const uint32_t x0 = std::min(w - 1, x * 2), x1 = std::min(w - 1, x * 2 + 1);
      for (int c = 0; c < 4; ++c) {
        const uint32_t s = src[(size_t(y0) * w + x0) * 4 + c] + src[(size_t(y0) * w + x1) * 4 + c] +
                           src[(size_t(y1) * w + x0) * 4 + c] + src[(size_t(y1) * w + x1) * 4 + c];
        out[(size_t(y) * *nw + x) * 4 + c] = uint8_t((s + 2) / 4);
      }
    }
  }
  return out;
}

// A map ready for ngl_vk: BGRA levels (ngl_vk's order), largest first, at most kSide; or (c3camo v2, bc7) a BC7
// map's stored levels as their blocks.
struct Ready {
  uint32_t kind = 0, width = 0, height = 0;
  std::vector<std::vector<uint8_t>> levels;
  float mean[3] = {0.5f, 0.5f, 0.5f};
  bool bc7 = false;  // [acct2-bro] 2026-11-23 c3camo v2: levels are BC7 blocks as the file has them (for the GPU)
};

// [acct2-bro] 2026-11-23 c3camo v2: a BC7 map uses its own stored levels, from the first one of at most kSide on a
// side (each exactly its blocks' bytes, as ParseCamo proved), on both paths: kept as blocks for the GPU (blocks), or
// each decoded here to BGRA. So the two paths give the GPU the same levels and the same pixels (BC7 decoding is exact),
// and the colour's mean is taken from the first level decoded, the same way on both. False (nothing made) when no
// stored level is that small (a hand-made one-level map over kSide: Prepare decodes and halves it as for BC1-BC3).
bool PrepareBc7(const MapView& m, bool blocks, Ready* r) {
  const uint8_t* at = m.base;
  uint32_t first = m.levels;
  for (uint32_t l = 0; l < m.levels; ++l) {
    const uint32_t w = std::max(1u, m.width >> l), h = std::max(1u, m.height >> l);
    if (w <= kSide && h <= kSide) {
      first = l;
      break;
    }
    at += LevelBytes(kBc7, w, h);
  }
  if (first >= m.levels) return false;
  r->kind = m.kind;
  r->bc7 = blocks;
  r->width = std::max(1u, m.width >> first), r->height = std::max(1u, m.height >> first);
  for (uint32_t l = first; l < m.levels; ++l) {
    const uint32_t w = std::max(1u, m.width >> l), h = std::max(1u, m.height >> l);
    const uint64_t n = LevelBytes(kBc7, w, h);
    if (blocks && l != first) {
      r->levels.emplace_back(at, at + n);
      at += n;
      continue;
    }
    MapView one = m;
    one.width = w, one.height = h, one.base = at, one.base_bytes = n, one.levels = 1;
    std::vector<uint8_t> px = DecodeBase(one);  // RGBA
    if (l == first) {
      double sum[3] = {0, 0, 0};
      for (size_t i = 0; i < px.size(); i += 4)
        for (int c = 0; c < 3; ++c) sum[c] += px[i + c];
      for (int c = 0; c < 3; ++c) r->mean[c] = float(sum[c] / (255.0 * std::max<size_t>(1, px.size() / 4)));
    }
    if (blocks) {
      r->levels.emplace_back(at, at + n);
    } else {
      for (size_t i = 0; i < px.size(); i += 4) std::swap(px[i], px[i + 2]);  // RGBA to ngl_vk's BGRA
      r->levels.push_back(std::move(px));
    }
    at += n;
  }
  return true;
}

// blocks: a BC7 map may stay as its blocks (the reading thread asks so only when the GPU takes them).
Ready Prepare(const MapView& m, bool blocks = false) {
  Ready r;
  if (m.fmt == kBc7 && PrepareBc7(m, blocks, &r)) return r;  // c3camo v2
  r = Ready{};
  r.kind = m.kind;
  std::vector<uint8_t> px = DecodeBase(m);
  uint32_t w = m.width, h = m.height;
  while (w > kSide || h > kSide) {
    uint32_t nw, nh;
    px = Half(px, w, h, &nw, &nh);
    w = nw, h = nh;
  }
  r.width = w, r.height = h;
  double sum[3] = {0, 0, 0};
  for (size_t i = 0; i < px.size(); i += 4)
    for (int c = 0; c < 3; ++c) sum[c] += px[i + c];
  for (int c = 0; c < 3; ++c) r.mean[c] = float(sum[c] / (255.0 * std::max<size_t>(1, px.size() / 4)));
  for (;;) {
    std::vector<uint8_t> bgra(px.size());
    for (size_t i = 0; i < px.size(); i += 4) {
      bgra[i] = px[i + 2], bgra[i + 1] = px[i + 1], bgra[i + 2] = px[i], bgra[i + 3] = px[i + 3];
    }
    r.levels.push_back(std::move(bgra));
    if (w == 1 && h == 1) break;
    uint32_t nw, nh;
    px = Half(px, w, h, &nw, &nh);
    w = nw, h = nh;
  }
  return r;
}

// ---- where the camos are: <game folder>/data/cod3/camos, the game folder as ngl_side's ScreenshotFolder finds it ----
std::filesystem::path CamoFolder() {
#if defined(NGL_CAMO_TEST)
  return TestCamoFolder();
#else
  std::error_code error;
#if defined(_WIN32)
  std::wstring name(32768, L'\0');
  const DWORD length = GetModuleFileNameW(nullptr, name.data(), static_cast<DWORD>(name.size()));
  if (length == 0 || length >= name.size()) error = std::make_error_code(std::errc::no_such_file_or_directory);
  name.resize(error ? 0 : length);
  const std::filesystem::path exe(name);
#else
  const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", error);
#endif
  std::vector<std::filesystem::path> roots;
  if (const char* appimage = std::getenv("APPIMAGE"); appimage && *appimage) {
    roots.push_back(std::filesystem::path(appimage).parent_path());
  }
  if (!error) {
    roots.push_back(exe.parent_path());
    roots.push_back(exe.parent_path().parent_path());
  }
  std::filesystem::path root = roots.empty() ? std::filesystem::current_path(error) : roots.front();
  for (const auto& candidate : roots) {
    std::error_code missing;
    if (std::filesystem::exists(candidate / "assets" / "default.xex", missing)) {
      root = candidate;
      break;
    }
  }
  return root / "data" / "cod3" / "camos";
#endif
}

bool IsCamoId(std::string_view id) {
  if (id.size() != 17 || id.substr(0, 5) != "camo_") return false;
  return std::all_of(id.begin() + 5, id.end(), [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

// ---- state -------------------------------------------------------------------------------------------------------
std::mutex g_mu;                                   // the reading thread's queue and results
std::condition_variable g_wake;
std::deque<std::string> g_todo;                    // ids to read
struct Prepared {                                  // one camo read and prepared on the reading thread
  std::vector<Ready> maps;
  float scroll[2] = {0, 0};
  float scale = 1;
};
std::unordered_map<std::string, Prepared> g_done;  // read and prepared, waiting for textures
std::set<std::string> g_failed;                    // refused this session (logged once, never read again)
std::unique_ptr<std::thread> g_reader;
std::atomic<bool> g_stop{false};
std::atomic<uint32_t> g_refused{0};
// [acct2-bro] 2026-11-23 c3camo v2: BC7 maps stay blocks for the GPU (ngl_vk takes them, the debug switch is off and
// the GPU hasn't refused one); the drawing thread sets it each frame, the reading thread reads it per camo.
std::atomic<bool> g_blocks_ok{false};
std::mutex g_seen_mu;                              // the guns seen (LayerFor's thread writes, F2's reads)
std::set<std::string> g_seen;

// the drawing thread's own (no lock)
struct Made {
  Layer layer;
  float scroll[2] = {0, 0};
  double phase[2] = {0, 0};  // x509: the crawl so far, in pattern widths (-1..1), moved by each frame's real time
};
struct Uploading {                                 // the camo whose maps become textures, one map a frame
  std::string id;
  Prepared prepared;
  size_t next = 0;
  Made made;
  bool any = false;
};
std::optional<Uploading> g_uploading;
std::unordered_map<std::string, Made> g_made;      // id -> its textures (until Forget)
std::set<std::string> g_asked;                     // handed to the reader, not made yet
std::set<std::string> g_seen_here;                 // the drawing thread's copy of g_seen (no lock to look up)
uint32_t g_textures = 0, g_made_count = 0;
bool g_full_logged = false;
bool g_blocks_failed = false;  // c3camo v2: ngl_vk refused a BC7 texture: CPU decoding for the rest of the session
int g_blocks_said = -1;        // c3camo v2: the way BC7 camos go, as last logged (-1: not yet)
std::string g_choices_text;
std::map<std::pair<int, std::string>, std::string> g_choices;  // (player, weapon or "*") -> id; drawing thread
std::vector<Entry> g_list;
bool g_listed = false;
float g_strength = 1.0f;
bool g_on = false;
double g_seconds = 0;
double g_last_seconds = 0;  // x509: the previous BeginFrame's clock (0: none yet)
double g_dt = 0;            // x509: real seconds since then, at most 0.25 (a stall moves the crawl one step only)
float g_crawl = 1.0f;       // x509: cod3_camo_crawl / 100

void Log(const std::string& s) {
#if defined(NGL_CAMO_TEST)
  TestLog(s);
#else
  REXLOG_INFO("cod3: camo: {}", s);
#endif
}

// [acct2-bro] 2026-11-23 c3camo v2: a BC7 map's levels checked again right before the GPU gets them: 1 to
// kMaxLevels levels, none past a full chain, each exactly ceil(w/4) x ceil(h/4) x 16 bytes for its place in the chain
// (ngl_vk reads that many from each); empty when they fit, else why.
std::string BlocksRefusal(const Ready& r) {
  if (r.width < 1 || r.width > kSide || r.height < 1 || r.height > kSide) return "a BC7 map of a wrong size";
  if (r.levels.empty() || r.levels.size() > std::min(kMaxLevels, FullLevels(r.width, r.height)))
    return "a BC7 map with a wrong number of levels";
  for (size_t i = 0; i < r.levels.size(); ++i) {
    const uint32_t w = std::max(1u, r.width >> i), h = std::max(1u, r.height >> i);
    if (r.levels[i].size() != LevelBytes(kBc7, w, h)) return "a BC7 level of a wrong size";
  }
  return {};
}

// [acct2-bro] 2026-11-22 c519: the pass multiplies the gun by the colour map over its mean, so a set with no colour
// map, or one all but black (x519: "blue damascus" keeps its pattern in the specular map and a 32 x 32 black colour
// map; its mean 0 made every gun it was on black), would turn the gun black: such a set is refused, and this says why
// (empty: it can be used). All but black: no channel's mean reaches 4 of 255.
std::string ColourRefusal(const std::vector<Ready>& maps) {
  const Ready* colour = nullptr;
  for (const Ready& r : maps)
    if (r.kind == kColor) colour = &r;
  if (!colour) return "it has no colour map";
  if (std::max({colour->mean[0], colour->mean[1], colour->mean[2]}) < kDarkestMean) return "its colour map is black";
  return {};
}

// The reading thread: reads, checks and prepares one camo at a time. Nothing it does can reach a frame.
void ReaderMain() {
  for (;;) {
    std::string id;
    {
      std::unique_lock<std::mutex> lock(g_mu);
      g_wake.wait(lock, [] { return g_stop.load() || !g_todo.empty(); });
      if (g_stop.load()) return;
      id = std::move(g_todo.front());
      g_todo.pop_front();
    }
    std::string why;
    Prepared prepared;
    const std::filesystem::path path = CamoFolder() / (id + ".c3camo");
    std::error_code ec, link_ec;
    const auto size = std::filesystem::file_size(path, ec);
    if (ec) {
      // [acct2-bro] 2026-11-22 Camo lane c512: and why (x511's runs said only "can't be read": its test folder's
      // links led outside what the run could see)
      why = "can't be read (" + ec.message() + ")";
    } else if (size > kMaxFileBytes || std::filesystem::is_symlink(path, link_ec)) {
      why = "too big or not a plain file";
    } else {
      std::vector<uint8_t> data(static_cast<size_t>(size));
      std::ifstream f(path, std::ios::binary);
      if (!f.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()))) {
        why = "can't be read (short)";  // [acct2-bro] 2026-11-22 Camo lane c512
      } else {
        CamoView v;
        if (ParseCamo(data.data(), data.size(), &v, &why)) {
          const bool blocks = g_blocks_ok.load();  // c3camo v2
          for (const MapView& m : v.maps) prepared.maps.push_back(Prepare(m, blocks));
          prepared.scroll[0] = v.scroll_u, prepared.scroll[1] = v.scroll_v, prepared.scale = v.scale;
          why = ColourRefusal(prepared.maps);  // [acct2-bro] 2026-11-22 c519
          if (!why.empty()) prepared.maps.clear();
        }
      }
    }
    std::lock_guard<std::mutex> lock(g_mu);
    if (prepared.maps.empty()) {
      g_failed.insert(id);
      g_refused.fetch_add(1);
      Log(id + ".c3camo not used: " + (why.empty() ? "no maps" : why));
    } else {
      g_done[id] = std::move(prepared);
    }
  }
}

// At the program's end: the reading thread is told to stop and waited for (it never holds anything else).
struct ReaderOwner {
  ~ReaderOwner() {
    {
      std::lock_guard<std::mutex> lock(g_mu);
      g_stop.store(true);
    }
    g_wake.notify_all();
    if (g_reader && g_reader->joinable()) g_reader->join();
  }
} g_reader_owner;

void StartReader() {
  if (!g_reader) g_reader = std::make_unique<std::thread>(ReaderMain);
}

using ChoiceMap = std::map<std::pair<int, std::string>, std::string>;

bool IsWeaponName(std::string_view w) {
  return !w.empty() && w.size() <= 64 &&
         std::all_of(w.begin(), w.end(), [](char c) { return std::isalnum(uint8_t(c)) || c == '_' || c == '*'; });
}

// "p<1-4> <weapon or *>=<camo id>; ..." (at most 256 items; anything else is skipped). [acct2-bro] 2026-11-22
// x511: also "p<1-4>:<weapon>=<id>" and "all" for "*", so a choice can be given on a command line as one word with
// no space or wildcard in it (the test runs' --set cod3_camo_choices=p1:all=camo_...).
ChoiceMap ParseChoicesText(const std::string& text) {
  ChoiceMap out;
  size_t at = 0;
  while (at < text.size() && out.size() < 256) {
    size_t end = text.find(';', at);
    if (end == std::string::npos) end = text.size();
    std::string item = text.substr(at, end - at);
    at = end + 1;
    const size_t first = item.find_first_not_of(' ');
    if (first == std::string::npos) continue;
    item.erase(0, first);
    if (item.size() < 4 || item[0] != 'p' || item[1] < '1' || item[1] > '4' || (item[2] != ' ' && item[2] != ':'))
      continue;
    const size_t eq = item.find('=');
    if (eq == std::string::npos || eq < 4) continue;
    std::string weapon = item.substr(3, eq - 3), id = item.substr(eq + 1);
    while (!weapon.empty() && weapon.back() == ' ') weapon.pop_back();
    while (!id.empty() && id.back() == ' ') id.pop_back();
    if (weapon == "all") weapon = "*";  // x511
    if (!IsWeaponName(weapon) || !IsCamoId(id)) continue;
    out[{item[1] - '0', weapon}] = id;
  }
  return out;
}

std::string ChoicesText(const ChoiceMap& all) {
  std::string s;
  for (const auto& [key, id] : all) {
    if (!s.empty()) s += "; ";
    s += "p" + std::to_string(key.first) + " " + key.second + "=" + id;
  }
  return s;
}

}  // namespace

// [acct2-bro] 2026-11-23 Camo lane (the gloves): see ngl_camo.h
std::filesystem::path Folder() { return CamoFolder(); }

// [acct2-bro] 2026-11-22 Camo lane x549 (the gloves, a probe): one .c3camo file's colour map, read and checked by the
// rules ReaderMain uses (size, plain file, ParseCamo, ColourRefusal), as BGRA levels largest first (at most kSide)
// for ngl_vk_texture_levels; false and why. On the caller's thread (once per glove image until the next Forget).
bool ReadColourLevels(const std::filesystem::path& path, std::vector<std::vector<uint8_t>>* levels, uint32_t* width,
                      uint32_t* height, std::string* why) {
  std::error_code ec, link_ec;
  const auto size = std::filesystem::file_size(path, ec);
  if (ec) return *why = "can't be read (" + ec.message() + ")", false;
  if (size > kMaxFileBytes || std::filesystem::is_symlink(path, link_ec))
    return *why = "too big or not a plain file", false;
  std::vector<uint8_t> data(static_cast<size_t>(size));
  std::ifstream f(path, std::ios::binary);
  if (!f.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size()))) return *why = "can't be read", false;
  CamoView v;
  if (!ParseCamo(data.data(), data.size(), &v, why)) return false;
  std::vector<Ready> maps;
  for (const MapView& m : v.maps) maps.push_back(Prepare(m, false));  // c3camo v2: BGRA always (BC7 decoded)
  *why = ColourRefusal(maps);
  if (!why->empty()) return false;
  for (Ready& r : maps)
    if (r.kind == kColor) {
      *levels = std::move(r.levels);
      *width = r.width, *height = r.height;
      return true;
    }
  return *why = "it has no colour map", false;
}

const std::vector<Entry>& List() {
  if (!g_listed) Rescan();
  return g_list;
}

void Rescan() {
  g_listed = true;
  g_list.clear();
  std::ifstream f(CamoFolder() / "index.tsv");
  std::string line;
  while (g_list.size() < 4096 && std::getline(f, line)) {
    if (line.empty() || line[0] == '#' || line.size() > 512) continue;
    const size_t tab = line.find('\t');
    if (tab == std::string::npos) continue;
    Entry e;
    e.id = line.substr(0, tab);
    const size_t tab2 = line.find('\t', tab + 1);
    e.name = line.substr(tab + 1, tab2 == std::string::npos ? std::string::npos : tab2 - tab - 1);
    if (!IsCamoId(e.id)) continue;
    e.name.erase(std::remove_if(e.name.begin(), e.name.end(), [](char c) { return uint8_t(c) < 32; }), e.name.end());
    if (e.name.empty()) e.name = e.id;
    g_list.push_back(std::move(e));
  }
}

void BeginFrame() {
#if defined(NGL_CAMO_TEST)
  g_on = TestOn();
  const std::string text = TestChoices();
  g_strength = TestStrength();
#else
  g_on = REXCVAR_GET(cod3_camo);
  const std::string text = REXCVAR_GET(cod3_camo_choices);
  g_strength = std::clamp(REXCVAR_GET(cod3_camo_strength), 0, 100) / 100.0f;
  g_crawl = std::clamp(REXCVAR_GET(cod3_camo_crawl), 0, 400) / 100.0f;
#endif
  // [acct2-bro] 2026-11-23 c3camo v2: BC7 maps go to the GPU as blocks only when ngl_vk takes them (the device has BC
  // textures on), the debug switch doesn't ask for CPU decoding and the GPU hasn't refused one this session
#if defined(NGL_CAMO_TEST_BLOCKS)
  const bool gpu_blocks = TestGpuBlocks(), cpu_blocks = TestCpuBlocks();
#elif defined(NGL_CAMO_TEST)
  const bool gpu_blocks = false, cpu_blocks = true;
#elif defined(COD3_NGL_VK) && defined(NGL_VK_BLOCK_TEXTURES)
  const bool gpu_blocks = ngl_vk_block_textures() != 0, cpu_blocks = REXCVAR_GET(cod3_camo_cpu_blocks);
#else
  const bool gpu_blocks = false, cpu_blocks = true;
#endif
  g_blocks_ok.store(gpu_blocks && !cpu_blocks && !g_blocks_failed);
  if (g_on && int(g_blocks_ok.load()) != g_blocks_said) {  // said once, and again when it changes
    g_blocks_said = int(g_blocks_ok.load());
    Log(g_blocks_said ? std::string("BC7 camos go to the GPU as their blocks")
                      : std::string("BC7 camos are decoded on the CPU (") +
                            (cpu_blocks ? "cod3_camo_cpu_blocks is on"
                             : g_blocks_failed ? "the GPU refused one"
                                               : "the device has no BC textures on") + ")");
  }
  // x509: the crawl by real time, the same at any frame rate: each frame adds speed x its own seconds to the phase
#if defined(NGL_CAMO_TEST)
  g_seconds = TestSeconds();   // the test program's clock (camo509_crawl: 30 and 240 frames a second, a stall)
#else
  g_seconds = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
#endif
  g_dt = g_last_seconds > 0 ? std::clamp(g_seconds - g_last_seconds, 0.0, 0.25) : 0.0;
  g_last_seconds = g_seconds;
  if (!g_on) return;
  for (auto& entry : g_made) {
    Made& made = entry.second;
    for (int k = 0; k < 2; ++k) made.phase[k] = std::fmod(made.phase[k] + double(made.scroll[k]) * g_crawl * g_dt, 1.0);
  }
  if (text != g_choices_text) {
    g_choices_text = text;
    g_choices = ParseChoicesText(text);
  }
  // hand the chosen camos not made yet to the reader (each once)
  for (const auto& [key, id] : g_choices) {
    if (g_made.count(id) || g_asked.count(id)) continue;
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_failed.count(id) || g_todo.size() >= 64) continue;   // a bounded queue: the rest is asked next frame
    g_asked.insert(id);
    g_todo.push_back(id);
    StartReader();
    g_wake.notify_one();
  }
  // make at most one map's texture this frame (a 1024 map with its levels is about 5.6 MB to hand over)
  if (!g_uploading) {
    std::lock_guard<std::mutex> lock(g_mu);
    if (g_done.empty()) return;
    auto it = g_done.begin();
    g_uploading.emplace();
    g_uploading->id = it->first;
    g_uploading->prepared = std::move(it->second);
    g_done.erase(it);
  }
  Uploading& up = *g_uploading;
  if (up.next < up.prepared.maps.size()) {
    if (g_textures + 1 > kMostTextures) {
      if (!g_full_logged) Log("no room for more camo textures until the next level; new choices show then");
      g_full_logged = true;
      g_asked.erase(up.id);
      g_uploading.reset();
      return;
    }
    Ready& r = up.prepared.maps[up.next++];
    // [acct2-bro] 2026-11-23 c3camo v2: a map read as BC7 blocks when blocks can't be used now (the switch was turned
    // on, or the GPU refused one since): this camo is read again and decoded on the CPU
    if (r.bc7 && !g_blocks_ok.load()) {
      g_asked.erase(up.id);
      g_uploading.reset();
      return;
    }
    if (r.bc7) {
      const std::string refusal = BlocksRefusal(r);
      if (!refusal.empty()) {
        std::lock_guard<std::mutex> lock(g_mu);
        g_failed.insert(up.id);
        g_refused.fetch_add(1);
        Log(up.id + ".c3camo not used: " + refusal);
        g_asked.erase(up.id);
        g_uploading.reset();
        return;
      }
    }
    std::vector<const void*> levels;
    for (const auto& l : r.levels) levels.push_back(l.data());
    uint32_t tex = 0;
#if defined(NGL_CAMO_TEST_BLOCKS)
    tex = r.bc7 ? TestBlockTexture(levels.data(), uint32_t(levels.size()), r.width, r.height)
                : TestTexture(levels.data(), uint32_t(levels.size()), r.width, r.height);
#elif defined(NGL_CAMO_TEST)
    tex = TestTexture(levels.data(), uint32_t(levels.size()), r.width, r.height);
#elif defined(COD3_NGL_VK)
#if defined(NGL_VK_BLOCK_TEXTURES)
    if (r.bc7)
      tex = ngl_vk_texture_blocks(NGL_VK_BLOCKS_BC7, levels.data(), uint32_t(levels.size()), r.width, r.height);
    else
#endif
      tex = ngl_vk_texture_levels(levels.data(), uint32_t(levels.size()), r.width, r.height);
#endif
    if (r.bc7 && !tex) {  // c3camo v2: the GPU refused BC7 blocks: decoded on the CPU from now on, this camo read again
      Log("the GPU refused a BC7 camo texture; BC7 camos are decoded on the CPU from now on");
      g_blocks_failed = true;
      g_blocks_ok.store(false);
      g_asked.erase(up.id);
      g_uploading.reset();
      return;
    }
    r.levels.clear();
    r.levels.shrink_to_fit();
    if (tex) {
      ++g_textures;
      up.any = up.any || r.kind != kEnv;
      if (r.kind == kColor) {
        up.made.layer.color = tex;
        std::copy(r.mean, r.mean + 3, up.made.layer.mean);
      } else if (r.kind == kSpecular) {
        up.made.layer.specular = tex;
      } else {
        up.made.layer.env = tex;
      }
    }
    if (up.next < up.prepared.maps.size()) return;
  }
  // every map handed over: the camo can be chosen from the next LayerFor on
  g_asked.erase(up.id);
  if (up.any) {
    up.made.scroll[0] = up.prepared.scroll[0], up.made.scroll[1] = up.prepared.scroll[1];
    up.made.layer.scale = up.prepared.scale;
    g_made[up.id] = up.made;
    ++g_made_count;
  } else {
    std::lock_guard<std::mutex> lock(g_mu);
    g_failed.insert(up.id);
    Log(up.id + ": its textures couldn't be made");
  }
  g_uploading.reset();
}

bool LayerFor(int player, std::string_view weapon, Layer* out) {
  if (!g_on || !out || player < 1 || player > 4 || weapon.empty() || weapon.size() > 64) return false;
  if (g_seen_here.size() < 128 && IsWeaponName(weapon) && weapon != "*" && !g_seen_here.count(std::string(weapon))) {
    g_seen_here.insert(std::string(weapon));
    std::lock_guard<std::mutex> lock(g_seen_mu);
    g_seen.insert(std::string(weapon));
  }
  auto it = g_choices.find({player, std::string(weapon)});
  if (it == g_choices.end()) it = g_choices.find({player, std::string("*")});
  if (it == g_choices.end()) return false;
  auto made = g_made.find(it->second);
  if (made == g_made.end() || (!made->second.layer.color && !made->second.layer.specular)) return false;
  *out = made->second.layer;
  out->strength = g_strength;
  // the crawl by real time (x509: the phase BeginFrame moves; within one pattern width, so no jump ever)
  out->offset[0] = float(made->second.phase[0]);
  out->offset[1] = float(made->second.phase[1]);
  return out->strength > 0.0f;
}

void Forget() {
  g_made.clear();
  g_uploading.reset();
  g_textures = 0;
  g_full_logged = false;
  // anything read but not made yet is made again from its file later (its textures would be from before the forget)
  std::lock_guard<std::mutex> lock(g_mu);
  g_done.clear();
  g_todo.clear();   // c503: without this every new level queued the same camos again behind the old entries
  g_asked.clear();
}

// F2's thread: the choices are read from and written to the cvar's text only (the drawing thread parses its own copy
// in BeginFrame), so nothing here is shared with a frame.
std::string Choice(int player, std::string_view weapon) {
#if defined(NGL_CAMO_TEST)
  const std::string text = TestChoices();
#else
  const std::string text = REXCVAR_GET(cod3_camo_choices);
#endif
  const auto all = ParseChoicesText(text);
  auto it = all.find({player, std::string(weapon)});
  return it == all.end() ? std::string() : it->second;
}

void SetChoice(int player, std::string_view weapon, std::string_view camo_id) {
  if (player < 1 || player > 4 || weapon.empty() || weapon.size() > 64 || !IsWeaponName(weapon)) return;
  if (!camo_id.empty() && !IsCamoId(camo_id)) return;
#if defined(NGL_CAMO_TEST)
  auto all = ParseChoicesText(TestChoices());
#else
  auto all = ParseChoicesText(REXCVAR_GET(cod3_camo_choices));
#endif
  if (camo_id.empty()) {
    all.erase({player, std::string(weapon)});
  } else {
    all[{player, std::string(weapon)}] = std::string(camo_id);
  }
  const std::string text = ChoicesText(all);
#if defined(NGL_CAMO_TEST)
  TestSetChoices(text);
#else
  REXCVAR_SET(cod3_camo_choices, text);
#endif
}

#if defined(NGL_CAMO_TEST)
// The test program's whole check of one file's bytes: the reader's rules, then every map decoded, halved and given its
// levels as the reading thread does; the levels' sizes are checked against their place in the chain. [acct2-bro]
// 2026-11-23 c3camo v2: blocks: BC7 maps kept as blocks, as the reading thread does when the GPU takes them (their
// levels checked as BeginFrame checks them before the GPU); a BC7 map's levels are its stored ones (maybe fewer than a
// full chain).
bool TestCheck(const uint8_t* d, size_t n, std::string* why, bool blocks) {
  CamoView v;
  if (!ParseCamo(d, n, &v, why)) return false;
  std::vector<Ready> made;
  for (const MapView& m : v.maps) {
    Ready r = Prepare(m, blocks);
    if (r.width < 1 || r.width > kSide || r.height < 1 || r.height > kSide || r.levels.empty()) return false;
    if (r.bc7) {
      if (!BlocksRefusal(r).empty()) return false;
    } else {
      for (size_t i = 0; i < r.levels.size(); ++i) {
        const uint32_t w = std::max(1u, r.width >> i), h = std::max(1u, r.height >> i);
        if (r.levels[i].size() != size_t(w) * h * 4) return false;
      }
      const size_t full = FullLevels(r.width, r.height);
      if (m.fmt == kBc7 ? r.levels.size() > full : r.levels.size() != full) return false;
    }
    made.push_back(std::move(r));
  }
  const std::string refusal = ColourRefusal(made);  // c519: as the reading thread refuses it
  if (!refusal.empty()) {
    if (why) *why = refusal;
    return false;
  }
  return true;
}
bool TestCheck(const uint8_t* d, size_t n, std::string* why) { return TestCheck(d, n, why, false); }

// [acct2-bro] 2026-11-23 c3camo v2: the GPU path and the CPU path of one checked file give the same thing: for every
// map, the same size, level count and mean, and each BC7 level's blocks decoded (as a GPU decodes BC7: exactly) equal
// the CPU path's pixels. 0: the same; else the first difference, as text in why.
bool TestPathsAgree(const uint8_t* d, size_t n, std::string* why) {
  CamoView v;
  if (!ParseCamo(d, n, &v, why)) return false;
  for (const MapView& m : v.maps) {
    const Ready g = Prepare(m, true), c = Prepare(m, false);
    if (g.width != c.width || g.height != c.height || g.levels.size() != c.levels.size())
      return *why = "the two paths give different sizes", false;
    for (int k = 0; k < 3; ++k)
      if (g.mean[k] != c.mean[k]) return *why = "the two paths give different means", false;
    for (size_t i = 0; i < g.levels.size(); ++i) {
      if (!g.bc7) {
        if (g.levels[i] != c.levels[i]) return *why = "the two paths give different pixels", false;
        continue;
      }
      const uint32_t w = std::max(1u, g.width >> i), h = std::max(1u, g.height >> i);
      MapView one = m;
      one.width = w, one.height = h, one.base = g.levels[i].data(), one.base_bytes = g.levels[i].size(), one.levels = 1;
      std::vector<uint8_t> px = DecodeBase(one);
      for (size_t k = 0; k < px.size(); k += 4) std::swap(px[k], px[k + 2]);
      if (px != c.levels[i]) return *why = "a BC7 level decodes differently from the CPU path", false;
    }
  }
  return true;
}
#endif

std::vector<std::string> GunsSeen() {
  std::lock_guard<std::mutex> lock(g_seen_mu);
  return std::vector<std::string>(g_seen.begin(), g_seen.end());
}

Counts GetCounts() {
  Counts c;
  c.refused = g_refused.load();
  c.made = g_made_count;
  std::lock_guard<std::mutex> lock(g_mu);
  c.waiting = uint32_t(g_todo.size() + g_done.size());
  return c;
}

}  // namespace cod3::ngl_camo
