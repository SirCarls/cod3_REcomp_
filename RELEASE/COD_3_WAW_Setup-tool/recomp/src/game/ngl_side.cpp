// cod3 - [acct2-bro] 2026-11-05 the frame's 2D (and a first try at its 3D) drawn natively by ngl_vk, beside the GPU
// emulation: steps 3 and 4 of dev/agents/acct2-bro/COD3-NGL-PLAN.md (owner 2026-11-05 09:11: "NGL renderer, BRO's
// way"). A check only: the game keeps drawing through the GPU emulation, and with cod3_debug_ngl_side off (the
// default) nothing here runs.
//
// With it on, every 10 s for the first 80 s (frames 600, 1200, ... 4800 counted from when it is turned on), at the
// start of nglListSend, the frame's NGL lists are read the way NGL's own back end walks them:
//   - scenes from the root (0x8290E9A8), each scene's children first (first child +0x2F8, next sibling +0x2F4);
//   - only scenes drawn to the 640x480 virtual screen (their color target, +0x328, has flag 0x2000 at +0x18; a scene
//     with no target counts too);
//   - per scene the opaque list (+0x334) in the order the quads came, then the blended list (+0x338) by its key,
//     largest first (Big Red One's order, std_InsertionSort_TranslucentNodes); both lists are pushed at their head, so
//     they are read backwards. Then a third list (+0x344, count +0x348) where 0x823F2DB0 puts the grass
//     (GrassRenderNode, vtable 0x8207B798; its Render, 0x823F79B0, calls 0x823F6DF0 with node +0x0C and +0x10).
//     A node: +0x00 its vtable (quads 0x82018020, strings 0x82018A20), +0x04 next, +0x08 key, +0x10 the quad's
//     0x68 bytes (four 0x14-byte vertices x, y, u, v, color; +0x60 the NGL texture).
//   - a string node (0x30 bytes, on the blended list): +0x0C the text, +0x10 the font, +0x14 x, +0x18 y, +0x1C z,
//     +0x20 / +0x24 the x and y scale, +0x28 the color; laid out the way Big Red One's nglBuildTextGlyphs does
//     (bro/src/ngl/ngl_font.c) from the font's glyph records (0x1C bytes a glyph: x, y, width, height, x and y
//     offsets, advance; CoD3's font layout is below, at kFontTexture).
// The quads and glyphs are drawn by ngl_vk (the library Big Red One's port draws everything with) into a 1280x720
// picture, the 640x480 screen stretched to it as the game does, saved as
// userdata/logs/screenshots/ngl-side-<frame>-vk.ppm, and the game's own screenshot of the same moment is asked for
// (report-*.png, named in the log). The first dump, and the first with a dozen quads or more, list every quad and
// string; each dump counts the other nodes in the lists (the 3D ones) by vtable.
//
// Textures are read from guest memory and decoded on the CPU, once each: the NGL texture's Direct3D texture (+0x14)
// carries a Xenos fetch constant at +0x1C (tiled bit and pitch, format and endianness, base address, size, swizzle).
// Formats 8888, DXT1, DXT2/3 and DXT4/5 are decoded (all the HUD and menus use in the census); others are drawn as
// their color alone and named once in the log. Tiled textures are untiled with the Xenos 2D tiling (as rexglue's and
// Xenia's texture code computes it). A texture 16 texels or less on a side with packed mips (fetch constant dword 5
// bit 11) keeps its top level 16 texels into the packed tile: down when it is wider than tall, else across.
//
// Step 4's first try at 3D: the generic surface nodes (kind 0x8201C3FC, Render 0x82143F70) drawn as meshes under the
// 2D, untextured (a color for each material) and lit by ngl_vk's fixed light, each scene with its own camera. What is
// read (runs acct2-bro-side6 and -side7):
//   - node +0x0C the object: its world matrix at +0x00 (Direct3D's 4x4, row-vector; its w column is left 0);
//   - node +0x10 the surface: +0x10 / +0x20 its box (smallest and largest corner, in its own space); at +0x40 and
//     +0x50 two buffers, each {Direct3D buffer, start, count, index size}. Which is which the buffer tells: a vertex
//     buffer's fetch constant (+0x18) has type 3 in its low two bits; an index buffer has its address there and its
//     size in bytes at +0x1C. +0x70 is taken as the vertex stride in bytes;
//   - the scene: +0x130 the projection, +0x170 the view, +0x1B0 their product (Direct3D's, row-vector). Only scenes
//     with a perspective camera are drawn. When the projection's depth runs backward (near 1, far 0), z is turned
//     over (w - z) for ngl_vk, whose depth runs near 0, far 1. +0x3D8..+0x3E4 and +0x3F0 its fog (FogWith), the way
//     Big Red One's nglSetFog keeps it (from 2026-11-05 13:50).
// A vertex's position is taken as three floats at its start; a surface whose first corners fall outside its box is
// left out, and every reason a surface is left out is counted in the log.
//
// cod3mp (side87) uses its own addresses for the same globals, vtables and functions (the decompile's tables, see
// kRootScene); the scenes' and nodes' offsets are the same NGL's in both programs.
//
// Direct3D's state at the draws (from 2026-11-05 13:00): after dumps 3 and 6 the next frames' draws read the device's
// shader constants (ngl_census.cpp's hooks on Direct3D's two draws call OnDraw); see kDevicePointer. The grass's
// draws save its shaders and vertex buffers (from 2026-11-05 15:40); see kGrassDrawReturn. The grass itself is drawn
// by ngl_vk's translation of the game's grass shaders (from 16:10); see GrassFor.

#include "game/ngl_side.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "game/guest.h"
#include "game/hd_textures.h"
#include "game/screenshot.h"

#if defined(COD3_NGL_VK)
extern "C" {
#include "ngl_vk.h"
}
#endif

REXCVAR_DEFINE_BOOL(cod3_debug_ngl_side, false, "cod3/Debug",
                    "Every 10 s for 80 s, draw the frame's 2D and its plain 3D surfaces with the native renderer "
                    "(ngl_vk) and save it beside the game's own screenshot (userdata/logs/screenshots/ngl-side-*); a "
                    "check for the native renderer")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_INT32(cod3_debug_ngl_side_arms, 0, "cod3/Debug",
                     "With cod3_debug_ngl_side, how the first-person arms (the front scene's skinned surfaces) are drawn "
                     "in the native picture: 0 as the rest, 1 plain white and unlit, 2 their texture unlit, 3 plain and "
                     "lit (to see where they land and what hides them)")
    .range(0, 3)
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_debug_ngl_side_shadow, true, "cod3/Debug",
                    "With cod3_debug_ngl_side, the sun's shadow in the native picture: the soldiers, props and trees "
                    "cast it and their sun is dimmed by it, as the game's sun cascades do")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace cod3::ngl_side {
namespace {

namespace g = cod3::guest;

// cod3mp's twins of the campaign's addresses below are from dev/waw-decomp/cod3/mp/globals_sp_to_mp.tsv (the votes
// in brackets) and cod3_relatives.tsv (side87); nothing else differs between the two programs here.
#if defined(COD3_MP)
constexpr uint32_t kRootScene = 0x82A8C130;       // (5 of 5)
constexpr uint32_t kListEndPointer = 0x825B2570;  // u32: the lists' end marker (12 of 12)
constexpr uint32_t kCurrentScene = 0x82A8C134;    // NGL's scene being drawn (133 of 133)
constexpr uint32_t kImageEnd = 0x82C00000;        // past the globals Readable follows (cod3mp's run to 0x82DD6000)
#else
constexpr uint32_t kRootScene = 0x8290E9A8;
constexpr uint32_t kListEndPointer = 0x825A1B34;  // u32: the lists' end marker
constexpr uint32_t kCurrentScene = 0x8290E9AC;    // NGL's scene being drawn
constexpr uint32_t kImageEnd = 0x82A00000;
#endif
// The blocks a compiled shader pass's constant lists index (bases 3, 4 and 6; base 7 through a pointer), and the
// tree parts' vertex formats (ShowTree).
#if defined(COD3_MP)
constexpr uint32_t kPassBase3 = 0x82A8E7F0, kPassBase4 = 0x82A8E1B0, kPassBase6 = 0x82A8F3F0;  // (14, 6, 5 of all)
constexpr uint32_t kPassBase7Pointer = 0x82A8E0C0;                                                // (6 of 6)
constexpr uint32_t kBranchFormat = 0x82BAD550, kFrondFormat = 0x82BAD544, kLeafFormat = 0x82BAD538;
#else
constexpr uint32_t kPassBase3 = 0x829118E0, kPassBase4 = 0x829112A0, kPassBase6 = 0x829124E0;
constexpr uint32_t kPassBase7Pointer = 0x829111B0;
constexpr uint32_t kBranchFormat = 0x82A1B8F4, kFrondFormat = 0x82A1B8E8, kLeafFormat = 0x82A1B8DC;
#endif
constexpr uint32_t kSceneNextSibling = 0x2F4;
constexpr uint32_t kSceneFirstChild = 0x2F8;
constexpr uint32_t kSceneColorTarget = 0x328;
constexpr uint32_t kSceneOpaque = 0x334;
constexpr uint32_t kSceneBlended = 0x338;
constexpr uint32_t kSceneLate = 0x344;  // a third list (count +0x348): the grass (GrassRenderNode, 0x823F2DB0)
constexpr uint32_t kTargetFlags = 0x18;
constexpr uint32_t kTargetVirtualScreen = 0x2000;
constexpr uint32_t kNodeNext = 0x04;
constexpr uint32_t kNodeKey = 0x08;
constexpr uint32_t kNodePayload = 0x10;
#if defined(COD3_MP)
constexpr uint32_t kQuadNode = 0x820193E0;     // (1 of 1, each of these vtables)
constexpr uint32_t kStringNode = 0x82019DF0;
constexpr uint32_t kGenericNode = 0x8201D75C;
constexpr uint32_t kTreeNode = 0x8207A7A0;
constexpr uint32_t kGrassNode = 0x8207A700;
#else
constexpr uint32_t kQuadNode = 0x82018020;
constexpr uint32_t kStringNode = 0x82018A20;
constexpr uint32_t kGenericNode = 0x8201C3FC;  // the generic surface node (Render 0x82143F70)
constexpr uint32_t kTreeNode = 0x8207B838;     // SpeedTree and grass (Render 0x820C84F0)
constexpr uint32_t kGrassNode = 0x8207B798;    // GrassRenderNode (Render 0x823F79B0; see kGrassDrawReturn)
#endif
// side90: the old material classes' render nodes. For a mesh whose material is one of the old classes (cdSimple,
// cdWorld, ...: ngl_census.cpp's kDraws), nglListAddMesh makes a render node whose Render (its vtable's third entry)
// is that class's draw, laid out as the generic node is: +0x0C the object (its matrix first), +0x10 the surface, +0x14
// the material. side90mp found them on cod3mp's mp_merv (cdSimple's x7, cdWorld's x2: the crates' planks); the forest
// has none. Their materials have no sampler list: the color map is the first texture the material holds, and a
// cdWorld's lightmap the second.
#if defined(COD3_MP)
constexpr uint32_t kSimpleDraw = 0x820F2B50, kWorldDraw = 0x820F80B0;
#else
constexpr uint32_t kSimpleDraw = 0x820E0AB8, kWorldDraw = 0x820E6070;
#endif
constexpr uint32_t kVtableRender = 0x08;
enum OldClass { kNotOld, kOldSimple, kOldWorld };
constexpr uint32_t kQuadVertexBytes = 0x14;
constexpr uint32_t kQuadTexture = 0x60;
constexpr uint32_t kStringText = 0x0C;
constexpr uint32_t kStringFont = 0x10;
constexpr uint32_t kStringX = 0x14;
constexpr uint32_t kStringY = 0x18;
constexpr uint32_t kStringScaleX = 0x20;
constexpr uint32_t kStringScaleY = 0x24;
constexpr uint32_t kStringColor = 0x28;
// CoD3's font as the game holds it (run acct2-bro-side3; the on-disc FONT asset is in acct2-ocr's FONTS.md): +0x00
// its name, +0x04 the atlas (an NGL texture), +0x08 the glyph records, +0x10 / +0x14 its quads' blend (6, 0x07C00000),
// +0x18 0x102, +0x1C the line height, +0x20 the ascent, +0x24 the first code (0x20), +0x28 the glyph count (224),
// +0x2C the UV table (atlas pixels), then the records themselves at +0x38.
constexpr uint32_t kFontTexture = 0x04;
constexpr uint32_t kFontGlyphs = 0x08;
constexpr uint32_t kFontCellHeight = 0x1C;
constexpr uint32_t kFontFirst = 0x24;
constexpr uint32_t kFontCount = 0x28;
constexpr uint32_t kGlyphBytes = 0x1C;
constexpr uint32_t kTextureWidth = 0x00;
constexpr uint32_t kTextureHeight = 0x04;
constexpr uint32_t kTextureName = 0x10;  // a string object, its characters at +4 (0x8213F8A0)
constexpr uint32_t kTextureDirect3D = 0x14;
constexpr uint32_t kTextureFlags = 0x18;
constexpr uint32_t kTextureAnimated = 4;
constexpr uint32_t kTextureFrames = 0x28;
constexpr uint32_t kFetchConstant = 0x1C;
constexpr float kScreenWidth = 640.0f;
constexpr float kScreenHeight = 360.0f;  // the 640x480 screen squeezed to 16:9, so it fills the picture as in the game
constexpr float kScreenSqueeze = kScreenHeight / 480.0f;
constexpr uint32_t kTargetWidth = 1280;
constexpr uint32_t kTargetHeight = 720;
constexpr uint64_t kDumpEvery = 600;  // frames
constexpr int kDumps = 8;
constexpr uint32_t kMaxScenes = 256;
constexpr size_t kMaxNodes = 20000;
constexpr size_t kMaxTextureBytes = 32u << 20;
constexpr size_t kMaxText = 1024;
constexpr int kListedQuads = 48;  // quads and strings listed by the first big dump
// the generic surface node and what it points to (see the top)
constexpr uint32_t kNodeObject = 0x0C;
constexpr uint32_t kNodeSurface = 0x10;
constexpr uint32_t kNodeMaterial = 0x14;
constexpr uint32_t kSurfaceBoxLow = 0x10;
constexpr uint32_t kSurfaceBoxHigh = 0x20;
constexpr uint32_t kSurfaceBuffers = 0x40;  // two of {Direct3D buffer, start, count, index size}, 0x10 bytes apart
constexpr uint32_t kSurfaceFormat = 0x60;
constexpr uint32_t kSurfaceStride = 0x70;
constexpr uint32_t kBufferFetch = 0x18;
constexpr uint32_t kSceneProjection = 0x130;
constexpr uint32_t kSceneViewProjection = 0x1B0;
constexpr int kShownSurfaces = 6;   // generic surfaces shown in full, once
constexpr size_t kShownScenes = 6;  // 3D scenes shown (their cameras), once each
constexpr uint32_t kMaxDraws = 16384;

bool Pointer(uint32_t p) { return p >= 0x10000 && (p & 3) == 0; }

// A physical address (as fetch constants hold them) as the guest sees it; others are already guest addresses.
uint32_t GuestAddress(uint32_t address) { return address < 0x20000000u ? (0xA0000000u | address) : address; }

// Inside the guest's views of physical memory (0xA0000000 and up), which are mapped whole, so reading is safe.
bool Physical(uint32_t address) { return address >= 0xA0000000u && address < 0xFFF00000u; }

// Physical memory or the program's own image (code and data, 0x82000000 to the globals past 0x82900000), where some
// materials keep their sampler lists.
bool Readable(uint32_t address) {
  return !(address & 3) && (Physical(address) || (address >= 0x82000000u && address < kImageEnd));
}

std::string Words(uint8_t* base, uint32_t at, uint32_t count) {
  std::string text;
  char item[12];
  for (uint32_t k = 0; k < count; ++k) {
    std::snprintf(item, sizeof item, "%s%08X", k ? " " : "", g::ReadU32(base, at + 4 * k));
    text += item;
  }
  return text;
}

#if defined(COD3_NGL_VK)
// ---------------------------------------------------------------------------------------------------------------
// Xenos textures to BGRA8.

struct Format {
  uint32_t block;   // texels across a block (1 or 4)
  uint32_t bytes;   // bytes a block
  uint32_t log2_bytes;
};

bool FormatOf(uint32_t format, Format* f) {
  switch (format) {
    case 6:  // k_8_8_8_8
      *f = {1, 4, 2};
      return true;
    case 18:  // k_DXT1
      *f = {4, 8, 3};
      return true;
    case 19:  // k_DXT2_3
    case 20:  // k_DXT4_5
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

void SwapBlock(uint8_t* b, uint32_t bytes, uint32_t endian) {
  for (uint32_t i = 0; i + 3 < bytes; i += 4) {
    uint8_t* w = b + i;
    switch (endian) {
      case 1:  // 8 in 16
        std::swap(w[0], w[1]);
        std::swap(w[2], w[3]);
        break;
      case 2:  // 8 in 32
        std::swap(w[0], w[3]);
        std::swap(w[1], w[2]);
        break;
      case 3:  // 16 in 32
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

void DecodeBlock(const uint8_t* b, uint32_t format, uint8_t texels[16][4]) {
  if (format == 18) {
    DecodeColors(b, false, texels);
  } else if (format == 19) {
    DecodeColors(b + 8, true, texels);
    for (int t = 0; t < 16; ++t) {
      const uint32_t a = (b[t / 2] >> (4 * (t & 1))) & 15;
      texels[t][3] = static_cast<uint8_t>(a * 17);
    }
  } else {  // 20
    DecodeColors(b + 8, true, texels);
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
    for (int t = 0; t < 16; ++t) texels[t][3] = static_cast<uint8_t>(a[(bits >> (3 * t)) & 7]);
  }
}

uint32_t Log2Ceil(uint32_t v) {
  uint32_t l = 0;
  while (l < 31 && (1u << l) < v) ++l;
  return l;
}

// One surface's texels as BGRA8 rows: width x height texels in blocks of format f from `source` (`bytes` long), tiled
// or not with pitch_blocks blocks a row, the first block at (offset_x, offset_y) in blocks (a level inside a packed
// surface); each texel's channels picked by the fetch constant's swizzle.
void DecodeSurface(const uint8_t* source, size_t bytes, const Format& f, uint32_t format, uint32_t endian,
                   uint32_t swizzle, bool tiled, uint32_t pitch_blocks, uint32_t offset_x, uint32_t offset_y,
                   uint32_t width, uint32_t height, std::vector<uint8_t>& bgra) {
  const uint32_t blocks_x = (width + f.block - 1) / f.block, blocks_y = (height + f.block - 1) / f.block;
  bgra.assign(static_cast<size_t>(width) * height * 4, 0);
  uint8_t block[16];
  uint8_t texels[16][4];
  for (uint32_t by = 0; by < blocks_y; ++by) {
    const int32_t sy = static_cast<int32_t>(by + offset_y);
    const int32_t row = tiled ? TiledRow(sy, static_cast<int32_t>(pitch_blocks), f.log2_bytes) : 0;
    for (uint32_t bx = 0; bx < blocks_x; ++bx) {
      const int32_t sx = static_cast<int32_t>(bx + offset_x);
      const uint32_t index = tiled ? static_cast<uint32_t>(TiledOffset(sx, sy, f.log2_bytes, row)) >> f.log2_bytes
                                   : static_cast<uint32_t>(sy) * pitch_blocks + static_cast<uint32_t>(sx);
      if (static_cast<size_t>(index) * f.bytes + f.bytes > bytes) continue;
      std::memcpy(block, source + static_cast<size_t>(index) * f.bytes, f.bytes);
      SwapBlock(block, f.bytes, endian);
      if (f.block == 1) {
        std::memcpy(texels[0], block, 4);
      } else {
        DecodeBlock(block, format, texels);
      }
      for (uint32_t ty = 0; ty < f.block; ++ty) {
        for (uint32_t tx = 0; tx < f.block; ++tx) {
          const uint32_t x = bx * f.block + tx, y = by * f.block + ty;
          if (x >= width || y >= height) continue;
          const uint8_t* t = texels[ty * f.block + tx];
          // the fetch constant's swizzle picks each channel: 0-3 a channel, 4 zero, 5 one
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

// A Direct3D texture's top level as BGRA8 rows; false (and the reason) when it can't be read.
// A Direct3D texture's fetch constant: the six words the GPU takes it by (+0x1C in the texture object, or a fetch
// constant of the device's for a draw's textures, side85).
bool ReadFetch(uint8_t* base, uint32_t d3d, uint32_t fetch[6]) {
  if (!Pointer(d3d)) return false;
  for (int i = 0; i < 6; ++i) fetch[i] = g::ReadU32(base, d3d + kFetchConstant + 4 * i);
  return true;
}

bool DecodeFetch(uint8_t* base, const uint32_t* fetch, std::vector<uint8_t>& bgra, uint32_t& width,
                 uint32_t& height, std::string& why) {
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
  DecodeSurface(base + (0xA0000000u | physical), bytes, f, format, endian, swizzle, tiled, pitch_blocks, offset_x,
                offset_y, width, height, bgra);
  return true;
}

bool Decode(uint8_t* base, uint32_t d3d, std::vector<uint8_t>& bgra, uint32_t& width, uint32_t& height,
            std::string& why) {
  uint32_t fetch[6];
  if (!ReadFetch(base, d3d, fetch)) {
    why = "no Direct3D texture";
    return false;
  }
  return DecodeFetch(base, fetch, bgra, width, height, why);
}

// The game's own smaller levels of a Direct3D texture (1 to its last), BGRA8 rows each, from where the GPU keeps them
// (acct2-decomp's for-acct2-bro/TEXTURE-LEVELS.md and tools/cod_convert/xtex.py layout(), checked on 7,169 of the
// game's textures and in the game): after the top level each level is a tiled surface of its own, pitch and rows
// rounded up to 32 blocks, until the first level whose shorter side is 16 texels or less; in a packed texture that
// level and every smaller one share one surface, each at GetPackedMipOffset's place. Levels 1 on start at the fetch
// constant's mip address when it has one (side75: soldiers' far levels came out grey when they were taken from right
// after the top level), else right after the top level. Only tiled 2D textures whose sides are powers of two; for
// others `levels` stays empty (the top level alone, as before).
void DecodeLevelsFetch(uint8_t* base, const uint32_t* fetch, std::vector<std::vector<uint8_t>>& levels) {
  levels.clear();
  const uint32_t format = fetch[1] & 0x3F, endian = (fetch[1] >> 6) & 3;
  const uint32_t physical = fetch[1] & 0x1FFFF000;
  const uint32_t width = (fetch[2] & 0x1FFF) + 1, height = ((fetch[2] >> 13) & 0x1FFF) + 1;
  const uint32_t swizzle = (fetch[3] >> 1) & 0xFFF;
  const uint32_t last = (fetch[4] >> 6) & 0xF;
  const uint32_t mip_address = fetch[5] & 0x1FFFF000;
  const bool tiled = (fetch[0] >> 31) != 0, packed = ((fetch[5] >> 11) & 1) != 0;
  Format f;
  if (!tiled || !last || !physical || !FormatOf(format, &f) || (width & (width - 1)) || (height & (height - 1))) return;
  const uint32_t log2_w = Log2Ceil(width), log2_h = Log2Ceil(height);
  const uint32_t first_packed = std::min(log2_w, log2_h) > 4 ? std::min(log2_w, log2_h) - 4 : 0;
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
      if (step < 3) {
        (log2_w > log2_h ? y : x) = 16u >> step;
      } else {
        (log2_w > log2_h ? x : y) = (1u << ((log2_w > log2_h ? log2_w : log2_h) - first_packed)) >> (step - 2);
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
    if (level == 0) continue;  // the top level is Decode's
    const size_t bytes = static_cast<size_t>(pitch) * rows * f.bytes;
    // a surface shared with the top level is at the base; every other is counted from where levels 1 on start
    const uint32_t start = (shared && first_packed == 0) ? physical
                           : mip_address ? mip_address : physical + static_cast<uint32_t>(top_bytes);
    const size_t from = (shared && first_packed == 0) ? at : at - top_bytes;
    if (from + bytes > kMaxTextureBytes) {
      levels.clear();
      return;
    }
    static int starts_shown = 0;  // diagnostics: where levels 1 on are, beside right after the top level
    if (level == 1 && starts_shown < 16) {
      ++starts_shown;
      REXLOG_INFO("cod3: ngl side: texture levels: base {:08X}, {}x{} format {}, {} levels, mip address {:08X}, top "
                  "level's end {:08X}",
                  physical, width, height, format, last + 1, mip_address, physical + static_cast<uint32_t>(top_bytes));
    }
    levels.emplace_back();
    DecodeSurface(base + (0xA0000000u | start) + from, bytes, f, format, endian, swizzle, true, pitch, x / f.block,
                  y / f.block, w, h, levels.back());
  }
}

// Smaller levels for a picture that has none of the game's (from an HD or user pack): each the level above averaged
// 2 x 2, down to 1 x 1. Sides that are powers of two only; otherwise none.
void BoxLevels(const std::vector<uint8_t>& top, uint32_t width, uint32_t height,
               std::vector<std::vector<uint8_t>>& levels) {
  levels.clear();
  if ((width & (width - 1)) || (height & (height - 1))) return;
  const std::vector<uint8_t>* above = &top;
  uint32_t w = width, h = height;
  while (w > 1 || h > 1) {
    const uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
    std::vector<uint8_t> level(static_cast<size_t>(nw) * nh * 4);
    for (uint32_t y = 0; y < nh; ++y) {
      for (uint32_t x = 0; x < nw; ++x) {
        for (int c = 0; c < 4; ++c) {
          uint32_t sum = 0;
          for (uint32_t dy = 0; dy < 2; ++dy) {
            for (uint32_t dx = 0; dx < 2; ++dx) {
              const uint32_t sx = std::min(w - 1, 2 * x + dx), sy = std::min(h - 1, 2 * y + dy);
              sum += (*above)[(static_cast<size_t>(sy) * w + sx) * 4 + c];
            }
          }
          level[(static_cast<size_t>(y) * nw + x) * 4 + c] = static_cast<uint8_t>((sum + 2) / 4);
        }
      }
    }
    levels.push_back(std::move(level));
    above = &levels.back();
    w = nw;
    h = nh;
  }
}

// A cut-out's smaller levels keep the top level's share of texels at alpha 128 or over (as xtex.py import's
// --alpha auto): each level's alpha is scaled so that the same share passes the cut. The game's own levels thin like
// a box filter (acct2-decomp 17:06: Saint-Lô's net goes from 28 % to 0 by 16 x 16), so far away a cut-out would
// fade out.
void KeepCoverage(const std::vector<uint8_t>& top, std::vector<std::vector<uint8_t>>& levels) {
  size_t passing = 0;
  for (size_t at = 3; at < top.size(); at += 4) passing += top[at] >= 128;
  const double share = top.empty() ? 0.0 : static_cast<double>(passing) / static_cast<double>(top.size() / 4);
  if (share <= 0.0) return;
  for (std::vector<uint8_t>& level : levels) {
    std::vector<uint8_t> alpha;
    for (size_t at = 3; at < level.size(); at += 4) alpha.push_back(level[at]);
    if (alpha.empty()) continue;
    const size_t keep = std::max<size_t>(1, static_cast<size_t>(share * static_cast<double>(alpha.size()) + 0.5));
    std::nth_element(alpha.begin(), alpha.begin() + static_cast<std::ptrdiff_t>(keep - 1), alpha.end(),
                     std::greater<uint8_t>());
    const uint8_t cut = alpha[keep - 1];  // the keep-th largest alpha
    if (cut == 0) continue;
    const double scale = 128.0 / cut;
    for (size_t at = 3; at < level.size(); at += 4) {
      level[at] = static_cast<uint8_t>(std::min(255.0, std::floor(level[at] * scale + 0.5)));
    }
  }
}

// ---------------------------------------------------------------------------------------------------------------
// The HD and user texture packs on the native path (owner 15:11: "make sure you know about the cod3 hd textures and
// the originals"). The setup tool rips every texture (data/cod3/textures/, hd_keys.tsv), upscales the color maps
// into data/cod3/hd_textures/ and builds the player's own pictures into data/cod3/user_textures/
// (docs/TEXTURE_REPLACEMENT.md, game/hd_textures.h). The GPU emulation swaps them in (patch 0016); ngl_vk is handed
// the game's own texture here, so the same lookup is made: the same key (patch 0016's ComputeKey), the same index
// lines, the user pack over the HD pack, the original's alpha kept. Only DXT1/3/5 for now (the color maps); a
// multiplayer match takes no replacements on this path yet.

std::filesystem::path ScreenshotFolder();  // below: <game folder>/userdata/logs/screenshots

// XXH64 with seed 0, the packs' hash (checked against the published vectors: "" EF46DB3751D8E999, "abc"
// 44BC2CF5AD770999).
uint64_t Xxh64(const uint8_t* p, size_t n) {
  constexpr uint64_t k1 = 11400714785074694791ull, k2 = 14029467366897019727ull, k3 = 1609587929392839161ull,
                     k4 = 9650029242287828579ull, k5 = 2870177450012600261ull;
  auto rotl = [](uint64_t x, int r) { return (x << r) | (x >> (64 - r)); };
  auto read64 = [](const uint8_t* q) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; --i) v = (v << 8) | q[i];
    return v;
  };
  auto step = [&](uint64_t acc, uint64_t v) { return rotl(acc + v * k2, 31) * k1; };
  size_t i = 0;
  uint64_t h;
  if (n >= 32) {
    uint64_t v[4] = {k1 + k2, k2, 0, 0ull - k1};
    for (; i + 32 <= n; i += 32) {
      for (int lane = 0; lane < 4; ++lane) v[lane] = step(v[lane], read64(p + i + 8 * lane));
    }
    h = rotl(v[0], 1) + rotl(v[1], 7) + rotl(v[2], 12) + rotl(v[3], 18);
    for (const uint64_t lane : v) h = (h ^ step(0, lane)) * k1 + k4;
  } else {
    h = k5;
  }
  h += n;
  for (; i + 8 <= n; i += 8) h = rotl(h ^ step(0, read64(p + i)), 27) * k1 + k4;
  if (i + 4 <= n) {
    const uint64_t word =
        uint64_t(p[i]) | uint64_t(p[i + 1]) << 8 | uint64_t(p[i + 2]) << 16 | uint64_t(p[i + 3]) << 24;
    h = rotl(h ^ (word * k1), 23) * k2 + k3;
    i += 4;
  }
  for (; i < n; ++i) h = rotl(h ^ (p[i] * k5), 11) * k1;
  h ^= h >> 33;
  h *= k2;
  h ^= h >> 29;
  h *= k3;
  return h ^ (h >> 32);
}

// A texture's pack key: its top level's blocks gathered into rows without padding (from inside its packed mip tile
// when the smaller side is 16 or less), byte-swapped as the GPU reads them, hashed. As patch 0016's ComputeKey: the
// pitch is at least the width, and a tiled pitch is aligned to 32 blocks.
bool TextureKeyFetch(uint8_t* base, const uint32_t* fetch, uint64_t& key, uint32_t& format_out) {
  const bool tiled = (fetch[0] >> 31) != 0;
  const uint32_t pitch = ((fetch[0] >> 22) & 0x1FF) * 32;
  const uint32_t format = fetch[1] & 0x3F, endian = (fetch[1] >> 6) & 3;
  const uint32_t physical = fetch[1] & 0x1FFFF000;
  const uint32_t width = (fetch[2] & 0x1FFF) + 1, height = ((fetch[2] >> 13) & 0x1FFF) + 1;
  const bool packed = ((fetch[5] >> 11) & 1) != 0;
  Format f;
  if (!FormatOf(format, &f) || physical == 0) return false;
  const uint32_t blocks_x = (width + f.block - 1) / f.block, blocks_y = (height + f.block - 1) / f.block;
  uint32_t offset_x = 0, offset_y = 0;
  if (packed && std::min(Log2Ceil(width), Log2Ceil(height)) <= 4) {
    (Log2Ceil(width) > Log2Ceil(height) ? offset_y : offset_x) = 16 / f.block;
  }
  uint32_t pitch_blocks = std::max(pitch / f.block, blocks_x);
  if (tiled) pitch_blocks = (pitch_blocks + 31) & ~31u;
  const size_t span = static_cast<size_t>(pitch_blocks) * (((blocks_y + offset_y) + 31) & ~31u) * f.bytes;
  if (span > kMaxTextureBytes) return false;
  const uint8_t* source = base + (0xA0000000u | physical);
  std::vector<uint8_t> gathered(static_cast<size_t>(blocks_x) * blocks_y * f.bytes);
  uint8_t* out = gathered.data();
  for (uint32_t by = 0; by < blocks_y; ++by) {
    const int32_t sy = static_cast<int32_t>(by + offset_y);
    const int32_t row = tiled ? TiledRow(sy, static_cast<int32_t>(pitch_blocks), f.log2_bytes) : 0;
    for (uint32_t bx = 0; bx < blocks_x; ++bx, out += f.bytes) {
      const int32_t sx = static_cast<int32_t>(bx + offset_x);
      const size_t at = tiled ? static_cast<size_t>(TiledOffset(sx, sy, f.log2_bytes, row))
                              : (static_cast<size_t>(sy) * pitch_blocks + static_cast<size_t>(sx)) * f.bytes;
      if (at + f.bytes > span) return false;
      std::memcpy(out, source + at, f.bytes);
      SwapBlock(out, f.bytes, endian);
    }
  }
  key = Xxh64(gathered.data(), gathered.size());
  format_out = format;
  return true;
}

bool TextureKey(uint8_t* base, uint32_t d3d, uint64_t& key, uint32_t& format_out) {
  uint32_t fetch[6];
  return ReadFetch(base, d3d, fetch) && TextureKeyFetch(base, fetch, key, format_out);
}

struct PackEntry {
  std::filesystem::path file;
  uint32_t width, height, format;
  bool user;
};
std::unordered_map<uint64_t, PackEntry> g_pack;
bool g_pack_read = false;
size_t g_pack_bytes = 0;                               // BGRA bytes handed to ngl_vk from the packs
constexpr size_t kPackBudget = size_t(1536) << 20;     // beyond it, the game's own textures
constexpr uint32_t kPackSide = 2048;                    // the largest side taken (a smaller mip of a larger file)

// A pack's file name is plain: relative, no "..", no backslash, drive letter or control character (as the SDK's
// IsPlainPackFile).
bool PlainPackFile(const std::string& name) {
  if (name.empty() || name.size() > 255 || name[0] == '/') return false;
  for (const char c : name) {
    if (static_cast<unsigned char>(c) < 0x20 || c == '\\' || c == ':') return false;
  }
  return name.find("..") == std::string::npos;
}

void ReadPack(const std::filesystem::path& folder, bool user) {
  std::error_code error;
  if (std::filesystem::file_size(folder / "index.tsv", error) > (size_t(64) << 20) || error) return;
  std::ifstream index(folder / "index.tsv");
  std::string line;
  int taken = 0;
  while (std::getline(index, line)) {
    if (line.empty() || line[0] == '#') continue;
    std::vector<std::string> field;
    size_t from = 0;
    for (size_t tab; (tab = line.find('\t', from)) != std::string::npos; from = tab + 1) {
      field.push_back(line.substr(from, tab - from));
    }
    field.push_back(line.substr(from));
    if (field.size() < 5 || field[0].size() != 16 || !PlainPackFile(field[4])) continue;
    char* end = nullptr;
    const uint64_t key = std::strtoull(field[0].c_str(), &end, 16);
    if (*end) continue;
    PackEntry entry{folder / field[4], static_cast<uint32_t>(std::strtoul(field[1].c_str(), nullptr, 10)),
                    static_cast<uint32_t>(std::strtoul(field[2].c_str(), nullptr, 10)),
                    static_cast<uint32_t>(std::strtoul(field[3].c_str(), nullptr, 10)), user};
    g_pack[key] = entry;  // the user pack is read second, so its file wins
    ++taken;
  }
  REXLOG_INFO("cod3: ngl side: {} pack: {} textures ({})", user ? "user" : "HD", taken, folder.string());
}

void ReadPacks() {
  if (g_pack_read) return;
  g_pack_read = true;
  const std::filesystem::path root = ScreenshotFolder().parent_path().parent_path().parent_path();
  if (GetHdTextures() == HdTextures::kOn) ReadPack(root / "data" / "cod3" / "hd_textures", false);
  if (hd::GetUserTextures() == HdTextures::kOn) ReadPack(root / "data" / "cod3" / "user_textures", true);
  if (g_pack.empty()) {
    REXLOG_INFO("cod3: ngl side: no texture pack in use (HD textures: {}; user textures: {})",
                DescribeHdTextures(GetHdTextures()), DescribeHdTextures(hd::GetUserTextures()));
  }
}

// A pack's DDS file as BGRA rows, its largest level no larger than kPackSide a side, the guest's swizzle applied as
// Decode does; false (and the reason) when it can't be used. Every header value is checked before use.
bool DecodePackFile(const PackEntry& entry, uint32_t swizzle, std::vector<uint8_t>& bgra, uint32_t& width,
                    uint32_t& height, std::string& why) {
  static const char* kFourCc[3] = {"DXT1", "DXT3", "DXT5"};  // formats 18, 19, 20
  if (entry.format < 18 || entry.format > 20) {
    why = "format " + std::to_string(entry.format) + " not taken natively yet";
    return false;
  }
  std::ifstream file(entry.file, std::ios::binary);
  uint8_t header[128];
  if (!file.read(reinterpret_cast<char*>(header), sizeof header)) {
    why = "can't read";
    return false;
  }
  auto u32 = [&](int at) {
    return uint32_t(header[at]) | uint32_t(header[at + 1]) << 8 | uint32_t(header[at + 2]) << 16 |
           uint32_t(header[at + 3]) << 24;
  };
  const char* four_cc = kFourCc[entry.format - 18];
  if (std::memcmp(header, "DDS ", 4) || u32(4) != 124 || u32(76) != 32 || std::memcmp(header + 84, four_cc, 4)) {
    why = "not a plain DDS of the original's format";
    return false;
  }
  uint32_t w = u32(16), h = u32(12);
  const uint32_t levels = std::max(1u, u32(28));
  if (!w || !h || w > 16384 || h > 16384 || levels > 15) {
    why = "size out of range";
    return false;
  }
  Format f;
  FormatOf(entry.format, &f);
  auto level_bytes = [&](uint32_t lw, uint32_t lh) { return size_t((lw + 3) / 4) * ((lh + 3) / 4) * f.bytes; };
  size_t skip = 0;
  uint32_t level = 0;
  while (std::max(w, h) > kPackSide && level + 1 < levels) {
    skip += level_bytes(w, h);
    w = std::max(1u, w / 2);
    h = std::max(1u, h / 2);
    ++level;
  }
  if (std::max(w, h) > kPackSide) {
    why = "larger than " + std::to_string(kPackSide) + " without a smaller level";
    return false;
  }
  std::vector<uint8_t> blocks(level_bytes(w, h));
  if (!file.seekg(static_cast<std::streamoff>(sizeof header + skip)) ||
      !file.read(reinterpret_cast<char*>(blocks.data()), static_cast<std::streamsize>(blocks.size()))) {
    why = "shorter than its levels";
    return false;
  }
  width = w;
  height = h;
  bgra.assign(static_cast<size_t>(w) * h * 4, 0);
  uint8_t texels[16][4];
  const uint32_t blocks_x = (w + 3) / 4, blocks_y = (h + 3) / 4;
  for (uint32_t by = 0; by < blocks_y; ++by) {
    for (uint32_t bx = 0; bx < blocks_x; ++bx) {
      DecodeBlock(&blocks[(static_cast<size_t>(by) * blocks_x + bx) * f.bytes], entry.format, texels);
      for (uint32_t ty = 0; ty < 4; ++ty) {
        for (uint32_t tx = 0; tx < 4; ++tx) {
          const uint32_t x = bx * 4 + tx, y = by * 4 + ty;
          if (x >= w || y >= h) continue;
          const uint8_t* t = texels[ty * 4 + tx];
          uint8_t c[4];
          for (int k = 0; k < 4; ++k) {
            const uint32_t s = (swizzle >> (3 * k)) & 7;
            c[k] = s < 4 ? t[s] : s == 5 ? 255 : 0;
          }
          uint8_t* out = &bgra[(static_cast<size_t>(y) * w + x) * 4];
          out[0] = c[2];
          out[1] = c[1];
          out[2] = c[0];
          out[3] = c[3];
        }
      }
    }
  }
  return true;
}

// The pack's picture for a texture whose own top level is `original` (width x height), or false. Never more
// see-through than the original: an opaque original stays opaque, else each texel keeps at least its alpha.
bool PackPicture(uint8_t* base, const uint32_t* fetch, const std::vector<uint8_t>& original,
                 uint32_t original_width, uint32_t original_height, std::vector<uint8_t>& bgra, uint32_t& width,
                 uint32_t& height) {
  ReadPacks();
  if (g_pack.empty() || hd::IsMultiplayerMode() || g_pack_bytes >= kPackBudget) return false;
  uint64_t key;
  uint32_t format;
  if (!TextureKeyFetch(base, fetch, key, format)) return false;
  const auto found = g_pack.find(key);
  if (found == g_pack.end()) return false;
  static int shown = 0;
  std::string why;
  const uint32_t swizzle = (fetch[3] >> 1) & 0xFFF;
  if (found->second.format != format || !DecodePackFile(found->second, swizzle, bgra, width, height, why)) {
    if (why.empty()) why = "the index names another format";
    REXLOG_INFO("cod3: ngl side: {:016x}: can't use {}: {}", key, found->second.file.filename().string(), why);
    return false;
  }
  bool opaque = true;
  for (size_t at = 3; at < original.size() && opaque; at += 4) opaque = original[at] == 255;
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      uint8_t& alpha = bgra[(static_cast<size_t>(y) * width + x) * 4 + 3];
      if (opaque) {
        alpha = 255;
      } else {
        const size_t ox = static_cast<size_t>(x) * original_width / width;
        const size_t oy = static_cast<size_t>(y) * original_height / height;
        alpha = std::max(alpha, original[(oy * original_width + ox) * 4 + 3]);
      }
    }
  }
  g_pack_bytes += bgra.size();
  if (shown++ < 8) {
    REXLOG_INFO("cod3: ngl side: {:016x} {}x{} replaced with {} ({}x{}, {} pack)", key, original_width,
                original_height, found->second.file.filename().string(), width, height,
                found->second.user ? "user" : "HD");
  }
  return true;
}

#endif  // COD3_NGL_VK

// ---------------------------------------------------------------------------------------------------------------
// The frame's quads and strings.

struct Quad {
  float x[4], y[4], u[4], v[4];
  uint32_t color[4];
  uint32_t texture;     // the NGL texture
  uint32_t scene = 0;   // for the listing: the scene, the quad's +0x50..+0x5C (z, flags, blend, its argument)
  uint32_t extra[4] = {};
};

struct Text {  // a string node, for the listing
  uint32_t node, font, color;
  float x, y, scale_x, scale_y;
  int glyphs;
  std::string text;
};

struct Solid {  // a generic surface node (or an old material class's, side90) and its scene
  uint32_t scene, node;
  int old_class = kNotOld;
};

struct Census {
  int scenes = 0, offscreen_scenes = 0, quads = 0, strings = 0, glyphs = 0, other_nodes = 0;
  std::unordered_map<uint32_t, int> others;  // the other nodes by vtable
  std::vector<Text> texts;
  std::vector<Solid> solids;
  std::vector<Solid> trees;                       // SpeedTree nodes (kTreeNode) and their scenes
  std::vector<Solid> grass;                       // grass nodes (kGrassNode) and their scenes
  int far_solids = 0, far_trees = 0;              // of those, from the scenes' third (far) lists; far trees left out
  int old_nodes = 0;                              // of the solids, old material classes' nodes (side90)
  std::unordered_map<uint32_t, int> scene_order;  // the order scenes are drawn in (children first)
};

// side91: the first-person gun's objects (the front scene's), for OnPass and OnDraw to probe one of its draws: what
// the GPU is handed there, against what the native side draws it with (the gun comes out wider than the game's,
// though the object's own world-view-projection, +0x40, is what the native side computes).
std::mutex g_front_mutex;
std::unordered_set<uint32_t> g_front_objects;

// An NGL texture: its Direct3D texture (+0x14) carries a texture fetch constant (type 2 in its low two bits).
// Only pointers into the physical views (where the heap and textures are, mapped whole) are followed, as a word that
// isn't a pointer could point anywhere.
bool IsTexture(uint8_t* base, uint32_t texture) {
  if (!Readable(texture)) return false;
  const uint32_t d3d = g::ReadU32(base, texture + kTextureDirect3D);
  return Readable(d3d) && (g::ReadU32(base, d3d + kFetchConstant) & 3) == 2;
}

// Every texture a material points to (first 0x60 bytes, and one pointer deep in the first 0x40 bytes of each),
// for the listing: "+14: 1024x512 format 18, +28 -> +04: ...".
std::string MaterialTextures(uint8_t* base, uint32_t material, uint32_t bytes = 0x60) {
  std::string text;
  auto add = [&](const std::string& where, uint32_t texture) {
    const uint32_t d3d = g::ReadU32(base, texture + kTextureDirect3D);
    char item[96];
    std::snprintf(item, sizeof item, "%s%s: %08X %ux%u format %u", text.empty() ? "" : ", ", where.c_str(), texture,
                  g::ReadU32(base, texture + kTextureWidth), g::ReadU32(base, texture + kTextureHeight),
                  g::ReadU32(base, d3d + kFetchConstant + 4) & 0x3F);
    text += item;
  };
  char where[32];
  for (uint32_t at = 0; at < bytes; at += 4) {
    const uint32_t word = g::ReadU32(base, material + at);
    if (IsTexture(base, word)) {
      std::snprintf(where, sizeof where, "+%02X", at);
      add(where, word);
    } else if (Physical(word) && !(word & 3) && word != material) {
      for (uint32_t k = 0; k < 0x80; k += 4) {
        const uint32_t inner = g::ReadU32(base, word + k);
        if (!IsTexture(base, inner)) continue;
        std::snprintf(where, sizeof where, "+%02X -> +%02X", at, k);
        add(where, inner);
      }
    }
  }
  return text.empty() ? "none" : text;
}

// A material's sampler list (material +0x30): after two words, entries of 0x20 bytes: the effect parameter, the NGL
// texture, then the texture's fetch constant (6 words) as the sampler uses it. Seen (side12): for a rock 4 entries,
// a DXT5 normal map, the lightmap (parameter 0x829114E0; a global texture when the surface has none), a DXT3 color
// map, a DXT1; for the terrain the lightmap first (the whole map in one texture), again under 0x829114E0, then a
// DXT1 color map.
constexpr uint32_t kMaterialSamplers = 0x30;
constexpr uint32_t kSamplerFirst = 0x08;
constexpr uint32_t kSamplerBytes = 0x20;
#if defined(COD3_MP)
constexpr uint32_t kLightMapParameter = 0x82A8E3F0;  // (1 of 1)
#else
constexpr uint32_t kLightMapParameter = 0x829114E0;
#endif
constexpr uint32_t kMaxSamplers = 8;

struct Samplers {
  uint32_t color = 0, lightmap = 0;  // NGL textures, 0 for none
  uint32_t looks = 0;                // the set of looks the color map was picked from (side86), 0 for none
  uint32_t env = 0, env_mask = 0;    // side88: a shiny surface's environment map and its specular map
};

// side33: a sampler parameter's +0x28 points at its sampler state as the effect's own text ("Texture=<Normal_Map_
// Texture>; AddressU = Wrap; ..."), so the texture variable's name tells the maps apart ("" when there is none).
constexpr uint32_t kParameterSamplerText = 0x28;
std::string SamplerTextureName(uint8_t* base, uint32_t parameter) {
  const uint32_t text = Readable(parameter) ? g::ReadU32(base, parameter + kParameterSamplerText) : 0;
  if (!Pointer(text) || !Readable(text)) return "";
  std::string s;
  for (uint32_t k = 0; k < 160 && Readable((text + k) & ~3u); ++k) {  // Readable wants a word's address
    const uint8_t c = base[text + k];
    if (c < 0x20 || c > 0x7E) break;
    s += static_cast<char>(c);
  }
  const size_t at = s.find("Texture=<"), end = at == std::string::npos ? at : s.find('>', at);
  return end == std::string::npos ? "" : s.substr(at + 9, end - at - 9);
}

std::string Lower(const std::string& name) {
  std::string lower;
  for (char c : name) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
  return lower;
}

// Whether a texture variable's name says it is the color map, in any case: Color_Map_Texture, Diffuse_Texture,
// k98_diffuse_Texture, BSpringfield_diff_Texture, bullet_D_tga_Texture (side47: "_d" as a word is the diffuse).
bool ColorMapName(const std::string& name) {
  const std::string lower = Lower(name);
  for (const char* word : {"color", "colour", "diff", "albedo"}) {
    if (lower.find(word) != std::string::npos) return true;
  }
  const size_t d = lower.find("_d_");
  return d != std::string::npos || (lower.size() > 2 && lower.compare(lower.size() - 2, 2, "_d") == 0);
}

// Whether a texture variable's name says it is something else (side47: the gun's environment map was taken for its
// color map, so it came out gold).
bool OtherMapName(const std::string& name) {
  const std::string lower = Lower(name);
  for (const char* word : {"env", "reflect", "spec", "normal", "_n_", "mask", "noise", "swatch", "bump", "light"}) {
    if (lower.find(word) != std::string::npos) return true;
  }
  return false;
}

// side88: a shiny surface's maps. The guns (side86's logs) name an environment map (Springfield and the bullets:
// "environment_Texture", "env_map_Texture", 128 x 128 DXT1) and specular maps ("springfield_s_Texture",
// "w_mp40_spec_Texture", "Scratch05_SPEC_Texture"); the first of each is taken. The K98's "reflection_tint" is
// neither.
bool EnvMapName(const std::string& name) {
  const std::string lower = Lower(name);
  return lower.find("environment") != std::string::npos || lower.find("env_map") != std::string::npos ||
         lower.compare(0, 4, "env_") == 0;
}

bool SpecularMapName(const std::string& name) {
  const std::string lower = Lower(name);
  return lower.find("spec") != std::string::npos || lower.find("_s_") != std::string::npos ||
         (lower.size() > 2 && lower.compare(lower.size() - 2, 2, "_s") == 0);
}

// side49: a soldier's color sampler can hold a set of looks instead of one texture (its uniform or head; the set is
// named at +0x10, "c_..._ng_body_text"): +0x24 how many, +0x28 where the list is (right after the set), unused
// entries the image's default texture (not an NGL texture). Which look a soldier wears isn't read yet, so the first
// one is taken; until now these were drawn in their placeholder tint, which made the uniforms white.
constexpr uint32_t kLookCount = 0x24;
constexpr uint32_t kLookList = 0x28;
constexpr uint32_t kMaxLooks = 16;
uint32_t FirstLook(uint8_t* base, uint32_t set) {
  if (!Readable(set) || !Readable(set + kLookList)) return 0;
  const uint32_t count = g::ReadU32(base, set + kLookCount), looks = g::ReadU32(base, set + kLookList);
  if (count == 0 || count > kMaxLooks) return 0;
  for (uint32_t k = 0; k < count && Readable(looks + 4 * k); ++k) {
    const uint32_t texture = g::ReadU32(base, looks + 4 * k);
    if (IsTexture(base, texture)) return texture;
  }
  return 0;
}

// side86: which look a soldier wears, as the game picks it. A set of looks is an animated NGL texture (flag 4 at +0x18,
// its frames the looks), and the game draws frame F mod count of it: 0x82155000 binds a material's samplers so
// (0x8213EB90 does the same for a single texture). F is set for each surface just before, by 0x820D48F0: from the
// object's render parameters (object +0x84: a 64-bit mask, then a word for each parameter, parameter n at +4 (n + 2))
// when the texture-frame parameter's bit is set (its number is kept at 0x8290F2A4 when the parameters are registered),
// else the scene's own frame (scene +0x424).
constexpr uint32_t kObjectRenderParameters = 0x84;
constexpr uint32_t kSceneTextureFrame = 0x424;
#if defined(COD3_MP)
constexpr uint32_t kTextureFrameParameter = 0x82A8C1B8;  // cod3mp's (mp/globals_sp_to_mp.tsv: 5 of 5 uses)
#else
constexpr uint32_t kTextureFrameParameter = 0x8290F2A4;
#endif
constexpr uint32_t kMaxFrames = 256;

struct TextureFrame {
  uint32_t value = 0;
  bool own = false;  // from the object's render parameters (else the scene's frame)
};

TextureFrame FrameFor(uint8_t* base, uint32_t scene, uint32_t object) {
  TextureFrame frame;
  if (Readable(scene + kSceneTextureFrame)) frame.value = g::ReadU32(base, scene + kSceneTextureFrame);
  const uint32_t bit = g::ReadU32(base, kTextureFrameParameter);  // a global of the image
  const uint32_t parameters =
      Readable(object + kObjectRenderParameters) ? g::ReadU32(base, object + kObjectRenderParameters) : 0;
  if (bit >= 64 || !Readable(parameters) || !Readable(parameters + 4 * (bit + 2))) return frame;
  const uint64_t mask = uint64_t{g::ReadU32(base, parameters)} << 32 | g::ReadU32(base, parameters + 4);
  if ((mask >> bit) & 1) {
    frame.value = g::ReadU32(base, parameters + 4 * (bit + 2));
    frame.own = true;
  }
  return frame;
}

// The look a set's frame picks (frames[F mod count], as 0x82155000 takes it), or the first look that is a texture when
// that entry isn't one (an unused entry) or the set isn't flagged animated; 0 when there is none.
uint32_t LookFor(uint8_t* base, uint32_t set, uint32_t frame) {
  if (!Readable(set) || !Readable(set + kLookList)) return 0;
  if (!(g::ReadU32(base, set + kTextureFlags) & kTextureAnimated)) return FirstLook(base, set);
  const uint32_t count = g::ReadU32(base, set + kLookCount), looks = g::ReadU32(base, set + kLookList);
  if (count == 0 || count > kMaxFrames || !Readable(looks + 4 * (frame % count))) return FirstLook(base, set);
  const uint32_t texture = g::ReadU32(base, looks + 4 * (frame % count));
  return IsTexture(base, texture) ? texture : FirstLook(base, set);
}

// The color map is the texture whose sampler names it so (side33; a set's look by the surface's frame, side49 and
// side86), else the first DXT1 or DXT3 texture that isn't the lightmap (the DXT5 ones seen are normal maps), else the
// first texture.
Samplers MaterialSamplers(uint8_t* base, uint32_t material, bool skinned, uint32_t frame = 0) {
  Samplers out;
  const uint32_t list = Pointer(material) ? g::ReadU32(base, material + kMaterialSamplers) : 0;
  if (!Readable(list)) return out;
  for (uint32_t k = 0; k < kMaxSamplers; ++k) {  // by name first
    const uint32_t entry = list + kSamplerFirst + kSamplerBytes * k;
    const uint32_t parameter = g::ReadU32(base, entry), texture = g::ReadU32(base, entry + 4);
    if (!Pointer(parameter)) break;
    if (parameter == kLightMapParameter) {
      if (IsTexture(base, texture) && Physical(texture)) out.lightmap = texture;
      continue;
    }
    const std::string name = SamplerTextureName(base, parameter);
    if (IsTexture(base, texture)) {
      if (!out.env && EnvMapName(name)) {
        out.env = texture;
        continue;
      }
      if (!out.env_mask && SpecularMapName(name)) out.env_mask = texture;
    }
    if (out.color || !ColorMapName(name)) continue;
    if (IsTexture(base, texture)) {
      out.color = texture;
    } else if ((out.color = LookFor(base, texture, frame)) != 0) {
      out.looks = texture;
    }
  }
  if (out.color) return out;
  out.lightmap = 0;
  uint32_t first = 0;
  for (uint32_t k = 0; k < kMaxSamplers; ++k) {
    const uint32_t entry = list + kSamplerFirst + kSamplerBytes * k;
    const uint32_t parameter = g::ReadU32(base, entry), texture = g::ReadU32(base, entry + 4);
    if (!Pointer(parameter)) break;
    if (!IsTexture(base, texture)) continue;
    if (parameter == kLightMapParameter) {
      if (Physical(texture)) out.lightmap = texture;  // one in the image is a global (a default or a render target)
      continue;
    }
    if (OtherMapName(SamplerTextureName(base, parameter))) continue;
    if (!first) first = texture;
    const uint32_t format = g::ReadU32(base, entry + 8 + 4) & 0x3F;  // the fetch constant's dword 1
    if (!out.color && (format == 18 || format == 19)) out.color = texture;
  }
  if (!out.color) out.color = first;
  // A soldier's list (side24) has its own DXT5 texture under 0x829114E0 and again as the next entry, then shared
  // ones: there the 0x829114E0 texture is the color map, not a lightmap. (The terrain's lists also name their
  // lightmap twice, so this is for skinned surfaces only; side25 turned the whole ground into its lightmap.)
  if (skinned && out.lightmap) {
    for (uint32_t k = 0; k < kMaxSamplers; ++k) {
      const uint32_t entry = list + kSamplerFirst + kSamplerBytes * k;
      const uint32_t parameter = g::ReadU32(base, entry), texture = g::ReadU32(base, entry + 4);
      if (!Pointer(parameter)) break;
      if (parameter != kLightMapParameter && texture == out.lightmap) {
        out.color = out.lightmap;
        out.lightmap = 0;
        out.looks = 0;
        break;
      }
    }
  }
  return out;
}

// The first materials' sampler lists in full, every texture saved (ngl-side-texture-<address>.ppm), to learn which
// entry is which map: "material M (stride S): 0 T WxH format F, 1 ..." (SaveTexture is below).
void SaveTexture(uint8_t* base, uint32_t texture);

// A name for an effect parameter, if one of its first eight words points to a short printable string ("-" if not).
std::string ParameterName(uint8_t* base, uint32_t parameter) {
  if (!Readable(parameter)) return "-";
  for (uint32_t at = 0; at < 0x20; at += 4) {
    const uint32_t text = g::ReadU32(base, parameter + at);
    if (!Readable(text & ~3u) || text < 0x82000000u) continue;
    std::string name;
    for (uint32_t k = 0; k < 40; ++k) {
      const uint8_t c = base[text + k];
      if (c == 0) break;
      if (c < 0x20 || c > 0x7E) {
        name.clear();
        break;
      }
      name += static_cast<char>(c);
    }
    if (name.size() >= 3) return "\"" + name + "\" at +" + std::to_string(at);
  }
  return "-";
}

std::string DeepName(uint8_t* base, uint32_t at);

// side31: the stride-52 lists shown were terrain pieces, not soldiers, so the skinned ones are picked by their vertices
// now: past the first 24 materials, 8 more skinned ones.
void ShowSamplers(uint8_t* base, uint32_t material, uint32_t stride, bool skinned, bool always = false) {
  static std::unordered_set<uint32_t> shown;
  static int skinned_shown = 0;
  if (!Pointer(material) || (shown.count(material) && !always)) return;
  if (!always && shown.size() >= 24 && !(skinned && skinned_shown++ < 8)) return;
  shown.insert(material);
  const uint32_t list = g::ReadU32(base, material + kMaterialSamplers);
  if (!Readable(list)) return;
  std::string text;
  for (uint32_t k = 0; k < kMaxSamplers; ++k) {
    const uint32_t entry = list + kSamplerFirst + kSamplerBytes * k;
    const uint32_t parameter = g::ReadU32(base, entry), texture = g::ReadU32(base, entry + 4);
    if (!Pointer(parameter)) break;
    char item[320];
    const std::string texture_name = SamplerTextureName(base, parameter);
    std::string name = texture_name.empty() ? DeepName(base, parameter) : "<" + texture_name + ">";
    // side32: a soldier's parameter holds the sampler itself (+0 its texture, then the fetch constant), with +0x20 and
    // +0x28 pointers and +0x24 a small number (6): the name is looked for behind those, and the number shown
    for (uint32_t at : {0x20u, 0x28u}) {
      const uint32_t inner = Readable(parameter) ? g::ReadU32(base, parameter + at) : 0;
      if (name == "-" && Pointer(inner) && Readable(inner)) {
        name = DeepName(base, inner);
        if (name != "-") name = "+" + std::to_string(at) + " -> " + name;
      }
    }
    if (Readable(parameter)) name += " #" + std::to_string(g::ReadU32(base, parameter + 0x24));
    if (IsTexture(base, texture)) {
      std::snprintf(item, sizeof item, "%s%u %08X %ux%u format %u (parameter %08X %s)", text.empty() ? "" : ", ", k,
                    texture, g::ReadU32(base, texture + kTextureWidth), g::ReadU32(base, texture + kTextureHeight),
                    g::ReadU32(base, entry + 12) & 0x3F, parameter, name.c_str());
      if (Physical(texture)) SaveTexture(base, texture);
    } else {
      std::snprintf(item, sizeof item, "%s%u (%08X, not a texture; parameter %08X %s)", text.empty() ? "" : ", ", k,
                    texture, parameter, name.c_str());
    }
    text += item;
  }
  REXLOG_INFO("cod3: ngl side: material {:08X} (stride {}{}): {}", material, stride, skinned ? ", skinned" : "",
              text);
  if (skinned && Readable(g::ReadU32(base, list + kSamplerFirst))) {  // a soldier's first parameter in full
    const uint32_t parameter = g::ReadU32(base, list + kSamplerFirst);
    REXLOG_INFO("cod3: ngl side: material {:08X} parameter {:08X}: {} / {}", material, parameter,
                Words(base, parameter, 8), Words(base, parameter + 0x20, 8));
    for (uint32_t at : {0x20u, 0x28u}) {  // and what its two pointers point at
      const uint32_t inner = g::ReadU32(base, parameter + at);
      if (Pointer(inner) && Readable(inner))
        REXLOG_INFO("cod3: ngl side: material {:08X} parameter {:08X} +{:02X} -> {:08X}: {} / {}", material, parameter,
                    at, inner, Words(base, inner, 8), Words(base, inner + 0x20, 8));
    }
  }
}

// A shader state's parameter list (node +0x18, the list at +0x34), for the first states seen: each parameter's
// semantic code, a name if one is found, its first words, and the four floats where its value goes (0x82142F20 fills
// them on each pass, so these are the game's last), to find FogColor and the lights' values:
// "state S parameter K: code C, value at V (a b c d), name N, words ...".
constexpr uint32_t kNodeShaderState = 0x18;
constexpr uint32_t kStateParameters = 0x34;
constexpr uint32_t kParameterNext = 0x0C;
constexpr uint32_t kParameterCode = 0x10;
constexpr uint32_t kParameterValue = 0x14;

// ParameterName, or the name one pointer deep (side29: the parameter entries' own words hold no string; their first
// two words point into the effect).
// The printable text right at `at` (a descriptor is {hash, name in place}), "" when it isn't 3 or more characters.
std::string InlineName(uint8_t* base, uint32_t at) {
  if (!Readable(at)) return "";
  std::string name;
  for (uint32_t k = 0; k < 40; ++k) {
    const uint8_t c = base[at + k];
    if (c == 0) break;
    if (c < 0x20 || c > 0x7E) return "";
    name += static_cast<char>(c);
  }
  return name.size() >= 3 ? name : "";
}

std::string DeepName(uint8_t* base, uint32_t at) {
  std::string name = ParameterName(base, at);
  if (name != "-") return name;
  if (!(name = InlineName(base, at + 4)).empty()) return "\"" + name + "\" in place";
  for (uint32_t k = 0; k < 0x10; k += 4) {
    const uint32_t inner = g::ReadU32(base, at + k);
    if (!Pointer(inner) || !Readable(inner)) continue;
    name = ParameterName(base, inner);
    if (name != "-") return "+" + std::to_string(k) + " -> " + name;
    if (!(name = InlineName(base, inner + 4)).empty())
      return "+" + std::to_string(k) + " -> \"" + name + "\" in place";
  }
  return "-";
}

void ShowShaderState(uint8_t* base, uint32_t state) {
  static std::unordered_set<uint32_t> shown;
  if (!Pointer(state) || !Readable(state) || shown.count(state) || shown.size() >= 12) return;
  shown.insert(state);
  REXLOG_INFO("cod3: ngl side: state {:08X}: {} / {}", state, Words(base, state, 8), Words(base, state + 0x20, 8));
  for (uint32_t at : {0x20u, 0x24u}) {  // the effect's objects
    const uint32_t effect = g::ReadU32(base, state + at);
    if (Pointer(effect) && Readable(effect))
      REXLOG_INFO("cod3: ngl side: state {:08X} +{:02X} effect {:08X}: {} / {}", state, at, effect,
                  Words(base, effect, 8), Words(base, effect + 0x20, 8));
  }
  uint32_t parameter = g::ReadU32(base, state + kStateParameters);
  for (int k = 0; k < 24 && Pointer(parameter) && Readable(parameter); ++k) {
    const uint32_t code = g::ReadU32(base, parameter + kParameterCode);
    const uint32_t value = g::ReadU32(base, parameter + kParameterValue);
    std::string floats = "-";
    if (Pointer(value) && Readable(value) && Readable(value + 12)) {
      char text[128];
      std::snprintf(text, sizeof text, "(%g %g %g %g)", g::ReadF32(base, value), g::ReadF32(base, value + 4),
                    g::ReadF32(base, value + 8), g::ReadF32(base, value + 12));
      floats = text;
    }
    const uint32_t described = g::ReadU32(base, parameter);
    REXLOG_INFO("cod3: ngl side: state {:08X} parameter {}: code {:02X}, value at {:08X} {}, name {}, words {}; "
                "described at {:08X}: {}",
                state, k, code, value, floats, DeepName(base, parameter), Words(base, parameter, 8), described,
                Pointer(described) && Readable(described) ? Words(base, described, 8) : std::string("-"));
    parameter = g::ReadU32(base, parameter + kParameterNext);
  }
}

// Any shader state's parameters named like fog (FogColor is on some effects, side29), each name once, to find the
// haze's color and distances: "fog parameter N on state S: code C, value (a b c d)".
void HuntFog(uint8_t* base, uint32_t state) {
  static std::unordered_set<std::string> shown;
  static std::unordered_set<uint32_t> states;
  if (shown.size() >= 12 || states.size() >= 4096 || !Pointer(state) || !Readable(state) ||
      !states.insert(state).second)
    return;
  uint32_t parameter = g::ReadU32(base, state + kStateParameters);
  for (int k = 0; k < 64 && Pointer(parameter) && Readable(parameter); ++k) {
    const std::string name = DeepName(base, parameter);
    std::string lower;
    for (char c : name) lower += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    if (lower.find("fog") != std::string::npos && shown.insert(name).second) {
      const uint32_t value = g::ReadU32(base, parameter + kParameterValue);
      std::string floats = "-";
      if (Pointer(value) && Readable(value) && Readable(value + 12)) {
        char text[128];
        std::snprintf(text, sizeof text, "(%g %g %g %g)", g::ReadF32(base, value), g::ReadF32(base, value + 4),
                      g::ReadF32(base, value + 8), g::ReadF32(base, value + 12));
        floats = text;
      }
      REXLOG_INFO("cod3: ngl side: fog parameter {} on state {:08X}: code {:02X}, value at {:08X} {}", name, state,
                  g::ReadU32(base, parameter + kParameterCode), value, floats);
    }
    parameter = g::ReadU32(base, parameter + kParameterNext);
  }
}

// The sun as the game's shaders get it, from a shader state's parameter list (side29-30, COD3-NGL-MAP.md):
// position_light0 / cod_shadow_position (code 0x35: the sun's position, its direction times 10^6; a point light's is
// a plain position, left out here), cod_shadow_projcolor (0x37: its color) and __40_temp1_cmn (0x2A: 0.39 gray in the
// forest), taken as the ambient when the list has it.
constexpr uint32_t kCodeLightPosition = 0x35;
constexpr uint32_t kCodeLightColor = 0x37;
constexpr uint32_t kCodeAmbient = 0x2A;
constexpr float kSunFarAway = 1e5f;      // longer than this: a direction, not a place
constexpr float kDefaultAmbient = 0.35f;

struct Sun {
  bool ok = false;
  float direction[3] = {0, 0, -1};  // the way the light travels
  float color[3] = {1, 1, 1};
  float ambient[3] = {kDefaultAmbient, kDefaultAmbient, kDefaultAmbient};
  // side99: up to three more directional lights (a light set's next strongest), as the game's vertex shaders light by
  // four (shader_C4130C72D4721102: c4-c7 the directions, c8-c10 their colors, c11 the ambient, saturated)
  uint32_t more = 0;
  float more_direction[3][3] = {};
  float more_color[3][3] = {};
};
Sun g_sun;  // the last one a state had, for draws whose own state has none
// side108: the way the sun travels, from the last light set with a directional light (a soldier's: the sun the light
// grid weights), for the native sun's shadow
float g_sun_way[3] = {0, 0, -1};
bool g_sun_way_known = false;

bool StateSun(uint8_t* base, uint32_t state, Sun& sun) {
  if (!Pointer(state) || !Readable(state)) return false;
  bool placed = false, colored = false;
  uint32_t parameter = g::ReadU32(base, state + kStateParameters);
  for (int k = 0; k < 24 && Pointer(parameter) && Readable(parameter); ++k) {
    const uint32_t code = g::ReadU32(base, parameter + kParameterCode);
    const uint32_t value = g::ReadU32(base, parameter + kParameterValue);
    if (Pointer(value) && Readable(value) && Readable(value + 12)) {
      const float v[3] = {g::ReadF32(base, value), g::ReadF32(base, value + 4), g::ReadF32(base, value + 8)};
      const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
      if (code == kCodeLightPosition && !placed && std::isfinite(length) && length > kSunFarAway) {
        for (int i = 0; i < 3; ++i) sun.direction[i] = -v[i] / length;
        placed = true;
      } else if (code == kCodeLightColor && !colored && std::isfinite(length)) {
        for (int i = 0; i < 3; ++i) sun.color[i] = v[i];
        colored = true;
      } else if (code == kCodeAmbient && std::isfinite(length)) {
        for (int i = 0; i < 3; ++i) sun.ambient[i] = v[i];
      }
    }
    parameter = g::ReadU32(base, parameter + kParameterNext);
  }
  sun.ok = placed && colored;
  return sun.ok;
}

// side89: a surface's own lights. Like Big Red One's nglListAddMesh, a surface's render parameters (object +0x84) can
// hold a light set (the light-set parameter, its number at 0x8290F2B0; cod3mp 0x82A8C1C4): the soldiers' (0x82427D18
// passes theirs, made from the lighting where they stand) and the instanced props' (0x820D27E0, Big Red One's
// cdSimpleInstance_AddListToScene: nglCreateLightSet 0x82150A20, one directional light 0x82150A88, the ambient
// 0x8214FEC0). The game's lighting (0x82142360) walks that set, so a soldier in the trees' shade is dark, where the
// one sun lit every soldier alike. A set is 0x70 bytes: +0x00 eight light lists (one per light group, each ending at
// the set), +0x60 the ambient (red, green, blue, clamped to 0..1). A light: +4 x group the next in that group, +0x28
// its kind (1 directional), +0x2C its data: +0x00 the way it travels, +0x10 its color. Here the strongest directional
// light of any group is the draw's light and the others are added to the ambient at half, as for Big Red One
// (ngl_mesh.c read_lights). side99: the four strongest are the draw's lights, as the game's vertex shaders take four
// (only the ones after those go to the ambient at half).
#if defined(COD3_MP)
constexpr uint32_t kLightSetParameter = 0x82A8C1C4;  // mp/globals_sp_to_mp.tsv: 6 of 6 uses
#else
constexpr uint32_t kLightSetParameter = 0x8290F2B0;
#endif
constexpr uint32_t kLightSetAmbient = 0x60;
constexpr uint32_t kLightGroups = 8;
constexpr uint32_t kLightKind = 0x28;
constexpr uint32_t kLightData = 0x2C;
constexpr uint32_t kLightDirectional = 1;
constexpr uint32_t kMaxSetLights = 16;

bool ObjectSun(uint8_t* base, uint32_t object, Sun& sun) {
  const uint32_t bit = g::ReadU32(base, kLightSetParameter);  // a global of the image
  const uint32_t parameters =
      Readable(object + kObjectRenderParameters) ? g::ReadU32(base, object + kObjectRenderParameters) : 0;
  if (bit >= 64 || !Readable(parameters) || !Readable(parameters + 4 * (bit + 2))) return false;
  const uint64_t mask = uint64_t{g::ReadU32(base, parameters)} << 32 | g::ReadU32(base, parameters + 4);
  if (!((mask >> bit) & 1)) return false;
  const uint32_t set = g::ReadU32(base, parameters + 4 * (bit + 2));
  if (!Pointer(set) || !Readable(set + kLightSetAmbient + 12)) return false;
  Sun own;
  for (uint32_t i = 0; i < 3; ++i) own.ambient[i] = g::ReadF32(base, set + kLightSetAmbient + 4 * i);
  uint32_t seen[kMaxSetLights];
  uint32_t seen_count = 0;
  struct Light {
    float direction[3], color[3], luminance;
  } lights[kMaxSetLights];
  uint32_t light_count = 0;
  for (uint32_t group = 0; group < kLightGroups; ++group) {
    uint32_t light = g::ReadU32(base, set + 4 * group);
    for (uint32_t n = 0; light != set && Pointer(light) && Readable(light + kLightData) && n < kMaxSetLights;
         light = g::ReadU32(base, light + 4 * group), ++n) {
      bool again = false;
      for (uint32_t k = 0; k < seen_count; ++k) again = again || seen[k] == light;
      if (again || seen_count == kMaxSetLights) continue;
      seen[seen_count++] = light;
      const uint32_t data = g::ReadU32(base, light + kLightData);
      const uint32_t kind = g::ReadU32(base, light + kLightKind);
      static int others_shown = 0;
      if (others_shown < 8 && Pointer(data) && Readable(data + 0x8C)) {
        // side100-101: what each light of a set holds (the game's lighting, 0x82142360, takes kind 3 as the sun,
        // kind 8 as nine four-float rows it adds the ambient and the other directional lights into, kinds 0 and 5 as
        // the two nearest point lights)
        ++others_shown;
        std::string rows;
        for (uint32_t r = 0; r < 9; ++r) {
          char row[96];
          std::snprintf(row, sizeof row, " (%.4g %.4g %.4g %.4g)", g::ReadF32(base, data + 16 * r),
                        g::ReadF32(base, data + 16 * r + 4), g::ReadF32(base, data + 16 * r + 8),
                        g::ReadF32(base, data + 16 * r + 12));
          rows += row;
        }
        REXLOG_INFO("cod3: ngl side: light set {:08X}: light {:08X} of kind {}, its data {:08X}:{}", set, light, kind,
                    data, rows);
      } else if (others_shown < 8) {
        ++others_shown;
        REXLOG_INFO("cod3: ngl side: light set {:08X}: light {:08X} of kind {}, its data {:08X} (not readable)", set,
                    light, kind, data);
      }
      if (kind != kLightDirectional || !Pointer(data) || !Readable(data + 0x1C)) continue;
      Light& l = lights[light_count];
      for (uint32_t i = 0; i < 3; ++i) {
        l.direction[i] = g::ReadF32(base, data + 4 * i);
        l.color[i] = g::ReadF32(base, data + 0x10 + 4 * i);
      }
      l.luminance = 0.3f * l.color[0] + 0.59f * l.color[1] + 0.11f * l.color[2];
      if (std::isfinite(l.luminance) && l.luminance > 0.0f) ++light_count;
    }
  }
  std::stable_sort(lights, lights + light_count,
                   [](const Light& a, const Light& b) { return a.luminance > b.luminance; });
  const bool found = light_count > 0;
  if (found) {
    std::memcpy(own.direction, lights[0].direction, sizeof own.direction);
    std::memcpy(own.color, lights[0].color, sizeof own.color);
    std::memcpy(g_sun_way, own.direction, sizeof g_sun_way);  // side108
    g_sun_way_known = true;
  }
  for (uint32_t k = 1; k < light_count; ++k) {
    if (k <= 3) {
      std::memcpy(own.more_direction[k - 1], lights[k].direction, sizeof own.more_direction[k - 1]);
      std::memcpy(own.more_color[k - 1], lights[k].color, sizeof own.more_color[k - 1]);
      own.more = k;
    } else {
      for (int i = 0; i < 3; ++i) own.ambient[i] += 0.5f * lights[k].color[i];
    }
  }
  if (!found) {
    for (float& c : own.color) c = 0.0f;
  }
  own.ok = true;
  static int shown = 0;
  if (shown < 8) {
    ++shown;
    REXLOG_INFO("cod3: ngl side: object {:08X}'s own light set {:08X}: ambient {:.3f} {:.3f} {:.3f}, {} light(s), the "
                "strongest travels {:.3f} {:.3f} {:.3f}, color {:.3f} {:.3f} {:.3f}; {} more drawn",
                object, set, own.ambient[0], own.ambient[1], own.ambient[2], seen_count, own.direction[0],
                own.direction[1], own.direction[2], own.color[0], own.color[1], own.color[2], own.more);
    for (uint32_t k = 0; k < own.more; ++k)
      REXLOG_INFO("cod3: ngl side:   light {}: travels {:.3f} {:.3f} {:.3f}, color {:.3f} {:.3f} {:.3f}", k + 2,
                  own.more_direction[k][0], own.more_direction[k][1], own.more_direction[k][2],
                  own.more_color[k][0], own.more_color[k][1], own.more_color[k][2]);
  }
  sun = own;
  return true;
}

#if defined(COD3_NGL_VK)  // the native draws' light and fog
void LightWith(ngl_vk_mesh_draw& d, const Sun& sun) {
  if (!sun.ok) return;
  d.lit = 1;
  std::memcpy(d.ambient, sun.ambient, sizeof d.ambient);
  std::memcpy(d.light_direction, sun.direction, sizeof d.light_direction);
  std::memcpy(d.light_color, sun.color, sizeof d.light_color);
  d.extra_light_count = sun.more;
  std::memcpy(d.extra_light_direction, sun.more_direction, sizeof d.extra_light_direction);
  std::memcpy(d.extra_light_color, sun.more_color, sizeof d.extra_light_color);
  d.light_saturate = 1;  // the game's vertex shaders saturate the lit color (shader_C4130C72D4721102's add_sat)
}

// The scene's fog, as the game sets it for each view: R_SetupFog (0x82417E08, called by 0x82430210) passes its
// distances and amounts to nglSetFog (0x8212AE18) and its color to nglSetFogColor (0x8212B5C8), Big Red One's
// 0x5E4D0 and 0x5EB60 with the scene's fields 0x50 further on. +0x3D8 the distance the fog starts at (never nearer
// than the camera's near plane, +0x414), +0x3DC the distance it ends at, +0x3E0 its amount before the start, +0x3E4
// its amount at the end; +0x3F0 (16-byte aligned) its color, red, green and blue from 0 to 1, then 1.
constexpr uint32_t kSceneFog = 0x3D8;
constexpr uint32_t kSceneFogColor = 0x3F0;

uint32_t UnitByte(float f) {
  const float clamped = f < 0.0f ? 0.0f : f > 1.0f ? 1.0f : f;
  return static_cast<uint32_t>(clamped * 255.0f + 0.5f);
}

void FogWith(uint8_t* base, uint32_t scene, ngl_vk_mesh_draw& d) {
  if (!Readable(scene + kSceneFogColor + 12)) return;
  float f[7];
  for (uint32_t i = 0; i < 4; ++i) f[i] = g::ReadF32(base, scene + kSceneFog + 4 * i);
  const uint32_t color = (scene + kSceneFogColor) & ~0xFu;
  for (uint32_t i = 0; i < 3; ++i) f[4 + i] = g::ReadF32(base, color + 4 * i);
  for (float v : f) {
    if (!std::isfinite(v)) return;
  }
  static std::unordered_set<uint32_t> shown;
  if (shown.size() < kShownScenes && shown.insert(scene).second) {
    REXLOG_INFO("cod3: ngl side: scene {:08X} fog: from {:.1f} to {:.1f}, amount {:.3f} to {:.3f}, color {:.3f} {:.3f} "
                "{:.3f}", scene, f[0], f[1], f[2], f[3], f[4], f[5], f[6]);
  }
  if (!(f[1] > f[0])) return;  // no fog
  d.fog[0] = f[0];
  d.fog[1] = f[1];
  d.fog[2] = f[3];
  d.fog_color = UnitByte(f[4]) | UnitByte(f[5]) << 8 | UnitByte(f[6]) << 16 | UnitByte(f[2]) << 24;
}
#endif

// The material's first texture, looking in its first 0x60 bytes and one pointer deep; 0 for none.
uint32_t MaterialTexture(uint8_t* base, uint32_t material) {
  if (!Pointer(material)) return 0;
  for (uint32_t at = 0; at < 0x60; at += 4) {
    const uint32_t word = g::ReadU32(base, material + at);
    if (IsTexture(base, word)) return word;
  }
  for (uint32_t at = 0; at < 0x60; at += 4) {
    const uint32_t inner = g::ReadU32(base, material + at);
    if (!Physical(inner) || (inner & 3) || inner == material) continue;
    for (uint32_t k = 0; k < 0x40; k += 4) {
      const uint32_t word = g::ReadU32(base, inner + k);
      if (IsTexture(base, word)) return word;
    }
  }
  return 0;
}

// side90: the second texture a material holds in its first 0x60 bytes (a cdWorld's lightmap), 0 for none.
uint32_t SecondTexture(uint8_t* base, uint32_t material) {
  if (!Pointer(material)) return 0;
  uint32_t first = 0;
  for (uint32_t at = 0; at < 0x60; at += 4) {
    const uint32_t word = g::ReadU32(base, material + at);
    if (!IsTexture(base, word)) continue;
    if (!first) {
      first = word;
    } else if (word != first) {
      return word;
    }
  }
  return 0;
}

#if defined(COD3_NGL_VK)
std::filesystem::path ScreenshotFolder();
void SavePpm(const std::filesystem::path& path, const uint8_t* rgba, uint32_t width, uint32_t height);
#endif

// A texture saved as a picture (userdata/logs/screenshots/ngl-side-texture-<address>.ppm), to see what it holds.
void SaveTexture(uint8_t* base, uint32_t texture) {
#if defined(COD3_NGL_VK)
  std::vector<uint8_t> bgra;
  uint32_t width = 0, height = 0;
  std::string why;
  if (!IsTexture(base, texture) ||
      !Decode(base, g::ReadU32(base, texture + kTextureDirect3D), bgra, width, height, why)) {
    return;
  }
  for (size_t i = 0; i + 3 < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
  char name[64];
  std::snprintf(name, sizeof name, "ngl-side-texture-%08X.ppm", texture);
  SavePpm(ScreenshotFolder() / name, bgra.data(), width, height);
#else
  (void)base;
  (void)texture;
#endif
}

// A generic surface in full, for the first few: the surface's buffers, its format, its first vertex bytes and
// indices, so the guesses at the top can be checked.
void ShowSurface(uint8_t* base, uint32_t node, int& shown) {
  if (shown >= kShownSurfaces) return;
  ++shown;
  const uint32_t surface = g::ReadU32(base, node + kNodeSurface);
  if (!Pointer(surface)) return;
  REXLOG_INFO("cod3: ngl side: surface {:08X} (node {:08X}, object {:08X}, material {:08X})", surface, node,
              g::ReadU32(base, node + kNodeObject), g::ReadU32(base, node + kNodeMaterial));
  for (uint32_t row = 0; row < 0x80; row += 0x20) {
    REXLOG_INFO("cod3: ngl side: surface {:08X} +{:02X}: {}", surface, row, Words(base, surface + row, 8));
  }
  for (uint32_t k = 0; k < 2; ++k) {
    const uint32_t buffer = g::ReadU32(base, surface + kSurfaceBuffers + 0x10 * k);
    if (!Pointer(buffer)) continue;
    REXLOG_INFO("cod3: ngl side: surface {:08X} buffer {}: {:08X}: {}", surface, k, buffer, Words(base, buffer, 8));
    const uint32_t f0 = g::ReadU32(base, buffer + kBufferFetch);
    const uint32_t data = GuestAddress((f0 & 3) == 3 ? f0 & ~3u : f0);
    if (Physical(data)) {
      for (uint32_t row = 0; row < 0x60; row += 0x20) {
        REXLOG_INFO("cod3: ngl side: surface {:08X} buffer {} data {:08X} +{:02X}: {}", surface, k, data, row,
                    Words(base, data + row, 8));
      }
    }
  }
  const uint32_t material = g::ReadU32(base, node + kNodeMaterial);
  const uint32_t texture = MaterialTexture(base, material);
  REXLOG_INFO("cod3: ngl side: surface {:08X} material {:08X}: texture {:08X} ({}x{}); all: {}", surface, material,
              texture, Pointer(texture) ? g::ReadU32(base, texture + kTextureWidth) : 0,
              Pointer(texture) ? g::ReadU32(base, texture + kTextureHeight) : 0,
              Pointer(material) ? MaterialTextures(base, material) : std::string("-"));
  if (Pointer(material)) {
    for (uint32_t at : {0x28u, 0x30u, 0x54u}) {
      const uint32_t inner = g::ReadU32(base, material + at);
      if (!Physical(inner) || (inner & 3)) continue;
      for (uint32_t row = 0; row < 0x60; row += 0x20) {
        REXLOG_INFO("cod3: ngl side: material {:08X} +{:02X} -> {:08X} +{:02X}: {}", material, at, inner, row,
                    Words(base, inner + row, 8));
      }
    }
  }
  SaveTexture(base, texture);
  const Samplers samplers = MaterialSamplers(base, material, false);  // (the listing: as for a solid)
  SaveTexture(base, samplers.color);
  SaveTexture(base, samplers.lightmap);
  REXLOG_INFO("cod3: ngl side: surface {:08X}: color map {:08X}, lightmap {:08X}", surface, samplers.color,
              samplers.lightmap);
  const uint32_t list = Pointer(material) ? g::ReadU32(base, material + kMaterialSamplers) : 0;
  for (uint32_t k = 0; Readable(list) && k < kMaxSamplers; ++k) {
    const uint32_t parameter = g::ReadU32(base, list + kSamplerFirst + kSamplerBytes * k);
    if (!Physical(parameter) || (parameter & 3)) break;
    std::string name;
    for (uint32_t at = 0; at < 0x20 && name.empty(); at += 4) {  // a name the parameter points to, if any
      const uint32_t text = g::ReadU32(base, parameter + at);
      if (!Physical(text) && !(text >= 0x82000000u && text < kImageEnd)) continue;
      const char* c = reinterpret_cast<const char*>(base + text);
      size_t n = 0;
      while (n < 32 && c[n] >= 32 && c[n] < 127) ++n;
      if (n >= 4 && c[n] == 0) name.assign(c, n);
    }
    REXLOG_INFO("cod3: ngl side: sampler {} parameter {:08X} \"{}\": {}", k, parameter, name,
                Words(base, parameter, 8));
  }
  const uint32_t object = g::ReadU32(base, node + kNodeObject);
  if (Pointer(object)) {
    REXLOG_INFO("cod3: ngl side: object {:08X} textures: {}", object, MaterialTextures(base, object, 0xC0));
  }
  const uint32_t format = g::ReadU32(base, surface + kSurfaceFormat);
  if (Pointer(format)) {
    REXLOG_INFO("cod3: ngl side: surface {:08X} format {:08X}: {} / {}", surface, format, Words(base, format, 8),
                Words(base, format + 0x20, 8));
  }
  const uint32_t elements = g::ReadU32(base, surface + kSurfaceStride + 4);
  if (Pointer(elements)) {
    REXLOG_INFO("cod3: ngl side: surface {:08X} +74 -> {:08X}: {} / {}", surface, elements, Words(base, elements, 8),
                Words(base, elements + 0x20, 8));
  }
}

void ShowSurface(uint8_t* base, uint32_t node) {
  static int shown = 0;
  ShowSurface(base, node, shown);
}

// Step 4's first look: the first node of each other kind (the 3D ones), once: its vtable's first eight entries (Render
// is +8), its first 0x60 bytes, and its scene's +0x100..+0x1FF (where Big Red One keeps the projection and view).
// A tree or grass node in depth, for the first few (step 4's next part): the node, then every block its words point to
// (0x40 bytes), and one level further for the Direct3D buffers among them (common word 0x00200001 vertex,
// 0x20200002 index), to find where SpeedTree keeps its geometry.
// The grass node, once (side55-57): +0x0C the grass object G, +0x10 the patch set. G's first word is the grass
// manager M that 0x823F6DF0 works with: M+0x00 the NGL material N, +0x04 and +0x28 the shared vertex and index
// buffers (+0x24 7000 vertices, +0x2C 8000 indices), +0x4C the spacing, +0x50 and +0x54 the samplers, +0x58 to +0xC4
// N's other parameters (kind at +0x10: 3 a float, 4 four, 0xB a sampler; value at +0x14), +0x194..+0x19C. N: +0x30
// NGL's parameter list (+0x04 name, +0x0C next, +0x10 kind, +0x14 value), +0x1C its value block, +0x0C's +0x0C its
// pass, whose compiled constant uploads (+0xDC) are lists at +0x80, +0xC0, +0x100 and +0x140 of {slot, offset, a
// SET_CONSTANT packet's 32 bytes} (0x82154DC0: the value is at the slot's base + offset; slot 1 is N+0x1C's block,
// slot 2 NGL's globals at 0x8290E9AC). The set: +0x04 the list of patches (count at +0x0E), each naming its cell
// (+0x0C, an index into the set's +0x00 list of {kind, data} pairs).
void ShowGrass(uint8_t* base, uint32_t node) {
  static bool shown = false;
  if (shown) return;
  shown = true;
  auto readable = [](uint32_t a) { return Readable(a) || (!(a & 3) && a >= 0x40000000u && a < 0x80000000u); };
  auto floats = [base](uint32_t at, int count) {
    std::string text;
    char item[24];
    for (int k = 0; k < count; ++k) {
      std::snprintf(item, sizeof item, "%s%.4g", k ? " " : "", g::ReadF32(base, at + 4u * static_cast<uint32_t>(k)));
      text += item;
    }
    return text;
  };
  const uint32_t object = g::ReadU32(base, node + 0x0C), set = g::ReadU32(base, node + 0x10);
  const uint32_t material = readable(object) ? g::ReadU32(base, object) : 0;
  REXLOG_INFO("cod3: ngl side: grass node {:08X}: {}; object {:08X}, material {:08X}, patch set {:08X}", node,
              Words(base, node, 8), object, material, set);
  if (readable(material)) {
    for (uint32_t row = 0; row < 0x1C0; row += 0x20) {
      REXLOG_INFO("cod3: ngl side: grass material +{:03X}: {}", row, Words(base, material + row, 8));
    }
    for (uint32_t at = 0x50; at <= 0xC4; at += 4) {
      const uint32_t parameter = g::ReadU32(base, material + at);
      if (!readable(parameter)) continue;
      const uint32_t value = g::ReadU32(base, parameter + 0x14);
      REXLOG_INFO("cod3: ngl side: grass material +{:03X} -> parameter {:08X}: {}; value {:08X}: {} ({})", at,
                  parameter, Words(base, parameter, 8), value, readable(value) ? Words(base, value, 4) : "-",
                  readable(value) ? floats(value, 4) : "-");
    }
    const uint32_t spacing = g::ReadU32(base, material + 0x4C);
    if (readable(spacing)) REXLOG_INFO("cod3: ngl side: grass spacing {:08X}: {}", spacing, floats(spacing, 8));
    REXLOG_INFO("cod3: ngl side: grass material +190: {}", floats(material + 0x190, 4));
    const uint32_t ngl = g::ReadU32(base, material);
    if (!readable(ngl)) return;
    REXLOG_INFO("cod3: ngl side: grass NGL material {:08X}: {} / {}", ngl, Words(base, ngl, 8),
                Words(base, ngl + 0x20, 8));
    int listed = 0;
    for (uint32_t p = g::ReadU32(base, ngl + 0x30); readable(p) && listed < 40;
         p = g::ReadU32(base, p + 0x0C), ++listed) {
      const uint32_t name = g::ReadU32(base, p + 0x04), value = g::ReadU32(base, p + 0x14);
      const char* text = readable(name) ? reinterpret_cast<const char*>(base + name + 4) : "";
      REXLOG_INFO("cod3: ngl side: grass parameter {:08X} \"{}\": kind {:X}; {}; value {:08X}: {}", p,
                  std::string(text, strnlen(text, 48)), g::ReadU32(base, p + 0x10), Words(base, p, 8), value,
                  readable(value) ? floats(value, 4) : "-");
    }
    const uint32_t holder = g::ReadU32(base, ngl + 0x0C);
    const uint32_t pass = readable(holder) ? g::ReadU32(base, holder + 0x0C) : 0;
    const uint32_t compiled = readable(pass) ? g::ReadU32(base, pass + 0xDC) : 0;
    REXLOG_INFO("cod3: ngl side: grass pass {:08X} (+0xD0 {:X}, +0x18 {:X}, +0xA4 {:X}), compiled {:08X}", pass,
                readable(pass) ? g::ReadU32(base, pass + 0xD0) : 0, readable(pass) ? g::ReadU32(base, pass + 0x18) : 0,
                readable(pass) ? g::ReadU32(base, pass + 0xA4) : 0, compiled);
    if (readable(compiled)) {
      for (uint32_t row = 0; row < 0x180; row += 0x20) {
        REXLOG_INFO("cod3: ngl side: grass compiled +{:03X}: {}", row, Words(base, compiled + row, 8));
      }
    }
    const uint32_t bases[8] = {0, g::ReadU32(base, ngl + 0x1C), g::ReadU32(base, kCurrentScene), kPassBase3,
                               kPassBase4, 0, kPassBase6, g::ReadU32(base, kPassBase7Pointer)};
    for (uint32_t list = 0x80; readable(compiled) && list <= 0x140; list += 0x40) {
      const uint32_t at = compiled + list;
      const uint32_t vectors = g::ReadU32(base, at + 0x34), entries = g::ReadU32(base, at + 0x38);
      REXLOG_INFO("cod3: ngl side: grass constants +{:03X}: {} / {} ({} vectors)", list, Words(base, at, 8),
                  Words(base, at + 0x20, 8), vectors);
      // The float constants (0x82155EC0): a SET_CONSTANT packet's header at +0x10 (its first register x 4 at +0x1C),
      // then one {byte offset into the bases, offset} pair a vector at +0x20 (+0x08 the number of vectors).
      const uint32_t count = g::ReadU32(base, at + 0x08), rows = g::ReadU32(base, at + 0x20);
      const uint32_t first = (g::ReadU32(base, at + 0x1C) & 0x7FF) / 4;
      for (uint32_t k = 0; k < count && k < 96 && readable(rows); ++k) {
        const uint32_t slot = g::ReadU32(base, rows + 8 * k) / 4, offset = g::ReadU32(base, rows + 8 * k + 4);
        const uint32_t value = ((slot < 8 ? bases[slot] : 0) + offset) & ~15u;
        REXLOG_INFO("cod3: ngl side: grass c{}: slot {} +{:X} -> {:08X}: {}", first + k, slot, offset, value,
                    readable(value) ? floats(value, 4) : "-");
      }
      for (uint32_t k = 0; k < vectors && k < 48 && readable(entries); ++k) {
        const uint32_t e = entries + 48 * k, slot = g::ReadU32(base, e), offset = g::ReadU32(base, e + 4);
        const uint32_t value = (slot < 8 ? bases[slot] : 0) + offset;
        REXLOG_INFO("cod3: ngl side: grass constant {}: slot {} +{:X}; packet {} / {}; value {:08X}: {}", k, slot,
                    offset, Words(base, e + 16, 4), Words(base, e + 32, 4), value,
                    readable(value & ~15u) ? floats(value & ~15u, 8) : "-");
      }
    }
  }
  if (!readable(set)) return;
  REXLOG_INFO("cod3: ngl side: grass patch set: {}", Words(base, set, 8));
  const uint32_t cells = g::ReadU32(base, set), list = g::ReadU32(base, set + 4);
  const uint32_t count = g::ReadU32(base, set + 0x0C) & 0xFFFF;
  for (uint32_t k = 0; k < count && k < 4 && readable(list); ++k) {
    const uint32_t patch = g::ReadU32(base, list + 4 * k);
    if (!readable(patch)) continue;
    const uint32_t cell = cells + 8 * (g::ReadU32(base, patch + 0x0C) >> 16);
    const uint32_t kind = g::ReadU32(base, cell), data = g::ReadU32(base, cell + 4);
    const uint32_t descriptor = readable(kind) ? g::ReadU32(base, kind) : 0;
    const uint32_t first = g::ReadU32(base, patch);
    REXLOG_INFO("cod3: ngl side: grass patch {} of {} ({:08X}): {} ({}), +0 -> {}; cell kind {:08X} -> {:08X}: {} "
                "({}); data {:08X}: {}",
                k, count, patch, Words(base, patch, 4), floats(patch, 4), readable(first) ? Words(base, first, 2) : "-",
                kind, descriptor, readable(descriptor) ? Words(base, descriptor, 8) : "-",
                readable(descriptor) ? floats(descriptor, 8) : "-", data, readable(data) ? Words(base, data, 8) : "-");
  }
}

void ShowTree(uint8_t* base, uint32_t node) {
  static int shown = 0;
  if (shown >= 3) return;
  ++shown;
  // the node's record (+0x0C: kind, two words, the tree) and the tree in depth
  const uint32_t record = g::ReadU32(base, node + 0x0C);
  const uint32_t tree = Readable(record) ? g::ReadU32(base, record + 0x0C) : 0;
  if (Readable(tree)) {
    for (uint32_t row = 0; row < 0x100; row += 0x20) {
      REXLOG_INFO("cod3: ngl side: tree {:08X} +{:02X}: {}", tree, row, Words(base, tree + row, 8));
    }
    int followed = 0;
    for (uint32_t at = 0; at < 0x100 && followed < 12; at += 4) {
      const uint32_t inner = g::ReadU32(base, tree + at);
      if (!Readable(inner) || inner == tree) continue;
      ++followed;
      REXLOG_INFO("cod3: ngl side: tree {:08X} +{:02X} -> {:08X}: {} / {}", tree, at, inner, Words(base, inner, 8),
                  Words(base, inner + 0x20, 8));
    }
    for (uint32_t row = 0x100; row < 0x400; row += 0x20) {
      REXLOG_INFO("cod3: ngl side: tree {:08X} +{:03X}: {}", tree, row, Words(base, tree + row, 8));
    }
    // What the branch and frond draws use (sub_823F5C80, sub_823F5868 draw through sub_820C7818): a vertex buffer,
    // index buffers and strip lengths by level of detail, the vertex count, and a vertex format kept in a global
    // ({0x74, 0x4C or 0x6C, ?, declaration}; side23). The buffers, their D3D headers and the declarations sit in the
    // heap's virtual pages (0x4xxxxxxx), followed here only where the game's draw follows them.
    struct Part {
      const char* name;
      uint32_t buffer, index_buffers, lengths, vertices, format;
    };
    const Part parts[] = {{"branches", 0x1C8, 0x1CC, 0x1D0, 0x54, kBranchFormat},
                          {"fronds", 0x1D4, 0x1D8, 0x1DC, 0xAC, kFrondFormat},
                          {"leaves' format", 0, 0, 0, 0, kLeafFormat}};
    auto heap = [](uint32_t a) { return !(a & 3) && a >= 0x40000000u && a < 0x80000000u; };
    for (const Part& part : parts) {
      const uint32_t declaration = g::ReadU32(base, part.format + 8);
      REXLOG_INFO("cod3: ngl side: tree {:08X} {}: format {} -> {} / {} / {} / {}", tree, part.name,
                  Words(base, part.format, 3), heap(declaration) ? Words(base, declaration, 8) : std::string("-"),
                  heap(declaration) ? Words(base, declaration + 0x20, 8) : std::string("-"),
                  heap(declaration) ? Words(base, declaration + 0x40, 8) : std::string("-"),
                  heap(declaration) ? Words(base, declaration + 0x60, 8) : std::string("-"));
      if (!part.buffer) continue;
      const uint32_t buffer = g::ReadU32(base, tree + part.buffer);
      const uint32_t index_buffers = g::ReadU32(base, tree + part.index_buffers);
      REXLOG_INFO("cod3: ngl side: tree {:08X} {}: buffer {:08X}: {}", tree, part.name, buffer,
                  heap(buffer) ? Words(base, buffer, 12) : std::string("-"));
      if (heap(buffer)) {
        const uint32_t data = GuestAddress(g::ReadU32(base, buffer + kBufferFetch) & ~3u);
        if (Physical(data)) {
          REXLOG_INFO("cod3: ngl side: tree {:08X} {} first vertices: {} / {} / {} / {}", tree, part.name,
                      Words(base, data, 8), Words(base, data + 0x20, 8), Words(base, data + 0x40, 8),
                      Words(base, data + 0x60, 8));
        }
      }
      const uint32_t first = Readable(index_buffers) ? g::ReadU32(base, index_buffers) : 0;
      if (heap(first)) {
        const uint32_t data = GuestAddress(g::ReadU32(base, first + 0x18));
        REXLOG_INFO("cod3: ngl side: tree {:08X} {} index buffer 0 {:08X}: {}; first indices {} / {}", tree,
                    part.name, first, Words(base, first, 12), Physical(data) ? Words(base, data, 8) : std::string("-"),
                    Physical(data) ? Words(base, data + 0x20, 8) : std::string("-"));
      }
    }
    for (const uint32_t at : {0x2B0u, 0x2B8u, 0x2CCu, 0x2E8u}) {  // the textures the draws set
      const uint32_t texture = g::ReadU32(base, tree + at);
      REXLOG_INFO("cod3: ngl side: tree {:08X} +{:03X} texture {:08X}: {}", tree, at, texture,
                  Readable(texture) || heap(texture) ? Words(base, texture, 12) : std::string("-"));
    }
    // The leaves (sub_823F5028): a level's record (72 bytes, a list at tree +0xF8: +4 its vertex buffer's number in
    // the list at tree +0x1E0, +8 its leaf count) drawn as a triangle list of six vertices a leaf (sub_820C78D0),
    // format 0x82A1B8DC (stride 0x6C).
    const uint32_t levels = g::ReadU32(base, tree + 0xF8), buffers = g::ReadU32(base, tree + 0x1E0);
    for (uint32_t level = 0; level < 2 && Readable(levels); ++level) {
      const uint32_t record = levels + 72 * level;
      REXLOG_INFO("cod3: ngl side: tree {:08X} leaves level {}: {} / {} / {}", tree, level, Words(base, record, 8),
                  Words(base, record + 0x20, 8), Words(base, record + 0x40, 2));
    }
    const uint32_t buffer = Readable(buffers) ? g::ReadU32(base, buffers) : 0;
    REXLOG_INFO("cod3: ngl side: tree {:08X} leaves: buffers {:08X} ({}), buffer 0 {:08X} ({})", tree, buffers,
                Readable(buffers) ? Words(base, buffers, 4) : std::string("-"), buffer,
                heap(buffer) ? Words(base, buffer, 8) : std::string("-"));
    if (heap(buffer)) {
      const uint32_t data = GuestAddress(g::ReadU32(base, buffer + kBufferFetch) & ~3u);
      for (uint32_t v = 0; v < 6 && Physical(data); ++v) {  // the first leaf's six vertices
        REXLOG_INFO("cod3: ngl side: tree {:08X} leaf vertex {}: {} / {} / {} / {}", tree, v,
                    Words(base, data + 0x6C * v, 8), Words(base, data + 0x6C * v + 0x20, 8),
                    Words(base, data + 0x6C * v + 0x40, 8), Words(base, data + 0x6C * v + 0x60, 3));
      }
    }
  }
  for (uint32_t row = 0; row < 0x80; row += 0x20) {
    REXLOG_INFO("cod3: ngl side: tree node {:08X} +{:02X}: {}", node, row, Words(base, node + row, 8));
  }
  for (uint32_t at = 4; at < 0x80; at += 4) {
    const uint32_t block = g::ReadU32(base, node + at);
    if (!Readable(block) || block == node) continue;
    REXLOG_INFO("cod3: ngl side: tree node {:08X} +{:02X} -> {:08X}: {} / {}", node, at, block, Words(base, block, 8),
                Words(base, block + 0x20, 8));
    for (uint32_t k = 0; k < 0x40; k += 4) {
      const uint32_t inner = g::ReadU32(base, block + k);
      if (!Readable(inner)) continue;
      const uint32_t common = g::ReadU32(base, inner);
      if (common == 0x00200001u || common == 0x20200002u) {
        REXLOG_INFO("cod3: ngl side: tree node {:08X} +{:02X} -> +{:02X}: {} buffer {:08X}: {}", node, at, k,
                    common == 0x00200001u ? "vertex" : "index", inner, Words(base, inner, 8));
      }
    }
  }
}

void ShowNode(uint8_t* base, uint32_t scene, uint32_t node, uint32_t vtable) {
  static std::unordered_set<uint32_t> shown;
  static bool scene_shown = false;
  if (shown.size() >= 8 || !shown.insert(vtable).second) return;
  auto words = [base](uint32_t at, uint32_t count) {
    std::string text;
    char item[12];
    for (uint32_t k = 0; k < count; ++k) {
      std::snprintf(item, sizeof item, "%s%08X", k ? " " : "", g::ReadU32(base, at + 4 * k));
      text += item;
    }
    return text;
  };
  REXLOG_INFO("cod3: ngl side: node kind {:08X}: vtable {}", vtable, Pointer(vtable) ? words(vtable, 8) : "?");
  for (uint32_t row = 0; row < 0x60; row += 0x20) {
    REXLOG_INFO("cod3: ngl side: node {:08X} (kind {:08X}) +{:02X}: {}", node, vtable, row, words(node + row, 8));
  }
  if (!scene_shown && Pointer(scene)) {
    scene_shown = true;
    for (uint32_t row = 0x100; row < 0x200; row += 0x20) {
      REXLOG_INFO("cod3: ngl side: scene {:08X} +{:03X}: {}", scene, row, words(scene + row, 8));
    }
  }
  // the generic surface node (Render 0x82143F70): its object, surface and material, and the globals its pass setup
  // (0x821563A8) hands the effect system (0x829118E0, 0x829112A0), to find the transforms and the camera
  if (vtable == kGenericNode) {
    const uint32_t object = g::ReadU32(base, node + 0x0C), surface = g::ReadU32(base, node + 0x10);
    const uint32_t material = g::ReadU32(base, node + 0x14);
    const struct {
      const char* what;
      uint32_t at, bytes;
    } blocks[] = {{"object", object, 0xC0},     {"surface", surface, 0x80},    {"material", material, 0x60},
                  {"pass base 3", kPassBase3, 0x140}, {"pass base 4", kPassBase4, 0x40}};
    for (const auto& b : blocks) {
      if (!Pointer(b.at)) continue;
      for (uint32_t row = 0; row < b.bytes; row += 0x20) {
        REXLOG_INFO("cod3: ngl side: {} {:08X} +{:03X}: {}", b.what, b.at, row, words(b.at + row, 8));
      }
    }
  }
}

// One list's nodes in the order they came (the list is pushed at its head).
std::vector<uint32_t> ListNodes(uint8_t* base, uint32_t head) {
  std::vector<uint32_t> nodes;
  const uint32_t end = g::ReadU32(base, kListEndPointer);
  for (uint32_t n = head; Pointer(n) && n != end && nodes.size() < kMaxNodes; n = g::ReadU32(base, n + kNodeNext)) {
    nodes.push_back(n);
  }
  std::reverse(nodes.begin(), nodes.end());
  return nodes;
}

Quad ReadQuad(uint8_t* base, uint32_t quad) {
  Quad q;
  for (uint32_t i = 0; i < 4; ++i) {
    const uint32_t vertex = quad + i * kQuadVertexBytes;
    q.x[i] = g::ReadF32(base, vertex);
    q.y[i] = g::ReadF32(base, vertex + 4);
    q.u[i] = g::ReadF32(base, vertex + 8);
    q.v[i] = g::ReadF32(base, vertex + 12);
    q.color[i] = g::ReadU32(base, vertex + 16);
  }
  q.texture = g::ReadU32(base, quad + kQuadTexture);
  for (uint32_t k = 0; k < 4; ++k) q.extra[k] = g::ReadU32(base, quad + 0x50 + 4 * k);
  return q;
}

float SaneScale(float scale) { return scale > 0.0f && scale < 100.0f ? scale : 1.0f; }

// A control code's number, as nglBuildTextGlyphs reads it: one byte, the number, one byte.
double CodeNumber(const std::string& s, size_t& i, bool hex) {
  if (i < s.size()) ++i;
  const char* begin = s.c_str() + i;
  char* end = nullptr;
  const double value = hex ? static_cast<double>(std::strtoul(begin, &end, 16)) : std::strtod(begin, &end);
  i += static_cast<size_t>(end - begin);
  if (i < s.size()) ++i;
  return value;
}

// A string node's glyphs as quads, laid out the way nglBuildTextGlyphs does (bro/src/ngl/ngl_font.c, lay_out): byte 1
// starts a color (hex RRGGBBAA), 2 a scale, 3 an x and a y scale; a tab is four spaces, a new line goes down the cell
// height times the line's largest y scale, back to the start.
void TakeString(uint8_t* base, uint32_t scene, uint32_t node, std::vector<Quad>& out, Census& census) {
  Text t;
  t.node = node;
  t.font = g::ReadU32(base, node + kStringFont);
  t.color = g::ReadU32(base, node + kStringColor);
  t.x = g::ReadF32(base, node + kStringX);
  t.y = g::ReadF32(base, node + kStringY);
  t.scale_x = g::ReadF32(base, node + kStringScaleX);
  t.scale_y = g::ReadF32(base, node + kStringScaleY);
  t.glyphs = 0;
  const uint32_t text = g::ReadU32(base, node + kStringText);
  if (text >= 0x10000) {
    const char* p = reinterpret_cast<const char*>(base + text);
    t.text.assign(p, strnlen(p, kMaxText));
  }
  const uint32_t font = t.font;
  const uint32_t glyphs = Pointer(font) ? g::ReadU32(base, font + kFontGlyphs) : 0;
  const int32_t first = Pointer(font) ? static_cast<int32_t>(g::ReadU32(base, font + kFontFirst)) : 0;
  const int32_t count = Pointer(font) ? static_cast<int32_t>(g::ReadU32(base, font + kFontCount)) : 0;
  if (!Pointer(glyphs) || count <= 0 || count > 0x10000) {
    census.texts.push_back(t);
    return;
  }
  const uint32_t texture = g::ReadU32(base, font + kFontTexture);
  const float cell = static_cast<float>(static_cast<int32_t>(g::ReadU32(base, font + kFontCellHeight)));
  const float texture_width = Pointer(texture) ? static_cast<float>(g::ReadU32(base, texture + kTextureWidth)) : 0.0f;
  const float texture_height = Pointer(texture) ? static_cast<float>(g::ReadU32(base, texture + kTextureHeight)) : 0.0f;
  // a glyph: x, y, width, height, x offset, y offset, advance; -1 when the font has none for c
  auto glyph = [&](unsigned char c, int32_t m[7]) -> int32_t {
    const int32_t index = static_cast<int32_t>(c) - first;
    if (index < 0 || index >= count) return -1;
    for (uint32_t k = 0; k < 7; ++k) {
      m[k] = static_cast<int32_t>(g::ReadU32(base, glyphs + static_cast<uint32_t>(index) * kGlyphBytes + 4 * k));
    }
    return index;
  };
  int32_t space[7] = {};
  const float space_advance = glyph(' ', space) >= 0 ? static_cast<float>(space[6]) : 0.0f;
  float scale_x = SaneScale(t.scale_x), scale_y = SaneScale(t.scale_y);
  float x = t.x, y = t.y, line_scale = scale_y;
  uint32_t color = t.color;
  const std::string& s = t.text;
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = static_cast<unsigned char>(s[i++]);
    switch (c) {
      case 1: {
        const uint32_t rgba = static_cast<uint32_t>(CodeNumber(s, i, true));
        color = rgba << 24 | rgba >> 8;
        break;
      }
      case 2:
        scale_x = scale_y = SaneScale(static_cast<float>(CodeNumber(s, i, false)));
        line_scale = std::max(line_scale, scale_y);
        break;
      case 3:
        scale_x = SaneScale(static_cast<float>(CodeNumber(s, i, false)));
        --i;  // the two numbers share the byte between them
        scale_y = SaneScale(static_cast<float>(CodeNumber(s, i, false)));
        line_scale = std::max(line_scale, scale_y);
        break;
      case '\t':
        x += space_advance * scale_x * 4.0f;
        break;
      case '\n':
        y += cell * line_scale;
        x = t.x;
        line_scale = scale_y;
        break;
      case ' ':
        x += space_advance * scale_x;
        break;
      default: {
        int32_t m[7];
        const int32_t index = glyph(c, m);
        if (index < 0) break;
        if (m[2] > 0 && m[3] > 0) {
          float u0, v0, u1, v1;
          if (texture_width > 0.0f && texture_height > 0.0f) {
            u0 = static_cast<float>(m[0]) / texture_width;
            v0 = static_cast<float>(m[1]) / texture_height;
            u1 = u0 + static_cast<float>(m[2]) / texture_width;
            v1 = v0 + static_cast<float>(m[3]) / texture_height;
          } else {
            u0 = v0 = u1 = v1 = 0.0f;
          }
          const float left = static_cast<float>(m[4]) * scale_x + x, top = static_cast<float>(m[5]) * scale_y + y;
          const float right = static_cast<float>(m[2]) * scale_x + left;
          const float bottom = static_cast<float>(m[3]) * scale_y + top;
          Quad q;
          const float xs[4] = {left, right, left, right}, ys[4] = {top, top, bottom, bottom};
          const float us[4] = {u0, u1, u0, u1}, vs[4] = {v0, v0, v1, v1};
          for (int k = 0; k < 4; ++k) {
            q.x[k] = xs[k];
            q.y[k] = ys[k];
            q.u[k] = us[k];
            q.v[k] = vs[k];
            q.color[k] = color;
          }
          q.texture = texture;
          q.scene = scene;
          out.push_back(q);
          t.glyphs++;
          census.glyphs++;
        }
        x += static_cast<float>(m[6]) * scale_x;
        break;
      }
    }
  }
  census.texts.push_back(t);
}

// side90: which old material class a node's Render is, if any.
int OldClassOf(uint8_t* base, uint32_t vtable) {
  if (!Pointer(vtable) || !Readable(vtable)) return kNotOld;
  const uint32_t render = g::ReadU32(base, vtable + kVtableRender);
  return render == kSimpleDraw ? kOldSimple : render == kWorldDraw ? kOldWorld : kNotOld;
}

void TakeList(uint8_t* base, uint32_t scene, uint32_t head, bool blended, std::vector<Quad>& out, Census& census) {
  std::vector<uint32_t> nodes = ListNodes(base, head);
  if (blended) {
    std::stable_sort(nodes.begin(), nodes.end(), [base](uint32_t a, uint32_t b) {
      return g::ReadF32(base, a + kNodeKey) > g::ReadF32(base, b + kNodeKey);
    });
  }
  for (const uint32_t n : nodes) {
    const uint32_t vtable = g::ReadU32(base, n);
    if (vtable == kQuadNode) {
      out.push_back(ReadQuad(base, n + kNodePayload));
      out.back().scene = scene;
      census.quads++;
    } else if (vtable == kStringNode) {
      census.strings++;
      TakeString(base, scene, n, out, census);
    } else {
      census.other_nodes++;
      census.others[vtable]++;
      ShowNode(base, scene, n, vtable);
      if (vtable == kGenericNode) {
        census.solids.push_back({scene, n});
        ShowSurface(base, n);
      } else if (const int old = OldClassOf(base, vtable)) {
        census.solids.push_back({scene, n, old});
        census.old_nodes++;
      } else if (vtable == kTreeNode) {
        census.trees.push_back({scene, n});
        ShowTree(base, n);
      } else if (kGrassNode && vtable == kGrassNode) {
        census.grass.push_back({scene, n});
        ShowGrass(base, n);
      }
    }
  }
}

void TakeScene(uint8_t* base, uint32_t scene, int depth, std::vector<Quad>& out, Census& census) {
  if (!Pointer(scene) || depth > 16 || census.scenes >= static_cast<int>(kMaxScenes)) return;
  census.scenes++;
  for (uint32_t child = g::ReadU32(base, scene + kSceneFirstChild), n = 0; Pointer(child) && n < kMaxScenes;
       child = g::ReadU32(base, child + kSceneNextSibling), ++n) {
    TakeScene(base, child, depth + 1, out, census);
  }
  const uint32_t target = g::ReadU32(base, scene + kSceneColorTarget);
  if (Pointer(target) && !(g::ReadU32(base, target + kTargetFlags) & kTargetVirtualScreen)) {
    census.offscreen_scenes++;
    return;
  }
  census.scene_order[scene] = static_cast<int>(census.scene_order.size());
  TakeList(base, scene, g::ReadU32(base, scene + kSceneOpaque), false, out, census);
  TakeList(base, scene, g::ReadU32(base, scene + kSceneBlended), true, out, census);
  // The third list: the far generic surfaces, and the grass. 0x82145088 (which makes every generic surface's node)
  // puts a surface there instead of in the opaque list when the camera is farther than 0x825A231C times its object's
  // size (object +0x8C, its sphere's radius, scene +0x30), and the scene's render (0x8214F770) draws it right after
  // the opaque list. Its surfaces are the near ones' kind (side41: the same surfaces, formats and materials, soldiers
  // among them), so they are drawn (side42: the distance filled in, no crumpled shape as in side39); trees would be
  // left out, but none came. The first few far surfaces are shown in full. The grass needs its own drawing.
  const size_t solids = census.solids.size(), trees = census.trees.size();
  TakeList(base, scene, g::ReadU32(base, scene + kSceneLate), true, out, census);
  static int far_shown = 0;
  for (size_t i = solids; i < census.solids.size() && far_shown < kShownSurfaces; ++i) {
    REXLOG_INFO("cod3: ngl side: a far surface (third list), node {:08X}:", census.solids[i].node);
    ShowSurface(base, census.solids[i].node, far_shown);
  }
  census.far_solids += static_cast<int>(census.solids.size() - solids);
  census.far_trees += static_cast<int>(census.trees.size() - trees);
  census.trees.resize(trees);
}

// The NGL texture as the listing shows it: its size and the Direct3D texture's format, tiling and packing.
std::string TextureText(uint8_t* base, uint32_t texture) {
  if (!Pointer(texture)) return "none";
  const uint32_t d3d = g::ReadU32(base, texture + kTextureDirect3D);
  char text[160];
  if (!Pointer(d3d)) {
    std::snprintf(text, sizeof text, "%08X (no Direct3D texture)", texture);
    return text;
  }
  const uint32_t f0 = g::ReadU32(base, d3d + kFetchConstant), f1 = g::ReadU32(base, d3d + kFetchConstant + 4);
  const uint32_t f2 = g::ReadU32(base, d3d + kFetchConstant + 8), f3 = g::ReadU32(base, d3d + kFetchConstant + 12);
  const uint32_t f5 = g::ReadU32(base, d3d + kFetchConstant + 20);
  std::snprintf(text, sizeof text, "%08X %ux%u format %u endian %u%s%s swizzle %03X flags %X", texture,
                (f2 & 0x1FFF) + 1, ((f2 >> 13) & 0x1FFF) + 1, f1 & 0x3F, (f1 >> 6) & 3, (f0 >> 31) ? " tiled" : "",
                ((f5 >> 11) & 1) ? " packed" : "", (f3 >> 1) & 0xFFF, g::ReadU32(base, texture + kTextureFlags));
  return text;
}

// The first big dump's listing: each quad (where, its colors, its texture) and string (its text, font and place).
void List(uint8_t* base, uint64_t frame, const std::vector<Quad>& quads, const Census& census) {
  int listed = 0;
  for (const Quad& q : quads) {
    if (listed++ >= kListedQuads) break;
    const uint32_t target = Pointer(q.scene) ? g::ReadU32(base, q.scene + kSceneColorTarget) : 0;
    REXLOG_INFO("cod3: ngl side: frame {} quad {}: ({:.1f}, {:.1f}) to ({:.1f}, {:.1f}), uv ({:.3f}, {:.3f}) to "
                "({:.3f}, {:.3f}), colors {:08X} {:08X}, +50 {:08X} {:08X} {:08X} {:08X}, scene {:08X} target {:08X} "
                "flags {:X}, texture {}",
                frame, listed, q.x[0], q.y[0], q.x[3], q.y[3], q.u[0], q.v[0], q.u[3], q.v[3], q.color[0], q.color[3],
                q.extra[0], q.extra[1], q.extra[2], q.extra[3], q.scene, target,
                Pointer(target) ? g::ReadU32(base, target + kTargetFlags) : 0, TextureText(base, q.texture));
  }
  static std::unordered_set<uint32_t> fonts_shown;
  for (const Text& t : census.texts) {
    if (!Pointer(t.font) || !fonts_shown.insert(t.font).second) continue;
    for (uint32_t row = 0; row < 0x80; row += 0x20) {
      uint32_t w[8];
      for (uint32_t k = 0; k < 8; ++k) w[k] = g::ReadU32(base, t.font + row + 4 * k);
      REXLOG_INFO("cod3: ngl side: font {:08X} +{:02X}: {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
                  t.font, row, w[0], w[1], w[2], w[3], w[4], w[5], w[6], w[7]);
    }
  }
  int shown = 0;
  for (const Text& t : census.texts) {
    if (shown++ >= 16) break;
    std::string printable;
    for (const char c : t.text) printable += (c >= 32 && c < 127) ? c : '?';
    REXLOG_INFO("cod3: ngl side: frame {} string \"{}\": font {:08X} (cell height {}, first {}, {} glyphs, texture "
                "{}), at ({:.1f}, {:.1f}), scale {:.2f} x {:.2f}, color {:08X}, {} glyphs drawn",
                frame, printable.substr(0, 60), t.font,
                Pointer(t.font) ? g::ReadU32(base, t.font + kFontCellHeight) : 0,
                Pointer(t.font) ? g::ReadU32(base, t.font + kFontFirst) : 0,
                Pointer(t.font) ? g::ReadU32(base, t.font + kFontCount) : 0,
                Pointer(t.font) ? TextureText(base, g::ReadU32(base, t.font + kFontTexture)) : std::string("none"), t.x,
                t.y, t.scale_x, t.scale_y, t.color, t.glyphs);
  }
}

// The other nodes' vtables, most first: "82012345 x120, ...".
std::string OthersText(const Census& census) {
  std::vector<std::pair<int, uint32_t>> sorted;
  for (const auto& [vtable, n] : census.others) sorted.emplace_back(n, vtable);
  std::sort(sorted.rbegin(), sorted.rend());
  std::string text;
  char item[32];
  for (size_t i = 0; i < sorted.size() && i < 8; ++i) {
    std::snprintf(item, sizeof item, "%s%08X x%d", i ? ", " : "", sorted[i].second, sorted[i].first);
    text += item;
  }
  return text.empty() ? "none" : text;
}

// Helpers the native draws and the diagnostics share (also built without ngl_vk).
[[maybe_unused]] float VertexFloat(uint8_t* base, uint32_t at, uint32_t endian) {
  uint32_t word = g::ReadU32(base, at);  // as the GPU reads it with 8-in-32 swapping
  if (endian == 0) word = __builtin_bswap32(word);
  float f;
  std::memcpy(&f, &word, 4);
  return f;
}

// Diagnostics: count registers from first of a constant table (4 floats each), as one line.
std::string RegistersText(const float* c, int first, int count) {
  std::string text;
  char item[96];
  for (int r = first; r < first + count; ++r) {
    std::snprintf(item, sizeof item, " c%d=(%g %g %g %g)", r, c[4 * r], c[4 * r + 1], c[4 * r + 2], c[4 * r + 3]);
    text += item;
  }
  return text;
}

bool GrassReadable(uint32_t a) { return Readable(a) || (!(a & 3) && a >= 0x40000000u && a < 0x80000000u); }

// The grass's vertices where the game's own grass draw fetches them: vertex fetch constant 95's two words (the
// address, then the size and byte order), kept at each of the game's grass draws (OnDraw). The vertex buffer object
// in the manager (+0x04) is another buffer (224,000 bytes at 0x04EB3000; the drawn one is 84,000 at 0x04E9F000):
// side72 read the blades from it and they came out tens of units wide.
std::atomic<uint64_t> g_grass_fetch{0};

#if defined(COD3_NGL_VK)
std::filesystem::path ScreenshotFolder() {
  std::error_code error;
  const std::filesystem::path exe = std::filesystem::read_symlink("/proc/self/exe", error);
  std::filesystem::path folder = (error ? std::filesystem::current_path() : exe.parent_path().parent_path()) /
                                 "userdata" / "logs" / "screenshots";
  std::filesystem::create_directories(folder, error);
  return folder;
}

int g_state = 0;  // 0 not started, 1 ready, -1 failed
std::unordered_map<uint32_t, uint32_t> g_textures;  // NGL texture -> ngl_vk texture (0: not drawable)
std::unordered_set<uint32_t> g_cut_out;             // ngl_vk textures with see-through parts (leaves, grass cards)
std::unordered_set<std::string> g_reported;

// A texture has see-through parts when at least one texel in 64 has alpha under one half: a color map like that is
// drawn with the alpha test (bushes and grass cards; run side22 drew them as solid cards).
bool HasCutOut(const std::vector<uint8_t>& bgra) {
  size_t clear = 0;
  for (size_t at = 3; at < bgra.size(); at += 4) clear += bgra[at] < 128;
  return clear * 64 >= bgra.size() / 4;
}

// An ngl_vk texture for the picture a fetch constant names: the game's own (or a pack's in its place) with its
// smaller levels. label names it in the log (the NGL texture, or the texture's address for a draw's fetch constant).
uint32_t MakeTexture(uint8_t* base, const uint32_t* fetch, uint32_t label, std::string& why) {
  std::vector<uint8_t> bgra;
  uint32_t width = 0, height = 0;
  if (!DecodeFetch(base, fetch, bgra, width, height, why)) return 0;
  const bool cut_out = HasCutOut(bgra);  // from the game's own picture
  static int keys_shown = 0;  // every texture's pack key, to check against the ripper's (hd_keys.tsv, and the
                              // insignia keys in docs/TEXTURE_REPLACEMENT.md)
  uint64_t key;
  uint32_t format;
  if (keys_shown < 4000 && TextureKeyFetch(base, fetch, key, format)) {
    ++keys_shown;
    REXLOG_INFO("cod3: ngl side: texture {:08X} key {:016x} ({}x{} format {})", label, key, width, height, format);
  }
  std::vector<uint8_t> hd;
  uint32_t hd_width = 0, hd_height = 0;
  // the smaller levels: the game's own, or made from a pack's picture (owner: far textures shimmered with the top
  // level alone); a cut-out's keep its coverage
  std::vector<std::vector<uint8_t>> levels;
  if (PackPicture(base, fetch, bgra, width, height, hd, hd_width, hd_height)) {
    bgra.swap(hd);
    width = hd_width;
    height = hd_height;
    BoxLevels(bgra, width, height, levels);
  } else {
    DecodeLevelsFetch(base, fetch, levels);
  }
  if (cut_out) KeepCoverage(bgra, levels);
  std::vector<const void*> pictures{bgra.data()};
  for (const std::vector<uint8_t>& level : levels) pictures.push_back(level.data());
  const uint32_t id =
      ngl_vk_texture_levels(pictures.data(), static_cast<uint32_t>(pictures.size()), width, height);
  static int levels_shown = 0;  // diagnostics: the first textures' level counts
  if (levels_shown < 12) {
    ++levels_shown;
    REXLOG_INFO("cod3: ngl side: texture {:08X} {}x{}: {} levels", label, width, height, pictures.size());
  }
  if (!id) why = "ngl_vk_texture failed";
  if (id && cut_out) g_cut_out.insert(id);
  return id;
}

uint32_t TextureFor(uint8_t* base, uint32_t texture) {
  if (!Pointer(texture)) return 0;
  if (g::ReadU32(base, texture + kTextureFlags) & kTextureAnimated) {
    const uint32_t frames = g::ReadU32(base, texture + kTextureFrames);
    if (Pointer(frames) && Pointer(g::ReadU32(base, frames))) texture = g::ReadU32(base, frames);
  }
  const auto found = g_textures.find(texture);
  if (found != g_textures.end()) return found->second;
  std::string why = "no Direct3D texture";
  uint32_t fetch[6];
  const uint32_t id =
      ReadFetch(base, g::ReadU32(base, texture + kTextureDirect3D), fetch) ? MakeTexture(base, fetch, texture, why) : 0;
  if (!id && g_reported.insert(why).second) {
    REXLOG_INFO("cod3: ngl side: texture {:08X} not drawn: {} (others like it aren't named again)", texture, why);
  }
  g_textures[texture] = id;
  return id;
}

void AddQuad(std::vector<ngl_vk_vertex>& v, const Quad& q, uint32_t texture) {
  static const int kCorners[6] = {0, 1, 2, 2, 1, 3};
  for (const int c : kCorners) {
    ngl_vk_vertex out;
    out.x = q.x[c];
    out.y = q.y[c] * kScreenSqueeze;
    out.u = q.u[c];
    out.v = q.v[c];
    out.color = q.color[c];
    out.textured = texture ? 2.0f : 0.0f;
    out.texture = texture;
    v.push_back(out);
  }
}

void SavePpm(const std::filesystem::path& path, const uint8_t* rgba, uint32_t width, uint32_t height) {
  std::FILE* file = std::fopen(path.string().c_str(), "wb");
  if (!file) return;
  std::fprintf(file, "P6\n%u %u\n255\n", width, height);
  std::vector<uint8_t> row(static_cast<size_t>(width) * 3);
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) std::memcpy(&row[x * 3], rgba + (static_cast<size_t>(y) * width + x) * 4, 3);
    std::fwrite(row.data(), 1, row.size(), file);
  }
  std::fclose(file);
}

// ---------------------------------------------------------------------------------------------------------------
// Step 4's 3D (see the top).

// A scene's camera for ngl_vk: its view-projection, depth turned over when it runs backward. False when it isn't a
// perspective camera (a shadow's, say) or its numbers aren't sane.
constexpr float kFrontDepth = 0.02f;  // the share of the depth range a scene drawn in front of the world gets
constexpr float kFrontNear = 0.1f;    // ... a scene whose near plane is closer than this share of the world's

// The distance to a scene's near plane, from its projection (row vectors: depth = (a z + b) / (c z + d), d being 0
// for a perspective): where depth is 1 when it runs backward, 0 when forward; the smaller positive one is the near
// plane either way (the other is the far). 0 when it can't be told.
float NearPlane(uint8_t* base, uint32_t scene) {
  const float a = g::ReadF32(base, scene + kSceneProjection + 4 * 10);
  const float b = g::ReadF32(base, scene + kSceneProjection + 4 * 14);
  const float c = g::ReadF32(base, scene + kSceneProjection + 4 * 11);
  float best = 0.0f;
  for (const float z : {c != a ? b / (c - a) : 0.0f, a != 0.0f ? -b / a : 0.0f}) {
    if (std::isfinite(z) && z > 0.0f && (best == 0.0f || z < best)) best = z;
  }
  return best;
}

bool SceneCamera(uint8_t* base, uint32_t scene, float view_projection[16], bool* reversed) {
  float projection[16];
  float sum = 0.0f;
  for (uint32_t k = 0; k < 16; ++k) {
    view_projection[k] = g::ReadF32(base, scene + kSceneViewProjection + 4 * k);
    projection[k] = g::ReadF32(base, scene + kSceneProjection + 4 * k);
    if (!std::isfinite(view_projection[k]) || !std::isfinite(projection[k])) return false;
    sum += std::fabs(view_projection[k]);
  }
  if (sum == 0.0f) return false;
  // the w column (Direct3D row-vector: element [row][column] is at 4 * row + column) moves with the position
  if (std::fabs(view_projection[3]) + std::fabs(view_projection[7]) + std::fabs(view_projection[11]) < 1e-6f) {
    return false;
  }
  auto depth = [&projection](float z) {
    const float w = z * projection[11] + projection[15];
    return (z * projection[10] + projection[14]) / (w != 0.0f ? w : 1.0f);
  };
  *reversed = depth(1000.0f) < depth(10.0f);
  if (*reversed) {
    for (int row = 0; row < 4; ++row) {
      view_projection[4 * row + 2] = view_projection[4 * row + 3] - view_projection[4 * row + 2];
    }
  }
  return true;
}

struct Geometry {
  uint32_t vertices = 0, vertex_start = 0, vertex_count = 0, vertex_bytes = 0, stride = 0, endian = 0;
  uint32_t indices = 0, index_start = 0, index_count = 0, index_bytes = 0, index_size = 2;
  int32_t position = -1;            // byte offsets in a vertex (-1: none)
  int32_t uv[2] = {-1, -1};         // texture coordinates 0 and 1
  bool uv_half[2] = {false, false}; // as two 16-bit floats, else two floats
  int32_t blend_weights = -1, blend_indices = -1;  // a skinned vertex's (offsets; -1: none)
  uint32_t blend_weights_type = 0, blend_indices_type = 0;
};

// Xbox 360 vertex element types (D3DDECLTYPE) and uses (D3DDECLUSAGE) read here.
constexpr uint32_t kDeclFloat2 = 0x2C23A5, kDeclFloat3 = 0x2A23B9, kDeclHalf2 = 0x2C235F;
constexpr uint32_t kUsagePosition = 0, kUsageBlendWeight = 1, kUsageBlendIndices = 2, kUsageTexcoord = 5;

// The surface's vertex declaration (surface +0x60: +0x00 the stride, +0x04 the elements, 12 bytes each: stream and
// offset (16 bits each), type, then method, use, use index, a pad byte; ended by stream 0xFF).
void ReadFormat(uint8_t* base, uint32_t surface, Geometry& geo) {
  const uint32_t format = g::ReadU32(base, surface + kSurfaceFormat);
  if (!Pointer(format)) return;
  geo.stride = g::ReadU32(base, format);
  const uint32_t elements = g::ReadU32(base, format + 4);
  if (!Pointer(elements)) return;
  for (uint32_t i = 0; i < 16; ++i) {
    const uint32_t e = elements + 12 * i;
    const uint32_t where = g::ReadU32(base, e), type = g::ReadU32(base, e + 4), use = g::ReadU32(base, e + 8);
    const uint32_t stream = where >> 16, offset = where & 0xFFFF;
    if (stream == 0xFF || stream == 0xFFFF) break;
    if (stream != 0) continue;
    const uint32_t usage = (use >> 16) & 0xFF, index = (use >> 8) & 0xFF;
    if (usage == kUsagePosition && index == 0 && type == kDeclFloat3) geo.position = static_cast<int32_t>(offset);
    if (usage == kUsageBlendWeight && index == 0) {
      geo.blend_weights = static_cast<int32_t>(offset);
      geo.blend_weights_type = type;
    }
    if (usage == kUsageBlendIndices && index == 0) {
      geo.blend_indices = static_cast<int32_t>(offset);
      geo.blend_indices_type = type;
    }
    if (usage == kUsageTexcoord && index < 2 && (type == kDeclFloat2 || type == kDeclHalf2)) {
      geo.uv[index] = static_cast<int32_t>(offset);
      geo.uv_half[index] = type == kDeclHalf2;
    }
  }
}

bool ReadGeometry(uint8_t* base, uint32_t surface, Geometry& geo, std::string& why) {
  for (uint32_t k = 0; k < 2; ++k) {
    const uint32_t at = surface + kSurfaceBuffers + 0x10 * k;
    const uint32_t buffer = g::ReadU32(base, at);
    if (!Pointer(buffer)) continue;
    const uint32_t f0 = g::ReadU32(base, buffer + kBufferFetch), f1 = g::ReadU32(base, buffer + kBufferFetch + 4);
    if ((f0 & 3) == 3) {  // a vertex fetch constant: address, then endianness and size in words
      geo.vertices = GuestAddress(f0 & ~3u);
      geo.endian = f1 & 3;
      geo.vertex_bytes = ((f1 >> 2) & 0xFFFFFF) * 4;
      geo.vertex_start = g::ReadU32(base, at + 4);
      geo.vertex_count = g::ReadU32(base, at + 8);
    } else {
      geo.indices = GuestAddress(f0);
      geo.index_bytes = f1;
      geo.index_start = g::ReadU32(base, at + 4);
      geo.index_count = g::ReadU32(base, at + 8);
      geo.index_size = g::ReadU32(base, at + 12) == 4 ? 4 : 2;
    }
  }
  geo.stride = g::ReadU32(base, surface + kSurfaceStride);
  ReadFormat(base, surface, geo);
  if (!geo.vertices || !geo.indices) {
    why = "no vertex or index buffer";
  } else if (!Physical(geo.vertices) || !Physical(geo.indices)) {
    why = "a buffer outside physical memory";
  } else if (geo.stride < 12 || geo.stride > 256 || (geo.stride & 3)) {
    why = "stride not 12..256";
  } else if (geo.position < 0 || static_cast<uint32_t>(geo.position) + 12 > geo.stride) {
    why = "no three-float position";
  } else if (geo.endian != 0 && geo.endian != 2) {
    why = "vertex endianness " + std::to_string(geo.endian);
  } else if (geo.vertex_count == 0 || geo.vertex_count > 65536 || geo.index_count < 3 || geo.index_count > (1u << 22)) {
    why = "counts out of range";
  } else if ((static_cast<uint64_t>(geo.vertex_start) + geo.vertex_count) * geo.stride > geo.vertex_bytes) {
    why = "vertices past their buffer";
  } else if ((static_cast<uint64_t>(geo.index_start) + geo.index_count) * geo.index_size > geo.index_bytes) {
    why = "indices past their buffer";
  } else {
    return true;
  }
  return false;
}

float HalfFloat(uint32_t h) {
  const uint32_t sign = (h >> 15) & 1, exponent = (h >> 10) & 31, mantissa = h & 1023;
  float value = exponent == 0 ? std::ldexp(static_cast<float>(mantissa), -24)
                              : std::ldexp(static_cast<float>(mantissa | 1024), static_cast<int>(exponent) - 25);
  if (exponent == 31) value = 0.0f;  // infinities and NaNs: none in texture coordinates
  return sign ? -value : value;
}

// Texture coordinates k of the vertex at `at` into u, v (0, 0 when the format has none).
void VertexUv(uint8_t* base, uint32_t at, const Geometry& geo, int k, float* u, float* v) {
  *u = *v = 0.0f;
  if (geo.uv[k] < 0 || static_cast<uint32_t>(geo.uv[k]) + (geo.uv_half[k] ? 4u : 8u) > geo.stride) return;
  const uint32_t where = at + static_cast<uint32_t>(geo.uv[k]);
  if (geo.uv_half[k]) {
    uint32_t word = g::ReadU32(base, where);
    if (geo.endian == 0) word = __builtin_bswap32(word);
    *u = HalfFloat(word >> 16);  // the first of the two in the word's high half (8-in-32 order)
    *v = HalfFloat(word & 0xFFFF);
  } else {
    *u = VertexFloat(base, where, geo.endian);
    *v = VertexFloat(base, where + 4, geo.endian);
  }
}

std::unordered_map<uint64_t, uint32_t> g_meshes;  // (vertices, indices) -> ngl_vk mesh (0: not drawable)
bool g_meshes_full = false;

// The surface's mesh, made once; 0 (and why) when it can't be.
uint32_t MeshFor(uint8_t* base, uint32_t surface, std::string& why) {
  Geometry geo;
  if (!ReadGeometry(base, surface, geo, why)) return 0;
  const uint64_t key = static_cast<uint64_t>(geo.vertices + geo.vertex_start * geo.stride) << 32 |
                       (geo.indices + geo.index_start * geo.index_size);
  const auto found = g_meshes.find(key);
  if (found != g_meshes.end()) {
    if (!found->second) why = "left out before";
    return found->second;
  }
  uint32_t& mesh = g_meshes[key];
  float low[3], high[3];
  for (int k = 0; k < 3; ++k) {
    low[k] = g::ReadF32(base, surface + kSurfaceBoxLow + 4 * k);
    high[k] = g::ReadF32(base, surface + kSurfaceBoxHigh + 4 * k);
  }
  const float pad = 1.0f + 0.05f * std::max({high[0] - low[0], high[1] - low[1], high[2] - low[2]});
  std::vector<float> corners(static_cast<size_t>(geo.vertex_count) * NGL_VK_CORNER_FLOATS, 0.0f);
  const uint32_t white = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < geo.vertex_count; ++i) {
    const uint32_t at = geo.vertices + (geo.vertex_start + i) * geo.stride;
    float* c = &corners[static_cast<size_t>(i) * NGL_VK_CORNER_FLOATS];
    VertexUv(base, at, geo, 0, &c[3], &c[4]);
    VertexUv(base, at, geo, 1, &c[5], &c[6]);
    for (int k = 0; k < 3; ++k) {
      c[k] = VertexFloat(base, at + static_cast<uint32_t>(geo.position) + 4 * k, geo.endian);
      if (!std::isfinite(c[k]) || (i < 16 && (c[k] < low[k] - pad || c[k] > high[k] + pad))) {
        why = "positions outside the box";
        return 0;
      }
    }
    std::memcpy(&c[7], &white, 4);
  }
  // the indices, in the byte order that keeps them all inside the vertices
  std::vector<uint16_t> indices(geo.index_count);
  for (int order = 0; order < 2; ++order) {
    bool fits = true;
    for (uint32_t i = 0; i < geo.index_count && fits; ++i) {
      const uint8_t* p = base + geo.indices + (geo.index_start + i) * geo.index_size;
      uint32_t value;
      if (geo.index_size == 4) {
        value = order == 0 ? (uint32_t{p[0]} << 24 | uint32_t{p[1]} << 16 | uint32_t{p[2]} << 8 | p[3])
                           : (uint32_t{p[3]} << 24 | uint32_t{p[2]} << 16 | uint32_t{p[1]} << 8 | p[0]);
      } else {
        value = order == 0 ? (uint32_t{p[0]} << 8 | p[1]) : (uint32_t{p[1]} << 8 | p[0]);
      }
      fits = value < geo.vertex_count;
      indices[i] = static_cast<uint16_t>(value);
    }
    if (fits) {
      const uint32_t triangles = geo.index_count / 3 * 3;
      mesh = ngl_vk_mesh(corners.data(), geo.vertex_count, indices.data(), triangles);
      if (!mesh) {
        why = "ngl_vk has no room";
        g_meshes_full = true;
      }
      return mesh;
    }
  }
  why = "indices past the vertices";
  return 0;
}

// Skinned surfaces (their declaration has BLENDINDICES as UBYTE4 and BLENDWEIGHT as UBYTE4N, run side18): each of a
// vertex's four index bytes names a palette slot and the weight byte in the same place its share; slot s is the
// skeleton's bone given by the 16-bit number at surface +0x70 + 2s; the object holds the bones' matrices (+0xA4 their
// count, +0xA8 the 4x4s, row-vector, 64 bytes each: each bone's pose in the object's space, run side19). A vertex is
// in the model's bind space, so it goes through the bone's inverse bind pose first, then its pose. The pose changes
// every frame, so a skinned surface's corners are made for this frame only (ngl_vk_frame_corners), its mesh giving the
// triangles.
constexpr uint32_t kSurfaceBones = 0x70;
constexpr uint32_t kObjectBoneCount = 0xA4;
constexpr uint32_t kObjectBones = 0xA8;
constexpr uint32_t kMaxSkinnedCorners = 65536;
// The skeleton (run side21): object +0x80 is the model, model +0x10 its skeleton {name, bone count, records}. A bone's
// record is 0x90 bytes: its bind pose against its parent's (+0x00), its inverse bind pose, the model's space to the
// bone's (+0x40), its name (+0x80) and its parent's number (+0x84, -1 for the root). Each pose is four rows (x, y, z
// and the translation) of three floats and a padding word. Checked: the inverse of a bone's +0x40 is its +0x00 times
// its parent's bind pose.
constexpr uint32_t kObjectModel = 0x80;
constexpr uint32_t kModelSkeleton = 0x10;
constexpr uint32_t kSkeletonBoneCount = 0x04;
constexpr uint32_t kSkeletonBones = 0x08;
constexpr uint32_t kBoneRecord = 0x90;
constexpr uint32_t kBoneInverseBind = 0x40;
constexpr bool kSkin = true;

uint32_t U16(uint8_t* base, uint32_t at) { return uint32_t{base[at]} << 8 | base[at + 1]; }

// The skinned position of the vertex at `at` (its own position p), or false when none of its bones is usable.
// `remap`: the slot names a bone through the surface's list (else the slot is the bone itself).
bool SkinPosition(uint8_t* base, uint32_t surface, uint32_t object, const Geometry& geo, uint32_t at, const float p[3],
                  bool remap, float out[3]) {
  const uint32_t count = g::ReadU32(base, object + kObjectBoneCount), bones = g::ReadU32(base, object + kObjectBones);
  if (!Physical(bones) || count == 0 || count > 512) return false;
  const uint32_t model = g::ReadU32(base, object + kObjectModel);
  const uint32_t skeleton = Readable(model) ? g::ReadU32(base, model + kModelSkeleton) : 0;
  if (!Readable(skeleton) || g::ReadU32(base, skeleton + kSkeletonBoneCount) < count) return false;
  const uint32_t records = g::ReadU32(base, skeleton + kSkeletonBones);
  if (!Readable(records)) return false;
  const uint32_t slots = g::ReadU32(base, at + static_cast<uint32_t>(geo.blend_indices));
  const uint32_t weights = g::ReadU32(base, at + static_cast<uint32_t>(geo.blend_weights));
  float sum[3] = {0.0f, 0.0f, 0.0f}, total = 0.0f;
  for (int k = 0; k < 4; ++k) {
    const float weight = static_cast<float>((weights >> (8 * k)) & 255u);
    if (weight == 0.0f) continue;
    const uint32_t slot = (slots >> (8 * k)) & 255u;
    const uint32_t bone = remap ? U16(base, surface + kSurfaceBones + 2 * slot) : slot;
    if (bone >= count) continue;
    const uint32_t inverse = records + bone * kBoneRecord + kBoneInverseBind;
    float v[3];  // the vertex in the bone's space
    for (int c = 0; c < 3; ++c) {
      v[c] = p[0] * g::ReadF32(base, inverse + 4 * c) + p[1] * g::ReadF32(base, inverse + 16 + 4 * c) +
             p[2] * g::ReadF32(base, inverse + 32 + 4 * c) + g::ReadF32(base, inverse + 48 + 4 * c);
    }
    float m[16];
    for (uint32_t e = 0; e < 16; ++e) m[e] = g::ReadF32(base, bones + bone * 64 + 4 * e);
    for (int c = 0; c < 3; ++c) sum[c] += weight * (v[0] * m[c] + v[1] * m[4 + c] + v[2] * m[8 + c] + m[12 + c]);
    total += weight;
  }
  if (total <= 0.0f) return false;
  for (int c = 0; c < 3; ++c) out[c] = sum[c] / total;
  return std::isfinite(out[0]) && std::isfinite(out[1]) && std::isfinite(out[2]);
}

// This frame's corners of a skinned surface (0 when it isn't skinned or can't be): positions skinned, texture
// coordinates as the mesh's.
uint32_t SkinnedCorners(uint8_t* base, uint32_t surface, uint32_t object, uint64_t frame) {
  Geometry geo;
  std::string why;
  if (!ReadGeometry(base, surface, geo, why) || geo.blend_indices < 0 || geo.blend_weights < 0 ||
      geo.vertex_count > kMaxSkinnedCorners) {
    return 0;
  }
  static int probes = 0;
  std::vector<float> corners(static_cast<size_t>(geo.vertex_count) * NGL_VK_CORNER_FLOATS, 0.0f);
  const uint32_t white = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < geo.vertex_count; ++i) {
    const uint32_t at = geo.vertices + (geo.vertex_start + i) * geo.stride;
    float* c = &corners[static_cast<size_t>(i) * NGL_VK_CORNER_FLOATS];
    float p[3];
    for (int k = 0; k < 3; ++k) p[k] = VertexFloat(base, at + static_cast<uint32_t>(geo.position) + 4 * k, geo.endian);
    if (!SkinPosition(base, surface, object, geo, at, p, true, c)) std::memcpy(c, p, sizeof p);
    if (i == 0 && probes < 4) {
      ++probes;
      float direct[3] = {0, 0, 0};
      const bool ok = SkinPosition(base, surface, object, geo, at, p, false, direct);
      REXLOG_INFO("cod3: ngl side: frame {}: skinned surface {:08X} corner 0 ({:.1f}, {:.1f}, {:.1f}), slots {:08X} "
                  "weights {:08X}, bones {} at {:08X}: through the list ({:.1f}, {:.1f}, {:.1f}), slot as bone "
                  "{}({:.1f}, {:.1f}, {:.1f}); list {}", frame, surface, p[0], p[1], p[2],
                  g::ReadU32(base, at + static_cast<uint32_t>(geo.blend_indices)),
                  g::ReadU32(base, at + static_cast<uint32_t>(geo.blend_weights)),
                  g::ReadU32(base, object + kObjectBoneCount), g::ReadU32(base, object + kObjectBones), c[0], c[1],
                  c[2], ok ? "" : "(none) ", direct[0], direct[1], direct[2],
                  Words(base, surface + kSurfaceBones, 8));
    }
    VertexUv(base, at, geo, 0, &c[3], &c[4]);
    VertexUv(base, at, geo, 1, &c[5], &c[6]);
    std::memcpy(&c[7], &white, 4);
  }
  return ngl_vk_frame_corners(corners.data(), geo.vertex_count);
}

// One corner of a draw followed through: the surface's first vertex, in the world and in clip space, to check the
// matrices against the picture.
void Probe(uint8_t* base, uint64_t frame, uint32_t surface, const ngl_vk_mesh_draw& d) {
  Geometry geo;
  std::string why;
  if (!ReadGeometry(base, surface, geo, why)) return;
  const uint32_t at = geo.vertices + geo.vertex_start * geo.stride + static_cast<uint32_t>(geo.position);
  const float p[4] = {VertexFloat(base, at, geo.endian), VertexFloat(base, at + 4, geo.endian),
                      VertexFloat(base, at + 8, geo.endian), 1.0f};
  float world[4], clip[4];
  for (int column = 0; column < 4; ++column) {
    world[column] = 0.0f;
    for (int row = 0; row < 4; ++row) world[column] += p[row] * d.world[4 * row + column];
  }
  for (int column = 0; column < 4; ++column) {
    clip[column] = 0.0f;
    for (int row = 0; row < 4; ++row) clip[column] += world[row] * d.view_projection[4 * row + column];
  }
  const float w = clip[3] != 0.0f ? clip[3] : 1.0f;
  float u0, v0, u1, v1;
  VertexUv(base, at - static_cast<uint32_t>(geo.position), geo, 0, &u0, &v0);
  VertexUv(base, at - static_cast<uint32_t>(geo.position), geo, 1, &u1, &v1);
  REXLOG_INFO("cod3: ngl side: surface {:08X}: stride {}, position at {}, uv0 at {}{} ({:.3f}, {:.3f}), uv1 at {}{} "
              "({:.3f}, {:.3f}), texture {}", surface, geo.stride, geo.position, geo.uv[0],
              geo.uv_half[0] ? " (16-bit)" : "", u0, v0, geo.uv[1], geo.uv_half[1] ? " (16-bit)" : "", u1, v1,
              d.texture);
  REXLOG_INFO("cod3: ngl side: frame {}: surface {:08X}'s first corner ({:.1f}, {:.1f}, {:.1f}): world ({:.1f}, "
              "{:.1f}, {:.1f}), clip ({:.2f}, {:.2f}, {:.2f}, {:.2f}), on screen ({:.3f}, {:.3f}) depth {:.4f}",
              frame, surface, p[0], p[1], p[2], world[0], world[1], world[2], clip[0], clip[1], clip[2], clip[3],
              clip[0] / w, clip[1] / w, clip[2] / w);
}

// side93: a skinned surface of the front scene (the first-person arms, missing from the native picture): its first
// corner skinned, then through world x view-projection (what ngl_vk is handed) and through the object's own +0x40 (its
// world-view-projection, as the gun's), with the object's position and bone 0's translation, to see which space the
// bones leave a vertex in.
// side95: also once for a skinned surface of the world (a soldier, drawn right), to set its bones beside the arms'.
void ProbeFrontSkinned(uint8_t* base, uint32_t surface, uint32_t object, const ngl_vk_mesh_draw& d, bool front) {
  static std::mutex probed_mutex;
  static std::unordered_set<uint32_t> probed;
  static int world_probed = 0;
  {
    std::lock_guard lock(probed_mutex);
    if (!front && world_probed >= 1) return;
    if (probed.size() >= 6 || !probed.insert(surface).second) return;
    world_probed += !front;
  }
  Geometry geo;
  std::string why;
  if (!ReadGeometry(base, surface, geo, why) || geo.blend_indices < 0 || geo.blend_weights < 0) return;
  const uint32_t at = geo.vertices + geo.vertex_start * geo.stride;
  float p[3], s[3] = {0.0f, 0.0f, 0.0f};
  for (int k = 0; k < 3; ++k) p[k] = VertexFloat(base, at + static_cast<uint32_t>(geo.position) + 4 * k, geo.endian);
  const bool skinned = SkinPosition(base, surface, object, geo, at, p, true, s);
  float wvp[16], own[16];
  for (int r = 0; r < 4; ++r) {
    for (int c = 0; c < 4; ++c) {
      float sum = 0.0f;
      for (int k = 0; k < 4; ++k) sum += d.world[4 * r + k] * d.view_projection[4 * k + c];
      wvp[4 * r + c] = sum;
      own[4 * r + c] = g::ReadF32(base, object + 0x40 + 4 * static_cast<uint32_t>(4 * r + c));
    }
  }
  auto clip = [&](const float* m, float out[4]) {
    for (int c = 0; c < 4; ++c) out[c] = s[0] * m[c] + s[1] * m[4 + c] + s[2] * m[8 + c] + m[12 + c];
  };
  float a[4], b[4];
  clip(wvp, a);
  clip(own, b);
  const uint32_t bones = g::ReadU32(base, object + kObjectBones);
  float bone0[3] = {0.0f, 0.0f, 0.0f};
  if (Physical(bones)) {
    for (int k = 0; k < 3; ++k) bone0[k] = g::ReadF32(base, bones + 48 + 4 * static_cast<uint32_t>(k));
  }
  auto ndc = [](const float v[4], int i) { return v[3] != 0.0f ? v[i] / v[3] : 0.0f; };
  REXLOG_INFO("cod3: ngl side: {} skinned surface {:08X} of object {:08X}: corner 0 ({:.2f}, {:.2f}, {:.2f}) "
              "skinned{} ({:.2f}, {:.2f}, {:.2f}); object at ({:.1f}, {:.1f}, {:.1f}); bone 0 at ({:.2f}, {:.2f}, "
              "{:.2f}); through world x view-projection: w {:.3f}, on screen ({:.3f}, {:.3f}); through +0x40: w "
              "{:.3f}, on screen ({:.3f}, {:.3f})", front ? "front" : "world", surface, object, p[0], p[1], p[2], skinned ? "" : " (failed)", s[0],
              s[1], s[2], d.world[12], d.world[13], d.world[14], bone0[0], bone0[1], bone0[2], a[3], ndc(a, 0),
              ndc(a, 1), b[3], ndc(b, 0), ndc(b, 1));
  // side94: the numbers themselves, to work out the arms' space offline: the object's matrix, bone 0's pose and its
  // inverse bind pose, and the scene's view-projection, row by row
  auto rows = [&](uint32_t at, int count, int stride_words) {
    std::string text;
    char item[24];
    for (int r = 0; r < count; ++r) {
      for (int c = 0; c < 4; ++c) {
        const uint32_t word = static_cast<uint32_t>(stride_words * r + c);
        std::snprintf(item, sizeof item, " %.4g", g::ReadF32(base, at + 4 * word));
        text += item;
      }
      text += r + 1 < count ? " |" : "";
    }
    return text;
  };
  std::string vp;
  char item[24];
  for (int k = 0; k < 16; ++k) {
    std::snprintf(item, sizeof item, " %.4g%s", d.view_projection[k], k % 4 == 3 && k < 15 ? " |" : "");
    vp += item;
  }
  const uint32_t model = g::ReadU32(base, object + kObjectModel);
  const uint32_t skeleton = Readable(model) ? g::ReadU32(base, model + kModelSkeleton) : 0;
  const uint32_t records = Readable(skeleton) ? g::ReadU32(base, skeleton + kSkeletonBones) : 0;
  REXLOG_INFO("cod3: ngl side: {} skinned object {:08X}: matrix{}; bone 0 pose{}; bone 0 inverse bind{}; "
              "view-projection{}", front ? "front" : "world", object, rows(object, 4, 4), Physical(bones) ? rows(bones, 4, 4) : std::string(" -"),
              Readable(records) ? rows(records + kBoneInverseBind, 4, 4) : std::string(" -"), vp);
}

// ---------------------------------------------------------------------------------------------------------------
// SpeedTree (runs side23-24). A tree node's record (node +0x0C) is {kind, ?, ?, the tree}: kind 0x8207B7E0 draws
// the tree's branches (sub_823F5C80), 0x8207B7F8 its fronds (sub_823F5868), 0x8207B810 its leaves (sub_823F5028,
// not drawn yet). Branches and fronds are each one vertex buffer and, by level of detail, an index buffer of 32-bit
// indices drawn as one triangle strip (sub_820C7818, type 6) whose length is a 16-bit number in the lengths list.
// A vertex starts with a float4 position in the tree's own space (z up); the base texture's u, v are at +32
// (strides 0x74 and 0x4C, from the vertex format globals 0x82A1B8F4 and 0x82A1B8E8). The tree's instances are a list
// from tree +0x10 (next at +0x98); each has its position at +0x10, its turn about z in degrees at +0x20 and its
// scale at +0x24 (sent to the vertex shader; the game picks a level of detail per instance, here it's always 0).
#if defined(COD3_MP)
constexpr uint32_t kTreeBranches = 0x8207A748, kTreeFronds = 0x8207A760, kTreeLeaves = 0x8207A778;  // (1 of 1)
#else
constexpr uint32_t kTreeBranches = 0x8207B7E0, kTreeFronds = 0x8207B7F8, kTreeLeaves = 0x8207B810;
#endif
constexpr uint32_t kTreeRecordTree = 0x0C;
constexpr uint32_t kTreeFirstInstance = 0x10;
constexpr uint32_t kInstancePosition = 0x10, kInstanceTurn = 0x20, kInstanceScale = 0x24, kInstanceNext = 0x98;
constexpr uint32_t kMaxTreeInstances = 1024;
constexpr uint32_t kTreeUv = 32;

struct TreePart {
  const char* name;
  uint32_t buffer, index_buffers, lengths, vertices, stride, texture;
};
// The texture words are a guess to check: branches set tree +0x2B0 and +0x2E8, fronds +0x2CC and +0x2E8.
const TreePart kBranchPart{"branches", 0x1C8, 0x1CC, 0x1D0, 0x54, 0x74, 0x2B0};
const TreePart kFrondPart{"fronds", 0x1D4, 0x1D8, 0x1DC, 0xAC, 0x4C, 0x2CC};
// Leaves: buffer = the list of vertex buffers by level, lengths = the list of level records (see LeafMesh).
const TreePart kLeafPart{"leaves", 0x1E0, 0, 0xF8, 0, 0x6C, 0x2CC};

// The heap's virtual pages, where the trees keep their D3D buffer headers: followed only where the game's own draw
// follows them.
bool Heap(uint32_t address) { return !(address & 3) && address >= 0x40000000u && address < 0x80000000u; }

std::unordered_map<uint64_t, uint32_t> g_tree_meshes;   // tree << 32 | part's buffer word -> mesh (0: not drawable)
std::unordered_map<uint32_t, uint32_t> g_d3d_textures;  // D3D texture -> ngl_vk texture (0: not drawable)

// An ngl_vk texture for a word that is an NGL texture or a bare D3D texture (fetch constant at +0x1C), else 0.
uint32_t AnyTextureFor(uint8_t* base, uint32_t word) {
  if (IsTexture(base, word)) return TextureFor(base, word);
  if (!(Readable(word) || Heap(word)) || (g::ReadU32(base, word + kFetchConstant) & 3) != 2) return 0;
  const auto found = g_d3d_textures.find(word);
  if (found != g_d3d_textures.end()) return found->second;
  std::vector<uint8_t> bgra;
  uint32_t width = 0, height = 0, id = 0;
  std::string why;
  if (Decode(base, word, bgra, width, height, why)) {
    id = ngl_vk_texture(bgra.data(), width, height);
    if (id && HasCutOut(bgra)) g_cut_out.insert(id);
    for (size_t i = 0; i + 3 < bgra.size(); i += 4) std::swap(bgra[i], bgra[i + 2]);
    char name[64];
    std::snprintf(name, sizeof name, "ngl-side-d3d-%08X.ppm", word);
    SavePpm(ScreenshotFolder() / name, bgra.data(), width, height);
  }
  REXLOG_INFO("cod3: ngl side: D3D texture {:08X}: {}x{} {}", word, width, height, id ? "made" : why.c_str());
  g_d3d_textures[word] = id;
  return id;
}

uint32_t TreeMesh(uint8_t* base, uint32_t tree, const TreePart& part, std::string& why) {
  const uint64_t key = static_cast<uint64_t>(tree) << 32 | part.buffer;
  const auto found = g_tree_meshes.find(key);
  if (found != g_tree_meshes.end()) {
    if (!found->second) why = "left out before";
    return found->second;
  }
  uint32_t& mesh = g_tree_meshes[key];
  const uint32_t buffer = g::ReadU32(base, tree + part.buffer);
  const uint32_t index_buffers = g::ReadU32(base, tree + part.index_buffers);
  const uint32_t lengths = g::ReadU32(base, tree + part.lengths);
  const uint32_t count = g::ReadU32(base, tree + part.vertices);
  if (!Heap(buffer) || !Readable(index_buffers) || !Readable(lengths) || count < 3 || count > 65535) {
    why = "no buffers";
    return 0;
  }
  const uint32_t vertices = GuestAddress(g::ReadU32(base, buffer + kBufferFetch) & ~3u);
  const uint32_t first = g::ReadU32(base, index_buffers);
  if (!Physical(vertices) || !Heap(first)) {
    why = "buffers outside memory";
    return 0;
  }
  const uint32_t indices = GuestAddress(g::ReadU32(base, first + 0x18));
  const uint32_t index_bytes = g::ReadU32(base, first + 0x1C);
  const uint32_t length = U16(base, lengths);
  if (!Physical(indices) || length < 3 || length * 4 > index_bytes) {
    why = "a strip longer than its index buffer";
    return 0;
  }
  std::vector<float> corners(static_cast<size_t>(count) * NGL_VK_CORNER_FLOATS, 0.0f);
  const uint32_t white = 0xFFFFFFFFu;
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t at = vertices + i * part.stride;
    float* c = &corners[static_cast<size_t>(i) * NGL_VK_CORNER_FLOATS];
    for (int k = 0; k < 3; ++k) c[k] = g::ReadF32(base, at + 4 * k);
    c[3] = g::ReadF32(base, at + kTreeUv);
    c[4] = g::ReadF32(base, at + kTreeUv + 4);
    if (!std::isfinite(c[0]) || !std::isfinite(c[1]) || !std::isfinite(c[2])) {
      why = "positions not numbers";
      return 0;
    }
    std::memcpy(&c[7], &white, 4);
  }
  std::vector<uint16_t> list;  // the strip as a triangle list, its joining (degenerate) triangles left out
  list.reserve(static_cast<size_t>(length) * 3);
  for (uint32_t k = 0; k + 2 < length; ++k) {
    uint32_t a = g::ReadU32(base, indices + 4 * k), b = g::ReadU32(base, indices + 4 * k + 4);
    const uint32_t c = g::ReadU32(base, indices + 4 * k + 8);
    if (a >= count || b >= count || c >= count) {
      why = "indices past the vertices";
      return 0;
    }
    if (a == b || b == c || a == c) continue;
    if (k & 1) std::swap(a, b);
    list.push_back(static_cast<uint16_t>(a));
    list.push_back(static_cast<uint16_t>(b));
    list.push_back(static_cast<uint16_t>(c));
  }
  mesh = ngl_vk_mesh(corners.data(), count, list.data(), static_cast<uint32_t>(list.size()));
  if (!mesh) why = "ngl_vk has no room";
  static int shown = 0;
  if (shown < 6) {
    ++shown;
    REXLOG_INFO("cod3: ngl side: tree {:08X} {}: {} vertices, strip of {} -> {} triangles, mesh {}", tree, part.name,
                count, length, list.size() / 3, mesh);
  }
  return mesh;
}

// A tree's leaves (side27). The game draws a level's leaves (a 72-byte record in the list at tree +0xF8: +4 its
// vertex buffer's number in the list at tree +0x1E0, +8 its leaf count) as six vertices a leaf (sub_820C78D0,
// a triangle list), turned to face the camera in the vertex shader. A leaf vertex (stride 0x6C) holds the leaf's
// center (float4 at 0, the tree's space), the atlas u, v and the corner's number 0-3 (at +32), and the leaf's width
// and height (at +48). Corner 0 is the top right, 1 top left, 2 bottom left, 3 bottom right. Here each leaf is two
// fixed crossed cards (in the x-z and y-z planes, both faces), so one mesh serves every instance and every camera.
uint32_t LeafMesh(uint8_t* base, uint32_t tree, std::string& why) {
  const uint64_t key = static_cast<uint64_t>(tree) << 32 | kLeafPart.buffer;
  const auto found = g_tree_meshes.find(key);
  if (found != g_tree_meshes.end()) {
    if (!found->second) why = "left out before";
    return found->second;
  }
  uint32_t& mesh = g_tree_meshes[key];
  const uint32_t levels = g::ReadU32(base, tree + kLeafPart.lengths);
  const uint32_t buffers = g::ReadU32(base, tree + kLeafPart.buffer);
  if (!Readable(levels) || !Readable(buffers)) {
    why = "no leaf levels";
    return 0;
  }
  const uint32_t number = g::ReadU32(base, levels + 4), leaves = g::ReadU32(base, levels + 8);
  const uint32_t buffer = number < 8 ? g::ReadU32(base, buffers + 4 * number) : 0;
  if (!Heap(buffer) || leaves == 0 || leaves > 4096) {
    why = "no leaf buffer";
    return 0;
  }
  const uint32_t vertices = GuestAddress(g::ReadU32(base, buffer + kBufferFetch) & ~3u);
  if (!Physical(vertices)) {
    why = "leaf buffer outside memory";
    return 0;
  }
  static const float kCorner[4][2] = {{0.5f, 0.5f}, {-0.5f, 0.5f}, {-0.5f, -0.5f}, {0.5f, -0.5f}};
  std::vector<float> corners;
  std::vector<uint16_t> list;
  corners.reserve(static_cast<size_t>(leaves) * 8 * NGL_VK_CORNER_FLOATS);
  const uint32_t white = 0xFFFFFFFFu;
  for (uint32_t leaf = 0; leaf < leaves && leaf < 65536 / 8; ++leaf) {
    float u[4] = {0, 0, 0, 0}, v[4] = {0, 0, 0, 0}, center[3] = {0, 0, 0}, size[2] = {0, 0};
    bool seen[4] = {false, false, false, false};
    for (uint32_t k = 0; k < 6; ++k) {
      const uint32_t at = vertices + (leaf * 6 + k) * kLeafPart.stride;
      const int corner = static_cast<int>(g::ReadF32(base, at + 40));
      if (corner < 0 || corner > 3) continue;
      seen[corner] = true;
      u[corner] = g::ReadF32(base, at + 32);
      v[corner] = g::ReadF32(base, at + 36);
      if (k == 0) {
        for (int c = 0; c < 3; ++c) center[c] = g::ReadF32(base, at + 4 * c);
        size[0] = g::ReadF32(base, at + 48);
        size[1] = g::ReadF32(base, at + 52);
      }
    }
    if (!(seen[0] && seen[1] && seen[2] && seen[3]) || !std::isfinite(center[0] + center[1] + center[2]) ||
        !(size[0] > 0.0f && size[0] < 10000.0f && size[1] > 0.0f && size[1] < 10000.0f)) {
      continue;
    }
    for (int plane = 0; plane < 2; ++plane) {  // the x-z card, then the y-z card
      const uint16_t first = static_cast<uint16_t>(corners.size() / NGL_VK_CORNER_FLOATS);
      for (int corner = 0; corner < 4; ++corner) {
        float c[NGL_VK_CORNER_FLOATS] = {};
        const float across = kCorner[corner][0] * size[0], up = kCorner[corner][1] * size[1];
        c[0] = center[0] + (plane == 0 ? across : 0.0f);
        c[1] = center[1] + (plane == 1 ? across : 0.0f);
        c[2] = center[2] + up;
        c[3] = u[corner];
        c[4] = v[corner];
        std::memcpy(&c[7], &white, 4);
        corners.insert(corners.end(), c, c + NGL_VK_CORNER_FLOATS);
      }
      static const uint16_t kFaces[12] = {0, 1, 2, 0, 2, 3, 0, 2, 1, 0, 3, 2};  // both faces
      for (const uint16_t f : kFaces) list.push_back(static_cast<uint16_t>(first + f));
    }
  }
  const uint32_t count = static_cast<uint32_t>(corners.size() / NGL_VK_CORNER_FLOATS);
  if (count == 0) {
    why = "no leaves read";
    return 0;
  }
  mesh = ngl_vk_mesh(corners.data(), count, list.data(), static_cast<uint32_t>(list.size()));
  if (!mesh) why = "ngl_vk has no room";
  static int shown = 0;
  if (shown < 4) {
    ++shown;
    REXLOG_INFO("cod3: ngl side: tree {:08X} leaves: level 0 buffer {} with {} leaves -> {} cards, mesh {}", tree,
                number, leaves, count / 4, mesh);
  }
  return mesh;
}

// The frame's trees (branches, fronds and leaves) as ngl_vk draws: every instance of each tree drawn.
// The grass (side59), drawn by ngl_vk's translation of the game's own grass shaders (ngl_vk/shaders/grass.vert,
// grass.frag; NOTES 2026-11-05 16:02). The strip of blades is made once from the grass manager's vertex and index
// buffers (Direct3D's: the vertex buffer's fetch constant at +0x18/+0x1C, the index buffer's data at +0x18, its size at
// +0x1C; big-endian, turned around here). A frame's constants: c12..c15 the meshes' view-projection (as Direct3D's
// columns), c16..c51 what NGL's pass sends (its compiled list at +0x80: one {base, offset} a vector; the list at
// +0x140 goes with another vertex shader, one without c44..c47), c80..c82 and b0 what 0x823F6DF0 sets from the
// manager's parameters; a patch's c84/c85 and its count of indices as 0x823F6DF0 and 0x823F1C70 work them out
// (cod3mp's 0x824101C8 and 0x8240B6E8).
constexpr uint32_t kGrassVertexBuffer = 0x04, kGrassVertexCount = 0x24, kGrassIndexBuffer = 0x28;
constexpr uint32_t kGrassIndexMost = 0x2C, kGrassIndexStep = 0x30, kGrassLodScale = 0x34;
constexpr uint32_t kGrassSpacing = 0x4C;  // +0x08 a cell's size (1000), +0x0C a patch's (100)
constexpr uint32_t kGrassHalfPatch = 0x194, kGrassHalfCell = 0x198, kGrassPatchesARow = 0x19C;
constexpr uint32_t kGrassBend = 0xC0;
// 0x8290E9AC is NGL's scene being drawn (set only while the render job draws it, so 0 here): the grass's own
// scene stands for it (+0x420 the time the wind runs on).
#if defined(COD3_MP)
constexpr uint32_t kGrassFade = 0x8268362C;     // c82.x's numerator (40 of 41)
#else
constexpr uint32_t kGrassFade = 0x825CE628;     // c82.x's numerator
#endif
constexpr uint32_t kGrassVertexBytes = NGL_VK_GRASS_VERTEX_BYTES;
constexpr size_t kGrassPatches = 1024;

// Diagnostics: a texture as decoded here, its channels (red, green, blue, alpha: means, lows, highs) and its middle
// texel, to check the grass's inputs against what its shader expects.
std::string TexelStats(uint8_t* base, uint32_t texture) {
  std::vector<uint8_t> bgra;
  uint32_t width = 0, height = 0;
  std::string why;
  if (!Pointer(texture) || !Decode(base, g::ReadU32(base, texture + kTextureDirect3D), bgra, width, height, why)) {
    return "not decoded (" + why + ")";
  }
  double sum[4] = {};
  int low[4] = {255, 255, 255, 255}, high[4] = {};
  const size_t texels = static_cast<size_t>(width) * height;
  for (size_t i = 0; i < texels; ++i) {
    const uint8_t* p = &bgra[i * 4];
    const int rgba[4] = {p[2], p[1], p[0], p[3]};
    for (int k = 0; k < 4; ++k) {
      sum[k] += rgba[k];
      low[k] = std::min(low[k], rgba[k]);
      high[k] = std::max(high[k], rgba[k]);
    }
  }
  const uint8_t* m = &bgra[(static_cast<size_t>(height / 2) * width + width / 2) * 4];
  char text[240];
  std::snprintf(text, sizeof text,
                "%ux%u, means %.0f %.0f %.0f %.0f, lows %d %d %d %d, highs %d %d %d %d, middle %d %d %d %d", width,
                height, sum[0] / texels, sum[1] / texels, sum[2] / texels, sum[3] / texels, low[0], low[1], low[2],
                low[3], high[0], high[1], high[2], high[3], m[2], m[1], m[0], m[3]);
  return text;
}

// Diagnostics: a texture as an 8x8 grid of mean brightness (0-255, rows top to bottom), to see its picture's shape.
std::string TexelGrid(uint8_t* base, uint32_t texture) {
  std::vector<uint8_t> bgra;
  uint32_t width = 0, height = 0;
  std::string why;
  if (!Pointer(texture) || !Decode(base, g::ReadU32(base, texture + kTextureDirect3D), bgra, width, height, why)) {
    return "not decoded (" + why + ")";
  }
  double sum[64] = {};
  int count[64] = {};
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t* p = &bgra[(static_cast<size_t>(y) * width + x) * 4];
      const int cell = static_cast<int>(y * 8 / height) * 8 + static_cast<int>(x * 8 / width);
      sum[cell] += 0.3 * p[2] + 0.59 * p[1] + 0.11 * p[0];
      ++count[cell];
    }
  }
  std::string text;
  char item[8];
  for (int cell = 0; cell < 64; ++cell) {
    std::snprintf(item, sizeof item, "%s%.0f", cell % 8 ? " " : " | ", count[cell] ? sum[cell] / count[cell] : 0.0);
    text += item;
  }
  return text;
}

struct GrassFrame {
  std::vector<float> constants;  // c0..c95
  uint32_t bend = 0, blade = 0;
  std::vector<ngl_vk_grass_patch> patches;
};

void ShowGrassInputs(uint8_t* base, const GrassFrame& frame, uint32_t scene) {
  REXLOG_INFO("cod3: ngl side: grass inputs: scene {:08X} time {} (current scene {:08X}); ours:{}", scene,
              g::ReadF32(base, scene + 0x420), g::ReadU32(base, kCurrentScene),
              RegistersText(frame.constants.data(), 12, 4) + RegistersText(frame.constants.data(), 80, 3));
  if (frame.patches.empty()) return;
  const ngl_vk_grass_patch& p = frame.patches.front();
  REXLOG_INFO("cod3: ngl side: grass inputs: first patch c84 ({} {} {} {}) c85 ({} {} {} {}), {} indices", p.place[0],
              p.place[1], p.place[2], p.place[3], p.place[4], p.place[5], p.place[6], p.place[7], p.index_count);
}

// A manager parameter's first float (its value at +0x14).
float GrassParameter(uint8_t* base, uint32_t manager, uint32_t at) {
  const uint32_t parameter = g::ReadU32(base, manager + at);
  if (!GrassReadable(parameter)) return 0.0f;
  const uint32_t value = g::ReadU32(base, parameter + 0x14);
  return GrassReadable(value) ? g::ReadF32(base, value) : 0.0f;
}

int g_grass_mesh = 0;  // 0 not tried, 1 made, -1 couldn't be

// Diagnostics (side72): bytes this side made, saved beside the screenshots, to set beside the game's own
// (ngl-side-grass-vb95.bin) on the PC: side71's blades came out 5-10 times wider than grass.vert's math on the game's
// buffers says.
void SaveHostBytes(const void* data, size_t bytes, const std::string& name) {
  const std::filesystem::path path = ScreenshotFolder() / name;
  std::ofstream out(path, std::ios::binary);
  out.write(static_cast<const char*>(data), static_cast<std::streamsize>(bytes));
  REXLOG_INFO("cod3: ngl side: grass: saved {} bytes to {}", bytes, path.string());
}

// The picture a grass texture is decoded to here (width, height, then BGRA), for the same comparison.
void SaveGrassPicture(uint8_t* base, uint32_t texture, const std::string& name) {
  std::vector<uint8_t> bgra;
  uint32_t size[2] = {0, 0};
  std::string why;
  if (!Pointer(texture) || !Decode(base, g::ReadU32(base, texture + kTextureDirect3D), bgra, size[0], size[1], why)) {
    REXLOG_INFO("cod3: ngl side: grass: {} not saved ({})", name, why);
    return;
  }
  std::vector<uint8_t> file(sizeof size + bgra.size());
  std::memcpy(file.data(), size, sizeof size);
  std::memcpy(file.data() + sizeof size, bgra.data(), bgra.size());
  SaveHostBytes(file.data(), file.size(), name);
}

void MakeGrassMesh(uint8_t* base, uint32_t manager) {
  if (g_grass_mesh) return;
  const uint64_t drawn = g_grass_fetch.load(std::memory_order_relaxed);
  if (!drawn) return;  // the game hasn't drawn its grass yet: tried again next frame
  g_grass_mesh = -1;
  const uint32_t vertex_buffer = g::ReadU32(base, manager + kGrassVertexBuffer);
  const uint32_t index_buffer = g::ReadU32(base, manager + kGrassIndexBuffer);
  const uint32_t vertices = g::ReadU32(base, manager + kGrassVertexCount);
  const uint32_t indices = g::ReadU32(base, manager + kGrassIndexMost);
  if (!GrassReadable(vertex_buffer) || !GrassReadable(index_buffer) || !vertices || vertices > 65535 || !indices ||
      indices > (1u << 20)) {
    REXLOG_INFO("cod3: ngl side: grass: no blades ({:08X}, {:08X}, {} vertices, {} indices)", vertex_buffer,
                index_buffer, vertices, indices);
    return;
  }
  const uint32_t fetch = static_cast<uint32_t>(drawn >> 32), size = static_cast<uint32_t>(drawn);
  const uint32_t vertex_data = GuestAddress(fetch & 0x1FFFFFFCu);
  const uint32_t index_data = GuestAddress(g::ReadU32(base, index_buffer + 0x18));
  REXLOG_INFO("cod3: ngl side: grass: vertices at {:08X}, as the game's draw fetches them; the manager's vertex buffer "
              "object {:08X}: {}",
              vertex_data, vertex_buffer, Words(base, vertex_buffer, 12));
  // The drawn address is kept nowhere a native-only path could read it so far: not within two pointers of the
  // manager, not in the heap pages of the game's own grass buffer objects, not in the image (side79-81,
  // acct2-bro/NOTES.md 18:47). So it is taken from the game's grass draw.
  if ((fetch & 3) != 3 || ((size >> 2) & 0xFFFFFF) * 4 < vertices * kGrassVertexBytes || !Physical(vertex_data) ||
      (g::ReadU32(base, index_buffer) & 0x80000000u) || g::ReadU32(base, index_buffer + 0x1C) < indices * 2 ||
      !Physical(index_data)) {
    REXLOG_INFO("cod3: ngl side: grass: blades not read (vertex fetch {:08X} {:08X}, index buffer {})", fetch, size,
                Words(base, index_buffer, 8));
    return;
  }
  std::vector<uint8_t> blades(static_cast<size_t>(vertices) * kGrassVertexBytes);
  for (uint32_t i = 0; i < vertices; ++i) {
    const uint32_t at = vertex_data + kGrassVertexBytes * i;
    const uint32_t root = g::ReadU32(base, at), shape = g::ReadU32(base, at + 4), variety = g::ReadU32(base, at + 8);
    uint8_t* out = blades.data() + static_cast<size_t>(kGrassVertexBytes) * i;
    for (int k = 0; k < 4; ++k) {  // 16_16: x in the low half; 8_8_8_8: x in the low byte
      out[k] = static_cast<uint8_t>(root >> (8 * k));
      out[4 + k] = static_cast<uint8_t>(shape >> (8 * k));
      out[8 + k] = static_cast<uint8_t>(variety >> (8 * k));
    }
  }
  std::vector<uint16_t> strip(indices);
  for (uint32_t i = 0; i < indices; ++i) strip[i] = g::ReadU16(base, index_data + 2 * i);
  SaveHostBytes(blades.data(), blades.size(), "ngl-side-grass-ours-vb.bin");  // diagnostics (side72)
  SaveHostBytes(strip.data(), strip.size() * sizeof strip[0], "ngl-side-grass-ours-ib.bin");
  if (ngl_vk_grass_mesh(blades.data(), vertices, strip.data(), indices) != 0) {
    REXLOG_INFO("cod3: ngl side: grass: ngl_vk took no blades (no grass pipeline?)");
    return;
  }
  g_grass_mesh = 1;
  REXLOG_INFO("cod3: ngl side: grass: {} vertices and {} indices made", vertices, indices);
}

GrassFrame GrassFor(uint8_t* base, const Census& census) {
  GrassFrame frame;
  if (census.grass.empty()) return frame;
  const Solid& node = census.grass.front();
  const uint32_t object = g::ReadU32(base, node.node + 0x0C), set = g::ReadU32(base, node.node + 0x10);
  const uint32_t manager = GrassReadable(object) ? g::ReadU32(base, object) : 0;
  const uint32_t ngl = GrassReadable(manager) ? g::ReadU32(base, manager) : 0;
  const uint32_t globals = node.scene;
  float view_projection[16];
  bool reversed = false;
  const bool camera = SceneCamera(base, node.scene, view_projection, &reversed);
  if (!GrassReadable(ngl) || !GrassReadable(set) || !GrassReadable(globals) || !camera) {
    static bool told = false;
    if (!told) {
      told = true;
      REXLOG_INFO("cod3: ngl side: grass: not drawn (material {:08X}, NGL material {:08X}, patch set {:08X}, scene "
                  "{:08X} {})",
                  manager, ngl, set, globals, camera ? "with a camera" : "without a perspective camera");
    }
    return frame;
  }
  MakeGrassMesh(base, manager);
  if (g_grass_mesh != 1) return frame;
  float* c = (frame.constants = std::vector<float>(NGL_VK_GRASS_CONSTANTS * 4, 0.0f)).data();
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) c[(12 + column) * 4 + row] = view_projection[4 * row + column];
  }
  // c16..c51 as NGL's pass sends them (0x82155EC0)
  static int grass_frames = 0;  // diagnostics: on the second frame drawn here, these registers and where each came from
  const bool show_list = ++grass_frames == 2;
  std::string list_text;
  const uint32_t holder = g::ReadU32(base, ngl + 0x0C);
  const uint32_t pass = GrassReadable(holder) ? g::ReadU32(base, holder + 0x0C) : 0;
  const uint32_t compiled = GrassReadable(pass) ? g::ReadU32(base, pass + 0xDC) : 0;
  if (GrassReadable(compiled)) {
    const uint32_t list = compiled + 0x80;
    const uint32_t bases[8] = {0, g::ReadU32(base, ngl + 0x1C), globals, kPassBase3, kPassBase4, 0, kPassBase6,
                               g::ReadU32(base, kPassBase7Pointer)};
    const uint32_t count = g::ReadU32(base, list + 0x08), rows = g::ReadU32(base, list + 0x20);
    const uint32_t first = (g::ReadU32(base, list + 0x1C) & 0x7FF) / 4;
    if (show_list) {
      char item[96];
      std::snprintf(item, sizeof item, " list %08X: %u rows from c%u, header %08X %08X;", list, count, first,
                    g::ReadU32(base, list + 0x18), g::ReadU32(base, list + 0x1C));
      list_text += item;
    }
    for (uint32_t k = 0; k < count && first + k < NGL_VK_GRASS_CONSTANTS && GrassReadable(rows); ++k) {
      const uint32_t slot = g::ReadU32(base, rows + 8 * k) / 4, offset = g::ReadU32(base, rows + 8 * k + 4);
      const uint32_t value = ((slot < 8 ? bases[slot] : 0) + offset) & ~15u;
      if (show_list) {
        char item[64];
        std::snprintf(item, sizeof item, " c%u slot %u+%X (%08X)", first + k, slot, offset, value);
        list_text += item;
      }
      if (!GrassReadable(value)) continue;
      for (uint32_t j = 0; j < 4; ++j) c[(first + k) * 4 + j] = g::ReadF32(base, value + 4 * j);
    }
  }
  if (show_list) {
    REXLOG_INFO("cod3: ngl side: grass constants from NGL's pass {:08X}:{}", pass, list_text);
    REXLOG_INFO("cod3: ngl side: grass constants, ours:{}{}{}{}{}", RegistersText(c, 24, 6), RegistersText(c, 34, 5),
                RegistersText(c, 40, 1), RegistersText(c, 42, 6), RegistersText(c, 50, 1));
  }
  // c80..c82 and b0 (0x823F6DF0)
  auto parameter = [&](uint32_t at) { return GrassParameter(base, manager, at); };
  const float c80[4] = {parameter(0x8C) * g::ReadF32(base, globals + 0x420), parameter(0x84) - parameter(0x80),
                        parameter(0x98) - 1.0f, parameter(0xA4) - 1.0f};
  const float c81[4] = {parameter(0x70) * 0.5f, parameter(0x74) * 0.5f, parameter(0x90) * 0.5f,
                        parameter(0x94) * 0.5f};
  const float range = parameter(0x90) - parameter(0x94);
  const float c82[4] = {range != 0.0f ? g::ReadF32(base, kGrassFade) / range : 0.0f, 0.7f, 0.0f, 0.0f};
  std::memcpy(c + 80 * 4, c80, sizeof c80);
  std::memcpy(c + 81 * 4, c81, sizeof c81);
  std::memcpy(c + 82 * 4, c82, sizeof c82);
  frame.bend = parameter(kGrassBend) != 0.0f;
  // the blade texture: the material's "grass_bladetexture"; and its "time", which the wind runs on when the scene
  // here has none (the game's c80.x is the time of the scene being drawn, a copy this side never sees)
  float time = -1.0f;
  int listed = 0;
  for (uint32_t p = g::ReadU32(base, ngl + 0x30); GrassReadable(p) && listed < 64; p = g::ReadU32(base, p + 0x0C)) {
    ++listed;
    const uint32_t name = g::ReadU32(base, p + 0x04), value = g::ReadU32(base, p + 0x14);
    if (!GrassReadable(name) || !GrassReadable(value)) continue;
    const char* text = reinterpret_cast<const char*>(base + name + 4);
    if (std::string(text, strnlen(text, 32)) == "time") time = g::ReadF32(base, value);
    if (std::string(text, strnlen(text, 32)) == "grass_bladetexture") {
      const uint32_t blade = g::ReadU32(base, value);
      frame.blade = TextureFor(base, blade);
      static bool blade_shown = false;  // diagnostics: the blade's picture as decoded here
      if (!blade_shown) {
        blade_shown = true;
        REXLOG_INFO("cod3: ngl side: grass blade texture {}: {}; brightness{}", TextureText(base, blade),
                    TexelStats(base, blade), TexelGrid(base, blade));
      }
    }
  }
  if (c80[0] == 0.0f && time > 0.0f) c[80 * 4] = parameter(0x8C) * time;
  // the patches
  const uint32_t spacing = g::ReadU32(base, manager + kGrassSpacing);
  const int per_row = static_cast<int>(g::ReadU32(base, manager + kGrassPatchesARow));
  if (!GrassReadable(spacing) || per_row <= 0) return frame;
  const float cell_size = g::ReadF32(base, spacing + 8), patch_size = g::ReadF32(base, spacing + 0x0C);
  const float half_patch = g::ReadF32(base, manager + kGrassHalfPatch);
  const float half_cell = g::ReadF32(base, manager + kGrassHalfCell);
  const uint32_t step = g::ReadU32(base, manager + kGrassIndexStep), most = g::ReadU32(base, manager + kGrassIndexMost);
  const float lod_scale = g::ReadF32(base, manager + kGrassLodScale);
  const uint32_t cells = g::ReadU32(base, set), list = g::ReadU32(base, set + 4);
  const uint32_t count = g::ReadU32(base, set + 0x0C) & 0xFFFF;
  uint32_t first_color = 0, first_heights = 0;  // diagnostics (side72): the first patch's two textures
  for (uint32_t k = 0; k < count && frame.patches.size() < kGrassPatches && GrassReadable(list); ++k) {
    const uint32_t patch = g::ReadU32(base, list + 4 * k);
    if (!GrassReadable(patch)) continue;
    const uint32_t cell = cells + 8 * (g::ReadU32(base, patch + 0x0C) >> 16);
    const uint32_t kind = g::ReadU32(base, cell), data = g::ReadU32(base, cell + 4);
    const uint32_t descriptor = GrassReadable(kind) ? g::ReadU32(base, kind) : 0;
    if (!GrassReadable(descriptor) || !GrassReadable(data)) continue;
    const uint32_t color = g::ReadU32(base, data + 0x0C), heights = g::ReadU32(base, data + 0x08);
    if (!color || !heights) continue;
    const int cells_a_row = static_cast<int>(g::ReadU32(base, descriptor + 0x1C));
    const uint32_t cell_at = g::ReadU32(base, data), patch_at = g::ReadU32(base, patch);
    if (cells_a_row <= 0 || !GrassReadable(cell_at) || !GrassReadable(patch_at)) continue;
    const int cell_index = static_cast<int>(g::ReadU32(base, cell_at));
    const int patch_index = static_cast<int>(g::ReadU32(base, patch_at));
    const float cell_y = static_cast<float>(cell_index / cells_a_row) * cell_size + g::ReadF32(base, descriptor + 8);
    const float cell_x = static_cast<float>(cell_index % cells_a_row) * cell_size + g::ReadF32(base, descriptor + 4);
    const float y = static_cast<float>(patch_index / per_row) * patch_size + cell_y + half_patch;
    const float x = static_cast<float>(patch_index % per_row) * patch_size + cell_x + half_patch;
    ngl_vk_grass_patch out{};
    out.color_texture = TextureFor(base, color);
    out.cell_texture = TextureFor(base, heights);
    static int textures_shown = 0;  // diagnostics: the first two cells' textures as decoded here
    static uint32_t last_shown = 0;
    if (textures_shown < 2 && data != last_shown) {
      ++textures_shown;
      last_shown = data;
      REXLOG_INFO("cod3: ngl side: grass cell {:08X}: color texture {}: {}", data, TextureText(base, color),
                  TexelStats(base, color));
      REXLOG_INFO("cod3: ngl side: grass cell {:08X}: cell texture {}: {}", data, TextureText(base, heights),
                  TexelStats(base, heights));
    }
    out.place[0] = ((x - cell_x) - half_cell) * 0.001f + 0.5f;
    out.place[1] = -(((y - cell_y) - half_cell) * 0.001f - 0.5f);
    out.place[2] = x;
    out.place[3] = y;
    const float low = g::ReadF32(base, descriptor + 0x0C);
    out.place[4] = low;
    out.place[5] = g::ReadF32(base, descriptor + 0x18) - low;
    const uint32_t wanted = step * static_cast<uint32_t>(std::max(0.0f, lod_scale * g::ReadF32(base, patch + 4)));
    out.index_count = wanted < step ? step : wanted > most ? most : wanted;
    if (frame.patches.empty()) {
      first_color = color;
      first_heights = heights;
    }
    frame.patches.push_back(out);
  }
  static bool told = false;
  if (!told) {
    told = true;
    REXLOG_INFO("cod3: ngl side: grass: {} of {} patches, blade texture {}, c80 ({} {} {} {}), c82.x {}, b0 {}",
                frame.patches.size(), count, frame.blade, c80[0], c80[1], c80[2], c80[3], c82[0], frame.bend);
    ShowGrassInputs(base, frame, globals);
    // diagnostics (side72): what ngl_vk is handed, to emulate grass.vert on exactly it on the PC; and the patches'
    // places, to set beside the game's own draws' c84 and c85 (the "grass draw" lines)
    std::vector<float> handed(frame.constants);
    for (size_t p = 0; p < frame.patches.size() && p < 16; ++p) {
      handed.insert(handed.end(), frame.patches[p].place, frame.patches[p].place + 8);
    }
    SaveHostBytes(handed.data(), handed.size() * sizeof(float), "ngl-side-grass-ours-constants.bin");
    SaveGrassPicture(base, first_color, "ngl-side-grass-color0.bgra");
    SaveGrassPicture(base, first_heights, "ngl-side-grass-cell0.bgra");
    std::string places;
    for (size_t p = 0; p < frame.patches.size() && p < 12; ++p) {
      const float* q = frame.patches[p].place;
      char item[96];
      std::snprintf(item, sizeof item, " (%g %g %g %g / %g %g)", q[0], q[1], q[2], q[3], q[4], q[5]);
      places += item;
    }
    REXLOG_INFO("cod3: ngl side: grass: first patches' c84 / c85:{}", places);
  }
  return frame;
}

void SetGrass(const GrassFrame& grass) {
  if (grass.constants.empty() || grass.patches.empty()) return;
  ngl_vk_set_grass(grass.constants.data(), grass.bend, grass.blade, grass.patches.data(),
                   static_cast<uint32_t>(grass.patches.size()));
}

void TreeDraws(uint8_t* base, const Census& census, std::vector<ngl_vk_mesh_draw>& draws,
               std::unordered_map<std::string, int>& left_out) {
  std::unordered_set<uint64_t> done;  // a tree's part is drawn once a frame, all its instances
  std::unordered_map<uint32_t, std::pair<bool, std::array<float, 16>>> cameras;
  for (const Solid& t : census.trees) {
    const uint32_t record = g::ReadU32(base, t.node + 0x0C);
    if (!Readable(record)) continue;
    const uint32_t kind = g::ReadU32(base, record);
    const TreePart* part = kind == kTreeBranches ? &kBranchPart
                           : kind == kTreeFronds ? &kFrondPart
                           : kind == kTreeLeaves ? &kLeafPart
                                                 : nullptr;
    if (!part) {  // others, by kind
      char label[64];
      std::snprintf(label, sizeof label, "(tree nodes of kind %08X not drawn yet)", kind);
      left_out[label]++;
      continue;
    }
    const uint32_t tree = g::ReadU32(base, record + kTreeRecordTree);
    if (!Readable(tree) || done.count(static_cast<uint64_t>(tree) << 32 | part->buffer)) continue;
    auto camera = cameras.find(t.scene);
    if (camera == cameras.end()) {
      std::array<float, 16> vp{};
      bool reversed = false;
      const bool ok = SceneCamera(base, t.scene, vp.data(), &reversed);
      camera = cameras.emplace(t.scene, std::make_pair(ok, vp)).first;
    }
    if (!camera->second.first) {  // the shadow map's scene (side25: every tree part is in one first)
      left_out["(trees: in a scene without a perspective camera)"]++;
      continue;
    }
    done.insert(static_cast<uint64_t>(tree) << 32 | part->buffer);
    std::string why;
    const uint32_t mesh = kind == kTreeLeaves ? LeafMesh(base, tree, why) : TreeMesh(base, tree, *part, why);
    if (!mesh) {
      left_out["(trees: " + why + ")"]++;
      continue;
    }
    ngl_vk_mesh_draw d{};
    d.mesh = mesh;
    d.texture = AnyTextureFor(base, g::ReadU32(base, tree + part->texture));
    d.tint = kind == kTreeBranches ? 0x5A4632u : 0x3C6432u;
    if (kind == kTreeLeaves && d.texture) g_cut_out.insert(d.texture);  // the leaf atlas is always cut out
    d.shading = NGL_VK_SHADE_LIGHT;
    d.alpha = g_cut_out.count(d.texture) ? NGL_VK_ALPHA_TEST : NGL_VK_ALPHA_OPAQUE;
    FogWith(base, t.scene, d);
    std::memcpy(d.view_projection, camera->second.second.data(), sizeof d.view_projection);
    uint32_t instance = g::ReadU32(base, tree + kTreeFirstInstance);
    for (uint32_t n = 0; Physical(instance) && n < kMaxTreeInstances && draws.size() < kMaxDraws; ++n) {
      const float x = g::ReadF32(base, instance + kInstancePosition);
      const float y = g::ReadF32(base, instance + kInstancePosition + 4);
      const float z = g::ReadF32(base, instance + kInstancePosition + 8);
      const float turn = g::ReadF32(base, instance + kInstanceTurn) * 3.14159265f / 180.0f;
      const float scale = g::ReadF32(base, instance + kInstanceScale);
      instance = g::ReadU32(base, instance + kInstanceNext);
      if (!std::isfinite(x + y + z + turn + scale) || scale <= 0.0f || scale > 100.0f) {
        left_out["(trees: an instance not sane)"]++;
        continue;
      }
      const float c = std::cos(turn) * scale, s = std::sin(turn) * scale;
      const float world[16] = {c, s, 0, 0, -s, c, 0, 0, 0, 0, scale, 0, x, y, z, 1};
      std::memcpy(d.world, world, sizeof world);
      draws.push_back(d);
      left_out[kind == kTreeBranches ? "(trees: branches drawn)"
               : kind == kTreeFronds ? "(trees: fronds drawn)"
                                     : "(trees: leaves drawn)"]++;
    }
  }
}

// The frame's generic surfaces as ngl_vk draws, scene by scene; the reasons any are left out, counted.
std::vector<ngl_vk_mesh_draw> SolidDraws(uint8_t* base, uint64_t frame, const Census& census) {
  std::vector<ngl_vk_mesh_draw> draws;
  std::unordered_map<std::string, int> left_out;
  struct Camera {
    bool ok, reversed, front, other;
    float view_projection[16];
  };
  std::unordered_map<uint32_t, Camera> cameras;
  static std::unordered_set<uint32_t> scenes_shown;
  // The scene with the most generic surfaces is the world. The first-person gun and arms have a scene of their own
  // whose camera's near plane is far closer (0.05 units, its far 128, against the world's 6 and 6500); the game
  // keeps them in front of the world, but ngl_vk has one depth buffer for everything, so such a scene's depth is
  // squeezed into the nearest 2% (as Big Red One's ngl_present.c does for the scene after its world). It isn't told
  // by order: side45 found the arms in a scene drawn first (order 0, no target of its own).
  std::unordered_map<uint32_t, int> per_scene;
  for (const Solid& s : census.solids) per_scene[s.scene]++;
  uint32_t world_scene = 0;
  int most = 0;
  for (const auto& [scene, n] : per_scene) {
    if (n > most) {
      most = n;
      world_scene = scene;
    }
  }
  const float world_near = world_scene ? NearPlane(base, world_scene) : 0.0f;
  for (const Solid& s : census.solids) {
    auto camera = cameras.find(s.scene);
    if (camera == cameras.end()) {
      Camera c{};
      c.ok = SceneCamera(base, s.scene, c.view_projection, &c.reversed);
      const float near_plane = NearPlane(base, s.scene);
      if (c.ok && s.scene != world_scene && world_near > 0.0f && near_plane > 0.0f &&
          near_plane < kFrontNear * world_near) {
        for (int row = 0; row < 4; ++row) c.view_projection[4 * row + 2] *= kFrontDepth;
        c.front = true;
        left_out["(scenes in front of the world)"]++;
      } else if (c.ok && s.scene != world_scene) {
        c.other = true;  // side83: neither the world nor the gun's (the third scene of side43-45)
      }
      camera = cameras.emplace(s.scene, c).first;
      if (scenes_shown.size() < kShownScenes && scenes_shown.insert(s.scene).second) {
        const uint32_t target = g::ReadU32(base, s.scene + kSceneColorTarget);
        REXLOG_INFO("cod3: ngl side: 3D scene {:08X}: target {:08X} flags {:X}, {} camera{}", s.scene, target,
                    Pointer(target) ? g::ReadU32(base, target + kTargetFlags) : 0,
                    c.ok ? "a perspective" : "not a perspective", c.ok && c.reversed ? ", depth backward" : "");
        for (uint32_t row = 0x100; row < 0x200; row += 0x20) {
          REXLOG_INFO("cod3: ngl side: 3D scene {:08X} +{:03X}: {}", s.scene, row, Words(base, s.scene + row, 8));
        }
      }
    }
    if (!camera->second.ok) {
      left_out["its scene has no perspective camera"]++;
      continue;
    }
    // The third perspective scene (side45: drawn first, no target of its own, near plane 0.43 to 4.6 units, up to 31
    // skinned surfaces): drawn through its own camera it put a soldier's body and a large hand near the bottom of the
    // picture where the game shows none (side76-82). Left out, nothing else visible goes (side83): most likely the
    // pass that draws the soldiers into their shadow texture.
    if (camera->second.other) {
      left_out["(side83: the third perspective scene)"]++;
      continue;
    }
    const uint32_t object = g::ReadU32(base, s.node + kNodeObject);
    const uint32_t surface = g::ReadU32(base, s.node + kNodeSurface);
    if (!Pointer(object) || !Pointer(surface)) {
      left_out["no object or surface"]++;
      continue;
    }
    if (g_meshes_full) {
      left_out["ngl_vk has no room"]++;
      continue;
    }
    std::string why;
    const uint32_t mesh = MeshFor(base, surface, why);
    if (!mesh) {
      left_out[why]++;
      continue;
    }
    ngl_vk_mesh_draw d{};
    d.mesh = mesh;
    const uint32_t material = g::ReadU32(base, s.node + kNodeMaterial);
    d.tint = 0x404040u | (((material * 2654435761u) >> 8) & 0xBFBFBFu);
    Geometry geo;
    std::string unused;
    ReadGeometry(base, surface, geo, unused);
    const bool skinned = geo.blend_weights >= 0 && geo.blend_indices >= 0;
    if (!s.old_class) {  // an old class's node has no shader state and its material no sampler list
      ShowSamplers(base, material, geo.stride, skinned);
      ShowShaderState(base, g::ReadU32(base, s.node + kNodeShaderState));
      HuntFog(base, g::ReadU32(base, s.node + kNodeShaderState));
    }
    const TextureFrame look_frame = FrameFor(base, s.scene, object);
    Samplers samplers = s.old_class ? Samplers{} : MaterialSamplers(base, material, skinned, look_frame.value);
    if (s.old_class) {
      if (s.old_class == kOldWorld) samplers.lightmap = SecondTexture(base, material);
      left_out[s.old_class == kOldWorld ? "(of those, cdWorld's nodes)" : "(of those, cdSimple's nodes)"]++;
      static int old_shown = 0;
      if (old_shown < 6) {
        ++old_shown;
        REXLOG_INFO("cod3: ngl side: {} node {:08X}: object {:08X}, surface {:08X}, material {:08X}: {} / {}; color "
                    "map {:08X}, lightmap {:08X}; stride {}", s.old_class == kOldWorld ? "cdWorld" : "cdSimple",
                    s.node, object, surface, material, Words(base, material, 8), Words(base, material + 0x20, 8),
                    MaterialTexture(base, material), samplers.lightmap, geo.stride);
      }
    }
    if (samplers.looks) {
      left_out[look_frame.own ? "(of those, a look picked by the object's frame)"
                              : "(of those, a look picked by the scene's frame)"]++;
      static int looks_shown = 0;
      if (looks_shown < 16) {
        ++looks_shown;
        const uint32_t name = g::ReadU32(base, samplers.looks + kTextureName);
        REXLOG_INFO("cod3: ngl side: looks {:08X} \"{}\" ({} of them): object {:08X} frame {} ({}) -> {:08X}",
                    samplers.looks, Pointer(name) ? InlineName(base, name + 4) : std::string(),
                    g::ReadU32(base, samplers.looks + kLookCount), object, look_frame.value,
                    look_frame.own ? "its own" : "the scene's", samplers.color);
      }
    }
    if (camera->second.front) {
      std::lock_guard<std::mutex> lock(g_front_mutex);
      if (g_front_objects.size() < 64) g_front_objects.insert(object);
    }
    if (camera->second.front && !skinned) {  // side90: does a rigid gun part hang on a bone? (the gun sits off)
      static int rigid_shown = 0;
      const uint32_t bone_count = g::ReadU32(base, object + kObjectBoneCount);
      if (rigid_shown < 8) {
        ++rigid_shown;
        REXLOG_INFO("cod3: ngl side: front rigid surface {:08X} of object {:08X}: {} bones at {:08X}, model {:08X}; "
                    "surface +60: {} / {}; object +30: {}", surface, object, bone_count,
                    g::ReadU32(base, object + kObjectBones), g::ReadU32(base, object + kObjectModel),
                    Words(base, surface + 0x60, 8), Words(base, surface + 0x80, 8), Words(base, object + 0x30, 8));
      }
    }
    if (camera->second.front) {  // the gun's materials, once each: the texture taken as the color map, and the list
      static std::unordered_set<uint32_t> front_materials;
      if (front_materials.size() < 10 && front_materials.insert(material).second) {
        REXLOG_INFO("cod3: ngl side: front scene {:08X} material {:08X} ({}): color map {:08X}", s.scene, material,
                    skinned ? "skinned" : "rigid", samplers.color);
        ShowSamplers(base, material, geo.stride, skinned, true);
      }
    }
    d.texture = TextureFor(base, samplers.color ? samplers.color : MaterialTexture(base, material));
    d.lightmap = TextureFor(base, samplers.lightmap);
    d.shading = d.lightmap ? NGL_VK_SHADE_LIGHTMAP : NGL_VK_SHADE_LIGHT;
    if (samplers.env && !d.lightmap) {  // side88: shiny (the guns): the environment map times the specular map, added
      d.env_texture = TextureFor(base, samplers.env);
      d.env_mask = samplers.env_mask ? TextureFor(base, samplers.env_mask) : 0;
      left_out[d.env_mask ? "(of those, shiny: environment map x specular map)" : "(of those, shiny: environment map)"]++;
    }
    {
      Sun sun;
      if (!s.old_class && StateSun(base, g::ReadU32(base, s.node + kNodeShaderState), sun)) {
        static bool said = false;
        if (!said) {
          said = true;
          REXLOG_INFO("cod3: ngl side: the sun travels {:.3f} {:.3f} {:.3f}, color {:.3f} {:.3f} {:.3f}, ambient "
                      "{:.3f} {:.3f} {:.3f}", sun.direction[0], sun.direction[1], sun.direction[2], sun.color[0],
                      sun.color[1], sun.color[2], sun.ambient[0], sun.ambient[1], sun.ambient[2]);
        }
        g_sun = sun;
      }
      if (d.shading == NGL_VK_SHADE_LIGHT) {
        Sun own;
        const bool has_own = ObjectSun(base, object, own);
        if (has_own) {  // side89: its own light set
          LightWith(d, own);
          left_out["(of those, lit by their own light set)"]++;
        } else {
          LightWith(d, sun.ok ? sun : g_sun);
          if (d.lit) left_out["(of those, lit by the game's sun)"]++;
        }
        // side106: what light a soldier gets. The game's light grid (0x824224F0, called by 0x82427920 for each
        // soldier) weights the sun by how much of it reaches where the soldier stands, so a soldier's set should be
        // darker under the trees; this shows whether the native side sees that.
        static int skinned_shown = 0, front_shown = 0;  // side109: the front scene's (the gun, the arms) too
        if ((skinned && skinned_shown < 48) || (camera->second.front && front_shown < 16)) {
          ++(camera->second.front ? front_shown : skinned_shown);
          const Sun& used = has_own ? own : (sun.ok ? sun : g_sun);
          REXLOG_INFO("cod3: ngl side: {} surface {:08X} of object {:08X}: {} light, ambient {:.3f} {:.3f} {:.3f}, "
                      "light color {:.3f} {:.3f} {:.3f} traveling {:.3f} {:.3f} {:.3f}, {} more",
                      camera->second.front ? (skinned ? "front skinned" : "front") : "skinned", surface, object,
                      has_own ? "its own" : "the game's", used.ambient[0], used.ambient[1],
                      used.ambient[2], used.color[0], used.color[1], used.color[2], used.direction[0],
                      used.direction[1], used.direction[2], used.more);
        }
      }
    }
    FogWith(base, s.scene, d);
    if (d.fog[1] > d.fog[0]) left_out["(of those, fogged)"]++;
    // a soldier's DXT5 color map keeps something else in its alpha (side24 drew them whole without the test)
    d.alpha = !skinned && g_cut_out.count(d.texture) ? NGL_VK_ALPHA_TEST : NGL_VK_ALPHA_OPAQUE;
    left_out[d.texture ? (d.lightmap ? "(drawn with color map and lightmap)" : "(drawn with a color map)")
                       : "(drawn untextured)"]++;
    if (d.alpha == NGL_VK_ALPHA_TEST) left_out["(of those, alpha-tested)"]++;
    if (skinned) left_out["(of those, skinned)"]++;
    static int untextured_shown = 0;
    if (!d.texture && untextured_shown < 6) {
      ++untextured_shown;
      const uint32_t list = g::ReadU32(base, material + kMaterialSamplers);
      REXLOG_INFO("cod3: ngl side: untextured surface {:08X}: material {:08X} ({}), samplers {:08X}: {} / {}",
                  surface, material, Words(base, material, 8), list,
                  Readable(list) ? Words(base, list, 8) : std::string("-"),
                  Readable(list) ? Words(base, list + 0x20, 8) : std::string("-"));
      // side49: a soldier's uniform (material AD584300) names a Diffuse_Texture sampler whose texture (AB4920E8) isn't
      // an NGL texture, so it was drawn in its tint: what it points at, and one pointer deeper.
      for (uint32_t k = 0; Readable(list) && k < kMaxSamplers; ++k) {
        const uint32_t entry = list + kSamplerFirst + kSamplerBytes * k;
        const uint32_t parameter = g::ReadU32(base, entry), texture = g::ReadU32(base, entry + 4);
        if (!Pointer(parameter)) break;
        if (IsTexture(base, texture) || !Readable(texture)) continue;
        REXLOG_INFO("cod3: ngl side: untextured surface {:08X} sampler {} <{}>: {:08X}: {} / {}", surface, k,
                    SamplerTextureName(base, parameter), texture, Words(base, texture, 8), Words(base, texture + 0x20, 8));
        for (uint32_t at = 0; at < 0x40; at += 4) {
          const uint32_t inner = g::ReadU32(base, texture + at);
          if (!Pointer(inner) || !Readable(inner) || inner == texture) continue;
          REXLOG_INFO("cod3: ngl side: untextured surface {:08X} sampler {} +{:02X} -> {:08X}{}: {}", surface, k, at,
                      inner, IsTexture(base, inner) ? " (an NGL texture)" : "", Words(base, inner, 8));
        }
      }
    }
    bool sane = true;
    for (uint32_t k = 0; k < 16; ++k) {
      d.world[k] = g::ReadF32(base, object + 4 * k);
      sane = sane && std::isfinite(d.world[k]);
    }
    if (!sane) {
      left_out["its world matrix isn't sane"]++;
      continue;
    }
    d.world[3] = d.world[7] = d.world[11] = 0.0f;
    d.world[15] = 1.0f;
    std::memcpy(d.view_projection, camera->second.view_projection, sizeof d.view_projection);
    if (draws.size() < 2) Probe(base, frame, surface, d);
    {  // side90: the object's second matrix (+0x40) beside world x the scene's view-projection, to see whether the
       // gun (drawn wider than the game's) has its own projection there
      static int front_matrices = 0, world_matrices = 0;
      int& shown = camera->second.front ? front_matrices : world_matrices;
      if (shown < 2) {
        ++shown;
        std::string own, product;
        char item[24];
        for (int r = 0; r < 4; ++r) {
          for (int c = 0; c < 4; ++c) {
            float sum = 0.0f;
            for (int k = 0; k < 4; ++k) sum += d.world[4 * r + k] * d.view_projection[4 * k + c];
            std::snprintf(item, sizeof item, " %.4g", g::ReadF32(base, object + 0x40 + 4 * (4 * r + c)));
            own += item;
            std::snprintf(item, sizeof item, " %.4g", sum);
            product += item;
          }
          own += r < 3 ? " |" : "";
          product += r < 3 ? " |" : "";
        }
        REXLOG_INFO("cod3: ngl side: {} object {:08X}: +40:{}; world x view-projection:{}",
                    camera->second.front ? "front" : "world", object, own, product);
      }
    }
    if (kSkin) d.corners = SkinnedCorners(base, surface, object, frame);
    if (skinned) ProbeFrontSkinned(base, surface, object, d, camera->second.front);  // side93/95: the arms
    if (skinned && camera->second.front && REXCVAR_GET(cod3_debug_ngl_side_arms) > 0) {  // side96-97: the arms
      const int how = REXCVAR_GET(cod3_debug_ngl_side_arms);
      if (how != 2) d.texture = d.lightmap = d.env_texture = d.env_mask = 0;
      if (how != 3) d.shading = NGL_VK_SHADE_UNLIT;
      static std::atomic<int> said{0};
      if (said.fetch_add(1, std::memory_order_relaxed) < 2) {
        REXLOG_INFO("cod3: ngl side: arms surface {:08X} drawn for the check ({}): texture {}, shading {}, alpha {}, "
                    "lit {}", surface, how, d.texture, d.shading, d.alpha, d.lit);
      }
    }
    draws.push_back(d);
    char in_scene[96];  // which scenes the draws come from, in what order, to which target
    const auto order = census.scene_order.find(s.scene);
    std::snprintf(in_scene, sizeof in_scene, "(drawn in scene %08X: order %d target %08X near %.2f%s%s)", s.scene,
                  order != census.scene_order.end() ? order->second : -1,
                  g::ReadU32(base, s.scene + kSceneColorTarget), NearPlane(base, s.scene),
                  s.scene == world_scene ? " world" : "", skinned ? " skinned" : "");
    left_out[in_scene]++;
    if (draws.size() >= kMaxDraws) break;
  }
  TreeDraws(base, census, draws, left_out);
  if (census.far_solids) left_out["(of the surfaces, from the far lists)"] = census.far_solids;
  if (census.far_trees) left_out["(far trees, left out)"] = census.far_trees;
  std::string reasons;
  for (const auto& [why, n] : left_out) reasons += (reasons.empty() ? "" : ", ") + why + " x" + std::to_string(n);
  REXLOG_INFO("cod3: ngl side: frame {}: 3D: {} generic surfaces, {} drawn ({} meshes made so far); left out: {}",
              frame, census.solids.size(), draws.size(), g_meshes.size(), reasons.empty() ? "none" : reasons);
  return draws;
}
#endif

// Direct3D's device at a draw (docs/COD3_D3D.md, "The device's state shadow"; owner 2026-11-05 12:56: use the
// decompiled Direct3D). The device keeps a copy of the GPU's registers that each draw sends, so at a draw it holds
// everything the GPU will use, constants the game wrote into it directly included: +0x0700 the vertex shader's 256 float
// constants and +0x1700 the pixel shader's, 16 bytes each; +0x284C the registers 0x2100-0x2114, RB_FOG_COLOR (0x2109)
// at +0x2870. No surface's parameter list carries the haze (side35's hunt for "fog" names found none), so at dumps 3
// and 6 the next draws are read on the render job's thread: for each new pair of call site and set of non-zero pixel
// constants, up to kDrawsLogged, the non-zero pixel constants, and for the first kVertexDrawsLogged the vertex ones.
#if defined(COD3_MP)
constexpr uint32_t kDevicePointer = 0x82A8C0FC;  // d3d_mp_twins.tsv
#else
constexpr uint32_t kDevicePointer = 0x8290F24C;
#endif
constexpr uint32_t kDeviceVertexConstants = 0x0700;
constexpr uint32_t kDevicePixelConstants = 0x1700;
constexpr uint32_t kDeviceFogColor = 0x2870;
constexpr int kDeviceConstants = 256;
constexpr int kConstantsListed = 64;
constexpr int kDrawsRead = 4000;  // about four frames of forest's draws
constexpr int kDrawsLogged = 32;
constexpr int kVertexDrawsLogged = 4;

std::atomic<int> g_draws_to_read{0};
std::mutex g_draw_mutex;
std::unordered_set<uint64_t> g_draw_keys;
int g_draws_logged = 0;

// The device lives in the guest's heap or physical memory; it is in use, so its pages are there.
bool DeviceReadable(uint32_t device) {
  return Pointer(device) && (Readable(device) || (device >= 0x40000000u && device < 0x80000000u));
}

// ---------------------------------------------------------------------------------------------------------------
// The colors as the game's picture is shown (side92; owner 2026-11-05 22:10: "consider the colorspaces and the patch
// we had to apply"). The GPU emulation presents the front buffer through the display's 256-entry gamma table, and the
// game's own screenshot is taken after it. Direct3D's SetGammaRamp (0x822E94D0; cod3mp 0x822B8218) puts the curve of
// the display the system reports (VdGetCurrentDisplayGamma, so kernel_display_gamma_type, which cod3_color_space sets
// through SDK patch 0040; game/color_output.cpp) on top of the game's own ramp (its brightness). It keeps the result in
// the device at +0x3510 (256 big-endian 16-bit values each of red, green and blue), and 0x822FCF50 writes their top 10
// bits to the table. With cod3_color_space bt709, patch 0040's display_output_transfer (bt1886) is folded into the
// presented table too. ngl_vk draws the game's values, so its pictures go through the same table here, the way the
// presenter applies it, before they are saved (the untouched picture is saved as well, "-drawn"). HDR output (patch
// 0044) comes after the table and isn't in the game's screenshot. The display format (+0x33A0) is logged beside the
// table: a 10-bit front buffer would be shown through the PWL table instead, which this doesn't read.
#if defined(COD3_NGL_VK)
constexpr uint32_t kDeviceGammaRamp = 0x3510;
constexpr uint32_t kDeviceDisplayFormat = 0x33A0;

// SDK patch 0040's BT.1886 (gamma 2.4, black at 0) decoded, then encoded as sRGB.
double Bt1886ToSrgb(double v) {
  v = std::clamp(v, 0.0, 1.0);
  const double linear = std::pow(v, 2.4);
  return linear <= 0.0031308 ? 12.92 * linear : 1.055 * v - 0.055;
}

// The presented table, as 8-bit values for each of the game's 8-bit values, per channel.
struct ShownColors {
  std::array<std::array<uint8_t, 256>, 3> table{};
  bool from_device = false;  // false: no device yet, so a plain ramp
  bool bt1886 = false;
  bool identity = true;
  uint32_t display_format = 0;
};

ShownColors ShownColorsNow(uint8_t* base) {
  ShownColors shown;
  const uint32_t device = g::ReadU32(base, kDevicePointer);
  shown.from_device = DeviceReadable(device);
  if (shown.from_device) shown.display_format = g::ReadU32(base, device + kDeviceDisplayFormat);
  shown.bt1886 = rex::cvar::GetFlagInfo("display_output_transfer") &&
                 rex::cvar::GetFlagByName("display_output_transfer") == "bt1886";
  for (int channel = 0; channel < 3; ++channel) {
    for (int i = 0; i < 256; ++i) {
      uint32_t value_10b = static_cast<uint32_t>(std::lround(i * 1023.0 / 255.0));
      if (shown.from_device)
        value_10b = g::ReadU16(base, device + kDeviceGammaRamp + static_cast<uint32_t>(channel * 256 + i) * 2) >> 6;
      if (shown.bt1886) value_10b = static_cast<uint32_t>(std::lround(Bt1886ToSrgb(value_10b / 1023.0) * 1023.0));
      const auto value = static_cast<uint8_t>(std::lround(value_10b * 255.0 / 1023.0));
      shown.table[channel][i] = value;
      shown.identity = shown.identity && value == i;
    }
  }
  return shown;
}

// A copy of an rgba picture through the presented table; the table is logged whenever it changes.
std::vector<uint8_t> AsShown(uint8_t* base, const uint8_t* rgba, uint32_t width, uint32_t height) {
  const ShownColors shown = ShownColorsNow(base);
  const auto& t = shown.table;
  char text[320];
  std::snprintf(text, sizeof(text),
                "%s%s, display format %08X: red 0/32/64/128/192/255 -> %u/%u/%u/%u/%u/%u, green 128 -> %u, "
                "blue 128 -> %u%s",
                shown.from_device ? "the game's" : "no device yet, a plain ramp",
                shown.bt1886 ? ", with BT.1886 shown on sRGB" : "", shown.display_format, t[0][0], t[0][32], t[0][64],
                t[0][128], t[0][192], t[0][255], t[1][128], t[2][128], shown.identity ? " (none: as drawn)" : "");
  static std::mutex logged_mutex;
  static std::string logged;
  {
    std::lock_guard lock(logged_mutex);
    if (logged != text) {
      logged = text;
      REXLOG_INFO("cod3: ngl side: presented gamma table: {}", text);
    }
  }
  std::vector<uint8_t> out(rgba, rgba + static_cast<size_t>(width) * height * 4);
  if (!shown.identity) {
    for (size_t i = 0; i < out.size(); i += 4) {
      for (int c = 0; c < 3; ++c) out[i + c] = t[c][out[i + c]];
    }
  }
  return out;
}

// Saves both: <stem>.ppm as shown (through the presented table, what side_compare.py sets beside the game's own
// screenshot) and <stem>-drawn.ppm as ngl_vk drew it.
void SaveShownPpm(uint8_t* base, const std::filesystem::path& folder, const std::string& stem, const uint8_t* rgba,
                  uint32_t width, uint32_t height) {
  const std::vector<uint8_t> shown = AsShown(base, rgba, width, height);
  SavePpm(folder / (stem + ".ppm"), shown.data(), width, height);
  SavePpm(folder / (stem + "-drawn.ppm"), rgba, width, height);
}
#endif  // COD3_NGL_VK

// The non-zero float constants of a block of kDeviceConstants, as "cN=(x y z w)"; key takes which ones they are.
std::string ConstantsText(uint8_t* base, uint32_t at, uint64_t* key) {
  std::string text;
  char item[96];
  int listed = 0;
  for (int c = 0; c < kDeviceConstants; ++c) {
    const uint32_t a = at + 16u * static_cast<uint32_t>(c);
    if (!(g::ReadU32(base, a) | g::ReadU32(base, a + 4) | g::ReadU32(base, a + 8) | g::ReadU32(base, a + 12))) continue;
    if (key) *key = (*key ^ static_cast<uint64_t>(c + 1)) * 1099511628211ull;
    if (++listed > kConstantsListed) continue;
    std::snprintf(item, sizeof item, " c%d=(%.4g %.4g %.4g %.4g)", c, g::ReadF32(base, a), g::ReadF32(base, a + 4),
                  g::ReadF32(base, a + 8), g::ReadF32(base, a + 12));
    text += item;
  }
  if (listed > kConstantsListed) text += " ... (" + std::to_string(listed) + " in all)";
  return text.empty() ? std::string(" none") : text;
}

void ReadNextDraws() {
  std::lock_guard<std::mutex> lock(g_draw_mutex);
  g_draw_keys.clear();
  g_draws_logged = 0;
  g_draws_to_read.store(kDrawsRead, std::memory_order_relaxed);
}

uint64_t g_frames = 0;
int g_dumps = 0;
bool g_listed = false;

#if defined(COD3_NGL_VK)
std::vector<ngl_vk_mesh_draw> SkyDraws(uint8_t* base, uint64_t frame);
std::vector<ngl_vk_mesh_draw> EffectDraws(uint8_t* base, uint64_t frame, const ngl_vk_mesh_draw* fog_from);

// side108: the sun's shadow. The game draws three orthographic maps around the camera every frame (0x8242BAA0: 384,
// 1024 and 2048 units wide, far plane 5000; dev/agents/shadows/NOTES.md), with the soldiers, props and trees in them
// (trees in a scene without a perspective camera, TreeDraws' count); a model's first light, the sun, comes through
// them, so under the trees the soldiers keep little more than their ambient (side106: the light sets' sun is the
// same 0.627 0.568 0.446 everywhere; side106off, `cod3_shadow_quality=off`, lights them). Natively one map,
// kShadowWidth units wide, centered kShadowAhead ahead of the camera along the ground, kShadowDepth each side along
// the sun's way; the casters and the shadowed are the world scene's draws lit by the sun (soldiers, props, trees),
// not the lightmapped world (its sun is in its lightmaps) and nothing blended.
constexpr float kShadowWidth = 3072.0f;
constexpr float kShadowAhead = 1200.0f;
constexpr float kShadowDepth = 4000.0f;
constexpr float kShadowTexels = 2048.0f;  // ngl_vk's map, a side

struct SunShadow {
  bool ok = false;
  float light[16];   // world to the light's clip space (Direct3D row vectors)
  float camera[16];  // the view-projection of the shadowed draws
};

bool Inverse4(const float m[16], double out[16]) {
  double a[16], cofactor[16], determinant = 0.0;
  for (int i = 0; i < 16; ++i) a[i] = m[i];
  for (int row = 0; row < 4; ++row) {
    for (int column = 0; column < 4; ++column) {
      double minor[9];
      int n = 0;
      for (int r = 0; r < 4; ++r) {
        for (int c = 0; c < 4; ++c) {
          if (r != row && c != column) minor[n++] = a[r * 4 + c];
        }
      }
      cofactor[row * 4 + column] = ((row + column) & 1 ? -1.0 : 1.0) *
                                   (minor[0] * (minor[4] * minor[8] - minor[5] * minor[7]) -
                                    minor[1] * (minor[3] * minor[8] - minor[5] * minor[6]) +
                                    minor[2] * (minor[3] * minor[7] - minor[4] * minor[6]));
    }
  }
  for (int column = 0; column < 4; ++column) determinant += a[column] * cofactor[column];
  if (!(std::fabs(determinant) > 1e-30)) return false;
  for (int row = 0; row < 4; ++row) {
    for (int column = 0; column < 4; ++column) out[row * 4 + column] = cofactor[column * 4 + row] / determinant;
  }
  return true;
}

SunShadow ShadowFor(std::vector<ngl_vk_mesh_draw>& draws, uint64_t frame) {
  SunShadow s;
  if (!REXCVAR_GET(cod3_debug_ngl_side_shadow)) return s;
  const float* way = g_sun_way_known ? g_sun_way : g_sun.ok ? g_sun.direction : nullptr;
  if (!way) return s;
  // the world's camera: the view-projection most of the sun-lit draws share
  std::map<std::array<float, 16>, int> counts;
  for (const ngl_vk_mesh_draw& d : draws) {
    if (d.shading != NGL_VK_SHADE_LIGHT || d.alpha == NGL_VK_ALPHA_BLEND) continue;
    std::array<float, 16> vp;
    std::memcpy(vp.data(), d.view_projection, sizeof d.view_projection);
    counts[vp]++;
  }
  if (counts.empty()) return s;
  const auto best = std::max_element(counts.begin(), counts.end(),
                                     [](const auto& a, const auto& b) { return a.second < b.second; });
  std::memcpy(s.camera, best->first.data(), sizeof s.camera);
  double inverse[16];
  if (!Inverse4(s.camera, inverse)) return s;
  // the eye is where clip x, y and w are 0: clip (0, 0, 1, 0) back into the world, the inverse's third row; the view's
  // middle far away is clip (0, 0, 1, 1), the third and fourth rows
  const double* e = inverse + 8;
  const double far_w = inverse[11] + inverse[15];
  if (std::fabs(e[3]) < 1e-12 || std::fabs(far_w) < 1e-12) return s;
  double eye[3], ahead[3];
  for (int i = 0; i < 3; ++i) {
    eye[i] = e[i] / e[3];
    ahead[i] = (inverse[8 + i] + inverse[12 + i]) / far_w - eye[i];
  }
  ahead[2] = 0.0;  // along the ground (z is up)
  const double ahead_length = std::sqrt(ahead[0] * ahead[0] + ahead[1] * ahead[1]);
  double center[3] = {eye[0], eye[1], eye[2]};
  if (ahead_length > 1e-6) {
    for (int i = 0; i < 2; ++i) center[i] += ahead[i] / ahead_length * kShadowAhead;
  }
  // the light's frame: forward the way the sun travels, right and up across it
  double f[3] = {way[0], way[1], way[2]};
  const double f_length = std::sqrt(f[0] * f[0] + f[1] * f[1] + f[2] * f[2]);
  if (!(f_length > 1e-6)) return s;
  for (double& v : f) v /= f_length;
  const double up0[3] = {std::fabs(f[2]) < 0.99 ? 0.0 : 1.0, 0.0, std::fabs(f[2]) < 0.99 ? 1.0 : 0.0};
  double r[3] = {up0[1] * f[2] - up0[2] * f[1], up0[2] * f[0] - up0[0] * f[2], up0[0] * f[1] - up0[1] * f[0]};
  const double r_length = std::sqrt(r[0] * r[0] + r[1] * r[1] + r[2] * r[2]);
  for (double& v : r) v /= r_length;
  const double u[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};
  // the center snapped to whole texels across the map, so the shadows don't crawl as the camera moves
  const double texel = kShadowWidth / kShadowTexels, half = kShadowWidth / 2.0;
  const double cr = std::round((center[0] * r[0] + center[1] * r[1] + center[2] * r[2]) / texel) * texel;
  const double cu = std::round((center[0] * u[0] + center[1] * u[1] + center[2] * u[2]) / texel) * texel;
  const double cf = center[0] * f[0] + center[1] * f[1] + center[2] * f[2];
  for (int i = 0; i < 3; ++i) {
    s.light[i * 4 + 0] = static_cast<float>(r[i] / half);
    s.light[i * 4 + 1] = static_cast<float>(u[i] / half);
    s.light[i * 4 + 2] = static_cast<float>(f[i] / (2.0 * kShadowDepth));
    s.light[i * 4 + 3] = 0.0f;
  }
  s.light[12] = static_cast<float>(-cr / half);
  s.light[13] = static_cast<float>(-cu / half);
  s.light[14] = static_cast<float>((kShadowDepth - cf) / (2.0 * kShadowDepth));
  s.light[15] = 1.0f;
  int marked = 0;
  for (ngl_vk_mesh_draw& d : draws) {
    if (d.shading != NGL_VK_SHADE_LIGHT || d.alpha == NGL_VK_ALPHA_BLEND ||
        std::memcmp(d.view_projection, s.camera, sizeof s.camera) != 0) {
      continue;
    }
    d.shadow_caster = 1;
    d.sun_shadowed = 1;
    ++marked;
  }
  s.ok = marked > 0;
  REXLOG_INFO("cod3: ngl side: frame {}: the sun's shadow: {} draws cast and take it; eye ({:.0f} {:.0f} {:.0f}), map "
              "center ({:.0f} {:.0f} {:.0f}), the sun traveling {:.3f} {:.3f} {:.3f}",
              frame, marked, eye[0], eye[1], eye[2], center[0], center[1], center[2], f[0], f[1], f[2]);
  return s;
}
#endif

void Dump(uint8_t* base, uint64_t frame) {
  std::vector<Quad> quads;
  Census census;
  TakeScene(base, g::ReadU32(base, kRootScene), 0, quads, census);
  const std::string name = "ngl-side-" + std::to_string(frame);
  REXLOG_INFO("cod3: ngl side: frame {}: {} scenes ({} drawn off screen), {} quads, {} strings ({} glyphs), {} other "
              "nodes ({})",
              frame, census.scenes, census.offscreen_scenes, census.quads, census.strings, census.glyphs,
              census.other_nodes, OthersText(census));
  if (g_dumps == 1 || (!g_listed && census.quads >= 12)) {  // the first dump, and the first with a dozen quads
    g_listed = g_listed || census.quads >= 12;
    List(base, frame, quads, census);
  }
#if defined(COD3_NGL_VK)
  if (g_state == 0) {
    g_state = ngl_vk_init(kTargetWidth, kTargetHeight) == 0 ? 1 : -1;
    REXLOG_INFO("cod3: ngl side: ngl_vk {} ({})", g_state == 1 ? "started" : "failed to start", ngl_vk_device_name());
  }
  if (g_state != 1) return;
  std::vector<ngl_vk_mesh_draw> draws = SkyDraws(base, frame);  // the sky first, under everything (side85)
  const std::vector<ngl_vk_mesh_draw> solids = SolidDraws(base, frame, census);
  draws.insert(draws.end(), solids.begin(), solids.end());
  {  // side104: the effects last, fogged as the first fogged world draw
    const ngl_vk_mesh_draw* fog_from = nullptr;
    for (const ngl_vk_mesh_draw& d : solids) {
      if (d.fog[1] > d.fog[0]) {
        fog_from = &d;
        break;
      }
    }
    const std::vector<ngl_vk_mesh_draw> effects = EffectDraws(base, frame, fog_from);
    draws.insert(draws.end(), effects.begin(), effects.end());
  }
  const SunShadow shadow = ShadowFor(draws, frame);  // side108
  const GrassFrame grass = GrassFor(base, census);
  // ngl_vk draws the 3D first and the quads over it, so quads of the scenes drawn before the first scene with 3D
  // (the frame's clearing quad, say) are left out when there is 3D: in the game they are under it.
  int first_3d = 1 << 30;
  for (const Solid& s : census.solids) {
    const auto order = census.scene_order.find(s.scene);
    if (order != census.scene_order.end()) first_3d = std::min(first_3d, order->second);
  }
  std::vector<ngl_vk_vertex> vertices;
  vertices.reserve(quads.size() * 6);
  int textured = 0, under = 0;
  for (const Quad& q : quads) {
    const auto order = census.scene_order.find(q.scene);
    if (!draws.empty() && order != census.scene_order.end() && order->second < first_3d) {
      ++under;
      continue;
    }
    const uint32_t texture = TextureFor(base, q.texture);
    textured += texture != 0;
    AddQuad(vertices, q, texture);
  }
  if (under) REXLOG_INFO("cod3: ngl side: frame {}: {} quads under the 3D left out", frame, under);
  if (!draws.empty()) {  // the 3D alone first, then under the 2D
    static bool listed = false;
    if (!listed) {
      listed = true;
      List(base, frame, quads, census);
    }
    const ngl_vk_vertex none{};
    ngl_vk_set_meshes(draws.data(), static_cast<uint32_t>(draws.size()));
    ngl_vk_set_sun_shadow(shadow.ok ? shadow.light : nullptr, shadow.ok ? shadow.camera : nullptr);
    SetGrass(grass);
    uint32_t width = 0, height = 0;
    const uint8_t* pixels = nullptr;
    if (ngl_vk_draw_quads(&none, 0, kScreenWidth, kScreenHeight, nullptr, 0, 0, 0) == 0 &&
        (pixels = ngl_vk_target_pixels(&width, &height)) != nullptr) {
      SaveShownPpm(base, ScreenshotFolder(), name + "-3d", pixels, width, height);  // side92: as shown
    }
  }
  ngl_vk_set_meshes(draws.data(), static_cast<uint32_t>(draws.size()));
  ngl_vk_set_sun_shadow(shadow.ok ? shadow.light : nullptr, shadow.ok ? shadow.camera : nullptr);
  SetGrass(grass);
  if (ngl_vk_draw_quads(vertices.data(), static_cast<uint32_t>(vertices.size()), kScreenWidth, kScreenHeight, nullptr,
                        0, 0, 0)) {
    REXLOG_WARN("cod3: ngl side: drawing frame {} failed; no more", frame);
    g_state = -1;
    return;
  }
  uint32_t width = 0, height = 0;
  const uint8_t* pixels = ngl_vk_target_pixels(&width, &height);
  if (!pixels) return;
  const std::filesystem::path path = ScreenshotFolder() / (name + "-vk.ppm");
  SaveShownPpm(base, ScreenshotFolder(), name + "-vk", pixels, width, height);  // side92: as shown
  const std::string shot = cod3::shot::Request(cod3::shot::Kind::kReport, "ngl side", name);
  REXLOG_INFO("cod3: ngl side: frame {}: {} quads and glyphs drawn ({} textured) to {}; the game's own: {}", frame,
              quads.size(), textured, path.string(), shot.empty() ? std::string("not taken") : shot);
#else
  (void)name;
  if (g_dumps == 1) REXLOG_INFO("cod3: ngl side: this build has no ngl_vk (Vulkan or glslc missing): counts only");
#endif
}

// The grass (from 2026-11-05 15:40). GrassRenderNode__vfunc_2 (0x823F79B0) runs 0x823F6DF0, which for each patch sets
// its samplers and origin and calls 0x823F1C70: one Direct3D draw of a triangle strip whose count comes from the
// patch and its distance, returning to kGrassDrawReturn. The blades are made by the vertex shader, so for ngl_vk to
// draw them the draws from the kGrassDrawProbed-th on (kGrassDrawsLogged of them) log the device's constants, and the
// first of them also its fetch constants and saves, in the screenshot folder, both shaders and every vertex buffer it
// fetches from (ngl-side-grass-*.bin, as in guest memory). A run with dump_shaders set has the SDK's disassembly of
// the same microcode to match them against.
//
// A shader object (CreateVertexShader 0x822E68D8; the load at 0x822FB4B0, which sends them with IM_LOAD): a header
// (the vertex shader's at +0x368, the pixel shader's at +0x28) with +0x00 flags, +0x04 its own size, +0x08 the
// microcode block's size and +0x18 (+0x20 for the vertex shader's second variant, flag 0x20) the offset, from the
// header, of {microcode offset in the block, microcode size in bytes}; the block's address is at +0x1C (vertex) or
// +0x18 (pixel).
#if defined(COD3_MP)
constexpr uint32_t kGrassDrawReturn = 0x8240B794;  // the same call in 0x8240B6E8 (cod3mp's 0x823F1C70), +0xAC
#else
constexpr uint32_t kGrassDrawReturn = 0x823F1D1C;
#endif
constexpr int kGrassDrawProbed = 300;
constexpr int kGrassDrawsLogged = 4;
constexpr uint32_t kDeviceFetchConstants = 0x0400;  // 32 of 24 bytes (six words: one texture or three vertex)
constexpr int kFetchConstants = 32;
constexpr uint32_t kFetchConstantBytes = 24;
constexpr uint32_t kDevicePixelShader = 0x3080;
constexpr uint32_t kDeviceVertexShader = 0x3084;
constexpr uint32_t kDeviceIndexBuffer = 0x2F84;
constexpr uint32_t kShaderFlags = 0x00, kShaderHeaderBytes = 0x04, kShaderBlockBytes = 0x08;
constexpr uint32_t kShaderMicrocode = 0x18;  // + 8 × variant
constexpr uint32_t kShaderSecondVariant = 0x20;
constexpr uint32_t kMaxShaderBytes = 0x10000;
constexpr uint32_t kMaxVertexBufferBytes = 0x100000;

std::atomic<int> g_grass_draws{0};

// Saves bytes of guest memory at address, as they are, to name in the screenshot folder.
void SaveGuestBytes(uint8_t* base, uint32_t address, uint32_t bytes, const std::string& name) {
#if defined(COD3_NGL_VK)
  const std::filesystem::path path = ScreenshotFolder() / name;
  std::ofstream out(path, std::ios::binary);
  out.write(reinterpret_cast<const char*>(base + address), bytes);
  REXLOG_INFO("cod3: ngl side: grass: saved {} bytes at {:08X} to {}", bytes, address, path.string());
#else
  (void)base;
  REXLOG_INFO("cod3: ngl side: grass: {} not saved ({} bytes at {:08X}): this build has no ngl_vk", name, bytes,
              address);
#endif
}

// prefix names the saved files (side93: the grass's, and the mesh sections' for the haze): <prefix>-vs/-ps-*.bin.
void SaveShader(uint8_t* base, uint32_t shader, uint32_t header_at, uint32_t block_at, bool vertex,
                const std::string& prefix) {
  const char* kind = vertex ? "vertex" : "pixel";
  if (!DeviceReadable(shader)) {
    REXLOG_INFO("cod3: ngl side: {}: no {} shader ({:08X})", prefix, kind, shader);
    return;
  }
  const uint32_t header = shader + header_at;
  const uint32_t flags = g::ReadU32(base, header + kShaderFlags);
  const uint32_t header_bytes = g::ReadU32(base, header + kShaderHeaderBytes);
  const uint32_t block = g::ReadU32(base, shader + block_at);
  const uint32_t block_bytes = g::ReadU32(base, header + kShaderBlockBytes);
  std::string variants;
  char item[64];
  const int count = vertex && (flags & kShaderSecondVariant) ? 2 : 1;
  for (int v = 0; v < count; ++v) {
    const uint32_t entry = header + g::ReadU32(base, header + kShaderMicrocode + 8 * static_cast<uint32_t>(v));
    std::snprintf(item, sizeof item, "%smicrocode +%X (%u bytes)", v ? ", " : "", g::ReadU32(base, entry),
                  g::ReadU32(base, entry + 4));
    variants += item;
  }
  REXLOG_INFO("cod3: ngl side: {}: {} shader {:08X}: flags {:08X}, header {} bytes, block {:08X} ({} bytes), {}; "
              "object: {}",
              prefix, kind, shader, flags, header_bytes, block, block_bytes, variants, Words(base, shader, 0x10));
  const std::string name = prefix + (vertex ? "-vs" : "-ps");
  if (header_bytes > 0 && header_bytes <= kMaxShaderBytes) {
    SaveGuestBytes(base, header, header_bytes, name + "-header.bin");
  }
  if (Physical(block) && block_bytes > 0 && block_bytes <= kMaxShaderBytes) {
    SaveGuestBytes(base, block, block_bytes, name + "-block.bin");
  }
}

// The fetch constants in use, and the vertex buffers they point at (type 3: the address in dwords in bits 2-31 of the
// first word, the size in dwords in bits 2-25 of the second).
void SaveFetches(uint8_t* base, uint32_t device) {
  int saved = 0;
  for (int f = 0; f < kFetchConstants; ++f) {
    const uint32_t at = device + kDeviceFetchConstants + kFetchConstantBytes * static_cast<uint32_t>(f);
    uint32_t any = 0;
    for (uint32_t w = 0; w < kFetchConstantBytes; w += 4) any |= g::ReadU32(base, at + w);
    if (!any) continue;
    REXLOG_INFO("cod3: ngl side: grass: fetch constant {}: {}", f, Words(base, at, kFetchConstantBytes / 4));
    if ((g::ReadU32(base, at) & 3) == 2) continue;  // a texture's six words
    for (uint32_t v = 0; v < 3; ++v) {
      const uint32_t word0 = g::ReadU32(base, at + 8 * v), word1 = g::ReadU32(base, at + 8 * v + 4);
      const uint32_t bytes = ((word1 >> 2) & 0xFFFFFF) * 4;
      if ((word0 & 3) != 3 || bytes == 0 || bytes > kMaxVertexBufferBytes || saved >= 4) continue;
      const uint32_t address = GuestAddress(word0 & 0x1FFFFFFC);
      SaveGuestBytes(base, address, bytes, "ngl-side-grass-vb" + std::to_string(3 * f + static_cast<int>(v)) + ".bin");
      ++saved;
    }
  }
}

// The index buffer (Direct3D's indexed draw, 0x822F3A28, reads it from the device): flags (bit 31: 32-bit indices),
// its data's address at +0x18 and size in bytes at +0x1C.
void SaveIndices(uint8_t* base, uint32_t device) {
  const uint32_t buffer = g::ReadU32(base, device + kDeviceIndexBuffer);
  if (!DeviceReadable(buffer)) {
    REXLOG_INFO("cod3: ngl side: grass: no index buffer ({:08X})", buffer);
    return;
  }
  const uint32_t address = GuestAddress(g::ReadU32(base, buffer + 0x18)), bytes = g::ReadU32(base, buffer + 0x1C);
  REXLOG_INFO("cod3: ngl side: grass: index buffer {:08X}: {}", buffer, Words(base, buffer, 8));
  if (Physical(address) && bytes > 0 && bytes <= kMaxVertexBufferBytes) {
    SaveGuestBytes(base, address, bytes, "ngl-side-grass-ib.bin");
  }
}

void ProbeGrass(uint8_t* base, int index) {
  const uint32_t device = g::ReadU32(base, kDevicePointer);
  if (!DeviceReadable(device)) return;
  REXLOG_INFO("cod3: ngl side: grass draw {} (device {:08X}): vertex constants:{}", index, device,
              ConstantsText(base, device + kDeviceVertexConstants, nullptr));
  REXLOG_INFO("cod3: ngl side: grass draw {}:   pixel constants:{}", index,
              ConstantsText(base, device + kDevicePixelConstants, nullptr));
  // the registers the grass shader reads (c12..c15 the view-projection, c16..c51 NGL's, c80..c85 the grass's own),
  // to set beside the native path's ("grass inputs")
  std::vector<float> c(96 * 4);
  for (size_t i = 0; i < c.size(); ++i) c[i] = g::ReadF32(base, device + kDeviceVertexConstants + 4 * i);
  const uint32_t scene = g::ReadU32(base, kCurrentScene);
  REXLOG_INFO("cod3: ngl side: grass draw {}: scene {:08X} time {};{}", index, scene,
              Pointer(scene) ? g::ReadF32(base, scene + 0x420) : 0.0f,
              RegistersText(c.data(), 12, 4) + RegistersText(c.data(), 80, 6));
  if (index != kGrassDrawProbed) return;
  REXLOG_INFO("cod3: ngl side: grass draw {}:{}", index, RegistersText(c.data(), 16, 36));
  SaveShader(base, g::ReadU32(base, device + kDeviceVertexShader), 0x368, 0x1C, true, "ngl-side-grass");
  SaveShader(base, g::ReadU32(base, device + kDevicePixelShader), 0x28, 0x18, false, "ngl-side-grass");
  SaveFetches(base, device);
  SaveIndices(base, device);
}

// side84: the old material classes' draws. nglDrawMeshSection (0x8213C9F8; COD3-NGL-MAP.md: cdSky's five draws a
// frame in the forest, cdDynamicDecal's one) draws through Direct3D itself, so the native side never meets those meshes
// in the scene lists, and the sky is missing from its picture. A draw whose return address is in its body is logged
// with what the GPU is handed (the fetch constants in use and the first words of each vertex buffer, the index buffer,
// the vertex constants: the sky's matrix); side85 draws the sky natively from them (KeepSkyDraw, SkyDraws).
#if defined(COD3_MP)
// cod3mp's nglDrawMeshSection (cod3_relatives.tsv), its 204 bytes as the campaign's
constexpr uint32_t kMeshSectionStart = 0x82155A80, kMeshSectionEnd = 0x82155A80 + 0xCC;
#else
constexpr uint32_t kMeshSectionStart = 0x8213C9F8, kMeshSectionEnd = 0x8213CC00;  // the end: a guess past its draws
#endif
constexpr int kMeshSectionProbed = 3000;  // about frame 600, in play
constexpr int kMeshSectionsLogged = 8;
std::atomic<int> g_mesh_section_draws{0};

void ProbeMeshSection(uint8_t* base, uint32_t call_site, int index) {
  const uint32_t device = g::ReadU32(base, kDevicePointer);
  if (!DeviceReadable(device)) return;
  REXLOG_INFO("cod3: ngl side: mesh section draw {} from {:08X}: vertex constants:{}", index, call_site,
              ConstantsText(base, device + kDeviceVertexConstants, nullptr));
  for (int f = 0; f < kFetchConstants; ++f) {
    const uint32_t at = device + kDeviceFetchConstants + kFetchConstantBytes * static_cast<uint32_t>(f);
    uint32_t any = 0;
    for (uint32_t w = 0; w < kFetchConstantBytes; w += 4) any |= g::ReadU32(base, at + w);
    if (!any) continue;
    std::string buffers;
    if ((g::ReadU32(base, at) & 3) != 2) {  // vertex fetches (type 3), three to a constant: each buffer's first words
      for (uint32_t v = 0; v < 3; ++v) {
        const uint32_t word0 = g::ReadU32(base, at + 8 * v);
        const uint32_t address = GuestAddress(word0 & 0x1FFFFFFC);
        if ((word0 & 3) == 3 && Physical(address)) {
          buffers += " | buffer " + std::to_string(3 * f + static_cast<int>(v)) + ": " + Words(base, address, 12);
        }
      }
    }
    REXLOG_INFO("cod3: ngl side: mesh section draw {}: fetch constant {}: {}{}", index, f,
                Words(base, at, kFetchConstantBytes / 4), buffers);
  }
  const uint32_t buffer = g::ReadU32(base, device + kDeviceIndexBuffer);
  REXLOG_INFO("cod3: ngl side: mesh section draw {}: index buffer {:08X}: {}", index, buffer,
              DeviceReadable(buffer) ? Words(base, buffer, 8) : std::string("not readable"));
  // side93: the haze. A world surface's draw hands the fog as vertex constants (c12: the amount before the start, the
  // start, 1 / (end - start), the amount's rise; c13: the eye in the object's space; c16: the color), linear by the
  // distance from the eye as ngl_vk does, yet the game's distance looks hazier. Each new pair of shaders is saved
  // (ngl-side-section<N>-vs/-ps-*.bin) with the pixel constants, to read what the pixel shader does with it.
  static std::mutex saved_mutex;
  static std::unordered_set<uint64_t> saved;
  const uint32_t vertex_shader = g::ReadU32(base, device + kDeviceVertexShader);
  const uint32_t pixel_shader = g::ReadU32(base, device + kDevicePixelShader);
  {
    std::lock_guard lock(saved_mutex);
    if (saved.size() >= 4 || !saved.insert(uint64_t{vertex_shader} << 32 | pixel_shader).second) return;
  }
  REXLOG_INFO("cod3: ngl side: mesh section draw {}: pixel constants:{}", index,
              ConstantsText(base, device + kDevicePixelConstants, nullptr));
  const std::string prefix = "ngl-side-section" + std::to_string(index);
  SaveShader(base, vertex_shader, 0x368, 0x1C, true, prefix);
  SaveShader(base, pixel_shader, 0x28, 0x18, false, prefix);
}

// side85: the sky, drawn natively from what nglDrawMeshSection hands Direct3D. side84 found, at the sky's draws: in
// texture fetch constant 0 the layer's picture (a DXT5 256 x 256 and a 128 x 128 in the forest); in vertex fetch
// constant 95 the dome (65 corners of six words: position x, y, z, u, v and a D3DCOLOR); the index buffer (a triangle
// list, 336 16-bit indices); and in c0..c3 the camera's view-projection without its position (Direct3D's columns;
// c12..c15 are the same with it), so the dome stays centered on the eye. Each such draw is kept (KeepSkyDraw), and the
// next native picture draws the ones from the last list sends first: at the far plane (z = w, as Big Red One's
// ngl_present.c does for its sky), the first layer opaque and the others blended over it by their alpha, in the order
// the game drew them. A draw whose c0..c3 has a position (cdDynamicDecal's, the other user of nglDrawMeshSection) is
// left out. The layers' blend states (side98) and drift (side102) are the draws' own.
struct SkyDraw {
  uint32_t texture[6] = {};  // the texture fetch constant
  uint32_t vertices = 0, vertex_words = 0, vertex_endian = 0;
  uint32_t indices = 0, index_bytes = 0;
  float columns[16] = {};  // c0..c3
  uint64_t list = 0;       // the list send it was last seen after
  // side98: how the layer goes onto the ones under it, RB_BLENDCONTROL0 as the draw sends it (bits 0-4 the source
  // factor, 5-7 the operation, 8-12 the destination factor), and the pixel shader's first two constants (a tint?)
  uint32_t blend_control = 0;
  float pixel[8] = {};
  // side102: the layer's drift, c4's x and y, which its vertex shader (shader_24CFBA2A030227A9) adds to the texture
  // coordinates (the game moves it with the clock: the clouds' scroll)
  float drift[2] = {};
};
constexpr uint32_t kDeviceBlendControl0 = 0x28B8;  // register 0x2201 (+0x28B4 holds 0x2200-0x220B)
constexpr uint32_t kSkyVertexFetch = 95;  // the stream the game's shaders read (fetch constant 31's third)
constexpr uint32_t kSkyVertexWords = 6;
constexpr uint32_t kSkyMaxCorners = 4096;
constexpr uint32_t kSkyMaxIndexBytes = 0x20000;
constexpr size_t kSkyMaxDraws = 16;
constexpr uint64_t kSkyRecent = 3;  // list sends a sky draw is still drawn for
std::atomic<uint64_t> g_list_sends{0};
std::mutex g_sky_mutex;
std::vector<SkyDraw> g_sky;  // in the order the game first drew them

void KeepSkyDraw(uint8_t* base) {
  static std::atomic<int> shown{0}, refused{0};
  const uint32_t device = g::ReadU32(base, kDevicePointer);
  if (!DeviceReadable(device)) return;
  SkyDraw s;
  for (uint32_t i = 0; i < 6; ++i) s.texture[i] = g::ReadU32(base, device + kDeviceFetchConstants + 4 * i);
  const uint32_t vertex_at = device + kDeviceFetchConstants + 8 * kSkyVertexFetch;
  const uint32_t word0 = g::ReadU32(base, vertex_at), word1 = g::ReadU32(base, vertex_at + 4);
  s.vertices = GuestAddress(word0 & 0x1FFFFFFC);
  s.vertex_words = (word1 >> 2) & 0xFFFFFF;
  s.vertex_endian = word1 & 3;
  const uint32_t buffer = g::ReadU32(base, device + kDeviceIndexBuffer);
  if (DeviceReadable(buffer)) {
    s.indices = GuestAddress(g::ReadU32(base, buffer + kBufferFetch));
    s.index_bytes = g::ReadU32(base, buffer + kBufferFetch + 4);
  }
  for (uint32_t i = 0; i < 16; ++i) s.columns[i] = g::ReadF32(base, device + kDeviceVertexConstants + 4 * i);
  for (uint32_t i = 0; i < 2; ++i) {
    s.drift[i] = g::ReadF32(base, device + kDeviceVertexConstants + 4 * (16 + i));
    if (!std::isfinite(s.drift[i])) s.drift[i] = 0.0f;
  }
  s.blend_control = g::ReadU32(base, device + kDeviceBlendControl0);
  for (uint32_t i = 0; i < 8; ++i) s.pixel[i] = g::ReadF32(base, device + kDevicePixelConstants + 4 * i);
  const float position = std::fabs(s.columns[3]) + std::fabs(s.columns[7]) + std::fabs(s.columns[15]);
  const char* why = nullptr;
  if ((s.texture[0] & 3) != 2) {
    why = "no texture in fetch constant 0";
  } else if ((word0 & 3) != 3 || !Physical(s.vertices) || !s.vertex_words || s.vertex_words % kSkyVertexWords ||
             s.vertex_words / kSkyVertexWords > kSkyMaxCorners) {
    why = "no dome in vertex fetch constant 95";
  } else if (!Physical(s.indices) || s.index_bytes < 6 || s.index_bytes > kSkyMaxIndexBytes) {
    why = "no index buffer";
  } else if (!(position < 1.0f)) {
    why = "its view-projection has a position (not the sky)";
  }
  if (why) {
    if (refused.fetch_add(1, std::memory_order_relaxed) < 4) {
      REXLOG_INFO("cod3: ngl side: mesh section draw left out of the sky: {}", why);
    }
    return;
  }
  s.list = g_list_sends.load(std::memory_order_relaxed);
  std::lock_guard<std::mutex> lock(g_sky_mutex);
  for (SkyDraw& kept : g_sky) {
    if (kept.vertices == s.vertices && kept.indices == s.indices && kept.texture[1] == s.texture[1]) {
      kept = s;
      return;
    }
  }
  if (g_sky.size() >= kSkyMaxDraws) {  // room for a new one: the one seen longest ago goes
    g_sky.erase(std::min_element(g_sky.begin(), g_sky.end(),
                                 [](const SkyDraw& a, const SkyDraw& b) { return a.list < b.list; }));
  }
  g_sky.push_back(s);
  if (shown.fetch_add(1, std::memory_order_relaxed) < 8) {
    REXLOG_INFO("cod3: ngl side: sky layer {}: texture {}x{} format {} at {:08X}; {} corners at {:08X} (byte order "
                "{}); {} index bytes at {:08X}; near {:.3f}; blend {:08X}; pixel c0 ({:.3f} {:.3f} {:.3f} {:.3f}) c1 "
                "({:.3f} {:.3f} {:.3f} {:.3f})",
                g_sky.size(), (s.texture[2] & 0x1FFF) + 1, ((s.texture[2] >> 13) & 0x1FFF) + 1, s.texture[1] & 0x3F,
                s.texture[1] & 0x1FFFF000, s.vertex_words / kSkyVertexWords, s.vertices, s.vertex_endian,
                s.index_bytes, s.indices, s.columns[11], s.blend_control, s.pixel[0], s.pixel[1], s.pixel[2],
                s.pixel[3], s.pixel[4], s.pixel[5], s.pixel[6], s.pixel[7]);
  }
}

// side104: the effects' quads (smoke, dust, sparks, muzzle flashes), drawn natively. The game's effects go through one
// quad batcher (0x820FAED0; its vertex shader shader_AACFBC52742E4628): four corners of 12 words a quad (a position
// in the world, a D3DCOLOR at word 3, the texture coordinates at words 7-8), six indices a quad, flushed with
// DrawIndexedVertices from 0x820FAF80. On the frame before a dump KeepEffectDraw copies each flush's quads (the
// batcher's quad count at 0x8290B5AC, its first index at 0x8290B5B4; the corners from vertex fetch constant 95, as the
// sky's), the texture in fetch constant 0, the blend state and the view-projection (c0..c3). The native picture draws
// them blended after the world, in their corners' colors and the scene's fog. Left out for now: the soft edge where a
// quad meets the scene (its pixel shader, shader_8356D96B00DE9DED, fades alpha by the depth difference), the tint in
// c16. side110: cod3mp's batcher is 0x8210CDF8 (cod3_relatives.tsv), the same 256 bytes with its globals moved: the
// quad count at 0x82A8843C, the first index at 0x82A88444, the flush returning at 0x8210CEA8 (the same +0xB0).
#if defined(COD3_NGL_VK)  // the effects' quads are kept only for ngl_vk to draw
#if defined(COD3_MP)
constexpr uint32_t kEffectDrawReturn = 0x8210CEA8;
constexpr uint32_t kEffectQuadCount = 0x82A8843C;
constexpr uint32_t kEffectFirstIndex = 0x82A88444;
#else
constexpr uint32_t kEffectDrawReturn = 0x820FAF80;
constexpr uint32_t kEffectQuadCount = 0x8290B5AC;
constexpr uint32_t kEffectFirstIndex = 0x8290B5B4;
#endif
#endif
#if !defined(COD3_MP)
// side107: what the game's lighting (0x82142360, called with 0x829112A0) hands a model's shaders. At 0x829112A0: the
// main light's color (+0x00) and way (+0x10; with a kind 3 light, the sun's way and the first kind 1's color), the
// nearest point light (+0x30 color, +0x40 place, +0x50 1 / radius) and the next (+0x60, +0x70, +0x80); at 0x82911390
// nine four-float rows, a quadratic form in the normal (xx, yy, zz, then the cross terms, then x, y, z), the set's
// ambient added into xx, yy and zz. The models' draw returns at 0x82144364.
constexpr uint32_t kModelDrawReturn = 0x82144364;
constexpr uint32_t kModelLights = 0x829112A0;
constexpr uint32_t kModelLightRows = 0x82911390;
#endif
#if defined(COD3_NGL_VK)  // (the effects' quads, kept for EffectDraws)
constexpr uint32_t kEffectCornerWords = 12;
constexpr uint32_t kEffectMaxQuads = 4096;
constexpr size_t kEffectMaxDraws = 256;

struct EffectDraw {
  uint32_t texture[6] = {};  // the texture fetch constant
  uint32_t blend_control = 0;
  float columns[16] = {};    // c0..c3
  uint32_t quads = 0;
  uint64_t list = 0;
  std::vector<float> corners;  // NGL_VK_CORNER_FLOATS a corner, four a quad
};
std::mutex g_effect_mutex;
std::vector<EffectDraw> g_effects;
uint32_t g_effect_pattern[6] = {0, 1, 2, 0, 2, 3};  // a quad's six indices, as the game's index buffer has them
std::atomic<bool> g_effect_pattern_read{false};

void KeepEffectDraw(uint8_t* base) {
  static std::atomic<int> refused{0}, shown{0};
  if (g_frames % kDumpEvery != kDumpEvery - 1) return;  // only the frame a dump draws
  const uint32_t device = g::ReadU32(base, kDevicePointer);
  if (!DeviceReadable(device)) return;
  const uint32_t quads = g::ReadU32(base, kEffectQuadCount) & 0x7FFFFFFF;
  const uint32_t first_index = g::ReadU32(base, kEffectFirstIndex) & 0x7FFFFFFF;
  const uint32_t first_corner = first_index / 6 * 4;
  const uint32_t vertex_at = device + kDeviceFetchConstants + 8 * kSkyVertexFetch;
  const uint32_t word0 = g::ReadU32(base, vertex_at), word1 = g::ReadU32(base, vertex_at + 4);
  const uint32_t vertices = GuestAddress(word0 & 0x1FFFFFFC);
  const uint32_t words = (word1 >> 2) & 0xFFFFFF, endian = word1 & 3;
  const char* why = nullptr;
  if (!quads || quads > kEffectMaxQuads) {
    why = "no quads, or too many";
  } else if ((word0 & 3) != 3 || !Physical(vertices)) {
    why = "no corners in vertex fetch constant 95";
  } else if (uint64_t{first_corner + 4 * quads} * kEffectCornerWords > words) {
    why = "its quads run past the corners";
  } else if ((g::ReadU32(base, device + kDeviceFetchConstants) & 3) != 2) {
    why = "no texture in fetch constant 0";
  }
  if (why) {
    if (refused.fetch_add(1, std::memory_order_relaxed) < 4) {
      REXLOG_INFO("cod3: ngl side: effect flush left out: {} ({} quads from index {})", why, quads, first_index);
    }
    return;
  }
  if (!g_effect_pattern_read.exchange(true)) {  // the game's corner order, from its index buffer, once
    const uint32_t buffer = g::ReadU32(base, device + kDeviceIndexBuffer);
    const uint32_t indices = DeviceReadable(buffer) ? GuestAddress(g::ReadU32(base, buffer + kBufferFetch)) : 0;
    for (int order = 0; order < 2 && Physical(indices); ++order) {
      uint32_t pattern[6];
      bool fits = true;
      for (uint32_t k = 0; k < 6 && fits; ++k) {
        const uint8_t* p = base + indices + 2 * (first_index + k);
        const uint32_t value = order == 0 ? (uint32_t{p[0]} << 8 | p[1]) : (uint32_t{p[1]} << 8 | p[0]);
        fits = value >= first_corner && value < first_corner + 4;
        pattern[k] = value - first_corner;
      }
      if (fits) {
        std::memcpy(g_effect_pattern, pattern, sizeof pattern);
        REXLOG_INFO("cod3: ngl side: an effect quad's indices: {} {} {} {} {} {}", pattern[0], pattern[1], pattern[2],
                    pattern[3], pattern[4], pattern[5]);
        break;
      }
    }
  }
  EffectDraw e;
  for (uint32_t i = 0; i < 6; ++i) e.texture[i] = g::ReadU32(base, device + kDeviceFetchConstants + 4 * i);
  e.blend_control = g::ReadU32(base, device + kDeviceBlendControl0);
  for (uint32_t i = 0; i < 16; ++i) e.columns[i] = g::ReadF32(base, device + kDeviceVertexConstants + 4 * i);
  e.quads = quads;
  // side105: the vertex shader's c17.xy scales the texture coordinates (a flipbook's frame in a grid of frames: a
  // corner's coordinates count frames), c16 tints the color and c14 fades the near quads. Direct3D's copy is read
  // here and c17 used when it is a whole-number grid (1/n for x and y); the others are logged to see if they're live.
  float c14[4], c16[4], c17[4];
  for (uint32_t i = 0; i < 4; ++i) {
    c14[i] = g::ReadF32(base, device + kDeviceVertexConstants + 16 * 14 + 4 * i);
    c16[i] = g::ReadF32(base, device + kDeviceVertexConstants + 16 * 16 + 4 * i);
    c17[i] = g::ReadF32(base, device + kDeviceVertexConstants + 16 * 17 + 4 * i);
  }
  const auto grid = [](float f) {
    if (!(f > 0.0f && f <= 1.0f)) return false;
    const float frames = 1.0f / f;
    return std::fabs(frames - std::round(frames)) < 1e-3f;
  };
  const bool flipbook = grid(c17[0]) && grid(c17[1]);
  e.corners.assign(static_cast<size_t>(4 * quads) * NGL_VK_CORNER_FLOATS, 0.0f);
  for (uint32_t q = 0; q < quads; ++q) {
    bool numbers = true;
    for (uint32_t c = 4 * q; c < 4 * q + 4; ++c) {
      const uint32_t at = vertices + 4 * kEffectCornerWords * (first_corner + c);
      float* out = &e.corners[static_cast<size_t>(c) * NGL_VK_CORNER_FLOATS];
      for (uint32_t k = 0; k < 3; ++k) out[k] = VertexFloat(base, at + 4 * k, endian);
      out[3] = out[5] = VertexFloat(base, at + 28, endian) * (flipbook ? c17[0] : 1.0f);
      out[4] = out[6] = VertexFloat(base, at + 32, endian) * (flipbook ? c17[1] : 1.0f);
      uint32_t color = g::ReadU32(base, at + 12);  // a D3DCOLOR, 0xAARRGGBB
      if (endian == 0) color = __builtin_bswap32(color);
      {  // side111: as the vertex shader does, the color times c16 and the alpha faded near the eye by
         // saturate(w x c14.x + c14.y), w the corner's clip w (its distance along the view, through c3)
        const float w = e.columns[12] * out[0] + e.columns[13] * out[1] + e.columns[14] * out[2] + e.columns[15];
        float fade = w * c14[0] + c14[1];
        fade = std::isfinite(fade) ? std::min(std::max(fade, 0.0f), 1.0f) : 1.0f;
        const float scale[4] = {c16[2], c16[1], c16[0], c16[3] * fade};  // blue, green, red, alpha (low byte first)
        uint32_t tinted = 0;
        for (uint32_t k = 0; k < 4; ++k) {
          const float v = static_cast<float>((color >> (8 * k)) & 255u) * (std::isfinite(scale[k]) ? scale[k] : 1.0f);
          tinted |= static_cast<uint32_t>(std::min(std::max(v, 0.0f), 255.0f) + 0.5f) << (8 * k);
        }
        color = tinted;
      }
      std::memcpy(&out[7], &color, 4);
      for (uint32_t k = 0; k < 5; ++k) numbers = numbers && std::isfinite(out[k]);
    }
    if (!numbers) {  // a quad with a corner that isn't a number: left as a point at the origin, drawing nothing
      const auto quad = e.corners.begin() + static_cast<std::ptrdiff_t>(4 * q) * NGL_VK_CORNER_FLOATS;
      std::fill(quad, quad + 4 * NGL_VK_CORNER_FLOATS, 0.0f);
    }
  }
  e.list = g_list_sends.load(std::memory_order_relaxed);
  if (shown.fetch_add(1, std::memory_order_relaxed) < 6) {
    REXLOG_INFO("cod3: ngl side: effect flush: {} quads from index {} (corners at {:08X}, byte order {}), texture {}x{} "
                "format {}, blend {:08X}; first corner ({:.1f} {:.1f} {:.1f}) uv ({:.3f} {:.3f}); c14 ({:.4g} {:.4g} "
                "{:.4g} {:.4g}) c16 ({:.4g} {:.4g} {:.4g} {:.4g}) c17 ({:.4g} {:.4g} {:.4g} {:.4g}){}",
                quads, first_index, vertices, endian, (e.texture[2] & 0x1FFF) + 1, ((e.texture[2] >> 13) & 0x1FFF) + 1,
                e.texture[1] & 0x3F, e.blend_control, e.corners[0], e.corners[1], e.corners[2], e.corners[3],
                e.corners[4], c14[0], c14[1], c14[2], c14[3], c16[0], c16[1], c16[2], c16[3], c17[0], c17[1], c17[2],
                c17[3], flipbook ? ", a flipbook" : "");
  }
  std::lock_guard<std::mutex> lock(g_effect_mutex);
  const uint64_t list = e.list;
  g_effects.erase(std::remove_if(g_effects.begin(), g_effects.end(),
                                 [list](const EffectDraw& kept) { return kept.list + 1 < list; }),
                  g_effects.end());
  if (g_effects.size() < kEffectMaxDraws) g_effects.push_back(std::move(e));
}
#endif

#if defined(COD3_NGL_VK)
std::unordered_map<uint64_t, uint32_t> g_sky_meshes;    // vertices << 32 | indices -> mesh (0: not drawable)
std::unordered_map<uint64_t, std::vector<float>> g_sky_corners;  // the same key -> the mesh's corners (side102)
std::unordered_map<uint64_t, uint32_t> g_sky_textures;  // texture address << 32 | size word -> texture (0: none)

uint32_t SkyMesh(uint8_t* base, const SkyDraw& s, std::string& why) {
  const uint64_t key = uint64_t{s.vertices} << 32 | s.indices;
  const auto found = g_sky_meshes.find(key);
  if (found != g_sky_meshes.end()) {
    if (!found->second) why = "left out before";
    return found->second;
  }
  uint32_t& mesh = g_sky_meshes[key];
  const uint32_t count = s.vertex_words / kSkyVertexWords;
  std::vector<float> corners(static_cast<size_t>(count) * NGL_VK_CORNER_FLOATS, 0.0f);
  for (uint32_t i = 0; i < count; ++i) {
    const uint32_t at = s.vertices + 4 * kSkyVertexWords * i;
    float* c = &corners[static_cast<size_t>(i) * NGL_VK_CORNER_FLOATS];
    for (uint32_t k = 0; k < 5; ++k) c[k] = VertexFloat(base, at + 4 * k, s.vertex_endian);
    c[5] = c[3];
    c[6] = c[4];
    uint32_t color = g::ReadU32(base, at + 20);  // a D3DCOLOR, 0xAARRGGBB
    if (s.vertex_endian == 0) color = __builtin_bswap32(color);
    std::memcpy(&c[7], &color, 4);
    for (uint32_t k = 0; k < 5; ++k) {
      if (!std::isfinite(c[k])) {
        why = "a corner isn't a number";
        return 0;
      }
    }
  }
  // the indices, in the byte order that keeps them all inside the corners
  const uint32_t index_count = s.index_bytes / 2 / 3 * 3;
  std::vector<uint16_t> indices(index_count);
  for (int order = 0; order < 2; ++order) {
    bool fits = true;
    for (uint32_t i = 0; i < index_count && fits; ++i) {
      const uint8_t* p = base + s.indices + 2 * i;
      const uint32_t value = order == 0 ? (uint32_t{p[0]} << 8 | p[1]) : (uint32_t{p[1]} << 8 | p[0]);
      fits = value < count;
      indices[i] = static_cast<uint16_t>(value);
    }
    if (fits) {
      mesh = ngl_vk_mesh(corners.data(), count, indices.data(), index_count);
      if (!mesh) why = "ngl_vk has no room";
      if (mesh) g_sky_corners[key] = std::move(corners);
      return mesh;
    }
  }
  why = "indices past the corners";
  return 0;
}

std::vector<ngl_vk_mesh_draw> SkyDraws(uint8_t* base, uint64_t frame) {
  std::vector<SkyDraw> sky;
  const uint64_t now = g_list_sends.load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(g_sky_mutex);
    for (const SkyDraw& s : g_sky) {
      if (s.list + kSkyRecent >= now) sky.push_back(s);
    }
  }
  std::vector<ngl_vk_mesh_draw> draws;
  std::string text;
  for (const SkyDraw& s : sky) {
    std::string why;
    const uint32_t mesh = SkyMesh(base, s, why);
    const uint64_t texture_key = uint64_t{s.texture[1] & 0x1FFFF000} << 32 | s.texture[2];
    auto texture = g_sky_textures.find(texture_key);
    if (texture == g_sky_textures.end()) {
      std::string texture_why;
      const uint32_t id = MakeTexture(base, s.texture, s.texture[1] & 0x1FFFF000, texture_why);
      if (!id) REXLOG_INFO("cod3: ngl side: sky texture {:08X} not drawn: {}", s.texture[1] & 0x1FFFF000, texture_why);
      texture = g_sky_textures.emplace(texture_key, id).first;
    }
    if (!mesh || !texture->second) {
      text += " (left out: " + (mesh ? std::string("no texture") : why) + ")";
      continue;
    }
    ngl_vk_mesh_draw d{};
    d.mesh = mesh;
    d.texture = texture->second;
    if (s.drift[0] != 0.0f || s.drift[1] != 0.0f) {  // side102: this frame's corners, moved by the layer's drift
      const auto kept = g_sky_corners.find(uint64_t{s.vertices} << 32 | s.indices);
      if (kept != g_sky_corners.end()) {
        std::vector<float> moved = kept->second;
        for (size_t at = 0; at + NGL_VK_CORNER_FLOATS <= moved.size(); at += NGL_VK_CORNER_FLOATS) {
          moved[at + 3] += s.drift[0];
          moved[at + 4] += s.drift[1];
        }
        d.corners = ngl_vk_frame_corners(moved.data(), static_cast<uint32_t>(moved.size() / NGL_VK_CORNER_FLOATS));
      }
    }
    d.shading = NGL_VK_SHADE_UNLIT;
    d.alpha = draws.empty() ? NGL_VK_ALPHA_OPAQUE : NGL_VK_ALPHA_BLEND;
    d.blend = NGL_VK_BLEND_ALPHA;
    const char* how = draws.empty() ? "opaque" : "blended";
    {  // side98: the layer's own blend state when the draw had one (Xenos factors: 0 zero, 1 one, 6 source alpha,
       // 7 one minus source alpha; operation 0 add)
      const uint32_t source = s.blend_control & 0x1F, operation = (s.blend_control >> 5) & 7;
      const uint32_t destination = (s.blend_control >> 8) & 0x1F;
      if (s.blend_control && source == 1 && destination == 0) {
        d.alpha = NGL_VK_ALPHA_OPAQUE;
        how = "opaque";
      } else if (s.blend_control && operation == 0 && destination == 1) {
        d.alpha = NGL_VK_ALPHA_BLEND;
        d.blend = NGL_VK_BLEND_ADD;
        how = "added";
      } else if (s.blend_control && operation == 0 && source == 6 && destination == 7) {
        d.alpha = NGL_VK_ALPHA_BLEND;
        how = "blended";
      }
    }
    d.world[0] = d.world[5] = d.world[10] = d.world[15] = 1.0f;
    for (int row = 0; row < 4; ++row) {
      for (int column = 0; column < 4; ++column) d.view_projection[4 * row + column] = s.columns[4 * column + row];
      d.view_projection[4 * row + 2] = d.view_projection[4 * row + 3];  // z = w: at the far plane
    }
    char item[96];
    std::snprintf(item, sizeof item, " %s %ux%u (%08X, drift %.3f %.3f)", how, (s.texture[2] & 0x1FFF) + 1,
                  ((s.texture[2] >> 13) & 0x1FFF) + 1, s.blend_control, s.drift[0], s.drift[1]);
    text += item;
    draws.push_back(d);
  }
  REXLOG_INFO("cod3: ngl side: frame {}: sky: {} of {} layers drawn:{}", frame, draws.size(), sky.size(),
              text.empty() ? std::string(" none") : text);
  return draws;
}
// side104: the last frame's effect flushes as blended draws of one shared quad mesh (kEffectMaxQuads quads of the
// game's corner order), each with its own corners for this frame; fogged as the world (fog_from: a world draw).
uint32_t g_effect_mesh = 0;
std::unordered_map<uint64_t, uint32_t> g_effect_textures;  // texture address << 32 | size word -> texture (0: none)

std::vector<ngl_vk_mesh_draw> EffectDraws(uint8_t* base, uint64_t frame, const ngl_vk_mesh_draw* fog_from) {
  std::vector<EffectDraw> effects;
  const uint64_t now = g_list_sends.load(std::memory_order_relaxed);
  {
    std::lock_guard<std::mutex> lock(g_effect_mutex);
    for (const EffectDraw& e : g_effects) {
      if (e.list + 1 == now) effects.push_back(e);
    }
  }
  std::vector<ngl_vk_mesh_draw> draws;
  if (effects.empty()) return draws;
  if (!g_effect_mesh) {
    std::vector<float> corners(static_cast<size_t>(4 * kEffectMaxQuads) * NGL_VK_CORNER_FLOATS, 0.0f);
    std::vector<uint16_t> indices(6 * kEffectMaxQuads);
    for (uint32_t q = 0; q < kEffectMaxQuads; ++q) {
      for (uint32_t k = 0; k < 6; ++k) indices[6 * q + k] = static_cast<uint16_t>(4 * q + g_effect_pattern[k]);
    }
    g_effect_mesh = ngl_vk_mesh(corners.data(), 4 * kEffectMaxQuads, indices.data(), 6 * kEffectMaxQuads);
    if (!g_effect_mesh) {
      REXLOG_INFO("cod3: ngl side: no room in ngl_vk for the effects' quad mesh");
      return draws;
    }
  }
  uint32_t quads = 0, added = 0, untextured = 0;
  for (const EffectDraw& e : effects) {
    const uint64_t texture_key = uint64_t{e.texture[1] & 0x1FFFF000} << 32 | e.texture[2];
    auto texture = g_effect_textures.find(texture_key);
    if (texture == g_effect_textures.end()) {
      std::string why;
      const uint32_t id = MakeTexture(base, e.texture, e.texture[1] & 0x1FFFF000, why);
      if (!id) REXLOG_INFO("cod3: ngl side: effect texture {:08X} not drawn: {}", e.texture[1] & 0x1FFFF000, why);
      texture = g_effect_textures.emplace(texture_key, id).first;
    }
    if (!texture->second) {
      ++untextured;
      continue;
    }
    ngl_vk_mesh_draw d{};
    d.mesh = g_effect_mesh;
    d.corners = ngl_vk_frame_corners(e.corners.data(), 4 * e.quads);
    if (!d.corners) break;  // the frame's room is used up
    d.index_count = 6 * e.quads;
    d.texture = texture->second;
    d.shading = NGL_VK_SHADE_VERTEX_COLOR;
    d.light_once = 1;  // the corners' colors as they are (the pixel shader's texture x color)
    d.alpha = NGL_VK_ALPHA_BLEND;
    // Xenos blend factors: 1 one, 6 source alpha, 7 one minus source alpha; a destination of one adds
    const uint32_t destination = (e.blend_control >> 8) & 0x1F;
    d.blend = destination == 1 ? NGL_VK_BLEND_ADD : NGL_VK_BLEND_ALPHA;
    added += destination == 1;
    d.world[0] = d.world[5] = d.world[10] = d.world[15] = 1.0f;
    for (int row = 0; row < 4; ++row) {
      for (int column = 0; column < 4; ++column) d.view_projection[4 * row + column] = e.columns[4 * column + row];
    }
    if (fog_from) {
      std::memcpy(d.fog, fog_from->fog, sizeof d.fog);
      d.fog_color = fog_from->fog_color;
    }
    quads += e.quads;
    draws.push_back(d);
  }
  REXLOG_INFO("cod3: ngl side: frame {}: effects: {} of {} flushes drawn ({} added, {} without a texture), {} quads",
              frame, draws.size(), effects.size(), added, untextured, quads);
  return draws;
}
#endif

}  // namespace

void OnListSend(uint8_t* base) {
  g_list_sends.fetch_add(1, std::memory_order_relaxed);
  if (!REXCVAR_GET(cod3_debug_ngl_side) || g_dumps >= kDumps) return;
  if (++g_frames % kDumpEvery != 0) return;
  ++g_dumps;
  Dump(base, g_frames);
  if (g_dumps == 3 || g_dumps == 6) ReadNextDraws();
}

// Diagnostics: which shader pairs the grass's draws use (draws kGrassDrawProbed to kGrassCensusEnd), to see whether
// the grass is drawn with more than the one vertex shader translated in grass.vert.
constexpr int kGrassCensusEnd = kGrassDrawProbed + 2000;
std::mutex g_grass_census_mutex;
std::map<std::pair<uint32_t, uint32_t>, int> g_grass_census;

void CountGrassShaders(uint8_t* base, int index) {
  const uint32_t device = g::ReadU32(base, kDevicePointer);
  if (!DeviceReadable(device)) return;
  const uint32_t vs = g::ReadU32(base, device + kDeviceVertexShader), ps = g::ReadU32(base, device + kDevicePixelShader);
  const uint32_t vs_block = GrassReadable(vs) ? g::ReadU32(base, vs + 0x1C) : 0;
  const uint32_t ps_block = GrassReadable(ps) ? g::ReadU32(base, ps + 0x18) : 0;
  std::lock_guard<std::mutex> lock(g_grass_census_mutex);
  ++g_grass_census[{vs_block, ps_block}];
  if (index != kGrassCensusEnd - 1) return;
  std::string text;
  for (const auto& [pair, count] : g_grass_census) {
    char item[64];
    std::snprintf(item, sizeof item, " vs %08X ps %08X: %d;", pair.first, pair.second, count);
    text += item;
  }
  REXLOG_INFO("cod3: ngl side: grass draws {} to {} by shader block:{} pixel c255 ({} {} {} {})", kGrassDrawProbed,
              kGrassCensusEnd - 1, text, g::ReadF32(base, device + kDevicePixelConstants + 255 * 16),
              g::ReadF32(base, device + kDevicePixelConstants + 255 * 16 + 4),
              g::ReadF32(base, device + kDevicePixelConstants + 255 * 16 + 8),
              g::ReadF32(base, device + kDevicePixelConstants + 255 * 16 + 12));
}

thread_local uint32_t t_pass_object = 0;  // side91: the object whose pass is being drawn (a gun's), else 0

// side103: the shaders the rest of the world is drawn with. Once in play (the mesh section probe's draw count), each
// new pixel shader at any draw, up to kShadersCensused, is saved with its vertex shader (ngl-side-draw<N>-vs/-ps-*.bin;
// results/acct2-bro-tools/match_shader.py names them) and logged with its pixel constants: the lightmapped surfaces'
// scale and added light (c16, c17 in shader_F38DD169E44123DB) among them.
constexpr int kShadersCensused = 24;

void CensusShader(uint8_t* base, uint32_t call_site) {
  static std::atomic<bool> done{false};
  static std::mutex census_mutex;
  static std::unordered_set<uint32_t> seen;
  static int saved = 0;
  if (done.load(std::memory_order_relaxed)) return;
  const uint32_t device = g::ReadU32(base, kDevicePointer);
  if (!DeviceReadable(device)) return;
  const uint32_t vertex_shader = g::ReadU32(base, device + kDeviceVertexShader);
  const uint32_t pixel_shader = g::ReadU32(base, device + kDevicePixelShader);
  int index;
  {
    std::lock_guard lock(census_mutex);
    if (saved >= kShadersCensused) {
      done.store(true, std::memory_order_relaxed);
      return;
    }
    if (!seen.insert(pixel_shader).second) return;
    index = saved++;
  }
  REXLOG_INFO("cod3: ngl side: shader census {} (a draw from {:08X}): vertex shader {:08X}, pixel shader {:08X}; pixel "
              "constants:{}",
              index, call_site, vertex_shader, pixel_shader,
              ConstantsText(base, device + kDevicePixelConstants, nullptr));
  const std::string prefix = "ngl-side-draw" + std::to_string(index);
  SaveShader(base, vertex_shader, 0x368, 0x1C, true, prefix);
  SaveShader(base, pixel_shader, 0x28, 0x18, false, prefix);
}

void OnPass(uint8_t* base, uint32_t object) {
  (void)base;
  t_pass_object = 0;
  if (!REXCVAR_GET(cod3_debug_ngl_side)) return;
  std::lock_guard<std::mutex> lock(g_front_mutex);
  if (g_front_objects.count(object)) t_pass_object = object;
}

void OnDraw(uint8_t* base, uint32_t call_site) {
  if (REXCVAR_GET(cod3_debug_ngl_side) &&
      g_mesh_section_draws.load(std::memory_order_relaxed) >= kMeshSectionProbed) {
    CensusShader(base, call_site);
  }
  if (t_pass_object && REXCVAR_GET(cod3_debug_ngl_side)) {  // side91: a draw of the gun: what the GPU is handed
    static std::atomic<int> probed{0};
    const uint32_t device = g::ReadU32(base, kDevicePointer);
    if (DeviceReadable(device) && probed.fetch_add(1, std::memory_order_relaxed) < 3) {
      REXLOG_INFO("cod3: ngl side: gun draw (object {:08X}, from {:08X}): vertex constants:{}", t_pass_object,
                  call_site, ConstantsText(base, device + kDeviceVertexConstants, nullptr));
      REXLOG_INFO("cod3: ngl side: gun draw: object +40: {} / {}", Words(base, t_pass_object + 0x40, 8),
                  Words(base, t_pass_object + 0x60, 8));
    }
    t_pass_object = 0;
  }
  if (call_site >= kMeshSectionStart && call_site < kMeshSectionEnd && REXCVAR_GET(cod3_debug_ngl_side)) {
    const int index = g_mesh_section_draws.fetch_add(1, std::memory_order_relaxed);
    if (index >= kMeshSectionProbed && index < kMeshSectionProbed + kMeshSectionsLogged) {
      ProbeMeshSection(base, call_site, index);
    }
    KeepSkyDraw(base);
  }
#if defined(COD3_NGL_VK)
  if (call_site == kEffectDrawReturn && REXCVAR_GET(cod3_debug_ngl_side)) KeepEffectDraw(base);  // side104, side110
#endif
#if !defined(COD3_MP)
  const bool section_draw = call_site >= kMeshSectionStart && call_site < kMeshSectionEnd;
  if ((call_site == kModelDrawReturn || section_draw) && REXCVAR_GET(cod3_debug_ngl_side) &&
      g_frames >= 3 * kDumpEvery - 1 && g_frames % kDumpEvery == kDumpEvery - 1) {
    // side107: the lights the game hands a model, on the frames before a dump; side109: the mesh sections' draws too
    // (the per-pixel lit models and the gun, shader FC767C09)
    static std::atomic<int> shown{0}, sections_shown{0};
    if ((section_draw ? sections_shown : shown).fetch_add(1, std::memory_order_relaxed) < (section_draw ? 24 : 36)) {
      auto vec = [&](uint32_t at) {
        char text[80];
        std::snprintf(text, sizeof text, "(%.3f %.3f %.3f %.3f)", g::ReadF32(base, at), g::ReadF32(base, at + 4),
                      g::ReadF32(base, at + 8), g::ReadF32(base, at + 12));
        return std::string(text);
      };
      std::string rows;
      for (uint32_t r = 0; r < 9; ++r) rows += " " + vec(kModelLightRows + 16 * r);
      REXLOG_INFO("cod3: ngl side: {} draw from {:08X} (frame {}): main light color {} way {}; point lights {} at {} / "
                  "{} at {}; rows:{}", section_draw ? "section" : "model", call_site, g_frames, vec(kModelLights),
                  vec(kModelLights + 0x10), vec(kModelLights + 0x30), vec(kModelLights + 0x40),
                  vec(kModelLights + 0x60), vec(kModelLights + 0x70), rows);
    }
  }
#endif
  if (call_site == kGrassDrawReturn && REXCVAR_GET(cod3_debug_ngl_side)) {
    const uint32_t device = g::ReadU32(base, kDevicePointer);
    if (DeviceReadable(device)) {  // where this draw fetched the blades from (vertex fetch constant 95)
      const uint32_t at = device + kDeviceFetchConstants + kFetchConstantBytes * 31 + 16;
      g_grass_fetch.store(uint64_t{g::ReadU32(base, at)} << 32 | g::ReadU32(base, at + 4), std::memory_order_relaxed);
    }
    const int index = g_grass_draws.fetch_add(1, std::memory_order_relaxed);
    if (index >= kGrassDrawProbed && index < kGrassDrawProbed + kGrassDrawsLogged) ProbeGrass(base, index);
    if (index >= kGrassDrawProbed && index < kGrassCensusEnd) CountGrassShaders(base, index);
  }
  if (g_draws_to_read.load(std::memory_order_relaxed) <= 0) return;
  if (g_draws_to_read.fetch_sub(1, std::memory_order_relaxed) <= 0) return;
  const uint32_t device = g::ReadU32(base, kDevicePointer);
  if (!DeviceReadable(device)) return;
  uint64_t key = call_site;
  const std::string pixel = ConstantsText(base, device + kDevicePixelConstants, &key);
  std::lock_guard<std::mutex> lock(g_draw_mutex);
  if (g_draws_logged >= kDrawsLogged || !g_draw_keys.insert(key).second) return;
  const int logged = g_draws_logged++;
  REXLOG_INFO("cod3: ngl side: draw from {:08X} (device {:08X}): RB_FOG_COLOR {:08X}; pixel constants:{}", call_site,
              device, g::ReadU32(base, device + kDeviceFogColor), pixel);
  if (logged < kVertexDrawsLogged) {
    REXLOG_INFO("cod3: ngl side:   its vertex constants:{}",
                ConstantsText(base, device + kDeviceVertexConstants, nullptr));
  }
}

}  // namespace cod3::ngl_side
