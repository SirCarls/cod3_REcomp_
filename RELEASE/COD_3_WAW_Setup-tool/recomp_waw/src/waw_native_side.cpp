// waw - World at War's native renderer: a frame's draws drawn natively by ngl_vk, beside the game's own picture
// (waw_native_side.h). [waw-decomp-waw] 2026-11-11
//
// A check only, off by default: the game keeps drawing through the GPU emulation, and with waw_debug_native_side off
// nothing here runs. With it on, the frames 600, 1200, ... 4800 (counted at Direct3D's swap from when it is on, so
// about every 10 s for the first 80 s, as Call of Duty 2's cod2_debug_native_side does) are kept:
//
//   - every draw is counted by its kind (indexed or from the tess buffers, surface type, technique type), and the
//     kept frame logs the counts: what a frame is made of, for the steps to come (models, effects, sky, 2D);
//   - the lit draws of the world and of the models are kept with what they submit (technique types 8 and up: 8
//     "lm_*", 10 "lm_sun_*" and "lp_sun_*", 14 "lm_spot_*" in runs waw-decomp-waw-cap1 and -side1b; the first pass
//     of the technique). The world's: surface types 0 (its triangles, tess function 0x82444988) and 2 (brush models,
//     0x82444BE0) with the world's 44-byte vertices. The models': surface types 1 (static models), 3 (rigid models)
//     and 4 (skinned models, from the cache the game's own skinning fills) with the 32-byte packed vertices. Kept:
//       - the vertex buffer's address (stream 0: the Direct3D vertex buffer's fetch constant at +0x18, plus the
//         stream's offset, which for the world is the surface's first vertex x 44) and the draw's 16-bit indices,
//         copied from the index buffer (+0x18 its address; the draw's arguments: +4 the triangle count, +8 the
//         first index);
//       - the textures, as Direct3D's device holds their fetch constants for the GPU at the draw (device + 0x480 +
//         24 x sampler slot): the color map in the slot the pass's "colorMap" argument names, and for the world the
//         two lightmaps the pass asks for (its custom sampler flags, pass + 0x4F: bit 1 the primary one in slot 2,
//         bit 2 the secondary one in slot 3; the world's draw function 0x82441CE0 binds them surface by surface
//         without telling the command buffer state, so the state's own slot table doesn't have them);
//       - the render state bits (state + 0x1154);
//       - the two matrices: the registers the pass's arguments give code constant 0x7B (the view-projection, c0-c3)
//         and 0x6B (the object's placement, c4-c7), read from Direct3D's own copy of the vertex shader constants
//         (device + 0x780). Both are relative to the eye: the placement of the world itself is a move by minus the
//         eye's place. The static and the rigid models' functions write the placement straight into the GPU's
//         command stream (0x8245E1E0) and leave the device's copy alone: theirs is read from where they wrote it
//         (kStaticModelDrawReturn);
//       - the sun, when the pass takes it: its direction and color from the pixel shader registers its arguments
//         give code constants 0x24 and 0x25 (device + 0x1780).
//
// At the swap a kept frame is drawn by ngl_vk (the project's ngl_vk/, as Call of Duty 2's, Call of Duty 3's and Big
// Red One's native renderers draw): each draw's geometry becomes an ngl_vk mesh once (by its vertex buffer and
// indices), its textures are decoded once each (waw_native_textures.h), and the picture is saved as
// userdata/logs/screenshots/ngl-side-<frame>-vk.ppm; a helper thread then saves the game's own picture of the same
// moment (the SDK presenter's CaptureGuestOutput) as ngl-side-<frame>-game.png, and one log line names both, for
// tools/ngl_side_compare.py. The world is lit by its lightmaps and the sun (LightmapFor), the models by the sun and
// their block of the model lighting texture (ModelLightAt, [waw-decomp-waw] 2026-11-11 round 3; a flat stand-in when
// that block can't be read).
//
// The game's projection is an infinite one with the depth turned over (clip z is the near distance, a constant, so
// depth = near / w: 1 at the near plane, 0 far away; c2 = (0, 0, 0, 4) in the capture). ngl_vk tests depth the usual
// way, so the z row handed to it is rebuilt from the w row: z = w x far / (far - near) - near x far / (far - near).
//
// The world's layered materials get their second layer drawn over the first (LayerOf, round 3), with their tints and
// the first layer's cut (BakedTextureFor, round 4); the lit draws are fogged (FogOf, round 4, as near as ngl_vk's
// linear fog comes); the sky's model is drawn unlit (a "sky" technique, round 4; a sky cube would be drawn as a box,
// AddSkyBox). Not yet drawn: effects, decals' own blends, the film (only logged: LogFilm), 2D (logged at frame 4200:
// LogSkyAnd2D). A build without ngl_vk (no Vulkan headers or glslc) only counts.
//
// [waw-decomp-waw] 2026-11-11 round 5: the native picture as the game's own (waw_native_vulkan, off by default;
// waw_native_window.h). With it on, the GPU emulation is off and every frame is drawn here (DrawnFrame), on the
// window's Vulkan device when ngl_vk can use it, and handed to the window through the game's gamma table
// (ShownColors): on the GPU, read back only for a saved frame; else through the CPU. What is logged or checked for the
// record alone (the frame's counts, the film, the world check, the sky and 2D lines) is done on the saved frames only
// (SavedFrame: waw_debug_native_side's 600, 1200, ... 4800), as Call of Duty 2's step 4z61 does: with every frame
// drawn it would fill the log. With waw_native_vulkan off everything here runs as before (the drawn frames are the
// saved ones). With it on, the 2D is drawn too (Keep2D, from the tess buffers) and the skinned models' corners are
// given frame by frame (SkinnedMeshFor).

#include "waw_native_side.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
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

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ui/presenter.h>

#include "waw_native_layout.h"
#include "waw_native_textures.h"
#include "waw_native_window.h"  // [waw-decomp-waw] 2026-11-11 round 5: waw_native_vulkan

#if defined(WAW_NGL_VK)
extern "C" {
#include "ngl_vk.h"
}
#endif

REXCVAR_DEFINE_BOOL(waw_debug_native_side, false, "waw",
                    "Tests: at frames 600, 1200, ... 4800, draw the frame natively (ngl_vk) and save it beside the "
                    "game's own picture (userdata/logs/screenshots/ngl-side-*), for tools/ngl_side_compare.py");
REXCVAR_DEFINE_INT32(waw_debug_native_light_test, 0, "waw",
                     "Tests: one part of the native picture's light changed, to find by pictures where it differs "
                     "from the game's. 0 nothing changed; 1 no sun; 2 the world without its lightmaps; 3 no color "
                     "maps (the light alone); 5 the world's lightmap light halved; 6 that light doubled; 7 the models "
                     "lit by the flat stand-in of before round 3, not by the model lighting texture; 8 the world's "
                     "layered materials without their second layer; 9 no fog; 10 the layers without their tints "
                     "and cut; 11 no sky box");
// [waw-decomp-waw] 2026-11-11 09:42: runs side3 and cand1 still show large grey shapes across the middle of the native
// picture with every model from an unknown caller left out, so they come from a kind not suspected yet: this draws
// one kind alone, to find it by pictures.
REXCVAR_DEFINE_INT32(waw_debug_native_only_surface, -1, "waw",
                     "Tests: with waw_debug_native_side, draw only the 3D draws of one surface type (0 the world's "
                     "triangles, 1 static models, 2 brush models, 3 rigid models, 4 skinned models); -1 all of them");

namespace waw::native {
namespace {

constexpr uint64_t kKeepEvery = 600;  // frames
constexpr uint64_t kKeepLast = 4800;
constexpr uint32_t kSurfaceTypes = 17;          // 0-15, and 16 for a draw outside the draw lists
constexpr uint32_t kWorldSurfaceType = 0;       // the world's triangles (tess function 0x82444988)
constexpr uint32_t kStaticModelSurfaceType = 1;  // static models (0x8244A818 draws them)
constexpr uint32_t kBrushModelSurfaceType = 2;  // brush models: the world's vertices, placed by their own matrix
constexpr uint32_t kRigidModelSurfaceType = 3;  // models in one piece (0x82449DB0)
constexpr uint32_t kSkinnedSurfaceType = 4;     // skinned models, from the cache the game's skinning fills
constexpr uint32_t kWorldStride = 44;
constexpr uint32_t kModelStride = 32;
constexpr uint32_t kFirstLitType = 8;  // technique types 8 and up light a surface (0-7: depth, shadow, unlit ones)
constexpr uint32_t kLastLitType = 49;
// Where the model functions call R_DrawIndexedPrimitive after writing the placement's registers (c4-c7) for the GPU
// themselves through 0x8245E1E0 (which leaves Direct3D's own copy of the registers as it was): the return addresses
// of the calls in 0x82449DB0 (rigid models) and 0x8244A818 (static models). At both, r11 points at the four
// registers just written (`mr r11,r3` after the second 0x8245E1E0, at 0x8244A088 and 0x8244AABC).
constexpr uint32_t kRigidModelDrawReturn = 0x8244A0D0u;
constexpr uint32_t kStaticModelDrawReturn = 0x8244AB04u;
// [waw-decomp-waw] 2026-11-11 09:35: three more model functions do the same, read in the generated code (gen.py):
// each sets r4 = 0, r5 = 4 (the first register), r6 = 4 (the count), calls 0x8245E1E0, does `mr r11,r3` and doesn't
// write r11 again before its R_DrawIndexedPrimitive call, as 0x8244A818 does at 0x8244AAA4-0x8244AB00. The draws they
// make were left out (counted "for an unknown placement") until now. 0x82449028, 0x82449308 and 0x824495C8 also call
// R_DrawIndexedPrimitive (returns 0x82449228, 0x824494E8, 0x8244977C) but not 0x8245E1E0 (below).
constexpr std::array<uint32_t, 5> kPlacedModelDrawReturns = {
    kRigidModelDrawReturn, kStaticModelDrawReturn,
    0x82449A58u,   // 0x824497C0: 0x8245E1E0 called at 0x82449A08, mr r11,r3 at 0x82449A10, the draw at 0x82449A54
    0x82449D5Cu,   // 0x82449B50: 0x82449D0C, 0x82449D14, 0x82449D58
    0x8244A7E4u};  // 0x8244A598: 0x8244A794, 0x8244A79C, 0x8244A7E0
// [waw-decomp-waw] 2026-11-11 09:40: the one caller run waw-decomp-waw-side3 logged for the left-out draws (rigid
// models, 16-40 a kept frame): 0x82449028's draw at 0x82449224. It writes the placement less the eye into the source
// state's first matrix (+0x00, four rows as c4-c7 hold them): 0x8243FE40 bumps the matrices' version numbers and
// returns the source unchanged, then `mr r11,r3` and four stvx128 at r11 + 0, 16, 32, 48 (0x824491A0-0x824491B8),
// the source being its r30 (its third argument; it also reads the eye's place at +0x12FC), the same source the draw
// list's run hands every draw (waw_native.cpp). 0x82449308 and 0x824495C8 call 0x8243FE40 too but store elsewhere:
// not read yet.
constexpr uint32_t kSourceMatrixDrawReturn = 0x82449228u;
// [waw-decomp-waw] 2026-11-11 11:00: the world's record, for the check of its draws (WorldCheck). 0x82441B78 (which
// sets the world's stream before a batch's draw) reads the record's address from 0x84A84C20: its vertex buffer is
// the Direct3D record at +0x68, the layers' at +0x90; 0x82441CE0 walks its surfaces from the pointer at +0x30C, 0x4C
// bytes each: +0x04 the first vertex, +0x08 the vertex count (16 bits), +0x0A the triangle count, +0x0C the first
// index. A batch is consecutive surfaces with one first vertex and following indices, drawn with its first surface's
// first index once the stream is set to that first vertex x 44.
constexpr uint32_t kWorldPointer = 0x84A84C20u;
constexpr uint32_t kWorldSurfaces = 0x30Cu, kWorldSurfaceBytes = 0x4Cu, kMaxWorldSurfaces = 65536u;
constexpr uint32_t kWorldIndexBuffer = 0x18u;  // the world's own index buffer record (run r2c1: all its lit draws)
constexpr size_t kMaxMeshDraws = 16000;           // a kept frame's (ngl_vk takes 16384)
constexpr uint32_t kMaxMeshTriangles = 0x20000u;  // one draw's; more is taken as a misread
constexpr uint32_t kPictureWidth = 1280, kPictureHeight = 720;  // the screen the game's picture is shown on
constexpr float kFarDistance = 65536.0f;  // the far plane given to ngl_vk in place of the game's infinite one
constexpr int kSamples = 2;               // draws logged whole for each surface type, over the run
// The world's light is kept in the lightmap texture divided by this (it reaches 2: LightmapFor), and ngl_vk multiplies
// it back. [waw-decomp-waw] 2026-11-11 09:40: no longer a guess, the game's shaders add two maps and don't double.
constexpr float kLightmapScale = 2.0f;
constexpr float kModelAmbient = 0.45f;    // the models' light besides the sun: a stand-in for their light probes
// [waw-decomp-waw] 2026-11-11 round 3: the static and the rigid models' functions write c8-c11 (the place in the model
// lighting texture first) through 0x8245E1E0 just before c4-c7 (0x8244A818: r5 = 8, r6 = 4 at 0x8244AA68, the call
// at 0x8244AA74, then 0x82433908 fills it; c4-c7 at 0x8244AAB4). Both go into the GPU's command stream one after the
// other, so c8 is taken to lie one packet before the placement: 64 bytes of c8-c11 and 16 bytes between the two
// packets' data before the placement's registers (ModelLightAt checks the numbers and falls back). Run
// waw-decomp-waw-r3d1 tried 0x48 and read there the device's c8.z and c8.w (0.50195 1.0) and a 0: c8 is 8 bytes
// earlier.
constexpr uint32_t kStreamLightingCoordsBefore = 0x50;

bool KeptFrame(uint64_t frame) { return frame > 0 && frame % kKeepEvery == 0 && frame <= kKeepLast; }
// [waw-decomp-waw] 2026-11-11 round 5: a frame whose pictures are saved and whose record lines are logged (the check,
// waw_debug_native_side), and a frame drawn at all: those, or every frame when the native picture is the game's
// (waw_native_vulkan, waw_native_window.h). With waw_native_vulkan off the two are the same frames.
bool SavedFrame(uint64_t frame) { return REXCVAR_GET(waw_debug_native_side) && KeptFrame(frame); }
bool DrawnFrame(uint64_t frame) { return SavedFrame(frame) || NativeVulkanOn(); }

// One 3D draw kept for ngl_vk.
struct MeshDraw {
  uint32_t surface_type = 0, technique_type = 0;
  uint32_t vertices = 0;       // the physical address of the draw's vertex 0
  uint32_t stride = 0;         // 44: the world's vertices; 32: the models' packed ones
  std::vector<uint32_t> used;  // the vertices its triangles use, three a triangle
  uint64_t key = 0;            // its geometry's fingerprint: the vertices' address and those indices
  uint64_t shape_key = 0;      // [waw-decomp-waw] 2026-11-11 round 5: the stride and the indices alone (SkinnedMeshFor)
  uint32_t material = 0, pass = 0;
  uint32_t state_bits = 0;     // the pass's first state word (IW3's GFXS0: blend, alpha test, cull)
  // the textures' fetch constants as the device has them at the draw (first word 0: none)
  std::array<uint32_t, 6> color_fetch{}, lightmap_fetch{}, sun_map_fetch{};
  float world[16] = {}, view_projection[16] = {};  // Direct3D's row-vector matrices, row by row
  bool sunlit = false;         // its pass takes the sun (code constants 0x24 and 0x25)
  float sun_direction[3] = {}, sun_color[3] = {};  // toward the sun, and its color
  float fog_color[3] = {};
  // [waw-decomp-waw] 2026-11-11 round 3: a model's light, its block of the model lighting texture (ModelLightAt): the
  // eight texels around its place, red, green, blue and alpha (0 to 1), texel k at the high side of axis a when bit a
  // of k is set; model_light false when it couldn't be read (the flat stand-in then)
  bool model_light = false;
  float light_texels[8][4] = {};
  // [waw-decomp-waw] 2026-11-11 round 3: a layered world draw's second layer (LayerOf): its color map's fetch constant,
  // and where each corner's second coordinates are (stream 1: the corner's number x layer_stride + layer_uv_offset)
  bool layer = false;
  std::array<uint32_t, 6> layer_fetch{};
  uint32_t layer_vertices = 0, layer_stride = 0, layer_uv_offset = 0;
  // [waw-decomp-waw] 2026-11-11 round 4: the layers' tints (the material's colorTint and colorTint1, 1 1 1 1 when it
  // has none) and whether the first layer is cut where its alpha is under one half (a "b0c0" technique; "r0c0" ones
  // replace, uncut)
  float tint[4] = {1.0f, 1.0f, 1.0f, 1.0f}, layer_tint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
  bool layer_cut = false;
  // [waw-decomp-waw] 2026-11-11 round 4: the fog's numbers (code constant 0x2A, vertex c21 in the world's passes;
  // FogOf), when the pass takes them
  bool fogged = false;
  float fog_consts[4] = {};
  // [waw-decomp-waw] 2026-11-11 round 4: the sky's model (a "sky" technique: pel1a's "sky_noncubemap_m", surface type
  // 3, technique type 10, run waw-decomp-waw-r4b1), drawn unlit; its farthest corner from its middle (its own units),
  // and whether its pass names no view-projection (then the first world draw's is taken)
  bool sky = false, sky_view_missing = false;
  float sky_radius = 0.0f;
};

// [waw-decomp-waw] 2026-11-11 round 3: how the models' light was found in a kept frame (record only).
struct ModelLightCounts {
  uint32_t from_stream = 0, from_device = 0;  // where the place in the texture was read
  uint32_t no_sampler = 0, no_texture = 0, not_volume = 0, outside = 0;
  uint32_t found_by_search = 0;  // the sampler or the place found by the fallbacks (ModelLightAt), not by number
  uint32_t layered = 0;  // the world's draws of declaration types 3, 4 and 7 (two color maps; LogLayer)
  // [waw-decomp-waw] 2026-11-11 round 4: of those, with a second layer found (LayerOf), with a tint not 1 1 1 1, and
  // with the first layer cut ("b0c0")
  uint32_t layers_found = 0, tinted = 0, cut = 0;
};

// [waw-decomp-waw] 2026-11-11 11:00: what the check of the world's draws (WorldCheck) found in a kept frame: record
// only, logged at the frame's end. Run waw-decomp-waw-cand3 (the world alone) drew long stretched triangles across
// the picture with holes; these numbers showed why (FirstIndexAddress) and keep showing that it holds.
struct WorldCheckCounts {
  uint32_t draws = 0, brush_draws = 0, wide = 0;
  uint64_t triangles = 0;
  uint32_t unaligned = 0;  // draws whose index buffer's address is not 4-byte aligned
  uint32_t own_buffer = 0, other_buffer = 0, first_other_buffer = 0;
  uint64_t degenerate_before = 0, degenerate_now = 0;  // triangles with two corners alike, read as before and now
  uint32_t offset_same = 0, offset_differs = 0, surface_not_found = 0;
  uint32_t stream1 = 0, stream2 = 0;
  std::map<uint32_t, uint32_t> declaration_types;
};

// [waw-decomp-waw] 2026-11-11 round 5: one corner of a kept 2D draw (Keep2D), and the draw.
struct Corner2D {
  float x = 0.0f, y = 0.0f;  // the screen's pixels
  float u = 0.0f, v = 0.0f;
  uint32_t color = 0;        // a D3DCOLOR, 0xAARRGGBB
};
struct Draw2D {
  uint32_t first_corner = 0, corner_count = 0;  // into Frame::corners_2d
  uint32_t material = 0;
  std::array<uint32_t, 6> fetch{};  // its color map's fetch constant (first word 0: none)
  bool opaque = false;              // its pass doesn't blend (source blend off): the texture's alpha isn't used
};

struct Frame {
  std::vector<MeshDraw> meshes;
  uint32_t dropped = 0;      // draws not kept (too many, or a misread)
  uint32_t no_matrices = 0;  // draws whose pass names no view-projection or placement
  uint32_t direct = 0;       // model draws whose placement was read where their function wrote it
  uint32_t stale = 0;        // static and rigid model draws from another caller: no placement known, left out
  std::map<uint32_t, uint32_t> stale_callers;  // those draws by their return address [waw-decomp-waw] 2026-11-11
  // (0 indexed / 1 from the tess buffers, surface type, technique type) -> {draws, triangles}
  std::map<std::tuple<uint32_t, uint32_t, uint32_t>, std::pair<uint32_t, uint64_t>> kinds;
  WorldCheckCounts world_check;  // [waw-decomp-waw] 2026-11-11 11:00
  ModelLightCounts model_light;  // [waw-decomp-waw] 2026-11-11 round 3
  // [waw-decomp-waw] 2026-11-11 round 4: the sky's cube (KeepSky): its color map's fetch constant, when it is a cube
  bool sky = false;
  std::array<uint32_t, 6> sky_fetch{};
  // [waw-decomp-waw] 2026-11-11 round 5: the 2D for the window's picture (Keep2D): the kept draws and their corners,
  // the screen as the frame's whole-screen quads give its size (0: none seen), the "feedbackblend" quads' largest
  // alpha (0 to 1) and the draws left out, by why
  std::vector<Corner2D> corners_2d;
  std::vector<Draw2D> draws_2d;
  float screen_width = 0.0f, screen_height = 0.0f;
  float blur = 0.0f;
  uint32_t left_gpu_made = 0, left_additive = 0, left_no_color = 0, left_unread = 0;

  void Clear() {
    meshes.clear();
    dropped = no_matrices = direct = stale = 0;
    stale_callers.clear();
    kinds.clear();
    world_check = WorldCheckCounts{};
    model_light = ModelLightCounts{};
    sky = false;
    sky_fetch.fill(0);
    corners_2d.clear();  // [waw-decomp-waw] 2026-11-11 round 5
    draws_2d.clear();
    screen_width = screen_height = blur = 0.0f;
    left_gpu_made = left_additive = left_no_color = left_unread = 0;
  }
};

// The draws and the swap run on one thread at a time (the back end's; at the start the movies' and the loading
// screen's threads swap too), so everything here is under one lock, and a frame's draws count only on the thread that
// swapped last.
std::mutex g_side_mutex;
Frame g_frame;
uint64_t g_frames = 0;         // swaps so far
std::thread::id g_swap_thread;  // the thread of the last swap
int g_samples[kSurfaceTypes] = {};  // draws logged whole, by surface type

// The texture in a sampler slot as the device has it: its six-word fetch constant (device + 0x480 + 24 x slot, where
// Direct3D's own 0x823615A8 sends them to the GPU from); false when the slot holds no texture.
bool DeviceTexture(const uint8_t* base, uint32_t device, uint32_t slot, std::array<uint32_t, 6>& fetch) {
  for (uint32_t w = 0; w < 6; ++w) fetch[w] = Load32(base, device + kDeviceFetchConstants + 24u * slot + 4u * w);
  if ((fetch[0] & 3u) != 2u || (fetch[1] & 0x1FFFF000u) == 0) {
    fetch.fill(0);
    return false;
  }
  return true;
}

// A pass's arguments: `visit(type, destination, second word)` for each.
template <typename Visit>
void ForEachArgument(const uint8_t* base, uint32_t pass, Visit visit) {
  const uint32_t args = Load32(base, pass + kPassArgs);
  const uint32_t count = uint32_t(base[pass + kPassPerPrimArgs]) + base[pass + kPassPerPrimArgs + 1] +
                         base[pass + kPassPerPrimArgs + 2];
  if (!Readable(args)) return;
  for (uint32_t i = 0; i < count && i < 96; ++i) {
    const uint32_t word0 = Load32(base, args + 8u * i);
    visit(word0 >> 16, word0 & 0xFFFFu, Load32(base, args + 8u * i + 4u));
  }
}

// The register a pass's arguments give a code constant (type 3: a vertex shader's, 5: a pixel shader's), or -1.
int RegisterOf(const uint8_t* base, uint32_t pass, uint32_t type, uint32_t code_constant) {
  int found = -1;
  ForEachArgument(base, pass, [&](uint32_t arg_type, uint32_t destination, uint32_t word1) {
    if (found < 0 && arg_type == type && (word1 >> 16) == code_constant && destination < 252)
      found = static_cast<int>(destination);
  });
  return found;
}

// The sampler slot of the material's color map in a pass: the material sampler argument named "colorMap", else the
// first one whose texture has the color map's semantic in the material's table; -1 when the pass reads none.
int ColorMapSlot(const uint8_t* base, uint32_t material, uint32_t pass) {
  int named = -1, by_semantic = -1;
  const uint32_t count = base[material + kMaterialTextureCount];
  const uint32_t table = Load32(base, material + kMaterialTextures);
  ForEachArgument(base, pass, [&](uint32_t arg_type, uint32_t destination, uint32_t hash) {
    if (arg_type != kArgMaterialSampler || destination >= 16) return;
    if (hash == kHashColorMap && named < 0) named = static_cast<int>(destination);
    if (by_semantic >= 0 || !Readable(table)) return;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t entry = table + kMaterialTextureBytes * i;
      if (Load32(base, entry) == hash && base[entry + 7] == kSemanticColorMap) by_semantic = int(destination);
    }
  });
  return named >= 0 ? named : by_semantic;
}

// Four vertex shader registers at a guest address as Direct3D's row-vector matrix: entry [i][r] = c(r)[i].
void MatrixFrom(const uint8_t* base, uint32_t registers, float out[16]) {
  for (uint32_t r = 0; r < 4; ++r)
    for (uint32_t i = 0; i < 4; ++i) out[4 * i + r] = LoadF32(base, registers + 16u * r + 4u * i);
}

// The game's infinite, turned-over depth made a usual one (see the top of the file): when the z column is a constant
// (the near distance), it becomes the w column x far / (far - near), less near x far / (far - near). The view
// model's draws (the player's own weapon and arms) come with a near distance of 0.1 where the scene's is 4, and the
// game draws them in a depth range of their own, in front of everything: here their depth is pressed into the
// nearest sixty-fourth.
void UsualDepth(float view_projection[16]) {
  const float near_distance = view_projection[14];
  if (view_projection[2] != 0.0f || view_projection[6] != 0.0f || view_projection[10] != 0.0f ||
      !(near_distance > 0.0f) || near_distance >= kFarDistance)
    return;
  float scale = kFarDistance / (kFarDistance - near_distance);
  float offset = -near_distance * scale;
  if (near_distance < 1.0f) {
    scale /= 64.0f;
    offset /= 64.0f;
  }
  for (int i = 0; i < 4; ++i) view_projection[4 * i + 2] = view_projection[4 * i + 3] * scale;
  view_projection[14] += offset;
}

bool SaneMatrix(const float m[16]) {
  for (int i = 0; i < 16; ++i)
    if (!std::isfinite(m[i]) || std::fabs(m[i]) > 1e7f) return false;
  return true;
}

// A placement as the game makes them: three rows of a turn times a scale (0.01 to 100), then a move.
bool SanePlacement(const float m[16]) {
  for (int row = 0; row < 3; ++row) {
    const float length = std::sqrt(m[4 * row] * m[4 * row] + m[4 * row + 1] * m[4 * row + 1] +
                                   m[4 * row + 2] * m[4 * row + 2]);
    if (!(length > 0.01f && length < 100.0f) || m[4 * row + 3] != 0.0f) return false;
  }
  return m[15] == 1.0f;
}

// [waw-decomp-waw] 2026-11-11 11:05: where a draw's first index is. An index buffer's address word (+0x18) is the
// indices' physical address itself, and 16-bit indices need only be 2-byte aligned. Run waw-decomp-waw-r2c1 (pel1a,
// frames 4200 and 4800) found the world's own index buffer (world + 0x18; 84E4AB28, address word AD19F362) in all
// 295 of its lit draws, the models' buffers 4-byte aligned in all 1,779 of theirs. Until then the low two bits were
// taken for a byte order and dropped, so the world's draws began one index early and every triangle took the one
// before's last corner (as read: 11482 0 1 | 2 0 2 ...; from the right address: 0 1 2 | 0 2 3 ...): the long
// stretched triangles and the holes of run cand3. The indices themselves are big-endian, as before.
uint32_t FirstIndexAddress(uint32_t address_word, bool wide, uint32_t first_index) {
  return (address_word & (wide ? 0x1FFFFFFCu : 0x1FFFFFFEu)) + (wide ? 4u : 2u) * first_index;
}

// The world's surfaces' first vertex by their first index (WorldCheck), made once for each world record. The array's
// length is not read yet: the walk stops at kMaxWorldSurfaces or unreadable memory, and the first surface with an
// index wins (what lies past the array's end comes later).
uint32_t g_surface_map_world = 0;
std::unordered_map<uint32_t, uint32_t> g_surface_first_vertex;

void MapWorldSurfaces(const uint8_t* base, uint32_t world) {
  if (world == g_surface_map_world) return;
  g_surface_map_world = world;
  g_surface_first_vertex.clear();
  const uint32_t surfaces = Load32(base, world + kWorldSurfaces);
  if (!Readable(surfaces)) return;
  for (uint32_t i = 0; i < kMaxWorldSurfaces; ++i) {
    const uint32_t s = surfaces + kWorldSurfaceBytes * i;
    if (!Readable(s + kWorldSurfaceBytes)) break;
    g_surface_first_vertex.emplace(Load32(base, s + 0x0Cu), Load32(base, s + 0x04u));
  }
}

// [waw-decomp-waw] 2026-11-11 11:00: the check of one lit draw of the world's vertices (surface types 0 and 2), record
// only: which index buffer it uses and whether its address is 4-byte aligned, its declaration type and streams
// (0x82441B78 sets stream 0 alone through 0x82433038, or with the layers' second stream and a third one through
// 0x82441AA0 / 0x82433118), whether the stream's offset is the batch's first surface's first vertex x 44 (found by the
// draw's first index), and how many of its triangles have two corners alike when read as before 11:05 and now.
void WorldCheck(const uint8_t* base, uint32_t prim, uint32_t index_buffer, uint32_t offset, uint32_t first_index,
                uint32_t triangles, uint32_t surface_type, Frame& f) {
  WorldCheckCounts& c = f.world_check;
  const uint32_t address_word = Load32(base, index_buffer + kIndexBufferAddress);
  ++(surface_type == kWorldSurfaceType ? c.draws : c.brush_draws);
  c.triangles += triangles;
  c.unaligned += (address_word & 3u) != 0;
  const uint32_t world = Load32(base, kWorldPointer);
  if (Readable(world) && index_buffer == world + kWorldIndexBuffer) {
    ++c.own_buffer;
  } else if (c.other_buffer++ == 0) {
    c.first_other_buffer = index_buffer;
  }
  ++c.declaration_types[Load32(base, prim + kPrimDeclType)];
  const uint32_t stream1 = prim + kPrimStreams + kPrimStreamBytes;
  c.stream1 += Load32(base, stream1 + 4u) != 0;
  c.stream2 += Load32(base, stream1 + kPrimStreamBytes + 4u) != 0;
  if (surface_type == kWorldSurfaceType && Readable(world)) {
    MapWorldSurfaces(base, world);
    if (auto found = g_surface_first_vertex.find(first_index); found != g_surface_first_vertex.end()) {
      ++(offset == found->second * kWorldStride ? c.offset_same : c.offset_differs);
    } else {
      ++c.surface_not_found;
    }
  }
  if ((Load32(base, index_buffer) >> 31) != 0) {  // 32-bit indices: none seen in the world's draws
    ++c.wide;
    return;
  }
  const uint32_t before = (address_word & 0x1FFFFFFCu) + 2u * first_index;
  const uint32_t now = FirstIndexAddress(address_word, false, first_index);
  auto degenerate = [&](uint32_t at) {
    const uint32_t a = GpuU16(base, at), b = GpuU16(base, at + 2u), d = GpuU16(base, at + 4u);
    return a == b || b == d || a == d;
  };
  for (uint32_t t = 0; t < triangles; ++t) {
    c.degenerate_before += degenerate(before + 6u * t);
    c.degenerate_now += degenerate(now + 6u * t);
  }
}

// [waw-decomp-waw] 2026-11-11 round 3: the Xenos 3D tiling (tiles of 32 x 32 x 4 texels): texel (x, y, z)'s byte offset
// from the texture's start, pitch and height in texels (rounded up to 32 here), log2_bytes the texel's size (2: 4
// bytes). Call of Duty 2's TiledOffset3D (recomp_cod2/src/game/native_side.cpp, after the SDK's
// texture_util::GetTiledOffset3D), the same steps.
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

// [waw-decomp-waw] 2026-11-11 round 3: a model's light, as the game's lit model pixel shaders take it. Read in the mod
// tools' compiled PC shaders (raw/shader_bin, d3d9dis.py on the PC): "lp_sun"-kind ps_3_0_022bfc47 (s4
// modelLightingSampler, c5 lightingLookupScale, c17 sunPosition, c18 sunDiffuse) does
//     n = normalize(normal); L = tex3D(s4, v3 + n / max(|n.x|, |n.y|, |n.z|) x lightingLookupScale)
//     light = 2 x L.rgb + L.a x saturate(dot(sunPosition, n)) x sunDiffuse; pixel = color map x corner color x light
// with v3 the model's place in the texture (vertex c8, baseLightingCoords; code constant 0x3B); the "lprobe" kind
// without a normal (ps_3_0_00d0a0a3) reads it at the place alone. As Call of Duty 2's static models' light volume
// (its VolumeLightAt), each model is taken to own a 2 x 2 x 2 block whose middle is its place, the lookup reaching
// the eight texels' centers along the eight diagonals: the light from eight directions, red, green and blue, and in
// alpha the share of the sun that reaches the model from there. The texture is "$model_lighting" (code texture 3, a
// 3D 8_8_8_8 one the game fills on the CPU: R_AllocModelLightingGlobal 0x82434018 makes its tiled layout's table).
// Its place: for the five callers that write the placement into the command stream (kPlacedModelDrawReturns), one
// packet before it (kStreamLightingCoordsBefore), else Direct3D's copy of the vertex register; a place outside 0..1
// is not taken. False (and counted) when the block can't be read: the flat stand-in then.
bool ModelLightAt(const uint8_t* base, uint32_t device, uint32_t pass, uint32_t stream_registers, MeshDraw& m,
                  ModelLightCounts& counts) {
  // a code sampler argument holds its number in the low half of its second word (run waw-decomp-waw-r3d1's log of an
  // "lp_amb_sun" pass: 4/4/00000003, the model lighting texture in slot 4; code constants hold theirs in the high
  // half: 3/8/003B0001, the place in c8)
  int slot = -1;
  ForEachArgument(base, pass, [&](uint32_t arg_type, uint32_t destination, uint32_t word1) {
    if (slot < 0 && arg_type == kArgCodeSampler && (word1 & 0xFFFFu) == kCodeTextureModelLighting && destination < 16)
      slot = static_cast<int>(destination);
  });
  int place_register = RegisterOf(base, pass, kArgCodeVertexConst, kCodeBaseLightingCoords);
  // run waw-decomp-waw-r3c1 found neither (it read the sampler's number from the high half): the pass's arguments are
  // logged once when one is missing, and the texture is then taken from the first sampler argument whose texture is a
  // 3D one (the model lighting texture is the only 3D texture a lit model reads), the place from c8
  const bool by_number = slot >= 0 && place_register >= 0;
  static bool logged_arguments = false;
  if (!by_number && !logged_arguments) {
    logged_arguments = true;
    std::string list;
    ForEachArgument(base, pass, [&](uint32_t arg_type, uint32_t destination, uint32_t word1) {
      char text[32];
      std::snprintf(text, sizeof text, " %u/%u/%08X", arg_type, destination, word1);
      list += text;
    });
    REXLOG_INFO("waw: native side: a model pass's arguments (type/destination/second word), ps \"{}\":{}",
                NameOf(base, Load32(base, pass + kPassPixelShader)), list.empty() ? " none" : list);
  }
  if (slot < 0) {
    ForEachArgument(base, pass, [&](uint32_t arg_type, uint32_t destination, uint32_t) {
      std::array<uint32_t, 6> found{};
      if (slot < 0 && (arg_type == kArgCodeSampler || arg_type == kArgMaterialSampler) && destination < 16 &&
          DeviceTexture(base, device, destination, found) && ((found[5] >> 9) & 3u) == 2)
        slot = static_cast<int>(destination);
    });
  }
  if (std::array<uint32_t, 6> found{};  // and else slot 4, where the capture saw it
      slot < 0 && DeviceTexture(base, device, 4, found) && ((found[5] >> 9) & 3u) == 2)
    slot = 4;
  if (place_register < 0) place_register = 8;
  counts.found_by_search += !by_number;
  std::array<uint32_t, 6> fetch{};
  if (slot < 0 || slot >= 16) {
    ++counts.no_sampler;
    return false;
  }
  if (!DeviceTexture(base, device, uint32_t(slot), fetch)) {
    ++counts.no_texture;
    return false;
  }
  const int32_t size[3] = {static_cast<int32_t>(fetch[2] & 0x7FFu) + 1,
                           static_cast<int32_t>((fetch[2] >> 11) & 0x7FFu) + 1,
                           static_cast<int32_t>((fetch[2] >> 22) & 0x3FFu) + 1};
  // a 3D texture (dimension, word 5 bits 9-10: 2), 8_8_8_8 (format 6) read as 0xAARRGGBB (endian 2), as Call of
  // Duty 2's light volume
  const bool volume = ((fetch[5] >> 9) & 3u) == 2 && (fetch[1] & 0x3Fu) == 6 && ((fetch[1] >> 6) & 3u) == 2;
  static bool logged_texture = false;
  if (!logged_texture) {
    logged_texture = true;
    REXLOG_INFO("waw: native side: the model lighting texture (slot {}): fetch {:08X} {:08X} {:08X} {:08X} {:08X} "
                "{:08X}: {}x{}x{}, format {}, endian {}, {}, dimension {}; {}", slot, fetch[0], fetch[1], fetch[2],
                fetch[3], fetch[4], fetch[5], size[0], size[1], size[2], fetch[1] & 0x3Fu, (fetch[1] >> 6) & 3u,
                (fetch[0] >> 31) ? "tiled" : "linear", (fetch[5] >> 9) & 3u, volume ? "read" : "not read (not as "
                "expected)");
  }
  if (!volume) {
    ++counts.not_volume;
    return false;
  }
  auto inside = [](const float p[4]) {
    return p[0] > 0.0f && p[0] < 1.0f && p[1] > 0.0f && p[1] < 1.0f && p[2] > 0.0f && p[2] < 1.0f;
  };
  float stream_place[4] = {}, device_place[4] = {};
  const bool have_stream = Readable(stream_registers) && Readable(stream_registers - kStreamLightingCoordsBefore);
  for (uint32_t k = 0; k < 4; ++k) {
    if (have_stream)
      stream_place[k] = LoadF32(base, stream_registers - kStreamLightingCoordsBefore + 4u * k);
    device_place[k] = LoadF32(base, device + kDeviceVertexConstants + 16u * uint32_t(place_register) + 4u * k);
  }
  const bool by_stream = have_stream && inside(stream_place);
  const float* place = by_stream ? stream_place : device_place;
  if (!by_stream && !inside(device_place)) {
    ++counts.outside;
    return false;
  }
  ++(by_stream ? counts.from_stream : counts.from_device);
  // the eight texels whose centers are around the place (a block's middle lies on texel borders)
  const uint32_t pitch = std::max(((fetch[0] >> 22) & 0x1FFu) * 32u, static_cast<uint32_t>(size[0]));
  const uint32_t at = fetch[1] & 0x1FFFF000u;
  const bool tiled = (fetch[0] >> 31) != 0;
  int32_t low[3];
  float part[3];
  for (int k = 0; k < 3; ++k) {
    const float texel = place[k] * static_cast<float>(size[k]) - 0.5f;
    low[k] = static_cast<int32_t>(std::floor(texel));
    part[k] = texel - static_cast<float>(low[k]);
  }
  uint32_t words[8] = {};
  for (int corner = 0; corner < 8; ++corner) {
    int32_t xyz[3];
    for (int k = 0; k < 3; ++k) xyz[k] = std::clamp(low[k] + ((corner >> k) & 1), 0, size[k] - 1);
    const int32_t offset = tiled ? TiledOffset3D(xyz[0], xyz[1], xyz[2], pitch, uint32_t(size[1]), 2)
                                 : ((xyz[2] * size[1] + xyz[1]) * static_cast<int32_t>(pitch) + xyz[0]) * 4;
    words[corner] = GpuU32(base, at + static_cast<uint32_t>(offset));
    m.light_texels[corner][0] = static_cast<float>((words[corner] >> 16) & 0xFFu) / 255.0f;
    m.light_texels[corner][1] = static_cast<float>((words[corner] >> 8) & 0xFFu) / 255.0f;
    m.light_texels[corner][2] = static_cast<float>(words[corner] & 0xFFu) / 255.0f;
    m.light_texels[corner][3] = static_cast<float>(words[corner] >> 24) / 255.0f;
  }
  static int logged = 0;
  if (logged < 6) {  // the first few, to check the place and the block
    ++logged;
    REXLOG_INFO("waw: native side: a model's light: place {:.5f} {:.5f} {:.5f} {:.5f} from the {} (stream copy {:.5f} "
                "{:.5f} {:.5f}, device c{} {:.5f} {:.5f} {:.5f}); texel parts {:.2f} {:.2f} {:.2f}; texels {:08X} "
                "{:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}", place[0], place[1], place[2], place[3],
                by_stream ? "command stream" : "device", stream_place[0], stream_place[1], stream_place[2],
                place_register, device_place[0], device_place[1], device_place[2], part[0], part[1], part[2],
                words[0], words[1], words[2], words[3], words[4], words[5], words[6], words[7]);
  }
  return true;
}

// [waw-decomp-waw] 2026-11-11 round 3: the world's layered materials (declaration types 3, 4 and 7: two color maps, the
// second's coordinates through stream 1, 12 bytes a vertex; 77 draws a frame in run r2c1). Record only, the first few
// over the run: the Direct3D declaration's elements (13 words of header, then 12 bytes an element: stream and
// offset, type, method and usage), stream 1, the first corner's words there and its color, and the material's
// samplers. The PC's "lm_b0c0b1c1" pixel shader (ps_3_0_050b38e1) mixes them: layer 0 = colorMap x colorTint where
// its alpha x v2.x >= 1/2 (else black), then lerp(layer 0, colorMapSampler1 at v2.zw x colorTint1, its alpha x
// v2.y), times the lightmaps' light and the sun as "lm_sun". What v2 is made of (the corners' weights and second
// coordinates) is what this log is for.
void LogLayer(const uint8_t* base, uint32_t state, const MeshDraw& m) {
  static int logged = 0;
  if (logged >= 3) return;
  ++logged;
  const uint32_t prim = state + kStatePrim;
  const uint32_t type = Load32(base, prim + kPrimDeclType);
  const uint32_t declarations = Load32(base, m.pass + kPassDeclaration);
  const uint32_t declaration = Readable(declarations) ? Load32(base, declarations + 4u + 4u * (type & 15u)) : 0;
  std::string elements;
  for (uint32_t i = 0; i < 12 && Readable(declaration); ++i) {
    const uint32_t e = declaration + 13u * 4u + 12u * i;
    const uint32_t word0 = Load32(base, e);
    if ((word0 >> 16) == 0xFFu || (word0 >> 16) > 15u) break;  // the list's end
    char text[48];
    std::snprintf(text, sizeof text, " s%u+%u:%06X/%08X", word0 >> 16, word0 & 0xFFFFu, Load32(base, e + 4u),
                  Load32(base, e + 8u));
    elements += text;
  }
  const uint32_t stream1 = prim + kPrimStreams + kPrimStreamBytes;
  const uint32_t stride1 = Load32(base, stream1), buffer1 = Load32(base, stream1 + 4u);
  const uint32_t offset1 = Load32(base, stream1 + 8u);
  std::string words;
  if (Readable(buffer1) && stride1 > 0 && stride1 <= 64 && !m.used.empty()) {
    const uint32_t at = (Load32(base, buffer1 + kVertexBufferFetch) & 0x1FFFFFFCu) + offset1 + m.used[0] * stride1;
    for (uint32_t k = 0; k < stride1 / 4; ++k) {
      char word[12];
      std::snprintf(word, sizeof word, " %08X", GpuU32(base, at + 4u * k));
      words += word;
    }
  }
  std::string samplers;
  const uint32_t count = base[m.material + kMaterialTextureCount];
  const uint32_t table = Load32(base, m.material + kMaterialTextures);
  ForEachArgument(base, m.pass, [&](uint32_t arg_type, uint32_t destination, uint32_t hash) {
    if (arg_type != kArgMaterialSampler) return;
    uint32_t semantic = 255;
    for (uint32_t i = 0; i < count && Readable(table); ++i)
      if (const uint32_t entry = table + kMaterialTextureBytes * i; Load32(base, entry) == hash)
        semantic = base[entry + 7];
    char text[40];
    std::snprintf(text, sizeof text, " slot %u %08X (semantic %u)", destination, hash, semantic);
    samplers += text;
  });
  REXLOG_INFO("waw: native side: a layered world draw: declaration type {}, material \"{}\", technique \"{}\", vs "
              "\"{}\" ps \"{}\"; elements:{}; stream 1 stride {} at {:08X} + {}, corner {}:{}; its color {:08X}; "
              "samplers:{}",
              type, NameOf(base, m.material), NameOf(base, Load32(base, state + kStateTechnique)),
              NameOf(base, Load32(base, m.pass + kPassVertexShaders + 4u * (type & 15u))),
              NameOf(base, Load32(base, m.pass + kPassPixelShader)), elements.empty() ? " none" : elements, stride1,
              buffer1, offset1, m.used.empty() ? 0u : m.used[0], words.empty() ? " none" : words,
              m.used.empty() ? 0u : GpuU32(base, m.vertices + m.used[0] * m.stride + 0x10u),
              samplers.empty() ? " none" : samplers);
}

// [waw-decomp-waw] 2026-11-11 round 4: a material constant's four numbers by its name, from the material's constant
// table (kMaterialConstants: 32 bytes an entry, the name's hash, the name's first 12 characters, four floats): the
// literal the game hands the pixel shader for a material constant argument (type 6). False, out untouched, when the
// material has none of that name or its numbers are not plausible (outside -64..64).
bool MaterialConstant(const uint8_t* base, uint32_t material, const char* name, float out[4]) {
  const uint32_t count = base[material + kMaterialConstantCount];
  const uint32_t table = Load32(base, material + kMaterialConstants);
  for (uint32_t i = 0; i < count && Readable(table) && Readable(table + 32u * i + 32u); ++i) {
    const uint32_t entry = table + 32u * i;
    char text[13] = {};
    std::memcpy(text, base + entry + 4u, 12);
    if (std::strcmp(text, name) != 0) continue;
    float value[4];
    for (uint32_t k = 0; k < 4; ++k) {
      value[k] = LoadF32(base, entry + 0x10u + 4u * k);
      if (!std::isfinite(value[k]) || std::fabs(value[k]) > 64.0f) return false;
    }
    std::memcpy(out, value, sizeof value);
    return true;
  }
  return false;
}

// [waw-decomp-waw] 2026-11-11 round 3: a layered world draw's second layer, for drawing it again over the first
// (SetMeshes, LayerMeshFor). The PC's layered vertex shader (vs_3_0_1bf3bd57, one of the 1,758 that take COLOR0,
// TEXCOORD3 and TEXCOORD5) passes the pixel shader v2 = (the corner color's red and green, TEXCOORD3's two numbers):
// `mov o4.xy, v1` (COLOR0) and `mov o4.zw, v4.xy` (TEXCOORD3), o4 being TEXCOORD6, the pixel shader's v2. So the
// second layer is the material's other color map (a sampler argument whose texture has the color map's semantic, not
// named colorMap: B60D1850 in slot 4 in run r3c1) read at stream 1's TEXCOORD3, mixed in by the corner's green
// (times its own alpha, which ngl_vk's corner_alpha leaves out). Not drawn: the first layer's cut where its alpha x
// the corner's red is under 1/2 (black there before the mix). False when the draw has no such layer.
bool LayerOf(const uint8_t* base, uint32_t state, uint32_t device, MeshDraw& m) {
  const uint32_t prim = state + kStatePrim;
  const uint32_t type = Load32(base, prim + kPrimDeclType);
  const uint32_t declarations = Load32(base, m.pass + kPassDeclaration);
  const uint32_t declaration = Readable(declarations) ? Load32(base, declarations + 4u + 4u * (type & 15u)) : 0;
  int uv_offset = -1;  // TEXCOORD3 in stream 1: an element {stream << 16 | offset, type, method usage index ...}
  for (uint32_t i = 0; i < 12 && Readable(declaration) && uv_offset < 0; ++i) {
    const uint32_t e = declaration + 13u * 4u + 12u * i;
    const uint32_t word0 = Load32(base, e), word2 = Load32(base, e + 8u);
    if ((word0 >> 16) > 15u) break;  // the list's end
    if ((word0 >> 16) == 1u && ((word2 >> 16) & 0xFFu) == 5u && ((word2 >> 8) & 0xFFu) == 3u)
      uv_offset = static_cast<int>(word0 & 0xFFFFu);
  }
  const uint32_t stream1 = prim + kPrimStreams + kPrimStreamBytes;
  const uint32_t stride1 = Load32(base, stream1), buffer1 = Load32(base, stream1 + 4u);
  if (uv_offset < 0 || stride1 == 0 || stride1 > 64 || !Readable(buffer1)) return false;
  const uint32_t fetch1 = Load32(base, buffer1 + kVertexBufferFetch);
  if ((fetch1 & 3u) != 3u) return false;
  int slot = -1;
  const uint32_t count = base[m.material + kMaterialTextureCount];
  const uint32_t table = Load32(base, m.material + kMaterialTextures);
  ForEachArgument(base, m.pass, [&](uint32_t arg_type, uint32_t destination, uint32_t hash) {
    if (slot >= 0 || arg_type != kArgMaterialSampler || hash == kHashColorMap || destination >= 16) return;
    for (uint32_t i = 0; i < count && Readable(table); ++i)
      if (const uint32_t entry = table + kMaterialTextureBytes * i;
          Load32(base, entry) == hash && base[entry + 7] == kSemanticColorMap)
        slot = static_cast<int>(destination);
  });
  if (slot < 0 || !DeviceTexture(base, device, uint32_t(slot), m.layer_fetch)) return false;
  m.layer = true;
  m.layer_vertices = (fetch1 & 0x1FFFFFFCu) + Load32(base, stream1 + 8u);
  m.layer_stride = stride1;
  m.layer_uv_offset = static_cast<uint32_t>(uv_offset);
  // [waw-decomp-waw] 2026-11-11 round 4: the two tints (the PC's lm_b0c0b1c1, ps_3_0_050b38e1: c5 colorTint times the
  // first color map, c6 colorTint1 times the second) and the first layer's cut, which only the "b0c0" techniques make
  // (b: blended, cut where the first map's alpha x the tint's x the corner's red is under 1/2; r: replaced, uncut; run
  // r3c1's layered draws were all "lm_r0c0n0s0b1c1n1s1_nc_w_sm3_sco")
  MaterialConstant(base, m.material, "colorTint", m.tint);
  MaterialConstant(base, m.material, "colorTint1", m.layer_tint);
  m.layer_cut = NameOf(base, Load32(base, state + kStateTechnique)).find("b0c0") != std::string::npos;
  return true;
}

// [waw-decomp-waw] 2026-11-11 round 4: record only, once: the sky pass's arguments (type/destination/its word), to
// see which matrices its vertex shader ("sky_noncubemap.hlsl*packed" in pel1a) takes.
void LogSkyArguments(const uint8_t* base, uint32_t pass) {
  static bool logged = false;
  if (logged) return;
  logged = true;
  std::string arguments;
  ForEachArgument(base, pass, [&](uint32_t type, uint32_t destination, uint32_t word) {
    char text[32];
    std::snprintf(text, sizeof text, " %u/%u/%08X", type, destination, word);
    arguments += text;
  });
  REXLOG_INFO("waw: native side: the sky's pass arguments (type/register/word):{}", arguments);
}

// Keeps one lit draw of a kept frame: the world's (44-byte vertices) or a model's (32-byte packed ones).
// direct_world: where the caller wrote the placement's four registers itself (static and rigid models), or 0.
// stream_registers: the same when it is in the command stream (kPlacedModelDrawReturns), for ModelLightAt, or 0.
// saved: a saved frame's draw (SavedFrame), whose record-only check (WorldCheck) is done. [waw-decomp-waw] round 5
void KeepMesh(const uint8_t* base, uint32_t state, uint32_t arguments, uint32_t surface_type,
              uint32_t technique_type, uint32_t direct_world, uint32_t stream_registers, bool saved, Frame& f) {
  if (f.meshes.size() >= kMaxMeshDraws) {
    ++f.dropped;
    return;
  }
  const uint32_t prim = state + kStatePrim;
  const uint32_t device = Load32(base, prim + kPrimDevice);
  const uint32_t index_buffer = Load32(base, prim + kPrimIndexBuffer);
  const uint32_t stride = Load32(base, prim + kPrimStreams);
  const uint32_t buffer = Load32(base, prim + kPrimStreams + 4u), offset = Load32(base, prim + kPrimStreams + 8u);
  const uint32_t material = Load32(base, state + kStateMaterial), pass = Load32(base, state + kStatePass);
  const uint32_t triangles = Load32(base, arguments + 4u), first_index = Load32(base, arguments + 8u);
  if (!Readable(device) || !Readable(index_buffer) || !Readable(buffer) || !Readable(material) || !Readable(pass) ||
      triangles == 0 || triangles > kMaxMeshTriangles) {
    ++f.dropped;
    return;
  }
  const uint32_t vertex_fetch = Load32(base, buffer + kVertexBufferFetch);
  if ((vertex_fetch & 3u) != 3u) {  // not a vertex buffer's fetch constant
    ++f.dropped;
    return;
  }
  // the world's draws checked (record only) [waw-decomp-waw] 2026-11-11 11:00; on the saved frames only (round 5: it
  // reads every triangle's indices twice)
  if (saved && stride == kWorldStride &&
      (surface_type == kWorldSurfaceType || surface_type == kBrushModelSurfaceType))
    WorldCheck(base, prim, index_buffer, offset, first_index, triangles, surface_type, f);
  const int view_register = RegisterOf(base, pass, kArgCodeVertexConst, kCodeViewProjection);
  const int world_register = RegisterOf(base, pass, kArgCodeVertexConst, kCodeWorldMatrix);
  // [waw-decomp-waw] 2026-11-11 round 4: the sky's model is kept without those registers (LogSkyArguments says which
  // its pass has): its placement from its caller or none, the view-projection from the world's draws
  const bool sky = NameOf(base, Load32(base, state + kStateTechnique)).find("sky") != std::string::npos;
  if (sky) LogSkyArguments(base, pass);
  if (!sky && (view_register < 0 || world_register < 0)) {
    ++f.no_matrices;
    return;
  }
  MeshDraw m;
  m.surface_type = surface_type;
  m.technique_type = technique_type;
  m.vertices = (vertex_fetch & 0x1FFFFFFCu) + offset;
  m.stride = stride;
  m.material = material;
  m.pass = pass;
  m.state_bits = Load32(base, state + kStateBits);
  m.sky = sky;
  m.sky_view_missing = view_register < 0;
  if (view_register >= 0)
    MatrixFrom(base, device + kDeviceVertexConstants + 16u * uint32_t(view_register), m.view_projection);
  // the placement: where the caller itself wrote it for the GPU, else the device's copy of the register (the sky
  // without either: none, its model around the eye)
  const bool direct = Readable(direct_world) && Readable(direct_world + 64u);
  if (direct || world_register >= 0) {
    MatrixFrom(base, direct ? direct_world : device + kDeviceVertexConstants + 16u * uint32_t(world_register),
               m.world);
  } else {
    for (int k = 0; k < 16; ++k) m.world[k] = (k % 5 == 0) ? 1.0f : 0.0f;
  }
  if (!SaneMatrix(m.world) || (!m.sky_view_missing && !SaneMatrix(m.view_projection)) ||
      !SanePlacement(m.world)) {
    ++f.no_matrices;
    return;
  }
  // the indices now, before the game moves on; the fingerprint is FNV-1a over the vertices' address and the indices,
  // and for the skinned models' cache, which holds other corners each frame, their positions too. From the buffer's
  // exact address (FirstIndexAddress) [waw-decomp-waw] 2026-11-11 11:05.
  const bool wide = (Load32(base, index_buffer) >> 31) != 0;
  const uint32_t first =
      FirstIndexAddress(Load32(base, index_buffer + kIndexBufferAddress), wide, first_index);
  uint64_t key = 14695981039346656037ull;
  auto mix = [&key](uint32_t word) {
    for (int b = 0; b < 4; ++b) key = (key ^ ((word >> (8 * b)) & 0xFFu)) * 1099511628211ull;
  };
  mix(m.vertices);
  mix(stride);
  const bool moving = surface_type == kSkinnedSurfaceType;
  uint64_t shape = 14695981039346656037ull;  // [waw-decomp-waw] 2026-11-11 round 5: the stride and the indices alone
  auto mix_shape = [&shape](uint32_t word) {
    for (int b = 0; b < 4; ++b) shape = (shape ^ ((word >> (8 * b)) & 0xFFu)) * 1099511628211ull;
  };
  mix_shape(stride);
  m.used.resize(3u * triangles);
  for (uint32_t i = 0; i < 3u * triangles; ++i) {
    m.used[i] = wide ? GpuU32(base, first + 4u * i) : GpuU16(base, first + 2u * i);
    mix(m.used[i]);
    mix_shape(m.used[i]);
    if (moving)
      for (uint32_t k = 0; k < 3; ++k) mix(GpuU32(base, m.vertices + m.used[i] * stride + 4u * k));
  }
  m.key = key;
  m.shape_key = shape;
  if (sky) {  // [waw-decomp-waw] 2026-11-11 round 4: how far its corners reach (SetMeshes scales it out)
    for (const uint32_t vertex_index : m.used) {
      float d2 = 0.0f;
      for (uint32_t k = 0; k < 3; ++k) {
        const float c = GpuF32(base, m.vertices + vertex_index * stride + 4u * k);
        d2 += c * c;
      }
      m.sky_radius = std::max(m.sky_radius, std::sqrt(d2));
    }
  }
  const int color_slot = ColorMapSlot(base, material, pass);
  if (color_slot >= 0) DeviceTexture(base, device, uint32_t(color_slot), m.color_fetch);
  const uint32_t custom = base[pass + kPassCustomSamplers];
  const bool world = stride == kWorldStride;
  if (world && (custom & 2u)) DeviceTexture(base, device, 2, m.sun_map_fetch);
  if (world && (custom & 4u)) DeviceTexture(base, device, 3, m.lightmap_fetch);
  const int sun_direction = RegisterOf(base, pass, kArgCodePixelConst, kCodeSunPosition);
  const int sun_color = RegisterOf(base, pass, kArgCodePixelConst, kCodeSunDiffuse);
  if (sun_direction >= 0 && sun_color >= 0) {
    m.sunlit = true;
    for (uint32_t k = 0; k < 3; ++k) {
      m.sun_direction[k] = LoadF32(base, device + kDevicePixelConstants + 16u * uint32_t(sun_direction) + 4u * k);
      m.sun_color[k] = LoadF32(base, device + kDevicePixelConstants + 16u * uint32_t(sun_color) + 4u * k);
    }
  }
  const int fog_color = RegisterOf(base, pass, kArgCodePixelConst, kCodeFogColor);
  for (uint32_t k = 0; k < 3 && fog_color >= 0; ++k)
    m.fog_color[k] = LoadF32(base, device + kDevicePixelConstants + 16u * uint32_t(fog_color) + 4u * k);
  // [waw-decomp-waw] 2026-11-11 round 4: the fog's numbers for the vertex shader (FogOf)
  if (const int fog = RegisterOf(base, pass, kArgCodeVertexConst, kCodeFogConsts); fog >= 0 && fog_color >= 0) {
    m.fogged = true;
    for (uint32_t k = 0; k < 4; ++k)
      m.fog_consts[k] = LoadF32(base, device + kDeviceVertexConstants + 16u * uint32_t(fog) + 4u * k);
    static int logged_fog = 0;
    if (logged_fog < 2) {
      ++logged_fog;
      REXLOG_INFO("waw: native side: the fog: vertex c{} {:.6f} {:.6f} {:.4f} {:.5f}, pixel c{} (its color) {:.3f} "
                  "{:.3f} {:.3f}; surface type {}", fog, m.fog_consts[0], m.fog_consts[1], m.fog_consts[2],
                  m.fog_consts[3], fog_color, m.fog_color[0], m.fog_color[1], m.fog_color[2], surface_type);
    }
  }
  // [waw-decomp-waw] 2026-11-11 round 3: a model's own light (read now: the game fills the texture for this frame), and
  // the world's layered draws counted and the first few logged
  if (!world) m.model_light = ModelLightAt(base, device, pass, stream_registers, m, f.model_light);
  if (const uint32_t layer_type = Load32(base, prim + kPrimDeclType);
      world && (layer_type == 3 || layer_type == 4 || layer_type == 7)) {
    ++f.model_light.layered;
    LogLayer(base, state, m);
    if (LayerOf(base, state, device, m)) {  // [waw-decomp-waw] 2026-11-11 round 4: counted, the first few logged
      ++f.model_light.layers_found;
      const bool tinted = m.tint[0] != 1.0f || m.tint[1] != 1.0f || m.tint[2] != 1.0f || m.tint[3] != 1.0f ||
                          m.layer_tint[0] != 1.0f || m.layer_tint[1] != 1.0f || m.layer_tint[2] != 1.0f ||
                          m.layer_tint[3] != 1.0f;
      f.model_light.tinted += tinted;
      f.model_light.cut += m.layer_cut;
      static int logged_tints = 0;
      if (logged_tints < 6) {
        ++logged_tints;
        REXLOG_INFO("waw: native side: a layered draw's tints: material \"{}\", technique \"{}\" (first layer {}): "
                    "colorTint {:.3f} {:.3f} {:.3f} {:.3f}, colorTint1 {:.3f} {:.3f} {:.3f} {:.3f}",
                    NameOf(base, m.material), NameOf(base, Load32(base, state + kStateTechnique)),
                    m.layer_cut ? "cut" : "not cut", m.tint[0], m.tint[1], m.tint[2], m.tint[3], m.layer_tint[0],
                    m.layer_tint[1], m.layer_tint[2], m.layer_tint[3]);
      }
    }
  }
  if (surface_type < kSurfaceTypes && g_samples[surface_type] < kSamples) {  // the first few whole, as a check
    ++g_samples[surface_type];
    const uint32_t v = m.vertices + m.used[0] * stride;
    const uint32_t declaration_type = Load32(base, prim + kPrimDeclType);
    auto size_of = [](const std::array<uint32_t, 6>& fetch) {
      return fetch[0] == 0 ? std::string("none")
                           : "format " + std::to_string(fetch[1] & 0x3Fu) + " " +
                                 std::to_string((fetch[2] & 0x1FFFu) + 1) + "x" +
                                 std::to_string(((fetch[2] >> 13) & 0x1FFFu) + 1);
    };
    std::string words;
    for (uint32_t k = 0; k < stride / 4 && k < 11; ++k) {
      char word[12];
      std::snprintf(word, sizeof word, " %08X", GpuU32(base, v + 4u * k));
      words += word;
    }
    REXLOG_INFO("waw: native side: sample: surface type {}, technique type {} \"{}\", material \"{}\", vs \"{}\" ps "
                "\"{}\"; {} triangles from index {}, stride {}, vertices at {:08X}, first indices {} {} {}; vertex "
                "{}:{}; state bits {:08X}; custom samplers {}; color map in slot {} ({}), lightmap {}, sun map {}; "
                "placement {} (c{}): rows ({:.3f} {:.3f} {:.3f}) ({:.3f} {:.3f} {:.3f}) ({:.3f} {:.3f} {:.3f}) "
                "({:.1f} {:.1f} {:.1f}); near {:.2f}; sun {} toward {:.3f} {:.3f} {:.3f} color {:.3f} {:.3f} {:.3f}",
                surface_type, technique_type, NameOf(base, Load32(base, state + kStateTechnique)),
                NameOf(base, material),
                NameOf(base, Load32(base, pass + kPassVertexShaders + 4u * (declaration_type & 15u))),
                NameOf(base, Load32(base, pass + kPassPixelShader)), triangles, first_index, stride, m.vertices,
                m.used[0], m.used[1], m.used[2], m.used[0], words, m.state_bits, custom, color_slot,
                size_of(m.color_fetch), size_of(m.lightmap_fetch), size_of(m.sun_map_fetch),
                direct ? "written by the caller" : "from the device", world_register, m.world[0], m.world[1],
                m.world[2], m.world[4], m.world[5], m.world[6], m.world[8], m.world[9], m.world[10], m.world[12],
                m.world[13], m.world[14], m.view_projection[14], m.sunlit ? "yes" : "no", m.sun_direction[0],
                m.sun_direction[1], m.sun_direction[2], m.sun_color[0], m.sun_color[1], m.sun_color[2]);
  }
  if (!m.sky_view_missing) UsualDepth(m.view_projection);
  f.meshes.push_back(std::move(m));
}

// ---------------------------------------------------------------------------------------------------------------
// The helper thread: saves the native picture, then the game's own, and logs both names.

struct Job {
  uint64_t frame = 0;
  std::vector<uint8_t> rgba;  // the native picture, 4 bytes a pixel (red first)
  uint32_t width = 0, height = 0;
  std::string summary;  // what was drawn (without the word "to": the compare tool looks for " to <picture>")
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

// The picture's red, green and blue as a PNG (the compare tool takes the game's picture as a .png; lossless), its
// data in stored (uncompressed) deflate blocks, so it needs nothing beyond standard C++. Written to <file>.part, then
// renamed, so a half-written file never has the real name. (As Call of Duty 2's native_side.cpp writes it.)
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

// The game's own picture (the frame the GPU emulation last sent out), as waw_debug_screenshots reads it.
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
    REXLOG_WARN("waw: native side: frame {}: the native picture couldn't be saved ({})", job.frame, native.string());
    return;
  }
  std::string game = "not taken";
  rex::ui::RawImage image;
  if (CaptureGame(image)) {
    const std::string name = stem + "-game.png";
    if (WritePng(g_folder / name, image)) game = name;
  }
  REXLOG_INFO("waw: native side: frame {}: {} to {}; the game's own: {}", job.frame, job.summary, native.string(),
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
      REXLOG_WARN("waw: native side: frame {}: no helper to save it (started with the check off?)", job.frame);
      return;
    }
    g_jobs.push_back(std::move(job));
  }
  g_jobs_cv.notify_one();
}

#if defined(WAW_NGL_VK)
// ---------------------------------------------------------------------------------------------------------------
// ngl_vk

int g_state = 0;  // 0 not started, 1 ready, -1 failed (no more tries)

struct CachedTexture {
  std::array<uint32_t, 6> fetch{};
  uint32_t id = 0;  // ngl_vk's, 0 when it couldn't be decoded
};
std::unordered_map<uint32_t, CachedTexture> g_textures;  // by the texture's address (its fetch constant's)
std::map<std::string, int> g_failed;                     // why textures weren't decoded, and how many
constexpr size_t kMaxTextures = 1500;                    // ngl_vk has room for 2048
std::unordered_map<uint64_t, uint32_t> g_meshes;         // the meshes made so far, by MeshDraw::key
// the world's lightmaps as ngl_vk takes them, by the addresses of the two textures they are made from
std::map<std::pair<uint32_t, uint32_t>, uint32_t> g_lightmaps;
// [waw-decomp-waw] 2026-11-11 round 4: the layers' color maps with their tint and cut (BakedTextureFor), by a
// fingerprint of the fetch constant, the tint and the cut
std::unordered_map<uint64_t, uint32_t> g_baked;
// [waw-decomp-waw] 2026-11-11 round 4: the sky boxes made so far (SkyBoxFor), by the sky cube's address: a mesh and a
// texture for each face
struct SkyBox {
  std::array<uint32_t, 6> meshes{}, faces{};
  bool made = false;
};
std::unordered_map<uint32_t, SkyBox> g_sky_boxes;
// [waw-decomp-waw] 2026-11-11 round 5: the skinned models' triangles kept by their shape (SkinnedMeshFor), by
// MeshDraw::shape_key: its mesh (0 for a shape with too many corners) and in order the vertex each of its corners is
// made from, so a frame's corners are read without sorting the indices again
struct Shape {
  uint32_t mesh = 0;
  std::vector<uint32_t> order;
};
std::unordered_map<uint64_t, Shape> g_shapes;
uint64_t g_layer_corners = 0, g_layer_corners_dim = 0;  // the layers' corners made, and those with red under 0xFF
bool g_forget = false;  // ngl_vk ran out of room: forget everything before the next frame
uint32_t g_white = 0;   // a 1 x 1 white texture (WhiteTexture)
bool g_logged_lightmap = false;

void ForgetAll() {
  ngl_vk_forget();
  g_textures.clear();
  g_meshes.clear();
  g_lightmaps.clear();
  g_baked.clear();
  g_sky_boxes.clear();
  g_shapes.clear();  // [waw-decomp-waw] 2026-11-11 round 5
  g_white = 0;
  g_forget = false;
}

// ngl_vk's texture for a fetch constant, decoded once (again when the constant changes); 0 when it can't be.
uint32_t TextureFor(const uint8_t* base, const std::array<uint32_t, 6>& fetch) {
  if (fetch[0] == 0) return 0;
  const uint32_t address = fetch[1] & 0x1FFFF000u;
  auto found = g_textures.find(address);
  if (found != g_textures.end() && found->second.fetch == fetch) return found->second.id;
  if (g_textures.size() >= kMaxTextures) g_forget = true;  // forgotten before the next frame (meshes would go too)
  CachedTexture& cached = g_textures[address];
  cached.fetch = fetch;
  cached.id = 0;
  std::vector<uint8_t> bgra;
  uint32_t width = 0, height = 0;
  std::string why;
  if (DecodeFetch(base, fetch, bgra, width, height, why)) {
    std::vector<std::vector<uint8_t>> levels;  // with the game's own smaller levels when they can be read
    DecodeLevelsFetch(base, fetch, levels);
    std::vector<const void*> pictures{bgra.data()};
    for (const auto& level : levels) pictures.push_back(level.data());
    cached.id = ngl_vk_texture_levels(pictures.data(), static_cast<uint32_t>(pictures.size()), width, height);
    if (cached.id == 0) why = "ngl_vk has no room";
    // [waw-decomp-waw] 2026-11-11 round 5: with the window's picture nothing else forgets (SkinnedMeshFor), so the
    // textures of the menus and the loading screen stayed until ngl_vk's room for textures was full (run r5b1: the
    // flamethrower and the HUD's icons drawn without theirs): everything is forgotten before the next frame
    if (cached.id == 0 && NativeVulkanOn()) g_forget = true;
  }
  if (cached.id == 0 && g_failed[why]++ == 0)
    REXLOG_INFO("waw: native side: the texture at {:08X} not drawn ({}); its color alone is (further ones not "
                "logged)", address, why);
  return cached.id;
}

// [waw-decomp-waw] 2026-11-11 round 4: a layered draw's color map with its tint, and for a cut first layer ("b0c0")
// the cut. ngl_vk multiplies a texture by no color of the draw's, and its lightmapped draws can't make a pixel black
// where the alpha is low (its alpha test lets what is behind show), so both are made into a copy of the texture, each
// level: red, green and blue x the tint's, alpha x the tint's; cut: black and opaque where that alpha is under 1/2
// (the shader compares the filtered alpha x the corner's red: here each texel before filtering, the red taken as 1:
// every layered corner logged in r3c1 had red 0xFF, and LayerMeshFor counts those that don't). The plain texture
// (TextureFor) when the tint is 1 1 1 1 and nothing is cut. Made once for each texture, tint and cut.
uint32_t BakedTextureFor(const uint8_t* base, const std::array<uint32_t, 6>& fetch, const float tint[4], bool cut) {
  if (!cut && tint[0] == 1.0f && tint[1] == 1.0f && tint[2] == 1.0f && tint[3] == 1.0f)
    return TextureFor(base, fetch);
  if (fetch[0] == 0) return 0;
  uint64_t key = 14695981039346656037ull;  // FNV-1a over the fetch constant, the tint and the cut
  auto mix = [&key](uint32_t word) {
    for (int b = 0; b < 4; ++b) key = (key ^ ((word >> (8 * b)) & 0xFFu)) * 1099511628211ull;
  };
  for (const uint32_t word : fetch) mix(word);
  for (int k = 0; k < 4; ++k) {
    uint32_t bits;
    std::memcpy(&bits, &tint[k], 4);
    mix(bits);
  }
  mix(cut ? 1u : 0u);
  if (auto found = g_baked.find(key); found != g_baked.end()) return found->second;
  uint32_t& id = g_baked[key];
  id = 0;
  std::vector<uint8_t> bgra;
  uint32_t width = 0, height = 0;
  std::string why;
  if (!DecodeFetch(base, fetch, bgra, width, height, why)) return 0;
  std::vector<std::vector<uint8_t>> levels;
  DecodeLevelsFetch(base, fetch, levels);
  uint64_t texels = 0, cut_texels = 0;
  auto bake = [&](std::vector<uint8_t>& pixels, bool count) {
    for (size_t i = 0; i + 3 < pixels.size(); i += 4) {
      const float alpha = pixels[i + 3] / 255.0f * tint[3];
      for (int k = 0; k < 3; ++k)  // blue, green, red (DecodeFetch's order): the tint's third, second, first
        pixels[i + k] = static_cast<uint8_t>(std::clamp(pixels[i + k] * tint[2 - k] + 0.5f, 0.0f, 255.0f));
      const bool hidden = cut && alpha < 0.5f;
      if (hidden) {
        pixels[i] = 0;
        pixels[i + 1] = 0;
        pixels[i + 2] = 0;
      }
      pixels[i + 3] = cut ? 255 : static_cast<uint8_t>(std::clamp(alpha * 255.0f + 0.5f, 0.0f, 255.0f));
      texels += count;
      cut_texels += count && hidden;
    }
  };
  bake(bgra, true);
  for (auto& level : levels) bake(level, false);
  std::vector<const void*> pictures{bgra.data()};
  for (const auto& level : levels) pictures.push_back(level.data());
  id = ngl_vk_texture_levels(pictures.data(), static_cast<uint32_t>(pictures.size()), width, height);
  if (id == 0) g_forget = true;
  static int logged = 0;
  if (logged < 6) {
    ++logged;
    REXLOG_INFO("waw: native side: a layer's color map made with its tint {:.3f} {:.3f} {:.3f} {:.3f}{}: {}x{}, {} "
                "of {} texels cut", tint[0], tint[1], tint[2], tint[3], cut ? " and cut" : "", width, height,
                cut_texels, texels);
  }
  return id;
}

// [waw-decomp-waw] 2026-11-11 round 4: how much of a texture's alpha is under one, for the second layers (their own
// alpha is part of their share in the game's shader; ngl_vk's corner_alpha leaves it out): the average alpha of the
// top level, 0 to 255, or -1 when it can't be read. Record only (SetMeshes logs the first few second layers').
double AverageAlpha(const uint8_t* base, const std::array<uint32_t, 6>& fetch) {
  std::vector<uint8_t> bgra;
  uint32_t width = 0, height = 0;
  std::string why;
  if (fetch[0] == 0 || !DecodeFetch(base, fetch, bgra, width, height, why) || bgra.empty()) return -1.0;
  double sum = 0.0;
  for (size_t i = 3; i < bgra.size(); i += 4) sum += bgra[i];
  return sum / double(bgra.size() / 4);
}

// [waw-decomp-waw] 2026-11-11 round 4: the fog, as ngl_vk can draw it. The world's layered vertex shader (PC
// vs_3_0_1bf3bd57, fogConsts in c21; read with d3d9dis.py, its instructions 29-47) makes the share of the color kept:
//     h = c21.x x (the corner's height above the eye) + 0.000001, F = (1 - 2^-h) / h (F = 1 when |height| <= 0.01
//     or c21.x <= 0), kept = min(2^(c21.w - c21.y x c21.z x F x the corner's distance from the eye), 1),
// and the pixel shaders mix toward the fog's color (pixel c0) by 1 - kept. Pel1a's c21 is 0.0005 0.00035881 1.0194
// 0.35881 (round 1): F is ln 2 = 0.693 at the eye's height (0.67 200 units above it, 0.72 200 below), so there is no
// fog to 1,415 units and half at 5,360. ngl_vk's fog is linear (by the view's depth) or exponential from the eye with
// no start (mesh.vert), so it is drawn linear here: from the start (kept 1) to where kept is 1/4, 3/4 fog there and
// held beyond; in between and beyond it is up to about 0.14 under the game's. F is taken as ln 2 everywhere (the
// height left out). WAW-NATIVE.md round 4 has the ngl_vk lines that would draw it exactly.
void FogOf(const MeshDraw& m, ngl_vk_mesh_draw& d) {
  // the rate in halvings a unit of distance
  const float rate = m.fog_consts[1] * m.fog_consts[2] * (m.fog_consts[0] > 0.0f ? 0.693147f : 1.0f);
  if (!m.fogged || !(rate > 0.0f) || !std::isfinite(rate) || !std::isfinite(m.fog_consts[3])) return;
  const float offset = m.fog_consts[3];
  const float start = std::max(offset, 0.0f) / rate, end = (offset + 2.0f) / rate;
  if (!(end > start)) return;
  d.fog[0] = start;
  d.fog[1] = end;
  d.fog[2] = 0.75f;
  const float before = offset < 0.0f ? 1.0f - std::exp2(offset) : 0.0f;  // the fog at the eye
  auto byte = [](float v) { return static_cast<uint32_t>(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
  d.fog_color = byte(m.fog_color[0]) | byte(m.fog_color[1]) << 8 | byte(m.fog_color[2]) << 16 | byte(before) << 24;
}

// [waw-decomp-waw] 2026-11-11 round 4: the sky, as Call of Duty 2's native side draws its own (SkyBoxFor and
// AddSkyBox there): the game's sky.hlsl (the mod tools' techniques/sky.tech: stateMap "color_only", the material's
// colorMap as skyMapSampler) colors the sky by its cube map looked up along the way from the eye. ngl_vk has no sky
// cube, so it is drawn as a box around the eye, one square a face with that face of the cube as a 2D texture: on a
// face's plane the cube's coordinates are linear, so the squares' texture coordinates are exact. A Xenos cube's faces
// lie one after another in memory (+X, -X, +Y, -Y, +Z, -Z), each its top level's size rounded up to 4 KB. Direct3D's
// faces: on face +X a texel at (s, t) = (2u - 1, 2v - 1) points along (1, -t, -s), and so on (kSkyFaces, Call of
// Duty 2's table).
constexpr int kSkyFaces[6][3][3] = {
    // x, y, z of the way, each as {s weight, t weight, constant}
    {{0, 0, 1}, {0, -1, 0}, {-1, 0, 0}},   // +X: (1, -t, -s)
    {{0, 0, -1}, {0, -1, 0}, {1, 0, 0}},   // -X: (-1, -t, s)
    {{1, 0, 0}, {0, 0, 1}, {0, 1, 0}},     // +Y: (s, 1, t)
    {{1, 0, 0}, {0, 0, -1}, {0, -1, 0}},   // -Y: (s, -1, -t)
    {{1, 0, 0}, {0, -1, 0}, {0, 0, 1}},    // +Z: (s, -t, 1)
    {{-1, 0, 0}, {0, -1, 0}, {0, 0, -1}},  // -Z: (-s, -t, -1)
};

// A Xenos texture format's block (texels on a side) and its bytes, for the formats a sky cube is likely in (the
// face's size in memory); false for the others (then no sky box).
bool BlockOf(uint32_t format, uint32_t& block, uint32_t& bytes) {
  switch (format) {
    case 2: block = 1; bytes = 1; return true;            // 8
    case 4: case 10: block = 1; bytes = 2; return true;   // 5_6_5, 8_8
    case 6: block = 1; bytes = 4; return true;            // 8_8_8_8
    case 18: block = 4; bytes = 8; return true;           // DXT1
    case 19: case 20: case 49: block = 4; bytes = 16; return true;  // DXT2_3, DXT4_5, DXN
    default: return false;
  }
}

// The sky box for a cube's fetch constant, made once; null when its faces can't be read.
const SkyBox* SkyBoxFor(const uint8_t* base, const std::array<uint32_t, 6>& fetch) {
  const uint32_t physical = fetch[1] & 0x1FFFF000u;
  if (auto found = g_sky_boxes.find(physical); found != g_sky_boxes.end())
    return found->second.made ? &found->second : nullptr;
  SkyBox& made = g_sky_boxes[physical];
  uint32_t block = 0, bytes = 0;
  if (((fetch[5] >> 9) & 3u) != 3u || !BlockOf(fetch[1] & 0x3Fu, block, bytes)) {
    REXLOG_INFO("waw: native side: the sky's cube (format {}) can't be read: no sky box", fetch[1] & 0x3Fu);
    return nullptr;
  }
  const uint32_t width = (fetch[2] & 0x1FFFu) + 1, height = ((fetch[2] >> 13) & 0x1FFFu) + 1;
  const bool tiled = (fetch[0] >> 31) != 0;
  const uint32_t blocks_x = (width + block - 1) / block, blocks_y = (height + block - 1) / block;
  uint32_t pitch_blocks = std::max(((fetch[0] >> 22) & 0x1FFu) * 32u / block, blocks_x);
  if (tiled) pitch_blocks = (pitch_blocks + 31u) & ~31u;
  const uint32_t rows = tiled ? (blocks_y + 31u) & ~31u : blocks_y;
  const uint32_t face_bytes = (pitch_blocks * rows * bytes + 4095u) & ~4095u;
  static constexpr float kUv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  static constexpr uint16_t kSquare[6] = {0, 1, 2, 0, 2, 3};
  const uint32_t white = 0xFFFFFFFFu;
  float white_bits;
  std::memcpy(&white_bits, &white, 4);
  for (uint32_t face = 0; face < 6; ++face) {
    std::array<uint32_t, 6> face_fetch = fetch;
    face_fetch[1] = (fetch[1] & ~0x1FFFF000u) | ((physical + face * face_bytes) & 0x1FFFF000u);
    std::vector<uint8_t> bgra;
    uint32_t w = 0, h = 0;
    std::string why;
    if (!DecodeFetch(base, face_fetch, bgra, w, h, why)) {
      REXLOG_INFO("waw: native side: the sky's cube face {} can't be read ({}): no sky box", face, why);
      return nullptr;
    }
    made.faces[face] = ngl_vk_texture(bgra.data(), w, h);
    float corners[4 * NGL_VK_CORNER_FLOATS] = {};
    for (int c = 0; c < 4; ++c) {
      float* corner = &corners[c * NGL_VK_CORNER_FLOATS];
      const float st[2] = {2.0f * kUv[c][0] - 1.0f, 2.0f * kUv[c][1] - 1.0f};
      for (int k = 0; k < 3; ++k)
        corner[k] = st[0] * kSkyFaces[face][k][0] + st[1] * kSkyFaces[face][k][1] + kSkyFaces[face][k][2];
      corner[3] = kUv[c][0];
      corner[4] = kUv[c][1];
      corner[7] = white_bits;  // the lightmap's coordinates stay 0, and no normal
    }
    made.meshes[face] = ngl_vk_mesh(corners, 4, kSquare, 6);
    if (made.faces[face] == 0 || made.meshes[face] == 0) {
      g_forget = true;
      return nullptr;
    }
  }
  made.made = true;
  REXLOG_INFO("waw: native side: the sky box: a {}x{} cube of format {}, faces {} bytes apart", width, height,
              fetch[1] & 0x3Fu, face_bytes);
  return &made;
}

// The sky box's six draws, first, opaque and unlit (not fogged: ngl_vk leaves unlit draws so, and sky.hlsl has no
// fog): the box (corners at -1 to 1) scaled to kSkyRadius around the eye (the world's draws' view-projection is
// relative to the eye, so the box is not moved), inside the far plane UsualDepth gives ngl_vk.
constexpr float kSkyRadius = 16384.0f;  // its corners 28,378 from the eye, inside kFarDistance
void AddSkyBox(const SkyBox& box, const float view_projection[16], std::vector<ngl_vk_mesh_draw>& out) {
  for (uint32_t face = 0; face < 6; ++face) {
    ngl_vk_mesh_draw d{};
    d.mesh = box.meshes[face];
    d.texture = box.faces[face];
    d.tint = 0x7F7F7Fu;  // no flag bit of ngl_vk's in it (SetMeshes)
    d.shading = NGL_VK_SHADE_UNLIT;
    d.alpha = NGL_VK_ALPHA_OPAQUE;
    const float r = kSkyRadius;
    const float world[16] = {r, 0, 0, 0, 0, r, 0, 0, 0, 0, r, 0, 0, 0, 0, 1};
    std::memcpy(d.world, world, sizeof d.world);
    std::memcpy(d.view_projection, view_projection, sizeof d.view_projection);
    out.push_back(d);
  }
}

// A 1 x 1 white texture, for the lightmaps alone (waw_debug_native_light_test 3).
uint32_t WhiteTexture() {
  if (g_white == 0) {
    const uint8_t pixel[4] = {255, 255, 255, 255};
    g_white = ngl_vk_texture(pixel, 1, 1);
  }
  return g_white;
}

// The world's lightmap as ngl_vk's lightmap_scale shading takes it, made the way the game's lit world pixel shaders
// make the light. [waw-decomp-waw] 2026-11-11 09:40: read in the mod tools' compiled PC shaders of those techniques
// (raw/shader_bin, disassembled on the PC by waw-decomp-tools/waw/d3d9dis.py: "lm_sun_r0c0_sm3" ps_3_0_cbfe86c6,
// "lm_r0c0" ps_3_0_c3686a4f) and checked against the 360's own lm_sun kind (shader E927DE2FA9E16370 of an earlier
// run's dump, waw-decomp-gfx/shaders1/waw-off-shaders: the same fetches, the same steps):
//   - the secondary lightmap (slot 3, 8-8-8-8, 512 x 2048) is two maps stacked, as Call of Duty 4's is: P at
//     (u, v / 2), its top half, and S at (u, v / 2 + 1/2), its bottom half;
//   - light = P.rgb + S.rgb x saturate(0.6 x 2^-(a^2 + b^2) + 0.4), a = 4.08 P.a - 2.08, b = 4.06452 S.a - 2.06452
//     (the PC shader's literals; the 360's scales both alphas by one literal pair that its dump doesn't print);
//   - the "lm_sun" passes add the sun: sunDiffuse (pixel c18) x saturate(N . sunPosition (c17)) x the primary
//     lightmap's x (slot 2, 8-bit, 1024 x 2048: twice a half's size each way), read at (u, v), the share of the sun
//     that reaches the place. The "lm" passes (no sun) don't read the primary lightmap;
//   - nothing doubled: the color map x that light, then the fog.
// ngl_vk's shading (ngl_vk.h lightmap_scale) is lightmap_scale x (the texture's color + the sun's color x its alpha x
// the normal against the sun), so the texture made here holds the light / kLightmapScale in its color and the sun's
// share in its alpha: at the primary's size when the pass has one (the halves' light read there bilinearly), else at
// a half's size with alpha 0. Not drawn: the 360 shader's projected lookup that also scales the share (tf4, likely
// the sun's shadow) and its color map x the corners' color. Made once for each pair of textures.
uint32_t LightmapFor(const uint8_t* base, const MeshDraw& m) {
  if (m.lightmap_fetch[0] == 0) return 0;
  const std::pair<uint32_t, uint32_t> key{m.lightmap_fetch[1] & 0x1FFFF000u, m.sun_map_fetch[1] & 0x1FFFF000u};
  if (auto found = g_lightmaps.find(key); found != g_lightmaps.end()) return found->second;
  uint32_t& id = g_lightmaps[key];
  id = 0;
  std::vector<uint8_t> stacked, share;
  uint32_t width = 0, height = 0, share_width = 0, share_height = 0;
  std::string why;
  if (!DecodeFetch(base, m.lightmap_fetch, stacked, width, height, why) || height < 2) return 0;
  const bool with_share = m.sun_map_fetch[0] != 0 &&
                          DecodeFetch(base, m.sun_map_fetch, share, share_width, share_height, why);
  // the two maps' light at a half's size: blue, green, red (DecodeFetch's order), 1 = the full 8 bits
  const uint32_t half = height / 2;
  std::vector<float> light(size_t(width) * half * 3);
  for (uint32_t y = 0; y < half; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const uint8_t* p = &stacked[(size_t(y) * width + x) * 4];
      const uint8_t* s = &stacked[(size_t(y + half) * width + x) * 4];
      const float a = 4.08f * (p[3] / 255.0f) - 2.08f;
      const float b = 4.06452f * (s[3] / 255.0f) - 2.06452f;
      const float facing = std::clamp(0.6f * std::exp2(-(a * a + b * b)) + 0.4f, 0.0f, 1.0f);
      float* out = &light[(size_t(y) * width + x) * 3];
      for (int k = 0; k < 3; ++k) out[k] = (p[k] + s[k] * facing) / 255.0f;
    }
  }
  // the texture, the light read bilinearly at each of its texels' middles (clamped at the edges)
  const uint32_t out_width = with_share ? share_width : width, out_height = with_share ? share_height : half;
  std::vector<uint8_t> texels(size_t(out_width) * out_height * 4);
  double sums[4] = {};
  for (uint32_t y = 0; y < out_height; ++y) {
    const float fy = std::clamp((y + 0.5f) * half / out_height - 0.5f, 0.0f, float(half - 1));
    const uint32_t y0 = uint32_t(fy), y1 = std::min(y0 + 1, half - 1);
    const float wy = fy - float(y0);
    for (uint32_t x = 0; x < out_width; ++x) {
      const float fx = std::clamp((x + 0.5f) * width / out_width - 0.5f, 0.0f, float(width - 1));
      const uint32_t x0 = uint32_t(fx), x1 = std::min(x0 + 1, width - 1);
      const float wx = fx - float(x0);
      uint8_t* texel = &texels[(size_t(y) * out_width + x) * 4];
      for (int k = 0; k < 3; ++k) {
        auto at = [&](uint32_t lx, uint32_t ly) { return light[(size_t(ly) * width + lx) * 3 + k]; };
        const float value = (at(x0, y0) * (1.0f - wx) + at(x1, y0) * wx) * (1.0f - wy) +
                            (at(x0, y1) * (1.0f - wx) + at(x1, y1) * wx) * wy;
        texel[k] = uint8_t(std::clamp(value / kLightmapScale * 255.0f + 0.5f, 0.0f, 255.0f));
      }
      // the sun's share: the primary lightmap's x (the fetch's first channel: red in DecodeFetch's order)
      texel[3] = with_share ? share[(size_t(y) * share_width + x) * 4 + 2] : 0;
      for (int k = 0; k < 4; ++k) sums[k] += texel[k];
    }
  }
  id = ngl_vk_texture(texels.data(), out_width, out_height);
  if (id == 0) g_forget = true;
  if (!g_logged_lightmap && with_share) {  // once: what the texture holds, to check how the two maps are read
    g_logged_lightmap = true;
    const double count = double(out_width) * out_height;
    REXLOG_INFO("waw: native side: a lightmap: secondary format {} {}x{} (two halves), primary format {} {}x{}; "
                "made {}x{}, averages blue {:.1f} green {:.1f} red {:.1f} (the light / {}), alpha {:.1f} (the sun's "
                "share)", m.lightmap_fetch[1] & 0x3Fu, width, height, m.sun_map_fetch[1] & 0x3Fu, share_width,
                share_height, out_width, out_height, sums[0] / count, sums[1] / count, sums[2] / count,
                kLightmapScale, sums[3] / count);
  }
  return id;
}

// A packed unit vector (Direct3D's DEC3N: three signed 10-bit parts, x lowest) as floats.
void UnpackNormal(uint32_t word, float out[3]) {
  for (int k = 0; k < 3; ++k) {
    int32_t part = static_cast<int32_t>((word >> (10 * k)) & 0x3FFu);
    if (part & 0x200) part -= 0x400;
    out[k] = std::clamp(static_cast<float>(part) / 511.0f, -1.0f, 1.0f);
  }
}

// A 16-bit float (the models' texture coordinates) as a float.
float HalfToFloat(uint32_t half) {
  const uint32_t exponent = (half >> 10) & 0x1Fu, mantissa = half & 0x3FFu;
  float value;
  if (exponent == 0) {
    value = std::ldexp(static_cast<float>(mantissa), -24);
  } else if (exponent == 31) {
    value = 65504.0f;  // infinities and not-a-numbers: the largest half, so nothing downstream breaks
  } else {
    value = std::ldexp(static_cast<float>(mantissa | 0x400u), static_cast<int>(exponent) - 25);
  }
  return (half & 0x8000u) ? -value : value;
}

// The ngl_vk mesh of a draw, made once. The world's 44-byte vertex (IW3's GfxWorldVertex; the Direct3D declaration
// of type 2 in run waw-decomp-waw-side1b: four floats at +0x00, a color at +0x10, four floats at +0x14, a packed
// vector at +0x24): position +0x00, the binormal's sign +0x0C, color +0x10, texture coordinate +0x14, lightmap
// coordinate +0x1C, packed normal +0x24, packed tangent +0x28. The models' 32-byte packed vertex (declaration type
// 1): position +0x00, the binormal's sign +0x0C, color +0x10, the texture coordinate as two 16-bit floats +0x14,
// packed normal +0x18, packed tangent +0x1C.
// One corner of a kept draw as ngl_vk_mesh takes it (NGL_VK_CORNER_FLOATS floats), from its vertex vertex_index.
// [waw-decomp-waw] 2026-11-11 round 5: taken out of MeshFor unchanged, for SkinnedMeshFor too.
void CornerAt(const uint8_t* base, const MeshDraw& m, uint32_t vertex_index, float out[NGL_VK_CORNER_FLOATS]) {
  const bool world = m.stride == kWorldStride;
  const uint32_t v = m.vertices + vertex_index * m.stride;
  const uint32_t color = 0xFFFFFFFFu;  // the corners' own colors are not used yet
  float color_bits;
  std::memcpy(&color_bits, &color, 4);
  float normal[3], u, v_coordinate, lightmap_u = 0.0f, lightmap_v = 0.0f;
  if (world) {
    u = GpuF32(base, v + 0x14u);
    v_coordinate = GpuF32(base, v + 0x18u);
    lightmap_u = GpuF32(base, v + 0x1Cu);
    lightmap_v = GpuF32(base, v + 0x20u);
    UnpackNormal(GpuU32(base, v + 0x24u), normal);
  } else {
    const uint32_t packed = GpuU32(base, v + 0x14u);
    u = HalfToFloat(packed >> 16);
    v_coordinate = HalfToFloat(packed & 0xFFFFu);
    UnpackNormal(GpuU32(base, v + 0x18u), normal);
  }
  const float corner[NGL_VK_CORNER_FLOATS] = {GpuF32(base, v), GpuF32(base, v + 4u), GpuF32(base, v + 8u),
                                              u, v_coordinate, lightmap_u, lightmap_v, color_bits,
                                              normal[0], normal[1], normal[2]};
  std::memcpy(out, corner, sizeof(corner));
}

// The corners and the 16-bit corner numbers of a kept draw as ngl_vk_mesh takes them, and in order the vertex each
// corner is made from; false when it has more than 65535 corners. [waw-decomp-waw] 2026-11-11 round 5: taken out of
// MeshFor unchanged (order added), for SkinnedMeshFor too.
bool MeshCorners(const uint8_t* base, const MeshDraw& m, std::vector<float>& corners, std::vector<uint16_t>& indices,
                 std::vector<uint32_t>* order = nullptr) {
  std::unordered_map<uint32_t, uint16_t> local;
  corners.clear();
  indices.clear();
  indices.reserve(m.used.size());
  for (const uint32_t vertex_index : m.used) {
    auto [it, inserted] = local.emplace(vertex_index, static_cast<uint16_t>(corners.size() / NGL_VK_CORNER_FLOATS));
    if (inserted) {
      if (corners.size() / NGL_VK_CORNER_FLOATS >= 65535) return false;
      float corner[NGL_VK_CORNER_FLOATS];
      CornerAt(base, m, vertex_index, corner);
      corners.insert(corners.end(), corner, corner + NGL_VK_CORNER_FLOATS);
      if (order) order->push_back(vertex_index);
    }
    indices.push_back(it->second);
  }
  return true;
}

uint32_t MeshFor(const uint8_t* base, const MeshDraw& m, uint32_t& built) {
  if (auto found = g_meshes.find(m.key); found != g_meshes.end()) return found->second;
  uint32_t& id = g_meshes[m.key];
  id = 0;
  std::vector<float> corners;
  std::vector<uint16_t> indices;
  if (!MeshCorners(base, m, corners, indices)) return 0;
  id = ngl_vk_mesh(corners.data(), static_cast<uint32_t>(corners.size() / NGL_VK_CORNER_FLOATS), indices.data(),
                   static_cast<uint32_t>(indices.size()));
  if (id == 0) g_forget = true;
  ++built;
  return id;
}

// [waw-decomp-waw] 2026-11-11 round 5: a skinned model's draw for the window's picture (waw_native_vulkan). Its corners
// are new each frame (the game's skinning moves them), so MeshFor (by a key that holds their positions) made a mesh of
// its own each frame, and in run r5a1 that filled ngl_vk's room about every 75 frames: everything was forgotten and
// made again, textures too (8 times in 600 frames; the 3D's setup 36.8 ms a frame on average, 20.6 frames a second).
// Here its triangles are made once by their shape (MeshDraw::shape_key: the stride and the indices) and its corners
// are given for this frame only (ngl_vk_frame_corners, into frame_corners: the draw's corners field); a shape's first
// frame draws with its own corners. MeshFor when the frame's room for corners is used up. Not used for the check's
// pictures (waw_debug_native_side alone), which stay as they were.
uint32_t SkinnedMeshFor(const uint8_t* base, const MeshDraw& m, uint32_t& built, uint32_t& frame_corners) {
  frame_corners = 0;
  const auto found = g_shapes.find(m.shape_key);
  if (found == g_shapes.end()) {  // the shape's first frame: its mesh, with this frame's corners
    Shape& shape = g_shapes[m.shape_key];
    std::vector<float> corners;
    std::vector<uint16_t> indices;
    if (!MeshCorners(base, m, corners, indices, &shape.order)) return 0;  // too many corners: mesh 0, kept so
    shape.mesh = ngl_vk_mesh(corners.data(), static_cast<uint32_t>(corners.size() / NGL_VK_CORNER_FLOATS),
                             indices.data(), static_cast<uint32_t>(indices.size()));
    if (shape.mesh == 0) {
      g_forget = true;
      g_shapes.erase(m.shape_key);
      return 0;
    }
    ++built;
    return shape.mesh;
  }
  const Shape& shape = found->second;
  if (shape.mesh == 0) return 0;
  std::vector<float> corners(shape.order.size() * NGL_VK_CORNER_FLOATS);
  for (size_t i = 0; i < shape.order.size(); ++i) CornerAt(base, m, shape.order[i], &corners[i * NGL_VK_CORNER_FLOATS]);
  frame_corners = ngl_vk_frame_corners(corners.data(), static_cast<uint32_t>(shape.order.size()));
  return frame_corners != 0 ? shape.mesh : MeshFor(base, m, built);
}

// [waw-decomp-waw] 2026-11-11 round 3: the ngl_vk mesh of a layered world draw's second layer (LayerOf), made once: the
// same corners, with the second layer's coordinates (stream 1) in the base texture's place and the corner color's
// green (the second layer's share, the pixel shader's v2.y) as the corner's alpha, for ngl_vk's corner_alpha.
uint32_t LayerMeshFor(const uint8_t* base, const MeshDraw& m, uint32_t& built) {
  const uint64_t key = (m.key ^ 0x4C61796572ull) * 1099511628211ull ^ m.layer_vertices;  // "Layer"
  if (auto found = g_meshes.find(key); found != g_meshes.end()) return found->second;
  uint32_t& id = g_meshes[key];
  id = 0;
  std::unordered_map<uint32_t, uint16_t> local;
  std::vector<float> corners;
  std::vector<uint16_t> indices;
  indices.reserve(m.used.size());
  for (const uint32_t vertex_index : m.used) {
    auto [it, inserted] = local.emplace(vertex_index, static_cast<uint16_t>(corners.size() / NGL_VK_CORNER_FLOATS));
    if (inserted) {
      if (corners.size() / NGL_VK_CORNER_FLOATS >= 65535) return 0;
      const uint32_t v = m.vertices + vertex_index * m.stride;
      const uint32_t l = m.layer_vertices + vertex_index * m.layer_stride + m.layer_uv_offset;
      const uint32_t share = (GpuU32(base, v + 0x10u) >> 8) & 0xFFu;  // the corner color's green (0xAARRGGBB)
      ++g_layer_corners;  // [waw-decomp-waw] 2026-11-11 round 4: the first layer's cut takes red as 1 (BakedTextureFor)
      g_layer_corners_dim += ((GpuU32(base, v + 0x10u) >> 16) & 0xFFu) != 0xFFu;
      const uint32_t color = (share << 24) | 0x00FFFFFFu;
      float color_bits, normal[3];
      std::memcpy(&color_bits, &color, 4);
      UnpackNormal(GpuU32(base, v + 0x24u), normal);
      const float corner[NGL_VK_CORNER_FLOATS] = {GpuF32(base, v), GpuF32(base, v + 4u), GpuF32(base, v + 8u),
                                                  GpuF32(base, l), GpuF32(base, l + 4u),
                                                  GpuF32(base, v + 0x1Cu), GpuF32(base, v + 0x20u), color_bits,
                                                  normal[0], normal[1], normal[2]};
      corners.insert(corners.end(), corner, corner + NGL_VK_CORNER_FLOATS);
    }
    indices.push_back(it->second);
  }
  id = ngl_vk_mesh(corners.data(), static_cast<uint32_t>(corners.size() / NGL_VK_CORNER_FLOATS), indices.data(),
                   static_cast<uint32_t>(indices.size()));
  if (id == 0) g_forget = true;
  ++built;
  return id;
}

// How a pass's pixels use alpha, from its first state word (IW3's GFXS0, as Call of Duty 2's native_side.cpp reads
// it): source blend in bits 0-3 and destination blend in bits 4-7 (0, or ONE and ZERO: not blended), bit 11 set turns
// the alpha test off.
// ---------------------------------------------------------------------------------------------------------------
// [waw-decomp-waw] 2026-11-11 round 5: the 2D for the window's picture (waw_native_vulkan): the HUD, its icons, the
// reticle, text and the menus, drawn by the game from the tess buffers (surface type 16, technique type 4: every one of
// the 42 tess draws of frame 4200 in run r4b1). The tess draw 0x824322B8 hands Direct3D (0x82352E60) the vertex count
// (arguments + 0), the triangle count (arguments + 4), the 16-bit indices at *(state + 0x114C) and the vertices at
// *(state + 0x1150), 32 bytes each laid out as the models' packed ones: +0x00 x, y, z (the screen's pixels for 2D),
// +0x10 the color (a D3DCOLOR), +0x14 the texture coordinates as two 16-bit floats. Each kept draw's corners are copied
// at the draw, with its color map's fetch constant from the device (the slot its pass's "colorMap" names, else slot
// 0). Left out: what reads the game's own GPU pictures (round 4's list: "glow_*", "filter_*", "postfx_*" (the film),
// "feedback*"; and "cinematic", the loading movie), the additive draws ngl_vk's quads can't blend (destination blend
// one: "$additive*", "light_corona"), and those that write no color. "feedbackblend" and "feedbackfilmblend" (the
// world blurred behind a menu) become ngl_vk's blur blanket by their corners' alpha, as Call of Duty 2's 4z60
// (IsBlurredScreen). The screen's size is the largest whole-screen quad's (any 2D draw from 0, 0, kept or not), else
// 1280 x 720 (run r5b1: 1280 x 720, the loading screen's movie quad).
constexpr uint32_t kTechniqueType2D = 4;
constexpr uint32_t kTessStride = 32;
constexpr size_t kMax2DCorners = 98304;          // a frame's (ngl_vk_draw_quads draws them all)
constexpr uint32_t kWritesColor = 0x08000000u;   // GFXS0's color write bit (as Call of Duty 2's 4z19, WritesColor)
constexpr uint32_t kBlendOne = 2;                // GFXS0's blend factor "one"

uint32_t Load16(const uint8_t* base, uint32_t address) {
  uint16_t value;
  std::memcpy(&value, base + address, sizeof(value));
  return __builtin_bswap16(value);
}

void Keep2D(const uint8_t* base, uint32_t state, uint32_t arguments, Frame& f) {
  if (Load32(base, state + kStateTechniqueType) != kTechniqueType2D) return;
  const uint32_t material = Load32(base, state + kStateMaterial), pass = Load32(base, state + kStatePass);
  const uint32_t indices = Load32(base, state + kStateTessIndices);
  const uint32_t vertices = Load32(base, state + kStateTessVertices);
  const uint32_t vertex_count = Readable(arguments) ? Load32(base, arguments) : 0;
  const uint32_t triangles = Readable(arguments) ? Load32(base, arguments + 4u) : 0;
  if (!Readable(material) || !Readable(pass) || !Readable(indices) || !Readable(vertices) || triangles == 0 ||
      vertex_count == 0 || vertex_count > 0x10000u || f.corners_2d.size() + 3u * triangles > kMax2DCorners) {
    ++f.left_unread;
    return;
  }
  // the corners first: a whole-screen quad says how large the screen is, kept or not
  const size_t start = f.corners_2d.size();
  float left = 1e30f, top = 1e30f, right = -1e30f, bottom = -1e30f;
  for (uint32_t i = 0; i < 3u * triangles; ++i) {
    const uint32_t index = Load16(base, indices + 2u * i);
    if (index >= vertex_count) {
      f.corners_2d.resize(start);
      ++f.left_unread;
      return;
    }
    const uint32_t v = vertices + kTessStride * index;
    Corner2D c;
    c.x = LoadF32(base, v);
    c.y = LoadF32(base, v + 4u);
    c.color = Load32(base, v + 0x10u);
    const uint32_t packed = Load32(base, v + 0x14u);
    c.u = HalfToFloat(packed >> 16);
    c.v = HalfToFloat(packed & 0xFFFFu);
    left = std::min(left, c.x), right = std::max(right, c.x), top = std::min(top, c.y), bottom = std::max(bottom, c.y);
    f.corners_2d.push_back(c);
  }
  if (left <= 1.0f && top <= 1.0f && right * bottom > f.screen_width * f.screen_height && right < 8192.0f &&
      bottom < 8192.0f) {
    f.screen_width = right;
    f.screen_height = bottom;
  }
  const std::string name = NameOf(base, material);
  const uint32_t bits = Load32(base, state + kStateBits);
  if (name == "feedbackblend" || name == "feedbackfilmblend")
    f.blur = std::max(f.blur, static_cast<float>(f.corners_2d[start].color >> 24) / 255.0f);
  // "cinematic": the loading screen's movie, a new picture each frame in the video's own planes (run r5b1 decoded them
  // as textures until ngl_vk's room was full); left out, as what the game's GPU made
  const bool gpu_made = name.rfind("glow_", 0) == 0 || name.rfind("filter_", 0) == 0 ||
                        name.rfind("postfx", 0) == 0 || name.rfind("feedback", 0) == 0 || name == "cinematic";
  const bool additive = ((bits >> 4) & 0xFu) == kBlendOne;
  if (gpu_made || additive || (bits & kWritesColor) == 0) {
    f.corners_2d.resize(start);
    ++(gpu_made ? f.left_gpu_made : additive ? f.left_additive : f.left_no_color);
    return;
  }
  Draw2D& d = f.draws_2d.emplace_back();
  d.first_corner = static_cast<uint32_t>(start);
  d.corner_count = 3u * triangles;
  d.material = material;
  d.opaque = (bits & 0xFu) == 0;
  const uint32_t device = Load32(base, state + kStatePrim + kPrimDevice);
  const int slot = ColorMapSlot(base, material, pass);
  if (Readable(device)) DeviceTexture(base, device, slot >= 0 ? uint32_t(slot) : 0u, d.fetch);
}

// The kept 2D as ngl_vk's quads (Keep2D), and on a saved frame one line of what they are.
void Quads2D(const uint8_t* base, uint64_t frame, bool saved, const Frame& kept, std::vector<ngl_vk_vertex>& out) {
  std::string names;
  for (size_t k = 0; k < kept.draws_2d.size(); ++k) {
    const Draw2D& d = kept.draws_2d[k];
    const uint32_t texture = TextureFor(base, d.fetch);
    for (uint32_t i = 0; i < d.corner_count; ++i) {
      const Corner2D& c = kept.corners_2d[d.first_corner + i];
      ngl_vk_vertex v{};
      v.x = c.x;
      v.y = c.y;
      v.u = c.u;
      v.v = c.v;
      v.color = c.color;
      v.textured = texture == 0 ? 0.0f : d.opaque ? 1.0f : 2.0f;  // the color alone, the picture opaque, with alpha
      v.texture = texture;
      out.push_back(v);
    }
    if (saved && k < 10) {
      const Corner2D& c = kept.corners_2d[d.first_corner];
      char text[160];
      std::snprintf(text, sizeof text, "%s\"%s\" (%u corners, the first at %.0f %.0f, color %08X, texture %s)",
                    names.empty() ? "" : ", ", NameOf(base, d.material).c_str(), d.corner_count, c.x, c.y, c.color,
                    texture ? "yes" : "no");
      names += text;
    }
  }
  if (saved) {
    REXLOG_INFO("waw: native Vulkan: frame {}: 2D: {} draws kept ({} corners); left out: {} reading the game's GPU "
                "pictures, {} additive, {} writing no color, {} unread; the screen {:.0f}x{:.0f} from its whole-screen "
                "quads (0: none, 1280x720 taken); blur {:.2f}; the first: {}", frame, kept.draws_2d.size(), out.size(),
                kept.left_gpu_made, kept.left_additive, kept.left_no_color, kept.left_unread, kept.screen_width,
                kept.screen_height, kept.blur, names.empty() ? std::string("none") : names);
  }
}

uint32_t AlphaOf(uint32_t state_bits) {
  const uint32_t source = state_bits & 0xFu, destination = (state_bits >> 4) & 0xFu;
  if (source != 0 && !(source == 2 && destination == 1)) return NGL_VK_ALPHA_BLEND;
  return (state_bits & 0x800u) == 0 ? NGL_VK_ALPHA_TEST : NGL_VK_ALPHA_OPAQUE;
}

// [waw-decomp-waw] 2026-11-11 round 3: a model's light from its block (ModelLightAt) as ngl_vk's irradiance rows and
// their third-degree term (ngl_vk.h irradiance_rows, irradiance_cubic; Call of Duty 2's light grid uses them so). The
// texture's lookup blends the eight texels by (1 +- t.x)(1 +- t.y)(1 +- t.z) / 8, t the normal divided by its
// largest part; taking t as the unit normal itself (equal along the axes, a little less one-sided between them), the
// light 2 x L.rgb is A + B . t + the three products of two parts + D t.x t.y t.z, A, B, C and D sums of the texels
// with the signs of their corners, all in the world's axes. The rows are in the mesh's own space, so they are turned
// by the placement's three rows made unit length (the mesh's axes in the world). The sun's share is the texels'
// alpha blended the shader's way toward the sun, and scales the sun's color.
void ModelLightRows(const MeshDraw& m, bool sun, ngl_vk_mesh_draw& d) {
  float axis[3][3];  // axis[j]: the mesh's axis j in the world, of unit length
  for (int j = 0; j < 3; ++j) {
    const float* row = &m.world[4 * j];
    const float length = std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
    for (int k = 0; k < 3; ++k) axis[j][k] = length > 0.0f ? row[k] / length : (j == k ? 1.0f : 0.0f);
  }
  float a[3] = {}, b[3][3] = {}, pairs[3][3] = {}, d_term[3] = {};  // pairs: xy, yz, zx
  for (int corner = 0; corner < 8; ++corner) {
    float sign[3];
    for (int k = 0; k < 3; ++k) sign[k] = ((corner >> k) & 1) ? 1.0f : -1.0f;
    for (int c = 0; c < 3; ++c) {
      const float v = 2.0f * m.light_texels[corner][c] / 8.0f;  // the shader's 2 x L.rgb, the weights' 1/8
      a[c] += v;
      for (int k = 0; k < 3; ++k) b[k][c] += sign[k] * v;
      pairs[0][c] += sign[0] * sign[1] * v;
      pairs[1][c] += sign[1] * sign[2] * v;
      pairs[2][c] += sign[2] * sign[0] * v;
      d_term[c] += sign[0] * sign[1] * sign[2] * v;
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
    d.irradiance_cubic[c] = d_term[c];
    d.ambient[c] = a[c];
  }
  for (int k = 0; k < 3; ++k)
    for (int j = 0; j < 3; ++j) d.irradiance_cubic_axes[k][j] = axis[j][k];  // world axis k in the mesh's space
  // the sun's share toward the sun: the lookup at the sun's way divided by its largest part
  float toward[3] = {m.sun_direction[0], m.sun_direction[1], m.sun_direction[2]};
  const float largest = std::max({std::fabs(toward[0]), std::fabs(toward[1]), std::fabs(toward[2])});
  float share = 0.0f;
  for (int corner = 0; corner < 8; ++corner) {
    float weight = 1.0f;
    for (int k = 0; k < 3; ++k) {
      const float t = largest > 0.0f ? toward[k] / largest : 0.0f;
      weight *= 0.5f * (1.0f + (((corner >> k) & 1) ? t : -t));
    }
    share += weight * m.light_texels[corner][3];
  }
  for (int k = 0; k < 3; ++k) d.light_color[k] = sun ? d.light_color[k] * std::clamp(share, 0.0f, 1.0f) : 0.0f;
}

// Hands the kept frame's draws to ngl_vk; the summary says what was made.
std::string SetMeshes(const uint8_t* base, const Frame& kept) {
  std::vector<ngl_vk_mesh_draw> draws;
  draws.reserve(kept.meshes.size() + 8);
  const int test = REXCVAR_GET(waw_debug_native_light_test);
  // [waw-decomp-waw] 2026-11-11 round 4: the sky box first (KeepSky, SkyBoxFor), with the first world draw's
  // view-projection; not with the light test's 11
  uint32_t sky_faces = 0, sky_draws = 0;
  const float* world_view = nullptr;  // the first world draw's view-projection, for a sky pass without its own
  for (const MeshDraw& m : kept.meshes)
    if (m.stride == kWorldStride && !m.sky) {
      world_view = m.view_projection;
      break;
    }
  if (kept.sky && test != 11) {
    const auto world = std::find_if(kept.meshes.begin(), kept.meshes.end(),
                                    [](const MeshDraw& m) { return m.stride == kWorldStride; });
    if (const SkyBox* box = SkyBoxFor(base, kept.sky_fetch); box != nullptr && world != kept.meshes.end()) {
      AddSkyBox(*box, world->view_projection, draws);
      sky_faces = 6;
    }
  }
  uint32_t built = 0, failed = 0, textured = 0, lightmapped = 0, blended = 0, models = 0, sunlit = 0;
  uint32_t model_lit = 0;  // models lit by their block of the model lighting texture [waw-decomp-waw] 2026-11-11
  uint32_t layers = 0;     // second layers drawn over layered world draws (LayerOf) [waw-decomp-waw] 2026-11-11
  uint32_t fogged = 0;     // draws given the fog (FogOf) [waw-decomp-waw] 2026-11-11 round 4
  const int only_surface = REXCVAR_GET(waw_debug_native_only_surface);
  for (const MeshDraw& m : kept.meshes) {
    if (only_surface >= 0 && m.surface_type != static_cast<uint32_t>(only_surface)) continue;
    uint32_t frame_corners = 0;  // [waw-decomp-waw] 2026-11-11 round 5: SkinnedMeshFor, with the window's picture
    const uint32_t mesh = NativeVulkanOn() && m.surface_type == kSkinnedSurfaceType
                              ? SkinnedMeshFor(base, m, built, frame_corners)
                              : MeshFor(base, m, built);
    if (mesh == 0) {
      ++failed;
      continue;
    }
    ngl_vk_mesh_draw d{};
    d.mesh = mesh;
    d.corners = frame_corners;
    d.tint = 0x7F7F7Fu;  // a draw without a texture: mid grey (not 0x808080: bit 23 is one of ngl_vk's flags)
    d.texture = test == 3 ? WhiteTexture() : TextureFor(base, m.color_fetch);
    d.alpha = AlphaOf(m.state_bits);
    std::memcpy(d.world, m.world, sizeof(d.world));
    std::memcpy(d.view_projection, m.view_projection, sizeof(d.view_projection));
    // the sun: the way its light travels, and its color (none for a pass without it, or with the test's 1)
    const bool sun = m.sunlit && test != 1;
    const float length = std::sqrt(m.sun_direction[0] * m.sun_direction[0] + m.sun_direction[1] * m.sun_direction[1] +
                                   m.sun_direction[2] * m.sun_direction[2]);
    for (int k = 0; k < 3; ++k) {
      d.light_direction[k] = sun && length > 0.0f ? -m.sun_direction[k] / length : 0.0f;
      d.light_color[k] = sun ? std::clamp(m.sun_color[k], 0.0f, 4.0f) : 0.0f;
    }
    if (m.sky) {
      // [waw-decomp-waw] 2026-11-11 round 4: the sky's model, its texture as it is (sky_noncubemap.hlsl's own
      // numbers, its pass's vertex c63 and code constant 0x12 in c22, not read yet) and fogged as the world is (its
      // pass takes the fog: code constants 0x2A in vertex c21 and 0x2B in pixel c0, run r4c1), scaled out around the
      // eye to kSkyRadius so the world stays in front of it (the game draws it without writing depth, which ngl_vk's
      // opaque draws can't); not with the light test's 11; a cube's sky is the box's (AddSkyBox). Shaded by its
      // corners' color once (white: its texture as it is), as ngl_vk fogs no unlit draw. Run r4c1 drew it unlit and
      // unfogged: a deeper blue than the game's hazy sky, diff 19.9 at 4800 against r4b1's 17.3 without it.
      if (test == 11 || ((m.color_fetch[5] >> 9) & 3u) == 3u || d.texture == 0) continue;
      d.shading = NGL_VK_SHADE_VERTEX_COLOR;
      d.light_once = 1;
      for (int k = 0; k < 3; ++k) d.light_color[k] = 0.0f;
      if (d.alpha == NGL_VK_ALPHA_BLEND) d.blend = NGL_VK_BLEND_ALPHA;
      float reach = 0.0f;  // the placement's largest scale x the model's farthest corner
      for (int j = 0; j < 3; ++j)
        reach = std::max(reach, std::sqrt(m.world[4 * j] * m.world[4 * j] + m.world[4 * j + 1] * m.world[4 * j + 1] +
                                          m.world[4 * j + 2] * m.world[4 * j + 2]));
      reach *= m.sky_radius;
      const float scale = reach > 0.0f ? kSkyRadius / reach : 1.0f;
      for (int k = 0; k < 15; ++k)
        if (k % 4 != 3) d.world[k] *= scale;  // the three axes and the move from the eye: the same ways, farther
      if (m.sky_view_missing) {
        if (world_view == nullptr) continue;
        std::memcpy(d.view_projection, world_view, sizeof(d.view_projection));
      }
      ++sky_draws;
    } else if (m.stride == kWorldStride) {
      // the world: the lightmap's color plus the sun's by the share the lightmap's alpha gives it (LightmapFor)
      d.lightmap = test == 2 ? 0 : LightmapFor(base, m);
      d.shading = d.lightmap ? NGL_VK_SHADE_LIGHTMAP : NGL_VK_SHADE_UNLIT;
      if (d.lightmap) {
        d.lit = 1;
        d.lightmap_scale = test == 5 ? 1.0f : test == 6 ? 4.0f : kLightmapScale;
        for (int k = 0; k < 3; ++k) d.light_color[k] /= d.lightmap_scale;  // the sun itself is not scaled
        ++lightmapped;
      }
    } else {
      // a model: the sun and its block of the model lighting texture (ModelLightRows, [waw-decomp-waw] 2026-11-11
      // round 3), or a flat stand-in for that light when the block couldn't be read (and with the light test's 7)
      d.shading = NGL_VK_SHADE_LIGHT;
      d.lit = 1;
      if (m.model_light && test != 7) {
        ModelLightRows(m, sun, d);
        ++model_lit;
      } else {
        for (int k = 0; k < 3; ++k) d.ambient[k] = kModelAmbient;
      }
      ++models;
    }
    // [waw-decomp-waw] 2026-11-11 round 4: the fog (FogOf; not with the light test's 9), and a layered world draw's
    // first color map with its tint and cut (BakedTextureFor; opaque when cut, black where the cut hides it; not with
    // the light tests 3, 8 and 10)
    if (test != 9) FogOf(m, d);
    fogged += d.fog[1] > d.fog[0];
    const bool layered = m.layer && d.lightmap != 0 && test != 3 && test != 8;
    if (layered && test != 10) {
      const uint32_t baked = BakedTextureFor(base, m.color_fetch, m.tint, m.layer_cut);
      if (baked != 0) d.texture = baked;
      if (baked != 0 && m.layer_cut) d.alpha = NGL_VK_ALPHA_OPAQUE;
    }
    sunlit += sun;
    textured += d.texture != 0;
    blended += d.alpha == NGL_VK_ALPHA_BLEND;
    draws.push_back(d);
    // [waw-decomp-waw] 2026-11-11 round 3: a layered world draw's second layer, drawn again over the first, lit the
    // same way, mixed in by its corners' share (LayerOf); not with the light tests 3 (no color maps) and 8
    if (layered) {
      const uint32_t layer_mesh = LayerMeshFor(base, m, built);
      static constexpr float kNoTint[4] = {1.0f, 1.0f, 1.0f, 1.0f};
      const uint32_t layer_texture = BakedTextureFor(base, m.layer_fetch, test == 10 ? kNoTint : m.layer_tint, false);
      static int logged_alpha = 0;  // [waw-decomp-waw] 2026-11-11 round 4: how much the second map's alpha would take
      if (logged_alpha < 6 && layer_texture != 0) {
        ++logged_alpha;
        REXLOG_INFO("waw: native side: a second layer's color map at {:08X}: average alpha {:.1f} of 255 (its share in "
                    "the game's shader is the corner's green x this; ngl_vk's corner_alpha takes the green alone)",
                    m.layer_fetch[1] & 0x1FFFF000u, AverageAlpha(base, m.layer_fetch));
      }
      if (layer_mesh != 0 && layer_texture != 0) {
        ngl_vk_mesh_draw second = d;
        second.mesh = layer_mesh;
        second.texture = layer_texture;
        second.alpha = NGL_VK_ALPHA_BLEND;
        second.blend = NGL_VK_BLEND_ALPHA;
        second.corner_alpha = 1;
        draws.push_back(second);
        ++layers;
      }
    }
  }
  ngl_vk_set_meshes(draws.data(), static_cast<uint32_t>(draws.size()));
  std::string callers;  // the left-out draws' callers (return addresses) and how many each
  for (const auto& [caller, count] : kept.stale_callers) {
    char text[32];
    std::snprintf(text, sizeof(text), "%s%08X x%u", callers.empty() ? " from " : ", ", caller, count);
    callers += text;
  }
  return std::to_string(draws.size()) + " draws (" + std::to_string(sky_faces) + " sky faces, " +
         std::to_string(sky_draws) + " of the sky's model, " +
         std::to_string(models) + " of models, " +
         std::to_string(model_lit) + " of them by the model lighting texture, " + std::to_string(layers) +
         " second layers (their corners so far " + std::to_string(g_layer_corners) + ", " +
         std::to_string(g_layer_corners_dim) + " with red under 0xFF), " + std::to_string(fogged) + " fogged, " +
         std::to_string(built) + " meshes made now, " + std::to_string(textured) + " with a color map, " +
         std::to_string(lightmapped) + " lightmapped, " + std::to_string(sunlit) + " in the sun, " +
         std::to_string(blended) + " blended, " + std::to_string(failed) + " not made, " +
         std::to_string(kept.dropped) + " dropped, " + std::to_string(kept.no_matrices) +
         " without matrices; " + std::to_string(kept.direct) + " models placed by their own function's registers, " +
         std::to_string(kept.stale) + " left out for an unknown placement" + callers + ") drawn";
}

// [waw-decomp-waw] 2026-11-11 round 5. The colors as the game's picture is shown: through the display's gamma ramp,
// which the game's own picture (the presenter's CaptureGuestOutput) is taken after and ngl_vk's picture is before.
// Read in the generated code (gen.py): 0x823F8E10 (called at 0x824255C8 when r_gamma, the dvar kept at 0x84E4AFE0,
// changed) builds the ramp (0x823F8D28: 256 entries from r_gamma) and hands it to 0x824227D8, which copies it to red,
// green and blue and calls Direct3D's SetGammaRamp 0x823588F0 with the device at *(0x84A84E00 + 8). SetGammaRamp makes
// the ramp's hardware form (0x82363A90), compares it with the 1536 bytes at device + 0x3A9C (`addi r30,r31,15004` at
// 0x82358918) and copies it there when they differ (0x82358998): three runs (red, green, blue) of 256 big-endian
// 16-bit values, of which the display takes the top 10 bits. Call of Duty 4 keeps it at device + 0x3A8C, Call of Duty 2
// at + 0x36DC (its ShownColors, step 4z36, which this is). The table: each channel's 8-bit value for each 8-bit value;
// false, and a plain ramp, when the device isn't there yet or what lies there isn't three runs that never fall.
constexpr uint32_t kGammaDevicePointer = 0x84A84E08u;
constexpr uint32_t kDeviceGammaRamp = 0x3A9Cu;

bool ShownColors(const uint8_t* base, uint8_t table[3][256]) {
  for (int channel = 0; channel < 3; ++channel)
    for (int i = 0; i < 256; ++i) table[channel][i] = static_cast<uint8_t>(i);
  const uint32_t device = Load32(base, kGammaDevicePointer);
  if (device < 0xA0000000u || device >= 0xFFF00000u) return false;
  const uint8_t* p = base + device + kDeviceGammaRamp;
  uint8_t read[3][256];
  for (int channel = 0; channel < 3; ++channel) {
    uint32_t before = 0;
    for (int i = 0; i < 256; ++i) {
      const uint32_t value = uint32_t{p[512 * channel + 2 * i]} << 8 | p[512 * channel + 2 * i + 1];
      if (value < before) return false;
      before = value;
      read[channel][i] = static_cast<uint8_t>(std::lround((value >> 6) * 255.0 / 1023.0));
    }
    if (before < 0x8000u) return false;
  }
  std::memcpy(table, read, sizeof read);
  return true;
}

// Which table the window's pictures go through, logged when that changes (at most 4 times over the run).
void LogShownColors(uint64_t frame, bool from_device, const uint8_t shown[3][256]) {
  static int last = -1, logged = 0;
  if (last == (from_device ? 1 : 0) || logged >= 4) return;
  last = from_device ? 1 : 0;
  ++logged;
  auto at = [&shown](int channel, int value) { return static_cast<int>(shown[channel][value]); };
  REXLOG_INFO("waw: native Vulkan: frame {}: the pictures go through {}: red 8 / 40 / 104 / 152 / 216 / 248 -> {} / "
              "{} / {} / {} / {} / {}, green 104 -> {}, blue 104 -> {}", frame,
              from_device ? "the device's gamma ramp (device + 0x3A9C)" : "a plain ramp (no device or no ramp yet)",
              at(0, 8), at(0, 40), at(0, 104), at(0, 152), at(0, 216), at(0, 248), at(1, 104), at(2, 104));
}

// [waw-decomp-waw] 2026-11-11 round 5: what the frames drawn for the window cost (waw_native_vulkan), record only:
// over every 600 of them, how often everything made was forgotten (ForgetAll: ngl_vk out of room, or half of its
// corners used) and how long the 3D's setup (SetMeshes), the drawing and the hand-over to the window took on average.
struct WindowCost {
  uint64_t frames = 0, forgets = 0, meshes = 0;
  double setup_ms = 0.0, draw_ms = 0.0, show_ms = 0.0;
};
WindowCost g_window_cost;

double MillisecondsSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

// The kept frame drawn into a picture and handed to the helper to save. [waw-decomp-waw] 2026-11-11 round 5: with
// waw_native_vulkan, every frame, handed to the window as the game's picture; saved only on the saved frames.
void DrawFrame(const uint8_t* base, uint64_t frame, const Frame& kept) {
  if (g_state < 0) return;
  const bool window = NativeVulkanOn();  // round 5: this picture is the game's (waw_native_window.h)
  const bool saved = SavedFrame(frame);
  if (g_state == 0) {
    // round 5: with waw_native_vulkan, on the window's own Vulkan device when ngl_vk can use it
    if ((window ? StartNglVkForWindow(kPictureWidth, kPictureHeight) : ngl_vk_init(kPictureWidth, kPictureHeight)) !=
        0) {
      REXLOG_WARN("waw: native side: ngl_vk couldn't start (see its reason on stderr); nothing more is drawn");
      g_state = -1;
      return;
    }
    g_state = 1;
    REXLOG_INFO("waw: native side: ngl_vk started on {}, a {}x{} picture", ngl_vk_device_name(), kPictureWidth,
                kPictureHeight);
  }
  const auto setup_start = std::chrono::steady_clock::now();
  // between frames, on the drawing thread: forgotten when ngl_vk ran out of room, or before a frame could
  uint32_t corners_used = 0, corner_room = 0;
  ngl_vk_room(nullptr, nullptr, &corners_used, &corner_room);
  if (g_forget || corners_used > corner_room / 2) {
    ForgetAll();
    ++g_window_cost.forgets;
  }
  const std::string summary = SetMeshes(base, kept);
  if (!kept.meshes.empty()) {  // behind everything: the fog's color, where the sky will be
    float clear[3];
    for (int k = 0; k < 3; ++k) clear[k] = std::clamp(kept.meshes.front().fog_color[k], 0.0f, 1.0f);
    ngl_vk_set_clear_color(clear);
  }
  const double setup_ms = MillisecondsSince(setup_start);
  const auto draw_start = std::chrono::steady_clock::now();
  // round 5: a picture that goes to the window on the GPU is read back only when it is also saved
  const bool on_gpu = window && PictureOnGpu();
  if (on_gpu) ngl_vk_set_readback(saved ? 1 : 0);
  // round 5: the 2D over the 3D, with the window's picture only (Keep2D, Quads2D); the world blurred behind a menu
  std::vector<ngl_vk_vertex> quads;
  float screen_width = static_cast<float>(kPictureWidth), screen_height = static_cast<float>(kPictureHeight);
  if (window) {
    Quads2D(base, frame, saved, kept, quads);
    if (kept.screen_width >= 320.0f && kept.screen_height >= 200.0f) {
      screen_width = kept.screen_width;
      screen_height = kept.screen_height;
    }
    if (kept.blur > 0.0f && !kept.meshes.empty()) ngl_vk_set_depth_of_field(2.0f, 2.0f, 0.0f, kept.blur);
  }
  const ngl_vk_vertex none{};
  if (ngl_vk_draw_quads(quads.empty() ? &none : quads.data(), static_cast<uint32_t>(quads.size()), screen_width,
                        screen_height, nullptr, 0, 0, 0) != 0) {
    REXLOG_WARN("waw: native side: drawing frame {} failed; nothing more is drawn", frame);
    g_state = -1;
    return;
  }
  const double draw_ms = MillisecondsSince(draw_start);
  const auto show_start = std::chrono::steady_clock::now();
  // round 5: with waw_native_vulkan this picture is the game's: to the window through the game's gamma table, on the
  // GPU (no read back) or through the CPU (waw_native_window.h)
  uint8_t shown[3][256];
  if (window) {
    LogShownColors(frame, ShownColors(base, shown), shown);
    if (on_gpu) ShowOnGpu(shown);
  }
  uint32_t out_width = 0, out_height = 0;
  const uint8_t* pixels = nullptr;
  if (!on_gpu || saved) {
    pixels = static_cast<const uint8_t*>(ngl_vk_target_pixels(&out_width, &out_height));
    if (!pixels || !out_width || !out_height) {
      static int warned = 0;  // round 5: every frame when the window's, so only the first few then
      if (saved || warned++ < 3) REXLOG_WARN("waw: native side: frame {}: ngl_vk gave no picture back", frame);
      return;
    }
    if (window && !on_gpu) ShowPixels(pixels, out_width, out_height, shown);
  }
  if (window) {  // round 5: the cost, every 600 frames
    WindowCost& c = g_window_cost;
    c.setup_ms += setup_ms;
    c.draw_ms += draw_ms;
    c.show_ms += MillisecondsSince(show_start);
    c.meshes += kept.meshes.size();
    if (++c.frames == 600) {
      REXLOG_INFO("waw: native Vulkan: frame {}: over the last 600 frames everything made was forgotten {} times (no "
                  "room); on average {:.0f} 3D draws kept, the 3D's setup {:.2f} ms, the drawing {:.2f} ms, the "
                  "hand-over to the window {:.2f} ms", frame, c.forgets, double(c.meshes) / 600.0, c.setup_ms / 600.0,
                  c.draw_ms / 600.0, c.show_ms / 600.0);
      c = WindowCost{};
    }
  }
  if (!saved) return;  // round 5: drawn for the window alone
  Job job;
  job.frame = frame;
  job.width = out_width;
  job.height = out_height;
  job.rgba.assign(pixels, pixels + size_t(out_width) * out_height * 4);
  job.summary = summary;
  HandOver(std::move(job));
}
#endif  // WAW_NGL_VK

// What the kept frame held: its draws by kind.
void LogFrame(uint64_t frame, const Frame& kept) {
  std::string indexed, tess;
  uint32_t indexed_draws = 0, tess_draws = 0;
  for (const auto& [kind, count] : kept.kinds) {
    char entry[64];
    std::snprintf(entry, sizeof entry, " s%u/t%u:%u(%llu)", std::get<1>(kind), std::get<2>(kind), count.first,
                  static_cast<unsigned long long>(count.second));
    (std::get<0>(kind) ? tess : indexed) += entry;
    (std::get<0>(kind) ? tess_draws : indexed_draws) += count.first;
  }
  REXLOG_INFO("waw: native side: frame {}: {} indexed draws, as surface type/technique type:draws(triangles):{}",
              frame, indexed_draws, indexed.empty() ? " none" : indexed);
  REXLOG_INFO("waw: native side: frame {}: {} draws from the tess buffers (surface type 16: outside the draw "
              "lists):{}", frame, tess_draws, tess.empty() ? " none" : tess);
  // [waw-decomp-waw] 2026-11-11 11:00: the check of the world's lit draws (WorldCheck)
  const WorldCheckCounts& c = kept.world_check;
  std::string declarations;
  for (const auto& [type, count] : c.declaration_types)
    declarations += " " + std::to_string(type) + ":" + std::to_string(count);
  REXLOG_INFO("waw: native side: frame {}: world check: {} draws of surface type 0 and {} of type 2 ({} triangles, {} "
              "with 32-bit indices); {} with an index buffer not 4-byte aligned; the world's own index buffer {}, "
              "another {} (the first {:08X}); triangles with two corners alike, read as before 11:05 {}, now {}; "
              "stream 0 offset = the surface's first vertex x 44: {} yes, {} no, {} surface not found; declaration "
              "types{}; stream 1 set {}, stream 2 set {}", frame, c.draws, c.brush_draws, c.triangles, c.wide,
              c.unaligned, c.own_buffer, c.other_buffer, c.first_other_buffer, c.degenerate_before, c.degenerate_now,
              c.offset_same, c.offset_differs, c.surface_not_found,
              declarations.empty() ? std::string(" none") : declarations, c.stream1, c.stream2);
  // [waw-decomp-waw] 2026-11-11 round 3: the models' light (ModelLightAt) and the layered world draws (LogLayer)
  const ModelLightCounts& l = kept.model_light;
  REXLOG_INFO("waw: native side: frame {}: models' light: place from the command stream {}, from the device {} ({} "
              "found by the fallbacks); not read: no model lighting sampler {}, no texture in it {}, texture not as "
              "expected {}, place outside the texture {}; layered world draws (declaration types 3, 4, 7) {}, with a "
              "second layer found {}, tinted {}, first layer cut {}", frame, l.from_stream, l.from_device,
              l.found_by_search, l.no_sampler, l.no_texture, l.not_volume, l.outside, l.layered, l.layers_found,
              l.tinted, l.cut);
}

// [waw-decomp-waw] 2026-11-11 round 3: the frame's film, record only (for a film step in ngl_vk later; WAW-NATIVE.md
// round 3 has the call it would take). 0x82402978 (read in the decompile; its only caller 0x82403400 hands it the view
// at *(frontEndData + 0xDB838) + index x 0x6DD0) leaves in the view the three constants the post effect's pixel shader
// takes (postfx_color, PC ps_3_0_2acda88e: c7 colorBias, c5 colorTintBase, c6 colorTintDelta):
//     rgb' = (rgb x colorBias.w + lum) x (colorTintBase + colorTintDelta x lum) + colorBias.rgb,
//     lum = dot(rgb, (0.299, 0.587, 0.114)).
// Read at the swap of a kept frame from the front end's data as it is then (the view the front end made last: this
// frame's or the next one's; the film changes slowly). Film off: colorBias 0 0 0 4095, base 1/4096 x 3, delta 0.
void LogFilm(const uint8_t* base, uint64_t frame) {
  const uint32_t data = Load32(base, kFrontEndDataPointer);
  if (!Readable(data) || !Readable(data + kFrontEndViews + 4u)) {
    REXLOG_INFO("waw: native side: frame {}: film: no front end data ({:08X})", frame, data);
    return;
  }
  const uint32_t views = Load32(base, data + kFrontEndViews), index = Load32(base, data + kFrontEndViewIndex);
  const uint32_t count = Load32(base, data + kFrontEndViewCount);
  const uint32_t view = views + kViewBytes * index;
  if (index > 16 || !Readable(views) || !Readable(view + kViewFilmTintDelta + 16u)) {
    REXLOG_INFO("waw: native side: frame {}: film: no view (views {:08X}, index {}, count {})", frame, views, index,
                count);
    return;
  }
  float bias[4], tint_base[4], tint_delta[4];
  for (uint32_t k = 0; k < 4; ++k) {
    bias[k] = LoadF32(base, view + kViewFilmColorBias + 4u * k);
    tint_base[k] = LoadF32(base, view + kViewFilmTintBase + 4u * k);
    tint_delta[k] = LoadF32(base, view + kViewFilmTintDelta + 4u * k);
  }
  REXLOG_INFO("waw: native side: frame {}: film (view {} of {} at {:08X}, enable byte {}): colorBias {:.4f} {:.4f} "
              "{:.4f} {:.4f}, colorTintBase {:.4f} {:.4f} {:.4f}, colorTintDelta {:.4f} {:.4f} {:.4f}", frame, index,
              count, view, static_cast<uint32_t>(base[view + kViewFilmEnable]), bias[0], bias[1], bias[2], bias[3],
              tint_base[0], tint_base[1], tint_base[2], tint_delta[0], tint_delta[1], tint_delta[2]);
}

// [waw-decomp-waw] 2026-11-11 round 4: record only, for drawing the sky and the 2D next. The sky: a draw whose
// technique's name holds "sky" (the mod tools' techniques/sky.tech: stateMap "color_only", sky.hlsl, the material's
// colorMap as its skyMapSampler, looked up by the way from the eye as Call of Duty 2's sky.hlsl does), the first 6 over
// the run. The 2D: every draw from the tess buffers in frame 4200, the first 80. Each with how it was drawn (indexed or
// from the tess buffers, surface type, technique type), its names, triangles, vertex stride, state bits and its color
// map's fetch constant (word 5 bits 9-10: 3 a cube; format; size).
void LogSkyAnd2D(const uint8_t* base, uint32_t state, uint32_t arguments, uint32_t surface_type, bool tess,
                 uint64_t frame) {
  static int logged_sky = 0, logged_2d = 0;
  const uint32_t technique = Load32(base, state + kStateTechnique);
  const std::string technique_name = NameOf(base, technique);
  const bool sky = technique_name.find("sky") != std::string::npos && logged_sky < 6;
  const bool two_d = tess && frame == 4200 && logged_2d < 80;
  if (!sky && !two_d) return;
  logged_sky += sky;
  logged_2d += two_d;
  const uint32_t prim = state + kStatePrim;
  const uint32_t device = Load32(base, prim + kPrimDevice);
  const uint32_t material = Load32(base, state + kStateMaterial), pass = Load32(base, state + kStatePass);
  std::array<uint32_t, 6> fetch{};
  const int slot = Readable(material) && Readable(pass) ? ColorMapSlot(base, material, pass) : -1;
  if (slot >= 0 && Readable(device)) DeviceTexture(base, device, uint32_t(slot), fetch);
  REXLOG_INFO("waw: native side: frame {}: {} {} draw: surface type {}, technique type {} \"{}\", material \"{}\", vs "
              "\"{}\" ps \"{}\"; {} triangles, stride {}, state bits {:08X}; color map in slot {}: fetch {:08X} "
              "{:08X} {:08X} {:08X} {:08X} {:08X} (dimension {}, format {}, {}x{})", frame, sky ? "a sky" : "a 2D",
              tess ? "tess" : "indexed", surface_type, Load32(base, state + kStateTechniqueType), technique_name,
              Readable(material) ? NameOf(base, material) : std::string("?"),
              Readable(pass) ? NameOf(base, Load32(base, pass + kPassVertexShaders +
                                                         4u * (Load32(base, prim + kPrimDeclType) & 15u)))
                             : std::string("?"),
              Readable(pass) ? NameOf(base, Load32(base, pass + kPassPixelShader)) : std::string("?"),
              Readable(arguments) ? Load32(base, arguments + 4u) : 0u, Load32(base, prim + kPrimStreams),
              Load32(base, state + kStateBits), slot, fetch[0], fetch[1], fetch[2], fetch[3], fetch[4], fetch[5],
              (fetch[5] >> 9) & 3u, fetch[1] & 0x3Fu, (fetch[2] & 0x1FFFu) + 1u, ((fetch[2] >> 13) & 0x1FFFu) + 1u);
}

// [waw-decomp-waw] 2026-11-11 round 4: the frame's sky (the first draw whose technique's name holds "sky"): its color
// map's fetch constant, kept when it is a cube (word 5 bits 9-10: 3) for the sky box (SkyBoxFor).
void KeepSky(const uint8_t* base, uint32_t state, Frame& f) {
  if (f.sky || NameOf(base, Load32(base, state + kStateTechnique)).find("sky") == std::string::npos) return;
  const uint32_t device = Load32(base, state + kStatePrim + kPrimDevice);
  const uint32_t material = Load32(base, state + kStateMaterial), pass = Load32(base, state + kStatePass);
  if (!Readable(device) || !Readable(material) || !Readable(pass)) return;
  const int slot = ColorMapSlot(base, material, pass);
  if (slot < 0 || !DeviceTexture(base, device, uint32_t(slot), f.sky_fetch)) return;
  f.sky = ((f.sky_fetch[5] >> 9) & 3u) == 3u;
}

}  // namespace

// [waw-decomp-waw] 2026-11-11 round 5: or when the native picture is the game's (waw_native_vulkan)
bool SideOn() { return REXCVAR_GET(waw_debug_native_side) || NativeVulkanOn(); }

void SideIndexedDraw(const uint8_t* base, uint32_t source, uint32_t state, uint32_t arguments,
                     uint32_t surface_type, uint32_t caller, uint32_t caller_r11) {
  std::lock_guard<std::mutex> lock(g_side_mutex);
  if (std::this_thread::get_id() != g_swap_thread || !DrawnFrame(g_frames + 1) || !Readable(state)) return;
  const bool saved = SavedFrame(g_frames + 1);  // [waw-decomp-waw] 2026-11-11 round 5: the record on these only
  const uint32_t technique_type = Load32(base, state + kStateTechniqueType);
  if (saved) {
    const uint32_t triangles = Readable(arguments) ? Load32(base, arguments + 4u) : 0;
    auto& kind = g_frame.kinds[{0u, surface_type, technique_type}];
    ++kind.first;
    kind.second += triangles;
    LogSkyAnd2D(base, state, arguments, surface_type, false, g_frames + 1);  // [waw-decomp-waw] 2026-11-11 round 4
  }
  KeepSky(base, state, g_frame);
  const bool lit = technique_type >= kFirstLitType && technique_type <= kLastLitType;
  const uint32_t technique = Load32(base, state + kStateTechnique);
  const bool first_pass = Readable(technique) && Load32(base, state + kStatePass) == technique + kTechniquePasses;
  if (!lit || !first_pass) return;
  const uint32_t stride = Load32(base, state + kStatePrim + kPrimStreams);
  const bool world = (surface_type == kWorldSurfaceType || surface_type == kBrushModelSurfaceType) &&
                     stride == kWorldStride;
  const bool model = (surface_type == kStaticModelSurfaceType || surface_type == kRigidModelSurfaceType ||
                      surface_type == kSkinnedSurfaceType) && stride == kModelStride;
  if (!world && !model) return;
  // a static or a rigid model's placement is where its function wrote it for the GPU (kStaticModelDrawReturn);
  // from any other caller the device's copy of the registers may be an earlier draw's, so the draw is left out
  // and counted until that caller is read
  // (any of kPlacedModelDrawReturns since 2026-11-11 09:35, for either type: the five write it the same way; and
  // kSourceMatrixDrawReturn's in the source state's first matrix)
  const bool placed_model = surface_type == kStaticModelSurfaceType || surface_type == kRigidModelSurfaceType;
  const bool by_registers = placed_model && std::find(kPlacedModelDrawReturns.begin(),
                                                      kPlacedModelDrawReturns.end(),
                                                      caller) != kPlacedModelDrawReturns.end();
  const bool by_source = placed_model && caller == kSourceMatrixDrawReturn && Readable(source);
  const bool direct = by_registers || by_source;
  if (placed_model && !direct) {
    ++g_frame.stale;
    ++g_frame.stale_callers[caller];
    static int logged_callers = 0;
    if (logged_callers < 6) {
      ++logged_callers;
      REXLOG_INFO("waw: native side: a model draw of surface type {} from {:08X}, not one of the callers whose "
                  "placement is known: left out", surface_type, caller);
    }
    return;
  }
  g_frame.direct += direct;
  KeepMesh(base, state, arguments, surface_type, technique_type, by_registers ? caller_r11 : by_source ? source : 0,
           by_registers ? caller_r11 : 0, saved, g_frame);
}

void SideTessDraw(const uint8_t* base, uint32_t source, uint32_t state, uint32_t arguments, uint32_t surface_type) {
  (void)source;
  std::lock_guard<std::mutex> lock(g_side_mutex);
  if (std::this_thread::get_id() != g_swap_thread || !DrawnFrame(g_frames + 1) || !Readable(state)) return;
  if (SavedFrame(g_frames + 1)) {  // [waw-decomp-waw] 2026-11-11 round 5: the record on the saved frames only
    auto& kind = g_frame.kinds[{1u, surface_type, Load32(base, state + kStateTechniqueType)}];
    ++kind.first;
    kind.second += Readable(arguments) ? Load32(base, arguments + 4u) : 0;
    LogSkyAnd2D(base, state, arguments, surface_type, true, g_frames + 1);  // [waw-decomp-waw] 2026-11-11 round 4
  }
  KeepSky(base, state, g_frame);
#if defined(WAW_NGL_VK)
  if (NativeVulkanOn()) Keep2D(base, state, arguments, g_frame);  // [waw-decomp-waw] 2026-11-11 round 5
#endif
}

void SideFrameEnd(const uint8_t* base) {
  if (!SideOn()) return;
  std::lock_guard<std::mutex> lock(g_side_mutex);
  const uint64_t frame = ++g_frames;
  const std::thread::id self = std::this_thread::get_id();
  if (self != g_swap_thread) {  // another thread's frame: what was kept belongs to no frame
    g_swap_thread = self;
    g_frame.Clear();
    return;
  }
  if (!DrawnFrame(frame)) return;  // [waw-decomp-waw] 2026-11-11 round 5: every frame with waw_native_vulkan
  if (SavedFrame(frame)) {  // round 5: the record lines on the saved frames only
    LogFrame(frame, g_frame);
    LogFilm(base, frame);  // [waw-decomp-waw] 2026-11-11 round 3, record only
  }
#if defined(WAW_NGL_VK)
  DrawFrame(base, frame, g_frame);
#else
  (void)base;
  if (frame == kKeepEvery)
    REXLOG_INFO("waw: native side: this build has no ngl_vk (Vulkan headers or glslc missing): counts only");
#endif
  g_frame.Clear();
}

void SideStart(const std::filesystem::path& folder, SideCaptureFn capture) {
  if (!SideOn() || g_helper.joinable()) return;
  g_folder = folder;
  g_capture = std::move(capture);
  g_stop = false;
  g_helper = std::thread(RunHelper);
  if (REXCVAR_GET(waw_debug_native_side)) {
    REXLOG_INFO("waw: native side: on; frames {} to {}, every {}, saved in {}", kKeepEvery, kKeepLast, kKeepEvery,
                folder.string());
  } else {  // [waw-decomp-waw] 2026-11-11 round 5
    REXLOG_INFO("waw: native side: on for the native picture in the window (waw_native_vulkan); no pictures saved");
  }
}

void SideStop() {
  {
    std::lock_guard lock(g_jobs_mutex);
    g_stop = true;
  }
  g_jobs_cv.notify_all();
  if (g_helper.joinable()) g_helper.join();
}

}  // namespace waw::native
