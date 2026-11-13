// cod2 - the native renderer's step 3: the frame's 2D drawn natively by ngl_vk, beside the GPU emulation (see
// native_side.h; dev NATIVE-RENDERER.md section 8). [waw-decomp] 2026-11-10
//
// A check only, off by default: the game keeps drawing through the GPU emulation, and with cod2_debug_native_side off
// nothing here runs. With it on, the frames 600, 1200, ... 4800 (counted from when it is on, so about every 10 s for
// the first 80 s, as Call of Duty 3's cod3_debug_ngl_side does) are kept:
//
//   - at each draw (0x82368310, its arguments r3) from the tess buffers (the arguments' first word 0) whose vertices
//     are the 64-byte layout (stride, args + 0x10, 0x40) and lie flat on the screen (every corner's z 0 and w 1, as
//     the stretch pic 0x82371C00 writes them): its triangles, from the 16-bit indices at the pointer 0x83CF8894
//     (3 x the triangle count, args + 0xC), each naming a vertex counted from the tess buffers' start 0x83CE8880 (the
//     stretch pic writes "vertex count + k" there); a corner's x, y at +0x00, its color at +0x1C (a D3DCOLOR,
//     0xAARRGGBB as loaded), its texture coordinate at +0x20 (X360-GAMES.md, "native step 2");
//   - the draw's texture: the image in texture slot 0 (0x83D314FC), its Direct3D texture (image + 4) and that
//     texture's fetch constant (six words at +0x10);
//   - the render target it draws to (id 0x83D31494) and that target's size (0x83D31498 and 0x83D3149C, which
//     0x82363E28 turns into the 2D's scale): the 2D is in the target's pixels. The frame's screen is the largest
//     target its 2D draws to (in a level smaller ones take 2D too: 128x128 in Moscow, run cod2side2); only its 2D is
//     drawn.
//
// At RB_SwapBuffers (0x82372D78) a kept frame's 2D is drawn by ngl_vk (the project's ngl_vk/, as Call of Duty 3's
// and Big Red One's native renderers draw) in the game's order, alpha-blended, into a picture the target's size, and
// saved as userdata/logs/screenshots/ngl-side-<frame>-vk.ppm; a helper thread then saves the game's own picture of
// the same moment (the SDK presenter's CaptureGuestOutput, as cod2_debug_shot_seconds reads it) as
// ngl-side-<frame>-game.png, and one log line names both, for tools/ngl_side_compare.py. Each kept frame logs what
// it found (draws by kind, technique types, targets); the first with 2D lists its first draws, and the first in a level
// (with indexed draws: the world) its first ones to the screen (the HUD).
//
// Step 4's census (2026-11-10 03:20, for the first world drawn natively): each kept frame also counts its indexed draws
// (the world's surfaces and the models, from the bound vertex and index buffers) by kind: technique type, render target
// and the pass's two shaders (pass +0x08 and +0x0C, each record's +0x00 its name), most triangles first. The kept frame
// in which a kind first shows logs its first two draws as they are drawn: the draw's arguments (+0x0C the triangles,
// +0x10 the base vertex, +0x14 the start index), stream 0 (0x83D314CC: the Direct3D vertex buffer, its offset and
// stride; the buffer's vertex fetch constant at +0x0C), the index buffer (0x83D314C8: +0x00 its Common word, bit 31 for
// 32-bit indices, +0x0C its address, +0x10 its size), the first index's vertex position and that position through c0-c3
// (the pass's code constant 0xD2, the world-view-projection; the constants as set are mirrored at 0x83D2F480, 16 bytes
// a register), c0-c3 themselves, and the first four texture slots' images (+0x20 the name) and sizes. From these the
// world's draws get their kind, their vertices and the matrix's layout before they are drawn.
//
// Step 4, the first world drawn natively (2026-11-10 03:35; the census above, run cod2side4, Moscow): the scene's draws
// (target 0) of technique types 8 (lmap_*: the lightmapped world, vertex format 1, 44 bytes: position +0x00, color
// +0x10, texture coordinate +0x14, lightmap coordinate +0x1C; its color map in texture slot 0, its directional lightmap
// in slots 1-5: see WorldLightmapFor), 11 (lprobe_*: rigid models, format 0, 64 bytes: position +0x00, normal +0x10,
// color +0x1C, texture coordinate +0x20, in their own space; color map in slot 2), 17 (lprobe_smc_*: static models, 64
// bytes in a layout of their own: position +0x00, normal +0x0C, color +0x18, texture coordinate +0x1C (MeshFor); cached
// in world space; color map in slot 1) and 14 (lprobe_sm_*: static models drawn one by one, in the rigid models'
// layout (4u), in their own space; color map in slot 1) are kept with their geometry (stream 0's vertex 0 at its
// fetch constant's address plus the stream's offset, the draw's base vertex; the 16-bit or 32-bit indices from the
// start index) and c0-c3: a
// corner's clip position is (p, 1) dotted with each register, so Direct3D's row-vector view-projection is c0-c3 turned
// over (entry [i][r] = c_r[i]; the census's world vertex came out at x -0.17, y -0.23, depth 0.996). The indices are
// copied when the draw is made: every world draw of the census used one small 16-bit index buffer (16,038 bytes, start
// indices up to about 5,000), one the game fills again each frame. Each geometry becomes an ngl_vk mesh once (by its
// vertex buffer, the vertices its indices name and their positions: the static models' cache puts other models where
// earlier ones were) and is drawn under the 2D: the world lightmapped (ngl_vk's lightmap_scale shading, with the sun
// from the pixel constants c16 and c17), the rigid models by their light-probe light's average and the sun (KeepMesh;
// those in their own space with their world matrix, c0-c3 times the inverse of the world's), the static models by
// their light volume, corner by corner (VolumeLightAt: the game's own 3D texture of each model's light from eight
// directions, and the sun, as lprobe_smc's pixel shader does it); alpha-tested or blended as the
// material's state word 0 (material +0x14, IW3's GFXS0) says; the world's corners take format 1's packed normals
// (CheckWorldNormals); everything is fogged as the game fogs it (exponential, kFogLiteral) and the sky (type 1) is a
// box around the eye with the sky cube's faces (SkyBoxFor; its first face's average color when that can't be made); the
// 3D draws from the tess buffers (the effects: smoke, fire, sparks; KeepEffect) and the particle clouds (the falling
// snow; KeepCloud) are drawn unlit, blended or added as their material says; the world's decals (stains on walls,
// type 26 in the world's 44-byte layout) are multiplied onto it as mul's pixel shader does (ED5E78502B611B3F in
// run cod2shaders1: lerp(1, texture x color, the color's alpha), ngl_vk's NGL_VK_BLEND_MULTIPLY with the corners'
// colors times their alpha), and their second pass adds back the fog the first took away (IsDecalFogPass). No
// shadow or gamma ramp yet, nor the decals on models (type 26, 64 bytes).
//
// Textures are decoded on the CPU once each (kept by the Direct3D texture's address and fetch constant), the same
// Xenos formats and tiling Call of Duty 3's ngl_side.cpp decodes; others are drawn as their color alone and named
// once in the log. A build without ngl_vk (no Vulkan headers or glslc) only counts.
//
// The multiplayer program's back end has other addresses, not mapped yet: there the check says so and does nothing.

#include "game/native_addresses.h"  // [waw-decomp] 2026-11-11: each program's guest addresses
#include "game/native_compare.h"  // [waw-decomp] 2026-11-11 4z56: the live A/B compare in the window
#include "game/native_side.h"
#include "game/native_window.h"  // [waw-decomp] 2026-11-11 4z61: the native picture as the game's (cod2_native_vulkan)

#include <rex/cvar.h>

REXCVAR_DEFINE_BOOL(cod2_debug_native_side, false, "cod2/Debug",
                    "Tests: at frames 600, 1200, ... 4800, draw the frame's 2D natively (ngl_vk) and save it beside "
                    "the game's own picture (userdata/logs/screenshots/ngl-side-*), for tools/ngl_side_compare.py");
REXCVAR_DEFINE_INT32(cod2_debug_native_light_test, 0, "cod2/Debug",
                     "Tests: one part of the native world's light changed, to find by pictures where it differs "
                     "from the game's. 0 nothing changed; 1 the world without the sun; 2 the sun on every world "
                     "surface (its share taken as 1); 3 the lightmaps by a flat normal's weights; 4 by a fully "
                     "tilted normal's; 5 the world's highlights ten times as strong; 6 the world without its "
                     "decals; 7 the world's sun and highlights by the corners' normals, not by the normal map; "
                     "8 the world without its highlights; 9 no shadows from the moving models (to compare "
                     "with the game run with sc_enable 0); 10 nothing changed, but each world lightmap's sun "
                     "share (slot 1, as read) and the light made from it saved as pictures "
                     "(screenshots/lightmap-<slot 1's texture>-share.ppm and -light.ppm)");
REXCVAR_DEFINE_INT32(cod2_debug_native_bump_frame, 0, "cod2/Debug",
                     "Tests: changes how the world's normal maps are laid against their surfaces (WorldBumpFrameOf's "
                     "bits, turned over): 1 the two bends change places; 2 the bend along u turned about; 4 the bend "
                     "along v turned about; sums allowed. 0 nothing changed");
REXCVAR_DEFINE_INT32(cod2_debug_native_model_bump_frame, 0, "cod2/Debug",
                     "Tests: the same for the shiny models' normal maps (BumpFrameOf's bits, turned over). 0 nothing "
                     "changed");

// [waw-decomp] 2026-11-11: both programs now (the multiplayer program's addresses: native_addresses.h).

#include <algorithm>
#include <cctype>
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
#include <tuple>
#include <mutex>
#include <string>
#include <system_error>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

#include <rex/logging.h>
#include <rex/ui/presenter.h>

#include <rex/system/kernel_state.h>
#include <rex/system/xmemory.h>  // physical_membase (GpuPointer)

#if defined(COD2_NGL_VK)
extern "C" {
#include "ngl_vk.h"
}
#endif

namespace cod2::native {
namespace {

constexpr uint32_t kTessVertices = guest::kTessVertices;  // the tess buffers' vertices (64 KB: 0x400 of 64 bytes)
constexpr uint32_t kTessIndexPointer = guest::kTessIndexPointer;  // the pointer to its 16-bit indices
constexpr uint32_t kTessMaterial = guest::kTessMaterial;
constexpr uint32_t kTessTechniqueType = guest::kTessTechniqueType;
constexpr uint32_t kTessStride = 0x40u;
constexpr uint32_t kTessMaxVertices = 0x400u;  // the stretch pic flushes before vertex count + 4 passes this
constexpr uint32_t kTessMaxIndices = 0x800u;   // ... or index count + 6 this
constexpr uint32_t kTextureCache = guest::kTextureCache;  // + 4 x slot: the image bound (0x82364F68)
constexpr uint32_t kTargetId = guest::kTargetId;  // the render target set (0x82365828)
// 4z22e: the viewport the game last set, a D3DVIEWPORT9 (x, y, width, height as integers, then the depth range's
// least and most as floats): R_SetViewport 0x82365660 keeps the first four here and hands the whole to
// D3DDevice_SetViewport 0x820B2C00; 0x82365788 sets the depth range alone to [0, its argument], called twice from
// 0x8236EA18 (in Libya the tank the player rides is drawn in a range of its own, it seems, so it stays in front)
constexpr uint32_t kViewportCache = guest::kViewportCache;
constexpr uint32_t kTargetWidth = guest::kTargetWidth;  // and its size, in pixels
constexpr uint32_t kTargetHeight = guest::kTargetHeight;
constexpr uint32_t kTextureFetch = 0x10u;        // the fetch constant in a Direct3D texture (Call of Duty 3's: 0x1C)
constexpr uint32_t kIndexCache = guest::kIndexCache;  // the index buffer bound (0x82364E88)
constexpr uint32_t kStreamCache = guest::kStreamCache;  // + 12 x stream: buffer, offset, stride (0x82364DC8)
constexpr uint32_t kConstantMirror = guest::kConstantMirror;  // the shader constants as set, 16 bytes a register
                                                              // (0x82367CB0)
constexpr int kIndexedSamples = 2;                 // logged per kind: its first draws, in whichever kept frame
constexpr size_t kListedKinds = 20;
// step 4: the 3D drawn natively
constexpr uint32_t kWorldType = 8;         // lmap_*: the lightmapped world
constexpr uint32_t kModelType = 11;        // lprobe_*: rigid models, in their own space
constexpr uint32_t kStaticModelType = 17;  // lprobe_smc_*: static models, in world space
constexpr uint32_t kCloudType = 26;        // with 20-byte vertices: particle_cloud_outdoor (KeepCloud)
constexpr uint32_t kCloudStride = 20;
constexpr uint32_t kDecalStride = 44;      // type 26 with the world's layout: its decals (mul.hlsl, multiplied)
constexpr uint32_t kLooseModelType = 14;   // lprobe_sm_*: static models drawn one by one, in their own space
constexpr uint32_t kSkyType = 1;           // sky.hlsl: the sky, a cube map looked up by direction
// 4z44: the four lit kinds above are the last of three technique types each, one for each fog a level can have:
// none (the world's 6, vertex shaders "lmap" and "lmap_s": El Alamein's trenches at night, run cod2side119),
// linear (7, "lmap_s_lfog": the Toujane ride, run cod2side120) and exponential (8, "lmap_s_efog": Moscow, Libya,
// Pointe du Hoc, the Rhine). The models run 9 10 11, the loose static models 12 13 14, the static models 15 16 17.
// With only the last of each kept, a level of the first two fogs showed its sky and effects and nothing else.
// 4z46: the clouds and the world's decals run 24 25 26 the same way (the crossroads' "mul" and "mul_pfog" decals
// are 25, run cod2side127).
enum FogKind : int { kFogNone = 0, kFogLinear = 1, kFogExponential = 2 };
struct TechniqueKind {
  uint32_t type;  // the kind's last type (kWorldType, kModelType ...), or the type itself when it is none of them
  int fog;        // FogKind; exponential for a type outside them
};
TechniqueKind KindOf(uint32_t type) {
  for (const uint32_t last : {kWorldType, kModelType, kLooseModelType, kStaticModelType, kCloudType})
    if (type + 2u >= last && type <= last) return {last, static_cast<int>(type + 2u - last)};
  return {type, kFogExponential};
}
int g_fog_kind = kFogExponential;  // the last lit draw's (SideDraw), for the effects, which have one type for all
// 4z45. A linear fog's start and end from vertex c11: what stays of the color is c11.x x the distance + c11.y,
// saturated (the Toujane ride, run cod2side123: -0.000105263 and 1.05263, all of it up to 500 and none from 10,000).
// False when the two numbers are no such pair.
bool LinearFog(float scale, float bias, float& start, float& end) {
  if (!(scale < 0.0f) || !(bias > 0.0f)) return false;
  start = (1.0f - bias) / scale;
  end = -bias / scale;
  return end > start && end > 0.0f;
}
// Whether a shader's name has a part ("s": a specular map, "a": alpha tested ...) between its underscores.
// "lmap_s_dtl_pfog.hlsl" has "s"; so has "lmap_s.hlsl", a level without fog's (4z44), where it is the last part.
bool HasPart(const std::string& name, const char* part) {
  const std::string stem = name.substr(0, name.find('.'));
  const std::string wanted = std::string("_") + part;
  for (size_t at = stem.find(wanted); at != std::string::npos; at = stem.find(wanted, at + 1)) {
    const size_t after = at + wanted.size();
    if (after == stem.size() || stem[after] == '_') return true;
  }
  return false;
}
// The fog: the vertex shaders (such as A4910235DE9553D3, E69A891EC85B5C7B in run cod2shaders1) make its share as
// c255.x - 2^(c11.z x c255.y x the distance from the eye), saturated, c255 one of the shader's own literals; the pixel
// shaders mix the lit color toward c21 by it. Moscow's c11 is (-1, 1, -0.00035, 0) (run cod2side7): c11.z is minus
// the density. The literal is (1, log2(e)): it is stored just before a fogged vertex shader of the same kind in the
// level's fast file (moscow.ff, found by its microcode: ff_shader_find.py, 06:42), the way HLSL's exp(x) is compiled
// (2^(x log2 e)), so the share is 1 - exp(-density x d). The distance is the length of the corner's place in view
// space (the shader's c8-c10), not the view's depth: ngl_vk's mesh.vert measures it from the eye.
constexpr float kFogLiteral = 1.442695f;
constexpr uint32_t kMeshSlots = 11;        // the texture slots kept a draw (the world's shaders use 0-7, 4z38: with
                                           // a specular map and a detail map 0-10)
constexpr uint32_t kPixelConstants = 256;  // the pixel shader's c0 in the constant mirror (vertex c0-c255 first)
constexpr uint32_t kSceneTarget = 0;
constexpr size_t kMaxMeshDraws = 16000;    // a kept frame's (ngl_vk takes 16384)
constexpr uint32_t kMaxMeshTriangles = 0x20000u;  // one draw's; more is taken as a misread
constexpr uint32_t kImageStart = 0x82000000u;
constexpr uint32_t kImageEnd = guest::kImageEnd;

constexpr uint64_t kKeepEvery = 600;  // frames
constexpr uint64_t kKeepLast = 4800;
constexpr size_t kMaxCorners = 120000;  // a kept frame's; more are counted as dropped
constexpr int kListedDraws = 24;        // listed from the first kept frame with 2D

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
    const unsigned char c = base[addr + i];
    if (c == 0) return out;
    if (c < 32 || c > 126) return "?";
    out += char(c);
  }
  return out;
}

bool KeptFrame(uint64_t frame) { return frame > 0 && frame % kKeepEvery == 0 && frame <= kKeepLast; }
// 4z56: a frame whose pictures are saved (the check, cod2_debug_native_side), and a frame drawn natively at all (those,
// or every frame while the live compare is on: game/native_compare.h)
bool SavedFrame(uint64_t frame) { return REXCVAR_GET(cod2_debug_native_side) && KeptFrame(frame); }
// 4z61: and every frame when the native picture is the game's (cod2_native_vulkan, native_window.h)
bool DrawnFrame(uint64_t frame) { return SavedFrame(frame) || compare::Wanted() || NativeVulkanOn(); }
// 4z56: true while a frame is drawn for the live compare alone: its lines aren't logged (one a frame would flood the
// log), but the ones logged a few times only still are
bool g_quiet = false;

constexpr uint32_t kDefaultWidth = 1280, kDefaultHeight = 720;  // when no 2D draw names a sane target size

struct Corner {
  float x, y;  // the target's pixels
  float u, v;
  uint32_t color;  // 0xAARRGGBB
};

struct Draw {
  uint32_t first_corner = 0;  // into Frame::corners
  uint32_t corner_count = 0;
  uint32_t technique_type = 0;
  uint32_t material = 0;
  uint32_t target = 0, target_width = 0, target_height = 0;
  uint32_t first_vertex = 0;  // args + 4, for the list
  uint32_t lowest_index = 0, highest_index = 0;
  uint32_t texture = 0;  // the Direct3D texture, 0 for none
  std::array<uint32_t, 6> fetch{};
  uint32_t meshes_before = 0;  // 4z22d: how many 3D draws the frame had kept when this one was drawn
};

// One corner of an effect (step 4i): as the tess buffers hold it, copied when it is drawn.
struct EffectCorner {
  float x = 0, y = 0, z = 0, u = 0, v = 0;
  uint32_t color = 0;  // a D3DCOLOR, 0xAARRGGBB
};

// One 3D draw kept for ngl_vk (step 4): its geometry, its textures and its view-projection.
struct MeshDraw {
  uint32_t type = 0;           // the technique type
  uint32_t vertices = 0;       // the physical address of stream 0's vertex 0
  uint32_t stride = 0;
  std::vector<uint32_t> used;  // the vertices its triangles use, three a triangle (the base vertex added)
  uint64_t key = 0;            // its geometry's fingerprint: the vertex buffer, the stride and those vertices
  uint32_t state0 = 0;         // the material's state word 0
  uint32_t color_slot = 0;     // its color map's slot
  std::array<uint32_t, kMeshSlots> textures{};  // the Direct3D texture in each slot (0: none)
  std::array<std::array<uint32_t, 6>, kMeshSlots> fetches{};
  float view_projection[16] = {};
  float sun_direction[3] = {}, sun_color[3] = {};  // the pixel shader's c16 and c17 (the world's)
  float fog_exponent = 0.0f;   // the vertex shader's c11.z x kFogLiteral (below 0: fogged)
  float fog_start = 0.0f, fog_end = 0.0f;  // 4z45: a linear fog's distances from the eye (LinearFog); end 0: none
  float ambient[3] = {};       // a model's: its light-probe light averaged over directions (KeepMesh)
  float sun_scale = 1.0f;      // a model's: c17.w, its sun's strength
  float grid[6][4] = {};       // a model's: the pixel shader's c8-c13, its light grid colors (red's two, green's, blue's)
  float fog_color[3] = {};     // the pixel shader's c21
  float volume_step[3] = {};   // a static model's: the pixel shader's c23, its light volume's step toward the normal
  float to_world[9] = {};      // a static model's: the vertex shader's c4-c6, its normals into world space (rows)
  float volume_center[3] = {};  // a type 14 model's: the vertex shader's c23, its block's center in the light volume
  bool lit_all_round = false;  // a static model with an "_amb" pixel shader (the pine canopies): no normal (4u)
  float feather = 0.0f;        // an effect with a zfeather shader: the vertex shader's c25.x, its soft scale (4y)
  float vertex_sun[3] = {};    // and the vertex shader's c17, the sun's color its corners take
  bool water = false;          // 4z26: drawn by a water pixel shader (KeepMesh): height map in slot 0, cube in slot 1
  float water_color[3] = {};   // and its pixel shader's c23, the water's own color
  // 4z38: the slot of the detail map a "_dtl" pixel shader adds to the color map (0: none) and the map's repeat
  // (pixel c23's x and y, the PC copy's detailScale)
  uint32_t detail_slot = 0;
  float detail_repeat[2] = {};
  bool shiny = false;          // 4z30: a model drawn by an "_s" pixel shader (KeepMesh): normal map, specular map, lookup
  float specular_color[3] = {};  // and its pixel shader's c19, the light's specular color
  bool world_shiny = false;      // 4z43: a world surface drawn by an "lmap..._s_" pixel shader (KeepMesh)
  float world_shadowed = 0.0f;   // and its pixel shader's c255.x, a highlight's share where the sun doesn't reach
  float eye[3] = {};           // the sky's: the vertex shader's c20, the eye's place (sky.hlsl: the way from it)
  float sky_radius = 0.0f;     // the sky's: its farthest corner from the eye
  std::vector<EffectCorner> effect;  // an effect's corners, three a triangle (KeepEffect); empty for the others
  bool corner_weighted = false;  // a world layer with an "lmap_a" pixel shader: blended by its corners' alpha (4z12)
  // 4z51: a decal's second pass (IsDecalFogPass), which gives back the fog its first pass took away: added, its
  // color (alpha - texture x color x alpha) x the fog's share at the corner x the fog's color (the vertex shader's
  // c21)
  bool decal_fog = false;
  float decal_fog_color[3] = {};
  uint32_t material = 0;       // its material (4z16: named in the covering draws' log, CoverOf)
  uint32_t pass = 0;           // its technique's pass, 0 for a particle cloud (+0xC its pixel shader)
  float kept_position[3] = {};  // 4z20: its first used corner's position when it was kept (KeepMesh), to compare
  int32_t viewport[4] = {};       // 4z22e: the viewport it was drawn in (kViewportCache): x, y, width, height
  float depth_low = 0.0f, depth_high = 1.0f;  // and its depth range
  uint32_t live_state[3] = {};  // 4z22g: the render state caches when it was drawn (0x83D31480, word 0, word 1)
};

struct Frame {
  std::vector<Corner> corners;
  std::vector<MeshDraw> meshes;  // step 4
  uint32_t meshes_dropped = 0;
  std::vector<Draw> draws;
  uint32_t seen = 0;           // draws
  uint32_t indexed = 0;        // not from the tess buffers
  uint32_t other_stride = 0;   // from them, another vertex layout
  uint32_t not_flat = 0;       // a corner's z not 0 or w not 1: not 2D
  uint32_t bad_indices = 0;    // no index list, or an index past the buffers
  uint32_t dropped = 0;        // past kMaxCorners
  std::string first_not_flat;  // the first such draw's material, for the log
  // step 4's census: the indexed draws by kind (technique type, target, vertex shader record, pixel shader record)
  struct Kind {
    uint32_t draws = 0, triangles = 0;
    int samples = 0;
  };
  std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, Kind> kinds;

  void Clear() {
    corners.clear();
    draws.clear();
    seen = indexed = other_stride = not_flat = bad_indices = dropped = 0;
    first_not_flat.clear();
    kinds.clear();
    meshes.clear();
    meshes_dropped = 0;
  }

  // The frame's screen: the largest render target its 2D draws to (at least 320 x 200), else 1280 x 720.
  void Screen(uint32_t& width, uint32_t& height) const {
    width = kDefaultWidth;
    height = kDefaultHeight;
    uint64_t largest = 0;
    for (const Draw& d : draws) {
      const uint64_t area = uint64_t(d.target_width) * d.target_height;
      if (d.target_width >= 320 && d.target_height >= 200 && area > largest) {
        largest = area;
        width = d.target_width;
        height = d.target_height;
      }
    }
  }
};

// Only the render thread (the draws and RB_SwapBuffers run there) touches these.
Frame g_frame;
std::atomic<uint64_t> g_frames{0};
std::atomic<bool> g_have_thread{false};
std::thread::id g_thread;
std::mutex g_thread_mutex;
bool g_listed_menu = false;   // the first frame with 2D listed its draws
bool g_listed_level = false;  // the first in a level (with indexed draws) listed its draws to the screen
// each kind of indexed draw (technique type, target, vertex and pixel shader) sampled so far, over all kept frames
std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, int> g_samples;

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

// The Direct3D texture in a slot (its image's +0x04) and its fetch constant; false when the slot is empty.
bool SlotTexture(const uint8_t* base, uint32_t slot, uint32_t& texture, std::array<uint32_t, 6>& fetch) {
  const uint32_t image = LoadU32(base, kTextureCache + 4u * slot);
  texture = IsGuestData(image) ? LoadU32(base, image + 4u) : 0;
  if (!IsGuestData(texture)) {
    texture = 0;
    return false;
  }
  for (uint32_t w = 0; w < 6; ++w) fetch[w] = LoadU32(base, texture + kTextureFetch + 4u * w);
  return true;
}

// Where the 64-byte layouts keep a corner's normal, color and texture coordinate. The static models' cache (type 17;
// vertex shaders B335C2F94428DDF8, 331D5525697CF6B6 in run cod2shaders1) has its own: position as three floats, normal
// +0x0C, color +0x18, texture coordinate +0x1C, lighting coordinate +0x24. The rigid models (type 11) and, 4u, the
// static models drawn one by one (type 14; vertex shaders 12DF58A89DC165B5, 60605A7F72D1B2CF: four floats, Offset 4, 7
// and 8 in words) have the rigid one: normal +0x10, color +0x1C, texture coordinate +0x20 (run cod2side28: type 14's
// +0x24 read as two 16-bit numbers gave 16087..16253 high, a float's top half between 0.4 and 1: a coordinate's v).
uint32_t NormalAt(uint32_t type) { return type == kStaticModelType ? 0x0Cu : 0x10u; }
uint32_t ColorAt(uint32_t type) { return type == kStaticModelType ? 0x18u : 0x1Cu; }
uint32_t UvAt(uint32_t type) { return type == kStaticModelType ? 0x1Cu : 0x20u; }

// Step 4: keeps one indexed draw of the scene (the world or a model) for ngl_vk; pass: its technique's pass (+0xC its
// pixel shader, whose first word points to its name).
// 4z22e: the viewport and depth range a 3D draw is made in (kViewportCache)
void KeepViewport(const uint8_t* base, MeshDraw& m) {
  for (uint32_t k = 0; k < 4; ++k) m.viewport[k] = static_cast<int32_t>(LoadU32(base, kViewportCache + 4u * k));
  m.depth_low = LoadF32(base, kViewportCache + 0x10u);
  m.depth_high = LoadF32(base, kViewportCache + 0x14u);
  if (!(m.depth_low >= 0.0f && m.depth_low <= 1.0f)) m.depth_low = 0.0f;  // not set (or not a number): the whole
  if (!(m.depth_high > m.depth_low && m.depth_high <= 1.0f)) m.depth_high = 1.0f;
  // 4z22g: and the render state the game has set for it (native_capture.cpp's caches: 0x83D31480, then state word 0
  // at 0x83D31488 and word 1 at 0x83D3148C, IW3's GFXS1: depth test and depth write among its bits)
  m.live_state[0] = LoadU32(base, guest::kRenderState);
  m.live_state[1] = LoadU32(base, guest::kStateWord0);
  m.live_state[2] = LoadU32(base, guest::kStateWord1);
}

void KeepMesh(const uint8_t* base, uint32_t args, uint32_t pass, uint32_t type, Frame& f) {
  if (f.meshes.size() >= kMaxMeshDraws) {
    ++f.meshes_dropped;
    return;
  }
  const uint32_t buffer = LoadU32(base, kStreamCache), offset = LoadU32(base, kStreamCache + 4u),
                 stride = LoadU32(base, kStreamCache + 8u);
  const uint32_t indices = LoadU32(base, kIndexCache);
  const uint32_t material = LoadU32(base, kTessMaterial);
  if (!IsGuestData(buffer) || !IsGuestData(indices) || !IsGuestData(material) || (stride != 44 && stride != 64)) {
    ++f.meshes_dropped;
    return;
  }
  const uint32_t triangles = LoadU32(base, args + 0xCu);
  if (triangles == 0 || triangles > kMaxMeshTriangles) {
    ++f.meshes_dropped;
    return;
  }
  MeshDraw m;
  m.type = type;
  m.vertices = (LoadU32(base, buffer + 0xCu) & 0x1FFFFFFCu) + offset;
  m.stride = stride;
  // the indices now, before the game fills their buffer again; the fingerprint is FNV-1a over the words and each used
  // vertex's position: the static models' cache (type 17's one buffer) puts other models where earlier ones were, so
  // the same corner numbers can be another model in a later frame (run cod2side9: crates drawn where rocks were)
  const uint32_t base_vertex = LoadU32(base, args + 0x10u);
  const bool wide = (LoadU32(base, indices) >> 31) != 0;
  const uint32_t first = (LoadU32(base, indices + 0xCu) & 0x1FFFFFFFu) + (wide ? 4u : 2u) * LoadU32(base, args + 0x14u);
  uint64_t key = 14695981039346656037ull;
  auto mix = [&key](uint32_t word) {
    for (int b = 0; b < 4; ++b) key = (key ^ ((word >> (8 * b)) & 0xFFu)) * 1099511628211ull;
  };
  mix(m.vertices);
  mix(m.stride);
  // a static model's corners also by their lighting coordinate (+0x24): a model given another place in the light
  // volume is lit anew (VolumeLightAt)
  const bool static_model = stride == 64 && (type == kStaticModelType || type == kLooseModelType);
  m.used.resize(3u * triangles);
  for (uint32_t i = 0; i < 3u * triangles; ++i) {
    m.used[i] = base_vertex + (wide ? GpuU32(base, first + 4u * i) : GpuU16(base, first + 2u * i));
    mix(m.used[i]);
    for (uint32_t k = 0; k < 3; ++k) mix(GpuU32(base, m.vertices + m.used[i] * stride + 4u * k));
    if (static_model) mix(GpuU32(base, m.vertices + m.used[i] * stride + 0x24u));
  }
  m.key = key;
  m.state0 = LoadU32(base, material + 0x14u);
  m.color_slot = type == kWorldType || type == kSkyType || type == kCloudType ? 0u : type == kModelType ? 2u : 1u;
  // 4z26: the water (Pointe du Hoc's sea, material water_duhoc) comes as technique type 11 with the world's 44-byte
  // corners and the shaders water_l_sun_*: the game's water.hlsl (the PC copy's main/iw_07.iwd has its text; on the
  // Xbox 360 pixel shader DD12484223A211FC in run cod2shaders2 does the same step by step). Slot 0 is its height map
  // (watersetup0, 64 x 64), slot 1 a cube of the sky, pixel c23 the water's color; the sun is c16 and c17 as for the
  // rest. Kept as water, not as a model: ngl_vk's water_cube draws it (SetMeshes). Known by the pixel shader's name.
  if (type == kModelType && stride == 44 && IsGuestData(pass)) {
    const uint32_t ps = LoadU32(base, pass + 0xCu);
    if (IsGuestData(ps) && Text(base, LoadU32(base, ps)).rfind("water", 0) == 0) {
      m.water = true;
      m.color_slot = 0;
      for (uint32_t k = 0; k < 3; ++k)
        m.water_color[k] = LoadF32(base, kConstantMirror + 16u * (kPixelConstants + 23u) + 4u * k);
    }
  }
  for (uint32_t slot = 0; slot < kMeshSlots; ++slot) SlotTexture(base, slot, m.textures[slot], m.fetches[slot]);
  // 4z38: the pixel shaders with "_dtl" in their names add a detail map to the color map before the light (the PC
  // copy's shade.hlsl: sample.rgb += tex2D(detailMapSampler, texCoords * detailScale.xy).rgb - 0.5; on the 360
  // lmap_s_dtl_pfog 6C7F0E21C61DF68F: r8.xy = the coordinates x c23.xy, fetch, - c255.w, + the color map). Left out,
  // Libya's sand came out 6 % too light and grey and Moscow's snow 4 % too dark (4z36's like-for-like compare). The
  // map is the shader's last slot (run cod2side102 / 103, by the textures' names: metal_01, rock_wall_01, wood_h_01,
  // detail_mud_01): the world's lmap shaders 8, with a specular map ("_s_") 10; the models' lprobe shaders 7 with a
  // specular map, 5 without (lprobe_dtl_pfog 9B1479CA71BA71E6 fetches slots 0 to 5; slot 4 is its normal map, as
  // run cod2side104 showed by its name). 4z40: the static models' lprobe_smc shaders, read the same way
  // (lprobe_smc_dtl_pfog 2E1F7BB17B21C0AA: r0.zw = the coordinates x c24.xy, fetch slot 3, - c254.w, + the color
  // map of slot 1; lprobe_smc_a_dtl_pfog the same; lprobe_smc_s_dtl_pfog F5B8E5...: slot 5): slot 3, with a specular
  // map 5, and the repeat is c24 there (c23 is their light's 0.002 to 0.008). 4z41: lprobe_smc_a_s_dtl_pfog
  // (982EBB...) fetches its detail (the fetch at r0.zw) from slot 4, its highlights' lookup from 5; run cod2side110
  // showed its c24 as 8 8 like the others'.
  if (IsGuestData(pass)) {
    const uint32_t ps = LoadU32(base, pass + 0xCu);
    const std::string ps_name = IsGuestData(ps) ? Text(base, LoadU32(base, ps)) : std::string();
    if (ps_name.find("_dtl") != std::string::npos) {
      const bool specular = HasPart(ps_name, "s");
      if (ps_name.rfind("lmap", 0) == 0) {
        m.detail_slot = specular ? 10 : 8;
      } else if (ps_name.rfind("lprobe_sm", 0) == 0) {
        // 4z49: the loose static models' "lprobe_sm_" shaders have the "lprobe_smc_" ones' slots (0 the light
        // volume, 1 the color map, 2 the normal map, 3 the specular map; run cod2side149's samples). Taken for
        // the models' they got slot 7, whatever an earlier draw had left there, and c23, their light's step
        // (0.004), as the repeat: decoytown's and the crossroads' yellow and blue props.
        m.detail_slot = HasPart(ps_name, "a") && specular ? 4 : specular ? 5 : 3;
      } else if (ps_name.rfind("lprobe", 0) == 0) {
        m.detail_slot = specular ? 7 : 5;
      }
      const uint32_t repeat_constant = ps_name.rfind("lprobe_sm", 0) == 0 ? 24u : 23u;
      for (uint32_t k = 0; k < 2; ++k)
        m.detail_repeat[k] = LoadF32(base, kConstantMirror + 16u * (kPixelConstants + repeat_constant) + 4u * k);
      // a repeat that is no repeat (the maps seen so far run 2 to 32 times across the color map) means the slot
      // or the constant was misread: better no detail than a wrong picture added to the color
      const bool sane = m.detail_repeat[0] >= 0.25f && m.detail_repeat[0] <= 1024.0f &&
                        m.detail_repeat[1] >= 0.25f && m.detail_repeat[1] <= 1024.0f;
      if (!sane) m.detail_slot = 0;
      static std::vector<std::string> detail_logged;
      if (detail_logged.size() < 12 &&
          std::find(detail_logged.begin(), detail_logged.end(), ps_name) == detail_logged.end()) {
        detail_logged.push_back(ps_name);
        const uint32_t image = m.detail_slot != 0 ? LoadU32(base, kTextureCache + 4u * m.detail_slot) : 0;
        REXLOG_INFO("cod2: native side: detail map: ps \"{}\", material \"{}\": slot {} \"{}\", repeat {:.3f} {:.3f}",
                    ps_name, Text(base, LoadU32(base, material)), m.detail_slot,
                    IsGuestData(image) ? Text(base, LoadU32(base, image + 0x20u)) : std::string("-"),
                    m.detail_repeat[0], m.detail_repeat[1]);
      }
    }
  }
  // 4z30: the models' "_s" pixel shaders (the soldiers' lprobe_s_pfog, 98A77B2DFA1049B4 in run cod2shaders1; the PC
  // copy's lightprobe.hlsl with its specular part on) read the normal map from slot 4, the specular map from slot 5
  // and the highlights' lookup picture ("$specularity", 32 x 256) from slot 6, and take the light's specular color
  // from pixel c19 (run cod2side85: 0.622 0.622 0.778). Known by the pixel shader's name; drawn so in SetMeshes.
  if (type == kModelType && stride == 64 && IsGuestData(pass)) {
    const uint32_t ps = LoadU32(base, pass + 0xCu);
    if (IsGuestData(ps) && HasPart(Text(base, LoadU32(base, ps)), "s")) {
      m.shiny = true;
      for (uint32_t k = 0; k < 3; ++k)
        m.specular_color[k] = LoadF32(base, kConstantMirror + 16u * (kPixelConstants + 19u) + 4u * k);
    }
  }
  // 4z12: the world's layers drawn over others (Moscow's snow over the trodden ground, stalingradwinter_snow02a; the
  // wall grime) have "lmap_a" pixel shaders (lmap_a_pfog E5DE90C17FC2B7A9, lmap_a_s_dtl_pfog 55952CA35B5D0429 in run
  // cod2shaders1): the color map times the corners' color, all four parts (r1 = r10 x r6), and its alpha goes out
  // (oC0.w = r1.w), so such a layer is blended by the color map's alpha times its corners'. The other world shaders
  // write alpha 1 (oC0.xyz1). Logged for the first ones: their color map's format and their corners' alpha.
  if (type == kWorldType) {
    const uint32_t ps = IsGuestData(pass) ? LoadU32(base, pass + 0xCu) : 0;
    m.corner_weighted = IsGuestData(ps) && Text(base, LoadU32(base, ps)).rfind("lmap_a", 0) == 0;
    // 4z43: the world's "_s" pixel shaders (Moscow's ground and walls are nearly all lmap_s_dtl_pfog,
    // 6C7F0E21C61DF68F) add a highlight to the lit color: the specular map (slot 8, at the color map's coordinates)
    // times pixel c19 times the highlights' lookup (slot 9, the "$specularity" picture the models use, read at the
    // specular map's alpha and N . H) times (the sun's share x the shadow cookie x c255.y + c255.x); its
    // instructions 14, 17, 29 to 31 and 39. Left out, Moscow's snow came out 9 % darker than the game's and its
    // walls 15 % (run cod2side113, a grid of frame 4800). Drawn so in SetMeshes (ngl_vk.h specular_lookup).
    const std::string world_ps = IsGuestData(ps) ? Text(base, LoadU32(base, ps)) : std::string();
    if (world_ps.rfind("lmap", 0) == 0 && HasPart(world_ps, "s")) {
      m.world_shiny = true;
      for (uint32_t k = 0; k < 3; ++k)
        m.specular_color[k] = LoadF32(base, kConstantMirror + 16u * (kPixelConstants + 19u) + 4u * k);
      m.world_shadowed = LoadF32(base, kConstantMirror + 16u * (kPixelConstants + 255u));
      static std::vector<std::string> shiny_logged;
      if (shiny_logged.size() < 6 &&
          std::find(shiny_logged.begin(), shiny_logged.end(), world_ps) == shiny_logged.end()) {
        shiny_logged.push_back(world_ps);
        std::string names;
        for (uint32_t slot = 8; slot <= 9; ++slot) {
          const uint32_t image = LoadU32(base, kTextureCache + 4u * slot);
          names += " slot " + std::to_string(slot) + " \"" +
                   (IsGuestData(image) ? Text(base, LoadU32(base, image + 0x20u)) : std::string("-")) + "\"";
        }
        REXLOG_INFO("cod2: native side: world highlights: ps \"{}\", material \"{}\":{}; c19 {:.3f} {:.3f} "
                    "{:.3f}; c255 {:.3f} {:.3f} {:.3f} {:.3f}",
                    world_ps, Text(base, LoadU32(base, material)), names, m.specular_color[0], m.specular_color[1],
                    m.specular_color[2], m.world_shadowed,
                    LoadF32(base, kConstantMirror + 16u * (kPixelConstants + 255u) + 4u),
                    LoadF32(base, kConstantMirror + 16u * (kPixelConstants + 255u) + 8u),
                    LoadF32(base, kConstantMirror + 16u * (kPixelConstants + 255u) + 12u));
      }
    }
    // 4z52b: what each world material's sun is made of, for the first materials of a run: c16 and c17 as this draw
    // has them, and the pictures in slot 1 (the sun's share) and slot 6 (the shadow cookie the share is times)
    static std::vector<uint32_t> sun_logged;
    if (sun_logged.size() < 24 && std::find(sun_logged.begin(), sun_logged.end(), material) == sun_logged.end()) {
      sun_logged.push_back(material);
      std::string pictures;
      for (const uint32_t slot : {1u, 6u}) {
        const uint32_t image = LoadU32(base, kTextureCache + 4u * slot);
        uint32_t texture = 0;
        std::array<uint32_t, 6> fetch{};
        SlotTexture(base, slot, texture, fetch);
        char item[200];
        std::snprintf(item, sizeof item, " slot %u \"%s\" %ux%u format %u (texture %08X);", slot,
                      IsGuestData(image) ? Text(base, LoadU32(base, image + 0x20u)).c_str() : "-",
                      (fetch[2] & 0x1FFFu) + 1, ((fetch[2] >> 13) & 0x1FFFu) + 1, fetch[1] & 0x3Fu, texture);
        pictures += item;
      }
      auto constant = [base](uint32_t number, uint32_t part) {  // a pixel constant's part, from the mirror
        return LoadF32(base, kConstantMirror + 16u * (kPixelConstants + number) + 4u * part);
      };
      REXLOG_INFO("cod2: native side: world sun: material \"{}\", ps \"{}\":{} c16 {:.3f} {:.3f} {:.3f}, c17 {:.3f} "
                  "{:.3f} {:.3f}",
                  Text(base, LoadU32(base, material)), world_ps, pictures, constant(16, 0), constant(16, 1),
                  constant(16, 2), constant(17, 0), constant(17, 1), constant(17, 2));
    }
    static int logged_weighted = 0;
    if (m.corner_weighted && logged_weighted < 12) {
      ++logged_weighted;
      uint32_t lowest = 255, highest = 0;
      uint64_t sums[4] = {};
      for (const uint32_t vertex_index : m.used) {
        const uint32_t color = GpuU32(base, m.vertices + vertex_index * stride + 0x10u);  // 0xAARRGGBB
        lowest = std::min(lowest, color >> 24);
        highest = std::max(highest, color >> 24);
        for (int k = 0; k < 4; ++k) sums[k] += (color >> (24 - 8 * k)) & 0xFFu;
      }
      const double count = std::max<size_t>(m.used.size(), 1);
      REXLOG_INFO("cod2: native side: a corner-weighted world layer: material \"{}\" (state word 0 {:08X}), ps \"{}\", "
                  "color map format {}, {} corners: alpha {}..{}, on average {:.0f}; red, green, blue {:.0f} {:.0f} "
                  "{:.0f}", Text(base, LoadU32(base, material)), m.state0, Text(base, LoadU32(base, ps)),
                  m.fetches[0][1] & 0x3Fu, m.used.size(), lowest, highest, sums[0] / count, sums[1] / count,
                  sums[2] / count, sums[3] / count);
    }
  }
  for (uint32_t k = 0; k < 3; ++k) {
    m.sun_direction[k] = LoadF32(base, kConstantMirror + 16u * (kPixelConstants + 16u) + 4u * k);
    m.sun_color[k] = LoadF32(base, kConstantMirror + 16u * (kPixelConstants + 17u) + 4u * k);
    m.fog_color[k] = LoadF32(base, kConstantMirror + 16u * (kPixelConstants + 21u) + 4u * k);
  }
  m.fog_exponent = LoadF32(base, kConstantMirror + 16u * 11u + 8u) * kFogLiteral;
  // 4z44: only the exponential fog's draws have that number in c11 (KindOf). No fog: none drawn. 4z45: the linear
  // fog's two numbers are c11's x and y (LinearFog).
  if (type != kSkyType && g_fog_kind != kFogExponential) {
    m.fog_exponent = 0.0f;
    const bool linear = g_fog_kind == kFogLinear &&
                        LinearFog(LoadF32(base, kConstantMirror + 16u * 11u),
                                  LoadF32(base, kConstantMirror + 16u * 11u + 4u), m.fog_start, m.fog_end);
    static bool logged_fog_kind = false;
    if (!logged_fog_kind) {
      logged_fog_kind = true;
      REXLOG_INFO("cod2: native side: this level's fog is {} (technique type {}): {}; vertex c11 {:g} {:g} {:g} {:g}",
                  g_fog_kind == kFogLinear ? "linear" : "none", LoadU32(base, kTessTechniqueType),
                  linear ? "from " + std::to_string(m.fog_start) + " to " + std::to_string(m.fog_end)
                         : std::string("drawn without fog"),
                  LoadF32(base, kConstantMirror + 16u * 11u), LoadF32(base, kConstantMirror + 16u * 11u + 4u),
                  LoadF32(base, kConstantMirror + 16u * 11u + 8u), LoadF32(base, kConstantMirror + 16u * 11u + 12u));
    }
  }
  if (type == kSkyType) {  // sky.hlsl's vertex shader (5702FE9431432564 in run cod2shaders1): the way from c20
    for (uint32_t k = 0; k < 3; ++k) m.eye[k] = LoadF32(base, kConstantMirror + 16u * 20u + 4u * k);
    for (const uint32_t vertex_index : m.used) {
      float d2 = 0.0f;
      for (uint32_t k = 0; k < 3; ++k) {
        const float d = GpuF32(base, m.vertices + vertex_index * stride + 4u * k) - m.eye[k];
        d2 += d * d;
      }
      m.sky_radius = std::max(m.sky_radius, std::sqrt(d2));
    }
  }
  // The models' light (pixel shaders lprobe_*, such as D070DE78E8946324 in run cod2shaders1): two cube maps of four
  // weights each, looked up at the normal (slots 0 and 1, "$lightgridweights0/1"), dotted with the pixel constants:
  // red = cube0 . c8 + cube1 . c9, green with c10 and c11, blue with c12 and c13; plus the sun, c17 x c17.w x the
  // shadow cookie x the normal against c16. The eight weights sum to 1 and average 1/8 each (GridWeightsFor measured
  // them), so that light averaged over all directions is (the four parts of c8 + those of c9) / 8 and so on: the
  // ambient here.
  if (type == kModelType || type == kStaticModelType || type == kLooseModelType) {
    auto parts = [&](uint32_t r) {
      float sum = 0.0f;
      for (uint32_t k = 0; k < 4; ++k) sum += LoadF32(base, kConstantMirror + 16u * (kPixelConstants + r) + 4u * k);
      return sum;
    };
    for (uint32_t c = 0; c < 3; ++c) m.ambient[c] = (parts(8u + 2u * c) + parts(9u + 2u * c)) / 8.0f;
    for (uint32_t r = 0; r < 6; ++r)  // 4z24: kept, for the weights as the cubes really have them (GridWeightsFor)
      for (uint32_t k = 0; k < 4; ++k)
        m.grid[r][k] = LoadF32(base, kConstantMirror + 16u * (kPixelConstants + 8u + r) + 4u * k);
    m.sun_scale = LoadF32(base, kConstantMirror + 16u * (kPixelConstants + 17u) + 12u);
    static bool logged_model = false;
    if (!logged_model) {
      logged_model = true;
      REXLOG_INFO("cod2: native side: the first model's light: ambient {:.3f} {:.3f} {:.3f} (pixel c8-c13 averaged), "
                  "sun {:.3f} {:.3f} {:.3f} x {:.3f} (c17), toward {:.3f} {:.3f} {:.3f} (c16)", m.ambient[0],
                  m.ambient[1], m.ambient[2], m.sun_color[0], m.sun_color[1], m.sun_color[2], m.sun_scale,
                  m.sun_direction[0], m.sun_direction[1], m.sun_direction[2]);
    }
  }
  // Step 4s: what a static model's light volume needs from this draw's constants (VolumeLightAt): the pixel shader's
  // c23, the lookup's step toward the normal (Moscow: 1/128, 1/128, 1/4, 0: half a texel of the 64 x 64 x 2 volume in
  // x and y, a quarter of its depth in z), and the vertex shader's c4-c6, the rows that turn the normals into world
  // space (vertex shader B335C2F94428DDF8: dp3 with each).
  if (static_model) {
    for (uint32_t k = 0; k < 3; ++k) {
      m.volume_step[k] = LoadF32(base, kConstantMirror + 16u * (kPixelConstants + 23u) + 4u * k);
      for (uint32_t r = 0; r < 3; ++r) m.to_world[3 * r + k] = LoadF32(base, kConstantMirror + 16u * (4u + r) + 4u * k);
      m.volume_center[k] = LoadF32(base, kConstantMirror + 16u * 23u + 4u * k);
      m.vertex_sun[k] = LoadF32(base, kConstantMirror + 16u * 17u + 4u * k);
    }
    // 4u: the pine canopies' shaders (lprobe_smc_amb_a_pfog, lprobe_sm_amb_a_pfog: pixel shader C93E5C77163A99A0,
    // vertex shaders 331D5525697CF6B6 and 60605A7F72D1B2CF in run cod2shaders1) take no normal at all: the volume is
    // read at the block's center as the vertex shader gives it (not moved toward a normal), and the pixel is texture
    // x corner color x (the volume's color + the vertex shader's c17 x the volume's alpha, the sun's share), its alpha
    // the texture's x the corner's. Known by the pixel shader's name.
    const uint32_t ps = IsGuestData(pass) ? LoadU32(base, pass + 0xCu) : 0;
    m.lit_all_round = IsGuestData(ps) && Text(base, LoadU32(base, ps)).find("_amb") != std::string::npos;
    if (m.lit_all_round) {
      mix(1u);  // its light is made another way (MeshFor)
      m.key = key;
      static int logged_round = 0;  // the first ones: their corners' alpha, and the two c17s
      if (logged_round < 4) {
        ++logged_round;
        uint32_t lowest = 255, highest = 0;
        for (const uint32_t vertex_index : m.used) {
          const uint32_t alpha = GpuU32(base, m.vertices + vertex_index * stride + ColorAt(type)) >> 24;
          lowest = std::min(lowest, alpha);
          highest = std::max(highest, alpha);
        }
        REXLOG_INFO("cod2: native side: lit all round {}: type {}, ps \"{}\", material \"{}\", {} corners, their "
                    "alpha {}..{}; vertex c17 {:.3f} {:.3f} {:.3f}, pixel c17 {:.3f} {:.3f} {:.3f}",
                    logged_round, type, Text(base, LoadU32(base, ps)), Text(base, LoadU32(base, material)),
                    m.used.size(), lowest, highest, m.vertex_sun[0], m.vertex_sun[1], m.vertex_sun[2],
                    m.sun_color[0], m.sun_color[1], m.sun_color[2]);
      }
    }
    // a type 14 model's vertices are the model's own, the same for every copy of it: each copy is its own mesh, by
    // where its light is and how it is turned (the light is put into the corners: MeshFor)
    if (type == kLooseModelType) {
      auto mix_float = [&mix](float value) {
        uint32_t bits;
        std::memcpy(&bits, &value, 4);
        mix(bits);
      };
      for (const float value : m.volume_center) mix_float(value);
      for (const float value : m.to_world) mix_float(value);
      m.key = key;
    }
    // a check (run cod2side20: some pines lit from outside the volume): the first static draws' lighting
    // coordinates, how many corners sit on texel borders (both numbers multiples of 256), and their c4 row. Run
    // cod2side21: type 17's are all odd multiples of 512 (the blocks' centers); type 14's +0x24 is no coordinate, and
    // its block's center is vertex c23 (cod2side22: 0.140625 0.484375 0.5, texels 9 and 31 of 64)
    static int logged_static = 0;
    if (logged_static < 12) {
      ++logged_static;
      int low[2] = {65536, 65536}, high[2] = {-65536, -65536};
      uint32_t on_borders = 0;
      for (const uint32_t vertex_index : m.used) {
        const uint32_t word = GpuU32(base, m.vertices + vertex_index * stride + 0x24u);
        const int both[2] = {static_cast<int16_t>(word >> 16), static_cast<int16_t>(word & 0xFFFFu)};
        for (int k = 0; k < 2; ++k) {
          low[k] = std::min(low[k], both[k]);
          high[k] = std::max(high[k], both[k]);
        }
        on_borders += (both[0] & 255) == 0 && (both[1] & 255) == 0;
      }
      const std::array<uint32_t, 6>& v = m.fetches[0];
      // 4t3: with the material's state word 0 and the color map's format (slot 1), to see how the pines are drawn
      const std::array<uint32_t, 6>& color = m.fetches[1];
      REXLOG_INFO("cod2: native side: static draw {}: type {}, {} corners, lighting coordinates {}..{} and {}..{} ({} "
                  "on texel borders), c4 {:.2f} {:.2f} {:.2f} {:.1f}, slot 0 {} x {} x {} (dimension {}, format {}); "
                  "state 0 {:08X}, slot 1 {} x {} format {}",
                  logged_static, type, m.used.size(), low[0], high[0], low[1], high[1], on_borders, m.to_world[0],
                  m.to_world[1], m.to_world[2], LoadF32(base, kConstantMirror + 16u * 4u + 12u), (v[2] & 0x7FFu) + 1,
                  ((v[2] >> 11) & 0x7FFu) + 1, ((v[2] >> 22) & 0x3FFu) + 1, (v[5] >> 9) & 3u, v[1] & 0x3Fu,
                  m.state0, (color[2] & 0x1FFFu) + 1, ((color[2] >> 13) & 0x1FFFu) + 1, color[1] & 0x3Fu);
    }
  }
  static bool logged_constants = false;
  if (type == kWorldType && !logged_constants) {  // the fog's numbers as the first world draw has them
    logged_constants = true;
    auto row = [&](uint32_t r) {
      char text[96];
      std::snprintf(text, sizeof text, "(%g %g %g %g)", LoadF32(base, kConstantMirror + 16u * r),
                    LoadF32(base, kConstantMirror + 16u * r + 4u), LoadF32(base, kConstantMirror + 16u * r + 8u),
                    LoadF32(base, kConstantMirror + 16u * r + 12u));
      return std::string(text);
    };
    REXLOG_INFO("cod2: native side: the world's fog: vertex c11 {}, vertex c255 {}, pixel c21 {}, pixel c255 {}",
                row(11), row(255), row(kPixelConstants + 21u), row(kPixelConstants + 255u));
    // 4z14: the fog's distance is the length of the corner times c8-c10 (lmap_efog A70A57E3BDA8D434: dp4 with each,
    // then dp3 and sqrt): the rows, to see whether they are the world-to-view turn alone (each of length 1)
    REXLOG_INFO("cod2: native side: the world's fog distance: vertex c8 {}, c9 {}, c10 {}", row(8), row(9), row(10));
  }
  // c0-c3 turned over: Direct3D's row-vector matrix, [i][r] = c_r[i]
  for (uint32_t r = 0; r < 4; ++r)
    for (uint32_t i = 0; i < 4; ++i) m.view_projection[4 * i + r] = LoadF32(base, kConstantMirror + 16u * r + 4u * i);
  // 4z20: the first used corner's position now, to see whether the buffer is filled again before the mesh is made
  // (SetMeshes logs it beside the position then, for the draws that cover the picture)
  if (!m.used.empty())
    for (uint32_t k = 0; k < 3; ++k) m.kept_position[k] = GpuF32(base, m.vertices + m.used[0] * m.stride + 4u * k);
  m.material = material;  // 4z16
  KeepViewport(base, m);  // 4z22e
  m.pass = pass;
  f.meshes.push_back(std::move(m));
}

// Step 4i: one 3D draw from the tess buffers, an effect (the sprites and particles the game writes there each frame:
// smoke, fire, sparks), in world space and the rigid models' 64-byte layout (position +0x00, color +0x1C, texture
// coordinate +0x20). Its corners are copied now: the buffers are written again for the next draw.
void KeepEffect(const uint8_t* base, uint32_t triangles, uint32_t indices, uint32_t pass, Frame& f) {
  if (f.meshes.size() >= kMaxMeshDraws) {
    ++f.meshes_dropped;
    return;
  }
  const uint32_t material = LoadU32(base, kTessMaterial);
  if (!IsGuestData(material)) {
    ++f.meshes_dropped;
    return;
  }
  MeshDraw m;
  m.type = LoadU32(base, kTessTechniqueType);
  m.stride = kTessStride;
  m.effect.reserve(3u * triangles);
  uint64_t key = 14695981039346656037ull;
  auto mix = [&key](uint32_t word) {
    for (int b = 0; b < 4; ++b) key = (key ^ ((word >> (8 * b)) & 0xFFu)) * 1099511628211ull;
  };
  for (uint32_t i = 0; i < 3u * triangles; ++i) {
    const uint32_t index = LoadU16(base, indices + 2 * i);
    if (index >= kTessMaxVertices) {
      ++f.meshes_dropped;
      return;
    }
    const uint32_t vertex = kTessVertices + index * kTessStride;
    EffectCorner c;
    c.x = LoadF32(base, vertex);
    c.y = LoadF32(base, vertex + 0x4u);
    c.z = LoadF32(base, vertex + 0x8u);
    c.color = LoadU32(base, vertex + 0x1Cu);
    c.u = LoadF32(base, vertex + 0x20u);
    c.v = LoadF32(base, vertex + 0x24u);
    for (uint32_t w : {0u, 4u, 8u, 0x1Cu, 0x20u, 0x24u}) mix(LoadU32(base, vertex + w));
    m.effect.push_back(c);
  }
  m.key = key;
  m.state0 = LoadU32(base, material + 0x14u);
  m.color_slot = 0;
  SlotTexture(base, 0, m.textures[0], m.fetches[0]);
  static std::vector<uint32_t> logged;  // the first effects' materials, each logged once
  if (logged.size() < 8 && std::find(logged.begin(), logged.end(), material) == logged.end()) {
    logged.push_back(material);
    const uint32_t image = LoadU32(base, kTextureCache);
    // 4x: with its shaders' names (the pass's +0x8 and +0xC), to read how the game colors it
    const uint32_t vs = IsGuestData(pass) ? LoadU32(base, pass + 8u) : 0;
    const uint32_t ps = IsGuestData(pass) ? LoadU32(base, pass + 0xCu) : 0;
    // 4z2: its texture's size and format, and its corners' texture coordinates' range (Moscow's snow sheets show as
    // flat rectangles in the native)
    const uint32_t texture = IsGuestData(image) ? LoadU32(base, image + 4u) : 0;
    const uint32_t size = IsGuestData(texture) ? LoadU32(base, texture + kTextureFetch + 8u) : 0;
    const uint32_t format = IsGuestData(texture) ? LoadU32(base, texture + kTextureFetch + 4u) & 0x3Fu : 0;
    float u0 = m.effect[0].u, u1 = u0, v0 = m.effect[0].v, v1 = v0;
    for (const EffectCorner& c : m.effect) {
      u0 = std::min(u0, c.u), u1 = std::max(u1, c.u);
      v0 = std::min(v0, c.v), v1 = std::max(v1, c.v);
    }
    REXLOG_INFO("cod2: native side: effect: type {}, material \"{}\" (state word 0 {:08X}), texture \"{}\" ({} x {} "
                "format {}), {} triangles, the first corner at {:.1f} {:.1f} {:.1f}, color {:08X}, texture "
                "coordinates {:.3f}..{:.3f} x {:.3f}..{:.3f}; vs \"{}\", ps \"{}\"",
                m.type, Text(base, LoadU32(base, material)), m.state0,
                IsGuestData(image) ? Text(base, LoadU32(base, image + 0x20u)) : "-", (size & 0x1FFFu) + 1,
                ((size >> 13) & 0x1FFFu) + 1, format, triangles, m.effect[0].x, m.effect[0].y, m.effect[0].z,
                m.effect[0].color, u0, u1, v0, v1, IsGuestData(vs) ? Text(base, LoadU32(base, vs)) : "-",
                IsGuestData(ps) ? Text(base, LoadU32(base, ps)) : "-");
  }
  // 4z: fogged only when its pixel shader is a fogged one ("pfog" in its name: zfeather_pfog, with the vertex shader
  // zfeather_efog). Those make the fog as the world's shaders do: the vertex shader's share kept, 2^(c11.z x c255.x x
  // the distance to the eye) with c255.x 1.442695 (found beside the shader in moscow.ff and common.ff, as the world's
  // kFogLiteral), and the pixel shader's color mixed toward c21 by it. The others (zfeather: the mist; vertcol_simple:
  // the sun and its flare) have no fog. MeshFor draws a fogged effect lit by its corners' colors once, which ngl_vk
  // fogs; 4i-4y drew every effect unlit, which it doesn't, so the far smoke was left dark.
  const uint32_t ps = IsGuestData(pass) ? LoadU32(base, pass + 0xCu) : 0;
  const std::string ps_name = IsGuestData(ps) ? Text(base, LoadU32(base, ps)) : std::string();
  if (ps_name.find("pfog") != std::string::npos) {
    for (uint32_t k = 0; k < 3; ++k)
      m.fog_color[k] = LoadF32(base, kConstantMirror + 16u * (kPixelConstants + 21u) + 4u * k);
    m.fog_exponent = LoadF32(base, kConstantMirror + 16u * 11u + 8u) * kFogLiteral;
    if (g_fog_kind != kFogExponential) m.fog_exponent = 0.0f;  // 4z44 (KindOf)
    if (g_fog_kind == kFogLinear)  // 4z45
      LinearFog(LoadF32(base, kConstantMirror + 16u * 11u), LoadF32(base, kConstantMirror + 16u * 11u + 4u),
                m.fog_start, m.fog_end);
  }
  for (uint32_t r = 0; r < 4; ++r)
    for (uint32_t i = 0; i < 4; ++i) m.view_projection[4 * i + r] = LoadF32(base, kConstantMirror + 16u * r + 4u * i);
  // 4y: the soft particles (zfeather_efog / zfeather_pfog: vertex shader 531753FCAEC1CE21, pixel shader
  // 638666FF28AA9C3A in run cod2shaders1). The pixel shader multiplies the alpha by saturate(|the opaque scene's clip z
  // (the floatz target, which floatz_build fills with clip z: vertex shader 716F1E115765C861) x pixel c23.x - its own
  // clip z x vertex c25.x|), and the vertex shader the corner's alpha by saturate(its clip z x c25.x): faded where it
  // nears what is behind it and near the eye. Its corners then depend on the view (MeshFor): the key takes it too.
  if (ps_name.rfind("zfeather", 0) == 0) {
    m.feather = LoadF32(base, kConstantMirror + 16u * 25u);
    for (const float value : m.view_projection) {
      uint32_t bits;
      std::memcpy(&bits, &value, 4);
      mix(bits);
    }
    m.key = key;
    static int logged_feather = 0;
    if (logged_feather < 2) {
      ++logged_feather;
      REXLOG_INFO("cod2: native side: a soft effect (\"{}\"): vertex c25.x {:.6g}, pixel c23.x {:.6g}",
                  Text(base, LoadU32(base, material)), m.feather,
                  LoadF32(base, kConstantMirror + 16u * (kPixelConstants + 23u)));
    }
  }
  m.material = material;  // 4z16
  KeepViewport(base, m);  // 4z22e
  m.pass = pass;
  f.meshes.push_back(std::move(m));
}

// Step 4l: one draw of a particle cloud (technique type 26 with 20-byte vertices; Moscow's falling snow), each particle
// a quad of corners with one position (+0x00, three floats) and a texture coordinate each (+0x0C, two). Its vertex
// shader (particle_cloud_outdoor, 9EE0952B62AC2E2C in run cod2shaders1) places a corner in view space: (p, 1) dotted
// with c8-c11, x and y moved by the texture coordinate less a half (s, t) turned by c27: x + s c27.x + t c27.z, y +
// s c27.y + t c27.w (the half is the shader's literal c255.y, taken as 0.5: the coordinates are 0 and 1); then c23-c26
// project it. The corners are worked out here the same way and drawn as an effect (KeepEffect's) in view space, with
// c23-c26 turned over as the matrix: unfogged (the shader has no fog).
// 4z23: and colored as its pixel shader colors it (AF8B3F163297335C in run cod2shaders1: the texture times pixel c23,
// the particles' color; the PC copy's particle_cloud.hlsl names it particleCloudColor), where it had been white:
// Pointe du Hoc's sea spray is a faint cloud, and drawn at full strength it was a field of hard white dots
// (cod2side72). Its other test, the alpha taken away where the outdoor map says the particle is under cover, isn't
// made yet.
uint32_t CloudColor(const uint8_t* base) {
  auto byte = [&](uint32_t k) {
    const float v = LoadF32(base, kConstantMirror + 16u * (kPixelConstants + 23u) + 4u * k);
    return static_cast<uint32_t>(std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f));  // NaN fails both tests: 0
  };
  return byte(3) << 24 | byte(0) << 16 | byte(1) << 8 | byte(2);  // 0xAARRGGBB
}

// 4z46. The clouds' outdoor test: snow and rain fall only under the open sky. The cloud's vertex shader
// (particle_cloud_outdoor, 9EE0952B62AC2E2C) hands on the particle's middle times vertex c12, c13 and c14; its pixel
// shader (AF8B3F163297335C) fetches slot 1 at the first two and, where that value is above the third, makes the
// alpha 0. Slot 1 is the level's outdoor map, a picture of heights seen from above. Left out, the snow fell inside
// Stalingrad's buildings (run cod2side126, "demolition"). The map is read once for each texture. The texture
// decoders are part of the native drawing (COD2_NGL_VK, below): a build without it reads no map and tests nothing
// (4z48: such a build didn't link, f2-menus 2026-11-11 00:29).
#if defined(COD2_NGL_VK)
bool DecodeFetch(const uint8_t* base, const std::array<uint32_t, 6>& fetch, std::vector<uint8_t>& bgra,
                 uint32_t& width, uint32_t& height, std::string& why);
#endif
struct OutdoorMap {
  uint32_t texture = 0;  // the Direct3D texture it was read from
  uint32_t width = 0, height = 0;
  std::vector<uint8_t> value;  // one height for each texel, 0 to 255 for 0 to 1
};
OutdoorMap g_outdoor_map;

// The outdoor map in slot 1, or nullptr when there is none or it can't be read.
const OutdoorMap* OutdoorMapInSlotOne(const uint8_t* base) {
  uint32_t texture = 0;
  std::array<uint32_t, 6> fetch{};
  if (!SlotTexture(base, 1, texture, fetch) || texture == 0) return nullptr;
  if (g_outdoor_map.texture != texture) {
    g_outdoor_map = OutdoorMap{};
    g_outdoor_map.texture = texture;
    std::vector<uint8_t> bgra;
    std::string why;
    bool read = false;
#if defined(COD2_NGL_VK)
    read = DecodeFetch(base, fetch, bgra, g_outdoor_map.width, g_outdoor_map.height, why);
#else
    why = "this build has no native drawing";
#endif
    if (read) {
      g_outdoor_map.value.resize(static_cast<size_t>(g_outdoor_map.width) * g_outdoor_map.height);
      for (size_t i = 0; i < g_outdoor_map.value.size(); ++i) g_outdoor_map.value[i] = bgra[4 * i + 2];  // red
    }
    const uint32_t image = LoadU32(base, kTextureCache + 4u);
    REXLOG_INFO("cod2: native side: the clouds' outdoor map \"{}\": {} x {}{}",
                IsGuestData(image) ? Text(base, LoadU32(base, image + 0x20u)) : std::string("-"), g_outdoor_map.width,
                g_outdoor_map.height, g_outdoor_map.value.empty() ? " (not read: " + why + ")" : std::string());
  }
  return g_outdoor_map.value.empty() ? nullptr : &g_outdoor_map;
}

// Whether the map's height at a place (between its four texels, held at the edges) is above a height.
bool CoveredAt(const OutdoorMap& map, float u, float v, float height) {
  const float x = std::clamp(u * static_cast<float>(map.width) - 0.5f, 0.0f, static_cast<float>(map.width - 1));
  const float y = std::clamp(v * static_cast<float>(map.height) - 0.5f, 0.0f, static_cast<float>(map.height - 1));
  const uint32_t x0 = static_cast<uint32_t>(x), y0 = static_cast<uint32_t>(y);
  const uint32_t x1 = std::min(x0 + 1, map.width - 1), y1 = std::min(y0 + 1, map.height - 1);
  const float fx = x - static_cast<float>(x0), fy = y - static_cast<float>(y0);
  auto at = [&map](uint32_t column, uint32_t row) {
    return static_cast<float>(map.value[static_cast<size_t>(row) * map.width + column]) / 255.0f;
  };
  const float top = at(x0, y0) + (at(x1, y0) - at(x0, y0)) * fx;
  const float bottom = at(x0, y1) + (at(x1, y1) - at(x0, y1)) * fx;
  return top + (bottom - top) * fy > height;
}

void KeepCloud(const uint8_t* base, uint32_t args, bool outdoor_test, Frame& f) {
  const uint32_t buffer = LoadU32(base, kStreamCache), offset = LoadU32(base, kStreamCache + 4u);
  const uint32_t indices = LoadU32(base, kIndexCache);
  const uint32_t material = LoadU32(base, kTessMaterial);
  const uint32_t triangles = LoadU32(base, args + 0xCu);
  if (f.meshes.size() >= kMaxMeshDraws || !IsGuestData(buffer) || !IsGuestData(indices) || !IsGuestData(material) ||
      triangles == 0 || 3u * triangles > 65535u) {
    ++f.meshes_dropped;
    return;
  }
  auto constant = [&](uint32_t r, uint32_t k) { return LoadF32(base, kConstantMirror + 16u * r + 4u * k); };
  MeshDraw m;
  m.type = kCloudType;
  m.stride = kCloudStride;
  m.vertices = (LoadU32(base, buffer + 0xCu) & 0x1FFFFFFCu) + offset;
  const uint32_t base_vertex = LoadU32(base, args + 0x10u);
  const bool wide = (LoadU32(base, indices) >> 31) != 0;
  const uint32_t first = (LoadU32(base, indices + 0xCu) & 0x1FFFFFFFu) + (wide ? 4u : 2u) * LoadU32(base, args + 0x14u);
  uint64_t key = 14695981039346656037ull;
  auto mix = [&key](float value) {
    uint32_t word;
    std::memcpy(&word, &value, 4);
    for (int b = 0; b < 4; ++b) key = (key ^ ((word >> (8 * b)) & 0xFFu)) * 1099511628211ull;
  };
  float first_w = 0.0f;
  const uint32_t cloud_color = CloudColor(base);  // 4z23
  const OutdoorMap* outdoor = outdoor_test ? OutdoorMapInSlotOne(base) : nullptr;  // 4z46
  uint32_t covered = 0;
  m.effect.reserve(3u * triangles);
  for (uint32_t i = 0; i < 3u * triangles; ++i) {
    const uint32_t index = base_vertex + (wide ? GpuU32(base, first + 4u * i) : GpuU16(base, first + 2u * i));
    const uint32_t vertex = m.vertices + index * kCloudStride;
    const float p[3] = {GpuF32(base, vertex), GpuF32(base, vertex + 4u), GpuF32(base, vertex + 8u)};
    const float u = GpuF32(base, vertex + 0xCu), v = GpuF32(base, vertex + 0x10u);
    const float s = u - 0.5f, t = v - 0.5f;
    float view[4];
    for (uint32_t r = 0; r < 4; ++r)
      view[r] = p[0] * constant(8 + r, 0) + p[1] * constant(8 + r, 1) + p[2] * constant(8 + r, 2) + constant(8 + r, 3);
    view[0] += s * constant(27, 0) + t * constant(27, 2);
    view[1] += s * constant(27, 1) + t * constant(27, 3);
    if (i == 0) first_w = view[3];
    if (!(std::fabs(view[3]) > 1e-6f)) {  // not a point ngl_vk can take (w is 1 for an ordinary view matrix)
      ++f.meshes_dropped;
      return;
    }
    EffectCorner c;
    c.x = view[0] / view[3];
    c.y = view[1] / view[3];
    c.z = view[2] / view[3];
    c.u = u;
    c.v = v;
    c.color = cloud_color;
    if (outdoor != nullptr) {  // 4z46: under a roof: no alpha (the color's top byte)
      float place[3];
      for (uint32_t r = 0; r < 3; ++r)
        place[r] = p[0] * constant(12 + r, 0) + p[1] * constant(12 + r, 1) + p[2] * constant(12 + r, 2) +
                   constant(12 + r, 3);
      if (CoveredAt(*outdoor, place[0], place[1], place[2])) {
        c.color &= 0x00FFFFFFu;
        ++covered;
      }
    }
    for (float value : {c.x, c.y, c.z, c.u, c.v}) mix(value);
    mix(static_cast<float>(c.color >> 24));
    m.effect.push_back(c);
  }
  static int logged_covered = 0;
  if (outdoor != nullptr && logged_covered < 4) {
    ++logged_covered;
    REXLOG_INFO("cod2: native side: a cloud's outdoor test: {} of {} corners under a roof; vertex c12 {:g} {:g} {:g} "
                "{:g}, c13 {:g} {:g} {:g} {:g}, c14 {:g} {:g} {:g} {:g}",
                covered, 3u * triangles, constant(12, 0), constant(12, 1), constant(12, 2), constant(12, 3),
                constant(13, 0), constant(13, 1), constant(13, 2), constant(13, 3), constant(14, 0), constant(14, 1),
                constant(14, 2), constant(14, 3));
  }
  float color_bits;  // 4z23: the color is in the corners, so in the key too
  std::memcpy(&color_bits, &cloud_color, 4);
  mix(color_bits);
  m.key = key;
  m.state0 = LoadU32(base, material + 0x14u);
  m.color_slot = 0;
  SlotTexture(base, 0, m.textures[0], m.fetches[0]);
  static int logged = 0;  // 4z23: the first four clouds of a run, with their color
  static uint32_t logged_material = 0;
  if (logged < 4 && material != logged_material) {
    ++logged;
    logged_material = material;
    const uint32_t image = LoadU32(base, kTextureCache);
    const EffectCorner& c = m.effect[0];
    REXLOG_INFO("cod2: native side: particle cloud: material \"{}\" (state word 0 {:08X}), texture \"{}\", {} "
                "triangles, the first corner at {:.2f} {:.2f} {:.2f} in view space (w {:.3f}), texture coordinate "
                "{:.2f} {:.2f}; c27 {:.4f} {:.4f} {:.4f} {:.4f}; its color (pixel c23) {:08X}",
                Text(base, LoadU32(base, material)), m.state0,
                IsGuestData(image) ? Text(base, LoadU32(base, image + 0x20u)) : "-", triangles, c.x, c.y, c.z, first_w,
                c.u, c.v, constant(27, 0), constant(27, 1), constant(27, 2), constant(27, 3), cloud_color);
  }
  m.fog_exponent = 0.0f;
  for (uint32_t r = 0; r < 4; ++r)
    for (uint32_t i = 0; i < 4; ++i) m.view_projection[4 * i + r] = constant(23 + r, i);
  m.material = material;  // 4z16
  KeepViewport(base, m);  // 4z22e
  f.meshes.push_back(std::move(m));
}

// ---------------------------------------------------------------------------------------------------------------
// Step 4's census: one indexed draw, as it is drawn (see the top).

void LogIndexedSample(const uint8_t* base, uint32_t args, uint32_t pass) {
  const uint32_t triangles = LoadU32(base, args + 0xCu);
  const uint32_t base_vertex = LoadU32(base, args + 0x10u), start_index = LoadU32(base, args + 0x14u);
  const uint32_t buffer = LoadU32(base, kStreamCache), offset = LoadU32(base, kStreamCache + 4u),
                 stride = LoadU32(base, kStreamCache + 8u);
  const uint32_t fetch0 = IsGuestData(buffer) ? LoadU32(base, buffer + 0xCu) : 0;
  const uint32_t fetch1 = IsGuestData(buffer) ? LoadU32(base, buffer + 0x10u) : 0;
  const uint32_t indices = LoadU32(base, kIndexCache);
  const uint32_t common = IsGuestData(indices) ? LoadU32(base, indices) : 0;
  const uint32_t index_address = IsGuestData(indices) ? LoadU32(base, indices + 0xCu) : 0;
  const uint32_t index_bytes = IsGuestData(indices) ? LoadU32(base, indices + 0x10u) : 0;
  const bool wide = (common >> 31) != 0;
  // the first index and its vertex's position (the first three floats), and that position through c0-c3 as the
  // vertex shader's dp4 against each register computes it
  uint32_t first_index = 0;
  float position[3] = {}, through[4] = {};
  float c[4][4] = {};
  for (uint32_t r = 0; r < 4; ++r)
    for (uint32_t k = 0; k < 4; ++k) c[r][k] = LoadF32(base, kConstantMirror + 16u * r + 4u * k);
  if (index_address != 0 && fetch0 != 0 && stride != 0) {
    const uint32_t at = (index_address & 0x1FFFFFFFu) + (wide ? 4u : 2u) * start_index;
    // a 16-bit index is the high half of its big-endian word when it starts the word, else the low half
    first_index = wide ? GpuU32(base, at) : (GpuU32(base, at & ~3u) >> ((at & 2u) ? 0u : 16u)) & 0xFFFFu;
    const uint32_t vertex = (fetch0 & 0x1FFFFFFCu) + offset + (base_vertex + first_index) * stride;
    for (uint32_t k = 0; k < 3; ++k) position[k] = GpuF32(base, vertex + 4u * k);
    for (uint32_t r = 0; r < 4; ++r)
      through[r] = position[0] * c[r][0] + position[1] * c[r][1] + position[2] * c[r][2] + c[r][3];
  }
  std::string constants;
  for (uint32_t r = 0; r < 4; ++r) {
    char row[96];
    std::snprintf(row, sizeof row, " (%.4g %.4g %.4g %.4g)", c[r][0], c[r][1], c[r][2], c[r][3]);
    constants += row;
  }
  std::string textures;
  for (uint32_t slot = 0; slot < 4; ++slot) {
    const uint32_t image = LoadU32(base, kTextureCache + 4u * slot);
    if (!IsGuestData(image)) continue;
    const uint32_t texture = LoadU32(base, image + 4u);
    const uint32_t size = IsGuestData(texture) ? LoadU32(base, texture + kTextureFetch + 8u) : 0;
    const uint32_t format = IsGuestData(texture) ? LoadU32(base, texture + kTextureFetch + 4u) & 0x3Fu : 0;
    const std::string name = Text(base, LoadU32(base, image + 0x20u));
    char item[160];
    std::snprintf(item, sizeof item, " %u:\"%s\" %ux%u format %u", slot, name.c_str(), (size & 0x1FFFu) + 1,
                  ((size >> 13) & 0x1FFFu) + 1, format);
    textures += item;
  }
  const uint32_t vs = IsGuestData(pass) ? LoadU32(base, pass + 8u) : 0;
  const uint32_t ps = IsGuestData(pass) ? LoadU32(base, pass + 0xCu) : 0;
  const uint32_t material = LoadU32(base, kTessMaterial);
  REXLOG_INFO(
      "cod2: native side: indexed sample: type {}, target {}, vs \"{}\", ps \"{}\", material \"{}\" (state word 0 "
      "{:08X}); {} triangles from "
      "index {} (base vertex {}); stream 0 {:08X} +{} stride {} (fetch {:08X} {:08X}); indices {:08X} ({}-bit, at "
      "{:08X}, {} bytes); first index {}, its position {:.1f} {:.1f} {:.1f}, through c0-c3 {:.3f} {:.3f} {:.3f} "
      "{:.3f}; c0-c3{}; textures{}",
      LoadU32(base, kTessTechniqueType), LoadU32(base, kTargetId),
      IsGuestData(vs) ? Text(base, LoadU32(base, vs)) : "-", IsGuestData(ps) ? Text(base, LoadU32(base, ps)) : "-",
      IsGuestData(material) ? Text(base, LoadU32(base, material)) : "-",
      IsGuestData(material) ? LoadU32(base, material + 0x14u) : 0u, triangles, start_index, base_vertex, buffer,
      offset, stride, fetch0, fetch1, indices, wide ? 32 : 16, index_address, index_bytes, first_index, position[0],
      position[1], position[2], through[0], through[1], through[2], through[3], constants,
      textures.empty() ? std::string(" none") : textures);
}

// ---------------------------------------------------------------------------------------------------------------
// The helper thread: saves the native picture, then asks for the game's own and saves it, then logs both.

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

// The game's own picture (the frame the GPU emulation last sent out), as cod2_debug_shot_seconds reads it.
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
    REXLOG_WARN("cod2: native side: frame {}: the native picture couldn't be saved ({})", job.frame, native.string());
    return;
  }
  std::string game = "not taken";
  rex::ui::RawImage image;
  if (CaptureGame(image)) {
    const std::string name = stem + "-game.png";
    if (WritePng(g_folder / name, image)) game = name;
  }
  REXLOG_INFO("cod2: native side: frame {}: {} to {}; the game's own: {}", job.frame, job.summary, native.string(),
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
      REXLOG_WARN("cod2: native side: frame {}: no helper to save it (started with the check off?)", job.frame);
      return;
    }
    g_jobs.push_back(std::move(job));
  }
  g_jobs_cv.notify_one();
}

#if defined(COD2_NGL_VK)
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
                 uint32_t& width, uint32_t& height, std::string& why) {
  if ((fetch[0] & 3) != 2) {
    why = "not a texture fetch constant";
    return false;
  }
  const bool tiled = (fetch[0] >> 31) != 0;
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

// Step 4t2: the game's own smaller levels of a texture (1 to its last, the fetch constant's dword 4 bits 6-9), BGRA8
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
std::unordered_map<uint32_t, CachedTexture> g_opaque_textures;  // and their copies with alpha 1 (4z9, TextureFor)
std::unordered_map<uint32_t, CachedTexture> g_turned_textures;  // and turned over (4z51, TurnedTextureFor)
std::map<std::string, int> g_failed;                     // why textures weren't decoded, and how many
constexpr size_t kMaxTextures = 1500;                    // ngl_vk has room for 2048
uint64_t g_textures_with_levels = 0;  // textures made with the game's own smaller levels (DecodeLevelsFetch), all told
uint64_t g_textures_top_only = 0;     // and with their top level alone

// Step 4: the meshes made so far, by their geometry's fingerprint (MeshDraw::key).
std::unordered_map<uint64_t, uint32_t> g_meshes;
bool g_forget = false;  // ngl_vk ran out of room: forget everything before the next frame

// 4z59: what a draw is drawn from (MeshFor): a mesh of its own, or this frame's corners (ngl_vk_frame_corners) with a
// mesh's triangles. Geometry whose key is seen in one frame only (the world's index lists change as the view moves,
// effects and moving models are new each frame) takes no lasting room that way; with the live compare drawing every
// frame, those meshes filled ngl_vk's room in about five frames, and ForgetAll threw the textures and light maps away
// with them (run cod2cmp2: 22 times in 120 frames, about 460 ms a frame). A key seen again in a later frame gets a mesh
// of its own, as before.
struct MeshUse {
  uint32_t mesh = 0;         // ngl_vk's mesh (0: none, the draw is left out)
  uint32_t corners = 0;      // from ngl_vk_frame_corners, or 0: the mesh's own corners
  uint32_t index_count = 0;  // how many of the mesh's indices are drawn, from its first (0: all of them)
};
std::unordered_map<uint64_t, uint64_t> g_seen;      // keys drawn from frame corners so far: the frame each was first in
constexpr size_t kMaxSeen = 100000;                  // then the list starts again (a key then waits one frame more)
std::unordered_map<uint64_t, MeshUse> g_this_frame;  // this frame's draws from frame corners, by key (copied once)
// The triangles drawn with frame corners come from a mesh of one corner (never read: the draw's corners field points
// at the frame's) and indices. Two kinds:
// - a shape: the draw's own indices, kept by their fingerprint once they are drawn in two frames (a moving model's
//   triangles stay the same while its corners move);
// - the indices 0, 1, 2, ... 65534 in order, drawn as far as the draw needs: an effect's (each corner its own), and any
//   other draw's with its corners written out once for each index (the world's index lists change as the view moves,
//   so a shape of them would be new in every frame and fill ngl_vk's room for indices instead).
std::unordered_map<uint64_t, uint32_t> g_shapes;
std::unordered_map<uint64_t, uint64_t> g_shapes_seen;  // shapes not kept yet: the frame each was first drawn in
uint32_t g_in_order = 0;
constexpr uint32_t kInOrder = 65535;
// 4z62: past a third of ngl_vk's room for mesh corners (set by DrawFrame each frame), a key drawn again gets no mesh of
// its own any more: it stays on frame corners. With every frame drawn (cod2_native_vulkan, run cod2-nat2) the world's
// keys kept coming back for a few frames as the view moved, about 15 meshes (25,000 corners) a frame were made, the
// room was half used every five seconds and ForgetAll threw the textures away with the meshes: the frame took 31 to 52
// ms (9 fps at the worst) instead of 5.6. The meshes made before that point are kept and drawn as before.
bool g_mesh_room_tight = false;
// For the live compare's log (DrawFrame): draws from frame corners (with a shape, or written out in order) and the
// corners they took, those that found the frame's room used up (made a mesh of their own instead), and the meshes
// made, with their corners.
struct MeshCount {
  uint64_t shape_draws = 0, order_draws = 0, frame_corners = 0, frame_full = 0, made = 0, made_corners = 0;
};
MeshCount g_mesh_count;

// Step 4: the world's lightmaps as ngl_vk takes them, by the five textures they are made from (slots 1-5).
std::map<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t, uint32_t, uint32_t>, uint32_t> g_lightmaps;
bool g_logged_lightmap = false;
std::unordered_map<uint32_t, uint32_t> g_skies;  // the sky's stand-in colors (SkyColorFor)
// The sky boxes made so far (SkyBoxFor), by the sky cube's Direct3D texture: a mesh and a texture for each face.
struct SkyBox {
  std::array<uint32_t, 6> meshes{}, faces{};
  bool made = false;  // all twelve: else the stand-in color is drawn
};
std::unordered_map<uint32_t, SkyBox> g_sky_boxes;
// The models' light grid weights read so far (GridWeightsFor), by the two cubes' Direct3D textures.
struct GridWeights {
  float mean[8] = {}, up[8] = {}, down[8] = {};
  // 4z25: each weight as the nearest second-degree function of the direction (least squares over the whole cube, by
  // solid angle): its nine factors for xx, yy, zz, xy, yz, zx, x, y and z, the order ngl_vk's irradiance rows take
  float fit[8][9] = {};
  float fit_error = 0.0f;  // how far the fitted weights are from the cubes' (root mean square, all eight together)
  // 4z32: the fit made again with one third-degree term, the product of the direction's three parts along `axes`
  // (unit length, square to one another, world space): each weight's factor for it, and how far that fit is off
  float cubic[8] = {};
  float axes[3][3] = {};
  float cubic_error = 0.0f;
  bool has_cubic = false;
  // 4z34: the first cube as an ngl_vk cube texture (0: none), made when the second cube is the first one looking the
  // other way: the second cube's weight k at a direction is the first cube's weight opposite[k] at the opposite one
  uint32_t cube = 0;
  int opposite[4] = {0, 1, 2, 3};
  bool made = false;
};
std::map<std::pair<uint32_t, uint32_t>, GridWeights> g_grid_weights;
// The water's sky cubes made so far (WaterCubeFor), by the cube's Direct3D texture: ngl_vk's cube texture (0: none).
std::unordered_map<uint32_t, uint32_t> g_water_cubes;
// The water's height maps made so far (WaterHeightFor), by the map's Direct3D texture: ngl_vk's texture, its size and
// a sum of the content it holds now.
struct WaterHeight {
  uint32_t id = 0, width = 0, height = 0, sum = 0;
};
std::unordered_map<uint32_t, WaterHeight> g_water_heights;
// 4z30: how each shiny model's two corner vectors run against its texture coordinates (BumpFrameOf), by the mesh's
// key; its normal maps as ngl_vk's bumped path reads them (BumpTextureFor), by the Direct3D texture and that frame;
// and the highlights' lookup pictures (LookupTextureFor), by the Direct3D texture.
std::unordered_map<uint64_t, uint8_t> g_bump_frames;
std::map<std::pair<uint32_t, uint8_t>, uint32_t> g_bump_textures;
std::unordered_map<uint32_t, uint32_t> g_lookup_textures;
// 4z52: the highlights' lookup once more as a cube (LookupCubeFor), by the Direct3D texture
std::unordered_map<uint32_t, uint32_t> g_lookup_cubes;

void ForgetAll() {
  ngl_vk_forget();
  g_textures.clear();
  g_opaque_textures.clear();
  g_turned_textures.clear();
  g_meshes.clear();
  g_shapes.clear();  // 4z59 (MeshUse)
  g_shapes_seen.clear();
  g_this_frame.clear();
  g_in_order = 0;
  g_lightmaps.clear();
  g_skies.clear();
  g_sky_boxes.clear();
  g_grid_weights.clear();
  g_water_cubes.clear();
  g_water_heights.clear();
  g_bump_frames.clear();
  g_bump_textures.clear();
  g_lookup_textures.clear();
  g_lookup_cubes.clear();
  g_forget = false;
}

// 4z9: source and destination blend both ONE (2): the color added whatever its alpha.
bool OneAndOne(uint32_t state0) { return (state0 & 0xFu) == 2 && ((state0 >> 4) & 0xFu) == 2; }

// 4z9: opaque, a copy with every texel's alpha at 1, for a material that adds its color whatever its alpha (MeshFor's
// draws: ONE + ONE).
uint32_t TextureFor(const uint8_t* base, uint32_t texture, const std::array<uint32_t, 6>& fetch, bool opaque) {
  if (texture == 0) return 0;
  auto& textures = opaque ? g_opaque_textures : g_textures;
  auto found = textures.find(texture);
  if (found != textures.end() && found->second.fetch == fetch) return found->second.id;
  if (textures.size() >= kMaxTextures) g_forget = true;  // forgotten before the next frame (meshes would go too)
  CachedTexture& cached = textures[texture];
  cached.fetch = fetch;
  cached.id = 0;
  std::vector<uint8_t> bgra;
  uint32_t width = 0, height = 0;
  std::string why;
  if (DecodeFetch(base, fetch, bgra, width, height, why)) {
    // with the game's own smaller levels when they can be read (DecodeLevelsFetch), sampled between levels
    std::vector<std::vector<uint8_t>> levels;
    DecodeLevelsFetch(base, fetch, levels);
    if (opaque) {
      for (size_t i = 3; i < bgra.size(); i += 4) bgra[i] = 255;
      for (auto& level : levels)
        for (size_t i = 3; i < level.size(); i += 4) level[i] = 255;
    }
    std::vector<const void*> pictures{bgra.data()};
    for (const auto& level : levels) pictures.push_back(level.data());
    cached.id = ngl_vk_texture_levels(pictures.data(), static_cast<uint32_t>(pictures.size()), width, height);
    if (cached.id == 0) why = "ngl_vk has no room";
    ++(levels.empty() ? g_textures_top_only : g_textures_with_levels);
  }
  if (cached.id == 0 && g_failed[why]++ == 0)
    REXLOG_INFO("cod2: native side: texture {:08X} not drawn ({}); its color alone is (further ones not logged)",
                texture, why);
  return cached.id;
}

// 4z51: a texture turned over, every texel 1 - its color with alpha 1, top level only: what a decal's second pass
// adds is (1 - texture x color) x alpha x the fog (the corners' color is taken as white, as the decals seen so far
// have it). 0 when it can't be read.
uint32_t TurnedTextureFor(const uint8_t* base, uint32_t texture, const std::array<uint32_t, 6>& fetch) {
  if (texture == 0) return 0;
  auto found = g_turned_textures.find(texture);
  if (found != g_turned_textures.end() && found->second.fetch == fetch) return found->second.id;
  if (g_turned_textures.size() >= kMaxTextures) g_forget = true;
  CachedTexture& cached = g_turned_textures[texture];
  cached.fetch = fetch;
  cached.id = 0;
  std::vector<uint8_t> bgra;
  uint32_t width = 0, height = 0;
  std::string why;
  if (DecodeFetch(base, fetch, bgra, width, height, why)) {
    for (size_t i = 0; i < bgra.size(); ++i) bgra[i] = i % 4 == 3 ? 255 : static_cast<uint8_t>(255 - bgra[i]);
    cached.id = ngl_vk_texture(bgra.data(), width, height);
    if (cached.id == 0) g_forget = true;
  }
  return cached.id;
}

// A fetched channel's value: the byte in a BGRA texel for channel 0-3 (x, y, z, w), as the fetch constant's sign for
// it says (dword 0, two bits a channel from bit 2: 0 unsigned, 1 signed, 2 unsigned biased (2t - 1), 3 gamma, taken
// as unsigned here).
float ChannelValue(const uint8_t* bgra, uint32_t channel, const std::array<uint32_t, 6>& fetch) {
  static constexpr int kByte[4] = {2, 1, 0, 3};
  const uint8_t b = bgra[kByte[channel]];
  switch ((fetch[0] >> (2 + 2 * channel)) & 3) {
    case 1:
      return std::max(-1.0f, static_cast<float>(static_cast<int8_t>(b)) / 127.0f);
    case 2:
      return static_cast<float>(b) / 127.5f - 1.0f;
    default:
      return static_cast<float>(b) / 255.0f;
  }
}

// The fetch constant's exponent adjustment (dword 3 bits 13-18, signed): every value fetched is times 2 to it.
float ExpScale(const std::array<uint32_t, 6>& fetch) {
  int e = static_cast<int>((fetch[3] >> 13) & 0x3F);
  if (e & 0x20) e -= 64;
  return std::ldexp(1.0f, e);
}

// Step 4: the world's light as one ngl_vk lightmap. Call of Duty 2's lightmaps are directional (its world pixel
// shaders, such as 203A8845DF338725 in run cod2shaders1): slots 2, 3 and 4 ("*lightmapN_0", "_1", "_2") hold the red,
// green and blue light as four coefficients each, and slot 5 a table of the four weights, looked up at the normal map's
// x and y (slot 7); a pixel's light is each color's coefficients dotted with those weights, plus the sun: its color
// (pixel constant c17) times slot 1 ("*lightmapN_3", the sun's share there) times the shadow cookie (slot 6) times the
// normal against the sun's direction (c16). This makes the light of the surface's normal map on average (4w, below)
// into a picture the size of slot 2, its color halved (drawn with lightmap_scale 2, so up to twice white) and its
// alpha the sun's share from slot 1, for ngl_vk's lightmap_scale shading. 0 when a texture can't be read (the draw is
// then lit by ngl_vk's fixed light).
uint32_t WorldLightmapFor(const uint8_t* base, const MeshDraw& m) {
  for (uint32_t slot = 1; slot <= 5; ++slot)
    if (m.textures[slot] == 0) return 0;
  const auto key =
      std::make_tuple(m.textures[1], m.textures[2], m.textures[3], m.textures[4], m.textures[5], m.textures[7]);
  if (auto found = g_lightmaps.find(key); found != g_lightmaps.end()) return found->second;
  uint32_t& id = g_lightmaps[key];
  id = 0;
  std::vector<uint8_t> planes[6];
  uint32_t widths[6] = {}, heights[6] = {};
  for (uint32_t slot = 1; slot <= 5; ++slot) {
    std::string why;
    if (!DecodeFetch(base, m.fetches[slot], planes[slot], widths[slot], heights[slot], why)) {
      if (!g_logged_lightmap) {
        g_logged_lightmap = true;
        REXLOG_INFO("cod2: native side: a world lightmap not made: slot {} ({})", slot, why);
      }
      return 0;
    }
  }
  const uint32_t width = widths[2], height = heights[2];
  if (widths[3] != width || widths[4] != width || heights[3] != height || heights[4] != height) return 0;
  // 4w: the four weights. The pixel shader looks the table up at the normal map's x and y as fetched (tfetch2D r1,
  // r8.xy, tf5): Moscow's normal maps are DXN with both unsigned (run cod2side30: format 49, signs 0, texels averaging
  // 0.58 and 0.57), so a flat normal lands in the table's middle (weights 0.015 0.014 0.953 0.015), not on its corner
  // (0.313 0.280 0 0.407: a normal tilted all the way, 4c to 4v). Here: the table's weights averaged over the normal
  // map's texels (every texel of up to 64 x 64, more spread out on a bigger map), each looked up as the shader does,
  // filtered linearly and wrapped (the table's clamp bits are 0); without a normal map (slot 7), the middle's.
  const auto& table = m.fetches[5];
  const uint32_t tw = widths[5], th = heights[5];
  auto weights_at = [&](float u, float v, float w[4], float share) {
    const float tu = u * static_cast<float>(tw) - 0.5f, tv = v * static_cast<float>(th) - 0.5f;
    const float fu = std::floor(tu), fv = std::floor(tv);
    const auto wrap = [](int i, uint32_t n) {
      const int size = static_cast<int>(n);
      return static_cast<uint32_t>(((i % size) + size) % size);
    };
    for (int c = 0; c < 4; ++c) {
      const int dx = c & 1, dy = c >> 1;
      const float part = (dx ? tu - fu : 1.0f - (tu - fu)) * (dy ? tv - fv : 1.0f - (tv - fv)) * share;
      const uint32_t x = wrap(static_cast<int>(fu) + dx, tw), y = wrap(static_cast<int>(fv) + dy, th);
      const uint8_t* t = &planes[5][(static_cast<size_t>(y) * tw + x) * 4];
      for (uint32_t k = 0; k < 4; ++k) w[k] += part * ChannelValue(t, k, table) * ExpScale(table);
    }
  };
  float weight[4] = {};
  std::vector<uint8_t> normals;
  uint32_t nw = 0, nh = 0;
  std::string normals_why;
  const auto& nf = m.fetches[7];
  const int light_test = REXCVAR_GET(cod2_debug_native_light_test);
  if (light_test == 3 || light_test == 4) {  // tests: a flat normal's weights, a fully tilted one's
    const float at = light_test == 3 ? 0.5f : 0.0f;
    weights_at(at, at, weight, 1.0f);
  } else if (m.textures[7] != 0 && DecodeFetch(base, nf, normals, nw, nh, normals_why) && nw > 0 && nh > 0) {
    const uint32_t step_x = std::max(1u, nw / 64u), step_y = std::max(1u, nh / 64u);
    const float share = 1.0f / static_cast<float>(((nw + step_x - 1) / step_x) * ((nh + step_y - 1) / step_y));
    for (uint32_t y = 0; y < nh; y += step_y)
      for (uint32_t x = 0; x < nw; x += step_x) {
        const uint8_t* n = &normals[(static_cast<size_t>(y) * nw + x) * 4];
        weights_at(ChannelValue(n, 0, nf) * ExpScale(nf), ChannelValue(n, 1, nf) * ExpScale(nf), weight, share);
      }
  } else {
    weights_at(0.5f, 0.5f, weight, 1.0f);
  }
  const float scale[3] = {ExpScale(m.fetches[2]), ExpScale(m.fetches[3]), ExpScale(m.fetches[4])};
  const uint32_t sw = widths[1], sh = heights[1];
  std::vector<uint8_t> out(static_cast<size_t>(width) * height * 4);
  float brightest = 0.0f;
  for (uint32_t y = 0; y < height; ++y) {
    for (uint32_t x = 0; x < width; ++x) {
      const size_t at = (static_cast<size_t>(y) * width + x) * 4;
      float light[3];
      for (uint32_t c = 0; c < 3; ++c) {
        const uint8_t* t = &planes[2 + c][at];
        float v = 0.0f;
        for (uint32_t k = 0; k < 4; ++k) v += ChannelValue(t, k, m.fetches[2 + c]) * weight[k];
        light[c] = std::max(0.0f, v * scale[c]);
        brightest = std::max(brightest, light[c]);
      }
      // the sun's share: slot 1's texels over the same part of the map, averaged
      const uint32_t x0 = x * sw / width, x1 = std::max(x0 + 1, (x + 1) * sw / width);
      const uint32_t y0 = y * sh / height, y1 = std::max(y0 + 1, (y + 1) * sh / height);
      float sun = 0.0f;
      for (uint32_t sy = y0; sy < y1; ++sy)
        for (uint32_t sx = x0; sx < x1; ++sx)
          sun += ChannelValue(&planes[1][(static_cast<size_t>(sy) * sw + sx) * 4], 0, m.fetches[1]);
      sun *= ExpScale(m.fetches[1]) / static_cast<float>((x1 - x0) * (y1 - y0));
      auto byte = [](float v) { return static_cast<uint8_t>(std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f)); };
      out[at + 0] = byte(light[2] * 0.5f);
      out[at + 1] = byte(light[1] * 0.5f);
      out[at + 2] = byte(light[0] * 0.5f);
      out[at + 3] = light_test == 2 ? 255 : byte(sun);
    }
  }
  // 4z57: the smaller levels. The game samples slots 1-4 with their own smaller levels (the fetch constants' last
  // level, dword 4 bits 6-9), so where the map's texels are small on screen it reads an average over a wider part of
  // the map: the tank hunt's sunlit brick wall fades toward its top, far from the eye, where the native one, with the
  // top level alone, kept a sharp shadow edge (runs 207 and 216; test 10 showed the share itself read right). Here
  // the made picture gets as many levels as the game's maps have (slot 1's share, twice the size, one fewer), each the
  // average of 2 x 2 texels of the one above, every channel (the share in alpha too); none when the game's have none.
  const uint32_t share_last = (m.fetches[1][4] >> 6) & 0xFu, light_last = (m.fetches[2][4] >> 6) & 0xFu;
  const uint32_t wanted = std::max(light_last, share_last > 0 ? share_last - 1 : 0u);
  std::vector<std::vector<uint8_t>> levels;
  for (uint32_t level = 1, w = width, h = height; level <= wanted && (w > 1 || h > 1); ++level) {
    const std::vector<uint8_t>& above = levels.empty() ? out : levels.back();
    const uint32_t nw = std::max(1u, w / 2), nh = std::max(1u, h / 2);
    std::vector<uint8_t> next(static_cast<size_t>(nw) * nh * 4);
    for (uint32_t y = 0; y < nh; ++y)
      for (uint32_t x = 0; x < nw; ++x)
        for (uint32_t c = 0; c < 4; ++c) {
          uint32_t sum = 0, count = 0;
          for (uint32_t dy = 0; dy < 2; ++dy)
            for (uint32_t dx = 0; dx < 2; ++dx) {
              const uint32_t sx = std::min(2 * x + dx, w - 1), sy = std::min(2 * y + dy, h - 1);
              sum += above[(static_cast<size_t>(sy) * w + sx) * 4 + c];
              ++count;
            }
          next[(static_cast<size_t>(y) * nw + x) * 4 + c] = static_cast<uint8_t>((sum + count / 2) / count);
        }
    levels.push_back(std::move(next));
    w = nw;
    h = nh;
  }
  std::vector<const void*> pictures{out.data()};
  for (const auto& level : levels) pictures.push_back(level.data());
  id = ngl_vk_texture_levels(pictures.data(), static_cast<uint32_t>(pictures.size()), width, height);
  if (id == 0) g_forget = true;
  static int levels_logged = 0;
  if (levels_logged < 4) {
    ++levels_logged;
    REXLOG_INFO("cod2: native side: a world lightmap with {} smaller levels (the game's last level: slot 1 {}, slots 2 "
                "to 4 {} {} {})", levels.size(), share_last, light_last, (m.fetches[3][4] >> 6) & 0xFu,
                (m.fetches[4][4] >> 6) & 0xFu);
  }
  // [waw-decomp] 2026-11-11 4z55, test 10: the sun's share as read from slot 1 and the light made here (its color
  // halved; blue, green, red), saved once a lightmap, to lay side by side on the PC: the tank hunt's sunlit wall is
  // lit natively where the game's light fades (run 207)
  if (light_test == 10) {
    std::error_code ec;
    std::filesystem::create_directories(g_folder, ec);
    char stem[32];
    std::snprintf(stem, sizeof stem, "lightmap-%08X", m.textures[1]);
    std::vector<uint8_t> share(static_cast<size_t>(sw) * sh * 4, 255);
    for (size_t i = 0; i < static_cast<size_t>(sw) * sh; ++i) {
      const float v = ChannelValue(&planes[1][i * 4], 0, m.fetches[1]) * ExpScale(m.fetches[1]);
      const uint8_t b = static_cast<uint8_t>(std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f));
      share[i * 4 + 0] = share[i * 4 + 1] = share[i * 4 + 2] = b;
    }
    const bool saved = WritePpm(g_folder / (std::string(stem) + "-share.ppm"), share.data(), size_t(sw) * 4, sw, sh) &&
                       WritePpm(g_folder / (std::string(stem) + "-light.ppm"), out.data(), size_t(width) * 4, width,
                                height);
    REXLOG_INFO("cod2: native side: test 10: {} ({} x {} share, {} x {} light) {}", stem, sw, sh, width, height,
                saved ? "saved" : "not saved");
  }
  if (!g_logged_lightmap) {
    g_logged_lightmap = true;
    REXLOG_INFO("cod2: native side: the world's normal map (slot 7): {} x {} format {}, signs {}{}{}",
                nw, nh, nf[1] & 0x3F, (nf[0] >> 2) & 3, (nf[0] >> 4) & 3,
                normals_why.empty() ? "" : " (" + normals_why + ")", m.textures[7] == 0 ? " (none)" : "");
    auto describe = [&](uint32_t slot) {
      const auto& f = m.fetches[slot];
      char text[160];
      std::snprintf(text, sizeof text, " %u: %ux%u format %u signs %u%u%u%u times %g clamp %u/%u;", slot,
                    widths[slot], heights[slot], f[1] & 0x3F, (f[0] >> 2) & 3, (f[0] >> 4) & 3, (f[0] >> 6) & 3,
                    (f[0] >> 8) & 3, ExpScale(f), (f[0] >> 10) & 7, (f[0] >> 13) & 7);
      return std::string(text);
    };
    REXLOG_INFO("cod2: native side: the first world lightmap:{}{}{}{}{} weights {:.3f} {:.3f} {:.3f} {:.3f}; "
                "brightest light {:.2f}; sun direction (c16) {:.3f} {:.3f} {:.3f}, color (c17) {:.3f} {:.3f} {:.3f}",
                describe(1), describe(2), describe(3), describe(4), describe(5), weight[0], weight[1], weight[2],
                weight[3], brightest, m.sun_direction[0], m.sun_direction[1], m.sun_direction[2], m.sun_color[0],
                m.sun_color[1], m.sun_color[2]);
  }
  return id;
}

// Step 4: the world's corner normals. Vertex format 1's word at +0x0C is the normal as FMT_2_10_10_10, signed (the
// world's vertex shaders, such as A70A57E3BDA8D434: x in bits 0-9, y in 10-19, z in 20-29, each a two's-complement
// part of 511; the tangent and binormal follow at +0x24 and +0x28). Checked once against the first world geometry with
// enough triangles (their edges crossed): 0 not checked yet, 1 they line up (used), -1 they don't (no corner normals:
// ngl_vk then takes the face's).
int g_world_normals = 0;
uint64_t g_volume_misses = 0;  // static models' corners outside their light volume (MeshFor), all told

void UnpackNormal(uint32_t word, float out[3]) {
  float v[3];
  for (int k = 0; k < 3; ++k) {
    int part = static_cast<int>((word >> (10 * k)) & 0x3FF);
    if (part & 0x200) part -= 0x400;
    v[k] = static_cast<float>(part) / 511.0f;
  }
  const float length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
  for (int k = 0; k < 3; ++k) out[k] = length > 0.0f ? v[k] / length : 0.0f;
}

void CheckWorldNormals(const uint8_t* base, const MeshDraw& m) {
  if (g_world_normals != 0 || m.stride != 44 || m.used.size() < 3 * 8) return;
  double agree = 0.0, sign = 0.0;
  int counted = 0;
  for (size_t t = 0; t + 2 < m.used.size() && counted < 256; t += 3) {
    float p[3][3];
    uint32_t word = 0;
    for (int c = 0; c < 3; ++c) {
      const uint32_t v = m.vertices + m.used[t + c] * m.stride;
      for (int k = 0; k < 3; ++k) p[c][k] = GpuF32(base, v + 4u * k);
      if (c == 0) word = GpuU32(base, v + 0xCu);
    }
    const float e1[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
    const float e2[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
    float g[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
    const float length = std::sqrt(g[0] * g[0] + g[1] * g[1] + g[2] * g[2]);
    if (!(length > 1e-3f)) continue;
    float n[3];
    UnpackNormal(word, n);
    const double d = (g[0] * n[0] + g[1] * n[1] + g[2] * n[2]) / length;
    agree += std::fabs(d);
    sign += d;
    ++counted;
  }
  if (counted < 8) return;
  g_world_normals = agree / counted > 0.85 ? 1 : -1;
  REXLOG_INFO("cod2: native side: the world's packed normals against {} triangles' own: {:.3f} alike ({:.3f} with "
              "sign); {}", counted, agree / counted, sign / counted, g_world_normals > 0 ? "used" : "not used");
}

// A decal's corner color (0xAARRGGBB) with red, green and blue times its alpha. mul's pixel shader makes
// lerp(1, texture x color, alpha), and the color multiplies what's behind; NGL_VK_BLEND_MULTIPLY gives behind x (its
// texture x this color) + behind x (1 - alpha), the same when the texture's own alpha is 1.
uint32_t TimesItsAlpha(uint32_t color) {
  const uint32_t alpha = color >> 24;
  auto times = [alpha](uint32_t byte) { return (byte * alpha + 127u) / 255u; };
  return (alpha << 24) | times((color >> 16) & 0xFFu) << 16 | times((color >> 8) & 0xFFu) << 8 | times(color & 0xFFu);
}

// Step 4s: the static models' light volume. lprobe_smc's pixel shader (2E1F7BB17B21C0AA in run cod2shaders1) lights
// a static model from slot 0, a 3D texture (Moscow: 64 x 64 x 2, 8_8_8_8, tiled) the game fills itself: each model is
// given a 2 x 2 x 2 block of it, the light from eight directions. The vertex shader (B335C2F94428DDF8) passes each
// corner's lighting coordinate (+0x24: two signed 16-bit numbers, the high one x and the low one y; its literal c254.z
// makes them texture coordinates, and the third is 0.5, between the two slices): the block's center. c254.z is
// 1/32768, a 16-bit normalized number: run cod2side21 logged the cache's coordinates up to 32,256 and every one an odd
// multiple of 512, so over 32768 they land on odd texel borders (texels 1, 7, ... 63 of 64), the centers of blocks
// that start on even texels (over 16384 they would run past the volume's edge). The pixel shader moves the lookup
// toward the normal: the normal in a frame of its own (kVolumeFrame) divided by its largest part, times c23 (half a
// texel in x and y, a quarter of the depth in z), so a normal along one of the frame's eight diagonals lands on one
// texel's center, and others blend the texels around them (filtered linearly: the steps are made for that). The
// texel's red, green and blue are the ambient light and its alpha the sun's share: light = ambient + c17 x share x
// max(N . c16, 0), and the pixel is texture x vertex color x light, fogged. Here it is done per corner, with the
// corner's normal (not the normal map's).
//
// The frame: the shader's own literals (its c254 and c255; the game's constant mirror doesn't hold them) aren't known,
// but its arithmetic gives the frame's rows as (a, 0, b), (e, f, b) and (d, c, b): three directions at one height. The
// orthonormal frame of that shape: three directions 120 degrees apart around the up axis, each 35.26 degrees above the
// horizon, so the frame's (1, 1, 1) diagonal is straight up and (-1, -1, -1) straight down. The sign of the second and
// third rows' sideways part is a guess (the other choice mirrors the sideways light, not the up and down).
constexpr float kVolumeFrame[3][3] = {
    {0.81649658f, 0.0f, 0.57735027f},           // (sqrt(2/3), 0, 1/sqrt(3))
    {-0.40824829f, 0.70710678f, 0.57735027f},   // (-1/sqrt(6), 1/sqrt(2), 1/sqrt(3))
    {-0.40824829f, -0.70710678f, 0.57735027f},  // (-1/sqrt(6), -1/sqrt(2), 1/sqrt(3))
};

// True when a draw is a static model lit by such a volume: type 17 or 14 in the 64-byte layout, with a tiled 3D
// 8_8_8_8 texture in slot 0 (fetch constant: dimension in dword 5 bits 9-10, 2; format dword 1 bits 0-5, 6; endian
// bits 6-7, 2: each 32-bit texel then reads as 0xAARRGGBB). 4z47: of any size up to 1024 x 1024 x 64 (it was 64 x
// 64 x 4, Moscow's 64 x 64 x 2 in mind: on the Toujane ride no static model passed, and its crates and radio were
// lit as loose models are, bright in a dark room; SetMeshes logs the first that don't pass).
bool UsesLightVolume(const MeshDraw& m) {
  if (m.stride != 64 || (m.type != kStaticModelType && m.type != kLooseModelType) || m.textures[0] == 0) return false;
  const std::array<uint32_t, 6>& fetch = m.fetches[0];
  const uint32_t width = (fetch[2] & 0x7FFu) + 1, height = ((fetch[2] >> 11) & 0x7FFu) + 1,
                 depth = ((fetch[2] >> 22) & 0x3FFu) + 1;
  return ((fetch[5] >> 9) & 3u) == 2 && (fetch[1] & 0x3Fu) == 6 && ((fetch[1] >> 6) & 3u) == 2 && (fetch[0] >> 31) &&
         width <= 1024 && height <= 1024 && depth <= 64;
}

// A static model's light at one corner (vertex: the corner's physical address), as above. light gets red, green and
// blue (1: as bright as the texture). False, and light untouched, when the corner's lighting coordinate is outside
// the volume (run cod2side20: some pines' were, and lit from the volume's edge they came out in patches of color).
bool VolumeLightAt(const uint8_t* base, const MeshDraw& m, uint32_t vertex, float light[3]) {
  const std::array<uint32_t, 6>& fetch = m.fetches[0];
  const int32_t size[3] = {static_cast<int32_t>(fetch[2] & 0x7FFu) + 1,
                           static_cast<int32_t>((fetch[2] >> 11) & 0x7FFu) + 1,
                           static_cast<int32_t>((fetch[2] >> 22) & 0x3FFu) + 1};
  const uint32_t pitch = std::max(((fetch[0] >> 22) & 0x1FFu) * 32u, static_cast<uint32_t>(size[0]));
  const uint32_t at = fetch[1] & 0x1FFFF000u;
  // the normal in world space (c4-c6), made unit length; then in the frame, divided by its largest part
  float normal[3], world[3] = {}, frame[3] = {};
  for (uint32_t k = 0; k < 3; ++k) normal[k] = GpuF32(base, vertex + NormalAt(m.type) + 4u * k);
  for (int r = 0; r < 3; ++r)
    for (int k = 0; k < 3; ++k) world[r] += m.to_world[3 * r + k] * normal[k];
  const float length = std::sqrt(world[0] * world[0] + world[1] * world[1] + world[2] * world[2]);
  if (length > 0.0f)
    for (float& w : world) w /= length;
  for (int r = 0; r < 3; ++r)
    for (int k = 0; k < 3; ++k) frame[r] += kVolumeFrame[r][k] * world[k];
  const float largest = std::max({std::fabs(frame[0]), std::fabs(frame[1]), std::fabs(frame[2])});
  for (float& f : frame) f = largest > 0.0f && !m.lit_all_round ? f / largest : 0.0f;  // 4u: the center itself
  // the block's center: a cached model's corner carries it (+0x24), a model drawn by itself has it in vertex c23
  const uint32_t word = GpuU32(base, vertex + 0x24u);
  const bool cached = m.type == kStaticModelType;
  const float center[2] = {cached ? static_cast<int16_t>(word >> 16) / 32768.0f : m.volume_center[0],
                           cached ? static_cast<int16_t>(word & 0xFFFFu) / 32768.0f : m.volume_center[1]};
  if (!(center[0] > 0.0f && center[0] < 1.0f && center[1] > 0.0f && center[1] < 1.0f)) return false;
  const float coordinate[3] = {center[0] + frame[0] * m.volume_step[0], center[1] + frame[1] * m.volume_step[1],
                               (cached ? 0.5f : m.volume_center[2]) + frame[2] * m.volume_step[2]};
  // linear filtering: the eight texels whose centers are around the coordinate, by how near each is (clamped to the
  // edges); red, green, blue and alpha summed by those weights
  int32_t low[3];
  float part[3];
  for (int k = 0; k < 3; ++k) {
    const float texel = coordinate[k] * static_cast<float>(size[k]) - 0.5f;
    low[k] = static_cast<int32_t>(std::floor(texel));
    part[k] = texel - static_cast<float>(low[k]);
  }
  float sum[4] = {};
  uint32_t texels[8] = {};  // for the log
  for (int corner = 0; corner < 8; ++corner) {
    int32_t xyz[3];
    float weight = 1.0f;
    for (int k = 0; k < 3; ++k) {
      const int bit = (corner >> k) & 1;
      xyz[k] = std::clamp(low[k] + bit, 0, size[k] - 1);
      weight *= bit ? part[k] : 1.0f - part[k];
    }
    if (weight <= 0.0f) continue;
    const uint32_t texel =
        GpuU32(base, at + static_cast<uint32_t>(TiledOffset3D(xyz[0], xyz[1], xyz[2], pitch, size[1], 2)));
    texels[corner] = texel;
    sum[0] += weight * static_cast<float>((texel >> 16) & 0xFFu);
    sum[1] += weight * static_cast<float>((texel >> 8) & 0xFFu);
    sum[2] += weight * static_cast<float>(texel & 0xFFu);
    sum[3] += weight * static_cast<float>(texel >> 24);
  }
  const float facing = std::max(m.sun_direction[0] * world[0] + m.sun_direction[1] * world[1] +
                                    m.sun_direction[2] * world[2], 0.0f);
  for (int k = 0; k < 3; ++k)  // 4u: lit all round, the whole sun's share in the vertex shader's color of it
    light[k] = sum[k] / 255.0f + (m.lit_all_round ? m.vertex_sun[k] : m.sun_color[k] * facing) * (sum[3] / 255.0f);
  static bool logged[2] = {};  // the first corner of each kind (cached, by itself), to check the numbers
  if (!logged[cached]) {
    logged[cached] = true;
    REXLOG_INFO("cod2: native side: the static models' light volume ({}): {}x{}x{} (pitch {}), c23 {:.4f} {:.4f} "
                "{:.4f}, c4-c6 {:.2f} {:.2f} {:.2f} / {:.2f} {:.2f} {:.2f} / {:.2f} {:.2f} {:.2f}; the first corner: "
                "lookup {:.4f} {:.4f} {:.3f}, ambient {:.3f} {:.3f} {:.3f}, sun's share {:.3f}, facing {:.3f}, light "
                "{:.3f} {:.3f} {:.3f}; the texels around {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}; "
                "its color {:08X}{}",
                cached ? "cached" : "by itself", size[0], size[1], size[2], pitch, m.volume_step[0], m.volume_step[1],
                m.volume_step[2], m.to_world[0], m.to_world[1], m.to_world[2], m.to_world[3], m.to_world[4],
                m.to_world[5], m.to_world[6], m.to_world[7], m.to_world[8], coordinate[0], coordinate[1],
                coordinate[2], sum[0] / 255.0f, sum[1] / 255.0f, sum[2] / 255.0f, sum[3] / 255.0f, facing, light[0],
                light[1], light[2], texels[0], texels[1], texels[2], texels[3], texels[4], texels[5], texels[6],
                texels[7], GpuU32(base, vertex + ColorAt(m.type)), m.lit_all_round ? " (lit all round)" : "");
  }
  return true;
}

// A geometry as an ngl_vk mesh, made the first time it is drawn: the corners its indices use, in the order first used,
// as ngl_vk takes them (position, texture coordinate, lightmap coordinate, color, normal), and its triangles. 0 when
// it can't be made (more than 65,535 corners, or no room: then everything is forgotten before the next frame).
bool EyeOf(const float view_projection[16], double eye[3]);  // below, with the sun's shadow

// 4z51: the fog's share at a distance from the eye, as the game's vertex shaders make it: 1 - 2^(exponent x the
// distance) for the exponential fog (kFogLiteral), the way from the start to the end for the linear one, else 0.
float FogShareAt(const MeshDraw& m, float distance) {
  if (m.fog_exponent < 0.0f) return std::clamp(1.0f - std::exp2(m.fog_exponent * distance), 0.0f, 1.0f);
  if (m.fog_end > m.fog_start && m.fog_end > 0.0f)
    return std::clamp((distance - m.fog_start) / (m.fog_end - m.fog_start), 0.0f, 1.0f);
  return 0.0f;
}

// 4z59: the mesh whose indices are 0 to 65534 in order (MeshUse), made once after each ForgetAll; 0 when there is no
// room.
uint32_t InOrderMesh() {
  if (g_in_order != 0) return g_in_order;
  std::vector<uint16_t> indices(kInOrder);
  for (uint32_t i = 0; i < kInOrder; ++i) indices[i] = static_cast<uint16_t>(i);
  const float corner[NGL_VK_CORNER_FLOATS] = {};
  g_in_order = ngl_vk_mesh(corner, 1, indices.data(), kInOrder);
  if (g_in_order == 0) g_forget = true;
  return g_in_order;
}

// 4z59: the kept mesh with these indices (MeshUse, a shape), made when they are drawn in a second frame; 0 while they
// have been drawn in this frame only, or when there is no room.
uint32_t ShapeMesh(const std::vector<uint16_t>& indices, uint64_t frame) {
  uint64_t key = 14695981039346656037ull;
  for (const uint16_t index : indices) {
    key = (key ^ (index & 0xFFu)) * 1099511628211ull;
    key = (key ^ (index >> 8)) * 1099511628211ull;
  }
  key = (key ^ indices.size()) * 1099511628211ull;
  if (auto found = g_shapes.find(key); found != g_shapes.end()) return found->second;
  if (auto seen = g_shapes_seen.find(key); seen == g_shapes_seen.end() || seen->second == frame) {
    if (seen == g_shapes_seen.end()) {
      if (g_shapes_seen.size() >= kMaxSeen) g_shapes_seen.clear();
      g_shapes_seen.emplace(key, frame);
    }
    return 0;
  }
  g_shapes_seen.erase(key);
  const float corner[NGL_VK_CORNER_FLOATS] = {};
  const uint32_t id = ngl_vk_mesh(corner, 1, indices.data(), static_cast<uint32_t>(indices.size()));
  if (id == 0) {
    g_forget = true;
    return 0;
  }
  g_shapes[key] = id;
  return id;
}

// 4z59: a draw's geometry from its corners and indices (MeshUse): a mesh of its own when its key was drawn in an
// earlier frame too, else this frame's corners with the triangles of a shared mesh (in_order: the indices are already
// 0, 1, 2, ... in order). When the frame's room is used up, a mesh of its own after all.
MeshUse Place(uint64_t key, uint64_t frame, const std::vector<float>& corners, const std::vector<uint16_t>& indices,
              bool in_order, uint32_t& built) {
  MeshUse use;
  const uint32_t count = static_cast<uint32_t>(corners.size() / NGL_VK_CORNER_FLOATS);
  const uint32_t index_count = static_cast<uint32_t>(indices.size());
  if (count == 0 || index_count == 0) {
    g_meshes[key] = 0;  // nothing to draw: remembered as such
    return use;
  }
  bool again = false;
  if (auto seen = g_seen.find(key); seen != g_seen.end()) {
    again = seen->second != frame;
  } else {
    if (g_seen.size() >= kMaxSeen) g_seen.clear();
    g_seen.emplace(key, frame);
  }
  if (!again || g_mesh_room_tight) {  // 4z62 (g_mesh_room_tight)
    uint32_t first = 0;
    const uint32_t shape = in_order ? 0 : ShapeMesh(indices, frame);
    if (shape != 0) {  // its own triangles, its corners as they are
      first = ngl_vk_frame_corners(corners.data(), count);
      if (first != 0) {
        use.mesh = shape;
        ++g_mesh_count.shape_draws;
        g_mesh_count.frame_corners += count;
      }
    } else if (index_count <= kInOrder && InOrderMesh() != 0) {  // the indices in order, the corners written out
      if (in_order) {
        first = ngl_vk_frame_corners(corners.data(), count);
      } else {
        std::vector<float> written(static_cast<size_t>(index_count) * NGL_VK_CORNER_FLOATS);
        for (uint32_t i = 0; i < index_count; ++i)
          std::memcpy(&written[static_cast<size_t>(i) * NGL_VK_CORNER_FLOATS],
                      &corners[static_cast<size_t>(indices[i]) * NGL_VK_CORNER_FLOATS],
                      NGL_VK_CORNER_FLOATS * sizeof(float));
        first = ngl_vk_frame_corners(written.data(), index_count);
      }
      if (first != 0) {
        use.mesh = g_in_order;
        ++g_mesh_count.order_draws;
        g_mesh_count.frame_corners += in_order ? count : index_count;
      }
    }
    if (first != 0) {
      use.corners = first;
      use.index_count = index_count;
      g_this_frame[key] = use;
      return use;
    }
    ++g_mesh_count.frame_full;
  }
  g_seen.erase(key);
  use.mesh = ngl_vk_mesh(corners.data(), count, indices.data(), index_count);
  if (use.mesh == 0) g_forget = true;
  g_meshes[key] = use.mesh;
  ++g_mesh_count.made;
  g_mesh_count.made_corners += count;
  ++built;
  return use;
}

MeshUse MeshFor(const uint8_t* base, const MeshDraw& m, uint64_t frame, uint32_t& built) {
  if (auto found = g_meshes.find(m.key); found != g_meshes.end()) return MeshUse{found->second, 0, 0};
  if (auto found = g_this_frame.find(m.key); found != g_this_frame.end()) return found->second;  // 4z59
  if (!m.effect.empty()) {  // an effect: its copied corners, each its own (KeepEffect)
    if (m.effect.size() > kInOrder) {
      g_meshes[m.key] = 0;
      return MeshUse{};
    }
    std::vector<float> corners;
    std::vector<uint16_t> indices;
    corners.reserve(m.effect.size() * NGL_VK_CORNER_FLOATS);
    // 4y, a soft effect (KeepEffect): with clip z = a w + b along the way from the eye (a from c2 and c3, b from the
    // corner), the game's fade k x (scene z - its z), k its c25.x, is ngl_vk's (scene depth - its depth) x scale with
    // scale = 1 / (the depth a clip-z step of 1 / k farther on, less its own), in the lightmap u's place (soft_depth);
    // and its alpha times saturate(its clip z x k), as the vertex shader does it
    const float* vp = m.view_projection;
    const float c2c3 = vp[2] * vp[3] + vp[6] * vp[7] + vp[10] * vp[11];
    const float c3c3 = vp[3] * vp[3] + vp[7] * vp[7] + vp[11] * vp[11];
    const float a = c3c3 > 0.0f ? c2c3 / c3c3 : 0.0f;
    const bool soft = m.feather > 0.0f && a > 0.0f && !OneAndOne(m.state0);
    const bool one_one = OneAndOne(m.state0);  // 4z9: added whatever its alpha (the draw's TextureFor too)
    for (const EffectCorner& c : m.effect) {
      uint32_t color = one_one ? c.color | 0xFF000000u : c.color;
      float scale = 0.0f;
      if (soft) {
        const float z = c.x * vp[2] + c.y * vp[6] + c.z * vp[10] + vp[14];
        const float w = c.x * vp[3] + c.y * vp[7] + c.z * vp[11] + vp[15];
        const float b = z - a * w, step = 1.0f / (m.feather * a);
        const float change = w > 0.0f ? b / (w + step) - b / w : 0.0f;  // the depth's change over that step
        scale = std::fabs(change) > 0.0f ? 1.0f / std::fabs(change) : 1e8f;
        const float near_fade = std::clamp(z * m.feather, 0.0f, 1.0f);
        const uint32_t alpha = static_cast<uint32_t>(static_cast<float>(color >> 24) * near_fade + 0.5f);
        color = (color & 0x00FFFFFFu) | (std::min(alpha, 255u) << 24);
      }
      float color_bits;
      std::memcpy(&color_bits, &color, 4);
      const float corner[NGL_VK_CORNER_FLOATS] = {c.x, c.y, c.z, c.u, c.v, scale, 0.0f, color_bits, 0.0f, 0.0f, 0.0f};
      indices.push_back(static_cast<uint16_t>(corners.size() / NGL_VK_CORNER_FLOATS));
      corners.insert(corners.end(), corner, corner + NGL_VK_CORNER_FLOATS);
    }
    return Place(m.key, frame, corners, indices, true, built);  // 4z59
  }
  if (m.type == kWorldType) CheckWorldNormals(base, m);
  std::unordered_map<uint32_t, uint16_t> local;
  std::vector<float> corners;
  std::vector<uint16_t> indices;
  indices.reserve(m.used.size());
  const bool format0 = m.stride == 64;  // else format 1 (44 bytes)
  // 64 bytes: the rigid models' layout (types 11 and 14) or the static models' cache's (type 17): NormalAt
  const uint32_t normal_at = NormalAt(m.type), color_at = ColorAt(m.type), uv_at = UvAt(m.type);
  const bool by_volume = UsesLightVolume(m);  // lit corner by corner from its light volume (VolumeLightAt)
  // a color times a light, halved (NGL_VK_SHADE_VERTEX_COLOR doubles it again), held to a byte
  auto lit = [](uint32_t byte, float light) {
    return static_cast<uint32_t>(std::clamp(static_cast<float>(byte) * light * 0.5f + 0.5f, 0.0f, 255.0f));
  };
  uint64_t color_sum[4] = {};  // 4z31, for the log: a shiny model's corners' colors (alpha, red, green, blue), summed
  uint32_t color_least[4] = {255, 255, 255, 255};
  uint32_t color_most[4] = {};  // 4z54, for the log: a world mesh's corners' colors at their brightest
  double eye[3] = {};  // 4z51: a decal's second pass measures the fog from the eye
  const bool fog_given = m.decal_fog && EyeOf(m.view_projection, eye);
  for (const uint32_t vertex_index : m.used) {
    auto [it, inserted] = local.emplace(vertex_index, static_cast<uint16_t>(corners.size() / NGL_VK_CORNER_FLOATS));
    if (inserted) {
      if (corners.size() / NGL_VK_CORNER_FLOATS >= 65535) {
        g_meshes[m.key] = 0;  // too many corners for 16-bit indices: not drawn, and remembered as such
        return MeshUse{};
      }
      const uint32_t v = m.vertices + vertex_index * m.stride;
      uint32_t color = GpuU32(base, v + (format0 ? color_at : 0x10u));  // 0xAARRGGBB
      for (int part = 0; part < 4; ++part) {
        const uint32_t byte = (color >> (24 - 8 * part)) & 0xFFu;
        color_sum[part] += byte;
        color_least[part] = std::min(color_least[part], byte);
        color_most[part] = std::max(color_most[part], byte);
      }
      if (m.decal_fog) {  // 4z51: alpha x the fog's share here x the fog's color, to be added (alpha 1)
        float share = 0.0f;
        if (fog_given) {
          const double away[3] = {GpuF32(base, v) - eye[0], GpuF32(base, v + 4u) - eye[1],
                                  GpuF32(base, v + 8u) - eye[2]};
          const double distance = std::sqrt(away[0] * away[0] + away[1] * away[1] + away[2] * away[2]);
          share = FogShareAt(m, static_cast<float>(distance)) * static_cast<float>(color >> 24) / 255.0f;
        }
        auto byte = [share](float fog) {
          return static_cast<uint32_t>(std::clamp(share * fog * 255.0f + 0.5f, 0.0f, 255.0f));
        };
        color = 0xFF000000u | byte(m.decal_fog_color[0]) << 16 | byte(m.decal_fog_color[1]) << 8 |
                byte(m.decal_fog_color[2]);
      } else if (m.type == kCloudType) {
        color = TimesItsAlpha(color);  // a decal: lerp(1, ...) by NGL_VK_BLEND_MULTIPLY
      }
      if (by_volume) {  // the vertex color times the corner's light, the alpha kept (4u: a type 14 model's own
                        // color at +0x1C; 4s5 had taken its +0x18, part of its normal, as none: white)
        float light[3];
        if (!VolumeLightAt(base, m, v, light)) {  // outside the volume: the light-probe average and the sun
          float normal[3], facing = 0.0f;
          for (uint32_t k = 0; k < 3; ++k) normal[k] = GpuF32(base, v + normal_at + 4u * k);
          for (int r = 0; r < 3; ++r)
            facing += m.sun_direction[r] * (m.to_world[3 * r] * normal[0] + m.to_world[3 * r + 1] * normal[1] +
                                            m.to_world[3 * r + 2] * normal[2]);
          if (m.lit_all_round) facing = 1.0f;
          for (int k = 0; k < 3; ++k) light[k] = m.ambient[k] + m.sun_color[k] * m.sun_scale * std::max(facing, 0.0f);
          ++g_volume_misses;
        }
        color = (color & 0xFF000000u) | lit((color >> 16) & 0xFFu, light[0]) << 16 |
                lit((color >> 8) & 0xFFu, light[1]) << 8 | lit(color & 0xFFu, light[2]);
      }
      float color_bits;
      std::memcpy(&color_bits, &color, 4);  // as ngl_vk takes it: bytes blue, green, red, alpha
      const uint32_t uv = format0 ? uv_at : 0x14u;
      const float corner[NGL_VK_CORNER_FLOATS] = {
          GpuF32(base, v), GpuF32(base, v + 4u), GpuF32(base, v + 8u),            // position
          GpuF32(base, v + uv), GpuF32(base, v + uv + 4u),                         // texture coordinate
          format0 ? 0.0f : GpuF32(base, v + 0x1Cu), format0 ? 0.0f : GpuF32(base, v + 0x20u),  // lightmap's
          color_bits,
          format0 ? GpuF32(base, v + normal_at) : 0.0f, format0 ? GpuF32(base, v + normal_at + 4u) : 0.0f,  // normal
          format0 ? GpuF32(base, v + normal_at + 8u) : 0.0f};
      corners.insert(corners.end(), corner, corner + NGL_VK_CORNER_FLOATS);
      if (!format0 && g_world_normals > 0)  // format 1's packed normal (CheckWorldNormals), else the face's
        UnpackNormal(GpuU32(base, v + 0xCu), &corners[corners.size() - 3]);
    }
    indices.push_back(it->second);
  }
  const MeshUse use = Place(m.key, frame, corners, indices, false, built);  // 4z59
  static int shiny_logged = 0;
  if (m.shiny && shiny_logged < 12 && !local.empty()) {
    ++shiny_logged;
    const double count = static_cast<double>(local.size());
    REXLOG_INFO("cod2: native side: shiny mesh {}: material \"{}\", {} corners; their colors' averages (alpha, red, "
                "green, blue) {:.0f} {:.0f} {:.0f} {:.0f}, least {} {} {} {}",
                shiny_logged, IsGuestData(m.material) ? Text(base, LoadU32(base, m.material)) : std::string("-"),
                local.size(), color_sum[0] / count, color_sum[1] / count, color_sum[2] / count, color_sum[3] / count,
                color_least[0], color_least[1], color_least[2], color_least[3]);
  }
  // 4z54: the world's corners' colors (the game's world pixel shaders multiply the color map by them; the tank hunt's
  // sunlit brick wall is lighter natively by a share that fades smoothly from its foot to its top, run 207)
  static int world_logged = 0;
  if (m.type == kWorldType && m.effect.empty() && world_logged < 40 && !local.empty()) {
    ++world_logged;
    const double count = static_cast<double>(local.size());
    REXLOG_INFO("cod2: native side: world mesh {}: material \"{}\", {} corners; their colors (alpha, red, green, "
                "blue) average {:.0f} {:.0f} {:.0f} {:.0f}, least {} {} {} {}, most {} {} {} {}",
                world_logged, IsGuestData(m.material) ? Text(base, LoadU32(base, m.material)) : std::string("-"),
                local.size(), color_sum[0] / count, color_sum[1] / count, color_sum[2] / count, color_sum[3] / count,
                color_least[0], color_least[1], color_least[2], color_least[3], color_most[0], color_most[1],
                color_most[2], color_most[3]);
  }
  return use;
}

// How a material's pixels use alpha, from its state word 0 (IW3's GFXS0, as Moscow's materials show it: the pine
// canopies 28127012, alpha-tested at 128 and not culled; the barbed wire 28128812, not tested): source blend in bits
// 0-3 and destination blend in bits 4-7 (0 or ONE and ZERO: not blended), bit 11 set turns the alpha test off (bits
// 12-13 its kind: > 0, < 128 or >= 128). A blended material is drawn blended even when it is also alpha-tested (the
// smoke: run cod2side12 drew it tested, as hard dark shapes).
uint32_t AlphaOf(uint32_t state0) {
  const uint32_t source = state0 & 0xFu, destination = (state0 >> 4) & 0xFu;
  if (source != 0 && !(source == 2 && destination == 1)) return NGL_VK_ALPHA_BLEND;
  return (state0 & 0x800u) == 0 ? NGL_VK_ALPHA_TEST : NGL_VK_ALPHA_OPAQUE;
}

// The sky's stand-in (step 4): a 1 x 1 texture of the average color of the sky's picture (the game's sky.hlsl looks a
// cube map up by direction, which ngl_vk can't draw yet; its first face is averaged), kept by the texture's address
// (g_skies).
uint32_t SkyColorFor(const uint8_t* base, uint32_t texture, const std::array<uint32_t, 6>& fetch) {
  if (texture == 0) return 0;
  if (auto found = g_skies.find(texture); found != g_skies.end()) return found->second;
  uint32_t& id = g_skies[texture];
  id = 0;
  std::vector<uint8_t> bgra;
  uint32_t width = 0, height = 0;
  std::string why;
  if (!DecodeFetch(base, fetch, bgra, width, height, why) || bgra.empty()) return 0;
  uint64_t sum[4] = {};
  for (size_t i = 0; i < bgra.size(); ++i) sum[i & 3] += bgra[i];
  const uint64_t texels = bgra.size() / 4;
  uint8_t average[4];
  for (int k = 0; k < 4; ++k) average[k] = static_cast<uint8_t>(sum[k] / texels);
  average[3] = 255;
  id = ngl_vk_texture(average, 1, 1);
  if (id == 0) g_forget = true;
  REXLOG_INFO("cod2: native side: the sky's stand-in color: red {}, green {}, blue {} (from a {}x{} picture)",
              average[2], average[1], average[0], width, height);
  return id;
}

// A cube map's faces as Direct3D has them: the way a texel points for (s, t) = (2u - 1, 2v - 1) of its face.
constexpr int kSkyFaces[6][3][3] = {
    // x, y, z of the way, each as {s weight, t weight, constant}
    {{0, 0, 1}, {0, -1, 0}, {-1, 0, 0}},  // +X: (1, -t, -s)
    {{0, 0, -1}, {0, -1, 0}, {1, 0, 0}},  // -X: (-1, -t, s)
    {{1, 0, 0}, {0, 0, 1}, {0, 1, 0}},    // +Y: (s, 1, t)
    {{1, 0, 0}, {0, 0, -1}, {0, -1, 0}},  // -Y: (s, -1, -t)
    {{1, 0, 0}, {0, -1, 0}, {0, 0, 1}},   // +Z: (s, -t, 1)
    {{-1, 0, 0}, {0, -1, 0}, {0, 0, -1}}, // -Z: (-s, -t, -1)
};

// One face of a cube map as DecodeFetch gives it (blue, green, red, alpha bytes). The six faces follow one another in
// memory (+X, -X, +Y, -Y, +Z, -Z), each rounded up to 4 KB. False when the texture isn't a cube (fetch constant dword
// 5 bits 9-10, 3) or the face can't be read.
bool DecodeCubeFace(const uint8_t* base, const std::array<uint32_t, 6>& fetch, uint32_t face,
                    std::vector<uint8_t>& bgra, uint32_t& width, uint32_t& height) {
  Format f;
  if (((fetch[5] >> 9) & 3) != 3 || !FormatOf(fetch[1] & 0x3F, &f)) return false;
  const uint32_t w = (fetch[2] & 0x1FFF) + 1, h = ((fetch[2] >> 13) & 0x1FFF) + 1;
  const bool tiled = (fetch[0] >> 31) != 0;
  const uint32_t blocks_x = (w + f.block - 1) / f.block, blocks_y = (h + f.block - 1) / f.block;
  uint32_t pitch_blocks = std::max(((fetch[0] >> 22) & 0x1FF) * 32 / f.block, blocks_x);
  if (tiled) pitch_blocks = (pitch_blocks + 31) & ~31u;
  const uint32_t rows = tiled ? (blocks_y + 31) & ~31u : blocks_y;
  const uint32_t face_bytes = (pitch_blocks * rows * f.bytes + 4095u) & ~4095u;
  const uint32_t physical = fetch[1] & 0x1FFFF000u;
  std::array<uint32_t, 6> face_fetch = fetch;
  face_fetch[1] = (fetch[1] & ~0x1FFFF000u) | ((physical + face * face_bytes) & 0x1FFFF000u);
  std::string why;
  return DecodeFetch(base, face_fetch, bgra, width, height, why);
}

// Solves `terms` least-squares sums in place (row i: the sums of term i with each term, then with the values, at
// [terms]) by elimination, the largest pivot first; afterwards unknown i is a[i][terms] / a[i][i]. False when the sums
// don't settle every unknown.
bool Eliminate(double a[10][11], int terms) {
  for (int column = 0; column < terms; ++column) {
    int pivot = column;
    for (int row = column + 1; row < terms; ++row)
      if (std::fabs(a[row][column]) > std::fabs(a[pivot][column])) pivot = row;
    if (std::fabs(a[pivot][column]) < 1e-12) return false;
    for (int j = 0; j <= terms; ++j) std::swap(a[column][j], a[pivot][j]);
    for (int row = 0; row < terms; ++row) {
      if (row == column) continue;
      const double factor = a[row][column] / a[column][column];
      for (int j = column; j <= terms; ++j) a[row][j] -= factor * a[column][j];
    }
  }
  return true;
}

// 4z24. The models' light as the game's lprobe pixel shaders make it (the PC copy's lightprobe.hlsl says the same):
// two cube maps of four weights each, looked up at the world normal (slots 0 and 1), dotted with the light grid's
// colors (pixel c8-c13: MeshDraw::grid). Here: the eight weights averaged over all directions, each texel counted by
// the solid angle it covers (1 / (1 + s^2 + t^2)^1.5 for a texel at (s, t) of its face), and the weights straight up
// and straight down, for the log. Measured (run cod2side76, Pointe du Hoc): two 32 x 32 cubes of 8_8_8_8; each of the
// eight weights averages 0.125, so the eight sum to 1 (0.5 a cube) and KeepMesh's average, the colors' 24 parts
// summed and divided by 8, was right. Straight up is the second cube's third weight alone (0.992), straight down the
// first cube's second: there the light was 0.83 against 0.38, with 0.47 as the average. So the models' dark look is
// the direction left out, not the average. Null when the two slots aren't cubes that can be read.
const GridWeights* GridWeightsFor(const uint8_t* base, const MeshDraw& m) {
  if (m.textures[0] == 0 || m.textures[1] == 0) return nullptr;
  const auto key = std::make_pair(m.textures[0], m.textures[1]);
  if (auto found = g_grid_weights.find(key); found != g_grid_weights.end())
    return found->second.made ? &found->second : nullptr;
  GridWeights& g = g_grid_weights[key];
  double sums[8] = {};
  double squares[8] = {}, products[8][9] = {}, normal[2][9][9] = {}, cube_angle[2] = {};  // 4z25: the fit's sums
  struct Sample {  // 4z32: every texel once more, for the second fit: its way, its solid angle and its four weights
    double way[3], angle;
    float four[4];
  };
  std::vector<Sample> samples[2];
  std::array<std::vector<uint8_t>, 6> first_faces;  // 4z34: the first cube's faces, for ngl_vk's cube texture
  bool square = true;
  uint32_t size[2] = {};
  for (uint32_t cube = 0; cube < 2; ++cube) {
    double cube_angles = 0.0;
    for (uint32_t face = 0; face < 6; ++face) {
      std::vector<uint8_t> bgra;
      uint32_t w = 0, h = 0;
      if (!DecodeCubeFace(base, m.fetches[cube], face, bgra, w, h) || w == 0 || h == 0) return nullptr;
      size[cube] = w;
      // as the shader reads them: x red, y green, z blue, w alpha
      auto weights = [&](uint32_t x, uint32_t y, float out[4]) {
        const uint8_t* texel = &bgra[(static_cast<size_t>(y) * w + x) * 4];
        out[0] = texel[2] / 255.0f;
        out[1] = texel[1] / 255.0f;
        out[2] = texel[0] / 255.0f;
        out[3] = texel[3] / 255.0f;
      };
      for (uint32_t y = 0; y < h; ++y) {
        for (uint32_t x = 0; x < w; ++x) {
          const double s = 2.0 * (x + 0.5) / w - 1.0, t = 2.0 * (y + 0.5) / h - 1.0;
          const double angle = 1.0 / std::pow(1.0 + s * s + t * t, 1.5);
          float four[4];
          weights(x, y, four);
          for (int k = 0; k < 4; ++k) sums[4 * cube + k] += angle * four[k];
          cube_angles += angle;
          // 4z25: the way this texel points (kSkyFaces), unit length, and the nine terms of it
          double way[3];
          for (int k = 0; k < 3; ++k)
            way[k] = s * kSkyFaces[face][k][0] + t * kSkyFaces[face][k][1] + kSkyFaces[face][k][2];
          const double length = std::sqrt(way[0] * way[0] + way[1] * way[1] + way[2] * way[2]);
          for (double& part : way) part /= length;
          const double terms[9] = {way[0] * way[0], way[1] * way[1], way[2] * way[2], way[0] * way[1], way[1] * way[2],
                                   way[2] * way[0], way[0], way[1], way[2]};
          for (int i = 0; i < 9; ++i) {
            for (int j = 0; j < 9; ++j) normal[cube][i][j] += angle * terms[i] * terms[j];
            for (int k = 0; k < 4; ++k) products[4 * cube + k][i] += angle * terms[i] * four[k];
          }
          for (int k = 0; k < 4; ++k) squares[4 * cube + k] += angle * four[k] * four[k];
          samples[cube].push_back({{way[0], way[1], way[2]}, angle, {four[0], four[1], four[2], four[3]}});
        }
      }
      if (face == 4) weights(w / 2, h / 2, &g.up[4 * cube]);    // +Z
      if (face == 5) weights(w / 2, h / 2, &g.down[4 * cube]);  // -Z
      if (w != h || (face != 0 && w != size[cube])) square = false;
      if (cube == 0) first_faces[face] = bgra;
    }
    for (int k = 0; k < 4; ++k) g.mean[4 * cube + k] = static_cast<float>(sums[4 * cube + k] / cube_angles);
    cube_angle[cube] = cube_angles;
  }
  // 4z25: the least-squares fit of each weight: the nine-by-nine sums solved against its products, by elimination
  // with the largest pivot first; what the fit leaves over is the weight's squares less the fit's share of them
  double left_over = 0.0, total_angle = 0.0;
  for (int w = 0; w < 8; ++w) {
    double a[9][10];
    for (int i = 0; i < 9; ++i) {
      for (int j = 0; j < 9; ++j) a[i][j] = normal[w / 4][i][j];
      a[i][9] = products[w][i];
    }
    for (int column = 0; column < 9; ++column) {
      int pivot = column;
      for (int row = column + 1; row < 9; ++row)
        if (std::fabs(a[row][column]) > std::fabs(a[pivot][column])) pivot = row;
      if (std::fabs(a[pivot][column]) < 1e-12) return nullptr;  // made stays false: the average is used
      for (int j = 0; j < 10; ++j) std::swap(a[column][j], a[pivot][j]);
      for (int row = 0; row < 9; ++row) {
        if (row == column) continue;
        const double factor = a[row][column] / a[column][column];
        for (int j = column; j < 10; ++j) a[row][j] -= factor * a[column][j];
      }
    }
    double explained = 0.0;
    for (int i = 0; i < 9; ++i) {
      g.fit[w][i] = static_cast<float>(a[i][9] / a[i][i]);
      explained += a[i][9] / a[i][i] * products[w][i];
    }
    left_over += std::max(squares[w] - explained, 0.0);
    total_angle += cube_angle[w / 4];
  }
  g.fit_error = static_cast<float>(std::sqrt(left_over / total_angle));
  g.made = true;
  // 4z32. What the second-degree fit left over (0.063 of a weight's 0.125 average, run cod2side76) is what eight
  // weights of the kind (1 +- a)(1 +- b)(1 +- c) / 8 leave: the product a b c, of the third degree (3 sqrt(3) / 8 x
  // the three parts of the direction along the weights' own axes; its root mean square over all directions is
  // 0.0634). So: the axes from the fit itself (each weight's straight part points at its corner of a cube; from
  // weight 0's corner the three nearest corners lie one axis away each), then each weight fitted again with the
  // product of the direction's three parts along those axes as a tenth term. The models' draws hand the tenth
  // term to ngl_vk as irradiance_cubic.
  g.has_cubic = false;
  double axes[3][3];
  {
    bool taken[8] = {true};  // weight 0 itself
    for (int n = 0; n < 3; ++n) {
      int best = -1;
      double best_dot = 0.0;
      for (int w = 1; w < 8; ++w) {
        if (taken[w]) continue;
        const double dot = static_cast<double>(g.fit[0][6]) * g.fit[w][6] +
                           static_cast<double>(g.fit[0][7]) * g.fit[w][7] +
                           static_cast<double>(g.fit[0][8]) * g.fit[w][8];
        if (best < 0 || dot > best_dot) {
          best = w;
          best_dot = dot;
        }
      }
      taken[best] = true;
      for (int k = 0; k < 3; ++k) axes[n][k] = static_cast<double>(g.fit[best][6 + k]) - g.fit[0][6 + k];
    }
  }
  // made square to one another and of unit length: the first as it is, the second without its share of the first,
  // the third across both, on the third edge's side
  auto unit = [](double v[3]) {
    const double length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (!(length > 1e-9)) return false;
    for (int k = 0; k < 3; ++k) v[k] /= length;
    return true;
  };
  bool framed = unit(axes[0]);
  if (framed) {
    const double share = axes[1][0] * axes[0][0] + axes[1][1] * axes[0][1] + axes[1][2] * axes[0][2];
    for (int k = 0; k < 3; ++k) axes[1][k] -= share * axes[0][k];
    framed = unit(axes[1]);
  }
  if (framed) {
    const double across[3] = {axes[0][1] * axes[1][2] - axes[0][2] * axes[1][1],
                              axes[0][2] * axes[1][0] - axes[0][0] * axes[1][2],
                              axes[0][0] * axes[1][1] - axes[0][1] * axes[1][0]};
    const double side = across[0] * axes[2][0] + across[1] * axes[2][1] + across[2] * axes[2][2];
    for (int k = 0; k < 3; ++k) axes[2][k] = side < 0.0 ? -across[k] : across[k];
  }
  double cubic_left_over = 0.0;
  if (framed) {
    double sums10[2][10][10] = {}, products10[8][10] = {};
    for (int cube = 0; cube < 2; ++cube) {
      for (const Sample& s : samples[cube]) {
        double along[3];  // the direction's parts along the three axes
        for (int n = 0; n < 3; ++n) along[n] = s.way[0] * axes[n][0] + s.way[1] * axes[n][1] + s.way[2] * axes[n][2];
        const double terms[10] = {s.way[0] * s.way[0], s.way[1] * s.way[1], s.way[2] * s.way[2], s.way[0] * s.way[1],
                                  s.way[1] * s.way[2], s.way[2] * s.way[0], s.way[0],            s.way[1],
                                  s.way[2],            along[0] * along[1] * along[2]};
        for (int i = 0; i < 10; ++i) {
          for (int j = 0; j < 10; ++j) sums10[cube][i][j] += s.angle * terms[i] * terms[j];
          for (int k = 0; k < 4; ++k) products10[4 * cube + k][i] += s.angle * terms[i] * s.four[k];
        }
      }
    }
    double fitted[8][10] = {};
    bool solved = true;
    for (int w = 0; w < 8 && solved; ++w) {
      double a[10][11];
      for (int i = 0; i < 10; ++i) {
        for (int j = 0; j < 10; ++j) a[i][j] = sums10[w / 4][i][j];
        a[i][10] = products10[w][i];
      }
      solved = Eliminate(a, 10);
      double explained = 0.0;
      for (int i = 0; solved && i < 10; ++i) {
        fitted[w][i] = a[i][10] / a[i][i];
        explained += fitted[w][i] * products10[w][i];
      }
      cubic_left_over += std::max(squares[w] - explained, 0.0);
    }
    if (solved) {
      for (int w = 0; w < 8; ++w) {
        for (int i = 0; i < 9; ++i) g.fit[w][i] = static_cast<float>(fitted[w][i]);
        g.cubic[w] = static_cast<float>(fitted[w][9]);
      }
      for (int n = 0; n < 3; ++n)
        for (int k = 0; k < 3; ++k) g.axes[n][k] = static_cast<float>(axes[n][k]);
      g.cubic_error = static_cast<float>(std::sqrt(cubic_left_over / total_angle));
      g.has_cubic = true;
    }
  }
  if (g.has_cubic) {
    REXLOG_INFO("cod2: native side: the light grid's weights fitted again with a third-degree term: their axes "
                "({:.3f} {:.3f} {:.3f}), ({:.3f} {:.3f} {:.3f}), ({:.3f} {:.3f} {:.3f}); the term's factors {:.3f} "
                "{:.3f} {:.3f} {:.3f} and {:.3f} {:.3f} {:.3f} {:.3f} (0.650 each way for weights of the kind "
                "(1 +- a)(1 +- b)(1 +- c) / 8); to within {:.4f} now, {:.4f} without the term",
                g.axes[0][0], g.axes[0][1], g.axes[0][2], g.axes[1][0], g.axes[1][1], g.axes[1][2], g.axes[2][0],
                g.axes[2][1], g.axes[2][2], g.cubic[0], g.cubic[1], g.cubic[2], g.cubic[3], g.cubic[4], g.cubic[5],
                g.cubic[6], g.cubic[7], g.cubic_error, g.fit_error);
  } else {
    REXLOG_INFO("cod2: native side: the light grid's weights have no third-degree term (no axes from the fit)");
  }
  // 4z34. The fits above are near the weights, not the weights (run cod2side91: the weights are
  // (1 +- p0)(1 +- p1)(1 +- p2) / 8 to within 0.002, but each p is no plain function of the direction's part: sqrt(3) x
  // the part held to -1..1 is 0.013 off, root mean square). So the cubes themselves go to ngl_vk (ngl_vk.h
  // grid_cube), which has one cube slot a draw: the first cube alone, if the second is the first looking the other
  // way. Checked here texel by texel: every texel of the second cube against the first cube's texel that looks the
  // opposite way, each of its four weights against each of the first cube's four.
  g.cube = 0;
  if (square && size[0] == size[1] && !samples[0].empty() && samples[0].size() == samples[1].size()) {
    double apart[4][4] = {};  // [the second cube's weight][the first cube's]: the largest difference of all texels
    for (const Sample& s : samples[1]) {
      const Sample* across = &samples[0][0];
      double least = 2.0;
      for (const Sample& o : samples[0]) {
        const double dot = s.way[0] * o.way[0] + s.way[1] * o.way[1] + s.way[2] * o.way[2];
        if (dot < least) {
          least = dot;
          across = &o;
        }
      }
      for (int k = 0; k < 4; ++k)
        for (int j = 0; j < 4; ++j)
          apart[k][j] = std::max(apart[k][j], static_cast<double>(std::fabs(s.four[k] - across->four[j])));
    }
    bool mirrored = true, used[4] = {};
    double largest = 0.0;
    for (int k = 0; k < 4; ++k) {
      int best = 0;
      for (int j = 1; j < 4; ++j)
        if (apart[k][j] < apart[k][best]) best = j;
      g.opposite[k] = best;
      largest = std::max(largest, apart[k][best]);
      if (used[best]) mirrored = false;
      used[best] = true;
    }
    if (largest > 3.0 / 255.0) mirrored = false;
    if (mirrored) {
      const void* pixels[6];
      for (int face = 0; face < 6; ++face) pixels[face] = first_faces[face].data();
      g.cube = ngl_vk_texture_cube(pixels, 1, size[0]);
    }
    REXLOG_INFO("cod2: native side: the light grid's second cube against the first looking the other way: its "
                "weights 0 to 3 are the first cube's {} {} {} {}, at most {:.4f} apart: {}; ngl_vk cube texture {}",
                g.opposite[0], g.opposite[1], g.opposite[2], g.opposite[3], largest,
                mirrored ? "the first cube alone is handed over" : "not a mirror, the fitted rows stay the light",
                g.cube);
  }
  REXLOG_INFO("cod2: native side: the light grid's weight cubes {:08X} ({} wide, format {}) and {:08X} ({} wide, "
              "format {}): averaged over all directions {:.3f} {:.3f} {:.3f} {:.3f} and {:.3f} {:.3f} {:.3f} {:.3f} "
              "(sums {:.3f} and {:.3f}); straight up {:.3f} {:.3f} {:.3f} {:.3f} and {:.3f} {:.3f} {:.3f} {:.3f}; "
              "straight down {:.3f} {:.3f} {:.3f} {:.3f} and {:.3f} {:.3f} {:.3f} {:.3f}; fitted by second-degree "
              "functions of the direction to within {:.4f} (root mean square)",
              m.textures[0], size[0], m.fetches[0][1] & 0x3Fu, m.textures[1], size[1], m.fetches[1][1] & 0x3Fu,
              g.mean[0], g.mean[1], g.mean[2], g.mean[3], g.mean[4], g.mean[5], g.mean[6], g.mean[7],
              g.mean[0] + g.mean[1] + g.mean[2] + g.mean[3], g.mean[4] + g.mean[5] + g.mean[6] + g.mean[7], g.up[0],
              g.up[1], g.up[2], g.up[3], g.up[4], g.up[5], g.up[6], g.up[7], g.down[0], g.down[1], g.down[2],
              g.down[3], g.down[4], g.down[5], g.down[6], g.down[7], g.fit_error);
  return &g;
}

// The sky (step 4g). The game's sky.hlsl draws the sky's brush with the way from the eye to each corner (its vertex
// shader 5702FE9431432564 in run cod2shaders1: the corner minus c20) and colors it from the cube map in slot 0 looked
// up along that way (its pixel shader E24902AFEBE2480C: the cube instruction and a cube fetch, as Direct3D samples a
// cube). ngl_vk has no sky cube, so the sky is drawn as a box around the eye: one square a face, each with that face of
// the cube as a 2D texture. On a face's plane the cube's coordinates are linear, so the squares' texture coordinates
// are exact. A Xenos cube's faces lie one after another in memory, +X, -X, +Y, -Y, +Z, -Z, each its top level's size
// rounded up to 4 KB (Xenia's texture layout); each face is decoded as a 2D texture at its own address. Direct3D's
// faces: on face +X a texel (u, v) is the way (1, 1 - 2v, 1 - 2u), and so on (kSkyFaces). False when the slot's texture
// isn't a cube (fetch constant dword 5 bits 9-10, 3) or a face can't be made: then the stand-in is drawn.
bool SkyBoxFor(const uint8_t* base, uint32_t texture, const std::array<uint32_t, 6>& fetch, SkyBox*& box) {
  box = nullptr;
  if (texture == 0 || ((fetch[5] >> 9) & 3) != 3) return false;
  if (auto found = g_sky_boxes.find(texture); found != g_sky_boxes.end()) {
    box = &found->second;
    return box->made;
  }
  SkyBox& made = g_sky_boxes[texture];
  box = &made;
  Format f;
  if (!FormatOf(fetch[1] & 0x3F, &f)) return false;
  const uint32_t width = (fetch[2] & 0x1FFF) + 1, height = ((fetch[2] >> 13) & 0x1FFF) + 1;
  const bool tiled = (fetch[0] >> 31) != 0;
  const uint32_t blocks_x = (width + f.block - 1) / f.block, blocks_y = (height + f.block - 1) / f.block;
  uint32_t pitch_blocks = std::max(((fetch[0] >> 22) & 0x1FF) * 32 / f.block, blocks_x);
  if (tiled) pitch_blocks = (pitch_blocks + 31) & ~31u;
  const uint32_t rows = tiled ? (blocks_y + 31) & ~31u : blocks_y;
  const uint32_t face_bytes = (pitch_blocks * rows * f.bytes + 4095u) & ~4095u;
  static constexpr float kUv[4][2] = {{0, 0}, {1, 0}, {1, 1}, {0, 1}};
  static constexpr uint16_t kSquare[6] = {0, 1, 2, 0, 2, 3};
  const uint32_t white = 0xFFFFFFFFu;
  float white_bits;
  std::memcpy(&white_bits, &white, 4);
  const uint32_t physical = fetch[1] & 0x1FFFF000u;
  for (uint32_t face = 0; face < 6; ++face) {
    std::array<uint32_t, 6> face_fetch = fetch;
    face_fetch[1] = (fetch[1] & ~0x1FFFF000u) | ((physical + face * face_bytes) & 0x1FFFF000u);
    std::vector<uint8_t> bgra;
    uint32_t w = 0, h = 0;
    std::string why;
    if (!DecodeFetch(base, face_fetch, bgra, w, h, why)) return false;
    made.faces[face] = ngl_vk_texture(bgra.data(), w, h);
    float corners[4 * NGL_VK_CORNER_FLOATS] = {};
    for (int c = 0; c < 4; ++c) {
      float* corner = &corners[c * NGL_VK_CORNER_FLOATS];
      const float st[2] = {2.0f * kUv[c][0] - 1.0f, 2.0f * kUv[c][1] - 1.0f};
      for (int k = 0; k < 3; ++k)
        corner[k] = st[0] * kSkyFaces[face][k][0] + st[1] * kSkyFaces[face][k][1] + kSkyFaces[face][k][2];
      corner[3] = kUv[c][0];
      corner[4] = kUv[c][1];
      corner[7] = white_bits;  // corners 5-6 (the lightmap's coordinates) and 8-10 (no normal) stay 0
    }
    made.meshes[face] = ngl_vk_mesh(corners, 4, kSquare, 6);
    if (made.faces[face] == 0 || made.meshes[face] == 0) {
      g_forget = true;
      return false;
    }
  }
  made.made = true;
  REXLOG_INFO("cod2: native side: the sky box: a {}x{} cube of format {}, faces {} bytes apart", width, height,
              fetch[1] & 0x3F, face_bytes);
  return true;
}

// The sky box's six draws: the box (corners at -1 to 1) scaled to the sky's radius (a quarter more, and inside the far
// plane when the projection has one) and moved to the eye; drawn first, opaque and unlit, so everything else covers it.
void AddSkyBox(const MeshDraw& m, const SkyBox& box, std::vector<ngl_vk_mesh_draw>& out) {
  // the projection's far plane from c2 (depth) = a c3 (w) + (0, 0, 0, b): depth 1 at w = b / (1 - a), when a > 1
  const float* vp = m.view_projection;  // [i][r] = c_r[i]: c2 is column 2, c3 column 3
  const double c3 = std::sqrt(double{vp[3]} * vp[3] + double{vp[7]} * vp[7] + double{vp[11]} * vp[11]);
  const double c2 = std::sqrt(double{vp[2]} * vp[2] + double{vp[6]} * vp[6] + double{vp[10]} * vp[10]);
  const double a = c3 > 0.0 ? c2 / c3 : 1.0, b = vp[14] - a * vp[15];
  double radius = std::max(1.25 * m.sky_radius, 100.0);
  if (a > 1.0 + 1e-6 && b < 0.0) radius = std::min(radius, 0.5 * (b / (1.0 - a)));  // its corners within the far
  static bool logged = false;
  if (!logged) {
    logged = true;
    REXLOG_INFO("cod2: native side: the sky box: the sky's farthest corner {:.0f} from the eye ({:.0f} {:.0f} "
                "{:.0f}), the box's radius {:.0f} (depth scale {:.6f}, offset {:.3f})",
                m.sky_radius, m.eye[0], m.eye[1], m.eye[2], radius, a, b);
  }
  for (uint32_t face = 0; face < 6; ++face) {
    ngl_vk_mesh_draw d{};
    d.mesh = box.meshes[face];
    d.texture = box.faces[face];
    d.tint = 0x7F7F7Fu;  // 4z39: no flag bit of ngl_vk's in it (SetMeshes)
    d.shading = NGL_VK_SHADE_UNLIT;
    d.alpha = NGL_VK_ALPHA_OPAQUE;
    const float r = static_cast<float>(radius);
    const float world[16] = {r, 0, 0, 0, 0, r, 0, 0, 0, 0, r, 0, m.eye[0], m.eye[1], m.eye[2], 1};
    std::memcpy(d.world, world, sizeof d.world);
    std::memcpy(d.view_projection, m.view_projection, sizeof d.view_projection);
    out.push_back(d);
  }
}

// 4z26. The cube a water draw mirrors (its slot 1) as an ngl_vk cube texture: its six faces' top levels, decoded as the
// sky box's are (DecodeCubeFace). 0 when the slot holds no cube, a face can't be read, or ngl_vk can't make it (then the
// draw is left as it was before 4z26).
uint32_t WaterCubeFor(const uint8_t* base, uint32_t texture, const std::array<uint32_t, 6>& fetch) {
  if (texture == 0) return 0;
  if (auto found = g_water_cubes.find(texture); found != g_water_cubes.end()) return found->second;
  uint32_t& id = g_water_cubes[texture];
  id = 0;
  std::array<std::vector<uint8_t>, 6> faces;
  const void* pixels[6] = {};
  uint32_t size = 0;
  for (uint32_t face = 0; face < 6; ++face) {
    uint32_t width = 0, height = 0;
    if (!DecodeCubeFace(base, fetch, face, faces[face], width, height) || width != height || width == 0 ||
        (face != 0 && width != size))
      return 0;
    size = width;
    pixels[face] = faces[face].data();
  }
  id = ngl_vk_texture_cube(pixels, 1, size);
  REXLOG_INFO("cod2: native side: the water's cube {:08X}: {} x {} a face, format {}, ngl_vk cube texture {}", texture,
              size, size, fetch[1] & 0x3Fu, id);
  return id;
}

// 4z28. A water draw's height map (its slot 0) as an ngl_vk texture of one level. The game writes the map anew in
// place as the sea moves (4z27's check, run cod2side81: the same texture at the same address, another content on
// every kept frame), so its content is summed each time and, when it has changed, the new pixels are put into the
// same ngl_vk texture (ngl_vk_texture_rewrite): the ripples move as the game's do. 0 when the map can't be read.
uint32_t WaterHeightFor(const uint8_t* base, uint32_t texture, const std::array<uint32_t, 6>& fetch) {
  if (texture == 0) return 0;
  std::vector<uint8_t> bgra;
  uint32_t width = 0, height = 0;
  std::string why;
  if (!DecodeFetch(base, fetch, bgra, width, height, why) || bgra.empty()) return 0;
  uint32_t sum = 2166136261u;  // FNV-1a over the pixels
  for (const uint8_t byte : bgra) sum = (sum ^ byte) * 16777619u;
  WaterHeight& kept = g_water_heights[texture];
  if (kept.id != 0 && kept.width == width && kept.height == height) {
    if (kept.sum != sum) {
      const int result = ngl_vk_texture_rewrite(kept.id, bgra.data(), width, height);
      kept.sum = sum;  // also when it couldn't be written: not tried again until the game writes the next one
      static int logged = 0;
      if (logged < 3) {
        ++logged;
        REXLOG_INFO("cod2: native side: the water's height map {:08X} written anew into ngl_vk texture {} ({} x {}): "
                    "{}", texture, kept.id, width, height, result == 0 ? "done" : "refused");
      }
    }
    return kept.id;
  }
  kept.id = ngl_vk_texture(bgra.data(), width, height);
  kept.width = width;
  kept.height = height;
  kept.sum = sum;
  return kept.id;
}

// 4z30. A shiny model's pixel shader bends the normal by the normal map's x along the corner's vector at +0x34 and by
// its y along the one at +0x28 (vertex shader 12DF58A89DC165B5: N + (2x - 1) V34 + (2y - 1) V28). ngl_vk's corner has
// no room for the two vectors: its bumped path makes a frame from the screen's slopes instead, one way along rising u
// and one along rising v. So here: which of the two vectors runs along u, and whether each points with or against
// its coordinate, summed over the mesh's first triangles (each triangle's place by u and by v from its corners'
// places and coordinates, against its corners' vectors). The answer's bits: 1 the +0x34 vector runs along v (and the
// +0x28 one along u); 2 the vector along u points against rising u; 4 the one along v against rising v.
uint8_t BumpFrameOf(const uint8_t* base, const MeshDraw& m) {
  if (auto found = g_bump_frames.find(m.key); found != g_bump_frames.end()) return found->second;
  double along[2][2] = {};  // [0: the +0x34 vector, 1: the +0x28 one][0: along u, 1: along v]
  double lengths[3] = {};   // 4z31, for the log: the corners' +0x34 vector's, +0x28 vector's and normal's lengths, summed
  size_t lengths_of = 0;
  const uint32_t uv_at = UvAt(m.type);
  const size_t triangles = std::min<size_t>(m.used.size() / 3, 256);
  for (size_t t = 0; t < triangles; ++t) {
    double place[3][3], uv[3][2], vectors[2][3] = {};
    for (int c = 0; c < 3; ++c) {
      const uint32_t v = m.vertices + m.used[3 * t + c] * m.stride;
      for (uint32_t k = 0; k < 3; ++k) {
        place[c][k] = GpuF32(base, v + 4u * k);
        vectors[0][k] += GpuF32(base, v + 0x34u + 4u * k);
        vectors[1][k] += GpuF32(base, v + 0x28u + 4u * k);
      }
      uv[c][0] = GpuF32(base, v + uv_at);
      uv[c][1] = GpuF32(base, v + uv_at + 4u);
      const uint32_t at[3] = {0x34u, 0x28u, NormalAt(m.type)};
      for (int which = 0; which < 3; ++which) {
        double squared = 0.0;
        for (uint32_t k = 0; k < 3; ++k) {
          const double part = GpuF32(base, v + at[which] + 4u * k);
          squared += part * part;
        }
        lengths[which] += std::sqrt(squared);
      }
      ++lengths_of;
    }
    const double du1 = uv[1][0] - uv[0][0], dv1 = uv[1][1] - uv[0][1];
    const double du2 = uv[2][0] - uv[0][0], dv2 = uv[2][1] - uv[0][1];
    const double area = du1 * dv2 - du2 * dv1;
    if (std::fabs(area) < 1e-12) continue;
    double by_u[3], by_v[3], length_u = 0.0, length_v = 0.0;
    for (int k = 0; k < 3; ++k) {
      const double e1 = place[1][k] - place[0][k], e2 = place[2][k] - place[0][k];
      by_u[k] = (e1 * dv2 - e2 * dv1) / area;
      by_v[k] = (e2 * du1 - e1 * du2) / area;
      length_u += by_u[k] * by_u[k];
      length_v += by_v[k] * by_v[k];
    }
    if (!(length_u > 0.0) || !(length_v > 0.0)) continue;
    for (int which = 0; which < 2; ++which)
      for (int k = 0; k < 3; ++k) {
        along[which][0] += vectors[which][k] * by_u[k] / std::sqrt(length_u);
        along[which][1] += vectors[which][k] * by_v[k] / std::sqrt(length_v);
      }
  }
  const bool swapped = std::fabs(along[0][1]) + std::fabs(along[1][0]) > std::fabs(along[0][0]) + std::fabs(along[1][1]);
  const double u_side = swapped ? along[1][0] : along[0][0], v_side = swapped ? along[0][1] : along[1][1];
  const uint8_t bits = static_cast<uint8_t>((swapped ? 1 : 0) | (u_side < 0.0 ? 2 : 0) | (v_side < 0.0 ? 4 : 0));
  if (g_bump_frames.size() >= kMaxSeen) g_bump_frames.clear();  // 4z59: keys are mostly new each frame when every
                                                                // frame is drawn (the live compare)
  g_bump_frames[m.key] = bits;
  static int logged = 0;
  if (logged < 8) {
    ++logged;
    REXLOG_INFO("cod2: native side: bump frame {}: material \"{}\", {} triangles: the +0x34 vector along u {:.1f}, "
                "along v {:.1f}; the +0x28 vector along u {:.1f}, along v {:.1f}; bits {}; average lengths: the "
                "+0x34 vector {:.3f}, the +0x28 one {:.3f}, the normal {:.3f}",
                logged, IsGuestData(m.material) ? Text(base, LoadU32(base, m.material)) : std::string("-"),
                triangles, along[0][0], along[0][1], along[1][0], along[1][1], bits,
                lengths_of ? lengths[0] / lengths_of : 0.0, lengths_of ? lengths[1] / lengths_of : 0.0,
                lengths_of ? lengths[2] / lengths_of : 0.0);
  }
  return bits;
}

// 4z30. A shiny model's normal map (DXN: x in red, y in green, both unsigned) as ngl_vk's bumped path reads one: the
// bend along rising u in alpha, the bend along rising v in green, each as 2t - 1. Which of the map's two channels
// goes where, and whether turned about (255 - t), is BumpFrameOf's answer. With the game's smaller levels when they
// can be read. 0 when the map can't be read or ngl_vk has no room.
uint32_t BumpTextureFor(const uint8_t* base, uint32_t texture, const std::array<uint32_t, 6>& fetch, uint8_t frame) {
  if (texture == 0) return 0;
  const auto key = std::make_pair(texture, frame);
  if (auto found = g_bump_textures.find(key); found != g_bump_textures.end()) return found->second;
  uint32_t& id = g_bump_textures[key];
  id = 0;
  std::vector<uint8_t> top;
  uint32_t width = 0, height = 0;
  std::string why;
  if (!DecodeFetch(base, fetch, top, width, height, why)) return 0;
  std::vector<std::vector<uint8_t>> levels;
  DecodeLevelsFetch(base, fetch, levels);
  static int logged = 0;
  if (logged < 8) {  // 4z31: the map's size, how many of the game's smaller levels were read, and its average x and y
    ++logged;
    uint64_t sum_x = 0, sum_y = 0;
    for (size_t i = 0; i + 3 < top.size(); i += 4) {
      sum_x += top[i + 2];
      sum_y += top[i + 1];
    }
    const double texels = std::max<double>(1.0, static_cast<double>(top.size() / 4));
    REXLOG_INFO("cod2: native side: bump map {}: {} x {}, format {}, last level {}, {} smaller levels read, frame {}; "
                "average x {:.1f}, y {:.1f} of 255",
                logged, width, height, fetch[1] & 0x3Fu, (fetch[4] >> 6) & 0xFu, levels.size(), frame,
                sum_x / texels, sum_y / texels);
  }
  auto refit = [frame](std::vector<uint8_t>& bgra) {
    for (size_t i = 0; i + 3 < bgra.size(); i += 4) {
      const uint8_t x = bgra[i + 2], y = bgra[i + 1];
      uint8_t by_u = (frame & 1) ? y : x, by_v = (frame & 1) ? x : y;
      if (frame & 2) by_u = static_cast<uint8_t>(255 - by_u);
      if (frame & 4) by_v = static_cast<uint8_t>(255 - by_v);
      bgra[i] = 255;
      bgra[i + 1] = by_v;
      bgra[i + 2] = 0;
      bgra[i + 3] = by_u;
    }
  };
  refit(top);
  std::vector<const void*> pictures{top.data()};
  for (auto& level : levels) {
    refit(level);
    pictures.push_back(level.data());
  }
  id = ngl_vk_texture_levels(pictures.data(), static_cast<uint32_t>(pictures.size()), width, height);
  return id;
}

// 4z30. The highlights' lookup picture (the game's "$specularity": the specular map's alpha across, N . H down, one
// grey channel) as an ngl_vk texture of one level. 0 when it can't be read.
uint32_t LookupTextureFor(const uint8_t* base, uint32_t texture, const std::array<uint32_t, 6>& fetch) {
  if (texture == 0) return 0;
  if (auto found = g_lookup_textures.find(texture); found != g_lookup_textures.end()) return found->second;
  uint32_t& id = g_lookup_textures[texture];
  id = 0;
  std::vector<uint8_t> bgra;
  uint32_t width = 0, height = 0;
  std::string why;
  if (DecodeFetch(base, fetch, bgra, width, height, why)) id = ngl_vk_texture(bgra.data(), width, height);
  if (id != 0 && width >= 4 && height >= 8) {  // 4z31: which way its rows run: a few rows at four places across
    std::string rows;
    for (uint32_t step = 0; step <= 8; ++step) {
      const uint32_t y = std::min(height - 1, step * height / 8);
      rows += " row " + std::to_string(y) + ":";
      for (uint32_t across = 0; across < 4; ++across) {
        const size_t texel = static_cast<size_t>(y) * width + across * (width - 1) / 3;
        rows += " " + std::to_string(static_cast<uint32_t>(bgra[4 * texel + 2]));
      }
      rows += ";";
    }
    REXLOG_INFO("cod2: native side: highlights' lookup {} x {} (red, at the left edge, a third, two thirds and the "
                "right edge):{}",
                width, height, rows);
  }
  return id;
}

// 4z52. The world's surfaces bend their normal as the shiny models do (pixel shader 6C7F0E21C61DF68F with the world's
// vertex shader A70A57E3BDA8D434: N + (2x - 1) V28 + (2y - 1) V24, x and y the normal map's, V28 and V24 the corners'
// packed vectors at +0x28 and +0x24, FMT_2_10_10_10 as the normal at +0x0C). BumpFrameOf's question for a world mesh
// (44-byte corners: the place at +0, the texture coordinates at +0x14), with the same answer's bits: 1 the x vector
// (+0x28) runs along v and the y vector (+0x24) along u; 2 the vector along u points against rising u; 4 the one
// along v against rising v.
uint8_t WorldBumpFrameOf(const uint8_t* base, const MeshDraw& m) {
  if (auto found = g_bump_frames.find(m.key); found != g_bump_frames.end()) return found->second;
  double along[2][2] = {};  // [0: the x vector, 1: the y vector][0: along u, 1: along v]
  const size_t triangles = std::min<size_t>(m.used.size() / 3, 256);
  for (size_t t = 0; t < triangles; ++t) {
    double place[3][3], uv[3][2], vectors[2][3] = {};
    for (int c = 0; c < 3; ++c) {
      const uint32_t v = m.vertices + m.used[3 * t + c] * m.stride;
      float x_vector[3], y_vector[3];
      UnpackNormal(GpuU32(base, v + 0x28u), x_vector);
      UnpackNormal(GpuU32(base, v + 0x24u), y_vector);
      for (uint32_t k = 0; k < 3; ++k) {
        place[c][k] = GpuF32(base, v + 4u * k);
        vectors[0][k] += x_vector[k];
        vectors[1][k] += y_vector[k];
      }
      uv[c][0] = GpuF32(base, v + 0x14u);
      uv[c][1] = GpuF32(base, v + 0x18u);
    }
    const double du1 = uv[1][0] - uv[0][0], dv1 = uv[1][1] - uv[0][1];
    const double du2 = uv[2][0] - uv[0][0], dv2 = uv[2][1] - uv[0][1];
    const double area = du1 * dv2 - du2 * dv1;
    if (std::fabs(area) < 1e-12) continue;
    double by_u[3], by_v[3], length_u = 0.0, length_v = 0.0;  // the triangle's place by rising u and by rising v
    for (int k = 0; k < 3; ++k) {
      const double e1 = place[1][k] - place[0][k], e2 = place[2][k] - place[0][k];
      by_u[k] = (e1 * dv2 - e2 * dv1) / area;
      by_v[k] = (e2 * du1 - e1 * du2) / area;
      length_u += by_u[k] * by_u[k];
      length_v += by_v[k] * by_v[k];
    }
    if (!(length_u > 0.0) || !(length_v > 0.0)) continue;
    for (int which = 0; which < 2; ++which)
      for (int k = 0; k < 3; ++k) {
        along[which][0] += vectors[which][k] * by_u[k] / std::sqrt(length_u);
        along[which][1] += vectors[which][k] * by_v[k] / std::sqrt(length_v);
      }
  }
  const bool swapped =
      std::fabs(along[0][1]) + std::fabs(along[1][0]) > std::fabs(along[0][0]) + std::fabs(along[1][1]);
  const double u_side = swapped ? along[1][0] : along[0][0], v_side = swapped ? along[0][1] : along[1][1];
  const uint8_t bits = static_cast<uint8_t>((swapped ? 1 : 0) | (u_side < 0.0 ? 2 : 0) | (v_side < 0.0 ? 4 : 0));
  if (g_bump_frames.size() >= kMaxSeen) g_bump_frames.clear();  // 4z59: keys are mostly new each frame when every
                                                                // frame is drawn (the live compare)
  g_bump_frames[m.key] = bits;
  static int logged = 0;
  if (logged < 8) {
    ++logged;
    REXLOG_INFO("cod2: native side: world bump frame {}: material \"{}\", {} triangles: the x vector (+0x28) along u "
                "{:.1f}, along v {:.1f}; the y vector (+0x24) along u {:.1f}, along v {:.1f}; bits {}",
                logged, IsGuestData(m.material) ? Text(base, LoadU32(base, m.material)) : std::string("-"),
                triangles, along[0][0], along[0][1], along[1][0], along[1][1], bits);
  }
  return bits;
}

// 4z52. The highlights' lookup (LookupTextureFor's picture) once more, as a cube for ngl_vk's world_tables: a world
// surface drawn by its normal map has the map where the lookup rode, and reads the lookup from a cube's +z face
// instead. Every face holds the picture (so the face's number can't matter), stretched to 256 x 256 the way a clamped,
// linear read of the picture itself would give it. 0 when it can't be read.
uint32_t LookupCubeFor(const uint8_t* base, uint32_t texture, const std::array<uint32_t, 6>& fetch) {
  if (texture == 0) return 0;
  if (auto found = g_lookup_cubes.find(texture); found != g_lookup_cubes.end()) return found->second;
  uint32_t& id = g_lookup_cubes[texture];
  id = 0;
  std::vector<uint8_t> bgra;
  uint32_t width = 0, height = 0;
  std::string why;
  if (!DecodeFetch(base, fetch, bgra, width, height, why) || width == 0 || height == 0) return 0;
  constexpr uint32_t kFace = 256;
  std::vector<uint8_t> face(static_cast<size_t>(kFace) * kFace * 4);
  auto held = [](int i, uint32_t size) {  // a texel's number held inside the picture (a clamped read)
    return static_cast<uint32_t>(std::clamp(i, 0, static_cast<int>(size) - 1));
  };
  for (uint32_t y = 0; y < kFace; ++y) {
    const float at_y = (static_cast<float>(y) + 0.5f) / kFace * static_cast<float>(height) - 0.5f;
    const float floor_y = std::floor(at_y), part_y = at_y - floor_y;
    const uint32_t y0 = held(static_cast<int>(floor_y), height), y1 = held(static_cast<int>(floor_y) + 1, height);
    for (uint32_t x = 0; x < kFace; ++x) {
      const float at_x = (static_cast<float>(x) + 0.5f) / kFace * static_cast<float>(width) - 0.5f;
      const float floor_x = std::floor(at_x), part_x = at_x - floor_x;
      const uint32_t x0 = held(static_cast<int>(floor_x), width), x1 = held(static_cast<int>(floor_x) + 1, width);
      for (uint32_t channel = 0; channel < 4; ++channel) {
        auto texel = [&](uint32_t tx, uint32_t ty) {
          return static_cast<float>(bgra[(static_cast<size_t>(ty) * width + tx) * 4 + channel]);
        };
        const float top = texel(x0, y0) + (texel(x1, y0) - texel(x0, y0)) * part_x;
        const float bottom = texel(x0, y1) + (texel(x1, y1) - texel(x0, y1)) * part_x;
        face[(static_cast<size_t>(y) * kFace + x) * 4 + channel] =
            static_cast<uint8_t>(std::clamp(top + (bottom - top) * part_y + 0.5f, 0.0f, 255.0f));
      }
    }
  }
  const void* faces[6] = {face.data(), face.data(), face.data(), face.data(), face.data(), face.data()};
  id = ngl_vk_texture_cube(faces, 1, kFace);
  if (id == 0) g_forget = true;
  return id;
}

// 4z36. The colors as the game's picture is shown. The game's own picture (the presenter's CaptureGuestOutput) is
// taken after the display's gamma table; what ngl_vk draws is before any. On the loading screens (2D only, the same
// picture both ways) the game's values were the native's bent by one curve in every level (runs cod2side92 to 94:
// 8 -> 6, 40 -> 42, 104 -> 111, 152 -> 160, 216 -> 216), which a compare run read as the game being 5 to 9 % lighter.
// Direct3D keeps the ramp it was given in its device, as Call of Duty 3's does (ngl_side.cpp, kDeviceGammaRamp): three
// runs (red, green, blue) of 256 big-endian 16-bit values, of which the display takes the top 10 bits. Found by a
// probe that looked through the guest's memory for such runs (run cod2side98: one place, physical 1F34E65C, its
// values 8 -> 5, 40 -> 42, 104 -> 111, 152 -> 159, 216 -> 219: the measured curve) and for the image's words that
// point just before it: the word at 0x83630E60 held BF34AF80, the ramp 0x36DC behind it.
constexpr uint32_t kDevicePointer = guest::kDevicePointer;  // Direct3D's device, in physical memory
constexpr uint32_t kDeviceGammaRamp = 0x36DCu;

// The table as 8-bit values for each 8-bit value of each channel. False, and a plain ramp, when the device isn't
// there yet or what lies at the ramp's place isn't three runs that never fall.
bool ShownColors(const uint8_t* base, uint8_t table[3][256]) {
  for (int channel = 0; channel < 3; ++channel)
    for (int i = 0; i < 256; ++i) table[channel][i] = static_cast<uint8_t>(i);
  const uint32_t device = LoadU32(base, kDevicePointer);
  if (device < 0xA0000000u || device >= 0xFFF00000u) return false;
  const uint32_t physical = (device & 0x1FFFFFFFu) + kDeviceGammaRamp;
  if (PhysicalRoom(physical) < 3 * 512) return false;
  const uint8_t* p = GpuPointer(base, physical);
  uint8_t read[3][256];
  for (int channel = 0; channel < 3; ++channel) {
    uint32_t before = 0;
    for (int i = 0; i < 256; ++i) {
      const uint32_t value = static_cast<uint32_t>(p[512 * channel + 2 * i]) << 8 | p[512 * channel + 2 * i + 1];
      if (value < before) return false;
      before = value;
      read[channel][i] = static_cast<uint8_t>(std::lround((value >> 6) * 255.0 / 1023.0));
    }
    if (before < 0x8000u) return false;
  }
  std::memcpy(table, read, sizeof read);
  return true;
}

// Direct3D's row-by-row 4 x 4 matrices: a times b, and the inverse (false when there is none).
void Multiply(const float a[16], const double b[16], float out[16]) {
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) {
      double sum = 0.0;
      for (int k = 0; k < 4; ++k) sum += a[4 * r + k] * b[4 * k + c];
      out[4 * r + c] = static_cast<float>(sum);
    }
}

bool Invert(const float m[16], double out[16]) {
  double a[4][8];
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 8; ++c) a[r][c] = c < 4 ? m[4 * r + c] : (c - 4 == r ? 1.0 : 0.0);
  for (int c = 0; c < 4; ++c) {  // Gauss-Jordan with the largest pivot
    int pivot = c;
    for (int r = c + 1; r < 4; ++r)
      if (std::fabs(a[r][c]) > std::fabs(a[pivot][c])) pivot = r;
    if (std::fabs(a[pivot][c]) < 1e-12) return false;
    if (pivot != c)
      for (int k = 0; k < 8; ++k) std::swap(a[c][k], a[pivot][k]);
    const double scale = 1.0 / a[c][c];
    for (int k = 0; k < 8; ++k) a[c][k] *= scale;
    for (int r = 0; r < 4; ++r) {
      if (r == c || a[r][c] == 0.0) continue;
      const double f = a[r][c];
      for (int k = 0; k < 8; ++k) a[r][k] -= f * a[c][k];
    }
  }
  for (int r = 0; r < 4; ++r)
    for (int c = 0; c < 4; ++c) out[4 * r + c] = a[r][c + 4];
  return true;
}

// 4z13: the shadow cookie. The world's pixel shaders take the sun's part times tf6 read at the pixel's place on the
// screen (lmap_pfog 203A8845DF338725 in run cod2shaders1: tfetch2D r7, r1.zw, tf6, r1 the position over w): a
// screen-sized mask the game's shadowcookie passes make each frame from the moving models (the soldiers, the
// vehicles); the lightmap already holds the static ones' shadows. ngl_vk's sun shadow stands in for it: the rigid
// models (type 11) are drawn into its depth map from the sun (the world's c16, toward it), and the world's sun part
// is dimmed where they are nearer the sun (its lightmap_scale shading takes that part times the shadow, as the game's
// shader takes it times the cookie). The map covers kShadowReach units to each side of a point that far ahead of the
// eye, and kShadowDepth units toward and away from the sun.
constexpr float kShadowReach = 1024.0f;
constexpr float kShadowDepth = 4096.0f;

// The eye's place from a camera's view-projection (Direct3D row vectors, [4 x input + output] as the draws'): the
// point whose clip x, y and w are all 0 (Cramer's rule over those three columns). False when there is no such point.
bool EyeOf(const float view_projection[16], double eye[3]) {
  const int columns[3] = {0, 1, 3};
  double a[3][3], b[3];
  for (int k = 0; k < 3; ++k) {
    for (int i = 0; i < 3; ++i) a[k][i] = view_projection[4 * i + columns[k]];
    b[k] = -view_projection[12 + columns[k]];
  }
  auto determinant = [](const double m[3][3]) {
    return m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
           m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]);
  };
  const double whole = determinant(a);
  if (std::fabs(whole) < 1e-12) return false;
  for (int i = 0; i < 3; ++i) {
    double replaced[3][3];
    for (int k = 0; k < 3; ++k)
      for (int j = 0; j < 3; ++j) replaced[k][j] = j == i ? b[k] : a[k][j];
    eye[i] = determinant(replaced) / whole;
  }
  return true;
}

// 4z50. A view-projection that draws everything a small share of its distance nearer the eye, at the same place in
// the picture: a corner moved toward the eye by the share f comes out at (1 - f) x its clip coordinates + f x the
// eye's, and the eye's are (0, 0, its clip z, 0), so only clip z changes, by f / (1 - f) x the eye's clip z. For the
// world's decals: a stain lies in its wall's plane but is cut into other triangles than the wall, so the two
// depths differ by rounding from pixel to pixel and the stain showed in saw-edged patches (the train yard's brick
// wall, run cod2side144: dark triangles where the game's wall is dark all over; without the decals, run 160, the
// wall is light all over). ngl_vk's own bias for multiplied draws (one depth step, no slope part) is too little
// for a wall seen at a slant. Left as it is when the eye can't be found.
constexpr double kDecalPull = 0.001;  // a thousandth of the distance: a unit at 1000 units

void PullTowardEye(float view_projection[16], double share) {
  double eye[3];
  if (!EyeOf(view_projection, eye)) return;
  const double eye_z = eye[0] * view_projection[2] + eye[1] * view_projection[6] + eye[2] * view_projection[10] +
                       view_projection[14];
  view_projection[14] += static_cast<float>(share / (1.0 - share) * eye_z);
}

// The sun's view-projection for ngl_vk_set_sun_shadow (Direct3D row vectors, [4 x input + output] as the draws'):
// orthographic, x and y -1 to 1 across the map, z 0 nearest the sun to 1. False when the eye can't be found from the
// camera's view-projection or the sun's direction is 0.
bool SunShadowFor(const float view_projection[16], const float toward_sun[3], float light[16]) {
  double eye[3];
  if (!EyeOf(view_projection, eye)) return false;
  auto normalized = [](double v[3]) {
    const double length = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (length <= 0.0) return false;
    for (int k = 0; k < 3; ++k) v[k] /= length;
    return true;
  };
  // ahead: clip w is the view's depth, so its column is the way the eye looks
  double ahead[3] = {view_projection[3], view_projection[7], view_projection[11]};
  double toward[3] = {toward_sun[0], toward_sun[1], toward_sun[2]};
  if (!normalized(ahead) || !normalized(toward)) return false;
  double center[3];
  for (int k = 0; k < 3; ++k) center[k] = eye[k] + ahead[k] * kShadowReach;
  // across the map: square to the sun's way, from the world's up (z), or its x when the sun is nearly overhead
  const bool overhead = std::fabs(toward[2]) >= 0.9;
  const double from[3] = {overhead ? 1.0 : 0.0, 0.0, overhead ? 0.0 : 1.0};
  double right[3] = {toward[1] * from[2] - toward[2] * from[1], toward[2] * from[0] - toward[0] * from[2],
                     toward[0] * from[1] - toward[1] * from[0]};
  if (!normalized(right)) return false;
  const double up[3] = {right[1] * toward[2] - right[2] * toward[1], right[2] * toward[0] - right[0] * toward[2],
                        right[0] * toward[1] - right[1] * toward[0]};
  auto dot = [](const double x[3], const double y[3]) { return x[0] * y[0] + x[1] * y[1] + x[2] * y[2]; };
  for (int i = 0; i < 3; ++i) {
    light[4 * i + 0] = static_cast<float>(right[i] / kShadowReach);
    light[4 * i + 1] = static_cast<float>(up[i] / kShadowReach);
    light[4 * i + 2] = static_cast<float>(-toward[i] / (2.0 * kShadowDepth));  // nearer the sun: smaller
    light[4 * i + 3] = 0.0f;
  }
  light[12] = static_cast<float>(-dot(center, right) / kShadowReach);
  light[13] = static_cast<float>(-dot(center, up) / kShadowReach);
  light[14] = static_cast<float>(0.5 + dot(center, toward) / (2.0 * kShadowDepth));
  light[15] = 1.0f;
  return true;
}

// A matrix whose first three rows (their first three columns) only turn and scale evenly: of one length and square to
// each other, within 5%.
bool TurnsEvenly(const float world[16]) {
  float length[3];
  for (int r = 0; r < 3; ++r)
    length[r] = std::sqrt(world[4 * r] * world[4 * r] + world[4 * r + 1] * world[4 * r + 1] +
                          world[4 * r + 2] * world[4 * r + 2]);
  if (length[0] <= 0.0f) return false;
  for (int r = 0; r < 3; ++r) {
    if (std::fabs(length[r] / length[0] - 1.0f) > 0.05f) return false;
    const int s = (r + 1) % 3;
    const float across = world[4 * r] * world[4 * s] + world[4 * r + 1] * world[4 * s + 1] +
                         world[4 * r + 2] * world[4 * s + 2];
    if (std::fabs(across) > 0.05f * length[r] * length[s]) return false;
  }
  return true;
}

// A world matrix that only turns, moves and scales evenly (a model's place in the world): TurnsEvenly, and no part
// in w. A view model's c0-c3 carry a projection of its own, so the matrix made for it from the camera's is not one,
// and it casts no shadow.
bool IsRigid(const float world[16]) {
  if (!TurnsEvenly(world)) return false;
  for (int r = 0; r < 3; ++r)
    if (std::fabs(world[4 * r + 3]) > 1e-3f) return false;
  return std::fabs(world[15] - 1.0f) < 1e-3f;
}

// 4z22i: a draw placed by its own c0-c3 all the way (as the game places it: oPos is its position dotted with c0-c3),
// yet lit the way it faces. ngl_vk's push_mesh_draw makes the draw's matrix as world x view-projection, keeping only
// the world's first three columns, and turns the sun into the model's space by the world's inverse. So the world
// becomes the turn of the matrix made for the draw from the camera's (its first three rows and columns, when they
// turn evenly), and the view-projection that turn's inverse x c0-c3: their product is c0-c3 again, made from small
// numbers alone. When there is no such turn the world is the identity and the model is lit as if its own space were
// the world's (as 4z15 did).
void PlaceByOwnProjection(const float own[16], float world[16], float view_projection[16]) {
  double turn[9], inverse[9];
  bool turned = TurnsEvenly(world);
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) turn[3 * r + c] = world[4 * r + c];
  if (turned) {  // the 3 x 3 inverse by cofactors (rows of the turn, as ngl_vk takes them)
    inverse[0] = turn[4] * turn[8] - turn[5] * turn[7];
    inverse[1] = turn[2] * turn[7] - turn[1] * turn[8];
    inverse[2] = turn[1] * turn[5] - turn[2] * turn[4];
    inverse[3] = turn[5] * turn[6] - turn[3] * turn[8];
    inverse[4] = turn[0] * turn[8] - turn[2] * turn[6];
    inverse[5] = turn[2] * turn[3] - turn[0] * turn[5];
    inverse[6] = turn[3] * turn[7] - turn[4] * turn[6];
    inverse[7] = turn[1] * turn[6] - turn[0] * turn[7];
    inverse[8] = turn[0] * turn[4] - turn[1] * turn[3];
    const double determinant = turn[0] * inverse[0] + turn[1] * inverse[3] + turn[2] * inverse[6];
    turned = std::fabs(determinant) > 1e-9;
    if (turned)
      for (double& v : inverse) v /= determinant;
  }
  std::memset(world, 0, 16 * sizeof(float));
  world[15] = 1.0f;
  if (!turned) {
    for (int i = 0; i < 3; ++i) world[5 * i] = 1.0f;
    std::memcpy(view_projection, own, 16 * sizeof(float));
    return;
  }
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 3; ++c) world[4 * r + c] = static_cast<float>(turn[3 * r + c]);
  for (int r = 0; r < 3; ++r)
    for (int c = 0; c < 4; ++c) {
      double sum = 0.0;
      for (int k = 0; k < 3; ++k) sum += inverse[3 * r + k] * own[4 * k + c];
      view_projection[4 * r + c] = static_cast<float>(sum);
    }
  for (int c = 0; c < 4; ++c) view_projection[12 + c] = own[12 + c];
}

// Step 4: the kept frame's 3D as ngl_vk mesh draws (drawn by the next ngl_vk_draw_quads, under its 2D).
// 4z19: whether a material writes color at all: its state word 0's bit 27 (IW3's GFXS0_COLORWRITE_RGB, set in every
// material drawn so far: the world's 28128812, the effects' 08129165, the decals' 28128931). One without it changes
// only alpha, depth or stencil on the console, nothing seen: Libya's "clear_alpha_stencil" (run cod2side54), a quad
// over the whole picture that resets alpha and stencil, and whose texture slot happens to hold a sheet of faces.
// 4z21: the 3D draws too (SetMeshes).
constexpr uint32_t kWritesColor = 0x08000000u;

uint32_t StateWord0(const uint8_t* base, uint32_t material) {
  return IsGuestData(material) ? LoadU32(base, material + 0x14u) : 0u;
}

bool WritesColor(const uint8_t* base, uint32_t material) {
  return !IsGuestData(material) || (StateWord0(base, material) & kWritesColor) != 0;
}

// 4z16: how much of the picture a draw covers as ngl_vk places it: up to 256 of its corners, through d.world and then
// d.view_projection (row vectors, as the game's c0-c3), the box of those in front of the eye (x and y over w) and the
// nearest one's w. In Libya (runs cod2side50 and 51) something near the eye covered the whole picture (a sand texture,
// then a face's): a draw whose box spans most of the picture both ways, or which comes within 5 units of the eye, is
// logged (SetMeshes).
struct Cover {
  float nearest = 1e30f, left = 1e30f, right = -1e30f, bottom = 1e30f, top = -1e30f;
  uint32_t sampled = 0, in_front = 0;
  bool Covering() const {
    return in_front > 0 && ((right - left >= 1.6f && top - bottom >= 1.6f) || nearest < 5.0f);
  }
};

Cover CoverOf(const uint8_t* base, const MeshDraw& m, const ngl_vk_mesh_draw& d) {
  Cover c;
  const size_t count = m.effect.empty() ? m.used.size() : m.effect.size();
  const size_t step = std::max<size_t>(1, count / 256);
  for (size_t i = 0; i < count; i += step) {
    float p[3];
    if (m.effect.empty()) {
      const uint32_t v = m.vertices + m.used[i] * m.stride;
      for (uint32_t k = 0; k < 3; ++k) p[k] = GpuF32(base, v + 4u * k);
    } else {
      p[0] = m.effect[i].x;
      p[1] = m.effect[i].y;
      p[2] = m.effect[i].z;
    }
    float placed[4], clip[4];
    for (int j = 0; j < 4; ++j)
      placed[j] = p[0] * d.world[j] + p[1] * d.world[4 + j] + p[2] * d.world[8 + j] + d.world[12 + j];
    for (int j = 0; j < 4; ++j)
      clip[j] = placed[0] * d.view_projection[j] + placed[1] * d.view_projection[4 + j] +
                placed[2] * d.view_projection[8 + j] + placed[3] * d.view_projection[12 + j];
    ++c.sampled;
    c.nearest = std::min(c.nearest, clip[3]);
    if (clip[3] > 1e-4f) {
      ++c.in_front;
      const float x = clip[0] / clip[3], y = clip[1] / clip[3];
      c.left = std::min(c.left, x);
      c.right = std::max(c.right, x);
      c.bottom = std::min(c.bottom, y);
      c.top = std::max(c.top, y);
    }
  }
  return c;
}

// 4z22b: the range of the fourth float after each used corner's position (+0x0C) in the rigid models' layout, over
// up to 256 of them, and the first one's
struct FourthRange {
  float low = 1.0f, high = 1.0f, first = 1.0f;
};

FourthRange FourthOf(const uint8_t* base, const MeshDraw& m) {
  FourthRange r;
  if (m.used.empty() || m.stride != 64) return r;
  r.first = GpuF32(base, m.vertices + m.used[0] * m.stride + 0xCu);
  r.low = r.high = r.first;
  const size_t step = std::max<size_t>(1, m.used.size() / 256);
  for (size_t i = 0; i < m.used.size(); i += step) {
    const float w = GpuF32(base, m.vertices + m.used[i] * m.stride + 0xCu);
    r.low = std::min(r.low, w);
    r.high = std::max(r.high, w);
  }
  return r;
}

void SetMeshes(const uint8_t* base, uint64_t frame, const Frame& kept) {
  g_this_frame.clear();  // 4z59: the last frame's corners are given back
  std::vector<ngl_vk_mesh_draw> draws, sky;
  draws.reserve(kept.meshes.size());
  uint32_t built = 0, failed = 0, textured = 0, lightmapped = 0, effects = 0, decals = 0, volumes = 0;
  // The frame's view-projection: the world's own c0-c3 (its corners are in world space). A model drawn in its own
  // space has c0-c3 = its world matrix x this, so its world matrix is c0-c3 x this one's inverse: ngl_vk then turns the
  // sun into the model's space as it lights it.
  const float* view_projection = nullptr;
  double inverse[16];
  float light[16];  // 4z13: the sun's view-projection, when the frame has a shadow
  bool shadowed = false;
  for (const MeshDraw& m : kept.meshes)
    if (m.type == kWorldType) {
      if (Invert(m.view_projection, inverse)) {
        view_projection = m.view_projection;
        shadowed = REXCVAR_GET(cod2_debug_native_light_test) != 9 &&  // 9, tests: no model shadows
                   SunShadowFor(view_projection, m.sun_direction, light);
      }
      break;
    }
  uint32_t casters = 0, projected = 0;  // 4z13, 4z15
  uint32_t covering = 0, covering_logged = 0, covering_world = 0;  // 4z16 (CoverOf)
  uint32_t no_color = 0;  // 4z21
  uint32_t fourth_not_one = 0, fourth_named = 0;  // 4z22b (FourthOf)
  uint32_t ranged = 0, ranged_named = 0;  // 4z22e (KeepViewport)
  std::string ranged_names;
  std::string fourth_names;
  for (const MeshDraw& m : kept.meshes)
    if (m.type == kWorldType && !g_quiet) {
      REXLOG_INFO("cod2: native side: frame {}: the view-projection from the world draw \"{}\" ({} triangles); its "
                  "c3 {:.4f} {:.4f} {:.4f} {:.4f}, its c2 {:.5f} {:.5f} {:.5f} {:.4f}", frame,
                  Text(base, LoadU32(base, m.material)), m.used.size() / 3, m.view_projection[3],
                  m.view_projection[7], m.view_projection[11], m.view_projection[15], m.view_projection[2],
                  m.view_projection[6], m.view_projection[10], m.view_projection[14]);
      break;
    }
  for (const MeshDraw& m : kept.meshes) {
    if (m.type == kSkyType && m.effect.empty() && sky.empty()) {  // the sky box, when its texture is a cube
      SkyBox* box = nullptr;
      if (SkyBoxFor(base, m.textures[0], m.fetches[0], box)) {
        AddSkyBox(m, *box, sky);
        continue;
      }
    } else if (m.type == kSkyType && m.effect.empty()) {
      continue;  // the box is drawn once
    }
    if ((m.state0 & kWritesColor) == 0) {  // 4z21: a draw that writes no color (WritesColor)
      ++no_color;
      static int logged_no_color = 0;
      if (logged_no_color < 12) {
        ++logged_no_color;
        REXLOG_INFO("cod2: native side: frame {}: a 3D draw that writes no color left out: type {} \"{}\" (state "
                    "word 0 {:08X}), {} triangles", frame, m.type,
                    IsGuestData(m.material) ? Text(base, LoadU32(base, m.material)) : "-", m.state0,
                    (m.effect.empty() ? m.used.size() : m.effect.size()) / 3);
      }
      continue;
    }
    const MeshUse use = MeshFor(base, m, frame, built);
    if (use.mesh == 0) {
      ++failed;
      continue;
    }
    ngl_vk_mesh_draw d{};
    d.mesh = use.mesh;
    d.corners = use.corners;          // 4z59: this frame's corners, or 0
    d.index_count = use.index_count;  // and how many of the mesh's indices, or 0: all
    // 4z39: the color of a draw without a texture, a mid grey. Not 0x808080: since 20:26 ngl_vk reads a textured lit
    // draw's bit 23 as a flag (NGL_VK_TINT_LIGHT_MAPPED 0x00800000, Call of Duty 3's tree branches: the lights
    // times the lightmap's slot), and that bit is the top of this color's red. With 0x808080 every lit model here was
    // taken for a tree (run cod2side106: the soldiers and the boat dark and blue, their sun gone).
    d.tint = 0x7F7F7Fu;
    // 4z9: a ONE + ONE material (GFXS0 source and destination blend both 2: Moscow's fire, the snow layers on the
    // bricks) adds its color whatever its alpha, where ngl_vk's NGL_VK_BLEND_ADD adds color x alpha: its color map is
    // drawn with alpha 1 (TextureFor's opaque copy), and an effect's corners too (MeshFor)
    const bool one_one = OneAndOne(m.state0);
    // 4z28: the water's height map is its own kind of texture: one level, written anew as the game writes it
    d.texture = m.water       ? WaterHeightFor(base, m.textures[0], m.fetches[0])
                : m.decal_fog ? TurnedTextureFor(base, m.textures[m.color_slot], m.fetches[m.color_slot])  // 4z51
                              : TextureFor(base, m.textures[m.color_slot], m.fetches[m.color_slot], one_one);
    const bool effect = !m.effect.empty();
    d.lightmap = m.type == kWorldType && !effect ? WorldLightmapFor(base, m) : 0;
    d.shading = d.lightmap ? NGL_VK_SHADE_LIGHTMAP : NGL_VK_SHADE_LIGHT;
    if (d.lightmap) {  // the light as the world's pixel shaders make it (WorldLightmapFor), the sun from c16 and c17
      d.lit = 1;
      d.lightmap_scale = 2.0f;
      const float length = std::sqrt(m.sun_direction[0] * m.sun_direction[0] +
                                     m.sun_direction[1] * m.sun_direction[1] + m.sun_direction[2] * m.sun_direction[2]);
      for (int k = 0; k < 3; ++k) {
        d.light_direction[k] = length > 0.0f ? -m.sun_direction[k] / length : 0.0f;  // the way it travels
        d.light_color[k] = std::clamp(m.sun_color[k] * 0.5f, 0.0f, 4.0f);              // halved: lightmap_scale 2
        if (REXCVAR_GET(cod2_debug_native_light_test) == 1) d.light_color[k] = 0.0f;   // tests: no sun
      }
      // 4z43: a world surface with a specular map (KeepMesh): its highlight as the game's "_s" world shaders make it
      // (ngl_vk.h specular_lookup, its second half). The share where the sun doesn't reach is the shader's own number
      // when it is a sane one, else the models' 0.3.
      if (m.world_shiny && REXCVAR_GET(cod2_debug_native_light_test) != 8) {  // 8, tests: no highlights
        const uint32_t lookup = LookupTextureFor(base, m.textures[9], m.fetches[9]);
        const uint32_t specular = lookup != 0 ? TextureFor(base, m.textures[8], m.fetches[8], false) : 0;
        if (specular != 0) {
          d.specular_map = specular;
          d.specular_lookup = lookup;
          const float strength = REXCVAR_GET(cod2_debug_native_light_test) == 5 ? 10.0f : 1.0f;  // tests
          for (int k = 0; k < 3; ++k) d.specular_lookup_color[k] = m.specular_color[k] * strength;
          d.specular_lookup_shadowed = m.world_shadowed > 0.0f && m.world_shadowed < 1.0f ? m.world_shadowed : 0.3f;
        }
      }
#if defined(NGL_VK_WORLD_NORMAL_MAP)
      // 4z52: the sun's part of the light and the highlight by the normal map (slot 7), as the game's world shaders
      // make them (ngl_vk.h world_normal_map); the lightmap itself stays WorldLightmapFor's. The map takes the place
      // the lookup rode in, so a surface with a highlight needs the lookup as a cube too (LookupCubeFor); without
      // either texture the draw stays as it was.
      if (m.textures[7] != 0 && g_world_normals > 0 && m.stride == 44 &&
          REXCVAR_GET(cod2_debug_native_light_test) != 7) {
        const uint32_t tables = d.specular_lookup != 0 ? LookupCubeFor(base, m.textures[9], m.fetches[9]) : 0;
        if (d.specular_lookup == 0 || tables != 0) {
          const uint32_t turned = static_cast<uint32_t>(REXCVAR_GET(cod2_debug_native_bump_frame));  // tests
          const uint8_t frame = static_cast<uint8_t>((WorldBumpFrameOf(base, m) ^ turned) & 7u);
          d.world_normal_map = BumpTextureFor(base, m.textures[7], m.fetches[7], frame);
          d.world_tables = d.world_normal_map != 0 ? tables : 0;
        }
      }
#endif
    }
    // 4z26: the water (KeepMesh) is no model: ngl_vk draws it by its height map (the draw's texture), its cube, its
    // color and the sun (ngl_vk.h water_cube). When the cube can't be made it stays a plain textured draw.
    if (m.water && !effect) {
      d.water_cube = WaterCubeFor(base, m.textures[1], m.fetches[1]);
      const float length = std::sqrt(m.sun_direction[0] * m.sun_direction[0] +
                                     m.sun_direction[1] * m.sun_direction[1] + m.sun_direction[2] * m.sun_direction[2]);
      for (int k = 0; k < 3; ++k) {
        d.water_color[k] = m.water_color[k];
        d.water_sun_toward[k] = length > 0.0f ? m.sun_direction[k] / length : 0.0f;  // c16 points toward the sun
        d.water_sun_color[k] = m.sun_color[k];
      }
      static bool logged_water = false;
      if (!logged_water) {
        logged_water = true;
        REXLOG_INFO("cod2: native side: the first water draw: material \"{}\", height map {:08X} (ngl_vk texture {}), "
                    "cube {}, color {:.3f} {:.3f} {:.3f} (c23), sun toward {:.3f} {:.3f} {:.3f} (c16), its color {:.3f} "
                    "{:.3f} {:.3f} (c17), state word 0 {:08X}",
                    IsGuestData(m.material) ? Text(base, LoadU32(base, m.material)) : std::string("-"), m.textures[0],
                    d.texture, d.water_cube, m.water_color[0], m.water_color[1], m.water_color[2],
                    d.water_sun_toward[0], d.water_sun_toward[1], d.water_sun_toward[2], m.sun_color[0],
                    m.sun_color[1], m.sun_color[2], m.state0);
      }
    }
    const bool model = !effect && !m.water &&
                       (m.type == kModelType || m.type == kStaticModelType || m.type == kLooseModelType);
    if (model) {  // the light-probe light averaged (KeepMesh) as the ambient, and the sun
      d.lit = 1;
      const float length = std::sqrt(m.sun_direction[0] * m.sun_direction[0] +
                                     m.sun_direction[1] * m.sun_direction[1] + m.sun_direction[2] * m.sun_direction[2]);
      // 4z24: the average with the cubes' own weights (GridWeightsFor) when they can be read, else KeepMesh's
      float ambient[3] = {m.ambient[0], m.ambient[1], m.ambient[2]};
      if (const GridWeights* g = GridWeightsFor(base, m)) {
        for (int c = 0; c < 3; ++c) {
          ambient[c] = 0.0f;
          for (int k = 0; k < 4; ++k)
            ambient[c] += g->mean[k] * m.grid[2 * c][k] + g->mean[4 + k] * m.grid[2 * c + 1][k];
        }
        static int logged_grid = 0;  // the first ones: the old average against the new, and the light up and down
        if (logged_grid < 6) {
          ++logged_grid;
          float up[3] = {}, down[3] = {};
          for (int c = 0; c < 3; ++c)
            for (int k = 0; k < 4; ++k) {
              up[c] += g->up[k] * m.grid[2 * c][k] + g->up[4 + k] * m.grid[2 * c + 1][k];
              down[c] += g->down[k] * m.grid[2 * c][k] + g->down[4 + k] * m.grid[2 * c + 1][k];
            }
          REXLOG_INFO("cod2: native side: model light {} (type {}): by quarters {:.3f} {:.3f} {:.3f}, by the cubes "
                      "{:.3f} {:.3f} {:.3f}; straight up {:.3f} {:.3f} {:.3f}, straight down {:.3f} {:.3f} {:.3f}; "
                      "sun {:.3f} {:.3f} {:.3f} x {:.3f}",
                      logged_grid, m.type, m.ambient[0], m.ambient[1], m.ambient[2], ambient[0], ambient[1],
                      ambient[2], up[0], up[1], up[2], down[0], down[1], down[2], m.sun_color[0], m.sun_color[1],
                      m.sun_color[2], m.sun_scale);
        }
      }
      for (int k = 0; k < 3; ++k) {
        d.ambient[k] = std::clamp(ambient[k], 0.0f, 4.0f);
        d.light_direction[k] = length > 0.0f ? -m.sun_direction[k] / length : 0.0f;  // the way it travels
        d.light_color[k] = std::clamp(m.sun_color[k] * m.sun_scale, 0.0f, 4.0f);
      }
    }
    const bool by_volume = !effect && UsesLightVolume(m);
    // 4z47: a static model whose slot 0 is no light volume the code knows: what it is, for the first four kinds
    static uint32_t logged_words[4] = {};
    if (!effect && !by_volume && m.stride == 64 && (m.type == kStaticModelType || m.type == kLooseModelType)) {
      const std::array<uint32_t, 6>& fetch = m.fetches[0];
      const uint32_t words = fetch[1] ^ fetch[2] ^ fetch[5];
      for (uint32_t& seen : logged_words) {
        if (seen == words) break;
        if (seen != 0) continue;
        seen = words;
        REXLOG_INFO("cod2: native side: a static model without a light volume: type {}, material \"{}\", slot 0 "
                    "texture {:08X}: dimension {}, format {}, endian {}, tiled {}, {} x {} x {} (fetch words {:08X} "
                    "{:08X} {:08X} {:08X} {:08X} {:08X})",
                    m.type, IsGuestData(m.material) ? Text(base, LoadU32(base, m.material)) : std::string("-"),
                    m.textures[0], (fetch[5] >> 9) & 3u, fetch[1] & 0x3Fu, (fetch[1] >> 6) & 3u, fetch[0] >> 31,
                    (fetch[2] & 0x7FFu) + 1, ((fetch[2] >> 11) & 0x7FFu) + 1, ((fetch[2] >> 22) & 0x3FFu) + 1,
                    fetch[0], fetch[1], fetch[2], fetch[3], fetch[4], fetch[5]);
        break;
      }
    }
    if (by_volume) {  // a static model lit corner by corner (MeshFor, VolumeLightAt): its corners' colors, doubled
      d.shading = NGL_VK_SHADE_VERTEX_COLOR;
      d.lit = 0;
    }
    if (effect) d.shading = NGL_VK_SHADE_UNLIT;  // its texture times its corners' colors
    const bool fog_linear = m.fog_end > m.fog_start && m.fog_end > 0.0f;  // 4z45 (LinearFog)
    if (effect && (m.fog_exponent < 0.0f || fog_linear)) {  // 4z: the same, once, and fogged (KeepEffect: "pfog")
      d.shading = NGL_VK_SHADE_VERTEX_COLOR;
      d.light_once = 1;
    }
    const bool decal = !effect && m.type == kCloudType;  // a decal of the world's (KeepMesh): its texture, multiplied
    if (decal) d.shading = NGL_VK_SHADE_UNLIT;
    if (m.type == kSkyType && !effect) {  // the stand-in when the sky box can't be made: its average color, unlit
      d.texture = SkyColorFor(base, m.textures[0], m.fetches[0]);
      d.shading = NGL_VK_SHADE_UNLIT;
    } else if (m.fog_exponent < 0.0f) {  // the game's exponential fog (kFogLiteral; ngl_vk.h fog, a start below 0)
      d.fog[0] = m.fog_exponent;
      d.fog[1] = 0.0f;
      d.fog[2] = 1.0f;
      auto byte = [](float v) { return static_cast<uint32_t>(std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f)); };
      d.fog_color = byte(m.fog_color[0]) | byte(m.fog_color[1]) << 8 | byte(m.fog_color[2]) << 16;
    } else if (fog_linear) {  // 4z45: the game's linear fog (ngl_vk.h fog: start, end, all of it at the end)
      d.fog[0] = m.fog_start;
      d.fog[1] = m.fog_end;
      d.fog[2] = 1.0f;
      auto byte = [](float v) { return static_cast<uint32_t>(std::clamp(v * 255.0f + 0.5f, 0.0f, 255.0f)); };
      d.fog_color = byte(m.fog_color[0]) | byte(m.fog_color[1]) << 8 | byte(m.fog_color[2]) << 16;
    }
    d.alpha = m.type == kSkyType && !effect ? NGL_VK_ALPHA_OPAQUE : AlphaOf(m.state0);
    d.soft_depth = effect && m.feather > 0.0f && d.alpha == NGL_VK_ALPHA_BLEND && !one_one ? 1 : 0;  // 4y (MeshFor)
    // added onto what's behind when the destination blend is ONE (GFXS0 bits 4-7, 2), else by alpha
    if (d.alpha == NGL_VK_ALPHA_BLEND) d.blend = ((m.state0 >> 4) & 0xFu) == 2 ? NGL_VK_BLEND_ADD : NGL_VK_BLEND_ALPHA;
    // multiplied onto it: source ZERO and destination SRCCOLOR (1 and 3: Moscow's wall stains, 28128931), or source
    // DESTCOLOR and destination ZERO (9 and 1)
    if (const uint32_t source = m.state0 & 0xFu, destination = (m.state0 >> 4) & 0xFu;
        d.alpha == NGL_VK_ALPHA_BLEND && ((source == 1 && destination == 3) || (source == 9 && destination == 1)))
      d.blend = NGL_VK_BLEND_MULTIPLY;
    // 4z51: a decal's second pass is added (the material's state word is its first pass's), and its fog is in its
    // corners' colors already (MeshFor)
    if (m.decal_fog) {
      d.alpha = NGL_VK_ALPHA_BLEND;
      d.blend = NGL_VK_BLEND_ADD;
      d.fog[0] = d.fog[1] = d.fog[2] = 0.0f;
    }
    // 4z12: a corner-weighted layer (KeepMesh) whose color map has no alpha of its own (DXT1, format 18 or 51: Moscow's
    // th_snow_blend) is blended by its corners' alpha alone, as ngl_vk's corner_alpha does; one whose map has alpha
    // (the wall grime's DXT5) keeps the map's for now (the product needs ngl_vk to multiply the two)
    if (const uint32_t format = m.fetches[m.color_slot][1] & 0x3Fu;
        m.corner_weighted && d.alpha == NGL_VK_ALPHA_BLEND && (format == 18 || format == 51))
      d.corner_alpha = 1;
    effects += effect;
    decals += decal;
    volumes += by_volume;
    bool own_projection = false;
    if (model && m.type != kStaticModelType && view_projection) {  // in its own space: its world matrix (above)
      Multiply(m.view_projection, inverse, d.world);
      std::memcpy(d.view_projection, view_projection, sizeof d.view_projection);
      // 4z15: a model drawn with a projection of its own (a view model: in Libya the tank the player rides and the
      // player's own head, cod2side50) has no true world matrix to find this way: the one made is a projection too,
      // and made from Libya's large world places (some 50000 units from the origin) it is far off, so those draws
      // covered the whole picture. It is drawn as the game draws it, with its own c0-c3 all the way (lit by the turn
      // of that matrix when it has one, else as if its own space were the world's), and casts no shadow.
      // 4z22i: so is one the game draws in a depth range of its own (KeepViewport: a view model, drawn with a view
      // and projection of its own). The matrix made for Libya's own tank passed IsRigid, but only to float's
      // precision: made from places some 53000 units from the origin, its w part (up to 1e-3 passes) is dropped by
      // ngl_vk and its corners came out as much as half a unit aside, so the antenna and the gun, a unit from the
      // eye, landed a fifth of the picture away in runs cod2side65 and 69 (and less so in 67 and 68: the rounding
      // changes from run to run with the tank's place). Placed by c0-c3 now (PlaceByOwnProjection), lit by its turn.
      const bool own_range = m.depth_low > 0.0f || m.depth_high < 1.0f;
      if (own_range || !IsRigid(d.world)) {
        own_projection = true;
        PlaceByOwnProjection(m.view_projection, d.world, d.view_projection);
        ++projected;
      }
    } else {
      for (int i = 0; i < 4; ++i) d.world[5 * i] = 1.0f;  // identity: c0-c3 take the corners all the way
      std::memcpy(d.view_projection, m.view_projection, sizeof d.view_projection);
      if (decal) PullTowardEye(d.view_projection, kDecalPull);  // 4z50: clear of its wall
    }
    // 4z25: a model lit by the light grid (not by a light volume) takes the grid's light by direction, as the game's
    // lprobe pixel shaders do (GridWeightsFor), in place of its average: each fitted weight times its color (pixel
    // c8-c13) gives nine rows in world space, turned into the mesh's own space by its world matrix's turn (a corner's
    // normal n goes to the world as n x world, Direct3D's row by row) and handed over as ngl_vk's irradiance rows.
    // The sun is added to it as before.
    if (model && !by_volume) {
      if (const GridWeights* g = GridWeightsFor(base, m)) {
        float turn[3][3];  // the world matrix's three rows, each made unit length
        bool turns = true;
        for (int r = 0; r < 3; ++r) {
          const float* row = &d.world[4 * r];
          const float length = std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]);
          if (!(length > 0.0f)) turns = false;
          for (int k = 0; k < 3; ++k) turn[r][k] = length > 0.0f ? row[k] / length : 0.0f;
        }
        for (int c = 0; turns && c < 3; ++c) {
          float rows[9] = {};  // this color's nine factors in world space
          for (int w = 0; w < 8; ++w)
            for (int i = 0; i < 9; ++i) rows[i] += g->fit[w][i] * m.grid[2 * c + w / 4][w % 4];
          // as a symmetric table for the squares and a row for the rest; then both turned
          const float squares[3][3] = {{rows[0], rows[3] / 2, rows[5] / 2},
                                       {rows[3] / 2, rows[1], rows[4] / 2},
                                       {rows[5] / 2, rows[4] / 2, rows[2]}};
          float turned[3][3] = {}, straight[3] = {};
          for (int r = 0; r < 3; ++r) {
            for (int k = 0; k < 3; ++k) straight[r] += turn[r][k] * rows[6 + k];
            for (int q = 0; q < 3; ++q)
              for (int k = 0; k < 3; ++k)
                for (int l = 0; l < 3; ++l) turned[r][q] += turn[r][k] * squares[k][l] * turn[q][l];
          }
          const float own[9] = {turned[0][0], turned[1][1], turned[2][2], 2 * turned[0][1], 2 * turned[1][2],
                                2 * turned[2][0], straight[0], straight[1], straight[2]};
          for (int i = 0; i < 9; ++i) d.irradiance[i][c] = own[i];
        }
        // 4z32: and the fit's third-degree term (GridWeightsFor): each weight's factor times its color, and the
        // weights' three axes turned into the mesh's own space as the rows' straight part is
        if (turns && g->has_cubic) {
          for (int c = 0; c < 3; ++c) {
            float sum = 0.0f;
            for (int w = 0; w < 8; ++w) sum += g->cubic[w] * m.grid[2 * c + w / 4][w % 4];
            d.irradiance_cubic[c] = sum;
          }
          for (int axis = 0; axis < 3; ++axis)
            for (int r = 0; r < 3; ++r) {
              float part = 0.0f;
              for (int k = 0; k < 3; ++k) part += turn[r][k] * g->axes[axis][k];
              d.irradiance_cubic_axes[axis][r] = part;
            }
        }
        // 4z34: and the grid as the game reads it, where ngl_vk can (ngl_vk.h grid_cube): the first cube, the grid's
        // colors (the second cube's four moved to where the first cube's opposite weights are), and the way from
        // the mesh's space to the world's (the world's part k of a normal is the normal against the turn's column k)
        if (turns && g->cube != 0) {
          d.grid_cube = g->cube;
          for (int c = 0; c < 3; ++c)
            for (int k = 0; k < 4; ++k) {
              d.grid_colors[2 * c][k] = m.grid[2 * c][k];
              d.grid_colors[2 * c + 1][g->opposite[k]] = m.grid[2 * c + 1][k];
            }
          for (int k = 0; k < 3; ++k)
            for (int r = 0; r < 3; ++r) d.grid_turn[k][r] = turn[r][k];
        }
        if (turns) {
          d.irradiance_rows = 9;
          static bool logged_rows = false;  // the first one: its red rows, to check against the log's up and down
          if (!logged_rows) {
            logged_rows = true;
            REXLOG_INFO("cod2: native side: the first model lit by the grid's direction (type {}): red's rows in its "
                        "own space {:.3f} {:.3f} {:.3f} {:.3f} {:.3f} {:.3f} {:.3f} {:.3f} {:.3f}",
                        m.type, d.irradiance[0][0], d.irradiance[1][0], d.irradiance[2][0], d.irradiance[3][0],
                        d.irradiance[4][0], d.irradiance[5][0], d.irradiance[6][0], d.irradiance[7][0],
                        d.irradiance[8][0]);
          }
        }
      }
    }
    // 4z38: a "_dtl" draw's detail map (KeepMesh), added to its color map as the game's shader adds it (ngl_vk.h
    // detail_add_map)
    if (m.detail_slot != 0 && m.detail_repeat[0] > 0.0f && m.detail_repeat[1] > 0.0f) {
      d.detail_add_map = TextureFor(base, m.textures[m.detail_slot], m.fetches[m.detail_slot], false);
      d.detail_add_repeat[0] = m.detail_repeat[0];
      d.detail_add_repeat[1] = m.detail_repeat[1];
      d.detail_add_offset = -0.5f;
    }
    // 4z30: a shiny model (KeepMesh) gets its normal map's bend and its highlight as the game's "_s" shaders make
    // them (ngl_vk.h specular_lookup): the normal map refitted to ngl_vk's frame (BumpFrameOf, BumpTextureFor), the
    // specular map, the lookup picture, c19; a highlight in the sun's shadow keeps 0.3 (the PC copy's
    // Phong_SpecularVisibility: lerp(0.3, 1, the sun's visibility); the 360 shader's own literals).
    if (model && m.shiny && !by_volume) {
      const uint32_t lookup = LookupTextureFor(base, m.textures[6], m.fetches[6]);
      const uint32_t turned = static_cast<uint32_t>(REXCVAR_GET(cod2_debug_native_model_bump_frame));  // tests
      const uint8_t model_frame = static_cast<uint8_t>((BumpFrameOf(base, m) ^ turned) & 7u);
      const uint32_t bump =
          lookup != 0 ? BumpTextureFor(base, m.textures[4], m.fetches[4], model_frame) : 0;
      const uint32_t specular = bump != 0 ? TextureFor(base, m.textures[5], m.fetches[5], false) : 0;
      if (specular != 0) {
        d.normal_map = bump;
        d.specular_map = specular;
        d.specular_lookup = lookup;
        for (int k = 0; k < 3; ++k) d.specular_lookup_color[k] = m.specular_color[k];
        d.specular_lookup_shadowed = 0.3f;
      }
    }
    // 4z22e: a draw the game makes in a depth range of its own (KeepViewport): its clip z mapped into that range as
    // the viewport maps it on the console (depth = least + z / w x (most - least)), so that it lands in front of (or
    // behind) the rest as there
    if (m.depth_low > 0.0f || m.depth_high < 1.0f) {
      const float span = m.depth_high - m.depth_low;
      for (int r = 0; r < 4; ++r)
        d.view_projection[4 * r + 2] = m.depth_low * d.view_projection[4 * r + 3] + span * d.view_projection[4 * r + 2];
      if (ranged_named < 4) {
        ++ranged_named;
        char text[160];
        std::snprintf(text, sizeof text, "%s\"%s\" %.4f to %.4f (viewport %d %d %d %d)", ranged_names.empty() ? "" : ", ",
                      IsGuestData(m.material) ? Text(base, LoadU32(base, m.material)).c_str() : "-", m.depth_low,
                      m.depth_high, m.viewport[0], m.viewport[1], m.viewport[2], m.viewport[3]);
        ranged_names += text;
      }
      ++ranged;
    }
    if (shadowed) {  // 4z13: the world under the moving models' shadow (SunShadowFor)
      d.sun_shadowed = d.lightmap != 0 ? 1 : 0;
      d.shadow_caster = m.type == kModelType && !effect && !own_projection && !m.water ? 1 : 0;  // 4z26: not water
      casters += d.shadow_caster;
    }
    textured += d.texture != 0;
    lightmapped += d.lightmap != 0;
    // 4z16: the draws that cover most of the picture or come near the eye; the world's own (the ground under the
    // feet does both) only counted after the first two. 4z61: on the saved frames only (CoverOf reads up to 256 of
    // each draw's corners, and with every frame drawn these lines filled the log)
    if (const Cover c = g_quiet ? Cover{} : CoverOf(base, m, d); c.Covering()) {
      const bool world = m.type == kWorldType && !effect;
      ++covering;
      covering_world += world;
      if (covering_logged < 12 && (!world || covering_world <= 2)) {
        ++covering_logged;
        const uint32_t ps = IsGuestData(m.pass) ? LoadU32(base, m.pass + 0xCu) : 0;
        const uint32_t vs = IsGuestData(m.pass) ? LoadU32(base, m.pass + 8u) : 0;
        float now[3] = {};
        if (!m.used.empty())
          for (uint32_t k = 0; k < 3; ++k) now[k] = GpuF32(base, m.vertices + m.used[0] * m.stride + 4u * k);
        REXLOG_INFO("cod2: native side: frame {}: covering: type {} \"{}\" (vertex shader \"{}\", pixel shader "
                    "\"{}\", state word 0 {:08X}, color map format {}), {} triangles, shading {}, alpha {}, blend "
                    "{}{}; {} of {} corners in front, the nearest w {:.2f}, box x {:.2f} to {:.2f}, y {:.2f} to "
                    "{:.2f}; world row 0 {:.3f} {:.3f} {:.3f}, its place {:.1f} {:.1f} {:.1f}; vertex buffer "
                    "{:08X}, first corner when kept {:.2f} {:.2f} {:.2f}, now {:.2f} {:.2f} {:.2f}",
                    frame, m.type, IsGuestData(m.material) ? Text(base, LoadU32(base, m.material)) : "-",
                    IsGuestData(vs) ? Text(base, LoadU32(base, vs)) : "-",
                    IsGuestData(ps) ? Text(base, LoadU32(base, ps)) : "-", m.state0,
                    m.fetches[m.color_slot][1] & 0x3Fu, (effect ? m.effect.size() : m.used.size()) / 3, d.shading,
                    d.alpha, d.blend, own_projection ? ", its own projection" : "", c.in_front, c.sampled, c.nearest,
                    c.left, c.right, c.bottom, c.top, d.world[0], d.world[1], d.world[2], d.world[12], d.world[13],
                    d.world[14], m.vertices, m.kept_position[0], m.kept_position[1], m.kept_position[2], now[0],
                    now[1], now[2]);
      }
    }
    // 4z22: the models near the eye (Libya's own tank, cod2side58: drawn turned), each with its c2 and c3 (the
    // rows that make clip z and w), to see how the game keeps a view model in front of the world: a near plane of
    // its own (r_znear_depthhack, a dvar CoD2 registers at 0x82389D30) would show in c2. 4z61: on the saved frames
    // only, as above
    if (!g_quiet && (model || own_projection)) {
      static uint64_t near_frame = ~0ull;
      static int near_logged = 0;
      if (near_frame != frame) {
        near_frame = frame;
        near_logged = 0;
      }
      if (const Cover c = CoverOf(base, m, d); near_logged < 20 && c.in_front > 0 && c.nearest < 250.0f) {
        ++near_logged;
        const float* vp = m.view_projection;
        const FourthRange fourth = FourthOf(base, m);  // 4z22b
        REXLOG_INFO("cod2: native side: frame {}: near the eye: draw {} of {}, type {} \"{}\", {} triangles{}; the "
                    "nearest w {:.2f}, {} of {} in front, box x {:.2f} to {:.2f}, y {:.2f} to {:.2f}; c2 {:.5f} "
                    "{:.5f} {:.5f} {:.4f}, c3 {:.4f} {:.4f} {:.4f} {:.4f}; world row 0 {:.3f} {:.3f} {:.3f}, "
                    "its place {:.1f} {:.1f} {:.1f}; the corners' fourth float (+0x0C) {:.4f} to {:.4f}, "
                    "the first {:.4f}; viewport {} {} {} {}, depth {:.4f} to {:.4f}; state caches {:08X} {:08X} "
                    "{:08X}, material +0x18 {:08X}",
                    frame, &m - kept.meshes.data(), kept.meshes.size(), m.type,
                    IsGuestData(m.material) ? Text(base, LoadU32(base, m.material)) : "-",
                    (effect ? m.effect.size() : m.used.size()) / 3, own_projection ? ", its own projection" : "",
                    c.nearest, c.in_front, c.sampled, c.left, c.right, c.bottom, c.top, vp[2], vp[6], vp[10],
                    vp[14], vp[3], vp[7], vp[11], vp[15], d.world[0], d.world[1], d.world[2], d.world[12],
                    d.world[13], d.world[14], fourth.low, fourth.high, fourth.first, m.viewport[0], m.viewport[1],
                    m.viewport[2], m.viewport[3], m.depth_low, m.depth_high, m.live_state[0], m.live_state[1],
                    m.live_state[2], IsGuestData(m.material) ? LoadU32(base, m.material + 0x18u) : 0u);
      }
    }
    // 4z22b: the rigid models' layout (types 11 and 14) has a fourth float after the position, and lprobe_s_efog
    // (12DF58A89DC165B5 in run cod2shaders1) fetches the position as four floats (FMT_32_32_32_32_FLOAT at +0x00)
    // and dots all four with c0-c3: a corner whose fourth float isn't 1 lands elsewhere than (p, 1) puts it. Counted
    // here, and the first few such draws named, to see whether Libya's own tank is one
    if (model && m.stride == 64 && (m.type == kModelType || m.type == kLooseModelType)) {
      const FourthRange fourth = FourthOf(base, m);
      if (fourth.low != 1.0f || fourth.high != 1.0f) {
        ++fourth_not_one;
        if (fourth_named < 8) {
          ++fourth_named;
          fourth_names += fourth_names.empty() ? "" : ", ";
          char range[64];
          std::snprintf(range, sizeof range, " %.4f to %.4f", fourth.low, fourth.high);
          fourth_names += "\"" + (IsGuestData(m.material) ? Text(base, LoadU32(base, m.material)) : std::string("-")) +
                          "\"" + range;
        }
      }
    }
    draws.push_back(d);
  }
  draws.insert(draws.begin(), sky.begin(), sky.end());  // the sky first: everything else covers it
  ngl_vk_set_meshes(draws.data(), static_cast<uint32_t>(draws.size()));
  ngl_vk_set_sun_shadow(shadowed ? light : nullptr, shadowed ? view_projection : nullptr);  // 4z13
  static int logged_shadow = 0;
  if (logged_shadow < 3) {
    ++logged_shadow;
    REXLOG_INFO("cod2: native side: frame {}: the sun's shadow {} ({} rigid models cast it); {} models drawn with "
                "a projection of their own", frame, shadowed ? "on" : "off", casters, projected);
  }
  // 4z48, for the log: the world's materials with the most triangles this frame and the pixel shader each is drawn by
  // (which of them take a highlight, a detail map, a blend by their corners), to tell a picture's surfaces apart
  static int logged_world = 0;
  if (logged_world < 3) {
    ++logged_world;
    std::map<std::string, size_t> triangles;
    for (const MeshDraw& m : kept.meshes) {
      if (m.type != kWorldType || !m.effect.empty()) continue;
      const uint32_t ps = IsGuestData(m.pass) ? LoadU32(base, m.pass + 0xCu) : 0;
      triangles[(IsGuestData(m.material) ? Text(base, LoadU32(base, m.material)) : std::string("-")) + " (" +
                (IsGuestData(ps) ? Text(base, LoadU32(base, ps)) : std::string("-")) + ")"] += m.used.size() / 3;
    }
    std::vector<std::pair<size_t, std::string>> biggest;
    for (const auto& [name, count] : triangles) biggest.emplace_back(count, name);
    std::sort(biggest.rbegin(), biggest.rend());
    std::string names;
    for (size_t i = 0; i < biggest.size() && i < 12; ++i)
      names += (i ? ", " : "") + biggest[i].second + " " + std::to_string(biggest[i].first);
    REXLOG_INFO("cod2: native side: frame {}: the world's materials by triangles: {}", frame, names);
  }
  if (g_quiet) return;  // 4z56: a frame for the live compare alone
  REXLOG_INFO("cod2: native side: frame {}: {} draws cover most of the picture or come near the eye ({} of them the "
              "world's); {} that write no color left out", frame, covering, covering_world, no_color);
  REXLOG_INFO("cod2: native side: frame {}: {} rigid model draws with a corner whose fourth float isn't 1{}{}", frame,
              fourth_not_one, fourth_names.empty() ? "" : ": ", fourth_names);
  REXLOG_INFO("cod2: native side: frame {}: {} 3D draws in a depth range of their own{}{}", frame, ranged,
              ranged_names.empty() ? "" : ": ", ranged_names);
  REXLOG_INFO("cod2: native side: frame {}: {} 3D draws kept, {} drawn ({} meshes made now, {} with a texture, {} "
              "lightmapped, {} effects, {} decals, {} lit by the light volume; {} corners outside it so far), "
              "{} not made, {} dropped; textures so far: {} with their smaller levels, {} with the top one alone",
              frame, kept.meshes.size(), draws.size(), built, textured, lightmapped, effects, decals, volumes,
              g_volume_misses, failed, kept.meshes_dropped, g_textures_with_levels, g_textures_top_only);
}

// The kept frame's 2D drawn into a picture the target's size, handed to the helper to save.
// 4z17: CoD2's glow (bloom) quads, "glow_apply_bloom" and "glow_apply_sky_bleed", read the quarter-size glow buffer
// the game's GPU makes from its own picture (texture BF574920 in Moscow and in Libya: 320 wide, 8-8-8-8). The native
// picture has no such buffer, and the guest memory under that texture holds whatever was there before: in Libya (run
// cod2side52) a sheet of faces from the loading, drawn over the whole picture. They are left out until the glow is
// made from the native picture.
bool IsGlow(const uint8_t* base, uint32_t material) {
  return IsGuestData(material) && Text(base, LoadU32(base, material)).rfind("glow_", 0) == 0;
}

// 4z60: the world blurred behind a menu (cod2mp's Teams menu in run cod2mp9, frames 2400 to 4800): after the world,
// the game draws one quad over the whole picture with the material "feedbackblend", reading a 320 x 180 copy of its
// own picture that its GPU blurred (texture BF5749E0, data BE0EA000, 8-8-8-8), at the corners' alpha (FF there). The
// native side doesn't read what the game's GPU made, so the quad is left out and ngl_vk blurs its own picture instead:
// a quarter-size copy blurred over everything (ngl_vk_set_depth_of_field's blanket), by the quad's alpha.
bool IsBlurredScreen(const uint8_t* base, uint32_t material) {
  return IsGuestData(material) && Text(base, LoadU32(base, material)) == "feedbackblend";
}

// 4z58: what the live compare's frames cost (game/native_compare.h; 2 frames a second in run cod2cmp1): over every 120
// frames drawn for it alone, how often everything made was forgotten (ForgetAll: ngl_vk out of room) and how long the
// 3D (SetMeshes), the 2D and the picture's reading back and handing over took on average, logged
struct CompareCost {
  uint64_t frames = 0, forgets = 0, meshes = 0;
  double setup_ms = 0.0, draw_ms = 0.0, read_ms = 0.0;
};
CompareCost g_compare_cost;

double MillisecondsSince(std::chrono::steady_clock::time_point start) {
  return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void DrawFrame(const uint8_t* base, uint64_t frame, const Frame& kept) {
  if (g_state < 0) return;
  uint32_t width = 0, height = 0;
  kept.Screen(width, height);
  if (g_state == 0) {
    // 4z61: with cod2_native_vulkan, on the window's own device when it can (native_window.h)
    if ((NativeVulkanOn() ? StartNglVkForWindow(width, height) : ngl_vk_init(width, height)) != 0) {
      REXLOG_WARN("cod2: native side: ngl_vk couldn't start (see its reason on stderr); nothing more is drawn");
      g_state = -1;
      return;
    }
    g_state = 1;
    REXLOG_INFO("cod2: native side: ngl_vk started on {}, a {}x{} picture", ngl_vk_device_name(), width, height);
  }
  // between frames, on the drawing thread: forgotten when ngl_vk ran out of room, or before a frame could (the world's
  // index lists change as the view does, so its meshes are mostly new in each kept frame)
  uint32_t corners_used = 0, corner_room = 0;
  ngl_vk_room(nullptr, nullptr, &corners_used, &corner_room);
  const auto setup_start = std::chrono::steady_clock::now();
  // 4z62: at three quarters (was half): a frame's new meshes rarely take more than the last quarter (900,000 corners),
  // and one that finds no room sets g_forget for the next frame (run nat4: every forget was the corners past half,
  // 370 to 400 of 2048 textures in use)
  if (g_forget || corners_used > corner_room / 4 * 3) {
    // 4z62: why, the first 12 times in a run (the textures and their room, the corners and theirs)
    static int forgets_logged = 0;
    if (forgets_logged < 12) {
      ++forgets_logged;
      uint32_t textures = 0, texture_room = 0;
      ngl_vk_room(&textures, &texture_room, nullptr, nullptr);
      REXLOG_INFO("cod2: native side: frame {}: everything forgotten: {}; {} of {} textures in ngl_vk ({} kept here), "
                  "{} of {} mesh corners", frame,
                  g_forget ? "something found no room" : "the corners past three quarters",
                  textures, texture_room, g_textures.size(), corners_used, corner_room);
    }
    ForgetAll();
    ++g_compare_cost.forgets;
    corners_used = 0;
  }
  g_mesh_room_tight = corners_used > corner_room / 3;  // 4z62
  if (!kept.meshes.empty()) SetMeshes(base, frame, kept);  // step 4: the 3D under the 2D
  g_compare_cost.setup_ms += MillisecondsSince(setup_start);
  g_compare_cost.meshes += kept.meshes.size();
  const auto draw_start = std::chrono::steady_clock::now();
  std::vector<ngl_vk_vertex> vertices;
  vertices.reserve(kept.corners.size());
  uint32_t drawn = 0, textured = 0, other_size = 0, glow = 0, no_color = 0, blurred = 0;
  float blur_share = 0.0f;  // 4z60: the most of the blurred picture any "feedbackblend" quad mixes in, 0 to 1
  for (const Draw& draw : kept.draws) {
    if (draw.target_width != width || draw.target_height != height) {  // another target's 2D (a smaller one)
      ++other_size;
      continue;
    }
    if (IsGlow(base, draw.material)) {  // 4z17 (IsGlow)
      ++glow;
      continue;
    }
    if (IsBlurredScreen(base, draw.material) && draw.corner_count > 0) {  // 4z60 (IsBlurredScreen)
      ++blurred;
      const uint32_t alpha = kept.corners[draw.first_corner].color >> 24;
      blur_share = std::max(blur_share, static_cast<float>(alpha) / 255.0f);
      continue;
    }
    // 4z18: the 2D draws that cover the whole picture (their corners' box at least 90% of it both ways): in Libya (run
    // cod2side53, the glow quads already left out) one of them still shows a sheet of faces over everything
    static int logged_whole = 0;
    if (logged_whole < 48 && draw.corner_count > 0) {
      float x0 = 1e30f, x1 = -1e30f, y0 = 1e30f, y1 = -1e30f, u0 = 1e30f, u1 = -1e30f, v0 = 1e30f, v1 = -1e30f;
      for (uint32_t i = 0; i < draw.corner_count; ++i) {
        const Corner& c = kept.corners[draw.first_corner + i];
        x0 = std::min(x0, c.x), x1 = std::max(x1, c.x), y0 = std::min(y0, c.y), y1 = std::max(y1, c.y);
        u0 = std::min(u0, c.u), u1 = std::max(u1, c.u), v0 = std::min(v0, c.v), v1 = std::max(v1, c.v);
      }
      if (x1 - x0 >= 0.9f * static_cast<float>(width) && y1 - y0 >= 0.9f * static_cast<float>(height)) {
        ++logged_whole;
        const uint32_t size = draw.fetch[2];
        REXLOG_INFO("cod2: native side: frame {}: a 2D draw over the whole picture: material \"{}\" (state word 0 "
                    "{:08X}), texture {:08X} (format {}, {}x{}, data {:08X}), {} corners, texture coordinates u {:.3f} "
                    "to {:.3f}, v {:.3f} to {:.3f}, corner 0's color {:08X}; after {} of the frame's 3D draws",
                    frame,
                    IsGuestData(draw.material) ? Text(base, LoadU32(base, draw.material)) : "-",
                    StateWord0(base, draw.material), draw.texture,
                    draw.fetch[1] & 0x3Fu, (size & 0x1FFFu) + 1, ((size >> 13) & 0x1FFFu) + 1,
                    draw.fetch[1] & 0xFFFFF000u, draw.corner_count, u0, u1, v0, v1,
                    kept.corners[draw.first_corner].color, draw.meshes_before);
      }
    }
    if (!WritesColor(base, draw.material)) {  // 4z19 (WritesColor)
      ++no_color;
      static int logged_no_color = 0;
      if (logged_no_color < 8) {
        ++logged_no_color;
        REXLOG_INFO("cod2: native side: frame {}: a 2D draw that writes no color left out: material \"{}\" (state "
                    "word 0 {:08X})", frame, Text(base, LoadU32(base, draw.material)),
                    StateWord0(base, draw.material));
      }
      continue;
    }
    const uint32_t texture = TextureFor(base, draw.texture, draw.fetch, false);
    ++drawn;
    textured += texture != 0;
    for (uint32_t i = 0; i < draw.corner_count; ++i) {
      const Corner& c = kept.corners[draw.first_corner + i];
      ngl_vk_vertex v{};
      v.x = c.x;
      v.y = c.y;
      v.u = c.u;
      v.v = c.v;
      v.color = c.color;
      v.textured = texture ? 2.0f : 0.0f;  // with the texture's alpha, or the color alone
      v.texture = texture;
      vertices.push_back(v);
    }
  }
  // 4z60: near and far depth above 1, so the blanket alone; keep_depth 0, so the gun is blurred too, as in the game
  if (blur_share > 0.0f && !kept.meshes.empty()) ngl_vk_set_depth_of_field(2.0f, 2.0f, 0.0f, blur_share);
  const ngl_vk_vertex none{};
  // 4z61: a picture that goes to the window on the GPU is read back only when it is also saved (cod2_debug_native_side)
  const bool on_gpu = NativeVulkanOn() && PictureOnGpu();
  if (on_gpu) ngl_vk_set_readback(SavedFrame(frame) ? 1 : 0);
  if (ngl_vk_draw_quads(vertices.empty() ? &none : vertices.data(), static_cast<uint32_t>(vertices.size()),
                        static_cast<float>(width), static_cast<float>(height), nullptr, 0, 0, 0) != 0) {
    REXLOG_WARN("cod2: native side: drawing frame {} failed; nothing more is drawn", frame);
    g_state = -1;
    return;
  }
  uint32_t out_width = 0, out_height = 0;
  g_compare_cost.draw_ms += MillisecondsSince(draw_start);
  const auto read_start = std::chrono::steady_clock::now();
  // 4z36: through the display's gamma table, as the game's own picture is taken after it (ShownColors)
  uint8_t shown[3][256];
  const bool from_device = ShownColors(base, shown);
  // 4z61: with cod2_native_vulkan this picture is the game's: to the window, on the GPU (no read back) or through the
  // CPU (native_window.h)
  if (on_gpu) ShowOnGpu(shown);
  const uint8_t* pixels = nullptr;
  if (!on_gpu || SavedFrame(frame)) {
    pixels = static_cast<const uint8_t*>(ngl_vk_target_pixels(&out_width, &out_height));
    if (!pixels || !out_width || !out_height) {
      REXLOG_WARN("cod2: native side: frame {}: ngl_vk gave no picture back", frame);
      return;
    }
    if (NativeVulkanOn() && !on_gpu) ShowPixels(pixels, out_width, out_height, shown);
  }
  // 4z56: to the window's live compare (game/native_compare.h), through the same table; not when the native picture
  // is the game's (there is no emulated picture to compare it with)
  if (compare::Wanted() && !NativeVulkanOn() && pixels) {
    compare::GammaTable table;
    for (int channel = 0; channel < 3; ++channel) std::memcpy(table[channel].data(), shown[channel], 256);
    compare::Hand(pixels, out_width, out_height, table);
  }
  g_compare_cost.read_ms += MillisecondsSince(read_start);
  if (g_quiet && ++g_compare_cost.frames == 120) {
    const CompareCost& c = g_compare_cost;
    REXLOG_INFO("cod2: native compare: frame {}: over the last 120 frames {} times everything was forgotten (no room); "
                "on average {:.0f} 3D draws, the 3D {:.1f} ms, the 2D and drawing {:.1f} ms, the picture read and "
                "handed over {:.1f} ms", frame, c.forgets, double(c.meshes) / 120.0, c.setup_ms / 120.0,
                c.draw_ms / 120.0, c.read_ms / 120.0);
    // 4z59: where the geometry came from (MeshUse), a frame on average
    const MeshCount& n = g_mesh_count;
    REXLOG_INFO("cod2: native compare: frame {}: on average {:.1f} draws from the frame's own corners with a kept "
                "shape and {:.1f} written out in order ({:.0f} corners), {:.2f} that found the frame's room used up, "
                "{:.2f} meshes made ({:.0f} corners); {} keys and {} shapes waiting for a second frame, {} shapes "
                "kept", frame, double(n.shape_draws) / 120.0, double(n.order_draws) / 120.0,
                double(n.frame_corners) / 120.0, double(n.frame_full) / 120.0, double(n.made) / 120.0,
                double(n.made_corners) / 120.0, g_seen.size(), g_shapes_seen.size(), g_shapes.size());
    g_compare_cost = CompareCost{};
    g_mesh_count = MeshCount{};
  } else if (!g_quiet) {
    g_compare_cost = CompareCost{};  // the saved frames are timed by the check's own lines
    g_mesh_count = MeshCount{};
  }
  if (!SavedFrame(frame) || !pixels) return;  // 4z56: drawn for the live compare alone (4z61: or for the window)
  Job job;
  job.frame = frame;
  job.width = out_width;
  job.height = out_height;
  job.rgba.assign(pixels, pixels + size_t(out_width) * out_height * 4);
  for (size_t i = 0; i + 3 < job.rgba.size(); i += 4)
    for (int channel = 0; channel < 3; ++channel) job.rgba[i + channel] = shown[channel][job.rgba[i + channel]];
  static int table_logged = -1;  // -1 nothing logged yet, 0 the plain ramp, 1 the device's
  if (table_logged != (from_device ? 1 : 0)) {
    table_logged = from_device ? 1 : 0;
    auto at = [&shown](int channel, int value) { return static_cast<int>(shown[channel][value]); };
    REXLOG_INFO("cod2: native side: frame {}: the kept pictures go through {}: red 8 / 40 / 104 / 152 / 216 / 248 -> "
                "{} / {} / {} / {} / {} / {}, green 104 -> {}, blue 104 -> {}",
                frame, from_device ? "the device's gamma ramp" : "a plain ramp (no device or no ramp in it yet)",
                at(0, 8), at(0, 40), at(0, 104), at(0, 152), at(0, 216), at(0, 248), at(1, 104), at(2, 104));
  }
  job.summary = std::to_string(drawn) + " 2D draws (" + std::to_string(vertices.size() / 3) + " triangles, " +
                std::to_string(textured) + " textured, " + std::to_string(other_size) + " left out for another " +
                "target size, " + std::to_string(glow) + " glow quads, " + std::to_string(blurred) +
                " blurred-picture quads (as a blur of " + std::to_string(static_cast<int>(blur_share * 100.0f)) +
                "%) and " + std::to_string(no_color) + " that write no color left out) drawn";
  HandOver(std::move(job));
}
#endif  // COD2_NGL_VK

// What the kept frame held: one line, and the first frame with 2D lists its first draws.
void LogFrame(const uint8_t* base, uint64_t frame, const Frame& kept) {
  std::map<uint32_t, uint32_t> types, targets;
  for (const Draw& d : kept.draws) {
    ++types[d.technique_type];
    ++targets[d.target];
  }
  auto spread = [](const std::map<uint32_t, uint32_t>& counts) {
    std::string out;
    for (const auto& [key, n] : counts) out += " " + std::to_string(key) + ":" + std::to_string(n);
    return out.empty() ? std::string(" -") : out;
  };
  uint32_t screen_width = 0, screen_height = 0;
  kept.Screen(screen_width, screen_height);
  REXLOG_INFO(
      "cod2: native side: frame {}: {} draws: {} indexed, {} from the tess buffers in another layout, {} not flat "
      "(the first: \"{}\"), {} with no or bad indices, {} dropped; {} kept as 2D ({} triangles), by technique type:{}; "
      "by render target:{}; the screen (the largest target) {}x{}",
      frame, kept.seen, kept.indexed, kept.other_stride, kept.not_flat, kept.first_not_flat, kept.bad_indices,
      kept.dropped, kept.draws.size(), kept.corners.size() / 3, spread(types), spread(targets), screen_width,
      screen_height);
  if (!kept.kinds.empty()) {  // step 4's census, most triangles first
    std::vector<std::pair<std::tuple<uint32_t, uint32_t, uint32_t, uint32_t>, Frame::Kind>> kinds(kept.kinds.begin(),
                                                                                              kept.kinds.end());
    std::sort(kinds.begin(), kinds.end(),
              [](const auto& a, const auto& b) { return a.second.triangles > b.second.triangles; });
    std::string list;
    for (size_t i = 0; i < kinds.size() && i < kListedKinds; ++i) {
      const auto& [type, target, vs, ps] = kinds[i].first;
      char item[256];
      std::snprintf(item, sizeof item, "; type %u target %u \"%s\" / \"%s\": %u draws, %u triangles", type, target,
                    IsGuestData(vs) ? Text(base, LoadU32(base, vs)).c_str() : "-",
                    IsGuestData(ps) ? Text(base, LoadU32(base, ps)).c_str() : "-", kinds[i].second.draws,
                    kinds[i].second.triangles);
      list += item;
    }
    REXLOG_INFO("cod2: native side: frame {}: {} indexed draws in {} kinds{}", frame, kept.indexed, kinds.size(), list);
  }
  if (kept.draws.empty()) return;
  const bool in_level = kept.indexed > 0;
  bool& listed = in_level ? g_listed_level : g_listed_menu;
  if (listed) return;
  listed = true;
  int shown = 0;
  for (size_t i = 0; i < kept.draws.size() && shown < kListedDraws; ++i) {
    const Draw& d = kept.draws[i];
    if (in_level && (d.target_width != screen_width || d.target_height != screen_height)) continue;
    ++shown;
    const Corner& c = kept.corners[d.first_corner];
    REXLOG_INFO(
        "cod2: native side: draw {}: technique type {}, material {:08X} \"{}\", target {} ({}x{}), {} corners, first "
        "vertex {} (indices {} to {}); corner 0: x {:.1f} y {:.1f} u {:.3f} v {:.3f} color {:08X}; texture {:08X}, "
        "fetch {:08X} {:08X} {:08X} {:08X} {:08X} {:08X}",
        i, d.technique_type, d.material, IsGuestData(d.material) ? Text(base, LoadU32(base, d.material)) : "-",
        d.target, d.target_width, d.target_height, d.corner_count, d.first_vertex, d.lowest_index, d.highest_index,
        c.x, c.y, c.u, c.v, c.color, d.texture, d.fetch[0], d.fetch[1], d.fetch[2], d.fetch[3], d.fetch[4],
        d.fetch[5]);
  }
}

}  // namespace

bool SideOn() { return REXCVAR_GET(cod2_debug_native_side) || compare::Wanted() || NativeVulkanOn(); }  // 4z61

// 4z8: each shader the kept frames draw with, once: its name and the eight guest words at its start in
// <the pictures' folder>/shaders/index.txt, and the 4096 bytes each of those words (but the name's) points to in the
// game's data, as stored, in <its address>+<offset>.bin. Which of the SDK's dumped shaders (run cod2shaders1,
// shader_<hash>.ucode.bin.*) a name is shows by finding the dump's microcode in those bytes, on the PC
// (results/waw-decomp-tools/cod2/shader_names.py). The pictures' folder is on the PC; nothing leaves it.
void SaveShaderOnce(const uint8_t* base, uint32_t shader) {
  static std::vector<uint32_t> saved;
  if (!IsGuestData(shader) || saved.size() >= 512 || std::find(saved.begin(), saved.end(), shader) != saved.end())
    return;
  saved.push_back(shader);
  std::error_code ec;
  const std::filesystem::path folder = g_folder / "shaders";
  std::filesystem::create_directories(folder, ec);
  const uint32_t name_at = LoadU32(base, shader);
  std::string name = IsGuestData(name_at) ? Text(base, name_at) : std::string("-");
  for (char& c : name)
    if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_' && c != '.') c = '_';
  char stem[16];
  std::snprintf(stem, sizeof stem, "%08X", shader);
  std::ofstream index(folder / "index.txt", std::ios::app);
  index << stem << ' ' << name;
  for (uint32_t offset = 0; offset < 0x20u; offset += 4u) {
    const uint32_t word = LoadU32(base, shader + offset);
    char text[16];
    std::snprintf(text, sizeof text, " %08X", word);
    index << text;
    if (offset > 0 && IsGuestData(word) && IsGuestData(word + 4095u)) {
      std::ofstream out(folder / (std::string(stem) + "+" + std::to_string(offset) + ".bin"), std::ios::binary);
      out.write(reinterpret_cast<const char*>(base + word), 4096);
    }
  }
  index << '\n';
}

// 4z41, a probe for the bloom to come: the game's own glow passes as it draws them (the materials and pixel shaders
// with "glow" or "filter" in their names: the bright parts kept, the blurs, the adding), each kind's first draw: the
// target it draws to, the size of the picture it reads (slot 0's size word) and the pixel constants that are set
// (the PC copy names them: glowSetup's cutoff, rescale and desaturation, filterTap[8] from c8, glowApply's sky bleed
// and bloom intensity). Read from the constant mirror only.
void LogGlowPass(const uint8_t* base, uint32_t pass, const Draw& d) {
  static std::vector<std::string> logged;
  if (logged.size() >= 16) return;
  const std::string material = IsGuestData(d.material) ? Text(base, LoadU32(base, d.material)) : std::string("-");
  const uint32_t ps = IsGuestData(pass) ? LoadU32(base, pass + 0xCu) : 0;
  const std::string ps_name = IsGuestData(ps) ? Text(base, LoadU32(base, ps)) : std::string("-");
  const auto named = [](const std::string& name) {
    return name.find("glow") != std::string::npos || name.find("filter") != std::string::npos;
  };
  if (!named(material) && !named(ps_name)) return;
  // a kind: the material, the shader, the target's width and (4z42) which way a blur's first tap goes
  const float first_tap_x = LoadF32(base, kConstantMirror + 16u * (kPixelConstants + 8u));
  const std::string kind = material + " / " + ps_name + " / " + std::to_string(d.target_width) +
                           (first_tap_x != 0.0f ? " / across" : " / down");
  if (std::find(logged.begin(), logged.end(), kind) != logged.end()) return;
  logged.push_back(kind);
  std::string constants;
  uint32_t listed = 0;
  for (uint32_t c = 0; c < 40 && listed < 20; ++c) {
    float v[4];
    for (uint32_t k = 0; k < 4; ++k) v[k] = LoadF32(base, kConstantMirror + 16u * (kPixelConstants + c) + 4u * k);
    if (v[0] == 0.0f && v[1] == 0.0f && v[2] == 0.0f && v[3] == 0.0f) continue;
    if (v[0] != v[0] || v[1] != v[1] || v[2] != v[2] || v[3] != v[3]) continue;  // never set (4z42: run cod2side113)
    char item[96];
    std::snprintf(item, sizeof item, "; c%u %.5g %.5g %.5g %.5g", c, v[0], v[1], v[2], v[3]);
    constants += item;
    ++listed;
  }
  REXLOG_INFO("cod2: native side: glow pass: material \"{}\", ps \"{}\", target {} ({}x{}), reads a {}x{} picture, "
              "{} corners{}",
              material, ps_name, d.target, d.target_width, d.target_height, (d.fetch[2] & 0x1FFFu) + 1u,
              ((d.fetch[2] >> 13) & 0x1FFFu) + 1u, d.corner_count, constants);
}

// 4z48. A decal in a fogged level is drawn twice with the same triangles: first by "mul" (multiplied onto what's
// behind), then by "mul_pfog" (E082855D0821FE28 in run cod2shaders1) or the linear fog's like: (alpha - texture x
// color x alpha) x the fog's share x the fog's color, which gives back the fog the first pass took away. Drawn as
// one more multiplied decal (the material's state word is the first pass's), it put every stain on twice:
// Stalingrad's walls came out about half as bright as the game's (run cod2side133, "demolition", frame 4800: 13
// against 28 under the ceiling). 4z51: left out, as 4z48 had it, the stains were too dark where the fog is thick near
// the eye (the train yard's linear fog starts at the eye: its wall 19 against the game's 24, run cod2side161). The
// second pass is drawn as what it is now, added (MeshDraw::decal_fog; MeshFor and SetMeshes).
bool IsDecalFogPass(const uint8_t* base, uint32_t ps) {
  if (!IsGuestData(ps)) return false;
  const std::string name = Text(base, LoadU32(base, ps));
  return name.rfind("mul_", 0) == 0 && name.find("fog") != std::string::npos;
}

void SideDraw(const uint8_t* base, uint32_t args, uint32_t pass) {
  if (!SideOn() || !IsGuestData(args)) return;
  const uint64_t frame = g_frames.load(std::memory_order_relaxed) + 1;
  if (!DrawnFrame(frame)) return;  // 4z56: or every frame for the live compare
  if (!OnRenderThread()) return;
  Frame& f = g_frame;
  ++f.seen;
  if (IsGuestData(pass)) {  // 4z8: the shaders' names against the SDK's dumps
    SaveShaderOnce(base, LoadU32(base, pass + 8u));
    SaveShaderOnce(base, LoadU32(base, pass + 0xCu));
  }
  if (LoadU32(base, args) != 0) {  // indexed: step 4's census
    ++f.indexed;
    const uint32_t vs = IsGuestData(pass) ? LoadU32(base, pass + 8u) : 0;
    const uint32_t ps = IsGuestData(pass) ? LoadU32(base, pass + 0xCu) : 0;
    const auto which = std::make_tuple(LoadU32(base, kTessTechniqueType), LoadU32(base, kTargetId), vs, ps);
    Frame::Kind& kind = f.kinds[which];
    ++kind.draws;
    kind.triangles += LoadU32(base, args + 0xCu);
    if (int& sampled = g_samples[which]; sampled < kIndexedSamples) {  // a kind's first draws, when it first shows
      ++sampled;
      ++kind.samples;
      LogIndexedSample(base, args, pass);
    }
    const TechniqueKind technique = KindOf(LoadU32(base, kTessTechniqueType));  // 4z44: every fog's types
    const uint32_t type = technique.type;
    if ((type == kWorldType || type == kModelType || type == kStaticModelType || type == kLooseModelType) &&
        LoadU32(base, kTargetId) == kSceneTarget)
      g_fog_kind = technique.fog;
    if ((type == kWorldType || type == kModelType || type == kStaticModelType || type == kLooseModelType ||
         type == kSkyType) &&
        LoadU32(base, kTargetId) == kSceneTarget)
      KeepMesh(base, args, pass, type, f);
    else if (type == kCloudType && LoadU32(base, kStreamCache + 8u) == kDecalStride &&
             LoadU32(base, kTargetId) == kSceneTarget && REXCVAR_GET(cod2_debug_native_light_test) != 6) {
      const size_t before = f.meshes.size();
      KeepMesh(base, args, pass, type, f);  // a decal of the world's
      if (IsDecalFogPass(base, ps) && f.meshes.size() > before) {  // 4z51: its second pass
        MeshDraw& m = f.meshes.back();
        m.decal_fog = true;
        for (uint32_t k = 0; k < 3; ++k) m.decal_fog_color[k] = LoadF32(base, kConstantMirror + 16u * 21u + 4u * k);
        // its corners' colors depend on where the eye is, so each view has a mesh of its own (and the first pass,
        // with the same corners, another)
        for (const float value : m.view_projection) {
          uint32_t word;
          std::memcpy(&word, &value, 4);
          m.key = (m.key ^ word) * 1099511628211ull;
        }
      }
    }
    else if (type == kCloudType && LoadU32(base, kStreamCache + 8u) == kCloudStride &&
             LoadU32(base, kTargetId) == kSceneTarget)
      KeepCloud(base, args, IsGuestData(ps) && Text(base, LoadU32(base, ps)).find("outdoor") != std::string::npos, f);
    return;
  }
  if (LoadU32(base, args + 0x10u) != kTessStride) {
    ++f.other_stride;
    return;
  }
  const uint32_t triangles = LoadU32(base, args + 0xCu);
  const uint32_t indices = LoadU32(base, kTessIndexPointer);
  if (triangles == 0 || triangles > kTessMaxIndices / 3 || !IsGuestData(indices)) {
    ++f.bad_indices;
    return;
  }
  if (f.corners.size() + 3 * triangles > kMaxCorners) {
    ++f.dropped;
    return;
  }
  const size_t start = f.corners.size();
  uint32_t lowest = 0xFFFFFFFFu, highest = 0;
  for (uint32_t i = 0; i < 3 * triangles; ++i) {
    const uint32_t index = LoadU16(base, indices + 2 * i);
    if (index >= kTessMaxVertices) {
      f.corners.resize(start);
      ++f.bad_indices;
      return;
    }
    const uint32_t vertex = kTessVertices + index * kTessStride;
    if (LoadU32(base, vertex + 0x8u) != 0 || LoadU32(base, vertex + 0xCu) != 0x3F800000u) {  // z 0, w 1.0
      f.corners.resize(start);
      if (f.not_flat++ == 0) {
        const uint32_t material = LoadU32(base, kTessMaterial);
        f.first_not_flat = IsGuestData(material) ? Text(base, LoadU32(base, material)) : "-";
      }
      if (LoadU32(base, kTargetId) == kSceneTarget) KeepEffect(base, triangles, indices, pass, f);  // 3D: an effect
      return;
    }
    lowest = std::min(lowest, index);
    highest = std::max(highest, index);
    f.corners.push_back({LoadF32(base, vertex), LoadF32(base, vertex + 0x4u), LoadF32(base, vertex + 0x20u),
                         LoadF32(base, vertex + 0x24u), LoadU32(base, vertex + 0x1Cu)});
  }
  Draw& d = f.draws.emplace_back();
  d.meshes_before = static_cast<uint32_t>(f.meshes.size());  // 4z22d
  d.first_corner = static_cast<uint32_t>(start);
  d.corner_count = 3 * triangles;
  d.technique_type = LoadU32(base, kTessTechniqueType);
  d.material = LoadU32(base, kTessMaterial);
  d.target = LoadU32(base, kTargetId);
  d.target_width = LoadU32(base, kTargetWidth);
  d.target_height = LoadU32(base, kTargetHeight);
  d.first_vertex = LoadU32(base, args + 0x4u);
  d.lowest_index = lowest;
  d.highest_index = highest;
  const uint32_t image = LoadU32(base, kTextureCache);  // slot 0
  const uint32_t texture = IsGuestData(image) ? LoadU32(base, image + 4u) : 0;
  if (IsGuestData(texture)) {
    d.texture = texture;
    for (uint32_t w = 0; w < 6; ++w) d.fetch[w] = LoadU32(base, texture + kTextureFetch + 4u * w);
  }
  LogGlowPass(base, pass, d);
}

void SideFrameEnd(const uint8_t* base) {
  if (!SideOn() || !OnRenderThread()) return;
  const uint64_t frame = g_frames.fetch_add(1, std::memory_order_relaxed) + 1;
  if (!DrawnFrame(frame)) return;  // 4z56: or every frame for the live compare
  g_quiet = !SavedFrame(frame);
  if (!g_quiet) LogFrame(base, frame, g_frame);
#if defined(COD2_NGL_VK)
  DrawFrame(base, frame, g_frame);
#else
  if (frame == kKeepEvery && !g_quiet)
    REXLOG_INFO("cod2: native side: this build has no ngl_vk (Vulkan headers or glslc missing): counts only");
#endif
  g_frame.Clear();
}

void SideStart(const std::filesystem::path& folder, SideCaptureFn capture) {
  // 4z56: the check alone saves pictures (the live compare draws, but saves nothing)
  if (!REXCVAR_GET(cod2_debug_native_side) || g_helper.joinable()) return;
  g_folder = folder;
  g_capture = std::move(capture);
  g_stop = false;
  g_helper = std::thread(RunHelper);
  REXLOG_INFO("cod2: native side: on; frames {} to {}, every {}, saved in {}", kKeepEvery, kKeepLast, kKeepEvery,
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

}  // namespace cod2::native
