// cod3 - [acct2-bro] 2026-11-05 NGL census (the campaign and cod3mp): what the game's NGL
// library draws, counted while the GPU emulation keeps drawing it. The first step of CoD3's native renderer at NGL's
// level, built the way Big Red One's is (owner 2026-11-05 09:11: "NGL renderer, BRO's way"; the plan is
// dev/agents/acct2-bro/COD3-NGL-PLAN.md).
//
// Off by default (cod3_debug_ngl_census, a test switch). Every hook calls the game's own function unchanged and writes
// nothing to guest memory. What it logs:
//   - every 10 s: frames (nglListSend calls), and per frame the mesh sections drawn and texture binds; then the
//     sections per frame for each material class (the class is the draw function the call came from);
//   - the first time each vertex layout is drawn (at most 64): the class and the layout's first 64 bytes;
//   - the first 48 textures bound: the NGL texture's first 64 bytes and its Direct3D texture's first 64 bytes;
//   - every 10 s: Direct3D's two draw calls per frame by the call site they came from (the 16 busiest), to find what
//     draws the world (census run 1: nglDrawMeshSection draws only about 14 sections a frame in forest, the sky and
//     effects; the rust-renderer notes count about 890 indexed draws a frame);
//   - the generic surface path (census run 2: 0x82143F70 makes 710-840 of those draws in forest): passes a frame, and
//     the first 40 shader states seen with the semantic codes of their parameters, and each new vertex layout there.
//
// Campaign addresses (dev/bro-decomp/cod3_xcheck/ngl_pairs.tsv and material_draws.txt; rust-renderer's
// NOTES-game-side.md; cod3mp's in mp/config/campaign_map.tsv, nglListSend's 0x82156B28):
//   0x8213DAA0 nglListSend: the frame's list goes to the render job (main thread, once a frame)
//   0x8213C9F8 nglDrawMeshSection(surface), on the render job's thread: vertex buffer +0x40, layout pointer +0x60
//              (its first word is the vertex size), index buffer +0x50
//   0x8213EB90 nglSetTexture(stage, texture), on the render job's thread: animated when texture +0x18 & 4; the
//              Direct3D texture at +0x14
//   0x822F3A28 Direct3D's indexed draw, 0x822F3620 its plain draw (the two nglDrawMeshSection ends with)
//   0x82142F20 (shader state, object, surface): called by the generic surface draw 0x82143F70 once per pass, before
//              its draws; it fills the state's parameters (a list from +0x34: +0x0C next, +0x10 a semantic code,
//              +0x14 where the value goes). 0x82143F70 itself is hooked by viewmodel_flip.cpp.
// The 2D lists aren't counted here: nglListAddQuad (0x8212AC58) and nglListAddString (0x82134F70) carry names in
// config/names.toml and ultrawide.cpp already hooks both; their counts come in step 3 from the scenes' own counters.

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iterator>
#include <mutex>
#include <string>
#include <unordered_set>

#include <rex/cvar.h>
#include <rex/hook.h>
#include <rex/logging.h>

#include "game/addresses.h"  // [acct2-bro] side224: the render job's group
#include "game/game_api.h"   // [acct2-bro] side224: WaitJobGroup
#include "game/guest.h"
#include "game/ngl_side.h"

REXCVAR_DEFINE_BOOL(cod3_debug_ngl_census, false, "cod3/Debug",
                    "Log what the game's NGL library draws every 10 s (mesh sections per material class, vertex "
                    "layouts, textures); for the native renderer's work")
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

namespace {

namespace g = cod3::guest;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kSurfaceLayout = 0x60;
constexpr uint32_t kTextureDirect3D = 0x14;
constexpr uint32_t kDumpBytes = 64;
constexpr double kReportSeconds = 10.0;
constexpr size_t kMaxLayouts = 64;
constexpr size_t kMaxTextures = 48;
constexpr size_t kMaxSites = 256;         // call sites of Direct3D's draws counted
constexpr size_t kSitesReported = 16;
constexpr size_t kMaxStates = 40;         // generic shader states described
constexpr int kMaxParameters = 48;        // parameters listed per state
constexpr uint32_t kStateParameters = 0x34;
constexpr uint32_t kParameterNext = 0x0C;
constexpr uint32_t kParameterSemantic = 0x10;

// Each material class's draw function, and its GetName, which follows the draw (so the draw ends there).
struct MaterialDraw {
  uint32_t draw;
  uint32_t end;
  const char* name;
};
#if defined(COD3_MP)
// cod3mp's: the pairs in dev/waw-decomp/cod3/cod3_relatives.tsv; the six it lacks (cdFlag's, cdGunSight's, cdGun's,
// cdPrelit's and cdBackground's draws, cdSky's end) by the order in the image and the class name each GetName gives.
constexpr MaterialDraw kDraws[] = {
    {0x820E3D18, 0x820E4CC8, "cdSimpleInstance"},   {0x820E7118, 0x820E74C8, "cdWheelMark"},
    {0x820E7DE0, 0x820E8390, "cdSimpleAlpha"},      {0x820E8A08, 0x820E92D8, "cdDynamicDecal"},
    {0x820E9958, 0x820E9BB8, "cdSimpleUVAnim"},     {0x820EA470, 0x820EAAD8, "cdFlag"},
    {0x820EAF18, 0x820EB1B0, "cdDebug"},            {0x820EB7E8, 0x820EB9D0, "cdGunSightSpecular"},
    {0x820EBFE0, 0x820EC1A8, "cdGunSight"},         {0x820EC7B8, 0x820EC980, "cdGun"},
    {0x820ED090, 0x820EDF38, "cdGlass"},            {0x820EE698, 0x820EE878, "cdPropeller"},
    {0x820EEE90, 0x820EF0A8, "cdDecal"},            {0x820EF6C8, 0x820EF8F0, "cdCharSpecular"},
    {0x820EFEE8, 0x820F00F0, "cdChar"},             {0x820F0688, 0x820F0890, "cdPrelit"},
    {0x820F0F60, 0x820F12C8, "cdAirplaneMetal"},    {0x820F1A28, 0x820F1C10, "cdSimpleSpecular"},
    {0x820F21F8, 0x820F2390, "cdSimplePrelit"},     {0x820F2B50, 0x820F2D58, "cdSimple"},
    {0x820F45F8, 0x820F4868, "cdWater"},            {0x820F5110, 0x820F5768, "cdWorldBlendPointLit"},
    {0x820F5E80, 0x820F64C0, "cdWorldPointLit"},    {0x820F6BC0, 0x820F7218, "cdWorldBlend"},
    {0x820F7820, 0x820F79A8, "cdWorldVertexLit"},   {0x820F80B0, 0x820F8688, "cdWorld"},
    {0x820F8C98, 0x820F8E30, "cdBackground"},       {0x820F93C0, 0x820F97C0, "cdSky"},
};
#else
constexpr MaterialDraw kDraws[] = {
    {0x820D1A10, 0x820D2978, "cdSimpleInstance"},   {0x820D4F70, 0x820D5320, "cdWheelMark"},
    {0x820D5CB8, 0x820D62B0, "cdSimpleAlpha"},      {0x820D6928, 0x820D71E8, "cdDynamicDecal"},
    {0x820D7868, 0x820D7AC8, "cdSimpleUVAnim"},     {0x820D8380, 0x820D8A38, "cdFlag"},
    {0x820D8E78, 0x820D9110, "cdDebug"},            {0x820D9748, 0x820D9930, "cdGunSightSpecular"},
    {0x820D9F40, 0x820DA108, "cdGunSight"},         {0x820DA718, 0x820DA8E0, "cdGun"},
    {0x820DB008, 0x820DBEA0, "cdGlass"},            {0x820DC600, 0x820DC7E0, "cdPropeller"},
    {0x820DCDF8, 0x820DD010, "cdDecal"},            {0x820DD678, 0x820DD8A0, "cdCharSpecular"},
    {0x820DDE98, 0x820DE0A0, "cdChar"},             {0x820DE638, 0x820DE840, "cdPrelit"},
    {0x820DEEC8, 0x820DF230, "cdAirplaneMetal"},    {0x820DF990, 0x820DFB78, "cdSimpleSpecular"},
    {0x820E0160, 0x820E02F8, "cdSimplePrelit"},     {0x820E0AB8, 0x820E0CC0, "cdSimple"},
    {0x820E25B8, 0x820E2828, "cdWater"},            {0x820E30D0, 0x820E3728, "cdWorldBlendPointLit"},
    {0x820E3E40, 0x820E4480, "cdWorldPointLit"},    {0x820E4B80, 0x820E51D8, "cdWorldBlend"},
    {0x820E57E0, 0x820E5968, "cdWorldVertexLit"},   {0x820E6070, 0x820E6648, "cdWorld"},
    {0x820E6C58, 0x820E6DF0, "cdBackground"},       {0x820E7380, 0x820E7790, "cdSky"},
};
#endif
constexpr int kClasses = static_cast<int>(std::size(kDraws));
constexpr int kOther = kClasses;  // called from elsewhere (effects nodes, ...)

int ClassOf(uint32_t return_address) {
  for (int i = 0; i < kClasses; ++i) {
    if (return_address >= kDraws[i].draw && return_address < kDraws[i].end) return i;
  }
  return kOther;
}

const char* ClassName(int c) { return c < kClasses ? kDraws[c].name : "other"; }

bool On() { return REXCVAR_GET(cod3_debug_ngl_census); }
bool Pointer(uint32_t p) { return p >= 0x10000 && (p & 3) == 0; }

std::string Words(uint8_t* base, uint32_t addr, uint32_t bytes) {
  std::string s;
  char word[12];
  for (uint32_t i = 0; i < bytes; i += 4) {
    std::snprintf(word, sizeof word, "%08X ", g::ReadU32(base, addr + i));
    s += word;
  }
  return s;
}

std::atomic<uint64_t> g_frames{0}, g_sections{0}, g_binds{0}, g_passes{0};
std::array<std::atomic<uint64_t>, kClasses + 1> g_class_sections{};

std::mutex g_seen_mutex;  // the render job's thread and the main thread both note new layouts and textures
std::unordered_set<uint32_t> g_layouts;
std::unordered_set<uint32_t> g_textures;
std::unordered_set<uint32_t> g_other_callers;
std::unordered_set<uint32_t> g_states;

// A vertex layout's elements as usage[index]@offset:type, from its D3DVERTEXELEMENT9s (12 bytes each, from +0x0C,
// ending with stream 0xFF).
std::string LayoutText(uint8_t* base, uint32_t layout) {
  static const char* const kUsages[] = {"position", "blendweight", "blendindices", "normal", "psize", "texcoord",
                                        "tangent", "binormal", "tessfactor", "positiont", "color", "fog", "depth",
                                        "sample"};
  std::string s;
  char item[64];
  for (uint32_t e = layout + 0x0C, n = 0; n < 16; e += 12, ++n) {
    const uint32_t stream_offset = g::ReadU32(base, e);
    if ((stream_offset >> 16) == 0xFF) break;
    const uint32_t type = g::ReadU32(base, e + 4);
    const uint32_t usage = g::ReadU8(base, e + 9), index = g::ReadU8(base, e + 10);
    std::snprintf(item, sizeof item, "%s%u@%u:%06X ", usage < std::size(kUsages) ? kUsages[usage] : "usage?", index,
                  stream_offset & 0xFFFF, type);
    s += item;
  }
  return s;
}

// Direct3D draws by call site (the return address), both draw calls together; sites past the first kMaxSites aren't
// counted.
struct Site {
  uint32_t site;
  uint64_t indexed, plain;
};
std::mutex g_site_mutex;
Site g_sites[kMaxSites];
size_t g_site_count = 0;

void NoteDraw(uint32_t site, bool indexed) {
  std::lock_guard<std::mutex> lock(g_site_mutex);
  Site* s = nullptr;
  for (size_t i = 0; i < g_site_count; ++i) {
    if (g_sites[i].site == site) {
      s = &g_sites[i];
      break;
    }
  }
  if (!s) {
    if (g_site_count == kMaxSites) return;
    s = &g_sites[g_site_count++];
    *s = {site, 0, 0};
  }
  (indexed ? s->indexed : s->plain)++;
}

std::string TakeSites(double per) {
  std::lock_guard<std::mutex> lock(g_site_mutex);
  std::string out;
  char item[64];
  for (size_t n = 0; n < kSitesReported; ++n) {
    size_t best = kMaxSites;
    for (size_t i = 0; i < g_site_count; ++i) {
      const uint64_t total = g_sites[i].indexed + g_sites[i].plain;
      if (total && (best == kMaxSites || total > g_sites[best].indexed + g_sites[best].plain)) best = i;
    }
    if (best == kMaxSites) break;
    std::snprintf(item, sizeof item, "%08X %.1f+%.1f, ", g_sites[best].site,
                  static_cast<double>(g_sites[best].indexed) * per, static_cast<double>(g_sites[best].plain) * per);
    out += item;
    g_sites[best].indexed = g_sites[best].plain = 0;
  }
  for (size_t i = 0; i < g_site_count; ++i) g_sites[i].indexed = g_sites[i].plain = 0;
  return out;
}

Clock::time_point g_last_report{};

void Report() {
  const auto now = Clock::now();
  if (g_last_report == Clock::time_point{}) {
    g_last_report = now;
    return;
  }
  const double seconds = std::chrono::duration<double>(now - g_last_report).count();
  if (seconds < kReportSeconds) return;
  g_last_report = now;

  const uint64_t frames = g_frames.exchange(0, std::memory_order_relaxed);
  const double per = frames ? 1.0 / static_cast<double>(frames) : 0.0;
  const double sections = static_cast<double>(g_sections.exchange(0, std::memory_order_relaxed)) * per;
  const double binds = static_cast<double>(g_binds.exchange(0, std::memory_order_relaxed)) * per;
  const double passes = static_cast<double>(g_passes.exchange(0, std::memory_order_relaxed)) * per;
  REXLOG_INFO("cod3: ngl census over {:.1f} s: {} frames ({:.1f} a second); a frame: {:.1f} mesh sections, {:.1f} "
              "generic surface passes, {:.1f} texture binds",
              seconds, frames, static_cast<double>(frames) / seconds, sections, passes, binds);

  std::string by_class;
  char item[64];
  for (int c = 0; c <= kClasses; ++c) {
    const uint64_t n = g_class_sections[c].exchange(0, std::memory_order_relaxed);
    if (n == 0) continue;
    std::snprintf(item, sizeof item, "%s %.1f, ", ClassName(c), static_cast<double>(n) * per);
    by_class += item;
  }
  if (!by_class.empty()) REXLOG_INFO("cod3: ngl census sections a frame by class: {}", by_class);
  const std::string sites = TakeSites(per);
  if (!sites.empty()) REXLOG_INFO("cod3: ngl census Direct3D draws a frame by call site (indexed+plain): {}", sites);
}

void NoteSection(uint8_t* base, uint32_t surface, uint32_t return_address) {
  g_sections.fetch_add(1, std::memory_order_relaxed);
  const int c = ClassOf(return_address);
  g_class_sections[c].fetch_add(1, std::memory_order_relaxed);
  if (!Pointer(surface)) return;
  const uint32_t layout = g::ReadU32(base, surface + kSurfaceLayout);
  std::lock_guard<std::mutex> lock(g_seen_mutex);
  if (c == kOther && g_other_callers.size() < 16 && g_other_callers.insert(return_address).second) {
    REXLOG_INFO("cod3: ngl census: a mesh section drawn from {:08X} (not a material class's draw)", return_address);
  }
  if (!Pointer(layout) || g_layouts.size() >= kMaxLayouts || !g_layouts.insert(layout).second) return;
  REXLOG_INFO("cod3: ngl census: vertex layout {:08X} ({}, surface {:08X}), stride {}: {}", layout, ClassName(c),
              surface, g::ReadU32(base, layout), LayoutText(base, layout));
  REXLOG_INFO("cod3: ngl census:   its surface: {}", Words(base, surface + 0x40, 0x28));
}

// One pass of the generic surface draw: its shader state's parameters (once per state) and its surface's layout (once
// per layout).
void NotePass(uint8_t* base, uint32_t state, uint32_t surface) {
  g_passes.fetch_add(1, std::memory_order_relaxed);
  if (!Pointer(state)) return;
  std::lock_guard<std::mutex> lock(g_seen_mutex);
  if (g_states.size() < kMaxStates && g_states.insert(state).second) {
    std::string semantics;
    char item[16];
    uint32_t p = g::ReadU32(base, state + kStateParameters);
    for (int n = 0; Pointer(p) && n < kMaxParameters; ++n, p = g::ReadU32(base, p + kParameterNext)) {
      std::snprintf(item, sizeof item, "%X ", g::ReadU32(base, p + kParameterSemantic));
      semantics += item;
    }
    REXLOG_INFO("cod3: ngl census: generic shader state {:08X}: {}; parameter semantics: {}", state,
                Words(base, state, kDumpBytes), semantics);
  }
  if (!Pointer(surface)) return;
  const uint32_t layout = g::ReadU32(base, surface + kSurfaceLayout);
  if (!Pointer(layout) || g_layouts.size() >= kMaxLayouts || !g_layouts.insert(layout).second) return;
  REXLOG_INFO("cod3: ngl census: vertex layout {:08X} (generic, surface {:08X}), stride {}: {}", layout, surface,
              g::ReadU32(base, layout), LayoutText(base, layout));
  REXLOG_INFO("cod3: ngl census:   its surface: {}", Words(base, surface + 0x40, 0x28));
}

void NoteTexture(uint8_t* base, uint32_t stage, uint32_t texture) {
  g_binds.fetch_add(1, std::memory_order_relaxed);
  if (!Pointer(texture)) return;
  std::lock_guard<std::mutex> lock(g_seen_mutex);
  if (g_textures.size() >= kMaxTextures || !g_textures.insert(texture).second) return;
  const uint32_t direct3d = g::ReadU32(base, texture + kTextureDirect3D);
  REXLOG_INFO("cod3: ngl census: texture {:08X} (stage {}): {}", texture, stage, Words(base, texture, kDumpBytes));
  if (Pointer(direct3d)) {
    REXLOG_INFO("cod3: ngl census:   its Direct3D texture {:08X}: {}", direct3d, Words(base, direct3d, kDumpBytes));
  }
}

}  // namespace

// nglListSend(): the end of the frame's list, on the main thread.
REX_EXTERN(__imp__sub_8213DAA0);
REX_HOOK_RAW(sub_8213DAA0) {
  if (On()) {
    g_frames.fetch_add(1, std::memory_order_relaxed);
    Report();
  }
  // [acct2-bro] side224: the last list's render job done first, so its effect flushes count with it (the game's
  // nglListSend waits for the same group before queuing the next job).
  // side225: kRenderJobGroup is the variable that holds the group, so pass the group it holds, as the game does
  // (`lwz r3, kRenderJobGroup` before the wait); side224 passed the variable's address, which never finished.
  if (cod3::ngl_side::WaitForRenderJobFirst()) {
    const uint32_t render_job_group = cod3::guest::ReadU32(base, cod3::addr::live_video::kRenderJobGroup);
    if (render_job_group) cod3::game::WaitJobGroup(ctx, base, render_job_group);
  }
  cod3::ngl_side::OnListSend(base);  // [acct2-bro] the 2D side-by-side check (ngl_side.cpp), off by default
  __imp__sub_8213DAA0(ctx, base);
}

// nglDrawMeshSection(surface), on the render job's thread.
REX_EXTERN(__imp__sub_8213C9F8);
REX_HOOK_RAW(sub_8213C9F8) {
  if (On()) NoteSection(base, ctx.r3.u32, uint32_t(ctx.lr));
  __imp__sub_8213C9F8(ctx, base);
}

// nglSetTexture(stage, texture), on the render job's thread.
REX_EXTERN(__imp__sub_8213EB90);
REX_HOOK_RAW(sub_8213EB90) {
  if (On()) NoteTexture(base, ctx.r3.u32, ctx.r4.u32);
  __imp__sub_8213EB90(ctx, base);
}

// Direct3D's indexed draw and its plain draw, on the render job's threads: counted by call site. ngl_side reads the
// device's constants at them for a few frames after some of its dumps.
REX_EXTERN(__imp__sub_822F3A28);
REX_HOOK_RAW(sub_822F3A28) {
  if (On()) NoteDraw(uint32_t(ctx.lr), true);
  cod3::ngl_side::OnDraw(base, uint32_t(ctx.lr));
  if (cod3::ngl_side::SkipGameDraw()) return;  // [acct2-bro] side191: a check (cod3_debug_skip_game_sky_layers)
  __imp__sub_822F3A28(ctx, base);
}

REX_EXTERN(__imp__sub_822F3620);
REX_HOOK_RAW(sub_822F3620) {
  if (On()) NoteDraw(uint32_t(ctx.lr), false);
  cod3::ngl_side::OnDraw(base, uint32_t(ctx.lr));
  if (cod3::ngl_side::SkipGameDraw()) return;  // [acct2-bro] side191: a check (cod3_debug_skip_game_sky_layers)
  __imp__sub_822F3620(ctx, base);
}

// The generic surface draw's per-pass parameter fill (shader state, object, surface).
REX_EXTERN(__imp__sub_82142F20);
REX_HOOK_RAW(sub_82142F20) {
  if (On()) NotePass(base, ctx.r3.u32, ctx.r5.u32);
  // [acct2-bro] side91: the gun's draws probed (ngl_side.cpp); [rexglue-native] 2026-11-10 rn43a: and the shader
  // state, whose value block this fill writes for the object
  cod3::ngl_side::OnPass(base, ctx.r4.u32, ctx.r3.u32);
  __imp__sub_82142F20(ctx, base);
}
