// cod3 - the glyphs the game's fonts lack, added in memory (see font_fill.h). [acct2-ocr]

#include "game/font_fill.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "game/guest.h"

REXCVAR_DEFINE_BOOL(cod3_font_fill, true, "cod3/Language",
                    "Add the letters and signs the game's fonts lack (« » „ “ ‘ ’ – ª º Ã Õ), made from "
                    "the fonts' own glyphs, from assets/text/fonts/. Off: plain stand-ins (\" for «). "
                    "Applies after a restart")
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

namespace cod3::font_fill {

namespace {

using cod3::guest::ReadS32;
using cod3::guest::ReadU32;
using cod3::guest::WriteU32;

// The FONT object (big-endian, as in the archive; addresses.h prompts::kFontBaseline).
constexpr uint32_t kLineHeight = 0x1C, kAscent = 0x20, kFirstCode = 0x24, kCount = 0x28;
constexpr uint32_t kRecords = 0x38, kRecordSize = 28;   // i32 x, y, w, h, x_off, y_off, advance
constexpr uint32_t kGlyphs = 224;
constexpr uint32_t kUvs = kRecords + kGlyphs * kRecordSize, kUvSize = 16;   // f32 u0, v0, u1, v1
// [acct2-ocr] 2026-11-05 The FONT as loaded (ui/text_native.cpp, NOTES 06:40): +4 its atlas TEX, +8 -> its
// records, +0x2C -> its UVs, which are 0..1 of the atlas (the archive's are pixels). Until now the TEX was looked
// for at +0x0C..0x14 only, so no font was ever filled ("found, but not its 512x256 DXT5 atlas" in every log).
constexpr uint32_t kFontTex = 0x04, kRecordsPtr = 0x08, kUvsPtr = 0x2C;
// The atlas TEX object: its texture fetch constant (6 words) at +0x48.
constexpr uint32_t kTexFetch = 0x48;
constexpr uint32_t kFormatDxt5 = 0x14, kEndianSwap8In16 = 1;
constexpr int kAtlasW = 512, kAtlasH = 256;

struct Glyph {
  uint32_t code;
  int x, y, w, h, x_off, y_off, advance;
  std::vector<uint8_t> alpha;   // w * h
};
struct FontFill {
  std::string name;
  std::array<int32_t, 9> check{};   // line height, ascent, the record of 'W'
  std::vector<Glyph> glyphs;
  std::vector<std::pair<uint32_t, uint32_t>> aliases;   // code -> the code whose glyph it shows
};

std::mutex g_mutex;
std::filesystem::path g_root;
bool g_loaded = false;
std::vector<FontFill> g_fills;
std::vector<uint32_t> g_seen;   // font objects already looked at
struct Filled {   // [acct2-ocr] a font filled: its g_fills index, how often it was filled again
  uint32_t font;
  size_t index;
  int again;
};
std::vector<Filled> g_filled;
std::atomic<bool> g_failed{false};
std::atomic<uint32_t> g_generation{1};

bool ParseInts(std::istringstream& in, int* out, int n) {
  for (int i = 0; i < n; ++i)
    if (!(in >> out[i])) return false;
  return true;
}

// assets/text/fonts/*.fill.txt (g_mutex held).
void LoadFiles() {
  g_loaded = true;
  g_fills.clear();
  std::error_code ec;
  const std::filesystem::path dir = g_root / "text" / "fonts";
  for (std::filesystem::directory_iterator it(dir, ec), end; !ec && it != end; it.increment(ec)) {
    const std::string file = it->path().filename().string();
    if (file.size() < 9 || file.compare(file.size() - 9, 9, ".fill.txt") != 0) continue;
    std::ifstream in(it->path(), std::ios::binary);
    FontFill f;
    std::string line;
    bool ok = true;
    while (ok && std::getline(in, line)) {
      if (!line.empty() && line.back() == '\r') line.pop_back();
      if (line.empty() || line[0] == '#') continue;
      std::istringstream s(line);
      std::string what;
      s >> what;
      if (what == "font") {
        s >> f.name;
      } else if (what == "check") {
        for (int32_t& v : f.check) ok = ok && static_cast<bool>(s >> v);
      } else if (what == "glyph") {
        Glyph g;
        std::string code;
        int v[7];
        ok = static_cast<bool>(s >> code) && ParseInts(s, v, 7);
        g.code = uint32_t(std::strtoul(code.c_str(), nullptr, 16));
        g.x = v[0], g.y = v[1], g.w = v[2], g.h = v[3], g.x_off = v[4], g.y_off = v[5], g.advance = v[6];
        ok = ok && g.code >= 0x20 && g.code < 0x20 + kGlyphs && g.w > 0 && g.h > 0 && g.x >= 0 && g.y >= 0 &&
             g.x + g.w <= kAtlasW && g.y + g.h <= kAtlasH;
        for (int row = 0; ok && row < g.h; ++row) {
          std::string hex;
          ok = static_cast<bool>(std::getline(in, hex));
          if (ok && !hex.empty() && hex.back() == '\r') hex.pop_back();
          ok = ok && int(hex.size()) == 2 * g.w;
          for (int i = 0; ok && i < g.w; ++i)
            g.alpha.push_back(uint8_t(std::strtoul(hex.substr(2 * i, 2).c_str(), nullptr, 16)));
        }
        if (ok) f.glyphs.push_back(std::move(g));
      } else if (what == "alias") {
        std::string a, b;
        ok = static_cast<bool>(s >> a >> b);
        if (ok) f.aliases.emplace_back(uint32_t(std::strtoul(a.c_str(), nullptr, 16)),
                                       uint32_t(std::strtoul(b.c_str(), nullptr, 16)));
      }
    }
    if (!ok || f.name.empty()) {
      REXLOG_WARN("cod3: fonts: {} can't be read: left out", file);
      g_failed.store(true);
      continue;
    }
    REXLOG_INFO("cod3: fonts: {} adds {} glyphs ({} shown as other glyphs)", f.name, f.glyphs.size(),
                f.aliases.size());
    g_fills.push_back(std::move(f));
  }
}

// --- the atlas: Xenos tiling, DXT5 -------------------------------------------------
// The block offset of a tiled 2D texture (block coordinates, row pitch in
// blocks, 16 bytes a block): the Xenos layout as the project's xenos crate has it.
uint32_t TiledBlock(uint32_t x, uint32_t y, uint32_t pitch_blocks) {
  constexpr uint32_t kBytesPerBlock = 16;
  const uint32_t aw = (pitch_blocks + 31) & ~31u;
  const uint32_t lb = (kBytesPerBlock >> 2) + ((kBytesPerBlock >> 1) >> (kBytesPerBlock >> 2));
  const uint32_t macro = ((x >> 5) + (y >> 5) * (aw >> 5)) << (lb + 7);
  const uint32_t micro = ((x & 7) + ((y & 6) << 2)) << lb;
  const uint32_t off = macro + ((micro & ~15u) << 1) + (micro & 15) + ((y & 8) << (3 + lb)) + ((y & 1) << 4);
  const uint32_t bank_pipe = ((((y & 8) >> 2) + (x >> 3)) & 3) << 6;
  return (((off & ~511u) << 3) + ((off & 448) << 2) + (off & 63) + ((y & 16) << 7) + bank_pipe) >> lb;
}

// One DXT5 block of white with this alpha (16 values, row by row), in the
// disc's byte order (Swap8In16).
std::array<uint8_t, 16> Dxt5WhiteBlock(const std::array<uint8_t, 16>& a) {
  const uint8_t hi = *std::max_element(a.begin(), a.end());
  const uint8_t lo = *std::min_element(a.begin(), a.end());
  uint64_t bits = 0;
  if (hi != lo) {
    // a0 > a1: codes 0 = a0, 1 = a1, 2..7 = (8-k)/7 a0 + (k-1)/7 a1.
    std::array<int, 8> pal{hi, lo};
    for (int k = 2; k < 8; ++k) pal[k] = ((8 - k) * hi + (k - 1) * lo) / 7;
    for (int i = 0; i < 16; ++i) {
      int best = 0;
      for (int k = 1; k < 8; ++k)
        if (std::abs(pal[k] - a[i]) < std::abs(pal[best] - a[i])) best = k;
      bits |= uint64_t(best) << (3 * i);
    }
  }
  std::array<uint8_t, 16> b{};
  b[0] = hi;
  b[1] = lo;
  for (int i = 0; i < 6; ++i) b[2 + i] = uint8_t(bits >> (8 * i));
  b[8] = b[9] = b[10] = b[11] = 0xFF;   // color 0 = color 1 = white (565), every index 0
  for (int i = 0; i < 16; i += 2) std::swap(b[i], b[i + 1]);
  return b;
}

// The atlas's fetch constant, if `tex` is a 512x256 DXT5 TEX the way the
// disc's fonts have them.
bool AtlasFetch(uint8_t* base, uint32_t tex, uint32_t* phys, uint32_t* pitch_blocks, bool* tiled) {
  if (tex < 0x40000000u || tex >= 0xF0000000u) return false;
  const uint32_t w0 = ReadU32(base, tex + kTexFetch), w1 = ReadU32(base, tex + kTexFetch + 4),
                 w2 = ReadU32(base, tex + kTexFetch + 8);
  const int width = int(w2 & 0x1FFF) + 1, height = int((w2 >> 13) & 0x1FFF) + 1;
  if ((w1 & 0x3F) != kFormatDxt5 || ((w1 >> 6) & 3) != kEndianSwap8In16 || width != kAtlasW || height != kAtlasH)
    return false;
  *phys = w1 & 0xFFFFF000u;
  *tiled = (w0 >> 31) != 0;
  *pitch_blocks = std::max<uint32_t>(((w0 >> 22) & 0x1FF) * 32 / 4, kAtlasW / 4);
  return *phys != 0;
}

bool InGuest(uint32_t a) { return a >= 0x40000000u && a < 0xF0000000u; }

// [acct2-ocr] Where the font's records and UVs are: its own pointers (+8, +0x2C), else right after its header.
uint32_t RecordsOf(uint8_t* base, uint32_t font) {
  const uint32_t p = ReadU32(base, font + kRecordsPtr);
  return InGuest(p) ? p : font + kRecords;
}
uint32_t UvsOf(uint8_t* base, uint32_t font) {
  const uint32_t p = ReadU32(base, font + kUvsPtr);
  return InGuest(p) ? p : font + kUvs;
}

float ReadF32(uint8_t* base, uint32_t at) {
  const uint32_t v = ReadU32(base, at);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}

std::array<int32_t, 7> ReadRecord(uint8_t* base, uint32_t font, uint32_t index) {
  std::array<int32_t, 7> r{};
  const uint32_t recs = RecordsOf(base, font);
  for (int i = 0; i < 7; ++i) r[i] = ReadS32(base, recs + index * kRecordSize + 4 * i);
  return r;
}

// [acct2-ocr] How the font's UVs are scaled: 1/atlas size when its 'W' has UVs below 2 for a box of pixels
// (the loaded font: 0..1), else 1 (pixels, as in the archive).
void UvScale(uint8_t* base, uint32_t font, float* sx, float* sy) {
  const auto w = ReadRecord(base, font, 'W' - 0x20);
  const float u1 = ReadF32(base, UvsOf(base, font) + ('W' - 0x20) * kUvSize + 8);
  const bool unit = w[0] + w[2] > 4 && u1 > 0.0f && u1 < 2.0f;
  *sx = unit ? 1.0f / kAtlasW : 1.0f;
  *sy = unit ? 1.0f / kAtlasH : 1.0f;
}

void WriteRecord(uint8_t* base, uint32_t font, uint32_t index, const std::array<int32_t, 7>& r, float sx, float sy) {
  const uint32_t recs = RecordsOf(base, font), uvs = UvsOf(base, font);
  for (int i = 0; i < 7; ++i) WriteU32(base, recs + index * kRecordSize + 4 * i, uint32_t(r[i]));
  const float uv[4] = {float(r[0]) * sx, float(r[1]) * sy, float(r[0] + r[2]) * sx, float(r[1] + r[3]) * sy};
  for (int i = 0; i < 4; ++i) {
    uint32_t v;
    std::memcpy(&v, &uv[i], 4);
    WriteU32(base, uvs + index * kUvSize + 4 * i, v);
  }
}

// Fills one font (g_mutex held). False: it matched a fill file but failed.
bool Apply(uint8_t* base, uint32_t font, const FontFill& f) {
  uint32_t phys = 0, pitch = 0, tex = 0;
  bool tiled = true;
  for (const uint32_t slot : {kFontTex, 0x0Cu, 0x10u, 0x14u}) {   // [acct2-ocr] +4 first (the loaded FONT)
    const uint32_t t = ReadU32(base, font + slot);
    if (AtlasFetch(base, t, &phys, &pitch, &tiled)) {
      tex = t;
      break;
    }
  }
  if (!tex) {
    REXLOG_WARN("cod3: fonts: {} found, but not its 512x256 DXT5 atlas: left as it is", f.name);
    return false;
  }
  float sx = 1.0f, sy = 1.0f;
  UvScale(base, font, &sx, &sy);
  auto* memory = rex::system::kernel_state()->memory();
  // Through the guest's own view of physical memory, as the game writes it,
  // so the texture cache's write watch sees it.
  uint8_t* atlas = memory->TranslateVirtual<uint8_t*>(0xA0000000u | (phys & 0x1FFFFFFFu));
  for (const Glyph& g : f.glyphs) {
    // Whole blocks from the glyph's top left (placed on a block corner); texels
    // round it in those blocks are 0.
    const int bx0 = g.x / 4, by0 = g.y / 4, bx1 = (g.x + g.w + 3) / 4, by1 = (g.y + g.h + 3) / 4;
    for (int by = by0; by < by1; ++by) {
      for (int bx = bx0; bx < bx1; ++bx) {
        std::array<uint8_t, 16> a{};
        for (int i = 0; i < 16; ++i) {
          const int px = bx * 4 + (i & 3) - g.x, py = by * 4 + (i >> 2) - g.y;
          if (px >= 0 && py >= 0 && px < g.w && py < g.h) a[i] = g.alpha[size_t(py * g.w + px)];
        }
        const uint32_t block = tiled ? TiledBlock(uint32_t(bx), uint32_t(by), pitch) : uint32_t(by) * pitch + uint32_t(bx);
        const auto bytes = Dxt5WhiteBlock(a);
        std::memcpy(atlas + size_t(block) * 16, bytes.data(), 16);
      }
    }
    WriteRecord(base, font, g.code - 0x20, {g.x, g.y, g.w, g.h, g.x_off, g.y_off, g.advance}, sx, sy);
  }
  for (const auto& [code, shown] : f.aliases)
    if (code >= 0x20 && shown >= 0x20 && code < 0x20 + kGlyphs && shown < 0x20 + kGlyphs)
      WriteRecord(base, font, code - 0x20, ReadRecord(base, font, shown - 0x20), sx, sy);
  // [acct2-ocr] Only the top mip level holds the new glyphs, so the atlas is sampled from that level alone
  // (mip_max_level 0, as ui/text_native's pages): scaled-down text would otherwise fade them into the empty
  // smaller levels. The changed fetch also makes the GPU take the texture again, with the new glyphs.
  const uint32_t w4 = ReadU32(base, tex + kTexFetch + 16), w5 = ReadU32(base, tex + kTexFetch + 20);
  WriteU32(base, tex + kTexFetch + 16, w4 & ~(0xFu << 6));
  REXLOG_INFO("cod3: fonts: {}: {} glyphs added to its atlas (font {:08X}, texture {:08X} at {:08X}{}; UVs x{}; "
              "fetch words 4-5 {:08X} {:08X}, now the top mip level only)",
              f.name, f.glyphs.size(), font, tex, phys, tiled ? ", tiled" : "", sx == 1.0f ? "1" : "1/size", w4, w5);
  return true;
}

}  // namespace

void OnFontUsed(uint8_t* base, uint32_t font) {
  if (font < 0x40000000u || font >= 0xF0000000u) return;
  std::lock_guard lock(g_mutex);
  if (std::find(g_seen.begin(), g_seen.end(), font) != g_seen.end()) {
    // [acct2-ocr] A filled font whose first added glyph's record is no longer ours was loaded again (a level
    // load): filled again.
    for (Filled& f : g_filled) {
      if (f.font != font || f.index >= g_fills.size() || g_fills[f.index].glyphs.empty()) continue;
      const Glyph& g = g_fills[f.index].glyphs.front();
      const auto r = ReadRecord(base, font, g.code - 0x20);
      if ((r[0] == g.x && r[1] == g.y && r[2] == g.w && r[3] == g.h) || f.again >= 32) break;
      ++f.again;
      REXLOG_INFO("cod3: fonts: {} (font {:08X}) was loaded again: filled again", g_fills[f.index].name, font);
      if (!Apply(base, font, g_fills[f.index]) && !g_failed.exchange(true)) g_generation.fetch_add(1);
      break;
    }
    return;
  }
  if (g_seen.size() > 64) return;   // fonts are few; something else is passed here
  g_seen.push_back(font);
  if (!REXCVAR_GET(cod3_font_fill)) return;
  if (!g_loaded) LoadFiles();
  if (ReadU32(base, font + kFirstCode) != 0x20 || ReadU32(base, font + kCount) != kGlyphs) return;
  const auto w = ReadRecord(base, font, 'W' - 0x20);
  for (const FontFill& f : g_fills) {
    bool same = ReadS32(base, font + kLineHeight) == f.check[0] && ReadS32(base, font + kAscent) == f.check[1];
    for (int i = 0; same && i < 7; ++i) same = w[size_t(i)] == f.check[size_t(2 + i)];
    if (!same) continue;
    if (Apply(base, font, f))
      g_filled.push_back({font, size_t(&f - g_fills.data()), 0});
    else if (!g_failed.exchange(true))
      g_generation.fetch_add(1);
    return;
  }
}

void SetGameDataRoot(const std::filesystem::path& root) {
  std::lock_guard lock(g_mutex);
  g_root = root;
  g_loaded = false;
}

bool Ready() {
  if (!REXCVAR_GET(cod3_font_fill) || g_failed.load()) return false;
  std::lock_guard lock(g_mutex);
  if (!g_loaded) LoadFiles();
  return !g_fills.empty();
}

uint32_t Generation() { return g_generation.load(); }

}  // namespace cod3::font_fill
