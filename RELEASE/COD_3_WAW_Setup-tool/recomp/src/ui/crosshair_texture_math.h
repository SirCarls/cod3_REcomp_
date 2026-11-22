// cod3 - the sums behind the sharp crosshair (see crosshair_texture.h): no SDK, no guest memory, so they are tested
// on their own (dev/agents/acct2-ocr/crosshair-texture). [acct2-ocr] 2026-11-21 task 125
//
// The game's picture as the GPU draws it: its texels' values at their centres ((i + 0.5) / width), filtered
// bilinearly, the texture's own address mode at its edges. Ours keeps exactly the points where that drawing is at
// least half the picture's strongest alpha (its "edge"), each with the value the game's drawing has there from the
// texels at that strength or more: inside, where the four texels it is drawn from are all at least half, that is the
// game's own value; at the edge the strength stays up to the edge instead of fading out over two texels. Antialiased
// by averaging 8 x 8 points in each of our texels. So the shape is the game's own (its half-strength outline), only
// with a sharp edge, at any size; a picture whose sharp copy would lose something (more than one color, a faint part
// away from the shape: a glow, a shadow) is left as it is (Check).

#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace cod3::crosshair_texture::math {

struct Rgba {
  uint8_t r = 0, g = 0, b = 0, a = 0;
};

// The Xenos 2D tiling (Xenia's TiledOffset2DOuter and Inner, as game/ngl_side.cpp and ui/text_native.cpp read and
// write it): the index of block (x, y) of a surface `pitch_blocks` wide (rounded up to 32 here), blocks of
// 2^log2_bytes bytes.
inline uint32_t TiledBlock(uint32_t x, uint32_t y, uint32_t pitch_blocks, uint32_t log2_bytes) {
  const int32_t width = static_cast<int32_t>((pitch_blocks + 31) & ~31u);
  const int32_t yi = static_cast<int32_t>(y), xi = static_cast<int32_t>(x), lb = static_cast<int32_t>(log2_bytes);
  const int32_t macro_y = ((yi >> 5) * (width >> 5)) << (lb + 7);
  const int32_t micro_y = ((yi & 6) << 2) << lb;
  const int32_t row = macro_y + ((micro_y & ~0xF) << 1) + (micro_y & 0xF) + ((yi & 8) << (3 + lb)) + ((yi & 1) << 4);
  const int32_t macro_x = (xi >> 5) << (lb + 7);
  const int32_t micro_x = (xi & 7) << lb;
  const int32_t offset = row + (macro_x + ((micro_x & ~0xF) << 1) + (micro_x & 0xF));
  const int32_t bytes = ((offset & ~0x1FF) << 3) + ((offset & 0x1C0) << 2) + (offset & 0x3F) + ((yi & 16) << 7) +
                        (((((yi & 8) >> 2) + (xi >> 3)) & 3) << 6);
  return static_cast<uint32_t>(bytes) >> log2_bytes;
}

// A block's bytes in the texture's byte order (the fetch constant's endian: 1 = 8 in 16, 2 = 8 in 32, 3 = 16 in 32)
// to little-endian, or back (each is its own inverse).
inline void SwapEndian(uint8_t* b, uint32_t bytes, uint32_t endian) {
  for (uint32_t i = 0; i + 3 < bytes; i += 4) {
    uint8_t* w = b + i;
    if (endian == 1) {
      std::swap(w[0], w[1]);
      std::swap(w[2], w[3]);
    } else if (endian == 2) {
      std::swap(w[0], w[3]);
      std::swap(w[1], w[2]);
    } else if (endian == 3) {
      std::swap(w[0], w[2]);
      std::swap(w[1], w[3]);
    }
  }
}

inline uint8_t Expand5(uint32_t v) { return static_cast<uint8_t>(v * 255 / 31); }
inline uint8_t Expand6(uint32_t v) { return static_cast<uint8_t>(v * 255 / 63); }

// The 5:6:5 color whose expansion (as the decoders here and in ngl_side expand it) is exactly `c`; false when none is.
inline bool Exact565(const Rgba& c, uint16_t* out) {
  for (uint32_t r = 0; r < 32; ++r) {
    if (Expand5(r) != c.r) continue;
    for (uint32_t g = 0; g < 64; ++g) {
      if (Expand6(g) != c.g) continue;
      for (uint32_t b = 0; b < 32; ++b) {
        if (Expand5(b) != c.b) continue;
        *out = static_cast<uint16_t>(r << 11 | g << 5 | b);
        return true;
      }
    }
  }
  return false;
}

// Formats read (Xenos): 18 DXT1, 19 DXT2_3, 20 DXT4_5 and their AS_16_16_16_16 kinds (51-53), whose blocks are the same.
inline uint32_t BaseFormat(uint32_t format) {
  if (format == 51) return 18;
  if (format == 52) return 19;
  if (format == 53) return 20;
  return format;
}
inline bool Readable(uint32_t format) {
  const uint32_t f = BaseFormat(format);
  return f == 18 || f == 19 || f == 20;
}
inline uint32_t BlockBytes(uint32_t format) { return BaseFormat(format) == 18 ? 8 : 16; }

// One block (little-endian) into 16 texels, row by row.
inline void DecodeBlock(const uint8_t* b, uint32_t format, Rgba out[16]) {
  format = BaseFormat(format);
  const uint8_t* color = format == 18 ? b : b + 8;
  const uint16_t c0 = static_cast<uint16_t>(color[0] | color[1] << 8), c1 = static_cast<uint16_t>(color[2] | color[3] << 8);
  Rgba pal[4];
  pal[0] = {Expand5(c0 >> 11), Expand6((c0 >> 5) & 63), Expand5(c0 & 31), 255};
  pal[1] = {Expand5(c1 >> 11), Expand6((c1 >> 5) & 63), Expand5(c1 & 31), 255};
  if (format != 18 || c0 > c1) {
    pal[2] = {static_cast<uint8_t>((2 * pal[0].r + pal[1].r) / 3), static_cast<uint8_t>((2 * pal[0].g + pal[1].g) / 3),
              static_cast<uint8_t>((2 * pal[0].b + pal[1].b) / 3), 255};
    pal[3] = {static_cast<uint8_t>((pal[0].r + 2 * pal[1].r) / 3), static_cast<uint8_t>((pal[0].g + 2 * pal[1].g) / 3),
              static_cast<uint8_t>((pal[0].b + 2 * pal[1].b) / 3), 255};
  } else {
    pal[2] = {static_cast<uint8_t>((pal[0].r + pal[1].r) / 2), static_cast<uint8_t>((pal[0].g + pal[1].g) / 2),
              static_cast<uint8_t>((pal[0].b + pal[1].b) / 2), 255};
    pal[3] = {0, 0, 0, 0};
  }
  const uint32_t bits = static_cast<uint32_t>(color[4] | color[5] << 8 | color[6] << 16) | uint32_t(color[7]) << 24;
  for (int t = 0; t < 16; ++t) out[t] = pal[(bits >> (2 * t)) & 3];
  if (format == 19) {
    for (int t = 0; t < 16; ++t) out[t].a = static_cast<uint8_t>(((b[t / 2] >> (4 * (t & 1))) & 15) * 17);
  } else if (format == 20) {
    uint32_t a[8] = {b[0], b[1]};
    if (a[0] > a[1]) {
      for (int k = 1; k < 7; ++k) a[k + 1] = ((7 - k) * a[0] + k * a[1]) / 7;
    } else {
      for (int k = 1; k < 5; ++k) a[k + 1] = ((5 - k) * a[0] + k * a[1]) / 5;
      a[6] = 0;
      a[7] = 255;
    }
    uint64_t abits = 0;
    for (int k = 0; k < 6; ++k) abits |= static_cast<uint64_t>(b[2 + k]) << (8 * k);
    for (int t = 0; t < 16; ++t) out[t].a = static_cast<uint8_t>(a[(abits >> (3 * t)) & 7]);
  }
}

// One DXT5 block (little-endian): this alpha (the 8-value ramp between the block's highest and lowest, as
// text_native's Dxt5WhiteBlock makes it) and one color: everywhere (both ends `c565`, every pick 0) or, `scaled`, by
// each texel's alpha (ends `c565` and black, the pick nearest alpha / 255 of 1, 2/3, 1/3, 0), for a picture whose clear
// texels are black: drawn with either blending (alpha, or premultiplied), the copy then shows what the picture does.
inline std::array<uint8_t, 16> Dxt5Block(const uint8_t a[16], uint16_t c565, bool scaled = false) {
  const uint8_t hi = *std::max_element(a, a + 16), lo = *std::min_element(a, a + 16);
  uint64_t bits = 0;
  if (hi != lo) {
    int pal[8] = {hi, lo};
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
  for (int i = 0; i < 6; ++i) b[2 + i] = static_cast<uint8_t>(bits >> (8 * i));
  b[8] = static_cast<uint8_t>(c565 & 0xFF);
  b[9] = static_cast<uint8_t>(c565 >> 8);
  if (!scaled) {
    b[10] = b[8];
    b[11] = b[9];
  } else {  // c1 black: picks 0 = c, 1 = black, 2 = 2/3 c, 3 = 1/3 c (DXT4/5 colors are always the four-color kind)
    uint32_t picks = 0;
    for (int i = 0; i < 16; ++i) {
      const int v = a[i];
      const int pick = v >= 213 ? 0 : v >= 128 ? 2 : v >= 43 ? 3 : 1;
      picks |= uint32_t(pick) << (2 * i);
    }
    for (int i = 0; i < 4; ++i) b[12 + i] = static_cast<uint8_t>(picks >> (8 * i));
  }
  return b;
}

// One DXT5 block (little-endian) of two colors: this alpha, each texel's mix of the colors (0 the darker, 255 the
// brighter) by the four-color picks between them (ends `bright` and `dark`: 1, 2/3, 1/3, 0 of the brighter).
inline std::array<uint8_t, 16> Dxt5Block2(const uint8_t a[16], const uint8_t mix[16], uint16_t bright, uint16_t dark) {
  std::array<uint8_t, 16> b = Dxt5Block(a, bright, false);
  b[10] = static_cast<uint8_t>(dark & 0xFF);
  b[11] = static_cast<uint8_t>(dark >> 8);
  uint32_t picks = 0;
  for (int i = 0; i < 16; ++i) {
    const int v = mix[i];
    const int pick = v >= 213 ? 0 : v >= 128 ? 2 : v >= 43 ? 3 : 1;
    picks |= uint32_t(pick) << (2 * i);
  }
  for (int i = 0; i < 4; ++i) b[12 + i] = static_cast<uint8_t>(picks >> (8 * i));
  return b;
}

// The texture's address mode on an axis (the fetch constant's clamp_x / clamp_y, Xenos ClampMode): which texel index
// `i` (one past either edge at most, for the bilinear filter) reads, or -1 for the border color.
inline int Address(int i, int n, uint32_t mode) {
  if (i >= 0 && i < n) return i;
  switch (mode) {
    case 0:  // repeat
      return ((i % n) + n) % n;
    case 1: {  // mirrored repeat
      const int m = ((i % (2 * n)) + 2 * n) % (2 * n);
      return m < n ? m : 2 * n - 1 - m;
    }
    case 6:  // clamp to border
    case 7:  // mirror, clamp to border
      return -1;
    default:  // clamp to the edge, the half-way kinds (one texel past is the edge texel)
      return std::clamp(i, 0, n - 1);
  }
}

struct Picture {
  int w = 0, h = 0;
  std::vector<Rgba> texels;   // w * h, row by row
  uint32_t mode_u = 2, mode_v = 2;
  uint8_t border_alpha = 0;
};

enum class Verdict { kSharp, kEmpty, kColors, kFaint };

struct Facts {
  int strongest = 0;     // the highest alpha
  int levels = 0;        // distinct alphas
  int part = 0;          // texels neither clear nor at the strongest
  int faint = 0;         // texels under half the strongest, but not clear
  Rgba color;            // the one color of every texel that shows (two colors: the brighter)
  uint16_t c565 = 0;
  bool scaled = false;   // its clear texels aren't that color (black: DXT1's, or a premultiplied picture's)
  // [acct2-ocr] 2026-11-21 two colors (the game's own cross: a white line in a black outline, run sd-mp60b): every shown
  // texel on the line from the darker to the brighter (each its mix of them, Mix), the in-between ones only where the
  // two meet (an edge's antialiasing); the copy's colour boundary goes where the game's own mix is half-way.
  bool two = false;
  Rgba dark;
  uint16_t c565_dark = 0;
  int mixed = 0;         // texels between the two colors
};

// A shown texel's mix of a two-color picture's colors: 0 the darker, 1 the brighter (one color: 1).
inline double Mix(const Facts& f, const Rgba& t) {
  if (!f.two) return 1.0;
  const double dx = double(f.color.r) - f.dark.r, dy = double(f.color.g) - f.dark.g, dz = double(f.color.b) - f.dark.b;
  const double len2 = dx * dx + dy * dy + dz * dz;
  if (len2 <= 0) return 1.0;
  return std::clamp(((t.r - f.dark.r) * dx + (t.g - f.dark.g) * dy + (t.b - f.dark.b) * dz) / len2, 0.0, 1.0);
}

// One texel of the exact copy (8:8:8:8): this alpha and the color the DXT5 blocks above aim at, unrounded: the mix of
// the two colors (two), else the one color everywhere, or by alpha (scaled: a picture whose clear texels are black).
inline Rgba CopyTexel(const Facts& f, uint8_t alpha, uint8_t mix) {
  auto lerp = [](int from, int to, double k) {
    return static_cast<uint8_t>(std::clamp(std::lround(from + k * (to - from)), 0L, 255L));
  };
  Rgba c = f.color;
  if (f.two) {
    const double k = mix / 255.0;
    c = {lerp(f.dark.r, f.color.r, k), lerp(f.dark.g, f.color.g, k), lerp(f.dark.b, f.color.b, k), 0};
  } else if (f.scaled) {
    const double k = alpha / 255.0;
    c = {lerp(0, f.color.r, k), lerp(0, f.color.g, k), lerp(0, f.color.b, k), 0};
  }
  c.a = alpha;
  return c;
}

// [acct2-ocr] 2026-11-21 what a picture's colors are, in numbers only (the log line of a picture left as the game's):
// its shown texels' distinct colors, each channel's range, how far the farthest color is from the line between the
// darkest and the brightest (0: all on it, one color fading into another), how strong the darkest and the brightest
// third of them are on average, and the clear texels' lightness range.
struct ColorFacts {
  int shown = 0, colors = 0;
  int lo[3] = {255, 255, 255}, hi[3] = {0, 0, 0};
  int off_line = 0;
  int alpha_dark = 0, alpha_bright = 0;
  int clear = 0, clear_lo = 255, clear_hi = 0;
};

inline ColorFacts Colors(const Picture& p) {
  ColorFacts f;
  std::vector<const Rgba*> shown;
  std::vector<uint32_t> seen;
  for (const Rgba& t : p.texels) {
    const int light = std::min({t.r, t.g, t.b});
    if (t.a == 0) {
      ++f.clear;
      f.clear_lo = std::min(f.clear_lo, light);
      f.clear_hi = std::max(f.clear_hi, light);
      continue;
    }
    shown.push_back(&t);
    const uint32_t c = uint32_t(t.r) << 16 | uint32_t(t.g) << 8 | t.b;
    if (std::find(seen.begin(), seen.end(), c) == seen.end()) seen.push_back(c);
    const int ch[3] = {t.r, t.g, t.b};
    for (int k = 0; k < 3; ++k) {
      f.lo[k] = std::min(f.lo[k], ch[k]);
      f.hi[k] = std::max(f.hi[k], ch[k]);
    }
  }
  f.shown = int(shown.size());
  f.colors = int(seen.size());
  if (shown.empty()) return f;
  auto luma = [](const Rgba* t) { return int(t->r) * 299 + int(t->g) * 587 + int(t->b) * 114; };
  std::sort(shown.begin(), shown.end(), [&](const Rgba* a, const Rgba* b) { return luma(a) < luma(b); });
  const Rgba& d = *shown.front();
  const Rgba& b = *shown.back();
  const double dx = double(b.r) - d.r, dy = double(b.g) - d.g, dz = double(b.b) - d.b;
  const double len2 = dx * dx + dy * dy + dz * dz;
  for (const Rgba* t : shown) {
    double s = len2 > 0 ? ((t->r - d.r) * dx + (t->g - d.g) * dy + (t->b - d.b) * dz) / len2 : 0.0;
    s = std::clamp(s, 0.0, 1.0);
    const double e = std::max({std::abs(t->r - (d.r + s * dx)), std::abs(t->g - (d.g + s * dy)),
                               std::abs(t->b - (d.b + s * dz))});
    f.off_line = std::max(f.off_line, int(std::lround(e)));
  }
  const size_t third = std::max<size_t>(1, shown.size() / 3);
  long sd = 0, sb = 0;
  for (size_t i = 0; i < third; ++i) {
    sd += shown[i]->a;
    sb += shown[shown.size() - 1 - i]->a;
  }
  f.alpha_dark = int(sd / long(third));
  f.alpha_bright = int(sb / long(third));
  return f;
}

// Whether the sharp copy keeps everything the picture shows: one color (an exact 5:6:5 one) on every texel that isn't
// clear, and each faint texel (under half the strongest alpha) beside one at half or more (an edge's antialiasing,
// which the half-strength outline takes in). Else the picture is left as the game's.
inline Verdict Check(const Picture& p, Facts* f) {
  *f = Facts{};
  bool seen[256] = {};
  bool one_color = true, have_color = false;
  for (const Rgba& t : p.texels) {
    if (!seen[t.a]) {
      seen[t.a] = true;
      ++f->levels;
    }
    f->strongest = std::max<int>(f->strongest, t.a);
    if (t.a == 0) continue;
    if (!have_color) {
      f->color = t;
      have_color = true;
    } else if (t.r != f->color.r || t.g != f->color.g || t.b != f->color.b) {
      one_color = false;
    }
  }
  if (f->strongest == 0) return Verdict::kEmpty;
  if (one_color) {
    f->color.a = 255;
    if (!Exact565(f->color, &f->c565)) return Verdict::kColors;
    for (const Rgba& t : p.texels)
      if (t.a == 0 && (t.r != f->color.r || t.g != f->color.g || t.b != f->color.b)) f->scaled = true;
  } else {
    // two colors: the darkest and the brightest shown, every other shown one on the line between (within 6 a channel)
    const ColorFacts c = Colors(p);
    if (c.off_line > 6) return Verdict::kColors;
    auto luma = [](const Rgba& t) { return int(t.r) * 299 + int(t.g) * 587 + int(t.b) * 114; };
    bool first = true;
    for (const Rgba& t : p.texels) {
      if (t.a == 0) continue;
      if (first || luma(t) < luma(f->dark)) f->dark = t;
      if (first || luma(t) > luma(f->color)) f->color = t;
      first = false;
    }
    f->dark.a = f->color.a = 255;
    if (!Exact565(f->color, &f->c565)) return Verdict::kColors;
    f->two = true;
    // a picture whose colour only follows its alpha (black where clear, the colour times the alpha elsewhere) is one
    // premultiplied colour, not two
    bool follows = true;
    for (const Rgba& t : p.texels) {
      if (t.a == 0) continue;
      const int ch[3] = {t.r, t.g, t.b}, full[3] = {f->color.r, f->color.g, f->color.b};
      for (int k = 0; k < 3; ++k)
        if (std::abs(ch[k] - t.a * full[k] / 255.0) > 12.0) follows = false;
    }
    if (follows) {
      f->two = false;
      f->scaled = true;
    } else {
      if (!Exact565(f->dark, &f->c565_dark)) return Verdict::kColors;
      // in-between texels only where the two colors meet: each beside a darker one and a brighter one
      for (int y = 0; y < p.h; ++y) {
        for (int x = 0; x < p.w; ++x) {
          const Rgba& t = p.texels[size_t(y) * size_t(p.w) + size_t(x)];
          const double m = Mix(*f, t);
          if (t.a == 0 || m <= 0.1 || m >= 0.9) continue;
          ++f->mixed;
          bool darker = false, brighter = false;
          for (int dy = -1; dy <= 1; ++dy) {
            for (int dx = -1; dx <= 1; ++dx) {
              const int xx = x + dx, yy = y + dy;
              if (!(dx || dy) || xx < 0 || yy < 0 || xx >= p.w || yy >= p.h) continue;
              const Rgba& n = p.texels[size_t(yy) * size_t(p.w) + size_t(xx)];
              if (n.a == 0) continue;
              const double nm = Mix(*f, n);
              darker |= nm <= 0.1;
              brighter |= nm >= 0.9;
            }
          }
          if (!darker || !brighter) return Verdict::kColors;
        }
      }
    }
  }
  for (int y = 0; y < p.h; ++y) {
    for (int x = 0; x < p.w; ++x) {
      const int a = p.texels[size_t(y) * size_t(p.w) + size_t(x)].a;
      if (a != 0 && a != f->strongest) ++f->part;
      if (a == 0 || 2 * a >= f->strongest) continue;
      ++f->faint;
      bool beside = false;
      for (int dy = -1; dy <= 1 && !beside; ++dy) {
        for (int dx = -1; dx <= 1 && !beside; ++dx) {
          const int xx = x + dx, yy = y + dy;
          if ((dx || dy) && xx >= 0 && yy >= 0 && xx < p.w && yy < p.h &&
              2 * p.texels[size_t(yy) * size_t(p.w) + size_t(xx)].a >= f->strongest)
            beside = true;
        }
      }
      if (!beside) return Verdict::kFaint;
    }
  }
  return Verdict::kSharp;
}

// The sharp copy's alpha, nw x nh texels (row by row), for a picture Check passed with `strongest`; with `facts` and
// `mix_out` (two colors) also each texel's mix of them (0 the darker, 255 the brighter): at each point the game's own
// mix there (its shown texels' mixes, filtered as it filters them, weighted by their alpha), taken as the brighter
// where that is half-way or more, averaged over the texel's points that are inside (or, outside, over all its points).
// `win_u` x `win_v` (in u and v): the window each texel averages over, centred on it (one screen pixel: 1 / the side's
// length in pixels, so the copy is the shape antialiased over exactly one pixel whatever its own texel size); 0: the
// texel's own area.
inline std::vector<uint8_t> Rebuild(const Picture& p, int strongest, int nw, int nh, int samples = 8,
                                    const Facts* facts = nullptr, std::vector<uint8_t>* mix_out = nullptr,
                                    double win_u = 0.0, double win_v = 0.0) {
  std::vector<uint8_t> out(size_t(nw) * size_t(nh), 0);
  const bool two = facts && facts->two && mix_out;
  if (two) mix_out->assign(size_t(nw) * size_t(nh), 255);
  std::vector<double> mix;
  if (two) {
    mix.resize(p.texels.size());
    for (size_t i = 0; i < p.texels.size(); ++i) mix[i] = Mix(*facts, p.texels[i]);
  }
  const double half = 0.5 * strongest;
  auto mix_at = [&](int i, int j) -> double {
    const int x = Address(i, p.w, p.mode_u), y = Address(j, p.h, p.mode_v);
    if (x < 0 || y < 0) return 0.0;
    return mix[size_t(y) * size_t(p.w) + size_t(x)];
  };
  auto alpha = [&](int i, int j) -> double {
    const int x = Address(i, p.w, p.mode_u), y = Address(j, p.h, p.mode_v);
    if (x < 0 || y < 0) return p.border_alpha;
    return p.texels[size_t(y) * size_t(p.w) + size_t(x)].a;
  };
  if (win_u > 0.0 && win_v > 0.0) {
    // The drawing on a lattice of `samples` cells a window, from half a window before the picture to half a window
    // after it (its edges as the sampler gives them); each texel the box over its window: the cells it covers, each by
    // how much of it the window covers. Made one axis after the other.
    const int n = std::max(samples, 1);
    const double hu = win_u / n, hv = win_v / n, u0 = -0.5 * win_u, v0 = -0.5 * win_v;
    const int lu = static_cast<int>(std::ceil((1.0 + win_u) / hu)) + 2;
    const int lv = static_cast<int>(std::ceil((1.0 + win_v) / hv)) + 2;
    struct Span {
      int first = 0;
      std::vector<double> share;
    };
    auto spans = [](int texels, double h, double start, double win, int cells) {
      std::vector<Span> out_spans(static_cast<size_t>(texels));
      for (int t = 0; t < texels; ++t) {
        const double c = (t + 0.5) / texels, a = c - 0.5 * win, b = c + 0.5 * win;
        const int k0 = std::max(0, static_cast<int>(std::floor((a - start) / h)));
        const int k1 = std::min(cells - 1, static_cast<int>(std::floor((b - start) / h)));
        out_spans[size_t(t)].first = k0;
        for (int k = k0; k <= k1; ++k) {
          const double lo = std::max(a, start + k * h), hi = std::min(b, start + (k + 1) * h);
          out_spans[size_t(t)].share.push_back(std::max(0.0, hi - lo) / h);
        }
      }
      return out_spans;
    };
    const std::vector<Span> su = spans(nw, hu, u0, win_u, lu), sv = spans(nh, hv, v0, win_v, lv);
    // each lattice column's two texel columns (as the sampler addresses them, -1 for the border) and its weight, and
    // the same for the rows
    struct Axis {
      int a, b;
      double f;
    };
    auto axis = [](int cells, double start, double h, int size, uint32_t mode) {
      std::vector<Axis> out_axis(static_cast<size_t>(cells));
      for (int k = 0; k < cells; ++k) {
        const double t = (start + (k + 0.5) * h) * size - 0.5;
        const int i0 = static_cast<int>(std::floor(t));
        out_axis[size_t(k)] = {Address(i0, size, mode), Address(i0 + 1, size, mode), t - i0};
      }
      return out_axis;
    };
    const std::vector<Axis> ax = axis(lu, u0, hu, p.w, p.mode_u), ay = axis(lv, v0, hv, p.h, p.mode_v);
    auto texel_alpha = [&](int x, int y) -> double {
      return x < 0 || y < 0 ? double(p.border_alpha) : double(p.texels[size_t(y) * size_t(p.w) + size_t(x)].a);
    };
    auto texel_mix = [&](int x, int y) -> double {
      return x < 0 || y < 0 ? 0.0 : mix[size_t(y) * size_t(p.w) + size_t(x)];
    };
    constexpr int kChannels = 4;   // value, value where brighter, brighter where shown, shown
    std::vector<float> cells(size_t(lu) * size_t(lv) * kChannels);
    for (int j = 0; j < lv; ++j) {
      const Axis& yj = ay[size_t(j)];
      for (int k = 0; k < lu; ++k) {
        // the game's drawing at this point, as the loop below takes it: inside where its bilinear alpha is half the
        // strongest or more, its value there from the texels at half or more; its mix, weighted by alpha
        const Axis& xk = ax[size_t(k)];
        const double fx = xk.f, fy = yj.f;
        const int xs[4] = {xk.a, xk.b, xk.a, xk.b}, ys[4] = {yj.a, yj.a, yj.b, yj.b};
        const double w[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
        double a[4], drawn = 0.0, num = 0.0, den = 0.0;
        for (int q = 0; q < 4; ++q) {
          a[q] = texel_alpha(xs[q], ys[q]);
          drawn += w[q] * a[q];
          if (a[q] >= half) {
            num += w[q] * a[q];
            den += w[q];
          }
        }
        const double value = drawn >= half && den > 0.0 ? num / den : 0.0;
        double bright = 0.0, shown = 0.0;
        if (two) {
          double mw = 0.0, mv = 0.0;
          for (int q = 0; q < 4; ++q) {
            mw += w[q] * a[q];
            mv += w[q] * a[q] * texel_mix(xs[q], ys[q]);
          }
          if (mw > 0.0) {
            shown = 1.0;
            bright = mv / mw >= 0.5 ? 1.0 : 0.0;
          }
        }
        float* c = &cells[(size_t(j) * size_t(lu) + size_t(k)) * kChannels];
        c[0] = float(value);
        c[1] = float(value * bright);
        c[2] = float(bright * shown);
        c[3] = float(shown);
      }
    }
    std::vector<double> rows(size_t(lv) * size_t(nw) * kChannels, 0.0);
    for (int j = 0; j < lv; ++j) {
      for (int X = 0; X < nw; ++X) {
        double* r = &rows[(size_t(j) * size_t(nw) + size_t(X)) * kChannels];
        const Span& s = su[size_t(X)];
        for (size_t q = 0; q < s.share.size(); ++q) {
          const float* c = &cells[(size_t(j) * size_t(lu) + size_t(s.first) + q) * kChannels];
          for (int ch = 0; ch < kChannels; ++ch) r[ch] += s.share[q] * c[ch];
        }
      }
    }
    const double total = double(n) * double(n);
    for (int Y = 0; Y < nh; ++Y) {
      const Span& s = sv[size_t(Y)];
      for (int X = 0; X < nw; ++X) {
        double sum[kChannels] = {};
        for (size_t q = 0; q < s.share.size(); ++q) {
          const double* r = &rows[((size_t(s.first) + q) * size_t(nw) + size_t(X)) * kChannels];
          for (int ch = 0; ch < kChannels; ++ch) sum[ch] += s.share[q] * r[ch];
        }
        out[size_t(Y) * size_t(nw) + size_t(X)] =
            static_cast<uint8_t>(std::clamp(std::lround(sum[0] / total), 0L, 255L));
        if (two) {
          const double m = sum[0] > 1e-9 ? sum[1] / sum[0] : sum[3] > 1e-9 ? sum[2] / sum[3] : 1.0;
          (*mix_out)[size_t(Y) * size_t(nw) + size_t(X)] =
              static_cast<uint8_t>(std::clamp(std::lround(m * 255.0), 0L, 255L));
        }
      }
    }
    return out;
  }
  for (int Y = 0; Y < nh; ++Y) {
    for (int X = 0; X < nw; ++X) {
      double sum = 0.0, mix_in = 0.0, mix_in_w = 0.0, mix_all = 0.0;
      int mix_all_n = 0;
      for (int sy = 0; sy < samples; ++sy) {
        const double v = (Y + (sy + 0.5) / samples) / nh;
        const double ty = v * p.h - 0.5;
        const int j0 = static_cast<int>(std::floor(ty));
        const double fy = ty - j0;
        for (int sx = 0; sx < samples; ++sx) {
          const double u = (X + (sx + 0.5) / samples) / nw;
          const double tx = u * p.w - 0.5;
          const int i0 = static_cast<int>(std::floor(tx));
          const double fx = tx - i0;
          const double a[4] = {alpha(i0, j0), alpha(i0 + 1, j0), alpha(i0, j0 + 1), alpha(i0 + 1, j0 + 1)};
          const double w[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
          double drawn = 0.0, num = 0.0, den = 0.0;
          for (int k = 0; k < 4; ++k) {
            drawn += w[k] * a[k];
            if (a[k] >= half) {
              num += w[k] * a[k];
              den += w[k];
            }
          }
          const bool inside = drawn >= half && den > 0.0;
          if (inside) sum += num / den;
          if (two) {
            double mw = 0.0, mv = 0.0;
            const double m[4] = {mix_at(i0, j0), mix_at(i0 + 1, j0), mix_at(i0, j0 + 1), mix_at(i0 + 1, j0 + 1)};
            for (int k = 0; k < 4; ++k) {
              mw += w[k] * a[k];
              mv += w[k] * a[k] * m[k];
            }
            if (mw > 0.0) {
              const double bright = mv / mw >= 0.5 ? 1.0 : 0.0;
              if (inside) {
                mix_in += bright * (num / den);
                mix_in_w += num / den;
              }
              mix_all += bright;
              ++mix_all_n;
            }
          }
        }
      }
      const double mean = sum / (samples * samples);
      out[size_t(Y) * size_t(nw) + size_t(X)] = static_cast<uint8_t>(std::clamp(std::lround(mean), 0L, 255L));
      if (two) {
        const double m = mix_in_w > 0.0 ? mix_in / mix_in_w : mix_all_n ? mix_all / mix_all_n : 1.0;
        (*mix_out)[size_t(Y) * size_t(nw) + size_t(X)] =
            static_cast<uint8_t>(std::clamp(std::lround(m * 255.0), 0L, 255L));
      }
    }
  }
  return out;
}

// Texels needed across a picture side drawn `pixels` long, `per_pixel` texels a pixel: whole blocks (4), at least the
// picture's own, at most 256.
inline int SideFor(double pixels, int own, int per_pixel = 1) {
  if (!(pixels > 0.0) || !std::isfinite(pixels)) return own;
  const int need = static_cast<int>(std::ceil(std::min(pixels * std::max(per_pixel, 1), 256.0) - 1e-3));
  return std::clamp((std::max(need, own) + 3) & ~3, 4, 256);
}

// FNV-1a, for the picture's bytes and settings (a cache key: a level loaded again with the same picture finds the
// copy made before; another picture at the same place does not).
inline uint64_t Hash(const void* data, size_t bytes, uint64_t h = 0xCBF29CE484222325ull) {
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < bytes; ++i) h = (h ^ p[i]) * 0x100000001B3ull;
  return h;
}

}  // namespace cod3::crosshair_texture::math
