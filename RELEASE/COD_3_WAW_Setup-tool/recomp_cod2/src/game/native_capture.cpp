// cod2 - the native renderer's step N1: the capture of each draw's submission. [acct2-cod2]
//
// Off by default. With cod2_native_capture=N (seconds), every frame the campaign records each draw's whole
// submission into a frame record, from the back end's own caches (dev NATIVE-RENDERER.md 2.10), with no
// Direct3D decoding and no drawing: this is what the native path will submit to ngl_vk_low_draw.
//
// One record a draw (at the draw 0x82368310, arguments r3, before the game's own code runs):
//   - the draw's arguments: from the tess buffers when their first word is 0 (an "UP" draw: the tess
//     buffers' vertex and index counts 0x83CF88B8 / 0x83CF88B4), else indexed (+0xC the triangle count,
//     +0x10 and +0x14 the index and vertex ranges);
//   - the tess state: technique type 0x83CF88A4, material 0x83CF88A0, and the pass "RB_SetupPass"
//     0x82368C40 last set up (the census keeps it);
//   - the caches: vertex and pixel shaders 0x83D3156C / 0x83D31568, the declaration 0x83D31570, the
//     streams 0x83D314CC + 12 x stream (buffer, offset, stride), the index buffer 0x83D314C8, the textures
//     0x83D314FC + 4 x slot, the render state caches 0x83D31480 and 0x83D31488 (word 0) and 0x83D3148C
//     (word 1), the render target id 0x83D31494;
//   - the shader constants the pass sets: its argument list (count: the 16-bit word at pass +0x10; 8-byte
//     entries at +0x14: 16-bit kind, 16-bit register, 32-bit value), each register's 16 bytes read from the
//     constant mirror 0x83D2F480 (16 bytes a register, 512 registers up to 0x83D31480). Which registers
//     are the pixel shader's is for build 18's samples to show; the register is kept as the list names it.
//
// At RB_SwapBuffers 0x82372D78 the record becomes the last whole frame, and every N seconds one summary
// line (how many draws had each piece, the distinct objects) and two of the frame's records are logged.
// N1 is done when Moscow gives 355-413 records a frame (the census's passes), each with shaders, a
// declaration, a stream, indices and a target. The hooks are the census's (one hook a guest function);
// they call CaptureDraw and CaptureFrameEnd. Addresses from the decompile (dev/waw-decomp/cod2_360/sp).
//
// [waw-decomp] 2026-11-09, step N2 (the struct map) for the draws from the tess buffers: N1 found they have no index
// buffer and stream 0 is unset or stale for them, because they don't use either. The draw hands them to 0x820A6F30,
// an "UP" draw that copies vertices and indices into the ring itself: the vertices from the tess buffers' start
// 0x83CE8880 (first vertex args + 4, count args + 8, stride args + 0x10), the 16-bit indices from the pointer at
// 0x83CF8894 (the tess state's +0x10014), 3 x the triangle count of them. Each such record keeps those now.

#include "game/native_capture.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

REXCVAR_DEFINE_INT32(cod2_native_capture, 0, "cod2/Native",
                     "Native renderer step N1: record each draw's submission every frame, and log a summary of "
                     "one frame every this many seconds (0: off)")
    .range(0, 600);

#if !defined(COD2_MP)

namespace cod2::native {
namespace {

constexpr uint32_t kTessMaterial = 0x83CF88A0u;       // the tess state's material
constexpr uint32_t kTessTechniqueType = 0x83CF88A4u;  // and technique type
// [waw-decomp] 2026-11-10: the stretch pic 0x82371C00 writes its vertices at 0x83CE8880 + 0x40 x the word at 0x83CF88B8
// and its indices (that word + 0, 1, 2, 3) at the index pointer + 2 x the word at 0x83CF88B4, flushing past 0x400 and
// 0x800 of them: so 0x83CF88B8 is the vertex count and 0x83CF88B4 the index count (this file had 0x83CF88B4 and
// 0x83CF88C4; N1's "1,488 tess vertices" for 496 triangles were the indices). 0x83CF88C4 is another word the render
// commands test before they flush (0x8236DFB8), not known yet.
constexpr uint32_t kTessVertexCount = 0x83CF88B8u;    // the tess buffers' vertices
constexpr uint32_t kTessIndexCount = 0x83CF88B4u;     // and indices
constexpr uint32_t kTessVertices = 0x83CE8880u;       // the tess buffers' vertices (the tess state is 0x10000 on)
constexpr uint32_t kTessIndexPointer = 0x83CF8894u;   // the indices an "UP" draw copies (the state's +0x10014)
constexpr uint32_t kIndexCache = 0x83D314C8u;         // the index buffer bound (0x82364E88)
constexpr uint32_t kStreamCache = 0x83D314CCu;        // + 12 x stream: buffer, offset, stride (0x82364DC8)
constexpr uint32_t kTextureCache = 0x83D314FCu;       // + 4 x slot: the texture bound (0x82364F68)
constexpr uint32_t kTargetId = 0x83D31494u;           // the render target id set (0x82365828)
constexpr uint32_t kPixelShaderCache = 0x83D31568u;   // 0x82098A88
constexpr uint32_t kVertexShaderCache = 0x83D3156Cu;  // 0x82098B10
constexpr uint32_t kDeclarationCache = 0x83D31570u;   // 0x82098BC8
constexpr std::array<uint32_t, 3> kStateCaches = {0x83D31480u, 0x83D31488u, 0x83D3148Cu};
constexpr uint32_t kConstantMirror = 0x83D2F480u;     // 16 bytes a register (0x82367CB0)
constexpr uint32_t kMirrorRegisters = 512;
constexpr uint32_t kImageStart = 0x82000000u;
constexpr uint32_t kImageEnd = 0x84290000u;

constexpr size_t kStreams = 4;        // ngl_vk_low_draw takes up to four
constexpr size_t kTextureSlots = 16;  // and 16 texture slots
constexpr size_t kMaxDraws = 8192;    // a frame; more are counted as dropped
constexpr size_t kMaxConstants = 131072;
constexpr uint32_t kMaxConstantsPerDraw = 64;

struct Stream {
  uint32_t buffer = 0, offset = 0, stride = 0;
};

struct Constant {
  uint16_t kind = 0;                // 0 a literal, 1 a code constant, 2 a material constant
  uint16_t reg = 0;                 // the register the pass's argument names
  std::array<uint32_t, 4> bits{};   // the mirror's four words for it (host order)
};

struct DrawRecord {
  uint32_t args = 0;                // the draw's arguments (r3 of 0x82368310)
  bool from_tess = false;           // their first word is 0: from the tess buffers
  uint32_t triangles = 0;           // args + 0xC
  uint32_t index_range = 0;         // args + 0x10 (indexed draws)
  uint32_t vertex_range = 0;        // args + 0x14 (indexed draws)
  uint32_t tess_vertices = 0;       // the tess buffers' counts (draws from them)
  uint32_t tess_indices = 0;
  // N2, draws from the tess buffers: what the draw hands to the "UP" draw 0x820A6F30.
  uint32_t up_vertices = 0;         // kTessVertices + first vertex x stride
  uint32_t up_first_vertex = 0;     // args + 4
  uint32_t up_vertex_count = 0;     // args + 8
  uint32_t up_stride = 0;           // args + 0x10
  uint32_t up_indices = 0;          // the pointer at kTessIndexPointer: 16-bit indices
  uint32_t up_index_count = 0;      // 3 x the triangle count
  uint32_t technique_type = 0;
  uint32_t material = 0;
  uint32_t pass = 0;
  uint32_t vertex_shader = 0;
  uint32_t pixel_shader = 0;
  uint32_t declaration = 0;
  std::array<Stream, kStreams> streams{};
  uint32_t index_buffer = 0;
  std::array<uint32_t, kTextureSlots> textures{};
  std::array<uint32_t, 3> state{};  // the caches 0x83D31480, 0x83D31488 (word 0), 0x83D3148C (word 1)
  uint32_t target = 0;
  uint32_t first_constant = 0;      // into Frame::constants
  uint32_t constant_count = 0;
};

struct Frame {
  std::vector<DrawRecord> draws;
  std::vector<Constant> constants;
  uint64_t number = 0;
  uint32_t dropped = 0;

  void Clear() {
    draws.clear();
    constants.clear();
    dropped = 0;
  }
};

// Both frames are touched only by the render thread (the draws and RB_SwapBuffers run there); a call
// from any other thread is counted and skipped.
Frame g_recording;
Frame g_last;
uint64_t g_frames = 0;
uint64_t g_last_log_ms = 0;
std::atomic<uint32_t> g_other_thread{0};
std::atomic<bool> g_have_thread{false};
std::thread::id g_thread;
std::mutex g_thread_mutex;

uint64_t NowMs() {
  return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
                                   std::chrono::steady_clock::now().time_since_epoch())
                                   .count());
}

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

// The first thread to call is the render thread.
bool OnRenderThread() {
  const std::thread::id self = std::this_thread::get_id();
  if (!g_have_thread.load(std::memory_order_acquire)) {
    std::lock_guard<std::mutex> lock(g_thread_mutex);
    if (!g_have_thread.load(std::memory_order_relaxed)) {
      g_thread = self;
      g_have_thread.store(true, std::memory_order_release);
    }
  }
  return g_thread == self;
}

void ReadConstants(const uint8_t* base, uint32_t pass, Frame& frame, DrawRecord& draw) {
  draw.first_constant = static_cast<uint32_t>(frame.constants.size());
  if (!IsGuestData(pass)) return;
  const uint32_t list = LoadU32(base, pass + 0x14u);
  if (!IsGuestData(list)) return;
  const uint32_t count = std::min<uint32_t>(LoadU16(base, pass + 0x10u), kMaxConstantsPerDraw);
  for (uint32_t i = 0; i < count && frame.constants.size() < kMaxConstants; ++i) {
    const uint32_t entry = list + 8u * i;
    Constant c;
    c.kind = LoadU16(base, entry);
    c.reg = LoadU16(base, entry + 2u);
    if (c.reg >= kMirrorRegisters) continue;
    for (uint32_t w = 0; w < 4; ++w) c.bits[w] = LoadU32(base, kConstantMirror + 16u * c.reg + 4u * w);
    frame.constants.push_back(c);
  }
  draw.constant_count = static_cast<uint32_t>(frame.constants.size()) - draw.first_constant;
}

size_t Distinct(std::vector<uint32_t> values) {
  values.erase(std::remove(values.begin(), values.end(), 0u), values.end());
  std::sort(values.begin(), values.end());
  return static_cast<size_t>(std::unique(values.begin(), values.end()) - values.begin());
}

std::string RecordText(const Frame& frame, size_t i) {
  const DrawRecord& d = frame.draws[i];
  std::string textures;
  for (size_t t = 0; t < kTextureSlots; ++t) {
    if (d.textures[t] == 0) continue;
    char one[24];
    std::snprintf(one, sizeof one, " %zu:%08X", t, d.textures[t]);
    textures += one;
  }
  char vertices[192];
  if (d.from_tess) {
    std::snprintf(vertices, sizeof vertices, "UP vertices %08X (first %u, %u of stride %u), indices %08X (%u)",
                  d.up_vertices, d.up_first_vertex, d.up_vertex_count, d.up_stride, d.up_indices, d.up_index_count);
  } else {
    std::snprintf(vertices, sizeof vertices, "stream 0 %08X +%u stride %u, stream 1 %08X; index buffer %08X",
                  d.streams[0].buffer, d.streams[0].offset, d.streams[0].stride, d.streams[1].buffer, d.index_buffer);
  }
  char text[768];
  std::snprintf(text, sizeof text,
                "draw %zu: %s, %u triangles, %s %u/%u; technique type %u, material %08X, pass %08X; vertex shader "
                "%08X, pixel shader %08X, declaration %08X; %s; state %08X %08X %08X; target %u; %u constants; "
                "textures%s",
                i, d.from_tess ? "from the tess buffers" : "indexed", d.triangles,
                d.from_tess ? "tess vertices/indices" : "index/vertex ranges",
                d.from_tess ? d.tess_vertices : d.index_range, d.from_tess ? d.tess_indices : d.vertex_range,
                d.technique_type, d.material, d.pass, d.vertex_shader, d.pixel_shader, d.declaration, vertices,
                d.state[0], d.state[1], d.state[2], d.target, d.constant_count,
                textures.empty() ? " none" : textures.c_str());
  return text;
}

void LogSummary(const Frame& frame) {
  size_t from_tess = 0, shaders = 0, declarations = 0, stream0 = 0, indices = 0, targets = 0, textures = 0;
  size_t up_complete = 0;  // tess draws with UP vertices (a count and a stride) and an index pointer
  std::vector<uint32_t> vs, ps, decls, materials, target_ids;
  for (const DrawRecord& d : frame.draws) {
    from_tess += d.from_tess;
    up_complete += d.from_tess && d.up_vertex_count != 0 && d.up_stride != 0 && IsGuestData(d.up_indices);
    shaders += d.vertex_shader != 0 && d.pixel_shader != 0;
    declarations += d.declaration != 0;
    stream0 += d.streams[0].buffer != 0;
    indices += d.from_tess ? d.tess_indices != 0 : d.index_buffer != 0;
    targets += 1;  // the id is always set; 0 is a real target (the back buffer)
    for (uint32_t t : d.textures) textures += t != 0;
    vs.push_back(d.vertex_shader);
    ps.push_back(d.pixel_shader);
    decls.push_back(d.declaration);
    materials.push_back(d.material);
    target_ids.push_back(d.target + 1u);  // so that target 0 counts as distinct
  }
  const size_t n = frame.draws.size();
  const double per = n ? 1.0 / double(n) : 0.0;
  REXLOG_INFO(
      "cod2: native capture: frame {}: {} draws ({} from the tess buffers, {} indexed), {} dropped; with both shaders "
      "{}, a declaration {}, stream 0 {}, indices {} (the index buffer when indexed, tess indices otherwise), a "
      "target {}; {:.1f} textures and {:.1f} constants a draw; distinct: {} vertex shaders, {} pixel shaders, {} "
      "declarations, {} materials, {} targets; calls from another thread so far: {}; tess draws with UP vertices, "
      "stride and indices: {}",
      frame.number, n, from_tess, n - from_tess, frame.dropped, shaders, declarations, stream0, indices, targets,
      double(textures) * per, double(frame.constants.size()) * per, Distinct(vs), Distinct(ps), Distinct(decls),
      Distinct(materials), Distinct(target_ids), g_other_thread.load(std::memory_order_relaxed), up_complete);
  if (n > 0) {
    REXLOG_INFO("cod2: native capture: {}", RecordText(frame, 0));
    REXLOG_INFO("cod2: native capture: {}", RecordText(frame, n / 2));
  }
}

}  // namespace

bool CaptureOn() { return REXCVAR_GET(cod2_native_capture) > 0; }

void CaptureDraw(const uint8_t* base, uint32_t args, uint32_t pass) {
  if (!CaptureOn() || !IsGuestData(args)) return;
  if (!OnRenderThread()) {
    g_other_thread.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  Frame& frame = g_recording;
  if (frame.draws.size() >= kMaxDraws) {
    ++frame.dropped;
    return;
  }
  DrawRecord& d = frame.draws.emplace_back();
  d.args = args;
  d.from_tess = LoadU32(base, args) == 0;
  d.triangles = LoadU32(base, args + 0xCu);
  if (d.from_tess) {
    d.tess_vertices = LoadU32(base, kTessVertexCount);
    d.tess_indices = LoadU32(base, kTessIndexCount);
    d.up_first_vertex = LoadU32(base, args + 0x4u);
    d.up_vertex_count = LoadU32(base, args + 0x8u);
    d.up_stride = LoadU32(base, args + 0x10u);
    d.up_vertices = kTessVertices + d.up_first_vertex * d.up_stride;
    d.up_indices = LoadU32(base, kTessIndexPointer);
    d.up_index_count = 3u * d.triangles;
  } else {
    d.index_range = LoadU32(base, args + 0x10u);
    d.vertex_range = LoadU32(base, args + 0x14u);
  }
  d.technique_type = LoadU32(base, kTessTechniqueType);
  d.material = LoadU32(base, kTessMaterial);
  d.pass = pass;
  d.vertex_shader = LoadU32(base, kVertexShaderCache);
  d.pixel_shader = LoadU32(base, kPixelShaderCache);
  d.declaration = LoadU32(base, kDeclarationCache);
  for (size_t s = 0; s < kStreams; ++s) {
    const uint32_t at = kStreamCache + 12u * static_cast<uint32_t>(s);
    d.streams[s] = {LoadU32(base, at), LoadU32(base, at + 4u), LoadU32(base, at + 8u)};
  }
  d.index_buffer = LoadU32(base, kIndexCache);
  for (size_t t = 0; t < kTextureSlots; ++t) d.textures[t] = LoadU32(base, kTextureCache + 4u * static_cast<uint32_t>(t));
  for (size_t i = 0; i < kStateCaches.size(); ++i) d.state[i] = LoadU32(base, kStateCaches[i]);
  d.target = LoadU32(base, kTargetId);
  ReadConstants(base, pass, frame, d);
}

void CaptureFrameEnd() {
  if (!CaptureOn()) {
    if (!g_recording.draws.empty()) g_recording.Clear();
    return;
  }
  if (!OnRenderThread()) {
    g_other_thread.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  std::swap(g_recording, g_last);
  g_recording.Clear();
  g_last.number = ++g_frames;
  const uint64_t now = NowMs();
  if (g_last_log_ms == 0) {
    g_last_log_ms = now;
    return;
  }
  if (now - g_last_log_ms < static_cast<uint64_t>(REXCVAR_GET(cod2_native_capture)) * 1000u) return;
  g_last_log_ms = now;
  LogSummary(g_last);
}

}  // namespace cod2::native

#else  // COD2_MP: the multiplayer program's addresses differ; nothing is captured there yet.

namespace cod2::native {
bool CaptureOn() { return false; }
void CaptureDraw(const uint8_t*, uint32_t, uint32_t) {}
void CaptureFrameEnd() {}
}  // namespace cod2::native

#endif  // !COD2_MP
