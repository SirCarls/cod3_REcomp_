// cod4 - Call of Duty 4: Modern Warfare (Xbox 360) static recompilation: the native renderer's side check (see
// cod4_native_side.h). [waw-decomp-cod4] 2026-11-11
//
// A check only, off by default: the game keeps drawing through the GPU emulation, and with cod4_debug_native_side off
// nothing here runs. With it on, the frames 600, 1200, ... 4800 (counted at the start of the back end's frame,
// 0x82128488, which runs the scene's views 0x82128268 and the render command loop 0x82123B90 in them; run cod4side3
// counted render command 13 instead, which in a level runs more than once a frame: every kept window had no draws)
// are kept and drawn natively by ngl_vk. The design and the Xenos texture
// decoding are Call of Duty 2's (../recomp_cod2/src/game/native_side.cpp, waw-decomp 2026-11-10); what differs is
// where Call of Duty 4's back end (IW3) keeps what a draw needs. All of it was read from the decompile
// (dev/x360-family/cod4_360/sp) and checked in the runs cod4cap1 and cod4cap2 (Killhouse); the whole map is in
// dev/agents/waw-decomp/COD4-NATIVE.md. In short:
//
//   - A draw reaches Direct3D's indexed draw 0x8209BD80 (r3 the device, r5 the base vertex, r6 the first index, r7
//     the index count: triangle lists). The draw-list loop (R_DrawSurfs 0x82123950) calls a tess function per run of
//     surfaces; its context (r31 at 0x82123A88) holds the frame's source state at +0 and the command buffer state at
//     +4. The world's surfaces (surface type 0) are drawn from the world's own vertex buffer: for the shadow maps and
//     the depth prepass by 0x8210C468 from its own index buffer, lit through the tess function with indices written
//     each frame (run cod4side4).
//   - The state (KisakCOD's GfxCmdBufState at the 360's offsets): +0x20 the 26 samplers' states, +0x88 what each
//     sampler has bound (the image + 4, which holds the pointer to its Direct3D texture: 0x8210A6F0 stores it and
//     0x820D4D28 hands the word there to Direct3D's SetTexture 0x82099D38), +0xF0 the
//     device, +0xF4 the index buffer, +0xF8 the vertex declaration's type, +0xFC stream 0 (stride, vertex buffer,
//     offset), +0x118 the material, +0x11C the technique type, +0x120 the technique, +0x124 the pass.
//   - A pass (Xenon's MaterialPass, 0x54 bytes): +0x00 the vertex declaration, +0x04 fifteen vertex shaders, one per
//     declaration type, +0x44 the pixel shader, +0x48 to +0x4A the argument counts (per primitive, per object,
//     stable), +0x50 the arguments, 8 bytes each: type u16, destination u16, then for type 3 (a code constant for the
//     vertex shader) the constant's index u16, first row u8 and row count u8; for type 2 (a material's texture for
//     the pixel shader) the texture's name hash; for type 4 (a code texture) its index (4 the primary lightmap, 5 the
//     secondary). The destination is the shader register or the sampler.
//   - The shader constants as the device holds them: vertex c0 at device + 0x780, pixel c0 at device + 0x1780, 16
//     bytes a register (0x820D59C8 writes pixel constant r to device + (r + 0x178) x 16). The world-view-projection
//     is the code constant 78 (KisakCOD's CONST_SRC_CODE_WORLD_VIEW_PROJECTION_MATRIX; 80 its transpose; 74 and 76
//     the view-projection, the same for the world's corners, which are in world space). Whether the four registers
//     are the matrix's columns (a corner's clip position is (p, 1) dotted with each) or its rows is decided per draw
//     by which way more of its corners land in front of the eye and on the screen (MeshDraw::columns).
//   - The frame's code constants: source + 0x800, 16 bytes each, 58 of them (35 the sun's position, 36 its diffuse
//     color, 41 the fog, 42 the fog's color); the code images at source + 0xBA0.
//   - A material: +0x00 its name, +0x18 its state-bits entry per technique type (26 bytes), +0x32 the texture count,
//     +0x3C the texture table (12 bytes each: name hash, first and last letter, sampler state, semantic (2 the color
//     map), the image), +0x44 the state-bits table (8 bytes each, word 0 IW3's GFXS0: blend and alpha test).
//   - Buffers: a vertex buffer's fetch constant is at +0x18 (the address in its first word, low two bits its type), an
//     index buffer's data address at +0x18 and its size at +0x1C; 16-bit indices unless the first word's top bit is
//     set. A texture's fetch constant (six words) is at +0x1C of its Direct3D record.
//   - The world's corners are 44 bytes: position +0x00, color +0x10, texture coordinate +0x14, lightmap coordinate
//     +0x1C (floats), as on the PC.
//
// What is drawn so far: the world's lit surfaces (surface type 0, technique types 7 to 13, the `lm_*` techniques) with
// their color map and their lightmap (the secondary lightmap's color; the primary's first channel as the sun's share,
// with the sun's color for the techniques that have the sun). No models, sky, effects, 2D, fog or shadows yet.
// [waw-decomp-cod4] 2026-11-11 09:55: the lightmap's page is taken by the surface's lightmap index
// (FindWorldPages, SetMeshes), and the kept picture goes through the film pass and the display's gamma ramp as the
// game's does (ShownColors, FilmFrom).
//
// Textures are decoded on the CPU once each (kept by the Direct3D texture's address and fetch constant). Nothing the
// console's GPU made is read: every texture and mesh is made from what the game submitted.

#include "cod4_native_side.h"
#include "cod4_native_window.h"  // [waw-decomp-cod4] 2026-11-11 round 6: the native picture as the game's

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(cod4_debug_native_side, false, "cod4",
                    "Debug (the native renderer): at frames 600, 1200, ... 4800, draw the frame natively (ngl_vk) "
                    "and save it beside the game's own picture (userdata/logs/screenshots/ngl-side-*), for "
                    "tools/ngl_side_compare.py. The game's own drawing doesn't change");
REXCVAR_DEFINE_BOOL(cod4_debug_native_side_variants, false, "cod4",
                    "Debug (the native renderer): with cod4_debug_native_side, each kept frame is also saved seven "
                    "other ways (ngl-side-<frame>-m1..m7.ppm: m1 the picture before the film pass and the display's "
                    "gamma ramp, m2 the light gamma-encoded, m3 no lightmap, m4 the lightmap's top half P alone, m5 "
                    "its bottom half S alone, m6 without the static models, m7 the gamma ramp without the film pass)");
// [waw-decomp-cod4] 2026-11-11 10:40: the static models' first try drew them in the wrong places (run cod4side18: the
// difference rose from 22 to 38), so they are drawn only on request until their placement is read
// [waw-decomp-cod4] 2026-11-11 round 5: placed by their instance since round 4, lit by the model lighting texture now
REXCVAR_DEFINE_BOOL(cod4_debug_native_side_models, false, "cod4",
                    "Debug (the native renderer): with cod4_debug_native_side, also draw the static models (surface "
                    "type 1: placed by their instance, lit by the model lighting texture where it is read)");
REXCVAR_DEFINE_DOUBLE(cod4_debug_native_light_scale, 0.0, "cod4",
                      "Debug (the native renderer): the factor the world's light (made halved) is multiplied by in "
                      "the native picture; 0 = the built-in 2 (the game's own: its shaders have no factor)");

// [waw-decomp-cod4] 2026-11-11 12:00: both programs (the multiplayer one's hooks and addresses at the end of this file
// and at the COD4_MP blocks below; before, cod4mp only had a stub here).

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <map>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/logging.h>
#include <rex/ui/presenter.h>

#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>  // physical_membase (GpuPointer)

#if defined(COD4_NGL_VK)
extern "C" {
#include "ngl_vk.h"
}
#endif

namespace cod4::native {
namespace {

// The back end's records (see the top of this file).
constexpr uint32_t kStateSamplerTextures = 0x88u;
constexpr uint32_t kStateDevice = 0xF0u;
constexpr uint32_t kStateIndexBuffer = 0xF4u;
constexpr uint32_t kStateStream0 = 0xFCu;  // stride, vertex buffer, offset
constexpr uint32_t kStateMaterial = 0x118u;
constexpr uint32_t kStateTechniqueType = 0x11Cu;
constexpr uint32_t kStateTechnique = 0x120u;
constexpr uint32_t kStatePass = 0x124u;
constexpr uint32_t kStateDepthRange = 0x12Cu;  // the depth range's kind (run cod4cap1; 0 for the scene's draws)
// [waw-decomp-cod4] 2026-11-11 10:05: +0x130 and +0x134 the depth range's near and far (1.0 and 0.0 for the scene's
// draws, run cod4cap1); run cod4side16 found the kind at +0x12C only 0 or 0xFFFFFFFF, so the two floats are logged
constexpr uint32_t kStateDepthNear = 0x130u, kStateDepthFar = 0x134u;
// The film pass (0x821110E8): with the view's film on (its +0x5734) it draws the material at 0x835A0F1C, else the one
// at 0x835A0F18, through 0x82127280 (a full-screen quad)
#if defined(COD4_MP)
// [waw-decomp-cod4] 2026-11-11 12:00: cod4mp's renderer globals: the campaign's block at 0x835A0000 is at 0x84CD0000
// there with the same offsets (cod4_image.py match: the same functions' lis/load pairs, offsets 0x008 to 0x284 and
// 0x1000 equal, e.g. 0x82110840 / 0x82145778, 0x82135CE8 / 0x82166878; past 0x1054 the two differ)
constexpr uint32_t kFilmMaterialPointer = 0x84CD0F1Cu, kPlainPostMaterialPointer = 0x84CD0F18u;
#else
constexpr uint32_t kFilmMaterialPointer = 0x835A0F1Cu, kPlainPostMaterialPointer = 0x835A0F18u;
#endif
constexpr uint32_t kDevicePixelConstants = 0x1780u;  // pixel c0 (0x820D59C8)
// The frame's views as the back end walks them (0x82331150; FilmFrom below)
#if defined(COD4_MP)
// [waw-decomp-cod4] 2026-11-11 12:20: cod4mp's backEndData. The campaign's 0x82331150 is cod4mp's 0x82362300 (the
// first 24 instructions equal with branch targets and 16-bit fields left out, at one place in cod4mp's code); its
// loads of 0x835A13C4 (+0x10, +0x188, +0x22C) are loads of 0x84FD3EB4 there, and the struct offsets read beside them
// are the same.
constexpr uint32_t kBackEndDataPointer = 0x84FD3EB4u;
#else
constexpr uint32_t kBackEndDataPointer = 0x835A13C4u;     // 0x82331150's backEndData
#endif
constexpr uint32_t kBackEndViewCount = 0xB6028u, kBackEndViews = 0xB602Cu;
constexpr uint32_t kViewNoFilm = 0x13A4u;                   // nonzero: 0x82331150 draws no film pass for the view
constexpr uint32_t kViewFilm = 0x5734u, kViewFilmVectors = 0x6700u;
constexpr uint32_t kPassArgumentCounts = 0x48u;
constexpr uint32_t kPassArguments = 0x50u;
constexpr uint32_t kDeviceVertexConstants = 0x780u;
constexpr uint32_t kSourceCodeConstants = 0x800u;
constexpr uint32_t kCodeConstantSunPosition = 35, kCodeConstantSunDiffuse = 36;
constexpr uint32_t kCodeConstantWorldViewProjection = 78;  // 79 its inverse, 80 its transpose, 81 both
constexpr uint32_t kCodeConstantViewProjection = 74;       // the same four for the view-projection
constexpr uint32_t kCodeConstantWorld = 58;                // and for the world matrix (60 its transpose)
constexpr uint32_t kCodeImageLightmapPrimary = 4, kCodeImageLightmapSecondary = 5;
// [waw-decomp-cod4] 2026-11-11 round 5: the static models' light: the model lighting texture (code image 3, IW3's
// TEXTURE_SRC_CODE_MODEL_LIGHTING) and the vertex register the model's place in it goes to (c8: 0x820FC6C0 writes it
// from the instance record's +0x26, COD4-NATIVE.md round 4)
constexpr uint32_t kCodeImageModelLighting = 3;
constexpr uint32_t kModelLightingPlaceRegister = 8;
constexpr uint32_t kMaterialStateEntries = 0x18u;
constexpr uint32_t kMaterialTextureCount = 0x32u;
constexpr uint32_t kMaterialTextures = 0x3Cu;
constexpr uint32_t kMaterialStateTable = 0x44u;
constexpr uint32_t kSemanticColorMap = 2;
constexpr uint32_t kBufferData = 0x18u;    // a vertex buffer's fetch constant, an index buffer's address
constexpr uint32_t kTextureFetch = 0x1Cu;  // the fetch constant in a Direct3D texture (as Call of Duty 3's)
constexpr uint32_t kWorldSurfaces = 0;     // the surface type of the world's triangles
constexpr uint32_t kWorldStride = 44;
// [waw-decomp-cod4] 2026-11-11 10:10: the static models (surface type 1): 32-byte corners, as the model vertex shaders
// fetch them (run cod4side9's dump, shader_7897C054200E613C: vfetch Stride=8 FMT_32_32_32_32_FLOAT at +0 for xyz,
// Offset=4 FMT_8_8_8_8 the color at +0x10, Offset=5 FMT_32_32_FLOAT the texture coordinate at +0x14)
constexpr uint32_t kStaticModelSurfaces = 1;
constexpr uint32_t kModelStride = 32;
constexpr uint32_t kFirstLitTechnique = 7, kLastLitTechnique = 13;  // lit; with the sun, a spot or an omni light
constexpr uint32_t kSunTechniqueFirst = 8, kSunTechniqueLast = 9;   // lit by the sun (9: with its shadow map)

constexpr uint64_t kKeepEvery = 600;  // frames
constexpr uint64_t kKeepLast = 4800;
constexpr size_t kMaxMeshDraws = 16000;           // a kept frame's (ngl_vk takes 16384)
constexpr uint32_t kMaxMeshIndices = 0x60000u;    // one draw's; more is taken as a misread
constexpr uint32_t kPictureWidth = 1280, kPictureHeight = 720;  // the game's output (its 3D is 1024 x 600, stretched)
// The world's light is made halved (LightmapFor); 2 restores it (the lm_r0c0 pixel shaders have no factor of their own)
constexpr float kLightScale = 2.0f;

bool IsGuestData(uint32_t a) {
  return (a >= 0x40000000u && a < 0x7FFF0000u) || (a >= 0x82000000u && a < 0x90000000u) ||
         (a >= 0xA0000000u && a < 0xFFFF0000u);
}

bool KeptFrame(uint64_t frame) { return frame > 0 && frame % kKeepEvery == 0 && frame <= kKeepLast; }
// [waw-decomp-cod4] 2026-11-11 round 6: a frame whose picture is saved (the check, cod4_debug_native_side), and a frame
// drawn natively at all: those, or every frame when the native picture is the game's (cod4_native_vulkan,
// cod4_native_window.h). Only the saved frames are logged (Call of Duty 2's first run with every frame drawn logged 35
// MB in 100 s).
bool SavedFrame(uint64_t frame) { return REXCVAR_GET(cod4_debug_native_side) && KeptFrame(frame); }
bool DrawnFrame(uint64_t frame) { return SavedFrame(frame) || NativeVulkanOn(); }

// One texture of a draw: its Direct3D record and that record's fetch constant.
struct TextureRef {
  uint32_t texture = 0;  // 0: none
  std::array<uint32_t, 6> fetch{};
};

// One world draw kept for ngl_vk: its geometry, its textures and its view-projection.
struct MeshDraw {
  uint32_t vertices = 0;       // the physical address of stream 0's vertex 0 (the stream's offset added)
  uint32_t stride = 0;
  std::vector<uint32_t> used;  // the vertices its triangles use, three a triangle (the base vertex added)
  uint64_t key = 0;            // its geometry's fingerprint: the vertex address, the stride and those vertices
  uint32_t material = 0, technique_type = 0;
  uint32_t state0 = 0;         // the material's state bits for this technique, word 0 (IW3's GFXS0)
  TextureRef color, lightmap_primary, lightmap_secondary;
  float rows[4][4] = {};       // the vertex shader's four registers of the world-view-projection
  bool columns = true;         // the registers are the matrix's columns (dotted with (p, 1)), else its rows
  uint32_t matrix_index = 0;   // the code constant they came from (74, 76, 78 or 80)
  float sun_position[4] = {}, sun_diffuse[4] = {};  // the frame's code constants 35 and 36
  // the world matrix's four registers (code constant 60, its transpose, or 58): the 360's view-projection is relative
  // to the eye, and even the world's surfaces get a world matrix, which moves them by the eye's place (run cod4side7:
  // the lm_r0c0 passes' arguments 3:4:003C0004 and 3:0:004C0004). Identity when the pass has none.
  float world_rows[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
  // [waw-decomp-cod4] 08:55: the world surface the draw's batch starts with (FindSurface), -1 when not found, and its
  // bytes +0x2C (the lightmap index 0x8210C618 binds by) and +0x2D
  int surface = -1;
  uint32_t lightmap_index = 0xFFu, probe_index = 0xFFu;
  // [waw-decomp-cod4] 2026-11-11 09:55: the lightmap was read from sampler 3 and passed the page check there (a
  // k_8_8_8_8 texture twice as tall as wide); SetMeshes learns the pages of the lightmap indices from these draws
  bool page_checked = false;
  bool model = false;  // 10:10: a static model's draw (kStaticModelSurfaces): 32-byte corners, no lightmap
  // [waw-decomp-cod4] 2026-11-11 round 5: how many of the sampled corners landed on the screen with the registers taken
  // as columns and as rows (CornersOnScreen), for the vote by view in SetMeshes
  uint32_t on_screen_as_columns = 0, on_screen_as_rows = 0;
  // a static model's light: the model lighting texture's fetch constant (all 0: none found) and the model's place in
  // it (vertex c8), read at the draw; the texels are read at the frame's end (ModelLightTexels)
  std::array<uint32_t, 6> lighting_fetch{};
  float lighting_place[4] = {};
  bool lighting_place_from_packet = false;  // from the command buffer's packet (else the device's copy of c8)
};

struct Frame {
  std::vector<MeshDraw> meshes;
  uint32_t seen = 0;        // indexed draws
  uint32_t no_state = 0;    // without a run's state to read
  uint32_t not_world = 0;   // lit draws of other surface types or from other callers (not drawn yet)
  uint32_t misread = 0;     // a record that doesn't look like what it should be
  uint32_t no_matrix = 0;   // a pass without the world-view-projection among its arguments
  uint32_t dropped = 0;     // past kMaxMeshDraws
  // every indexed draw by (surface type, technique type): draws and triangles
  std::map<std::pair<uint32_t, uint32_t>, std::pair<uint32_t, uint32_t>> kinds;
  int samples = 0;          // kept draws logged in full (the first ones of a kept frame)
  int model_samples = 0;    // 10:10: the same for the static models' draws
  uint32_t model_draws = 0;
  uint32_t model_placed = 0;  // [waw-decomp-cod4] 12:25: of them, placed by their instance's matrix (InstancePlacement)
  uint32_t as_rows = 0;     // draws whose matrix registers were taken as rows (MeshDraw::columns)
  // [waw-decomp-cod4] 2026-11-11 09:55: every indexed draw by (surface type, the state's depth range kind): the
  // models in front of the player (the gun, the arms) are drawn with a depth range of their own in IW3; and the
  // last draw's source state and device, read again at the frame's end (the film pass's constants, the gamma ramp)
  // 10:05: keyed by (surface type, kind, near x 1000, far x 1000)
  std::map<std::tuple<uint32_t, uint32_t, int, int>, std::pair<uint32_t, uint32_t>> depth_kinds;
  uint32_t source = 0, device = 0;
  // 10:05: the frame's last draws (material, surface type, technique type), what is drawn after the scene; and the
  // film pass's draw when it goes through the indexed draw (its arguments and the pixel constants they set)
  std::deque<std::array<uint32_t, 3>> last_draws;
  std::string film_draw;
  uint32_t film_view = 0;  // 10:40: the frame's first view, read while the back end draws (SideDraw)
  // [waw-decomp-cod4] 2026-11-11 round 5: what draws the sky (none of the kinds drawn so far): the first material of
  // each (surface type, technique type), and the callers of the draws without a run's state
  std::map<std::pair<uint32_t, uint32_t>, uint32_t> kind_materials;
  // [waw-decomp-cod4] 2026-11-11 round 6: and the first technique of each kind (its name at +0), and for the technique
  // types 14 to 16 the first draw's pass arguments (type:destination:value): what technique type 15 is
  std::map<std::pair<uint32_t, uint32_t>, uint32_t> kind_techniques;
  std::map<std::pair<uint32_t, uint32_t>, std::string> kind_arguments;
  std::map<uint32_t, uint32_t> no_state_callers;  // caller -> draws
  // every material whose name has "sky" in it: its kind (surface type, technique type) and draws (run cod4mpside6's
  // first materials of each kind had none)
  std::map<std::string, std::pair<std::pair<uint32_t, uint32_t>, uint32_t>> sky_materials;
  uint32_t model_light_found = 0;  // static model draws whose model lighting texture and place were found
  uint32_t model_light_from_packet = 0;  // of them, with the place from the command buffer's packet
  int model_light_samples = 0;           // logged in full
  bool quiet = false;  // [waw-decomp-cod4] 2026-11-11 round 6: drawn for the window alone: no samples logged

  void Clear() {
    meshes.clear();
    seen = no_state = not_world = misread = no_matrix = dropped = as_rows = model_draws = model_placed = 0;
    model_light_found = model_light_from_packet = 0;
    model_light_samples = 0;
    sky_materials.clear();
    kind_materials.clear();
    kind_techniques.clear();
    kind_arguments.clear();
    no_state_callers.clear();
    kinds.clear();
    depth_kinds.clear();
    source = device = 0;
    last_draws.clear();
    film_draw.clear();
    film_view = 0;
    samples = model_samples = 0;
  }
};

// The draws come from the back end's threads; the frame's end from its render thread.
std::mutex g_frame_mutex;
Frame g_frame;
std::atomic<uint64_t> g_frames{0};

// ---------------------------------------------------------------------------------------------------------------
// The GPU's memory.

constexpr size_t kPhysicalBytes = 0x20000000u;  // the GPU's memory: every read stops at its end
size_t PhysicalRoom(size_t physical) { return physical < kPhysicalBytes ? kPhysicalBytes - physical : 0; }

// The host's mapping of the guest's physical memory: a page of the guest's 0xA0000000 view can be unmapped when the
// game allocated it through another view (Call of Duty 3's side138), so the GPU's data is read through this one.
const uint8_t* PhysicalBase() {
  static std::atomic<const uint8_t*> physical{nullptr};
  const uint8_t* p = physical.load(std::memory_order_acquire);
  if (!p) {
    auto* kernel = rex::system::kernel_state();
    p = kernel && kernel->memory() ? kernel->memory()->physical_membase() : nullptr;
    physical.store(p, std::memory_order_release);
  }
  return p;
}

// The GPU's data at a physical address (as fetch constants hold them).
const uint8_t* GpuPointer(const uint8_t* base, uint32_t physical) {
  const uint8_t* p = PhysicalBase();
  return p ? p + physical : base + (0xA0000000u | physical);
}

// A big-endian word of the GPU's memory at a physical address, 0 past its end.
uint32_t GpuU32(const uint8_t* base, uint32_t physical) {
  if (PhysicalRoom(physical) < 4) return 0;
  uint32_t v;
  std::memcpy(&v, GpuPointer(base, physical), 4);
  return __builtin_bswap32(v);
}

float GpuF32(const uint8_t* base, uint32_t physical) {
  const uint32_t bits = GpuU32(base, physical);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

// A 16-bit index of the GPU's memory (big-endian, the high half of its word when it starts the word).
uint32_t GpuU16(const uint8_t* base, uint32_t physical) {
  return (GpuU32(base, physical & ~3u) >> ((physical & 2u) ? 0u : 16u)) & 0xFFFFu;
}

// A guest address on the host: the console's physical memory (0xA0000000 to 0xDFFFFFFF, where the device, the heaps'
// buffers and the textures are) through the host's whole mapping of it (a page of the guest's own view of it can be
// unmapped, as Call of Duty 3's side138 found), the rest as it is.
const uint8_t* Host(const uint8_t* base, uint32_t addr) {
  if (addr >= 0xA0000000u && addr < 0xE0000000u) {
    if (const uint8_t* physical = PhysicalBase()) return physical + (addr & 0x1FFFFFFFu);
  }
  return base + addr;
}

uint32_t LoadU32(const uint8_t* base, uint32_t addr) {
  uint32_t v;
  std::memcpy(&v, Host(base, addr), 4);
  return __builtin_bswap32(v);
}

uint16_t LoadU16(const uint8_t* base, uint32_t addr) {
  uint16_t v;
  std::memcpy(&v, Host(base, addr), 2);
  return __builtin_bswap16(v);
}

uint8_t LoadU8(const uint8_t* base, uint32_t addr) { return *Host(base, addr); }

float LoadF32(const uint8_t* base, uint32_t addr) {
  const uint32_t bits = LoadU32(base, addr);
  float f;
  std::memcpy(&f, &bits, 4);
  return f;
}

// The text at addr (printable, at most 63 characters), "-" when none, "?" when not text.
std::string Text(const uint8_t* base, uint32_t addr) {
  if (!IsGuestData(addr) || !IsGuestData(addr + 64u)) return "-";
  std::string out;
  for (uint32_t i = 0; i < 63; ++i) {
    const unsigned char c = LoadU8(base, addr + i);
    if (c == 0) return out;
    if (c < 32 || c > 126) return "?";
    out += char(c);
  }
  return out;
}

// The name a record starts with (a material, a technique, a shader).
std::string NameOf(const uint8_t* base, uint32_t record) {
  return IsGuestData(record) ? Text(base, LoadU32(base, record)) : std::string("-");
}

// Where the textures' fetch constants were found in their Direct3D records, by offset / 4 (BoundTexture), for the log.
std::atomic<uint32_t> g_fetch_found[16];
std::atomic<uint32_t> g_fetch_missed{0};

// A plausible texture fetch constant: its type bits 2 (a texture), an address, a format, a known size.
bool LooksLikeTextureFetch(const std::array<uint32_t, 6>& fetch) {
  const uint32_t width = (fetch[2] & 0x1FFFu) + 1, height = ((fetch[2] >> 13) & 0x1FFFu) + 1;
  return (fetch[0] & 3u) == 2u && (fetch[1] & 0x1FFFF000u) != 0 && width <= 8192 && height <= 8192 &&
         (fetch[1] & 0x3Fu) != 0;
}

// What a sampler has bound: the state holds the image + 4, and there the pointer to the image's Direct3D texture,
// whose fetch constant (six words) is at kTextureFetch as in Call of Duty 3's records; the record's other places
// from +0x10 to +0x2C are tried too when that one doesn't look like one (counted for the log). Nothing when none
// does.
// [waw-decomp-cod4] 2026-11-11 09:55: split in two: TextureAt reads what a holder (a word that points to a Direct3D
// texture) holds; the world's lightmap pages are found through such holders too (SetMeshes).
TextureRef TextureAt(const uint8_t* base, uint32_t holder);

TextureRef BoundTexture(const uint8_t* base, uint32_t state, uint32_t sampler) {
  if (sampler >= 26) return TextureRef{};
  return TextureAt(base, LoadU32(base, state + kStateSamplerTextures + 4u * sampler));
}

TextureRef TextureAt(const uint8_t* base, uint32_t holder) {
  TextureRef ref;
  if (!IsGuestData(holder)) return ref;
  const uint32_t texture = LoadU32(base, holder);
  if (!IsGuestData(texture)) return ref;
  constexpr uint32_t kTried[] = {kTextureFetch, 0x18u, 0x20u, 0x10u, 0x14u, 0x24u, 0x28u, 0x2Cu};
  for (const uint32_t offset : kTried) {
    for (uint32_t w = 0; w < 6; ++w) ref.fetch[w] = LoadU32(base, texture + offset + 4u * w);
    if (LooksLikeTextureFetch(ref.fetch)) {
      ref.texture = texture;
      g_fetch_found[offset / 4u].fetch_add(1, std::memory_order_relaxed);
      return ref;
    }
  }
  g_fetch_missed.fetch_add(1, std::memory_order_relaxed);
  ref.fetch = {};
  return ref;
}
// [waw-decomp-cod4] 2026-11-11 12:25: a static model instance's placement. The campaign's 0x820FC6C0 (kind 1 of
// 0x820FCE28's groups; cod4mp's 0x8212D300) builds each instance's world matrix (the record at *(world + 0x2FC) +
// 0x2C x instance: its origin at +4 less the view's offset, its three packed axes at +0x10 unpacked with vupkd3d128
// and scaled by 8208.03125 / -24624.09375, i.e. x / 511, times the instance's scale) and writes it straight into the
// command buffer, not through the device's copy of the registers at +0x780 (which keeps the identity that the tess
// function 0x821046F0 set as the object's placement): a SET_CONSTANT packet, header 0xC0102D00 | *(device + 0x31A4),
// then 0x10 (c4, where the pass's code constant 58 goes), then the four rows; the device's write pointer
// *(device + 0x30) is left on its last word (the next write goes 4 bytes on), and 0x8209BD80 is called right after.
// So at the draw the four registers are read from the packet just before the write pointer. False when no such packet
// is there (0x820FCB60's kind and any other path): then the device's copy is kept, as before.
// [waw-decomp-cod4] 2026-11-11 round 5: the same reading for the packet before the last one (CommandPacketRegisters,
// `earlier` true): 0x820FC6C0 writes the model's place in the model lighting texture (c8, then c9 to c11 zero) in a
// packet of its own just before the world matrix's (c4, the last one). Each packet starts with three NOP words
// (0x80000000) after the write pointer and its header is then moved on so that its 16 floats start on 16 bytes (its
// decompile: `(0xfffffff8 - p & 0xf) + p`), so the earlier packet's header is 18 to 21 words before the last one's.
// Candidate 9 (run cod4mpside6) stepped back by whole packets of 18 words, missing it when padding sat between: its
// first model took the device's copy of c8 (0 0 0 0), a stale one for these models, and models came out white or black.
bool CommandPacketRegisters(const uint8_t* base, uint32_t device, uint32_t first_register, bool earlier,
                            float rows[4][4]) {
  constexpr uint32_t kCommandWritePointer = 0x30u;
  constexpr uint32_t kSetConstantHeader = 0xC0102D00u;  // a type-3 packet, SET_CONSTANT (0x2D), 17 words after it
  constexpr uint32_t kPacketLastWord = 17u * 4u;         // the header's distance from the packet's last word
  const uint32_t write = LoadU32(base, device + kCommandWritePointer);
  if (!IsGuestData(write) || !IsGuestData(write - kPacketLastWord)) return false;
  const auto is_header = [base](uint32_t at) {
    return IsGuestData(at) && (LoadU32(base, at) & 0xFFFFFF00u) == kSetConstantHeader;
  };
  uint32_t header = write - kPacketLastWord;
  if (!is_header(header)) return false;
  if (earlier) {
    uint32_t found = 0;
    for (uint32_t back = 18; back <= 21 && found == 0; ++back)
      if (is_header(header - 4u * back)) found = header - 4u * back;
    if (found == 0) return false;
    header = found;
  }
  if (LoadU32(base, header + 4u) != 4u * first_register) return false;
  float placed[4][4];
  for (uint32_t r = 0; r < 4; ++r)
    for (uint32_t k = 0; k < 4; ++k) {
      placed[r][k] = LoadF32(base, header + 8u + 16u * r + 4u * k);
      if (!std::isfinite(placed[r][k]) || std::fabs(placed[r][k]) > 1e7f) return false;
    }
  std::memcpy(rows, placed, sizeof placed);
  return true;
}

bool InstancePlacement(const uint8_t* base, uint32_t device, uint32_t first_register, float rows[4][4]) {
  return CommandPacketRegisters(base, device, first_register, false, rows);
}


// How many of up to 48 of a draw's corners land in front of the eye and on the screen through the world matrix's and
// the view-projection's registers taken as the matrices' columns (each dotted with (p, 1)) or as their rows (p.x row 0
// + p.y row 1 + p.z row 2 + row 3).
uint32_t CornersOnScreen(const uint8_t* base, const MeshDraw& m, bool columns) {
  uint32_t inside = 0;
  const size_t step = std::max<size_t>(1, m.used.size() / 48);
  for (size_t i = 0; i < m.used.size(); i += step) {
    const uint32_t v = m.vertices + m.used[i] * m.stride;
    const float corner[4] = {GpuF32(base, v), GpuF32(base, v + 4u), GpuF32(base, v + 8u), 1.0f};
    float p[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    for (int r = 0; r < 3; ++r)
      for (int k = 0; k < 4; ++k) p[r] += columns ? corner[k] * m.world_rows[r][k] : corner[k] * m.world_rows[k][r];
    float clip[4] = {};
    for (int r = 0; r < 4; ++r)
      for (int k = 0; k < 4; ++k) clip[r] += columns ? p[k] * m.rows[r][k] : p[k] * m.rows[k][r];
    if (clip[3] > 0.0f && std::fabs(clip[0]) <= clip[3] * 1.05f && std::fabs(clip[1]) <= clip[3] * 1.05f) ++inside;
  }
  return inside;
}

// Direct3D's SetTexture 0x82099D38 (called by 0x820D4D28 with the word at the state's sampler entry): the texture
// bound to sampler s is kept at device + 0x30F8 + 4 s, and the GPU's fetch constant built from its record (+0x1C to
// +0x30) at device + 0x480 + 24 s. The world's lightmaps are bound there without a pass argument (run cod4side7: the
// lm_r0c0 passes have only the color map's), so they are read from the device.
constexpr uint32_t kDeviceBoundTextures = 0x30F8u;
constexpr uint32_t kDeviceFetchConstants = 0x480u;

TextureRef DeviceTexture(const uint8_t* base, uint32_t device, uint32_t sampler) {
  TextureRef ref;
  const uint32_t texture = LoadU32(base, device + kDeviceBoundTextures + 4u * sampler);
  if (!IsGuestData(texture)) return ref;
  for (uint32_t w = 0; w < 6; ++w)
    ref.fetch[w] = LoadU32(base, device + kDeviceFetchConstants + 24u * sampler + 4u * w);
  if (LooksLikeTextureFetch(ref.fetch)) ref.texture = texture;
  return ref;
}

// [waw-decomp-cod4] 08:55: the world's surfaces as 0x8210C618 (the lit world's tess loop) walks them: 0x48 bytes
// each in the array at *(0x835A0F10) + 0x2F4; +0x04 the first vertex (the stream's offset is it x 44), +0x0C the
// first index (the draw's), +0x2C the lightmap index (0x1F: none), +0x2D the image index at its context +0x08. A batch
// is drawn with its first surface's first index and the stream set from its +0x04, so the draw finds its surface by
// those two (by the first index alone when no offset matches). -1 when not found.
#if defined(COD4_MP)
// [waw-decomp-cod4] 2026-11-11 round 5: cod4mp's world pointer, read from its own code. The campaign's static model
// draw 0x820FC6C0 loads the world plainly (the decompile's `iVar10 = DAT_835a0f10`, line 71 of the function); the
// same function in cod4mp, 0x8212D300 (round 4's match), has `iVar10 = DAT_84cd3510` at the same line. Round 3's
// 0x84CD0F10 (taken by the block's offset) held no world: run cod4mpside5 found no surface for any draw ("lightmap
// pages by index, the array not found").
constexpr uint32_t kWorldPointer = 0x84CD3510u;
#else
constexpr uint32_t kWorldPointer = 0x835A0F10u;
#endif
constexpr uint32_t kLightmapSampler = 3;  // the lm shaders' tf3 (KeepWorldDraw)
constexpr uint32_t kWorldSurfaceArray = 0x2F4u, kSurfaceSize = 0x48u, kMaxSurfaces = 32768u;

// [waw-decomp-cod4] 2026-11-11 12:00: the world (*kWorldPointer), 0 when none. In cod4mp, whose address is taken
// from the campaign's by the block's offset (not read from its own code), only a world whose name starts with
// "maps/" (GfxWorld's first field) is taken.
uint32_t TheWorld(const uint8_t* base) {
  const uint32_t world = LoadU32(base, kWorldPointer);
  if (!IsGuestData(world)) return 0;
#if defined(COD4_MP)
  if (Text(base, LoadU32(base, world)).rfind("maps/", 0) != 0) return 0;
#endif
  return world;
}

int FindSurface(const uint8_t* base, uint32_t first_index, uint32_t stream_offset, uint32_t& lightmap,
                uint32_t& probe) {
  const uint32_t world = TheWorld(base);
  if (!IsGuestData(world)) return -1;
  const uint32_t surfaces = LoadU32(base, world + kWorldSurfaceArray);
  if (!IsGuestData(surfaces)) return -1;
  int by_index = -1;
  for (uint32_t i = 0; i < kMaxSurfaces; ++i) {
    const uint32_t s = surfaces + kSurfaceSize * i;
    if (!IsGuestData(s + kSurfaceSize)) break;
    if (LoadU32(base, s + 0x0Cu) != first_index) continue;
    if (by_index < 0) by_index = static_cast<int>(i);
    if (LoadU32(base, s + 0x04u) * kWorldStride == stream_offset) {
      by_index = static_cast<int>(i);
      break;
    }
  }
  if (by_index >= 0) {
    const uint32_t s = surfaces + kSurfaceSize * static_cast<uint32_t>(by_index);
    lightmap = LoadU8(base, s + 0x2Cu);
    probe = LoadU8(base, s + 0x2Du);
  }
  return by_index;
}

// Keeps one lit world draw: everything the native picture needs is copied or noted now, at the game's own draw.
void KeepWorldDraw(const uint8_t* base, uint32_t state, uint32_t source, uint32_t device, uint32_t base_vertex,
                   uint32_t first_index, uint32_t index_count, uint32_t technique_type, Frame& f, bool model) {
  if (f.meshes.size() >= kMaxMeshDraws) {
    ++f.dropped;
    return;
  }
  const uint32_t material = LoadU32(base, state + kStateMaterial), pass = LoadU32(base, state + kStatePass);
  const uint32_t index_buffer = LoadU32(base, state + kStateIndexBuffer);
  const uint32_t stride = LoadU32(base, state + kStateStream0);
  const uint32_t vertex_buffer = LoadU32(base, state + kStateStream0 + 4u);
  const uint32_t offset = LoadU32(base, state + kStateStream0 + 8u);
  if (!IsGuestData(material) || !IsGuestData(pass) || !IsGuestData(index_buffer) || !IsGuestData(vertex_buffer) ||
      stride != (model ? kModelStride : kWorldStride) || index_count == 0 || index_count > kMaxMeshIndices ||
      index_count % 3 != 0) {
    ++f.misread;
    return;
  }
  MeshDraw m;
  m.material = material;
  m.technique_type = technique_type;
  m.stride = stride;
  m.vertices = (LoadU32(base, vertex_buffer + kBufferData) & 0x1FFFFFFCu) + offset;
  const bool wide = (LoadU32(base, index_buffer) >> 31) != 0;
  const uint32_t indices =
      (LoadU32(base, index_buffer + kBufferData) & 0x1FFFFFFFu) + (wide ? 4u : 2u) * first_index;
  // the indices now (a buffer the game fills anew each frame would have others later); the fingerprint is FNV-1a
  // over the vertex address, the stride and the vertices used
  uint64_t key = 14695981039346656037ull;
  auto mix = [&key](uint32_t word) {
    for (int b = 0; b < 4; ++b) key = (key ^ ((word >> (8 * b)) & 0xFFu)) * 1099511628211ull;
  };
  mix(m.vertices);
  mix(m.stride);
  m.used.resize(index_count);
  for (uint32_t i = 0; i < index_count; ++i) {
    m.used[i] = base_vertex + (wide ? GpuU32(base, indices + 4u * i) : GpuU16(base, indices + 2u * i));
    mix(m.used[i]);
  }
  m.key = key;
  m.model = model;
  if (!model) m.surface = FindSurface(base, first_index, offset, m.lightmap_index, m.probe_index);
  // the pass's arguments: where the world-view-projection goes, and which samplers take the color map and the
  // lightmaps
  const uint32_t argument_count = uint32_t(LoadU8(base, pass + kPassArgumentCounts)) +
                                  LoadU8(base, pass + kPassArgumentCounts + 1u) +
                                  LoadU8(base, pass + kPassArgumentCounts + 2u);
  const uint32_t arguments = LoadU32(base, pass + kPassArguments);
  const uint32_t texture_count = LoadU8(base, material + kMaterialTextureCount);
  const uint32_t textures = LoadU32(base, material + kMaterialTextures);
  int matrix_register = -1, color_sampler = -1, first_sampler = -1, primary_sampler = -1, secondary_sampler = -1;
  int world_register = -1;
  int model_lighting_sampler = -1;  // [waw-decomp-cod4] 2026-11-11 round 5
  uint32_t material_samplers = 0;  // the samplers the pass's material textures go to, one bit each
  for (uint32_t i = 0; i < argument_count && i < 64 && IsGuestData(arguments); ++i) {
    const uint32_t argument = arguments + 8u * i;
    const uint32_t type = LoadU16(base, argument), destination = LoadU16(base, argument + 2u);
    const uint32_t value = LoadU32(base, argument + 4u);
    if (type == 3) {  // a code constant for the vertex shader: index, first row, row count
      const uint32_t index = value >> 16, first_row = (value >> 8) & 0xFFu;
      const bool matrix = index == kCodeConstantWorldViewProjection || index == kCodeConstantWorldViewProjection + 2u ||
                          index == kCodeConstantViewProjection || index == kCodeConstantViewProjection + 2u;
      if (matrix && first_row == 0 && matrix_register < 0) {
        matrix_register = static_cast<int>(destination);
        m.matrix_index = index;
      }
      if ((index == kCodeConstantWorld || index == kCodeConstantWorld + 2u) && first_row == 0)
        world_register = static_cast<int>(destination);
    } else if (type == 2) {  // one of the material's textures, by its name's hash
      if (first_sampler < 0) first_sampler = static_cast<int>(destination);
      if (destination < 32) material_samplers |= 1u << destination;
      for (uint32_t t = 0; t < texture_count && t < 16 && IsGuestData(textures); ++t)
        if (LoadU32(base, textures + 12u * t) == value && LoadU8(base, textures + 12u * t + 7u) == kSemanticColorMap)
          color_sampler = static_cast<int>(destination);
    } else if (type == 4) {  // a code image
      if (value == kCodeImageLightmapPrimary) primary_sampler = static_cast<int>(destination);
      if (value == kCodeImageLightmapSecondary) secondary_sampler = static_cast<int>(destination);
      // [waw-decomp-cod4] 2026-11-11 round 5: the model lighting texture (World at War's helper read the same
      // engine's code image 3 in the low half of the word: "4/4/00000003", WAW-NATIVE.md round 3)
      if ((value & 0xFFFFu) == kCodeImageModelLighting && model_lighting_sampler < 0)
        model_lighting_sampler = static_cast<int>(destination);
    }
  }
  if (matrix_register < 0 || matrix_register > 252) {
    ++f.no_matrix;
    return;
  }
  if (color_sampler < 0) color_sampler = first_sampler;
  if (color_sampler >= 0) m.color = BoundTexture(base, state, static_cast<uint32_t>(color_sampler));
  if (primary_sampler >= 0) m.lightmap_primary = BoundTexture(base, state, static_cast<uint32_t>(primary_sampler));
  if (secondary_sampler >= 0)
    m.lightmap_secondary = BoundTexture(base, state, static_cast<uint32_t>(secondary_sampler));
  // no lightmap among the arguments: the first two textures the device has bound beside the material's (samplers 1
  // to 7), taken as the primary and the secondary lightmap (in Killhouse the primary is sampler 3, the lm_r0c0
  // shaders' tf3, both lightmaps stacked in it: LightmapFor)
  // [waw-decomp-cod4] 09:00: the lm shaders read the lightmap at tf3, and run cod4side13 showed sampler 3 holds the
  // page of the surface's lightmap index (index 0 B21D8F84 1024 x 2048, 1 B21D9020 512 x 2048, 2 B21D90BC 512 x 1024)
  // while samplers 1 and 2 often hold other images (B216B5C8, B216B614, ... 0x4C apart: probably the reflection
  // probes, bound by the surface's +0x2D), which the scan below took as the lightmap for most draws. So sampler 3
  // first, when no material texture goes there and it holds a 32-bit texture twice as tall as wide.
  if (!model && primary_sampler < 0 && secondary_sampler < 0 && !(material_samplers & (1u << kLightmapSampler))) {
    const TextureRef ref = DeviceTexture(base, device, kLightmapSampler);
    const uint32_t width = (ref.fetch[2] & 0x1FFFu) + 1, height = ((ref.fetch[2] >> 13) & 0x1FFFu) + 1;
    // [waw-decomp-cod4] 2026-11-11 round 5: or four times as tall, as 2D textures (fetch word 5's dimension 1; the
    // reflection probes are cubes): mp_crash's page is 512 x 2048 (run cod4mpside7: AEE63060), as Killhouse's index 1
    // (B21D9020, above); with twice-as-tall only, cod4mp checked no page at all and took what sampler 3 held: for 479
    // of index 0's 490 draws at frame 1200 another image (AEE01E80, AEE02178, ... 0x4C apart: the reflection probes),
    // and the walls came out white
    const bool flat = ((ref.fetch[5] >> 9) & 3u) == 1u;
    if (ref.texture != 0 && (ref.fetch[1] & 0x3Fu) == 6 && flat && (height == 2 * width || height == 4 * width)) {
      m.lightmap_primary = m.lightmap_secondary = ref;
      primary_sampler = secondary_sampler = static_cast<int>(kLightmapSampler);
      m.page_checked = true;
    }
  }
  if (!model && primary_sampler < 0 && secondary_sampler < 0) {
    int found = 0;
    for (uint32_t sampler = 1; sampler < 8 && found < 2; ++sampler) {
      if (material_samplers & (1u << sampler)) continue;
      const TextureRef ref = DeviceTexture(base, device, sampler);
      if (ref.texture == 0) continue;
      (found == 0 ? m.lightmap_primary : m.lightmap_secondary) = ref;
      (found == 0 ? primary_sampler : secondary_sampler) = static_cast<int>(sampler);
      ++found;
    }
    if (found == 1) m.lightmap_secondary = m.lightmap_primary;
  }
  if (world_register >= 0 && world_register <= 252)
    for (uint32_t r = 0; r < 4; ++r)
      for (uint32_t k = 0; k < 4; ++k)
        m.world_rows[r][k] = LoadF32(base, device + kDeviceVertexConstants +
                                               16u * (static_cast<uint32_t>(world_register) + r) + 4u * k);
  // [waw-decomp-cod4] 2026-11-11 12:25: a static model's own placement, from the command buffer (InstancePlacement)
  bool placed_by_instance = false;
  if (model && world_register >= 0 && world_register <= 252 &&
      InstancePlacement(base, device, static_cast<uint32_t>(world_register), m.world_rows)) {
    ++f.model_placed;
    placed_by_instance = true;
  }
  // [waw-decomp-cod4] 2026-11-11 round 5: a static model's light: the model lighting texture (code image 3, by the
  // pass's argument, else the first 3D texture the device has bound in samplers 0 to 15, as World at War's helper
  // found it) and the model's place in it, c8 (0x820FC6C0's packet beside the placement's; else the device's copy)
  if (model) {
    int sampler = model_lighting_sampler;
    for (uint32_t s = 0; s < 16 && sampler < 0; ++s) {
      const TextureRef ref = DeviceTexture(base, device, s);
      if (ref.texture != 0 && ((ref.fetch[5] >> 9) & 3u) == 2u) sampler = static_cast<int>(s);
    }
    if (sampler >= 0 && sampler < 16) {
      const TextureRef ref = DeviceTexture(base, device, static_cast<uint32_t>(sampler));
      if (ref.texture != 0) m.lighting_fetch = ref.fetch;
    }
    // a model placed by its instance's packet has its place in the packet before (the device's copy of c8 is another
    // model's then: left at 0, outside the texture, so it is drawn unlit); one placed otherwise, the device's copy
    float packet[4][4];
    m.lighting_place_from_packet = CommandPacketRegisters(base, device, kModelLightingPlaceRegister, true, packet);
    const uint32_t device_place = device + kDeviceVertexConstants + 16u * kModelLightingPlaceRegister;
    for (uint32_t k = 0; k < 4; ++k)
      m.lighting_place[k] = m.lighting_place_from_packet ? packet[0][k]
                            : placed_by_instance          ? 0.0f
                                                          : LoadF32(base, device_place + 4u * k);
    if (m.lighting_fetch[0] != 0) ++f.model_light_found;
    f.model_light_from_packet += m.lighting_place_from_packet;
    if (!f.quiet && f.model_light_samples < 3 && m.lighting_place_from_packet) {  // the first few: the place
      ++f.model_light_samples;
      REXLOG_INFO("cod4: native side: model light sample: material \"{}\", place {:.5f} {:.5f} {:.5f} {:.5f} (from "
                  "the command buffer's packet)", NameOf(base, material), m.lighting_place[0], m.lighting_place[1],
                  m.lighting_place[2], m.lighting_place[3]);
    }
  }
  for (uint32_t r = 0; r < 4; ++r)
    for (uint32_t k = 0; k < 4; ++k)
      m.rows[r][k] = LoadF32(base, device + kDeviceVertexConstants +
                                        16u * (static_cast<uint32_t>(matrix_register) + r) + 4u * k);
  // columns or rows: the way more corners land on the screen; on a tie, the transposed constants (60, 76, 80) are
  // columns, as Direct3D's shaders take a matrix by default
  const uint32_t as_columns = CornersOnScreen(base, m, true), as_rows = CornersOnScreen(base, m, false);
  const bool transposed = m.matrix_index == kCodeConstantWorldViewProjection + 2u ||
                          m.matrix_index == kCodeConstantViewProjection + 2u;
  m.columns = as_columns != as_rows ? as_columns > as_rows : transposed;
  if (!m.columns) ++f.as_rows;
  m.on_screen_as_columns = as_columns;  // [waw-decomp-cod4] 2026-11-11 round 5: for the vote by view (SetMeshes)
  m.on_screen_as_rows = as_rows;
  for (uint32_t k = 0; k < 4; ++k) {
    m.sun_position[k] = LoadF32(base, source + kSourceCodeConstants + 16u * kCodeConstantSunPosition + 4u * k);
    m.sun_diffuse[k] = LoadF32(base, source + kSourceCodeConstants + 16u * kCodeConstantSunDiffuse + 4u * k);
  }
  const uint32_t entry = LoadU8(base, material + kMaterialStateEntries + technique_type);
  const uint32_t state_table = LoadU32(base, material + kMaterialStateTable);
  m.state0 = entry != 0xFFu && IsGuestData(state_table) ? LoadU32(base, state_table + 8u * entry) : 0x800u;
  if (model) {  // 10:10: the static models: the first two of a kept frame in short
    ++f.model_draws;
    if (!f.quiet && f.model_samples < 2) {  // [waw-decomp-cod4] round 6: saved frames only
      ++f.model_samples;
      const uint32_t v = m.vertices + m.used[0] * stride;
      REXLOG_INFO("cod4: native side: model sample: material \"{}\", technique type {}, {} indices, color map "
                  "{:08X} (sampler {}), matrix code constant {} at c{}, world matrix at c{} (moves by {:.1f} {:.1f} "
                  "{:.1f}); first corner {:.2f} {:.2f} {:.2f}, color {:08X}, texture coordinate {:.4f} {:.4f}",
                  NameOf(base, material), technique_type, index_count, m.color.texture, color_sampler,
                  m.matrix_index, matrix_register, world_register, m.world_rows[0][3], m.world_rows[1][3],
                  m.world_rows[2][3], GpuF32(base, v), GpuF32(base, v + 4u), GpuF32(base, v + 8u),
                  GpuU32(base, v + 0x10u), GpuF32(base, v + 0x14u), GpuF32(base, v + 0x18u));
    }
  } else if (!f.quiet && f.samples < 6) {  // the first ones of a kept frame in full (08:55: 6, was 3; round 6: saved)
    ++f.samples;
    std::string listed;
    for (uint32_t i = 0; i < argument_count && i < 24 && IsGuestData(arguments); ++i) {
      char entry[48];
      std::snprintf(entry, sizeof entry, " %u:%u:%08X", LoadU16(base, arguments + 8u * i),
                    LoadU16(base, arguments + 8u * i + 2u), LoadU32(base, arguments + 8u * i + 4u));
      listed += entry;
    }
    std::string bound;
    for (uint32_t sampler = 0; sampler < 16; ++sampler) {
      const TextureRef ref = DeviceTexture(base, device, sampler);
      if (ref.texture == 0) continue;
      char entry[64];
      std::snprintf(entry, sizeof entry, " s%u:%08X fmt %u %ux%u", sampler, ref.texture, ref.fetch[1] & 0x3Fu,
                    (ref.fetch[2] & 0x1FFFu) + 1, ((ref.fetch[2] >> 13) & 0x1FFFu) + 1);
      bound += entry;
    }
    float uv_low[2] = {1e30f, 1e30f}, uv_high[2] = {-1e30f, -1e30f};
    for (const uint32_t vertex : m.used) {
      for (uint32_t k = 0; k < 2; ++k) {
        const float value = GpuF32(base, m.vertices + vertex * stride + 0x1Cu + 4u * k);
        uv_low[k] = std::min(uv_low[k], value);
        uv_high[k] = std::max(uv_high[k], value);
      }
    }
    REXLOG_INFO("cod4: native side: sample: lightmap coordinates u {:.4f} to {:.4f}, v {:.4f} to {:.4f}", uv_low[0],
                uv_high[0], uv_low[1], uv_high[1]);
    // [waw-decomp-cod4] 08:55: the page: the surface's lightmap index beside the texture the lightmap is read from,
    // and the fetch constants' signs (dword 0 bits 2-9, two bits a channel x y z w: 0 unsigned, 1 signed, 2 biased,
    // 3 gamma)
    auto signs = [](uint32_t dword0) {
      char text[8];
      std::snprintf(text, sizeof text, "%u%u%u%u", (dword0 >> 2) & 3u, (dword0 >> 4) & 3u, (dword0 >> 6) & 3u,
                    (dword0 >> 8) & 3u);
      return std::string(text);
    };
    REXLOG_INFO("cod4: native side: sample: surface {} (lightmap index {}, +0x2D {}), first index {}, stream offset "
                "{} ({} corners); the lightmap read: sampler {} texture {:08X} fetch {:08X} {:08X} {:08X} signs {}; "
                "the color map's signs {} (format {})",
                m.surface, m.lightmap_index, m.probe_index, first_index, offset, offset / kWorldStride,
                primary_sampler, m.lightmap_primary.texture, m.lightmap_primary.fetch[0], m.lightmap_primary.fetch[1],
                m.lightmap_primary.fetch[2], signs(m.lightmap_primary.fetch[0]), signs(m.color.fetch[0]),
                m.color.fetch[1] & 0x3Fu);
    REXLOG_INFO("cod4: native side: sample arguments (type:destination:value):{}; the device's samplers:{}; world "
                "matrix at c{}: moves by {:.1f} {:.1f} {:.1f}", listed, bound, world_register, m.world_rows[0][3],
                m.world_rows[1][3], m.world_rows[2][3]);
    const uint32_t v = m.vertices + m.used[0] * stride;
    const float p[3] = {GpuF32(base, v), GpuF32(base, v + 4u), GpuF32(base, v + 8u)};
    float q[3] = {};
    for (int r = 0; r < 3; ++r)
      q[r] = m.columns ? p[0] * m.world_rows[r][0] + p[1] * m.world_rows[r][1] + p[2] * m.world_rows[r][2] +
                             m.world_rows[r][3]
                       : p[0] * m.world_rows[0][r] + p[1] * m.world_rows[1][r] + p[2] * m.world_rows[2][r] +
                             m.world_rows[3][r];
    float clip[4];
    for (int r = 0; r < 4; ++r)
      clip[r] = m.columns ? q[0] * m.rows[r][0] + q[1] * m.rows[r][1] + q[2] * m.rows[r][2] + m.rows[r][3]
                          : q[0] * m.rows[0][r] + q[1] * m.rows[1][r] + q[2] * m.rows[2][r] + m.rows[3][r];
    REXLOG_INFO("cod4: native side: sample: material \"{}\", technique type {} \"{}\", {} indices from {} (base "
                "vertex {}), vertices at {:08X}; matrix (code constant {}, corners on screen as columns {}, as rows "
                "{}) at c{}, samplers color {} primary {} secondary {}; textures "
                "{:08X} {:08X} {:08X}, color fetch {:08X} {:08X} {:08X}; state word 0 {:08X}; first corner {:.1f} "
                "{:.1f} {:.1f} -> clip {:.2f} {:.2f} {:.4f} {:.2f}; sun {:.3f} {:.3f} {:.3f}, color {:.3f} {:.3f} "
                "{:.3f}",
                NameOf(base, material), technique_type, NameOf(base, LoadU32(base, state + kStateTechnique)),
                index_count, first_index, base_vertex, m.vertices, m.matrix_index, as_columns, as_rows,
                matrix_register, color_sampler, primary_sampler,
                secondary_sampler, m.color.texture, m.lightmap_primary.texture, m.lightmap_secondary.texture,
                m.color.fetch[0], m.color.fetch[1], m.color.fetch[2], m.state0, p[0], p[1], p[2], clip[0], clip[1],
                clip[2], clip[3], m.sun_position[0], m.sun_position[1], m.sun_position[2], m.sun_diffuse[0],
                m.sun_diffuse[1], m.sun_diffuse[2]);
  }
  f.meshes.push_back(std::move(m));
}

// ---------------------------------------------------------------------------------------------------------------
// The helper thread: saves the native picture and the game's own of the same moment.

struct Job {
  uint64_t frame = 0;
  std::vector<uint8_t> rgba;  // the native picture, 4 bytes a pixel (red first)
  uint32_t width = 0, height = 0;
  std::string summary;  // what was drawn (without the word "to": the compare tool looks for " to <picture>")
  std::vector<std::vector<uint8_t>> variants;  // cod4_debug_native_side_variants: the same frame lit other ways
};

std::mutex g_jobs_mutex;
std::condition_variable g_jobs_cv;
std::deque<Job> g_jobs;
bool g_stop = false;
std::thread g_helper;
std::filesystem::path g_folder;
SideCaptureFn g_capture;

bool WritePpm(const std::filesystem::path& file, const uint8_t* rgba, size_t stride, uint32_t width,
              uint32_t height) {
  std::ofstream out(file, std::ios::binary);
  if (!out) return false;
  out << "P6\n" << width << " " << height << "\n255\n";
  std::string row(size_t(width) * 3, '\0');
  for (uint32_t y = 0; y < height; ++y) {
    const uint8_t* src = rgba + size_t(y) * stride;
    for (uint32_t x = 0; x < width; ++x) {
      row[x * 3] = char(src[x * 4]);
      row[x * 3 + 1] = char(src[x * 4 + 1]);
      row[x * 3 + 2] = char(src[x * 4 + 2]);
    }
    out.write(row.data(), std::streamsize(row.size()));
  }
  return bool(out);
}

uint32_t Crc32(const uint8_t* data, size_t size, uint32_t crc = 0) {
  static const std::array<uint32_t, 256> table = [] {
    std::array<uint32_t, 256> t{};
    for (uint32_t n = 0; n < 256; ++n) {
      uint32_t c = n;
      for (int k = 0; k < 8; ++k) c = (c & 1) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      t[n] = c;
    }
    return t;
  }();
  crc = ~crc;
  for (size_t i = 0; i < size; ++i) crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
  return ~crc;
}

void PutU32(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(uint8_t(v >> 24));
  out.push_back(uint8_t(v >> 16));
  out.push_back(uint8_t(v >> 8));
  out.push_back(uint8_t(v));
}

// One PNG chunk: its length, its type, its data and the CRC of type and data.
void PutChunk(std::vector<uint8_t>& out, const char type[4], const std::vector<uint8_t>& data) {
  PutU32(out, uint32_t(data.size()));
  const size_t start = out.size();
  out.insert(out.end(), type, type + 4);
  out.insert(out.end(), data.begin(), data.end());
  PutU32(out, Crc32(out.data() + start, out.size() - start));
}

// The picture's red, green and blue as a PNG (the compare tool takes the game's picture as a .png), its data in stored
// (uncompressed) deflate blocks, so it needs nothing beyond standard C++; the file is about the size of a PPM. Written
// to <file>.part, then renamed, so a half-written file never has the real name.
bool WritePng(const std::filesystem::path& file, const rex::ui::RawImage& image) {
  const uint32_t width = image.width, height = image.height;
  std::vector<uint8_t> raw;  // each row: its filter (0, none), then its pixels
  raw.reserve((size_t(width) * 3 + 1) * height);
  for (uint32_t y = 0; y < height; ++y) {
    raw.push_back(0);
    const uint8_t* src = image.data.data() + size_t(y) * image.stride;
    for (uint32_t x = 0; x < width; ++x) raw.insert(raw.end(), src + x * 4, src + x * 4 + 3);
  }
  std::vector<uint8_t> z = {0x78, 0x01};  // zlib: deflate, a 32 KB window, no preset dictionary
  size_t at = 0;
  do {
    const uint16_t n = uint16_t(std::min<size_t>(65535, raw.size() - at));
    z.push_back(at + n == raw.size() ? 1 : 0);  // the last block, stored
    z.push_back(uint8_t(n));
    z.push_back(uint8_t(n >> 8));
    z.push_back(uint8_t(~n));
    z.push_back(uint8_t(uint16_t(~n) >> 8));
    z.insert(z.end(), raw.begin() + std::ptrdiff_t(at), raw.begin() + std::ptrdiff_t(at + n));
    at += n;
  } while (at < raw.size());
  uint32_t a = 1, b = 0;  // Adler-32 of the raw data
  for (uint8_t byte : raw) {
    a = (a + byte) % 65521u;
    b = (b + a) % 65521u;
  }
  PutU32(z, (b << 16) | a);
  std::vector<uint8_t> header;
  PutU32(header, width);
  PutU32(header, height);
  header.insert(header.end(), {8, 2, 0, 0, 0});  // 8 bits a channel, red green blue, no interlace
  std::vector<uint8_t> png = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
  PutChunk(png, "IHDR", header);
  PutChunk(png, "IDAT", z);
  PutChunk(png, "IEND", {});
  std::filesystem::path part = file;
  part += ".part";
  {
    std::ofstream out(part, std::ios::binary);
    if (!out) return false;
    out.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size()));
    if (!out) return false;
  }
  std::error_code ec;
  std::filesystem::rename(part, file, ec);
  if (ec) std::filesystem::remove(part, ec);
  return !ec;
}

// The game's own picture (the frame the GPU emulation last sent out), as cod4_debug_screenshots reads it.
bool CaptureGame(rex::ui::RawImage& image) {
  for (int attempt = 0; attempt < 5; ++attempt) {
    rex::ui::RawImage shot;
    if (g_capture && g_capture(shot) && shot.width && shot.height && shot.stride >= size_t(shot.width) * 4 &&
        shot.data.size() >= shot.stride * (shot.height - 1) + size_t(shot.width) * 4) {
      image = std::move(shot);
      return true;
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  return false;
}

void SaveJob(const Job& job) {
  std::error_code ec;
  std::filesystem::create_directories(g_folder, ec);
  const std::string stem = "ngl-side-" + std::to_string(job.frame);
  const std::filesystem::path native = g_folder / (stem + "-vk.ppm");
  if (!WritePpm(native, job.rgba.data(), size_t(job.width) * 4, job.width, job.height)) {
    REXLOG_WARN("cod4: native side: frame {}: the native picture couldn't be saved ({})", job.frame, native.string());
    return;
  }
  std::string variants;
  for (size_t k = 0; k < job.variants.size(); ++k) {
    const std::string name = stem + "-m" + std::to_string(k + 1) + ".ppm";
    if (job.variants[k].size() == job.rgba.size() &&
        WritePpm(g_folder / name, job.variants[k].data(), size_t(job.width) * 4, job.width, job.height))
      variants += " " + name;
  }
  if (!variants.empty()) REXLOG_INFO("cod4: native side: frame {}: variants:{}", job.frame, variants);
  std::string game = "not taken";
  rex::ui::RawImage image;
  if (CaptureGame(image)) {
    const std::string name = stem + "-game.png";
    if (WritePng(g_folder / name, image)) game = name;
  }
  REXLOG_INFO("cod4: native side: frame {}: {} to {}; the game's own: {}", job.frame, job.summary, native.string(),
              game);
}

void RunHelper() {
  for (;;) {
    Job job;
    {
      std::unique_lock lock(g_jobs_mutex);
      g_jobs_cv.wait(lock, [] { return g_stop || !g_jobs.empty(); });
      if (g_jobs.empty()) return;  // stopping, nothing left
      job = std::move(g_jobs.front());
      g_jobs.pop_front();
    }
    SaveJob(job);
  }
}

[[maybe_unused]] void HandOver(Job job) {
  {
    std::lock_guard lock(g_jobs_mutex);
    if (!g_helper.joinable() || g_jobs.size() >= 2) {
      REXLOG_WARN("cod4: native side: frame {}: no helper to save it (started with the check off?)", job.frame);
      return;
    }
    g_jobs.push_back(std::move(job));
  }
  g_jobs_cv.notify_one();
}

#if defined(COD4_NGL_VK)
// ---------------------------------------------------------------------------------------------------------------
// Xenos textures to BGRA8: the same formats, tiling and packed-mip rule as Call of Duty 3's ngl_side.cpp (from
// rexglue's and Xenia's texture code), read through the host's whole mapping of physical memory.

constexpr size_t kMaxTextureBytes = 32u << 20;

struct Format {
  uint32_t block;  // texels across a block (1 or 4)
  uint32_t bytes;  // bytes a block
  uint32_t log2_bytes;
};

bool FormatOf(uint32_t format, Format* f) {
  switch (format) {
    case 2:   // k_8
    case 8:   // k_8_A
    case 9:   // k_8_B
      *f = {1, 1, 0};
      return true;
    case 10:  // k_8_8
    case 3:   // k_1_5_5_5
    case 4:   // k_5_6_5
    case 5:   // k_6_5_5
    case 15:  // k_4_4_4_4
      *f = {1, 2, 1};
      return true;
    case 6:   // k_8_8_8_8
    case 50:  // k_8_8_8_8_AS_16_16_16_16
    case 7:   // k_2_10_10_10
    case 54:  // k_2_10_10_10_AS_16_16_16_16
    case 16:  // k_10_11_11
    case 55:  // k_10_11_11_AS_16_16_16_16
    case 17:  // k_11_11_10
    case 56:  // k_11_11_10_AS_16_16_16_16
      *f = {1, 4, 2};
      return true;
    case 18:  // k_DXT1
    case 51:  // k_DXT1_AS_16_16_16_16
    case 58:  // k_DXT3A
    case 59:  // k_DXT5A
      *f = {4, 8, 3};
      return true;
    case 19:  // k_DXT2_3
    case 20:  // k_DXT4_5
    case 52:  // k_DXT2_3_AS_16_16_16_16
    case 53:  // k_DXT4_5_AS_16_16_16_16
    case 49:  // k_DXN
      *f = {4, 16, 4};
      return true;
    default:
      return false;
  }
}

// The Xenos 2D tiling, in blocks: the row's part, then the block's offset (Xenia's TiledOffset2DOuter / Inner).
int32_t TiledRow(int32_t y, int32_t width, uint32_t log2_bytes) {
  const int32_t macro = ((y >> 5) * (width >> 5)) << (log2_bytes + 7);
  const int32_t micro = ((y & 6) << 2) << log2_bytes;
  return macro + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 8) << (3 + log2_bytes)) + ((y & 1) << 4);
}

int32_t TiledOffset(int32_t x, int32_t y, uint32_t log2_bytes, int32_t row) {
  const int32_t macro = (x >> 5) << (log2_bytes + 7);
  const int32_t micro = (x & 7) << log2_bytes;
  const int32_t offset = row + (macro + ((micro & ~0xF) << 1) + (micro & 0xF));
  return ((offset & ~0x1FF) << 3) + ((offset & 0x1C0) << 2) + (offset & 0x3F) + ((y & 16) << 7) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6);
}

// The Xenos 3D tiling (tiles of 32 x 32 x 4 texels): texel (x, y, z)'s byte offset from the texture's start, with
// pitch and height in texels (rounded up to 32 here) and log2_bytes the texel's size (2: 4 bytes). The same steps as
// the SDK's texture_util::GetTiledOffset3D (rexglue-sdk src/graphics/pipeline/texture/util.cpp, from Xenia, which
// reconstructed it from XGRAPHICS::TileVolume); written out here to be read beside the 2D one.
int32_t TiledOffset3D(int32_t x, int32_t y, int32_t z, uint32_t pitch, uint32_t height, uint32_t log2_bytes) {
  pitch = (pitch + 31u) & ~31u;
  height = (height + 31u) & ~31u;
  const int32_t macro_outer =
      ((y >> 4) + (z >> 2) * static_cast<int32_t>(height >> 4)) * static_cast<int32_t>(pitch >> 5);
  const int32_t macro = ((((x >> 5) + macro_outer) << (log2_bytes + 6)) & 0xFFFFFFF) << 1;
  const int32_t micro = (((x & 7) + ((y & 6) << 2)) << (log2_bytes + 6)) >> 6;
  const int32_t offset_outer = ((y >> 3) + (z >> 2)) & 1;
  const int32_t offset1 = offset_outer + ((((x >> 3) + (offset_outer << 1)) & 3) << 1);
  const int32_t offset2 =
      ((macro + (micro & ~15)) << 1) + (micro & 15) + ((z & 3) << (log2_bytes + 6)) + ((y & 1) << 4);
  int32_t address = (offset1 & 1) << 3;
  address += (offset2 >> 6) & 7;
  address <<= 3;
  address += offset1 & ~1;
  address <<= 2;
  address += offset2 & ~511;
  address <<= 3;
  address += offset2 & 63;
  return address;
}

// The fetch constant's endianness: 1 swaps the bytes in each 16 bits, 2 in each 32, 3 the 16-bit halves.
void SwapBlock(uint8_t* b, uint32_t bytes, uint32_t endian) {
  for (uint32_t i = 0; i + 3 < bytes; i += 4) {
    uint8_t* w = b + i;
    switch (endian) {
      case 1:
        std::swap(w[0], w[1]);
        std::swap(w[2], w[3]);
        break;
      case 2:
        std::swap(w[0], w[3]);
        std::swap(w[1], w[2]);
        break;
      case 3:
        std::swap(w[0], w[2]);
        std::swap(w[1], w[3]);
        break;
      default:
        break;
    }
  }
}

void Color565(uint16_t c, uint8_t out[4]) {
  out[0] = static_cast<uint8_t>(((c >> 11) & 31) * 255 / 31);
  out[1] = static_cast<uint8_t>(((c >> 5) & 63) * 255 / 63);
  out[2] = static_cast<uint8_t>((c & 31) * 255 / 31);
  out[3] = 255;
}

// A BC1 color block (little-endian, after the swap) into 16 rgba texels; four colors always when `four` (BC2, BC3).
void DecodeColors(const uint8_t* b, bool four, uint8_t texels[16][4]) {
  const uint16_t c0 = static_cast<uint16_t>(b[0] | b[1] << 8), c1 = static_cast<uint16_t>(b[2] | b[3] << 8);
  uint8_t colors[4][4];
  Color565(c0, colors[0]);
  Color565(c1, colors[1]);
  if (four || c0 > c1) {
    for (int k = 0; k < 4; ++k) {
      colors[2][k] = static_cast<uint8_t>((2 * colors[0][k] + colors[1][k]) / 3);
      colors[3][k] = static_cast<uint8_t>((colors[0][k] + 2 * colors[1][k]) / 3);
    }
  } else {
    for (int k = 0; k < 4; ++k) colors[2][k] = static_cast<uint8_t>((colors[0][k] + colors[1][k]) / 2);
    colors[3][0] = colors[3][1] = colors[3][2] = colors[3][3] = 0;
  }
  const uint32_t bits = static_cast<uint32_t>(b[4] | b[5] << 8 | b[6] << 16) | static_cast<uint32_t>(b[7]) << 24;
  for (int t = 0; t < 16; ++t) std::memcpy(texels[t], colors[(bits >> (2 * t)) & 3], 4);
}

// A BC4 block (two ends, then 3-bit picks) into one channel of 16 texels.
void DecodeChannelBlock(const uint8_t* b, uint8_t texels[16][4], int channel) {
  uint32_t a[8] = {b[0], b[1]};
  if (a[0] > a[1]) {
    for (int k = 1; k < 7; ++k) a[k + 1] = ((7 - k) * a[0] + k * a[1]) / 7;
  } else {
    for (int k = 1; k < 5; ++k) a[k + 1] = ((5 - k) * a[0] + k * a[1]) / 5;
    a[6] = 0;
    a[7] = 255;
  }
  uint64_t bits = 0;
  for (int k = 0; k < 6; ++k) bits |= static_cast<uint64_t>(b[2 + k]) << (8 * k);
  for (int t = 0; t < 16; ++t) texels[t][channel] = static_cast<uint8_t>(a[(bits >> (3 * t)) & 7]);
}

void DecodeBlock(const uint8_t* b, uint32_t format, uint8_t texels[16][4]) {
  if (format == 58 || format == 59 || format == 49) {  // one or two channels; the others 0, 0, 1 as sampled
    for (int t = 0; t < 16; ++t) {
      texels[t][0] = texels[t][1] = texels[t][2] = 0;
      texels[t][3] = 255;
    }
    if (format == 58) {
      for (int t = 0; t < 16; ++t) texels[t][0] = static_cast<uint8_t>(((b[t / 2] >> (4 * (t & 1))) & 15) * 17);
    } else {
      DecodeChannelBlock(b, texels, 0);
      if (format == 49) DecodeChannelBlock(b + 8, texels, 1);
    }
    return;
  }
  if (format == 51) format = 18;  // the AS_16_16_16_16 kinds hold the same blocks
  if (format == 52) format = 19;
  if (format == 53) format = 20;
  if (format == 18) {
    DecodeColors(b, false, texels);
  } else if (format == 19) {
    DecodeColors(b + 8, true, texels);
    for (int t = 0; t < 16; ++t) texels[t][3] = static_cast<uint8_t>(((b[t / 2] >> (4 * (t & 1))) & 15) * 17);
  } else {  // 20: BC3, its alpha a BC4 block
    DecodeColors(b + 8, true, texels);
    DecodeChannelBlock(b, texels, 3);
  }
}

uint32_t Log2Ceil(uint32_t v) {
  uint32_t l = 0;
  while (l < 31 && (1u << l) < v) ++l;
  return l;
}

// A packed texel's fields x, y, z, w (x in the low bits): each one's bits; a format with no w field gets 255 there.
struct PackedFields {
  uint8_t bits[4];
};

bool PackedFormat(uint32_t format, PackedFields* fields) {
  switch (format) {
    case 3: *fields = {{5, 5, 5, 1}}; return true;    // k_1_5_5_5
    case 4: *fields = {{5, 6, 5, 0}}; return true;    // k_5_6_5
    case 5: *fields = {{6, 5, 5, 0}}; return true;    // k_6_5_5
    case 15: *fields = {{4, 4, 4, 4}}; return true;   // k_4_4_4_4
    case 7:
    case 54: *fields = {{10, 10, 10, 2}}; return true;  // k_2_10_10_10
    case 16:
    case 55: *fields = {{10, 11, 11, 0}}; return true;  // k_10_11_11
    case 17:
    case 56: *fields = {{11, 11, 10, 0}}; return true;  // k_11_11_10
    default: return false;
  }
}

void UnpackTexel(uint32_t word, const PackedFields& fields, uint8_t texel[4]) {
  uint32_t shift = 0;
  for (int k = 0; k < 4; ++k) {
    const uint32_t n = fields.bits[k];
    if (n == 0) {
      texel[k] = 255;
      continue;
    }
    const uint32_t value = (word >> shift) & ((1u << n) - 1);
    texel[k] = static_cast<uint8_t>((value * 255 + ((1u << n) - 1) / 2) / ((1u << n) - 1));
    shift += n;
  }
}

// One surface's texels as BGRA8 rows: width x height texels in blocks of format f from `source` (`bytes` long), tiled
// or not with pitch_blocks blocks a row, the first block at (offset_x, offset_y) in blocks (a level inside a packed
// surface); each texel's channels picked by the fetch constant's swizzle (0-3 a channel, 4 zero, 5 one).
void DecodeSurface(const uint8_t* source, size_t bytes, const Format& f, uint32_t format, uint32_t endian,
                   uint32_t swizzle, bool tiled, uint32_t pitch_blocks, uint32_t offset_x, uint32_t offset_y,
                   uint32_t width, uint32_t height, std::vector<uint8_t>& bgra) {
  const uint32_t blocks_x = (width + f.block - 1) / f.block, blocks_y = (height + f.block - 1) / f.block;
  bgra.assign(static_cast<size_t>(width) * height * 4, 0);
  uint8_t block[16];
  uint8_t texels[16][4];
  PackedFields packed{};
  const bool is_packed = PackedFormat(format, &packed);
  for (uint32_t by = 0; by < blocks_y; ++by) {
    const int32_t sy = static_cast<int32_t>(by + offset_y);
    const int32_t row = tiled ? TiledRow(sy, static_cast<int32_t>(pitch_blocks), f.log2_bytes) : 0;
    for (uint32_t bx = 0; bx < blocks_x; ++bx) {
      const int32_t sx = static_cast<int32_t>(bx + offset_x);
      const uint32_t index = tiled ? static_cast<uint32_t>(TiledOffset(sx, sy, f.log2_bytes, row)) >> f.log2_bytes
                                   : static_cast<uint32_t>(sy) * pitch_blocks + static_cast<uint32_t>(sx);
      if (static_cast<size_t>(index) * f.bytes + f.bytes > bytes) continue;
      if (f.bytes < 4) {  // one or two bytes a texel: each byte found through the 32-bit word swap
        const size_t at = static_cast<size_t>(index) * f.bytes;
        const size_t flip = endian == 1 ? 1u : endian == 2 ? 3u : endian == 3 ? 2u : 0u;
        uint8_t v[2] = {0, 0};
        for (uint32_t k = 0; k < f.bytes; ++k) {
          const size_t from = (at + k) ^ flip;
          v[k] = from < bytes ? source[from] : uint8_t{0};
        }
        if (is_packed) {
          UnpackTexel(uint32_t{v[0]} | uint32_t{v[1]} << 8, packed, texels[0]);
        } else {
          texels[0][0] = v[0];
          texels[0][1] = f.bytes > 1 ? v[1] : 0;
          texels[0][2] = 0;
          texels[0][3] = 255;
        }
      } else if (f.block == 1) {
        std::memcpy(block, source + static_cast<size_t>(index) * f.bytes, f.bytes);
        SwapBlock(block, f.bytes, endian);
        if (is_packed) {
          UnpackTexel(uint32_t{block[0]} | uint32_t{block[1]} << 8 | uint32_t{block[2]} << 16 |
                          uint32_t{block[3]} << 24,
                      packed, texels[0]);
        } else {
          std::memcpy(texels[0], block, 4);
        }
      } else {
        std::memcpy(block, source + static_cast<size_t>(index) * f.bytes, f.bytes);
        SwapBlock(block, f.bytes, endian);
        DecodeBlock(block, format, texels);
      }
      for (uint32_t ty = 0; ty < f.block; ++ty) {
        for (uint32_t tx = 0; tx < f.block; ++tx) {
          const uint32_t x = bx * f.block + tx, y = by * f.block + ty;
          if (x >= width || y >= height) continue;
          const uint8_t* t = texels[ty * f.block + tx];
          uint8_t c[4];
          for (int k = 0; k < 4; ++k) {
            const uint32_t s = (swizzle >> (3 * k)) & 7;
            c[k] = s < 4 ? t[s] : s == 5 ? 255 : 0;
          }
          uint8_t* out = &bgra[(static_cast<size_t>(y) * width + x) * 4];
          out[0] = c[2];
          out[1] = c[1];
          out[2] = c[0];
          out[3] = c[3];
        }
      }
    }
  }
}

// A texture's top level from its fetch constant: dword 0 tiled (bit 31) and pitch (bits 22-30, x 32 texels); dword 1
// format (bits 0-5), endianness (bits 6-7) and address (& 0x1FFFF000); dword 2 width - 1 and height - 1 (13 bits
// each); dword 3 swizzle (bits 1-12); dword 5 packed mips (bit 11). False (and why) when it can't be read.
bool DecodeFetch(const uint8_t* base, const std::array<uint32_t, 6>& fetch, std::vector<uint8_t>& bgra,
                 uint32_t& width, uint32_t& height, std::string& why, bool as_linear = false) {
  if ((fetch[0] & 3) != 2) {
    why = "not a texture fetch constant";
    return false;
  }
  const bool tiled = (fetch[0] >> 31) != 0 && !as_linear;  // as_linear: a test, read as if not tiled
  const uint32_t pitch = ((fetch[0] >> 22) & 0x1FF) * 32;
  const uint32_t format = fetch[1] & 0x3F, endian = (fetch[1] >> 6) & 3;
  const uint32_t physical = fetch[1] & 0x1FFFF000;
  width = (fetch[2] & 0x1FFF) + 1;
  height = ((fetch[2] >> 13) & 0x1FFF) + 1;
  const uint32_t swizzle = (fetch[3] >> 1) & 0xFFF;
  const bool packed = ((fetch[5] >> 11) & 1) != 0;
  Format f;
  if (!FormatOf(format, &f)) {
    why = "format " + std::to_string(format);
    return false;
  }
  const uint32_t blocks_x = (width + f.block - 1) / f.block, blocks_y = (height + f.block - 1) / f.block;
  // a small texture's top level in its packed mip tile (Xenia's GetPackedMipOffset, level 0)
  uint32_t offset_x = 0, offset_y = 0;
  if (packed && std::min(Log2Ceil(width), Log2Ceil(height)) <= 4) {
    if (Log2Ceil(width) > Log2Ceil(height)) {
      offset_y = 16 / f.block;
    } else {
      offset_x = 16 / f.block;
    }
  }
  uint32_t pitch_blocks = std::max(pitch / f.block, blocks_x + offset_x);
  if (tiled) pitch_blocks = (pitch_blocks + 31) & ~31u;
  const uint32_t rows = tiled ? (blocks_y + offset_y + 31) & ~31u : blocks_y + offset_y;
  const size_t bytes = static_cast<size_t>(pitch_blocks) * rows * f.bytes;
  if (bytes > kMaxTextureBytes || physical == 0) {
    why = "size or address out of range";
    return false;
  }
  DecodeSurface(GpuPointer(base, physical), std::min(bytes, PhysicalRoom(physical)), f, format, endian, swizzle,
                tiled, pitch_blocks, offset_x, offset_y, width, height, bgra);
  return true;
}

// The game's own smaller levels of a texture (1 to its last, the fetch constant's dword 4 bits 6-9), BGRA8
// rows each, from where the GPU keeps them; with only the top level, far alpha-tested surfaces (the pines) kept every
// texel of the top level and came out fuller than the game's. The layout is Call of Duty 3's DecodeLevelsFetch
// (ngl_side.cpp, rexglue-native and acct2-decomp; checked there on 7,169 textures), the same Xenos rules: after the top
// level each level is a tiled surface of its own, pitch and rows rounded up to 32 blocks, until the first level whose
// shorter side is 16 texels or less; in a packed texture (dword 5 bit 11) that level and every smaller one share one
// surface, each at Xenia's GetPackedMipOffset place. Levels 1 on start at the mip address (dword 5, & 0x1FFFF000)
// when there is one, else right after the top level. Only tiled 2D textures whose sides are powers of two; for others
// `levels` stays empty (the top level alone, as before).
void DecodeLevelsFetch(const uint8_t* base, const std::array<uint32_t, 6>& fetch,
                       std::vector<std::vector<uint8_t>>& levels) {
  levels.clear();
  const uint32_t format = fetch[1] & 0x3F, endian = (fetch[1] >> 6) & 3;
  const uint32_t physical = fetch[1] & 0x1FFFF000;
  const uint32_t width = (fetch[2] & 0x1FFF) + 1, height = ((fetch[2] >> 13) & 0x1FFF) + 1;
  const uint32_t swizzle = (fetch[3] >> 1) & 0xFFF;
  const uint32_t last = (fetch[4] >> 6) & 0xF;
  const uint32_t mip_address = fetch[5] & 0x1FFFF000;
  const bool tiled = (fetch[0] >> 31) != 0, packed = ((fetch[5] >> 11) & 1) != 0;
  Format f;
  if ((fetch[0] & 3) != 2 || !tiled || last == 0 || physical == 0 || !FormatOf(format, &f) ||
      (width & (width - 1)) != 0 || (height & (height - 1)) != 0)
    return;
  const uint32_t log2_w = Log2Ceil(width), log2_h = Log2Ceil(height);
  const uint32_t shorter = std::min(log2_w, log2_h);
  const uint32_t first_packed = shorter > 4 ? shorter - 4 : 0;  // the first level in the shared surface
  auto round32 = [](uint32_t v) { return (v + 31) & ~31u; };
  size_t next = 0, shared_at = 0, top_bytes = 0;  // top_bytes: the top level's own surface (0 when it is shared)
  uint32_t shared_pitch = 0, shared_rows = 0;
  bool shared = false;
  for (uint32_t level = 0; level <= last; ++level) {
    const uint32_t w = std::max(1u, width >> level), h = std::max(1u, height >> level);
    const uint32_t blocks_x = (w + f.block - 1) / f.block, blocks_y = (h + f.block - 1) / f.block;
    size_t at;
    uint32_t pitch, rows, x = 0, y = 0;  // x, y: the level's place in a shared surface, in texels
    if (packed && level >= first_packed) {
      if (!shared) {
        shared = true;
        shared_at = next;
        shared_pitch = round32(blocks_x);
        shared_rows = round32(blocks_y);
        next += static_cast<size_t>(shared_pitch) * shared_rows * f.bytes;
      }
      const uint32_t step = level - first_packed;
      const bool wider = log2_w > log2_h;
      if (step < 3) {
        (wider ? y : x) = 16u >> step;
      } else {
        (wider ? x : y) = (1u << ((wider ? log2_w : log2_h) - first_packed)) >> (step - 2);
      }
      at = shared_at;
      pitch = shared_pitch;
      rows = shared_rows;
    } else {
      at = next;
      pitch = round32(blocks_x);
      rows = round32(blocks_y);
      next += static_cast<size_t>(pitch) * rows * f.bytes;
      if (level == 0) top_bytes = next;
    }
    if (level == 0) continue;  // the top level is DecodeFetch's
    const size_t bytes = static_cast<size_t>(pitch) * rows * f.bytes;
    // a surface shared with the top level is at the base; every other is counted from where levels 1 on start
    const bool with_top = shared && first_packed == 0;
    const uint32_t start =
        with_top ? physical : mip_address ? mip_address : physical + static_cast<uint32_t>(top_bytes);
    const size_t from = with_top ? at : at - top_bytes;
    if (from + bytes > kMaxTextureBytes || PhysicalRoom(static_cast<size_t>(start) + from) == 0) {
      levels.clear();
      return;
    }
    levels.emplace_back();
    DecodeSurface(GpuPointer(base, start) + from, std::min(bytes, PhysicalRoom(static_cast<size_t>(start) + from)), f,
                  format, endian, swizzle, true, pitch, x / f.block, y / f.block, w, h, levels.back());
  }
}

// ---------------------------------------------------------------------------------------------------------------
// ngl_vk

int g_state = 0;  // 0 not started, 1 ready, -1 failed (no more tries)

struct CachedTexture {
  std::array<uint32_t, 6> fetch{};
  uint32_t id = 0;  // ngl_vk's, 0 when it couldn't be decoded
};
std::unordered_map<uint32_t, CachedTexture> g_textures;  // by the Direct3D texture's address
std::map<std::string, int> g_failed;                     // why textures weren't decoded, and how many
constexpr size_t kMaxTextures = 1500;                    // ngl_vk has room for 2048
std::unordered_map<uint64_t, uint32_t> g_meshes;         // the meshes made so far, by MeshDraw::key
// by the primary's and the secondary's textures and the light's mode (LightmapFor)
std::map<std::tuple<uint32_t, uint32_t, int>, uint32_t> g_lightmaps;
bool g_forget = false;  // ngl_vk ran out of room: forget everything before the next frame
bool g_logged_lightmap = false;
// the world corners' colors in the meshes made so far (the lm_r0c0 shaders multiply the color map by them; ngl_vk's
// lightmap shading does not: how much that matters)
double g_corner_colors[4] = {};
uint64_t g_corner_count = 0, g_corners_not_white = 0;

// [waw-decomp-cod4] 2026-11-11 round 6: with every frame drawn (cod4_native_vulkan), Call of Duty 2's answer to
// forgetting too often (its 4z62, run nat5: forgets 15 -> 3 in 100 s, the 3D 35.9 -> 25.3 ms on average): everything is
// forgotten at three quarters of ngl_vk's room for mesh corners (DrawFrame), and past half of it a draw whose geometry
// has no mesh yet gets none: its corners are written out in order as this frame's corners (ngl_vk_frame_corners) and
// drawn with one shared mesh whose indices are 0, 1, 2, ... (InOrderMesh). Set by DrawFrame each frame; off for the
// check alone (cod4_debug_native_side: forgotten at half, as before).
bool g_mesh_room_tight = false;
uint32_t g_in_order = 0;  // the shared mesh, 0 until made
constexpr uint32_t kInOrder = 65535;

// [waw-decomp-cod4] 2026-11-11 round 6: what the native picture cost, every 600 frames drawn (cod4_native_vulkan)
struct NativeCost {
  uint64_t frames = 0, forgets = 0, made = 0, frame_corner_draws = 0, draws = 0;
  double setup_ms = 0.0, draw_ms = 0.0;
};
NativeCost g_native_cost;

double MillisecondsSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void ForgetAll() {
  ngl_vk_forget();
  g_textures.clear();
  g_meshes.clear();
  g_lightmaps.clear();
  g_forget = false;
  g_in_order = 0;  // [waw-decomp-cod4] round 6: forgotten with the rest
}

// [waw-decomp-cod4] 2026-11-11 round 6: the shared mesh for corners written out in order (Call of Duty 2's InOrderMesh)
uint32_t InOrderMesh() {
  if (g_in_order != 0) return g_in_order;
  std::vector<uint16_t> indices(kInOrder);
  for (uint32_t i = 0; i < kInOrder; ++i) indices[i] = static_cast<uint16_t>(i);
  const float corner[NGL_VK_CORNER_FLOATS] = {};
  g_in_order = ngl_vk_mesh(corner, 1, indices.data(), kInOrder);
  if (g_in_order == 0) g_forget = true;
  return g_in_order;
}

// A draw's texture as an ngl_vk texture, with the game's own smaller levels when they can be read; 0 for none.
uint32_t TextureFor(const uint8_t* base, const TextureRef& ref) {
  if (ref.texture == 0) return 0;
  auto found = g_textures.find(ref.texture);
  if (found != g_textures.end() && found->second.fetch == ref.fetch) return found->second.id;
  if (g_textures.size() >= kMaxTextures) g_forget = true;  // forgotten before the next frame
  CachedTexture& cached = g_textures[ref.texture];
  cached.fetch = ref.fetch;
  cached.id = 0;
  std::vector<uint8_t> bgra;
  uint32_t width = 0, height = 0;
  std::string why;
  if (DecodeFetch(base, ref.fetch, bgra, width, height, why)) {
    std::vector<std::vector<uint8_t>> levels;
    DecodeLevelsFetch(base, ref.fetch, levels);
    std::vector<const void*> pictures{bgra.data()};
    for (const auto& level : levels) pictures.push_back(level.data());
    cached.id = ngl_vk_texture_levels(pictures.data(), static_cast<uint32_t>(pictures.size()), width, height);
    if (cached.id == 0) why = "ngl_vk has no room";
  }
  if (cached.id == 0 && g_failed[why]++ == 0)
    REXLOG_INFO("cod4: native side: texture {:08X} not drawn ({}); its color alone is (further ones not logged)",
                ref.texture, why);
  return cached.id;
}

// The world's lightmap as ngl_vk takes it (its lightmap_scale shading), from the texture the device has bound beside
// the material's (KeepWorldDraw: sampler 3 in Killhouse, k_8_8_8_8, 512 x 1024 or 512 x 2048). [waw-decomp-cod4]
// 08:20: read as the lm_r0c0 pixel shaders read it (shader_CF291A36D8A094EF.ucode.frag, run cod4side9's dump; its
// literals c255 = 0.5, 1, 2, -1 by how they're used): two lightmaps stacked, the top half P fetched at (u, v / 2)
// and the bottom half S at (u, v / 2 + 1/2) from the corners' lightmap coordinates, and the light is
//     P.rgb + S.rgb x rsq_sat(1 + a^2 + b^2),  a = 2 P.a - 1, b = 2 S.a - 1
// (instructions 4-11: the two alphas are a direction's x and y with z 1, and rsq gives the flat surface's facing to
// it; no factor of its own), then the color map x the corners' color x that light, fogged (12-13). Made here as one
// texture of the half's size holding the light halved (up to 2 does not fit a byte); lightmap_scale 2 restores it
// (kLightScale). Run cod4side10 (08:16) drew that and its light did not fall where the game's does (cod4_lightcheck.py:
// correlation -0.1 with the game's picture over the unlit one); cod4side11's layouts (halves swapped, u and v swapped,
// v turned over, not tiled) did no better, and the lm vertex shaders hand the corners' +0x14..+0x20 on unchanged
// (866741BC87D80B5A: vfetch Offset=5 FMT_32_32_32_32_FLOAT, o0 = it), so the coordinates are the game's. 08:55: the
// variants (cod4_debug_native_side_variants) now test the page and the output's gamma, mode: 0 the shaders' reading;
// 1 the same from the page the draw's lightmap index is most often read with this frame (SetMeshes: a wrong page
// would show); 2 the light gamma-encoded (light^(1/2.2): the picture as a gamma render target would hold it, the
// color maps' fetch signs being 0); 4 P alone; 5 S alone. Alpha 0: no sun share (the shaders have none). 0 when the
// texture can't be read. The first lightmaps read are also saved as decoded (ngl-side-lightmap-<texture>.ppm, blue
// green red) for the tiling check on the PC.
uint32_t LightmapFor(const uint8_t* base, const TextureRef& ref, int mode) {
  if (ref.texture == 0) return 0;
  if (mode == 1) mode = 0;  // the same reading; only the page differs (SetMeshes)
  const auto key = std::make_tuple(ref.texture, ref.fetch[1], mode);
  if (auto found = g_lightmaps.find(key); found != g_lightmaps.end()) return found->second;
  uint32_t& id = g_lightmaps[key];
  id = 0;
  std::vector<uint8_t> texels;
  uint32_t width = 0, height = 0;
  std::string why;
  if (!DecodeFetch(base, ref.fetch, texels, width, height, why)) {
    if (!g_logged_lightmap) {
      g_logged_lightmap = true;
      REXLOG_INFO("cod4: native side: a world lightmap not made ({})", why);
    }
    return 0;
  }
  static std::vector<uint32_t> saved;
  // [waw-decomp-cod4] 2026-11-11 round 6: only when the check saves pictures (its folder is set then)
  if (mode == 0 && REXCVAR_GET(cod4_debug_native_side) && saved.size() < 3 &&
      std::find(saved.begin(), saved.end(), ref.texture) == saved.end()) {
    saved.push_back(ref.texture);
    char name[64];
    std::snprintf(name, sizeof(name), "ngl-side-lightmap-%08X.ppm", ref.texture);
    WritePpm(g_folder / name, texels.data(), size_t(width) * 4, width, height);
  }
  const uint32_t rows = height / 2;  // the size of each half, and of the light made
  std::vector<uint8_t> out(static_cast<size_t>(width) * rows * 4);
  double sum[4] = {};
  for (uint32_t y = 0; y < rows; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t* p = &texels[(static_cast<size_t>(y) * width + x) * 4];          // the top half
      const uint8_t* s = &texels[(static_cast<size_t>(y + rows) * width + x) * 4];   // the bottom half
      const float a = p[3] / 127.5f - 1.0f, b = s[3] / 127.5f - 1.0f;
      const float facing = std::min(1.0f, 1.0f / std::sqrt(1.0f + a * a + b * b));
      uint8_t* o = &out[(static_cast<size_t>(y) * width + x) * 4];
      for (int k = 0; k < 3; ++k) {
        float light = mode == 4 ? p[k] : mode == 5 ? s[k] : p[k] + s[k] * facing;  // 0 to 510
        if (mode == 2) light = 255.0f * std::pow(light / 255.0f, 1.0f / 2.2f);
        o[k] = static_cast<uint8_t>(std::min(255.0f, light * 0.5f + 0.5f));
        sum[k] += light;
      }
      sum[3] += facing;
      o[3] = 0;  // no sun share
    }
  }
  id = ngl_vk_texture(out.data(), width, rows);
  if (id == 0) g_forget = true;
  static uint32_t logged_modes = 0;
  if (!(logged_modes & (1u << mode))) {
    logged_modes |= 1u << mode;
    const double texels_read = static_cast<double>(width) * rows;
    REXLOG_INFO("cod4: native side: lightmap mode {}: texture {:08X} {}x{} format {} (fetch {:08X} {:08X} {:08X}), "
                "halves of {} rows; the light's averages (0-510) blue {:.1f} green {:.1f} red {:.1f}, facing "
                "{:.3f}", mode, ref.texture, width, height, ref.fetch[1] & 0x3Fu, ref.fetch[0], ref.fetch[1],
                ref.fetch[2], rows, sum[0] / texels_read, sum[1] / texels_read, sum[2] / texels_read,
                sum[3] / texels_read);
  }
  return id;
}

// A draw's geometry as an ngl_vk mesh, made once: the world's 44-byte corners (position, color +0x10, texture
// coordinate +0x14, lightmap coordinate +0x1C), no corner normals yet (ngl_vk then takes the face's).
// [waw-decomp-cod4] 2026-11-11 round 6: past half of the room (g_mesh_room_tight) a geometry without a mesh is drawn
// from this frame's corners instead: frame_corners and index_count are then set for its draw (else left 0).
uint32_t MeshFor(const uint8_t* base, const MeshDraw& m, uint32_t& built, uint32_t& frame_corners,
                 uint32_t& index_count) {
  frame_corners = index_count = 0;
  if (auto found = g_meshes.find(m.key); found != g_meshes.end()) return found->second;
  std::unordered_map<uint32_t, uint16_t> local;
  std::vector<float> corners;
  std::vector<uint16_t> indices;
  indices.reserve(m.used.size());
  for (const uint32_t vertex_index : m.used) {
    auto [it, inserted] = local.emplace(vertex_index, static_cast<uint16_t>(corners.size() / NGL_VK_CORNER_FLOATS));
    if (inserted) {
      if (corners.size() / NGL_VK_CORNER_FLOATS >= 65535) {
        g_meshes[m.key] = 0;  // too many corners: remembered as not drawable
        return 0;
      }
      const uint32_t v = m.vertices + vertex_index * m.stride;
      const uint32_t color = GpuU32(base, v + 0x10u);  // 0xAARRGGBB
      for (int k = 0; k < 4; ++k) g_corner_colors[k] += (color >> (8 * k)) & 0xFFu;  // blue, green, red, alpha
      g_corner_count += 1;
      g_corners_not_white += (color & 0xFFFFFFu) != 0xFFFFFFu;
      float color_bits;
      std::memcpy(&color_bits, &color, 4);  // as ngl_vk takes it: bytes blue, green, red, alpha
      const float corner[NGL_VK_CORNER_FLOATS] = {
          GpuF32(base, v), GpuF32(base, v + 4u), GpuF32(base, v + 8u),   // position
          GpuF32(base, v + 0x14u), GpuF32(base, v + 0x18u),              // texture coordinate
          m.model ? 0.0f : GpuF32(base, v + 0x1Cu),                      // lightmap coordinate (none on a
          m.model ? 0.0f : GpuF32(base, v + 0x20u),                      // static model's 32-byte corner)
          color_bits, 0.0f, 0.0f, 0.0f};
      corners.insert(corners.end(), corner, corner + NGL_VK_CORNER_FLOATS);
    }
    indices.push_back(it->second);
  }
  // [waw-decomp-cod4] 2026-11-11 round 6: the room past half: this frame's corners, written out in order
  if (g_mesh_room_tight && indices.size() <= kInOrder && InOrderMesh() != 0) {
    std::vector<float> written(indices.size() * NGL_VK_CORNER_FLOATS);
    for (size_t i = 0; i < indices.size(); ++i)
      std::memcpy(&written[i * NGL_VK_CORNER_FLOATS], &corners[size_t{indices[i]} * NGL_VK_CORNER_FLOATS],
                  NGL_VK_CORNER_FLOATS * sizeof(float));
    const uint32_t first = ngl_vk_frame_corners(written.data(), static_cast<uint32_t>(indices.size()));
    if (first != 0) {
      frame_corners = first;
      index_count = static_cast<uint32_t>(indices.size());
      ++g_native_cost.frame_corner_draws;
      return g_in_order;
    }
  }
  uint32_t& id = g_meshes[m.key];
  id = ngl_vk_mesh(corners.data(), static_cast<uint32_t>(corners.size() / NGL_VK_CORNER_FLOATS), indices.data(),
                   static_cast<uint32_t>(indices.size()));
  if (id == 0) g_forget = true;
  ++built;
  ++g_native_cost.made;
  return id;
}

// How a material's pixels use alpha, from its state word 0 (IW3's GFXS0, the same bits as Call of Duty 2's): source
// blend in bits 0-3 and destination blend in bits 4-7 (0, or ONE and ZERO: not blended), bit 11 set turns the alpha
// test off.
uint32_t AlphaOf(uint32_t state0) {
  const uint32_t source = state0 & 0xFu, destination = (state0 >> 4) & 0xFu;
  if (source != 0 && !(source == 2 && destination == 1)) return NGL_VK_ALPHA_BLEND;
  return (state0 & 0x800u) == 0 ? NGL_VK_ALPHA_TEST : NGL_VK_ALPHA_OPAQUE;
}

// [waw-decomp-cod4] 2026-11-11 09:55: the page of a lightmap index. The world's tess loop 0x8210C618 (its context
// in r3: +0x08 the reflection probes' array, +0x0C and +0x10 two arrays indexed by the lightmap index, +0x28 which
// of them the pass wants) binds, when a surface's byte +0x2C changes (its lightmap index; 0x1F none), the entry at
// (+0x0C) + 4 x index with 0x820D4D28(state, 2) and the one at (+0x10) + 4 x index with 0x820D4D28(state, 3);
// r_lightMap other than 1 binds the debug images 0x835A0E88 / 8C / 90 instead. So each index has one page, a word
// pointing to its Direct3D texture, which the lm shaders read at tf3. Run cod4side15 read the pages at sampler 3
// (index 0 B21D8F84, 1 B21D9020, 2 B21D90BC) but 21 draws' sampler 3 held something else (index 1's 11, index 31's
// 10), so the scan took a reflection probe for them. The tess function is called through the table 0x8202FDA0 with
// a context made elsewhere, so the arrays' place in the world (*0x835A0F10) wasn't read from the decompile: it is
// looked for once a world, as the word in the world's first 0x800 bytes pointing to an array whose entries at the
// indices seen hold the pages checked at sampler 3 (entries 4 bytes apart as the tess loop indexes them; 8 with the
// page first or second also tried, for IW3's GfxLightmapArray of a primary and a secondary image). When none is
// found the frame's most frequent checked page of the index is taken; index 0x1F has no lightmap.
constexpr uint32_t kNoLightmapIndex = 0x1Fu;

struct WorldPages {
  uint32_t world = 0;  // the world it was looked for in (0: not yet)
  uint32_t offset = 0, stride = 0, inner = 0;
  bool found = false;
};
WorldPages g_world_pages;

void FindWorldPages(const uint8_t* base, const std::map<uint32_t, uint32_t>& checked) {
  const uint32_t world = TheWorld(base);
  if (!IsGuestData(world) || checked.empty() || g_world_pages.world == world) return;
  g_world_pages = WorldPages{};
  g_world_pages.world = world;
  constexpr uint32_t kLayouts[3][2] = {{4, 0}, {8, 0}, {8, 4}};  // entry size, the page's place in an entry
  for (uint32_t offset = 0; offset < 0x800u && !g_world_pages.found; offset += 4) {
    const uint32_t array = LoadU32(base, world + offset);
    if (!IsGuestData(array)) continue;
    for (const auto& layout : kLayouts) {
      bool all = true;
      for (const auto& [index, texture] : checked) {
        const uint32_t holder = array + layout[0] * index + layout[1];
        if (!IsGuestData(holder) || LoadU32(base, holder) != texture) {
          all = false;
          break;
        }
      }
      if (all) {
        g_world_pages.offset = offset;
        g_world_pages.stride = layout[0];
        g_world_pages.inner = layout[1];
        g_world_pages.found = true;
        break;
      }
    }
  }
  std::string known;
  for (const auto& [index, texture] : checked) known += " " + std::to_string(index);
  if (g_world_pages.found) {
    REXLOG_INFO("cod4: native side: the world {:08X}'s lightmap pages: the array at its +0x{:X} ({:08X}), entries {} "
                "bytes apart, the page at +{} (matched at the indices{}); the word before it {:08X}", world,
                g_world_pages.offset, LoadU32(base, world + g_world_pages.offset), g_world_pages.stride,
                g_world_pages.inner, known, LoadU32(base, world + g_world_pages.offset - 4u));
  } else {
    REXLOG_INFO("cod4: native side: the world {:08X}'s lightmap pages: no array in its first 0x800 bytes holds the "
                "pages checked at the indices{}; the frame's own checked pages are used", world, known);
  }
}

// [waw-decomp-cod4] 2026-11-11 09:55: what the game does to its picture after the scene, read from the decompile:
//   - The display's gamma ramp. 0x821238C8 (r_gamma or r_ignorehwgamma changed, the latter 0, the byte 0x835A1028
//     set) builds a ramp with 0x82135430: entry i = (i / 255)^(1 / r_gamma) x 65535 + 0.5 (i x 0x101 when r_gamma is
//     1), copies it to red, green and blue (0x82123110) and hands it to Direct3D's SetGammaRamp 0x82097F48 with the
//     device at 0x835A0F88, which keeps the 0x600 bytes at device + 0x3A8C. The game's own picture is taken after the
//     ramp (Call of Duty 2's ShownColors, 4z36: the same reading at its device + 0x36DC), ngl_vk's before it.
//   - The film pass (r_filmTweak*, or the vision set's film when r_filmUseTweaks is 0): 0x8213A6B0 copies the film
//     settings (enabled, brightness, contrast, desaturation, invert, dark and light tint) and 0x8213A310 makes three
//     vectors of them at its +0x6700 / +0x6710 / +0x6720 (k contrast, b brightness, d = max(desaturation, 1/4096)):
//     bias = (b + 0.5 - 0.5 k) x 3, 1/d - 1; base = dark tint x k d; delta = (light tint - dark tint) x k d (with
//     invert: k d turned negative and bias + 1). With the film off they are (0, 0, 0, 4095), (1/4096) x 3 and 0,
//     for which out = bias + (base + delta x lum) x (color x bias.w + lum) is the color again: that is the form
//     taken here, lum = dot(color, (0.299, 0.587, 0.114)) (the shader's weights not read). 10:15: they are read
//     where the back end has them: 0x82331150 walks the frame's views, *(*0x835A13C4 + 0xB602C) + n x the view's
//     size, *(*0x835A13C4 + 0xB6028) of them, and for a view whose +0x13A4 is 0 draws the film pass (0x82127158 with
//     the material at 0x835A0F38) when 0x82110B28 says the film is on and not the defaults (contrast 1, brightness
//     0, desaturation 0, no invert, tints 1). The first view's film is read at the frame's end (FilmFrom; run
//     cod4side16 found the source state's code constants 45 to 47, KisakCOD's COLOR_BIAS / TINT_BASE / TINT_DELTA,
//     holding other values, so the 360's order differs there).
#if defined(COD4_MP)
// [waw-decomp-cod4] 2026-11-11 12:00: cod4mp: the device is the one its draws go to (SideDraw's), and r_gamma's and
// r_ignorehwgamma's records are taken only when their names say so (the campaign's 0x83781D18 / 0x83781D40 are
// 0x85009B54 / 0x85009B7C there, 0x1887E3C on; these two by the same step, not read from its own code)
constexpr uint32_t kDevicePointer = 0;
constexpr uint32_t kGammaDvar = 0x85009ABCu, kIgnoreHardwareGammaDvar = 0x85009B88u;
#else
constexpr uint32_t kDevicePointer = 0x835A0F88u;  // the device 0x82123110 hands to SetGammaRamp
constexpr uint32_t kGammaDvar = 0x83781C80u, kIgnoreHardwareGammaDvar = 0x83781D4Cu;  // dvar records; value +0xC
#endif
constexpr uint32_t kDeviceGammaRamp = 0x3A8Cu;    // where SetGammaRamp 0x82097F48 keeps the ramp

// The dvar record kept at `pointer` (0 when none, or in cod4mp when its name isn't `name`).
uint32_t DvarAt(const uint8_t* base, uint32_t pointer, const char* name) {
  const uint32_t dvar = LoadU32(base, pointer);
  if (!IsGuestData(dvar)) return 0;
#if defined(COD4_MP)
  if (Text(base, LoadU32(base, dvar)) != name) return 0;
#else
  (void)name;
#endif
  return dvar;
}

// The ramp as 8-bit values for each 8-bit value of each channel, and where it came from: 2 the device's, 1 made from
// r_gamma as 0x82135430 makes it (the device's not readable), 0 none (a plain ramp: r_ignorehwgamma set or nothing
// readable), 3 the device's in its PWL form.
// [waw-decomp-cod4] 2026-11-11 12:40: the 0x600 bytes at device + 0x3A8C are the ramp's hardware form, in one of two
// layouts of 3 x 256 big-endian 16-bit words (channel c at 0x200 x c), each a 10-bit value shifted left by 6:
// SetGammaRamp 0x82097F48 stores 0x8209F5C0's form, entry i = curve[ramp[i] >> 6] << 6 for the 256 entries; the PWL
// setter 0x82098010 (called only by the device's set-up 0x820A6D98, when the display format it reads is 7 or 0x36)
// stores 0x8209F658's, 128 pairs (curve[base >> 6] << 6, (curve[end] - curve[start]) << 6) per channel, a segment's
// start and its rise. Both go through a 1024-entry curve that 0x8209F308 hands them (its argument 0 from
// SetGammaRamp; the PWL setter's not read), so even a linear ramp (r_gamma 1) is stored as that curve: red 8 / 40 /
// 104 / 152 / 216 -> 3 / 24 / 90 / 141 / 212 in Killhouse (runs cod4side16 to 22), read right as the 256-entry form.
// The PWL form failed the 256-entry reading's rising test (a rise after each start) and fell back to r_gamma's ramp:
// it is read as such now.
// [waw-decomp-cod4] 12:00: `drawn_device` the device the frame's draws went to (cod4mp's ramp is read from it).
int ShownColors(const uint8_t* base, uint32_t drawn_device, uint8_t table[3][256], float& gamma) {
  for (int channel = 0; channel < 3; ++channel)
    for (int i = 0; i < 256; ++i) table[channel][i] = static_cast<uint8_t>(i);
  gamma = 0.0f;
  const uint32_t gamma_dvar = DvarAt(base, kGammaDvar, "r_gamma");
  const uint32_t ignore_dvar = DvarAt(base, kIgnoreHardwareGammaDvar, "r_ignorehwgamma");
  if (IsGuestData(gamma_dvar)) gamma = LoadF32(base, gamma_dvar + 0xCu);
  const bool ignored = IsGuestData(ignore_dvar) && LoadU8(base, ignore_dvar + 0xCu) != 0;
  const uint32_t device = kDevicePointer != 0 ? LoadU32(base, kDevicePointer) : drawn_device;
  if (device >= 0xA0000000u && device < 0xFFF00000u) {
    const uint32_t physical = (device & 0x1FFFFFFFu) + kDeviceGammaRamp;
    if (PhysicalRoom(physical) >= 3 * 512) {
      const uint8_t* p = GpuPointer(base, physical);
      uint8_t read[3][256];
      bool plausible = true;
      for (int channel = 0; channel < 3 && plausible; ++channel) {
        uint32_t before = 0;
        for (int i = 0; i < 256; ++i) {
          const uint32_t value = static_cast<uint32_t>(p[512 * channel + 2 * i]) << 8 | p[512 * channel + 2 * i + 1];
          if (value < before) {
            plausible = false;
            break;
          }
          before = value;
          read[channel][i] = static_cast<uint8_t>(std::lround((value >> 6) * 255.0 / 1023.0));
        }
        if (before < 0x8000u) plausible = false;
      }
      if (plausible) {
        std::memcpy(table, read, sizeof read);
        return 2;
      }
      // the PWL form: a 10-bit input's segment is its top 7 bits, the low 3 its place in the segment (eighths)
      plausible = true;
      for (int channel = 0; channel < 3 && plausible; ++channel) {
        uint32_t before = 0;
        for (int i = 0; i < 256; ++i) {
          const uint32_t input = static_cast<uint32_t>(std::lround(i * 1023.0 / 255.0));
          const uint8_t* pair = p + 512 * channel + 4 * (input >> 3);
          const uint32_t start = (static_cast<uint32_t>(pair[0]) << 8 | pair[1]) >> 6;
          const uint32_t rise = (static_cast<uint32_t>(pair[2]) << 8 | pair[3]) >> 6;
          const uint32_t value = start + rise * (input & 7u) / 8u;
          if (value < before || value > 1023u) {
            plausible = false;
            break;
          }
          before = value;
          read[channel][i] = static_cast<uint8_t>(std::lround(value * 255.0 / 1023.0));
        }
        if (before < 0x200u) plausible = false;
      }
      if (plausible) {
        std::memcpy(table, read, sizeof read);
        return 3;
      }
    }
  }
  if (ignored || !(gamma > 0.1f && gamma < 10.0f)) return 0;
  for (int i = 0; i < 256; ++i) {
    const double value = std::pow(i / 255.0, 1.0 / gamma) * 65535.0 + 0.5;
    const uint8_t shown = static_cast<uint8_t>(std::lround((static_cast<uint32_t>(value) >> 6) * 255.0 / 1023.0));
    for (int channel = 0; channel < 3; ++channel) table[channel][i] = shown;
  }
  return 1;
}

// The film pass's three vectors from the frame's first view (see above), and its settings as text for the log: true
// when the game draws the pass (0x82110B28's test) and the vectors look like 0x8213A310's.
struct Film {
  float bias[4], base[4], delta[4];
};

bool FilmFrom(const uint8_t* base, uint32_t view, Film& film, std::string& settings) {
  settings = "no view";
  if (!IsGuestData(view) || !IsGuestData(view + kViewFilmVectors + 0x30u)) return false;
  const uint32_t at = view + kViewFilm;
  const bool enabled = LoadU8(base, at) != 0, invert = LoadU8(base, at + 0x10u) != 0;
  float f[9];  // brightness, contrast, desaturation, dark tint, light tint
  const uint32_t offsets[9] = {0x04, 0x08, 0x0C, 0x14, 0x18, 0x1C, 0x20, 0x24, 0x28};
  for (int k = 0; k < 9; ++k) f[k] = LoadF32(base, at + offsets[k]);
  char text[320];
  std::snprintf(text, sizeof text, "view %08X: on %d, brightness %.3f, contrast %.3f, desaturation %.3f, invert %d, "
                "dark tint %.3f %.3f %.3f, light tint %.3f %.3f %.3f, no-film word %u", view, enabled ? 1 : 0, f[0],
                f[1], f[2], invert ? 1 : 0, f[3], f[4], f[5], f[6], f[7], f[8], LoadU32(base, view + kViewNoFilm));
  settings = text;
  bool defaults = f[0] == 0.0f && f[1] == 1.0f && f[2] == 0.0f && !invert;
  for (int k = 3; k < 9; ++k) defaults = defaults && f[k] == 1.0f;
  // 10:45: Killhouse's view has +0x13A4 = 1 (run cod4side20), for which 0x82331150 skips its own film pass, yet the
  // game's picture is warmer than the native one the way this film's tints make it (game red green blue 63 57 44,
  // native 61 61 59 at frame 4800): 0x82110CB8 takes another path for such a view (0x82110BD0, not read), so the
  // film is applied whatever +0x13A4 holds
  if (!enabled || defaults) return false;
  float* parts[3] = {film.bias, film.base, film.delta};
  for (uint32_t v = 0; v < 3; ++v)
    for (uint32_t k = 0; k < 4; ++k) parts[v][k] = LoadF32(base, view + kViewFilmVectors + 16u * v + 4u * k);
  bool plausible = std::isfinite(film.bias[3]) && film.bias[3] >= 0.0f && film.bias[3] <= 4096.0f;
  for (int k = 0; k < 3 && plausible; ++k)
    plausible = std::fabs(film.bias[k]) < 4.0f && std::fabs(film.base[k]) < 8.0f && std::fabs(film.delta[k]) < 8.0f;
  char vectors[160];
  std::snprintf(vectors, sizeof vectors, "; bias %.4f %.4f %.4f %.2f, base %.4f %.4f %.4f, delta %.4f %.4f %.4f",
                film.bias[0], film.bias[1], film.bias[2], film.bias[3], film.base[0], film.base[1], film.base[2],
                film.delta[0], film.delta[1], film.delta[2]);
  settings += vectors;
  return plausible;
}

// The film pass on one 8-bit picture, red first, 4 bytes a pixel.
void ApplyFilm(const Film& film, std::vector<uint8_t>& rgba) {
  for (size_t i = 0; i + 3 < rgba.size(); i += 4) {
    float c[3];
    for (int k = 0; k < 3; ++k) c[k] = rgba[i + k] / 255.0f;
    const float lum = 0.299f * c[0] + 0.587f * c[1] + 0.114f * c[2];
    for (int k = 0; k < 3; ++k) {
      const float out = film.bias[k] + (film.base[k] + film.delta[k] * lum) * (c[k] * film.bias[3] + lum);
      rgba[i + k] = static_cast<uint8_t>(std::clamp(out * 255.0f + 0.5f, 0.0f, 255.0f));
    }
  }
}

// [waw-decomp-cod4] 2026-11-11 round 5: a static model's light, as World at War's helper read the same engine's lit
// model pixel shaders (WAW-NATIVE.md round 3: the mod tools' PC "lp_sun" shader ps_3_0_022bfc47)
//     L = tex3D(model lighting, place + n / max(|n.x|, |n.y|, |n.z|) x lightingLookupScale)
//     light = 2 x L.rgb + L.a x saturate(dot(toward the sun, n)) x the sun's color
// the texture ("$model_lighting", code image 3) being a 3D 8_8_8_8 one the CPU fills, each model owning a 2 x 2 x 2
// block whose middle is its place (c8). Here: the eight texels around the place (red, green, blue, and in alpha the
// share of the sun that reaches the model), 0 to 1. False when the texture isn't such a 3D texture or the place is
// outside it (the model is then drawn unlit, as before). Read at the frame's end: the texture is the game's cache of
// the models' light, and a visible model's block stays as it is from one frame to the next.
bool ModelLightTexels(const uint8_t* base, const MeshDraw& m, float texels[8][4]) {
  const std::array<uint32_t, 6>& fetch = m.lighting_fetch;
  if (fetch[0] == 0) return false;
  const int32_t size[3] = {static_cast<int32_t>(fetch[2] & 0x7FFu) + 1,
                           static_cast<int32_t>((fetch[2] >> 11) & 0x7FFu) + 1,
                           static_cast<int32_t>((fetch[2] >> 22) & 0x3FFu) + 1};
  const uint32_t endian = (fetch[1] >> 6) & 3u;
  const bool volume = ((fetch[5] >> 9) & 3u) == 2u && (fetch[1] & 0x3Fu) == 6u && (endian == 2u || endian == 0u);
  const float* place = m.lighting_place;
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO("cod4: native side: the model lighting texture: fetch {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}: "
                "{}x{}x{}, format {}, endian {}, {}, dimension {} ({}); the first model's place {:.5f} {:.5f} {:.5f} "
                "{:.5f} (from {})", fetch[0], fetch[1], fetch[2], fetch[3], fetch[4], fetch[5], size[0], size[1],
                size[2], fetch[1] & 0x3Fu, endian, (fetch[0] >> 31) ? "tiled" : "linear", (fetch[5] >> 9) & 3u,
                volume ? "read" : "not read: not a 3D 8_8_8_8 texture", place[0], place[1], place[2], place[3],
                m.lighting_place_from_packet ? "the command buffer's packet" : "the device's copy of c8");
  }
  if (!volume) return false;
  for (int k = 0; k < 3; ++k)
    if (!(place[k] > 0.0f && place[k] < 1.0f)) return false;
  const uint32_t pitch = std::max(((fetch[0] >> 22) & 0x1FFu) * 32u, static_cast<uint32_t>(size[0]));
  const uint32_t at = fetch[1] & 0x1FFFF000u;
  const bool tiled = (fetch[0] >> 31) != 0;
  int32_t low[3];
  for (int k = 0; k < 3; ++k) low[k] = static_cast<int32_t>(std::floor(place[k] * static_cast<float>(size[k]) - 0.5f));
  uint32_t words[8];
  for (int corner = 0; corner < 8; ++corner) {
    int32_t xyz[3];
    for (int k = 0; k < 3; ++k) xyz[k] = std::clamp(low[k] + ((corner >> k) & 1), 0, size[k] - 1);
    const int32_t offset = tiled ? TiledOffset3D(xyz[0], xyz[1], xyz[2], pitch, static_cast<uint32_t>(size[1]), 2)
                                 : ((xyz[2] * size[1] + xyz[1]) * static_cast<int32_t>(pitch) + xyz[0]) * 4;
    uint32_t word = GpuU32(base, at + static_cast<uint32_t>(offset));  // 0xAARRGGBB with endian 2
    if (endian == 0u) word = __builtin_bswap32(word);
    words[corner] = word;
    texels[corner][0] = static_cast<float>((word >> 16) & 0xFFu) / 255.0f;
    texels[corner][1] = static_cast<float>((word >> 8) & 0xFFu) / 255.0f;
    texels[corner][2] = static_cast<float>(word & 0xFFu) / 255.0f;
    texels[corner][3] = static_cast<float>(word >> 24) / 255.0f;
  }
  static int logged_texels = 0;
  if (logged_texels < 3) {  // the first few blocks, to check the place against the texels
    ++logged_texels;
    REXLOG_INFO("cod4: native side: model light block at {:.5f} {:.5f} {:.5f} (texel {} {} {}): {:08X} {:08X} {:08X} "
                "{:08X} {:08X} {:08X} {:08X} {:08X}", place[0], place[1], place[2], low[0], low[1], low[2], words[0],
                words[1], words[2], words[3], words[4], words[5], words[6], words[7]);
  }
  return true;
}

// [waw-decomp-cod4] 2026-11-11 round 5: the model's light as ngl_vk's irradiance rows (World at War's helper's
// ModelLightRows, recomp_waw/src/waw_native_side.cpp, the same engine's shader): the lookup blends the eight texels by
// (1 +- t.x)(1 +- t.y)(1 +- t.z) / 8, t the normal divided by its largest part, taken here as the unit normal: the
// light 2 x L.rgb is A + B . t + the products of two parts + D t.x t.y t.z, sums of the texels with their corners'
// signs in the world's axes, turned into the mesh's own axes by the placement's rows (d.world, filled before). The
// sun's share is the texels' alpha blended toward the sun, and scales the sun's color (techniques with the sun only).
void ModelLightRows(const float texels[8][4], const float toward_sun[4], const float sun_color[4], bool sun,
                    ngl_vk_mesh_draw& d) {
  float axis[3][3];  // axis[j]: the mesh's axis j in the world, of unit length
  for (int j = 0; j < 3; ++j) {
    const float* row = &d.world[4 * j];
    const float length = std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
    for (int k = 0; k < 3; ++k) axis[j][k] = length > 0.0f ? row[k] / length : (j == k ? 1.0f : 0.0f);
  }
  float a[3] = {}, b[3][3] = {}, pairs[3][3] = {}, cubic[3] = {};  // pairs: xy, yz, zx
  for (int corner = 0; corner < 8; ++corner) {
    float sign[3];
    for (int k = 0; k < 3; ++k) sign[k] = ((corner >> k) & 1) ? 1.0f : -1.0f;
    for (int c = 0; c < 3; ++c) {
      const float v = 2.0f * texels[corner][c] / 8.0f;  // the shader's 2 x L.rgb, the weights' 1/8
      a[c] += v;
      for (int k = 0; k < 3; ++k) b[k][c] += sign[k] * v;
      pairs[0][c] += sign[0] * sign[1] * v;
      pairs[1][c] += sign[1] * sign[2] * v;
      pairs[2][c] += sign[2] * sign[0] * v;
      cubic[c] += sign[0] * sign[1] * sign[2] * v;
    }
  }
  d.irradiance_rows = 9;
  for (int c = 0; c < 3; ++c) {
    float q[3][3] = {};  // the products of two parts as a symmetric form in the world's axes
    q[0][1] = q[1][0] = pairs[0][c] / 2.0f;
    q[1][2] = q[2][1] = pairs[1][c] / 2.0f;
    q[2][0] = q[0][2] = pairs[2][c] / 2.0f;
    float turned[3][3] = {};  // the same in the mesh's own axes
    for (int i = 0; i < 3; ++i)
      for (int j = 0; j < 3; ++j)
        for (int k = 0; k < 3; ++k)
          for (int l = 0; l < 3; ++l) turned[i][j] += axis[i][k] * q[k][l] * axis[j][l];
    for (int i = 0; i < 3; ++i) {
      d.irradiance[i][c] = a[c] + turned[i][i];  // xx, yy, zz: A once (x x + y y + z z = 1) and the form's own
      float linear = 0.0f;
      for (int k = 0; k < 3; ++k) linear += b[k][c] * axis[i][k];
      d.irradiance[6 + i][c] = linear;  // x, y, z
    }
    d.irradiance[3][c] = 2.0f * turned[0][1];  // xy
    d.irradiance[4][c] = 2.0f * turned[1][2];  // yz
    d.irradiance[5][c] = 2.0f * turned[2][0];  // zx
    d.irradiance_cubic[c] = cubic[c];
    d.ambient[c] = a[c];
  }
  for (int k = 0; k < 3; ++k)
    for (int j = 0; j < 3; ++j) d.irradiance_cubic_axes[k][j] = axis[j][k];  // world axis k in the mesh's space
  // the sun: the way it travels, and its color times its share toward the sun (the lookup at the sun's way divided by
  // its largest part)
  const float length = std::sqrt(toward_sun[0] * toward_sun[0] + toward_sun[1] * toward_sun[1] +
                                 toward_sun[2] * toward_sun[2]);
  const float largest = std::max({std::fabs(toward_sun[0]), std::fabs(toward_sun[1]), std::fabs(toward_sun[2])});
  float share = 0.0f;
  for (int corner = 0; corner < 8; ++corner) {
    float weight = 1.0f;
    for (int k = 0; k < 3; ++k) {
      const float t = largest > 0.0f ? toward_sun[k] / largest : 0.0f;
      weight *= 0.5f * (1.0f + (((corner >> k) & 1) ? t : -t));
    }
    share += weight * texels[corner][3];
  }
  for (int k = 0; k < 3; ++k) {
    d.light_direction[k] = length > 0.0f ? -toward_sun[k] / length : 0.0f;
    d.light_color[k] = sun ? std::clamp(sun_color[k], 0.0f, 4.0f) * std::clamp(share, 0.0f, 1.0f) : 0.0f;
  }
}

// [waw-decomp-cod4] 2026-11-11 round 5: a view's registers' way. KeepWorldDraw decides columns or rows per draw by
// which way more of up to 48 of its corners land on the screen, and on a tie (often 0 to 0: a wall near the eye whose
// corners are all off the screen) by the code constant's index. Run cod4mpside5 (mp_crash, in play in a garage) took
// 133 of 791 draws as rows against 9 in Killhouse (cod4side24), and its native picture showed the buildings beyond the
// garage's walls: walls drawn with their matrix turned over. The same four registers can only be right one way (a
// projection's matrix is not its own transpose), so every draw with the same view-projection's registers takes the way
// most of its draws that weren't tied chose (the draw's own when none was clear).
uint64_t ViewKey(const MeshDraw& m) {
  uint64_t key = 14695981039346656037ull;
  for (const auto& row : m.rows)
    for (const float value : row) {
      uint32_t bits;
      std::memcpy(&bits, &value, 4);
      key = (key ^ bits) * 1099511628211ull;
    }
  return key;
}

// Hands the kept frame's world draws to ngl_vk.
void SetMeshes(const uint8_t* base, uint64_t frame, const Frame& kept, int mode, std::string& summary) {
  std::vector<ngl_vk_mesh_draw> draws;
  draws.reserve(kept.meshes.size());
  uint32_t built = 0, failed = 0, textured = 0, lightmapped = 0, turned = 0;
  const double asked = REXCVAR_GET(cod4_debug_native_light_scale);
  const float light_scale = asked > 0.0 ? static_cast<float>(asked) : kLightScale;
  bool logged = false;
  // [waw-decomp-cod4] 08:55: the pages: for each surface lightmap index (FindSurface) the textures its draws read
  // the lightmap from, and how often (logged once). 09:55: and the pages the draws are given (page_of).
  std::map<uint32_t, std::map<uint32_t, uint32_t>> seen;
  std::map<uint32_t, std::map<uint32_t, uint32_t>> checked_seen;  // the same, of the draws whose page was checked
  std::map<uint32_t, TextureRef> by_texture;
  for (const MeshDraw& m : kept.meshes) {
    if (m.model) continue;
    const TextureRef& own = m.lightmap_primary.texture != 0 ? m.lightmap_primary : m.lightmap_secondary;
    ++seen[m.surface < 0 ? 0x100u : m.lightmap_index][own.texture];
    if (m.surface >= 0 && m.page_checked && m.lightmap_index < kNoLightmapIndex)
      ++checked_seen[m.lightmap_index][own.texture];
    by_texture[own.texture] = own;
  }
  std::map<uint32_t, uint32_t> checked;  // lightmap index -> its most frequent checked page
  for (const auto& [index, textures] : checked_seen) {
    uint32_t most = 0;
    for (const auto& [texture, count] : textures)
      if (count > most) most = count, checked[index] = texture;
  }
  std::string listed;
  for (const auto& [index, textures] : seen) {
    char entry[48];
    if (index == 0x100u) {
      listed += "; no surface found:";
    } else {
      std::snprintf(entry, sizeof entry, "; index %u:", index);
      listed += entry;
    }
    for (const auto& [texture, count] : textures) {
      std::snprintf(entry, sizeof entry, " %08X x%u", texture, count);
      listed += entry;
    }
  }
  FindWorldPages(base, checked);
  // the page a draw is lit from: by its surface's lightmap index (FindWorldPages' comment); `none` when the index
  // says it has no lightmap
  auto page_of = [&](const MeshDraw& m, bool& none) -> TextureRef {
    none = false;
    const TextureRef& own = m.lightmap_primary.texture != 0 ? m.lightmap_primary : m.lightmap_secondary;
    if (m.surface < 0 || m.lightmap_index > kNoLightmapIndex) return own;  // no surface found: as read
    if (m.lightmap_index == kNoLightmapIndex) {
      none = true;
      return TextureRef{};
    }
    if (g_world_pages.found) {
      const uint32_t array = LoadU32(base, g_world_pages.world + g_world_pages.offset);
      const TextureRef ref =
          TextureAt(base, array + g_world_pages.stride * m.lightmap_index + g_world_pages.inner);
      if (ref.texture != 0) return ref;
    }
    if (const auto found = checked.find(m.lightmap_index); found != checked.end())
      if (const auto ref = by_texture.find(found->second); ref != by_texture.end()) return ref->second;
    return own;
  };
  static uint64_t pages_logged = ~0ull;
  if (mode == 0 && pages_logged == ~0ull && !kept.meshes.empty()) {
    pages_logged = frame;
    REXLOG_INFO("cod4: native side: frame {}: the lightmap textures read by the surfaces' lightmap index{}", frame,
                listed);
  }
  const bool quiet = !SavedFrame(frame);  // [waw-decomp-cod4] 2026-11-11 round 6: drawn for the window alone
  if (mode == 0 && !quiet) {  // every kept frame: the pages given, and how many draws they changed
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> given;  // index -> (page, draws given another than read)
    for (const MeshDraw& m : kept.meshes) {
      if (m.model || m.surface < 0) continue;
      bool none = false;
      const TextureRef page = page_of(m, none);
      const TextureRef& own = m.lightmap_primary.texture != 0 ? m.lightmap_primary : m.lightmap_secondary;
      auto& entry = given[m.lightmap_index];
      entry.first = page.texture;
      entry.second += page.texture != own.texture;
    }
    std::string pages;
    for (const auto& [index, entry] : given) {
      char text[64];
      std::snprintf(text, sizeof text, "; index %u: %08X (%u draws changed)", index, entry.first, entry.second);
      pages += text;
    }
    char where[96] = "not found";
    if (g_world_pages.found)
      std::snprintf(where, sizeof where, "the world's +0x%X (entries %u bytes apart, the page at +%u)",
                    g_world_pages.offset, g_world_pages.stride, g_world_pages.inner);
    REXLOG_INFO("cod4: native side: frame {}: lightmap pages by index, the array {}{}", frame, where, pages);
  }
  uint32_t models = 0;
  // [waw-decomp-cod4] 2026-11-11 round 5: the registers' way by view (ViewKey), and the static models lit by their
  // block of the model lighting texture (ModelLightTexels)
  std::unordered_map<uint64_t, std::array<uint32_t, 2>> votes;  // view -> draws that chose columns, rows
  for (const MeshDraw& m : kept.meshes)
    if (m.on_screen_as_columns != m.on_screen_as_rows)
      ++votes[ViewKey(m)][m.on_screen_as_columns > m.on_screen_as_rows ? 0 : 1];
  auto columns_of = [&votes](const MeshDraw& m) {
    const auto found = votes.find(ViewKey(m));
    if (found == votes.end() || found->second[0] == found->second[1]) return m.columns;
    return found->second[0] > found->second[1];
  };
  uint32_t turned_by_view = 0, models_lit = 0;
  for (const MeshDraw& m : kept.meshes) {
    if (m.model && mode == 6) continue;  // 10:10: mode 6 is mode 0 without the static models
    uint32_t frame_corners = 0, index_count = 0;  // [waw-decomp-cod4] round 6 (g_mesh_room_tight)
    const uint32_t mesh = MeshFor(base, m, built, frame_corners, index_count);
    if (mesh == 0) {
      ++failed;
      continue;
    }
    ngl_vk_mesh_draw d{};
    d.mesh = mesh;
    d.corners = frame_corners;  // [waw-decomp-cod4] round 6: this frame's corners, or 0: the mesh's own
    d.index_count = index_count;  // and how many of the shared mesh's indices, or 0: all
    d.tint = 0x7F7F7Fu;  // a draw without a texture: mid grey (not 0x808080: bit 23 is a flag on lit draws)
    d.texture = TextureFor(base, m.color);
    bool no_lightmap = m.model;  // the surface's lightmap index is 0x1F (or a static model): drawn unlit
    const TextureRef page = m.model ? TextureRef{} : page_of(m, no_lightmap);
    models += m.model;
    d.lightmap = mode == 3 || no_lightmap ? 0 : LightmapFor(base, page, mode == 6 ? 0 : mode);
    d.shading = d.lightmap                  ? NGL_VK_SHADE_LIGHTMAP
                : mode == 3 || no_lightmap ? NGL_VK_SHADE_UNLIT
                                           : NGL_VK_SHADE_LIGHT;
    textured += d.texture != 0;
    lightmapped += d.lightmap != 0;
    if (d.lightmap) {
      // the light: scale x (the lightmap's color + the sun's color x the lightmap's alpha x the normal against the
      // sun); the sun only for the techniques that have it
      d.lit = 1;
      d.lightmap_scale = light_scale;
      const bool sun = mode == 0 && m.technique_type >= kSunTechniqueFirst && m.technique_type <= kSunTechniqueLast;
      const float* s = m.sun_position;  // a direction: toward the sun
      const float length = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
      for (int k = 0; k < 3; ++k) {
        d.light_direction[k] = length > 0.0f ? -s[k] / length : 0.0f;  // the way it travels
        d.light_color[k] = sun ? std::clamp(m.sun_diffuse[k] / light_scale, 0.0f, 4.0f) : 0.0f;
      }
    }
    d.alpha = AlphaOf(m.state0);
    if (d.alpha == NGL_VK_ALPHA_BLEND) d.blend = ((m.state0 >> 4) & 0xFu) == 2 ? NGL_VK_BLEND_ADD : NGL_VK_BLEND_ALPHA;
    // the world matrix (KeepWorldDraw; for the world's surfaces a move by the eye's place) in Direct3D's row-vector
    // form, the registers taken the same way as the view-projection's
    // [waw-decomp-cod4] 2026-11-11 round 5: the way the draw's view chose (columns_of), not the draw's own
    const bool columns = columns_of(m);
    turned_by_view += columns != m.columns;
    for (int i = 0; i < 4; ++i)
      for (int r = 0; r < 4; ++r) d.world[4 * i + r] = columns ? m.world_rows[r][i] : m.world_rows[i][r];
    // Direct3D's row-vector matrix from the four registers: as columns, entry [i][r] = register r's part i; as
    // rows, entry [i][r] = register i's part r
    for (int i = 0; i < 4; ++i)
      for (int r = 0; r < 4; ++r) d.view_projection[4 * i + r] = columns ? m.rows[r][i] : m.rows[i][r];
    // [waw-decomp-cod4] 2026-11-11 round 5: a static model lit by its block of the model lighting texture (else
    // unlit, as before); not in mode 3 (nothing lit there)
    if (m.model && mode != 3) {
      float texels[8][4];
      if (ModelLightTexels(base, m, texels)) {
        d.shading = NGL_VK_SHADE_LIGHT;
        d.lit = 1;
        const bool sun = mode == 0 && m.technique_type >= kSunTechniqueFirst && m.technique_type <= kSunTechniqueLast;
        ModelLightRows(texels, m.sun_position, m.sun_diffuse, sun, d);
        ++models_lit;
      }
    }
    // The console's depth runs the other way (1 at the near plane, 0 far away: the state's depth range is 1 to 0,
    // run cod4cap1): far along the view (the w register's direction) clip z over w nears `limit`, and where that is
    // below one half, z is turned over (w - z) for ngl_vk, which takes Direct3D's usual 0 near to 1 far.
    const float z[3] = {d.view_projection[2], d.view_projection[6], d.view_projection[10]};
    const float w[3] = {d.view_projection[3], d.view_projection[7], d.view_projection[11]};
    const float along = w[0] * w[0] + w[1] * w[1] + w[2] * w[2];
    const float limit = along > 0.0f ? (z[0] * w[0] + z[1] * w[1] + z[2] * w[2]) / along : 1.0f;
    if (limit < 0.5f) {
      ++turned;
      for (int i = 0; i < 4; ++i)
        d.view_projection[4 * i + 2] = d.view_projection[4 * i + 3] - d.view_projection[4 * i + 2];
    }
    if (!logged && mode == 0 && !quiet) {  // [waw-decomp-cod4] round 6: saved frames only
      logged = true;
      REXLOG_INFO("cod4: native side: frame {}: the first draw's registers: x {:.4f} {:.4f} {:.4f} {:.2f}, y {:.4f} "
                  "{:.4f} {:.4f} {:.2f}, z {:.5f} {:.5f} {:.5f} {:.3f}, w {:.4f} {:.4f} {:.4f} {:.2f} (taken as {}); "
                  "depth far away {:.4f}{}; lightmap scale {:.2f}", frame, m.rows[0][0], m.rows[0][1], m.rows[0][2],
                  m.rows[0][3], m.rows[1][0], m.rows[1][1], m.rows[1][2], m.rows[1][3], m.rows[2][0], m.rows[2][1],
                  m.rows[2][2], m.rows[2][3], m.rows[3][0], m.rows[3][1], m.rows[3][2], m.rows[3][3],
                  m.columns ? "columns" : "rows", limit, limit < 0.5f ? " (turned over)" : "", light_scale);
    }
    draws.push_back(d);
  }
  ngl_vk_set_meshes(draws.data(), static_cast<uint32_t>(draws.size()));
  ngl_vk_set_sun_shadow(nullptr, nullptr);
  summary = std::to_string(draws.size()) + " world draws (" + std::to_string(models) + " of them static models', " +
            std::to_string(models_lit) + " models lit by their light's texture, " + std::to_string(turned_by_view) +
            " with their registers' way taken from their view, " +
            std::to_string(textured) + " textured, " + std::to_string(lightmapped) + " lightmapped, " +
            std::to_string(turned) + " with the depth turned over, " + std::to_string(built) + " meshes made, " +
            std::to_string(failed) + " not made) drawn";
  if (g_corner_count > 0) {
    const double n = static_cast<double>(g_corner_count);
    char line[256];
    std::snprintf(line, sizeof(line),
                  " (the corners' colors so far: averages blue %.1f green %.1f red %.1f alpha %.1f, %.1f%% not white)",
                  g_corner_colors[0] / n, g_corner_colors[1] / n, g_corner_colors[2] / n, g_corner_colors[3] / n,
                  100.0 * static_cast<double>(g_corners_not_white) / n);
    summary += line;
  }
}

// Draws a kept frame natively and hands the picture to the helper thread. [waw-decomp-cod4] 2026-11-11 round 6: with
// cod4_native_vulkan every frame, each picture handed to the window (cod4_native_window.h); only the saved frames (the
// check's) are logged, read back and saved then.
void DrawFrame(const uint8_t* base, uint64_t frame, const Frame& kept) {
  if (g_state < 0) return;
  const bool saved = SavedFrame(frame);
  if (g_state == 0) {
    // [waw-decomp-cod4] round 6: with cod4_native_vulkan, on the window's own device when it can
    if ((NativeVulkanOn() ? StartNglVkForWindow(kPictureWidth, kPictureHeight)
                          : ngl_vk_init(kPictureWidth, kPictureHeight)) != 0) {
      REXLOG_WARN("cod4: native side: ngl_vk couldn't start (see its reason on stderr); nothing more is drawn");
      g_state = -1;
      return;
    }
    g_state = 1;
    REXLOG_INFO("cod4: native side: ngl_vk started on {}, a {}x{} picture", ngl_vk_device_name(), kPictureWidth,
                kPictureHeight);
  }
  const auto setup_start = std::chrono::steady_clock::now();
  // between frames, on the drawing thread: forgotten when ngl_vk ran out of room, or before a frame could
  // [waw-decomp-cod4] 2026-11-11 round 6: with cod4_native_vulkan at three quarters, and no new meshes past half
  // (g_mesh_room_tight); the check alone forgets at half, as before
  uint32_t corners_used = 0, corner_room = 0;
  ngl_vk_room(nullptr, nullptr, &corners_used, &corner_room);
  if (g_forget || corners_used > (NativeVulkanOn() ? corner_room / 4 * 3 : corner_room / 2)) {
    static int forgets_logged = 0;
    if (NativeVulkanOn() && forgets_logged < 12) {
      ++forgets_logged;
      uint32_t textures = 0, texture_room = 0;
      ngl_vk_room(&textures, &texture_room, nullptr, nullptr);
      REXLOG_INFO("cod4: native side: frame {}: everything forgotten: {}; {} of {} textures in ngl_vk, {} of {} mesh "
                  "corners", frame, g_forget ? "something found no room" : "the corners past three quarters",
                  textures, texture_room, corners_used, corner_room);
    }
    ForgetAll();
    ++g_native_cost.forgets;
    corners_used = 0;
  }
  g_mesh_room_tight = NativeVulkanOn() && corners_used > corner_room / 2;
  // mode 0 the picture; with cod4_debug_native_side_variants the light's modes 2 to 5 too (LightmapFor, SetMeshes).
  // [waw-decomp-cod4] 2026-11-11 09:55: every picture then goes through the film pass and the display's gamma ramp
  // as the game's does (ShownColors, FilmFrom); m1 is mode 0 before both, m7 mode 0 through the ramp alone; 10:10: m6
  // is mode 0 without the static models. Round 6: on the saved frames only.
  const int modes = saved && REXCVAR_GET(cod4_debug_native_side_variants) && !kept.meshes.empty() ? 7 : 1;
  uint8_t shown[3][256];
  float gamma = 0.0f;
  const int ramp = ShownColors(base, kept.device, shown, gamma);
  Film film{};
  std::string film_settings;
  const bool film_on = FilmFrom(base, kept.film_view, film, film_settings);
  auto through_ramp = [&shown](std::vector<uint8_t>& rgba) {
    for (size_t i = 0; i + 3 < rgba.size(); i += 4)
      for (int channel = 0; channel < 3; ++channel) rgba[i + channel] = shown[channel][rgba[i + channel]];
  };
  if (saved) {  // [waw-decomp-cod4] round 6: saved frames only
    std::string constants;  // the source's code constants 43 to 48 as read, to check the film's index by
    for (uint32_t index = 43; index <= 48 && IsGuestData(kept.source); ++index) {
      char entry[96];
      const uint32_t at = kept.source + kSourceCodeConstants + 16u * index;
      std::snprintf(entry, sizeof entry, " %u: %.4f %.4f %.4f %.4f;", index, LoadF32(base, at), LoadF32(base, at + 4u),
                    LoadF32(base, at + 8u), LoadF32(base, at + 12u));
      constants += entry;
    }
    REXLOG_INFO("cod4: native side: frame {}: after the scene: gamma ramp {} (r_gamma {:.3f}; red 8 / 40 / 104 / 152 "
                "/ 216 -> {} / {} / {} / {} / {}), film pass {}; source {:08X} code constants{}", frame,
                ramp == 2   ? "from the device"
                : ramp == 3 ? "from the device (PWL form)"
                : ramp == 1 ? "made from r_gamma"
                            : "none (plain)",
                gamma,
                int{shown[0][8]}, int{shown[0][40]}, int{shown[0][104]}, int{shown[0][152]}, int{shown[0][216]},
                film_on ? "applied" : "not applied (off, the defaults, or not recognized)", kept.source, constants);
    REXLOG_INFO("cod4: native side: frame {}: the film: {}", frame, film_settings);
  }
  // [waw-decomp-cod4] 2026-11-11 round 6: a picture that goes to the window on the GPU is read back only when it is
  // also saved
  const bool on_gpu = NativeVulkanOn() && PictureOnGpu();
  if (on_gpu) ngl_vk_set_readback(saved ? 1 : 0);
  Job job;
  job.frame = frame;
  for (int mode = 0; mode < modes; ++mode) {
    if (mode == 1) continue;  // m1 is mode 0's picture before the film and the ramp (below)
    std::string summary;
    SetMeshes(base, frame, kept, mode, summary);
    if (mode == 0) g_native_cost.setup_ms += MillisecondsSince(setup_start);
    const auto draw_start = std::chrono::steady_clock::now();
    const ngl_vk_vertex none{};
    if (ngl_vk_draw_quads(&none, 0, static_cast<float>(kPictureWidth), static_cast<float>(kPictureHeight), nullptr,
                          0, 0, 0) != 0) {
      REXLOG_WARN("cod4: native side: drawing frame {} failed; nothing more is drawn", frame);
      g_state = -1;
      return;
    }
    // [waw-decomp-cod4] 2026-11-11 round 6: with cod4_native_vulkan this picture is the game's: to the window on the
    // GPU through the gamma ramp (ngl_vk_output takes one table per channel, so the film pass, which mixes the
    // channels, isn't applied there yet: COD4-NATIVE.md round 4's ngl_vk_set_film_tweak), or through the CPU with the
    // film applied first (below)
    if (mode == 0 && on_gpu) {
      ShowOnGpu(shown);
      g_native_cost.draw_ms += MillisecondsSince(draw_start);
      if (!saved) break;
    }
    uint32_t out_width = 0, out_height = 0;
    const uint8_t* pixels = static_cast<const uint8_t*>(ngl_vk_target_pixels(&out_width, &out_height));
    if (!pixels || !out_width || !out_height) {
      REXLOG_WARN("cod4: native side: frame {}: ngl_vk gave no picture back", frame);
      return;
    }
    std::vector<uint8_t> picture(pixels, pixels + size_t(out_width) * out_height * 4);
    if (mode == 0) {
      job.width = out_width;
      job.height = out_height;
      job.summary = summary;
      if (modes > 1) job.variants.push_back(picture);  // m1: before the film and the ramp
    }
    if (film_on) ApplyFilm(film, picture);
    if (mode == 0 && NativeVulkanOn() && !on_gpu) {  // round 6: through the CPU, the ramp applied on the way
      ShowPixels(picture.data(), out_width, out_height, shown);
      g_native_cost.draw_ms += MillisecondsSince(draw_start);
      if (!saved) break;
    }
    through_ramp(picture);
    if (mode == 0) {
      job.rgba = std::move(picture);
    } else {
      job.variants.push_back(std::move(picture));
    }
  }
  // [waw-decomp-cod4] 2026-11-11 round 6: what the native picture cost, a line every 600 frames drawn
  g_native_cost.draws += kept.meshes.size();
  if (NativeVulkanOn() && ++g_native_cost.frames == 600) {
    const NativeCost& c = g_native_cost;
    REXLOG_INFO("cod4: native Vulkan: frame {}: over the last 600 frames {} times everything was forgotten; on average "
                "{:.0f} 3D draws, their set-up {:.1f} ms, the drawing and the hand-over {:.1f} ms; {} meshes made, {} "
                "draws from this frame's corners (the room past half)", frame, c.forgets,
                static_cast<double>(c.draws) / 600.0, c.setup_ms / 600.0, c.draw_ms / 600.0, c.made,
                c.frame_corner_draws);
    g_native_cost = NativeCost{};
  }
  if (!saved) return;  // round 6: drawn for the window alone
  if (modes > 1 && !job.variants.empty()) {  // m7: mode 0 through the ramp alone
    std::vector<uint8_t> ramp_only = job.variants.front();
    through_ramp(ramp_only);
    job.variants.push_back(std::move(ramp_only));
  }
  HandOver(std::move(job));
}
#endif  // COD4_NGL_VK

// What the kept frame held, in one line.
void LogFrame(const uint8_t* base, uint64_t frame, const Frame& kept) {
  std::string kinds;
  for (const auto& [kind, count] : kept.kinds) {
    char entry[64];
    std::snprintf(entry, sizeof entry, " %u/%u:%u(%u)", kind.first, kind.second, count.first, count.second);
    kinds += entry;
  }
  REXLOG_INFO("cod4: native side: frame {} kept: {} indexed draws, {} world draws kept ({} of them static models', {} "
              "placed by their instance); left out: {} without a state, {} lit of other kinds, {} misread, {} without "
              "the matrix, {} past the room; by surface type/technique type:draws(triangles):{}", frame, kept.seen,
              kept.meshes.size(), kept.model_draws, kept.model_placed, kept.no_state, kept.not_world, kept.misread,
              kept.no_matrix, kept.dropped, kinds);
  std::string depth_kinds;
  for (const auto& [kind, count] : kept.depth_kinds) {
    char entry[80];
    std::snprintf(entry, sizeof entry, " %u/%d/%.3f-%.3f:%u(%u)", std::get<0>(kind),
                  static_cast<int>(std::get<1>(kind)), std::get<2>(kind) / 1000.0, std::get<3>(kind) / 1000.0,
                  count.first, count.second);
    depth_kinds += entry;
  }
  REXLOG_INFO("cod4: native side: frame {}: by surface type/depth range kind/near-far (state +0x12C, +0x130, "
              "+0x134):draws(triangles):{}", frame, depth_kinds);
  std::string last;
  for (const auto& draw : kept.last_draws)
    last += " \"" + NameOf(base, draw[0]) + "\" " + std::to_string(draw[1]) + "/" + std::to_string(draw[2]) + ";";
  REXLOG_INFO("cod4: native side: frame {}: the last draws (material surface type/technique type):{} the film "
              "material (0x835A0F1C) \"{}\", the plain one (0x835A0F18) \"{}\"; the film's draw: {}", frame, last,
              NameOf(base, LoadU32(base, kFilmMaterialPointer)),
              NameOf(base, LoadU32(base, kPlainPostMaterialPointer)),
              kept.film_draw.empty() ? std::string("not seen at the indexed draw") : kept.film_draw);
  std::string offsets;
  for (uint32_t k = 0; k < 16; ++k) {
    if (const uint32_t n = g_fetch_found[k].load(std::memory_order_relaxed)) {
      char entry[32];
      std::snprintf(entry, sizeof entry, " +0x%X:%u", 4u * k, n);
      offsets += entry;
    }
  }
  REXLOG_INFO("cod4: native side: frame {}: {} matrices taken as rows; textures' fetch constants found at{} (missed "
              "{}), all told", frame, kept.as_rows, offsets.empty() ? std::string(" -") : offsets,
              g_fetch_missed.load(std::memory_order_relaxed));
  // [waw-decomp-cod4] 2026-11-11 round 5: what draws the sky: the first material of each kind, and where the draws
  // without a run's state come from; and how many static models had their light's texture and place
  std::string materials;
  for (const auto& [kind, material] : kept.kind_materials) {
    materials += " " + std::to_string(kind.first) + "/" + std::to_string(kind.second) + " \"" + NameOf(base, material) +
                 "\"";
    // [waw-decomp-cod4] 2026-11-11 round 6: its technique's name, and past the lit range its pass's arguments
    if (const auto t = kept.kind_techniques.find(kind); t != kept.kind_techniques.end())
      materials += " (technique \"" + NameOf(base, t->second) + "\")";
    if (const auto a = kept.kind_arguments.find(kind); a != kept.kind_arguments.end())
      materials += " arguments" + a->second;
    materials += ";";
  }
  std::string callers;
  for (const auto& [caller, count] : kept.no_state_callers) {
    char entry[32];
    std::snprintf(entry, sizeof entry, " %08X x%u", caller, count);
    callers += entry;
  }
  std::string skies;
  for (const auto& [name, seen] : kept.sky_materials)
    skies += " \"" + name + "\" " + std::to_string(seen.first.first) + "/" + std::to_string(seen.first.second) + " x" +
             std::to_string(seen.second) + ";";
  REXLOG_INFO("cod4: native side: frame {}: the first material of each kind (surface type/technique type):{} the "
              "draws without a state come from (return address):{}; {} of {} static model draws with their light's "
              "texture, {} with their place from the command buffer; materials named \"sky\":{}", frame, materials,
              callers.empty() ? std::string(" none") : callers, kept.model_light_found, kept.model_draws,
              kept.model_light_from_packet, skies.empty() ? std::string(" none") : skies);
}

// The kept frame's end, on the back end's render thread.
void FrameEnd(const uint8_t* base) {
  const uint64_t frame = g_frames.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!DrawnFrame(frame)) return;  // [waw-decomp-cod4] round 6: or every frame (cod4_native_vulkan)
  Frame kept;
  {
    std::lock_guard lock(g_frame_mutex);
    std::swap(kept, g_frame);
    g_frame.Clear();
  }
  if (SavedFrame(frame)) LogFrame(base, frame, kept);  // [waw-decomp-cod4] round 6: saved frames only
#if defined(COD4_NGL_VK)
  DrawFrame(base, frame, kept);
#else
  (void)base;
  if (frame == kKeepEvery && SavedFrame(frame))
    REXLOG_INFO("cod4: native side: this build has no ngl_vk (Vulkan headers or glslc missing): counts only");
#endif
}

}  // namespace

// [waw-decomp-cod4] 2026-11-11 round 6: or the native picture is the game's (cod4_native_vulkan)
bool SideOn() { return REXCVAR_GET(cod4_debug_native_side) || NativeVulkanOn(); }

void SideDraw(const uint8_t* base, uint32_t context, uint32_t surface_type, uint32_t device, uint32_t caller,
              uint32_t base_vertex, uint32_t first_index, uint32_t index_count) {
  if (!SideOn()) return;
  const uint64_t next = g_frames.load(std::memory_order_relaxed) + 1;
  if (!DrawnFrame(next)) return;  // [waw-decomp-cod4] round 6: or every frame (cod4_native_vulkan)
  std::lock_guard lock(g_frame_mutex);
  Frame& f = g_frame;
  f.quiet = !SavedFrame(next);  // round 6: no samples logged for a frame drawn for the window alone
  ++f.seen;
  if (!IsGuestData(context) || !IsGuestData(device)) {
    ++f.no_state;
    return;
  }
  const uint32_t source = LoadU32(base, context), state = LoadU32(base, context + 4u);
  // the run's state must be the one this draw goes through (a draw outside the draw lists, the 2D's, comes after
  // the last run's context has gone)
  if (!IsGuestData(source) || !IsGuestData(state) || LoadU32(base, state + kStateDevice) != device) {
    ++f.no_state;
    // [waw-decomp-cod4] 2026-11-11 round 5: where they come from (the sky may be one of them: LogFrame)
    if (f.no_state_callers.size() < 8 || f.no_state_callers.count(caller)) ++f.no_state_callers[caller];
    return;
  }
  const uint32_t technique_type = LoadU32(base, state + kStateTechniqueType);
  auto& kind = f.kinds[{surface_type, technique_type}];
  ++kind.first;
  kind.second += index_count / 3;
  if (kind.first == 1) f.kind_materials[{surface_type, technique_type}] = LoadU32(base, state + kStateMaterial);
  // [waw-decomp-cod4] 2026-11-11 round 6: the technique, and past the lit range its pass's arguments (saved frames)
  if (kind.first == 1 && !f.quiet) {
    f.kind_techniques[{surface_type, technique_type}] = LoadU32(base, state + kStateTechnique);
    const uint32_t pass = LoadU32(base, state + kStatePass);
    if (technique_type > kLastLitTechnique && technique_type <= 16 && IsGuestData(pass)) {
      const uint32_t count = uint32_t(LoadU8(base, pass + kPassArgumentCounts)) +
                             LoadU8(base, pass + kPassArgumentCounts + 1u) +
                             LoadU8(base, pass + kPassArgumentCounts + 2u);
      const uint32_t arguments = LoadU32(base, pass + kPassArguments);
      std::string text;
      for (uint32_t i = 0; i < count && i < 16 && IsGuestData(arguments); ++i) {
        char entry[48];
        std::snprintf(entry, sizeof entry, " %u:%u:%08X", LoadU16(base, arguments + 8u * i),
                      LoadU16(base, arguments + 8u * i + 2u), LoadU32(base, arguments + 8u * i + 4u));
        text += entry;
      }
      f.kind_arguments[{surface_type, technique_type}] = text;
    }
  }
  const int depth_near = static_cast<int>(std::lround(LoadF32(base, state + kStateDepthNear) * 1000.0f));
  const int depth_far = static_cast<int>(std::lround(LoadF32(base, state + kStateDepthFar) * 1000.0f));
  auto& depth_kind = f.depth_kinds[{surface_type, LoadU32(base, state + kStateDepthRange), depth_near, depth_far}];
  ++depth_kind.first;
  depth_kind.second += index_count / 3;
  f.source = source;
  f.device = device;
  // 10:40: the view while it is drawn (run cod4side19 found the back end with no view at the frame's end, where
  // FilmFrom first looked); 12:00: not in cod4mp yet (kBackEndDataPointer 0)
  if (f.film_view == 0 && kBackEndDataPointer != 0) {
    const uint32_t back_end = LoadU32(base, kBackEndDataPointer);
    if (IsGuestData(back_end) && LoadU32(base, back_end + kBackEndViewCount) != 0)
      f.film_view = LoadU32(base, back_end + kBackEndViews);
  }
  const uint32_t material = LoadU32(base, state + kStateMaterial);
  if (const std::string name = NameOf(base, material); name.find("sky") != std::string::npos) {
    auto& sky = f.sky_materials[name];
    sky.first = {surface_type, technique_type};
    ++sky.second;
  }
  f.last_draws.push_back({material, surface_type, technique_type});
  if (f.last_draws.size() > 8) f.last_draws.pop_front();
  if (f.film_draw.empty() && material != 0 &&
      (material == LoadU32(base, kFilmMaterialPointer) || material == LoadU32(base, kPlainPostMaterialPointer))) {
    // the film pass's arguments, and for each pixel code constant (type 5) the registers' values as set now
    const uint32_t pass = LoadU32(base, state + kStatePass);
    const uint32_t count = IsGuestData(pass) ? uint32_t(LoadU8(base, pass + kPassArgumentCounts)) +
                                                   LoadU8(base, pass + kPassArgumentCounts + 1u) +
                                                   LoadU8(base, pass + kPassArgumentCounts + 2u)
                                             : 0u;
    const uint32_t arguments = IsGuestData(pass) ? LoadU32(base, pass + kPassArguments) : 0u;
    std::string text = "\"" + NameOf(base, material) + "\" arguments";
    for (uint32_t i = 0; i < count && i < 16 && IsGuestData(arguments); ++i) {
      const uint32_t type = LoadU16(base, arguments + 8u * i), destination = LoadU16(base, arguments + 8u * i + 2u);
      const uint32_t value = LoadU32(base, arguments + 8u * i + 4u);
      char entry[160];
      std::snprintf(entry, sizeof entry, " %u:%u:%08X", type, destination, value);
      text += entry;
      if (type == 5) {
        const uint32_t rows = std::min<uint32_t>(value & 0xFFu, 4u);
        for (uint32_t r = 0; r < rows && destination + r < 256; ++r) {
          const uint32_t at = device + kDevicePixelConstants + 16u * (destination + r);
          std::snprintf(entry, sizeof entry, " [c%u %.4f %.4f %.4f %.4f]", destination + r, LoadF32(base, at),
                        LoadF32(base, at + 4u), LoadF32(base, at + 8u), LoadF32(base, at + 12u));
          text += entry;
        }
      }
    }
    f.film_draw = text;
  }
  if (technique_type < kFirstLitTechnique || technique_type > kLastLitTechnique) return;
  // the world's lit surfaces come through its tess function (run cod4side4: none of them from 0x8210C468, the
  // world's draw of its own index buffer, which draws the shadow maps and the depth prepass), with the world's
  // 44-byte corners (KeepWorldDraw checks the stride)
  (void)caller;
  // [waw-decomp-cod4] 2026-11-11 round 6: and always in the game's own native picture (cod4_native_vulkan)
  const bool static_model = surface_type == kStaticModelSurfaces &&
                            (REXCVAR_GET(cod4_debug_native_side_models) || NativeVulkanOn());
  if (surface_type != kWorldSurfaces && !static_model) {
    ++f.not_world;
    return;
  }
  KeepWorldDraw(base, state, source, device, base_vertex, first_index, index_count, technique_type, f, static_model);
}

void SideFrame(const uint8_t* base) {
  if (!SideOn()) return;
  FrameEnd(base);
}

void SideStart(const std::filesystem::path& folder, SideCaptureFn capture) {
  // [waw-decomp-cod4] 2026-11-11 round 6: the check alone saves pictures (cod4_native_vulkan saves none)
  if (!REXCVAR_GET(cod4_debug_native_side) || g_helper.joinable()) return;
  g_folder = folder;
  g_capture = std::move(capture);
  g_stop = false;
  g_helper = std::thread(RunHelper);
  REXLOG_INFO("cod4: native side: on; frames {} to {}, every {}, saved in {}", kKeepEvery, kKeepLast, kKeepEvery,
              folder.string());
}

void SideStop() {
  {
    std::lock_guard lock(g_jobs_mutex);
    g_stop = true;
  }
  g_jobs_cv.notify_all();
  if (g_helper.joinable()) g_helper.join();
}

}  // namespace cod4::native

// [waw-decomp-cod4] 2026-11-11 12:00: cod4mp's two hooks (cod4_capture_draw, cod4_native_frame in cod4_census.cpp) are
// midasm hooks in mp/config/default.toml, at the campaign's code's places there. 12:20: Direct3D's indexed draw
// 0x820AB660 (the campaign's 0x8209BD80) and the back end's frame 0x82159AC8 (0x82128488): each the one place in
// cod4mp's code equal to the campaign function's first instructions with branch targets and 16-bit fields left out,
// reading both images flat (file offset = address - 0x82000000; round 3's 0x820A9058 / 0x821574C8 came from reading
// the code by the sections' raw offsets, wrong for these images).
