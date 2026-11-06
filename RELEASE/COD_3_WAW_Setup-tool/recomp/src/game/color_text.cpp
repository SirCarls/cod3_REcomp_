// cod3 - color codes inside a line (see color_text.h). [acct2-ocr]

#include "game/color_text.h"

#include <chrono>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_set>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include "game/color_codes.h"
#include "game/guest.h"

REXCVAR_DEFINE_BOOL(cod3_color_codes_in_text, true, "cod3/Game",
                    "Draw ^0-^9 and ^: (rainbow) color codes anywhere in a line, as World at War does (names in the "
                    "kill feed, scoreboard and chat); off: Call of Duty 3's own (only a code at a line's start)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);
REXCVAR_DEFINE_BOOL(cod3_debug_color_codes, false, "cod3/Game", "Log each line drawn with color codes (once)")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace cod3::color_text {

namespace {

constexpr uint32_t kFontFirst = 0x24, kFontCount = 0x28, kRecords = 0x38, kRecordSize = 28, kAdvance = 24;
constexpr uint32_t kRing = 256 * 1024;
constexpr size_t kMaxLine = 1024;

std::mutex g_mutex;   // the ring and the log set
uint32_t g_ring = 0, g_ring_at = 0;
std::unordered_set<std::string> g_logged;

// A NUL-terminated copy of `s` in the guest ring; 0 when there is no ring.
uint32_t RingCopy(uint8_t* base, std::string_view s) {
  const uint32_t need = (uint32_t(s.size()) + 1 + 3) & ~3u;
  if (!g_ring) g_ring = rex::system::kernel_state()->memory()->SystemHeapAlloc(kRing);
  if (!g_ring || need > kRing) return 0;
  if (g_ring_at + need > kRing) g_ring_at = 0;
  const uint32_t at = g_ring + g_ring_at;
  std::memcpy(base + at, s.data(), s.size());
  base[at + s.size()] = 0;
  g_ring_at += need;
  return at;
}

// 0xAARRGGBB with the line's alpha and a code's color (color_codes.h's 0xRRGGBBAA).
uint32_t WithAlpha(uint32_t line_argb, uint32_t rgba) { return (line_argb & 0xFF000000u) | (rgba >> 8); }

double Seconds() {
  using namespace std::chrono;
  return duration<double>(steady_clock::now().time_since_epoch()).count();
}

}  // namespace

bool DrawRuns(uint8_t* base, PPCContext& ctx, DrawFn draw) {
  namespace cc = cod3::color_codes;
  if (!REXCVAR_GET(cod3_color_codes_in_text) || ctx.r4.u32 == 0 || ctx.r3.u32 == 0) return false;
  const std::string text = cod3::guest::ReadString(base, ctx.r4.u32, kMaxLine);
  if (text.find('^') == std::string::npos) return false;
  bool coded = false;
  for (size_t i = 0; i < text.size() && !coded; ++i) coded = cc::IsCode(text, i);
  if (!coded) return false;

  const uint32_t font = ctx.r3.u32;
  const uint32_t first = cod3::guest::ReadU32(base, font + kFontFirst);
  const uint32_t count = cod3::guest::ReadU32(base, font + kFontCount);
  if (count == 0 || count > 256 || first > 0xFF) return false;   // not a FONT we know: as it is
  auto advance = [&](std::string_view run) {
    int32_t sum = 0;
    for (unsigned char c : run)
      if (c >= first && c < first + count)
        sum += cod3::guest::ReadS32(base, font + kRecords + (c - first) * kRecordSize + kAdvance);
    return float(sum);
  };

  const PPCContext saved = ctx;
  const uint32_t line_color = saved.r8.u32;
  const float x_scale = float(saved.f4.f64);
  float x = float(saved.f1.f64);
  const double now = Seconds();
  int64_t rainbow_n = 0;
  std::lock_guard lock(g_mutex);
  if (REXCVAR_GET(cod3_debug_color_codes) && g_logged.size() < 256 && g_logged.insert(text).second)
    REXLOG_INFO("cod3: color codes: \"{}\" (color {:08X})", text, line_color);
  auto put = [&](std::string_view piece, uint32_t argb) {
    if (piece.empty()) return;
    const uint32_t at = RingCopy(base, piece);
    if (!at) return;
    ctx = saved;
    ctx.r4.u64 = at;
    ctx.r8.u64 = argb;
    ctx.f1.f64 = x;
    draw(ctx, base);
    x += advance(piece) * x_scale;
  };
  cc::ForEachRun(text, [&](std::string_view run, int code) {
    if (code == cc::kRainbow) {
      for (size_t i = 0; i < run.size(); ++i) {
        const uint32_t rgba = cc::RainbowRgba(now, run[i] == ' ' ? rainbow_n : rainbow_n++);
        put(run.substr(i, 1), WithAlpha(line_color, rgba));
      }
    } else {
      put(run, code >= 0 && code < 10 ? WithAlpha(line_color, cc::kCodeRgba[code]) : line_color);
    }
  });
  return true;
}

}  // namespace cod3::color_text
