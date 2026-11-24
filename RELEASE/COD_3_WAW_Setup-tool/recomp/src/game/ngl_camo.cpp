// [acct2-bro] 2026-11-22 cod3 - the camo layer's files, textures and choices (Camo lane). What and why: ngl_camo.h.
//
// The .c3camo file is c3w/camo_pack.py's (version 1, little endian): a 64-byte header ("C3CAMO1\0", version, header
// bytes, map count, flags, scroll u, scroll v, scale, reserved, data bytes, data crc32, 3 reserved, header crc32 of
// bytes 0-59 and the table), a table of 32 bytes a map (kind, format, width, height, levels, offset, bytes, reserved)
// and the maps' levels, largest first, packed. ParseCamo keeps the tool's read_camo rules exactly.
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
enum : uint32_t { kRgba8 = 1, kBc1 = 2, kBc2 = 3, kBc3 = 4 };

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
    case kBc3: return bw * bh * 16;
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
  if (version != 1 || head != kHeader || flags != 0 || U32(d + 48) || U32(d + 52) || U32(d + 56))
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
    if (kind < kColor || kind > kEnv || (kinds & (1u << kind)) || fmt < kRgba8 || fmt > kBc3 || U32(e + 28))
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
    v.maps.push_back(m);
  }
  if (expect != data_bytes) return fail("bytes after the last map");
  if (Crc32(d + table_end, data_bytes) != data_crc) return fail("its data checksum is wrong");
  *out = std::move(v);
  return true;
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
      ColorBlock(m.fmt == kBc1 ? blk : blk + 8, m.fmt == kBc1, px);
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

// A map ready for ngl_vk: BGRA levels (ngl_vk's order), largest first, at most kSide.
struct Ready {
  uint32_t kind = 0, width = 0, height = 0;
  std::vector<std::vector<uint8_t>> levels;
  float mean[3] = {0.5f, 0.5f, 0.5f};
};
Ready Prepare(const MapView& m) {
  Ready r;
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
          for (const MapView& m : v.maps) prepared.maps.push_back(Prepare(m));
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
    std::vector<const void*> levels;
    for (const auto& l : r.levels) levels.push_back(l.data());
    uint32_t tex = 0;
#if defined(NGL_CAMO_TEST)
    tex = TestTexture(levels.data(), uint32_t(levels.size()), r.width, r.height);
#elif defined(COD3_NGL_VK)
    tex = ngl_vk_texture_levels(levels.data(), uint32_t(levels.size()), r.width, r.height);
#endif
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
// levels as the reading thread does; the levels' sizes are checked against their place in the chain.
bool TestCheck(const uint8_t* d, size_t n, std::string* why) {
  CamoView v;
  if (!ParseCamo(d, n, &v, why)) return false;
  std::vector<Ready> made;
  for (const MapView& m : v.maps) {
    Ready r = Prepare(m);
    if (r.width < 1 || r.width > kSide || r.height < 1 || r.height > kSide || r.levels.empty()) return false;
    for (size_t i = 0; i < r.levels.size(); ++i) {
      const uint32_t w = std::max(1u, r.width >> i), h = std::max(1u, r.height >> i);
      if (r.levels[i].size() != size_t(w) * h * 4) return false;
    }
    if (r.levels.size() != FullLevels(r.width, r.height)) return false;
    made.push_back(std::move(r));
  }
  const std::string refusal = ColourRefusal(made);  // c519: as the reading thread refuses it
  if (!refusal.empty()) {
    if (why) *why = refusal;
    return false;
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
