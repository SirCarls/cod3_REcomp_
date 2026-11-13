// waw - World at War's native renderer: Xbox 360 textures decoded to BGRA8 (waw_native_textures.h).
// [waw-decomp-waw] 2026-11-11
//
// The same formats, tiling and packed-mip rule as Call of Duty 2's native_side.cpp and Call of Duty 3's ngl_side.cpp
// decode (from rexglue's and Xenia's texture code; this file is Call of Duty 2's decoder of 2026-11-11, its 3D tiling
// left out), read through the host's whole mapping of physical memory. Nothing here is the game's code or data.

#include "waw_native_textures.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>  // physical_membase

namespace waw::native {

size_t PhysicalRoom(size_t physical) { return physical < kPhysicalBytes ? kPhysicalBytes - physical : 0; }

namespace {

// The host's mapping of the guest's physical memory: a page of the guest's 0xA0000000 view can be unmapped when the
// game allocated it through another view (Call of Duty 3's side138), so the GPU's data is read through this one.
const uint8_t* PhysicalBase() {
  static std::atomic<const uint8_t*> physical{nullptr};
  const uint8_t* p = physical.load(std::memory_order_acquire);
  if (!p) {
    auto* kernel = rex::system::kernel_state();
    p = kernel && kernel->memory() ? kernel->memory()->physical_membase() : nullptr;
    physical.store(p, std::memory_order_release);
  }
  return p;
}

}  // namespace

const uint8_t* GpuPointer(const uint8_t* base, uint32_t physical) {
  const uint8_t* p = PhysicalBase();
  return p ? p + physical : base + (0xA0000000u | physical);
}

uint32_t GpuU32(const uint8_t* base, uint32_t physical) {
  if (PhysicalRoom(physical) < 4) return 0;
  uint32_t v;
  std::memcpy(&v, GpuPointer(base, physical), 4);
  return __builtin_bswap32(v);
}

float GpuF32(const uint8_t* base, uint32_t physical) {
  const uint32_t bits = GpuU32(base, physical);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

uint32_t GpuU16(const uint8_t* base, uint32_t physical) {
  return (GpuU32(base, physical & ~3u) >> ((physical & 2u) ? 0u : 16u)) & 0xFFFFu;
}

namespace {

constexpr size_t kMaxTextureBytes = 32u << 20;

struct Format {
  uint32_t block;  // texels across a block (1 or 4)
  uint32_t bytes;  // bytes a block
  uint32_t log2_bytes;
};

bool FormatOf(uint32_t format, Format* f) {
  switch (format) {
    case 2:   // k_8
    case 8:   // k_8_A
    case 9:   // k_8_B
      *f = {1, 1, 0};
      return true;
    case 10:  // k_8_8
    case 3:   // k_1_5_5_5
    case 4:   // k_5_6_5
    case 5:   // k_6_5_5
    case 15:  // k_4_4_4_4
      *f = {1, 2, 1};
      return true;
    case 6:   // k_8_8_8_8
    case 50:  // k_8_8_8_8_AS_16_16_16_16
    case 7:   // k_2_10_10_10
    case 54:  // k_2_10_10_10_AS_16_16_16_16
    case 16:  // k_10_11_11
    case 55:  // k_10_11_11_AS_16_16_16_16
    case 17:  // k_11_11_10
    case 56:  // k_11_11_10_AS_16_16_16_16
      *f = {1, 4, 2};
      return true;
    case 18:  // k_DXT1
    case 51:  // k_DXT1_AS_16_16_16_16
    case 58:  // k_DXT3A
    case 59:  // k_DXT5A
      *f = {4, 8, 3};
      return true;
    case 19:  // k_DXT2_3
    case 20:  // k_DXT4_5
    case 52:  // k_DXT2_3_AS_16_16_16_16
    case 53:  // k_DXT4_5_AS_16_16_16_16
    case 49:  // k_DXN
      *f = {4, 16, 4};
      return true;
    default:
      return false;
  }
}

// The Xenos 2D tiling, in blocks: the row's part, then the block's offset (Xenia's TiledOffset2DOuter / Inner).
int32_t TiledRow(int32_t y, int32_t width, uint32_t log2_bytes) {
  const int32_t macro = ((y >> 5) * (width >> 5)) << (log2_bytes + 7);
  const int32_t micro = ((y & 6) << 2) << log2_bytes;
  return macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 8) << (3 + log2_bytes)) + ((y & 1) << 4);
}

int32_t TiledOffset(int32_t x, int32_t y, uint32_t log2_bytes, int32_t row) {
  const int32_t macro = (x >> 5) << (log2_bytes + 7);
  const int32_t micro = (x & 7) << log2_bytes;
  const int32_t offset = row + (macro + ((micro & ~0xF) << 1) + (micro & 0xF));
  return ((offset & ~0x1FF) << 3) + ((offset & 0x1C0) << 2) + (offset & 0x3F) + ((y & 16) << 7) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
}

// The fetch constant's endianness: 1 swaps the bytes in each 16 bits, 2 in each 32, 3 the 16-bit halves.
void SwapBlock(uint8_t* b, uint32_t bytes, uint32_t endian) {
  for (uint32_t i = 0; i + 3 < bytes; i += 4) {
    uint8_t* w = b + i;
    switch (endian) {
      case 1:
        std::swap(w[0], w[1]);
        std::swap(w[2], w[3]);
        break;
      case 2:
        std::swap(w[0], w[3]);
        std::swap(w[1], w[2]);
        break;
      case 3:
        std::swap(w[0], w[2]);
        std::swap(w[1], w[3]);
        break;
      default:
        break;
    }
  }
}

void Color565(uint16_t c, uint8_t out[4]) {
  out[0] = static_cast<uint8_t>(((c >> 11) & 31) * 255 / 31);
  out[1] = static_cast<uint8_t>(((c >> 5) & 63) * 255 / 63);
  out[2] = static_cast<uint8_t>((c & 31) * 255 / 31);
  out[3] = 255;
}

// A BC1 color block (little-endian, after the swap) into 16 rgba texels; four colors always when `four` (BC2, BC3).
void DecodeColors(const uint8_t* b, bool four, uint8_t texels[16][4]) {
  const uint16_t c0 = static_cast<uint16_t>(b[0] | b[1] << 8), c1 = static_cast<uint16_t>(b[2] | b[3] << 8);
  uint8_t colors[4][4];
  Color565(c0, colors[0]);
  Color565(c1, colors[1]);
  if (four || c0 > c1) {
    for (int k = 0; k < 4; ++k) {
      colors[2][k] = static_cast<uint8_t>((2 * colors[0][k] + colors[1][k]) / 3);
      colors[3][k] = static_cast<uint8_t>((colors[0][k] + 2 * colors[1][k]) / 3);
    }
  } else {
    for (int k = 0; k < 4; ++k) colors[2][k] = static_cast<uint8_t>((colors[0][k] + colors[1][k]) / 2);
    colors[3][0] = colors[3][1] = colors[3][2] = colors[3][3] = 0;
  }
  const uint32_t bits = static_cast<uint32_t>(b[4] | b[5] << 8 | b[6] << 16) | static_cast<uint32_t>(b[7]) << 24;
  for (int t = 0; t < 16; ++t) std::memcpy(texels[t], colors[(bits >> (2 * t)) & 3], 4);
}

// A BC4 block (two ends, then 3-bit picks) into one channel of 16 texels.
void DecodeChannelBlock(const uint8_t* b, uint8_t texels[16][4], int channel) {
  uint32_t a[8] = {b[0], b[1]};
  if (a[0] > a[1]) {
    for (int k = 1; k < 7; ++k) a[k + 1] = ((7 - k) * a[0] + k * a[1]) / 7;
  } else {
    for (int k = 1; k < 5; ++k) a[k + 1] = ((5 - k) * a[0] + k * a[1]) / 5;
    a[6] = 0;
    a[7] = 255;
  }
  uint64_t bits = 0;
  for (int k = 0; k < 6; ++k) bits |= static_cast<uint64_t>(b[2 + k]) << (8 * k);
  for (int t = 0; t < 16; ++t) texels[t][channel] = static_cast<uint8_t>(a[(bits >> (3 * t)) & 7]);
}

void DecodeBlock(const uint8_t* b, uint32_t format, uint8_t texels[16][4]) {
  if (format == 58 || format == 59 || format == 49) {  // one or two channels; the others 0, 0, 1 as sampled
    for (int t = 0; t < 16; ++t) {
      texels[t][0] = texels[t][1] = texels[t][2] = 0;
      texels[t][3] = 255;
    }
    if (format == 58) {
      for (int t = 0; t < 16; ++t) texels[t][0] = static_cast<uint8_t>(((b[t / 2] >> (4 * (t & 1))) & 15) * 17);
    } else {
      DecodeChannelBlock(b, texels, 0);
      if (format == 49) DecodeChannelBlock(b + 8, texels, 1);
    }
    return;
  }
  if (format == 51) format = 18;  // the AS_16_16_16_16 kinds hold the same blocks
  if (format == 52) format = 19;
  if (format == 53) format = 20;
  if (format == 18) {
    DecodeColors(b, false, texels);
  } else if (format == 19) {
    DecodeColors(b + 8, true, texels);
    for (int t = 0; t < 16; ++t) texels[t][3] = static_cast<uint8_t>(((b[t / 2] >> (4 * (t & 1))) & 15) * 17);
  } else {  // 20: BC3, its alpha a BC4 block
    DecodeColors(b + 8, true, texels);
    DecodeChannelBlock(b, texels, 3);
  }
}

uint32_t Log2Ceil(uint32_t v) {
  uint32_t l = 0;
  while (l < 31 && (1u << l) < v) ++l;
  return l;
}

// A packed texel's fields x, y, z, w (x in the low bits): each one's bits; a format with no w field gets 255 there.
struct PackedFields {
  uint8_t bits[4];
};

bool PackedFormat(uint32_t format, PackedFields* fields) {
  switch (format) {
    case 3: *fields = {{5, 5, 5, 1}}; return true;    // k_1_5_5_5
    case 4: *fields = {{5, 6, 5, 0}}; return true;    // k_5_6_5
    case 5: *fields = {{6, 5, 5, 0}}; return true;    // k_6_5_5
    case 15: *fields = {{4, 4, 4, 4}}; return true;   // k_4_4_4_4
    case 7:
    case 54: *fields = {{10, 10, 10, 2}}; return true;  // k_2_10_10_10
    case 16:
    case 55: *fields = {{10, 11, 11, 0}}; return true;  // k_10_11_11
    case 17:
    case 56: *fields = {{11, 11, 10, 0}}; return true;  // k_11_11_10
    default: return false;
  }
}

void UnpackTexel(uint32_t word, const PackedFields& fields, uint8_t texel[4]) {
  uint32_t shift = 0;
  for (int k = 0; k < 4; ++k) {
    const uint32_t n = fields.bits[k];
    if (n == 0) {
      texel[k] = 255;
      continue;
    }
    const uint32_t value = (word >> shift) & ((1u << n) - 1);
    texel[k] = static_cast<uint8_t>((value * 255 + ((1u << n) - 1) / 2) / ((1u << n) - 1));
    shift += n;
  }
}

// One surface's texels as BGRA8 rows: width x height texels in blocks of format f from `source` (`bytes` long), tiled
// or not with pitch_blocks blocks a row, the first block at (offset_x, offset_y) in blocks (a level inside a packed
// surface); each texel's channels picked by the fetch constant's swizzle (0-3 a channel, 4 zero, 5 one).
void DecodeSurface(const uint8_t* source, size_t bytes, const Format& f, uint32_t format, uint32_t endian,
                   uint32_t swizzle, bool tiled, uint32_t pitch_blocks, uint32_t offset_x, uint32_t offset_y,
                   uint32_t width, uint32_t height, std::vector<uint8_t>& bgra) {
  const uint32_t blocks_x = (width + f.block - 1) / f.block, blocks_y = (height + f.block - 1) / f.block;
  bgra.assign(static_cast<size_t>(width) * height * 4, 0);
  uint8_t block[16];
  uint8_t texels[16][4];
  PackedFields packed{};
  const bool is_packed = PackedFormat(format, &packed);
  for (uint32_t by = 0; by < blocks_y; ++by) {
    const int32_t sy = static_cast<int32_t>(by + offset_y);
    const int32_t row = tiled ? TiledRow(sy, static_cast<int32_t>(pitch_blocks), f.log2_bytes) : 0;
    for (uint32_t bx = 0; bx < blocks_x; ++bx) {
      const int32_t sx = static_cast<int32_t>(bx + offset_x);
      const uint32_t index = tiled ? static_cast<uint32_t>(TiledOffset(sx, sy, f.log2_bytes, row)) >> f.log2_bytes
                                   : static_cast<uint32_t>(sy) * pitch_blocks + static_cast<uint32_t>(sx);
      if (static_cast<size_t>(index) * f.bytes + f.bytes > bytes) continue;
      if (f.bytes < 4) {  // one or two bytes a texel: each byte found through the 32-bit word swap
        const size_t at = static_cast<size_t>(index) * f.bytes;
        const size_t flip = endian == 1 ? 1u : endian == 2 ? 3u : endian == 3 ? 2u : 0u;
        uint8_t v[2] = {0, 0};
        for (uint32_t k = 0; k < f.bytes; ++k) {
          const size_t from = (at + k) ^ flip;
          v[k] = from < bytes ? source[from] : uint8_t{0};
        }
        if (is_packed) {
          UnpackTexel(uint32_t{v[0]} | uint32_t{v[1]} << 8, packed, texels[0]);
        } else {
          texels[0][0] = v[0];
          texels[0][1] = f.bytes > 1 ? v[1] : 0;
          texels[0][2] = 0;
          texels[0][3] = 255;
        }
      } else if (f.block == 1) {
        std::memcpy(block, source + static_cast<size_t>(index) * f.bytes, f.bytes);
        SwapBlock(block, f.bytes, endian);
        if (is_packed) {
          UnpackTexel(uint32_t{block[0]} | uint32_t{block[1]} << 8 | uint32_t{block[2]} << 16 |
                          uint32_t{block[3]} << 24,
                      packed, texels[0]);
        } else {
          std::memcpy(texels[0], block, 4);
        }
      } else {
        std::memcpy(block, source + static_cast<size_t>(index) * f.bytes, f.bytes);
        SwapBlock(block, f.bytes, endian);
        DecodeBlock(block, format, texels);
      }
      for (uint32_t ty = 0; ty < f.block; ++ty) {
        for (uint32_t tx = 0; tx < f.block; ++tx) {
          const uint32_t x = bx * f.block + tx, y = by * f.block + ty;
          if (x >= width || y >= height) continue;
          const uint8_t* t = texels[ty * f.block + tx];
          uint8_t c[4];
          for (int k = 0; k < 4; ++k) {
            const uint32_t s = (swizzle >> (3 * k)) & 7;
            c[k] = s < 4 ? t[s] : s == 5 ? 255 : 0;
          }
          uint8_t* out = &bgra[(static_cast<size_t>(y) * width + x) * 4];
          out[0] = c[2];
          out[1] = c[1];
          out[2] = c[0];
          out[3] = c[3];
        }
      }
    }
  }
}

}  // namespace

// A texture's top level from its fetch constant: dword 0 tiled (bit 31) and pitch (bits 22-30, x 32 texels); dword 1
// format (bits 0-5), endianness (bits 6-7) and address (& 0x1FFFF000); dword 2 width - 1 and height - 1 (13 bits
// each); dword 3 swizzle (bits 1-12); dword 5 packed mips (bit 11). False (and why) when it can't be read.
bool DecodeFetch(const uint8_t* base, const std::array<uint32_t, 6>& fetch, std::vector<uint8_t>& bgra,
                 uint32_t& width, uint32_t& height, std::string& why) {
  if ((fetch[0] & 3) != 2) {
    why = "not a texture fetch constant";
    return false;
  }
  const bool tiled = (fetch[0] >> 31) != 0;
  const uint32_t pitch = ((fetch[0] >> 22) & 0x1FF) * 32;
  const uint32_t format = fetch[1] & 0x3F, endian = (fetch[1] >> 6) & 3;
  const uint32_t physical = fetch[1] & 0x1FFFF000;
  width = (fetch[2] & 0x1FFF) + 1;
  height = ((fetch[2] >> 13) & 0x1FFF) + 1;
  const uint32_t swizzle = (fetch[3] >> 1) & 0xFFF;
  const bool packed = ((fetch[5] >> 11) & 1) != 0;
  Format f;
  if (!FormatOf(format, &f)) {
    why = "format " + std::to_string(format);
    return false;
  }
  const uint32_t blocks_x = (width + f.block - 1) / f.block, blocks_y = (height + f.block - 1) / f.block;
  // a small texture's top level in its packed mip tile (Xenia's GetPackedMipOffset, level 0)
  uint32_t offset_x = 0, offset_y = 0;
  if (packed && std::min(Log2Ceil(width), Log2Ceil(height)) <= 4) {
    if (Log2Ceil(width) > Log2Ceil(height)) {
      offset_y = 16 / f.block;
    } else {
      offset_x = 16 / f.block;
    }
  }
  uint32_t pitch_blocks = std::max(pitch / f.block, blocks_x + offset_x);
  if (tiled) pitch_blocks = (pitch_blocks + 31) & ~31u;
  const uint32_t rows = tiled ? (blocks_y + offset_y + 31) & ~31u : blocks_y + offset_y;
  const size_t bytes = static_cast<size_t>(pitch_blocks) * rows * f.bytes;
  if (bytes > kMaxTextureBytes || physical == 0) {
    why = "size or address out of range";
    return false;
  }
  DecodeSurface(GpuPointer(base, physical), std::min(bytes, PhysicalRoom(physical)), f, format, endian, swizzle,
                tiled, pitch_blocks, offset_x, offset_y, width, height, bgra);
  return true;
}

// Step 4t2: the game's own smaller levels of a texture (1 to its last, the fetch constant's dword 4 bits 6-9), BGRA8
// rows each, from where the GPU keeps them; with only the top level, far alpha-tested surfaces (the pines) kept every
// texel of the top level and came out fuller than the game's. The layout is Call of Duty 3's DecodeLevelsFetch
// (ngl_side.cpp, rexglue-native and acct2-decomp; checked there on 7,169 textures), the same Xenos rules: after the top
// level each level is a tiled surface of its own, pitch and rows rounded up to 32 blocks, until the first level whose
// shorter side is 16 texels or less; in a packed texture (dword 5 bit 11) that level and every smaller one share one
// surface, each at Xenia's GetPackedMipOffset place. Levels 1 on start at the mip address (dword 5, & 0x1FFFF000)
// when there is one, else right after the top level. Only tiled 2D textures whose sides are powers of two; for others
// `levels` stays empty (the top level alone, as before).
void DecodeLevelsFetch(const uint8_t* base, const std::array<uint32_t, 6>& fetch,
                       std::vector<std::vector<uint8_t>>& levels) {
  levels.clear();
  const uint32_t format = fetch[1] & 0x3F, endian = (fetch[1] >> 6) & 3;
  const uint32_t physical = fetch[1] & 0x1FFFF000;
  const uint32_t width = (fetch[2] & 0x1FFF) + 1, height = ((fetch[2] >> 13) & 0x1FFF) + 1;
  const uint32_t swizzle = (fetch[3] >> 1) & 0xFFF;
  const uint32_t last = (fetch[4] >> 6) & 0xF;
  const uint32_t mip_address = fetch[5] & 0x1FFFF000;
  const bool tiled = (fetch[0] >> 31) != 0, packed = ((fetch[5] >> 11) & 1) != 0;
  Format f;
  if ((fetch[0] & 3) != 2 || !tiled || last == 0 || physical == 0 || !FormatOf(format, &f) ||
      (width & (width - 1)) != 0 || (height & (height - 1)) != 0)
    return;
  const uint32_t log2_w = Log2Ceil(width), log2_h = Log2Ceil(height);
  const uint32_t shorter = std::min(log2_w, log2_h);
  const uint32_t first_packed = shorter > 4 ? shorter - 4 : 0;  // the first level in the shared surface
  auto round32 = [](uint32_t v) { return (v + 31) & ~31u; };
  size_t next = 0, shared_at = 0, top_bytes = 0;  // top_bytes: the top level's own surface (0 when it is shared)
  uint32_t shared_pitch = 0, shared_rows = 0;
  bool shared = false;
  for (uint32_t level = 0; level <= last; ++level) {
    const uint32_t w = std::max(1u, width >> level), h = std::max(1u, height >> level);
    const uint32_t blocks_x = (w + f.block - 1) / f.block, blocks_y = (h + f.block - 1) / f.block;
    size_t at;
    uint32_t pitch, rows, x = 0, y = 0;  // x, y: the level's place in a shared surface, in texels
    if (packed && level >= first_packed) {
      if (!shared) {
        shared = true;
        shared_at = next;
        shared_pitch = round32(blocks_x);
        shared_rows = round32(blocks_y);
        next += static_cast<size_t>(shared_pitch) * shared_rows * f.bytes;
      }
      const uint32_t step = level - first_packed;
      const bool wider = log2_w > log2_h;
      if (step < 3) {
        (wider ? y : x) = 16u >> step;
      } else {
        (wider ? x : y) = (1u << ((wider ? log2_w : log2_h) - first_packed)) >> (step - 2);
      }
      at = shared_at;
      pitch = shared_pitch;
      rows = shared_rows;
    } else {
      at = next;
      pitch = round32(blocks_x);
      rows = round32(blocks_y);
      next += static_cast<size_t>(pitch) * rows * f.bytes;
      if (level == 0) top_bytes = next;
    }
    if (level == 0) continue;  // the top level is DecodeFetch's
    const size_t bytes = static_cast<size_t>(pitch) * rows * f.bytes;
    // a surface shared with the top level is at the base; every other is counted from where levels 1 on start
    const bool with_top = shared && first_packed == 0;
    const uint32_t start =
        with_top ? physical : mip_address ? mip_address : physical + static_cast<uint32_t>(top_bytes);
    const size_t from = with_top ? at : at - top_bytes;
    if (from + bytes > kMaxTextureBytes || PhysicalRoom(static_cast<size_t>(start) + from) == 0) {
      levels.clear();
      return;
    }
    levels.emplace_back();
    DecodeSurface(GpuPointer(base, start) + from, std::min(bytes, PhysicalRoom(static_cast<size_t>(start) + from)), f,
                  format, endian, swizzle, true, pitch, x / f.block, y / f.block, w, h, levels.back());
  }
}

// ---------------------------------------------------------------------------------------------------------------
// ngl_vk


}  // namespace waw::native
