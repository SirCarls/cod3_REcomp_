// cod2 - the native renderer's census (dev NATIVE-RENDERER.md, step 1). [acct2-cod2]
//
// Tests only, off by default: with cod2_debug_native_census=N (seconds), the
// campaign logs what its back end draws, so the native renderer's hooks and
// tables can be checked against a running game:
//
// - once, the two tables the back end dispatches through: the surface types'
//   tess functions (0x820389E8) and the render commands' handlers (0x82038A08);
// - every N seconds, a frame's averages over those N seconds: techniques drawn
//   by technique type (RB_DrawSingleTechnique 0x82368F20, r3), their passes
//   (the 16-bit count at technique + 6) and how each pass was drawn (the draw
//   0x82368310: from the tess buffers when its arguments' first word is 0, else
//   indexed), the draw surfaces by surface type and the lists by kind (the
//   draw-surface loop 0x8236EA18: r3 the list, r4 the count, r5 the kind), and
//   the render targets set by id (RB_SetRenderTarget 0x82365828, r3), and the
//   render commands by id with each id's size (the frame's command list, walked
//   at RB_SwapBuffers: the back-end data RB_ExecuteRenderCommands 0x823744E8
//   keeps at 0x8360E4EC, its list at +0x1FBFD0, each command a 16-bit id and
//   its 16-bit size, id 0 the end). A frame is one RB_SwapBuffers (0x82372D78).
//
// - with each of those log lines, the next frame's submission of six draws
//   (draws 0, 60, 120, ... of that frame) as "draw sample" lines: the tess
//   state (technique type 0x83CF88A4, material 0x83CF88A0 and its name), the
//   draw arguments, the pass "RB_SetupPass" 0x82368C40 last set up (r3) with
//   its shaders' names and its argument list (count at +0x10, 8-byte entries
//   at +0x14), and what the back end's caches hold for the draw: the vertex
//   and pixel shaders (0x83D3156C, 0x83D31568), the vertex declaration
//   (0x83D31570), the streams (0x83D314CC + 12 x stream: buffer, offset,
//   stride), the index buffer (0x83D314C8), the textures (0x83D314FC + 4 x
//   stage) and the render target id (0x83D31494), with the first words of
//   each Direct3D object, to map their layouts (NATIVE-RENDERER.md 2.10).
//   [waw-decomp] 2026-11-10, build 18b, for the struct map's open points
//   (X360-GAMES.md, "native step 2"): 40 words of the vertex declaration (its
//   elements are past the first 12) and, for the first texture stage, 16 words
//   of the Direct3D texture the game's image points to (image + 4), where its
//   fetch constant is.
//
// Addresses and registers from the decompile (dev/waw-decomp/cod2_360/sp). The
// hooks only count and read; the game draws as before.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/native_capture.h"  // step N1: the census's hooks also feed the capture
#include "game/native_side.h"     // [waw-decomp] 2026-11-10 step 3: and the 2D side by side

REXCVAR_DEFINE_INT32(cod2_debug_native_census, 0, "cod2/Debug",
                     "Tests: every this many seconds, log what the back end drew a frame (0: off)")
    .range(0, 600);

#if !defined(COD2_MP)

namespace {

constexpr uint32_t kTessTable = 0x820389E8u;     // the surface types' tess functions
constexpr uint32_t kCommandTable = 0x82038A08u;  // the render commands' handlers
constexpr uint32_t kTessMaterial = 0x83CF88A0u;  // the tess state's material
constexpr uint32_t kImageStart = 0x82000000u;
constexpr uint32_t kImageEnd = 0x84290000u;

constexpr uint32_t kTechniqueTypes = 64;
constexpr uint32_t kSurfaceTypes = 16;
constexpr uint32_t kListKinds = 16;
constexpr uint32_t kTargets = 32;
constexpr uint32_t kCommandIds = 64;
constexpr uint32_t kBackendData = 0x8360E4ECu;  // the back-end data being executed
constexpr uint32_t kCommandList = 0x1FBFD0u;    // its command list
constexpr uint32_t kTessTechniqueType = 0x83CF88A4u;
constexpr uint32_t kIndexCache = 0x83D314C8u;    // the index buffer bound (0x82364E88)
constexpr uint32_t kStreamCache = 0x83D314CCu;   // + 12 x stream: buffer, offset, stride (0x82364DC8)
constexpr uint32_t kTextureCache = 0x83D314FCu;  // + 4 x stage: the texture bound (0x82364F68)
constexpr uint32_t kTargetId = 0x83D31494u;      // the render target id set (0x82365828)
constexpr uint32_t kPixelShaderCache = 0x83D31568u;   // 0x82098A88
constexpr uint32_t kVertexShaderCache = 0x83D3156Cu;  // 0x82098B10
constexpr uint32_t kDeclarationCache = 0x83D31570u;   // 0x82098BC8
constexpr int kSamplesPerWindow = 6;
constexpr uint32_t kSampleEvery = 60;  // draws 0, 60, 120, ... of the sampled frame

struct Counts {
  std::array<std::atomic<uint32_t>, kTechniqueTypes> techniques{};
  std::array<std::atomic<uint32_t>, kSurfaceTypes + 1> surfaces{};  // the last: other types
  std::array<std::atomic<uint32_t>, kListKinds + 1> lists{};        // the last: other kinds
  std::array<std::atomic<uint32_t>, kTargets + 1> targets{};        // the last: other ids
  std::atomic<uint32_t> passes{0};
  std::atomic<uint32_t> draws_from_tess{0};
  std::atomic<uint32_t> draws_indexed{0};
  std::atomic<uint32_t> frames{0};
  std::array<std::atomic<uint32_t>, kCommandIds + 1> commands{};  // the last: other ids
  std::array<std::atomic<uint32_t>, kCommandIds> command_size{};  // the last size seen per id
  std::atomic<uint32_t> command_bytes{0};
};

Counts g_counts;
std::atomic<bool> g_tables_logged{false};
std::atomic<bool> g_sample_frame{false};   // the next frame's draws are sampled
std::atomic<int> g_samples{0};
std::atomic<uint32_t> g_frame_draws{0};
std::atomic<uint32_t> g_last_pass{0};      // RB_SetupPass's r3, r4
std::atomic<uint32_t> g_last_pass_format{0};
std::atomic<uint64_t> g_window_start_ms{0};

bool On() { return REXCVAR_GET(cod2_debug_native_census) > 0; }

bool IsGuestData(uint32_t a) {
  return (a >= 0x40000000u && a < 0x7FFF0000u) || (a >= kImageStart && a < kImageEnd) ||
         (a >= 0xA0000000u && a < 0xFFFF0000u);
}

uint32_t LoadU32(const uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, base + addr, 4);
  return __builtin_bswap32(v);
}

uint16_t LoadU16(const uint8_t* base, uint32_t addr) {
  uint16_t v;
  std::memcpy(&v, base + addr, 2);
  return __builtin_bswap16(v);
}

void Add(std::atomic<uint32_t>& c, uint32_t n = 1) { c.fetch_add(n, std::memory_order_relaxed); }

// The table's entries (at most `max`): empty ones are skipped, and it ends at
// the first entry that is neither empty nor in the program image.
std::string TableText(const uint8_t* base, uint32_t table, uint32_t max) {
  std::string out;
  for (uint32_t i = 0; i < max; ++i) {
    const uint32_t entry = LoadU32(base, table + 4u * i);
    if (entry == 0) continue;
    if (entry < kImageStart || entry >= kImageEnd) break;
    char item[24];
    std::snprintf(item, sizeof item, " %u:%08X", i, entry);
    out += item;
  }
  return out.empty() ? " (none)" : out;
}

// " i:average" for each non-zero counter, averaged over `frames`, then reset.
template <size_t N>
std::string Spread(std::array<std::atomic<uint32_t>, N>& counters, uint32_t frames, bool last_is_other) {
  std::string out;
  for (size_t i = 0; i < N; ++i) {
    const uint32_t n = counters[i].exchange(0, std::memory_order_relaxed);
    if (n == 0) continue;
    char item[32];
    if (last_is_other && i + 1 == N) {
      std::snprintf(item, sizeof item, " other:%.1f", double(n) / frames);
    } else {
      std::snprintf(item, sizeof item, " %zu:%.1f", i, double(n) / frames);
    }
    out += item;
  }
  return out.empty() ? " -" : out;
}

// The frame's render commands: counted by id, each id's size kept. Stops at the
// end (id 0), or at a size that can't be a command's (the layout not as read).
void CountCommands(const uint8_t* base) {
  const uint32_t data = LoadU32(base, kBackendData);
  if (!IsGuestData(data)) return;
  uint32_t p = data + kCommandList;
  for (int i = 0; i < 16384; ++i) {
    const uint16_t id = LoadU16(base, p);
    if (id == 0) return;
    const uint16_t size = LoadU16(base, p + 2);
    Add(g_counts.commands[id < kCommandIds ? id : kCommandIds]);
    if (id < kCommandIds) g_counts.command_size[id].store(size, std::memory_order_relaxed);
    if (size < 4 || (size & 3) != 0) return;
    Add(g_counts.command_bytes, size);
    p += size;
  }
}

// " id:average(size)" for each id seen, averaged over `frames`, then reset.
std::string CommandSpread(uint32_t frames) {
  std::string out;
  for (uint32_t i = 0; i <= kCommandIds; ++i) {
    const uint32_t n = g_counts.commands[i].exchange(0, std::memory_order_relaxed);
    if (n == 0) continue;
    char item[48];
    if (i == kCommandIds)
      std::snprintf(item, sizeof item, " other:%.1f", double(n) / frames);
    else
      std::snprintf(item, sizeof item, " %u:%.1f(%u)", i, double(n) / frames,
                    g_counts.command_size[i].load(std::memory_order_relaxed));
    out += item;
  }
  return out.empty() ? " -" : out;
}

// n words at addr as " XXXXXXXX ...", or " -" when addr isn't guest data.
std::string Words(const uint8_t* base, uint32_t addr, uint32_t n) {
  if (n == 0) return "";
  if (!IsGuestData(addr) || !IsGuestData(addr + 4u * n)) return " -";
  std::string out;
  for (uint32_t i = 0; i < n; ++i) {
    char w[12];
    std::snprintf(w, sizeof w, " %08X", LoadU32(base, addr + 4u * i));
    out += w;
  }
  return out;
}

// The text at addr (printable, at most 63 characters), "-" when none, "?" when not text.
std::string Text(const uint8_t* base, uint32_t addr) {
  if (!IsGuestData(addr) || !IsGuestData(addr + 64u)) return "-";
  std::string out;
  for (uint32_t i = 0; i < 63; ++i) {
    const unsigned char c = base[addr + i];
    if (c == 0) return out;
    if (c < 32 || c > 126) return "?";
    out += char(c);
  }
  return out;
}

// One draw's submission, as the back end's caches hold it when the draw starts.
void LogDrawSample(const uint8_t* base, uint32_t args, uint32_t draw, int sample) {
  const uint32_t material = LoadU32(base, kTessMaterial);
  const uint32_t pass = g_last_pass.load(std::memory_order_relaxed);
  std::string vs_name = "-", ps_name = "-", vs_words = " -", ps_words = " -", arguments = " -";
  if (IsGuestData(pass)) {
    const uint32_t vs = LoadU32(base, pass + 8u), ps = LoadU32(base, pass + 0xCu);
    if (IsGuestData(vs)) {
      vs_name = Text(base, LoadU32(base, vs));
      vs_words = Words(base, vs, 4);
    }
    if (IsGuestData(ps)) {
      ps_name = Text(base, LoadU32(base, ps));
      ps_words = Words(base, ps, 4);
    }
    const uint32_t count = LoadU16(base, pass + 0x10u);
    arguments = " " + std::to_string(count) + ":" + Words(base, LoadU32(base, pass + 0x14u), 2u * std::min(count, 8u));
  }
  const uint32_t vs_d3d = LoadU32(base, kVertexShaderCache), ps_d3d = LoadU32(base, kPixelShaderCache);
  const uint32_t decl = LoadU32(base, kDeclarationCache);
  REXLOG_INFO(
      "cod2: native census: draw sample {} (draw {} of the frame): technique type {}, material {:08X} \"{}\" ({}), "
      "target {}, arguments {:08X}:{}; pass {:08X} (vertex format {}):{}; vertex shader \"{}\" ({}), pixel shader "
      "\"{}\" ({}); its arguments{}",
      sample, draw, LoadU32(base, kTessTechniqueType), material, IsGuestData(material) ? Text(base, LoadU32(base, material)) : "-",
      Words(base, material, 8), LoadU32(base, kTargetId), args, Words(base, args, 6), pass,
      g_last_pass_format.load(std::memory_order_relaxed), Words(base, pass, 7), vs_name, vs_words, ps_name, ps_words,
      arguments);
  std::string streams;
  for (uint32_t s = 0; s < 2; ++s) {
    const uint32_t at = kStreamCache + 12u * s;
    const uint32_t buffer = LoadU32(base, at);
    if (buffer == 0) continue;
    char head[64];
    std::snprintf(head, sizeof head, " %u: %08X +%u stride %u:", s, buffer, LoadU32(base, at + 4u), LoadU32(base, at + 8u));
    streams += head + Words(base, buffer, 8) + ";";
  }
  std::string textures;
  bool first = true;
  for (uint32_t t = 0; t < 16; ++t) {
    const uint32_t texture = LoadU32(base, kTextureCache + 4u * t);
    if (texture == 0) continue;
    char head[32];
    std::snprintf(head, sizeof head, " %u:%08X", t, texture);
    textures += head;
    if (first) {
      textures += " (" + Words(base, texture, 13) + " )";
      // [waw-decomp] build 18b: the Direct3D texture the image points to (image + 4).
      const uint32_t d3d_texture = LoadU32(base, texture + 4u);
      char object[24];
      std::snprintf(object, sizeof object, " texture %08X:", d3d_texture);
      textures += object + Words(base, d3d_texture, 16) + ";";
      first = false;
    }
  }
  const uint32_t indices = LoadU32(base, kIndexCache);
  // The Direct3D shader objects (0x8209BE90 / 0x8209BB00): the vertex shader's own constants at +0x250 (their
  // size at +0x25C) and a pointer at +0x28; the pixel shader's constants at +0x34 (size at +0x40).
  REXLOG_INFO(
      "cod2: native census: draw sample {} buffers: vertex shader {:08X}:{} | +0x248:{}; pixel shader {:08X}:{}; "
      "declaration {:08X}:{}; streams{}; indices {:08X}:{}; textures{}",
      sample, vs_d3d, Words(base, vs_d3d, 12), Words(base, vs_d3d + 0x248u, 6), ps_d3d, Words(base, ps_d3d, 18),
      decl, Words(base, decl, 40),  // [waw-decomp] build 18b: 40 words (was 12), to reach the elements
      streams.empty() ? std::string(" -") : streams, indices, Words(base, indices, 8),
      textures.empty() ? std::string(" -") : textures);
}

uint64_t NowMs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

}  // namespace

// RB_DrawSingleTechnique(technique type r3, vertex format r4, draw arguments r5).
REX_EXTERN(__imp__sub_82368F20);
REX_HOOK_RAW(sub_82368F20) {
  if (On()) {
    const uint32_t type = ctx.r3.u32;
    Add(g_counts.techniques[type < kTechniqueTypes ? type : kTechniqueTypes - 1]);
    const uint32_t material = LoadU32(base, kTessMaterial);
    if (IsGuestData(material) && type < kTechniqueTypes) {
      const uint32_t techset = LoadU32(base, material + 0x20);
      if (IsGuestData(techset)) {
        const uint32_t technique = LoadU32(base, techset + 4u * (type + 1));
        if (IsGuestData(technique)) Add(g_counts.passes, LoadU16(base, technique + 6));
      }
    }
  }
  __imp__sub_82368F20(ctx, base);
}

// The draw of one pass (draw arguments r3): from the tess buffers when the
// arguments' first word is 0, else from the bound vertex and index buffers.
REX_EXTERN(__imp__sub_82368310);
REX_HOOK_RAW(sub_82368310) {
  cod2::native::CaptureDraw(base, ctx.r3.u32, g_last_pass.load(std::memory_order_relaxed));
  cod2::native::SideDraw(base, ctx.r3.u32, g_last_pass.load(std::memory_order_relaxed));  // [waw-decomp] 2026-11-10
  if (On() && IsGuestData(ctx.r3.u32)) {
    Add(LoadU32(base, ctx.r3.u32) == 0 ? g_counts.draws_from_tess : g_counts.draws_indexed);
    const uint32_t draw = g_frame_draws.fetch_add(1, std::memory_order_relaxed);
    if (g_sample_frame.load(std::memory_order_relaxed) && draw % kSampleEvery == 0) {
      const int sample = g_samples.fetch_add(1, std::memory_order_relaxed);
      if (sample < kSamplesPerWindow) LogDrawSample(base, ctx.r3.u32, draw, sample);
    }
  }
  __imp__sub_82368310(ctx, base);
}

// "RB_SetupPass"(pass r3, vertex format r4): kept for the draw samples.
REX_EXTERN(__imp__sub_82368C40);
REX_HOOK_RAW(sub_82368C40) {
  if (On() || cod2::native::CaptureOn() || cod2::native::SideOn()) {  // [waw-decomp] SideOn: step 4's census
    g_last_pass.store(ctx.r3.u32, std::memory_order_relaxed);
    g_last_pass_format.store(ctx.r4.u32, std::memory_order_relaxed);
  }
  __imp__sub_82368C40(ctx, base);
}

// The draw-surface loop (list r3, count r4, r5): each draw surface is 8 bytes,
// a sort key and a pointer to the surface, whose first word is its type. r5 is
// counted as the list's kind when it is small (16 and up count as "other": in
// Moscow most lists pass a larger value there, so it may be a pointer).
REX_EXTERN(__imp__sub_8236EA18);
REX_HOOK_RAW(sub_8236EA18) {
  if (On()) {
    const uint32_t list = ctx.r3.u32, count = ctx.r4.u32, kind = ctx.r5.u32;
    Add(g_counts.lists[kind < kListKinds ? kind : kListKinds]);
    if (IsGuestData(list) && count < 65536) {
      for (uint32_t i = 0; i < count; ++i) {
        const uint32_t surface = LoadU32(base, list + 8u * i + 4u);
        const uint32_t type = IsGuestData(surface) ? LoadU32(base, surface) : 0xFFFFFFFFu;
        Add(g_counts.surfaces[type < kSurfaceTypes ? type : kSurfaceTypes]);
      }
    }
  }
  __imp__sub_8236EA18(ctx, base);
}

// RB_SetRenderTarget(id r3).
REX_EXTERN(__imp__sub_82365828);
REX_HOOK_RAW(sub_82365828) {
  if (On()) Add(g_counts.targets[ctx.r3.u32 < kTargets ? ctx.r3.u32 : kTargets]);
  __imp__sub_82365828(ctx, base);
}

// RB_SwapBuffers: one frame; the log line every cod2_debug_native_census seconds.
REX_EXTERN(__imp__sub_82372D78);
REX_HOOK_RAW(sub_82372D78) {
  if (On()) CountCommands(base);
  __imp__sub_82372D78(ctx, base);
  cod2::native::CaptureFrameEnd();
  cod2::native::SideFrameEnd(base);  // [waw-decomp] 2026-11-10 (cod2_debug_native_side)
  g_frame_draws.store(0, std::memory_order_relaxed);
  if (g_sample_frame.load(std::memory_order_relaxed) && g_samples.load(std::memory_order_relaxed) > 0)
    g_sample_frame.store(false, std::memory_order_relaxed);
  if (!On()) return;
  if (!g_tables_logged.exchange(true)) {
    REXLOG_INFO("cod2: native census: tess functions by surface type (0x{:08X}):{}", kTessTable,
                TableText(base, kTessTable, kSurfaceTypes));
    REXLOG_INFO("cod2: native census: render command handlers by id (0x{:08X}):{}", kCommandTable,
                TableText(base, kCommandTable, 64));
  }
  Add(g_counts.frames);
  const uint64_t now = NowMs();
  const uint64_t start = g_window_start_ms.load(std::memory_order_relaxed);
  if (start == 0) {
    g_window_start_ms.store(now, std::memory_order_relaxed);
    return;
  }
  const uint64_t window = static_cast<uint64_t>(REXCVAR_GET(cod2_debug_native_census)) * 1000u;
  if (now - start < window) return;
  g_window_start_ms.store(now, std::memory_order_relaxed);
  const uint32_t frames = std::max<uint32_t>(1, g_counts.frames.exchange(0));
  const double passes = double(g_counts.passes.exchange(0)) / frames;
  const double from_tess = double(g_counts.draws_from_tess.exchange(0)) / frames;
  const double indexed = double(g_counts.draws_indexed.exchange(0)) / frames;
  const std::string techniques = Spread(g_counts.techniques, frames, false);
  const std::string surfaces = Spread(g_counts.surfaces, frames, true);
  const std::string lists = Spread(g_counts.lists, frames, true);
  const std::string targets = Spread(g_counts.targets, frames, true);
  const double command_bytes = double(g_counts.command_bytes.exchange(0)) / frames;
  REXLOG_INFO("cod2: native census: render commands a frame by id (size in bytes), {:.0f} bytes:{}", command_bytes,
              CommandSpread(frames));
  REXLOG_INFO(
      "cod2: native census: over {} frames, a frame: {:.1f} passes in the techniques, {:.1f} pass draws from the "
      "tess buffers and {:.1f} indexed; techniques by type:{}; draw surfaces by type:{}; lists by kind:{}; render "
      "targets set by id:{}",
      frames, passes, from_tess, indexed, techniques, surfaces, lists, targets);
  g_samples.store(0, std::memory_order_relaxed);
  g_sample_frame.store(true, std::memory_order_relaxed);
}

#endif  // !COD2_MP
