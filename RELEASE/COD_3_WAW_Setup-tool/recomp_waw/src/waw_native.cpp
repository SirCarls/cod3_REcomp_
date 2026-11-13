// waw - World at War's native renderer: the hooks in the game's back end, and the struct map's capture.
// [waw-decomp-waw] 2026-11-11
//
// Each function here is named by a [[midasm_hook]] block in config/default.toml. Addresses are the disc program's
// (default.xex: campaign, co-op and zombies), read in the decompile (dev/waw-decomp/x360_sp) and the generated code:
//
//   0x824223D0  the draw-list loop (IW3's R_DrawSurfs). Its r3 is the run's context: +0 the source state, +4 the
//               command buffer state, +8 the list's index, +0xC the list {draw surfaces, count}. For each pass of the
//               run's technique (state + 0x130, pass count at technique + 6) it sets the pass up (0x82437A90) and
//               calls the surface type's tess function through the table at 0x820ABEC4; the type is bits 54-57 of the
//               64-bit draw surface.
//   0x824224DC  that call (`bctrl`): r31 the context, r28 the type x 4, r30 the pass index.
//   0x82432268  R_DrawIndexedPrimitive(r3 the prim state, r4 the arguments: +4 the triangle count, +8 the first
//               index): Direct3D's indexed draw 0x82352F00 (device, 4 = a triangle list, 0, first index, index
//               count), which only it calls. Nineteen functions call it (the tess functions and their depth and
//               shadow variants). The prim state is the command buffer state's +0xF0 (0x82444988, the world's tess
//               function, hands R_ChangeIndices state + 0xF0), so the state is r3 - 0xF0.
//   0x8243226C  its second instruction, where the capture reads r3 and r4 (the first has acct2-waw's census hook).
//   0x824322B8  the tess buffers' draw (r3 the command buffer state, r4 the arguments: +0 the vertex count, +4 the
//               triangle count): Direct3D's indexed draw from memory 0x82352E60 (device = state + 0xF0, 4, 0, vertex
//               count, index count, the indices = state + 0x114C, 1 = 16-bit, the vertices = state + 0x1150, the
//               stride 32). Only the tess path 0x82428578 calls it (2D, effects, code meshes).
//   0x824322BC  its second instruction, for the same reason.
//   0x8235E6D8  Direct3D's swap (the only caller of VdSwap): the frame's end on the thread that draws.
//
// waw_debug_native_capture (record only, off by default): every N seconds one frame's draws are logged, the first of
// each technique and surface type, with the command buffer state's words and what they lead to (material, technique,
// pass and its arguments, the material's textures, the buffers, the vertex declaration, the device's shader
// constants), to map the layouts the native renderer reads (dev/agents/waw-decomp/WAW-NATIVE.md).

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include <rex/cvar.h>
#include <rex/logging.h>

#include "generated/default/waw_pch.h"
#include "waw_native_layout.h"
#include "waw_native_side.h"
#include "waw_native_textures.h"

REXCVAR_DEFINE_INT32(waw_debug_native_capture, 0, "waw",
                     "Debug (the native renderer's struct map): every N seconds, log one frame's draws as the back "
                     "end submits them, the first of each technique and surface type, with the command buffer "
                     "state for it (material, technique, pass, textures, buffers, constants); 0 = off. Record only");

using namespace waw::native;

namespace {

constexpr uint32_t kTessSlots = 16;

// `count` words from `address` as hex, for a record whose layout is being mapped.
std::string Words(const uint8_t* base, uint32_t address, uint32_t count) {
  if (!Readable(address) || !Readable(address + 4 * count)) return "-";
  std::string text;
  char word[12];
  for (uint32_t i = 0; i < count; ++i) {
    std::snprintf(word, sizeof word, "%s%08X", i ? " " : "", Load32(base, address + 4 * i));
    text += word;
  }
  return text;
}

// `count` words of the GPU's memory from a physical address.
std::string GpuWords(const uint8_t* base, uint32_t physical, uint32_t count) {
  std::string text;
  char word[12];
  for (uint32_t i = 0; i < count; ++i) {
    std::snprintf(word, sizeof word, "%s%08X", i ? " " : "", GpuU32(base, physical + 4 * i));
    text += word;
  }
  return text;
}

// `count` registers of four floats from `address`, as "c0(x y z w) c1(...)"; registers of zeros are left out.
std::string Registers(const uint8_t* base, uint32_t address, uint32_t count) {
  if (!Readable(address) || !Readable(address + 16 * count)) return "-";
  std::string text;
  for (uint32_t i = 0; i < count; ++i) {
    float f[4];
    bool any = false;
    for (uint32_t k = 0; k < 4; ++k) {
      f[k] = LoadF32(base, address + 16 * i + 4 * k);
      any = any || Load32(base, address + 16 * i + 4 * k) != 0;
    }
    if (!any) continue;
    char entry[96];
    std::snprintf(entry, sizeof entry, " c%u(%.5g %.5g %.5g %.5g)", i, f[0], f[1], f[2], f[3]);
    text += entry;
  }
  return text.empty() ? " all zero" : text;
}

// The texts a record's words point to, as "+offset:text", to find where a record keeps its names.
std::string TextsIn(const uint8_t* base, uint32_t record, uint32_t words) {
  if (!Readable(record) || !Readable(record + 4 * words)) return "-";
  std::string text;
  for (uint32_t i = 0; i < words; ++i) {
    const std::string name = GuestText(base, Load32(base, record + 4 * i));
    if (name.empty()) continue;
    char offset[16];
    std::snprintf(offset, sizeof offset, " +0x%X:", 4 * i);
    text += offset + name;
  }
  return text.empty() ? " none" : text;
}

// The draw-list loop's context of the run being drawn and the run's surface type, kept by waw_native_run on the
// thread that draws it. 16 = no run (the tess path outside the draw lists: 2D).
thread_local uint32_t t_run_context = 0;
thread_local uint32_t t_run_type = kTessSlots;

constexpr uint32_t kCaptureLines = 260;  // a captured frame's short lines
constexpr uint32_t kCaptureWhole = 40;   // and how many of them are followed by the whole records
struct Capture {
  std::mutex mutex;
  bool armed = false;
  uint32_t logged = 0, whole = 0;
  uint64_t frame_indexed = 0, frame_tess = 0;
  // (technique, surface type, 0 indexed / 1 from the tess buffers) logged this frame
  std::vector<std::pair<uint32_t, uint32_t>> seen;
  std::vector<uint32_t> whole_types;  // the technique types whose records were logged whole, over all frames
  std::chrono::steady_clock::time_point since = std::chrono::steady_clock::now();
};

Capture& TheCapture() {
  static Capture capture;
  return capture;
}

// The whole records behind one draw, for the struct map. kind: 0 indexed, 1 from the tess buffers.
void LogWhole(const uint8_t* base, uint32_t line, uint32_t state, uint32_t source, uint32_t device,
              uint32_t arguments, uint32_t kind) {
  const uint32_t material = Load32(base, state + kStateMaterial);
  const uint32_t technique = Load32(base, state + kStateTechnique);
  const uint32_t pass = Load32(base, state + kStatePass);
  const uint32_t prim = state + kStatePrim;
  const uint32_t declaration_type = Load32(base, prim + kPrimDeclType);
  REXLOG_INFO("waw: capture {}: state +0x00: {}; +0x88 (the sampler slots): {}; +0x128: {}; +0x1140: {}", line,
              Words(base, state, 34), Words(base, state + kStateSamplerTextures, 26),
              Words(base, state + kStateMaterial, 14), Words(base, state + 0x1140, 12));
  if (Readable(material)) {
    const uint32_t techset = Load32(base, material + kMaterialTechniqueSet);
    REXLOG_INFO("waw: capture {}: material +0x00: {}; technique set \"{}\" +0x00: {}; technique +0x00: {}", line,
                Words(base, material, 24), NameOf(base, techset), Words(base, techset, 6), Words(base, technique, 2));
    const uint32_t texture_count = base[material + kMaterialTextureCount];
    const uint32_t constant_count = base[material + kMaterialConstantCount];
    const uint32_t textures = Load32(base, material + kMaterialTextures);
    const uint32_t constants = Load32(base, material + kMaterialConstants);
    for (uint32_t i = 0; i < texture_count && i < 12 && Readable(textures); ++i) {
      const uint32_t entry = textures + kMaterialTextureBytes * i;
      const uint32_t image = Load32(base, entry + 0xC);
      const uint32_t texture = Readable(image) ? Load32(base, image + kImageTexture) : 0;
      REXLOG_INFO("waw: capture {}: material texture {}: {}; image +0x00: {}; texts{}; its texture +0x00: {}", line,
                  i, Words(base, entry, 4), Words(base, image, 10), TextsIn(base, image, 10),
                  Words(base, texture, 14));
    }
    for (uint32_t i = 0; i < constant_count && i < 8 && Readable(constants); ++i) {
      REXLOG_INFO("waw: capture {}: material constant {}: hash {:08X} \"{}\"{}", line, i,
                  Load32(base, constants + 32 * i), GuestText(base, constants + 32 * i + 4),
                  Registers(base, constants + 32 * i + 16, 1));
    }
  }
  if (Readable(pass)) {
    const uint32_t args = Load32(base, pass + kPassArgs);
    const uint32_t count = uint32_t(base[pass + kPassPerPrimArgs]) + base[pass + kPassPerPrimArgs + 1] +
                           base[pass + kPassPerPrimArgs + 2];
    REXLOG_INFO("waw: capture {}: pass +0x00: {}; its {} arguments: {}", line, Words(base, pass, kPassBytes / 4),
                count, Words(base, args, 2 * std::min(count, 48u)));
    const uint32_t vs = Load32(base, pass + kPassVertexShaders + 4 * (declaration_type & 15));
    const uint32_t ps = Load32(base, pass + kPassPixelShader);
    const uint32_t declarations = Load32(base, pass + kPassDeclaration);
    const uint32_t declaration = Readable(declarations) ? Load32(base, declarations + 4 + 4 * declaration_type) : 0;
    REXLOG_INFO("waw: capture {}: vertex shader +0x00: {}; pixel shader +0x00: {}; declarations +0x00: {}; the "
                "Direct3D declaration of type {} +0x00: {}", line, Words(base, vs, 4), Words(base, ps, 4),
                Words(base, declarations, 18), declaration_type, Words(base, declaration, 48));
  }
  // The bound textures, slot by slot: the pointer, the image record around it, the Direct3D texture.
  for (uint32_t slot = 0; slot < 12; ++slot) {
    const uint32_t bound = Load32(base, state + kStateSamplerTextures + 4 * slot);
    if (!Readable(bound)) continue;
    const uint32_t texture = Load32(base, bound);
    REXLOG_INFO("waw: capture {}: slot {}: {:08X}; image (from 4 before) +0x00: {}; texts{}; its texture {:08X} "
                "+0x00: {}", line, slot, bound, Words(base, bound - 4, 10), TextsIn(base, bound - 4, 10), texture,
                Words(base, texture, 14));
  }
  if (kind == 0) {
    // The index buffer and its first indices; each stream's vertex buffer and the first index's vertex.
    const uint32_t index_buffer = Load32(base, prim + kPrimIndexBuffer);
    const uint32_t first_index = Load32(base, arguments + 8);
    uint32_t index = 0;
    if (Readable(index_buffer)) {
      const bool wide = (Load32(base, index_buffer) >> 31) != 0;
      const uint32_t data = Load32(base, index_buffer + kIndexBufferAddress) & 0x1FFFFFFCu;
      const uint32_t at = data + (wide ? 4u : 2u) * first_index;
      index = wide ? GpuU32(base, at) : GpuU16(base, at);
      REXLOG_INFO("waw: capture {}: index buffer {:08X} +0x00: {}; from index {}: {}", line, index_buffer,
                  Words(base, index_buffer, 10), first_index, GpuWords(base, at & ~3u, 4));
    }
    for (uint32_t k = 0; k < 3; ++k) {
      const uint32_t stream = prim + kPrimStreams + kPrimStreamBytes * k;
      const uint32_t stride = Load32(base, stream), buffer = Load32(base, stream + 4);
      const uint32_t offset = Load32(base, stream + 8);
      if (!Readable(buffer) || stride == 0 || stride > 256) continue;
      const uint32_t data = (Load32(base, buffer + kVertexBufferFetch) & 0x1FFFFFFCu) + offset;
      REXLOG_INFO("waw: capture {}: stream {}: stride {}, offset {:X}, vertex buffer {:08X} +0x00: {}; vertex {}: "
                  "{}; the next: {}", line, k, stride, offset, buffer, Words(base, buffer, 10), index,
                  GpuWords(base, data + index * stride, (stride + 3) / 4),
                  GpuWords(base, data + (index + 1) * stride, (stride + 3) / 4));
    }
  } else {
    const uint32_t vertices = Load32(base, state + kStateTessVertices);
    const uint32_t indices = Load32(base, state + kStateTessIndices);
    REXLOG_INFO("waw: capture {}: tess indices {:08X}: {}; tess vertices {:08X}: {}", line, indices,
                Words(base, indices, 3), vertices, Words(base, vertices, 24));
  }
  if (Readable(device)) {
    REXLOG_INFO("waw: capture {}: vertex constants:{}", line, Registers(base, device + kDeviceVertexConstants, 40));
    REXLOG_INFO("waw: capture {}: pixel constants:{}", line, Registers(base, device + kDevicePixelConstants, 40));
  }
  if (Readable(source)) {
    REXLOG_INFO("waw: capture {}: source +0xF90 (the code images): {}; +0x12F0: {}", line,
                Words(base, source + 0xF90, 36), Words(base, source + 0x12F0, 40));
  }
}

// One draw of an armed frame: a short line for the first of each (technique, surface type, kind), and the whole
// records for the first draw of each technique type. kind: 0 indexed, 1 from the tess buffers.
void CaptureDraw(PPCContext& ctx, const uint8_t* base, uint32_t state, uint32_t arguments, uint32_t kind) {
  Capture& c = TheCapture();
  std::lock_guard<std::mutex> lock(c.mutex);
  ++(kind ? c.frame_tess : c.frame_indexed);
  if (!c.armed || c.logged >= kCaptureLines || !Readable(state)) return;
  const uint32_t technique = Load32(base, state + kStateTechnique);
  const std::pair<uint32_t, uint32_t> key{technique, t_run_type | (kind << 8)};
  if (std::find(c.seen.begin(), c.seen.end(), key) != c.seen.end()) return;
  c.seen.push_back(key);
  ++c.logged;
  const uint32_t context = t_run_context;
  const uint32_t run_state = Readable(context) ? Load32(base, context + 4) : 0;
  const uint32_t source = run_state == state ? Load32(base, context) : 0;
  const uint32_t device = Load32(base, state + kStatePrim + kPrimDevice);
  const uint32_t material = Load32(base, state + kStateMaterial), pass = Load32(base, state + kStatePass);
  const uint32_t type = Load32(base, state + kStateTechniqueType);
  const uint32_t declaration_type = Load32(base, state + kStatePrim + kPrimDeclType);
  const bool pass_ok = Readable(pass);
  const uint32_t vs = pass_ok ? Load32(base, pass + kPassVertexShaders + 4 * (declaration_type & 15)) : 0;
  const uint32_t ps = pass_ok ? Load32(base, pass + kPassPixelShader) : 0;
  REXLOG_INFO("waw: capture {}: {} draw, surface type {}, arguments {}, from {:08X}; state {:08X}{} source {:08X}; "
              "material \"{}\", technique type {} \"{}\", pass {} of {}: vs \"{}\" ps \"{}\" args {} {} {} custom "
              "samplers {}; state bits {}; prim +0xF0: {}",
              c.logged, kind ? "tess" : "indexed", t_run_type, Words(base, arguments, 3), uint32_t(ctx.lr), state,
              run_state == state ? " (the run's)" : " (not the run's)", source, NameOf(base, material), type,
              NameOf(base, technique), pass_ok ? (pass - technique - kTechniquePasses) / kPassBytes : 0u,
              Readable(technique) ? uint32_t(Load32(base, technique + 4) & 0xFFFFu) : 0u, NameOf(base, vs),
              NameOf(base, ps), pass_ok ? uint32_t(base[pass + kPassPerPrimArgs]) : 0u,
              pass_ok ? uint32_t(base[pass + kPassPerPrimArgs + 1]) : 0u,
              pass_ok ? uint32_t(base[pass + kPassPerPrimArgs + 2]) : 0u,
              pass_ok ? uint32_t(base[pass + kPassCustomSamplers]) : 0u, Words(base, state + kStateBits, 2),
              Words(base, state + kStatePrim, 14));
  // The whole records for the first draw of each technique type, surface type and kind, over all captured frames.
  const uint32_t whole_key = type | (kind << 8) | (t_run_type << 12);
  if (c.whole < kCaptureWhole &&
      std::find(c.whole_types.begin(), c.whole_types.end(), whole_key) == c.whole_types.end()) {
    c.whole_types.push_back(whole_key);
    ++c.whole;
    LogWhole(base, c.logged, state, source, device, arguments, kind);
  }
}

// The frame's end: counts the frame's draws and arms the next frame every N seconds.
void CaptureFrameEnd() {
  const int32_t seconds = REXCVAR_GET(waw_debug_native_capture);
  if (seconds <= 0) return;
  Capture& c = TheCapture();
  std::lock_guard<std::mutex> lock(c.mutex);
  if (c.armed) {
    REXLOG_INFO("waw: capture: the frame had {} indexed draws and {} from the tess buffers; {} lines logged",
                c.frame_indexed, c.frame_tess, c.logged);
  }
  c.frame_indexed = c.frame_tess = 0;
  c.armed = false;
  if (std::chrono::steady_clock::now() - c.since >= std::chrono::seconds(seconds)) {
    c.since = std::chrono::steady_clock::now();
    c.armed = true;
    c.logged = c.whole = 0;
    c.seen.clear();
  }
}

}  // namespace

// The draw-list loop's call of a tess function (0x824224DC, before the bctrl).
void waw_native_run(PPCContext& ctx, uint8_t* base) {
  (void)base;
  t_run_context = ctx.r31.u32;
  t_run_type = (ctx.r28.u32 >> 2) & (kTessSlots - 1);
}

// R_DrawIndexedPrimitive's second instruction (0x8243226C): r3 the prim state (the command buffer state + 0xF0), r4
// the arguments. The caller's other registers are still as it left them (the function has saved nothing yet): the
// native side reads the model functions' r11 (waw_native_side.cpp, kStaticModelDrawReturn).
void waw_native_indexed(PPCContext& ctx, uint8_t* base) {
  const bool capture = REXCVAR_GET(waw_debug_native_capture) > 0;
  const bool side = waw::native::SideOn();
  if (!capture && !side) return;
  const uint32_t state = ctx.r3.u32 - kStatePrim;
  if (capture) CaptureDraw(ctx, base, state, ctx.r4.u32, 0);
  if (side) {
    const uint32_t context = t_run_context;
    const bool in_run = Readable(context) && Load32(base, context + 4) == state;
    waw::native::SideIndexedDraw(base, in_run ? Load32(base, context) : 0, state, ctx.r4.u32,
                                 in_run ? t_run_type : kTessSlots, uint32_t(ctx.lr), ctx.r11.u32);
  }
}

// The tess buffers' draw's second instruction (0x824322BC): r3 the command buffer state, r4 the arguments.
void waw_native_tess(PPCContext& ctx, uint8_t* base) {
  const bool capture = REXCVAR_GET(waw_debug_native_capture) > 0;
  const bool side = waw::native::SideOn();
  if (!capture && !side) return;
  const uint32_t state = ctx.r3.u32;
  // A tess draw outside the run that was last drawn belongs to no run (2D): the type is "none".
  const uint32_t context = t_run_context;
  const bool in_run = Readable(context) && Load32(base, context + 4) == state;
  const uint32_t kept_type = t_run_type;
  if (!in_run) t_run_type = kTessSlots;
  if (capture) CaptureDraw(ctx, base, state, ctx.r4.u32, 1);
  if (side) {
    waw::native::SideTessDraw(base, in_run ? Load32(base, context) : 0, state, ctx.r4.u32,
                              in_run ? kept_type : kTessSlots);
  }
  t_run_type = kept_type;
}

// Direct3D's swap, at its start (0x8235E6D8): the frame's end on the thread that draws.
void waw_native_swap(PPCContext& ctx, uint8_t* base) {
  (void)ctx;
  CaptureFrameEnd();
  if (waw::native::SideOn()) waw::native::SideFrameEnd(base);
}
