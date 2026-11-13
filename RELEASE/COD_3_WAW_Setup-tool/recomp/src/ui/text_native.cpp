// cod3 - Russian/CJK drawn through the game's own text path (see text_native.h). [acct2-ocr]

#include "ui/text_native.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <climits>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include <imstb_truetype.h>

#include "game/button_prompts.h"  // [acct2-ocr] the key caps, for the overlap check
#include "game/color_codes.h"
#include "game/guest.h"
#include "game/text_language.h"
#include "ui/text_overlay.h"

REXCVAR_DEFINE_BOOL(cod3_text_native, true, "cod3/Language",
                    "Russian, Chinese, Japanese and Korean text drawn inside the game's own text (measured, "
                    "layered and faded like it). Off: drawn on top by our own layer")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_debug_text_native, false, "cod3/Language",
                    "Log the pages the native text makes and the first lines it draws")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [acct2-ocr] 2026-11-05 (owner 11:25 "is the full game translated and ready?"): a way to find what still reaches
// the screen untranslated, level by level.
REXCVAR_DEFINE_BOOL(cod3_debug_text_english, false, "cod3/Language",
                    "Log each line still drawn in English (once per line, up to 2000) while Russian, Chinese, "
                    "Japanese or Korean is chosen: the texts that still need a translation")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace cod3::text_native {

namespace {

using cod3::guest::ReadS32;
using cod3::guest::ReadU32;
using cod3::guest::WriteU32;

// The loaded FONT (NOTES 06:40): +4 texture object, +8 records (28 bytes: x y w h x_off y_off advance),
// +28 line height, +32 ascent, +36 first code, +40 count, +44 UVs (16 bytes: u0 v0 u1 v1, 0..1).
constexpr uint32_t kFontTex = 4, kFontRecs = 8, kFontLine = 28, kFontAscent = 32, kFontFirst = 36,
                   kFontCount = 40, kFontUvs = 44, kFontHeader = 0x38;
constexpr uint32_t kSlots = 224, kRecSize = 28, kUvSize = 16;
// The texture object (0x60 bytes): +0 width, +4 height, +0x14 -> its D3D header at +0x2C, whose fetch
// constant is at +0x48 (dwords 0-5).
constexpr uint32_t kTexSize = 0x60, kTexW = 0, kTexH = 4, kTexHeaderPtr = 0x14, kTexHeader = 0x2C,
                   kTexFetch = 0x48;
constexpr int kPageW = 512, kPageH = 256;
constexpr uint32_t kPageBytes = kPageW * kPageH;   // DXT5: one byte a texel
constexpr float kGameLineHeight = 40.0f;           // what the .overlay.txt em is for

struct Glyph {
  int page = -1, slot = 0;
  int advance = 0;
};

struct Page {
  uint32_t font = 0;     // our FONT (guest)
  uint32_t texels = 0;   // the atlas (guest, physical)
  int used = 1;          // slot 0 is the space
  int shelf_x = 0, shelf_y = 0, shelf_h = 0;
  // Drawn at least once: the GPU has its copy and won't see later writes (runs native-ru2..clone1, NOTES
  // 07:15), so nothing more is written to it; new glyphs go on a new page.
  bool sealed = false;
};

struct Face;

struct FontSet {   // one game font in one locale's face
  Face* face = nullptr;
  uint32_t game = 0;
  int line = 40, ascent = 31;
  float px = 30.0f;   // our em in pixels
  float scale = 0;    // stb's (height)
  float scale_x = 0;  // width: narrower where our font is wider than the game's (Latin/Cyrillic only)
  int space = 8;      // advance of ' '
  std::vector<Page> pages;
  std::unordered_map<uint32_t, Glyph> glyphs;   // code point -> where it is
  size_t next = 0;    // into the face's charset: the next one to put on a new page
};

// [acct2-ocr] 2026-11-05 (owner 12:30: "latin american n characters" on soldier names, prompts and loading cards,
// in several languages): a locale's font file and the pages made from it. Each locale read stays loaded, so a line
// the game still holds from a language chosen before (a name tag, a hint, a card: they change only when the game
// asks again) is drawn with a face that has its characters, not handed to the game font, whose single bytes showed
// UTF-8 Cyrillic as "Ñ" (0xD1) and "Ð", kana as "ã".
struct Face {
  std::string locale;
  std::vector<unsigned char> ttf;
  stbtt_fontinfo info{};
  float em = 0, ascent = 0;
  std::vector<uint32_t> charset;   // this locale's characters (its text pack's and ASCII), most used first
  std::map<uint32_t, std::unique_ptr<FontSet>> sets;
};

std::mutex g_mutex;
std::map<std::string, std::unique_ptr<Face>> g_faces;   // [acct2-ocr] by locale; null: no font file for it
uint32_t g_scratch = 0;   // guest bytes for one run
int g_logged = 0;
// [acct2-ocr] 2026-11-07 (owner 16:57 "review these for potential exploits"): other players' names and chat are drawn
// here too, and each character not seen before takes a slot, 223 to a 128 KB page that is never freed: names or chat
// with many different characters could fill the guest's memory. So at most kMaxSetPages pages for a game font in a
// locale (a pack's whole charset takes 16 at most: zh, 1,994 characters, run acct2-ocr-lang1-all; 8 were the most
// made in any run) and kMaxPages in all; past that, new characters aren't drawn. Characters the face has no glyph
// for share one slot (kNoGlyph).
constexpr size_t kMaxSetPages = 32;
constexpr int kMaxPages = 256;              // 33.5 MB of pages at most
constexpr uint32_t kNoGlyph = 0x110000;     // past Unicode: stb draws the face's "missing" glyph for it
int g_pages = 0;                            // pages made (the ones of a font made again stay in guest memory)
bool g_full_told = false;

uint8_t* Host(uint32_t guest) {
  return rex::system::kernel_state()->memory()->TranslateVirtual<uint8_t*>(guest);
}

// The GPU (physical) address of an allocation from the physical system heap. The 0xE0000000 range sits
// 0x1000 bytes up in physical memory (rex/system/xmemory.h PhysicalHostOffset), so masking alone points 4 KB
// short, into someone else's memory.
uint32_t PhysicalOf(uint32_t guest) {
  return (guest & 0x1FFFFFFFu) + (guest >= 0xE0000000u ? 0x1000u : 0u);
}

// The block offset of a tiled 2D DXT texture: the Xenos layout (the same as rexglue's
// texture_util::GetTiledOffset2D, pitch in blocks, which the GPU detiles with).
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

// One DXT5 block of white with this alpha, in the disc's byte order (Swap8In16), as font_fill writes them.
std::array<uint8_t, 16> Dxt5WhiteBlock(const std::array<uint8_t, 16>& a) {
  const uint8_t hi = *std::max_element(a.begin(), a.end());
  const uint8_t lo = *std::min_element(a.begin(), a.end());
  uint64_t bits = 0;
  if (hi != lo) {
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
  b[8] = b[9] = b[10] = b[11] = 0xFF;
  for (int i = 0; i < 16; i += 2) std::swap(b[i], b[i + 1]);
  return b;
}

int NextCodePoint(std::string_view s, size_t& at) {
  const uint8_t c = uint8_t(s[at++]);
  if (c < 0x80) return c;
  const int extra = c >= 0xF8 ? -1 : c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
  if (extra < 0) return -1;
  int cp = c & (0x3F >> extra);
  for (int i = 0; i < extra; ++i) {
    if (at >= s.size() || (uint8_t(s[at]) & 0xC0) != 0x80) return -1;
    cp = (cp << 6) | (uint8_t(s[at++]) & 0x3F);
  }
  // [acct2-ocr] 2026-11-07 only what UTF-8 allows (other players' names and chat come here too): no overlong forms,
  // surrogates or code points past U+10FFFF
  static constexpr int kLeast[4] = {0, 0x80, 0x800, 0x10000};
  if (cp < kLeast[extra] || cp > 0x10FFFF || (cp >= 0xD800 && cp < 0xE000)) return -1;
  return cp;
}

// [acct2-ocr] The face of `locale`, read the first time (null when the locale has no font file or it can't be
// read; asked once). The old pages stay in guest memory and are used again when the language comes back.
Face* FaceFor(const std::string& locale) {   // g_mutex held
  if (locale.empty()) return nullptr;
  if (auto it = g_faces.find(locale); it != g_faces.end()) return it->second.get();
  std::unique_ptr<Face>& slot = g_faces[locale];
  std::filesystem::path file;
  auto face = std::make_unique<Face>();
  face->locale = locale;
  if (!cod3::text_overlay::FontFile(locale, &file, &face->em, &face->ascent)) return nullptr;
  std::ifstream in(file, std::ios::binary);
  face->ttf.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
  if (face->ttf.size() < 1024 ||
      !stbtt_InitFont(&face->info, face->ttf.data(), stbtt_GetFontOffsetForIndex(face->ttf.data(), 0))) {
    REXLOG_WARN("cod3: native text: {} can't be read: {} stays on our own layer", file.string(), locale);
    return nullptr;
  }
  // Every page is filled when it is made (before the GPU first sees it), so pages hold the characters most
  // likely to come: the ones the locale's text pack uses most.
  std::unordered_map<uint32_t, uint32_t> count;
  for (uint32_t c = 0x21; c < 0x7F; ++c) count[c] = 1u << 30;
  std::ifstream pack(cod3::text::GameDataRoot() / "text" / (locale + ".txt"), std::ios::binary);
  const std::string text((std::istreambuf_iterator<char>(pack)), std::istreambuf_iterator<char>());
  for (size_t at = 0; at < text.size();) {
    const int cp = NextCodePoint(text, at);
    if (cp > 0x20 && cp != 0x7F && stbtt_FindGlyphIndex(&face->info, cp) != 0) ++count[uint32_t(cp)];
  }
  for (const auto& [c, n] : count) face->charset.push_back(c);
  std::sort(face->charset.begin(), face->charset.end(), [&](uint32_t a, uint32_t b) {
    return count[a] != count[b] ? count[a] > count[b] : a < b;
  });
  REXLOG_INFO("cod3: native text: {} from {} ({} characters)", locale, file.filename().string(),
              face->charset.size());
  slot = std::move(face);
  return slot.get();
}

FontSet* SetFor(uint8_t* base, Face* face, uint32_t game) {   // g_mutex held
  if (auto it = face->sets.find(game); it != face->sets.end()) {
    // [acct2-ocr] 2026-11-07 Still the same game font at this address? A level load can put another one there (another
    // size): its lines would be drawn at the old font's size and on its baseline. Then the set is made again (the old
    // pages stay in guest memory, unused).
    FontSet* set = it->second.get();
    if (ReadS32(base, game + kFontLine) == set->line && ReadS32(base, game + kFontAscent) == set->ascent) return set;
    REXLOG_INFO("cod3: native text: font {:08X} is another font now (line {} -> {}): {} pages made again", game,
                set->line, ReadS32(base, game + kFontLine), face->locale);
    face->sets.erase(it);
  }
  if (ReadU32(base, game + kFontFirst) != 0x20 || ReadU32(base, game + kFontCount) != kSlots) return nullptr;
  const uint32_t tex = ReadU32(base, game + kFontTex);
  if (tex < 0x40000000u || tex >= 0xF0000000u) return nullptr;
  if (int(ReadS32(base, tex + kTexW)) != kPageW || int(ReadS32(base, tex + kTexH)) != kPageH) return nullptr;
  auto set = std::make_unique<FontSet>();
  set->face = face;
  set->game = game;
  set->line = ReadS32(base, game + kFontLine);
  set->ascent = ReadS32(base, game + kFontAscent);
  const std::string& locale = face->locale;
  set->px = face->em * float(set->line) / kGameLineHeight;
  set->scale = stbtt_ScaleForMappingEmToPixels(&face->info, set->px);
  int adv = 0, lsb = 0;
  stbtt_GetCodepointHMetrics(&face->info, ' ', &adv, &lsb);
  // The game's fonts are narrow: match our capitals' mean advance to theirs (ru: Noto Sans comes out ~1.5x
  // as wide, run native-ru7/en2), within 75-100%. Chinese/Japanese/Korean keep their square glyphs.
  set->scale_x = set->scale;
  if (locale.rfind("zh", 0) != 0 && locale.rfind("ja", 0) != 0 && locale.rfind("ko", 0) != 0) {
    const uint32_t recs = ReadU32(base, game + kFontRecs);
    float theirs = 0, ours = 0;
    for (uint32_t c = 'A'; c <= 'Z'; ++c) {
      int a = 0, l = 0;
      stbtt_GetCodepointHMetrics(&face->info, int(c), &a, &l);
      ours += float(a) * set->scale;
      theirs += float(ReadS32(base, recs + (c - 0x20) * kRecSize + 24));
    }
    if (ours > 0 && theirs > 0) set->scale_x = set->scale * std::clamp(theirs / ours, 0.75f, 1.0f);
    if (REXCVAR_GET(cod3_debug_text_native))
      REXLOG_INFO("cod3: native text: font {:08X}: capitals {:.1f} px on average, ours {:.1f}: width x{:.2f}", game,
                  theirs / 26, ours / 26, set->scale_x / set->scale);
  }
  set->space = std::max(1, int(std::lround(float(adv) * set->scale_x)));
  if (REXCVAR_GET(cod3_debug_text_native)) {
    const uint32_t recs = ReadU32(base, game + kFontRecs), uvs = ReadU32(base, game + kFontUvs);
    for (const char c : {'S', 'A', '!'}) {
      const uint32_t i = uint32_t(c) - 0x20;
      float uv[4];
      for (int k = 0; k < 4; ++k) {
        const uint32_t v = ReadU32(base, uvs + i * kUvSize + 4 * uint32_t(k));
        std::memcpy(&uv[k], &v, 4);
      }
      REXLOG_INFO("cod3: native text: game font {:08X} '{}': rec {} {} {} {} {} {} {}, uv {} {} {} {}", game, c,
                  ReadS32(base, recs + i * kRecSize), ReadS32(base, recs + i * kRecSize + 4),
                  ReadS32(base, recs + i * kRecSize + 8), ReadS32(base, recs + i * kRecSize + 12),
                  ReadS32(base, recs + i * kRecSize + 16), ReadS32(base, recs + i * kRecSize + 20),
                  ReadS32(base, recs + i * kRecSize + 24), uv[0], uv[1], uv[2], uv[3]);
    }
  }
  FontSet* raw = set.get();
  face->sets[game] = std::move(set);
  return raw;
}

// A new page for `set`: our FONT (the game font's header, our records and UVs) and texture object (the game
// font's, pointing at our atlas, mips off). g_mutex held.
bool NewPage(uint8_t* base, FontSet* set) {
  if (set->pages.size() >= kMaxSetPages || g_pages >= kMaxPages) {   // [acct2-ocr] 2026-11-07
    if (!g_full_told) {
      g_full_told = true;
      REXLOG_WARN("cod3: native text: no more glyph pages ({} for font {:08X}, {} in all): characters not drawn yet "
                  "are left out", set->pages.size(), set->game, g_pages);
    }
    return false;
  }
  auto* mem = rex::system::kernel_state()->memory();
  const uint32_t game_tex = ReadU32(base, set->game + kFontTex);
  const uint32_t font = mem->SystemHeapAlloc(kFontHeader + kSlots * (kRecSize + kUvSize));
  const uint32_t tex = mem->SystemHeapAlloc(kTexSize, 0x40);
  const uint32_t texels = mem->SystemHeapAlloc(kPageBytes, 0x1000, rex::memory::kSystemHeapPhysical);
  if (!font || !tex || !texels) {
    for (const uint32_t a : {font, tex, texels})   // [acct2-ocr] 2026-11-07 the parts that were had back (asked again)
      if (a) mem->SystemHeapFree(a);
    REXLOG_WARN("cod3: native text: no guest memory for a page; these lines stay as they are");
    return false;
  }
  ++g_pages;   // [acct2-ocr]
  std::memcpy(base + font, base + set->game, kFontHeader);
  const uint32_t recs = font + kFontHeader, uvs = recs + kSlots * kRecSize;
  std::memset(base + recs, 0, kSlots * (kRecSize + kUvSize));
  WriteU32(base, font + kFontTex, tex);
  WriteU32(base, font + kFontRecs, recs);
  WriteU32(base, font + kFontUvs, uvs);
  WriteU32(base, recs + 24, uint32_t(set->space));   // slot 0: ' '
  std::memcpy(base + tex, base + game_tex, kTexSize);
  WriteU32(base, tex + kTexHeaderPtr, tex + kTexHeader);
  const uint32_t w1 = ReadU32(base, tex + kTexFetch + 4), w4 = ReadU32(base, tex + kTexFetch + 16);
  WriteU32(base, tex + kTexFetch + 4, (PhysicalOf(texels) & 0xFFFFF000u) | (w1 & 0xFFFu));
  WriteU32(base, tex + kTexFetch + 16, w4 & ~(0xFu << 6));   // mip_max_level 0: our page has no mips
  std::memset(Host(texels), 0, kPageBytes);
  Page page;
  page.font = font;
  page.texels = texels;
  set->pages.push_back(page);
  if (REXCVAR_GET(cod3_debug_text_native))
    REXLOG_INFO("cod3: native text: page {} for font {:08X} (line {}, {:.1f} px): font {:08X}, texture {:08X}, "
                "texels {:08X}",
                set->pages.size(), set->game, set->line, set->px, font, tex, texels);
  return true;
}

// Puts `cp` on `page` (the last of `set`, not yet drawn). False when it has no room left. g_mutex held.
bool Place(uint8_t* base, FontSet* set, Page* page, uint32_t cp) {
  // [acct2-ocr] 2026-11-07 The slot whose byte is '^' stays empty: a run starting with it, then a digit's slot, would
  // be taken by the game for a color code (its own "^N" at a line's start) and two of our characters would vanish.
  if (page->used == int('^' - 0x20)) ++page->used;
  if (page->used >= int(kSlots)) return false;
  int adv = 0, lsb = 0, x0 = 0, y0 = 0, x1 = 0, y1 = 0;
  const stbtt_fontinfo* info = &set->face->info;
  stbtt_GetCodepointHMetrics(info, int(cp), &adv, &lsb);
  stbtt_GetCodepointBitmapBox(info, int(cp), set->scale_x, set->scale, &x0, &y0, &x1, &y1);
  int gw = std::max(0, x1 - x0), gh = std::max(0, y1 - y0);
  // [acct2-ocr] 2026-11-07 a glyph larger than a page (with its texel of air) would be written past the page: blank
  if (gw + 2 > kPageW || gh + 2 > kPageH) gw = gh = 0;
  // Whole 4x4 blocks with a texel of air round the glyph: no two glyphs share a block.
  const int cw = (gw + 2 + 3) & ~3, ch = (gh + 2 + 3) & ~3;
  int px = page->shelf_x, py = page->shelf_y;
  if (px + cw > kPageW) {
    px = 0;
    py += page->shelf_h;
  }
  if (py + ch > kPageH) return false;
  if (px == 0 && page->shelf_x != 0) page->shelf_h = 0;
  page->shelf_x = px + cw;
  page->shelf_y = py;
  page->shelf_h = std::max(page->shelf_h, ch);
  const int slot = page->used++;
  Glyph g;
  g.page = int(set->pages.size()) - 1;
  g.slot = slot;
  g.advance = std::max(0, int(std::lround(float(adv) * set->scale_x)));
  if (gw > 0 && gh > 0) {
    std::vector<uint8_t> alpha(size_t(cw) * size_t(ch), 0);
    stbtt_MakeCodepointBitmap(info, alpha.data() + cw + 1, gw, gh, cw, set->scale_x, set->scale, int(cp));
    uint8_t* atlas = Host(page->texels);
    constexpr uint32_t kPitchBlocks = kPageW / 4;
    for (int by = 0; by < ch / 4; ++by) {
      for (int bx = 0; bx < cw / 4; ++bx) {
        std::array<uint8_t, 16> a{};
        for (int i = 0; i < 16; ++i) a[size_t(i)] = alpha[size_t((by * 4 + (i >> 2)) * cw + bx * 4 + (i & 3))];
        const auto bytes = Dxt5WhiteBlock(a);
        const uint32_t block = TiledBlock(uint32_t(px / 4 + bx), uint32_t(py / 4 + by), kPitchBlocks);
        std::memcpy(atlas + size_t(block) * 16, bytes.data(), 16);
      }
    }
  }
  // The record: the cell (with its air), placed so the glyph sits on the game font's baseline.
  const uint32_t recs = ReadU32(base, page->font + kFontRecs), uvs = ReadU32(base, page->font + kFontUvs);
  const int rec[7] = {px, py, cw, ch, x0 - 1, set->ascent + y0 - 1, g.advance};
  for (int i = 0; i < 7; ++i) WriteU32(base, recs + uint32_t(slot) * kRecSize + 4 * uint32_t(i), uint32_t(rec[i]));
  // UVs are 0..1 (the game font's 'S', 22x34 at 161,120: 0.3145 0.4688 0.3574 0.6016; run native-uv1).
  const float uv[4] = {float(px) / kPageW, float(py) / kPageH, float(px + cw) / kPageW, float(py + ch) / kPageH};
  for (int i = 0; i < 4; ++i) {
    uint32_t v;
    std::memcpy(&v, &uv[i], 4);
    WriteU32(base, uvs + uint32_t(slot) * kUvSize + 4 * uint32_t(i), v);
  }
  set->glyphs[cp] = g;
  return true;
}

// Where `cp` is drawn from in `set`. A glyph not made yet goes on the last page if it hasn't been drawn,
// else on a new page, which is then filled up with the locale's next characters. g_mutex held.
const Glyph* GlyphFor(uint8_t* base, FontSet* set, uint32_t cp) {
  if (auto it = set->glyphs.find(cp); it != set->glyphs.end()) return &it->second;
  // [acct2-ocr] 2026-11-07 a character the face has no glyph for: its one "missing" glyph, in one slot for them all
  if (cp != kNoGlyph && stbtt_FindGlyphIndex(&set->face->info, int(cp)) == 0) return GlyphFor(base, set, kNoGlyph);
  if (set->pages.empty() || set->pages.back().sealed || !Place(base, set, &set->pages.back(), cp)) {
    if (!NewPage(base, set) || !Place(base, set, &set->pages.back(), cp)) return nullptr;
    Page* page = &set->pages.back();
    const std::vector<uint32_t>& charset = set->face->charset;
    for (; set->next < charset.size(); ++set->next) {
      const uint32_t c = charset[set->next];
      if (!set->glyphs.count(c) && !Place(base, set, page, c)) break;
    }
    if (REXCVAR_GET(cod3_debug_text_native))
      REXLOG_INFO("cod3: native text: page {} for font {:08X} holds {} glyphs ({} of {} characters done)",
                  set->pages.size(), set->game, page->used - 1, set->next, charset.size());
  }
  return &set->glyphs[cp];
}


// Valid UTF-8 with a character past Latin-1, and no control bytes but '\n'.
bool IsOurs(std::string_view s) {
  bool beyond = false;
  for (size_t at = 0; at < s.size();) {
    const int cp = NextCodePoint(s, at);
    if (cp < 0 || (cp < 0x20 && cp != '\n')) return false;
    beyond |= cp >= 0x100;
  }
  return beyond;
}

// [acct2-ocr] 2026-11-05 (owner 08:36: the green Russian text "shows a brief box before the character shows"): the
// game's typing effect hands over lines cut by bytes, so a character's first byte comes alone (its other bytes a
// moment later) and the line is no longer valid UTF-8: it went to the game font, which drew that byte as a box.
// The cut bytes at either end are left out (a lead byte at the end missing its continuation bytes; continuation
// bytes at the start); `partial` is set when the line held nothing else.
std::string_view WithoutCutEnds(std::string_view s, bool* partial) {
  *partial = false;
  size_t b = 0, e = s.size();
  while (b < e && b < 3 && (uint8_t(s[b]) & 0xC0) == 0x80) ++b;
  size_t k = e;
  int cont = 0;
  while (k > b && cont < 3 && (uint8_t(s[k - 1]) & 0xC0) == 0x80) --k, ++cont;
  if (k > b) {
    const uint8_t c = uint8_t(s[k - 1]);
    const int need = c >= 0xF0 ? 3 : c >= 0xE0 ? 2 : c >= 0xC0 ? 1 : -1;
    if (need > cont) e = k - 1;
  }
  if (b == 0 && e == s.size()) return s;
  *partial = b >= e;
  return s.substr(b, b < e ? e - b : 0);
}

// [acct2-ocr] The line to draw or measure here: ours as it is, or ours once its cut ends are left out, or nothing
// but a cut character (`partial`). False: the game's own (Latin-1 "Café" stays whole).
bool OursToDraw(const std::string& whole, std::string* s, bool* partial) {
  const std::string_view cut = WithoutCutEnds(whole, partial);
  if (*partial) return true;
  if (!IsOurs(cut)) return false;
  *s = std::string(cut);
  return true;
}

// The code points to draw: text_language's line-break spaces between CJK characters left out.
std::vector<uint32_t> Shown(std::string_view s) {
  std::vector<uint32_t> out;
  for (size_t at = 0; at < s.size();) {
    const int cp = NextCodePoint(s, at);
    if (cp < 0) break;
    if (cp == ' ' && !out.empty() && cod3::text::IsCjk(char32_t(out.back()))) {
      size_t next = at;
      const int after = next < s.size() ? NextCodePoint(s, next) : -1;
      if (after >= 0 && cod3::text::IsCjk(char32_t(after))) continue;
    }
    out.push_back(uint32_t(cp));
  }
  return out;
}

// [acct2-ocr] The locale whose font a character's script needs, for a line no loaded face can draw (one still held
// from a language chosen before any line of it was drawn).
const char* LocaleForScript(uint32_t cp) {
  if (cp >= 0x0400 && cp < 0x0530) return "ru-RU";
  if ((cp >= 0xAC00 && cp < 0xD7B0) || (cp >= 0x1100 && cp < 0x1200) || (cp >= 0x3130 && cp < 0x3190)) return "ko-KR";
  if ((cp >= 0x3040 && cp < 0x3100) || (cp >= 0x31F0 && cp < 0x3200) || (cp >= 0xFF66 && cp < 0xFFA0)) return "ja-JP";
  if ((cp >= 0x3000 && cp < 0x3040) || (cp >= 0x3400 && cp < 0xA000) || (cp >= 0xF900 && cp < 0xFB00) ||
      (cp >= 0xFF00 && cp < 0xFFF0))
    return "zh-Hans";
  return nullptr;
}

// [acct2-ocr] How many of these characters `face` has no glyph for.
int Missing(const Face* face, const std::vector<uint32_t>& wide) {
  int n = 0;
  for (uint32_t cp : wide) n += stbtt_FindGlyphIndex(&face->info, int(cp)) == 0 ? 1 : 0;
  return n;
}

// [acct2-ocr] The face to draw `line` with: the chosen locale's when it has all the line's characters past Latin-1
// (the usual case); else the loaded face that lacks the fewest; else the one the line's script needs. Null: none.
Face* FaceForLine(std::string_view line) {   // g_mutex held
  std::vector<uint32_t> wide;
  for (size_t at = 0; at < line.size() && wide.size() < 64;) {
    const int cp = NextCodePoint(line, at);
    if (cp >= 0x100 && std::find(wide.begin(), wide.end(), uint32_t(cp)) == wide.end()) wide.push_back(uint32_t(cp));
  }
  Face* const chosen = FaceFor(cod3::text::Chosen());
  Face* best = chosen;
  int least = chosen ? Missing(chosen, wide) : INT_MAX;
  if (least == 0) return chosen;
  for (const auto& [locale, face] : g_faces) {
    if (!face || face.get() == chosen) continue;
    const int m = Missing(face.get(), wide);
    if (m < least) best = face.get(), least = m;
    if (m == 0) return best;
  }
  if (const char* locale = wide.empty() ? nullptr : LocaleForScript(wide.front())) {
    if (Face* face = FaceFor(locale); face && face != chosen && Missing(face, wide) < least) best = face;
  }
  return best;
}

bool Ready(uint8_t* base, uint32_t font, std::string_view line, FontSet** set) {   // g_mutex held
  if (!REXCVAR_GET(cod3_text_native)) return false;
  if (font < 0x40000000u || font >= 0xF0000000u) return false;
  Face* const face = FaceForLine(line);
  if (!face) return false;
  *set = SetFor(base, face, font);
  return *set != nullptr;
}

}  // namespace

bool Active(std::string_view locale) {
  if (!REXCVAR_GET(cod3_text_native) || locale.empty()) return false;
  // [acct2-ocr] 2026-11-07 Each locale's answer kept (the files are looked at once per locale). It was the last
  // locale's only: with F2's Text list open (it asks for all six) the text layer's ask for the chosen one read the
  // font files again for every line drawn.
  static std::mutex mutex;
  static std::map<std::string, bool, std::less<>> known;
  {
    std::lock_guard lock(mutex);
    if (const auto it = known.find(locale); it != known.end()) return it->second;
  }
  std::filesystem::path file;
  float em = 0, ascent = 0;
  const bool ok = cod3::text_overlay::FontFile(locale, &file, &em, &ascent);
  std::lock_guard lock(mutex);
  known[std::string(locale)] = ok;
  return ok;
}

// [acct2-ocr] cod3_debug_text_english: a line the game font draws with a word of three Latin letters or more, while
// a script of ours is chosen (a Latin-language line can't be told from English, so those languages aren't looked at).
void NoteEnglish(const std::string& s) {
  if (!REXCVAR_GET(cod3_debug_text_english)) return;
  int run = 0;
  bool word = false;
  for (unsigned char c : s) {
    run = std::isalpha(c) ? run + 1 : 0;
    if (run >= 3) {
      word = true;
      break;
    }
  }
  if (!word) return;
  const std::string chosen = cod3::text::Chosen();
  if (chosen.empty() || !Active(chosen)) return;
  static std::mutex mutex;
  static std::unordered_map<std::string, int> seen;
  std::lock_guard lock(mutex);
  if (seen.size() >= 2000 || seen[s]++ != 0) return;
  REXLOG_INFO("cod3: text still English ({}): \"{}\"", chosen, s);
}

// [acct2-ocr] 2026-11-05 (owner 12:30: "I saw some latin american n characters"): with cod3_debug_text_english, a
// line that reaches the game font with bytes past ASCII while a script of ours is chosen: our UTF-8 there shows as
// "Ã‘"-style letters (Ñ is 0xD1, the first byte of most Cyrillic). `why`: not valid as ours, or no page for its font.
void NoteNotOurs(const std::string& s, const char* why, uint32_t font, bool any_locale) {
  if (!REXCVAR_GET(cod3_debug_text_english)) return;
  bool high = false;
  for (unsigned char c : s) high |= c >= 0x80;
  if (!high) return;
  const std::string chosen = cod3::text::Chosen();
  if (!any_locale && (chosen.empty() || !Active(chosen))) return;  // Latin languages' lines have such bytes
  static std::mutex mutex;
  static std::unordered_map<std::string, int> seen;
  std::lock_guard lock(mutex);
  if (seen.size() >= 500 || seen[s]++ != 0) return;
  std::string shown;
  for (unsigned char c : s.substr(0, 200)) {
    if (c >= 0x20 && c < 0x7F) {
      shown += char(c);
    } else {
      char hex[8];
      std::snprintf(hex, sizeof hex, "\\x%02X", c);
      shown += hex;
    }
  }
  REXLOG_INFO("cod3: text not drawn as ours ({}, {}, font {:08X}): \"{}\"", chosen, why, font, shown);
}

bool DrawLine(uint8_t* base, PPCContext& ctx, DrawFn draw) {
  const uint32_t font = ctx.r3.u32, text = ctx.r4.u32;
  if (text < 0x40000000u || text >= 0xF0000000u) return false;
  std::string s;
  bool partial = false;
  if (const std::string whole = cod3::guest::ReadString(base, text, 1024); !OursToDraw(whole, &s, &partial)) {
    NoteEnglish(whole);
    NoteNotOurs(whole, "not valid", font, false);  // [acct2-ocr]
    return false;
  }
  std::lock_guard lock(g_mutex);
  FontSet* set = nullptr;
  if (!Ready(base, font, s, &set)) {
    NoteNotOurs(s, "no page", font, true);  // [acct2-ocr]
    return false;
  }
  // [acct2-ocr] a line still held from a language chosen before (cod3_debug_text_english / _native: once each)
  if (const std::string chosen = cod3::text::Chosen();
      !s.empty() && set->face->locale != chosen &&
      (REXCVAR_GET(cod3_debug_text_english) || REXCVAR_GET(cod3_debug_text_native))) {
    static std::unordered_map<std::string, int> held;
    if (held.size() < 200 && held[s]++ == 0)
      REXLOG_INFO("cod3: native text: a held {} line drawn while {} is chosen: \"{}\"", set->face->locale,
                  chosen.empty() ? std::string("the game's own text") : chosen, s);
  }
  if (REXCVAR_GET(cod3_debug_text_native)) {   // [acct2-ocr] how often the game hands over a cut line
    static int cut_lines = 0;
    const uint32_t whole = uint32_t(cod3::guest::ReadString(base, text, 1024).size());
    if (whole != s.size() && cut_lines < 40 && (++cut_lines <= 10 || cut_lines % 10 == 0))
      REXLOG_INFO("cod3: native text: a cut line ({} bytes, {} drawn): \"{}\"", whole, s.size(), s);
  }
  if (partial) return true;  // [acct2-ocr] only the first byte(s) of a character so far: nothing to draw yet
  if (!g_scratch) g_scratch = rex::system::kernel_state()->memory()->SystemHeapAlloc(4096);
  if (!g_scratch) return false;
  const float x0 = float(ctx.f1.f64), sx = float(ctx.f4.f64), sy = float(ctx.f5.f64);
  const uint32_t line_color = ctx.r8.u32, alpha = line_color & 0xFF000000u;
  float x = x0, y = float(ctx.f2.f64);
  int page = -1;
  uint32_t color = line_color;
  std::string run;
  float run_x = x;
  auto flush = [&]() {
    if (run.empty() || page < 0) return;
    if (run.size() > 4000) run.resize(4000);
    std::memcpy(base + g_scratch, run.data(), run.size());
    base[g_scratch + run.size()] = 0;
    PPCContext c = ctx;
    c.r3.u64 = set->pages[size_t(page)].font;
    c.r4.u64 = g_scratch;
    c.f1.f64 = run_x;
    c.f2.f64 = y;
    c.r8.u64 = color;
    set->pages[size_t(page)].sealed = true;
    draw(c, base);
    run.clear();
  };
  namespace cc = cod3::color_codes;
  cc::ForEachRun(s, [&](std::string_view piece, int code) {
    uint32_t want = line_color;
    if (code >= 0 && code < 10) {
      const uint32_t rgba = cc::kCodeRgba[code];   // 0xRRGGBBAA
      want = alpha | (rgba >> 8);
    }
    for (uint32_t cp : Shown(piece)) {
      if (cp == '\n') {
        flush();
        x = x0;
        y += float(set->line) * sy;
        continue;
      }
      int slot = 0, gpage = page, advance = set->space;
      if (cp != ' ') {
        const Glyph* g = GlyphFor(base, set, cp);
        if (!g) continue;
        slot = g->slot, gpage = g->page, advance = g->advance;
      }
      if (gpage < 0) gpage = 0;
      if (set->pages.empty() && !NewPage(base, set)) return;
      if (gpage != page || want != color) {
        flush();
        page = gpage;
        color = want;
        run_x = x;
      }
      run.push_back(char(0x20 + slot));
      x += float(advance) * sx;
    }
  });
  flush();
  // [acct2-ocr] 2026-11-05 (owner 07:46: "double check the keys don't overlap with the text"): each line once,
  // where our text ends against the key caps on its row (the caps are laid out from the game's measure of the text,
  // ours from our advances: they should meet, not cross).
  if (REXCVAR_GET(cod3_debug_text_native)) {
    static std::unordered_map<std::string, int> seen;
    // the 10th draw: the caps of a line are noted as it is drawn, so the first draws may not have them yet
    if (seen.size() < 400 && ++seen[s] == 10) {
      const float y0 = float(ctx.f2.f64), h = float(set->line) * sy;
      std::string near;
      for (const auto& cap : cod3::prompts::CurrentKeyCaps(250)) {
        if (cap.y1 < y0 - 2.0f * h || cap.y0 > y0 + 2.0f * h) continue;  // another row
        char buf[160];
        const float gap_after = cap.x0 - x, gap_before = x0 - cap.x1;   // > 0: apart
        std::snprintf(buf, sizeof(buf), " [%s %.1f-%.1f y %.1f-%.1f: %s %.1f]", cap.name.c_str(), cap.x0, cap.x1,
                      cap.y0, cap.y1, cap.x0 >= x0 ? "after text end by" : "before text start by",
                      cap.x0 >= x0 ? gap_after : gap_before);
        near += buf;
      }
      REXLOG_INFO("cod3: native text layout: \"{}\" x {:.1f}-{:.1f} y {:.1f} (line {:.1f}){}", s, x0, x, y0, h,
                  near.empty() ? std::string(" no caps near") : near);
    }
  }
  if (REXCVAR_GET(cod3_debug_text_native) && g_logged < 30) {
    ++g_logged;
    REXLOG_INFO("cod3: native text: \"{}\" at {:.1f},{:.1f} scale {:.3f}/{:.3f} font {:08X}: {} pages, {} glyphs", s,
                x0, float(ctx.f2.f64), sx, sy, font, set->pages.size(), set->glyphs.size());
  }
  return true;
}

// [acct2-ocr] The width the game measures a line at, for our lines: from the same advances.
bool Measure(uint8_t* base, uint32_t font, uint32_t text, float sx, float sy, uint32_t width_at, uint32_t height_at) {
  if (text < 0x40000000u || text >= 0xF0000000u) return false;
  std::string s;
  bool partial = false;  // [acct2-ocr] measured as drawn: cut ends left out, a lone cut character 0 wide
  if (!OursToDraw(cod3::guest::ReadString(base, text, 1024), &s, &partial)) return false;
  std::lock_guard lock(g_mutex);
  FontSet* set = nullptr;
  if (!Ready(base, font, s, &set)) return false;
  float widest = 0, x = 0;
  int lines = 1;
  namespace cc = cod3::color_codes;
  cc::ForEachRun(s, [&](std::string_view piece, int) {
    for (uint32_t cp : Shown(piece)) {
      if (cp == '\n') {
        widest = std::max(widest, x);
        x = 0;
        ++lines;
        continue;
      }
      if (cp == ' ') {
        x += float(set->space) * sx;
        continue;
      }
      if (const Glyph* g = GlyphFor(base, set, cp)) x += float(g->advance) * sx;
    }
  });
  widest = std::max(widest, x);
  if (width_at >= 0x40000000u && width_at < 0xF0000000u) WriteU32(base, width_at, uint32_t(int32_t(widest)));
  if (height_at >= 0x40000000u && height_at < 0xF0000000u)
    WriteU32(base, height_at, uint32_t(int32_t(float(set->line * lines) * sy)));
  return true;
}

}  // namespace cod3::text_native

// The game's line measure: our lines measured from our advances (text_native.h). The line layout measures each
// piece of a line with it, so the key caps (button_prompts.cpp) land where our text ends.
// [acct2-ocr] 2026-11-05 cod3mp too (its 0x8214CA50, the same code: mp/config/campaign_map.tsv), so its caps line up.
REX_EXTERN(__imp__sub_82134B90);
REX_HOOK_RAW(sub_82134B90) {
  // [acct2-ocr] 2026-11-05 a raw English line measured as ours, as nglListAddString's hook draws it (text_language.h)
  if (const uint32_t ours = cod3::text::RawTextFor(base, ctx.r4.u32)) ctx.r4.u64 = ours;
  if (cod3::text_native::Measure(base, ctx.r3.u32, ctx.r4.u32, float(ctx.f1.f64), float(ctx.f2.f64), ctx.r5.u32,
                                 ctx.r6.u32))
    return;
  __imp__sub_82134B90(ctx, base);
}
