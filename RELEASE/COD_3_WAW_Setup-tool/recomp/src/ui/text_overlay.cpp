// cod3 - our own text layer for Russian, Chinese, Japanese and Korean (see text_overlay.h). [acct2-ocr]

#include "ui/text_overlay.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cfloat>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include <imgui.h>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "game/button_prompts.h"  // [rexglue-native] 2026-11-17 P0 the latch per game frame
#include "game/color_codes.h"  // [acct2-ocr] codes inside a line
#include "game/guest.h"
#include "game/text_language.h"
#include "ui/text_native.h"  // [acct2-ocr]

REXCVAR_DEFINE_DOUBLE(cod3_text_overlay_dy, 0.0, "cod3/Language",
                      "Russian, Chinese, Japanese and Korean text: moves our lines up (-) or down (+), in the "
                      "game's 480-high units, to sit on the game's own baseline")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_DOUBLE(cod3_text_overlay_scale, 1.0, "cod3/Language",
                      "Russian, Chinese, Japanese and Korean text: our letters' size, 1 = as tall as the "
                      "game's own capitals")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_debug_text_overlay, false, "cod3/Language",
                    "Log the first lines the text layer takes over (place, scales, color)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace cod3::text_overlay {

namespace {

using cod3::guest::ReadS32;
using cod3::guest::ReadU32;
using cod3::guest::ReadString;
using cod3::guest::WriteU32;

constexpr std::array<const char*, 6> kOverlayLocales{"ru-RU", "zh-Hans", "zh-Hant-TW", "zh-Hant-HK", "ja-JP",
                                                     "ko-KR"};
// The game's FONT object (game/font_fill.cpp has the layout).
constexpr uint32_t kLineHeight = 0x1C, kAscent = 0x20, kFirstCode = 0x24, kCount = 0x28;
constexpr uint32_t kRecords = 0x38, kRecordSize = 28, kAdvance = 24, kGlyphs = 224;
constexpr uint32_t kRecordsPtr = 0x08;   // [acct2-ocr] 2026-11-07 the loaded FONT's pointer to its records
constexpr float kGameLineHeight = 40.0f;   // i_helvetica_bold: what the .overlay.txt sizes are for
constexpr float kBakePixels = 48.0f;       // one bake, drawn smaller (scaling down stays sharp)
constexpr auto kShowFor = std::chrono::milliseconds(50);   // as the key caps: a few frames

struct Metrics {
  std::string font_file;
  float em = 0, ascent = 0;
  std::map<int, float> lead;   // UTF-8 lead byte -> advance (px at the game's line height 40)
};

struct Line {
  std::string text;   // UTF-8, color codes taken out
  float x, y, scale;  // the game's 640x480 space; scale: its y scale
  float line_height, ascent;   // the game font's
  uint32_t color;     // D3DCOLOR (alpha in the top byte)
  std::chrono::steady_clock::time_point when;
  // [rexglue-native] 2026-11-17 P0 the game frame it was drawn in (cod3::prompts::GameFrameIn; 0 unknown) and when the game drew it (a copy
  // the latch adds keeps both; `when` is its last-seen time)
  uint32_t frame = 0;
  std::chrono::steady_clock::time_point drawn{};
};

std::mutex g_mutex;
std::filesystem::path g_root;
std::string g_baked;          // the locale whose font is in the atlas ("" none)
Metrics g_metrics;            // g_baked's
ImFont* g_font = nullptr;
ImVector<ImWchar> g_ranges;   // must outlive the atlas build
// font -> its advances for 0x80-0xFF before ours, and ours ([acct2-ocr] 2026-11-07: to tell a font loaded again)
struct Saved {
  std::array<int32_t, 128> own{}, ours{};
};
std::map<uint32_t, Saved> g_saved;
std::vector<Line> g_lines;
int g_logged = 0;

bool IsOverlay(std::string_view locale) {
  return std::find(kOverlayLocales.begin(), kOverlayLocales.end(), locale) != kOverlayLocales.end();
}

std::filesystem::path Dir() { return g_root / "text" / "fonts" / "overlay"; }

bool ReadMetrics(std::string_view locale, Metrics* m) {
  std::ifstream in(Dir() / (std::string(locale) + ".overlay.txt"));
  if (!in) return false;
  std::string line;
  while (std::getline(in, line)) {
    if (!line.empty() && line.back() == '\r') line.pop_back();
    if (line.empty() || line[0] == '#') continue;
    std::istringstream s(line);
    std::string what;
    s >> what;
    if (what == "font") s >> m->font_file;
    else if (what == "em") s >> m->em;
    else if (what == "ascent") s >> m->ascent;
    else if (what == "lead") {
      std::string hex;
      float px = 0;
      if (s >> hex >> px) m->lead[int(std::strtol(hex.c_str(), nullptr, 16))] = px;
    }
  }
  return !m->font_file.empty() && m->em > 0;
}

// One code point from UTF-8 (advanced); -1 for a byte that doesn't start one.
int NextCodePoint(const std::string& s, size_t& at) {
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

// Ours: valid UTF-8 with at least one character past Latin-1 (Cyrillic, CJK, Hangul).
bool IsOurs(const std::string& s) {
  bool beyond = false;
  for (size_t at = 0; at < s.size();) {
    const int cp = NextCodePoint(s, at);
    if (cp < 0) return false;
    beyond |= cp >= 0x100;
  }
  return beyond;
}

// What we draw: the game's color codes (^0-^9) left out, and the line-break
// spaces text_language put between CJK characters (text::WithLineBreaks).
std::string Shown(const std::string& s) {
  std::string out;
  int before = 0;   // the code point before this one
  for (size_t i = 0; i < s.size();) {
    if (s[i] == '^' && i + 1 < s.size() && std::isdigit(uint8_t(s[i + 1]))) {
      i += 2;
      continue;
    }
    if (s[i] == ' ' && cod3::text::IsCjk(char32_t(std::max(before, 0)))) {
      size_t next = i + 1;
      const int after = next < s.size() ? NextCodePoint(s, next) : -1;
      if (after >= 0 && cod3::text::IsCjk(char32_t(after))) {
        ++i;
        continue;
      }
    }
    const size_t from = i;
    before = NextCodePoint(s, i);
    out.append(s, from, i - from);
  }
  return out;
}

bool HasBreakSpaces(std::string_view locale) { return locale.starts_with("zh-") || locale == "ja-JP"; }

// The font's advances for the bytes 0x80-0xFF: ours while a language of this
// layer is on, its own otherwise (g_mutex held).
void SetAdvances(uint8_t* base, uint32_t font, bool ours) {
  if (ReadU32(base, font + kFirstCode) != 0x20 || ReadU32(base, font + kCount) != kGlyphs) return;
  // [acct2-ocr] 2026-11-07 The records where the loaded FONT points (+8), as font_fill and text_native read them;
  // right after the header only when it has no pointer.
  const uint32_t p = ReadU32(base, font + kRecordsPtr);
  const uint32_t recs = p >= 0x40000000u && p < 0xF0000000u ? p : font + kRecords;
  auto at = [&](int byte) { return recs + uint32_t(byte - 0x20) * kRecordSize + kAdvance; };
  auto saved = g_saved.find(font);
  // [acct2-ocr] 2026-11-07 A font loaded again (a level load) has its own advances back: what was kept for it is
  // stale, and putting it back would give the font another one's widths.
  if (saved != g_saved.end()) {
    bool still = true;
    for (int b : {0x80, 0x81, 0xA0, 0xBF, 0xC3, 0xD0, 0xE3, 0xEF})
      still = still && ReadS32(base, at(b)) == saved->second.ours[size_t(b - 0x80)];
    if (!still) {
      g_saved.erase(saved);
      saved = g_saved.end();
    }
  }
  if (ours == (saved != g_saved.end())) return;
  if (!ours) {
    for (int b = 0x80; b <= 0xFF; ++b) WriteU32(base, at(b), uint32_t(saved->second.own[size_t(b - 0x80)]));
    g_saved.erase(saved);
    return;
  }
  Saved& keep = g_saved[font];
  for (int b = 0x80; b <= 0xFF; ++b) keep.own[size_t(b - 0x80)] = ReadS32(base, at(b));
  const float scale = float(ReadS32(base, font + kLineHeight)) / kGameLineHeight;
  float fallback = g_metrics.em * 0.6f;
  if (!g_metrics.lead.empty()) {
    fallback = 0;
    for (const auto& [lead, px] : g_metrics.lead) fallback += px;
    fallback /= float(g_metrics.lead.size());
  }
  // Chinese and Japanese: most CJK characters are followed by a line-break
  // space the game measures but we don't draw; their lead bytes (U+3000-U+9FFF,
  // U+FF00-U+FFEF) give that width back.
  const int32_t space = HasBreakSpaces(g_baked) ? ReadS32(base, at(0x20)) : 0;
  for (int b = 0x80; b <= 0xFF; ++b) {
    float px = 0;   // continuation bytes take no room
    if (b >= 0xC0) {
      const auto it = g_metrics.lead.find(b);
      px = it != g_metrics.lead.end() ? it->second : fallback;
    }
    int32_t v = int32_t(std::lround(px * scale));
    if (space > 0 && ((b >= 0xE3 && b <= 0xE9) || b == 0xEF)) v = std::max<int32_t>(1, v - space);
    WriteU32(base, at(b), uint32_t(v));
    keep.ours[size_t(b - 0x80)] = v;
  }
}

}  // namespace

void SetGameDataRoot(const std::filesystem::path& root) {
  std::lock_guard lock(g_mutex);
  g_root = root;
}

bool FontFile(std::string_view locale, std::filesystem::path* file, float* em, float* ascent) {   // [acct2-ocr]
  if (!IsOverlay(locale)) return false;
  std::lock_guard lock(g_mutex);
  Metrics m;
  std::error_code ec;
  if (!ReadMetrics(locale, &m) || !std::filesystem::is_regular_file(Dir() / m.font_file, ec)) return false;
  *file = Dir() / m.font_file;
  *em = m.em;
  *ascent = m.ascent > 0 ? m.ascent : m.em * 0.9f;
  return true;
}

bool Available(std::string_view locale) {
  if (!IsOverlay(locale)) return false;
  std::lock_guard lock(g_mutex);
  Metrics m;
  std::error_code ec;
  return ReadMetrics(locale, &m) && std::filesystem::is_regular_file(Dir() / m.font_file, ec);
}

bool Ready(std::string_view locale) {
  std::lock_guard lock(g_mutex);
  return g_font != nullptr && !g_baked.empty() && g_baked == locale;
}

ImFont* Font() {
  std::lock_guard lock(g_mutex);
  return g_font;
}

void AddFonts(ImFontAtlas* atlas) {
  const std::string locale = cod3::text::Chosen();
  if (!IsOverlay(locale)) return;
  std::lock_guard lock(g_mutex);
  Metrics m;
  if (!ReadMetrics(locale, &m)) {
    REXLOG_WARN("cod3: text layer: no text/fonts/overlay/{}.overlay.txt: {} texts stay in English", locale, locale);
    return;
  }
  std::ifstream in(Dir() / m.font_file, std::ios::binary);
  std::vector<char> bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
  if (bytes.size() < 1024) {
    REXLOG_WARN("cod3: text layer: {} can't be read: {} texts stay in English", m.font_file, locale);
    return;
  }
  // The characters our texts use (and ASCII), nothing more: a CJK atlas stays small.
  ImFontGlyphRangesBuilder builder;
  builder.AddRanges(atlas->GetGlyphRangesDefault());
  std::ifstream pack(g_root / "text" / (locale + ".txt"), std::ios::binary);
  std::string all((std::istreambuf_iterator<char>(pack)), std::istreambuf_iterator<char>());
  builder.AddText(all.c_str(), all.c_str() + all.size());
  builder.BuildRanges(&g_ranges);
  ImFontConfig config;
  const bool cjk = locale != "ru-RU";
  config.OversampleH = cjk ? 1 : 2;
  config.OversampleV = 1;
  config.PixelSnapH = true;
  void* data = IM_ALLOC(bytes.size());   // the atlas owns it
  std::memcpy(data, bytes.data(), bytes.size());
  g_font = atlas->AddFontFromMemoryTTF(data, int(bytes.size()), kBakePixels, &config, g_ranges.Data);
  if (!g_font) {
    REXLOG_WARN("cod3: text layer: {} isn't a font ImGui can use: {} texts stay in English", m.font_file, locale);
    return;
  }
  g_baked = locale;
  g_metrics = std::move(m);
  REXLOG_INFO("cod3: text layer: {} from {} ({} ranges)", locale, g_metrics.font_file, g_ranges.Size / 2);
}

// [acct2-ocr] cod3_debug_text_overlay: each font object once, its texture object and what that points at
// (for drawing our scripts through the game's own text path; dev notes 06:40).
void DumpFont(uint8_t* base, uint32_t font) {
  static std::vector<uint32_t> seen;
  if (std::find(seen.begin(), seen.end(), font) != seen.end() || seen.size() > 16) return;
  seen.push_back(font);
  auto hex = [&](uint32_t at, uint32_t n) {
    std::string out;
    for (uint32_t i = 0; i < n; i += 4) out += fmt::format("{:08X} ", ReadU32(base, at + i));
    return out;
  };
  auto ok = [](uint32_t p) { return p >= 0x40000000u && p < 0xF0000000u; };
  REXLOG_INFO("cod3: text layer: font {:08X}: {}", font, hex(font, 0x40));
  const uint32_t tex = ReadU32(base, font + 4);
  if (!ok(tex)) return;
  REXLOG_INFO("cod3: text layer:  texture {:08X}: {}", tex, hex(tex, 0x80));
  for (uint32_t off : {16u, 20u, 28u, 32u}) {
    const uint32_t p = ReadU32(base, tex + off);
    if (ok(p)) REXLOG_INFO("cod3: text layer:   tex+{} -> {:08X}: {}", off, p, hex(p, 0x40));
  }
}

void OnString(uint8_t* base, PPCContext& ctx) {
  const uint32_t font = ctx.r3.u32, text = ctx.r4.u32;
  if (font < 0x40000000u || font >= 0xF0000000u) return;
  if (REXCVAR_GET(cod3_debug_text_overlay)) DumpFont(base, font);   // [acct2-ocr]
  const std::string chosen = cod3::text::Chosen();
  const bool native = cod3::text_native::Active(chosen);   // [acct2-ocr] before the lock: it reads FontFile()
  std::lock_guard lock(g_mutex);
  // [acct2-ocr] 2026-11-07 (owner 09:49: "swapping languages mid game seems to break the characters shown"): the
  // font's 0x80-0xFF advances are ours only while this layer draws the chosen language. With the language drawn in
  // the game's own text (text_native, the default) our lines are measured and drawn from their own pages, and the
  // rewritten advances only broke the game font's lines: a Latin line still held from a language chosen before
  // ("Objectifs mis à jour.", a name tag) came back squeezed and overlapping after a change back to the language
  // the game started in (its bytes 0x80-0xBF 0 wide, the others as wide as CJK).
  const bool on = g_font != nullptr && g_baked == chosen && !native;
  SetAdvances(base, font, on);
  if (!on || text < 0x40000000u || text >= 0xF0000000u) return;
  if (native) return;   // [acct2-ocr] drawn in the game's own text (ui/text_native.h)
  const std::string s = ReadString(base, text, 1024);
  if (!IsOurs(s)) return;
  Line line;
  line.text = Shown(s);
  line.x = float(ctx.f1.f64);
  line.y = float(ctx.f2.f64);
  line.scale = float(ctx.f5.f64);
  line.line_height = float(ReadS32(base, font + kLineHeight));
  line.ascent = float(ReadS32(base, font + kAscent));
  line.color = ctx.r8.u32;
  line.when = std::chrono::steady_clock::now();
  line.frame = cod3::prompts::GameFrameIn(base);  // [rexglue-native] 2026-11-17 P0
  line.drawn = line.when;                          // [rexglue-native] 2026-11-17 P0
  cod3::prompts::PresentProbeAsked(cod3::prompts::ProbeElement::kText, line.frame);  // [rexglue-native flicker 2026-11-17] (debug)
  if (REXCVAR_GET(cod3_debug_text_overlay) && g_logged < 40) {
    ++g_logged;
    REXLOG_INFO("cod3: text layer: \"{}\" at {:.1f},{:.1f} scale {:.3f}/{:.3f} color {:08X} font {:08X} (line {}, "
                "ascent {})",
                line.text, line.x, line.y, ctx.f4.f64, line.scale, line.color, font, line.line_height, line.ascent);
  }
  if (g_lines.size() > 4096) g_lines.erase(g_lines.begin(), g_lines.begin() + 2048);
  g_lines.push_back(std::move(line));
  ctx.r8.u64 = ctx.r8.u32 & 0x00FFFFFFu;   // the game draws it invisibly (keeps its place)
}

void Draw(float ax, float ay, float aw, float ah) {
  if (aw < 1.0f || ah < 1.0f) return;
  std::vector<Line> lines;
  ImFont* font = nullptr;
  float em = 0, ascent = 0;
  {
    std::lock_guard lock(g_mutex);
    font = g_font;
    em = g_metrics.em;
    ascent = g_metrics.ascent > 0 ? g_metrics.ascent : em * 0.9f;
    const auto now = std::chrono::steady_clock::now();
    // [rexglue-native] 2026-11-17 P0 (owner 16:48: the lines flickered at low frame rates): the latch per game frame (button_prompts.h
    // LatchPick). Each line (its text, place and color) that the game's latest complete frame had (or within its
    // cadence of it: the frames between its newest two copies, 1 to 8) gets a copy last seen now, its frame and draw
    // time kept, so the kShowFor (50 ms) check below passes until the game's next frame is complete; past the safety
    // net (OverlaySafetyMs) the copies stop and that check takes over. A copy is added only when the line's newest is
    // older than half of kShowFor. Off (cod3_overlay_frame_latch) or with the frame unknown: nothing added.
    if (cod3::prompts::OverlayFrameLatchOn()) {
      const cod3::prompts::LatchState latch = cod3::prompts::ReadLatchState();
      constexpr size_t kNone = size_t(-1);
      struct Copies {
        size_t newest, before;  // into g_lines: the copy of the highest frame, of the highest frame below that
        std::chrono::steady_clock::time_point seen;  // the line's newest last-seen time
      };
      std::vector<Copies> each;
      for (size_t i = g_lines.size(); latch.counter != 0 && i-- > 0;) {
        const Line& l = g_lines[i];
        if (now - l.when > 4 * kShowFor) break;   // gone at the erase below anyway
        if (l.frame == 0) continue;
        const auto same = std::find_if(each.begin(), each.end(), [&](const Copies& c) {
          const Line& o = g_lines[c.newest];
          return o.text == l.text && o.x == l.x && o.y == l.y && o.color == l.color;
        });
        if (same == each.end()) {
          each.push_back({i, kNone, l.when});
          continue;
        }
        if (l.when > same->seen) same->seen = l.when;
        const uint32_t top = g_lines[same->newest].frame;
        if (int32_t(l.frame - top) > 0) {
          same->before = same->newest;
          same->newest = i;
        } else if (l.frame != top &&
                   (same->before == kNone || int32_t(l.frame - g_lines[same->before].frame) > 0)) {
          same->before = i;
        }
      }
      const auto age_ms = [&](const Line& l) {
        const auto drawn = l.drawn == std::chrono::steady_clock::time_point{} ? l.when : l.drawn;
        return int64_t(std::chrono::duration_cast<std::chrono::milliseconds>(now - drawn).count());
      };
      std::vector<Line> added;
      for (const Copies& c : each) {
        const Line& n = g_lines[c.newest];
        uint32_t cadence = 1;
        if (c.before != kNone) {
          const uint32_t since = n.frame - g_lines[c.before].frame;
          cadence = since >= 1 && since <= 8 ? since : 1;
        }
        const cod3::prompts::LatchCopy newest{n.frame, age_ms(n), cadence};
        cod3::prompts::LatchCopy before{};
        if (c.before != kNone) before = {g_lines[c.before].frame, age_ms(g_lines[c.before]), cadence};
        const int pick = cod3::prompts::LatchPick(latch, newest, c.before != kNone ? &before : nullptr, 0);
        if (pick == 0 || now - c.seen <= kShowFor / 2) continue;
        Line copy = g_lines[pick == 1 ? c.newest : c.before];
        copy.when = now;
        added.push_back(std::move(copy));
      }
      for (Line& l : added) g_lines.push_back(std::move(l));
    }
    std::erase_if(g_lines, [&](const Line& l) { return now - l.when > 4 * kShowFor; });
    // The newest of each line (the game draws the same lines every frame).
    for (auto it = g_lines.rbegin(); it != g_lines.rend(); ++it) {
      if (now - it->when > kShowFor) break;
      const bool seen = std::any_of(lines.begin(), lines.end(), [&](const Line& l) {
        return l.text == it->text && l.x == it->x && l.y == it->y && l.color == it->color;
      });
      if (!seen) lines.push_back(*it);
    }
    {  // [rexglue-native flicker 2026-11-17] cod3_debug_overlay_latch: the lines shown
      uint64_t h = 1469598103934665603ull;
      for (const Line& l : lines) {
        for (const char ch : l.text) h = (h ^ uint8_t(ch)) * 1099511628211ull;
        h = (h ^ uint64_t(int64_t(std::lround(l.x * 4.0f)))) * 1099511628211ull;
        h = (h ^ uint64_t(int64_t(std::lround(l.y * 4.0f)))) * 1099511628211ull;
        h = (h ^ l.color) * 1099511628211ull;
      }
      cod3::prompts::NoteOverlayShown("text layer", h, lines.size());
    }
  }
  cod3::prompts::PresentProbeShown(cod3::prompts::ProbeElement::kText, font && !lines.empty());  // [rexglue-native flicker 2026-11-17] (debug)
  if (!font || lines.empty()) return;
  const float sx = aw / 640.0f, sy = ah / 480.0f;
  const float dy = float(REXCVAR_GET(cod3_text_overlay_dy)), user = float(REXCVAR_GET(cod3_text_overlay_scale));
  ImDrawList* dl = ImGui::GetBackgroundDrawList();
  for (auto it = lines.rbegin(); it != lines.rend(); ++it) {   // in the game's order
    const Line& l = *it;
    const uint32_t a = l.color >> 24, r = (l.color >> 16) & 0xFF, g = (l.color >> 8) & 0xFF, b = l.color & 0xFF;
    if (a == 0) continue;
    const float px = em * (l.line_height / kGameLineHeight) * l.scale * sy * user;
    if (px < 2.0f) continue;
    // On the game's baseline: its y is the top of the line box, its ascent below
    // that; ImGui puts the font's own ascent (hhea, as in .overlay.txt) above it.
    const float baseline = ay + (l.y + l.ascent * l.scale + dy) * sy;
    const float top = baseline - ascent * (px / em);
    // ^0-^9 / ^: inside the line (a player's name): its runs in their colors (game/color_text.h), codes not drawn.
    namespace cc = cod3::color_codes;
    float x = ax + l.x * sx;
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    int64_t rainbow_n = 0;
    auto put = [&](std::string_view piece, uint32_t rgba) {   // rgba 0xRRGGBBAA, or 0 for the line's own color
      const std::string s(piece);
      const ImU32 col = rgba ? IM_COL32(rgba >> 24, (rgba >> 16) & 0xFF, (rgba >> 8) & 0xFF, a) : IM_COL32(r, g, b, a);
      dl->AddText(font, px, ImVec2(x, top), col, s.c_str());
      x += font->CalcTextSizeA(px, FLT_MAX, 0.0f, s.c_str()).x;
    };
    cc::ForEachRun(l.text, [&](std::string_view run, int code) {
      if (code == cc::kRainbow) {
        for (size_t i = 0; i < run.size();) {   // one code point at a time
          size_t n = 1;
          while (i + n < run.size() && (uint8_t(run[i + n]) & 0xC0) == 0x80) ++n;
          put(run.substr(i, n), cc::RainbowRgba(now, run[i] == ' ' ? rainbow_n : rainbow_n++));
          i += n;
        }
      } else {
        put(run, code >= 0 && code < 10 ? cc::kCodeRgba[code] : 0u);
      }
    });
  }
}

}  // namespace cod3::text_overlay
