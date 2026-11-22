// cod3 - the game's own crosshair drawn sharp at the screen's resolution (see crosshair_texture.h). [acct2-ocr]
// 2026-11-21 task 125

#include "ui/crosshair_texture.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>

#include "game/diagnostics.h"
#include "game/guest.h"
#include "game/native_memory.h"
#include "game/render_resolution.h"
#include "ui/crosshair_texture_math.h"

REXCVAR_DEFINE_BOOL(cod3_crosshair_sharp, true, "cod3/Display",
                    "The game's own crosshair drawn sharp at the screen's resolution: each of its pictures made "
                    "again at the size it is drawn, its outline where the game's own drawing is half its strength. "
                    "Where it is, its size, color, fade and spread stay the game's. Off: the game's picture "
                    "magnified (soft above 640x480), as the HD or your user texture pack gives it")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [acct2-ocr] 2026-11-21 for the checks: the copy and the game's picture in turn, each this many seconds, each change in
// the log, so one run's screenshots show both on the same ground (two runs spawn in different places).
REXCVAR_DEFINE_INT32(cod3_debug_crosshair_ab, 0, "cod3/Debug",
                     "Checks: the sharp crosshair and the game's picture in turn, each this many seconds (0: off)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
// [acct2-ocr] 2026-11-21 for the checks: each picture made again and its copy as written, to the logs folder, for a
// check on the same computer that draws both the way the GPU does (dev/agents/acct2-ocr/crosshair-texture).
REXCVAR_DEFINE_BOOL(cod3_debug_crosshair_dump, false, "cod3/Debug",
                    "Checks: write each crosshair picture made again, and its copy as written, to logs/crosshair-*.bin")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace cod3::crosshair_texture {

namespace {

namespace m = math;

thread_local int t_drawing = 0;   // inside the game's CG_DrawCrosshair (nested calls counted)

constexpr uint32_t kQuadTexture = 0x60;   // the quad's NGL texture
constexpr uint32_t kVertexStride = 0x14;  // four vertices {x, y, u, v, color} (ultrawide's uw::kQuadVertexStride)
// The NGL texture object (0x60 bytes, as ui/text_native.cpp makes its pages): +0 width, +4 height, +0x14 -> its
// Direct3D header, which the game's own have in the object itself at +0x2C (0x34 bytes: the fetch constant at +0x1C).
constexpr uint32_t kTexSize = 0x60, kTexW = 0, kTexH = 4, kTexHeaderPtr = 0x14, kTexHeader = 0x2C;
constexpr uint32_t kHeaderFetch = 0x1C;
constexpr int kMaxSource = 64;      // texels a side: a bigger picture is no crosshair piece; left alone
constexpr size_t kMaxCopies = 64;   // copies kept (never freed: the GPU may still draw one)
constexpr uint32_t kMaxCopyBytes = 4u << 20;   // their texels together (256 KB a copy at most; 1080p's are 36 KB)
// [acct2-ocr] 2026-11-21 the copy: each texel the shape antialiased over exactly one screen pixel (Rebuild's window),
// kept at 4 texels a pixel so the GPU's bilinear between them adds a quarter pixel at most, the window's points on a
// lattice of 16 a pixel; exact 8:8:8:8 texels (DXT5's 3 picks between two colors moved the colour's edge up to 0.14 px:
// xsim on the run's dumps against the ideal, 0.05 px at most this way)
constexpr int kPerPixel = 4, kLatticePerPixel = 16;
// The GPU draws the crosshair's quads clamped to the edge, whatever the texture's own fetch says (the game sets the
// sampler when it draws its list; its texture's fetch reads repeat): run acct2-ocr-sg-ab60's frames against xsim, both
// arms' ends within 0.05 px with clamp, the inner ends 0.8 to 0.9 px short with repeat. The copy models it so.
constexpr uint32_t kDrawnAddress = 2;
constexpr size_t kMaxTold = 48;
constexpr uint32_t kPhysicalBytes = 0x20000000u;

struct Copy {
  uint64_t key = 0;     // the picture's bytes and settings, and the copy's size
  uint32_t texture = 0; // our NGL texture object
};

std::mutex g_mutex;
std::vector<Copy> g_copies;
std::vector<uint64_t> g_told;  // pictures (and why) already in the log
bool g_full_told = false;
uint32_t g_copy_bytes = 0;   // the copies' texels together

auto* Memory() { return rex::system::kernel_state()->memory(); }
uint8_t* Host(uint32_t guest) { return Memory()->TranslateVirtual<uint8_t*>(guest); }

uint32_t Be32(const uint8_t* p) {
  uint32_t v;
  std::memcpy(&v, p, 4);
  return __builtin_bswap32(v);
}
void PutBe32(uint8_t* p, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(p, &v, 4);
}

bool Object(uint32_t a) { return a >= 0x10000u && a < 0xFFF00000u && (a & 3) == 0; }

uint32_t Log2Ceil(uint32_t v) {
  uint32_t l = 0;
  while (l < 31 && (1u << l) < v) ++l;
  return l;
}

// The GPU (physical) address of an allocation from the physical system heap (ui/text_native.cpp's PhysicalOf: the
// 0xE0000000 range sits 0x1000 bytes up in physical memory).
uint32_t PhysicalOf(uint32_t guest) { return (guest & 0x1FFFFFFFu) + (guest >= 0xE0000000u ? 0x1000u : 0u); }

// Once per picture (and why): what was found, numbers only.
bool FirstTime(uint64_t key) {
  if (std::find(g_told.begin(), g_told.end(), key) != g_told.end() || g_told.size() >= kMaxTold) return false;
  g_told.push_back(key);
  return true;
}

// The picture the game's quad samples, read as the GPU reads it (game/ngl_side.cpp's DecodeFetch: its fetch
// constant's format, byte order, tiling, pitch and a small texture's place in its packed mip tile), and a key of its
// bytes and the fetch settings that change how it is drawn. False (and `why`) for any picture this leaves alone.
struct Source {
  m::Picture picture;            // its address modes the GPU's as it draws the crosshair (kDrawnAddress)
  uint32_t mode_u = 0, mode_v = 0;   // its fetch's own
  uint32_t format = 0;
  uint64_t key = 0;
  uint32_t object = 0;   // the game's NGL texture
  uint32_t fetch[6] = {};
};

bool ReadSource(uint32_t object, Source* s, const char** why) {
  *why = nullptr;
  if (!Object(object)) return false;
  const uint8_t* obj = Host(object);
  if (!obj) return false;
  if (Be32(obj + kTexHeaderPtr) != object + kTexHeader) {
    *why = "its Direct3D header isn't in its NGL texture";
    return false;
  }
  s->object = object;
  for (int i = 0; i < 6; ++i) s->fetch[i] = Be32(obj + kTexHeader + kHeaderFetch + 4 * i);
  const uint32_t* f = s->fetch;
  const uint32_t type = f[0] & 3, signs = (f[0] >> 2) & 0xFF;
  const uint32_t mode_u = (f[0] >> 10) & 7, mode_v = (f[0] >> 13) & 7;
  const uint32_t pitch = ((f[0] >> 22) & 0x1FF) * 32;
  const bool tiled = (f[0] >> 31) != 0;
  const uint32_t format = f[1] & 0x3F, endian = (f[1] >> 6) & 3;
  const uint32_t w = (f[2] & 0x1FFF) + 1, h = ((f[2] >> 13) & 0x1FFF) + 1;
  const uint32_t swizzle = (f[3] >> 1) & 0xFFF, exponent = (f[3] >> 13) & 0x3F;
  const uint32_t border = f[5] & 3, dimension = (f[5] >> 9) & 3;
  const bool packed = ((f[5] >> 11) & 1) != 0;
  if (type != 2 || dimension != 1) {
    *why = "not a 2D texture";
    return false;
  }
  if (!m::Readable(format)) {
    *why = "a format other than DXT1, DXT3 or DXT5";
    return false;
  }
  // as the GPU shows it with nothing changed: unsigned, no exponent, alpha from alpha and no color from it
  const uint32_t sx = swizzle & 7, sy = (swizzle >> 3) & 7, sz = (swizzle >> 6) & 7, sw = (swizzle >> 9) & 7;
  if (signs != 0 || exponent != 0 || sw != 3 || sx == 3 || sy == 3 || sz == 3) {
    *why = "read with signs, an exponent or its channels moved";
    return false;
  }
  if (w > uint32_t(kMaxSource) || h > uint32_t(kMaxSource)) {
    *why = "bigger than a crosshair piece";
    return false;
  }
  // the picture's blocks where the GPU reads them
  const uint32_t bb = m::BlockBytes(format), log2_bb = bb == 8 ? 3 : 4;
  const uint32_t bw = (w + 3) / 4, bh = (h + 3) / 4;
  uint32_t ox = 0, oy = 0;
  if (packed && std::min(Log2Ceil(w), Log2Ceil(h)) <= 4) (Log2Ceil(w) > Log2Ceil(h) ? oy : ox) = 16 / 4;
  uint32_t pitch_blocks = std::max(pitch / 4, bw + ox);
  if (tiled) pitch_blocks = (pitch_blocks + 31) & ~31u;
  const uint32_t rows = tiled ? (bh + oy + 31) & ~31u : bh + oy;
  const size_t bytes = size_t(pitch_blocks) * rows * bb;
  const uint8_t* data = nullptr;
  size_t room = 0;
  if (cod3::native_memory::InMovedPool(f[1])) {  // the move's streaming pool: read as the guest sees it
    const uint32_t page = f[1] & 0xFFFFF000u;
    data = Host(page);
    room = cod3::native_memory::GpuRoom(page);
  } else {
    const uint32_t physical = f[1] & 0x1FFFF000u;
    uint8_t* membase = Memory()->physical_membase();
    data = membase ? membase + physical : nullptr;
    room = kPhysicalBytes - physical;
  }
  if (!data || bytes > room || (f[1] & 0xFFFFF000u) == 0) {
    *why = "its memory is out of reach";
    return false;
  }
  m::Picture& p = s->picture;
  p.w = int(w);
  p.h = int(h);
  p.texels.assign(size_t(w) * h, m::Rgba{});
  p.mode_u = p.mode_v = kDrawnAddress;
  p.border_alpha = border == 1 ? 255 : 0;
  s->mode_u = mode_u;
  s->mode_v = mode_v;
  const uint32_t settings[] = {w, h, m::BaseFormat(format), mode_u, mode_v, border, swizzle};
  uint64_t key = m::Hash(settings, sizeof settings);
  for (uint32_t by = 0; by < bh; ++by) {
    for (uint32_t bx = 0; bx < bw; ++bx) {
      const uint32_t at = tiled ? m::TiledBlock(bx + ox, by + oy, pitch_blocks, log2_bb)
                                : (by + oy) * pitch_blocks + (bx + ox);
      if (size_t(at + 1) * bb > bytes) {
        *why = "its memory is out of reach";
        return false;
      }
      uint8_t block[16];
      std::memcpy(block, data + size_t(at) * bb, bb);
      m::SwapEndian(block, bb, endian);
      key = m::Hash(block, bb, key);
      m::Rgba t[16];
      m::DecodeBlock(block, format, t);
      for (int k = 0; k < 16; ++k) {
        const uint32_t x = bx * 4 + (k & 3), y = by * 4 + (k >> 2);
        if (x < w && y < h) p.texels[size_t(y) * w + x] = t[k];
      }
    }
  }
  s->format = format;
  s->key = key;
  return true;
}

// Our copy: the game's NGL texture object and Direct3D header (its sampler settings kept) on our texels, a tiled
// 8:8:8:8 picture of nw x nh with no mips (each texel exact, CopyTexel), written whole before the GPU first sees it. 0
// when guest memory is short.
uint32_t MakeCopy(const Source& s, const std::vector<uint8_t>& alpha, const std::vector<uint8_t>& mix,
                  const m::Facts& facts, int nw, int nh, uint32_t* texels_out = nullptr,
                  uint32_t* bytes_out = nullptr) {
  auto* mem = Memory();
  const uint32_t pitch = (uint32_t(nw) + 31) & ~31u, rows = (uint32_t(nh) + 31) & ~31u;
  const uint32_t bytes = pitch * rows * 4;
  const uint32_t tex = mem->SystemHeapAlloc(kTexSize, 0x40);
  const uint32_t texels = mem->SystemHeapAlloc(bytes, 0x1000, rex::memory::kSystemHeapPhysical);
  if (!tex || !texels) {
    for (const uint32_t a : {tex, texels})
      if (a) mem->SystemHeapFree(a);
    return 0;
  }
  uint8_t* out = Host(texels);
  std::memset(out, 0, bytes);   // clear outside the picture (alpha 0)
  for (int y = 0; y < nh; ++y) {
    for (int x = 0; x < nw; ++x) {
      const size_t at = size_t(y) * size_t(nw) + size_t(x);
      const m::Rgba c = m::CopyTexel(facts, alpha[at], facts.two ? mix[at] : 255);
      uint8_t t[4] = {c.r, c.g, c.b, c.a};   // x in the word's low byte, as the picture's channels were
      m::SwapEndian(t, 4, 2);                // stored 8 in 32, as the fetch below says
      std::memcpy(out + size_t(m::TiledBlock(uint32_t(x), uint32_t(y), pitch, 2)) * 4, t, 4);
    }
  }
  uint8_t* obj = Host(tex);
  std::memcpy(obj, Host(s.object), kTexSize);
  PutBe32(obj + kTexW, uint32_t(nw));
  PutBe32(obj + kTexH, uint32_t(nh));
  PutBe32(obj + kTexHeaderPtr, tex + kTexHeader);
  uint32_t f[6];
  std::memcpy(f, s.fetch, sizeof f);
  f[0] = (f[0] & ~(0x1FFu << 22)) | ((pitch / 32) << 22) | (1u << 31);              // pitch in texels / 32; tiled
  f[1] = (PhysicalOf(texels) & 0xFFFFF000u) | (f[1] & 0xF00u) | (2u << 6) | 6u;      // 8_8_8_8, 8 in 32
  f[2] = (f[2] & ~0x3FFFFFFu) | uint32_t(nw - 1) | (uint32_t(nh - 1) << 13);
  f[4] &= ~(0xFFu << 2);                                                            // mip levels 0 to 0
  f[5] &= ~((1u << 11) | 0xFFFFF000u);                                              // not packed, no mip address
  for (int i = 0; i < 6; ++i) PutBe32(obj + kTexHeader + kHeaderFetch + 4 * i, f[i]);
  if (texels_out) *texels_out = texels;
  if (bytes_out) *bytes_out = bytes;
  return tex;
}

// cod3_debug_crosshair_dump: "XHD2", then u32s picture w, h, address u, v (as the GPU draws it), border alpha, format,
// copy w, h, pitch in texels, rows; floats the pixels across u and v; the picture's texels (r g b a each); the copy's
// bytes as in guest memory (8:8:8:8, tiled, 8 in 32).
void Dump(const Source& s, uint64_t key, int nw, int nh, float side_u, float side_v, uint32_t texels) {
  const uint32_t pitch = (uint32_t(nw) + 31) & ~31u, rows = (uint32_t(nh) + 31) & ~31u;
  char name[48];
  std::snprintf(name, sizeof name, "crosshair-%016llx.bin", static_cast<unsigned long long>(key));
  std::error_code ec;
  const std::filesystem::path dir = cod3::diag::LogsDir();
  std::filesystem::create_directories(dir, ec);
  std::ofstream out(dir / name, std::ios::binary | std::ios::trunc);
  if (!out) return;
  auto u32 = [&](uint32_t v) { out.write(reinterpret_cast<const char*>(&v), 4); };
  auto f32 = [&](float v) { out.write(reinterpret_cast<const char*>(&v), 4); };
  out.write("XHD2", 4);
  for (uint32_t v : {uint32_t(s.picture.w), uint32_t(s.picture.h), uint32_t(s.picture.mode_u),
                     uint32_t(s.picture.mode_v), uint32_t(s.picture.border_alpha), uint32_t(s.format), uint32_t(nw),
                     uint32_t(nh), pitch, rows})
    u32(v);
  f32(side_u);
  f32(side_v);
  for (const m::Rgba& t : s.picture.texels) {
    const char px[4] = {char(t.r), char(t.g), char(t.b), char(t.a)};
    out.write(px, 4);
  }
  out.write(reinterpret_cast<const char*>(Host(texels)), std::streamsize(size_t(pitch) * rows * 4));
  REXLOG_INFO("cod3: sharp crosshair: the picture and its copy written to {} for the checks", name);
}

const char* VerdictText(m::Verdict v) {
  switch (v) {
    case m::Verdict::kEmpty: return "nothing shows";
    case m::Verdict::kColors: return "more than one color (or one 5:6:5 can't hold)";
    case m::Verdict::kFaint: return "a faint part away from its shape (a glow or a shadow would be lost)";
    default: return "";
  }
}

// The sharp copy for this quad, or 0 to leave it as the game's. g_mutex held.
uint32_t CopyFor(uint8_t* base, uint32_t quad, uint32_t object, float hud_scale) {
  Source s;
  const char* why = nullptr;
  if (!ReadSource(object, &s, &why)) {
    if (why && FirstTime(m::Hash(&object, sizeof object) ^ m::Hash(why, std::strlen(why))))
      REXLOG_INFO("cod3: sharp crosshair: a picture of the crosshair left as the game's: {}", why);
    return 0;
  }
  // how long each side of the picture is on screen, in pixels: the quad's map from its texture coordinates to the
  // 640 x 480 HUD space (from the three corners that give it best), at the picture's size
  float x[4], y[4], u[4], v[4];
  for (int i = 0; i < 4; ++i) {
    const uint32_t at = quad + kVertexStride * uint32_t(i);
    x[i] = cod3::guest::ReadF32(base, at);
    y[i] = cod3::guest::ReadF32(base, at + 4);
    u[i] = cod3::guest::ReadF32(base, at + 8);
    v[i] = cod3::guest::ReadF32(base, at + 12);
  }
  static constexpr int kTriples[4][3] = {{0, 1, 2}, {0, 1, 3}, {0, 2, 3}, {1, 2, 3}};
  double best = 0.0, dxdu = 0, dxdv = 0, dydu = 0, dydv = 0;
  for (const auto& t : kTriples) {
    const double du1 = u[t[1]] - u[t[0]], dv1 = v[t[1]] - v[t[0]], du2 = u[t[2]] - u[t[0]], dv2 = v[t[2]] - v[t[0]];
    const double det = du1 * dv2 - du2 * dv1;
    if (!(std::abs(det) > best)) continue;
    best = std::abs(det);
    const double dx1 = x[t[1]] - x[t[0]], dy1 = y[t[1]] - y[t[0]], dx2 = x[t[2]] - x[t[0]], dy2 = y[t[2]] - y[t[0]];
    dxdu = (dx1 * dv2 - dx2 * dv1) / det;
    dxdv = (dx2 * du1 - dx1 * du2) / det;
    dydu = (dy1 * dv2 - dy2 * dv1) / det;
    dydv = (dy2 * du1 - dy1 * du2) / det;
  }
  if (!(best > 1e-6)) return 0;
  const cod3::RenderPlan plan = cod3::GetRenderPlan();
  const double out_w = plan.output_width(), out_h = plan.output_height();
  if (out_w < 1.0 || out_h < 1.0) return 0;
  const double px_x = out_w / 640.0 * (hud_scale > 0.0f ? hud_scale : 1.0), px_y = out_h / 480.0;
  // to a sixteenth of a pixel (the window is one pixel of these; the same quad found again finds the same copy)
  const double side_u = std::round(std::hypot(dxdu * px_x, dydu * px_y) * 16.0) / 16.0;
  const double side_v = std::round(std::hypot(dxdv * px_x, dydv * px_y) * 16.0) / 16.0;
  if (!(side_u > 0.0 && side_v > 0.0 && std::isfinite(side_u) && std::isfinite(side_v))) return 0;
  if (side_u <= s.picture.w && side_v <= s.picture.h) return 0;   // not magnified: the game's picture is as sharp
  const int nw = m::SideFor(side_u, s.picture.w, kPerPixel), nh = m::SideFor(side_v, s.picture.h, kPerPixel);
  const uint32_t size[4] = {uint32_t(nw), uint32_t(nh), uint32_t(std::lround(side_u * 16.0)),
                            uint32_t(std::lround(side_v * 16.0))};
  const uint64_t key = m::Hash(size, sizeof size, s.key);
  for (const Copy& c : g_copies)
    if (c.key == key) return c.texture;
  m::Facts facts;
  const m::Verdict verdict = m::Check(s.picture, &facts);
  if (verdict != m::Verdict::kSharp) {
    if (FirstTime(s.key ^ uint64_t(verdict))) {
      REXLOG_INFO("cod3: sharp crosshair: a {}x{} picture (format {}) of the crosshair left as the game's: {}",
                  s.picture.w, s.picture.h, s.format, VerdictText(verdict));
      const m::ColorFacts c = m::Colors(s.picture);   // [acct2-ocr] numbers only, to see what it is made of
      REXLOG_INFO("cod3: sharp crosshair:   its {} shown texels: {} colors, red {}-{} green {}-{} blue {}-{}, the "
                  "farthest {} from the line between the darkest and the brightest; strength {} (darkest third), {} "
                  "(brightest third); {} clear texels, lightness {}-{}",
                  c.shown, c.colors, c.lo[0], c.hi[0], c.lo[1], c.hi[1], c.lo[2], c.hi[2], c.off_line, c.alpha_dark,
                  c.alpha_bright, c.clear, c.clear_lo, c.clear_hi);
    }
    return 0;
  }
  if (g_copies.size() >= kMaxCopies) {
    if (!g_full_told) {
      g_full_told = true;
      REXLOG_WARN("cod3: sharp crosshair: {} copies made; new sizes are drawn as the game's", g_copies.size());
    }
    return 0;
  }
  if (g_copy_bytes + ((uint32_t(nw) + 31) & ~31u) * ((uint32_t(nh) + 31) & ~31u) * 4 > kMaxCopyBytes) {
    if (!g_full_told) {
      g_full_told = true;
      REXLOG_WARN("cod3: sharp crosshair: {} copies made ({} KB); new sizes are drawn as the game's", g_copies.size(),
                  g_copy_bytes / 1024);
    }
    return 0;
  }
  std::vector<uint8_t> mix;
  const std::vector<uint8_t> alpha = m::Rebuild(s.picture, facts.strongest, nw, nh, kLatticePerPixel, &facts, &mix,
                                                1.0 / side_u, 1.0 / side_v);
  uint32_t texels = 0, bytes = 0;
  const uint32_t texture = MakeCopy(s, alpha, mix, facts, nw, nh, &texels, &bytes);
  if (!texture) {
    if (!g_full_told) {
      g_full_told = true;
      REXLOG_WARN("cod3: sharp crosshair: no guest memory for a copy; drawn as the game's");
    }
    return 0;
  }
  g_copies.push_back({key, texture});
  g_copy_bytes += bytes;
  REXLOG_INFO("cod3: sharp crosshair: made again at {}x{} ({} copies, {} KB), drawn {:.2f} x {:.2f} pixels: a {}x{} "
              "picture (format {}, {} alpha levels, {} texels part-covered, {} of them faint, its clear texels {}; "
              "filters mag {} min {} mip {}, its fetch's address {} {}, drawn clamped to the edge)",
              nw, nh, g_copies.size(), g_copy_bytes / 1024, side_u, side_v, s.picture.w, s.picture.h, s.format,
              facts.levels, facts.part, facts.faint,
              facts.two ? "of the darker color (two colors, " + std::to_string(facts.mixed) + " texels between)"
                        : std::string(facts.scaled ? "black: color by alpha" : "its color"),
              (s.fetch[3] >> 19) & 3, (s.fetch[3] >> 21) & 3, (s.fetch[3] >> 23) & 3, s.mode_u, s.mode_v);
  if (REXCVAR_GET(cod3_debug_crosshair_dump)) Dump(s, key, nw, nh, side_u, side_v, texels);
  return texture;
}

// cod3_debug_crosshair_ab: whether this moment is the copy's (counted from the first crosshair drawn), the change in the
// log as it comes. g_mutex held.
bool AbCopy() {
  const int period = REXCVAR_GET(cod3_debug_crosshair_ab);
  if (period <= 0) return true;
  using Clock = std::chrono::steady_clock;
  static const Clock::time_point start = Clock::now();
  static int last = -1;
  const long long ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - start).count();
  const int copy = (ms / (1000LL * period)) % 2 == 0 ? 1 : 0;
  if (copy != last) {
    last = copy;
    REXLOG_INFO("cod3: sharp crosshair: A/B: {} from now", copy ? "the sharp copy" : "the game's picture");
  }
  return copy != 0;
}

}  // namespace

Drawing::Drawing() { ++t_drawing; }
Drawing::~Drawing() { --t_drawing; }

QuadSwap::QuadSwap(uint8_t* base, uint32_t quad, float hud_scale) {
  if (t_drawing <= 0 || !quad || !REXCVAR_GET(cod3_crosshair_sharp)) return;
  const uint32_t object = cod3::guest::ReadU32(base, quad + kQuadTexture);
  if (!object) return;
  uint32_t texture = 0;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    if (!AbCopy()) return;
    texture = CopyFor(base, quad, object, hud_scale);
  }
  if (!texture) return;
  base_ = base;
  quad_ = quad;
  texture_ = object;
  cod3::guest::WriteU32(base, quad + kQuadTexture, texture);
}

QuadSwap::~QuadSwap() {
  if (texture_) cod3::guest::WriteU32(base_, quad_ + kQuadTexture, texture_);
}

}  // namespace cod3::crosshair_texture
